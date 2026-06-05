/* SPDX-License-Identifier: BSD-3-Clause
 *
 * keyd.c  --  Key agreement daemon for the DPDK encryptor control plane.
 *
 * Role
 * ----
 * Runs in the Linux kernel network namespace (NOT DPDK). Establishes a
 * mutually-authenticated TLS 1.3 session to the PEER box's keyd over the
 * kni0 exception-path interface, derives a shared AES-128 traffic key from
 * the TLS session using the RFC 5705 exporter (both ends derive the SAME
 * bytes), assigns a rotating epoch id, and pushes {epoch, key} to the local
 * DPDK data plane over an AF_UNIX SOCK_SEQPACKET socket.
 *
 * Rekey: every --rotate seconds it triggers a TLS key update / re-export and
 * installs a new epoch, keeping data-plane forward secrecy aligned with the
 * TLS session.
 *
 * One side runs as TLS server (--listen), the other as client (--connect).
 * Both present certs and verify the peer against a pinned CA.
 *
 * This file is fully testable on two ordinary Linux hosts with kernel NICs,
 * BEFORE any DPDK / virtio-user integration (see test_keyd_local.sh).
 *
 * Build:
 *   cc -O2 -Wall -o keyd keyd.c -lssl -lcrypto
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>

#include "keyd_proto.h"

/* RFC 5705 exporter label — MUST be identical on both boxes. */
#define KEYD_EXPORT_LABEL   "EXPORTER-l2fwd-traffic-key"
#define KEYD_DEFAULT_PORT   4434
#define KEYD_DEFAULT_ROTATE 120

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ───────────────────────── config ───────────────────────── */
struct cfg {
    int   is_server;            /* 1 = --listen, 0 = --connect */
    char  peer_ip[64];          /* for client: peer kni0 IP */
    char  bind_ip[64];          /* for server: local kni0 IP (or 0.0.0.0) */
    int   port;
    int   rotate_sec;
    char  cert[256];
    char  key[256];
    char  ca[256];
    char  sock_path[256];       /* AF_UNIX path to DPDK app */
};

static void usage(const char *p)
{
    fprintf(stderr,
      "usage: %s --listen|--connect PEER_IP [opts]\n"
      "  --listen                 run as TLS server\n"
      "  --connect PEER_IP        run as TLS client to PEER_IP\n"
      "  --bind IP                server bind IP (default 0.0.0.0)\n"
      "  --port N                 TLS port (default %d)\n"
      "  --rotate SEC             rekey interval (default %d)\n"
      "  --cert FILE --key FILE   this node's cert + private key (PEM)\n"
      "  --ca FILE                pinned CA to verify the peer (PEM)\n"
      "  --sock PATH              AF_UNIX path to DPDK app (default %s)\n",
      p, KEYD_DEFAULT_PORT, KEYD_DEFAULT_ROTATE, KEYD_SOCK_PATH);
}

/* ───────────────────────── AF_UNIX to DPDK app ───────────────────────── */
/*
 * keyd is the SERVER of the AF_UNIX socket; the DPDK app connects to it.
 * (Either direction works; this lets the app come and go without keyd caring
 * about ordering — keyd accepts whenever the app connects.)
 */
static int unix_listen(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) { perror("socket(AF_UNIX)"); return -1; }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);

    /* ensure dir + clean stale socket */
    unlink(path);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("bind(AF_UNIX)"); close(fd); return -1;
    }
    chmod(path, 0600);
    if (listen(fd, 1) < 0) { perror("listen"); close(fd); return -1; }
    /* Non-blocking: accept() must never stall the rotation loop when no DPDK
     * app is connected (e.g. the loopback keyd test). */
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

