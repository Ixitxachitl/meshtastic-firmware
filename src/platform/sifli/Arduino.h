/**
 * Arduino.h - Zephyr compatibility shim for nRF54L15
 *
 * Provides the Arduino API surface expected by Meshtastic, backed by
 * Zephyr primitives.  Only the subset actually used by Meshtastic is
 * implemented; the rest compiles as no-ops / stubs for now.
 *
 * Phase 2: compile only.  Real GPIO / SPI / Wire implementations follow
 * in Phase 3 once the build is clean.
 */

#pragma once
#ifndef Arduino_h
#define Arduino_h

// ── C standard headers ───────────────────────────────────────────────────────
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>

// picolibc declares struct timezone only under __BSD_VISIBLE, which is off
// here. SensorLib's SensorRTC.h names the type in a default argument.
#if !defined(__BSD_VISIBLE) || !__BSD_VISIBLE
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
#endif
#include <string.h>
#include <strings.h> /* strcasecmp, strncasecmp */

// ── Zephyr kernel ────────────────────────────────────────────────────────────
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

// ── Basic Arduino types ──────────────────────────────────────────────────────
typedef bool boolean;
typedef uint8_t byte;
typedef uint16_t word;

// ── Pin / digital constants ──────────────────────────────────────────────────
#define INPUT 0u
#define OUTPUT 1u
#define INPUT_PULLUP 2u
#define INPUT_PULLDOWN 3u
#define OUTPUT_OPENDRAIN 4u

#define HIGH 1u
#define LOW 0u

#define CHANGE 1
#define FALLING 2
#define RISING 3

#ifndef LED_BUILTIN
#define LED_BUILTIN -1
#endif

// ── Math / trig constants ────────────────────────────────────────────────────
#ifndef PI
#define PI 3.14159265358979323846
#endif
#define HALF_PI 1.57079632679489661923
#define TWO_PI 6.28318530717958647693
#define DEG_TO_RAD 0.01745329251994329576
#define RAD_TO_DEG 57.2957795130823208767
#define EULER 2.71828182845904523536

// picolibc hides the M_* family behind __GNU_VISIBLE, which this build turns
// off by compiling with _POSIX_C_SOURCE. Arduino cores elsewhere get these
// from math.h, and portable code reasonably expects it.
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Bit utilities ────────────────────────────────────────────────────────────
#define bitRead(v, b) (((v) >> (b)) & 1)
#define bitSet(v, b) ((v) |= (1UL << (b)))
#define bitClear(v, b) ((v) &= ~(1UL << (b)))
#define bitToggle(v, b) ((v) ^= (1UL << (b)))
#define bitWrite(v, b, x) ((x) ? bitSet(v, b) : bitClear(v, b))
#define bit(b) (1UL << (b))
#define lowByte(w) ((uint8_t)((w)&0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
// word(h,l) - only define if not already defined (conflicts with typedef above)
#undef word
#define word(h, l) ((uint16_t)(((h) << 8) | (l)))

// ── UART config constants ─────────────────────────────────────────────────────
#define SERIAL_8N1 0x800001cu
#define SERIAL_8N2 0x8000001eu
#define SERIAL_8E1 0x8000001eu
#define SERIAL_7E1 0x8000001cu

// ── Integer order ────────────────────────────────────────────────────────────
// Adafruit BusIO's SPIDevice.h has `typedef BitOrder BusIOBitOrder;` which
// requires BitOrder to be a *type*, not a macro. Mirror the ArduinoCore-API
// enum definition rather than #defines.
enum BitOrder : uint8_t {
    LSBFIRST = 0,
    MSBFIRST = 1,
};

// ── pgmspace compatibility (no-ops on Cortex-M) ──────────────────────────────
#define PROGMEM
#define PSTR(s) (s)
#define F(s) (s)
#define pgm_read_byte(addr) (*((const uint8_t *)(addr)))
#define pgm_read_word(addr) (*((const uint16_t *)(addr)))
#define pgm_read_dword(addr) (*((const uint32_t *)(addr)))
#define pgm_read_float(addr) (*((const float *)(addr)))
#define pgm_read_ptr(addr) (*((const void **)(addr)))
#define strlen_P(s) strlen(s)
#define strcpy_P(d, s) strcpy(d, s)
#define strncpy_P(d, s, n) strncpy(d, s, n)
#define strcmp_P(a, b) strcmp(a, b)
#define memcpy_P(d, s, n) memcpy(d, s, n)
#define sprintf_P sprintf
typedef const char *PGM_P;
typedef const char *PGM_VOID_P;

