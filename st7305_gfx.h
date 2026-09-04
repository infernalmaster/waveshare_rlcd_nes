/* st7305_gfx.h - header-only graphics library for the ST7305 400x300 landscape RLCD
 *
 *   #define ST7305_GFX_IMPLEMENTATION
 *   #include "st7305_gfx.h"
 *
 * in exactly ONE translation unit; include it plainly everywhere else.
 *
 * The framebuffer IS the panel's GRAM. One byte covers 2 landscape columns x
 * 4 landscape rows, so the fast axis in landscape is VERTICAL and filled
 * primitives sweep by column. Flushing needs no format conversion at all.
 *
 * No dynamic allocation. No platform code - all I/O goes through three
 * user-supplied callbacks, so this compiles and tests on the host.
 *
 * Layout reference: ST7305_BUFFER_LAYOUT.md
 */
#ifndef ST7305_GFX_H
#define ST7305_GFX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ST7305_W        400
#define ST7305_H        300
#define ST7305_STRIDE   75
#define ST7305_FB_SIZE  15000

/* Paper semantics: WHITE is the blank background, BLACK is ink.
 * WHITE also happens to be GRAM bit 1, so there is no inversion anywhere. */
typedef enum { ST7305_BLACK = 0, ST7305_WHITE = 1 } st7305_color_t;

/* FAST = high power scan (responsive), SLOW = low power scan (static content). */
typedef enum { ST7305_FAST = 0, ST7305_SLOW = 1 } st7305_mode_t;

/* Scan rates for st7305_set_fps. Names are <rounded Hz>_<0xD8 byte>, because
 * two register pairs measure at the same rate and Hz alone would collide.
 *
 * Each value packs the two registers it programs: high byte = 0xD8's OSC byte,
 * low byte = 0xB2's payload. A real 0xD8 byte always has OSCEN (bit 7) set, so
 * a high byte of 0x00 cannot be a rate and is used to mark the LPM entries.
 *
 * CLEAN vs WASHED is this panel's contrast threshold, bracketed by measurement
 * at 23.22..25.97 Hz: at or below it the blacks are black, above it they wash
 * out and flicker. It is not a preference. Layout doc 5a and 6.4. */
typedef enum {
    /* HPM, HFRA = 0 (half of OSCSW's max). All eight MEASURED on hardware
     * 2026-08-27 by counting TE edges over a 4 s window. */
    ST7305_FPS_17_A6 = 0xA600,  /* 16.98 Hz  clean - what st7305_init ships  */
    ST7305_FPS_18_A2 = 0xA200,  /* 18.48 Hz  clean                           */
    ST7305_FPS_18_A4 = 0xA400,  /* 18.48 Hz  clean - measured identical to A2*/
    ST7305_FPS_20_A0 = 0xA000,  /* 20.47 Hz  clean                           */
    ST7305_FPS_21_86 = 0x8600,  /* 20.97 Hz  clean                           */
    ST7305_FPS_23_82 = 0x8200,  /* 22.97 Hz  clean                           */
    ST7305_FPS_23_84 = 0x8400,  /* 23.22 Hz  clean - the fastest almost clean rung  */
    ST7305_FPS_26_80 = 0x8000,  /* 25.97 Hz  WASHED - the vendor's own value */

    /* HPM, HFRA = 1 (the un-halved rate). NEVER MEASURED except 0x80: the Hz
     * in these names are twice the measured half-rate above, not datasheet
     * figures. Every one is past the threshold, so expect all of them to look
     * bad - 0x80 was tried and was markedly worse. They exist so the rate
     * sweep in examples/arduino/st7305_anim can reach them, not because any
     * of them is a reasonable choice. */
    ST7305_FPS_34_A6 = 0xA610,  /* ~33.96 Hz  predicted */
    ST7305_FPS_37_A2 = 0xA210,  /* ~36.96 Hz  predicted */
    ST7305_FPS_37_A4 = 0xA410,  /* ~36.96 Hz  predicted */
    ST7305_FPS_41_A0 = 0xA010,  /* ~40.94 Hz  predicted */
    ST7305_FPS_42_86 = 0x8610,  /* ~41.94 Hz  predicted */
    ST7305_FPS_46_82 = 0x8210,  /* ~45.94 Hz  predicted */
    ST7305_FPS_46_84 = 0x8410,  /* ~46.44 Hz  predicted */
    ST7305_FPS_51_80 = 0x8010,  /* 51 Hz, measured once - markedly worse     */

    /* LPM (LFRA). These are IDLE rates and nothing else: writing GRAM in LPM
     * triggers a refresh that runs at ~8 Hz whatever LFRA says, measured
     * across the whole 32x ladder. So picking LPM_0_25 for an animation gets
     * you ~8 fps, not 0.25 - what it buys is near-zero power while the screen
     * is static. LPM_1 is the shipped value and is already optimal; raising it
     * costs idle power and buys nothing. Layout doc 5b.
     *
     * No register suffix here: unlike the HPM ladder, no two LFRA values share
     * a rate, so there is nothing to disambiguate. */
    ST7305_FPS_LPM_0_25 = 0x0000,  /* LFRA 0 */
    ST7305_FPS_LPM_0_5  = 0x0001,  /* LFRA 1 */
    ST7305_FPS_LPM_1    = 0x0002,  /* LFRA 2 - what st7305_init ships */
    ST7305_FPS_LPM_2    = 0x0003,  /* LFRA 3 */
    ST7305_FPS_LPM_4    = 0x0004,  /* LFRA 4 */
    ST7305_FPS_LPM_8    = 0x0005   /* LFRA 5 */
} st7305_fps_t;

typedef struct {
    /* dc: 0 = command (len is always 1), 1 = data.
     * MUST be synchronous: the library passes fb directly for the 15000-byte
     * write, so a DMA implementation must wait for completion before returning. */
    void (*xfer)(void *ctx, int dc, const uint8_t *buf, size_t len);
    void (*delay_ms)(void *ctx, uint32_t ms);
    void (*reset)(void *ctx, int level);   /* may be NULL if RST is strapped */
    void *ctx;
} st7305_io_t;

/* `osc` and `b2` shadow the two timing registers, because HFRA (an HPM knob)
 * and LFRA (an LPM knob) share one byte and neither is readable back over this
 * bus. st7305_init sets them; st7305_set_fps maintains them.
 *
 * READ them if you want to show or log what the panel is actually running at -
 * that is what examples/arduino/st7305_anim puts on screen, and it is more
 * truthful than echoing back the constant you passed in. Do NOT write them: the
 * chip would not hear about it and every later st7305_set_fps would preserve a
 * half-register that was never programmed. */
