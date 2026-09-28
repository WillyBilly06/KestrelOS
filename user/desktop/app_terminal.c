/* app_terminal.c - a terminal window that runs real programs.
 *
 * Commands are handed to /bin/shell with its standard streams redirected
 * through pipes, so everything the text-mode shell can do works here: the
 * window is a view onto the same shell, not a reimplementation of it.
 */
#include "desktop.h"

#define TERM_COLS_MAX 220
#define TERM_ROWS_MAX 120
#define HISTORY_MAX   32

typedef struct {
    char     cell[TERM_ROWS_MAX][TERM_COLS_MAX];
    uint8_t  colour[TERM_ROWS_MAX][TERM_COLS_MAX];
    int      cols, rows;
    int      cx, cy;
    uint8_t  pen;

    char     input[512];
    size_t   input_len;
    size_t   caret;

    char     history[HISTORY_MAX][512];
    int      history_count;
    int      history_pos;

    int      to_shell;        /* write end of the shell's stdin  */
    int      from_shell;      /* read end of the shell's stdout  */
    int      shell_pid;
    bool     busy;            /* a command is running            */
    uint64_t started;

    char     pending[4096];   /* output not yet folded into cells */
    size_t   pending_len;

    /* Escape-sequence state, so colours from programs survive. */
    int      esc;             /* 0 none, 1 saw ESC, 2 inside CSI */
    int      esc_arg[8];
    int      esc_argc;
} terminal_t;

/* The palette the terminal renders SGR colours with. */
static const colour_t term_palette[16] = {
    RGB(0x1C, 0x20, 0x28), RGB(0xE0, 0x5A, 0x5A), RGB(0x5A, 0xC8, 0x7A), RGB(0xE8, 0xB3, 0x39),
    RGB(0x4A, 0x9E, 0xE0), RGB(0xB0, 0x7A, 0xD8), RGB(0x4A, 0xC8, 0xC8), RGB(0xC8, 0xCE, 0xD8),
    RGB(0x50, 0x58, 0x66), RGB(0xF0, 0x7A, 0x7A), RGB(0x7A, 0xE0, 0x98), RGB(0xF6, 0xD0, 0x60),
    RGB(0x6E, 0xB8, 0xF0), RGB(0xC8, 0x96, 0xE8), RGB(0x6E, 0xE0, 0xE0), RGB(0xF2, 0xF6, 0xFC),
};

/* Lengths here scale with the interface.
 *
 * Written as plain numbers they are correct on exactly one display and wrong
 * on every other: the text drawn against them grows with the interface scale
 * and the boxes holding it do not, so rows sit on top of one another and
 * columns run into the ones beside them.  This file used gui_scale() nowhere
 * at all before that was noticed on a 1440p screen.
 */
#define TERM_PAD (8 * gui_scale())

static int term_char_w(void) { return gui_font_advance(FONT_MONO, 'M'); }
static int term_char_h(void) { return gui_font_height(FONT_MONO) + 1; }

/* ---------------------------------------------------------------- the grid */

static void term_clear(terminal_t *t) {
    for (int y = 0; y < TERM_ROWS_MAX; y++)
        for (int x = 0; x < TERM_COLS_MAX; x++) { t->cell[y][x] = ' '; t->colour[y][x] = 7; }
    t->cx = t->cy = 0;
}

static void term_scroll(terminal_t *t) {
    for (int y = 0; y < t->rows - 1; y++) {
        memcpy(t->cell[y], t->cell[y + 1], (size_t)t->cols);
        memcpy(t->colour[y], t->colour[y + 1], (size_t)t->cols);
    }
    for (int x = 0; x < t->cols; x++) {
        t->cell[t->rows - 1][x] = ' ';
        t->colour[t->rows - 1][x] = t->pen;
    }
    t->cy = t->rows - 1;
}

static void term_newline(terminal_t *t) {
    t->cx = 0;
    if (t->cy + 1 >= t->rows) term_scroll(t);
    else t->cy++;
}

