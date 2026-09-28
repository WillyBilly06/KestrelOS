/* web_post_host_test.c - exercise the real web_fetch.c POST path off-target.
 *
 * A POST that sends the wrong method line, a wrong Content-Length, or the body
 * before the blank line reaches a server as a broken request, and there is no
 * good place on real hardware to notice that.  This runs the unmodified
 * user/libweb/web_fetch.c against stub sockets that capture exactly what it
 * would put on the wire, and scripts the responses - so it proves the request
 * bytes, and that a redirect changes the method the way browsers do (307/308
 * repeat the POST, everything else becomes a GET without the body).
 *
 * Build + run (from the repo root), forcing the shim ahead of web.h:
 *   clang -std=c11 -Wall -Wextra -include tools/web_test_shim.h \
 *         tools/web_post_host_test.c user/libweb/web_fetch.c -o web_post_test
 *   ./web_post_test
 */
#include "web_test_shim.h"
#include <ctype.h>

/* ---- host-backed BSD string helpers ------------------------------------- */
size_t strlcpy(char *d, const char *s, size_t cap) {
    size_t n = strlen(s);
    if (cap) { size_t c = n < cap - 1 ? n : cap - 1; memcpy(d, s, c); d[c] = 0; }
    return n;
}
size_t strlcat(char *d, const char *s, size_t cap) {
    size_t dl = strlen(d);
    if (dl >= cap) return cap + strlen(s);
    return dl + strlcpy(d + dl, s, cap - dl);
}
int strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) { int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y; if (!x) return 0; }
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y) return x - y; if (!x) return 0; } return 0;
}

/* ---- capture buffer + scripted responses -------------------------------- */
static char  sent[8192]; static int sent_len;
static const char *script[4]; static int script_n, script_i;
static int recv_done;               /* whether this connection's body was read */

uint32_t net_resolve(const char *h, int t) { (void)h; (void)t; return 0x7f000001; }
int  tcp_connect(uint32_t ip, uint16_t p, int t) { (void)ip; (void)p; (void)t;
     memset(sent, 0, sizeof sent); sent_len = 0; recv_done = 0; return 3; }
int  tls_connect(uint32_t ip, uint16_t p, const char *h, int t) {
     (void)ip; (void)p; (void)h; (void)t;
     memset(sent, 0, sizeof sent); sent_len = 0; recv_done = 0; return 4; }
int  tls_peer(int h, ktlspeer_t *o) { (void)h; if (o) o->dummy = 1; return 0; }
void tcp_close(int h) { (void)h; }
int  log_write(int l, const char *s, const char *m) { (void)l; (void)s; (void)m; return 0; }

int tcp_send(int h, const void *d, int n, int t) { (void)h; (void)t;
    if (sent_len + n < (int)sizeof sent) { memcpy(sent + sent_len, d, n); sent_len += n; }
    return n;                       /* the stub always accepts the whole write */
}
int tcp_recv(int h, void *buf, int n, int t) { (void)h; (void)t;
    if (recv_done) return 0;         /* EOF: the far end closed (Connection: close) */
    const char *r = script[script_i < script_n ? script_i : script_n - 1];
    int len = (int)strlen(r); if (len > n) len = n;
    memcpy(buf, r, len); recv_done = 1; script_i++; return len;
}

/* ---- assertions --------------------------------------------------------- */
static int fails = 0;
static int contains(const char *h, const char *n) { return strstr(h, n) != NULL; }
static int header_line(const char *name, const char *val) {
    char pat[128]; snprintf(pat, sizeof pat, "%s: %s\r\n", name, val); return contains(sent, pat);
}
static const char *body_after_headers(void) {
    char *p = strstr(sent, "\r\n\r\n"); return p ? p + 4 : sent + sent_len;
}
#define OK(cond, msg) do { if (cond) printf("  PASS %s\n", msg); \
    else { printf("  FAIL %s\n", msg); fails++; } } while (0)

