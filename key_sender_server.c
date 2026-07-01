/*
 * key_sender_server.c
 *
 * Rewritten from test_server_key_main.c.
 *
 * Original behaviour: call get_server_tls_keys() ONCE, print the keys,
 * exit.
 *
 * New behaviour: call get_server_tls_keys() in an infinite loop. Your
 * PQC TLS does a full fresh handshake on every call (the TLS session
 * does not persist between calls), so every call naturally produces a
 * brand new key. Each new key is written straight to the DPDK app's
 * AF_UNIX socket the moment it is received. After each successful
 * key delivery, sleep for ROTATE_INTERVAL_SEC seconds, then repeat.
 *
 * No epoch logic is exposed here -- a simple internal counter is
 * incremented automatically on each loop purely so the DPDK app's
 * existing overlap-window logic (which already expects an epoch byte
 * per key, used only to let in-flight packets under the previous key
 * keep decrypting correctly for a few seconds during rotation)
 * continues to work unmodified. You never need to manage or reason
 * about this counter.
 *
 * Usage:
 *  Run this on the NXP / server box.
 *  Compile and link exactly as the original test_server_key_main.c was.
 *
 * --- Compile and link (from project directory) ---
 *   gcc src/key_sender_server.c lib/call_server_key.o \
 *   -I"/opt/jdk-21.0.7/include" \
 *   -I"/opt/jdk-21.0.7/include/linux" \
 *   -L"/opt/jdk-21.0.7/lib/server" -ljvm \
 *   -Wl,-rpath,/opt/jdk-21.0.7/lib/server \
 *   -o bin/key_sender_server
 *
 * --- Execute (from project directory) ---
 *   mkdir -p /run/l2fwd
 *   LD_LIBRARY_PATH=/opt/jdk-21.0.7/lib/server bin/key_sender_server
 */

#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "keyd_proto.h"   /* struct keyd_msg, KEYD_MSG_* constants */

/* ── original constants, unchanged ──────────────────────────────────────── */
#define SERVER_NAME "MyServer"
#define PASSWORD    "pass123"
#define PORT        4433

/* ── new constants for socket delivery + rotation timing ──────────────────  */
#define DPDK_SOCK_PATH      "/run/l2fwd/keyd.sock"
#define ROTATE_INTERVAL_SEC 120     /* how long to wait before next handshake */
#define AES_KEY_LEN         16      /* bytes of keys[0] sent to DPDK as the AES key */

/* Function prototype -- identical to original */
extern jbyte **get_server_tls_keys(const char *server_name, const char *password, int port, int key_lengths[2]);

/* ── AF_UNIX connect to DPDK app ───────────────────────────────────────────  */
static int connect_to_dpdk(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        perror("[KEY] socket");
        return -1;
    }

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, DPDK_SOCK_PATH, sizeof(sa.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        perror("[KEY] connect");
        close(fd);
        return -1;
    }

    fprintf(stderr, "[KEY] connected to DPDK app at %s\n", DPDK_SOCK_PATH);
    return fd;
}

static int reconnect_to_dpdk(void)
{
    int fd = -1;
    while (fd < 0) {
        fd = connect_to_dpdk();
        if (fd < 0) {
            fprintf(stderr, "[KEY] DPDK app not reachable -- retrying in 2s\n");
            sleep(2);
        }
    }
    return fd;
}

/* ── send one keyd_msg, returns 0 on success ──────────────────────────────  */
static int send_msg(int fd, const struct keyd_msg *msg)
{
    ssize_t n = send(fd, msg, sizeof(*msg), 0);
    if (n != (ssize_t)sizeof(*msg)) {
        perror("[KEY] send");
        return -1;
    }
    return 0;
}

static void send_peer_up(int fd)
{
    struct keyd_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic   = KEYD_MSG_MAGIC;
    msg.version = KEYD_MSG_VERSION;
    msg.type    = KEYD_MSG_HELLO;
    if (send_msg(fd, &msg) == 0)
        fprintf(stderr, "[KEY] sent HELLO to DPDK\n");
}

