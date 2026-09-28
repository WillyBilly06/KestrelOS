/* btusb_model.c - a Bluetooth controller, in software, for the driver to be
 * driven against.
 *
 * Every other piece of hardware this system drives and cannot get hold of has
 * one of these: the Intel and MediaTek and Atheros and Realtek radios, four
 * NVIDIA models, an AMD one, a Realtek Ethernet one.  Bluetooth was the
 * exception, and the consequence was precise: the driver's command sequence -
 * the reset, the version, the address, the event mask, the inquiry - had never
 * once been run. Its parsing was tested and its conversation was not.
 *
 * ---------------------------------------------------------------------------
 * WHAT A MODEL OF THIS HAS TO GET RIGHT
 *
 * A Bluetooth controller is not a register file.  It is a device that is sent
 * commands and answers with events, and the two travel by different routes -
 * commands out through a control transfer, events in on an interrupt endpoint.
 * So this is not a memory that answers reads; it is a correspondent.
 *
 * The things it has to do faithfully, because each one is a way for a driver
 * to be wrong and look right:
 *
 *   It answers exactly one Command Complete per command, carrying the opcode
 *   it is answering.  A driver that waits for "an event" rather than for the
 *   answer to what it asked works perfectly until two things are in flight.
 *
 *   It refuses a command sent before the reset.  A controller the firmware
 *   left half-configured answers oddly until reset, which is precisely why the
 *   driver resets first - and a model that answers anything at any time would
 *   let a driver that skipped it pass.
 *
 *   Its address arrives least significant byte first, and reads backwards to
 *   a person.  A driver that prints it in wire order prints a real-looking
 *   address that is not the one on the label.
 */
#include "kernel.h"
#include "klog.h"

/* The driver's own entry points, which this stands in front of and behind. */
void btusb_event(void *ctx, const u8 *data, int len);

#define HCI_OP(ogf, ocf)     ((u16)(((ogf) << 10) | (ocf)))
#define HCI_RESET            HCI_OP(0x03, 0x0003)
#define HCI_READ_LOCAL_VER   HCI_OP(0x04, 0x0001)
#define HCI_READ_BD_ADDR     HCI_OP(0x04, 0x0009)
#define HCI_SET_EVENT_MASK   HCI_OP(0x03, 0x0001)
#define HCI_INQUIRY          HCI_OP(0x01, 0x0001)
#define HCI_INQUIRY_CANCEL   HCI_OP(0x01, 0x0002)

#define EV_INQUIRY_COMPLETE  0x01
#define EV_INQUIRY_RESULT    0x02
#define EV_COMMAND_COMPLETE  0x0E
#define EV_COMMAND_STATUS    0x0F

/* Who the model says it is.  The address is written here the way a person
 * would say it; it goes onto the wire backwards. */
static const u8 model_address[6] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF };
static const u8 model_manufacturer_lo = 0x0F;   /* 15: Broadcom */
static const u8 model_manufacturer_hi = 0x00;