typedef struct {
    st7305_io_t io;
    uint8_t    *fb;
    uint8_t     osc;   /* last 0xD8 byte 1 written */
    uint8_t     b2;    /* last 0xB2 payload written */
} st7305_t;

/* --- panel -----------------------------------------------------------------
 *
 * The only four functions in this header that touch the wire. Everything below
 * them writes to your framebuffer and does no I/O at all.
 */

/* Resets and initialises the panel, leaving a blank white screen.
 *
 * `fb` must point at ST7305_FB_SIZE (15000) bytes that outlive `d`. The library
 * never allocates and never copies pixel data, so this buffer IS the working
 * framebuffer from here on. `io` is copied into `d`, so it may be a temporary.
 *
 * Pulses RST high/low/high (50/20/50 ms, via io.delay_ms) if io.reset is
 * non-NULL; pass NULL if RST is strapped on your board and the pulse is
 * skipped. Then runs the two vendor init tables with the mandatory 120 ms
 * sleep-out delay between them, clears the framebuffer to ST7305_WHITE and
 * flushes it.
 *
 * WHAT IT LEAVES BEHIND, because you probably do not need to change any of it:
 * high power mode (0x38), display on (0x29), TE enabled (0x35). Those are the
 * last entries in the init table. A working picture needs no further calls.
 *
 * Costs roughly 240 ms of delays plus one full flush. */
void st7305_init    (st7305_t *d, const st7305_io_t *io, uint8_t *fb);

/* Streams the whole framebuffer to the panel's GRAM. THIS is what makes drawing
 * visible - every drawing function below is memory-only until you call it.
 *
 * Always a full 400x300 frame: full column window (0x2A), full page window
 * (0x2B), then one 0x2C and all 15000 bytes in a single xfer. There is no
 * partial-update path and no dirty tracking. The framebuffer is already in
 * native GRAM order, so this is a straight copy with zero format conversion.
 *
 * Costs ~6.5 ms at 20 MHz, measured (examples/arduino/st7305_speed). DO NOT
 * read that as a frame rate. The panel scans on its own clock - 16.96 Hz with
 * the 0xD8 = 0xA6 shipped here - and GRAM is single-buffered, so flushing
 * faster than it scans overwrites memory mid-scan and tears. Pace flushes to
 * the panel, ideally off the TE pin. */
void st7305_flush   (st7305_t *d);

/* Selects the panel's scan rate. One bare opcode; 0xB2/0xB3/0xB4 already carry
 * both modes' values and do NOT need reprogramming.
 *
 *   ST7305_FAST  0x38, high power. 16.96 Hz constant scan.
 *   ST7305_SLOW  0x39, low power. ~1 Hz idle - but writing GRAM triggers a
 *                refresh, so you get ~8 Hz for as long as you keep flushing.
 *                Lowest power, and the better default for static content.
 *
 * Both sit below this panel's contrast threshold, so this is purely a
 * power/refresh trade and does not affect how black the blacks look. Touches
 * neither the framebuffer nor the displayed image. The measurements behind
 * these numbers are at the implementation. */
void st7305_set_mode(st7305_t *d, st7305_mode_t m);

/* Picks an exact scan rate, and the mode that goes with it.
 *
 * st7305_set_mode is the coarse switch - fast or slow, at whatever rate the
 * timing registers currently hold. This is the fine one: it reprograms those
 * registers and then re-enters the mode the chosen rate belongs to, so an
 * ST7305_FPS_* constant is the only thing you need to pass. Calling this makes
 * a following st7305_set_mode redundant, and vice versa.
 *
 *     st7305_set_fps(&d, ST7305_FPS_23_84);    23.22 Hz HPM, fastest clean rung
 *     st7305_set_fps(&d, ST7305_FPS_LPM_1);    1 Hz idle, ~8 Hz while writing
 *
 * Sends 0xD8, then 0xB2, then 0x38 or 0x39 - IN THAT ORDER. The controller
 * latches the rate when a mode is entered, so the opcode has to come last or
 * the new registers sit there doing nothing until something else re-enters the
 * mode. That is why this cannot be a plain register poke.
 *
 * HPM and LPM rates do not disturb each other: the HPM constants keep whatever
 * LFRA you last set, the LPM constants keep OSCSW and HFRA. The device carries
 * two shadow bytes for this, so it must have been through st7305_init.
 *
 * WHAT IT DOES NOT TOUCH. 0xB3 (gate EQ) is the third register of this setting
 * and its counts are denominated in oscillator ticks that U8g2 tuned against
 * 0xD8 = 0xA6 specifically. Moving off 0xA6 therefore un-matches the pair on
 * paper. It is left alone because the hardware sweeps that measured this whole
 * ladder left it alone too and found every rung below the threshold clean - so
 * changing it here would invalidate the numbers in these comments and replace
 * them with nothing. Also does not touch the framebuffer or the picture.
 *
 * Expect a visible settle of a few hundred ms after a rate change. No delay is
 * taken here; add one yourself if you are switching rates to look at them. */
void st7305_set_fps (st7305_t *d, st7305_fps_t f);

/* The picture on/off switch. Draws nothing, erases nothing.
 *
 * Think of a monitor's power button: press it and the image is gone, press it
 * again and the SAME image is back. The panel keeps its GRAM contents across an
 * off/on, so making it visible again needs no redraw and no flush.
 *
 * Not to be confused with st7305_flush - they sit at opposite ends of the path:
 *
 *   st7305_flush        "here is a new picture"  15000 bytes, ~6.5 ms
 *   st7305_set_display  "show it, or don't"      one opcode, instant
 *
 * Sends 0x29 when visible, 0x28 when not.
 *
 * YOU WILL RARELY NEED THIS. st7305_init already leaves the display on, and the
 * obvious use - hiding a half-drawn frame - does not apply: you draw into fb in
 * MCU RAM, so nothing is on the glass until you flush. There is no partial
 * drawing to hide.
 *
 * The one case that IS real: a tear-free update when TE is not available. A
 * flush takes 6.5 ms and the panel may scan GRAM in the middle of it, so
 * blank / flush / unblank guarantees a clean swap. Fine for occasional updates,
 * useless for animation - it blinks once per frame. If TE is wired, pacing off
 * TE beats this: no tearing AND no blink.
 *
 * NOT A POWER-SAVING CALL, and do not reach for it as one. The measured power
 * lever is st7305_set_mode(ST7305_SLOW). Deep sleep (0x10) is not exposed by
 * this library at all.
 *
 * [U] What "not visible" actually looks like here is unverified - nobody has
 * watched the glass with 0x28 sent. This is a REFLECTIVE panel with no
 * backlight, so off means undriven crystals, not a black screen. Note also that
 * this panel is NOT bistable: it needs continuous refresh, so if 0x28 does stop
 * the drive, the image on the glass decays. What survives an off/on is GRAM -
 * the controller's memory - not the picture on the crystals. */
