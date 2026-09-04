/* st7305_tft.h - C++ wrapper around st7305_gfx.h for Arduino / ESP32-S3.
 *
 * Two jobs:
 *
 *  1. Own the platform glue st7305_gfx.h deliberately does not have - the SPI
 *     bus, the three I/O callbacks, the 15000-byte framebuffer - so a sketch
 *     gets a working panel from a default-constructed object.
 *
 *  2. Present the same method surface as the NES emulator's ST7789 `TFTDriver`
 *     (fillScreen / pushImage / drawString / drawFilledRect / color565), so
 *     src/tft_driver.h can typedef this to TFTDriver and the emulator's own
 *     code compiles unchanged against a 1-bit panel.
 *
 * ONE DIFFERENCE FROM TFTDriver, AND IT MATTERS: nothing here talks to the
 * panel. Every draw call writes the framebuffer and returns; the glass does not
 * change until you call flush(), which costs ~6.5 ms of SPI. The ST7789 driver
 * pushed pixels immediately and this cannot - a 15000-byte full-frame write per
 * drawString would be absurd. Draw, then flush.
 *
 * Colour: callers pass RGB565 because that is what the emulator speaks. It is
 * reduced here to ink-or-paper by luminance, through one of the rules in
 * st7305_mono_t below. The default is a 4x4 ordered dither, and ordered rather
 * than error-diffused is the default on purpose: a dither cell that depends
 * only on (x, y) puts the same pattern in the same place every frame, so a
 * static area of a game screen stays still. Floyd-Steinberg makes it crawl, and
 * on a panel with tens of milliseconds of LC response that crawl is the most
 * visible thing on screen. Both diffusion modes are here anyway, because which
 * of these reads best is a matter of looking at the glass, not of argument.
 */
#ifndef ST7305_TFT_H
#define ST7305_TFT_H

#include <Arduino.h>
#include <SPI.h>

#include "hw_config.h"
#include "st7305_gfx.h"

/* Geometry, under the names the emulator's video code already uses. */
#define DISPLAY_WIDTH  ST7305_W
#define DISPLAY_HEIGHT ST7305_H

/* RGB565 names the emulator passes around. On a mono panel only their
 * luminance survives, so RED/GREEN/BLUE are here for source compatibility and
 * nothing else - RED and BLUE both land on ink, GREEN on paper. */
#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF
#define TFT_RED   0xF800
#define TFT_GREEN 0x07E0
#define TFT_BLUE  0x001F

