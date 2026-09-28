/* input.h - the event record /dev/input delivers, shared with userland.
 *
 * One record type covers the keyboard and the mouse so a graphical program can
 * run a single event loop instead of polling two devices.
 */
#ifndef KESTREL_INPUT_EVENTS_H
#define KESTREL_INPUT_EVENTS_H

#include <stdint.h>

enum {
    KEV_KEY = 1,        /* a key went down or came up          */
    KEV_MOUSE_MOVE = 2, /* the pointer moved                   */
    KEV_MOUSE_BUTTON= 3,/* a mouse button changed state        */
    KEV_MOUSE_WHEEL = 4,/* the wheel turned                    */
};

/* Mouse buttons, as a bit mask. */
#define MB_LEFT   0x01
#define MB_RIGHT  0x02
#define MB_MIDDLE 0x04

/* Modifier bits, matching the kernel's keyboard state. */
#define KMOD_SHIFT 0x01
#define KMOD_CTRL  0x02
#define KMOD_ALT   0x04
#define KMOD_CAPS  0x08

/* Keys with no character of their own, above the ASCII range. */
enum {
    KK_UP = 0x100, KK_DOWN, KK_LEFT, KK_RIGHT,
    KK_HOME, KK_END, KK_PAGEUP, KK_PAGEDOWN,
    KK_INSERT, KK_DELETE,
    KK_F1, KK_F2, KK_F3, KK_F4, KK_F5, KK_F6,
    KK_F7, KK_F8, KK_F9, KK_F10, KK_F11, KK_F12,
};

typedef struct {
    uint32_t type;        /* KEV_*                                        */
    uint32_t code;        /* key code, or the button mask that changed    */
    int32_t  x, y;        /* pointer position, in pixels                  */
    int32_t  dx, dy;      /* movement since the last event                */
    int32_t  wheel;       /* notches, positive is away from the user      */
    uint32_t buttons;     /* every button currently held                  */
    uint32_t mods;        /* KMOD_*                                       */
    uint32_t pressed;     /* 1 for a press, 0 for a release               */
    uint64_t time_ms;     /* uptime when it happened                      */
} kinput_event_t;

/* Filled in by the framebuffer syscall. */
typedef struct {
    uint64_t address;     /* where it was mapped in this process          */
    uint32_t width, height;
    uint32_t pitch;       /* bytes per row                                */
    uint32_t bpp;
    uint32_t red_shift, green_shift, blue_shift;
    uint32_t size;        /* bytes mapped                                 */
} kframebuffer_t;

#endif
