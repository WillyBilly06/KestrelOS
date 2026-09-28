/* bthid.c - a keyboard or a mouse over Bluetooth.
 *
 * The stack underneath this carried data and nothing used it.  A link that
 * moves bytes is not a keyboard; what makes it one is an agreement about what
 * the bytes mean, and this is that agreement - the one the specification calls
 * HID over GATT and everyone else calls "why my Bluetooth keyboard works".
 *
 * ---------------------------------------------------------------------------
 * WHAT IT REUSES, AND WHY THAT MATTERS MORE THAN IT SOUNDS
 *
 * A device publishes a report descriptor: the same descriptor a USB device
 * publishes, in the same format, meaning the same things.  So this does not
 * parse it.  It hands it to `hid_parse_descriptor`, the parser the USB driver
 * uses, and hands each report that arrives to `hid_handle_report`, which
 * already knows how to turn one into key presses and pointer movement.
 *
 * A second report-descriptor parser would be a second set of the same bugs,
 * found separately, years apart, by different people.  The one being reused
 * has been driven against real descriptors with and without report
 * identifiers, and everything it has learned applies here for free.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS EASY TO GET WRONG
 *
 * Nothing arrives until it is asked for.  A device is silent by default: it
 * publishes everything about itself, answers every question, and sends no
 * reports at all until told to.  Being told is two bytes written to a
 * descriptor sitting beside the value.  Forget it and the keyboard connects,
 * describes itself perfectly, and never reports a key - which looks exactly
 * like a keyboard that is broken rather than one that was never asked.
 *
 * The report descriptor is longer than one message.  It arrives in pieces and
 * the last piece is the short one; a driver that waits for an empty piece
 * waits forever on a descriptor that happened to end on a boundary.
 *
 * A device has more than one report characteristic - a keyboard with media
 * keys has two or three - and they are told apart by the report identifier in
 * the descriptor, not by the order they appear in.
 */
#include "kernel.h"
#include "klog.h"
#include "usb.h"

/* From the transport. */
int  btusb_read_attribute(void *ctx, u16 attr_handle, void *out, u32 cap);
int  btusb_read_long_attribute(void *ctx, u16 attr_handle, void *out, u32 cap);
bool btusb_write_attribute(void *ctx, u16 attr_handle, const void *value, u16 len);
int  btusb_await_notification(void *ctx, u16 *attr_handle, void *out, u32 cap,
                              u32 timeout_ms);
int  btusb_find_characteristics(void *ctx, u16 uuid, u16 *handles, int max);

/* From the HID layer, which knows nothing about how the bytes arrived. */
typedef struct hid_device hid_t;
bool hid_parse_descriptor(hid_t *h, const u8 *d, int len);
void hid_handle_report(hid_t *h, const u8 *data, int len);
hid_t *hid_alloc_detached(void);
void hid_free_detached(hid_t *h);
bool hid_is_keyboard(const hid_t *h);
bool hid_is_pointer(const hid_t *h);

#define UUID_REPORT_MAP       0x2A4B
#define UUID_REPORT           0x2A4D
#define UUID_CCC_DESCRIPTOR   0x2902

#define MAX_REPORT_CHARS      4
#define REPORT_MAP_MAX        512

typedef struct {
    bool used;
    void *transport;
    hid_t *hid;
    bool  is_keyboard, is_pointer;

    u16   report_handles[MAX_REPORT_CHARS];
    int   report_count;

    u32   reports_seen;
} bt_hid_t;

static bt_hid_t device;

/* --------------------------------------------------------------- bring-up */

/* Ask a device to start telling us when a value changes.
 *
 * The descriptor that switches this on sits immediately after the value it
 * belongs to, which is a convention rather than a rule - but it is the
 * convention every device follows, and finding it by searching costs a round
 * trip per characteristic on a link measured in milliseconds.
 */
static bool subscribe(void *transport, u16 value_handle) {
    const u8 notify_me[2] = { 0x01, 0x00 };
    return btusb_write_attribute(transport, (u16)(value_handle + 1),
                                 notify_me, sizeof notify_me);
}