static void term_putc(terminal_t *t, char c) {
    /* Just enough of the escape grammar to keep colours and clears working. */
    if (t->esc == 1) {
        if (c == '[') { t->esc = 2; t->esc_argc = 0; memset(t->esc_arg, 0, sizeof t->esc_arg); }
        else t->esc = 0;
        return;
    }
    if (t->esc == 2) {
        if (c >= '0' && c <= '9') {
            if (t->esc_argc == 0) t->esc_argc = 1;
            if (t->esc_argc <= 8) t->esc_arg[t->esc_argc - 1] = t->esc_arg[t->esc_argc - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') { if (t->esc_argc < 8) t->esc_argc++; return; }
        if (c == 'm') {
            if (t->esc_argc == 0) t->pen = 7;
            for (int i = 0; i < t->esc_argc; i++) {
                int v = t->esc_arg[i];
                if (v == 0) t->pen = 7;
                else if (v == 1) t->pen |= 8;
                else if (v >= 30 && v <= 37) t->pen = (uint8_t)((t->pen & 8) | (v - 30));
                else if (v >= 90 && v <= 97) t->pen = (uint8_t)(8 | (v - 90));
                else if (v == 39) t->pen = 7;
            }
        } else if (c == 'J') {
            term_clear(t);
        } else if (c == 'H' || c == 'f') {
            t->cx = t->cy = 0;
        }
        t->esc = 0;
        return;
    }

    switch (c) {
    case 0x1B: t->esc = 1; return;
    case '\n': term_newline(t); return;
    case '\r': t->cx = 0; return;
    case '\b': if (t->cx > 0) t->cx--; return;
    case '\t':
        do { if (t->cx < t->cols) { t->cell[t->cy][t->cx] = ' '; t->cx++; } } while (t->cx % 8);
        return;
    case 0x07: return;
    default: break;
    }
    if ((unsigned char)c < 32) return;

    if (t->cx >= t->cols) term_newline(t);
    t->cell[t->cy][t->cx] = c;
    t->colour[t->cy][t->cx] = t->pen;
    t->cx++;
}

static void term_write(terminal_t *t, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) term_putc(t, s[i]);
}

static void term_print(terminal_t *t, const char *s) { term_write(t, s, strlen(s)); }

/* --------------------------------------------------------------- the shell */

/* Start a shell with its streams wired to this window.  One shell is kept for
 * the life of the terminal so the working directory persists between
 * commands, exactly as it would on the text console. */
static bool term_start_shell(terminal_t *t) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0) return false;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return false; }

    /* The child inherits this process's first three descriptors, so swap them
     * around the spawn and put them straight back. */
    int saved_in = dup(STDIN_FD);
    int saved_out = dup(STDOUT_FD);
    int saved_err = dup(STDERR_FD);

    dup2(in_pipe[0], STDIN_FD);
    dup2(out_pipe[1], STDOUT_FD);
    dup2(out_pipe[1], STDERR_FD);

    const char *argv[1] = { "shell" };
    int pid = spawn("/bin/shell", argv, 1);

    dup2(saved_in, STDIN_FD);
    dup2(saved_out, STDOUT_FD);
    dup2(saved_err, STDERR_FD);
    close(saved_in);
    close(saved_out);
    close(saved_err);

    /* The ends the child owns are no longer ours. */
    close(in_pipe[0]);
    close(out_pipe[1]);

    if (pid < 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        return false;
    }

    t->to_shell = in_pipe[1];
    t->from_shell = out_pipe[0];
    t->shell_pid = pid;
    return true;
}

/* Pull whatever the shell has produced into the grid.  Never blocks: the
 * window has to stay responsive while a command is still running. */
static bool term_drain(terminal_t *t) {
    if (t->from_shell < 0) return false;

    bool got = false;
    for (int guard = 0; guard < 64; guard++) {
        uint32_t pending = 0;
        if (ioctl(t->from_shell, 1, &pending) < 0) break;
        if (!pending) break;

        char buf[1024];
        size_t want = pending < sizeof buf ? pending : sizeof buf;
        ssize_t n = read(t->from_shell, buf, want);
        if (n <= 0) break;

        term_write(t, buf, (size_t)n);
        got = true;

        /* The shell prints its prompt when it is ready for the next command,
         * so a '$' arriving is what marks the end of the previous one. */
        for (ssize_t i = 0; i < n; i++) if (buf[i] == '$') t->busy = false;
    }
    return got;
}

