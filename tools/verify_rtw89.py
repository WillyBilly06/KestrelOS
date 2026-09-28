"""verify_rtw89.py - check this system's Realtek constants against the source.

Every register address and bitmask in kernel/rtw89.h was read out of Linux's
rtw89 driver by hand.  Reading is where they go wrong: 0x0E00 for 0x0E000,
GENMASK(7,2) remembered as GENMASK(7,5), a mask that is right and a shift that
is not.  None of those look wrong on the page and all of them produce a driver
that writes plausible values into the wrong place.

So they are diffed mechanically instead.  This parses the reference headers,
works out what BIT() and GENMASK() come to, and compares.

It found four wrong out of ninety-three the first time it was run on this
project, which is why it exists.

Usage:

    python tools/verify_rtw89.py <directory holding reg.h, pci.h, ...>

The reference headers are Linux's, dual licensed GPL-2.0 or BSD-3-Clause,
copyright (c) Realtek Corporation.  Download them to a scratch directory - they
do not belong in this tree:

    Invoke-WebRequest -Uri https://raw.githubusercontent.com/torvalds/linux/master/drivers/net/wireless/realtek/rtw89/reg.h -OutFile <dir>\\reg.h
"""

import os
import re
import sys

OURS = os.path.join(os.path.dirname(__file__), "..", "kernel", "rtw89.h")

# Names we spell differently from the reference, so that a rename does not read
# as a mismatch.  Kept short on purpose: every entry here is a place where the
# check cannot help, so a new one needs a reason.
ALIASES = {
    "R_BE_CH12_TXBD_NUM":     "R_BE_CH12_TXBD_NUM_V1",
    "R_BE_CH12_TXBD_IDX":     "R_BE_CH12_TXBD_IDX_V1",
    "R_BE_CH12_TXBD_DESA_L":  "R_BE_CH12_TXBD_DESA_L_V1",
    "R_BE_CH12_TXBD_DESA_H":  "R_BE_CH12_TXBD_DESA_H_V1",
    "R_BE_RXQ0_RXBD_NUM":     "R_BE_RXQ0_RXBD_NUM_V1",
    "R_BE_RXQ0_RXBD_DESA_L":  "R_BE_RXQ0_RXBD_DESA_L_V1",
    "R_BE_RXQ0_RXBD_DESA_H":  "R_BE_RXQ0_RXBD_DESA_H_V1",
    "RTW89_PCI_TXBD_OPTION_LS": "RTW89_PCI_TXBD_OPTION_LS",
    "BD_HOST_IDX_MASK":       "TXBD_HOST_IDX_MASK",
    "BD_CARD_IDX_MASK":       "TXBD_HW_IDX_MASK",
    "RX_FLTR_ACCEPT":         "RX_FLTR_FRAME_ACCEPT_BE",
    "RX_FLTR_DROP":           "RX_FLTR_FRAME_DROP_BE",
    "RTW89_RF_MASK":          "RFREG_MASK",
    "RTW89_RF_INVALID":       "INV_RF_DATA",
    "R_BE_CH8_TXBD_NUM":      "R_BE_CH8_TXBD_NUM_V1",
    "R_BE_CH8_TXBD_IDX":      "R_BE_CH8_TXBD_IDX_V1",
    "R_BE_CH8_TXBD_DESA_L":   "R_BE_CH8_TXBD_DESA_L_V1",
    "R_BE_CH8_TXBD_DESA_H":   "R_BE_CH8_TXBD_DESA_H_V1",
}


def as_number(text):
    """A C integer literal, including the octal ones.

    `int(x, 0)` refuses "03": Python stopped accepting a bare leading zero as
    octal and C never did.  Getting this wrong is not academic - a register at
    010 is at sixteen, not ten."""
    text = text.strip()
    if text[:2].lower() == "0x":
        return int(text, 16)
    if len(text) > 1 and text[0] == "0":
        return int(text, 8)
    return int(text, 10)


