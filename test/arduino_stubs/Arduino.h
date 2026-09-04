/* Arduino.h - stub, host compilation only. NOT a runtime shim.
 *
 * There is no Arduino toolchain in this repo (AGENTS.md section 5), so sketches
 * used to be unverifiable until someone flashed a board. These stubs let a
 * sketch be compiled - syntax, types, signatures, format strings - with:
 *
 *   cc -x c++ -fsyntax-only -Itest/arduino_stubs -include Arduino.h <sketch>.ino
 *
 * That catches the class of error that wastes a flash cycle. It does NOT
 * execute anything and it does NOT check behaviour: SPI, timing, interrupts and
 * the panel itself stay entirely unexercised. A sketch that compiles here has
 * been spell-checked, not tested.
 */
#ifndef ARDUINO_STUB_H
#define ARDUINO_STUB_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HIGH 1
#define LOW  0
#define INPUT        0x01
#define OUTPUT       0x03
#define INPUT_PULLUP 0x05

#define RISING  0x01
#define FALLING 0x02
#define CHANGE  0x03

#define IRAM_ATTR
#define PROGMEM

/* The core pulls this in for every sketch, so tables marked PROGMEM and read
 * with pgm_read_byte compile without the sketch including anything. */
#include <pgmspace.h>

typedef uint8_t byte;

/* FreeRTOS, which the Arduino ESP32 core makes available to every sketch.
 * Nothing schedules on the host: a task created here never runs, so anything
 * living in a background task is compiled and never exercised. */
typedef void *TaskHandle_t;
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms)  ((TickType_t)(ms))
#define pdTRUE  1
#define pdFALSE 0
#define portMAX_DELAY ((TickType_t)0xFFFFFFFF)

void vTaskDelay(TickType_t ticks);
int  xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                 void *arg, unsigned prio, TaskHandle_t *handle);
int  xTaskCreatePinnedToCore(void (*fn)(void *), const char *name,
                             uint32_t stack, void *arg, unsigned prio,
                             TaskHandle_t *handle, int core);

/* Macros, not templates - matching the core, which means mixed-type arguments
 * like min(200, some_int) compile here exactly as they do on the target. */
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef constrain
#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))
#endif

/* Just enough String for sketches that compare a filename against a suffix.
 * Nothing here allocates or copies - it borrows the pointer it was built from,
 * which is fine for compilation and wrong for anything else. */
class String {
public:
    String(const char *s = "");
    const char *c_str(void) const;
    unsigned    length(void) const;
    bool        endsWith(const String &suffix) const;
    bool        startsWith(const String &prefix) const;
    bool        equals(const String &other) const;
    char        operator[](unsigned i) const;
private:
    const char *p_;
};

unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int  digitalRead(uint8_t pin);

int  digitalPinToInterrupt(uint8_t pin);
void attachInterrupt(int interrupt, void (*fn)(void), int mode);
void detachInterrupt(int interrupt);
void noInterrupts(void);
void interrupts(void);

/* This board is an ESP32-S3 with native USB, so Serial is the core's HWCDC and
 * not a UART. Defined here so sketches guarding CDC-only calls behind it get
 * that branch compiled rather than skipped - an #if that is never true checks
 * nothing, which is the failure mode these stubs exist to prevent. */
#define ARDUINO_USB_CDC_ON_BOOT 1

class SerialStub {
public:
    void begin(unsigned long baud);
    void end(void);
    operator bool() const;
    size_t print(const char *s);
    size_t println(const char *s);
    size_t println(void);
    /* printf is checked against the format string, which is most of the value
     * of compiling a sketch at all - a %lu fed an int is a real bug here. */
    int printf(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    /* Documented as "the number of bytes that can be written without
     * blocking", which is the only reliable way to ask whether a write is
     * about to stall waiting for a host. */
    int availableForWrite(void);

    /* Input side, for sketches that take commands or act as a gamepad.
     * available() answers 0 on the host, so those loops compile and never
     * execute a body - the usual caveat. */
    int    available(void);
    int    read(void);
    int    peek(void);
    void   flush(void);
    size_t print(int v);
    size_t print(unsigned v);
    size_t println(int v);
    int    vprintf(const char *fmt, __builtin_va_list args);
};

extern SerialStub Serial;

#endif /* ARDUINO_STUB_H */
