/* btusb.c - the Bluetooth adapter, over USB.
 *
 * Bluetooth over USB is one of the few genuinely standard things on a modern
 * machine.  Whatever the radio is - Intel, Realtek, Broadcom - the way a host
 * talks to it is the same: an interface of class 0xE0, and four endpoints
 * carrying the four kinds of traffic the specification defines.
 *
 *   Commands go out through the device's control endpoint.
 *   Events come back on an interrupt endpoint.
 *   Audio, if any, goes over isochronous endpoints this does not use.
 *   Everything else - the actual data of a connection - is bulk.
 *
 * So the transport is common and only the firmware differs, which is why one
 * driver in Linux covers every adapter anyone has and the vendor-specific part
 * is a few hundred lines of loading a blob.
 *
 * What this does is the part that is the same everywhere: find the adapter,
 * reset it, and ask it who it is.  Those three commands are defined by the
 * specification, answered by every adapter, and answered before any firmware
 * has been loaded - so they establish that the radio is there and being talked
 * to correctly, which is the thing that has to work before anything else can.
 *
 * On top of that transport there is now a usable amount of stack:
 *
 *   Discovery on both radios.  Bluetooth is two protocols sharing an antenna -
 *   the older one is asked to run an inquiry and reports what answers, the
 *   low-energy one is told to listen and reports what advertises - and someone
 *   asking what is nearby does not care which, so both run and the results
 *   merge into one list keyed by address.
 *
 *   Connections, of both kinds.  Low energy opens a link with no ceremony; the
 *   older kind will not carry anything until both ends hold the same key, so
 *   that exchange is conducted here and the key is kept on disk.
 *
 *   L2CAP, and the attribute channel on top of it, which is how nearly
 *   everything low-energy publishes a value.
 *
 * What is still missing is the profiles - the agreements about what the bytes
 * on a link mean for a headset, a keyboard, a file transfer.  A link that
 * carries data is not the same as a headset that plays sound, and the second
 * needs a profile the first does not.
 */
#include "kernel.h"
#include "klog.h"
#include "usb.h"
#include "vfs.h"
#include "time.h"
#include "proc.h"

/* The class the specification assigns to a Bluetooth transport. */
#define USB_CLASS_WIRELESS   0xE0
#define SUBCLASS_RF          0x01
#define PROTOCOL_BLUETOOTH   0x01

/* Commands are grouped, and the group is part of the number.  0x03 is the
 * host controller's own control, 0x04 is asking it about itself. */
#define HCI_OP(group, cmd)   (u16)(((group) << 10) | (cmd))
#define HCI_RESET            HCI_OP(0x03, 0x0003)
#define HCI_READ_LOCAL_VER   HCI_OP(0x04, 0x0001)
#define HCI_READ_BD_ADDR     HCI_OP(0x04, 0x0009)

/* Events the adapter sends back. */
/* Discovery, which is the first thing an adapter can be asked to do that has
 * a visible answer.  Two kinds, because Bluetooth is two protocols sharing a
 * radio: the older one is asked to run an inquiry and reports what answers,
 * the low-energy one is told to listen and reports what advertises. */
#define HCI_INQUIRY          HCI_OP(0x01, 0x0001)
#define HCI_INQUIRY_CANCEL   HCI_OP(0x01, 0x0002)
#define HCI_LE_SET_SCAN_PARAMS HCI_OP(0x08, 0x000B)
#define HCI_LE_SET_SCAN_ENABLE HCI_OP(0x08, 0x000C)
#define HCI_SET_EVENT_MASK   HCI_OP(0x03, 0x0001)
#define HCI_LE_SET_EVENT_MASK HCI_OP(0x08, 0x0001)

#define HCI_EV_INQUIRY_COMPLETE 0x01
#define HCI_EV_INQUIRY_RESULT   0x02
#define HCI_EV_INQUIRY_RESULT_RSSI 0x22
#define HCI_EV_EXT_INQUIRY_RESULT  0x2F
#define HCI_EV_LE_META          0x3E
#define HCI_LE_ADVERTISING_REPORT 0x02

/* Connecting.  The low-energy path is the one implemented: it needs no
 * pairing to open a link, where the older kind negotiates security before it
 * will carry anything - so this is the half that can be finished honestly. */
#define HCI_LE_CREATE_CONN       HCI_OP(0x08, 0x000D)
#define HCI_LE_CREATE_CONN_CANCEL HCI_OP(0x08, 0x000E)
#define HCI_DISCONNECT           HCI_OP(0x01, 0x0006)

#define HCI_EV_DISCONN_COMPLETE  0x05
#define HCI_EV_COMMAND_STATUS    0x0F
#define HCI_LE_CONNECTION_COMPLETE 0x01

/* The older protocol's way in.  A headset, a keyboard and a mouse are almost
 * always this kind, and it differs from the low-energy path in one way that
 * shapes everything: it will not carry anything until the two ends have agreed
 * on a key, and agreeing on one is a conversation rather than a command. */
#define HCI_CREATE_CONNECTION    HCI_OP(0x01, 0x0005)
#define HCI_AUTH_REQUESTED       HCI_OP(0x01, 0x0011)
#define HCI_SET_CONN_ENCRYPTION  HCI_OP(0x01, 0x0013)
#define HCI_IO_CAP_REPLY         HCI_OP(0x01, 0x002B)
#define HCI_USER_CONFIRM_REPLY   HCI_OP(0x01, 0x002C)
#define HCI_LINK_KEY_NEG_REPLY   HCI_OP(0x01, 0x000C)
#define HCI_WRITE_SSP_MODE       HCI_OP(0x03, 0x0056)
#define HCI_REMOTE_NAME_REQ      HCI_OP(0x01, 0x0019)

#define HCI_EV_CONNECT_COMPLETE  0x03
#define HCI_EV_AUTH_COMPLETE     0x06
#define HCI_EV_REMOTE_NAME       0x07
#define HCI_EV_ENCRYPT_CHANGE    0x08
#define HCI_EV_LINK_KEY_REQ      0x17
#define HCI_EV_LINK_KEY_NOTIFY   0x18
#define HCI_EV_IO_CAP_REQUEST    0x31
#define HCI_EV_USER_CONFIRM_REQ  0x33
#define HCI_EV_SIMPLE_PAIR_DONE  0x36

#define HCI_EV_COMMAND_COMPLETE 0x0E

#define MAX_ADAPTERS 2

typedef struct {
    bool used;
    bool modelled;          /* answered by btusb_model.c rather than a radio */
    usb_device_t *dev;
    u8   interface;

    bool  reset_done;
    u8    bd_addr[6];
    u8    hci_version;
    u16   hci_revision;
    u16   manufacturer;
    char  maker[32];

    /* The last event, kept until whoever sent the command has looked at it. */
    u8   event[260];
    int  event_len;
    bool event_waiting;
} bt_t;

static bt_t adapters[MAX_ADAPTERS];

/* What discovery has turned up.
 *
 * Kept here rather than per adapter: what somebody wants to know is what is
 * nearby, not which of two radios happened to hear it, and a device heard by
 * both should appear once.
 */
#define MAX_FOUND 32

typedef struct {
    bool used;
    u8   addr[6];
    bool low_energy;
    s8   rssi;              /* 127 when the adapter did not report one       */
    u32  device_class;      /* zero for low-energy, which has no such field  */
    char name[32];
    u64  last_seen_ms;
} bt_found_t;

static bt_found_t found[MAX_FOUND];

/* An open link.
 *
 * One at a time, deliberately.  An adapter will hold several, but nothing
 * above this can yet use more than one, and a table of links that nothing
 * addresses is a table that goes stale without anybody noticing.
 */
typedef struct {
    bool used;
    u16  handle;            /* what the adapter calls this link              */
    u8   addr[6];
    bool low_energy;
    u16  interval;          /* in 1.25 ms units, as the adapter reports it   */
    u16  latency, timeout;
} bt_link_t;

static bt_link_t link;

/* The key two devices agreed on, so a second connection does not pair again.
 *
 * Kept on the system volume, because a key that does not outlive the power is
 * one the user re-confirms on every boot - which is the difference between a
 * headset that reconnects when it is switched on and one that has to be paired
 * again every morning.
 *
 * The file holds nothing but addresses and keys, and a key is what lets
 * something claim to be that device: it is written where the rest of the
 * system's own state lives and nowhere a program can reach.  Losing the file
 * costs a re-pairing and nothing else, so a machine with no writable volume
 * keeps them in memory and says so.
 */
