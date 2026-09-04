/* waveshare_rlcd_nes - a NES emulator on the ESP32-S3-RLCD-4.2 reflective panel.
 *
 * Nofrendo - vendored in src/nofrendo/, forked from Esp32NofrendobyDSN -
 * rendered onto a 400x300 1-bit reflective LCD through st7305_gfx. The emulator
 * runs at full speed; the panel scans at NES_PANEL_FPS, which is a fraction of
 * that, so most emulated frames are dropped on the way to the glass. That is
 * the panel, not the emulator - see the notes below.
 *
 * SETUP:
 *
 *  1. No emulator library to install. The Nofrendo core is vendored at
 *     src/nofrendo/ - a fork of Esp32NofrendobyDSN carrying the ST7305 changes,
 *     which the upstream release does not have. Arduino compiles a sketch's
 *     src/ subfolder recursively, so it builds with the sketch. If an older
 *     Esp32NofrendobyDSN is still installed in your libraries folder, remove it.
 *
 *  2. Arduino IDE board settings (Tools menu):
 *       Board            ESP32S3 Dev Module
 *       PSRAM            OPI PSRAM          <- required, the ROM lives there
 *       Flash Size       16MB (128Mb)
 *       Partition Scheme 16M Flash (3MB APP/9.9MB FATFS) or any >=3 MB app
 *       USB CDC On Boot  Enabled            <- for the serial gamepad
 *
 *  3. A TF card, as shipped: NES_EMBEDDED_ROM is 0, so the board browses the
 *     card and plays what you pick. The card must be FAT32 with the .nes files
 *     in its root - exFAT will not mount, because ESP-IDF builds FatFs without
 *     it. Set the flag to 1 in hw_config.h to play the ROM already embedded in
 *     nes_rom_data.h instead, which needs no card at all.
 *
 * The sketch folder also holds hw_config.h, st7305_gfx.h and st7305_tft.h. The
 * emulator core in src/nofrendo/ includes those from here - the Arduino builder
 * puts the sketch folder on the include path for every translation unit it
 * compiles, which is the same mechanism the upstream example relied on for
 * hw_config.h back when the core was a separate library.
 *
 * WHAT TO EXPECT ON THE GLASS
 *  - The picture fills all 400x300, at the aspect ratio the game was drawn for.
 *    NES pixels were never square: the PPU renders 256x240, a television showed
 *    the middle 224 rows, and those 256x224 filled a 4:3 screen. This panel is
 *    4:3, so the overscan rows are dropped and the rest is stretched to fit.
 *  - Colour becomes ink-or-paper through a 4x4 ordered dither, so sprites read
 *    as texture rather than as flat black. Ordered, not error-diffused, so
 *    static parts of the screen hold still between frames.
 *  - One pushed frame per panel scan, over 60 fps of emulation - the serial log
 *    prints what that works out to. Games play at correct speed; fast motion
 *    smears, because the liquid crystal itself takes tens of ms.
 *  - No tearing. The flush is phase-locked to the panel's TE pin rather than to
 *    a timer, which is what the second framebuffer in st7305_tft.h is for. If
 *    the serial log ever shows te_hz=0 that lock has been lost and the seam is
 *    back; everything else in the log will look normal.
 *  - SELECT + B inverts ink and paper. Dark games often read far better.
 */
#include <Arduino.h>
#include <esp_heap_caps.h>

#include "hw_config.h"
#include "src/nofrendo/tft_driver.h"     /* -> st7305_tft.h, because NES_USE_ST7305 is 1 */
#include "ble_keyboard.h"

#if !NES_EMBEDDED_ROM
#include <FS.h>
#include <SD_MMC.h>
#endif

/* osd.cpp inside the library refers to this by name. */
TFTDriver tft;

extern void setup_controller();
extern "C" int nofrendo_main(int argc, char *argv[]);
extern "C" int show_menu();
extern "C" const char *get_selected_game();
extern "C" int nes_get_gamepad_state();

