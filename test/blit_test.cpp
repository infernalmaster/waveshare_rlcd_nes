/* blit_test.cpp - host check for ST7305Tft::blitIndexed() and
 * ST7305Tft::blitIndexedStretched().
 *
 * Both write whole GRAM bytes using precomputed bit positions and skip every
 * bounds test, which makes them fast and makes a mistake in the coordinate
 * arithmetic invisible until it is on the glass. So each is asserted
 * byte-identical to a naive per-pixel reference here - the same rule the
 * library's own fast paths follow (AGENTS.md section 4).
 *
 * The reference below is transcribed from the layout doc using / and %, not
 * from the wrapper's shifts, so an error in one is not an error in both.
 *
 * Arduino IDE ignores sketch subfolders other than src/ and data/, so this
 * folder is not part of the sketch build.
 *
 *   cd waveshare_rlcd_nes
 *   cc -x c++ -std=c++17 -O2 -Wall -Wextra -I. -Itest/arduino_stubs \
 *      -include Arduino.h test/blit_test.cpp test/stub_impl.cpp st7305_tft.cpp \
 *      -o /tmp/blit_test && /tmp/blit_test
 */
#include <stdio.h>
#include <string.h>

#include "st7305_tft.h"

static int checks = 0;
static int failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

/* --- the reference ---------------------------------------------------------
 * Straight from ST7305_BUFFER_LAYOUT.md: landscape, iy = 299 - y, one byte per
 * 2 columns x 4 rows, bit 7 first. Division and modulo throughout, so this is
 * not a re-spelling of the shifts under test. */
static size_t ref_index(int x, int y)
{
    const int iy = (ST7305_H - 1) - y;
    return (size_t)(x / 2) * ST7305_STRIDE + (size_t)(iy / 4);
}

static int ref_bit(int x, int y)
{
    const int iy = (ST7305_H - 1) - y;
    return 7 - (((iy % 4) * 2) + (x % 2));
}

static const int ref_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/* The other masks, transcribed the same way. These are the ONLY numbers this
 * test shares with the implementation - everything about how they are scaled,
 * replicated across an 8x8 cell and turned into a bit position is restated
 * here from the layout document rather than copied. */
static const int ref_bayer2[2][2] = {
    { 0, 2 },
    { 3, 1 }
};

static const int ref_bayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

static const int ref_cluster4[4][4] = {
    { 12,  5,  6, 13 },
    {  4,  0,  1,  7 },
    { 11,  3,  2,  8 },
    { 15, 10,  9, 14 }
};

static const int ref_blue8[8][8] = {
    { 50, 43,  9, 29, 12, 26, 45, 30 },
    { 25,  1, 55, 34, 49,  3, 57, 14 },
    { 61, 38, 46, 15, 60, 18, 36,  7 },
    { 32, 16, 22,  5, 31, 40, 52, 44 },
    { 10, 54, 42, 59, 11, 24,  0, 21 },
    { 48,  6, 27, 37, 47, 56, 28, 63 },
    { 13, 33, 51,  2, 17,  8, 35, 39 },
    { 58, 19, 23, 62, 41, 53, 20,  4 }
};

/* Rec.601 on 8-bit channels - what setPaletteEntry() stores. */
static int ref_lum(int r, int g, int b)
{
    return (77 * r + 150 * g + 29 * b) >> 8;
}

/* The threshold at destination (x, y) under a given mode, and the tone a
 * palette index maps to. Both are rebuilt by ref_configure() before each case,
 * so every comparator below can stay mode-agnostic.
 *
 * Note ref_thr is indexed [y % 8][x % 8] EVEN FOR THE 4x4 AND 2x2 MASKS. That
 * is the point of the phase checks in main(): a 4x4 mask replicated into an
 * 8x8 cell must produce identical bytes to the 4x4 cell it replaces, and an
 * 8x8 one must not, so an origin whose phase is 4 rather than 0 is a real test
 * of the base offset the blits compute and a no-op for the old masks. */
static int ref_thr[8][8];
static int ref_tone[256];

