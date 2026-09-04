/* hw_config.h - Waveshare ESP32-S3-RLCD-4.2 pin map for the NES emulator.
 *
 * This file is included BOTH by the sketch root and by the vendored emulator
 * core (src/nofrendo/osd.cpp, src/nofrendo/tft_driver.h) - the Arduino builder
 * puts the sketch root on the include path for every translation unit, which is
 * how the upstream Dsn_nes_Emulator example already worked back when the core
 * was a separate library. So every switch the core reads has to live here, not
 * in a .cpp.
 *
 * Board pin assignments come from the vendor's own examples in this repo -
 * 02_Example/ESP-IDF/09_LVGL_V9_Test/main/user_config.h for the panel,
 * 02_Example/Arduino/06_SD_Card/sdcard_bsp.h for the card, and the ESPHome
 * YAMLs for audio and the two buttons. They are not guesses.
 * The gamepad pins are the exception: nothing on this board is wired
 * to a D-pad, so those are free GPIOs picked for you to solder to. Change them.
 */
#ifndef HW_CONFIG_H
#define HW_CONFIG_H

/* ===== which display driver the emulator compiles against ==================
 * 1 = ST7305 400x300 reflective mono LCD (this board), via ST7305Tft.
 * 0 = the upstream ST7789 colour path. Leave at 1 here. */
#define NES_USE_ST7305 1

/* ===== ST7305 reflective LCD (SPI3/HSPI) ================================== */
#define RLCD_SCK    11
#define RLCD_MOSI   12
#define RLCD_DC      5
#define RLCD_CS     40
#define RLCD_RST    41
/* The panel pulses this once per scan (0x35, sent by st7305_init). It is what
 * makes the game tear-free: the flush is driven off this edge rather than off a
 * timer, so the SPI write starts where the scan does and stays ahead of it. See
 * startTePacing() in st7305_tft.h. */
#define RLCD_TE      6

/* 20 MHz, not 24. The ESP32-S3 divides 80 MHz by an integer, so a 24 MHz
 * request silently becomes 20 - see st7305_gfx/AGENTS.md section 3. Ask for
 * what you actually get. A full 15000-byte flush at this rate costs ~6.5 ms. */
#define RLCD_SPI_HZ 20000000

/* ===== video policy ========================================================
 *
 * THE PANEL'S SCAN RATE IS SET HERE AND NOWHERE ELSE. Change this line and
 * nothing else needs touching: the flush is phase-locked to the panel's own TE
 * pin rather than to any number in the source, so the push rate, the drop rate
 * and the latency all follow the panel by construction. Nothing downstream is
 * allowed to restate the figure - if you find a comment quoting a rate in Hz
 * outside this block, it is a bug in the comment.
 *
 * st7305_init ships ST7305_FPS_17_A6 (16.98 Hz); this overrides it once, on the
 * way into the emulator. st7305_gfx.h's st7305_fps_t has the whole ladder and
 * what was measured versus predicted - every rung up to 23_84 is clean, and
 * 26_80 above it is the vendor's own value and washes the blacks out.
 *
 * Rate is not free, and faster is not simply better: this panel's liquid
 * crystal takes tens of milliseconds to settle, so a quicker scan starts the
 * next frame before the last one has fully arrived and fast motion greys out
 * rather than sharpening. Which rung reads best is a matter of looking at it. */
#define NES_PANEL_FPS ST7305_FPS_17_A6

/* FALLBACK ONLY. Nothing paces the game with this.
 *
 * The game is paced by the TE pin, so the push rate is the panel's own scan
 * rate and no interval here can improve on it. This is what the flush task
 * falls back to if TE goes silent - a board revision that does not wire it, a
 * lost 0x35. That path is already tearing whatever value it uses, so it does
 * NOT have to track NES_PANEL_FPS: it only has to be in the right neighbourhood
 * to keep the picture moving, which is why changing the rate above leaves this
 * line alone. Somewhere near the slowest rung of the ladder is right.
 *
 * It is the task's own timeout, so unlike the emulator-side timer it replaced
 * it is not quantised by the 60 Hz tick - 55 here really does mean 55. */
#define NES_FRAME_INTERVAL_MS 59

/* 1 = swap ink and paper for the game area. NES games with dark backgrounds
 * (most of them) otherwise come out as a mostly-black rectangle, which on a
 * reflective panel is both ugly and slow to settle. Toggle at runtime by
 * pressing SELECT + B together. */