static int unix_send(int conn, const struct keyd_msg *m)
{
    if (conn < 0) return -1;
    ssize_t n = send(conn, m, sizeof(*m), MSG_NOSIGNAL);
    if (n != (ssize_t)sizeof(*m)) {
        fprintf(stderr, "keyd: unix_send short/err: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* ───────────────────────── OpenSSL helpers ───────────────────────── */
static void ssl_die(const char *what)
{
    fprintf(stderr, "keyd: %s\n", what);
    ERR_print_errors_fp(stderr);
    exit(1);
}

static SSL_CTX *make_ctx(const struct cfg *c)
{
    SSL_CTX *ctx = SSL_CTX_new(c->is_server ? TLS_server_method()
                                            : TLS_client_method());
    if (!ctx) ssl_die("SSL_CTX_new");

    /* TLS 1.3 only -> ECDHE, forward secrecy by default. */
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    if (SSL_CTX_use_certificate_chain_file(ctx, c->cert) != 1)
        ssl_die("load cert");
    if (SSL_CTX_use_PrivateKey_file(ctx, c->key, SSL_FILETYPE_PEM) != 1)
        ssl_die("load key");
    if (SSL_CTX_check_private_key(ctx) != 1)
        ssl_die("cert/key mismatch");

    if (SSL_CTX_load_verify_locations(ctx, c->ca, NULL) != 1)
        ssl_die("load CA");

    /* MUTUAL auth: both sides must present a cert the other trusts. */
    SSL_CTX_set_verify(ctx,
        SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
    SSL_CTX_set_verify_depth(ctx, 4);
    return ctx;
}

/* Open the TCP connection (over kni0) and run the TLS handshake. */
static SSL *tls_connect_or_accept(const struct cfg *c, SSL_CTX *ctx, int *tcp_out)
{
    int s;
    if (c->is_server) {
        int ls = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1; setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(c->port);
        a.sin_addr.s_addr = c->bind_ip[0] ? inet_addr(c->bind_ip) : INADDR_ANY;
        if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind tcp"); return NULL; }
        if (listen(ls, 1) < 0) { perror("listen tcp"); return NULL; }
        fprintf(stderr, "keyd: TLS server listening on :%d\n", c->port);
        s = accept(ls, NULL, NULL);
        close(ls);
        if (s < 0) { perror("accept"); return NULL; }
    } else {
        s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a; memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(c->port);
        a.sin_addr.s_addr = inet_addr(c->peer_ip);
        fprintf(stderr, "keyd: TLS connecting to %s:%d\n", c->peer_ip, c->port);
        if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
            perror("connect"); close(s); return NULL;
        }
    }

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, s);
    int r = c->is_server ? SSL_accept(ssl) : SSL_connect(ssl);
    if (r != 1) {
        fprintf(stderr, "keyd: TLS handshake failed\n");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl); close(s); return NULL;
    }
    fprintf(stderr, "keyd: TLS up (%s), peer verified\n", SSL_get_version(ssl));
    /* Make the socket non-blocking AFTER the handshake so the rotation loop's
     * SSL_peek() returns immediately (WANT_READ) instead of blocking forever
     * when there is no application data — handshake itself was blocking. */
    {
        int fl = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, fl | O_NONBLOCK);
    }
    *tcp_out = s;
    return ssl;
}

/*
 * Derive the shared traffic key via RFC 5705 exporter. Both ends call this
 * with the SAME label + context and get IDENTICAL bytes because they share
 * the TLS secret. We mix the epoch into the context so each epoch's exported
 * key is independent even within one TLS session.
 */
static int export_traffic_key(SSL *ssl, uint8_t epoch, uint8_t *out_key)
{
    unsigned char ctx_buf[1] = { epoch };
    int r = SSL_export_keying_material(
                ssl, out_key, KEYD_KEY_LEN,
                KEYD_EXPORT_LABEL, strlen(KEYD_EXPORT_LABEL),
                ctx_buf, sizeof(ctx_buf), 1 /* use_context */);
    if (r != 1) {
        fprintf(stderr, "keyd: exporter failed\n");
        ERR_print_errors_fp(stderr);
        return -1;
    }
    return 0;
}

