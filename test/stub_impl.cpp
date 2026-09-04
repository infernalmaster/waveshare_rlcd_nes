/* stub_impl.cpp - bodies for the Arduino/SPI stubs, so a host test can LINK.
 *
 * test/arduino_stubs only declares; that is all -fsyntax-only needs. blit_test
 * actually runs code, so the handful of symbols st7305_tft.cpp references have
 * to exist. None of them does anything: GPIO writes are discarded and SPI
 * transfers are dropped on the floor, which is correct for a test that only
 * looks at the framebuffer and never at the wire.
 */
#include <stdarg.h>
#include <stdio.h>

#include "Arduino.h"
#include "SPI.h"

unsigned long millis(void) { return 0; }
unsigned long micros(void) { return 0; }
void delay(unsigned long) {}
void delayMicroseconds(unsigned int) {}

void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t, uint8_t) {}
int  digitalRead(uint8_t) { return 1; }          /* pull-up, nothing pressed */

int  digitalPinToInterrupt(uint8_t p) { return p; }
void attachInterrupt(int, void (*)(void), int) {}
void detachInterrupt(int) {}
void noInterrupts(void) {}
void interrupts(void) {}

SPISettings::SPISettings(uint32_t, uint8_t, uint8_t) {}
SPIClass::SPIClass(uint8_t) {}
void SPIClass::begin(int8_t, int8_t, int8_t, int8_t) {}
void SPIClass::beginTransaction(SPISettings) {}
void SPIClass::endTransaction(void) {}
void SPIClass::transferBytes(const uint8_t *, uint8_t *, uint32_t) {}

SerialStub Serial;

void   SerialStub::begin(unsigned long) {}
void   SerialStub::end(void) {}
SerialStub::operator bool() const { return true; }
size_t SerialStub::print(const char *s) { return fputs(s, stdout) < 0 ? 0 : 1; }
size_t SerialStub::println(const char *s) { printf("%s\n", s); return 1; }
size_t SerialStub::println(void) { printf("\n"); return 1; }
size_t SerialStub::print(int v) { printf("%d", v); return 1; }
size_t SerialStub::print(unsigned v) { printf("%u", v); return 1; }
size_t SerialStub::println(int v) { printf("%d\n", v); return 1; }
int    SerialStub::availableForWrite(void) { return 128; }
int    SerialStub::available(void) { return 0; }
int    SerialStub::read(void) { return -1; }
int    SerialStub::peek(void) { return -1; }
void   SerialStub::flush(void) { fflush(stdout); }

int SerialStub::printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    const int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int SerialStub::vprintf(const char *fmt, __builtin_va_list ap)
{
    return ::vprintf(fmt, ap);
}