// ── Arduino numeric base constants (used by Print, RadioLib, etc.) ───────────
#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

// ── ulong / uint typedef (used by RadioLibInterface, etc.) ───────────────────
typedef unsigned long ulong;
typedef unsigned int uint;

// ── Interrupt stubs ──────────────────────────────────────────────────────────
static inline void interrupts() {}
static inline void noInterrupts() {}
#define digitalPinToInterrupt(p) (p)

// ── portMAX_DELAY - freertosinc.h also defines this; let it win ──────────────
// We intentionally do NOT define portMAX_DELAY here.  freertosinc.h defines
// it for the FreeRTOS / Meshtastic threading layer and must not be overridden.

// ── Timing & system functions - declared with C linkage ──────────────────────
// buzz.cpp and others forward-declare delay() as extern "C"; keep linkage
// consistent by wrapping in extern "C" here.
#ifdef __cplusplus
extern "C" {
#endif
void NVIC_SystemReset(void);
uint32_t millis(void);
uint32_t micros(void);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield(void);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

#include <cctype>
#include <cstdarg>

// ── C++ STL - include BEFORE defining any min/max helpers ───────────────────
// Include algorithm first so its min/max templates are in scope.
// We must NOT define min/max as function-like macros: the C++ STL uses
// 3-argument versions (min(a,b,comp)) that the preprocessor would treat as
// calling a 2-arg macro with 3 args.
#include <algorithm>
// Bring 2-arg std::min / std::max into the global namespace as unqualified
// names so that Arduino code calling min(a,b) continues to compile.
// (Arduino convention; kept minimal to avoid surprises.)
#undef min
#undef max
using std::max;
using std::min;

// ── Arduino math helpers ─────────────────────────────────────────────────────
// Templates, not macros, for the same reason as min/max above: as macros these
// names follow every later #include, and <chrono> declares its own abs() and
// round() over durations. That collision only shows up once a translation unit
// pulls in both Arduino.h and the standard library.
// round() is left to the libc: <cmath> already provides overloads for every
// arithmetic type, and adding another would make every integer argument
// ambiguous. Arduino's version returns long, the libc's returns double, which
// callers convert on assignment either way.
#undef abs
#undef constrain
#undef round
#include <cmath>
using std::round;

template <class T> constexpr T abs(const T &x)
{
    return x >= 0 ? x : -x;
}
template <class T, class L, class H> constexpr T constrain(const T &x, const L &l, const H &h)
{
    return x < (T)l ? (T)l : (x > (T)h ? (T)h : x);
}
#define radians(d) ((d)*DEG_TO_RAD)
#define degrees(r) ((r)*RAD_TO_DEG)
#define sq(x) ((x) * (x))

// ── Random ───────────────────────────────────────────────────────────────────
static inline void randomSeed(unsigned long seed)
{
    srand((unsigned int)seed);
}
static inline long random(void)
{
    return (long)rand();
}
static inline long random(long bound)
{
    return bound > 0 ? (rand() % bound) : 0;
}
static inline long random(long lo, long hi)
{
    return hi > lo ? lo + rand() % (hi - lo) : lo;
}

// ── GPIO - real Zephyr implementation (Phase 3) ──────────────────────────────
// Implemented in sifli_arduino.cpp using Zephyr GPIO/SPI APIs.
// Pin numbering: P0.n = n, P1.n = 16+n, P2.n = 32+n
void pinMode(uint32_t pin, uint32_t mode);
void digitalWrite(uint32_t pin, uint32_t value);
int digitalRead(uint32_t pin);
static inline void digitalToggle(uint32_t pin)
{
    digitalWrite(pin, !digitalRead(pin));
}
static inline uint32_t analogRead(uint32_t)
{
    return 0;
}
static inline void analogWrite(uint32_t, uint32_t) {}
static inline void analogReadResolution(int) {}
static inline void analogWriteResolution(int) {}

// ── __WFI - provided by CMSIS core_cm33.h; do NOT redefine here ─────────────

// ── __FlashStringHelper - Arduino PROGMEM string class (no-op on Cortex-M) ──
class __FlashStringHelper;

// ── attachInterrupt / detachInterrupt - real Zephyr GPIO interrupt impl ──────
typedef void (*voidFuncPtr)(void);
void attachInterrupt(uint32_t pin, voidFuncPtr cb, int mode);
void detachInterrupt(uint32_t pin);

// ── Forward declaration of String (needed by Print / Stream) ─────────────────
class String;

// ── Print base class ─────────────────────────────────────────────────────────
class Print
{
  public:
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t *buf, size_t n)
    {
        size_t written = 0;
        while (n--)
            written += write(*buf++);
        return written;
    }
    size_t write(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    size_t write(const char *s, size_t n) { return write((const uint8_t *)s, n); }

    size_t print(const char *s) { return s ? write((const uint8_t *)s, strlen(s)) : 0; }
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));

    size_t print(char c) { return write((uint8_t)c); }
    size_t print(const String &s);
    size_t print(unsigned char n, int base = 10);
    size_t print(int n, int base = 10);
    size_t print(long n, int base = 10);
    size_t print(unsigned int n, int base = 10);
    size_t print(unsigned long n, int base = 10);
    size_t print(float n, int digits = 2);
    size_t print(double n, int digits = 2);
    size_t print(bool b) { return print(b ? "true" : "false"); }

    size_t println() { return write((uint8_t)'\n'); }
    size_t println(const char *s)
    {
        size_t r = print(s);
        return r + println();
    }
    size_t println(char c)
    {
        size_t r = print(c);
        return r + println();
    }
    size_t println(const String &s);
    size_t println(int n, int base = 10)
    {
        size_t r = print(n, base);
        return r + println();
    }
    size_t println(long n, int base = 10)
    {
        size_t r = print(n, base);
        return r + println();
    }
    size_t println(unsigned long n, int base = 10)
    {
        size_t r = print(n, base);
        return r + println();
    }
    size_t println(unsigned int n, int base = 10)
    {
        size_t r = print(n, base);
        return r + println();
    }
    size_t println(float n, int d = 2)
    {
        size_t r = print(n, d);
        return r + println();
    }
    size_t println(double n, int d = 2)
    {
        size_t r = print(n, d);
        return r + println();
    }
    size_t println(bool b)
    {
        size_t r = print(b);
        return r + println();
    }

    virtual void flush() {}
    virtual int availableForWrite() { return 0; }
};