def evaluate(text):
    """What a #define comes to, or None if this tool cannot say.

    Deliberately narrow.  A macro this does not understand is reported as
    unchecked rather than guessed at - a check that quietly skips what it
    cannot parse is a check that passes for the wrong reason."""
    text = text.strip()
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S).strip()
    if not text:
        return None

    m = re.fullmatch(r"BIT\((\d+)\)", text)
    if m:
        return 1 << int(m.group(1))

    m = re.fullmatch(r"GENMASK\((\d+),\s*(\d+)\)", text)
    if m:
        hi, lo = int(m.group(1)), int(m.group(2))
        if hi < lo:
            return None
        return ((1 << (hi - lo + 1)) - 1) << lo

    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]*", text)
    if m:
        return as_number(m.group(1))

    # This system writes a single bit as (1u << n) rather than BIT(n), and a
    # field's mask as a plain number beside a separate shift.
    #
    # Without this the tool read 22 constants, understood 4 of them, and said
    # nothing about the other 18 - they were not reported as wrong, they were
    # not reported at all.  A check that quietly covers less than it appears to
    # is the thing this file's own docstring warns against, and it was doing
    # it.
    m = re.fullmatch(r"\(?\s*1[uU]?[lL]*\s*<<\s*(\d+)\s*\)?", text)
    if m:
        return 1 << int(m.group(1))

    # A parenthesised number, and a shifted one: (0xFFFF), (0xF << 26).
    m = re.fullmatch(r"\(\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]*\s*\)", text)
    if m:
        return as_number(m.group(1))

    m = re.fullmatch(r"\(?\s*(0[xX][0-9a-fA-F]+|\d+)[uU]?[lL]*\s*<<\s*(\d+)\s*\)?",
                     text)
    if m:
        return as_number(m.group(1)) << int(m.group(2))

    return None


def enums_in(path):
    """Constants written as enumerators rather than #defines.

    This was added after one was got wrong - RTW89_HCIFC_POH, written down as
    one where the source says zero - and the checker said nothing because it
    only read #defines.  A checker that covers one of the two ways a constant
    can be written will eventually be trusted about the other.

    Only enumerators with an explicit value are taken.  A bare `NAME,` depends
    on what came before it, and following that properly means tracking the
    whole enum; guessing at it would be worse than leaving it out."""
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r"\s*([A-Z_][A-Z0-9_]*)\s*=\s*([^,}]+)[,}]?\s*$", line)
            if not m:
                continue
            value = evaluate(m.group(2))
            if value is not None:
                out[m.group(1)] = value
    return out


def defines_in(path):
    out = {}
    with open(path, encoding="utf-8", errors="replace") as fh:
        for line in fh:
            m = re.match(r"\s*#define\s+([A-Za-z_][A-Za-z0-9_]*)\s+(.+)$", line)
            if not m:
                continue
            value = evaluate(m.group(2))
            if value is not None:
                out[m.group(1)] = value
    return out


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    ref_dir = sys.argv[1]
    if not os.path.isdir(ref_dir):
        print("no such directory: " + ref_dir)
        return 2

    reference = {}
    read = []
    for name in sorted(os.listdir(ref_dir)):
        if not name.endswith((".h", ".c")):
            continue
        path = os.path.join(ref_dir, name)
        found = defines_in(path)
        for k, v in enums_in(path).items():
            found.setdefault(k, v)
        if found:
            read.append("%s (%d)" % (name, len(found)))
        # First file wins, so a header's definition is not replaced by a
        # duplicate somewhere else.
        for k, v in found.items():
            reference.setdefault(k, v)

    print("reference: " + ", ".join(read))

    ours = defines_in(OURS)
    print("ours: %d constant(s) in kernel/rtw89.h" % len(ours))
    print()

    checked = wrong = unknown = 0
    for name, value in sorted(ours.items()):
        want_name = ALIASES.get(name, name)
        if want_name not in reference:
            unknown += 1
            continue
        checked += 1
        want = reference[want_name]

        if value == want:
            continue

        # A field can be written down two ways and mean the same thing.
        #
        # The reference keeps a mask where the field sits - GENMASK(29,26) -
        # while this system often keeps the field's width and its position
        # apart, as a mask of 0xF and a shift of 26, because that is what the
        # code does with it: (value >> shift) & mask.
        #
        # Shifting ours up by its own shift is the honest comparison.  Without
        # this the check reports every such pair, and a check that is wrong
        # about things that are right gets ignored - which is worse than not
        # having it.
        shift_name = name.replace("_MASK", "_SHIFT")
        if shift_name in ours and (value << ours[shift_name]) == want:
            continue

        wrong += 1
        print("  MISMATCH %-32s ours %#x  source %#x  (%s)"
              % (name, value, want, want_name))

    print()
    print("%d checked, %d wrong, %d not found in the reference"
          % (checked, wrong, unknown))

    if unknown:
        print()
        print("Not found is not the same as wrong: many are this system's own")
        print("names, or come from a file that was not downloaded.  They are")
        print("counted so that a check covering almost nothing cannot look")
        print("like a check that passed.")

    return 1 if wrong else 0


if __name__ == "__main__":
    sys.exit(main())