void st7305_set_display(st7305_t *d, bool visible);

/* --- drawing ----------------------------------------------------------------
 *
 * These touch d->fb and nothing else: no I/O, no panel state, no allocation.
 * Nothing reaches the glass until st7305_flush.
 *
 * EVERY function here clips to the 400x300 panel. Off-panel coordinates are
 * silently ignored rather than wrapped or rejected, so drawing partly or wholly
 * off-screen is always safe. st7305_fill_triangle has one extra restriction,
 * documented on it.
 *
 * Coordinates are landscape: x in [0,400) left to right, y in [0,300) top to
 * bottom. Colour is paper semantics - ST7305_WHITE is the blank background,
 * ST7305_BLACK is ink.
 *
 * THE FAST AXIS IS VERTICAL. One byte spans 2 columns x 4 rows, so a vertical
 * run stays inside a byte while a horizontal one strides 75 bytes per 2 px -
 * measured 4x the cost for glyph blitting. Prefer column-major loops and
 * vertical spans in anything hot.
 */

/* Fills the whole framebuffer with one colour. A single memset, and the
 * cheapest way to start a frame. Nothing to clip. */
void st7305_clear (st7305_t *d, st7305_color_t c);

/* Sets one pixel. Out-of-range x or y is a silent no-op. */
void st7305_pixel (st7305_t *d, int x, int y, st7305_color_t c);

/* Bresenham line with INCLUSIVE endpoints - both (x0,y0) and (x1,y1) are drawn.
 *
 * Exactly horizontal or vertical lines are special-cased to span writes and are
 * dramatically faster than the general case; a vertical one is about the
 * cheapest thing in this library.
 *
 * Clipping is per-pixel, and only on the slow path - a line with both endpoints
 * on-panel is bounds-checked once and then writes directly. That is deliberate
 * rather than Cohen-Sutherland: moving an endpoint changes which lattice points
 * the walk visits, so a clipped line would not be a sub-segment of the
 * unclipped one.
 *
 * THE COST: a line with far-off-screen endpoints still iterates every
 * off-screen step, writing nothing. Clamp coordinates yourself if they can get
 * wild. */
void st7305_line  (st7305_t *d, int x0, int y0, int x1, int y1, st7305_color_t c);

/* Rectangle outline, 1 px wide, drawn INSIDE the box: corners are (x,y) and
 * (x+w-1, y+h-1), so w=1,h=1 is a single pixel.
 *
 * Negative w or h is legal and extends the other way - x=10,w=-4 covers x 7..10.
 * A w or h of exactly 0 draws nothing. */
void st7305_rect     (st7305_t *d, int x, int y, int w, int h, st7305_color_t c);

/* Filled rectangle. Same inclusive box and same negative-w/h handling as
 * st7305_rect.
 *
 * By far the fastest primitive here, and it has a fast path worth aiming at:
 * when the box covers whole bytes it degenerates to memset, because one byte is
 * then a solid 2x4 block. That needs x even, w even, y a multiple of 4 and h a
 * multiple of 4. A full-height fill (h = 300 from y = 0) collapses further into
 * ONE memset across the whole span. Miss the alignment and it falls back to
 * masked per-column spans - still fast, but not memset-fast. */
void st7305_fill_rect(st7305_t *d, int x, int y, int w, int h, st7305_color_t c);

/* Midpoint circle outline, 1 px wide. r == 0 draws a single pixel at the
 * centre; r < 0 draws nothing. */
void st7305_circle     (st7305_t *d, int cx, int cy, int r, st7305_color_t c);

/* Filled disc. r == 0 is a single pixel, r < 0 draws nothing.
 *
 * Emitted as vertical spans - the fast axis - so per pixel covered this is much
 * cheaper than the outline. Some columns get written twice; masked writes are
 * idempotent for a single colour, which costs less than deduplicating them. */
void st7305_fill_circle(st7305_t *d, int cx, int cy, int r, st7305_color_t c);

/* Triangle outline: three st7305_line calls, inheriting their behaviour exactly
 * - including the far-off-screen iteration cost.
 *
 * MIND THE ASYMMETRY with st7305_fill_triangle: this function has no coordinate
 * limit, that one silently rejects vertices beyond +/-16000. Outlining and
 * filling the same very large triangle will not agree. */
void st7305_triangle     (st7305_t *d, int x0, int y0, int x1, int y1,
                          int x2, int y2, st7305_color_t c);

/* Filled triangle. Vertices may be given in any order; they are sorted
 * internally. Swept by COLUMN rather than by row, because the fast axis is
 * vertical.
 *
 * SILENTLY DRAWS NOTHING if any vertex lies outside +/-16000 on either axis:
 * edge interpolation is 16.16 fixed point and (y << 16) would overflow int32_t.
 * This rejects the WHOLE triangle - it is not a clip, and it is the one place
 * in this API where an off-panel coordinate costs you more than the off-panel
 * part. Clamp vertices that can get large. */
void st7305_fill_triangle(st7305_t *d, int x0, int y0, int x1, int y1,
                          int x2, int y2, st7305_color_t c);

/* ========================================================================== */
#ifdef ST7305_GFX_IMPLEMENTATION

/* Vertex coordinates beyond this make (y << 16) overflow int32_t in the
 * triangle DDA, so such triangles are rejected outright. */
#define ST7305__VMAX 16000

/* --- coordinate mapping --------------------------------------------------- */
/*   iy  = 299 - y
 *   idx = (x >> 1) * 75 + (iy >> 2)
 *   bit = 7 - (((iy & 3) << 1) | (x & 1))                                    */

static inline size_t st7305__idx(int x, int y)
{
    int iy = (ST7305_H - 1) - y;
    return (size_t)(x >> 1) * ST7305_STRIDE + (size_t)(iy >> 2);
}

static inline uint8_t st7305__mask(int x, int y)
{
    int iy = (ST7305_H - 1) - y;
    return (uint8_t)(1u << (7 - (((iy & 3) << 1) | (x & 1))));
}

/* Mask covering a contiguous local_y range [a..b] at an EVEN column (lx = 0).
 * Bit for local_y k is 1 << (7 - 2k). Shift right by lx for odd columns.
 * Entries with a > b are unused. */