/* Which rule turns a luminance into ink or paper for IMAGES. Text and the
 * primitives are never dithered - see toMono().
 *
 * The first six are ordered: the decision for a pixel depends only on its
 * destination (x, y) and its palette index, so the whole rule collapses into
 * the lookup table blitIndexed() reads and costs nothing per pixel beyond one
 * load. Switching between them is a rebuildDither() and no change at all to the
 * inner loop. The last two are error diffusion and cannot be a table; they get
 * their own slower pass, and only on the stretched blit (see blitIndexed()).
 *
 * The short names are what monoModeName() returns, four characters so a HUD can
 * carry one without eating the picture.
 *
 *   BAYER4    "BAY4"  16 levels, the classic recursive matrix. What this
 *                     wrapper shipped before any of the others existed, and
 *                     still the default. Its texture is a fine diagonal
 *                     cross-hatch, which is either invisible or the first thing
 *                     you see depending on the game.
 *   BAYER8    "BAY8"  64 levels of the same construction. Smoother gradients
 *                     than BAYER4 - a NES sky stops banding - at the cost of a
 *                     coarser, more obvious cross-hatch in flat areas.
 *   BAYER2    "BAY2"  4 levels. Almost no dither texture and much more
 *                     contrast, because a pixel has to clear one of only four
 *                     widely-spaced thresholds. Flattens shading into blocks;
 *                     good for games that were high-contrast to begin with.
 *   CLUSTER4  "CLUS"  Clustered-dot 4x4 - a newsprint halftone screen, where
 *                     BAYER4 disperses. Dots grow from a centre instead of
 *                     scattering, which on a reflective panel whose pixels bleed
 *                     into each other often reads cleaner than dispersed noise.
 *   BLUE8     "BLUE"  8x8 void-and-cluster blue-noise mask. Same cost as BAYER8
 *                     and no cross-hatch: the thresholds are arranged so that
 *                     the "on" pixels at every level are spread without ever
 *                     lining up, so flat areas look like grain rather than like
 *                     a weave. This is 8x8 and not 4x4 for a reason - at 4x4
 *                     there are only 16 cells and BAYER4 is already the
 *                     maximally dispersed arrangement of them, so a 4x4
 *                     "blue noise" mask can only be worse than Bayer, not
 *                     different-and-better.
 *   THRESHOLD "THRS"  No dither at all: ink below mid grey, paper above. Every
 *                     shade collapses, so pictures lose their modelling - but
 *                     text, HUDs and hard-edged sprite art come out perfectly
 *                     crisp, with no pattern crawling over them. setBrightness()
 *                     is the level knob (see below).
 *   FLOYD     "FLOY"  Floyd-Steinberg error diffusion at DESTINATION
 *                     resolution, serpentine. The best tonal accuracy here by a
 *                     wide margin, and the only mode with no repeating texture
 *                     whatsoever. Two costs, and BOTH ARE LARGE.
 *
 *                     THE CPU, MEASURED ON HARDWARE 2026-08-30: ~36 ms per
 *                     frame against an ordered mode's ~2.9 ms, so about 12x.
 *                     That is not time the emulator pays - osd.cpp runs this
 *                     on core 0 - but it does set the rate at which finished
 *                     pictures reach the panel, which is why a diffusion mode
 *                     shows `pushed=9/s` in the log where an ordered one shows
 *                     18. The picture is half as fresh; the game is not slower.
 *                     Do not try to close that gap by tuning this loop against
 *                     a host benchmark, which has already been done and
 *                     already made it worse - see blitStretchedDiffused(). The
 *                     one trade that would work is diffusing at SOURCE
 *                     resolution: 2.1x fewer pixels, and a visibly blockier
 *                     result because each decision then covers up to two
 *                     destination columns and two rows.
 *
 *                     THE CRAWL, which is the thing this wrapper was built to
 *                     avoid: the pattern is recomputed from scratch every
 *                     frame, so a stationary area does not keep a stationary
 *                     pattern. Whether that matters is a matter of looking at
 *                     it; on a still screen it is beautiful.
 *   ATKINSON  "ATKN"  As FLOYD, but diffusing only 6/8 of each error. Loses
 *                     shadow and highlight detail on purpose, which buys
 *                     noticeably more contrast and less of the "worming" FLOYD
 *                     shows in flat areas. The classic 1-bit look.
 */
typedef enum {
    ST7305_MONO_BAYER4 = 0,
    ST7305_MONO_BAYER8,
    ST7305_MONO_BAYER2,
    ST7305_MONO_CLUSTER4,
    ST7305_MONO_BLUE8,
    ST7305_MONO_THRESHOLD,
    ST7305_MONO_FLOYD,
    ST7305_MONO_ATKINSON,
    ST7305_MONO_COUNT
} st7305_mono_t;

class ST7305Tft {
public:
    ST7305Tft();
    ~ST7305Tft();

    /* --- panel ------------------------------------------------------------ */

    /* Brings up SPI and the panel, leaves a blank white screen already
     * flushed. Safe to call once; calling twice re-runs the vendor init. */
    void init();

    /* Sends the framebuffer. This is the only call that costs bus time.
     *
     * Synchronous and unpaced: it returns when the last byte has been clocked
     * out, having taken no notice of where the panel's scan happens to be. For
     * a menu that is right. For animation see startTePacing() below. */
    void flush();

    /* flush() only if something has been drawn since the last one. Cheap to
     * call in a poll loop. */
    void flushIfDirty();

    /* The coarse switch. ST7305_FAST = constant scan at whatever rate the timing
     * registers hold - st7305_init's 16.98 Hz unless st7305_set_fps() has moved
     * it. ST7305_SLOW = 1 Hz idle but ~8 Hz whenever you write, which is the
     * better mode for a menu: same responsiveness to input, a fraction of the
     * power. Use st7305_set_fps() through raw() to pick an exact rate; it
     * re-enters the matching mode itself, so it replaces this call. */
    void setMode(st7305_mode_t m);

    /* The st7305_gfx handle, for anything this wrapper does not cover
     * (st7305_line, st7305_fill_circle, st7305_set_fps, ...). */
    st7305_t *raw() { return &lcd_; }

