#!/usr/bin/env python3
"""Run production HID attach/idle/report code against mocked USB transport.

Checks protocol ownership, idle across a long GPU boot, and first input without
replugging. Does not emulate xHCI or prove physical USB transfers.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SRC = (ROOT / "kernel/usbhid.c").read_text()
USB = (ROOT / "kernel/usb.h").read_text()


def function(name):
    match = re.search(r"^(?:static )?(?:void\s*\*?|bool|int)\s*" + name +
                      r"\([^;]*?\)\s*\{", SRC, re.M)
    assert match, name
    return SRC[match.start():SRC.index("\n}", match.end()) + 2]


def main():
    source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
typedef uint64_t u64; typedef int32_t s32;
typedef struct { int controller; } usb_device_t;
typedef struct { u8 number, dev_class, subclass, protocol; u16 report_desc_len; } usb_interface_t;
#define kmalloc malloc
#define kfree free
#define kwarn(...) ((void)0)
#define kinfo(...) ((void)0)
'''
    for src, names in ((SRC, ("MAX_HID", "HID_MAX_FIELDS", "HID_MAX_KEYS", "REPEAT_RATE_MS")),
                       (USB, ("USB_CLASS_HID", "HID_SUBCLASS_BOOT", "HID_PROTO_KEYBOARD",
                              "HID_PROTO_MOUSE", "USB_DIR_IN", "USB_DIR_OUT",
                              "USB_TYPE_STANDARD", "USB_TYPE_CLASS", "USB_RECIP_INTERFACE",
                              "USB_REQ_GET_DESCRIPTOR", "USB_DT_REPORT",
                              "HID_REQ_SET_PROTOCOL", "HID_REQ_SET_IDLE"))):
        for name in names:
            source += re.search(r"^#define\s+" + name + r"\s+[^\n]+", src, re.M)[0] + "\n"
    source += SRC[SRC.index("typedef struct {"):SRC.index("\nint hid_mouse_source(")]
    source += r'''
static u64 g_uptime_ms;
static int protocol_calls, idle_calls, last_protocol, keyboards, pointers;
static int present, key_events, descriptor_reports, boot_reports;
static bool fail_protocol, parse_ok = true;
static bool parsed_keyboard, parsed_pointer, parsed_ids;
static int usb_control(usb_device_t *dev, int type, int request, int value,
                       int interface, void *data, int length) {
    assert(dev && interface == 3);
    if (request == USB_REQ_GET_DESCRIPTOR) {
        assert(type == (USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE));
        memset(data, 0, length); return length;
    }
    assert(type == (USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE));
    if (request == HID_REQ_SET_PROTOCOL) {
        protocol_calls++; last_protocol = value;
        return fail_protocol ? -1 : 0;
    }
    assert(request == HID_REQ_SET_IDLE && value == 0);
    idle_calls++; return 0;
}
static bool hid_parse_descriptor(hid_t *h, const u8 *data, int length) {
    assert(data && length == 8);
    h->is_keyboard = parsed_keyboard; h->is_pointer = parsed_pointer;
    h->uses_report_ids = parsed_ids; h->nfields = 3;
    return parse_ok;
}
static void usb_count_hid(int k, int p) { keyboards += k; pointers += p; }
static void mouse_set_present(void) { present++; }
static void input_inject_key(u16 code, u8 mods, bool pressed) {
    assert(code == 42 && mods == 1); (void)pressed; key_events++;
}
static void hid_handle_report(hid_t *h, const u8 *data, int length) {
    assert(h->nfields == 3 && data && length == 8); descriptor_reports++;
}
static void boot_report(hid_t *h, const u8 *data, int length) {
    assert(h->boot_mode && !h->nfields && !h->uses_report_ids);
    assert(data && length == 8); boot_reports++;
}
'''
    source += "\n".join(function(n) for n in ("usbhid_probe", "usbhid_tick", "usbhid_detach"))
    source += "\nu32 g_hid_reports, g_hid_rejected; u8 g_hid_last[8], g_hid_last_len;\n"
    source += function("usbhid_report")
    source += r'''
static void reset(void) {
    memset(slots, 0, sizeof slots); g_uptime_ms = 7000;
    protocol_calls = idle_calls = keyboards = pointers = present = key_events = 0;
    descriptor_reports = boot_reports = 0; last_protocol = -1;
    fail_protocol = false; parse_ok = true;
    parsed_keyboard = parsed_pointer = parsed_ids = false;
}
int main(void) {
    usb_device_t dev = {0};
    usb_interface_t ifc = {3, USB_CLASS_HID, HID_SUBCLASS_BOOT, HID_PROTO_MOUSE, 8};
    u8 report[8] = {0};
    for (int keyboard = 0; keyboard < 2; keyboard++) {
        reset(); parsed_keyboard = keyboard; parsed_pointer = !keyboard; parsed_ids = true;
        ifc.protocol = keyboard ? HID_PROTO_KEYBOARD : HID_PROTO_MOUSE;
        hid_t *h = usbhid_probe(&dev, &ifc);
        assert(h && h->used && !h->boot_mode && h->uses_report_ids);
        assert(protocol_calls == 1 && last_protocol == 1 && idle_calls == 1);
        assert(keyboards == keyboard && pointers == !keyboard && present == !keyboard);
        /* Includes the old three-second fallback threshold and long GPU tests.
         * Two timer callers model the two host controllers in the boot log. */
        for (int ms = 0; ms < 180000; ms += 10) {
            g_uptime_ms += 10; usbhid_tick(); usbhid_tick();
        }
        assert(protocol_calls == 1 && idle_calls == 1 && !h->boot_mode && h->nfields == 3);
        usbhid_report(h, report, sizeof report);
        assert(h->reports_seen == 1 && descriptor_reports == 1 && !boot_reports);
        h->repeat_key = 42; h->repeat_mods = 1; h->repeat_at = g_uptime_ms;
        usbhid_tick(); usbhid_tick(); assert(key_events == 1);
        g_uptime_ms += REPEAT_RATE_MS; usbhid_tick(); assert(key_events == 2);
        usbhid_detach(h); assert(!h->used && !keyboards && !pointers && key_events == 3);
    }
    /* Partial/failed parse must not leave report fields in a boot decoder. */
    reset(); parse_ok = false; parsed_keyboard = parsed_pointer = parsed_ids = true;
    ifc.protocol = HID_PROTO_MOUSE;
    hid_t *h = usbhid_probe(&dev, &ifc);
    assert(h && h->boot_mode && !h->is_keyboard && h->is_pointer);
    assert(last_protocol == 0 && !h->nfields && !h->uses_report_ids);
    usbhid_report(h, report, sizeof report); assert(boot_reports == 1);
    /* A protocol byte alone does not advertise the boot subclass. */
    reset(); ifc.subclass = 0; parse_ok = false; parsed_pointer = true;
    assert(!usbhid_probe(&dev, &ifc) && !protocol_calls && !idle_calls);
    reset(); parsed_pointer = true; parsed_ids = true;
    h = usbhid_probe(&dev, &ifc);
    assert(h && !h->boot_mode && !protocol_calls && idle_calls == 1);
    /* Never claim an endpoint whose wire format could not be established. */
    reset(); ifc.subclass = HID_SUBCLASS_BOOT; parsed_pointer = true; fail_protocol = true;
    assert(!usbhid_probe(&dev, &ifc) && !slots[0].used && !pointers && !idle_calls);
    reset(); ifc.report_desc_len = 0;
    h = usbhid_probe(&dev, &ifc); assert(h && h->boot_mode && last_protocol == 0);
    puts("HID startup: protocol selection, 180s idle, first report, repeat, detach and failures PASS");
    return 0;
}
'''
    clang = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
    with tempfile.TemporaryDirectory(prefix="kestrel-hid-test-") as tmp:
        c = Path(tmp) / "hid_startup.c"
        exe = Path(tmp) / "hid_startup.exe"
        c.write_text(source)
        subprocess.run([clang, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                        str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