#define NES_INVERT_VIDEO 0

/* ===== how colour becomes black and white ==================================
 *
 * WHICH OF THESE READS BEST IS NOT DECIDABLE FROM HERE. It depends on the game
 * and on the light in the room, so all of them are on SELECT + A and this is
 * only where the machine starts. st7305_mono_t in st7305_tft.h says what each
 * one does and what it costs; the short version is:
 *
 *   ST7305_MONO_BAYER4     the original, a fine cross-hatch, 16 grey levels
 *   ST7305_MONO_BAYER8     64 levels, smoother gradients, coarser texture
 *   ST7305_MONO_BAYER2     4 levels, hard contrast, almost no dither texture
 *   ST7305_MONO_CLUSTER4   newsprint halftone, kind to a bleeding panel
 *   ST7305_MONO_BLUE8      8x8 blue noise, grain instead of a weave
 *   ST7305_MONO_THRESHOLD  no dither, crisp text and sprites, no shading
 *   ST7305_MONO_FLOYD      error diffusion; the best tone, and it crawls
 *   ST7305_MONO_ATKINSON   error diffusion with contrast; also crawls
 *
 * MEASURED ON HARDWARE 2026-08-30. The six ordered modes are all within 15% of
 * each other, ~2.9 ms a frame. The two diffusion ones cost ~36 ms and malloc
 * 4 KB while selected - about 12x. None of that is paid by the emulator, which
 * holds 60 fps in every mode because osd.cpp dithers on core 0; what it costs
 * is the rate at which finished pictures reach the glass, `pushed=` in the log,
 * which halves from 18/s to 9/s. So the real choice here is how fresh the
 * picture is against how good it looks, and only the glass can settle it. */
#define NES_MONO_MODE ST7305_MONO_BAYER2

/* 1 = the reduction runs on its own task on CORE 0; 0 = inline on the emulator
 * thread, which is where it used to be.
 *
 * WHAT IT BUYS. Dithering is a fixed cost per SHOWN frame, and paid inline it
 * comes straight out of the emulator's budget. Measured against the 18 Hz
 * panel: an ordered mode is ~2.9 ms, 5% of the core, and the emulator holds 61
 * fps either way. Atkinson is ~45 ms, 66% of the core, and inline that drops
 * the emulator to 26 fps. On core 0 it stays at 60 in every mode - only the
 * rate at which finished pictures reach the glass changes, `pushed=` in the
 * log. Core 0 is otherwise almost idle here; the BLE keyboard is the only other
 * task on it and sits at a higher priority, so a keypress still comes first.
 *
 * WHAT IT COSTS. Two more NES frame buffers, 122 KB of PSRAM, because the PPU
 * must have somewhere to write that the dither task is not reading - without
 * that the two would mix and lay a moving seam across the picture. Three in
 * total, so that neither side ever waits for the other; see the long note above
 * nes_present_frame() in osd.cpp for why two was not enough.
 *
 * TURN IT OFF to compare, or if the extra PSRAM is ever wanted elsewhere. The
 * inline path is unchanged and still correct, just slower for the emulator. The
 * `on=` field in the serial log says which one is actually running, and reads
 * `emu` both when this is 0 and when the buffers or the task could not be
 * created at boot. */
#define NES_DITHER_ON_CORE0 1

/* Tone curve, applied to every palette entry before it meets a threshold.
 * 100/100/0 is an exact identity and produces the picture this sketch produced
 * before any of this existed.
 *
 * Gamma is the one worth moving. A reflective LCD has perhaps 5:1 of real
 * contrast in room light against a CRT's 100:1, so a NES palette mapped
 * linearly puts far too much of the picture into the bottom of the range and a
 * dark game comes out as texture on black. Above 100 lifts the midtones.
 *
 * Brightness is also the level knob for ST7305_MONO_THRESHOLD, and it is on
 * SELECT + UP / SELECT + DOWN at runtime. Contrast deliberately does nothing in
 * that mode - see setContrast() in st7305_tft.h. */
#define NES_MONO_GAMMA      100     /* 25..400, 100 = linear          */
#define NES_MONO_CONTRAST   100     /* 25..400, 100 = unchanged       */
#define NES_MONO_BRIGHTNESS 0       /* -128..127, added last          */