int main(void) {
    web_response_t r;
    const char *form = "user=alice&pass=hunter2&note=a+b";

    /* 1. a plain POST carries the method, the describing headers and the body. */
    script[0] = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                "Connection: close\r\n\r\nthanks"; script_n = 1; script_i = 0;
    memset(&r, 0, sizeof r);
    bool ok = web_fetch_post("http://host.test/login", form, (int)strlen(form),
                             NULL, 5, &r);
    printf("test 1: plain POST (status=%d, ok=%d)\n", r.status, ok);
    OK(ok && r.status == 200, "fetch succeeded with 200");
    OK(strncmp(sent, "POST /login HTTP/1.1\r\n", 22) == 0, "request line is POST /login");
    OK(header_line("Content-Type", "application/x-www-form-urlencoded"),
       "default Content-Type is form-urlencoded");
    { char cl[64]; snprintf(cl, sizeof cl, "%d", (int)strlen(form));
      OK(header_line("Content-Length", cl), "Content-Length equals body length"); }
    OK(strcmp(body_after_headers(), form) == 0, "body sent verbatim after blank line");
    OK(contains(sent, "Host: host.test\r\n"), "Host header present");
    web_free(&r);

    /* 2. an explicit content type is honoured. */
    script[0] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nok"; script_n = 1; script_i = 0;
    memset(&r, 0, sizeof r);
    const char *json = "{\"a\":1}";
    web_fetch_post("http://host.test/api", json, (int)strlen(json),
                   "application/json", 5, &r);
    printf("test 2: explicit content-type\n");
    OK(header_line("Content-Type", "application/json"), "Content-Type honoured");
    OK(strcmp(body_after_headers(), json) == 0, "json body verbatim");
    web_free(&r);

    /* 3. a 307 repeats the POST and body on the new location. */
    script[0] = "HTTP/1.1 307 Temporary Redirect\r\n"
                "Location: http://host.test/v2\r\nConnection: close\r\n\r\n";
    script[1] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\ndone";
    script_n = 2; script_i = 0; memset(&r, 0, sizeof r);
    web_fetch_post("http://host.test/api", form, (int)strlen(form), NULL, 5, &r);
    printf("test 3: 307 keeps method+body (status=%d)\n", r.status);
    OK(r.status == 200, "followed the redirect to 200");
    OK(strncmp(sent, "POST /v2 HTTP/1.1\r\n", 19) == 0, "second request is still POST /v2");
    OK(strcmp(body_after_headers(), form) == 0, "body resent on 307");
    web_free(&r);

    /* 4. a 303 downgrades to GET and drops the body. */
    script[0] = "HTTP/1.1 303 See Other\r\n"
                "Location: http://host.test/welcome\r\nConnection: close\r\n\r\n";
    script[1] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhi";
    script_n = 2; script_i = 0; memset(&r, 0, sizeof r);
    web_fetch_post("http://host.test/login", form, (int)strlen(form), NULL, 5, &r);
    printf("test 4: 303 downgrades to GET (status=%d)\n", r.status);
    OK(r.status == 200, "followed the 303 to 200");
    OK(strncmp(sent, "GET /welcome HTTP/1.1\r\n", 23) == 0, "second request is GET /welcome");
    OK(!contains(sent, "Content-Length:"), "no body/Content-Length on the GET");
    web_free(&r);

    /* 5. a plain GET is unchanged by any of this. */
    script[0] = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npage"; script_n = 1; script_i = 0;
    memset(&r, 0, sizeof r);
    web_fetch("http://host.test/index", 5, &r);
    printf("test 5: GET regression (status=%d)\n", r.status);
    OK(strncmp(sent, "GET /index HTTP/1.1\r\n", 21) == 0, "GET request line intact");
    OK(!contains(sent, "Content-Length:"), "GET carries no Content-Length");
    OK(r.body && strcmp(r.body, "page") == 0, "GET body decoded");
    web_free(&r);

    printf("\n%s (%d failures)\n", fails ? "FAILURES" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
