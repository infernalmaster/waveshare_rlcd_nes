/* ble_menu.cpp - pick a Bluetooth keyboard on the panel.
 *
 * Replaces having to edit NES_BLE_KEYBOARD_NAME and reflash. The choice is
 * saved to NVS, so this only appears the first time - hold SELECT at boot to
 * get back to it.
 *
 * Input comes from nes_get_gamepad_state(), which at this point means the GPIO
 * buttons, the board's BOOT/KEY buttons and the serial pad. Not the Bluetooth
 * keyboard, for the obvious reason.
 *
 * Selection is stored by NAME rather than address: this keyboard was observed
 * advertising under two different addresses in consecutive runs, so an address
 * identifies a moment, not a device.
 */
#include <Arduino.h>
#include <string.h>
#include <strings.h>

#include "hw_config.h"

#if defined(NES_BLE_KEYBOARD) && NES_BLE_KEYBOARD

#include "src/nofrendo/tft_driver.h"
#include "ble_keyboard.h"

#define HW_MASK_A      0x01
#define HW_MASK_B      0x02
#define HW_MASK_START  0x08
#define HW_MASK_UP     0x10
#define HW_MASK_DOWN   0x20

#define INK   TFT_BLACK
#define PAPER TFT_WHITE

#define MAX_FOUND  8
#define LIST_TOP   64
#define ROW_H      26

/* Two fixed entries after the discovered devices. */
#define EXTRA_RESCAN 0
#define EXTRA_SKIP   1
#define EXTRA_FORGET 2
#define EXTRA_COUNT  3

extern TFTDriver tft;
extern "C" int nes_get_gamepad_state();

static ble_kb_device_t found[MAX_FOUND];
static int             found_count = 0;

static void banner(const char *line1, const char *line2)
{
    tft.fillScreen(PAPER);
    tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 26, INK);
    tft.drawString(8, 6, "BLUETOOTH KEYBOARD", PAPER, INK, 2);
    tft.drawString(20, 90, line1, INK, PAPER, 2);
    if (line2) tft.drawString(20, 120, line2, INK, PAPER, 1);
    tft.flush();
}

static void draw_row(int slot, const char *text, const char *right,
                     bool selected)
{
    const int y = LIST_TOP + slot * ROW_H;
    const uint16_t bg = selected ? INK : PAPER;
    const uint16_t fg = selected ? PAPER : INK;

    tft.drawFilledRect(4, y - 3, DISPLAY_WIDTH - 8, ROW_H - 2, bg);
    tft.drawString(14, y, text, fg, bg, 2);
    if (right && *right)
        tft.drawString(DISPLAY_WIDTH - 14 - TFTDriver::textWidth(right, 1),
                       y + 4, right, fg, bg, 1);
}

/* Row label for index i, which spans the found devices then the fixed extras. */
static void row_label(int i, char *out, size_t len, char *right, size_t rlen)
{
    right[0] = '\0';

    if (i < found_count) {
        const char *saved = ble_keyboard_target_name();
        const bool  last  = saved && saved[0] && found[i].name[0] &&
                            strcasecmp(found[i].name, saved) == 0;

        /* "(no name)" is generated here, for display only - the stored name
         * stays empty so the matcher falls back to the address. */
        snprintf(out, len, "%s%s", last ? "* " : "",
                 found[i].name[0] ? found[i].name : "(NO NAME)");
        /* Bonded matters: it is the difference between "will just connect" and
         * "has to pair, so the keyboard must be on a free profile". */
        snprintf(right, rlen, "%s %s", found[i].addr,
                 found[i].bonded ? "PAIRED" : "");
        return;
    }

    switch (i - found_count) {
    case EXTRA_RESCAN: snprintf(out, len, "SCAN AGAIN"); break;
    case EXTRA_SKIP:   snprintf(out, len, "PLAY WITHOUT KEYBOARD"); break;
    case EXTRA_FORGET: snprintf(out, len, "FORGET SAVED + ALL PAIRINGS"); break;
    default:           snprintf(out, len, "?"); break;
    }
}