// ── Stream base class ────────────────────────────────────────────────────────
class Stream : public Print
{
  public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void setTimeout(unsigned long) {}
    virtual bool find(const char *) { return false; }
    virtual size_t readBytes(uint8_t *buffer, size_t length)
    {
        size_t n = 0;
        while (n < length) {
            int c = read();
            if (c < 0)
                break;
            buffer[n++] = (uint8_t)c;
        }
        return n;
    }
    size_t readBytes(char *buffer, size_t length) { return readBytes((uint8_t *)buffer, length); }
    String readString();
    String readStringUntil(char terminator);
};

// ── Minimal Arduino String class (backed by a char buffer) ───────────────────
class String
{
  public:
    String() : _buf(nullptr), _len(0), _cap(0) {}
    // Implicit conversion is part of the Arduino String contract, used pervasively across the codebase.
    // cppcheck-suppress noExplicitConstructor
    String(const char *cstr) : _buf(nullptr), _len(0), _cap(0)
    {
        if (cstr)
            assign(cstr, strlen(cstr));
    }
    // cppcheck-suppress noExplicitConstructor
    String(const String &s) : _buf(nullptr), _len(0), _cap(0) { assign(s._buf ? s._buf : "", s._len); }
    // cppcheck-suppress noExplicitConstructor
    String(char c) : _buf(nullptr), _len(0), _cap(0)
    {
        const char tmp[2] = {c, 0};
        assign(tmp, 1);
    }
    // cppcheck-suppress noExplicitConstructor
    String(int n) : _buf(nullptr), _len(0), _cap(0)
    {
        char tmp[16];
        snprintf(tmp, 16, "%d", n);
        assign(tmp, strlen(tmp));
    }
    // cppcheck-suppress noExplicitConstructor
    String(unsigned int n) : _buf(nullptr), _len(0), _cap(0)
    {
        char tmp[16];
        snprintf(tmp, 16, "%u", n);
        assign(tmp, strlen(tmp));
    }
    // cppcheck-suppress noExplicitConstructor
    String(long n) : _buf(nullptr), _len(0), _cap(0)
    {
        char tmp[24];
        snprintf(tmp, 24, "%ld", n);
        assign(tmp, strlen(tmp));
    }
    // cppcheck-suppress noExplicitConstructor
    String(unsigned long n) : _buf(nullptr), _len(0), _cap(0)
    {
        char tmp[24];
        snprintf(tmp, 24, "%lu", n);
        assign(tmp, strlen(tmp));
    }
    // cppcheck-suppress noExplicitConstructor
    String(float n, int d = 2) : _buf(nullptr), _len(0), _cap(0)
    {
        char tmp[32];
        snprintf(tmp, 32, "%.*f", d, n);
        assign(tmp, strlen(tmp));
    }
    // cppcheck-suppress noExplicitConstructor
    String(double n, int d = 2) : _buf(nullptr), _len(0), _cap(0)
    {
        char tmp[32];
        snprintf(tmp, 32, "%.*f", d, (double)n);
        assign(tmp, strlen(tmp));
    }
    ~String() { free(_buf); }

