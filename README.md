# waveshare_rlcd_nes

A NES emulator for the **Waveshare ESP32-S3-RLCD-4.2**, running on its 400×300 1-bit reflective LCD.

Reflective panels have no backlight — they are readable in direct sunlight and draw almost nothing — but they are black and white, and they are slow. This sketch takes Nofrendo's 256×240 colour output, reduces it to ink and paper through your choice of eight dither modes, stretches it to the panel's real 4∶3 geometry, and phase-locks the write to the panel's own scan so there is no tearing. The emulator itself runs at a full 60 fps; the panel shows as many of those frames as it can.

Everything needed is in this repository. Open `waveshare_rlcd_nes.ino` in the Arduino IDE and press Upload.

> **Status.** Verified on hardware on 2026-08-30: Super Mario Bros at 60 emulated fps, `dither=2850us push=6579us emul_fps=60`. Since that measurement the video path changed — flushes moved from a timer to the panel's TE pin — and **that change has not been re-verified on hardware.** The host tests pass; the board has not been reflashed. If `te_hz=0` appears in the serial log, that lock was never acquired; see [Troubleshooting](#tearing-or-te_hz0).

## Features

- **Full-speed emulation.** 60 fps of Nofrendo, independent of how fast the panel can show it. Dithering runs on the second core, so the reduction you pick changes how often the picture refreshes, never how fast the game runs.
- **Eight black-and-white reductions**, switchable while playing: four ordered dithers, a clustered halftone, a blue-noise mask, plain thresholding, and Floyd–Steinberg and Atkinson error diffusion.
- **No tearing.** The flush is driven by the panel's TE pin rather than a timer.
- **Correct geometry.** NES pixels were never square; the picture fills all 400×300 at the aspect ratio the games were drawn for.
- **Sound**, through the board's ES8311 codec.
- **Four input sources at once** — soldered buttons, the board's own two buttons, a Bluetooth keyboard, or a USB-serial pad. None is required.
- **Games from a TF card or built into the binary.** With a ROM embedded the board boots straight into it and needs no card at all.
- **34 mappers**, including MMC1 and MMC3.

## Quick start

### What you need

