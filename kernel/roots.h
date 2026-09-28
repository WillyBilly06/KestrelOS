/* roots.h - the list of certificate authorities, and the chain check.
 *
 * See roots.c.  There is one entry point that matters: roots_check_chain
 * answers, for a chain a server sent and a host name that was asked for,
 * either NULL - meaning every check passed - or a sentence saying which one
 * did not, in words that can go straight to whoever is looking at the screen.
 */
#ifndef KESTREL_ROOTS_H
#define KESTREL_ROOTS_H

#include "kernel.h"
#include "x509.h"

/* NULL when the chain is good; otherwise what is wrong with it. */
const char *roots_check_chain(const x509_cert_t *chain, int count,
                              const char *host);

/* The authority that signed `child`, if this machine has it. */
bool roots_find_issuer(const x509_cert_t *child, x509_cert_t *out);

/* Is this certificate itself one of the trusted authorities?  Matched on the
 * name and key, so a cross-signed second copy of the same authority counts. */
bool roots_contains(const x509_cert_t *cert, x509_cert_t *out);

int roots_trusted_count(void);
int roots_selftest(void);
int roots_check_selftest(void);

#endif