/* Two places, tried in order - and this file is where I got it wrong first
 * time.  The comment above says a key that does not outlive the power is one
 * the user re-confirms every boot; writing it only to /data meant exactly that
 * on a stick, which is the machine this system is mostly run from.  The bug
 * was written into the fix for the bug. */
#define KEYS_PATH      "/data/bt-keys"
#define KEYS_PATH_BOOT "/boot/KESTREL/bt-keys"
#define KEYS_MAGIC 0x4B544235u          /* "5BTK" */
#define MAX_KEYS 8

typedef struct {
    bool used;
    u8   addr[6];
    u8   key[16];
} bt_key_t;

static bt_key_t keys[MAX_KEYS];

static bt_key_t *key_for(const u8 *addr) {
    for (int i = 0; i < MAX_KEYS; i++)
        if (keys[i].used && !memcmp(keys[i].addr, addr, 6)) return &keys[i];
    return NULL;
}

/* Written whole rather than appended to, because the table is small and a
 * partly-written one is worse than none: a truncated key is a device that
 * fails to connect for a reason nothing reports. */
static void keys_save(void) {
    struct __attribute__((packed)) {
        u32 magic;
        u32 count;
        struct { u8 addr[6]; u8 key[16]; } entry[MAX_KEYS];
    } file;

    memset(&file, 0, sizeof file);
    file.magic = KEYS_MAGIC;

    for (int i = 0; i < MAX_KEYS; i++) {
        if (!keys[i].used) continue;
        memcpy(file.entry[file.count].addr, keys[i].addr, 6);
        memcpy(file.entry[file.count].key, keys[i].key, 16);
        file.count++;
    }

    if (vfs_write_file(KEYS_PATH, &file, sizeof file) == 0) return;
    if (vfs_write_file(KEYS_PATH_BOOT, &file, sizeof file) == 0) return;

    kinfo("btusb", "there is nowhere to keep pairings, so they last until "
                   "the power goes off");
}

static void keys_load(void) {
    struct __attribute__((packed)) {
        u32 magic;
        u32 count;
        struct { u8 addr[6]; u8 key[16]; } entry[MAX_KEYS];
    } file;

    file_t *f = NULL;
    if (vfs_open(KEYS_PATH, 0, &f) < 0 &&
        vfs_open(KEYS_PATH_BOOT, 0, &f) < 0) return;

    ssize_t_k n = vfs_read(f, &file, sizeof file);
    vfs_close(f);

    if (n != (ssize_t_k)sizeof file || file.magic != KEYS_MAGIC) return;
    if (file.count > MAX_KEYS) return;

    for (u32 i = 0; i < file.count; i++) {
        keys[i].used = true;
        memcpy(keys[i].addr, file.entry[i].addr, 6);
        memcpy(keys[i].key, file.entry[i].key, 16);
    }
    if (file.count)
        kinfo("btusb", "%u remembered pairing(s)", file.count);
}

/* Whoever asked for a connection, so the events that arrive unasked during
 * pairing can be answered without being told again who they are about. */
static bt_t *pairing_adapter;
static u8    pairing_addr[6];
static bool  pairing_done;
static bool  pairing_ok;

/* Addresses arrive least significant byte first, which is the opposite of how
 * one is written down.  Reversing on the way in means everything above this
 * point can treat them the way a person would. */
static void addr_from_wire(u8 *out, const u8 *wire) {
    for (int i = 0; i < 6; i++) out[i] = wire[5 - i];
}

static bt_found_t *remember(const u8 *addr, bool low_energy) {
    for (int i = 0; i < MAX_FOUND; i++)
        if (found[i].used && !memcmp(found[i].addr, addr, 6)) {
            found[i].last_seen_ms = g_uptime_ms;
            return &found[i];
        }

    for (int i = 0; i < MAX_FOUND; i++)
        if (!found[i].used) {
            memset(&found[i], 0, sizeof found[i]);
            found[i].used = true;
            memcpy(found[i].addr, addr, 6);
            found[i].low_energy = low_energy;
            found[i].rssi = 127;
            found[i].last_seen_ms = g_uptime_ms;
            return &found[i];
        }
    return NULL;   /* full: what is already known is more useful than churn */
}

/* --------------------------------------------------------------- who made it */

/* The company identifiers the specification assigns.  Only the ones that turn
 * up in a personal computer are listed; anything else is reported by number,
 * which is more useful than a wrong name. */
static const char *manufacturer_name(u16 id) {
    switch (id) {
    case 0x0002: return "Intel";
    case 0x000F: return "Broadcom";
    case 0x001D: return "Qualcomm";
    case 0x005D: return "Realtek";
    case 0x0046: return "MediaTek";
    case 0x0499: return "Realtek";
    case 0x0059: return "Nordic";
    default:     return NULL;
    }
}

/* --------------------------------------------------------------- commands */

/* Standard HCI USB command transport: class request to the device, request,
 * value and index all zero.  Linux btusb.c alloc_ctrl_urb()/btusb_probe() use
 * the same setup.  Interface recipient is for the different AMP transport,
 * not the e0/01/01 interface this driver binds. */
/* When a model is standing in for the adapter.
 *
 * Every command leaves through this one function and every event arrives
 * through btusb_event, so a model needs exactly these two seams and no
 * others - which is why the driver below is unchanged apart from the branch. */
void btusb_model_command(void *ctx, const u8 *packet, int len);
void btusb_model_attribute(u16 cid, const void *payload, u16 len);
int  btusb_model_attribute_reply(u16 *cid_out, void *out, u32 cap);

static bool send_command(bt_t *bt, u16 opcode, const void *params, u8 len) {
    u8 packet[3 + 255];
    packet[0] = (u8)(opcode & 0xFF);
    packet[1] = (u8)(opcode >> 8);
    packet[2] = len;
    if (len && params) memcpy(packet + 3, params, len);

    if (bt->modelled) {
        btusb_model_command(bt, packet, 3 + len);
        return true;
    }

    int sent = usb_control(bt->dev,
                           USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_DEVICE,
                           0, 0, 0, packet, (u16)(3 + len));
    return sent >= 0;
}

/* Wait for the adapter to say the command finished.
 *
 * Every command this driver sends is answered by one event, so waiting for the
 * next event is the same as waiting for the answer - which is only true
 * because nothing else is being asked of the adapter at the same time, and
 * would not be once there are connections to service. */
static bool wait_for_complete(bt_t *bt, u16 opcode, u8 *params_out,
                              int *params_len, int timeout_ms) {
    if (!bt || !bt->used || timeout_ms <= 0) return false;
    u64 deadline = g_uptime_ms + (u64)timeout_ms;

    while (g_uptime_ms < deadline) {
        /* btusb_start runs inside the USB controller's enumeration thread.
         * Its outer pump cannot run until we return.  Control-transfer success
         * only acknowledges command delivery, not the later HCI event. */
        if (!bt->event_waiting && !bt->modelled)
            usb_poll_device_events(bt->dev);
        if (!bt->used) return false; /* unplug may have been delivered above */
        if (bt->event_waiting) {
            bt->event_waiting = false;

            /* An event is a code, a length, and then its contents. */
            if (bt->event_len >= 6 && bt->event[0] == HCI_EV_COMMAND_COMPLETE &&
                bt->event[1] >= 4 && bt->event[1] <= bt->event_len - 2) {
                u16 answered = (u16)(bt->event[3] | (bt->event[4] << 8));
                if (answered == opcode) {
                    /* The first byte of the answer is the status; anything but
                     * zero means the adapter refused. */
                    if (bt->event[5] != 0) return false;

                    if (params_out && params_len) {
                        int n = bt->event[1] - 4;
                        if (n > *params_len) n = *params_len;
                        if (n > 0) memcpy(params_out, bt->event + 6, (size_t)n);
                        *params_len = n > 0 ? n : 0;
                    }
                    return true;
                }
            }
        }
        sched_sleep_ms(1);
    }
    return false;
}

/* ------------------------------------------------------------- events in */

/* Called by the USB layer with whatever arrived on the interrupt endpoint. */
/* An inquiry result: one or more devices that answered, each with its address
 * and what kind of device it says it is.  The three shapes of this event differ
 * only in what they add after the address, so the part that matters is read the
 * same way for all of them. */
static void inquiry_result(const u8 *p, int len, u8 code) {
    if (len < 1) return;
    int count = p[0];
    const u8 *entry = p + 1;

    /* Each entry: address, page scan repetition mode, and then a reserved byte
     * the older event has and the newer ones do not. */
    int stride = (code == HCI_EV_INQUIRY_RESULT) ? 14 : 14;
    if (code == HCI_EV_EXT_INQUIRY_RESULT) count = 1;   /* one, with more data */

    for (int i = 0; i < count; i++) {
        if ((entry - p) + 6 > len) return;

        u8 addr[6];
        addr_from_wire(addr, entry);

        bt_found_t *f = remember(addr, false);
        if (f) {
            /* The class of device sits three bytes after the scan mode fields;
             * it says what the thing is - a headset, a phone, a keyboard. */
            int off = (int)(entry - p) + 9;
            if (off + 3 <= len)
                f->device_class = (u32)entry[9] | ((u32)entry[10] << 8) |
                                  ((u32)entry[11] << 16);
        }
        entry += stride;
    }
}