/* ------------------------------------------------------------------ paint */

static void term_layout(terminal_t *t, int w, int h) {
    t->cols = (w - 2 * TERM_PAD) / term_char_w();
    t->rows = (h - 2 * TERM_PAD - term_char_h() - 6) / term_char_h();
    if (t->cols < 20) t->cols = 20;
    if (t->rows < 4) t->rows = 4;
    if (t->cols > TERM_COLS_MAX) t->cols = TERM_COLS_MAX;
    if (t->rows > TERM_ROWS_MAX) t->rows = TERM_ROWS_MAX;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void term_paint(terminal_t *t, surface_t *s) {
    gui_clear(s, term_palette[0]);

    int cw = term_char_w(), ch = term_char_h();

    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < t->cols; x++) {
            char c = t->cell[y][x];
            if (c == ' ' || !c) continue;
            char one[2] = { c, 0 };
            gui_text(s, FONT_MONO, TERM_PAD + x * cw, TERM_PAD + y * ch,
                     one, term_palette[t->colour[y][x] & 15]);
        }
    }

    /* The input line sits below the output, always visible. */
    int iy = s->height - ch - 8;
    gui_hline(s, 0, iy - 5, s->width, colour_shade(term_palette[0], 18));

    if (t->busy) {
        char note[80];
        snprintf(note, sizeof note, "running... (%llu s)",
                 (unsigned long long)((uptime_ms() - t->started) / 1000));
        gui_text(s, FONT_MONO, TERM_PAD, iy, note, term_palette[11]);
        return;
    }

    gui_text(s, FONT_MONO, TERM_PAD, iy, "$", term_palette[10]);
    gui_text(s, FONT_MONO, TERM_PAD + cw * 2, iy, t->input, term_palette[15]);

    if ((uptime_ms() / 500) % 2 == 0) {
        int cx = TERM_PAD + cw * 2 + (int)t->caret * cw;
        gui_fill(s, rect_make(cx, iy, 1, ch - 2), term_palette[15]);
    }
}

/* ------------------------------------------------------------------ input */

static void term_submit(terminal_t *t) {
    char line[520];
    snprintf(line, sizeof line, "%s\n", t->input);

    /* Echo it the way a terminal does, then hand it to the shell. */
    term_print(t, "\x1b[32m$\x1b[0m ");
    term_print(t, t->input);
    term_print(t, "\n");

    if (t->input_len && t->history_count < HISTORY_MAX) {
        strlcpy(t->history[t->history_count++], t->input, sizeof t->history[0]);
    } else if (t->input_len) {
        memmove(t->history[0], t->history[1], sizeof t->history[0] * (HISTORY_MAX - 1));
        strlcpy(t->history[HISTORY_MAX - 1], t->input, sizeof t->history[0]);
    }
    t->history_pos = t->history_count;

    if (t->to_shell >= 0) {
        if (write(t->to_shell, line, strlen(line)) < 0)
            term_print(t, "\x1b[31mthe shell is no longer running\x1b[0m\n");
        else { t->busy = true; t->started = uptime_ms(); }
    }

    t->input[0] = 0;
    t->input_len = 0;
    t->caret = 0;
}