    /* --- TE-paced presentation (the animation path) ------------------------
     *
     * WHY THIS EXISTS. GRAM is single-buffered and the panel scans it on its
     * own clock. A flush writes all 15000 bytes in 6.5 ms while a scan takes
     * tens of ms - several times longer at every rung of the rate ladder - so
     * an unpaced flush always crosses the scan line: the rows above the
     * crossing show the old frame for the rest of that scan and the rows below
     * show the new one. That seam is there on every unpaced frame, and it walks
     * down the screen as the two periods beat against each other. Nothing on
     * the MCU side fixes it - the race is between the SPI write and the panel's
     * own scan, inside the controller.
     *
     * WHAT FIXES IT. Start the write at the moment the scan restarts. The write
     * is several times faster than the scan, so from then on it stays ahead of
     * the beam all the way down and the beam only ever reads new bytes. That
     * margin is what makes this rate-independent: it holds at every rung, and
     * by a wider margin the slower the panel runs. The panel says
     * when that moment is: TE (0x35, enabled by st7305_init) pulses once per
     * frame on a pin. So the flush has to be driven by that pin, not by a timer
     * - which means it happens on its own task, and the producer needs a second
     * buffer to draw into while the first one is on the wire.
     *
     * HOW TO USE IT. Once, when animation starts:
     *
     *     tft.startTePacing(RLCD_TE, 50);
     *
     * then per frame, instead of "draw; flush()":
     *
     *     if (!tft.beginFrame()) { ...frame dropped, skip the drawing... }
     *     else { ...draw...; tft.present(); }
     *
     * present() returns immediately; the frame goes out on the next TE edge.
     * beginFrame() returns false while a frame is already queued - drawing
     * another would only throw that one away unseen, so production is paced to
     * the panel rather than run flat out. The caller must SKIP the frame, not
     * wait: waiting would stall whatever else that thread owes (in the
     * emulator's case, audio).
     *
     * WHAT IT COSTS. A second ST7305_FB_SIZE buffer, and up to one panel period
     * of latency: a frame finished just after an edge waits out the whole scan
     * before the next one. That scales with the rate - it is the price of the
     * lock, and it stays inside this panel's own LC response at every rung.
     *
     * Only ONE thread may drive beginFrame()/present(), and while pacing is on
     * nothing else may touch the panel - the task owns the SPI bus. Menus and
     * splash screens should run before startTePacing() or after stopTePacing().
     *
     * `te_pin` is the GPIO TE is wired to, or -1 to set up the state machine
     * with no interrupt and no task (which is what the host test drives).
     * `fallback_ms` is the safety net: if that long passes with no edge three
     * times over, the task says so once on Serial and falls back to pushing on
     * a timer, so a board that does not wire TE degrades to today's tearing
     * rather than to a frozen picture. A later edge restores TE pacing. */
    bool startTePacing(int te_pin, uint32_t fallback_ms);
    void stopTePacing();
    bool tePaced() const { return paced_; }

    /* Claims a buffer to draw into. False means a frame is already waiting for
     * the panel and this one should be skipped - not drawn and discarded, and
     * not waited on. Idempotent: calling it twice without present() in between
     * returns the same buffer. */
    bool beginFrame();

    /* Hands the drawn buffer to the TE task and returns at once. With pacing
     * off this is just flush(), so the same code works either way. */
    void present();

    /* --- the TE task's half of the state machine ---------------------------
     * Public because test/te_pacing_test.cpp drives them directly; there is no
     * reason for application code to call either.
     *
     * takeReady() returns the buffer index to send, or -1 if the producer has
     * not finished one since the last take. sendDone() releases it afterwards.
     *
     * THE ORDER INSIDE takeReady() IS LOAD-BEARING and is why none of this
     * needs a lock: it sets sending_ BEFORE clearing ready_, so at every
     * instant the buffer it is taking is named by at least one of the two slots
     * beginFrame() excludes. Swap those two lines and there is a window in
     * which the producer is handed the buffer that is about to go on the
     * wire. */
    int  takeReady();
    void sendDone();

    /* State, for the invariant checks in the host test and for the serial log.
     * -1 means "no buffer in this slot". */
    int  drawingIndex() const { return drawing_; }
    int  readyIndex()   const { return ready_; }
    int  sendingIndex() const { return sending_; }

    /* Since startTePacing(). teEdges() not climbing means TE is not arriving;
     * pushes() is the true picture rate, which is the panel's rate once paced
     * and has nothing to do with how fast the producer runs. */
    uint32_t teEdges() const;
    uint32_t pushes()  const { return pushes_; }