    String &operator=(const String &s)
    {
        assign(s._buf ? s._buf : "", s._len);
        return *this;
    }
    String &operator=(const char *s)
    {
        assign(s ? s : "", s ? strlen(s) : 0);
        return *this;
    }
    String &operator=(char c)
    {
        const char tmp[2] = {c, 0};
        assign(tmp, 1);
        return *this;
    }

    String &operator+=(const String &s)
    {
        concat(s._buf ? s._buf : "", s._len);
        return *this;
    }
    String &operator+=(const char *s)
    {
        if (s)
            concat(s, strlen(s));
        return *this;
    }
    String &operator+=(char c)
    {
        concat(&c, 1);
        return *this;
    }
    String &operator+=(int n) { return *this += String(n); }
    String &operator+=(unsigned long n) { return *this += String(n); }

    String operator+(const String &rhs) const
    {
        String r(*this);
        r += rhs;
        return r;
    }
    String operator+(const char *rhs) const
    {
        String r(*this);
        r += rhs;
        return r;
    }
    String operator+(char rhs) const
    {
        String r(*this);
        r += rhs;
        return r;
    }

    bool operator==(const String &s) const { return _len == s._len && (_len == 0 || strcmp(_buf, s._buf) == 0); }
    bool operator==(const char *s) const { return s && strcmp(c_str(), s) == 0; }
    bool operator!=(const String &s) const { return !(*this == s); }
    bool operator!=(const char *s) const { return !(*this == s); }
    bool operator<(const String &s) const { return strcmp(c_str(), s.c_str()) < 0; }
    bool operator>(const String &s) const { return strcmp(c_str(), s.c_str()) > 0; }

    char operator[](unsigned int i) const { return (_buf && i < _len) ? _buf[i] : 0; }
    char &operator[](unsigned int i)
    {
        static char dummy = 0;
        return (_buf && i < _len) ? _buf[i] : dummy;
    }