/* A low-energy advertisement.  These carry the name inline more often than the
 * older kind do, which is why a scan usually names more devices than an
 * inquiry does. */
static void advertising_report(const u8 *p, int len) {
    if (len < 2) return;
    int reports = p[0];
    const u8 *r = p + 1;

    for (int i = 0; i < reports; i++) {
        if ((r - p) + 9 > len) return;

        u8 addr[6];
        addr_from_wire(addr, r + 2);          /* type, address type, address */

        bt_found_t *f = remember(addr, true);
        int data_len = r[8];
        const u8 *data = r + 9;
        if ((data - p) + data_len + 1 > len) return;

        /* The advertisement is a list of {length, type, value}.  Type 8 is a
         * shortened name and 9 is the complete one; either is better than an
         * address for somebody choosing a device from a list. */
        for (int at = 0; at + 1 < data_len; ) {
            int flen = data[at];
            if (flen < 1 || at + flen >= data_len + 1) break;
            u8 type = data[at + 1];

            if (f && (type == 0x08 || type == 0x09)) {
                int n = flen - 1;
                if (n > (int)sizeof f->name - 1) n = (int)sizeof f->name - 1;
                memcpy(f->name, data + at + 2, (size_t)n);
                f->name[n] = 0;
            }
            at += flen + 1;
        }

        if (f) f->rssi = (s8)data[data_len];
        r = data + data_len + 1;
    }
}

void btusb_event(void *ctx, const u8 *data, int len) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || len <= 0) return;

    /* Discovery results arrive unasked, in among the answers to commands, and
     * are taken here rather than left in the one-deep buffer below - which
     * holds whatever came last and would otherwise lose a command's answer to
     * a device advertising at the wrong moment. */
    if (len >= 2) {
        u8 code = data[0];
        u8 plen = data[1];
        const u8 *params = data + 2;
        int avail = len - 2;
        if (plen < avail) avail = plen;

        if (code == HCI_EV_INQUIRY_RESULT ||
            code == HCI_EV_INQUIRY_RESULT_RSSI ||
            code == HCI_EV_EXT_INQUIRY_RESULT) {
            inquiry_result(params, avail, code);
            return;
        }
        if (code == HCI_EV_LE_META && avail >= 1 &&
            params[0] == HCI_LE_ADVERTISING_REPORT) {
            advertising_report(params + 1, avail - 1);
            return;
        }
        if (code == HCI_EV_INQUIRY_COMPLETE) {
            kinfo("btusb", "the inquiry finished");
            return;
        }

        /* A link opened.  The address comes back with it, which is how this is
         * matched to whatever was asked for rather than assumed. */
        if (code == HCI_EV_LE_META && avail >= 2 &&
            params[0] == HCI_LE_CONNECTION_COMPLETE) {
            const u8 *q = params + 1;
            if (avail >= 19) {
                if (q[0] != 0) {
                    kwarn("btusb", "the link did not open (status %#x)", q[0]);
                    memset(&link, 0, sizeof link);
                } else {
                    memset(&link, 0, sizeof link);
                    link.used = true;
                    link.low_energy = true;
                    link.handle = (u16)(q[1] | (q[2] << 8));
                    addr_from_wire(link.addr, q + 5);
                    link.interval = (u16)(q[11] | (q[12] << 8));
                    link.latency  = (u16)(q[13] | (q[14] << 8));
                    link.timeout  = (u16)(q[15] | (q[16] << 8));

                    kinfo("btusb", "connected to "
                          "%02x:%02x:%02x:%02x:%02x:%02x, link %u, one exchange "
                          "every %u.%02u ms",
                          link.addr[0], link.addr[1], link.addr[2],
                          link.addr[3], link.addr[4], link.addr[5],
                          link.handle,
                          (link.interval * 125) / 100 / 1000,
                          ((link.interval * 125) / 100) % 1000 / 10);
                }
            }
            return;
        }

        /* Pairing is a conversation the adapter starts on our behalf, and
         * every step of it arrives here as an event that must be answered
         * before the next one comes.  Answering from the event handler is what
         * makes that possible: a connect waiting on a reply cannot also be
         * reading these, and an unanswered request times out the pairing with
         * no explanation on either side. */
        if (code == HCI_EV_IO_CAP_REQUEST && avail >= 6 && pairing_adapter) {
            u8 reply[9];
            memcpy(reply, params, 6);       /* the address, still on the wire */
            reply[6] = 0x03;                /* no display and no keyboard     */
            reply[7] = 0x00;                /* no out-of-band data            */
            reply[8] = 0x00;                /* no bonding required of them    */
            send_command(pairing_adapter, HCI_IO_CAP_REPLY, reply, sizeof reply);
            return;
        }

        /* "Do both ends show the same number?"  With nothing to show a number
         * on, the honest answer is to accept: this is the pairing model a
         * headset with no screen and no keypad uses, and refusing would mean
         * refusing every such device. */
        if (code == HCI_EV_USER_CONFIRM_REQ && avail >= 6 && pairing_adapter) {
            send_command(pairing_adapter, HCI_USER_CONFIRM_REPLY, params, 6);
            return;
        }

        /* The adapter asking whether there is already a key for this device.
         * Saying no when there is not is what starts pairing; not answering
         * leaves it waiting. */
        if (code == HCI_EV_LINK_KEY_REQ && avail >= 6 && pairing_adapter) {
            u8 addr[6];
            addr_from_wire(addr, params);
            bt_key_t *k = key_for(addr);
            if (k) {
                u8 reply[22];
                memcpy(reply, params, 6);
                memcpy(reply + 6, k->key, 16);
                send_command(pairing_adapter, HCI_OP(0x01, 0x000B), reply, 22);
            } else {
                send_command(pairing_adapter, HCI_LINK_KEY_NEG_REPLY, params, 6);
            }
            return;
        }

        /* And the key itself, once both ends have it. */
        if (code == HCI_EV_LINK_KEY_NOTIFY && avail >= 23) {
            u8 addr[6];
            addr_from_wire(addr, params);

            bt_key_t *k = key_for(addr);
            if (!k)
                for (int i = 0; i < MAX_KEYS; i++)
                    if (!keys[i].used) { k = &keys[i]; break; }

            if (k) {
                k->used = true;
                memcpy(k->addr, addr, 6);
                memcpy(k->key, params + 6, 16);
                kinfo("btusb", "paired with %02x:%02x:%02x:%02x:%02x:%02x",
                      addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
                keys_save();
            }
            return;
        }

        if (code == HCI_EV_SIMPLE_PAIR_DONE && avail >= 7) {
            pairing_done = true;
            pairing_ok = (params[0] == 0);
            if (!pairing_ok)
                kwarn("btusb", "pairing did not complete (status %#x)", params[0]);
            return;
        }

        /* A link on the older protocol.  Same meaning as the low-energy one,
         * different event and a different order of fields. */
        if (code == HCI_EV_CONNECT_COMPLETE && avail >= 11) {
            if (params[0] != 0) {
                kwarn("btusb", "the link did not open (status %#x)", params[0]);
                memset(&link, 0, sizeof link);
            } else {
                memset(&link, 0, sizeof link);
                link.used = true;
                link.low_energy = false;
                link.handle = (u16)(params[1] | (params[2] << 8));
                addr_from_wire(link.addr, params + 3);
                kinfo("btusb", "connected to "
                      "%02x:%02x:%02x:%02x:%02x:%02x, link %u",
                      link.addr[0], link.addr[1], link.addr[2],
                      link.addr[3], link.addr[4], link.addr[5], link.handle);
            }
            return;
        }

        if (code == HCI_EV_AUTH_COMPLETE && avail >= 3) {
            if (params[0] == 0) kinfo("btusb", "the link is authenticated");
            else kwarn("btusb", "authentication failed (status %#x)", params[0]);
            pairing_done = true;
            pairing_ok = (params[0] == 0);
            return;
        }

        if (code == HCI_EV_ENCRYPT_CHANGE && avail >= 4) {
            kinfo("btusb", "encryption is %s", params[3] ? "on" : "off");
            return;
        }

        if (code == HCI_EV_DISCONN_COMPLETE && avail >= 4) {
            u16 h = (u16)(params[1] | (params[2] << 8));
            if (link.used && link.handle == h) {
                kinfo("btusb", "the link closed (reason %#x)", params[3]);
                memset(&link, 0, sizeof link);
            }
            return;
        }
    }

    if (len > (int)sizeof bt->event) len = (int)sizeof bt->event;
    memcpy(bt->event, data, (size_t)len);
    bt->event_len = len;
    bt->event_waiting = true;
}