/* One device for an inquiry to find. */
static const u8 found_address[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

static bool attached;
static bool reset_seen;
static int  commands_seen;
static int  refused_before_reset;
static bool address_read;
static bool event_mask_set;
static bool inquiry_started;

/* ------------------------------------------------------------- answering */

static void complete(void *ctx, u16 opcode, const u8 *params, int len) {
    u8 e[64];
    e[0] = EV_COMMAND_COMPLETE;
    /* Command credit + opcode + status are four bytes before return data. */
    e[1] = (u8)(4 + len);
    e[2] = 1;                                  /* one command slot free again */
    e[3] = (u8)(opcode & 0xFF);
    e[4] = (u8)(opcode >> 8);
    e[5] = 0;                                  /* status: it worked */
    if (len > 0 && params) memcpy(e + 6, params, (size_t)len);
    btusb_event(ctx, e, 6 + len);
}

static void refuse(void *ctx, u16 opcode, u8 status) {
    u8 e[8];
    e[0] = EV_COMMAND_COMPLETE;
    e[1] = 4;
    e[2] = 1;
    e[3] = (u8)(opcode & 0xFF);
    e[4] = (u8)(opcode >> 8);
    e[5] = status;                             /* anything but zero is a no */
    btusb_event(ctx, e, 6);
}

/* One command, and the answer it deserves. */
void btusb_model_command(void *ctx, const u8 *packet, int len) {
    if (!attached || len < 3) return;

    u16 opcode = (u16)(packet[0] | (packet[1] << 8));
    commands_seen++;

    /* Before the reset, a controller that the firmware left half-configured
     * answers unreliably.  This one says no, which is the honest version of
     * the same thing and catches a driver that skipped it. */
    if (!reset_seen && opcode != HCI_RESET) {
        refused_before_reset++;
        refuse(ctx, opcode, 0x0C);             /* command disallowed */
        return;
    }

    switch (opcode) {
    case HCI_RESET:
        reset_seen = true;
        complete(ctx, opcode, NULL, 0);
        return;

    case HCI_READ_LOCAL_VER: {
        u8 p[8];
        p[0] = 0x0C;                           /* HCI version: 5.3 */
        p[1] = 0x34; p[2] = 0x12;              /* revision */
        p[3] = 0x0C;                           /* LMP version */
        p[4] = model_manufacturer_lo;
        p[5] = model_manufacturer_hi;
        p[6] = 0x00; p[7] = 0x00;              /* LMP subversion */
        complete(ctx, opcode, p, 8);
        return;
    }

    case HCI_READ_BD_ADDR: {
        /* Backwards, as it travels.  A driver that does not turn it round
         * reports an address that looks real and is not. */
        u8 p[6];
        for (int i = 0; i < 6; i++) p[i] = model_address[5 - i];
        address_read = true;
        complete(ctx, opcode, p, 6);
        return;
    }

    case HCI_SET_EVENT_MASK:
        event_mask_set = true;
        complete(ctx, opcode, NULL, 0);
        return;

    case HCI_INQUIRY: {
        /* An inquiry is answered by a Command Status rather than a Command
         * Complete - it has not finished, it has started.  Then the results
         * arrive, and then a completion. */
        u8 st[4] = { EV_COMMAND_STATUS, 4, 0x00, 1 };
        u8 e[8];
        e[0] = EV_COMMAND_STATUS;
        e[1] = 4;
        e[2] = 0x00;                           /* status */
        e[3] = 1;                              /* slots free */
        e[4] = (u8)(opcode & 0xFF);
        e[5] = (u8)(opcode >> 8);
        (void)st;
        btusb_event(ctx, e, 6);
        inquiry_started = true;

        /* One device answering, in the shape the specification gives: a count,
         * then the address least significant byte first, then what it says it
         * is. */
        u8 r[16];
        r[0] = EV_INQUIRY_RESULT;
        r[1] = 14;
        r[2] = 1;                              /* one response */
        for (int i = 0; i < 6; i++) r[3 + i] = found_address[5 - i];
        r[9] = 0x01;                           /* page scan repetition mode */
        r[10] = 0x00; r[11] = 0x00;            /* reserved */
        /* The class of device, three bytes least significant first.  The
         * major class is five bits from bit eight, and four there means
         * audio - so 0x240404, not 0x240204, which is a telephone.  This said
         * "audio headset" in a comment and carried a phone's number; the
         * driver read it correctly and the test caught the model. */
        r[12] = 0x04; r[13] = 0x04; r[14] = 0x24;
        r[15] = 0x00;
        btusb_event(ctx, r, 16);

        u8 done[4] = { EV_INQUIRY_COMPLETE, 1, 0x00, 0 };
        btusb_event(ctx, done, 3);
        return;
    }

    case HCI_INQUIRY_CANCEL:
        complete(ctx, opcode, NULL, 0);
        return;

    default:
        /* Anything else is accepted without comment, which is what a real
         * controller does with the many commands a driver sends and does not
         * check the answer to. */
        complete(ctx, opcode, NULL, 0);
        return;
    }
}

/* ------------------------------------------------------------- the harness */

void btusb_model_attach(void) {
    attached = true;
    reset_seen = false;
    commands_seen = 0;
    refused_before_reset = 0;
    address_read = false;
    event_mask_set = false;
    inquiry_started = false;
}

void btusb_model_detach(void) { attached = false; }

bool btusb_model_is_attached(void) { return attached; }
bool btusb_model_reset_first(void) {
    /* The reset happened, and nothing was refused for having come before it. */
    return reset_seen && refused_before_reset == 0;
}
int  btusb_model_commands(void) { return commands_seen; }
bool btusb_model_address_read(void) { return address_read; }
const u8 *btusb_model_address(void) { return model_address; }
const u8 *btusb_model_found_address(void) { return found_address; }

/* ==================================================== a keyboard, in software
 *
 * Everything above models the command channel.  This models the other one: the
 * attribute channel, where a low-energy device publishes what it is and then
 * reports what it is doing.
 *
 * It is a keyboard.  It publishes a report description, answers questions
 * about itself, stays silent until it is asked to report, and then sends a key.
 *
 * The silence is the point.  A device that answered every question AND sent
 * reports without being asked would let a driver that never subscribes pass -
 * and forgetting to subscribe produces a keyboard that describes itself
 * perfectly and never reports a key, which is the exact failure this exists to
 * catch.
 */
#define ATT_ERROR_RSP          0x01
#define ATT_READ_BY_TYPE_REQ   0x08
#define ATT_READ_BY_TYPE_RSP   0x09
#define ATT_READ_REQ           0x0A
#define ATT_READ_RSP           0x0B
#define ATT_READ_BLOB_REQ      0x0C
#define ATT_READ_BLOB_RSP      0x0D
#define ATT_WRITE_REQ          0x12
#define ATT_WRITE_RSP          0x13
#define ATT_HANDLE_VALUE_NTF   0x1B

#define M_UUID_REPORT_MAP      0x2A4B
#define M_UUID_REPORT          0x2A4D
#define M_UUID_BATTERY_LEVEL   0x2A19

/* Where this device keeps things.  A value sits one after its declaration, and
 * the "keep me told" descriptor one after the value - the layout every real
 * device uses and the one the driver assumes. */
#define H_MAP_DECL             0x0010
#define H_MAP_VALUE            0x0011
#define H_REPORT_DECL          0x0020
#define H_REPORT_VALUE         0x0021
#define H_REPORT_CCC           0x0022
#define H_BATTERY_DECL         0x0030
#define H_BATTERY_VALUE        0x0031

/* What this keyboard says is left in it.  A number a test can recognise
 * rather than a round one, so a driver returning something plausible by
 * accident does not pass. */
#define MODEL_BATTERY_PERCENT  73

/* A boot keyboard, described: eight modifier bits, a byte of padding, then six
 * key codes.  This is the shape almost every keyboard publishes, and the shape
 * the shared parser has to make sense of. */
static const u8 model_report_map[] = {
    0x05, 0x01,        /* usage page: generic desktop        */
    0x09, 0x06,        /* usage: keyboard                    */
    0xA1, 0x01,        /* collection: application            */
    0x05, 0x07,        /*   usage page: key codes            */
    0x19, 0xE0,        /*   usage minimum: left control      */
    0x29, 0xE7,        /*   usage maximum: right meta        */
    0x15, 0x00,        /*   logical minimum: 0               */
    0x25, 0x01,        /*   logical maximum: 1               */
    0x75, 0x01,        /*   report size: 1 bit               */
    0x95, 0x08,        /*   report count: 8                  */
    0x81, 0x02,        /*   input: data, variable, absolute  */
    0x95, 0x01,        /*   report count: 1                  */
    0x75, 0x08,        /*   report size: 8 bits              */
    0x81, 0x01,        /*   input: constant - the padding    */
    0x95, 0x06,        /*   report count: 6                  */
    0x75, 0x08,        /*   report size: 8 bits              */
    0x15, 0x00,        /*   logical minimum: 0               */
    0x25, 0x65,        /*   logical maximum: 101             */
    0x05, 0x07,        /*   usage page: key codes            */
    0x19, 0x00,        /*   usage minimum: none              */
    0x29, 0x65,        /*   usage maximum: 101               */
    0x81, 0x00,        /*   input: data, array               */
    0xC0               /* end collection                     */
};

static u8   att_reply[64];
static int  att_reply_len;
static bool subscribed;
static int  reads_before_subscribed;

static void reply(const void *bytes, int len) {
    if (len > (int)sizeof att_reply) len = (int)sizeof att_reply;
    memcpy(att_reply, bytes, (size_t)len);
    att_reply_len = len;
}

static void reply_error(u8 opcode, u16 handle, u8 why) {
    u8 e[5] = { ATT_ERROR_RSP, opcode, (u8)handle, (u8)(handle >> 8), why };
    reply(e, sizeof e);
}

/* One declaration, in the shape a read-by-type answer carries them. */
static int put_declaration(u8 *at, u16 decl, u16 value, u16 uuid) {
    at[0] = (u8)decl;      at[1] = (u8)(decl >> 8);
    at[2] = 0x12;          /* readable, and it notifies */
    at[3] = (u8)value;     at[4] = (u8)(value >> 8);
    at[5] = (u8)uuid;      at[6] = (u8)(uuid >> 8);
    return 7;
}

void btusb_model_attribute(u16 cid, const void *payload, u16 len) {
    const u8 *req = payload;
    att_reply_len = 0;
    if (!attached || cid != 0x0004 || !req || len < 1) return;

    switch (req[0]) {
    case ATT_READ_BY_TYPE_REQ: {
        if (len < 7) { reply_error(req[0], 0, 0x04); return; }
        u16 from = (u16)(req[1] | (req[2] << 8));

        /* Only what is at or after where the driver asked to carry on from.
         * A model that ignored that would let a driver which never advances
         * its search look correct, and such a driver finds the first batch of
         * a device and none of the rest. */
        /* This reply can publish all three declarations, not only two. */
        u8 out[2 + 7 * 3];
        out[0] = ATT_READ_BY_TYPE_RSP;
        out[1] = 7;
        int n = 2;

        if (from <= H_MAP_DECL)
            n += put_declaration(out + n, H_MAP_DECL, H_MAP_VALUE,
                                 M_UUID_REPORT_MAP);
        if (from <= H_REPORT_DECL)
            n += put_declaration(out + n, H_REPORT_DECL, H_REPORT_VALUE,
                                 M_UUID_REPORT);
        if (from <= H_BATTERY_DECL)
            n += put_declaration(out + n, H_BATTERY_DECL, H_BATTERY_VALUE,
                                 M_UUID_BATTERY_LEVEL);

        if (n == 2) { reply_error(req[0], from, 0x0A); return; }
        reply(out, n);
        return;
    }

    case ATT_READ_REQ: {
        if (len < 3) { reply_error(req[0], 0, 0x04); return; }
        u16 handle = (u16)(req[1] | (req[2] << 8));
        if (handle != H_BATTERY_VALUE) { reply_error(req[0], handle, 0x0A); return; }
        u8 out[2] = { ATT_READ_RSP, MODEL_BATTERY_PERCENT };
        reply(out, 2);
        return;
    }

    case ATT_READ_BLOB_REQ: {
        if (len < 5) { reply_error(req[0], 0, 0x04); return; }
        u16 handle = (u16)(req[1] | (req[2] << 8));
        u16 offset = (u16)(req[3] | (req[4] << 8));

        if (handle != H_MAP_VALUE) { reply_error(req[0], handle, 0x0A); return; }
        if (offset >= sizeof model_report_map) {
            reply_error(req[0], handle, 0x07);        /* past the end */
            return;
        }

        /* Twenty-two bytes at a time, so a description this long cannot arrive
         * in one piece and the driver has to ask again.  A model that answered
         * with all of it would let a driver that reads only the first piece
         * pass, and a real keyboard would then arrive half-described. */
        int left = (int)sizeof model_report_map - offset;
        int piece = left > 22 ? 22 : left;

        u8 out[32];
        out[0] = ATT_READ_BLOB_RSP;
        memcpy(out + 1, model_report_map + offset, (size_t)piece);
        reply(out, 1 + piece);
        return;
    }

    case ATT_WRITE_REQ: {
        if (len < 5) { reply_error(req[0], 0, 0x04); return; }
        u16 handle = (u16)(req[1] | (req[2] << 8));
        if (handle != H_REPORT_CCC) { reply_error(req[0], handle, 0x0A); return; }
        if (req[3] == 0x01) subscribed = true;
        u8 ok[1] = { ATT_WRITE_RSP };
        reply(ok, 1);
        return;
    }

    default:
        reply_error(req[0], 0, 0x06);                 /* not supported */
        return;
    }
}

int btusb_model_attribute_reply(u16 *cid_out, void *out, u32 cap) {
    if (cid_out) *cid_out = 0x0004;

    if (att_reply_len > 0) {
        int n = att_reply_len;
        if ((u32)n > cap) n = (int)cap;
        if (out) memcpy(out, att_reply, (size_t)n);
        att_reply_len = 0;
        return n;
    }

    /* Nothing was asked, so this is the driver waiting to be told something.
     *
     * Which happens only after being asked to tell.  A keyboard that reported
     * keys to a driver that never subscribed would hide the one mistake that
     * matters most here. */
    if (!subscribed) { reads_before_subscribed++; return -1; }

    /* The letter A held down with no modifiers, in the shape the description
     * above says a report takes: modifiers, padding, then six key codes. */
    static const u8 report[8] = { 0x00, 0x00, 0x04, 0, 0, 0, 0, 0 };
    u8 pkt[3 + sizeof report];
    pkt[0] = ATT_HANDLE_VALUE_NTF;
    pkt[1] = (u8)H_REPORT_VALUE;
    pkt[2] = (u8)(H_REPORT_VALUE >> 8);
    memcpy(pkt + 3, report, sizeof report);

    int n = (int)sizeof pkt;
    if ((u32)n > cap) n = (int)cap;
    if (out) memcpy(out, pkt, (size_t)n);
    return n;
}

void btusb_model_forget_subscription(void) {
    subscribed = false;
    reads_before_subscribed = 0;
    att_reply_len = 0;
}

int  btusb_model_battery(void) { return MODEL_BATTERY_PERCENT; }

bool btusb_model_was_subscribed(void) { return subscribed; }
int  btusb_model_reads_before_subscribed(void) { return reads_before_subscribed; }
