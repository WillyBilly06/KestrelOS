/* tls.h - a secure connection.
 *
 * See tls.c.  The shape mirrors the plain TCP calls next door, because from
 * the caller's side that is the whole difference: connect, send, receive,
 * close, with the bytes on the wire unreadable to anyone in between.
 */
#ifndef KESTREL_TLS_H
#define KESTREL_TLS_H

#include "kernel.h"
#include "net.h"

/* What the connection turned out to be talking to, for anything that wants to
 * show it - a browser's padlock, or a diagnostic. */
typedef struct {
    char subject[128];
    char issuer[128];
    char organisation[128];
    u16  valid_until_year;
    u8   valid_until_month, valid_until_day;
    int  chain_length;
    char cipher[32];
} tls_peer_t;

/* Connect and complete the handshake.  Returns a handle, or a negative error;
 * `why` is filled with a sentence explaining any failure. */
int  tls_connect(ipv4_t ip, u16 port, const char *host, int timeout_ms,
                 const char **why);

int  tls_send_data(int handle, const void *data, int len, int timeout_ms);
int  tls_receive_data(int handle, void *buf, int len, int timeout_ms);
void tls_close(int handle);
bool tls_is_open(int handle);

/* What the far end proved it was. */
bool tls_peer_info(int handle, tls_peer_t *out);

int  tls_selftest(void);

#endif