static bool term_key(terminal_t *t, uint32_t key, uint32_t mods) {
    if (t->busy) {
        /* Ctrl-C is the only thing that means anything mid-command. */
        if (key == 3 && t->shell_pid > 0) {
            term_print(t, "^C\n");
            return true;
        }
        return false;
    }

    switch (key) {
    case '\n':
        term_submit(t);
        return true;

    case '\b':
        if (!t->caret) return false;
        memmove(t->input + t->caret - 1, t->input + t->caret, t->input_len - t->caret + 1);
        t->caret--;
        t->input_len--;
        return true;

    case KK_DELETE:
        if (t->caret >= t->input_len) return false;
        memmove(t->input + t->caret, t->input + t->caret + 1, t->input_len - t->caret);
        t->input_len--;
        return true;

    case KK_LEFT:  if (t->caret) t->caret--; return true;
    case KK_RIGHT: if (t->caret < t->input_len) t->caret++; return true;
    case KK_HOME:  t->caret = 0; return true;
    case KK_END:   t->caret = t->input_len; return true;

    case KK_UP:
        if (t->history_pos > 0) {
            t->history_pos--;
            strlcpy(t->input, t->history[t->history_pos], sizeof t->input);
            t->input_len = strlen(t->input);
            t->caret = t->input_len;
        }
        return true;

    case KK_DOWN:
        if (t->history_pos < t->history_count - 1) {
            t->history_pos++;
            strlcpy(t->input, t->history[t->history_pos], sizeof t->input);
        } else {
            t->history_pos = t->history_count;
            t->input[0] = 0;
        }
        t->input_len = strlen(t->input);
        t->caret = t->input_len;
        return true;

    default:
        if (key == 21) { t->input[0] = 0; t->input_len = 0; t->caret = 0; return true; }  /* Ctrl-U */
        if (key < 32 || key > 126) return false;
        if (t->input_len + 1 >= sizeof t->input) return false;
        memmove(t->input + t->caret + 1, t->input + t->caret, t->input_len - t->caret + 1);
        t->input[t->caret++] = (char)key;
        t->input_len++;
        (void)mods;
        return true;
    }
}

/* ------------------------------------------------------------------ window */

static bool terminal_proc(window_t *w, const wevent_t *ev) {
    terminal_t *t = w->data;

    switch (ev->kind) {
    case WE_PAINT:
        term_paint(t, w->canvas);
        return false;

    case WE_RESIZE:
        term_layout(t, ev->x, ev->y);
        return true;

    case WE_KEY_DOWN:
        return term_key(t, ev->key, ev->mods);

    case WE_TICK: {
        bool changed = term_drain(t);
        /* A command that runs for a while should not look frozen, and the
         * caret blinks either way, so repaint on every tick. */
        return changed || true;
    }

    case WE_CLOSE:
        if (t->shell_pid > 0) kill(t->shell_pid);
        if (t->to_shell >= 0) close(t->to_shell);
        if (t->from_shell >= 0) close(t->from_shell);
        free(t);
        return false;

    default:
        return false;
    }
}

/* Open a terminal, optionally with a command already typed into it.
 *
 * Typing it in rather than spawning the program directly is deliberate: the
 * shell is what knows how to find a program, how to hand a .exe to the Windows
 * loader, and what to do when it fails.  A second path that did any of that
 * differently would be a second thing to keep right. */
static void terminal_open(wm_t *wm, const char *command) {
    terminal_t *t = calloc(1, sizeof *t);
    if (!t) return;

    t->to_shell = t->from_shell = -1;
    t->shell_pid = -1;
    t->pen = 7;
    t->cols = 80;
    t->rows = 24;
    term_clear(t);

    window_t *w = desktop_new_window(wm, "Terminal", ICON_TERMINAL, 720, 440, terminal_proc, t);
    if (!w) { free(t); return; }
    w->min_w = 380;
    w->min_h = 200;

    rect_t client = wm_client_rect(w);
    term_layout(t, client.w, client.h);

    if (!term_start_shell(t)) {
        term_print(t, "\x1b[31mCould not start /bin/shell.\x1b[0m\n");
        term_print(t, "The terminal needs the shell to run commands.\n");
    } else {
        term_print(t, "\x1b[36mKestrelOS terminal\x1b[0m\n");
        term_print(t, "Running /bin/shell. Type \x1b[1mhelp\x1b[0m for the command list.\n\n");
    }

    /* The command goes in as if it had been typed, once the shell is there to
     * read it. */
    if (command && command[0] && t->to_shell >= 0) {
        write(t->to_shell, command, strlen(command));
        write(t->to_shell, "\n", 1);
    }
}

void app_terminal_launch(wm_t *wm) { terminal_open(wm, NULL); }

/* Run one command in a terminal of its own, which is what opening a program
 * from the file browser does. */
void app_terminal_run(wm_t *wm, const char *command) { terminal_open(wm, command); }
