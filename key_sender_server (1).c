/*
 * key_sender_server.c
 *
 * Run on NXP box.
 *
 * This process is the AF_UNIX SERVER -- it creates /run/l2fwd/keyd.sock
 * and waits for the DPDK app to connect to it. This matches how the
 * original keyd worked (keyd --listen created the socket, DPDK connected).
 *
 * Sequence:
 *   1. Create /run/l2fwd/keyd.sock and listen
 *   2. Wait for DPDK app to connect (kl_init inside DPDK calls connect())
 *   3. Call get_server_tls_keys() -- full PQC TLS handshake, blocks until done
 *   4. Write keys[0] first 16 bytes to DPDK via the socket
 *   5. Sleep ROTATE_INTERVAL_SEC seconds
 *   6. Repeat from step 3 (new TLS handshake, new key, same socket connection)
 *
 * If DPDK disconnects: re-create socket, wait for DPDK to reconnect.
 *
 * Compile:
 *   gcc src/key_sender_server.c lib/call_server_key.o \
 *       -I"<path_to_l2fwd-crypto>" \
 *       -I"/usr/lib/jvm/jdk-21.0.7-oracle-x64/include" \
 *       -I"/usr/lib/jvm/jdk-21.0.7-oracle-x64/include/linux" \
 *       -L"/usr/lib/jvm/jdk-21.0.7-oracle-x64/lib/server" -ljvm \
 *       -Wl,-rpath,/usr/lib/jvm/jdk-21.0.7-oracle-x64/lib/server \
 *       -o bin/key_sender_server
 *
 * Run BEFORE the DPDK app so the socket exists when kl_init() runs.
 *   mkdir -p /run/l2fwd
 *   LD_LIBRARY_PATH=/usr/lib/jvm/jdk-21.0.7-oracle-x64/lib/server \
 *       bin/key_sender_server
 */

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <errno.h>

#include "keyd_proto.h"

/* ── configuration ───────────────────────────────────────────────────────── */
#define SERVER_NAME         "MyServer"
#define PASSWORD            "pass123"
#define PORT                4433
#define DPDK_SOCK_PATH      "/run/l2fwd/keyd.sock"
#define ROTATE_INTERVAL_SEC 120
#define AES_KEY_LEN         16

/* ── function prototype ──────────────────────────────────────────────────── */
extern jbyte **get_server_tls_keys(const char *server_name,
                                    const char *password,
                                    int port,
                                    int key_lengths[2]);

/* ── create socket, wait for DPDK to connect ─────────────────────────────── */
static int create_and_accept(void)
{
    unlink(DPDK_SOCK_PATH);

    int lfd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (lfd < 0) { perror("[KEY] socket"); return -1; }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, DPDK_SOCK_PATH, sizeof(sa.sun_path) - 1);

    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("[KEY] bind"); close(lfd); return -1;
    }
    if (listen(lfd, 1) < 0) {
        perror("[KEY] listen"); close(lfd); return -1;
    }

    fprintf(stderr, "[KEY] socket created: %s\n", DPDK_SOCK_PATH);
    fprintf(stderr, "[KEY] waiting for DPDK app to connect...\n");

    int fd = accept(lfd, NULL, NULL);
    close(lfd);
    if (fd < 0) { perror("[KEY] accept"); return -1; }

    fprintf(stderr, "[KEY] DPDK app connected\n");
    return fd;
}

/* ── send one keyd_msg ───────────────────────────────────────────────────── */
static int send_msg(int fd, const struct keyd_msg *msg)
{
    ssize_t n = send(fd, msg, sizeof(*msg), 0);
    if (n != (ssize_t)sizeof(*msg)) { perror("[KEY] send"); return -1; }
    return 0;
}

static void send_hello(int fd)
{
    struct keyd_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic   = KEYD_MSG_MAGIC;
    msg.version = KEYD_MSG_VERSION;
    msg.type    = KEYD_MSG_HELLO;
    if (send_msg(fd, &msg) == 0)
        fprintf(stderr, "[KEY] sent HELLO to DPDK\n");
}

static int send_key_to_dpdk(int fd, uint8_t epoch,
                              const uint8_t *key, int key_len)
{
    struct keyd_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic   = KEYD_MSG_MAGIC;
    msg.version = KEYD_MSG_VERSION;
    msg.type    = KEYD_MSG_INSTALL_KEY;
    msg.epoch   = epoch;

    int copy = key_len < AES_KEY_LEN ? key_len : AES_KEY_LEN;
    memcpy(msg.key, key, copy);

    if (send_msg(fd, &msg) < 0) return -1;

    fprintf(stderr, "[KEY] key sent epoch=%u key[0..3]="
            "%02x %02x %02x %02x\n",
            epoch, key[0], key[1], key[2], key[3]);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */
int main(void)
{
    fprintf(stderr, "[KEY] key_sender_server starting\n");
    fprintf(stderr, "[KEY] server=%s port=%d rotate=%ds\n",
            SERVER_NAME, PORT, ROTATE_INTERVAL_SEC);

    mkdir("/run/l2fwd", 0755);

    uint8_t epoch = 0;

    while (1) {
        /* Create socket -- DPDK connects to this */
        int fd = create_and_accept();
        if (fd < 0) { sleep(2); continue; }

        send_hello(fd);

        while (1) {
            int key_lengths[2] = {0, 0};

            fprintf(stderr, "[KEY] calling get_server_tls_keys...\n");
            jbyte **keys = get_server_tls_keys(SERVER_NAME, PASSWORD,
                                                PORT, key_lengths);
            if (keys == NULL) {
                fprintf(stderr, "[KEY] NULL keys -- retrying in 2s\n");
                sleep(2);
                continue;
            }

            /* Print keys -- same as original test file */
            printf("Encryption Key (%d bytes):\n", key_lengths[0]);
            for (int i = 0; i < key_lengths[0]; ++i)
                printf("%02X", (unsigned char)keys[0][i]);
            printf("\n");

            printf("Decryption Key (%d bytes):\n", key_lengths[1]);
            for (int i = 0; i < key_lengths[1]; ++i)
                printf("%02X", (unsigned char)keys[1][i]);
            printf("\n");

            if (key_lengths[0] < AES_KEY_LEN) {
                fprintf(stderr, "[KEY] key too short (%d) -- skipping\n",
                        key_lengths[0]);
                free(keys[0]); free(keys[1]); free(keys);
                sleep(2);
                continue;
            }

            /* Send keys[0] to DPDK */
            int ret = send_key_to_dpdk(fd, epoch,
                                        (uint8_t *)keys[0],
                                        key_lengths[0]);
            free(keys[0]); free(keys[1]); free(keys);

            if (ret < 0) {
                fprintf(stderr, "[KEY] DPDK disconnected\n");
                close(fd);
                break;  /* outer loop re-creates socket */
            }

            epoch++;
            if (epoch == 0xFF) epoch = 0;

            fprintf(stderr, "[KEY] next rotation in %ds\n",
                    ROTATE_INTERVAL_SEC);
            sleep(ROTATE_INTERVAL_SEC);
        }
    }
    return 0;
}
