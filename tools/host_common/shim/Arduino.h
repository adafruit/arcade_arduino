// Host-harness Arduino.h shim -- see ../README.md.
//
// Several ArcadeMachine_* sources include <Arduino.h> purely for DEBUG
// `Serial.print`/`Serial.println` instrumentation. This shim satisfies that include when the machine
// sources are compiled for the host, mapping Serial straight to stdout.
// It deliberately implements ONLY what those files actually use (verified
// by grep before writing this: Serial.print/println and the HEX base
// constant -- no millis(), delay(), String, or GPIO anywhere in the
// machine layer, which is exactly what makes this harness possible).
#ifndef ARDUINO_H_HOST_SHIM
#define ARDUINO_H_HOST_SHIM

#include <stdio.h>
#include <stdint.h>

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

#endif
