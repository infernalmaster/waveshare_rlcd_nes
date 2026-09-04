/* SD_MMC.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * The ESP32-S3-RLCD-4.2's TF slot is on the SDMMC peripheral, not on SPI, so a
 * sketch that reads the card uses this rather than SD.h. begin() here always
 * fails, which means the "no card" branch is the one that gets exercised - the
 * stubs compile code, they do not run it.
 */
#ifndef SD_MMC_STUB_H
#define SD_MMC_STUB_H

#include <stdint.h>
#include "FS.h"

class SDMMCFS : public fs::FS {
public:
    bool setPins(int clk, int cmd, int d0);
    bool begin(const char *mountpoint = "/sdcard", bool mode1bit = false,
               bool format_if_mount_failed = false);
    void end(void);
    uint64_t cardSize(void);
    uint64_t totalBytes(void);
    uint64_t usedBytes(void);
};

extern SDMMCFS SD_MMC;

#endif /* SD_MMC_STUB_H */