| | |
|---|---|
| Board | Waveshare ESP32-S3-RLCD-4.2 (N16R8) |
| Library | `NimBLE-Arduino` 2.x — **only** if you want the Bluetooth keyboard |
| ROM | built in — see [below](#getting-a-game-on-it). No card needed as shipped |
| Buttons | optional. The board's own BOOT and KEY buttons are enough to navigate |

**There is no emulator library to install.** The Nofrendo core lives in [src/nofrendo/](src/nofrendo); Arduino compiles a sketch's `src/` subfolder recursively, so it builds with the sketch. It is a fork with ST7305 support added — an installed copy of the upstream library will not build this sketch, and should be removed if you have one.

### Arduino IDE settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| PSRAM | **OPI PSRAM** — required, the ROM lives there |
| Flash Size | 16MB (128Mb) |
| Partition Scheme | any with ≥ 3 MB app |
| USB CDC On Boot | Enabled — for the serial pad and the log |

Then Upload. On first boot the board scans for a Bluetooth keyboard (skip with any button), then starts the built-in game.

Watch the serial log at **115200** — it narrates everything, and every troubleshooting section below is written around one line of it.

## Getting a game on it

`NES_EMBEDDED_ROM` in [hw_config.h](hw_config.h) picks between two routes.

### Built into the binary

The default. The game is compiled in and the board boots straight into it, with no card and no browser. To change the game:

```sh
python3 tools/embed_rom.py SomeOtherGame.nes   # regenerates nes_rom_data.h
```

The array is `const`, so it stays in memory-mapped flash and costs **no RAM** — 256 KB of the app partition and about 1.4 MB of generated C source, nothing more.

### From a TF card

Set `NES_EMBEDDED_ROM` to **0** and put `.nes` files in the **root** of the card. Files in folders are not found.

The card must be **FAT32**. ESP-IDF builds FatFs with exFAT off and exposes no switch for it, so an exFAT card simply fails to mount and the browser shows an empty list — which looks like a dead card rather than a wrong format. Both Disk Utility and the official SD Card Formatter default to exFAT above 32 GB, so on a large card force it:

```sh
diskutil eraseDisk "MS-DOS FAT32" NES MBRFormat /dev/diskN   # check diskN twice
```

`MBRFormat` matters — with GPT neither card readers nor the ESP32 will see it.

### Swapping the card

**The card is read once per boot, not once per menu.** Backing out of a game returns to the list instantly instead of rereading a card that has not moved — on a 551-entry card that scan took nearly 18 seconds.

The cost is that a card swapped while the board is running is not noticed on its own: this board has no card-detect pin, so there is nothing to notice it with. **SELECT + B** in the browser rereads the card, and the footer says so. The same chord works on the `NO .NES FILES ON THE CARD` screen, which is how you recover from booting with no card in the slot.

That chord does a full remount, not just another directory read. FatFs caches FAT and directory sectors, so scanning a new card over the old mount returns the *old* card's listing — or a mix of the two, which is the worse failure because it looks plausible until a game refuses to open. A failed remount leaves the existing list alone and says `CARD NOT FOUND`.

## Controls

Four input sources are OR'd together and none has to be present, so any of them works on its own and two work at once. All of them are in [controller.cpp](controller.cpp).

| Pad button | Soldered GPIO | Board button | BLE keyboard | Serial |
|---|---|---|---|---|
| UP | 1 | | `W` (or ↑) | `W` |
| DOWN | 2 | | `S` (or ↓) | `S` |
| LEFT | 3 | | `A` (or ←) | `A` |
| RIGHT | 7 | | `D` (or →) | `D` |
| A | 15 | **BOOT** | `F` | `K` |
| B | 17 | | `G` | `J` |
| START | 42 | **KEY** | `5` | Enter |
| SELECT | 47 | | `4` | Space |

Buttons go to GND; internal pull-ups are used, so no resistors. The GPIOs are free pins picked for you to solder to, not a board feature — change them in [hw_config.h](hw_config.h) to match what you wire.

**BOOT is A and KEY is START** so that the board does something before anything is soldered. That pairing is what makes the menus usable bare: they treat START as "next entry" and wrap, so KEY walks the list and BOOT confirms — which matters most in the Bluetooth keyboard picker, since by definition no keyboard is connected while it is on screen.

For the serial pad use [tools/serial_pad.py](tools/serial_pad.py) rather than the Arduino Serial Monitor, which waits for Enter and so turns a held direction into one 90 ms tap.

### Chords

Everything the emulator itself offers is on SELECT plus one other button. SELECT is the least-used button on a NES pad, and holding it costs a running game nothing: **every chord below is swallowed**, so the game never sees the SELECT, nor the button it was combined with.

| Chord | What it does |
|---|---|
| **SELECT + A** | Next black-and-white reduction. Cycles all eight and wraps — see [below](#eight-ways-to-become-black-and-white). The four-letter name in the corner readout says which one you are on |
| **SELECT + B** | Swap ink and paper. Most NES games have dark backgrounds and read far better inverted on a reflective panel |
| **SELECT + UP** | Brighter, by 8 of 255 |
| **SELECT + DOWN** | Darker, by 8 |
| **SELECT + START**, held ~0.75 s | Quit to the ROM list |

On the BLE keyboard that reads as `4+F`, `4+G`, `4+W`, `4+S` and `4+5`; the keyboard-wait screen lists them, which is the only place they are discoverable without reading this.

In the **ROM browser** — not in a game — SELECT + B instead [rereads the card](#swapping-the-card). The emulator's frame loop is not running there, so the two uses never collide.

The first four are edge-triggered, so a held chord fires once. **SELECT + START is a hold and not a tap**, unlike the rest, because SELECT and START together is the soft reset on a real NES and several games bind it themselves. Three quarters of a second is longer than any of that.

Brightness is also the level knob for the `THRS` reduction, which has no dither to spread a tone change across and so shows it most plainly. Contrast deliberately does nothing in that mode — it pivots on mid grey and mid grey is that mode's only threshold. Gamma and contrast have no chord; they are `NES_MONO_GAMMA` and `NES_MONO_CONTRAST` in [hw_config.h](hw_config.h).

### Leaving a game

**SELECT + START, held.** The emulator stops at the end of the frame, the video path is torn down in order — dither task drained, panel handed back by the TE task, 60 Hz tick stopped, then the ROM and the machine freed — and you land back in the ROM browser with the cursor on the game you just left.

There is no way out with `NES_EMBEDDED_ROM` set to 1, because there is no list to go back to; the chord still stops the emulator and the sketch simply restarts the same ROM.

The serial log narrates it, and the heap figures are there to be watched across several games — if they fall each time, something is not being released:

```
[EXIT] SELECT+START held - leaving the game
[EXIT] emulator returned 0 - tearing down
[EXIT] free heap 8458544, PSRAM 8322340
[MENU] 271 ROM(s) held from the last scan - SELECT+B rereads the card
```

That last line is the point of [the card being read once](#swapping-the-card): the trip back costs nothing. `[MENU] scanning card...` in its place means this was the first menu since boot.

## Bluetooth keyboard

`NES_BLE_KEYBOARD` (on by default) makes the board a BLE central and talks HID-over-GATT to any BLE keyboard. `W A S D` d-pad (arrows work too), **F** = A, **G** = B, **5** = START, **4** = SELECT. Needs **NimBLE-Arduino 2.x** from the Library Manager — and nothing else; it runs on the stock ESP32 core.

Unlike the serial pad this gets real key-down and key-up, so a held direction is genuinely held and the 90 ms latch hack is not needed.

**BLE only, and that is the chip, not the library.** The ESP32-S3 has no Bluetooth Classic radio, so a BR/EDR device cannot be seen at all — that rules out every 8BitDo mode, DualShock/DualSense and the Switch Pro controller. ZMK keyboards are BLE by construction and are fine.

If your keyboard runs **ZMK**: leave it on the default HKRO report. Setting `CONFIG_ZMK_HID_REPORT_TYPE_NKRO=y` swaps the six key slots for a usage bitmap, which the boot protocol used here cannot express. Six simultaneous keys is already more than any NES game asks for — the most is five.

### Choosing the keyboard

**Every boot** the board scans and shows every BLE HID device it can see, with name, address and whether it is already paired. The previous choice is marked `*` and the cursor starts on it, so the usual case is one button press — and there is no state you can get into that needs a reflash to leave.

Navigate with the board's **own two buttons**: **KEY** moves to the next entry (wrapping), **BOOT** selects. That combination is deliberate — while this menu is on screen there is by definition no Bluetooth keyboard, external buttons are optional, and serial means being tethered to a computer, so the two soldered-on buttons are the only input guaranteed to exist. A d-pad works too: UP/DOWN and A. Both menus in this sketch use the same scheme, which is why START moves the cursor rather than confirming.

The menu also offers **FORGET SAVED + ALL PAIRINGS**, which is the recovery path when `pairing failed` starts appearing: it drops the board's stored keys so a fresh pairing can happen. Clear the keyboard's side too (`&bt BT_CLR` on its profile) — a bond only one side still believes in can never re-establish encryption, and there is no way to ask a BLE peer to forget you.

The selection is stored **by name, not address**. BLE private addresses rotate: this keyboard was seen under two different addresses in consecutive runs, so an address identifies a moment rather than a device. `NES_BLE_KEYBOARD_NAME` in [hw_config.h](hw_config.h) is only the default for a board that has never been through the menu.

### If it pairs and then no key does anything

This cost an evening, so: **ZMK sends keystrokes only to its ACTIVE profile**, while keeping connections alive to *every* bonded host. So a board can be connected, encrypted, bonded and correctly subscribed, and still receive nothing — the serial log will say `ready (...)` and then `no reports in 5 s`, which is an accurate description of the state and not a fault.

The fix is on the keyboard: `&bt BT_SEL <n>` until reports appear. Find the right `n` empirically — select a profile, press `W`, watch for `[BLE] report 1 from handle N (8 bytes): 00 00 1a 00 00 00 00 00`.

Two traps in the same area:

- **`BT_CLR` clears only the selected profile.** Clearing profile 0 does nothing to a bond living on profile 4. Worse, it leaves profile 0 empty *and active*, so the keyboard starts advertising as pairable again — which reads as "it never paired with the board", when in fact it is bonded on another profile.
- **Bonds survive reflashing the keyboard.** MAC addresses and keys are stored separately from the firmware, so re-flashing ZMK to fix a tangled bond achieves nothing. Use `BT_CLR` / `BT_CLR_ALL`.

Why not Bluepad32, the obvious answer to "Bluetooth controller on an ESP32": its Arduino API does not expose keyboards. `ControllerData`'s union holds only gamepad, mouse and balance board — there is no `isKeyboard()` and no key accessor. Keyboards exist only in its ESP-IDF C layer. It would also mean installing a forked ESP32 core, since it replaces the Bluetooth stack with BTstack.

## Sound

On by default, through the board's ES8311 codec at 16 kHz mono, upmixed to stereo. Volume exists twice: the in-game menu scales samples in software, and `NES_CODEC_VOLUME` sets the DAC once at startup. Use the first; the second is there for a speaker that distorts.

Set `ENABLE_SOUND` to 0 for a silent build — APU emulation is not free, and the frame-rate readout in the corner is the way to see what it costs. If there is no sound at all, see [Troubleshooting](#there-is-no-sound); the wiring it depends on is described under [How the sound path works](#how-the-sound-path-works).

## Troubleshooting

Watch the serial log at 115200 before guessing — every failure below has been mistaken for a bad card at least once, and each is one line apart from the others.

### It is not in the list

The browser skips a file and says why:

```
[MENU] skipped, name over 127 chars: <name>
[MENU] 271 ROM(s) out of 551 entries in 17796 ms
[MENU] LIST FULL at 512 - the rest of the card is not shown
```

The list holds 512 names of 128 bytes in PSRAM. A name that does not fit whole is **skipped rather than truncated**, because the list entry *is* the path handed to `SD_MMC.open()` later — a truncated copy loses the `.nes` and fails to open, which reads as a corrupt card rather than a buffer that was too small. Shorten the filename or raise `NAME_LEN` in [nes_menu.cpp](nes_menu.cpp).

Only the card root is scanned; files in folders are not found.

### It is in the list and refuses to start

```
[ROM] /Some Game.nes: mapper 69, PRG 256K, CHR 128K, vert
rom_load: unsupported mapper 69
GUI: ROM not loaded
[EXIT] emulator returned 1 - tearing down
```

The panel says `COULD NOT LOAD` with the filename and drops back to the list, so this one does not need the serial log — but the log names the mapper, which is the part that decides whether it is worth doing anything about.

34 mappers are compiled in — 0, 1, 2, 3, 4, 5, 7, 8, 9, 11, 15, 16, 18, 19, 21–25, 32–34, 40, 64–66, 70, 75, 78, 79, 85, 94, 99, 231. That includes 1 (MMC1) and 4 (MMC3), which between them carry a large share of the licensed library.

**Ten more are in the source and simply not wired up.** `src/nofrendo/map010.c`, `041`, `042`, `046`, `050`, `073`, `087`, `093`, `160` and `229` are all built but missing from the `mappers[]` table in `src/nofrendo/mmclist.c`. Adding one is two lines — an `extern mapintf_t mapNN_intf;` and an entry in the array. Nobody has checked whether those files work, which is presumably why they are not there.

### It starts, plays, and then stops

The picture freezes, the music stops, and **the emulator carries on perfectly**: 60 fps, the panel still refreshing, the SELECT chords still changing the dither on the frozen image. That combination is the signature — it means the emulated 6502 stopped, not the firmware.

Two kinds of stopped, and one log line separates them:

```
[CPU] JAMMED: illegal opcode $F2 at $C31A - the 6502 stops here
```

If that appears, the game jumped into something that is not code. If it does **not** appear and `pc=` in the periodic line keeps moving, the game is alive and waiting for something that is never going to arrive.

A **sprite 0 hit** is the thing to suspect first, and it is the one case confirmed here. Games split the screen by parking sprite 0 over a known piece of background and spinning on `$2002` until the PPU reports the overlap. This is Battletoads, bank 0, and it is where that game stops:

```
863C: A9 40      LDA #$40        ; bit 6 = sprite 0 hit
863E: 2C 02 20   BIT $2002
8641: F0 FB      BEQ $863E       ; loop while it is clear
```

nofrendo resolves a whole scanline against one snapshot of PPU state and runs that scanline's 113 CPU cycles as a single block. A game that changes scroll, mirroring or bank *part way down a scanline* and times the change off that hit is asking for a resolution this emulator does not have. **Battletoads is the known case**: it switches the whole background between the two one-screen nametables twice a frame and hangs at the start of level 1. Diagnosed down to the pixel and not fixed — see [why the timing cannot simply be made finer](#why-the-timing-cannot-simply-be-made-finer).

### There is no sound

Three things have to be right together, and each one alone produces the same silence with nothing in any log — see [How the sound path works](#how-the-sound-path-works) for what they are.

If the log shows `[CODEC] ES8311 not found`, the codec is not answering on I²C at all and nothing downstream matters.

### Tearing, or `te_hz=0`

A seam that walks down the screen means the flush is no longer locked to the panel's scan. `te_hz=0` in the serial log is that state: the TE pin on GPIO 6 is not pulsing, and the code has fallen back to pushing on a timer. Everything else in the log will look normal. See [why the flush is driven by a pin](#tearing-and-why-the-flush-is-driven-by-a-pin).

### Turning the diagnostics on

`NES_PPU_DIAGNOSTICS` in [hw_config.h](hw_config.h), off by default. It grows the periodic line a tail that names the state, and traces two fixed memory cells:

```
pc=$863E spr0=0/s p2002=256681/s ppu=bg+obj scan=482/s opaque=362/s
s0=y28,x254,t$00,a$40 col=$FF bg=$00 vaddr=$0080 xofs=0 tail=$00FF00
pat=$0000@$1FF0 chr=0/s top=$1FFF nt=1
```

Read it outside in:

| | |
|---|---|
| `spr0=` `p2002=` | hits against `$2002` reads. **Zero hits and tens of thousands of reads a second is the failure above** — the game is asking, the PPU never answers |
| `ppu=` | `OFF` explains a still picture by itself |
| `scan=` `opaque=` | how often sprite 0 is drawn at all, and how often it has an opaque pixel. `scan=0` means it is off screen |
| `col=` `bg=` | which of sprite 0's eight columns are solid, and which of the background columns under it are. **An AND of zero between them is a hit that can never happen** |
| `s0=` | sprite 0's raw OAM entry — `y240` answers a lot on its own |
| `vaddr=` `xofs=` `tail=` `pat=` | scroll, fine offset, the tile indices at the right edge and the pattern bytes behind them, for a background that comes back blank |
| `chr=` `top=` `nt=` | CHR RAM write traffic, its high-water mark, and which one-screen nametable is mapped |

The traces are hard-coded to the addresses the last hunt needed — the pattern of tile `$FF` and the nametable byte at `$209F`. Move them in `src/nofrendo/nes_ppu.c` to wherever the next one leads.

It is off by default because the counters sit in the PPU's hottest paths: one runs on every `$2002` read, another 33 times per scanline, and the traces compare an address on every VRAM write.

Two things are **not** behind the flag, because they cost nothing and answer the first question anyone asks: the `[ROM] … mapper N` line, and the `JAMMED` warning — a jammed 6502 was otherwise completely silent.

## Configuration

Every pin and switch is in [hw_config.h](hw_config.h), which is read by the sketch and by the emulator core alike. The ones worth knowing:

| | |
|---|---|
| `NES_EMBEDDED_ROM` | 1 = play the ROM built into the binary, 0 = browse a TF card |
| `NES_PANEL_FPS` | panel scan rate. Every rung up to 23.22 Hz is clean; above ~26 Hz the blacks wash out |
| `NES_MONO_MODE` | which of the eight reductions to start on |
| `NES_MONO_GAMMA` / `NES_MONO_CONTRAST` | tone before the reduction. No chord for these |
| `NES_DITHER_ON_CORE0` | dither on the second core. On by default; costs 122 KB of PSRAM |
| `NES_BLE_KEYBOARD` | the Bluetooth keyboard client, and the picker at boot |
| `ENABLE_SOUND` | the APU and the codec |
| `NES_CODEC_VOLUME` | DAC volume, set once at startup |
| `NES_PPU_DIAGNOSTICS` | the PPU counters described above. Costs real time in hot paths |
| `NES_RENDER_AFTER_CPU` | draw-then-CPU (0) or CPU-then-draw (1) within a scanline |
| `SDMMC_CLK` / `_CMD` / `_D0` | the TF slot. SDMMC, one data line — **not** SPI |

---

# How it works

Everything below is internals. Nothing here is needed to play a game.

## The picture

The panel is 400×300, one bit per pixel, and scans at whatever `NES_PANEL_FPS` selects — that one line is the only place the rate is written down, and everything below follows it rather than restating it. Every rung up to 23.22 Hz is clean; above roughly 26 Hz the blacks wash out, so the ceiling is a contrast limit, not a bandwidth one. Sitting below the ceiling is a judgement about motion rather than a compromise: the liquid crystal takes tens of milliseconds to settle, so a faster scan starts the next frame before the last one has finished arriving and fast motion greys out instead of sharpening.

**The NES image is stretched to fill the panel, and that is the correct geometry.** NES pixels were never square. The PPU renders 256×240 at a dot clock that made each pixel about 8∶7 as wide as it was tall, and an NTSC television showed only the middle 224 rows — so what a player saw was 256×224 of wide pixels filling a 4∶3 screen. This panel is 400×300, which is exactly 4∶3, so `blitIndexedStretched()` drops the 8 overscan rows top and bottom and scales what is left across all 400×300. That lands within about 2% of the original aspect ratio; the old centred 1∶1 blit was 22% too tall.

Nearest-neighbour, because anything else would cost per-pixel arithmetic the frame budget does not have. It bands far less than it sounds like it should: the dither threshold is picked by *destination* x and y, so a source pixel that lands in two adjacent columns is thresholded twice differently and reads as texture instead of as one fat pixel. The cost is two array lookups per pixel — 120000 table reads against the old 61440, roughly 2× the dither time.

The consequence is that there is no margin left. Every one of the 15000 GRAM bytes is rewritten each frame, so the frame-rate readout has to be drawn on top of the picture after the blit, every frame — it lives in a small white square in the bottom-left corner. The filename banner the sketch used to leave on screen is now only a loading splash.

**Most frames are dropped.** The panel takes one frame per scan and the emulator makes one every 16.7 ms, so most of them never leave the MCU. The emulator still runs at 60 fps — game logic, timing and input are untouched, only the trip to the glass is rate-limited. `dropped=` climbing fast is correct behaviour, not a fault. Dropped frames are also cheap: the emulator hands the finished frame over and returns, so one nobody will see costs a pointer swap.

## Eight ways to become black and white

**SELECT + A** cycles them; `NES_MONO_MODE` in [hw_config.h](hw_config.h) picks which one you start on. The four-letter name is what the corner readout shows. `st7305_mono_t` in [st7305_tft.h](st7305_tft.h) has the long version.

| | | |
|---|---|---|
| `BAY4` | Bayer 4×4, 16 levels | the original and still the default; a fine diagonal cross-hatch |
| `BAY8` | Bayer 8×8, 64 levels | smoother gradients, a NES sky stops banding; coarser weave in flat areas |
| `BAY2` | Bayer 2×2, 4 levels | almost no dither texture and much more contrast; flattens shading into blocks |
| `CLUS` | clustered-dot 4×4 | a newsprint halftone. Grows dots from a centre instead of scattering, which on a panel whose pixels bleed often reads cleaner than dispersed noise |
| `BLUE` | blue noise 8×8 | void-and-cluster mask, same cost as `BAY8`. Grain instead of a weave — no cross-hatch at all |
| `THRS` | no dither | every shade collapses, so pictures lose their modelling, but text and hard-edged sprite art come out perfectly crisp |
| `FLOY` | Floyd–Steinberg | the best tonal accuracy by a wide margin and no repeating texture whatsoever |
| `ATKN` | Atkinson | as `FLOY` but passing on only 6/8 of each error: more contrast, flatter flat areas, the classic 1-bit look |

**The default is ordered on purpose.** An error-diffused dither reshuffles the whole row when one pixel changes, so a still background crawls every frame, and on a panel with tens of milliseconds of LC response that crawl is the most visible thing on screen. An ordered cell depends only on (x, y), so anything that did not change stays put. `FLOY` and `ATKN` are here anyway because on a mostly-still screen they are beautiful and the argument is settled by looking.

They also cost about 12× an ordered mode — ~36 ms a frame against ~2.9 ms, measured on the board. **None of that comes out of the emulator**, which holds 60 fps in every mode because the dithering runs on core 0 ([below](#dithering-runs-on-the-other-core)); what it costs is how often a finished picture reaches the glass, `pushed=` in the log, which halves from 18/s to 9/s.

`BLUE` is 8×8 and not 4×4 for a reason: a 4×4 cell holds sixteen ranks and the maximally dispersed arrangement of sixteen cells *is* the Bayer matrix, so a 4×4 "blue noise" mask can only be worse than Bayer, not different-and-better. Blue noise needs room.

## Dithering runs on the other core

`NES_DITHER_ON_CORE0`, on by default. Reducing 400×300 to one bit is a fixed cost per *shown* frame, and paid on the emulator's thread it made the game's frame rate hostage to which reduction was selected. Measured on the board against the 18 Hz panel:

| | dither | share of the core | emulated fps |
|---|---|---|---|
| an ordered mode | ~2.9 ms | 5% | 61 |
| Atkinson, inline | ~45 ms | 66% | **26** |
| Atkinson, on core 0 | ~45 ms | 0% of core 1 | **60** |

Core 0 is otherwise almost idle here — only the BLE keyboard task, and at a higher priority, so a keypress still preempts a frame. Core 1 keeps the emulator and the TE flush task.

It costs **two more NES frame buffers, 122 KB of PSRAM**. The PPU writes its 8-bit frame all through the emulated frame, so a dither pass reading that same buffer from another core would mix two frames and lay a moving seam across the picture — the artefact TE pacing exists to remove, reintroduced one layer up. Three buffers rather than two, so neither side ever waits for the other: with two, the hand-over could only happen at an emulated frame boundary, and the 16.7 ms of waiting that added to Atkinson's 45 ms pushed the cycle past the panel's period and halved the push rate.

What remains mode-dependent is `pushed=` — how often a finished picture reaches the glass. 18/s for the ordered modes, 9/s for the two diffusion ones. The game is not slower; the picture is less fresh.

The `on=` field in the serial log says which path is live, and reads `emu` both when the flag is 0 and when the buffers or the task could not be created.

<a name="tearing-and-why-the-flush-is-driven-by-a-pin"></a>
## Tearing, and why the flush is driven by a pin

GRAM is single-buffered and the panel scans it on its own clock. A flush writes all 15000 bytes in 6.5 ms while a scan takes tens of ms, so an unpaced flush always crosses the scan line: the rows above the crossing keep showing the old frame for the rest of that scan and the rows below show the new one. That seam is on *every* unpaced frame, and it walks down the screen as the two periods beat against each other — which is what makes it visible. Nothing on the MCU side fixes it. Double buffering in ESP32 RAM does not help either: the race is between the SPI write and the panel's own scan, inside the controller, and the ST7305 has one GRAM and no page flip.

What does fix it is starting the write at the moment the scan restarts. The write is several times faster than the scan — six to eight, depending on the rung — so from there it stays ahead of the beam all the way down and the beam only ever reads new bytes. That margin is why none of this depends on which rung `NES_PANEL_FPS` picks: it holds at every one of them, and by more the slower the panel runs. The panel says when that moment is: TE (`0x35`, sent by `st7305_init`) pulses once per frame on GPIO 6. So the flush runs on its own task woken by that pin — and *that* is what needs the second framebuffer, so the emulator has somewhere to draw during the 6.5 ms the first one is on the wire. See `startTePacing()` in [st7305_tft.h](st7305_tft.h); the buffer hand-off is asserted in [test/te_pacing_test.cpp](test/te_pacing_test.cpp).

It costs 15 KB of DRAM and up to one scan period of latency — a frame finished just after an edge waits for the next one, so what reaches the glass is most of a scan period old. That scales with the rate and stays inside this panel's own LC response at every rung.

If TE ever goes quiet the task says so once on serial and falls back to pushing on a `NES_FRAME_INTERVAL_MS` timer, so a board that does not wire the pin degrades to the old tearing rather than to a frozen picture. `te_hz=0` in the serial log is that state.

## The blit

It is fast because it is written to the panel's real memory layout. One GRAM byte covers 2 columns × 4 rows in landscape, so the centring offset is rounded to a multiple of 4 in both axes and every destination byte is then a whole byte built from 8 source pixels — no read-modify-write, no clipping test, no dither-phase arithmetic in the loop. Dithering and palette luminance are baked into a 4 KB table, so the inner loop is eight table reads and seven ORs per byte: 61 440 lookups producing 7 680 stores.

Expected per shown frame: ~2 ms of dithering plus ~6.5 ms of SPI, against a 59 ms budget. Measured `dither=`/`push=` numbers print to serial every 3 s.

## How the sound path works

The emulator's audio path was written for a MAX98357A — a class-D amplifier with no control interface, where "push I²S samples" is the whole driver. An ES8311 is a chip with registers, and it holds its DAC in reset until they are written, so that path alone produces silence. [nes_codec.cpp](nes_codec.cpp) writes them: a reduced port of Espressif's `es8311.c`, playback only, over Arduino `Wire`.

Three things have to be right together, and each one alone produces the same silence with nothing in any log:

| | |
|---|---|
| Codec registers | `nes_codec_begin()`, ported from the vendor's own copy of `esp_codec_dev` — see their `07_Audio_Test` Arduino example |
| MCLK on GPIO 16 | at 256× the sample rate. The board config says `use_mclk: 1` — there is no internal-oscillator fallback. `osd_init_sound()` asks I²S for it |
| Amplifier enable on GPIO 46 | `I2S_PA_EN`. Nothing reaches the speaker while it is low |

That third pin is the one worth knowing about: it is **not** in the ESPHome YAMLs the rest of [hw_config.h](hw_config.h) was built from. It is in the vendor's codec board table, entry `Board: S3_RLCD_4_2`, field `pa: 46`.

Serviced once per emulated frame — including the ones the panel never shows, because the APU's buffer has to keep draining whatever the display is doing.

## Why the timing cannot simply be made finer

`NES_RENDER_AFTER_CPU` is the one cheap knob, and it ships at **0**. It swaps which comes first within a scanline:

- **0** — draw, then run the CPU. A write made during line N shows up on line N+1.
- **1** — run the CPU, then draw. The same write shows up on line N.

Neither is what the hardware does, which is to interleave them pixel by pixel; they are the two ways of being wrong by one line, in opposite directions. Games that change nothing mid-frame cannot tell. It was tried at 1 for Battletoads and changed nothing, so it is back at 0 — the setting still shifts the timing every other game sees, and carrying an untested change for no gain is a bad trade.

Going properly finer is not a patch. A NES frame is 341 dots × 262 lines = **89 342 PPU dots**; at 60 fps on a 240 MHz core that is about **45 core cycles per dot for everything** — CPU, PPU, APU and audio together. On a rough count of instructions, not a measurement, the current design spends something like a twentieth of that budget on the background, because `draw_bgtile()` emits eight pixels from two pattern bytes, `ppu_renderbg()` decodes the scroll once per scanline rather than once per pixel, and `nes6502_execute()` keeps the 6502's registers in machine registers across all 113 cycles behind a computed-goto dispatch. Per-dot rendering gives all three up.

The realistic middle is the standard **catch-up**: run the CPU in blocks as now, but when it touches `$2000`, `$2001`, `$2005`, `$2006` or a mapper register, finish the current scanline up to exactly that dot and carry on with the new state. Cost scales with mid-scanline writes, not with dots — a handful per frame for most games. It needs `ppu_renderbg()` and `ppu_renderoam()` taught to draw a *part* of a scanline, which the sprite path in particular is not built for, and it would not help a game that polls `$2002` a quarter of a million times a second: that one is predicted by `strike_cycle`, and that prediction is where the accuracy runs out.

---

# Working on it

## Files

| | |
|---|---|
| [waveshare_rlcd_nes.ino](waveshare_rlcd_nes.ino) | setup, card mount, launch |
| [hw_config.h](hw_config.h) | every pin and switch, read by the emulator core too |
| [st7305_gfx.h](st7305_gfx.h) | the panel driver — header-only, no Arduino dependency |
| [st7305_tft.h](st7305_tft.h) / [.cpp](st7305_tft.cpp) | the wrapper — SPI glue, RGB565→mono, the indexed blit, TE pacing |
| [controller.cpp](controller.cpp) | gamepad, four input sources OR'd together |
| [nes_menu.cpp](nes_menu.cpp) | ROM browser, for the card path |
| [nes_rom_embedded.cpp](nes_rom_embedded.cpp) | serves the ROM out of flash; replaces the card reader |
| [nes_rom_data.h](nes_rom_data.h) | the ROM itself, generated by [tools/embed_rom.py](tools/embed_rom.py) |
| [nes_codec.h](nes_codec.h) / [.cpp](nes_codec.cpp) | ES8311 playback bring-up — registers, MCLK ratio, amplifier enable |
| [ble_keyboard.h](ble_keyboard.h) / [.cpp](ble_keyboard.cpp) | BLE HID-over-GATT keyboard client |
| [ble_menu.cpp](ble_menu.cpp) | on-panel keyboard picker, saved to NVS |
| [src/nofrendo/](src/nofrendo) | the emulator core — see [its README](src/nofrendo/README.md) |
| [tools/serial_pad.py](tools/serial_pad.py) | raw serial terminal; macOS `screen` is broken |
| [test/](test) | host tests and the Arduino stubs they build against |

## Building and testing on the host

There is no Arduino toolchain requirement for a syntax check. The fakes in [test/arduino_stubs/](test/arduino_stubs) are enough to compile the sketch on a desktop, and the blit is worth *running*:

```sh
for f in waveshare_rlcd_nes.ino st7305_tft.cpp controller.cpp nes_menu.cpp \
         ble_menu.cpp nes_rom_embedded.cpp; do
    cc -x c++ -std=c++17 -fsyntax-only -Wall -Wextra -I. -Itest/arduino_stubs \
       -include Arduino.h -include esp_timer.h -include esp_heap_caps.h $f
done

cc -x c++ -std=c++17 -O2 -Wall -Wextra -I. -Itest/arduino_stubs -include Arduino.h \
   test/blit_test.cpp test/stub_impl.cpp st7305_tft.cpp -lc++ -o /tmp/blit_test \
   && /tmp/blit_test          # expect: 49 checks, 0 failures
```

**A sketch that passes this has been spell-checked, not tested.** It executes nothing: SPI, timing, interrupts and the panel are entirely unexercised.

Three files in `src/nofrendo/` are not checkable this way — `osd.cpp` pulls `<driver/i2s.h>`, `config.c` pulls `<esp_log.h>` and `mem_shim.c` pulls `<esp32-hal-psram.h>`, and none of the three has a stub. Everything else in the core compiles cleanly against `-I. -Itest/arduino_stubs`.

Headers inside `src/nofrendo/` must be included with **quotes, never angle brackets** — a sketch's `src/` folder gets no `-I` of its own, so `#include
<bitmap.h>` will not resolve. Guard:

```sh
( cd src/nofrendo
  grep -hoE '#include[[:space:]]*<[^>/]+\.h>' *.c *.cpp *.h | sed 's/.*<//; s/>//' | sort -u \
  | while read -r h; do [ -e "$h" ] && echo "ANGLE-BRACKETED LOCAL HEADER: $h"; done )
# expect: no output
```

## The wrapper

`ST7305Tft` exists for two reasons. It owns the platform glue `st7305_gfx.h` deliberately does not have — SPI bus, the three I/O callbacks, the 15 000-byte framebuffer. And it presents the same method surface as the emulator's ST7789 `TFTDriver`, so `src/nofrendo/tft_driver.h` can `typedef` it and the emulator's own code compiles against a 1-bit panel unchanged.

One difference from `TFTDriver` matters: **nothing here talks to the panel.** Drawing writes the framebuffer and returns; the glass does not change until `flush()`, which costs ~6.5 ms of SPI. The ST7789 driver pushed pixels immediately and this cannot — a 15 000-byte full-frame write per `drawString()` would be absurd.

## Changes to the emulator core

All behind `#if NES_USE_ST7305`, so the ST7789 build still works:

- `src/nofrendo/tft_driver.h` — `typedef ST7305Tft TFTDriver`
- `src/nofrendo/osd.cpp` — card access through `NES_FS` (SDMMC, not SPI — the TF slot is not on the SPI bus at all); the palette also fed to the wrapper at full 8-bit precision; a rate-limited `vid_flush()`; the splash and the built-in ROM browser compiled out. The browser goes because it marks the selected row with a red bar behind white text, and red and black land within ~30 luminance units of each other — after the 1-bit reduction the list has no visible cursor. The audio path was factored into one `nes_audio_service()` shared by both back ends, so it keeps draining the APU on frames the panel never shows.

### Bugs fixed in the emulator core

These are **not** behind a flag and are not ST7305-specific — they were wrong for every back end. Listed because anyone diffing against upstream nofrendo will find them and wonder.

| Where | What |
|---|---|
| `src/nofrendo/osd.cpp` | The NES bitmaps were created with `bmp_create(w, h, 0)` — **no margin**. `ppu_renderbg()` starts at `vidbuf - ppu.tile_xofs` and lays down 33 tiles, so it writes up to 7 pixels *before* the line and 8 past it. With the lines packed edge to edge, line N's leading pixels landed on the tail of line N−1 and drew a column of the wrong tile down the right-hand edge of any scrolling picture. Upstream passes 8; the commented-out original is still in `src/nofrendo/nes.c`. Now `NES_BITMAP_OVERDRAW`, at all four call sites |
| `src/nofrendo/bitmap.c` | `bmp_create()` sized the allocation as `((pitch * height) + 3) & ~7`, which rounds **down**, and counted neither the 4-byte alignment of `line[0]` nor the right margin after the last line. At 256×240 with no margin the two happened to agree exactly; with the margin restored it ran three bytes past the end. Sized from the layout `_make_bitmap()` actually builds |
| `src/nofrendo/nes_ppu.c` | `ppu_renderoam()` passed `draw_oamtile()`'s return value — which of the sprite's eight pixels overlapped, 0–7 — straight to `ppu_setstrike()`, which wants the position along the scanline and divides by 3 to turn PPU dots into CPU cycles. Every sprite 0 hit was reported up to 84 cycles early, so raster splits landed in the wrong place. `ppu_fakeoam()` next to it had always passed `sprite_x + n` |
| `src/nofrendo/nes6502.c` | A jammed 6502 set `cpu.jammed` and said nothing. The flag was only ever read to suppress interrupts, so an illegal opcode froze the game while the emulator ran on at full speed. Now one line, once |

## Licence and credits

**GPL-3.0.** See [LICENSE](LICENSE).

The emulator is **Nofrendo**, by Matthew Conte, by way of [Esp32NofrendobyDSN](https://github.com/derdacavga/Esp32-nes-emulator-by-DSN) by DSN Industries, which ships under GPL-3.0. See [src/nofrendo/README.md](src/nofrendo/README.md) for what this fork changes and [src/nofrendo/README.upstream.md](src/nofrendo/README.upstream.md) for the upstream project's own notes.

Because this sketch is built together with that core, **the whole is GPL-3.0** — including the display, input, audio and menu code written for this board.

Pin assignments come from Waveshare's own examples for the board, not from guesswork: their ESP-IDF LVGL demo for the panel, their SD card example for the TF slot, their codec board table for the amplifier enable, and the ESPHome YAMLs for audio and the two buttons.

Bring your own ROMs.