    const char *c_str() const { return _buf ? _buf : ""; }
    unsigned int length() const { return _len; }
    bool isEmpty() const { return _len == 0; }
    bool equals(const String &s) const { return *this == s; }
    bool equals(const char *s) const { return *this == s; }
    bool equalsIgnoreCase(const String &s) const
    {
        if (_len != s._len)
            return false;
        for (unsigned i = 0; i < _len; i++)
            if (std::tolower(_buf[i]) != std::tolower(s._buf[i]))
                return false;
        return true;
    }
    bool startsWith(const String &pfx) const
    {
        if (pfx._len > _len)
            return false;
        return strncmp(c_str(), pfx.c_str(), pfx._len) == 0;
    }
    bool startsWith(const char *pfx) const
    {
        if (!pfx)
            return false;
        size_t pl = strlen(pfx);
        return pl <= _len && strncmp(c_str(), pfx, pl) == 0;
    }
    bool endsWith(const String &sfx) const
    {
        if (sfx._len > _len)
            return false;
        return strcmp(c_str() + _len - sfx._len, sfx.c_str()) == 0;
    }
    int indexOf(char c, unsigned from = 0) const
    {
        if (!_buf)
            return -1;
        const char *p = strchr(_buf + from, c);
        return p ? (int)(p - _buf) : -1;
    }
    int indexOf(const String &s, unsigned from = 0) const
    {
        if (!_buf)
            return -1;
        const char *p = strstr(_buf + from, s.c_str());
        return p ? (int)(p - _buf) : -1;
    }
    int lastIndexOf(char c) const
    {
        if (!_buf)
            return -1;
        const char *p = strrchr(_buf, c);
        return p ? (int)(p - _buf) : -1;
    }
    String substring(unsigned beginIndex) const
    {
        if (!_buf || beginIndex >= _len)
            return String();
        return String(_buf + beginIndex);
    }
    String substring(unsigned beginIndex, unsigned endIndex) const
    {
        if (!_buf || beginIndex >= _len)
            return String();
        if (endIndex > _len)
            endIndex = _len;
        if (endIndex <= beginIndex)
            return String();
        String r;
        r.assign(_buf + beginIndex, endIndex - beginIndex);
        return r;
    }
    void toUpperCase()
    {
        if (_buf)
            for (unsigned i = 0; i < _len; i++)
                _buf[i] = (char)std::toupper(_buf[i]);
    }
    void toLowerCase()
    {
        if (_buf)
            for (unsigned i = 0; i < _len; i++)
                _buf[i] = (char)std::tolower(_buf[i]);
    }
    void trim()
    {
        if (!_buf || _len == 0)
            return;
        unsigned s = 0;
        while (s < _len && std::isspace(_buf[s]))
            s++;
        unsigned e = _len;
        while (e > s && std::isspace(_buf[e - 1]))
            e--;
        if (s > 0 || e < _len) {
            memmove(_buf, _buf + s, e - s);
            _len = e - s;
            _buf[_len] = 0;
        }
    }
    void replace(char from, char to)
    {
        if (_buf)
            for (unsigned i = 0; i < _len; i++)
                if (_buf[i] == from)
                    _buf[i] = to;
    }
    void replace(const String &from, const String &to);
    bool remove(unsigned index, unsigned count = 1)
    {
        if (!_buf || index >= _len)
            return false;
        if (index + count > _len)
            count = _len - index;
        memmove(_buf + index, _buf + index + count, _len - index - count + 1);
        _len -= count;
        return true;
    }
    void clear()
    {
        _len = 0;
        if (_buf)
            _buf[0] = 0;
    }
    char charAt(unsigned i) const { return (*this)[i]; }
    void setCharAt(unsigned i, char c)
    {
        if (_buf && i < _len)
            _buf[i] = c;
    }
    void toCharArray(char *buf, unsigned int bufsize, unsigned int index = 0) const
    {
        if (!buf || bufsize == 0)
            return;
        unsigned int avail = (_buf && _len > index) ? (_len - index) : 0;
        unsigned int copy = avail < bufsize - 1 ? avail : bufsize - 1;
        if (copy > 0)
            memcpy(buf, _buf + index, copy);
        buf[copy] = '\0';
    }
    void concat(const String &s) { *this += s; }
    void concat(const char *s) { *this += s; }
    long toInt() const { return _buf ? atol(_buf) : 0; }
    float toFloat() const { return _buf ? (float)atof(_buf) : 0.0f; }
    double toDouble() const { return _buf ? atof(_buf) : 0.0; }

  private:
    char *_buf;
    unsigned int _len;
    unsigned int _cap;