/* Draw a small readout in a white box in the bottom-left corner: emulated
 * frames per second, the panel's own measured scan rate, and the four-letter
 * name of the reduction above. It sits on top of the picture - the game now
 * fills the whole panel, so there is no margin left to put it in.
 *
 * The panel figure is counted TE edges per second, so it is measured and not
 * restated from NES_PANEL_FPS. Nothing else on the device will tell you that
 * TE pacing has silently fallen back to a timer; a zero here will. */
#define NES_SHOW_FPS 1

/* WHY A GAME THAT LOOKS ALIVE HAS STOPPED.
 *
 * A NES game can wedge without the emulator noticing anything: the 6502 keeps
 * executing, frames keep being drawn and pushed, the input chords keep working,
 * and the picture simply never changes again. From outside that is identical to
 * a healthy emulator showing a still screen, which is how it ends up blamed on
 * the panel, the card or the PSRAM.
 *
 * Set to 1 and the periodic log line grows a tail that names the real state:
 *
 *   spr0=  p2002=   sprite 0 hits against $2002 reads. A game polling for a
 *                   raster split it will never get shows zero hits against tens
 *                   of thousands of reads a second - that pair alone separates
 *                   "waiting" from "running"
 *   ppu=            whether the background and sprites are switched on at all
 *   scan= opaque=   how often sprite 0 is drawn, and how often it has an opaque
 *                   pixel, which says whether it is even on screen
 *   s0= col= bg=    where sprite 0 is, which of its columns are solid, and which
 *                   of the background columns under it are - an AND of zero
 *                   between the last two is a hit that can never happen
 *   vaddr= xofs=    scroll position and fine horizontal offset
 *   tail= pat=      the tile indices at the right edge and the pattern bytes
 *                   behind them, for a background that comes back blank
 *   chr= top= nt=   CHR RAM write traffic, its high-water mark, and which
 *                   one-screen nametable is mapped
 *
 * It also traces writes to two fixed cells - the pattern of tile $FF and the
 * nametable byte at $209F - which is what the last hunt through this needed.
 * Move those addresses to wherever the next one leads.
 *
 * OFF BY DEFAULT AND WORTH KEEPING THAT WAY. The counters sit in the PPU's
 * hottest paths: one runs on every $2002 read, another 33 times a scanline, and
 * the traces compare an address on every VRAM write. None of it is expensive on
 * its own and all of it is pure cost when nothing is wrong.
 *
 * Two things are NOT behind this flag, because they cost nothing and answer the
 * first question anyone asks: the mapper and geometry line the ROM loader
 * prints, and the warning nes6502.c emits when the CPU jams on an illegal
 * opcode - which is otherwise completely silent. */
#define NES_PPU_DIAGNOSTICS 0

/* WHEN A SCANLINE IS DRAWN RELATIVE TO THE CPU THAT SETS IT UP.
 *
 * nofrendo resolves a whole scanline against one snapshot of PPU state and then
 * runs that scanline's 113 CPU cycles as a single block. Which of the two comes
 * first decides where a mid-frame write lands:
 *
 *   0  draw first, then run the CPU. A write made during scanline N - a scroll
 *      change, a nametable switch - shows up from line N+1. This is what the
 *      emulator has always done.
 *   1  run the CPU first, then draw. The same write shows up on line N itself.
 *
 * Neither is what the hardware does, which is to interleave them pixel by
 * pixel; they are the two ways of being wrong by one line, in opposite
 * directions. Games that change nothing mid-frame cannot tell the difference.
 * Games built on a raster split can, and Battletoads is one: it switches the
 * whole background between the two one-screen nametables twice a frame and
 * times the changeover off a sprite 0 hit, which it aims at two background
 * pixels it paints at x=254. Measured here, its target lands about three
 * scanlines away from the sprite meant to strike it.
 *
 * VBlank is deliberately not moved - only the visible lines. Scanline 241 sets
 * the VBlank flag and 261 clears it, and the NMI is checked between them, so
 * shifting those would break interrupt timing in every game to fix a raster
 * artefact in one.
 *
 * TRIED AT 1 AND IT CHANGED NOTHING for Battletoads - the frozen state came
 * back byte for byte identical, so the error there is not one scanline in
 * either direction. Left at 0, the order the emulator has always used, because
 * the setting still shifts the timing every other game sees and carrying an
 * untested change for no gain is a bad trade. Kept rather than deleted: it is
 * one edit away if another game ever wants the other side. */
