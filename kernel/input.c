/* input.c - i8042 PS/2 keyboard.
 *
 * The controller is left in translated scancode-set-1 mode, which is what
 * firmware normally hands over and what every emulator provides.  Key events go
 * into a small ring the console line discipline drains; nothing here blocks, so
 * the interrupt handler stays short.
 *
 * On a machine whose only keyboard is USB and whose firmware does not emulate a
 * PS/2 port, this driver finds nothing - that case needs the xHCI driver.
 */
#include "kernel.h"
#include "input.h"
#include "cpu.h"
#include "apic.h"
#include "acpi.h"
#include "klog.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define ST_OUTPUT_FULL 0x01
#define ST_INPUT_FULL  0x02
#define ST_FROM_MOUSE  0x20

static bool present;
static u8   mods;
static bool extended;

#define QUEUE_SIZE 128
static key_event_t queue[QUEUE_SIZE];
static volatile u32 q_head, q_tail;

/* Scancode set 1, unshifted and shifted, indexed by make code. */
static const u8 map_plain[0x60] = {
    0,    27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b', '\t',
    'q',  'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a',  's',
    'd',  'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0,  '\\', 'z', 'x', 'c', 'v',
    'b',  'n', 'm', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2',  '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static const u8 map_shift[0x60] = {
    0,    27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b', '\t',
    'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A',  'S',
    'D',  'F', 'G', 'H', 'J', 'K', 'L', ':', '"',  '~', 0,  '|',  'Z', 'X', 'C', 'V',
    'B',  'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,    0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2',  '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};

static void enqueue(key_event_t ev) {
    u32 next = (q_head + 1) % QUEUE_SIZE;
    if (next == q_tail) {
        /* Drop the oldest rather than the newest, so a burst of autorepeat
         * never hides the key the user just pressed. */
        q_tail = (q_tail + 1) % QUEUE_SIZE;
    }
    queue[q_head] = ev;
    q_head = next;
}

static u16 translate(u8 code, bool ext) {
    if (ext) {
        switch (code) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PAGEUP;
        case 0x51: return KEY_PAGEDOWN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        case 0x1C: return '\n';        /* keypad enter */
        case 0x35: return '/';         /* keypad slash */
        default:   return KEY_NONE;
        }
    }
    if (code >= 0x3B && code <= 0x44) return (u16)(KEY_F1 + (code - 0x3B));
    if (code == 0x57) return KEY_F11;
    if (code == 0x58) return KEY_F12;
    if (code >= 0x60) return KEY_NONE;

    bool shift = (mods & MOD_SHIFT) != 0;
    /* Caps lock only affects letters. */
    u8 base = map_plain[code];
    if ((mods & MOD_CAPS) && base >= 'a' && base <= 'z') shift = !shift;

    u8 c = shift ? map_shift[code] : map_plain[code];
    if (!c) return KEY_NONE;

    if (mods & MOD_CTRL) {
        if (c >= 'a' && c <= 'z') return (u16)(c - 'a' + 1);
        if (c >= 'A' && c <= 'Z') return (u16)(c - 'A' + 1);
        if (c == '[') return 27;
        if (c == '\\') return 28;
        if (c == ']') return 29;
    }
    return c;
}

void tty_wake_readers(void);
void input_push_key(u16 code, u8 mods, bool pressed);

static regs_t *keyboard_isr(regs_t *r, void *ctx) {
    (void)ctx;
    bool woke = false;

    /* Drain the buffer: several bytes can be pending for one interrupt.
     * Bit 5 of the status register marks a byte that came from the second
     * port - reading those here would steal the mouse driver's packets. */
    int guard = 0;
    while ((inb(PS2_STATUS) & (ST_OUTPUT_FULL | ST_FROM_MOUSE)) == ST_OUTPUT_FULL && ++guard < 32) {
        u8 code = inb(PS2_DATA);

        if (code == 0xE0) { extended = true; continue; }
        if (code == 0xE1) { extended = false; continue; }   /* pause; ignored */

        bool released = (code & 0x80) != 0;
        u8 make = code & 0x7F;

        /* Modifier tracking. */
        if (!extended && (make == 0x2A || make == 0x36)) {
            if (released) mods &= (u8)~MOD_SHIFT; else mods |= MOD_SHIFT;
            continue;
        }
        if (make == 0x1D) { if (released) mods &= (u8)~MOD_CTRL; else mods |= MOD_CTRL; extended = false; continue; }
        if (make == 0x38) { if (released) mods &= (u8)~MOD_ALT;  else mods |= MOD_ALT;  extended = false; continue; }
        if (!extended && make == 0x3A) {
            if (!released) mods ^= MOD_CAPS;
            continue;
        }

        u16 sym = translate(make, extended);
        extended = false;
        if (sym == KEY_NONE) continue;

        key_event_t ev = { sym, mods, !released };
        enqueue(ev);
        /* Graphical programs read the unified stream instead of the tty, and
         * need releases as well as presses. */
        input_push_key(sym, mods, !released);
        woke = true;
    }

    /* Anything blocked in a console read can run again now. */
    if (woke) tty_wake_readers();

    lapic_eoi();
    return r;
}

/* A key from somewhere other than the PS/2 port - a USB keyboard, say.
 * Everything downstream of the scancode translation is shared, so the two
 * drivers converge here and the console cannot tell them apart. */
void input_inject_key(u16 code, u8 key_mods, bool pressed) {
    if (code == KEY_NONE) return;

    key_event_t ev = { code, key_mods, pressed };
    bool irq = irq_save();
    enqueue(ev);
    irq_restore(irq);

    input_push_key(code, key_mods, pressed);
    tty_wake_readers();
}

bool input_poll(key_event_t *out) {
    bool irq = irq_save();
    if (q_tail == q_head) { irq_restore(irq); return false; }
    *out = queue[q_tail];
    q_tail = (q_tail + 1) % QUEUE_SIZE;
    irq_restore(irq);
    return true;
}

int input_pending(void) {
    bool irq = irq_save();
    int n = (int)((q_head + QUEUE_SIZE - q_tail) % QUEUE_SIZE);
    irq_restore(irq);
    return n;
}

int input_getchar(void) {
    for (;;) {
        key_event_t ev;
        if (input_poll(&ev)) {
            if (!ev.pressed) continue;
            if (ev.code < 0x100) return ev.code;
            continue;
        }
        __asm__ volatile("hlt");
    }
}

int input_encode(const key_event_t *ev, char *buf, size_t cap) {
    if (!ev->pressed || cap < 8) return 0;

    if (ev->code < 0x100) { buf[0] = (char)ev->code; return 1; }

    const char *seq = NULL;
    switch (ev->code) {
    case KEY_UP:       seq = "\x1b[A"; break;
    case KEY_DOWN:     seq = "\x1b[B"; break;
    case KEY_RIGHT:    seq = "\x1b[C"; break;
    case KEY_LEFT:     seq = "\x1b[D"; break;
    case KEY_HOME:     seq = "\x1b[H"; break;
    case KEY_END:      seq = "\x1b[F"; break;
    case KEY_PAGEUP:   seq = "\x1b[5~"; break;
    case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
    case KEY_INSERT:   seq = "\x1b[2~"; break;
    case KEY_DELETE:   seq = "\x1b[3~"; break;
    case KEY_F1:       seq = "\x1bOP"; break;
    case KEY_F2:       seq = "\x1bOQ"; break;
    case KEY_F3:       seq = "\x1bOR"; break;
    case KEY_F4:       seq = "\x1bOS"; break;
    case KEY_F5:       seq = "\x1b[15~"; break;
    case KEY_F6:       seq = "\x1b[17~"; break;
    case KEY_F7:       seq = "\x1b[18~"; break;
    case KEY_F8:       seq = "\x1b[19~"; break;
    case KEY_F9:       seq = "\x1b[20~"; break;
    case KEY_F10:      seq = "\x1b[21~"; break;
    case KEY_F11:      seq = "\x1b[23~"; break;
    case KEY_F12:      seq = "\x1b[24~"; break;
    default: return 0;
    }
    size_t n = strlen(seq);
    if (n >= cap) return 0;
    memcpy(buf, seq, n);
    return (int)n;
}

/* ------------------------------------------------------------------------- */
/* controller bring-up                                                       */
/* ------------------------------------------------------------------------- */

static bool wait_input_clear(void) {
    for (int i = 0; i < 100000; i++) if (!(inb(PS2_STATUS) & ST_INPUT_FULL)) return true;
    return false;
}

static bool wait_output_full(void) {
    for (int i = 0; i < 100000; i++) if (inb(PS2_STATUS) & ST_OUTPUT_FULL) return true;
    return false;
}

static bool ps2_command(u8 cmd) {
    if (!wait_input_clear()) return false;
    outb(PS2_CMD, cmd);
    return true;
}

static bool ps2_write_data(u8 val) {
    if (!wait_input_clear()) return false;
    outb(PS2_DATA, val);
    return true;
}

static int ps2_read_data(void) {
    if (!wait_output_full()) return -1;
    return inb(PS2_DATA);
}

void input_init(void) {
    if (!acpi_has_8042()) {
        kinfo("ps2", "firmware reports no 8042; legacy keyboard probe skipped");
        return;
    }
    /* Disable both ports while we reconfigure. */
    ps2_command(0xAD);
    ps2_command(0xA7);

    /* Flush anything the firmware left behind. */
    for (int i = 0; i < 32 && (inb(PS2_STATUS) & ST_OUTPUT_FULL); i++) (void)inb(PS2_DATA);

    /* Read the configuration byte. */
    if (!ps2_command(0x20)) { kwarn("ps2", "controller did not respond; no PS/2 keyboard"); return; }
    int cfg = ps2_read_data();
    if (cfg < 0) { kwarn("ps2", "cannot read the controller configuration"); return; }

    /* Port 1 interrupt on, port 2 interrupt off, translation on. */
    u8 newcfg = (u8)((cfg | 0x01 | 0x40) & ~0x02u & ~0x10u);
    ps2_command(0x60);
    ps2_write_data(newcfg);

    /* Controller self test.  It can reset the configuration, so write it back. */
    ps2_command(0xAA);
    int res = ps2_read_data();
    if (res != 0x55) {
        kwarn("ps2", "controller self-test returned %#x; continuing anyway", res);
    } else {
        ps2_command(0x60);
        ps2_write_data(newcfg);
    }

    /* Port 1 test. */
    ps2_command(0xAB);
    res = ps2_read_data();
    if (res != 0x00) {
        kerr("ps2", "keyboard port test failed (%#x); no keyboard available", res);
        return;
    }

    ps2_command(0xAE);      /* enable port 1 */

    /* Ask the keyboard to start scanning. */
    ps2_write_data(0xF4);
    res = ps2_read_data();
    if (res != 0xFA) kwarn("ps2", "keyboard did not acknowledge enable (%#x)", res);

    /* Route IRQ 1. */
    bool active_low = false, level = false;
    u32 gsi = acpi_irq_to_gsi(1, &active_low, &level);
    irq_install(VEC_IRQ_BASE + 1, keyboard_isr, NULL);
    if (!ioapic_route(gsi, VEC_IRQ_BASE + 1, active_low, level, lapic_id())) {
        kerr("ps2", "cannot route keyboard IRQ 1 (GSI %u)", gsi);
        return;
    }
    ioapic_mask(gsi, false);

    present = true;
    kinfo("ps2", "keyboard ready on GSI %u -> vector %#x", gsi, VEC_IRQ_BASE + 1);
}
