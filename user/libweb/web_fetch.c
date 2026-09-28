/* web_fetch.c - getting a page.
 *
 * The protocol is simple enough to state in a paragraph: open a connection,
 * send a line saying which page you want and a few lines of headers, and read
 * back a status line, more headers, a blank line, and the page.  Almost
 * everything here is the handling of the ways that goes slightly differently
 * in practice - the response arriving in pieces, the body sent in chunks with
 * their own lengths, the server answering "not here, look over there", and the
 * whole thing possibly happening inside an encrypted connection.
 */
#include "web.h"

static web_progress_t progress_fn;
static void          *progress_ctx;

void web_on_progress(web_progress_t fn, void *ctx) {
    progress_fn = fn;
    progress_ctx = ctx;
}

static void progress(const char *what) {
    if (progress_fn) progress_fn(what, progress_ctx);
}

/* ------------------------------------------------------------- addresses */

static bool is_scheme_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

/* Collapse "." and ".." the way a path is meant to be read, so that a link to
 * "../style.css" from /a/b/page.html means /a/style.css and cannot be used to
 * climb above the root. */
static void normalise_path(char *path) {
    char out[768];
    int at = 0;
    const char *p = path;

    while (*p) {
        if (p[0] == '/' && p[1] == '.' && p[2] == '/') { p += 2; continue; }
        if (p[0] == '/' && p[1] == '.' && p[2] == 0) { p += 2; break; }
        if (p[0] == '/' && p[1] == '.' && p[2] == '.' && (p[3] == '/' || p[3] == 0)) {
            while (at > 0 && out[--at] != '/') { }
            p += (p[3] == '/') ? 3 : 3;
            if (p[0] == 0) break;
            continue;
        }
        if (at < (int)sizeof out - 1) out[at++] = *p;
        p++;
    }
    if (at == 0) out[at++] = '/';
    out[at] = 0;
    strlcpy(path, out, 768);
}

bool web_parse_url(const char *url, const web_url_t *base, web_url_t *out) {
    memset(out, 0, sizeof *out);

    /* Is there a scheme?  "http:" and "https:" are the two that mean anything
     * here; a colon inside a path is not a scheme. */
    const char *colon = url;
    while (is_scheme_char(*colon)) colon++;
    bool has_scheme = (*colon == ':' && colon > url && colon[1] == '/' && colon[2] == '/');

    if (!has_scheme) {
        if (!base) {
            /* A bare "example.com" typed into a bar is meant as an address,
             * and treating it as one is what everybody expects. */
            strlcpy(out->scheme, "http", sizeof out->scheme);
            out->port = 80;
            out->secure = false;

            const char *slash = strchr(url, '/');
            size_t host_len = slash ? (size_t)(slash - url) : strlen(url);
            if (!host_len || host_len >= sizeof out->host) return false;
            memcpy(out->host, url, host_len);
            out->host[host_len] = 0;
            strlcpy(out->path, slash ? slash : "/", sizeof out->path);
        } else {
            /* Relative to where we are. */
            *out = *base;
            if (url[0] == '/') {
                strlcpy(out->path, url, sizeof out->path);
            } else if (url[0] == '#') {
                /* A place within the same page. */
                return true;
            } else {
                char joined[768];
                strlcpy(joined, base->path, sizeof joined);
                char *last = strrchr(joined, '/');
                if (last) last[1] = 0;
                else strlcpy(joined, "/", sizeof joined);
                strlcat(joined, url, sizeof joined);
                strlcpy(out->path, joined, sizeof out->path);
            }
        }
    } else {
        size_t scheme_len = (size_t)(colon - url);
        if (scheme_len >= sizeof out->scheme) return false;
        memcpy(out->scheme, url, scheme_len);
        out->scheme[scheme_len] = 0;

        if (!strcasecmp(out->scheme, "https")) { out->secure = true; out->port = 443; }
        else if (!strcasecmp(out->scheme, "http")) { out->secure = false; out->port = 80; }
        else return false;

        const char *rest = colon + 3;
        const char *slash = strchr(rest, '/');
        const char *port_colon = strchr(rest, ':');
        if (port_colon && slash && port_colon > slash) port_colon = NULL;

        size_t host_len = port_colon ? (size_t)(port_colon - rest)
                        : slash ? (size_t)(slash - rest) : strlen(rest);
        if (!host_len || host_len >= sizeof out->host) return false;
        memcpy(out->host, rest, host_len);
        out->host[host_len] = 0;

        if (port_colon) {
            int port = atoi(port_colon + 1);
            if (port <= 0 || port > 65535) return false;
            out->port = (uint16_t)port;
        }

        strlcpy(out->path, slash ? slash : "/", sizeof out->path);
    }

    /* A fragment is for the browser, never for the server. */
    char *hash = strchr(out->path, '#');
    if (hash) *hash = 0;
    if (!out->path[0]) strlcpy(out->path, "/", sizeof out->path);

    normalise_path(out->path);
    return out->host[0] != 0;
}