/* Declared here rather than with the other fixtures because ref_configure()
 * below needs it. */
static int palette[256][3];

static void ref_configure(st7305_mono_t mode, bool invert)
{
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            switch (mode) {
            case ST7305_MONO_BAYER8:
                ref_thr[y][x] = ref_bayer8[y][x] * 4 + 2;      break;
            case ST7305_MONO_BLUE8:
                ref_thr[y][x] = ref_blue8[y][x] * 4 + 2;       break;
            case ST7305_MONO_BAYER2:
                ref_thr[y][x] = ref_bayer2[y % 2][x % 2] * 64 + 32; break;
            case ST7305_MONO_CLUSTER4:
                ref_thr[y][x] = ref_cluster4[y % 4][x % 4] * 16 + 8; break;
            case ST7305_MONO_THRESHOLD:
                ref_thr[y][x] = 127;                           break;
            default:
                ref_thr[y][x] = ref_bayer[y % 4][x % 4] * 16 + 8; break;
            }
        }
    }

    for (int i = 0; i < 256; i++) {
        const int l = ref_lum(palette[i][0], palette[i][1], palette[i][2]);
        ref_tone[i] = invert ? 255 - l : l;
    }
}

/* Floor to a multiple of 4, spelled without a mask. */
static int floor4(int v) { return v - (v % 4); }

/* --- fixtures -------------------------------------------------------------- */

static uint8_t image[240][256];
static uint8_t *lines[240];

static void apply_palette(ST7305Tft &tft)
{
    for (int i = 0; i < 256; i++)
        tft.setPaletteEntry(i, palette[i][0], palette[i][1], palette[i][2]);
}

/* Rebuilds the whole framebuffer one pixel at a time and returns how many of
 * the 15000 bytes disagree with what blitIndexed() produced. */
static int compare(const ST7305Tft &tft, int w, int h)
{
    static uint8_t ref[ST7305_FB_SIZE];
    memset(ref, 0x00, sizeof ref);

    const int cw = floor4(w);
    const int ch = floor4(h);
    const int x0 = floor4((ST7305_W - cw) / 2);
    const int y0 = floor4((ST7305_H - ch) / 2);

    for (int sy = 0; sy < ch; sy++) {
        for (int sx = 0; sx < cw; sx++) {
            const int x = x0 + sx;
            const int y = y0 + sy;

            if (ref_tone[lines[sy][sx]] > ref_thr[y % 8][x % 8])
                ref[ref_index(x, y)] |= (uint8_t)(1 << ref_bit(x, y));
        }
    }

    const uint8_t *got = tft.framebuffer();
    int diff = 0;
    for (size_t i = 0; i < ST7305_FB_SIZE; i++)
        if (got[i] != ref[i]) diff++;
    return diff;
}

/* The same, for blitIndexedStretched(): destination is the whole panel, source
 * coordinates come from centre-sampled nearest neighbour. Only the mapping is
 * restated from the spec - the GRAM index, the bit position and the dither
 * phase all still come from the layout document via ref_index/ref_bit. */
static int compare_stretched(const ST7305Tft &tft, int w, int srcY0, int srcH)
{
    static uint8_t ref[ST7305_FB_SIZE];
    memset(ref, 0x00, sizeof ref);

    for (int y = 0; y < ST7305_H; y++) {
        const int sy = srcY0 + ((2 * y + 1) * srcH) / (2 * ST7305_H);
        for (int x = 0; x < ST7305_W; x++) {
            const int sx = ((2 * x + 1) * w) / (2 * ST7305_W);

            if (ref_tone[lines[sy][sx]] > ref_thr[y % 8][x % 8])
                ref[ref_index(x, y)] |= (uint8_t)(1 << ref_bit(x, y));
        }
    }

    const uint8_t *got = tft.framebuffer();
    int diff = 0;
    for (size_t i = 0; i < ST7305_FB_SIZE; i++)
        if (got[i] != ref[i]) diff++;
    return diff;
}

/* Starts from an all-ONES framebuffer while the reference starts from zero, so
 * a byte the blit fails to write shows up as a difference. This blit is
 * supposed to cover all 15000 of them; that is the point of it. */