bool bt_hid_attach(void *transport) {
    if (!transport) return false;

    memset(&device, 0, sizeof device);
    device.transport = transport;

    /* What it publishes as its report characteristics.  A keyboard with media
     * keys has more than one, and all of them have to be subscribed to: the
     * letter keys and the volume keys arrive on different ones. */
    device.report_count = btusb_find_characteristics(transport, UUID_REPORT,
                                                     device.report_handles,
                                                     MAX_REPORT_CHARS);
    if (device.report_count <= 0) {
        kinfo("bthid", "this device publishes no reports, so it is not a "
                       "keyboard or a pointer");
        return false;
    }

    /* And the descriptor saying what those reports mean. */
    u16 map_handle = 0;
    if (btusb_find_characteristics(transport, UUID_REPORT_MAP,
                                   &map_handle, 1) < 1) {
        kwarn("bthid", "it publishes reports but no description of them, so "
                       "there is no way to know what they mean");
        return false;
    }

    static u8 map[REPORT_MAP_MAX];
    int map_len = btusb_read_long_attribute(transport, map_handle, map,
                                            sizeof map);
    if (map_len <= 0) {
        kwarn("bthid", "its report description could not be read");
        return false;
    }

    device.hid = hid_alloc_detached();
    if (!device.hid) return false;

    if (!hid_parse_descriptor(device.hid, map, map_len)) {
        kwarn("bthid", "its report description (%d bytes) would not parse",
              map_len);
        return false;
    }

    /* What it turned out to be, which is decided by the description rather
     * than by anything the device claims about itself. */
    device.is_keyboard = hid_is_keyboard(device.hid);
    device.is_pointer  = hid_is_pointer(device.hid);

    kinfo("bthid", "a %s over Bluetooth: %d byte description, %d report "
                   "characteristic(s)",
          device.is_keyboard ? (device.is_pointer ? "keyboard and pointer"
                                                  : "keyboard")
                             : (device.is_pointer ? "pointer" : "device"),
          map_len, device.report_count);

    /* Last, and not optional.  Everything above works on a device that will
     * never send anything. */
    int told = 0;
    for (int i = 0; i < device.report_count; i++)
        if (subscribe(transport, device.report_handles[i])) told++;

    if (!told) {
        kwarn("bthid", "it would not be told to report, so it will describe "
                       "itself perfectly and send nothing");
        return false;
    }

    kinfo("bthid", "asked to be told about %d of %d report(s)", told,
          device.report_count);

    device.used = true;
    return true;
}

/* ------------------------------------------------------------- in service */

/* One report, whenever one arrives.  Returns false when nothing came. */
bool bt_hid_poll(u32 timeout_ms) {
    if (!device.used) return false;

    u8 report[32];
    u16 from = 0;
    int n = btusb_await_notification(device.transport, &from, report,
                                     sizeof report, timeout_ms);
    if (n <= 0) return false;

    /* From one of ours, rather than from anything else the device publishes -
     * a battery level arrives on this same channel and is not a key press. */
    bool mine = false;
    for (int i = 0; i < device.report_count; i++)
        if (device.report_handles[i] == from) mine = true;
    if (!mine) return false;

    device.reports_seen++;
    hid_handle_report(device.hid, report, n);
    return true;
}

/* Let go of the device, and of any key it was holding.
 *
 * The release matters more than the tidying.  A report saying a key is down is
 * not an event, it is a STATE, and the state persists until a report says
 * otherwise - so a device that reports a key and then goes away leaves the
 * system holding that key down, repeating it forever.
 *
 * That is not hypothetical: the check that drives a keyboard against a model
 * received one report of the letter A and detached, and the machine then
 * auto-repeated A into every window for the rest of the boot.  Nothing typed
 * into it worked again, and the failure surfaced three layers away as a
 * program that would not start.
 */
void bt_hid_detach(void) {
    if (!device.used) return;

    static const u8 nothing_held[8] = { 0 };
    hid_handle_report(device.hid, nothing_held, sizeof nothing_held);

    hid_free_detached(device.hid);
    memset(&device, 0, sizeof device);
}

bool bt_hid_present(void) { return device.used; }
u32  bt_hid_reports(void) { return device.reports_seen; }

/* ------------------------------------------------------- how much is left
 *
 * A wireless keyboard runs on a battery and a person needs to know when it is
 * going flat.  The device already publishes it: one byte, a percentage, on the
 * same channel everything else here travels on, and no pairing is needed for a
 * value a device is willing to publish.
 *
 * This is a whole profile and it is nine lines, which is worth noticing.  The
 * expensive parts - discovery, reading, the transport under both - were built
 * for the keyboard, and everything published over that channel comes almost
 * free afterwards.  A heart-rate strap or a thermometer would be the same
 * shape with a different number.
 */
#define UUID_BATTERY_LEVEL    0x2A19

int bt_battery_percent(void *transport) {
    if (!transport) return -1;

    u16 handle = 0;
    if (btusb_find_characteristics(transport, UUID_BATTERY_LEVEL,
                                   &handle, 1) < 1)
        return -1;                       /* it does not publish one */

    u8 level = 0;
    if (btusb_read_attribute(transport, handle, &level, 1) != 1) return -1;

    /* A percentage, and devices do report nonsense: a value above a hundred is
     * not a battery that is more than full, it is a device that put something
     * else there. */
    if (level > 100) return -1;
    return (int)level;
}
