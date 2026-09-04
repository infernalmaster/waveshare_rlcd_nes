/* nes_menu.cpp - ROM browser for the 400x300 mono panel.
 *
 * Replaces the emulator's own show_menu() / get_selected_game(), which osd.cpp
 * compiles out when NES_USE_ST7305 is set. That menu marks the selected entry
 * with a red bar behind white text; red and black differ by about 30 units of
 * luminance out of 255, so after the 1-bit reduction the selected row and the
 * others are the same two colours in the same places and the list has no
 * visible cursor at all.
 *
 * Here selection is a filled black bar with knocked-out white text, which is
 * the one contrast a 1-bit panel definitely has.
 */
#include <Arduino.h>

#include "hw_config.h"

/* With the ROM built into the binary there is no card to browse, and
 * nes_rom_embedded.cpp supplies show_menu() and get_selected_game() instead. */
#if !(defined(NES_EMBEDDED_ROM) && NES_EMBEDDED_ROM)

#include <FS.h>
#include <strings.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include "src/nofrendo/tft_driver.h"

#define HW_MASK_A      0x01
#define HW_MASK_B      0x02
#define HW_MASK_SELECT 0x04
#define HW_MASK_START  0x08
#define HW_MASK_UP     0x10
#define HW_MASK_DOWN   0x20

#define INK   TFT_BLACK
#define PAPER TFT_WHITE

/* THE LIST LIVES IN PSRAM, which is what lets these be large enough not to
 * matter. 512 x 96 is 48 KB - nothing against 8 MB, and everything against the
 * 6 KB of internal RAM the old 64 x 40 static array cost.
 *
 * Both old limits were wrong in a way that was silent. 64 entries truncated a
 * well-stocked card and only said so on the serial log, which nobody watching
 * the panel would see. And 40 characters is shorter than plenty of real ROM
 * names - "Teenage Mutant Ninja Turtles II (USA).nes" is 41 - so a long name
 * was stored truncated and then failed to open, which looks like a corrupt
 * card rather than a buffer that was too small. */
#define MAX_GAMES  512
#define NAME_LEN   128

/* get_selected_game() prepends a slash, so its buffer is one longer. */

/* Used only if the PSRAM allocation fails, which on this board means PSRAM is
 * not configured at all - in which case the emulator will not run either, and
 * showing a short list beats showing none. */
#define MAX_GAMES_FALLBACK 32

#define HEADER_H   26
#define LIST_TOP   38
#define ROW_H      26
#define ROWS       9
#define FOOTER_Y   (DISPLAY_HEIGHT - 16)

/* Repeat rates for a held direction: a long-ish first step so a tap moves
 * exactly one row, then faster, so a 60-entry card is still walkable. */
#define REPEAT_FIRST_MS 320
#define REPEAT_NEXT_MS  110

/* Rereads the card. SELECT+B, because the menu loop below reads only UP, DOWN,
 * START and A, and the SELECT+B the emulator uses to invert the picture is
 * handled in its frame loop, which is not running while this one is. Two
 * buttons rather than one so it cannot be hit by accident on the way past. */
#define HW_CHORD_RESCAN (HW_MASK_SELECT | HW_MASK_B)

extern TFTDriver tft;
extern "C" int nes_get_gamepad_state();
/* The .ino. Mounts SD_MMC on the hw_config.h pins, and logs the result. */
extern bool mount_card();

static char *games      = NULL;     /* games_cap * NAME_LEN, PSRAM if possible */
static int   games_cap  = 0;
static int   game_count = 0;
/* THE CARD IS READ ONCE PER BOOT, not once per menu. show_menu() is reachable
 * again from every game, and rescanning there cost seconds of "READING CARD"
 * rediscovering a card that had not moved.
 *
 * Only show_menu()'s opening scan consults this. The SELECT+B chord goes
 * straight to reread_card() rather than clearing the flag, because a swap needs
 * the mount rebuilt as well as the list refilled - see there. */
static bool  list_loaded = false;
/* NOT reset by show_menu(). Coming back from a game should land the cursor on
 * the game you just left, not at the top of the list. */
static int   game_selected = 0;

static char *game_at(int i) { return games + (size_t)i * NAME_LEN; }

/* Once. show_menu() can be called many times now that there is a way back to
 * it, and re-allocating 48 KB each time would fragment PSRAM for nothing. */
