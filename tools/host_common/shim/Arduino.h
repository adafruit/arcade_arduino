// SPDX-FileCopyrightText: 2026 John Park for Adafruit Industries
//
// SPDX-License-Identifier: MIT

// Host-harness Arduino.h shim -- see ../README.md.
//
// Several ArcadeMachine_* sources include <Arduino.h> for DEBUG
// `Serial.print`/`Serial.println` instrumentation and for micros() in their
// own frame-timing instruments. This shim satisfies that include when the
// machine sources are compiled for the host, mapping Serial straight to
// stdout. It deliberately implements ONLY what those files actually use --
// no delay(), String, or GPIO anywhere in the machine layer, which is
// exactly what makes this harness possible.
//
// micros() was added later than the rest: galaga_machine.cpp's starvation
// instrument calls it, and (unlike lrescue_machine.cpp) that file does not
// include <Arduino.h> itself, so galaga_host stopped building the moment
// that instrument landed. Host time is real monotonic time, which is
// meaningless as a frame-budget figure here -- there is no DVI pump to be
// paced against -- but the instruments that use it are debug output, and a
// harness that builds beats a harness that reports a prettier zero.
#ifndef ARDUINO_H_HOST_SHIM
#define ARDUINO_H_HOST_SHIM

#include <stdio.h>
#include <stdint.h>
#include <time.h>

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

struct HostSerial {
    void begin(unsigned long = 0) {}
    void flush() { fflush(stdout); }
    operator bool() const { return true; }

    // Distinct char vs uint8_t overloads: a char literal like ' ' must
    // print as a character, while a uint8_t pixel/register value must
    // print as a number. Letting both fall through to one overload
    // silently prints register values as ASCII garbage.
    void print(char c)               { fputc(c, stdout); }
    void print(const char *s)        { fputs(s ? s : "(null)", stdout); }
    void print(unsigned char v, int base = DEC) { print((unsigned long)v, base); }
    void print(int v, int base = DEC)           { print((long)v, base); }
    void print(unsigned int v, int base = DEC)  { print((unsigned long)v, base); }
    void print(long v, int base = DEC) {
        if (base == HEX) printf("%lX", (unsigned long)v); else printf("%ld", v);
    }
    void print(unsigned long v, int base = DEC) {
        if (base == HEX) printf("%lX", v); else printf("%lu", v);
    }
    void print(double v)             { printf("%g", v); }

    void println()                             { fputc('\n', stdout); }
    template <class T> void println(T v)             { print(v); fputc('\n', stdout); }
    template <class T> void println(T v, int base)   { print(v, base); fputc('\n', stdout); }
};

extern HostSerial Serial;

// Wraps at 2^32 us like the device's, so any code written to survive that
// wrap is exercised the same way here (see DEVNOTES.md problems #26/#27 for
// how much a host/device width mismatch can cost).
static inline uint32_t micros(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL);
}

#endif
