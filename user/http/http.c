/* http - fetch a URL.
 *
 *   http <url> [-o file] [-q]
 *
 * Both http:// and https:// work.  The secure case is the interesting one: it
 * proves the certificate the server presented was issued, through a chain of
 * signatures, by an authority this machine already trusted, and that the
 * server holds the private key that goes with it.  When it prints who the
 * certificate belongs to, that is not decoration - it is the result of the
 * check, and nothing gets printed if the check did not pass.
 *
 * This is also the honest test of the whole stack beneath it: a page that
 * comes back whole over TLS means the TCP implementation, the record layer,
 * the key schedule, the signature verification and the certificate chain are
 * all correct, against a server that is no part of this system.
 */
#include "kestrel.h"
#include "web.h"

static bool quiet;

static void show_progress(const char *what, void *ctx) {
    (void)ctx;
    if (!quiet) printf("http: %s\n", what);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: http <url> [-o file] [-q]\n");
        printf("       http https://example.com/\n");
        return 1;
    }

    const char *out_path = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out_path = argv[++i];
        else if (!strcmp(argv[i], "-q")) quiet = true;
    }

    web_on_progress(show_progress, NULL);

    web_response_t response;
    if (!web_fetch(argv[1], 5, &response)) {
        printf("http: %s\n", response.error);
        if (!strncmp(argv[1], "https", 5))
            printf("      `log` shows what the secure connection objected to.\n");
        web_free(&response);
        return 1;
    }

    if (response.secure) {
        printf("http: secure - %s\n", response.peer.cipher);
        printf("      the certificate belongs to %s%s%s\n",
               response.peer.subject[0] ? response.peer.subject : "(unnamed)",
               response.peer.organisation[0] ? ", " : "",
               response.peer.organisation[0] ? response.peer.organisation : "");
        printf("      issued by %s, valid until %u-%02u-%02u\n",
               response.peer.issuer[0] ? response.peer.issuer : "(unnamed)",
               response.peer.valid_until_year, response.peer.valid_until_month,
               response.peer.valid_until_day);
    }

    printf("http: %d, %d bytes%s%s\n", response.status, response.body_len,
           response.content_type[0] ? ", " : "", response.content_type);

    if (out_path) {
        int fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            printf("http: cannot write %s\n", out_path);
            web_free(&response);
            return 1;
        }
        int written = (int)write(fd, response.body, (size_t)response.body_len);
        close(fd);
        if (written != response.body_len) {
            printf("http: only %d of %d bytes reached %s\n", written,
                   response.body_len, out_path);
            web_free(&response);
            return 1;
        }
        printf("http: saved to %s\n", out_path);
    } else {
        /* Straight to the screen, with a limit: a page is often far more than
         * a terminal can usefully show. */
        int show = response.body_len;
        if (show > 4000) show = 4000;
        for (int i = 0; i < show; i++) putchar(response.body[i]);
        if (show < response.body_len)
            printf("\n... %d more bytes; use -o to save the whole page\n",
                   response.body_len - show);
        else if (show && response.body[show - 1] != '\n') putchar('\n');
    }

    web_free(&response);
    return 0;
}