static void run_stretched_case(const char *label, int w, int srcY0, int srcH,
                               bool invert,
                               st7305_mono_t mode = ST7305_MONO_BAYER4)
{
    static ST7305Tft tft;
    char msg[208];

    apply_palette(tft);
    tft.setInvertVideo(invert);
    tft.setMonoMode(mode);
    tft.rebuildDither();
    tft.clear(ST7305_WHITE);
    ref_configure(mode, invert);

    tft.blitIndexedStretched(lines, w, srcY0, srcH);

    const int diff = compare_stretched(tft, w, srcY0, srcH);
    snprintf(msg, sizeof msg,
             "%s (%d wide, rows %d..%d, invert=%d, %s): %d of %d bytes differ",
             label, w, srcY0, srcY0 + srcH - 1, (int)invert,
             ST7305Tft::monoModeName(mode), diff, ST7305_FB_SIZE);
    check(diff == 0, msg);
}

/* Both sides start from an all-zero framebuffer, so a byte the blit writes
 * outside its own rectangle shows up as a difference rather than being
 * masked by whatever was there before. */
static void run_case(const char *label, int w, int h, bool invert,
                     st7305_mono_t mode = ST7305_MONO_BAYER4)
{
    static ST7305Tft tft;               /* ~50 KB - not a stack object */
    char msg[192];

    apply_palette(tft);
    tft.setInvertVideo(invert);
    tft.setMonoMode(mode);
    tft.rebuildDither();
    tft.clear(ST7305_BLACK);
    ref_configure(mode, invert);

    tft.blitIndexed(lines, w, h);

    const int diff = compare(tft, w, h);
    snprintf(msg, sizeof msg,
             "%s (%dx%d, invert=%d, %s): %d of %d bytes differ",
             label, w, h, (int)invert, ST7305Tft::monoModeName(mode), diff,
             ST7305_FB_SIZE);
    check(diff == 0, msg);
}

/* --- the error-diffusion path ----------------------------------------------
 *
 * A second implementation of the same rule, written the obvious way: a whole
 * 400x300 plane of error, every tap bounds-checked, and each pixel's bit set
 * the moment it is decided.
 *
 * It shares the weights and the clamp with the code under test, because there
 * is no independent way to restate those. What it therefore checks is
 * everything AROUND them, which is where this kind of code actually goes wrong:
 * that the serpentine direction flips on the right rows, that the rolling
 * three-row window is advanced and cleared correctly rather than carrying stale
 * error into the row two ahead, and above all that BANKING four rows of
 * decisions and assembling them into GRAM bytes afterwards puts every bit in
 * exactly the place writing it immediately would have. A single misplaced row
 * within a group would be invisible in a whole-image average and is a solid
 * block of wrong bytes here.
 * ------------------------------------------------------------------------- */
static int compare_diffused(const ST7305Tft &tft, int w, int srcY0, int srcH,
                            bool atkinson)
{
    static uint8_t ref[ST7305_FB_SIZE];
    /* Two spare rows below and two spare columns each side, so the taps that
     * fall off the picture have somewhere to land. */
    static int err[ST7305_H + 2][ST7305_W + 4];

    memset(ref, 0x00, sizeof ref);
    memset(err, 0, sizeof err);

    for (int y = 0; y < ST7305_H; y++) {
        const int sy  = srcY0 + ((2 * y + 1) * srcH) / (2 * ST7305_H);
        const int dir = (y % 2) ? -1 : 1;

        for (int n = 0; n < ST7305_W; n++) {
            const int x  = (y % 2) ? ST7305_W - 1 - n : n;
            const int sx = ((2 * x + 1) * w) / (2 * ST7305_W);
            const int c  = x + 2;           /* into the padded column space */

            int v = ref_tone[lines[sy][sx]] + (err[y][c] >> 4);
            if (v < -256) v = -256; else if (v > 511) v = 511;

            const int white = (v > 127);
            if (white) ref[ref_index(x, y)] |= (uint8_t)(1 << ref_bit(x, y));

            const int e = v - (white ? 255 : 0);
            if (atkinson) {
                err[y    ][c +     dir] += e * 2;
                err[y    ][c + 2 * dir] += e * 2;
                err[y + 1][c -     dir] += e * 2;
                err[y + 1][c          ] += e * 2;
                err[y + 1][c +     dir] += e * 2;
                err[y + 2][c          ] += e * 2;
            } else {
                err[y    ][c +     dir] += e * 7;
                err[y + 1][c -     dir] += e * 3;
                err[y + 1][c          ] += e * 5;
                err[y + 1][c +     dir] += e;
            }
        }
    }

    const uint8_t *got = tft.framebuffer();
    int diff = 0;
    for (size_t i = 0; i < ST7305_FB_SIZE; i++)
        if (got[i] != ref[i]) diff++;
    return diff;
}