#define NES_RENDER_AFTER_CPU 0

/* ===== where the ROM comes from ============================================
 * 1 = built into the binary from nes_rom_data.h. No card, no browser: the
 *     sketch boots straight into that one game. Regenerate the header with
 *     tools/embed_rom.py to change which game.
 * 0 = read from the TF card, with the ROM browser.
 *
 * Embedding costs the ROM's size in flash (256 KB here) and about 1.4 MB of
 * generated C source, so check your partition scheme still fits the app. It
 * costs nothing in RAM - the array stays in memory-mapped flash and nes_rom.c
 * copies PRG and CHR into PSRAM as it always does. */
#define NES_EMBEDDED_ROM 0

/* ===== TF card =============================================================
 * The slot on this board is wired to the SDMMC peripheral, NOT to SPI, so the
 * upstream SD.h + SPIClass path cannot reach it. One data line only.
 * Ignored when NES_EMBEDDED_ROM is 1. */
#define NES_USE_SD_MMC 1
#define SDMMC_CLK 38
#define SDMMC_CMD 21
#define SDMMC_D0  39

/* ===== sound ===============================================================
 * ON. The ES8311 register sequence this board needs now exists in
 * nes_codec.cpp, so the emulator's MAX98357A-style "just push I2S samples" path
 * has something to push into. Three things have to be right together or the
 * result is silence with no error anywhere - the codec registers, MCLK, and the
 * amplifier enable below. See nes_codec.h.
 *
 * Set back to 0 to get the silent build: the sketch then passes -nosound, the
 * APU is never asked for samples, and none of this is compiled in. Worth trying
 * if the frame rate readout drops - APU emulation is not free. */
#define ENABLE_SOUND 1

/* 1 = drive the ES8311 over I2C before streaming. There is no reason to turn
 * this off on this board; it exists so the emulator's audio path still builds
 * for a plain amplifier that needs no setup. */
#define NES_USE_ES8311 1

#define I2S_BCK   9   /* ES8311 BCLK  */
#define I2S_WS   45   /* ES8311 LRCLK */
#define I2S_DO    8   /* ES8311 DIN   */
#define I2S_MCLK 16   /* required - the codec has no internal oscillator here */
#define I2C_SDA  13
#define I2C_SCL  14

/* Power amplifier enable. Nothing reaches the speaker while this is low, no
 * matter how correct the codec and the I2S stream are.
 *
 * This pin is not in the ESPHome YAMLs the rest of this file came from - it is
 * in the vendor's codec board table, 02_Example/Arduino/07_Audio_Test/src/
 * ExternLib/codec_board/board_cfg.h, entry "Board: S3_RLCD_4_2", field "pa: 46".
 * Which is why GPIO 46 shows up in the not-free list below without a name. */
#define I2S_PA_EN 46

/* DAC output level, 0-100, applied once at startup. Separate from the in-game
 * volume menu, which scales samples in software before they reach I2S. Turn
 * this down only if the speaker distorts at full software volume. */
#define NES_CODEC_VOLUME 85

/* ===== gamepad =============================================================
 * Eight free GPIOs, each expecting a button to GND (internal pull-ups are used,
 * so no external resistors). None of these is a boot strapping pin that cares
 * about being pulled low at reset.
 *
 *   GPIO 4, 16, 19, 20, 33-37, 43, 44, 46 are NOT free on this board:
 *   battery ADC, ES8311 MCLK, USB D-/D+, octal PSRAM, UART, strapping, and 46
 *   is the audio amplifier enable (I2S_PA_EN above). */
#define BTN_UP     1
#define BTN_DOWN   2
#define BTN_LEFT   3
#define BTN_RIGHT  7
#define BTN_A     15
#define BTN_B     17
#define BTN_START 42
#define BTN_SELECT 47

/* The two buttons the board already has, so the example does something before
 * you solder anything. BOOT doubles as A, KEY doubles as START.
 *
 * That pairing is what makes the menus usable on a bare board: they treat START
 * as "next entry" and wrap, so KEY walks the list and BOOT confirms. Two
 * buttons, everything reachable - which matters most for the Bluetooth keyboard
 * picker, since by definition no keyboard is connected while it is on screen.
 *
 * Holding BOOT at reset still enters the USB download mode - that is the pin's
 * other job and this does not change it. Press it after boot, not during. */
