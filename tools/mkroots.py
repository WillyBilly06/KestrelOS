#!/usr/bin/env python3
"""Turn a bundle of certificate authorities into something the kernel can read.

A browser has to decide whether the certificate a site presents traces back to
someone worth believing.  That question has no answer without a starting list,
and the starting list is not something a machine can work out for itself - it
is a decision about who to trust, and it has to come from somewhere.

The list here is the one Mozilla maintains for Firefox, which is the same list
most of the world's software ends up using.  It arrives as text; the kernel
would rather not carry a text parser and a base64 decoder just to read it, so
it is converted here, once, at build time.

All of them are kept.  This once excluded the elliptic-curve authorities,
because a signature that cannot be checked is not a starting point for
anything and listing one would have created the impression of a check that
never happened.  That reservation no longer applies: both curves in use are
verified now, so leaving those authorities out would exclude a large part of
the web for no reason.
"""

import base64
import os
import re
import struct

MAGIC = b"KROOTS\x00\x01"

# The object identifier for rsaEncryption, as it appears inside a key.
RSA_OID = bytes.fromhex("2a864886f70d010101")


def parse_pem(text):
    """Every certificate in a bundle, as DER."""
    blocks = re.findall(
        r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", text, re.S
    )
    out = []
    for block in blocks:
        try:
            out.append(base64.b64decode("".join(block.split())))
        except Exception:
            pass
    return out


def build(pem_path, out_path):
    with open(pem_path, encoding="utf-8") as f:
        certificates = parse_pem(f.read())

    kept = certificates
    skipped = 0

    body = b""
    for der in kept:
        body += struct.pack("<I", len(der)) + der
        # Four-byte alignment, so the kernel can walk the file without ever
        # reading a length from an unaligned address.
        while len(body) % 4:
            body += b"\x00"

    with open(out_path, "wb") as f:
        f.write(MAGIC)
        f.write(struct.pack("<II", len(kept), len(body)))
        f.write(body)

    return len(kept), skipped, os.path.getsize(out_path)


if __name__ == "__main__":
    import sys

    if len(sys.argv) != 3:
        print("usage: mkroots.py <cacert.pem> <roots.bin>")
        raise SystemExit(2)
    count, _, size = build(sys.argv[1], sys.argv[2])
    print("%d authorities, %d bytes" % (count, size))