static void run_diffused_case(const char *label, int w, int srcY0, int srcH,
                              bool invert, st7305_mono_t mode)
{
    static ST7305Tft tft;
    char msg[208];

    apply_palette(tft);
    tft.setInvertVideo(invert);
    tft.setMonoMode(mode);
    tft.rebuildDither();
    tft.clear(ST7305_WHITE);
    ref_configure(mode, invert);

    /* If the scratch could not be allocated, rebuildDither() has quietly moved
     * to BAY4 and the comparison below would be against the wrong algorithm
     * entirely - so say which failure it was. */
    if (tft.monoMode() != mode) {
        check(false, "diffusion mode fell back - no scratch buffers");
        return;
    }

    tft.blitIndexedStretched(lines, w, srcY0, srcH);

    const int diff = compare_diffused(tft, w, srcY0, srcH,
                                      mode == ST7305_MONO_ATKINSON);
    snprintf(msg, sizeof msg,
             "%s (%d wide, rows %d..%d, invert=%d, %s): %d of %d bytes differ",
             label, w, srcY0, srcY0 + srcH - 1, (int)invert,
             ST7305Tft::monoModeName(mode), diff, ST7305_FB_SIZE);
    check(diff == 0, msg);
}

/* How many of the 120000 destination pixels came out as paper. Used for the
 * tone-curve checks, which assert a DIRECTION rather than a number - restating
 * the pow() would only prove the formula was copied twice. */
static int paper_pixels(st7305_mono_t mode, int gamma, int contrast, int bright)
{
    static ST7305Tft tft;

    apply_palette(tft);
    tft.setInvertVideo(false);
    tft.setMonoMode(mode);
    tft.setGamma(gamma);
    tft.setContrast(contrast);
    tft.setBrightness(bright);
    tft.rebuildDither();
    tft.clear(ST7305_BLACK);
    tft.blitIndexedStretched(lines, 256, 8, 224);

    const uint8_t *fb = tft.framebuffer();
    int set = 0;
    for (size_t i = 0; i < ST7305_FB_SIZE; i++)
        for (int b = 0; b < 8; b++)
            if (fb[i] & (1 << b)) set++;
    return set;
}