/* ------------------------------------------------------------------ probe */

void *btusb_probe(usb_device_t *dev, const usb_interface_t *ifc) {
    if (ifc->dev_class != USB_CLASS_WIRELESS ||
        ifc->subclass != SUBCLASS_RF ||
        ifc->protocol != PROTOCOL_BLUETOOTH)
        return NULL;

    /* The events endpoint is what makes the adapter answerable; without it a
     * command could be sent and nothing would ever come back. */
    if (!ifc->ep_addr) {
        kwarn("btusb", "%s: a Bluetooth interface with no event endpoint",
              usb_device_name(dev));
        return NULL;
    }

    bt_t *bt = NULL;
    for (int i = 0; i < MAX_ADAPTERS; i++)
        if (!adapters[i].used) { bt = &adapters[i]; break; }
    if (!bt) return NULL;

    memset(bt, 0, sizeof *bt);
    bt->used = true;
    bt->dev = dev;
    bt->interface = ifc->number;

    kinfo("btusb", "%s: a Bluetooth adapter", usb_device_name(dev));
    return bt;
}

/* Ask the adapter who it is.  Called once the event endpoint is running,
 * because every one of these needs an answer to come back. */
void btusb_start(void *ctx) {
    bt_t *bt = ctx;
    if (!bt || !bt->used) return;

    /* Whatever this machine has paired with before.  Read once, on the first
     * adapter to start - a second adapter shares the same list, because what
     * was paired belongs to the machine and not to the radio. */
    static bool keys_read;
    if (!keys_read) { keys_read = true; keys_load(); }

    /* Reset first.  An adapter that the firmware left half-configured answers
     * everything else oddly until it has been reset, and the reset is defined
     * to work from any state. */
    if (!send_command(bt, HCI_RESET, NULL, 0) ||
        !wait_for_complete(bt, HCI_RESET, NULL, NULL, 1000)) {
        kwarn("btusb", "the adapter did not answer a reset");
        return;
    }
    bt->reset_done = true;

    u8 params[16];
    int len = (int)sizeof params;
    if (send_command(bt, HCI_READ_LOCAL_VER, NULL, 0) &&
        wait_for_complete(bt, HCI_READ_LOCAL_VER, params, &len, 1000) &&
        len >= 8) {
        bt->hci_version = params[0];
        bt->hci_revision = (u16)(params[1] | (params[2] << 8));
        bt->manufacturer = (u16)(params[4] | (params[5] << 8));

        const char *who = manufacturer_name(bt->manufacturer);
        if (who) strlcpy(bt->maker, who, sizeof bt->maker);
        else snprintf(bt->maker, sizeof bt->maker, "company %u", bt->manufacturer);
    }

    len = (int)sizeof params;
    if (send_command(bt, HCI_READ_BD_ADDR, NULL, 0) &&
        wait_for_complete(bt, HCI_READ_BD_ADDR, params, &len, 1000) &&
        len >= 6) {
        /* The address comes back least significant byte first, and is written
         * the other way round everywhere a person sees it. */
        for (int i = 0; i < 6; i++) bt->bd_addr[i] = params[5 - i];
    }

    /* The version number is a code for a specification version, not the
     * version itself: 12 is Bluetooth 5.3, 13 is 5.4, 14 is 6.0. */
    static const char *spec[] = {
        "1.0b", "1.1", "1.2", "2.0", "2.1", "3.0", "4.0", "4.1", "4.2",
        "5.0", "5.1", "5.2", "5.3", "5.4", "6.0",
    };
    const char *version = (bt->hci_version < ARRAY_LEN(spec))
                        ? spec[bt->hci_version] : "an unknown version";

    kinfo("btusb", "%s adapter, Bluetooth %s, "
                   "%02x:%02x:%02x:%02x:%02x:%02x",
          bt->maker[0] ? bt->maker : "an", version,
          bt->bd_addr[0], bt->bd_addr[1], bt->bd_addr[2],
          bt->bd_addr[3], bt->bd_addr[4], bt->bd_addr[5]);

    /* What this can and cannot do, said accurately.
     *
     * This line used to read "scanning and pairing need the host stack above
     * this transport, which is not written" - which stopped being true when
     * the scanning and the pairing were written, in this same file, and was
     * never updated.  A log that describes the system as it was is worse than
     * one that says nothing: it is the first thing anybody reads, and it sent
     * every reader to the wrong conclusion about what was missing. */
    /* Updated when the first profile landed.  The previous version of this
     * line said keyboards were not written, and that line had already been
     * wrong once before for the same reason - it described the system as it
     * was rather than as it is, and it is the first thing anybody reads. */
    kinfo("btusb", "it can discover devices, pair with them, open a link, and "
                   "drive a keyboard or pointer over it; audio and file "
                   "transfer are not written");
}

/* ------------------------------------------------------------- reporting */

/* ------------------------------------------------------------------- tests
 *
 * Every byte this driver reads out of an event arrives at an offset counted
 * from a specification, and getting one wrong does not fail - it produces a
 * plausible wrong answer.  An address read one byte late is still six bytes
 * and still looks like an address; a name read from the wrong field is still
 * text.  The device is not there to argue, so nothing contradicts it.
 *
 * These are real event shapes with known contents, so the offsets are checked
 * against something other than the same reading of the specification that
 * produced them.  No adapter is needed: an event is bytes.
 */
static int check_addr(const u8 *got, const u8 *want, const char *what) {
    if (!memcmp(got, want, 6)) return 0;
    kwarn("btusb", "selftest: %s came out "
                   "%02x:%02x:%02x:%02x:%02x:%02x, expected "
                   "%02x:%02x:%02x:%02x:%02x:%02x",
          what, got[0], got[1], got[2], got[3], got[4], got[5],
          want[0], want[1], want[2], want[3], want[4], want[5]);
    return 1;
}

/* Drive the driver's own start-up and discovery against a model of an
 * adapter.  What this proves is narrow and worth stating: that the sequence is
 * in the right order, that each command is matched to its own answer, and that
 * an address arriving backwards is turned round.  It does not prove a real
 * adapter agrees - nothing here can, until there is one. */
void btusb_model_attach(void);
void btusb_model_detach(void);
bool btusb_model_reset_first(void);
int  btusb_model_commands(void);
bool btusb_model_address_read(void);
const u8 *btusb_model_address(void);
const u8 *btusb_model_found_address(void);
void btusb_start(void *ctx);

/* A keyboard over Bluetooth, from nothing to a key.
 *
 * Every layer under this was already verified and none of it had ever been
 * used for anything: a link that carries bytes is not a keyboard, and the
 * whole point of a profile is the agreement about what the bytes mean.
 *
 * What is driven here is that agreement, end to end - find what the device
 * publishes, read the description of its reports across several messages,
 * parse it with the SAME parser the USB keyboards use, ask to be told, and
 * receive a key.  The model refuses to report anything until it is asked,
 * which is the one mistake that produces a keyboard that connects, describes
 * itself perfectly, and does nothing.
 */
bool bt_hid_attach(void *transport);
bool bt_hid_poll(u32 timeout_ms);
bool bt_hid_present(void);
void bt_hid_detach(void);
u32  bt_hid_reports(void);
bool btusb_model_was_subscribed(void);
int  btusb_model_reads_before_subscribed(void);
void btusb_model_forget_subscription(void);
int  btusb_model_battery(void);
int  bt_battery_percent(void *transport);