    void assign(const char *s, unsigned int n)
    {
        // reserve() keeps the old (smaller) buffer on OOM, so a failed grow must abort the
        // write: memcpy'ing n >= _cap bytes would overflow into adjacent heap.
        if (n + 1 == 0)
            return; // n + 1 would wrap
        if (n >= _cap && !reserve(n + 1))
            return;
        if (_buf) {
            memcpy(_buf, s, n);
            _buf[n] = 0;
            _len = n;
        }
    }
    void concat(const char *s, unsigned int n)
    {
        if (!s || n == 0)
            return;
        unsigned newlen = _len + n;
        if (newlen < _len || newlen + 1 == 0)
            return; // length arithmetic wrapped
        if (newlen >= _cap && !reserve(newlen + 1))
            return; // OOM: keep the existing content intact instead of writing past the buffer
        if (_buf) {
            memcpy(_buf + _len, s, n);
            _len = newlen;
            _buf[_len] = 0;
        }
    }
    bool reserve(unsigned int n)
    {
        if (n == 0)
            return false;
        char *b = (char *)realloc(_buf, n);
        if (b) {
            _buf = b;
            _cap = n;
            return true;
        }
        return false;
    }
};

inline String operator+(const char *lhs, const String &rhs)
{
    return String(lhs) + rhs;
}
inline String operator+(char lhs, const String &rhs)
{
    return String(lhs) + rhs;
}

// ── Print inline definitions that need String ────────────────────────────────
inline size_t Print::print(const String &s)
{
    return write((const uint8_t *)s.c_str(), s.length());
}
inline size_t Print::println(const String &s)
{
    size_t r = print(s);
    return r + println();
}

// ── Stream inline definitions that need String ───────────────────────────────
inline String Stream::readString()
{
    return String();
}
inline String Stream::readStringUntil(char)
{
    return String();
}

// ── HardwareSerial ───────────────────────────────────────────────────────────
//
// Backed by a Zephyr UART device. Ports that were constructed without one -
// or before their device is ready - swallow writes and report no input, so
// callers do not need to know which ports a board actually wires up.
//
// Receive runs off the interrupt-driven UART API into the ring buffer below;
// the buffer is deliberately a plain array rather than Zephyr's ring_buf so
// this header stays free of Zephyr includes.
struct device;

class HardwareSerial : public Stream
{
  public:
    HardwareSerial() = default;
    // The device pointer is resolved from devicetree in sifli_arduino.cpp.
    explicit HardwareSerial(const struct device *dev) : _dev(dev) {}

    void begin(unsigned long baud);
    void begin(unsigned long baud, uint16_t) { begin(baud); }
    void begin(unsigned long baud, uint32_t, int8_t = -1, int8_t = -1, bool = false) { begin(baud); }
    void end();
    unsigned long baudRate() const { return _baud; }
    void updateBaudRate(unsigned long baud) { begin(baud); }

    // Pin selection lives in the board devicetree, not in calling code.
    void setPins(int rx, int tx) {}
    void setPinout(int tx, int rx) {}
    void setFIFOSize(size_t) {}
    void setRxBufferSize(size_t) {}

    int available() override;
    int read() override;
    int peek() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t *buf, size_t n) override;
    using Print::write; // un-hide base class write(const char*)
    size_t readBytes(uint8_t *buf, size_t len) override;
    size_t readBytes(char *buf, size_t len) { return readBytes((uint8_t *)buf, len); }
    operator bool() const { return _dev != nullptr; }
    void flush() override {}
    String readString();
    String readStringUntil(char terminator);

    // Called from the UART ISR; public only so the C callback can reach it.
    void rxPush(uint8_t c);

  private:
    static constexpr uint16_t RX_SIZE = 256;

    const struct device *_dev = nullptr;
    unsigned long _baud = 0;
    bool _started = false;
    volatile uint16_t _head = 0;
    volatile uint16_t _tail = 0;
    uint8_t _rx[RX_SIZE] = {};
};

// Uart - nRF52 BSP alias for HardwareSerial (used by GPS.h when ARCH_NRF52)
typedef HardwareSerial Uart;