/* ───────────────────────── main loop ───────────────────────── */
int main(int argc, char **argv)
{
    struct cfg c;
    memset(&c, 0, sizeof(c));
    c.port = KEYD_DEFAULT_PORT;
    c.rotate_sec = KEYD_DEFAULT_ROTATE;
    snprintf(c.sock_path, sizeof(c.sock_path), "%s", KEYD_SOCK_PATH);

    static struct option opts[] = {
        {"listen",  no_argument,       0, 'L'},
        {"connect", required_argument, 0, 'C'},
        {"bind",    required_argument, 0, 'b'},
        {"port",    required_argument, 0, 'p'},
        {"rotate",  required_argument, 0, 'r'},
        {"cert",    required_argument, 0, 'e'},
        {"key",     required_argument, 0, 'k'},
        {"ca",      required_argument, 0, 'a'},
        {"sock",    required_argument, 0, 's'},
        {0,0,0,0}
    };
    int ch;
    while ((ch = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (ch) {
        case 'L': c.is_server = 1; break;
        case 'C': c.is_server = 0; snprintf(c.peer_ip, sizeof(c.peer_ip), "%s", optarg); break;
        case 'b': snprintf(c.bind_ip, sizeof(c.bind_ip), "%s", optarg); break;
        case 'p': c.port = atoi(optarg); break;
        case 'r': c.rotate_sec = atoi(optarg); break;
        case 'e': snprintf(c.cert, sizeof(c.cert), "%s", optarg); break;
        case 'k': snprintf(c.key, sizeof(c.key), "%s", optarg); break;
        case 'a': snprintf(c.ca, sizeof(c.ca), "%s", optarg); break;
        case 's': snprintf(c.sock_path, sizeof(c.sock_path), "%s", optarg); break;
        default: usage(argv[0]); return 1;
        }
    }
    if (!c.cert[0] || !c.key[0] || !c.ca[0] ||
        (!c.is_server && !c.peer_ip[0])) {
        usage(argv[0]); return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* AF_UNIX listener for the DPDK app. Non-fatal if app not yet connected. */
    int ulisten = unix_listen(c.sock_path);
    if (ulisten < 0) return 1;
    fprintf(stderr, "keyd: AF_UNIX listening at %s\n", c.sock_path);

    SSL_CTX *ctx = make_ctx(&c);

    while (!g_stop) {
        int tcp = -1;
        SSL *ssl = tls_connect_or_accept(&c, ctx, &tcp);
        if (!ssl) { sleep(2); continue; }     /* retry on handshake failure */

        /* Accept the DPDK app connection if it is ALREADY waiting, but do NOT
         * block on it — the app may connect later (or never, in a loopback
         * test). The rotation loop below re-checks via a non-blocking accept
         * each tick, so we must not stall here. */
        int uconn = accept4(ulisten, NULL, NULL, SOCK_NONBLOCK);
        if (uconn >= 0) {
            struct keyd_msg hello = { .magic = KEYD_MSG_MAGIC,
                .version = KEYD_MSG_VERSION, .type = KEYD_MSG_HELLO };
            unix_send(uconn, &hello);
        }

        uint8_t epoch = 0;
        time_t last = 0;

        while (!g_stop) {
            time_t now = time(NULL);
            if (now - last >= c.rotate_sec) {
                struct keyd_msg m;
                memset(&m, 0, sizeof(m));
                m.magic = KEYD_MSG_MAGIC;
                m.version = KEYD_MSG_VERSION;
                m.type = KEYD_MSG_INSTALL_KEY;
                m.epoch = epoch;

                if (export_traffic_key(ssl, epoch, m.key) != 0)
                    break;   /* exporter failed -> tear down, re-handshake */

                if (uconn >= 0 && unix_send(uconn, &m) != 0) {
                    /* app went away; try to re-accept next loop */
                    close(uconn); uconn = -1;
                }
                fprintf(stderr, "keyd: installed epoch %u\n", epoch);
                explicit_bzero(&m, sizeof(m));

                /* Trigger a TLS key update so the exporter advances with the
                 * session (forward secrecy each rotation). Client initiates. */
                if (!c.is_server)
                    SSL_key_update(ssl, SSL_KEY_UPDATE_REQUESTED);

                epoch++;
                last = now;
            }

            /* Drive TLS (process key updates / detect peer close). Socket is
             * non-blocking, so WANT_READ/WANT_WRITE just mean "idle, no data". */
            char tmp[64];
            int r = SSL_peek(ssl, tmp, sizeof(tmp));
            if (r <= 0) {
                int e = SSL_get_error(ssl, r);
                if (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_WANT_WRITE) {
                    /* idle — expected when no app data is flowing */
                } else if (e == SSL_ERROR_ZERO_RETURN || e == SSL_ERROR_SYSCALL) {
                    fprintf(stderr, "keyd: TLS peer closed; re-handshaking\n");
                    break;
                }
            }
            /* If the app hasn't connected yet, try a non-blocking accept. */
            if (uconn < 0) {
                int u = accept4(ulisten, NULL, NULL, SOCK_NONBLOCK);
                if (u >= 0) uconn = u;
            }
            usleep(200 * 1000);   /* 200 ms control-plane tick */
        }

        if (uconn >= 0) close(uconn);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(tcp);
    }

    SSL_CTX_free(ctx);
    close(ulisten);
    unlink(c.sock_path);
    fprintf(stderr, "keyd: stopped\n");
    return 0;
}
