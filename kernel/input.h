#ifndef KESTREL_INPUT_H
#define KESTREL_INPUT_H

#include "kernel.h"

/* Keys that have no ASCII representation are reported as these codes, chosen
 * above the ASCII range so a single int can carry either. */
enum {
    KEY_NONE = 0,
    KEY_ESC = 27,
    KEY_UP = 0x100, KEY_DOWN, KEY_LEFT, KEY_RIGHT,
    KEY_HOME, KEY_END, KEY_PAGEUP, KEY_PAGEDOWN,
    KEY_INSERT, KEY_DELETE,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
};

#define MOD_SHIFT 0x01
#define MOD_CTRL  0x02
#define MOD_ALT   0x04
#define MOD_CAPS  0x08

typedef struct {
    u16  code;        /* ASCII, or one of the KEY_* values */
    u8   mods;
    bool pressed;     /* false for a key release */
} key_event_t;

void input_init(void);

/* Non-blocking: returns false when the queue is empty. */
bool input_poll(key_event_t *out);
/* Blocking read of the next printable/─control character, honouring modifiers. */
int  input_getchar(void);
/* How many events are waiting. */
int  input_pending(void);

/* Translate a key event into the byte sequence a terminal would send, so the
 * console line discipline and user programs see ordinary VT input. */
int input_encode(const key_event_t *ev, char *buf, size_t cap);

#endif
