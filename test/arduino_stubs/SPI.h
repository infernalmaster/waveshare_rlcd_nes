/* SPI.h - stub, host compilation only. See Arduino.h in this folder. */
#ifndef SPI_STUB_H
#define SPI_STUB_H

#include <stdint.h>
#include <stddef.h>

#define MSBFIRST   1
#define LSBFIRST   0
#define SPI_MODE0  0x00
#define SPI_MODE1  0x01
#define SPI_MODE2  0x02
#define SPI_MODE3  0x03

#define FSPI 0
#define HSPI 1
#define VSPI 2

class SPISettings {
public:
    SPISettings(uint32_t clock, uint8_t bitOrder, uint8_t dataMode);
};

class SPIClass {
public:
    explicit SPIClass(uint8_t spi_bus);
    void begin(int8_t sck, int8_t miso, int8_t mosi, int8_t ss);
    void beginTransaction(SPISettings settings);
    void endTransaction(void);
    void transferBytes(const uint8_t *data, uint8_t *out, uint32_t size);

    /* The single-byte helpers the older ST7789-style drivers use. */
    void     setFrequency(uint32_t freq);
    void     setDataMode(uint8_t mode);
    void     setBitOrder(uint8_t order);
    void     write(uint8_t data);
    void     write16(uint16_t data);
    void     writeBytes(const uint8_t *data, uint32_t size);
    uint8_t  transfer(uint8_t data);
};

/* The core's default bus object. A driver that calls SPI.begin(...) rather than
 * owning an SPIClass is talking to this one. */
extern SPIClass SPI;

#endif /* SPI_STUB_H */