extern HardwareSerial Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;

// ── map() utility ────────────────────────────────────────────────────────────
static inline long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// ── shiftIn / shiftOut stubs ─────────────────────────────────────────────────
static inline uint8_t shiftIn(uint8_t, uint8_t, uint8_t)
{
    return 0;
}
static inline void shiftOut(uint8_t, uint8_t, uint8_t, uint8_t) {}

// ── tone / noTone stubs ──────────────────────────────────────────────────────
static inline void tone(uint8_t, unsigned int, unsigned long = 0) {}
static inline void noTone(uint8_t) {}

// ── pulseIn stub ─────────────────────────────────────────────────────────────
static inline unsigned long pulseIn(uint8_t, uint8_t, unsigned long = 1000000UL)
{
    return 0;
}

// ── strcasestr - GNU extension picolibc does not provide ────────────────────
#ifndef STRCASESTR
#define STRCASESTR
static inline char *strcasestr(const char *haystack, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0)
        return (char *)haystack;
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, n) == 0)
            return (char *)haystack;
    }
    return nullptr;
}
#endif

// ── strnstr - BSD extension not in Zephyr libc; defined in meshUtils.cpp ─────
// Declare here so callers (GPS.cpp etc.) don't need ARCH_PORTDUINO.
#ifndef STRNSTR
#define STRNSTR
char *strnstr(const char *s, const char *find, size_t slen);
#endif

// ── strlcpy - BSD extension; implementation in sifli_arduino.cpp ──────────
#ifndef HAVE_STRLCPY
#define HAVE_STRLCPY
#ifdef __cplusplus
extern "C" {
#endif
size_t strlcpy(char *dst, const char *src, size_t size);
#ifdef __cplusplus
}
#endif
#endif

#include <stdlib.h>
#include <time.h>

// picolibc declares tzset()/getenv() but not setenv(); the implementation comes
// from Zephyr's POSIX layer (CONFIG_POSIX_SINGLE_PROCESS), which has no header
// picolibc pulls in, so declare it here.
#ifdef __cplusplus
extern "C" {
#endif
int setenv(const char *name, const char *value, int overwrite);
#ifdef __cplusplus
}
#endif

// ── dbgHeapFree / dbgHeapTotal - nRF52 BSP heap diagnostics ─────────────────
// Used by memGet.cpp when ARCH_NRF52 is defined.  Return 0 for Phase 2.
static inline uint32_t dbgHeapFree(void)
{
    return 0;
}
static inline uint32_t dbgHeapTotal(void)
{
    return 0;
}

// ── WCharacter helpers ───────────────────────────────────────────────────────
static inline bool isAlpha(char c)
{
    return std::isalpha((unsigned char)c) != 0;
}
static inline bool isAlphaNumeric(char c)
{
    return std::isalnum((unsigned char)c) != 0;
}
static inline bool isDigit(char c)
{
    return std::isdigit((unsigned char)c) != 0;
}
static inline bool isSpace(char c)
{
    return std::isspace((unsigned char)c) != 0;
}
static inline bool isUpperCase(char c)
{
    return std::isupper((unsigned char)c) != 0;
}
static inline bool isLowerCase(char c)
{
    return std::islower((unsigned char)c) != 0;
}
static inline char toUpperCase(char c)
{
    return (char)std::toupper((unsigned char)c);
}
static inline char toLowerCase(char c)
{
    return (char)std::tolower((unsigned char)c);
}

#else /* C only */
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef abs
#define abs(x) ((x) >= 0 ? (x) : -(x))
#endif
#define constrain(x, l, h) ((x) < (l) ? (l) : ((x) > (h) ? (h) : (x)))
#define round(x) ((x) >= 0 ? (long)((x) + 0.5) : (long)((x)-0.5))
#endif /* __cplusplus */

// LovyanGFX's generic Arduino platform expects Arduino.h to have defined the
// SPI bus class by the time it is included. Pulled in last so SPI.h sees the
// rest of this header; the include guards make the cycle a no-op.
#ifdef __cplusplus
#include "SPI.h"
#endif

#endif /* Arduino_h */