static const uint8_t st7305__vm[4][4] = {
    { 0x80, 0xA0, 0xA8, 0xAA },
    { 0x00, 0x20, 0x28, 0x2A },
    { 0x00, 0x00, 0x08, 0x0A },
    { 0x00, 0x00, 0x00, 0x02 }
};

/* --- basics --------------------------------------------------------------- */

void st7305_clear(st7305_t *d, st7305_color_t c)
{
    memset(d->fb, c ? 0xFF : 0x00, ST7305_FB_SIZE);
}

void st7305_pixel(st7305_t *d, int x, int y, st7305_color_t c)
{
    if ((unsigned)x >= (unsigned)ST7305_W) return;
    if ((unsigned)y >= (unsigned)ST7305_H) return;
    size_t  i = st7305__idx(x, y);
    uint8_t m = st7305__mask(x, y);
    if (c) d->fb[i] |= m; else d->fb[i] &= (uint8_t)~m;
}

/* --- span helpers --------------------------------------------------------- */
/* These assume ORDERED, ALREADY-CLIPPED arguments. Clipping and normalising
 * is the public API's job, done once per call rather than once per span. */

/* Preconditions: 0 <= x < ST7305_W, 0 <= y0 <= y1 < ST7305_H.
 * Fast axis: the bytes for one column run contiguously. */
static void st7305__vspan(uint8_t *fb, int x, int y0, int y1, st7305_color_t c)
{
    int ia = (ST7305_H - 1) - y1;          /* smallest iy */
    int ib = (ST7305_H - 1) - y0;          /* largest  iy */
    int ga = ia >> 2, gb = ib >> 2;
    int lx = x & 1;
    uint8_t *p = fb + (size_t)(x >> 1) * ST7305_STRIDE;

    if (ga == gb) {
        uint8_t m = (uint8_t)(st7305__vm[ia & 3][ib & 3] >> lx);
        if (c) p[ga] |= m; else p[ga] &= (uint8_t)~m;
        return;
    }

    uint8_t mf = (uint8_t)(st7305__vm[ia & 3][3] >> lx);
    uint8_t ml = (uint8_t)(st7305__vm[0][ib & 3] >> lx);
    uint8_t mm = (uint8_t)(0xAAu >> lx);
    int g;

    if (c) {
        p[ga] |= mf;
        for (g = ga + 1; g < gb; g++) p[g] |= mm;
        p[gb] |= ml;
    } else {
        p[ga] &= (uint8_t)~mf;
        for (g = ga + 1; g < gb; g++) p[g] &= (uint8_t)~mm;
        p[gb] &= (uint8_t)~ml;
    }
}

/* Preconditions: 0 <= y < ST7305_H, 0 <= x0 <= x1 < ST7305_W.
 * Slow axis: consecutive column pairs are ST7305_STRIDE bytes apart. */
static void st7305__hspan(uint8_t *fb, int y, int x0, int x1, st7305_color_t c)
{
    int iy = (ST7305_H - 1) - y;
    int ly = iy & 3;
    uint8_t *p    = fb + (iy >> 2);
    uint8_t even  = (uint8_t)(1u << (7 - (ly << 1)));
    uint8_t odd   = (uint8_t)(even >> 1);
    uint8_t both  = (uint8_t)(even | odd);
    int x = x0;

    if (x & 1) {                                   /* leading odd column */
        size_t i = (size_t)(x >> 1) * ST7305_STRIDE;
        if (c) p[i] |= odd; else p[i] &= (uint8_t)~odd;
        x++;
    }
    for (; x + 1 <= x1; x += 2) {                  /* whole column pairs */
        size_t i = (size_t)(x >> 1) * ST7305_STRIDE;
        if (c) p[i] |= both; else p[i] &= (uint8_t)~both;
    }
    if (x <= x1) {                                 /* trailing even column */
        size_t i = (size_t)(x >> 1) * ST7305_STRIDE;
        if (c) p[i] |= even; else p[i] &= (uint8_t)~even;
    }
}

static void st7305__vspan_clip(st7305_t *d, int x, int y0, int y1, st7305_color_t c)
{
    if ((unsigned)x >= (unsigned)ST7305_W) return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y0 < 0) y0 = 0;
    if (y1 > ST7305_H - 1) y1 = ST7305_H - 1;
    if (y0 > y1) return;
    st7305__vspan(d->fb, x, y0, y1, c);
}

static void st7305__hspan_clip(st7305_t *d, int y, int x0, int x1, st7305_color_t c)
{
    if ((unsigned)y >= (unsigned)ST7305_H) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x0 < 0) x0 = 0;
    if (x1 > ST7305_W - 1) x1 = ST7305_W - 1;
    if (x0 > x1) return;
    st7305__hspan(d->fb, y, x0, x1, c);
}

/* --- rectangles ----------------------------------------------------------- */

/* Normalise w/h to an inclusive, panel-clipped box.
 * Returns false if empty or entirely off-panel. */
static bool st7305__norm(int x, int y, int w, int h,
                         int *ox0, int *oy0, int *ox1, int *oy1)
{
    int x0, y0, x1, y1;
    if (w == 0 || h == 0) return false;
    if (w < 0) { x += w + 1; w = -w; }
    if (h < 0) { y += h + 1; h = -h; }
    x0 = x; y0 = y; x1 = x + w - 1; y1 = y + h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > ST7305_W - 1) x1 = ST7305_W - 1;
    if (y1 > ST7305_H - 1) y1 = ST7305_H - 1;
    if (x0 > x1 || y0 > y1) return false;
    *ox0 = x0; *oy0 = y0; *ox1 = x1; *oy1 = y1;
    return true;
}

void st7305_rect(st7305_t *d, int x, int y, int w, int h, st7305_color_t c)
{
    int ex0, ey0, ex1, ey1;
    if (w == 0 || h == 0) return;
    if (w < 0) { x += w + 1; w = -w; }
    if (h < 0) { y += h + 1; h = -h; }
    ex0 = x; ey0 = y; ex1 = x + w - 1; ey1 = y + h - 1;

    st7305__hspan_clip(d, ey0, ex0, ex1, c);
    if (ey1 != ey0) st7305__hspan_clip(d, ey1, ex0, ex1, c);
    st7305__vspan_clip(d, ex0, ey0, ey1, c);
    if (ex1 != ex0) st7305__vspan_clip(d, ex1, ey0, ey1, c);
}