static void draw_all(int selected, int top, int rows)
{
    char label[48], right[32];

    tft.fillScreen(PAPER);
    tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 26, INK);
    tft.drawString(8, 6, "BLUETOOTH KEYBOARD", PAPER, INK, 2);

    if (found_count == 0)
        tft.drawString(14, 36, "NO BLE HID DEVICES FOUND - IS IT AWAKE?",
                       INK, PAPER, 1);
    else
        tft.drawString(14, 36, "PICK ONE, OR PLAY WITHOUT", INK, PAPER, 1);

    const int total = found_count + EXTRA_COUNT;
    for (int slot = 0; slot < rows && top + slot < total; slot++) {
        row_label(top + slot, label, sizeof label, right, sizeof right);
        draw_row(slot, label, right, top + slot == selected);
    }

    /* Both control schemes, because which one you have is not knowable from
     * here. The board's own two buttons are the only input guaranteed to exist
     * at this point: the Bluetooth keyboard is what we are choosing, external
     * buttons are optional, and serial means being tethered to a computer. */
    tft.drawString(8, DISPLAY_HEIGHT - 26, "KEY BUTTON: NEXT    BOOT BUTTON: "
                   "SELECT", INK, PAPER, 1);
    tft.drawString(8, DISPLAY_HEIGHT - 14, "OR  UP/DN  CHOOSE   A  SELECT",
                   INK, PAPER, 1);
    tft.flush();
}

/* Waits for every button to be released, so one press is not read twice by the
 * next screen. */
static void wait_release(void)
{
    while (nes_get_gamepad_state()) delay(20);
    delay(60);
}

bool ble_keyboard_menu(void)
{
    const int rows = (DISPLAY_HEIGHT - 40 - LIST_TOP) / ROW_H;

    for (;;) {
        banner("SCANNING...", "5 SECONDS");
        found_count = ble_keyboard_scan(found, MAX_FOUND, 5000);
        Serial.printf("[BLE] menu: %d device(s) found\n", found_count);

        const int total = found_count + EXTRA_COUNT;

        /* Start on whatever was chosen last time, so the common case - same
         * keyboard, same desk - is one button press. */
        int selected = 0;
        const char *saved = ble_keyboard_target_name();
        if (saved && saved[0]) {
            for (int i = 0; i < found_count; i++) {
                if (strcasecmp(found[i].name, saved) == 0) {
                    selected = i;
                    break;
                }
            }
        }

        int top      = 0;
        int drawn    = -1;
        int drawn_top = -1;
        uint32_t repeat_at = 0;
        int held = 0;

        wait_release();

        for (;;) {
            if (selected != drawn || top != drawn_top) {
                draw_all(selected, top, rows);
                drawn = selected;
                drawn_top = top;
            }

            const int hw = nes_get_gamepad_state();
            const uint32_t now = millis();

            /* START doubles as "next entry" so the board's KEY button can walk
             * the list on its own. It wraps, which is what makes one button
             * enough to reach everything. A d-pad user never notices: UP/DOWN
             * still work and A is still what confirms. */
            int dir = (hw & HW_MASK_UP)   ? -1
                    : (hw & HW_MASK_DOWN) ?  1
                    : (hw & HW_MASK_START) ? 1 : 0;

            if (dir == 0) {
                held = 0;
            } else if (dir != held) {
                held = dir;
                repeat_at = now + 320;
                selected += dir;
            } else if ((int32_t)(now - repeat_at) >= 0) {
                repeat_at = now + 140;
                selected += dir;
            }

            if (selected < 0) selected = total - 1;          /* wrap */
            if (selected >= total) selected = 0;
            if (selected < top) top = selected;
            if (selected >= top + rows) top = selected - rows + 1;

            /* A only - START is the "next" key above and cannot also confirm. */
            if (hw & HW_MASK_A) {
                wait_release();

                if (selected < found_count) {
                    ble_keyboard_set_target(found[selected].name,
                                            found[selected].addr);
                    banner("SELECTED", found[selected].name);
                    delay(700);
                    return true;
                }

                switch (selected - found_count) {
                case EXTRA_RESCAN:
                    break;                              /* out to rescan */
                case EXTRA_SKIP:
                    ble_keyboard_disable();
                    banner("NO KEYBOARD", "BUTTONS AND SERIAL STILL WORK");
                    delay(700);
                    return false;
                case EXTRA_FORGET:
                    ble_keyboard_forget();
                    banner("FORGOTTEN", "CLEAR IT ON THE KEYBOARD TOO");
                    delay(1200);
                    break;                              /* out to rescan */
                }
                break;                                  /* rescan */
            }

            delay(20);
        }
    }
}

#endif /* NES_BLE_KEYBOARD */