    /* --- TFTDriver-compatible surface -------------------------------------
     * Same signatures as the emulator's ST7789 driver. All buffered. */

    void fillScreen(uint16_t color);
    void pushImage(int x, int y, int w, int h, const uint16_t *data,
                   bool preswapped = false);
    void drawFilledRect(int x, int y, int w, int h, uint16_t color);
    void drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int scale = 1);
    void drawString(int x, int y, const char *str, uint16_t fg, uint16_t bg,
                    int scale = 1);
    void drawNumber(int x, int y, int num, uint16_t fg, uint16_t bg,
                    int scale = 1);

    static uint16_t color565(uint8_t r, uint8_t g, uint8_t b)
    {
        return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }

    /* Text metrics, so callers can centre a label instead of guessing. */
    static int textWidth(const char *str, int scale = 1);
    static int textHeight(int scale = 1) { return 7 * scale; }

    /* --- 8-bit indexed video (the NES path) --------------------------------
     *
     * setPaletteEntry() for each of the 256 NES colours, then rebuildDither()
     * once. That bakes luminance, the tone curve AND the dither threshold into
     * a lookup table, so blitIndexed() is one table read per pixel with no
     * arithmetic at all.
     *
     * rebuildDither() IS THE COMMIT POINT FOR EVERYTHING BELOW. The palette,
     * the invert flag, the mono mode and all three tone knobs are stored when
     * you set them and take effect when you call it - so a caller changing
     * several at once pays for one rebuild, and a caller who forgets is still
     * looking at a table that describes the old settings. It costs 16384 table
     * entries plus a 256-entry tone curve, which is tens of microseconds; call
     * it whenever, just not per frame.
     *
     * SAFE TO CALL WHILE A BLIT IS RUNNING ON ANOTHER THREAD, and osd.cpp does
     * exactly that - it dithers on core 0 while a gamepad chord on core 1
     * changes the mode. Everything this writes is a table of plain bytes, so a
     * blit that catches it mid-rebuild produces one frame from a mix of the old
     * and new tables and nothing worse. That property is not free: it is why
     * the error-diffusion scratch is allocated once and never released, and why
     * there is a long note about it in the implementation. Do not add a free()
     * or a realloc() to this path. */
    void setPaletteEntry(int index, int r, int g, int b);
    void rebuildDither();

    /* Ink/paper swap for images only - text and primitives are not affected.
     * Takes effect on the next rebuildDither(). */
    void setInvertVideo(bool invert);
    bool invertVideo() const { return invert_; }

    /* Which reduction rule images use. See st7305_mono_t for what each one
     * looks like and what it costs. Takes effect on the next rebuildDither().
     *
     * If a diffusion mode is selected and its scratch buffers cannot be
     * allocated, rebuildDither() falls back to ST7305_MONO_BAYER4 and says so
     * once on Serial - monoMode() then reports the fallback, not what you
     * asked for, because that is what is actually on the glass. */
    void          setMonoMode(st7305_mono_t m);
    st7305_mono_t monoMode() const { return mono_; }

    /* Four characters and a NUL, for a HUD. Never null, even for a mode out of
     * range - that returns "????" rather than reading off the end. */
    static const char *monoModeName(st7305_mono_t m);
    const char *monoModeName() const { return monoModeName(mono_); }

    /* --- tone curve --------------------------------------------------------
     *
     * Applied to the luminance of every palette entry BEFORE it meets a
     * threshold, in this order: invert, gamma, contrast, brightness. It is
     * folded into the same table, so none of it costs anything per pixel - and
     * because it is upstream of the threshold it works identically for the
     * ordered modes and for the two diffusion ones.
     *
     * Worth having on this panel specifically. A reflective LCD has perhaps
     * 5:1 of real contrast in room light against a CRT's 100:1, so a NES
     * palette mapped linearly puts far too much of the picture into the bottom
     * of the range and dark games come out as texture on black. Lifting the
     * gamma is the fix, and it is a different fix per game.
     *
     *   setGamma      100 = linear. ABOVE 100 lifts the midtones (brighter,
     *                 more shadow detail, flatter); below 100 deepens them.
     *                 Clamped to 25..400. This is the display-gamma
     *                 convention: the exponent applied is 100/gamma.
     *   setContrast   100 = unchanged. Scales distance from mid grey, so above
     *                 100 pushes shades apart and clips the ends. 25..400.
     *                 IT DOES NOTHING AT ALL UNDER ST7305_MONO_THRESHOLD, and
     *                 that is arithmetic rather than an oversight: it pivots on
     *                 mid grey and that mode's one threshold IS mid grey, so
     *                 there is nothing it can push across. Use brightness
     *                 there.
     *   setBrightness Added last, -128..127. FOR ST7305_MONO_THRESHOLD THIS IS
     *                 THE LEVEL KNOB - the threshold itself is fixed at mid
     *                 grey, and moving the picture up is the same thing as
     *                 moving the level down, with the sign the way a user
     *                 expects it ("+ is brighter").
     *
     * All three take effect on the next rebuildDither(). Defaults are
     * 100/100/0, which is an exact identity - a build that never touches them
     * produces the same bytes this wrapper produced before they existed. */
    void setGamma(int gamma_x100);
    void setContrast(int contrast_x100);
    void setBrightness(int bias);
    int  gamma100()   const { return gamma100_; }
    int  contrast100() const { return contrast100_; }
    int  brightness() const { return brightness_; }

    /* Dithers an 8-bit indexed image straight into the panel's GRAM layout and
     * centres it. `lines` is a per-row pointer array (nofrendo's bitmap_t::line).
     *
     * Fast because the destination byte boundaries are made to line up with the
     * source: one GRAM byte in landscape covers 2 columns x 4 rows, so both the
     * size and the centring offset are rounded to a multiple of 4 and every
     * byte is then a pure write - no read-modify-write, no clipping test, no
     * runtime dither phase in the inner loop. For 256x240 that is 7680 byte
     * stores built from 61440 table reads.
     *
     * w and h are rounded DOWN to a multiple of 4, and the origin can land up
     * to 2 px off true centre; 256x240 divides exactly, so neither shows. An
     * image larger than the panel is rejected outright rather than clipped.
     *
     * ALWAYS ORDERED, whatever monoMode() says. The two diffusion modes are
     * implemented for blitIndexedStretched() only - that is the path the
     * emulator uses, and giving this one its own diffusion pass would be a
     * second copy of the same 80 lines to serve no caller. In a diffusion mode
     * rebuildDither() keeps the table filled with BAYER4, so this stays
     * correct rather than becoming undefined; it just is not what the game
     * picture is doing. */
    void blitIndexed(uint8_t *const *lines, int w, int h);

    /* Same, but nearest-neighbour scaled to fill ALL 400x300 - which is what
     * the NES actually wants, because its pixels were never square.
     *
     * The PPU renders 256x240 at a dot clock that made each pixel about 8/7 as
     * wide as it was tall, and a TV showed only the middle 224 rows. So the
     * picture a player saw was 256x224 of non-square pixels filling a 4:3
     * screen - and this panel is exactly 4:3. Stretching 256x224 to 400x300
     * reproduces that to within about 2%; drawing it 1:1 does not, and leaves
     * everything 22% too tall.
     *
     * `lines` is indexed from srcY0 to srcY0+srcH-1, so pass the 240-row array
     * with srcY0=8, srcH=224 to drop the overscan rows games write junk into.
     * `w` source columns are stretched across the full panel width.
     *
     * Writes every one of the 15000 GRAM bytes, so nothing survives underneath
     * it - a HUD or a border has to be drawn AFTER this, every frame.
     *
     * Costs about twice what blitIndexed() does (120000 table reads against
     * 61440) and no arithmetic beyond two array lookups per pixel: the source
     * coordinate maps are built once and reused until w/srcY0/srcH change.
     *
     * One thing the nearest-neighbour duplication does NOT do here is band:
     * the dither threshold is chosen by DESTINATION x and y, so a source pixel
     * that lands in two adjacent columns gets two different thresholds and the
     * doubling reads as texture rather than as a visible fat pixel.
     *
     * In ST7305_MONO_FLOYD or ST7305_MONO_ATKINSON this hands off to a
     * serpentine error-diffusion pass instead. Same output rectangle, same
     * upscale, same tone curve; a very different cost (~12x on hardware, see
     * st7305_mono_t) and a different failure mode. Everything above about the
     * table and the per-pixel cost applies to the six ordered modes only -
     * which, measured against each other, are all within 15% of one another
     * and can be treated as costing the same. */
    void blitIndexedStretched(uint8_t *const *lines, int w, int srcY0,
                              int srcH);

    /* --- diffusion profiling -----------------------------------------------
     *
     * Microseconds the last diffused frame spent in each of its three phases:
     * the tone prefetch, the diffusion loop proper, and assembling GRAM bytes.
     * All three are zero in an ordered mode, which never calls the code that
     * sets them.
     *
     * HERE BECAUSE A HOST BENCHMARK DOES NOT PREDICT THIS CHIP, and that is
     * not a suspicion - it is what happened. A restructured diffusion loop
     * measured 27% faster on a development machine and measured SLOWER on the
     * S3, and it was these three numbers that showed it: the phase the rewrite
     * added cost more than the phase it sped up. The rewrite is gone. Anything
     * that replaces it should be judged the same way, from the board. They cost
     * about 900 timer reads per diffused frame, well under 1% of one. */
    uint32_t diffToneUs() const { return t_tone_us_; }
    uint32_t diffLoopUs() const { return t_loop_us_; }
    uint32_t diffAsmUs()  const { return t_asm_us_; }

    /* Where blitIndexed() would put an image of this size - useful for drawing
     * a border or a HUD that does not collide with it. Pass the same w/h you
     * pass to blitIndexed(); the multiple-of-4 rounding is applied here too.
     * Meaningless for blitIndexedStretched(), which leaves no margin. */
    static int blitOriginX(int w) { return ((ST7305_W - (w & ~3)) / 2) & ~3; }
    static int blitOriginY(int h) { return ((ST7305_H - (h & ~3)) / 2) & ~3; }

    /* --- misc -------------------------------------------------------------- */

    void clear(st7305_color_t c);
    bool dirty() const { return dirty_; }

    /* The 15000 bytes drawing currently lands in, in the panel's own GRAM
     * layout. Exposed so test/blit_test.cpp can check the fast blit against a
     * naive reference without a panel. Under TE pacing this follows whichever
     * buffer beginFrame() handed out, so it is only meaningful between a
     * beginFrame() and its present(). */
    const uint8_t *framebuffer() const { return lcd_.fb; }

    /* RGB565 -> ink or paper, by luminance. Public because callers building
     * their own UI want the same rule this uses. */
    static st7305_color_t toMono(uint16_t rgb565);