void st7305_fill_rect(st7305_t *d, int x, int y, int w, int h, st7305_color_t c)
{
    int x0, y0, x1, y1, ia, ib, ga, gb, run, p0, p1, q0, q1, p;
    bool aligned;
    uint8_t v;

    if (!st7305__norm(x, y, w, h, &x0, &y0, &x1, &y1)) return;

    ia = (ST7305_H - 1) - y1;
    ib = (ST7305_H - 1) - y0;
    ga = ia >> 2; gb = ib >> 2;
    run = gb - ga + 1;
    aligned = ((ia & 3) == 0) && ((ib & 3) == 3);
    v = c ? 0xFF : 0x00;

    p0 = x0 >> 1; p1 = x1 >> 1;
    q0 = p0; q1 = p1;

    /* Partial edge column pairs need a single-column span each. */
    if ((p0 << 1) < x0)       { st7305__vspan(d->fb, x0, y0, y1, c); q0 = p0 + 1; }
    if (((p1 << 1) + 1) > x1) { st7305__vspan(d->fb, x1, y0, y1, c); q1 = p1 - 1; }
    if (q0 > q1) return;

    if (aligned) {
        if (run == ST7305_STRIDE) {
            /* Full height: the runs join up across pairs -> a single memset. */
            memset(d->fb + (size_t)q0 * ST7305_STRIDE, v,
                   (size_t)(q1 - q0 + 1) * ST7305_STRIDE);
        } else {
            for (p = q0; p <= q1; p++)
                memset(d->fb + (size_t)p * ST7305_STRIDE + ga, v, (size_t)run);
        }
    } else {
        for (p = q0; p <= q1; p++) {
            st7305__vspan(d->fb, (p << 1),     y0, y1, c);
            st7305__vspan(d->fb, (p << 1) + 1, y0, y1, c);
        }
    }
}

/* --- lines ---------------------------------------------------------------- */

void st7305_line(st7305_t *d, int x0, int y0, int x1, int y1, st7305_color_t c)
{
    int dx, dy, sx, sy, err;
    bool inside;

    if (x0 == x1) { st7305__vspan_clip(d, x0, y0, y1, c); return; }
    if (y0 == y1) { st7305__hspan_clip(d, y0, x0, x1, c); return; }

    dx = x1 - x0; dy = y1 - y0;
    sx = dx > 0 ? 1 : -1;
    sy = dy > 0 ? 1 : -1;
    dx = dx > 0 ? dx : -dx;
    dy = dy > 0 ? dy : -dy;
    err = dx - dy;

    /* One test up front instead of a bounds check per pixel. */
    inside = (unsigned)x0 < (unsigned)ST7305_W &&
             (unsigned)y0 < (unsigned)ST7305_H &&
             (unsigned)x1 < (unsigned)ST7305_W &&
             (unsigned)y1 < (unsigned)ST7305_H;

    for (;;) {
        if (inside) {
            size_t  i = st7305__idx(x0, y0);
            uint8_t m = st7305__mask(x0, y0);
            if (c) d->fb[i] |= m; else d->fb[i] &= (uint8_t)~m;
        } else {
            st7305_pixel(d, x0, y0, c);
        }
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = err << 1;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0 += sy; }
        }
    }
}

/* --- circles -------------------------------------------------------------- */

void st7305_circle(st7305_t *d, int cx, int cy, int r, st7305_color_t c)
{
    int x, y, e;
    if (r < 0) return;
    if (r == 0) { st7305_pixel(d, cx, cy, c); return; }
    x = 0; y = r; e = 1 - r;
    while (x <= y) {
        st7305_pixel(d, cx + x, cy + y, c); st7305_pixel(d, cx - x, cy + y, c);
        st7305_pixel(d, cx + x, cy - y, c); st7305_pixel(d, cx - x, cy - y, c);
        st7305_pixel(d, cx + y, cy + x, c); st7305_pixel(d, cx - y, cy + x, c);
        st7305_pixel(d, cx + y, cy - x, c); st7305_pixel(d, cx - y, cy - x, c);
        x++;
        if (e < 0) e += 2 * x + 1;
        else { y--; e += 2 * (x - y) + 1; }
    }
}

/* Column-major fill, emitting vertical spans from BOTH midpoint symmetry
 * families. dx rises by 1 and dy falls by at most 1 per step, so every integer
 * column offset in [0,r] is covered - no gaps. A few columns are emitted twice;
 * masked writes are idempotent for a single colour, which is cheaper than
 * deduplicating. */
void st7305_fill_circle(st7305_t *d, int cx, int cy, int r, st7305_color_t c)
{
    int x, y, e;
    if (r < 0) return;
    x = 0; y = r; e = 1 - r;
    while (x <= y) {
        st7305__vspan_clip(d, cx + x, cy - y, cy + y, c);
        if (x) st7305__vspan_clip(d, cx - x, cy - y, cy + y, c);
        if (y != x) {
            st7305__vspan_clip(d, cx + y, cy - x, cy + x, c);
            if (y) st7305__vspan_clip(d, cx - y, cy - x, cy + x, c);
        }
        x++;
        if (e < 0) e += 2 * x + 1;
        else { y--; e += 2 * (x - y) + 1; }
    }
}

/* --- triangles ------------------------------------------------------------ */

static bool st7305__vok(int v)
{
    return v >= -ST7305__VMAX && v <= ST7305__VMAX;
}

void st7305_triangle(st7305_t *d, int x0, int y0, int x1, int y1,
                     int x2, int y2, st7305_color_t c)
{
    st7305_line(d, x0, y0, x1, y1, c);
    st7305_line(d, x1, y1, x2, y2, c);
    st7305_line(d, x2, y2, x0, y0, c);
}

/* Transposed scanline: sweeps by COLUMN because the fast axis is vertical.
 * Edge interpolation is 16.16 fixed point, so there is no division per column.
 * Relies on arithmetic right shift of negative values (guaranteed by GCC and
 * Clang, which covers the xtensa and riscv ESP toolchains). */
