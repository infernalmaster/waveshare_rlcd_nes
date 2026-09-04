/* controller.cpp - gamepad for the ST7305 NES example.
 *
 * Provides the two symbols the emulator core expects a sketch to supply:
 * setup_controller() and nes_get_gamepad_state(). Four input sources are OR'd
 * together, so any of them works and none has to be present:
 *
 *   - eight external buttons to GND on the GPIOs in hw_config.h
 *   - the board's own BOOT and KEY buttons, as A and START
 *   - a BLE keyboard (ble_keyboard.cpp)
 *   - the USB serial console, so the example is playable on a bare board
 *
 * Returned bits are the HW_MASK_* set osd.cpp maps to the NES pad.
 */
#include <Arduino.h>
#include <string.h>

#include "hw_config.h"
#include "src/nofrendo/tft_driver.h"      /* TFTDriver == ST7305Tft here */
#include "ble_keyboard.h"

#define NES_A      0x01
#define NES_B      0x02
#define NES_SELECT 0x04
#define NES_START  0x08
#define NES_UP     0x10
#define NES_DOWN   0x20
#define NES_LEFT   0x40
#define NES_RIGHT  0x80

extern TFTDriver tft;

/* Ends the emulation loop AND stops main_loop() re-inserting the same
 * cartridge, which is what makes the quit chord reach the ROM browser instead
 * of the game's own title screen. Frees nothing, so it is safe to call from
 * here - see the note above main_stop() in nofrendo.c. Declared rather than
 * including nofrendo.h, which drags the emulator core API into a file that
 * wants one function. */
extern "C" void main_stop(void);

#if NES_SERIAL_CONTROL
/* A terminal sends a keypress as one character and tells you nothing about the
 * release, so each key is latched "down" for a fixed window and key repeat
 * keeps renewing it. That is what makes held-direction movement work at all
 * from a console; it also means a tap always registers as ~90 ms of button. */
static uint32_t serial_hold_until[8];

static int serial_bit_for(int ch)
{
    switch (ch) {
    case 'w': case 'W': return NES_UP;
    case 's': case 'S': return NES_DOWN;
    case 'a': case 'A': return NES_LEFT;
    case 'd': case 'D': return NES_RIGHT;
    case 'k': case 'K': return NES_A;
    case 'j': case 'J': return NES_B;
    case '\r': case '\n': return NES_START;
    case ' ': return NES_SELECT;
    default: return 0;
    }
}

static int serial_state()
{
    const uint32_t now = millis();

    while (Serial.available() > 0) {
        const int bit = serial_bit_for(Serial.read());
        if (!bit) continue;
        for (int i = 0; i < 8; i++)
            if (bit & (1 << i)) serial_hold_until[i] = now + NES_SERIAL_HOLD_MS;
    }

    int state = 0;
    for (int i = 0; i < 8; i++) {
        /* Signed comparison so this stays correct across the millis() wrap. */
        if ((int32_t)(serial_hold_until[i] - now) > 0) state |= 1 << i;
    }
    return state;
}
#endif /* NES_SERIAL_CONTROL */

void setup_controller()
{
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_LEFT, INPUT_PULLUP);
    pinMode(BTN_RIGHT, INPUT_PULLUP);
    pinMode(BTN_A, INPUT_PULLUP);
    pinMode(BTN_B, INPUT_PULLUP);
    pinMode(BTN_START, INPUT_PULLUP);
    pinMode(BTN_SELECT, INPUT_PULLUP);

#if NES_USE_ONBOARD_BUTTONS
    pinMode(BTN_ONBOARD_BOOT, INPUT_PULLUP);
    pinMode(BTN_ONBOARD_KEY, INPUT_PULLUP);
#endif

#if NES_SERIAL_CONTROL
    memset(serial_hold_until, 0, sizeof serial_hold_until);
#endif
}