/* Teardown between games. nofrendo registers its own shutdown through atexit(),
 * which never runs here because main_loop() ends with a plain return - so the
 * sketch has to call these itself. vid_shutdown() and osd_shutdown() are
 * osd.cpp's; main_eject() is nofrendo's and is what destroys the NES instance
 * and frees the ROM. */
extern "C" void vid_shutdown();
extern "C" void osd_shutdown();
extern "C" void main_eject(void);

#if !NES_EMBEDDED_ROM
/* NOT static: nes_menu.cpp calls this too. Swapping a card needs the mount torn
 * down and rebuilt, and there is no reason for the browser to carry a second
 * copy of the pin setup to do it. */
bool mount_card()
{
    /* SDMMC, one data line. The slot on this board is not on the SPI bus at
     * all, so SD.begin() with a chip select can never find it. */
    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_D0);
    if (!SD_MMC.begin("/sdcard", true /* 1-bit */, false /* no format */)) {
        Serial.println("     x TF card mount failed");
        return false;
    }
    Serial.printf("     ok TF card, %llu MB\n", SD_MMC.cardSize() >> 20);
    return true;
}
#endif

#if NES_BLE_KEYBOARD
/* Shows what the BLE task is doing, until it connects or you get bored.
 *
 * Worth the wait screen rather than just booting: pairing is the one step that
 * genuinely can fail (keyboard not in pairing mode, a stale bond on one side),
 * and the failure is silent unless someone is watching the serial log. Any
 * button skips - the game does not need a keyboard to start. */
static void wait_for_keyboard(uint32_t timeout_ms)
{
    if (timeout_ms == 0) return;

    /* A button still held from the picker would otherwise read as "skip" the
     * instant this starts, and the wait would look like it never happened. */
    while (nes_get_gamepad_state()) delay(20);
    delay(80);

    /* Compared by content, not by pointer: ble_keyboard_status() hands back the
     * same buffer every time, so a pointer test would see one update and then
     * never redraw again. */
    char shown[64] = "";
    int  shown_left = -1;
    const uint32_t started = millis();

    const char *why = "connected";
    uint32_t    held_since = 0;

    while (!ble_keyboard_connected()) {
        const uint32_t elapsed = millis() - started;
        if (elapsed >= timeout_ms) { why = "timed out"; break; }

        /* Held, not merely seen. A stray byte from the serial pad - a terminal
         * echoing a newline is enough - counts as START for 90 ms, and that was
         * dismissing this screen in about a second. */
        if (nes_get_gamepad_state()) {
            if (!held_since) held_since = millis();
            if (millis() - held_since >= 400) { why = "skipped"; break; }
        } else {
            held_since = 0;
        }

        const char *now  = ble_keyboard_status();
        const int   left = (int)((timeout_ms - elapsed) / 1000);

        /* Redraw on a status change or once a second - a flush costs 6.5 ms and
         * the panel only scans at 17 Hz, so there is nothing to gain from
         * going faster, and a visibly counting number is what distinguishes
         * "still working" from "hung". */
        if (left != shown_left || strncmp(now, shown, sizeof shown) != 0) {
            char line[48];

            tft.fillScreen(TFT_WHITE);
            tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 26, TFT_BLACK);
            tft.drawString(8, 6, "WAITING FOR KEYBOARD", TFT_WHITE, TFT_BLACK, 2);

            tft.drawString(20, 60, now, TFT_BLACK, TFT_WHITE, 1);

            snprintf(line, sizeof line, "STARTING IN %d S", left);
            tft.drawString(20, 90, line, TFT_BLACK, TFT_WHITE, 2);

            tft.drawString(20, 140, "WASD    F   G", TFT_BLACK,
                           TFT_WHITE, 1);
            tft.drawString(20, 156, "4 - SELECT       5 - START", TFT_BLACK,
                           TFT_WHITE, 1);

            /* The in-game chords, listed here because this screen is the only
             * one the player is looking at with nothing else to do, and none of
             * them is discoverable by pressing things at random. SELECT is 4,
             * so every one of them starts with 4 - which is worth showing as
             * the key rather than as the pad name, since the key is what the
             * hand is on. */
            tft.drawString(20, 184, "IN GAME", TFT_BLACK, TFT_WHITE, 1);
            tft.drawString(20, 200, "4+F  DITHER MODE     4+G  INVERT",
                           TFT_BLACK, TFT_WHITE, 1);
            tft.drawString(20, 216, "4+W / 4+S  BRIGHTNESS",
                           TFT_BLACK, TFT_WHITE, 1);
            tft.drawString(20, 232, "4+5 HELD   BACK TO THE ROM LIST",
                           TFT_BLACK, TFT_WHITE, 1);

            tft.drawString(20, 262, "HOLD ANY BUTTON TO SKIP - IT KEEPS",
                           TFT_BLACK, TFT_WHITE, 1);
            tft.drawString(20, 278, "CONNECTING IN THE BACKGROUND ANYWAY",
                           TFT_BLACK, TFT_WHITE, 1);
            tft.flush();

            strncpy(shown, now, sizeof shown - 1);
            shown[sizeof shown - 1] = '\0';
            shown_left = left;
        }
        delay(60);
    }

    Serial.printf("      BLE wait ended after %lu ms: %s - %s\n",
                  (unsigned long)(millis() - started), why,
                  ble_keyboard_status());
}
#endif