static int drive_bluetooth_keyboard(bt_t *bt) {
    int failures = 0;

    /* A link has to be open for any of this; the model answers on it. */
    memset(&link, 0, sizeof link);
    link.used = true;
    link.low_energy = true;
    link.handle = 0x0040;

    btusb_model_forget_subscription();

    if (!bt_hid_attach(bt)) {
        kwarn("btusb", "selftest: the keyboard the model publishes was not "
                       "taken on");
        link.used = false;
        return 1;
    }

    /* Asked to report, and asked BEFORE anything was expected of it. */
    if (!btusb_model_was_subscribed()) {
        kwarn("btusb", "selftest: the device was never asked to report, so it "
                       "would describe itself and send nothing");
        failures++;
    }

    /* And now a key, which only arrives because it was asked for. */
    if (!bt_hid_poll(100)) {
        kwarn("btusb", "selftest: no report arrived from the keyboard");
        failures++;
    } else if (bt_hid_reports() != 1) {
        kwarn("btusb", "selftest: %u report(s) counted, expected 1",
              bt_hid_reports());
        failures++;
    }

    /* And what it says is left in it, which is a different service on the
     * same channel and costs almost nothing once the channel works. */
    int battery = bt_battery_percent(bt);
    if (battery != btusb_model_battery()) {
        kwarn("btusb", "selftest: the device says %d%% of its battery is left "
                       "and the driver read %d%%", btusb_model_battery(),
              battery);
        failures++;
    }

    if (!failures)
        kinfo("btusb", "a keyboard over Bluetooth: its report description read "
                       "in pieces and parsed by the same parser the USB "
                       "keyboards use, a key press received, and %d%% of its "
                       "battery left", battery);

    /* Let go of the key the model reported.  A held key is a state, not an
     * event, and leaving it held repeats it into every window forever. */
    bt_hid_detach();

    link.used = false;
    return failures;
}

/* Register a modelled adapter as a LIVE one, so the `bt` tool can scan and
 * connect against it on a machine that has no real Bluetooth radio - a virtual
 * machine, chiefly.  Behind the `btmodel` boot flag: a machine with a real
 * radio (the user's has a Realtek one) must not be handed a fake alongside it.
 *
 * The same model the selftest drives, but it stays attached and lives in the
 * adapters table where btusb_adapter(0) finds it. */
void btusb_model_register(void) {
    bt_t *bt = NULL;
    for (int i = 0; i < MAX_ADAPTERS; i++)
        if (!adapters[i].used) { bt = &adapters[i]; break; }
    if (!bt) return;

    memset(bt, 0, sizeof *bt);
    bt->used = true;
    bt->modelled = true;
    btusb_model_attach();
    btusb_start(bt);
    kinfo("btusb", "a modelled adapter is live for scanning and connecting "
                   "(the `btmodel` boot flag); a real radio is used in "
                   "preference where one is present");
}

static int drive_against_model(void) {
    int failures = 0;
    static bt_t model_bt;

    memset(&model_bt, 0, sizeof model_bt);
    model_bt.used = true;
    model_bt.modelled = true;

    btusb_model_attach();
    btusb_start(&model_bt);

    if (!model_bt.reset_done) {
        kwarn("btusb", "selftest: the adapter was not reset");
        failures++;
    }
    if (!btusb_model_reset_first()) {
        kwarn("btusb", "selftest: something was asked of the adapter before it "
                       "was reset - a real one answers those unreliably");
        failures++;
    }
    if (btusb_model_commands() < 3) {
        kwarn("btusb", "selftest: only %d command(s) were sent",
              btusb_model_commands());
        failures++;
    }

    /* The address, turned round.  A driver that keeps wire order reports an
     * address that looks entirely real and belongs to nobody. */
    if (!btusb_model_address_read()) {
        kwarn("btusb", "selftest: the adapter's own address was never asked for");
        failures++;
    } else if (memcmp(model_bt.bd_addr, btusb_model_address(), 6) != 0) {
        kwarn("btusb", "selftest: the adapter's address came out "
                       "%02x:%02x:%02x:%02x:%02x:%02x - it arrives least "
                       "significant byte first and has to be turned round",
              model_bt.bd_addr[0], model_bt.bd_addr[1], model_bt.bd_addr[2],
              model_bt.bd_addr[3], model_bt.bd_addr[4], model_bt.bd_addr[5]);
        failures++;
    }

    if (model_bt.manufacturer != 15) {
        kwarn("btusb", "selftest: the maker read back as %u, expected 15",
              model_bt.manufacturer);
        failures++;
    }

    /* And discovery, all the way round: the driver asks, the model answers
     * with a device, and the driver files it.
     *
     * The inquiry is sent directly rather than through btusb_scan, which
     * listens for however many seconds it was asked for - a wait that belongs
     * in a scan and not in a check that runs at every boot. */
    memset(found, 0, sizeof found);
    send_command(&model_bt, HCI_INQUIRY, NULL, 0);

    {
        bt_found_t *f = NULL;
        for (int i = 0; i < MAX_FOUND; i++)
            if (found[i].used) { f = &found[i]; break; }

        if (!f) {
            kwarn("btusb", "selftest: an inquiry found nothing, though the "
                           "model answered with a device");
            failures++;
        } else {
            failures += check_addr(f->addr, btusb_model_found_address(),
                                   "a discovered address");
            if (strcmp(btusb_kind_name(f->device_class), "audio")) {
                kwarn("btusb", "selftest: the discovered device's class read "
                               "as \"%s\", expected audio",
                      btusb_kind_name(f->device_class));
                failures++;
            }
        }
    }

    /* And the profile on top, which is what makes any of this a keyboard. */
    failures += drive_bluetooth_keyboard(&model_bt);

    btusb_model_detach();
    return failures;
}

int btusb_selftest(void) {
    int failures = 0;

    memset(found, 0, sizeof found);
    memset(&link, 0, sizeof link);

    /* An address on the wire is least significant byte first, which is the
     * reverse of how one is written down.  Reading it in wire order is the
     * single easiest mistake here and produces an address that looks entirely
     * reasonable. */
    {
        const u8 wire[6] = { 0x21, 0x43, 0x65, 0x87, 0xA9, 0xCB };
        const u8 want[6] = { 0xCB, 0xA9, 0x87, 0x65, 0x43, 0x21 };
        u8 got[6];
        addr_from_wire(got, wire);
        failures += check_addr(got, want, "an address off the wire");
    }

    /* A low-energy advertisement carrying a complete name.
     *
     *   reports = 1
     *   event type 0, address type 0, address (little endian)
     *   data length 9: {2, flags=0x01, 0x06} then {5, name=0x09, "Pods"}
     *   then the signal strength, which follows the data
     */
    {
        const u8 report[] = {
            0x01,                                     /* one report          */
            0x00, 0x00,                               /* type, address type  */
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66,       /* address, wire order */
            0x09,                                     /* nine bytes of data  */
            0x02, 0x01, 0x06,                         /* flags               */
            0x05, 0x09, 'P', 'o', 'd', 's',           /* complete name       */
            (u8)(s8)-55,                              /* signal, -55 dBm     */
        };
        advertising_report(report, (int)sizeof report);

        const u8 want[6] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x11 };
        bt_found_t *f = NULL;
        for (int i = 0; i < MAX_FOUND; i++)
            if (found[i].used) { f = &found[i]; break; }

        if (!f) {
            kwarn("btusb", "selftest: an advertisement was not remembered");
            failures++;
        } else {
            failures += check_addr(f->addr, want, "an advertised address");
            if (!f->low_energy) {
                kwarn("btusb", "selftest: an advertisement was filed as the "
                               "older protocol");
                failures++;
            }
            if (strcmp(f->name, "Pods")) {
                kwarn("btusb", "selftest: the name came out \"%s\", expected "
                               "\"Pods\"", f->name);
                failures++;
            }
            if (f->rssi != -55) {
                kwarn("btusb", "selftest: the signal came out %d, expected -55",
                      f->rssi);
                failures++;
            }
        }
    }

    /* The same device advertising again is the same device, not a second one.
     * A scan that lists everything twice is one that filed by something other
     * than the address. */
    {
        const u8 again[] = {
            0x01, 0x00, 0x00,
            0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
            0x02, 0x01, 0x06,
            (u8)(s8)-60,
        };
        advertising_report(again, (int)sizeof again);

        int n = 0;
        for (int i = 0; i < MAX_FOUND; i++) if (found[i].used) n++;
        if (n != 1) {
            kwarn("btusb", "selftest: the same device was remembered %d times",
                  n);
            failures++;
        }
    }

    /* An inquiry result on the older protocol, whose class of device says what
     * the thing is.  0x240404 is an audio device: the major class is five bits
     * from bit eight, and four there means audio. */
    {
        memset(found, 0, sizeof found);
        const u8 result[] = {
            0x01,                                     /* one device          */
            0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,       /* address, wire order */
            0x01, 0x00, 0x00,                         /* scan modes, reserved */
            0x04, 0x04, 0x24,                         /* class, little endian */
            0x00, 0x00,                               /* clock offset        */
        };
        inquiry_result(result, (int)sizeof result, HCI_EV_INQUIRY_RESULT);

        bt_found_t *f = NULL;
        for (int i = 0; i < MAX_FOUND; i++)
            if (found[i].used) { f = &found[i]; break; }

        const u8 want[6] = { 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA };
        if (!f) {
            kwarn("btusb", "selftest: an inquiry result was not remembered");
            failures++;
        } else {
            failures += check_addr(f->addr, want, "an inquiry address");
            if (f->low_energy) {
                kwarn("btusb", "selftest: an inquiry result was filed as "
                               "low energy");
                failures++;
            }
            if (strcmp(btusb_kind_name(f->device_class), "audio")) {
                kwarn("btusb", "selftest: class %#x read as \"%s\", expected "
                               "audio", f->device_class,
                      btusb_kind_name(f->device_class));
                failures++;
            }
        }
    }

    memset(found, 0, sizeof found);

    /* And the conversation, not only the parsing: the driver's own start-up
     * sequence run against a model of an adapter. */
    failures += drive_against_model();
    memset(found, 0, sizeof found);

    if (!failures)
        kinfo("btusb", "the event parsing reads addresses, names, signal "
                       "strengths and device classes correctly, and the "
                       "adapter start-up runs in order against a model");
    return failures;
}