extern "C" int IRAM_ATTR nes_get_gamepad_state()
{
    int state = 0;

    if (digitalRead(BTN_A) == LOW)      state |= NES_A;
    if (digitalRead(BTN_B) == LOW)      state |= NES_B;
    if (digitalRead(BTN_SELECT) == LOW) state |= NES_SELECT;
    if (digitalRead(BTN_START) == LOW)  state |= NES_START;
    if (digitalRead(BTN_UP) == LOW)     state |= NES_UP;
    if (digitalRead(BTN_DOWN) == LOW)   state |= NES_DOWN;
    if (digitalRead(BTN_LEFT) == LOW)   state |= NES_LEFT;
    if (digitalRead(BTN_RIGHT) == LOW)  state |= NES_RIGHT;

#if NES_USE_ONBOARD_BUTTONS
    if (digitalRead(BTN_ONBOARD_BOOT) == LOW) state |= NES_A;
    if (digitalRead(BTN_ONBOARD_KEY) == LOW)  state |= NES_START;
#endif

    /* Real HID reports carry the set of keys held right now, so unlike the
     * serial pad this needs no hold-window guesswork - a key is down exactly
     * while it is down. */
    state |= ble_keyboard_buttons();

#if NES_SERIAL_CONTROL
    state |= serial_state();
#endif

    /* --- SELECT + START, HELD: quit to the ROM menu -------------------------
     *
     * A HOLD AND NOT A TAP, unlike every other chord here, because SELECT and
     * START together is something games genuinely use - it is the soft reset on
     * a real NES and several games bind it themselves. Three quarters of a
     * second is longer than any of that and shorter than it takes to wonder
     * whether it worked.
     *
     * main_stop() only sets two flags; nes_emulate()'s loop notices at the end
     * of the current frame and returns, main_loop() then declines to re-insert
     * the cartridge, and the sketch tears the emulator down and goes back to
     * show_menu(). Nothing here blocks and nothing here frees. */
    static uint32_t quit_since = 0;
    if ((state & NES_SELECT) && (state & NES_START)) {
        const uint32_t now = millis();
        if (!quit_since) quit_since = now;
        if (now - quit_since >= 750) {
            quit_since = 0;
            Serial.println("[EXIT] SELECT+START held - leaving the game");
            main_stop();
        }
        state &= ~(NES_SELECT | NES_START);
    } else {
        quit_since = 0;
    }

    /* --- picture chords, all held with SELECT -------------------------------
     *
     * WHY CHORDS AND NOT A MENU. Every one of these is a knob you can only set
     * by looking at the glass while a game is running - whether a given game
     * reads better inverted depends on how dark its backgrounds are, which
     * dither suits its art depends on the art, and the right brightness depends
     * on the light in the room. A menu would mean leaving the picture to change
     * the picture. SELECT is the least-used button on the pad and holding it
     * costs a running game nothing, because every chord below is swallowed
     * before the state is returned.
     *
     *   SELECT + B      swap ink and paper
     *   SELECT + A      next reduction algorithm
     *   SELECT + UP     brighter, by 8
     *   SELECT + DOWN   darker, by 8
     *
     * All edge-triggered, so a held chord fires once. Brightness would be
     * pleasanter with key repeat, but repeat needs a timer and a rate and this
     * needs neither - 16 presses cross the whole useful range. */
    const bool sel = (state & NES_SELECT) != 0;

    static bool invert_was_down = false;
    const bool invert_down = sel && (state & NES_B);
    if (invert_down) {
        if (!invert_was_down) {
            tft.setInvertVideo(!tft.invertVideo());
            tft.rebuildDither();
        }
        state &= ~(NES_SELECT | NES_B);
    }
    invert_was_down = invert_down;

    static bool mode_was_down = false;
    const bool mode_down = sel && (state & NES_A);
    if (mode_down) {
        if (!mode_was_down) {
            /* Wraps, so the chord alone reaches every mode and there is no
             * "back" to need a second one. */
            const int next = ((int)tft.monoMode() + 1) % ST7305_MONO_COUNT;
            tft.setMonoMode((st7305_mono_t)next);
            /* Allocates or frees the diffusion scratch as well as rebuilding
             * the table, and may refuse the mode if there is no room - which is
             * why the readout reads monoMode() back rather than trusting this. */
            tft.rebuildDither();
            Serial.printf("[VIDEO] dither mode -> %s\n", tft.monoModeName());
        }
        state &= ~(NES_SELECT | NES_A);
    }
    mode_was_down = mode_down;

    static bool bright_was_down = false;
    const int  bright_dir = (state & NES_UP) ? 1 : ((state & NES_DOWN) ? -1 : 0);
    const bool bright_down = sel && bright_dir != 0;
    if (bright_down) {
        if (!bright_was_down) {
            tft.setBrightness(tft.brightness() + bright_dir * 8);
            tft.rebuildDither();
            Serial.printf("[VIDEO] brightness -> %d\n", tft.brightness());
        }
        state &= ~(NES_SELECT | NES_UP | NES_DOWN);
    }
    bright_was_down = bright_down;

    return state;
}
