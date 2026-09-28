/* tty.c - the console device.
 *
 * Provides /dev/console, which is what every process gets as its standard
 * input, output and error.  Reads come from the keyboard queue; in cooked mode
 * the driver does the line editing and only returns a completed line, which is
 * what makes the shell's prompt behave without the shell having to know about
 * scan codes.
 */
#include "kernel.h"
#include "vfs.h"
#include "input.h"
#include "proc.h"
#include "klog.h"
#include "mm.h"
#include "../include/kestrel/syscall.h"

#define LINE_MAX 512

static bool raw_mode;
static char line[LINE_MAX];
static int  line_len;      /* characters entered so far          */
static int  line_ready;    /* bytes of a completed line waiting  */
static int  line_taken;    /* how many of those were consumed    */

/* Processes blocked waiting for input, so the keyboard interrupt can wake them
 * rather than having them poll. */
static proc_t *waiters[PROC_MAX];

static void tty_wait(void) {
    proc_t *me = proc_current();
    if (!me) { __asm__ volatile("hlt"); return; }

    bool irq = irq_save();
    int slot = -1;
    for (int i = 0; i < PROC_MAX; i++) if (!waiters[i]) { waiters[i] = me; slot = i; break; }
    irq_restore(irq);

    /* If every slot is taken, fall back to a short sleep so we still make
     * progress instead of spinning. */
    if (slot < 0) { sched_sleep_ms(5); return; }

    sched_block(PROC_BLOCKED);

    irq = irq_save();
    if (waiters[slot] == me) waiters[slot] = NULL;
    irq_restore(irq);
}

void tty_wake_readers(void) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *p = waiters[i];
        if (p) sched_wake(p);
    }
}

/* ------------------------------------------------------------------------- */
/* line editing                                                              */
/* ------------------------------------------------------------------------- */

static void echo(const char *s, size_t n) { console_write(s, n); }

static bool at_eof;

/* Drain the keyboard queue into the line buffer.  Only the reading thread
 * touches this buffer - the interrupt handler stops at the key queue - so no
 * locking is needed here, which keeps the echo out of a critical section. */
static void pump(void) {
    key_event_t ev;
    while (input_poll(&ev)) {
        if (!ev.pressed) continue;

        char seq[8];
        int n = input_encode(&ev, seq, sizeof seq);
        if (n <= 0) continue;

        if (raw_mode) {
            for (int i = 0; i < n && line_ready < LINE_MAX; i++) line[line_ready++] = seq[i];
            continue;
        }

        /* Cooked mode: only single characters take part in editing; escape
         * sequences are ignored so arrow keys do not corrupt the line. */
        if (n != 1) continue;
        char c = seq[0];

        if (c == '\n' || c == '\r') {
            if (line_len < LINE_MAX - 1) line[line_len++] = '\n';
            echo("\n", 1);
            line_ready = line_len;
            line_taken = 0;
            line_len = 0;
            return;
        }
        if (c == '\b' || c == 0x7F) {
            if (line_len > 0) { line_len--; echo("\b \b", 3); }
            continue;
        }
        if (c == 3) {                       /* Ctrl-C: abandon the line */
            line_len = 0;
            echo("^C\n", 3);
            line_ready = 0;
            line_taken = 0;
            continue;
        }
        if (c == 4) {                       /* Ctrl-D: end of input */
            if (line_len == 0) { at_eof = true; return; }
            continue;
        }
        if (c == 21) {                      /* Ctrl-U: clear the line */
            while (line_len > 0) { line_len--; echo("\b \b", 3); }
            continue;
        }
        if ((unsigned char)c < 32 && c != '\t') continue;

        if (line_len < LINE_MAX - 2) {
            line[line_len++] = c;
            echo(&c, 1);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* device operations                                                         */
/* ------------------------------------------------------------------------- */

static ssize_t_k tty_read(void *ctx, void *buf, size_t len, u64 off) {
    (void)ctx; (void)off;
    if (!len) return 0;

    /* Block until a whole line (cooked) or any byte (raw) is available.
     * Ctrl-D with an empty line reports end of input by returning zero. */
    for (;;) {
        if (proc_stop_requested()) return -E_INTR;
        pump();
        if (line_ready > line_taken) break;
        if (at_eof) { at_eof = false; return 0; }
        tty_wait();
    }

    size_t avail = (size_t)(line_ready - line_taken);
    if (avail > len) avail = len;
    memcpy(buf, line + line_taken, avail);
    line_taken += (int)avail;
    if (line_taken >= line_ready) { line_ready = 0; line_taken = 0; }

    return (ssize_t_k)avail;
}

static ssize_t_k tty_write(void *ctx, const void *buf, size_t len, u64 off) {
    (void)ctx; (void)off;
    console_write(buf, len);
    return (ssize_t_k)len;
}

static int tty_ioctl(void *ctx, u32 cmd, void *arg) {
    (void)ctx;
    switch (cmd) {
    case CON_GET_SIZE: {
        int cols, rows;
        console_get_size(&cols, &rows);
        ((int *)arg)[0] = cols;
        ((int *)arg)[1] = rows;
        return 0;
    }
    case CON_SET_CURSOR:
        console_set_cursor(((int *)arg)[0], ((int *)arg)[1]);
        return 0;
    case CON_GET_CURSOR: {
        int col, row;
        console_get_cursor(&col, &row);
        ((int *)arg)[0] = col;
        ((int *)arg)[1] = row;
        return 0;
    }
    case CON_SET_RAW: {
        bool want = (*(int *)arg) != 0;
        bool irq = irq_save();
        if (want != raw_mode) { line_len = 0; line_ready = 0; line_taken = 0; }
        raw_mode = want;
        irq_restore(irq);
        return 0;
    }
    case CON_CLEAR:
        console_clear();
        return 0;
    case CON_SHOW_CURSOR:
        console_show_cursor((*(int *)arg) != 0);
        return 0;
    default:
        return -E_INVAL;
    }
}

static int tty_ioctl_shape(void *ctx, u32 cmd, vfs_ioctl_shape_t *shape) {
    (void)ctx;
    *shape = (vfs_ioctl_shape_t){0, 0, true};
    switch (cmd) {
    case CON_GET_SIZE: case CON_GET_CURSOR: shape->out_bytes = 8; break;
    case CON_SET_CURSOR: shape->in_bytes = 8; break;
    case CON_SET_RAW: case CON_SHOW_CURSOR: shape->in_bytes = 4; break;
    case CON_CLEAR: shape->required = false; break;
    default: return -E_INVAL;
    }
    return 0;
}

static const devfs_ops_t tty_ops = {
    .read = tty_read, .write = tty_write, .ioctl = tty_ioctl,
    .ioctl_shape = tty_ioctl_shape,
};

void tty_init(void) {
    devfs_register("console", VN_CHR, &tty_ops, NULL);
    devfs_register("tty", VN_CHR, &tty_ops, NULL);
    kinfo("tty", "console device ready");
}