/* send_peer_down removed -- KEYD_MSG_PEER_DOWN not in keyd_proto.h */

/* ── write a freshly-derived key out to the DPDK app ──────────────────────  */
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

    if (send_msg(fd, &msg) < 0)
        return -1;

    printf("Key written to DPDK socket (%d bytes used of %d):\n",
           copy, key_len);
    for (int i = 0; i < copy; ++i)
        printf("%02X", (unsigned char)key[i]);
    printf("\n");

    return 0;
}

int main(void)
{
    fprintf(stderr, "[KEY] key_sender_server starting\n");
    fprintf(stderr, "[KEY] server=%s port=%d\n", SERVER_NAME, PORT);
    fprintf(stderr, "[KEY] rotation interval: %ds\n", ROTATE_INTERVAL_SEC);

    mkdir("/run/l2fwd", 0755);

    int fd = reconnect_to_dpdk();
    send_peer_up(fd);

    uint8_t epoch = 0;   /* internal only -- purely feeds DPDK's existing
                             overlap-window mechanism, you never touch this */

    while (1) {
        int key_lengths[2];

        /* Original call -- this performs a FULL fresh TLS handshake
         * each time it is invoked, and returns once the handshake
         * has completed and keys are derived. */
        jbyte **keys = get_server_tls_keys(SERVER_NAME, PASSWORD, PORT, key_lengths);

        if (keys == NULL)
        {
            fprintf(stderr, "[ERROR] Failed to retrieve keys (NULL). Retrying in 2s.\n");
            sleep(2);
            continue;
        }

        /* Allocate unsigned char arrays -- same as original */
        unsigned char *enc_key = (unsigned char *)malloc(key_lengths[0]);
        unsigned char *dec_key = (unsigned char *)malloc(key_lengths[1]);

        if (!enc_key || !dec_key)
        {
            fprintf(stderr, "[ERROR] Memory allocation failed.\n");
            free(keys[0]);
            free(keys[1]);
            free(keys);
            free(enc_key);
            free(dec_key);
            sleep(2);
            continue;
        }

        memcpy(enc_key, keys[0], key_lengths[0]);
        memcpy(dec_key, keys[1], key_lengths[1]);

        printf("Encryption Key (%d bytes):\n", key_lengths[0]);
        for (int i = 0; i < key_lengths[0]; ++i)
            printf("%02X", (unsigned char)keys[0][i]);
        printf("\n");

        printf("Decryption Key (%d bytes):\n", key_lengths[1]);
        for (int i = 0; i < key_lengths[1]; ++i)
            printf("%02X", (unsigned char)keys[1][i]);
        printf("\n");

        /* ── write the key straight to the DPDK socket ──────────────────
         * Using enc_key (keys[0]) as the single AES key for both
         * directions, same simplification you asked for earlier. */
        if (key_lengths[0] < AES_KEY_LEN) {
            fprintf(stderr, "[KEY] enc_key too short (%d < %d) -- skipping\n",
                    key_lengths[0], AES_KEY_LEN);
        } else if (send_key_to_dpdk(fd, epoch, enc_key, key_lengths[0]) < 0) {
            fprintf(stderr, "[KEY] DPDK connection lost -- reconnecting\n");
            close(fd);
            fd = reconnect_to_dpdk();
            send_peer_up(fd);
            send_key_to_dpdk(fd, epoch, enc_key, key_lengths[0]);
        }

        /* Cleanup -- same as original, plus our extra buffers */
        free(keys[0]);
        free(keys[1]);
        free(keys);
        free(enc_key);
        free(dec_key);

        epoch++;
        if (epoch == 0xFF) epoch = 0;

        fprintf(stderr, "[KEY] next handshake in %ds\n", ROTATE_INTERVAL_SEC);
        sleep(ROTATE_INTERVAL_SEC);
    }

    close(fd);
    return 0;
}