/* ---------------------------------------------------------------- scanning
 *
 * Two radios' worth of discovery, started together because a person asking
 * what is nearby does not care which of the two a device speaks.
 *
 * The event mask has to be widened first.  An adapter fresh from a reset
 * reports only the events the specification calls default, and the low-energy
 * meta-event - which is how every advertisement arrives - is not among them.
 * Leaving it masked produces a scan that runs correctly and finds nothing,
 * which is indistinguishable from an empty room.
 */
bool btusb_scan(void *ctx, int seconds) {
    bt_t *bt = ctx;
    if (!bt || !bt->used) return false;
    if (seconds < 1) seconds = 1;
    if (seconds > 30) seconds = 30;

    /* Every event, including the low-energy meta-event. */
    u8 mask[8];
    memset(mask, 0xFF, sizeof mask);
    send_command(bt, HCI_SET_EVENT_MASK, mask, sizeof mask);
    wait_for_complete(bt, HCI_SET_EVENT_MASK, NULL, NULL, 1000);

    u8 le_mask[8] = { 0x1F, 0, 0, 0, 0, 0, 0, 0 };
    send_command(bt, HCI_LE_SET_EVENT_MASK, le_mask, sizeof le_mask);
    wait_for_complete(bt, HCI_LE_SET_EVENT_MASK, NULL, NULL, 1000);

    /* Passive listening rather than asking: a passive scan hears anything that
     * advertises without transmitting, which is enough to list what is there
     * and does not announce this machine to the room. */
    u8 params[7] = {
        0x00,               /* passive                                      */
        0x10, 0x00,         /* interval, in 0.625 ms units                  */
        0x10, 0x00,         /* window, the same - so it listens continuously */
        0x00,               /* this machine's own public address            */
        0x00,               /* accept everything, not just known devices    */
    };
    send_command(bt, HCI_LE_SET_SCAN_PARAMS, params, sizeof params);
    wait_for_complete(bt, HCI_LE_SET_SCAN_PARAMS, NULL, NULL, 1000);

    u8 enable[2] = { 0x01, 0x01 };   /* on, and drop duplicate reports      */
    if (!send_command(bt, HCI_LE_SET_SCAN_ENABLE, enable, sizeof enable) ||
        !wait_for_complete(bt, HCI_LE_SET_SCAN_ENABLE, NULL, NULL, 1000)) {
        kwarn("btusb", "the adapter would not start a low-energy scan");
    }

    /* And the older kind, which runs for a fixed length of its own.  The unit
     * is 1.28 seconds, so the count is the seconds asked for divided by that,
     * and at least one. */
    u8 inquiry[5] = { 0x33, 0x8B, 0x9E,          /* the general inquiry code */
                      (u8)((seconds * 100) / 128 + 1), 0 };
    if (!send_command(bt, HCI_INQUIRY, inquiry, sizeof inquiry)) {
        kwarn("btusb", "the adapter would not start an inquiry");
    }

    kinfo("btusb", "listening for %d second(s)", seconds);
    return true;
}

/* Stop, so the radio is not left scanning after whoever asked has gone. */
void btusb_scan_stop(void *ctx) {
    bt_t *bt = ctx;
    if (!bt || !bt->used) return;

    u8 off[2] = { 0x00, 0x00 };
    send_command(bt, HCI_LE_SET_SCAN_ENABLE, off, sizeof off);
    wait_for_complete(bt, HCI_LE_SET_SCAN_ENABLE, NULL, NULL, 500);

    send_command(bt, HCI_INQUIRY_CANCEL, NULL, 0);
    wait_for_complete(bt, HCI_INQUIRY_CANCEL, NULL, NULL, 500);
}

/* Open a link on the older protocol, pairing on the way if it has to.
 *
 * Almost every headset, keyboard and mouse is this kind.  The difference from
 * the low-energy path is that a link alone is not enough: the device will
 * refuse to carry anything until both ends hold the same key, and getting one
 * means a short exchange the adapter conducts and this answers - see the
 * events above.
 */
static bool connect_classic(bt_t *bt, const u8 *addr) {
    /* Secure Simple Pairing, which is what everything made since about 2007
     * uses.  Without this the adapter falls back to asking for a PIN, and
     * there is nowhere here to type one. */
    u8 on = 1;
    send_command(bt, HCI_WRITE_SSP_MODE, &on, 1);
    wait_for_complete(bt, HCI_WRITE_SSP_MODE, NULL, NULL, 1000);

    pairing_adapter = bt;
    memcpy(pairing_addr, addr, 6);
    pairing_done = false;
    pairing_ok = false;

    u8 p[13];
    for (int i = 0; i < 6; i++) p[i] = addr[5 - i];
    p[6] = 0x18; p[7] = 0xCC;      /* the packet types a headset accepts     */
    p[8] = 0x01;                   /* page scan repetition mode              */
    p[9] = 0x00;                   /* reserved                               */
    p[10] = 0x00; p[11] = 0x00;    /* let the adapter choose the clock offset */
    p[12] = 0x01;                  /* this end may become the central        */

    if (!send_command(bt, HCI_CREATE_CONNECTION, p, sizeof p)) {
        kwarn("btusb", "the adapter would not take a connect request");
        pairing_adapter = NULL;
        return false;
    }

    kinfo("btusb", "opening a link to %02x:%02x:%02x:%02x:%02x:%02x",
          addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    /* A device that is asleep has to be woken by the paging above, which can
     * take several seconds on something with a long page scan interval. */
    for (int i = 0; i < 1000; i++) {
        if (link.used) break;
        timer_mdelay(10);
    }
    if (!link.used) {
        kwarn("btusb", "no answer; the device may be off, out of range, or "
                       "not accepting connections");
        pairing_adapter = NULL;
        return false;
    }

    /* Ask for a key if there is not one already.  The exchange that follows is
     * answered by the event handler. */
    if (!key_for(addr)) {
        u8 h[2] = { (u8)link.handle, (u8)(link.handle >> 8) };
        send_command(bt, HCI_AUTH_REQUESTED, h, sizeof h);

        for (int i = 0; i < 3000; i++) {
            if (pairing_done) break;
            timer_mdelay(10);
        }
        if (!pairing_ok) {
            kwarn("btusb", "the link opened but the two ends did not agree on "
                           "a key, so it will carry nothing");
            pairing_adapter = NULL;
            return false;
        }
    }

    /* And turn encryption on, which most devices require before they will
     * accept anything at all. */
    u8 e[3] = { (u8)link.handle, (u8)(link.handle >> 8), 0x01 };
    send_command(bt, HCI_SET_CONN_ENCRYPTION, e, sizeof e);

    pairing_adapter = NULL;
    return true;
}

/* Open a link to one of the devices discovery found.
 *
 * Only the low-energy kind.  The older sort will not carry anything until the
 * two ends have agreed on security, and agreeing on security means a pairing
 * exchange and somewhere to keep the result - neither of which exists here
 * yet.  Refusing plainly is better than opening something that then drops
 * every packet for a reason nothing reports.
 */
bool btusb_connect(void *ctx, const u8 *addr) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !addr) return false;

    if (link.used) {
        kwarn("btusb", "there is already a link open; close it first");
        return false;
    }

    /* Scanning and connecting use the same radio, and an adapter asked to do
     * both answers the second with "command disallowed". */
    btusb_scan_stop(ctx);

    /* Is this one of the low-energy devices?  Asked rather than assumed,
     * because the answer decides whether this can work at all. */
    bool known_le = false;
    for (int i = 0; i < MAX_FOUND; i++)
        if (found[i].used && !memcmp(found[i].addr, addr, 6)) {
            known_le = found[i].low_energy;
            break;
        }
    if (!known_le) return connect_classic(bt, addr);

    u8 p[25];
    memset(p, 0, sizeof p);
    p[0] = 0x60; p[1] = 0x00;      /* how often to look, in 0.625 ms units   */
    p[2] = 0x30; p[3] = 0x00;      /* and for how long each time             */
    p[4] = 0x00;                   /* use the address given, not a list      */
    p[5] = 0x00;                   /* a public address                       */
    for (int i = 0; i < 6; i++) p[6 + i] = addr[5 - i];   /* back to the wire */
    p[12] = 0x00;                  /* this machine's own public address      */
    p[13] = 0x18; p[14] = 0x00;    /* shortest exchange interval, 30 ms      */
    p[15] = 0x28; p[16] = 0x00;    /* longest, 50 ms                          */
    p[17] = 0x00; p[18] = 0x00;    /* no skipped exchanges                    */
    p[19] = 0x2A; p[20] = 0x00;    /* give up after 4.2 s of silence          */

    if (!send_command(bt, HCI_LE_CREATE_CONN, p, 21)) {
        kwarn("btusb", "the adapter would not take a connect request");
        return false;
    }

    kinfo("btusb", "opening a link to %02x:%02x:%02x:%02x:%02x:%02x",
          addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

    /* The answer is an event rather than a completion, and it can take as long
     * as the other end's advertising interval. */
    for (int i = 0; i < 500; i++) {
        if (link.used) return true;
        timer_mdelay(10);
    }

    kwarn("btusb", "no answer; the device may be out of range or not "
                   "advertising");
    send_command(bt, HCI_LE_CREATE_CONN_CANCEL, NULL, 0);
    return false;
}