static void alloc_list()
{
    static char fallback[MAX_GAMES_FALLBACK][NAME_LEN];

    if (games) return;

    games = (char *)heap_caps_malloc((size_t)MAX_GAMES * NAME_LEN,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (games) {
        games_cap = MAX_GAMES;
        return;
    }

    Serial.println("[MENU] no PSRAM for the ROM list - short list only");
    games     = &fallback[0][0];
    games_cap = MAX_GAMES_FALLBACK;
}

static bool has_nes_suffix(const char *name)
{
    const size_t n = strlen(name);
    if (n < 5) return false;                        /* "x.nes" is the shortest */
    return strcasecmp(name + n - 4, ".nes") == 0;
}

/* Progress, because a card with a few hundred files takes long enough that a
 * still panel reads as a hung one. Drawn straight rather than through the list
 * machinery, which does not exist yet at this point. */
static void draw_scanning(int found, int seen)
{
    char line[40];

    tft.fillScreen(PAPER);
    tft.drawFilledRect(0, 0, DISPLAY_WIDTH, HEADER_H, INK);
    tft.drawString(8, 6, "NES", PAPER, INK, 2);
    tft.drawString(8 + 4 * 12, 10, "READING CARD", PAPER, INK, 1);

    snprintf(line, sizeof line, "%d ROMS", found);
    tft.drawString(20, 110, line, INK, PAPER, 2);
    snprintf(line, sizeof line, "%d FILES SEEN", seen);
    tft.drawString(20, 140, line, INK, PAPER, 1);
    tft.flush();
}

static void scan_games()
{
    const uint32_t t0 = millis();
    int seen = 0;

    game_count = 0;
    /* Set before the card is touched, so the failure path below still counts as
     * having looked. Nothing retries on its own - SELECT+B is the retry. */
    list_loaded = true;
    alloc_list();

    File root = SD_MMC.open("/");
    if (!root) {
        Serial.println("[MENU] cannot open card root");
        return;
    }

    for (;;) {
        File entry = root.openNextFile();
        if (!entry) break;
        seen++;

        /* Directories first and cheapest - a card organised into folders can
         * have more of them than ROMs, and there is no point measuring a name
         * that cannot be one. */
        if (!entry.isDirectory()) {
            const char *name = entry.name();
            /* SD_MMC hands back a leading slash where SD does not, depending
             * on core version - strip it either way so the list holds bare
             * names. */
            if (name[0] == '/') name++;

            if (name[0] != '.' && has_nes_suffix(name) &&
                game_count < games_cap) {
                /* LISTED ONLY IF IT FITS WHOLE. This entry IS the path handed
                 * to SD_MMC.open() when the game starts, so a truncated copy
                 * does not merely display wrong - it loses the ".nes" and the
                 * open fails, which reads on the serial log as a bad card
                 * rather than as a buffer that was too small. That is exactly
                 * how the old 40-byte limit hid itself. Shortening for the
                 * glass is display_name()'s job and happens on a copy.
                 *
                 * Skipped rather than stored, and said out loud: a game missing
                 * from the list with a reason on the log is findable. One that
                 * is in the list and refuses to start is not. */
                if (strlen(name) >= NAME_LEN) {
                    Serial.printf("[MENU] skipped, name over %d chars: %s\n",
                                  NAME_LEN - 1, name);
                } else {
                    strcpy(game_at(game_count), name);
                    game_count++;
                }
            }
        }
        entry.close();

        /* NOT PER ENTRY, either of these. A serial line is about 4 ms at
         * 115200 and a panel flush is 6.5 ms, so doing them for every file on
         * a 300-ROM card would add three seconds to a scan and then be blamed
         * on the card. Every 24th is often enough to look alive. */
        if ((seen % 24) == 0) draw_scanning(game_count, seen);
    }
    root.close();

    Serial.printf("[MENU] %d ROM(s) out of %d entries in %lu ms\n",
                  game_count, seen, (unsigned long)(millis() - t0));

    if (game_count >= games_cap)
        Serial.printf("[MENU] LIST FULL at %d - the rest of the card is not "
                      "shown\n", games_cap);
}

/* A SWAPPED CARD NEEDS THE MOUNT REBUILT, not just the directory read again.
 * FatFs caches FAT and directory sectors, so scanning a new card over the old
 * mount hands back the old card's listing - or a mix of the two, which is the
 * worse failure because it looks plausible until a game refuses to open.
 *
 * A failed remount leaves the list alone. Pulling a card should not cost you a
 * working menu. */
static bool reread_card()
{
    Serial.println("\n[MENU] rereading the card...");

    SD_MMC.end();
    if (!mount_card()) {
        Serial.println("[MENU] remount failed - the list is left as it was");
        tft.fillScreen(PAPER);
        tft.drawString(20, 120, "CARD NOT FOUND", INK, PAPER, 2);
        tft.drawString(20, 150, "NOTHING CHANGED - PUSH IT IN AND TRY AGAIN",
                       INK, PAPER, 1);
        tft.flush();
        delay(1800);
        return false;
    }

    scan_games();
    return true;
}

/* Edge, not level: the chord has to be let go before it counts again, or
 * holding it would start a fresh scan on every pass of a 20 ms loop. */
static bool rescan_pressed(int hw, bool *latched)
{
    const bool down  = (hw & HW_CHORD_RESCAN) == HW_CHORD_RESCAN;
    const bool fired = down && !*latched;

    *latched = down;
    return fired;
}

/* Copies a filename into `out` without its extension, clipped to what will fit
 * across the panel at this scale. */
static void display_name(const char *src, char *out, size_t out_len, int scale)
{
    const int fits = (DISPLAY_WIDTH - 28) / (6 * scale);
    size_t limit = (size_t)(fits > 0 ? fits : 1);
    if (limit > out_len - 1) limit = out_len - 1;

    size_t n = strlen(src);
    if (n >= 4 && strcasecmp(src + n - 4, ".nes") == 0) n -= 4;
    if (n > limit) n = limit;

    memcpy(out, src, n);
    out[n] = '\0';
}

static void draw_header()
{
    char right[24];

    tft.drawFilledRect(0, 0, DISPLAY_WIDTH, HEADER_H, INK);
    tft.drawString(8, 6, "NES", PAPER, INK, 2);
    tft.drawString(8 + 4 * 12, 10, "SELECT A GAME", PAPER, INK, 1);

    snprintf(right, sizeof right, "%d/%d", game_count ? game_selected + 1 : 0,
             game_count);
    tft.drawString(DISPLAY_WIDTH - 8 - TFTDriver::textWidth(right, 1), 6, right,
                   PAPER, INK, 1);
}

static void draw_row(int slot, int index, bool selected)
{
    const int y = LIST_TOP + slot * ROW_H;
    const uint16_t bg = selected ? INK : PAPER;
    const uint16_t fg = selected ? PAPER : INK;
    char name[NAME_LEN];

    tft.drawFilledRect(4, y - 3, DISPLAY_WIDTH - 8, ROW_H - 2, bg);
    if (index < 0 || index >= game_count) return;

    display_name(game_at(index), name, sizeof name, 2);
    tft.drawString(14, y, name, fg, bg, 2);
}

static void draw_list(int top)
{
    for (int slot = 0; slot < ROWS; slot++) {
        const int index = top + slot;
        draw_row(slot, index < game_count ? index : -1, index == game_selected);
    }
}

static void draw_footer()
{
    tft.drawFilledRect(0, FOOTER_Y - 16, DISPLAY_WIDTH, 28, PAPER);
    tft.drawString(8, FOOTER_Y - 12,
                   "KEY BUTTON: NEXT    BOOT BUTTON: PLAY    SEL+B: RESCAN",
                   INK, PAPER, 1);
    /* The way back is the one worth the space here: a player who does not know
     * it has to reach for the reset button, and on a board that spends several
     * seconds finding a keyboard that is a real cost. The picture chords are on
     * the keyboard-wait screen, which is where there is room for all of them. */
    tft.drawString(8, FOOTER_Y, "OR  UP/DN  CHOOSE   A  PLAY   "
                                "SEL+START  BACK HERE", INK, PAPER, 1);
}

static void draw_empty()
{
    tft.fillScreen(PAPER);
    draw_header();
    tft.drawString(20, 90, "NO .NES FILES ON THE CARD", INK, PAPER, 2);
    tft.drawString(20, 120, "FAT32, ROMS IN THE ROOT FOLDER", INK, PAPER, 1);
    tft.drawString(20, 136, "TF SLOT IS SDMMC 1-BIT: CLK 38 CMD 21 D0 39", INK,
                   PAPER, 1);
    /* THE WAY OUT. This screen used to be a dead end that only the reset button
     * cleared. Harmless when every trip through the menu reread the card; a
     * trap now that it is read once per boot, because pushing a card in would
     * otherwise have no way of being noticed. */
    tft.drawString(20, 168, "INSERT A CARD, THEN SELECT+B TO READ IT AGAIN",
                   INK, PAPER, 1);
    tft.flush();
}

extern "C" int show_menu()
{
    /* Low power scan mode: writing GRAM still forces a refresh at ~8 Hz, so a
     * menu feels no slower than high power mode while drawing a fraction of the
     * current. See st7305_gfx/AGENTS.md section 2. Set BEFORE the scan so the
     * progress screen it draws is in the same mode as the list that follows,
     * and so returning here from a game leaves the panel where the menu wants
     * it rather than at the game's scan rate. */
    tft.setMode(ST7305_SLOW);

    if (!list_loaded) {
        Serial.println("\n[MENU] scanning card...");
        scan_games();
    } else {
        Serial.printf("\n[MENU] %d ROM(s) held from the last scan - SELECT+B "
                      "rereads the card\n", game_count);
    }

    /* One pass per list. A rescan breaks out of the inner loop to here, which
     * redraws everything against whatever the card turned out to hold. */
    for (;;) {
        /* Seeded from what is held RIGHT NOW, so a chord still down from the
         * rescan that got us here does not immediately start another one. */
        bool latched = (nes_get_gamepad_state() & HW_CHORD_RESCAN)
                       == HW_CHORD_RESCAN;

        if (game_count == 0) {
            draw_empty();
            for (;;) {                      /* nothing to run - stay readable */
                if (rescan_pressed(nes_get_gamepad_state(), &latched)) break;
                delay(20);
            }
            reread_card();
            continue;
        }

        /* Kept from last time - see the declaration. Clamped, because the
         * card may have changed since. */
        if (game_selected >= game_count) game_selected = game_count - 1;
        if (game_selected < 0)           game_selected = 0;

        int  top = game_selected >= ROWS ? game_selected - ROWS + 1 : 0;
        int  drawn_top    = -1;
        int  drawn_select = -1;
        uint32_t repeat_at = 0;
        int  held          = 0;

        tft.fillScreen(PAPER);
        draw_footer();

        for (;;) {
            if (top != drawn_top) {
                draw_header();
                draw_list(top);
                drawn_top    = top;
                drawn_select = game_selected;
            } else if (game_selected != drawn_select) {
                /* Only the two rows that changed, plus the counter in the
                 * header. A full redraw would be another ~4 ms of CPU for no
                 * visible gain - the flush costs the same either way. */
                if (drawn_select >= top && drawn_select < top + ROWS)
                    draw_row(drawn_select - top, drawn_select, false);
                draw_row(game_selected - top, game_selected, true);
                draw_header();
                drawn_select = game_selected;
            }
            tft.flushIfDirty();

            const int hw = nes_get_gamepad_state();
            const uint32_t now = millis();

            /* Checked before the movement keys, though nothing in the chord
             * overlaps them - SELECT|B is 0x06, and UP, DOWN, START and A are
             * none of it. It reads as the exception it is up here. */
            if (rescan_pressed(hw, &latched)) {
                reread_card();
                break;              /* to the outer loop, with a new list */
            }

            /* START doubles as "next entry", so the board's KEY button alone
             * can walk the list - on a bare board that and BOOT are the only
             * inputs there are. Wraps, which is what makes one enough. */
            const int dir = (hw & HW_MASK_UP)    ? -1
                          : (hw & HW_MASK_DOWN)  ?  1
                          : (hw & HW_MASK_START) ?  1 : 0;

            if (dir == 0) {
                held = 0;
            } else if (dir != held) {
                held = dir;
                repeat_at = now + REPEAT_FIRST_MS;
                game_selected += dir;
            } else if ((int32_t)(now - repeat_at) >= 0) {
                repeat_at = now + REPEAT_NEXT_MS;
                game_selected += dir;
            }

            /* Wraps, so KEY-as-next can reach every entry going one way. */
            if (game_selected < 0) game_selected = game_count - 1;
            if (game_selected >= game_count) game_selected = 0;
            if (game_selected < top) top = game_selected;
            if (game_selected >= top + ROWS) top = game_selected - ROWS + 1;

            /* A only - START is the "next" key above. */
            if (hw & HW_MASK_A) {
                Serial.printf("[MENU] starting %s\n", game_at(game_selected));
                tft.fillScreen(PAPER);
                tft.flush();
                /* Out of low power mode. draw_loading_screen() then picks the
                 * exact game scan rate with st7305_set_fps(NES_PANEL_FPS). */
                tft.setMode(ST7305_FAST);
                /* Let the button go before the emulator starts reading the pad,
                 * or A-to-launch also becomes A-in-the-first-frame. */
                while (nes_get_gamepad_state() & (HW_MASK_A | HW_MASK_START))
                    delay(20);
                return game_selected;
            }

            delay(20);
        }
    }

    /* Not reached: the inner loop only leaves by returning a game or by
     * breaking to the outer one. */
}

extern "C" const char *get_selected_game()
{
    static char path[NAME_LEN + 2];

    if (game_count > 0 && game_selected >= 0 && game_selected < game_count)
        snprintf(path, sizeof path, "/%s", game_at(game_selected));
    else
        path[0] = '\0';

    return path;
}

#endif /* !NES_EMBEDDED_ROM */