void st7305_fill_triangle(st7305_t *d, int x0, int y0, int x1, int y1,
                          int x2, int y2, st7305_color_t c)
{
    int t;
    int32_t yl, dl;

    if (!st7305__vok(x0) || !st7305__vok(y0) ||
        !st7305__vok(x1) || !st7305__vok(y1) ||
        !st7305__vok(x2) || !st7305__vok(y2)) return;

    /* sort so that x0 <= x1 <= x2 */
    if (x0 > x1) { t=x0; x0=x1; x1=t;  t=y0; y0=y1; y1=t; }
    if (x1 > x2) { t=x1; x1=x2; x2=t;  t=y1; y1=y2; y2=t; }
    if (x0 > x1) { t=x0; x0=x1; x1=t;  t=y0; y0=y1; y1=t; }

    if (x0 == x2) {                        /* degenerate: a single column */
        int lo = y0, hi = y0;
        if (y1 < lo) lo = y1;
        if (y1 > hi) hi = y1;
        if (y2 < lo) lo = y2;
        if (y2 > hi) hi = y2;
        st7305__vspan_clip(d, x0, lo, hi, c);
        return;
    }

    yl = (int32_t)y0 << 16;                                     /* long edge x0->x2 */
    dl = (int32_t)((((int64_t)(y2 - y0)) << 16) / (x2 - x0));

    if (x1 > x0) {                         /* segment 1: x0 .. x1-1 */
        int32_t ys = (int32_t)y0 << 16;
        int32_t ds = (int32_t)((((int64_t)(y1 - y0)) << 16) / (x1 - x0));
        int x;
        for (x = x0; x < x1; x++) {
            int a = (int)(yl >> 16), b = (int)(ys >> 16);
            st7305__vspan_clip(d, x, a < b ? a : b, a < b ? b : a, c);
            yl += dl; ys += ds;
        }
    } else {                               /* x0 == x1: vertical left edge */
        st7305__vspan_clip(d, x0, y0 < y1 ? y0 : y1, y0 < y1 ? y1 : y0, c);
    }

    if (x2 > x1) {                         /* segment 2: x1 .. x2 */
        int32_t ys = (int32_t)y1 << 16;
        int32_t ds = (int32_t)((((int64_t)(y2 - y1)) << 16) / (x2 - x1));
        int x;
        for (x = x1; x <= x2; x++) {
            int a = (int)(yl >> 16), b = (int)(ys >> 16);
            st7305__vspan_clip(d, x, a < b ? a : b, a < b ? b : a, c);
            yl += dl; ys += ds;
        }
    } else {                               /* x1 == x2: vertical right edge */
        int lo = y1 < y2 ? y1 : y2, hi = y1 < y2 ? y2 : y1;
        int a = (int)(yl >> 16);
        if (a < lo) lo = a;
        if (a > hi) hi = a;
        st7305__vspan_clip(d, x2, lo, hi, c);
    }
}

/* --- panel protocol ------------------------------------------------------- */

static void st7305__cmd(st7305_t *d, uint8_t c)
{
    d->io.xfer(d->io.ctx, 0, &c, 1);
}

static void st7305__dat(st7305_t *d, const uint8_t *b, size_t n)
{
    if (n) d->io.xfer(d->io.ctx, 1, b, n);
}

/* Records of {cmd, len, data...}. Panel-specific power, gate-timing and
 * waveform settings - do not tune without a reason as good as the one below.
 * Split around the 0x11 sleep-out delay.
 *
 * These are the values sent by BOTH vendor paths (ST7305_U8g2.cpp fullInit and
 * display_bsp.cpp RLCD_Init), which agree exactly - with ONE deliberate
 * deviation, at 0xB3. See the note there. */
