/* SD.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * The SPI-bus card API. The ESP32-S3-RLCD-4.2's own TF slot is NOT on SPI - it
 * is on the SDMMC peripheral, so sketches for this board want SD_MMC.h. This
 * exists so code that still has an SD.h branch can be compile-checked.
 */
#ifndef SD_STUB_H
#define SD_STUB_H

#include <stdint.h>
#include "FS.h"
#include "SPI.h"

class SDFS : public fs::FS {
public:
    bool begin(uint8_t ssPin = 4, SPIClass &spi = *(SPIClass *)0,
               uint32_t frequency = 4000000, const char *mountpoint = "/sd",
               uint8_t max_files = 5, bool format_if_empty = false);
    void end(void);
    uint64_t cardSize(void);
    uint64_t totalBytes(void);
    uint64_t usedBytes(void);
};

extern SDFS SD;

#endif /* SD_STUB_H */