void btusb_disconnect(void *ctx) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !link.used) return;

    u8 p[3] = { (u8)link.handle, (u8)(link.handle >> 8), 0x13 };  /* user ended */
    send_command(bt, HCI_DISCONNECT, p, sizeof p);

    for (int i = 0; i < 200; i++) {
        if (!link.used) return;
        timer_mdelay(10);
    }
    memset(&link, 0, sizeof link);
}

/* ------------------------------------------------------------------- L2CAP
 *
 * A link on its own carries nothing addressable.  Everything that travels over
 * one is inside L2CAP, which is a thin envelope: a length, a channel, and the
 * payload.  The channels below 0x40 are fixed and mean the same thing on every
 * device, which is what makes them usable without negotiating anything first.
 *
 *   0x0004  attributes - reading and writing a device's values
 *   0x0005  the low-energy signalling channel
 *   0x0006  security, which is where pairing would happen
 *
 * The attribute channel is the one worth having: it is how a heart-rate strap
 * reports a heart rate and how almost anything low-energy reports a battery
 * level, and it needs no pairing for values a device is willing to publish.
 */
#define L2CAP_CID_ATT     0x0004

#define ATT_READ_BY_TYPE_REQ  0x08
#define ATT_READ_BY_TYPE_RSP  0x09
#define ATT_READ_REQ          0x0A
#define ATT_READ_RSP          0x0B
#define ATT_ERROR_RSP         0x01

/* What a keyboard needs beyond reading one short value.
 *
 *   A report descriptor is longer than one message can carry, so it is read in
 *   pieces: each request says where to carry on from, and the last piece is
 *   the one shorter than the rest.
 *
 *   Nothing arrives from a device until it is asked to send it.  Writing two
 *   bytes to the descriptor beside a value is how "tell me when this changes"
 *   is spelled, and without it a keyboard connects, publishes everything about
 *   itself, and reports no keys at all - which looks exactly like a keyboard
 *   that is not working.
 *
 *   And then reports arrive unasked, which is the one thing on this channel
 *   that is not an answer to a question.
 */
#define ATT_READ_BLOB_REQ     0x0C
#define ATT_READ_BLOB_RSP     0x0D
#define ATT_WRITE_REQ         0x12
#define ATT_WRITE_RSP         0x13
#define ATT_HANDLE_VALUE_NTF  0x1B

/* The service a keyboard or mouse publishes, and the values inside it. */
#define UUID_HID_SERVICE      0x1812
#define UUID_REPORT_MAP       0x2A4B
#define UUID_REPORT           0x2A4D
#define UUID_HID_INFORMATION  0x2A4A
#define UUID_CCC_DESCRIPTOR   0x2902   /* where "keep me told" is written */

/* An ACL packet: the link it belongs to, how the first two bits say whether it
 * begins a message or continues one, and then the L2CAP envelope. */
static bool acl_send(bt_t *bt, u16 cid, const void *payload, u16 len) {
    if (!bt || !link.used || len > 200) return false;

    u8 pkt[256];
    u16 handle = link.handle & 0x0FFF;

    /* 0x2000 in the top bits: the first packet of a message, and the message
     * is going to a single device rather than being broadcast. */
    u16 flags = handle | 0x2000;
    pkt[0] = (u8)flags;
    pkt[1] = (u8)(flags >> 8);
    pkt[2] = (u8)(len + 4);            /* everything after this field       */
    pkt[3] = (u8)((len + 4) >> 8);
    pkt[4] = (u8)len;                  /* the L2CAP payload alone           */
    pkt[5] = (u8)(len >> 8);
    pkt[6] = (u8)cid;
    pkt[7] = (u8)(cid >> 8);
    if (payload && len) memcpy(pkt + 8, payload, len);

    /* Against a model there is no bus to put this on.  The model is handed the
     * envelope's contents and keeps whatever answer it decides on until the
     * driver asks for it, which is the same shape as a real device: the answer
     * is not produced by the sending, it is waiting afterwards. */
    if (bt->modelled) {
        btusb_model_attribute(cid, payload, len);
        return true;
    }

    int n = usb_bulk(bt->dev, false, pkt, (u32)(len + 8), 1000);
    return n == (int)(len + 8);
}

static int acl_receive(bt_t *bt, u16 *cid_out, void *payload, u32 cap,
                       u32 timeout_ms) {
    if (!bt || !link.used) return -1;

    if (bt->modelled) {
        int got = btusb_model_attribute_reply(cid_out, payload, cap);
        return got;
    }

    u8 pkt[256];
    int n = usb_bulk(bt->dev, true, pkt, sizeof pkt, timeout_ms);
    if (n < 8) return -1;

    u16 l2_len = (u16)(pkt[4] | (pkt[5] << 8));
    u16 cid    = (u16)(pkt[6] | (pkt[7] << 8));
    if (cid_out) *cid_out = cid;

    if (l2_len > (u16)(n - 8)) l2_len = (u16)(n - 8);
    if (l2_len > cap) l2_len = (u16)cap;
    if (payload && l2_len) memcpy(payload, pkt + 8, l2_len);
    return (int)l2_len;
}

/* Read one of a device's values by its handle.
 *
 * The handle is a number the device assigns to each thing it publishes; a
 * device lists them when asked, and this reads one that has already been
 * found.  What comes back is the value as the device stores it - this does not
 * interpret it, because what a value means is the profile's business and not
 * the transport's.
 */
int btusb_read_attribute(void *ctx, u16 attr_handle, void *out, u32 cap) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !link.used) return -1;

    u8 req[3] = { ATT_READ_REQ, (u8)attr_handle, (u8)(attr_handle >> 8) };
    if (!acl_send(bt, L2CAP_CID_ATT, req, sizeof req)) {
        kwarn("btusb", "the request would not go out");
        return -1;
    }

    u8 rsp[64];
    u16 cid = 0;
    int n = acl_receive(bt, &cid, rsp, sizeof rsp, 2000);
    if (n < 1) {
        kwarn("btusb", "no answer to reading attribute %u", attr_handle);
        return -1;
    }
    if (cid != L2CAP_CID_ATT) return -1;

    if (rsp[0] == ATT_ERROR_RSP) {
        /* The device answered and refused, which is a different thing from
         * silence and worth reporting as such: the usual reason is that the
         * value needs a pairing this does not do. */
        kwarn("btusb", "the device refused attribute %u (error %#x)",
              attr_handle, n >= 5 ? rsp[4] : 0);
        return -1;
    }
    if (rsp[0] != ATT_READ_RSP) return -1;

    int len = n - 1;
    if (len > (int)cap) len = (int)cap;
    if (out && len > 0) memcpy(out, rsp + 1, (size_t)len);
    return len;
}

/* Read a value too long for one message.
 *
 * Each request names where to carry on from and the device answers with as
 * much as fits.  It is finished when an answer comes back shorter than the one
 * before it - not when it comes back empty, which is what a device that ended
 * exactly on a boundary sends and which a driver testing for empty would wait
 * for forever.
 */