int main(void)
{
    /* Channels that move at different rates, so a swapped luminance weight or
     * a swapped channel changes the result. */
    for (int i = 0; i < 256; i++) {
        palette[i][0] = (i * 7) & 0xFF;
        palette[i][1] = (i * 13 + 40) & 0xFF;
        palette[i][2] = (i * 29 + 90) & 0xFF;
    }

    /* Gradients, fine detail and flat fields: a dither-phase error, an
     * off-by-one row and a swapped column each move some byte in at least one
     * of these bands. */
    for (int y = 0; y < 240; y++) {
        lines[y] = image[y];
        for (int x = 0; x < 256; x++) {
            if (y < 60)       image[y][x] = (uint8_t)x;
            else if (y < 120) image[y][x] = (uint8_t)((x ^ y) & 0xFF);
            else if (y < 180) image[y][x] = 0x40;
            else              image[y][x] = (uint8_t)((x + y) & 0xFF);
        }
    }

    check(ST7305Tft::blitOriginX(256) == 72, "256 wide centres at x=72");
    check(ST7305Tft::blitOriginY(240) == 28, "240 tall centres at y=28");

    run_case("NES frame", 256, 240, false);
    run_case("NES frame inverted", 256, 240, true);
    run_case("small", 100, 80, false);
    run_case("size not a multiple of 4", 102, 83, false);
    run_case("single row group", 256, 4, false);

    /* --- the stretched path, which is what the game now goes through -------- */

    /* The NES frame as it is actually shipped: 256x224 across all 400x300. */
    run_stretched_case("NES 4:3 fullscreen", 256, 8, 224, false);
    run_stretched_case("NES 4:3 fullscreen inverted", 256, 8, 224, true);

    /* Two calls back to back must agree - the coordinate maps are cached
     * between calls, so a stale-cache bug only shows on the second one. */
    run_stretched_case("NES 4:3 fullscreen again", 256, 8, 224, false);

    /* Changing the source geometry has to invalidate those maps. Run through
     * three different shapes, checking each against its own reference; if the
     * maps were not rebuilt, the second and third would still be sampling the
     * first one's rows. */
    run_stretched_case("no crop", 256, 0, 240, false);
    run_stretched_case("heavy crop", 256, 40, 160, false);
    run_stretched_case("back to 4:3", 256, 8, 224, false);

    /* Downscale as well as up - 400 destination columns from 400+ source ones
     * exercises the map in the other direction. */
    run_stretched_case("narrow source", 64, 8, 224, false);

    /* --- the other ordered modes ------------------------------------------- */

    run_stretched_case("bayer8",    256, 8, 224, false, ST7305_MONO_BAYER8);
    run_stretched_case("bayer2",    256, 8, 224, false, ST7305_MONO_BAYER2);
    run_stretched_case("cluster4",  256, 8, 224, false, ST7305_MONO_CLUSTER4);
    run_stretched_case("blue8",     256, 8, 224, false, ST7305_MONO_BLUE8);
    run_stretched_case("threshold", 256, 8, 224, false, ST7305_MONO_THRESHOLD);
    run_stretched_case("blue8 inverted", 256, 8, 224, true,
                       ST7305_MONO_BLUE8);

    /* Switching back must rebuild the table, not leave the previous mode's
     * thresholds in it. */
    run_stretched_case("back to bayer4", 256, 8, 224, false,
                       ST7305_MONO_BAYER4);

    /* --- the 8x8 phase arithmetic, which is the whole risk in an 8x8 cell ----
     *
     * A 4x4 mask replicated across the 8x8 table cannot tell a phase of 0 from
     * a phase of 4, so the cases above prove nothing about the base offset the
     * blits compute. These do: each origin below lands the image on a different
     * quadrant of the dither cell, and under an 8x8 mask each therefore has a
     * different correct answer. Get the offset wrong in either axis and three
     * of the four still pass.
     *
     * blitIndexed() is the one that needs this - it centres, so its origin is
     * whatever the size makes it. The stretched blit always starts at 0,0. */
    check(ST7305Tft::blitOriginX(248) == 76, "248 wide centres at x=76");
    check(ST7305Tft::blitOriginY(232) == 32, "232 tall centres at y=32");

    run_case("phase 0,0", 256, 232, false, ST7305_MONO_BAYER8);
    run_case("phase 4,0", 248, 232, false, ST7305_MONO_BAYER8);
    run_case("phase 0,4", 256, 240, false, ST7305_MONO_BAYER8);
    run_case("phase 4,4", 248, 240, false, ST7305_MONO_BAYER8);
    run_case("phase 4,4 blue8", 248, 240, false, ST7305_MONO_BLUE8);

    /* --- error diffusion ---------------------------------------------------- */

    run_diffused_case("floyd-steinberg", 256, 8, 224, false,
                      ST7305_MONO_FLOYD);
    run_diffused_case("floyd-steinberg inverted", 256, 8, 224, true,
                      ST7305_MONO_FLOYD);
    run_diffused_case("atkinson", 256, 8, 224, false, ST7305_MONO_ATKINSON);
    run_diffused_case("atkinson inverted", 256, 8, 224, true,
                      ST7305_MONO_ATKINSON);

    /* Two in a row on the same object: the error plane has to be cleared at the
     * start of every call, or the second frame starts with the first one's
     * error still in it. */
    run_diffused_case("atkinson again", 256, 8, 224, false,
                      ST7305_MONO_ATKINSON);

    /* Leaving a diffusion mode has to leave the ordered table correct - it was
     * being kept at BAY4 while diffusion was selected, and nothing else
     * rebuilds it. */
    run_stretched_case("ordered after diffusion", 256, 8, 224, false,
                       ST7305_MONO_CLUSTER4);

    /* --- the tone curve ----------------------------------------------------- */

    {
        /* Direction, not magnitude. THRESHOLD is the mode that shows a tone
         * change most plainly, having no dither to spread it across. */
        const int dark   = paper_pixels(ST7305_MONO_THRESHOLD, 100, 100, -64);
        const int flat   = paper_pixels(ST7305_MONO_THRESHOLD, 100, 100, 0);
        const int bright = paper_pixels(ST7305_MONO_THRESHOLD, 100, 100, 64);
        check(dark < flat && flat < bright,
              "brightness moves the threshold balance the way it reads");

        const int lifted = paper_pixels(ST7305_MONO_THRESHOLD, 220, 100, 0);
        const int sunk   = paper_pixels(ST7305_MONO_THRESHOLD, 45, 100, 0);
        check(sunk < flat && flat < lifted,
              "gamma above 100 lifts the midtones and below 100 sinks them");

        /* CONTRAST IS A NO-OP UNDER THRESHOLD, and that is arithmetic rather
         * than a bug: it scales distance from mid grey, and the one threshold
         * this mode has IS mid grey, so nothing can be pushed across it. The
         * check is here so that the surprise is recorded rather than
         * rediscovered. */
        check(paper_pixels(ST7305_MONO_THRESHOLD, 100, 250, 0) == flat,
              "contrast cannot move a pure mid-grey threshold");

        /* Where there are thresholds either side of mid grey it does move
         * pixels - in both directions at once, so this asserts only that it
         * does something. */
        const int plain  = paper_pixels(ST7305_MONO_BAYER8, 100, 100, 0);
        const int punchy = paper_pixels(ST7305_MONO_BAYER8, 100, 250, 0);
        check(punchy != plain, "contrast changes a dithered picture");

        /* And the identity that keeps every case above this line meaningful. */
        check(paper_pixels(ST7305_MONO_THRESHOLD, 100, 100, 0) == flat,
              "neutral tone settings are reproducible");
    }

    /* Selecting a diffusion mode and blitting WITHOUT rebuilding must not
     * dereference the scratch buffers that rebuildDither() would have
     * allocated. The documented rule is that a mode takes effect on the next
     * rebuildDither(), so the correct behaviour is the previous ordered
     * picture - which is exactly what makes the crash impossible rather than
     * merely guarded against. */
    {
        static ST7305Tft tft;
        apply_palette(tft);
        tft.setInvertVideo(false);
        tft.setMonoMode(ST7305_MONO_BAYER4);
        tft.rebuildDither();
        ref_configure(ST7305_MONO_BAYER4, false);

        tft.setMonoMode(ST7305_MONO_FLOYD);     /* deliberately no rebuild */
        tft.clear(ST7305_WHITE);
        tft.blitIndexedStretched(lines, 256, 8, 224);

        check(compare_stretched(tft, 256, 8, 224) == 0,
              "a diffusion mode without a rebuild still blits ordered");
    }

    /* An out-of-range mode must be refused rather than indexed with. */
    {
        static ST7305Tft tft;
        tft.setMonoMode(ST7305_MONO_BLUE8);
        tft.setMonoMode((st7305_mono_t)99);
        tft.setMonoMode((st7305_mono_t)-1);
        check(tft.monoMode() == ST7305_MONO_BLUE8,
              "an out-of-range mono mode is ignored");
        check(strcmp(ST7305Tft::monoModeName((st7305_mono_t)99), "????") == 0,
              "an out-of-range mode still has a printable name");
    }

    /* The maps must stay inside the source image: reading lines[240] or
     * column 256 is out of bounds, and on a device it would be silent. */
    {
        int min_row = 1 << 30, max_row = -1, min_col = 1 << 30, max_col = -1;
        for (int y = 0; y < ST7305_H; y++) {
            const int sy = 8 + ((2 * y + 1) * 224) / (2 * ST7305_H);
            if (sy < min_row) min_row = sy;
            if (sy > max_row) max_row = sy;
        }
        for (int x = 0; x < ST7305_W; x++) {
            const int sx = ((2 * x + 1) * 256) / (2 * ST7305_W);
            if (sx < min_col) min_col = sx;
            if (sx > max_col) max_col = sx;
        }
        check(min_row == 8 && max_row == 231,
              "224-row crop samples exactly rows 8..231");
        check(min_col == 0 && max_col == 255,
              "256-column stretch samples exactly columns 0..255");
    }

    /* Degenerate arguments must leave the framebuffer alone. */
    {
        static ST7305Tft tft;
        apply_palette(tft);
        tft.setInvertVideo(false);
        tft.rebuildDither();
        tft.clear(ST7305_BLACK);

        tft.blitIndexedStretched(lines, 0, 8, 224);
        tft.blitIndexedStretched(lines, 256, 8, 0);
        tft.blitIndexedStretched(lines, 256, -1, 224);
        tft.blitIndexedStretched(nullptr, 256, 8, 224);

        const uint8_t *fb = tft.framebuffer();
        int nonzero = 0;
        for (size_t i = 0; i < ST7305_FB_SIZE; i++) if (fb[i]) nonzero++;
        check(nonzero == 0, "degenerate stretch arguments write nothing");
    }

    /* One lit pixel, checked against the layout formula directly rather than
     * through the bulk comparison. */
    {
        static ST7305Tft tft;
        static uint8_t cell[4][4];
        static uint8_t *l4[4];

        for (int i = 0; i < 4; i++) { l4[i] = cell[i]; memset(cell[i], 0, 4); }
        for (int i = 0; i < 256; i++) tft.setPaletteEntry(i, 255, 255, 255);
        tft.setPaletteEntry(0, 0, 0, 0);
        tft.setInvertVideo(false);
        tft.rebuildDither();
        tft.clear(ST7305_BLACK);

        cell[0][0] = 1;                     /* white, at the image's top-left */
        tft.blitIndexed(l4, 4, 4);

        const int x0 = ST7305Tft::blitOriginX(4);
        const int y0 = ST7305Tft::blitOriginY(4);
        check(x0 == 196 && y0 == 148, "4x4 centres on a multiple of 4");

        const uint8_t *fb = tft.framebuffer();
        check(fb[ref_index(x0, y0)] == (uint8_t)(1 << ref_bit(x0, y0)),
              "the lit pixel is the only bit set in its byte");

        int set = 0;
        for (size_t i = 0; i < ST7305_FB_SIZE; i++)
            for (int b = 0; b < 8; b++)
                if (fb[i] & (1 << b)) set++;
        check(set == 1, "exactly one bit set in the whole framebuffer");
    }

    /* Rejected sizes must leave the framebuffer alone rather than write
     * somewhere out of range. */
    {
        static ST7305Tft tft;
        apply_palette(tft);
        tft.setInvertVideo(false);
        tft.rebuildDither();
        tft.clear(ST7305_BLACK);

        tft.blitIndexed(lines, 0, 240);
        tft.blitIndexed(lines, 256, 0);
        tft.blitIndexed(lines, 3, 3);           /* rounds down to nothing */
        tft.blitIndexed(nullptr, 256, 240);

        const uint8_t *fb = tft.framebuffer();
        int nonzero = 0;
        for (size_t i = 0; i < ST7305_FB_SIZE; i++) if (fb[i]) nonzero++;
        check(nonzero == 0, "degenerate sizes write nothing");
    }

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
