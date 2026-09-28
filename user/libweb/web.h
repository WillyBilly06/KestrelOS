/* web.h - fetching a URL.
 *
 * Both the command line tool and the browser need the same thing: give it an
 * address, get back a page or a reason why not.  Neither should have to know
 * whether the connection was encrypted, how many redirects were followed, or
 * how the response was chunked - that is all this library's business.
 *
 * See web_fetch.c.
 */
#ifndef KESTREL_WEB_H
#define KESTREL_WEB_H

#include "kestrel.h"

#define WEB_MAX_URL 1024

typedef struct {
    char     scheme[8];
    char     host[256];
    char     path[768];
    uint16_t port;
    bool     secure;
} web_url_t;

/* Break an address apart.  `base` may be NULL; when it is not, a relative
 * address is resolved against it, which is what following a link means. */
bool web_parse_url(const char *url, const web_url_t *base, web_url_t *out);

/* Put one back together. */
void web_format_url(const web_url_t *url, char *out, size_t cap);

typedef struct {
    int   status;                  /* 200, 404, and so on; 0 if none arrived */
    char *body;                    /* freed by web_free */
    int   body_len;

    char  content_type[96];
    char  final_url[WEB_MAX_URL];  /* after any redirects */
    char  error[192];              /* empty when the fetch worked */

    bool       secure;
    ktlspeer_t peer;               /* only meaningful when secure */
} web_response_t;

/* Fetch, following redirects up to `redirect_limit` times.  Returns false and
 * fills `error` on failure; the caller shows that sentence. */
bool web_fetch(const char *url, int redirect_limit, web_response_t *out);

/* Like web_fetch, but sends a POST with the given body.  content_type may be
 * NULL, meaning application/x-www-form-urlencoded (what an HTML form sends).
 * A redirect turns the POST into a GET except for 307/308, as browsers do. */
bool web_fetch_post(const char *url, const char *body, int body_len,
                    const char *content_type, int redirect_limit,
                    web_response_t *out);

void web_free(web_response_t *r);

/* Progress, for a caller that wants to show something while it waits. */
typedef void (*web_progress_t)(const char *what, void *ctx);
void web_on_progress(web_progress_t fn, void *ctx);

#endif