int btusb_read_long_attribute(void *ctx, u16 attr_handle, void *out, u32 cap) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !link.used || !out) return -1;

    u8 *dst = out;
    u32 got = 0;
    int chunk_before = -1;

    while (got < cap) {
        u8 req[5] = { ATT_READ_BLOB_REQ, (u8)attr_handle, (u8)(attr_handle >> 8),
                      (u8)got, (u8)(got >> 8) };
        if (!acl_send(bt, L2CAP_CID_ATT, req, sizeof req)) return -1;

        u8 rsp[64];
        u16 cid = 0;
        int n = acl_receive(bt, &cid, rsp, sizeof rsp, 2000);
        if (n < 1 || cid != L2CAP_CID_ATT) return -1;

        if (rsp[0] == ATT_ERROR_RSP) {
            /* Reading past the end is how a device says "that was all", and is
             * not a failure when something has already arrived. */
            if (got) break;
            return -1;
        }
        if (rsp[0] != ATT_READ_BLOB_RSP) return -1;

        int piece = n - 1;
        if (piece <= 0) break;
        if ((u32)piece > cap - got) piece = (int)(cap - got);
        memcpy(dst + got, rsp + 1, (size_t)piece);
        got += (u32)piece;

        /* Shorter than the last one means this was the last one. */
        if (chunk_before >= 0 && piece < chunk_before) break;
        chunk_before = piece;
    }

    return (int)got;
}

/* Write to an attribute and wait for the device to say it took it. */
bool btusb_write_attribute(void *ctx, u16 attr_handle, const void *value,
                           u16 len) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !link.used || len > 32) return false;

    u8 req[40];
    req[0] = ATT_WRITE_REQ;
    req[1] = (u8)attr_handle;
    req[2] = (u8)(attr_handle >> 8);
    if (value && len) memcpy(req + 3, value, len);

    if (!acl_send(bt, L2CAP_CID_ATT, req, (u16)(3 + len))) return false;

    u8 rsp[32];
    u16 cid = 0;
    int n = acl_receive(bt, &cid, rsp, sizeof rsp, 2000);
    if (n < 1 || cid != L2CAP_CID_ATT) return false;

    if (rsp[0] == ATT_ERROR_RSP) {
        kwarn("btusb", "the device refused a write to attribute %u (error %#x)",
              attr_handle, n >= 5 ? rsp[4] : 0);
        return false;
    }
    return rsp[0] == ATT_WRITE_RSP;
}

/* Wait for a device to send something it was told to keep us told about.
 *
 * Everything else on this channel is an answer; this is the one thing that
 * arrives on its own.  Returns the attribute it concerns and how many bytes
 * came with it. */
int btusb_await_notification(void *ctx, u16 *attr_handle, void *out, u32 cap,
                             u32 timeout_ms) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !link.used) return -1;

    u8 pkt[64];
    u16 cid = 0;
    int n = acl_receive(bt, &cid, pkt, sizeof pkt, timeout_ms);
    if (n < 3 || cid != L2CAP_CID_ATT) return -1;
    if (pkt[0] != ATT_HANDLE_VALUE_NTF) return -1;

    if (attr_handle) *attr_handle = (u16)(pkt[1] | (pkt[2] << 8));

    int len = n - 3;
    if (len > (int)cap) len = (int)cap;
    if (out && len > 0) memcpy(out, pkt + 3, (size_t)len);
    return len;
}

/* Find where a device keeps a particular kind of value.
 *
 * A device's attributes are a flat numbered list, and what makes it navigable
 * is that every value is announced by a declaration in front of it: a byte of
 * properties, the number the value itself lives at, and what kind of value it
 * is.  Reading all the declarations and keeping the ones of the right kind is
 * how anything on this channel is found.
 *
 * Asked for in batches, because a device answers with as many as fit in one
 * message and expects to be asked again starting after the last one.  A driver
 * that asks once finds whatever fitted and silently misses the rest - which on
 * a keyboard means finding the letters and not the volume keys.
 */
#define ATT_TYPE_CHARACTERISTIC  0x2803

int btusb_find_characteristics(void *ctx, u16 uuid, u16 *handles, int max) {
    bt_t *bt = ctx;
    if (!bt || !bt->used || !link.used || !handles || max <= 0) return -1;

    int found_here = 0;
    u16 from = 0x0001;

    while (from && from < 0xFFFF && found_here < max) {
        u8 req[7] = { ATT_READ_BY_TYPE_REQ,
                      (u8)from, (u8)(from >> 8),
                      0xFF, 0xFF,
                      (u8)ATT_TYPE_CHARACTERISTIC,
                      (u8)(ATT_TYPE_CHARACTERISTIC >> 8) };
        if (!acl_send(bt, L2CAP_CID_ATT, req, sizeof req)) break;

        u8 rsp[64];
        u16 cid = 0;
        int n = acl_receive(bt, &cid, rsp, sizeof rsp, 2000);
        if (n < 2 || cid != L2CAP_CID_ATT) break;

        /* Running off the end is how the list finishes, not a failure. */
        if (rsp[0] == ATT_ERROR_RSP) break;
        if (rsp[0] != ATT_READ_BY_TYPE_RSP) break;

        int each = rsp[1];
        /* Two for the declaration's own number, then properties, then the two
         * for where the value is, then the kind - which is two bytes for the
         * kinds this understands and sixteen for a maker's own. */
        if (each < 7) break;

        int entries = (n - 2) / each;
        if (entries <= 0) break;

        u16 last = 0;
        for (int i = 0; i < entries && found_here < max; i++) {
            const u8 *e = rsp + 2 + i * each;
            last = (u16)(e[0] | (e[1] << 8));

            u16 value_at = (u16)(e[3] | (e[4] << 8));
            u16 kind = (u16)(e[5] | (e[6] << 8));

            if (each == 7 && kind == uuid) handles[found_here++] = value_at;
        }

        if (!last || last == 0xFFFF) break;
        from = (u16)(last + 1);
    }

    return found_here;
}

bool btusb_link(u8 *addr, u16 *handle, u16 *interval_us) {
    if (!link.used) return false;
    if (addr) memcpy(addr, link.addr, 6);
    if (handle) *handle = link.handle;
    if (interval_us) *interval_us = (u16)(link.interval * 1250 / 1000);
    return true;
}

int btusb_found_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_FOUND; i++) if (found[i].used) n++;
    return n;
}

bool btusb_found_get(int index, u8 *addr, char *name, size_t name_cap,
                     int *rssi, u32 *device_class, bool *low_energy) {
    int n = 0;
    for (int i = 0; i < MAX_FOUND; i++) {
        if (!found[i].used) continue;
        if (n++ != index) continue;

        if (addr) memcpy(addr, found[i].addr, 6);
        if (name) strlcpy(name, found[i].name, name_cap);
        if (rssi) *rssi = found[i].rssi;
        if (device_class) *device_class = found[i].device_class;
        if (low_energy) *low_energy = found[i].low_energy;
        return true;
    }
    return false;
}

/* What the class of device says it is.  The major class is five bits starting
 * at bit eight, and it is the part worth showing: somebody choosing from a
 * list wants to know which entry is the headset. */
const char *btusb_kind_name(u32 device_class) {
    switch ((device_class >> 8) & 0x1F) {
    case 0x01: return "computer";
    case 0x02: return "phone";
    case 0x03: return "network";
    case 0x04: return "audio";
    case 0x05: return "keyboard or mouse";
    case 0x06: return "imaging";
    case 0x07: return "wearable";
    case 0x08: return "toy";
    case 0x09: return "health";
    default:   return "";
    }
}

/* The adapter itself, for the operations that act on it rather than read it. */
void *btusb_adapter(int index) {
    int n = 0;
    for (int i = 0; i < MAX_ADAPTERS; i++) {
        if (!adapters[i].used) continue;
        if (n++ == index) return &adapters[i];
    }
    return NULL;
}

int btusb_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_ADAPTERS; i++) if (adapters[i].used) n++;
    return n;
}

bool btusb_get(int index, char *maker, size_t maker_cap, u8 *addr,
               u8 *version_out) {
    int n = 0;
    for (int i = 0; i < MAX_ADAPTERS; i++) {
        if (!adapters[i].used) continue;
        if (n++ != index) continue;

        if (maker) strlcpy(maker, adapters[i].maker, maker_cap);
        if (addr) memcpy(addr, adapters[i].bd_addr, 6);
        if (version_out) *version_out = adapters[i].hci_version;
        return true;
    }
    return false;
}

void btusb_detach(void *ctx) {
    bt_t *bt = ctx;
    if (bt) bt->used = false;
}