/* A loading splash and nothing more permanent than that. The game blit fills
 * all 400x300 now, so the first frame that reaches the glass overwrites every
 * pixel of this; it exists only so the panel is not still showing the menu
 * while the ROM is being unpacked into PSRAM. */
static void draw_loading_screen(const char *rom)
{
    /* Takes the panel to the game's scan rate. st7305_set_fps re-enters high
     * power mode itself, so this also undoes the ST7305_SLOW the card path
     * leaves behind after the menu - no setMode needed either way. Costs a few
     * hundred ms of visible settling, which this splash absorbs. */
    st7305_set_fps(tft.raw(), NES_PANEL_FPS);

    if (rom && *rom == '/') rom++;

    tft.fillScreen(TFT_WHITE);
    tft.drawString((DISPLAY_WIDTH - TFTDriver::textWidth(rom, 2)) / 2,
                   DISPLAY_HEIGHT / 2 - 20, rom, TFT_BLACK, TFT_WHITE, 2);
    tft.drawString((DISPLAY_WIDTH - TFTDriver::textWidth("LOADING", 1)) / 2,
                   DISPLAY_HEIGHT / 2 + 10, "LOADING", TFT_BLACK, TFT_WHITE, 1);
    tft.flush();
}

void setup()
{
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== ESP32-S3-RLCD-4.2 : NES on a reflective LCD ===");

    setup_controller();
    Serial.println("[1/4] controller ready");

    Serial.println("[2/4] display...");
    tft.init();                       /* clears to white and flushes */
    Serial.printf("     ok ST7305 %dx%d\n", DISPLAY_WIDTH, DISPLAY_HEIGHT);

#if NES_BLE_KEYBOARD
    ble_keyboard_init();              /* radio up, saved choice loaded */

    /* Every boot. The cursor starts on last time's keyboard, so the usual case
     * is one button press, and there is never a state you cannot get out of
     * without a reflash. */
    const bool want_keyboard = ble_keyboard_menu();

    /* Returns straight away - scanning and pairing run on their own task, so a
     * keyboard that is switched on later still connects, and one that never
     * appears does not hold up the game. */
    ble_keyboard_begin();
    Serial.println("      WASD = d-pad, F = A, G = B, 5 = START, 4 = SELECT");
#endif

#if NES_EMBEDDED_ROM
    Serial.println("[3/4] ROM is built into this binary - no card needed");
#else
    Serial.println("[3/4] TF card...");
    mount_card();                     /* the menu reports an empty/absent card */
#endif

    Serial.println("[4/4] memory");
    Serial.printf("     heap   %u bytes\n",
                  (unsigned)esp_get_free_heap_size());
    Serial.printf("     PSRAM  %u of %u bytes free\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        Serial.println("     x no PSRAM - set Tools > PSRAM to OPI PSRAM.");
        Serial.println("       ROMs and the NES framebuffer will not fit without it.");
    }

#if NES_SERIAL_CONTROL
    Serial.println("\nSerial gamepad: WASD = d-pad, K = A, J = B, "
                   "Enter = START, Space = SELECT");
#endif

#if NES_BLE_KEYBOARD
    /* Skipped when the picker was told to play without one - there would be
     * nothing to wait for. */
    if (want_keyboard) wait_for_keyboard(12000);
#endif

    /* MENU, GAME, MENU AGAIN. It used to be menu-then-game-forever, with the
     * only way back a reset - which on a board that spends several seconds
     * finding a Bluetooth keyboard is a real cost, and the reason this loop
     * exists rather than an esp_restart() on the quit chord.
     *
     * Two ways round: the player holds SELECT+START (controller.cpp calls
     * nes_poweroff(), nofrendo_main returns 0), or the ROM would not load
     * (nofrendo_main returns non-zero). Both come back here. */
    for (;;) {
        const int selected = show_menu();
        if (selected < 0) {
            Serial.println("nothing selected");
            return;
        }

        const char *rom = get_selected_game();
        Serial.printf("\nstarting %s\n", rom);
        draw_loading_screen(rom);

#if ENABLE_SOUND
        char *argv[] = { (char *)"nes", (char *)"-sound", (char *)"-volume",
                         (char *)"100", (char *)"-sample", (char *)"16000",
                         (char *)rom };
        const int rc = nofrendo_main(7, argv);
#else
        char *argv[] = { (char *)"nes", (char *)"-nosound", (char *)rom };
        const int rc = nofrendo_main(3, argv);
#endif

        /* TEARDOWN, AND THE ORDER MATTERS.
         *
         * vid_shutdown() first: it drains the dither task and takes the panel
         * back from the TE task, so from here this thread owns the glass again
         * and the menu can draw. osd_shutdown() then stops the 60 Hz tick that
         * would otherwise keep calling into an emulator that no longer exists.
         * main_eject() last, because it is what frees the ROM, the mapper, the
         * PPU, the APU and the CPU - none of which may go while something is
         * still running against them.
         *
         * The NES bitmaps are deliberately NOT freed: osd.cpp keeps its three
         * and reuses them, which is why vid_init() can be entered again. */
        Serial.printf("[EXIT] emulator returned %d - tearing down\n", rc);
        vid_shutdown();
        osd_shutdown();
        main_eject();
        Serial.printf("[EXIT] free heap %u, PSRAM %u\n",
                      (unsigned)esp_get_free_heap_size(),
                      (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

        /* A ROM that would not load leaves nothing on the glass to explain
         * itself - the loading splash is still up and looks like a hang, which
         * is exactly what it used to be. Say so, then go back to the list. */
        if (rc != 0) {
            const char *name = (rom && *rom == '/') ? rom + 1 : rom;
            tft.fillScreen(TFT_WHITE);
            tft.drawString(20, 90, "COULD NOT LOAD", TFT_BLACK, TFT_WHITE, 2);
            tft.drawString(20, 120, name, TFT_BLACK, TFT_WHITE, 1);
            tft.drawString(20, 150, "SEE THE SERIAL LOG FOR WHY",
                           TFT_BLACK, TFT_WHITE, 1);
            tft.flush();
            delay(2500);
        }
    }
}

void loop()
{
    delay(1000);
}
