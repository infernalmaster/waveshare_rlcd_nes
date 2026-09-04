/* nes_rom_embedded.cpp - serve the ROM out of flash instead of off a card.
 *
 * Active when NES_EMBEDDED_ROM is 1 in hw_config.h. It replaces four things
 * osd.cpp would otherwise provide (they are compiled out there under the same
 * flag) plus the ROM browser from nes_menu.cpp:
 *
 *   osd_rom_open / osd_rom_read / osd_rom_close   the streaming reader
 *   osd_getromdata / osd_release_romdata          the whole-image API
 *   show_menu / get_selected_game                 nothing to choose from
 *
 * The streaming trio is what actually matters. nes_rom.c reads a ROM strictly
 * in order - 16-byte header, optional trainer, PRG, CHR, close - and never
 * seeks, so a cursor over a flash array is a complete implementation, not an
 * approximation of one. It also copies PRG and CHR into its own PSRAM buffers,
 * which is why a const array in flash is safe: nothing ever writes back.
 */
#include <Arduino.h>
#include <string.h>

#include "hw_config.h"

#if defined(NES_EMBEDDED_ROM) && NES_EMBEDDED_ROM

#include "src/nofrendo/tft_driver.h"

/* Defines the array. Included here and nowhere else. */
#include "nes_rom_data.h"

static unsigned rom_cursor = 0;

extern "C" int osd_rom_open(const char *path)
{
    (void)path;                 /* only one ROM exists in this build */
    rom_cursor = 0;
    Serial.printf("[ROM] embedded: %s, %u bytes\n", NES_ROM_NAME, nes_rom_size);
    return 0;
}

extern "C" int osd_rom_read(void *dst, int len)
{
    if (!dst || len <= 0) return -1;

    unsigned remaining = nes_rom_size - rom_cursor;
    if ((unsigned)len > remaining) len = (int)remaining;

    /* Flash is memory-mapped on the ESP32-S3, so this is an ordinary copy
     * through the instruction cache - no esp_partition_read needed. */
    memcpy(dst, nes_rom_data + rom_cursor, (size_t)len);
    rom_cursor += (unsigned)len;
    return len;
}

extern "C" void osd_rom_close(void)
{
    rom_cursor = 0;
}

/* Unused by this port - nes_rom.c goes through the streaming API above - but
 * osd.h declares them, so they have to exist and they may as well be right. */
extern "C" char *osd_getromdata(void)
{
    return (char *)nes_rom_data;
}

extern "C" void osd_release_romdata(void)
{
    /* Nothing to release. Freeing a flash address would be a fault, which is
     * the whole reason this is not just left as osd.cpp's free(). */
}

/* No card, no list, no choice. Kept as functions rather than deleted so the
 * sketch's flow is identical either way. */
extern "C" int show_menu(void)
{
    return 0;
}

extern "C" const char *get_selected_game(void)
{
    return NES_ROM_NAME ".nes";
}

#endif /* NES_EMBEDDED_ROM */
