#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * GemuSerial - generic serial-terminal interface.
 *
 * Any machine that does character I/O stores a pointer to one of these.
 * Any terminal device (VT100, etc.) fills one in via its _as_serial() helper.
 * The machine never includes terminal-specific headers.
 */
typedef struct {
    void    *ud;
    void   (*write_byte)(void *ud, uint8_t ch);   /* host → terminal */
    uint8_t (*read_byte)(void *ud);               /* terminal → host (0 = none) */
    bool    (*key_available)(void *ud);
    void   (*poll)(void *ud);                     /* pump events, blink cursor */
    bool   (*should_quit)(void *ud);              /* user closed terminal window */
    /* Optional: chars typed in the *host* window that the terminal intercepted
     * but couldn't push back via SDL_PushEvent (SDL3-compat crashes on TEXTINPUT
     * push-back).  The machine calls this after poll() to drain the buffer. */
    uint32_t (*pop_forwarded)(void *ud);          /* 0 = empty */
} GemuSerial;

static inline bool gemu_serial_attached(const GemuSerial *s) {
    return s != NULL && s->write_byte != NULL;
}