void web_format_url(const web_url_t *url, char *out, size_t cap) {
    bool default_port = (url->secure && url->port == 443) ||
                        (!url->secure && url->port == 80);
    if (default_port)
        snprintf(out, cap, "%s://%s%s", url->scheme, url->host, url->path);
    else
        snprintf(out, cap, "%s://%s:%u%s", url->scheme, url->host,
                 url->port, url->path);
}

/* --------------------------------------------------------------- headers */

/* Header names are case-insensitive, and enough servers vary the case that
 * comparing them exactly would work on some sites and not others. */
static const char *find_header(const char *headers, int len, const char *name,
                               int *value_len) {
    size_t name_len = strlen(name);
    for (int i = 0; i + (int)name_len + 1 < len; i++) {
        if (i && headers[i - 1] != '\n') continue;
        if (strncasecmp(headers + i, name, name_len)) continue;
        int at = i + (int)name_len;
        while (at < len && (headers[at] == ' ' || headers[at] == '\t')) at++;
        if (at >= len || headers[at] != ':') continue;
        at++;
        while (at < len && (headers[at] == ' ' || headers[at] == '\t')) at++;
        int end = at;
        while (end < len && headers[end] != '\r' && headers[end] != '\n') end++;
        *value_len = end - at;
        return headers + at;
    }
    return NULL;
}

static void copy_header(const char *headers, int len, const char *name,
                        char *out, size_t cap) {
    int value_len = 0;
    const char *value = find_header(headers, len, name, &value_len);
    out[0] = 0;
    if (!value) return;
    if ((size_t)value_len >= cap) value_len = (int)cap - 1;
    memcpy(out, value, (size_t)value_len);
    out[value_len] = 0;
}

static bool header_is(const char *headers, int len, const char *name,
                      const char *wanted) {
    int value_len = 0;
    const char *value = find_header(headers, len, name, &value_len);
    if (!value) return false;
    size_t wanted_len = strlen(wanted);
    return (size_t)value_len >= wanted_len &&
           !strncasecmp(value, wanted, wanted_len);
}

/* ---------------------------------------------------------------- chunks
 *
 * A server that does not know how long the page will be sends it in pieces,
 * each preceded by its length in hexadecimal and ended by one of length zero.
 * Undoing that in place is safe because the result is always shorter.
 */
static int undo_chunking(char *body, int len) {
    int read = 0, written = 0;

    while (read < len) {
        int length = 0;
        bool any = false;
        while (read < len) {
            char c = body[read];
            int digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else break;
            length = length * 16 + digit;
            read++;
            any = true;
        }
        if (!any) break;

        while (read < len && body[read] != '\n') read++;   /* any extension */
        if (read < len) read++;

        if (length == 0) break;                            /* the last one */
        if (read + length > len) length = len - read;
        if (length <= 0) break;

        memmove(body + written, body + read, (size_t)length);
        written += length;
        read += length;

        while (read < len && (body[read] == '\r' || body[read] == '\n')) read++;
    }
    return written;
}

/* ---------------------------------------------------------------- fetching */

#define RESPONSE_LIMIT (4 * 1024 * 1024)

/* What to send: a plain GET when this is NULL, otherwise the method and body
 * named here.  Kept internal - callers reach it through web_fetch (GET) or
 * web_fetch_post.  The body is not owned; it must outlive the call. */
typedef struct {
    const char *method;        /* "POST" - GET is the NULL case            */
    const char *body;
    int         body_len;
    const char *content_type;  /* NULL -> application/x-www-form-urlencoded */
} web_send_t;