#define NES_USE_ONBOARD_BUTTONS 1
#define BTN_ONBOARD_BOOT  0   /* -> A     */
#define BTN_ONBOARD_KEY  18   /* -> START */

/* ===== BLE keyboard =========================================================
 * A Bluetooth keyboard as the gamepad, over HID-over-GATT. Needs the
 * NimBLE-Arduino library (2.x) from the Library Manager - and nothing else: it
 * runs on the stock ESP32 core, unlike Bluepad32, which would require a forked
 * board package and does not expose keyboards through its Arduino API anyway.
 *
 * BLE only. That is a property of this chip, not of the library: the ESP32-S3
 * has no Bluetooth Classic radio, so a BR/EDR keyboard or gamepad (every
 * 8BitDo mode, DualShock, Switch Pro) cannot be seen at all. ZMK builds are
 * BLE by construction, so they are fine.
 *
 * If the keyboard runs ZMK: leave it on the default HKRO report. Setting
 * CONFIG_ZMK_HID_REPORT_TYPE_NKRO=y swaps the six key slots for a usage
 * bitmap, which the boot protocol this uses cannot express. Six keys at once
 * is already more than any NES game asks for. */
#define NES_BLE_KEYBOARD 1

/* Dumps the HID service's characteristics on connect and hex-prints the first
 * few reports. Worth leaving on: the failure modes here (subscribed to the
 * wrong characteristic, keyboard sending to a different host) all look
 * identical from outside - "paired" and then nothing. */
#define NES_BLE_DEBUG 1

/* Connect only to a keyboard whose advertised name contains this. Empty string
 * = take the first HID device that answers, which is fine on a quiet desk and
 * wrong everywhere else: a mouse, a TV remote and somebody else's keyboard all
 * advertise the same HID service, and BLE private addresses rotate, so "it
 * connected to something" is not evidence it connected to yours.
 *
 * Matching is case-insensitive and by substring, because the name in an
 * advertisement is often truncated - only about 29 bytes fit. */
#define NES_BLE_KEYBOARD_NAME "Sofle"

/* How long to wait at boot for the keyboard before starting the game anyway.
 *
 * Generous on purpose. One scan alone is 5 s, and a first-time pairing adds
 * the encryption handshake and service discovery on top - so a short timeout
 * gives up while the thing is still working and looks like it never tried.
 * Nothing is lost by waiting: the game does not need the keyboard to start, and
 * the keyboard keeps connecting in the background if you skip.
 *
 * Press any button to skip. 0 disables the wait entirely. */
#define NES_BLE_WAIT_MS 60000

/* HID usage codes, Usage Page 0x07 - what the keyboard actually sends, not
 * ASCII. Letters run a=0x04..z=0x1D, digits 1=0x1E..9=0x26 then 0=0x27.
 * The arrow keys are hard-wired as aliases for the d-pad in ble_keyboard.cpp. */
#define NES_KEY_UP     0x1A   /* W */
#define NES_KEY_DOWN   0x16   /* S */
#define NES_KEY_LEFT   0x04   /* A */
#define NES_KEY_RIGHT  0x07   /* D */
#define NES_KEY_A      0x09   /* F */
#define NES_KEY_B      0x0A   /* G */
/* 5 is START and 4 is SELECT, which is the other way round from the digits'
 * own order on purpose - it puts SELECT next to the letter keys, where the
 * thumb already is for the SELECT + something chords, and leaves START on the
 * outside where a game only reaches for it between lives. Changing these two
 * lines is the whole of the keyboard remap; nothing else reads the codes. */
#define NES_KEY_START  0x22   /* 5 */
#define NES_KEY_SELECT 0x21   /* 4 */

/* Play over the USB serial console when no buttons are wired at all:
 *   W A S D = d-pad, K = A, J = B, Enter = START, Space = SELECT.
 * A key stays "held" for NES_SERIAL_HOLD_MS after its character arrives, which
 * is what makes autorepeat from a terminal feel like a held button. */
#define NES_SERIAL_CONTROL  1
#define NES_SERIAL_HOLD_MS 90

#endif /* HW_CONFIG_H */