private:
    st7305_t    lcd_;
    SPIClass    spi_;

    /* Two of them, and lcd_.fb points at whichever one is being drawn into -
     * so every drawing path, this wrapper's own fast blits and the library
     * primitives reached through raw() alike, follows the swap for free.
     * Unpaced, only fb_[0] is ever used and this is the old single buffer. */
    uint8_t     fb_[2][ST7305_FB_SIZE];

    /* Which buffer is in which role, or -1. drawing_ is written only by the
     * producer thread, sending_ only by the TE task; ready_ is the handoff and
     * is written by both, which the ordering rule on takeReady() makes safe
     * without a lock. Single aligned bytes, so the loads and stores are atomic
     * on both cores. */
    volatile int8_t drawing_;
    volatile int8_t ready_;
    volatile int8_t sending_;
    bool        paced_;

    /* A second device handle for the task, identical to lcd_ except that its
     * fb points at the buffer being sent. st7305_flush() reads exactly io and
     * fb and nothing else, so this stays truthful without having to move
     * lcd_.fb out from under the producer mid-frame. */
    st7305_t    send_lcd_;
    /* Written by the task, read by the producer for the log line - and the two
     * are on different cores, so volatile rather than a stale cached word. */
    volatile uint32_t pushes_;

    bool        dirty_;
    bool        invert_;

    /* What the caller asked of the reduction, none of it acted on until
     * rebuildDither(). */
    st7305_mono_t mono_;
    int         gamma100_;
    int         contrast100_;
    int         brightness_;

    /* Rec.601 luminance 0..255 per palette index, straight from
     * setPaletteEntry() and with nothing applied to it yet; then the same after
     * invert, gamma, contrast and brightness. Keeping both means a tone knob
     * can move without the palette having to be re-sent, which matters because
     * the emulator sends it once and the knobs move while a game is running. */
    uint8_t     lum_[256];
    uint8_t     tone_[256];

    /* The current mode's 8x8 threshold matrix, 0..255 per cell. Kept separately
     * from the table below because pushImage() works on RGB565 and so cannot go
     * through a palette-indexed table at all - it needs the raw thresholds. */
    uint8_t     thr_[8][8];

    /* The table the ordered blits read, and the whole trick:
     * dither_[(y & 7) * 8 + (x & 7)][index] is the GRAM bit for that pixel,
     * ALREADY SHIFTED to its position in the destination byte, so the inner
     * loop is eight loads OR'd together and contains no arithmetic at all.
     *
     * 16 KB of DRAM. It is 8x8 rather than 4x4 (4 KB) so that BAYER8 and BLUE8
     * exist; the 4x4 and 2x2 matrices are simply replicated into it, which
     * costs nothing at run time and keeps ONE inner loop for every ordered
     * mode. Sixteen kilobytes of internal SRAM is not cached on the ESP32-S3,
     * so unlike a table this size behind a cache it is exactly as fast to read
     * as the 4 KB one was - the cost is RAM and nothing else. */
    uint8_t     dither_[64][256];

    /* Scratch for the two error-diffusion modes, malloc'd by rebuildDither()
     * when one is selected and freed when it is not, because it is 4.4 KB that
     * every ordered mode would otherwise carry for nothing.
     *
     * err_ is three destination rows of accumulated error in 1/16 units,
     * padded by 2 cells at each end so that the +-2 column taps need no bounds
     * test in the inner loop; the padding collects the error that falls off the
     * edge, which is where it should go.
     *
     * bits_ is four rows of results, ALREADY SHIFTED to their bit position in
     * the GRAM byte, held back until a whole 4-row group is done - error
     * diffusion has to run in row order and GRAM bytes are written in 4-row
     * groups, and this is what reconciles the two.
     *
     * tonerow_ is one destination row of tone values, and it points INTO the
     * bits_ allocation as a fifth row rather than being malloc'd separately.
     * It is what keeps the three-deep dependent load chain
     * tone_[src[colmap_[x]]] out of the diffusion loop; see the note above
     * blitStretchedDiffused(). */
    int16_t    *err_;
    uint8_t    *bits_;
    uint8_t    *tonerow_;

    /* Written once per diffused frame, read by the serial log. See
     * diffToneUs() above for why they exist at all. */
    uint32_t    t_tone_us_;
    uint32_t    t_loop_us_;
    uint32_t    t_asm_us_;

    /* Destination pixel -> source pixel, for blitIndexedStretched(). Rebuilt
     * only when the source geometry changes, which in practice means once.
     * 1.4 KB to turn a per-pixel divide into a per-pixel array read. */
    uint16_t    colmap_[ST7305_W];
    uint16_t    rowmap_[ST7305_H];
    int         map_w_, map_y0_, map_h_;    /* what colmap_/rowmap_ describe */

    /* TE plumbing. te_task_ is a TaskHandle_t behind a void* so that FreeRTOS
     * stays out of this header and the host test keeps building against it. */
    int              te_pin_;
    uint32_t         fallback_ms_;
    /* Volatile because stopTePacing() spins on it: the task clears it on its
     * way out and that is the only signal that it is safe to reuse the bus. */
    void   *volatile te_task_;
    volatile uint32_t te_edges_;
    volatile bool    te_stop_;

    static void ioXfer(void *ctx, int dc, const uint8_t *buf, size_t len);
    static void ioDelay(void *ctx, uint32_t ms);
    static void ioReset(void *ctx, int level);

    /* lum_ -> tone_, then mono_ -> thr_. Both are called by rebuildDither() and
     * exist separately only to keep that function readable. */
    void rebuildTone();
    void buildMatrix();

    /* Allocates err_/bits_ if they are not already there. False means malloc
     * failed and the caller must not select a diffusion mode. */
    bool ensureDiffusion();
    void freeDiffusion();

    /* The FLOYD/ATKINSON half of blitIndexedStretched(). Assumes the caller has
     * already validated the arguments and built colmap_/rowmap_. */
    void blitStretchedDiffused(uint8_t *const *lines, bool atkinson);

#if defined(ARDUINO_ARCH_ESP32)
    /* IRAM_ATTR belongs on the FIRST declaration - GCC rejects a section
     * attribute that only appears on the definition. */
    static void IRAM_ATTR teIsr(void *arg);
    static void teTaskEntry(void *arg);
    void        teTaskLoop();
#endif
};

#endif /* ST7305_TFT_H */