static const uint8_t st7305__init_a[] = {
    /* 0xD6 = NVM Load Control. Parameters decode as
     *     1st: [0 0 0 1 0 VS_EN ID_EN 1]      2nd: [0 0 0 0 0 NRDTIME NRDSLP 0]
     *
     * 0x17 sets VS_EN = 1, "enable Source High/Low Voltage load from NVM", and
     * the 0x02 second parameter sets NRDSLP = 1, "NVM load will be triggered by
     * Sleep Out". Together they reload the factory OTP source voltages at the
     * 0x11 sleep-out below - which comes AFTER the 0xC1/0xC2/0xC4/0xC5 writes,
     * silently discarding them. With 0x17 the panel shows whatever contrast
     * Sitronix burned in, and the four voltage registers below are dead code.
     *
     * 0x17 is the chip's own reset default, which is the tell: no vendor path
     * ever considered this register. Mainline U8g2 sends 0x13 in BOTH the
     * 168x384 and 300x400 profiles while keeping 0x17 in the 200x200 profile
     * that does not retune voltages - it had to, or its values would not survive.
     *
     * TRIED 0x13, REVERTED (2026-08-26). Clearing VS_EN in isolation is a TRAP.
     * It does not "restore" anything - it promotes the vendor's 0xC1/0xC2/0xC4/
     * 0xC5 values from dead code to live drive levels FOR THE FIRST TIME. Those
     * values have therefore never driven this panel, are unvalidated by anyone,
     * and decode to a 0.8 V polarity asymmetry with 0.4 V of common-mode DC (see
     * the note on them below). On hardware the result was WORSE flicker.
     *
     * If you clear VS_EN you MUST replace 0xC1/0xC2/0xC4/0xC5 in the SAME change
     * with a set that is actually balanced - U8g2's, quoted below. The NVM load
     * bit and the voltage block are one decision, not two.
     * See ST7305_BUFFER_LAYOUT.md section 6.1. */
    0xD6,  2, 0x17, 0x02,

    0xD1,  1, 0x01,

    /* 0xC0 = Gate Voltage Control, by lookup table (not a formula):
     *     0x11 -> VGH  16.5 V        0x04 -> VGL  -7.0 V
     * Datasheet default is 0x0E/0x0A = 15.0 / -10.0 V; U8g2 uses 0x12/0x0A =
     * 17.0 / -10.0 V. Our VGL is 3 V shallower than either, leaving only ~3 V of
     * TFT off-margin against VSHN (-4.0 V) where U8g2 has ~5.7 V. Thin enough to
     * permit subthreshold leakage and pixel droop. Suspected secondary cause of
     * washed-out blacks; untested. */
    0xC0,  2, 0x11, 0x04,

    /* Source drive levels. Four bytes each = four voltage modes; 0xC9 in init_b
     * selects mode 1. Datasheet formulas:
     *     VSHP = 3.7 + 0.02n     VSLP = 0.02n
     *     VSHN = -2.5 - 0.02n    VSLN = 1 - 0.02n
     *
     * These vendor values therefore mean
     *     VSHP +5.80 V   VSLP +0.50 V   VSHN -4.00 V   VSLN +0.50 V
     * -> positive swing 5.30 V, negative swing 4.50 V, and 0.40 V of common-mode
     * offset between lit and unlit pixels. With frame inversion (0xB8 = 0x29)
     * that asymmetry alternates every frame, which is a standing DC across the
     * liquid crystal - the classic cause of contrast that decays after power-on
     * plus luminance ripple at fps/2 (~13 Hz here).
     *
     * U8g2's set for this panel is instead exactly balanced: 4.20 V on both
     * polarities with 0.30 V common mode on both lit and unlit (and 4.28/0.30
     * for its mode 2). That precision is clearly deliberate.
     *
     * REMEMBER: with 0xD6 = 0x17 above, none of these four writes survive the
     * sleep-out - the panel runs factory OTP levels instead. That also means the
     * numbers here have never been exercised on real glass, so do not treat them
     * as a validated baseline. Changing them alone does nothing; changing 0xD6
     * alone activates values nobody has checked. Move both together or neither. */
    0xC1,  4, 0x69, 0x69, 0x69, 0x69,
    0xC2,  4, 0x19, 0x19, 0x19, 0x19,
    0xC4,  4, 0x4B, 0x4B, 0x4B, 0x4B,
    0xC5,  4, 0x19, 0x19, 0x19, 0x19,

    /* 0xD8 = OSC Setting: [OSCEN 0 OSCSW2 0 0 OSCSW1 OSCSW0 0], then a fixed
     * 0xE9. OSCSW sets the MAXIMUM HPM frame rate: 000 -> 51 Hz, 111 -> 32 Hz.
     *
     * DEVIATES FROM THE VENDOR, which sends 0x80 (OSCSW 000, max 51 Hz). We send
     * 0xA6 (OSCSW 111, max 32 Hz), which 0xB2's HFRA = 0 halves to 16 Hz.
     *
     * WHY, measured on hardware 2026-08-27 by examples/arduino/st7305_lfra:
     * this panel's blacks are a function of FRAME RATE and nothing else. Clean at
     * every rate from 0.25 Hz up to and including 16.96 Hz; washed out and
     * flickering at 25.94 Hz; worse at 32; worse again at 51. The threshold sits
     * somewhere in 17..26 Hz and 16 Hz is the fastest setting below it that the
     * controller offers. The vendor's 0x80 puts us at 25.5 Hz - just the wrong
     * side of it, which is the whole story of the washed-out blacks.
     *
     * This also RESOLVES a long-standing mismatch rather than creating one. 0xB3
     * below is U8g2's, and its gate-EQ counts are denominated in oscillator
     * ticks that U8g2 tuned against exactly this 0xA6. The init was a hybrid for
     * as long as we sent 0x80; it is now self-consistent for the first time.
     *
     * Note 0xA6 was TRIED and REVERTED on 2026-08-26 - but that attempt paired it
     * with 0xB2 = 0x12 for a genuine 32 Hz (above the threshold) AND was stacked
     * on a 0xD6 change that had just made the drive voltages live. Wrong rate,
     * unreadable conditions. It is the pairing with 0xB2 = 0x02 that matters.
     *
     * COST: 16 Hz instead of 25.5. If you need more than that, you need a rate
     * between 16 and 25.5 - OSCSW is a 3-bit field and only 000 and 111 are
     * documented here, so the other six values are unexplored.
     *
     * 0xD8, 0xB2 and 0xB3 are ONE SETTING SPREAD OVER THREE REGISTERS. Layout doc
     * 5a and 6.4. */
    0xD8,  2, 0xA6, 0xE9,

    /* 0xB2 = Frame Rate Control: [0 0 0 HFRA 0 LFRA2 LFRA1 LFRA0].
     * HFRA picks full (1) or half (0) of the OSC's max HPM rate from 0xD8.
     * LFRA indexes the LPM rate: 0=0.25 1=0.5 2=1 3=2 4=4 5=8 Hz.
     *
     * 0x02 = HFRA 0, LFRA 2 -> half of 0xD8's 32 Hz max = 16 Hz HPM, 1 Hz LPM.
     * Measured 16.96 Hz and clean. (Against the vendor's old 0xD8 = 0x80 this
     * same byte gave 25.5 Hz predicted / 25.95 measured, which is what confirmed
     * the decode in the first place - and what looked washed out.)
     *
     * LFRA 2 -> 1 Hz is the IDLE rate only. Writing GRAM in LPM triggers a
     * refresh that runs at ~8 Hz regardless of LFRA, so this value gives you low
     * power when the screen is static and ~8 Hz the moment you flush. Raising
     * LFRA costs idle power and buys nothing. Measured 2026-08-27, layout 5a.
     *
     * 0x12 has now been tried TWICE and been worse both times:
     *   - alone, against 0xD8 = 0x80, where HFRA = 1 asks for the full 51 Hz;
     *   - paired with 0xD8 = 0xA6 for a genuine 32 Hz (2026-08-26).
     * Both are above the ~17..26 Hz contrast threshold, so both are expected to
     * look bad. Do not try it a third time. */
    0xB2,  1, 0x02,

    /* 0xB3 = Update Period Gate EQ Control in HPM (0xB4 is the LPM twin).
     *
     * DEVIATES FROM THE VENDOR. Both vendor paths send
     *     E5 F6 05 46 77 77 77 77 76 45
     * which is byte-for-byte the variant mainline U8g2 has commented out in
     * u8x8_d_st7305.c with the note "however this has lower contrast". The
     * value below is the one U8g2 actually ships for this panel.
     *
     * This value measured better than the vendor's on this board, which is the
     * only hardware evidence anyone has for either, so it stays.
     *
     * BUT NOTE WHAT IT DOES NOT EXPLAIN. The old note here read the HPM-washed /
     * LPM-clean asymmetry as evidence that an HPM-only register like this one was
     * at fault. That inference is now dead: HPM at 16 Hz is clean (2026-08-27),
     * so the asymmetry was frame RATE all along, and 0x38-vs-0x39 only ever
     * mattered because it changed the rate. Gate EQ is not the contrast story.
     * The banding may still be the "One Line Interlace" of 0xB8 = 0x29 - two
     * interlace fields settling differently when drive is marginal, turned into
     * flicker at fps/2 by frame inversion - but that is now an explanation of a
     * symptom we no longer see, at a rate we no longer run.
     *
     * Decoded: bytes 3..10 are pairs of GPCHPUn[2:0] gate pulse-chop counts, one
     * per gate group. The vendor's ramp 0,5,4,6,7,7,... tapering to 7,6,4,5 gives
     * less EQ time than the value below, which sits at the 7 maximum on all but
     * the first and last - which is exactly why U8g2 annotates the vendor variant
     * "lower contrast".
     *
     * RESOLVED 2026-08-27: these counts are in oscillator units, and U8g2 tuned
     * them against its own 0xD8 = 0xA6. For a long time we sent 0xB3 = U8g2's
     * against 0xD8 = the vendor's, so the absolute EQ timing was neither party's
     * - a documented hybrid. 0xD8 is now 0xA6 (see its note above, changed for
     * an unrelated reason: frame rate), so this deviation and the oscillator it
     * was tuned for finally agree. Nothing here needs to change. */
    0xB3, 10, 0xE5, 0xF6, 0x17, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x71,

    0xB4,  8, 0x05, 0x46, 0x77, 0x77, 0x77, 0x77, 0x76, 0x45,
    0x62,  3, 0x32, 0x03, 0x1F,
    0xB7,  1, 0x13,
    0xB0,  1, 0x64,
    0x11,  0
};

