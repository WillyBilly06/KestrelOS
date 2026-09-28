/* web_test_shim.h - lets the real user/libweb/web_fetch.c build on the host.
 *
 * web_fetch.c does #include "web.h", and the real web.h pulls in the whole OS
 * libc header (kestrel.h), which does not exist off-target.  Force-including
 * this file ahead of everything (clang -include) defines the real header's
 * guard, KESTREL_WEB_H, so the real web.h expands to nothing and these
 * host-backed declarations stand in its place.  The code under test is then
 * the unmodified web_fetch.c on disk - see tools/web_post_host_test.c. */
#ifndef WEB_TEST_SHIM_H
#define WEB_TEST_SHIM_H
#define KESTREL_WEB_H            /* suppress the real user/libweb/web.h body */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define WEB_MAX_URL 1024
typedef struct { int dummy; } ktlspeer_t;

typedef struct { char scheme[8]; char host[256]; char path[768];
                 uint16_t port; bool secure; } web_url_t;
bool web_parse_url(const char *url, const web_url_t *base, web_url_t *out);
void web_format_url(const web_url_t *url, char *out, size_t cap);

typedef struct {
    int status; char *body; int body_len;
    char content_type[96]; char final_url[WEB_MAX_URL]; char error[192];
    bool secure; ktlspeer_t peer;
} web_response_t;
bool web_fetch(const char *url, int redirect_limit, web_response_t *out);
bool web_fetch_post(const char *url, const char *body, int body_len,
                    const char *content_type, int redirect_limit,
                    web_response_t *out);
void web_free(web_response_t *r);
typedef void (*web_progress_t)(const char *what, void *ctx);
void web_on_progress(web_progress_t fn, void *ctx);

/* Kernel-side calls web_fetch.c makes; the test file stubs them. */
uint32_t net_resolve(const char *host, int timeout_ms);
int  tcp_connect(uint32_t ip, uint16_t port, int timeout_ms);
int  tcp_send(int handle, const void *data, int len, int timeout_ms);
int  tcp_recv(int handle, void *buf, int len, int timeout_ms);
void tcp_close(int handle);
int  tls_connect(uint32_t ip, uint16_t port, const char *host, int timeout_ms);
int  tls_peer(int handle, ktlspeer_t *out);
int  log_write(int level, const char *subsys, const char *msg);

/* BSD/OS string helpers web_fetch.c uses that the host libc may lack. */
size_t strlcpy(char *d, const char *s, size_t cap);
size_t strlcat(char *d, const char *s, size_t cap);
int    strcasecmp(const char *a, const char *b);
int    strncasecmp(const char *a, const char *b, size_t n);

#endif