static bool fetch_once(const web_url_t *url, const web_send_t *send,
                       web_response_t *out,
                       char *location, size_t location_cap) {
    char note[320];
    snprintf(note, sizeof note, "Looking up %s", url->host);
    progress(note);

    uint32_t ip = net_resolve(url->host, 5000);
    if (!ip) {
        snprintf(out->error, sizeof out->error,
                 "Could not find a machine called %s.", url->host);
        return false;
    }

    snprintf(note, sizeof note, "Connecting to %s", url->host);
    progress(note);

    int sock;
    if (url->secure) {
        sock = tls_connect(ip, url->port, url->host, 15000);
        if (sock < 0) {
            /* The kernel logged the specific reason; what reaches here is that
             * the secure connection did not happen. */
            snprintf(out->error, sizeof out->error,
                     "Could not establish a secure connection to %s. "
                     "The system log says why.", url->host);
            return false;
        }
        out->secure = true;
        tls_peer(sock, &out->peer);
    } else {
        sock = tcp_connect(ip, url->port, 8000);
        if (sock < 0) {
            snprintf(out->error, sizeof out->error,
                     "%s did not answer.", url->host);
            return false;
        }
        out->secure = false;
    }

    /* The request line and headers.  A POST carries a body, so it also carries
     * the two headers that describe it - Content-Type and Content-Length - and
     * the body itself is sent right after this header block. */
    char request[1200];
    int n;
    if (send && send->method) {
        const char *ctype = send->content_type
                          ? send->content_type
                          : "application/x-www-form-urlencoded";
        n = snprintf(request, sizeof request,
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: Kestrel/1.0 (KestrelOS)\r\n"
                     "Accept: text/html,text/plain,*/*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %d\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     send->method, url->path, url->host, ctype, send->body_len);
    } else {
        n = snprintf(request, sizeof request,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: Kestrel/1.0 (KestrelOS)\r\n"
                     "Accept: text/html,text/plain,*/*\r\n"
                     "Accept-Encoding: identity\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     url->path, url->host);
    }

    int sent = tcp_send(sock, request, n, 8000);
    if (sent != n) {
        snprintf(out->error, sizeof out->error, "The request could not be sent.");
        char detail[192];
        snprintf(detail, sizeof detail, "%s: sent %d of %d request bytes",
                 url->host, sent, n);
        log_write(2, "web", detail);
        tcp_close(sock);
        return false;
    }

    /* The body follows the header block on the same connection. */
    if (send && send->method && send->body_len > 0) {
        int bsent = tcp_send(sock, (char *)send->body, send->body_len, 8000);
        if (bsent != send->body_len) {
            snprintf(out->error, sizeof out->error,
                     "The request could not be sent.");
            char detail[192];
            snprintf(detail, sizeof detail, "%s: sent %d of %d body bytes",
                     url->host, bsent, send->body_len);
            log_write(2, "web", detail);
            tcp_close(sock);
            return false;
        }
    }

    progress("Waiting for a reply");

    /* The response is read until the far end closes, which is what asking for
     * Connection: close arranges.  It grows as it goes: a page can be anything
     * from a few hundred bytes to a few megabytes, and guessing wrong in
     * either direction costs either a truncated page or a wasted megabyte. */
    int capacity = 65536;
    char *response = malloc((size_t)capacity);
    if (!response) {
        snprintf(out->error, sizeof out->error, "There is not enough memory.");
        tcp_close(sock);
        return false;
    }

    int total = 0;
    int first_read = 0;
    for (;;) {
        if (total == capacity) {
            if (capacity >= RESPONSE_LIMIT) break;
            int bigger = capacity * 2;
            if (bigger > RESPONSE_LIMIT) bigger = RESPONSE_LIMIT;
            char *grown = realloc(response, (size_t)bigger);
            if (!grown) break;
            response = grown;
            capacity = bigger;
        }
        int got = tcp_recv(sock, response + total, capacity - total, 15000);
        if (!total) first_read = got;
        if (got <= 0) break;
        total += got;

        if ((total & 0xFFFF) == 0 || total < 8192) {
            snprintf(note, sizeof note, "Received %d bytes", total);
            progress(note);
        }
    }
    tcp_close(sock);

    if (total <= 0) {
        snprintf(out->error, sizeof out->error, "Nothing came back from %s.",
                 url->host);
        char detail[192];
        snprintf(detail, sizeof detail,
                 "%s: the request went out but nothing came back (the first "
                 "read returned %d)", url->host, first_read);
        log_write(2, "web", detail);
        free(response);
        return false;
    }

    /* Split the headers from the body at the blank line. */
    int header_len = total;
    for (int i = 0; i + 3 < total; i++) {
        if (response[i] == '\r' && response[i + 1] == '\n' &&
            response[i + 2] == '\r' && response[i + 3] == '\n') {
            header_len = i + 4;
            break;
        }
    }

    if (total < 12 || strncmp(response, "HTTP/", 5)) {
        snprintf(out->error, sizeof out->error,
                 "%s answered with something that is not a web page.", url->host);
        free(response);
        return false;
    }
    const char *space = strchr(response, ' ');
    out->status = space ? atoi(space + 1) : 0;

    copy_header(response, header_len, "Content-Type",
                out->content_type, sizeof out->content_type);
    copy_header(response, header_len, "Location", location, location_cap);

    char *body = response + header_len;
    int body_len = total - header_len;

    if (header_is(response, header_len, "Transfer-Encoding", "chunked"))
        body_len = undo_chunking(body, body_len);

    /* Hand back just the body, in its own allocation, so the caller does not
     * have to know where the headers ended. */
    char *page = malloc((size_t)body_len + 1);
    if (!page) {
        snprintf(out->error, sizeof out->error, "There is not enough memory.");
        free(response);
        return false;
    }
    memcpy(page, body, (size_t)body_len);
    page[body_len] = 0;
    free(response);

    out->body = page;
    out->body_len = body_len;
    return true;
}

static bool fetch_loop(web_url_t url, web_send_t send, int redirect_limit,
                       web_response_t *out) {
    /* A GET has no method set; a POST does.  Following a redirect can change a
     * POST into a GET (see below), so `send` is a local copy we can edit. */
    bool have_body = send.method != NULL;

    for (int hop = 0; hop <= redirect_limit; hop++) {
        char location[WEB_MAX_URL];
        location[0] = 0;

        if (out->body) { free(out->body); out->body = NULL; }
        if (!fetch_once(&url, have_body ? &send : NULL, out,
                        location, sizeof location))
            return false;

        web_format_url(&url, out->final_url, sizeof out->final_url);

        bool redirect = (out->status == 301 || out->status == 302 ||
                         out->status == 303 || out->status == 307 ||
                         out->status == 308);
        if (!redirect || !location[0]) return true;

        if (hop == redirect_limit) {
            snprintf(out->error, sizeof out->error,
                     "This address kept redirecting and never arrived anywhere.");
            return false;
        }

        web_url_t next;
        if (!web_parse_url(location, &url, &next)) {
            snprintf(out->error, sizeof out->error,
                     "The server redirected to an address this browser cannot read.");
            return false;
        }

        /* Following a redirect from a secure page to a plain one silently
         * would undo the security the first connection provided. */
        if (url.secure && !next.secure) {
            snprintf(out->error, sizeof out->error,
                     "This secure page tried to send the browser to an "
                     "unencrypted address, which was not followed.");
            return false;
        }

        url = next;
        snprintf(out->final_url, sizeof out->final_url, "%s", location);

        /* A 303 always continues as GET; browsers also turn the 301/302 of a
         * POST into a GET (fetching the named page, not re-submitting).  Only
         * 307 and 308 repeat the original method and body. */
        if (have_body && out->status != 307 && out->status != 308)
            have_body = false;
    }
    return true;
}

bool web_fetch(const char *url_text, int redirect_limit, web_response_t *out) {
    memset(out, 0, sizeof *out);

    web_url_t url;
    if (!web_parse_url(url_text, NULL, &url)) {
        snprintf(out->error, sizeof out->error,
                 "\"%s\" is not an address this browser understands.", url_text);
        return false;
    }

    web_send_t get = {0};
    return fetch_loop(url, get, redirect_limit, out);
}

bool web_fetch_post(const char *url_text, const char *body, int body_len,
                    const char *content_type, int redirect_limit,
                    web_response_t *out) {
    memset(out, 0, sizeof *out);

    web_url_t url;
    if (!web_parse_url(url_text, NULL, &url)) {
        snprintf(out->error, sizeof out->error,
                 "\"%s\" is not an address this browser understands.", url_text);
        return false;
    }

    web_send_t post = {
        .method       = "POST",
        .body         = body,
        .body_len     = body_len,
        .content_type = content_type,
    };
    return fetch_loop(url, post, redirect_limit, out);
}

void web_free(web_response_t *r) {
    if (r->body) free(r->body);
    r->body = NULL;
    r->body_len = 0;
}