/* Decoded, for the ones that are not self-evident:
 *   0xC9 = 0x00  Source Voltage Select -> mode 1 (VSHPA1/VSLPA1/VSHNA1/VSLNA1),
 *                i.e. the first byte of each 0xC1/0xC2/0xC4/0xC5 group above.
 *   0xB9 = 0x20  Gamma Mode: bit 5 = 1 = Mono (0 would be 4-level greyscale).
 *                Correct as-is - a 4GS setting here WOULD render 1 bpp as grey.
 *   0xB8 = 0x29  Panel Setting [0 DOTINV1 DOTINV0 0 DPSCN1 DPSCN0 LAY1 LAY0] =
 *                1-dot inversion, frame-interval gate scan, One Line Interlace.
 *                The interlace is why marginal drive shows as horizontal banding.
 *   0xD0 = 0xFF  Auto Power Down enabled (bit 7). Chip default. */
static const uint8_t st7305__init_b[] = {
    0xC9, 1, 0x00,
    0x36, 1, 0x48,
    0x3A, 1, 0x11,
    0xB9, 1, 0x20,
    0xB8, 1, 0x29,
    0x21, 0,
    0x2A, 2, 0x12, 0x2A,
    0x2B, 2, 0x00, 0xC7,
    0x35, 1, 0x00,
    0xD0, 1, 0xFF,
    0x38, 0,
    0x29, 0
};

static void st7305__run(st7305_t *d, const uint8_t *t, size_t n)
{
    size_t i = 0;
    while (i + 2 <= n) {
        uint8_t cmd = t[i++];
        uint8_t len = t[i++];
        st7305__cmd(d, cmd);
        st7305__dat(d, t + i, len);
        i += len;
    }
}

void st7305_flush(st7305_t *d)
{
    static const uint8_t caset[2] = { 0x12, 0x2A };   /* full column window */
    static const uint8_t raset[2] = { 0x00, 0xC7 };   /* full page window   */
    st7305__cmd(d, 0x2A); st7305__dat(d, caset, 2);
    st7305__cmd(d, 0x2B); st7305__dat(d, raset, 2);
    st7305__cmd(d, 0x2C); st7305__dat(d, d->fb, ST7305_FB_SIZE);
}

/* 0x38 = high power mode, 0x39 = low power mode.
 *
 * MEASURED on ESP32-S3-RLCD-4.2 by counting TE edges on GPIO 6 in each mode
 * (examples/arduino/st7305_speed): against the vendor's old 0xD8 = 0x80,
 * 0x38 -> 25.95 Hz scan (38.53 ms period) and 0x39 -> 0.97 Hz (1.028 s), and
 * the change reverses cleanly. So this pairing is confirmed, not inferred. The
 * frame-rate registers (0xB2/0xB3/0xB4) do NOT need reprogramming - the bare
 * opcode does it.
 *
 * With the 0xD8 = 0xA6 we now ship, 0x38 is 16.96 Hz instead. BOTH MODES ARE
 * NOW BELOW THIS PANEL'S CONTRAST THRESHOLD, so unlike before, picking a mode
 * is purely a power/refresh trade and no longer affects how it looks:
 *
 *   ST7305_FAST  16.96 Hz constant, higher power
 *   ST7305_SLOW   1 Hz idle, ~8 Hz while you are writing (see 0xB2), lowest
 *                 power - the better default for static or slow content
 *
 * Layout doc 5a. */
void st7305_set_mode(st7305_t *d, st7305_mode_t m)
{
    st7305__cmd(d, m == ST7305_SLOW ? 0x39 : 0x38);
}

/* The two fields live in one byte - 0xB2 is [0 0 0 HFRA 0 LFRA2 LFRA1 LFRA0] -
 * and the bus is write-only, so the half this call is not changing has to come
 * from the shadow copy. Without that, selecting an LPM rate would silently
 * knock HPM back to whatever HFRA bit happened to be in the constant.
 *
 * The 0xE9 second parameter of 0xD8 is fixed in every implementation anyone has
 * and is not a rate field; it is written unchanged so this stays a faithful
 * two-parameter command rather than a partial one.
 *
 * osc == 0 means the device never went through st7305_init. 0x00 would clear
 * OSCEN and stop the oscillator, which is the one outcome here that needs a
 * camera and a reflash to diagnose, so fall back to the init default instead. */
void st7305_set_fps(st7305_t *d, st7305_fps_t f)
{
    uint8_t hi = (uint8_t)((unsigned)f >> 8);
    uint8_t lo = (uint8_t)((unsigned)f & 0xFFu);
    uint8_t d8[2];

    if (hi) {                                   /* HPM: new OSCSW and HFRA */
        d->osc = hi;
        d->b2  = (uint8_t)(lo | (d->b2 & 0x07u));
    } else {                                    /* LPM: new LFRA only */
        d->b2  = (uint8_t)((d->b2 & 0x10u) | lo);
    }
    if (!d->osc) d->osc = 0xA6;

    d8[0] = d->osc;
    d8[1] = 0xE9;
    st7305__cmd(d, 0xD8); st7305__dat(d, d8, 2);
    st7305__cmd(d, 0xB2); st7305__dat(d, &d->b2, 1);
    st7305__cmd(d, hi ? 0x38 : 0x39);
}

void st7305_set_display(st7305_t *d, bool visible)
{
    st7305__cmd(d, visible ? 0x29 : 0x28);
}

void st7305_init(st7305_t *d, const st7305_io_t *io, uint8_t *fb)
{
    d->io = *io;
    d->fb = fb;
    /* Must track what st7305__init_a actually sends for 0xD8 and 0xB2, or the
     * first st7305_set_fps preserves the wrong half of a shared register. */
    d->osc = 0xA6;
    d->b2  = 0x02;

    if (d->io.reset) {
        d->io.reset(d->io.ctx, 1); d->io.delay_ms(d->io.ctx, 50);
        d->io.reset(d->io.ctx, 0); d->io.delay_ms(d->io.ctx, 20);
        d->io.reset(d->io.ctx, 1); d->io.delay_ms(d->io.ctx, 50);
    }

    st7305__run(d, st7305__init_a, sizeof st7305__init_a);
    d->io.delay_ms(d->io.ctx, 120);          /* after 0x11 sleep-out */
    st7305__run(d, st7305__init_b, sizeof st7305__init_b);

    st7305_clear(d, ST7305_WHITE);
    st7305_flush(d);
}

#endif /* ST7305_GFX_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif
#endif /* ST7305_GFX_H */
