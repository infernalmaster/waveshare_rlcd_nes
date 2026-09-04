/* st7305_tft.cpp - implementation of the ST7305Tft wrapper.
 *
 * This is the ONE translation unit that defines ST7305_GFX_IMPLEMENTATION, so
 * the header-only library's code lands here and nowhere else. Everything that
 * includes st7305_tft.h (the sketch, the emulator's osd.cpp) gets declarations.
 */
#define ST7305_GFX_IMPLEMENTATION
#include "st7305_gfx.h"

#include "st7305_tft.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#endif

/* WHERE THE DIFFUSION SCRATCH LANDS IS WORTH MORE THAN EVERY OTHER
 * OPTIMISATION IN THIS FILE PUT TOGETHER, so it is not left to chance.
 *
 * This project allocates from PSRAM deliberately and almost everywhere - the
 * ROM, the NES bitmap in bitmap.c, the emulator's frame buffer. Plain malloc()
 * on an Arduino build with PSRAM enabled may return either heap depending on
 * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, and these buffers are touched several
 * times per pixel, 120000 pixels a frame, on a bus that is an order of
 * magnitude slower than internal SRAM and behind a shared cache. A wrong guess
 * here does not cost a few percent.
 *
 * 4.4 KB, so asking for internal is not a big ask. If it is refused the caller
 * still gets memory and a warning rather than a failure, because a slow picture
 * beats no picture. */
static void *st7305_alloc_internal(size_t n)
{
#if defined(ARDUINO_ARCH_ESP32)
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) return p;
    Serial.println("[VIDEO] diffusion scratch would not fit in internal SRAM - "
                   "falling back to the default heap, expect it to be slow");
#endif
    return malloc(n);
}

/* Boot settings for the reduction. All four are overridable from hw_config.h so
 * a board can ship a different look without touching this file; the defaults
 * here are exactly what this wrapper did before any of them existed. */
#ifndef NES_MONO_MODE
#define NES_MONO_MODE ST7305_MONO_BAYER4
#endif
#ifndef NES_MONO_GAMMA
#define NES_MONO_GAMMA 100
#endif
#ifndef NES_MONO_CONTRAST
#define NES_MONO_CONTRAST 100
#endif
#ifndef NES_MONO_BRIGHTNESS
#define NES_MONO_BRIGHTNESS 0
#endif

/* ---------------------------------------------------------------------------
 * 5x7 font, 5 column bytes per glyph, bit 0 = top row. Public-domain cell used
 * by every small LCD library; the same table the emulator's ST7789 driver
 * carries, kept here so this wrapper has no dependency on that header.
 * Covers 0x20..0x7F, which is what ROM filenames need.
 * ------------------------------------------------------------------------- */
static const uint8_t st7305_font5x7[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, /* 0x20 ' ' */
    0x00, 0x00, 0x5F, 0x00, 0x00, /* !  */
    0x00, 0x07, 0x00, 0x07, 0x00, /* "  */
    0x14, 0x7F, 0x14, 0x7F, 0x14, /* #  */
    0x24, 0x2A, 0x7F, 0x2A, 0x12, /* $  */
    0x23, 0x13, 0x08, 0x64, 0x62, /* %  */
    0x36, 0x49, 0x55, 0x22, 0x50, /* &  */
    0x00, 0x05, 0x03, 0x00, 0x00, /* '  */
    0x00, 0x1C, 0x22, 0x41, 0x00, /* (  */
    0x00, 0x41, 0x22, 0x1C, 0x00, /* )  */
    0x14, 0x08, 0x3E, 0x08, 0x14, /* *  */
    0x08, 0x08, 0x3E, 0x08, 0x08, /* +  */
    0x00, 0x50, 0x30, 0x00, 0x00, /* ,  */
    0x08, 0x08, 0x08, 0x08, 0x08, /* -  */
    0x00, 0x60, 0x60, 0x00, 0x00, /* .  */
    0x20, 0x10, 0x08, 0x04, 0x02, /* /  */
    0x3E, 0x51, 0x49, 0x45, 0x3E, /* 0  */
    0x00, 0x42, 0x7F, 0x40, 0x00, /* 1  */
    0x42, 0x61, 0x51, 0x49, 0x46, /* 2  */
    0x21, 0x41, 0x45, 0x4B, 0x31, /* 3  */
    0x18, 0x14, 0x12, 0x7F, 0x10, /* 4  */
    0x27, 0x45, 0x45, 0x45, 0x39, /* 5  */
    0x3C, 0x4A, 0x49, 0x49, 0x30, /* 6  */
    0x01, 0x71, 0x09, 0x05, 0x03, /* 7  */
    0x36, 0x49, 0x49, 0x49, 0x36, /* 8  */
    0x06, 0x49, 0x49, 0x29, 0x1E, /* 9  */
    0x00, 0x36, 0x36, 0x00, 0x00, /* :  */
    0x00, 0x56, 0x36, 0x00, 0x00, /* ;  */
    0x08, 0x14, 0x22, 0x41, 0x00, /* <  */
    0x14, 0x14, 0x14, 0x14, 0x14, /* =  */
    0x00, 0x41, 0x22, 0x14, 0x08, /* >  */
    0x02, 0x01, 0x51, 0x09, 0x06, /* ?  */
    0x32, 0x49, 0x79, 0x41, 0x3E, /* @  */
    0x7E, 0x11, 0x11, 0x11, 0x7E, /* A  */
    0x7F, 0x49, 0x49, 0x49, 0x36, /* B  */
    0x3E, 0x41, 0x41, 0x41, 0x22, /* C  */
    0x7F, 0x41, 0x41, 0x22, 0x1C, /* D  */
    0x7F, 0x49, 0x49, 0x49, 0x41, /* E  */
    0x7F, 0x09, 0x09, 0x09, 0x01, /* F  */
    0x3E, 0x41, 0x49, 0x49, 0x7A, /* G  */
    0x7F, 0x08, 0x08, 0x08, 0x7F, /* H  */
    0x00, 0x41, 0x7F, 0x41, 0x00, /* I  */
    0x20, 0x40, 0x41, 0x3F, 0x01, /* J  */
    0x7F, 0x08, 0x14, 0x22, 0x41, /* K  */
    0x7F, 0x40, 0x40, 0x40, 0x40, /* L  */
    0x7F, 0x02, 0x0C, 0x02, 0x7F, /* M  */
    0x7F, 0x04, 0x08, 0x10, 0x7F, /* N  */
    0x3E, 0x41, 0x41, 0x41, 0x3E, /* O  */
    0x7F, 0x09, 0x09, 0x09, 0x06, /* P  */
    0x3E, 0x41, 0x51, 0x21, 0x5E, /* Q  */
    0x7F, 0x09, 0x19, 0x29, 0x46, /* R  */
    0x46, 0x49, 0x49, 0x49, 0x31, /* S  */
    0x01, 0x01, 0x7F, 0x01, 0x01, /* T  */
    0x3F, 0x40, 0x40, 0x40, 0x3F, /* U  */
    0x1F, 0x20, 0x40, 0x20, 0x1F, /* V  */
    0x7F, 0x20, 0x18, 0x20, 0x7F, /* W  */
    0x63, 0x14, 0x08, 0x14, 0x63, /* X  */
    0x03, 0x04, 0x78, 0x04, 0x03, /* Y  */
    0x61, 0x51, 0x49, 0x45, 0x43, /* Z  */
    0x00, 0x7F, 0x41, 0x41, 0x00, /* [  */
    0x02, 0x04, 0x08, 0x10, 0x20, /* \  */
    0x00, 0x41, 0x41, 0x7F, 0x00, /* ]  */
    0x04, 0x02, 0x01, 0x02, 0x04, /* ^  */
    0x40, 0x40, 0x40, 0x40, 0x40, /* _  */
    0x00, 0x01, 0x02, 0x04, 0x00, /* `  */
    0x20, 0x54, 0x54, 0x54, 0x78, /* a  */
    0x7F, 0x48, 0x44, 0x44, 0x38, /* b  */
    0x38, 0x44, 0x44, 0x44, 0x20, /* c  */
    0x38, 0x44, 0x44, 0x48, 0x7F, /* d  */
    0x38, 0x54, 0x54, 0x54, 0x18, /* e  */
    0x08, 0x7E, 0x09, 0x01, 0x02, /* f  */
    0x0C, 0x52, 0x52, 0x52, 0x3E, /* g  */
    0x7F, 0x08, 0x04, 0x04, 0x78, /* h  */
    0x00, 0x44, 0x7D, 0x40, 0x00, /* i  */
    0x20, 0x40, 0x44, 0x3D, 0x00, /* j  */
    0x7F, 0x10, 0x28, 0x44, 0x00, /* k  */
    0x00, 0x41, 0x7F, 0x40, 0x00, /* l  */
    0x7C, 0x04, 0x18, 0x04, 0x78, /* m  */
    0x7C, 0x08, 0x04, 0x04, 0x78, /* n  */
    0x38, 0x44, 0x44, 0x44, 0x38, /* o  */
    0x7C, 0x14, 0x14, 0x14, 0x08, /* p  */
    0x08, 0x14, 0x14, 0x18, 0x7C, /* q  */
    0x7C, 0x08, 0x04, 0x04, 0x08, /* r  */
    0x48, 0x54, 0x54, 0x54, 0x20, /* s  */
    0x04, 0x3F, 0x44, 0x40, 0x20, /* t  */
    0x3C, 0x40, 0x40, 0x20, 0x7C, /* u  */
    0x1C, 0x20, 0x40, 0x20, 0x1C, /* v  */
    0x3C, 0x40, 0x30, 0x40, 0x3C, /* w  */
    0x44, 0x28, 0x10, 0x28, 0x44, /* x  */
    0x0C, 0x50, 0x50, 0x50, 0x3C, /* y  */
    0x44, 0x64, 0x54, 0x4C, 0x44, /* z  */
    0x00, 0x08, 0x36, 0x41, 0x00, /* {  */
    0x00, 0x00, 0x7F, 0x00, 0x00, /* |  */
    0x00, 0x41, 0x36, 0x08, 0x00, /* }  */
    0x10, 0x08, 0x08, 0x10, 0x08, /* ~  */
    0x00, 0x00, 0x00, 0x00, 0x00  /* 0x7F DEL */
};

/* --- the ordered-dither masks ----------------------------------------------
 *
 * Every one of these is a permutation of 0..n-1, which is what makes it a
 * threshold mask rather than a pattern: each cell gets a distinct rank, and a
 * grey level of k turns on exactly the k cells ranked below it. buildMatrix()
 * scales whichever is selected to 0..255 and replicates it across thr_[8][8].
 * ------------------------------------------------------------------------- */

/* Bayer 4x4, the recursive dispersed-dot matrix. This is what the wrapper used
 * before there was a choice and it is still the default. */
static const uint8_t st7305_bayer[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 }
};

/* Bayer 2x2. Four levels instead of sixteen: the thresholds land at 32, 96,
 * 160 and 224, so most of a NES palette clears either all of them or none and
 * the result is far more contrast with almost no visible dither cell. */
static const uint8_t st7305_bayer2[2][2] = {
    { 0, 2 },
    { 3, 1 }
};

/* Bayer 8x8, the same construction one level deeper - 64 thresholds, so a
 * gradient that bands into 16 steps under the 4x4 gets 64. The trade is that
 * the repeating cell is twice as wide and its cross-hatch correspondingly
 * easier to see in a flat area. */
static const uint8_t st7305_bayer8[8][8] = {
    {  0, 32,  8, 40,  2, 34, 10, 42 },
    { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44,  4, 36, 14, 46,  6, 38 },
    { 60, 28, 52, 20, 62, 30, 54, 22 },
    {  3, 35, 11, 43,  1, 33,  9, 41 },
    { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47,  7, 39, 13, 45,  5, 37 },
    { 63, 31, 55, 23, 61, 29, 53, 21 }
};

/* Clustered-dot 4x4 - a halftone screen. Where Bayer scatters the "on" pixels
 * as far apart as it can, this grows them outward from the centre of each cell,
 * so mid grey comes out as a grid of fattening dots rather than as a weave.
 * Worth having on a REFLECTIVE panel in particular: isolated single pixels have
 * the least contrast against their neighbours of anything you can draw here, so
 * a rule that never draws one often survives the glass better than a rule whose
 * whole idea is to draw them. */
static const uint8_t st7305_cluster4[4][4] = {
    { 12,  5,  6, 13 },
    {  4,  0,  1,  7 },
    { 11,  3,  2,  8 },
    { 15, 10,  9, 14 }
};

/* Blue noise, 8x8, generated by void-and-cluster (Ulichney's method: settle a
 * half-density binary pattern by repeatedly moving the pixel in the tightest
 * cluster into the largest void, then rank every cell by removing and adding
 * them in that same order). A Gaussian of sigma 1.5 on an 8x8 torus was the
 * filter.
 *
 * THE POINT OF IT is that Bayer's ranks are a recursive doubling, so its "on"
 * sets at every level are perfectly periodic and the eye reads that periodicity
 * as a diagonal weave laid over the picture. These ranks are aperiodic: spread
 * out, but never lining up. Flat areas come out as grain instead.
 *
 * IT IS 8x8 AND NOT 4x4 ON PURPOSE. A 4x4 cell holds sixteen ranks, and the
 * maximally dispersed arrangement of sixteen cells on a 4x4 torus IS the Bayer
 * matrix - at that size there is no aperiodic arrangement left to find, only
 * worse periodic ones. Blue noise needs room, and this is the smallest cell
 * with any. */
static const uint8_t st7305_blue8[8][8] = {
    { 50, 43,  9, 29, 12, 26, 45, 30 },
    { 25,  1, 55, 34, 49,  3, 57, 14 },
    { 61, 38, 46, 15, 60, 18, 36,  7 },
    { 32, 16, 22,  5, 31, 40, 52, 44 },
    { 10, 54, 42, 59, 11, 24,  0, 21 },
    { 48,  6, 27, 37, 47, 56, 28, 63 },
    { 13, 33, 51,  2, 17,  8, 35, 39 },
    { 58, 19, 23, 62, 41, 53, 20,  4 }
};

/* RGB565 -> 0..254 luminance. Rec.601 weights, pre-multiplied by each channel's
 * 5/6-bit-to-8-bit scale so the whole thing is one shift and no division. */
static inline int st7305_lum565(uint16_t c)
{
    const int r = (c >> 11) & 0x1F;
    const int g = (c >> 5)  & 0x3F;
    const int b =  c        & 0x1F;
    return (r * 633 + g * 607 + b * 238) >> 8;
}

/* --- I/O callbacks --------------------------------------------------------- */

void ST7305Tft::ioXfer(void *ctx, int dc, const uint8_t *buf, size_t len)
{
    ST7305Tft *self = (ST7305Tft *)ctx;
    digitalWrite(RLCD_DC, dc ? HIGH : LOW);
    digitalWrite(RLCD_CS, LOW);
    /* Synchronous, as st7305_flush() requires - transferBytes does not return
     * until the last byte has been clocked out. */
    self->spi_.transferBytes((uint8_t *)buf, nullptr, len);
    digitalWrite(RLCD_CS, HIGH);
}

void ST7305Tft::ioDelay(void *ctx, uint32_t ms)
{
    (void)ctx;
    delay(ms);
}

void ST7305Tft::ioReset(void *ctx, int level)
{
    (void)ctx;
    digitalWrite(RLCD_RST, level ? HIGH : LOW);
}

/* --- lifecycle ------------------------------------------------------------- */

ST7305Tft::ST7305Tft()
    : spi_(HSPI),
      drawing_(-1), ready_(-1), sending_(-1), paced_(false), pushes_(0),
      dirty_(false), invert_(NES_INVERT_VIDEO ? true : false),
      mono_(NES_MONO_MODE), gamma100_(NES_MONO_GAMMA),
      contrast100_(NES_MONO_CONTRAST), brightness_(NES_MONO_BRIGHTNESS),
      err_(nullptr), bits_(nullptr), tonerow_(nullptr),
      t_tone_us_(0), t_loop_us_(0), t_asm_us_(0),
      te_pin_(-1), fallback_ms_(0), te_task_(nullptr), te_edges_(0),
      te_stop_(false)
{
    memset(&lcd_, 0, sizeof lcd_);
    memset(&send_lcd_, 0, sizeof send_lcd_);
    memset(fb_, 0xFF, sizeof fb_);          /* white paper */
    /* Point the device at the framebuffer here rather than waiting for
     * st7305_init() to do it. Drawing before init() is a mistake, but it should
     * be a wasted call rather than a null dereference inside st7305_clear().
     * fb_[0] until pacing starts, which is what makes every unpaced caller -
     * the menus, the splash - behave exactly as they did single-buffered. */
    lcd_.fb = fb_[0];
    memset(lum_, 0, sizeof lum_);
    memset(tone_, 0, sizeof tone_);
    memset(dither_, 0, sizeof dither_);
    memset(colmap_, 0, sizeof colmap_);
    memset(rowmap_, 0, sizeof rowmap_);
    /* -1 rather than 0: a caller asking for a 0-wide source must not be told
     * the maps already describe it. */
    map_w_ = map_y0_ = map_h_ = -1;

    /* thr_ is the one thing that must be valid before init(), because
     * pushImage() reads it and does not go through rebuildDither(). A boot mode
     * that needs the diffusion buffers does not get them here - malloc in a
     * constructor that runs before setup() is a bad trade, and rebuildDither()
     * at the end of init() is where it belongs. */
    buildMatrix();
}

ST7305Tft::~ST7305Tft()
{
    freeDiffusion();
}

void ST7305Tft::init()
{
    pinMode(RLCD_DC, OUTPUT);
    pinMode(RLCD_CS, OUTPUT);
    pinMode(RLCD_RST, OUTPUT);
    digitalWrite(RLCD_CS, HIGH);
    digitalWrite(RLCD_DC, HIGH);

    spi_.begin(RLCD_SCK, -1, RLCD_MOSI, -1);
    /* Opened once and left open: nothing else shares this bus on this board -
     * the TF card is on the SDMMC peripheral, not on SPI. */
    spi_.beginTransaction(SPISettings(RLCD_SPI_HZ, MSBFIRST, SPI_MODE0));

    st7305_io_t io = { ioXfer, ioDelay, ioReset, this };
    st7305_init(&lcd_, &io, fb_[0]);        /* clears to white and flushes */
    dirty_ = false;

    rebuildDither();                        /* valid, if all-black, table */
}

void ST7305Tft::flush()
{
    st7305_flush(&lcd_);
    dirty_ = false;
}

void ST7305Tft::flushIfDirty()
{
    if (dirty_) flush();
}

void ST7305Tft::setMode(st7305_mode_t m)
{
    st7305_set_mode(&lcd_, m);
}

/* --- TE-paced presentation --------------------------------------------------
 *
 * Four small functions and no lock between them. The reasoning, once, because
 * it is the whole reason this is safe:
 *
 *   - drawing_ is written only by the producer thread.
 *   - sending_ is written only by the TE task, which is strictly sequential:
 *     it cannot start a take while its own previous send is open.
 *   - ready_ is the handoff. The producer writes it in present(), the task
 *     clears it in takeReady().
 *
 * The only way this breaks is if beginFrame() hands out the buffer the task is
 * about to put on the wire. beginFrame() refuses anything named by ready_ or
 * sending_, so that can only happen in a window where the buffer is named by
 * NEITHER - which is exactly what takeReady()'s write order rules out.
 * ------------------------------------------------------------------------- */

bool ST7305Tft::beginFrame()
{
    if (!paced_) return true;               /* unpaced: fb_[0], as ever */
    if (drawing_ >= 0) return true;         /* already holding one */

    /* PRODUCTION IS PACED TO THE PANEL, NOT RUN FLAT OUT. A frame is already
     * queued, so drawing another would only throw the first one away before
     * anyone saw it - and the drawing is not free: for the NES blit it is ~3 ms
     * of dithering, which at the producer's 60 Hz is 18% of this core against
     * a third of that at any of the panel's rates.
     *
     * The cost of the choice is latency: what goes out on the next edge was
     * drawn just after the last one rather than just before this one, so it is
     * most of a scan period old. Deleting this one test spends the CPU to get
     * that back; if the controls ever feel soft, that is the knob. */
    if (ready_ >= 0) return false;

    for (int i = 0; i < 2; i++) {
        if (i == ready_ || i == sending_) continue;
        drawing_ = (int8_t)i;
        lcd_.fb  = fb_[i];
        return true;
    }

    /* Unreachable with the test above - two buffers, at most one queued, at
     * most one in flight. Kept because the loop is the actual contract and the
     * early return is an optimisation on top of it. */
    return false;
}

void ST7305Tft::present()
{
    if (!paced_) { flush(); return; }
    if (drawing_ < 0) return;               /* present() without beginFrame() */

    /* Whatever was queued and not yet taken is superseded here and becomes
     * free again. That is the frame drop: the producer runs several times
     * faster than the panel, so most frames end their life on this line. */
    ready_   = drawing_;
    drawing_ = -1;
    dirty_   = false;
}

int ST7305Tft::takeReady()
{
    const int i = ready_;
    if (i < 0) return -1;

    /* ORDER IS LOAD-BEARING - see the note in the header. sending_ must name
     * the buffer before ready_ stops naming it, or a beginFrame() landing
     * between the two writes sees a buffer that looks free and is not. */
    sending_ = (int8_t)i;
    ready_   = -1;
    return i;
}

void ST7305Tft::sendDone()
{
    sending_ = -1;
}

uint32_t ST7305Tft::teEdges() const
{
    return te_edges_;
}

#if defined(ARDUINO_ARCH_ESP32)

/* Nothing but a wakeup. The 6.5 ms of SPI belongs on a task, not in here. */
void ST7305Tft::teIsr(void *arg)
{
    ST7305Tft *self = (ST7305Tft *)arg;
    self->te_edges_++;

    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR((TaskHandle_t)self->te_task_, &woken);
    if (woken) portYIELD_FROM_ISR();
}

void ST7305Tft::teTaskEntry(void *arg)
{
    ((ST7305Tft *)arg)->teTaskLoop();
}

void ST7305Tft::teTaskLoop()
{
    /* Three consecutive silences before giving up on TE, rather than one: a
     * single missed edge is a dropped interrupt, not a board without the pin
     * wired, and it should not cost a warning line or a mode change. */
    int  silences = 0;
    bool on_timer = false;

    while (!te_stop_) {
        const uint32_t wait = on_timer ? fallback_ms_ : (fallback_ms_ * 2 + 100);

        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait)) == 0) {
            if (!on_timer && ++silences >= 3) {
                on_timer = true;
                Serial.printf("[VIDEO] no TE edge on GPIO %d for %u ms - "
                              "falling back to a %u ms timer. Expect tearing.\n",
                              te_pin_, (unsigned)wait, (unsigned)fallback_ms_);
            }
            /* Push anyway. A frozen picture would look like a hung emulator,
             * which is a far worse failure than the tearing we started with. */
        } else if (on_timer) {
            /* An edge arrived after all - TE is alive, so stop guessing. */
            on_timer = false;
            silences = 0;
            Serial.println("[VIDEO] TE is back, resuming paced flushes");
        } else {
            silences = 0;
        }

        const int i = takeReady();
        if (i < 0) continue;                /* no new frame since the last edge */

        /* The producer is drawing into the other buffer for the next 6.5 ms,
         * which is the entire point of the exercise. */
        send_lcd_.fb = fb_[i];
        st7305_flush(&send_lcd_);
        pushes_++;
        sendDone();
    }

    te_task_ = nullptr;
    vTaskDelete(NULL);
}

#endif /* ARDUINO_ARCH_ESP32 */

bool ST7305Tft::startTePacing(int te_pin, uint32_t fallback_ms)
{
    if (paced_) return true;

    te_pin_      = te_pin;
    fallback_ms_ = fallback_ms ? fallback_ms : 50;
    te_edges_    = 0;
    pushes_      = 0;
    te_stop_     = false;

    /* The task's handle on the panel: same bus, same callbacks, different
     * framebuffer. Copied after init() so io and ctx are already valid. */
    send_lcd_ = lcd_;

    /* Both buffers free. The first beginFrame() will take fb_[0], which still
     * holds whatever the caller last flushed - harmless, because the panel
     * keeps its own copy in GRAM and overwriting ours does not touch the
     * glass. */
    drawing_ = -1;
    ready_   = -1;
    sending_ = -1;
    te_task_ = nullptr;
    paced_   = true;

#if defined(ARDUINO_ARCH_ESP32)
    if (te_pin_ >= 0) {
        /* Core 1 alongside the emulator, not core 0: this task spends 6.5 ms
         * per panel period on SPI, which is time core 1 already pays inside the
         * old inline flush - moving it here changes where it is spent, not how
         * much. Core 0 is the BLE keyboard's, at the same priority, and has no
         * headroom to spare for a 15% duty cycle.
         *
         * Priority 2 puts it above the Arduino loopTask the emulator runs on,
         * so an edge preempts emulation rather than waiting on it - a wait of
         * even a few ms would put the write back into the middle of the scan
         * and undo the entire mechanism. */
        TaskHandle_t h = nullptr;
        if (xTaskCreatePinnedToCore(teTaskEntry, "st7305_te", 4096, this, 2,
                                    &h, 1) != pdPASS) {
            paced_   = false;
            te_task_ = nullptr;
            return false;
        }
        te_task_ = h;

        pinMode(te_pin_, INPUT);
        /* Created before the interrupt is attached, or an edge arriving in
         * between notifies a null handle. */
        attachInterruptArg(digitalPinToInterrupt(te_pin_), teIsr, this, RISING);
    }
#endif

    return true;
}

void ST7305Tft::stopTePacing()
{
    if (!paced_) return;

#if defined(ARDUINO_ARCH_ESP32)
    if (te_pin_ >= 0) detachInterrupt(digitalPinToInterrupt(te_pin_));

    /* Set the flag and wait it out rather than poking the task awake. A notify
     * would need the handle to still be valid, and the task may have exited
     * between the null check and the call - a race worth one wasted timeout
     * (~190 ms, once, when the emulator quits) to not have. */
    te_stop_ = true;
    for (int i = 0; i < 200 && te_task_; i++) delay(10);
#endif

    paced_   = false;
    drawing_ = -1;
    ready_   = -1;
    sending_ = -1;

    /* Back to the single-buffer world the menus expect. fb_[0] may be a frame
     * behind, but every unpaced caller redraws before it flushes. */
    lcd_.fb = fb_[0];
}

/* --- primitives ------------------------------------------------------------ */

st7305_color_t ST7305Tft::toMono(uint16_t rgb565)
{
    return st7305_lum565(rgb565) >= 128 ? ST7305_WHITE : ST7305_BLACK;
}

void ST7305Tft::clear(st7305_color_t c)
{
    st7305_clear(&lcd_, c);
    dirty_ = true;
}

void ST7305Tft::fillScreen(uint16_t color)
{
    clear(toMono(color));
}

void ST7305Tft::drawFilledRect(int x, int y, int w, int h, uint16_t color)
{
    st7305_fill_rect(&lcd_, x, y, w, h, toMono(color));
    dirty_ = true;
}

void ST7305Tft::drawChar(int x, int y, char c, uint16_t fg, uint16_t bg,
                         int scale)
{
    if (scale < 1) scale = 1;

    const st7305_color_t cfg = toMono(fg);
    const st7305_color_t cbg = toMono(bg);

    /* Opaque background, like the ST7789 driver's drawChar - callers rely on it
     * to erase what was underneath instead of clearing first. */
    st7305_fill_rect(&lcd_, x, y, 5 * scale, 7 * scale, cbg);

    if (cfg == cbg) {                       /* nothing would be visible */
        dirty_ = true;
        return;
    }

    const uint8_t uc  = (c >= 0x20 && c <= 0x7F) ? (uint8_t)c : (uint8_t)'?';
    const uint8_t *gl = &st7305_font5x7[(uc - 0x20) * 5];

    for (int cx = 0; cx < 5; cx++) {
        const uint8_t bits = gl[cx];
        if (!bits) continue;
        for (int cy = 0; cy < 7; cy++) {
            if (!((bits >> cy) & 1)) continue;
            if (scale == 1)
                st7305_pixel(&lcd_, x + cx, y + cy, cfg);
            else
                st7305_fill_rect(&lcd_, x + cx * scale, y + cy * scale,
                                 scale, scale, cfg);
        }
    }
    dirty_ = true;
}

void ST7305Tft::drawString(int x, int y, const char *str, uint16_t fg,
                           uint16_t bg, int scale)
{
    if (!str) return;
    if (scale < 1) scale = 1;

    int px = x;
    for (int i = 0; str[i]; i++) {
        drawChar(px, y, str[i], fg, bg, scale);
        px += 6 * scale;
        if (px >= ST7305_W) break;          /* nothing further can be visible */
    }
}

void ST7305Tft::drawNumber(int x, int y, int num, uint16_t fg, uint16_t bg,
                           int scale)
{
    char buf[12];
    snprintf(buf, sizeof buf, "%d", num);
    drawString(x, y, buf, fg, bg, scale);
}

int ST7305Tft::textWidth(const char *str, int scale)
{
    if (!str || scale < 1) return 0;
    int n = 0;
    while (str[n]) n++;
    return n * 6 * scale;
}

/* --- RGB565 image ----------------------------------------------------------
 *
 * The compatibility path: whatever the emulator (or anything else) built as a
 * 565 buffer, ordered-dithered to 1 bpp. Per-pixel and therefore slow-ish -
 * blitIndexed() is what the NES actually goes through. Kept because it is what
 * makes this a drop-in TFTDriver.
 *
 * FOLLOWS monoMode()'S THRESHOLD MATRIX, so an image drawn this way and the
 * game picture wear the same texture. It does NOT follow the tone curve or
 * either diffusion mode, and cannot: it is handed 565 pixels rather than
 * palette indices, so there is no table to have folded the curve into and
 * nothing to read but the thresholds themselves. In a diffusion mode this
 * silently gets BAYER4, which is what buildMatrix() leaves in thr_.
 */
void ST7305Tft::pushImage(int x, int y, int w, int h, const uint16_t *data,
                          bool preswapped)
{
    if (!data || w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        const int dy = y + row;
        if ((unsigned)dy >= (unsigned)ST7305_H) continue;
        const uint16_t *src = data + (size_t)row * w;
        const uint8_t  *thr = thr_[dy & 7];

        for (int col = 0; col < w; col++) {
            const int dx = x + col;
            if ((unsigned)dx >= (unsigned)ST7305_W) continue;
            uint16_t c = src[col];
            /* "preswapped" means the caller already byte-swapped for the
             * ST7789's big-endian bus, so undo that to read the channels. */
            if (preswapped) c = (uint16_t)__builtin_bswap16(c);
            const int lum = st7305_lum565(c);
            const int t   = thr[dx & 7];
            st7305_pixel(&lcd_, dx, dy,
                         (invert_ ? (254 - lum) : lum) > t ? ST7305_WHITE
                                                           : ST7305_BLACK);
        }
    }
    dirty_ = true;
}

/* --- 8-bit indexed video --------------------------------------------------- */

void ST7305Tft::setPaletteEntry(int index, int r, int g, int b)
{
    if ((unsigned)index >= 256u) return;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    lum_[index] = (uint8_t)((77 * r + 150 * g + 29 * b) >> 8);
}

void ST7305Tft::setInvertVideo(bool invert)
{
    invert_ = invert;
}

void ST7305Tft::setMonoMode(st7305_mono_t m)
{
    if ((unsigned)m < (unsigned)ST7305_MONO_COUNT) mono_ = m;
}

const char *ST7305Tft::monoModeName(st7305_mono_t m)
{
    switch (m) {
    case ST7305_MONO_BAYER4:    return "BAY4";
    case ST7305_MONO_BAYER8:    return "BAY8";
    case ST7305_MONO_BAYER2:    return "BAY2";
    case ST7305_MONO_CLUSTER4:  return "CLUS";
    case ST7305_MONO_BLUE8:     return "BLUE";
    case ST7305_MONO_THRESHOLD: return "THRS";
    case ST7305_MONO_FLOYD:     return "FLOY";
    case ST7305_MONO_ATKINSON:  return "ATKN";
    default:                    return "????";
    }
}

void ST7305Tft::setGamma(int gamma_x100)
{
    if (gamma_x100 < 25)  gamma_x100 = 25;
    if (gamma_x100 > 400) gamma_x100 = 400;
    gamma100_ = gamma_x100;
}

void ST7305Tft::setContrast(int contrast_x100)
{
    if (contrast_x100 < 25)  contrast_x100 = 25;
    if (contrast_x100 > 400) contrast_x100 = 400;
    contrast100_ = contrast_x100;
}

void ST7305Tft::setBrightness(int bias)
{
    if (bias < -128) bias = -128;
    if (bias >  127) bias =  127;
    brightness_ = bias;
}

/* --- the reduction, built once per palette or setting change ---------------- */

void ST7305Tft::rebuildTone()
{
    for (int v = 0; v < 256; v++) {
        int l = invert_ ? (255 - (int)lum_[v]) : (int)lum_[v];

        /* BOTH ARE SKIPPED, NOT COMPUTED, AT THEIR NEUTRAL SETTINGS. That is
         * not an optimisation - it is what makes a default build's tone_ bit
         * for bit its lum_, so the blits emit exactly the bytes they emitted
         * before any of this existed and test/blit_test.cpp is still checking
         * something. Round-tripping 0..255 through pow() would not be an
         * identity, and the failure would be one grey level wide and invisible
         * until someone diffed a framebuffer. */
        if (gamma100_ != 100) {
            const double n = pow((double)l / 255.0, 100.0 / (double)gamma100_);
            l = (int)(n * 255.0 + 0.5);
        }
        if (contrast100_ != 100) {
            const double d = ((double)l - 128.0) * (double)contrast100_ / 100.0;
            l = 128 + (int)(d < 0.0 ? d - 0.5 : d + 0.5);
        }
        l += brightness_;

        if (l < 0) l = 0; else if (l > 255) l = 255;
        tone_[v] = (uint8_t)l;
    }
}

void ST7305Tft::buildMatrix()
{
    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            int t;
            switch (mono_) {
            /* n ranks scaled to sit at the CENTRES of the n bands they divide
             * 0..255 into, which is why each is (rank * step + step/2) rather
             * than (rank * step). Off-centre thresholds would push the whole
             * picture half a band light or dark. */
            case ST7305_MONO_BAYER8:
                t = st7305_bayer8[r][c] * 4 + 2;
                break;
            case ST7305_MONO_BLUE8:
                t = st7305_blue8[r][c] * 4 + 2;
                break;
            case ST7305_MONO_BAYER2:
                t = st7305_bayer2[r & 1][c & 1] * 64 + 32;
                break;
            case ST7305_MONO_CLUSTER4:
                t = st7305_cluster4[r & 3][c & 3] * 16 + 8;
                break;
            /* One threshold everywhere, so there is no cell and no pattern.
             * 127 and not 128 because the comparison downstream is ">", which
             * makes this the same "luminance of 128 or more is paper" rule
             * toMono() applies to text. */
            case ST7305_MONO_THRESHOLD:
                t = 127;
                break;
            /* BAYER4, and also the two diffusion modes: they do not read the
             * table for the game picture, but blitIndexed() and pushImage()
             * still do, and those must not be handed a stale or empty one. */
            case ST7305_MONO_BAYER4:
            default:
                t = st7305_bayer[r & 3][c & 3] * 16 + 8;
                break;
            }
            thr_[r][c] = (uint8_t)t;
        }
    }
}

bool ST7305Tft::ensureDiffusion()
{
    if (!err_)
        err_ = (int16_t *)st7305_alloc_internal((size_t)3 * (ST7305_W + 4)
                                                * sizeof(int16_t));
    /* Five rows, not four: the fifth is tonerow_. One allocation because they
     * have identical lifetimes and there is no reason to make freeDiffusion()
     * track two pointers to say one thing. */
    if (!bits_) {
        bits_ = (uint8_t *)st7305_alloc_internal((size_t)5 * ST7305_W);
        if (bits_) tonerow_ = bits_ + (size_t)4 * ST7305_W;
    }

    if (err_ && bits_) return true;

    freeDiffusion();                        /* all or nothing */
    return false;
}

/* ONLY EVER CALLED WITH NOTHING RUNNING - see the note on rebuildDither(). A
 * blit reading these buffers on another core while this runs is a
 * use-after-free, and it is not a theoretical one: it crashed the board. */
void ST7305Tft::freeDiffusion()
{
    free(err_);
    free(bits_);
    err_     = nullptr;
    bits_    = nullptr;
    tonerow_ = nullptr;                     /* points into bits_, never freed */
    t_tone_us_ = t_loop_us_ = t_asm_us_ = 0;
}

void ST7305Tft::rebuildDither()
{
    if (mono_ == ST7305_MONO_FLOYD || mono_ == ST7305_MONO_ATKINSON) {
        if (!ensureDiffusion()) {
            /* 4 KB that is not there. Say so once and carry on in a mode that
             * needs no allocation at all, rather than leave blitIndexedStretched
             * with a null buffer to test for on every frame. */
            Serial.println("[VIDEO] no room for the error-diffusion scratch - "
                           "staying on BAY4");
            mono_ = ST7305_MONO_BAYER4;
        }
    }
    /* AND NO else THAT FREES THEM, WHICH IS THE ONE THING IN THIS FUNCTION THAT
     * IS NOT AN OPTIMISATION QUESTION.
     *
     * This runs on whichever thread changed a setting - in the emulator that is
     * the gamepad chord, on the emulator's own core - while the blits run
     * wherever the caller put them, which in osd.cpp is the other core. Every
     * other thing this function touches is a table of plain bytes: a reader
     * that catches it mid-rebuild gets a frame built from a mix of the old and
     * new tables, which is one odd-looking frame out of eighteen a second and
     * costs nothing to allow.
     *
     * free() is not like that. Handing the allocator memory another core is
     * three hundred rows deep in reading is a use-after-free, and leaving the
     * pointer null behind it is a null dereference on the next frame. It
     * crashed the board on a LoadProhibited at address 0, from the GRAM
     * assembly loop, the first time a mode chord moved off a diffusion mode
     * while one was on screen.
     *
     * So the scratch is allocated when a diffusion mode is first selected and
     * kept for the life of the object. It costs 4.4 KB that a build which never
     * selects one never pays, and it makes the pointers immutable-once-set,
     * which is what makes every other race in here benign. */

    rebuildTone();
    buildMatrix();

    for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
            const int t = thr_[r][c];
            /* Landscape GRAM: rows 0..3 of a byte occupy bit 1+2r, minus one
             * for an odd column. Derived from the layout in AGENTS.md section 2
             * with iy = 299 - y and the row group aligned to 4, which is what
             * lets blitIndexed() write whole bytes.
             *
             * The BIT comes from r & 3 and c & 1 because a GRAM byte is 2
             * columns by 4 rows; the THRESHOLD comes from the full r and c
             * because the dither cell is 8 by 8. One 8x8 cell therefore spans
             * two byte rows and four byte columns, and that is the only reason
             * the blits need a base offset into this table at all. */
            const uint8_t bit = (uint8_t)(1u << (1 + 2 * (r & 3) - (c & 1)));
            uint8_t      *dst = dither_[r * 8 + c];

            for (int v = 0; v < 256; v++)
                dst[v] = ((int)tone_[v] > t) ? bit : 0; /* set bit = paper */
        }
    }
}

void ST7305Tft::blitIndexed(uint8_t *const *lines, int w, int h)
{
    if (!lines) return;

    const int cw = w & ~3;                  /* whole byte-column pairs */
    const int ch = h & ~3;                  /* whole 4-row groups      */
    if (cw <= 0 || ch <= 0) return;
    if (cw > ST7305_W || ch > ST7305_H) return;

    const int x0     = blitOriginX(cw);     /* multiple of 4 */
    const int y0     = blitOriginY(ch);     /* multiple of 4 */
    const int bx0    = x0 >> 1;             /* first GRAM byte column   */
    const int page0  = (ST7305_H - 4 - y0) >> 2; /* GRAM page of row group 0 */
    const int groups = ch >> 2;
    const int pairs  = cw >> 1;             /* even, since cw is a multiple of 4 */

    /* dither_ is [(y & 7) * 8 + (x & 7)][index], so a byte's eight source
     * pixels sit at FIXED offsets from wherever its 8x8 cell starts:
     *
     *      r*8 + c  for r = row in the byte 0..3, c = column in the byte 0..3
     *
     * which is 0,1,8,9,16,17,24,25 for the left byte and 2,3,10,11,18,19,26,27
     * for the right one. All eight are compile-time constants, exactly as they
     * were when the cell was 4x4; the only thing an 8x8 cell adds is `base`,
     * which says whether this byte lands in the cell's top or bottom half and
     * its left or right half.
     *
     * x0 and y0 are multiples of 4, so each is one of two positions and never
     * anything in between - hence the & 1 on the count of 4s rather than a
     * modulo. That is what keeps the inner loop free of any per-pixel phase. */
    for (int g = 0; g < groups; g++) {
        const uint8_t *s0 = lines[g * 4 + 0];
        const uint8_t *s1 = lines[g * 4 + 1];
        const uint8_t *s2 = lines[g * 4 + 2];
        const uint8_t *s3 = lines[g * 4 + 3];
        uint8_t *dst = lcd_.fb + (size_t)bx0 * ST7305_STRIDE
                               + (size_t)(page0 - g);
        const int rb = ((((y0 >> 2) + g) & 1) ? 4 : 0) * 8;

        for (int p = 0; p < pairs; p += 2) {
            const int sx = p << 1;          /* source column of this byte pair */
            const int cb = (((x0 >> 2) + (sx >> 2)) & 1) ? 4 : 0;
            const uint8_t *const d = &dither_[rb + cb][0];

            dst[0] = (uint8_t)(d[  0 * 256 + s0[sx    ]] |
                               d[  1 * 256 + s0[sx + 1]] |
                               d[  8 * 256 + s1[sx    ]] |
                               d[  9 * 256 + s1[sx + 1]] |
                               d[ 16 * 256 + s2[sx    ]] |
                               d[ 17 * 256 + s2[sx + 1]] |
                               d[ 24 * 256 + s3[sx    ]] |
                               d[ 25 * 256 + s3[sx + 1]]);

            dst[ST7305_STRIDE] =
                     (uint8_t)(d[  2 * 256 + s0[sx + 2]] |
                               d[  3 * 256 + s0[sx + 3]] |
                               d[ 10 * 256 + s1[sx + 2]] |
                               d[ 11 * 256 + s1[sx + 3]] |
                               d[ 18 * 256 + s2[sx + 2]] |
                               d[ 19 * 256 + s2[sx + 3]] |
                               d[ 26 * 256 + s3[sx + 2]] |
                               d[ 27 * 256 + s3[sx + 3]]);

            dst += 2 * ST7305_STRIDE;
        }
    }
    dirty_ = true;
}

void ST7305Tft::blitIndexedStretched(uint8_t *const *lines, int w, int srcY0,
                                     int srcH)
{
    if (!lines || w <= 0 || srcH <= 0 || srcY0 < 0) return;

    if (w != map_w_ || srcY0 != map_y0_ || srcH != map_h_) {
        /* Centre-sampled nearest neighbour: the source coordinate under the
         * CENTRE of the destination pixel, not under its left edge. Same cost,
         * but it spreads the doubled-up columns evenly instead of biasing the
         * whole picture half a source pixel to one side - visible at 256->400,
         * where every other column is duplicated. */
        for (int dx = 0; dx < ST7305_W; dx++)
            colmap_[dx] = (uint16_t)(((2 * dx + 1) * w) / (2 * ST7305_W));
        for (int dy = 0; dy < ST7305_H; dy++)
            rowmap_[dy] = (uint16_t)(srcY0 +
                                     ((2 * dy + 1) * srcH) / (2 * ST7305_H));
        map_w_  = w;
        map_y0_ = srcY0;
        map_h_  = srcH;
    }

    /* The two modes that cannot be a table get their own pass over the same
     * maps. Everything above this line - validation, the geometry, the source
     * coordinate maps - is shared, and everything below it is the ordered
     * path.
     *
     * THE err_/bits_ TEST IS NOT DEFENSIVE, IT IS THE CONTRACT. setMonoMode()
     * only records what was asked for; rebuildDither() is what allocates the
     * scratch, and it is also what the header promises a mode takes effect on.
     * Testing the buffers rather than the mode alone is what makes those two
     * statements the same statement - a caller who sets a diffusion mode and
     * blits without rebuilding gets the previous ordered picture, which is what
     * "takes effect on the next rebuildDither()" means, instead of a null
     * dereference 120000 pixels deep. */
    if ((mono_ == ST7305_MONO_FLOYD || mono_ == ST7305_MONO_ATKINSON)
        && err_ && bits_) {
        blitStretchedDiffused(lines, mono_ == ST7305_MONO_ATKINSON);
        dirty_ = true;
        return;
    }

    /* Destination is the whole panel, so x0 = y0 = 0 and the general phase
     * arithmetic in blitIndexed() collapses: the row half is just the parity of
     * the group, and the column half is bit 2 of dx. See that function for what
     * the eight constants are and why they are constants. */
    const uint16_t *const cm = colmap_;

    for (int g = 0; g < ST7305_H / 4; g++) {
        const uint8_t *s0 = lines[rowmap_[g * 4 + 0]];
        const uint8_t *s1 = lines[rowmap_[g * 4 + 1]];
        const uint8_t *s2 = lines[rowmap_[g * 4 + 2]];
        const uint8_t *s3 = lines[rowmap_[g * 4 + 3]];
        /* Page of row group g, counting down from the bottom of GRAM, with the
         * byte column at 0: lcd_.fb + 0 * ST7305_STRIDE + (page0 - g). */
        uint8_t *dst = lcd_.fb + (size_t)(ST7305_H / 4 - 1 - g);
        const int rb = ((g & 1) ? 4 : 0) * 8;

        for (int dx = 0; dx < ST7305_W; dx += 4) {
            const int c0 = cm[dx],     c1 = cm[dx + 1];
            const int c2 = cm[dx + 2], c3 = cm[dx + 3];
            const uint8_t *const d = &dither_[rb + (dx & 4)][0];

            dst[0] = (uint8_t)(d[  0 * 256 + s0[c0]] |
                               d[  1 * 256 + s0[c1]] |
                               d[  8 * 256 + s1[c0]] |
                               d[  9 * 256 + s1[c1]] |
                               d[ 16 * 256 + s2[c0]] |
                               d[ 17 * 256 + s2[c1]] |
                               d[ 24 * 256 + s3[c0]] |
                               d[ 25 * 256 + s3[c1]]);

            dst[ST7305_STRIDE] =
                     (uint8_t)(d[  2 * 256 + s0[c2]] |
                               d[  3 * 256 + s0[c3]] |
                               d[ 10 * 256 + s1[c2]] |
                               d[ 11 * 256 + s1[c3]] |
                               d[ 18 * 256 + s2[c2]] |
                               d[ 19 * 256 + s2[c3]] |
                               d[ 26 * 256 + s3[c2]] |
                               d[ 27 * 256 + s3[c3]]);

            dst += 2 * ST7305_STRIDE;
        }
    }
    dirty_ = true;
}

/* --- error diffusion --------------------------------------------------------
 *
 * The other half of blitIndexedStretched(), and everything the ordered path is
 * not: sequential, stateful, and MUCH more expensive - 11x on a host, because
 * the ordered path is branch-free and independent and pipelines almost for
 * free, while every pixel here depends on the one before it. Called only with
 * arguments that path has already validated and maps it has already built.
 *
 * TWO THINGS SHAPE THIS CODE and neither is the diffusion arithmetic.
 *
 * First, it runs at DESTINATION resolution - 120000 pixels, not the source's
 * 57344. Diffusing at the source and then upscaling would be four times
 * cheaper and would throw away the entire point: the error has to be settled
 * against the pixels that actually exist on the panel, or the nearest-neighbour
 * duplication smears each decision across two columns and the result is a
 * blocky mess with none of the tonal accuracy the method is for.
 *
 * Second, it has to reconcile two incompatible orders. Diffusion must run row
 * by row, because a pixel's input depends on its neighbours' outputs. GRAM must
 * be written in 4-row groups, because that is what a byte is. So the row loop
 * banks its decisions in bits_ and only assembles bytes on every fourth row -
 * which is also why bits_ has exactly four rows and not three hundred.
 *
 * Serpentine, i.e. alternate rows run right to left. It costs one sign and it
 * stops the error from marching consistently in one direction, which is what
 * produces the diagonal "worms" plain left-to-right diffusion is known for.
 * ------------------------------------------------------------------------- */
/* FIVE THINGS WERE DONE TO THIS LOOP AND THEY ARE WHY IT LOOKS ODD.
 *
 * MEASURED ON HARDWARE, both spellings on core 0 against a full-speed
 * emulator, which is the only comparison that ever meant anything here:
 *
 *              tone + loop      asm      dither
 *   plain         50.2 ms      2.0 ms    53.1 ms
 *   this          42.4 ms      1.6 ms    44.9 ms      15% faster
 *
 * IT TOOK THREE TRIES TO ESTABLISH THAT, and the two failures are worth more
 * than the result. A host benchmark said +27%; the board said slower. What the
 * board was actually saying was that the two versions were never measured under
 * the same conditions - one ran while the emulator was starved to 26 fps and
 * barely touching PSRAM, the other while it ran flat out and contended for it.
 * The lesson is not "do not optimise", it is: change ONE thing, and read
 * dither=, tone=, loop= and asm= off the board with everything else held still.
 *
 * Every one of the five attacks the same thing: an in-order core stalls on a
 * load whose address or value depends on the instruction in front of it, and
 * the plain spelling is nothing but such loads.
 *
 * 1. THE TONE LOOKUP IS LIFTED OUT OF THE ROW. It was tone_[s[colmap_[x]]] -
 * three loads, each depending on the last, per pixel. It is now a separate pass
 * into tonerow_, where the 400 chains are independent and pipeline freely,
 * leaving the diffusion loop one flat load. The pass is SKIPPED whenever the
 * nearest-neighbour map repeats a source row, which for 224 rows stretched to
 * 300 is a quarter of them.
 *
 * 2. THE SAME-ROW ERROR NEVER REACHES MEMORY. Both rules push error onto the
 * pixel immediately ahead, which the very next iteration reads back: a store
 * followed at once by a load of the same address, the worst shape there is. It
 * lives in `carry` (and `carry2`, for Atkinson's second tap) instead.
 *
 * 3. THE NEXT-ROW WRITES ARE A TWO-DEEP REGISTER PIPELINE. Each next-row cell
 * collects from three consecutive pixels, so its total is not known until the
 * third - but it is known then, and never needed again. Holding the two
 * outstanding cells in `A` and `B` turns three read-modify-writes per pixel
 * into ONE store for Floyd-Steinberg, and one load plus one store for Atkinson,
 * which also has to preserve what the row before it left there.
 *
 * 4. bits_ HOLDS PRE-SHIFTED BITS, so assembling a GRAM byte is eight loads and
 * seven ORs - the shape of the ordered path's inner loop. Worth 0.4 ms.
 *
 * 5. POINTERS, NOT SUBSCRIPTS. The loops touch four or five arrays by the same
 * index and Xtensa has no scaled-index addressing, so each subscript was an
 * address computation of its own. Incrementing pointers folds them into the
 * loads and stores.
 *
 * The arithmetic is untouched by all of it: test/blit_test.cpp checks both
 * rules byte-for-byte against a plainly-spelled reference with a full-frame
 * error plane, and that is the check that made this safe to write.
 *
 * TWO MORE THINGS SHAPE THE CODE, and neither is the diffusion arithmetic.
 *
 * First, it runs at DESTINATION resolution - 120000 pixels, not the source's
 * 57344. Diffusing at the source and upscaling would be twice as cheap and
 * would throw away the point: the error has to be settled against the pixels
 * that actually exist on the panel, or the nearest-neighbour duplication smears
 * each decision across two columns and the result is a blocky mess with none of
 * the tonal accuracy the method is for.
 *
 * Second, it reconciles two incompatible orders. Diffusion must run row by row,
 * because a pixel's input depends on its neighbours' outputs. GRAM must be
 * written in 4-row groups, because that is what a byte is. So the row loop banks
 * its decisions in bits_ and assembles bytes on every fourth row - which is also
 * why bits_ has four rows and not three hundred.
 *
 * Serpentine, i.e. alternate rows run right to left. It costs one sign and it
 * stops the error marching consistently in one direction, which is what produces
 * the diagonal "worms" plain left-to-right diffusion is known for.
 * ------------------------------------------------------------------------- */
void ST7305Tft::blitStretchedDiffused(uint8_t *const *lines, bool atkinson)
{
    const int PAD = 2;                      /* the widest tap either rule uses */
    const int ROW = ST7305_W + 2 * PAD;

    /* Errors are carried in 1/16 of a luminance step, which is what lets the
     * weights be added as plain integers: Floyd-Steinberg's 7/16 is "+ err*7"
     * and Atkinson's 1/8 is "+ err*2", with the single >> 4 on the way back out
     * paying for all of it. No division and no float anywhere in here. */
    memset(err_, 0, (size_t)3 * ROW * sizeof *err_);
    int16_t *e0 = err_ + 0 * ROW + PAD;     /* this row          */
    int16_t *e1 = err_ + 1 * ROW + PAD;     /* the next one      */
    int16_t *e2 = err_ + 2 * ROW + PAD;     /* two ahead, Atkinson only */

    int cached_src = -1;                    /* which source row tonerow_ holds */

    uint32_t tone_us = 0, loop_us = 0, asm_us = 0;

    for (int y = 0; y < ST7305_H; y++) {
        /* (1) The row's tone, once, off the diffusion loop's critical path.
         *
         * STRAIGHT-LINE ON PURPOSE. An earlier version skipped the lookup when
         * colmap_ repeated a source column, which it does for 144 of the 400 -
         * but the run lengths of a 256-to-400 stretch alternate irregularly, so
         * that test is a branch nothing can predict, and 90000 mispredicts a
         * frame cost more than the two loads they save. The skip that IS worth
         * having is the one on whole rows, whose condition holds for long runs
         * and predicts perfectly. */
        if ((int)rowmap_[y] != cached_src) {
            const uint32_t t0 = micros();
            const uint8_t  *const s   = lines[rowmap_[y]];
            const uint16_t       *cmp = colmap_;
            uint8_t              *tp  = tonerow_;
            for (int n = ST7305_W; n; n--)
                *tp++ = tone_[s[*cmp++]];
            cached_src = (int)rowmap_[y];
            tone_us += micros() - t0;
        }

        /* (4) Where a decision for this row lands inside its GRAM byte. The
         * row's part, 1 + 2r, is fixed for the whole row; the column's part is
         * one less for an odd column. */
        const int      r      = y & 3;
        const int      shbase = 1 + 2 * r;
        uint8_t *const bo     = bits_ + (size_t)r * ST7305_W;

        const int dir = (y & 1) ? -1 : 1;
        const int beg = (y & 1) ? ST7305_W - 1 : 0;

        /* (2) same-row error, and (3) the next row's two outstanding cells. */
        int carry = 0, carry2 = 0;
        int A = 0, B = 0;

        /* (5) walked, not indexed. */
        const uint8_t *tp  = tonerow_ + beg;
        const int16_t *ep  = e0 + beg;
        uint8_t       *bp  = bo + beg;
        int16_t       *e1p = e1 + beg - dir;
        int16_t       *e2p = e2 + beg;

        /* The bit position depends on the column's parity and nothing else, so
         * it can be flipped rather than recomputed - which is what lets the
         * loop drop x entirely and count down instead. */
        const int shflip = 2 * shbase - 1;
        int       sh     = shbase - (beg & 1);

        const uint32_t t1 = micros();

        if (atkinson) {
            /* Six neighbours at 1/8 each, so only 6/8 of the error is passed on
             * at all. Discarding the rest is not a bug and not an approximation
             * of Floyd-Steinberg - it is the whole character of the thing. It
             * clips the extremes, which costs detail in deep shadow and bright
             * highlight and buys contrast and much flatter flat areas
             * everywhere in between. */
            for (int n = ST7305_W; n; n--) {
                int v = (int)*tp + ((*ep + carry) >> 4);

                /* Clamping the ACCUMULATOR rather than the error is deliberate:
                 * it bounds what one cell can be handed, which keeps the int16
                 * accumulation honest at every real input, while still letting a
                 * run of black or white push error along instead of swallowing
                 * it the way clamping to 0..255 would. */
                if (v < -256) v = -256; else if (v > 511) v = 511;

                const int white = (v > 127);
                *bp = (uint8_t)(white << sh);
                sh  = shflip - sh;

                const int q = (v - (white ? 255 : 0)) * 2;

                carry  = carry2 + q;        /* was 2 ahead, is now 1 ahead */
                carry2 = q;

                /* += and not =, because the row two above already put its own
                 * tap here when this buffer was its e2. */
                *e1p = (int16_t)(*e1p + A + q);
                A = B + q;
                B = q;

                /* Each cell is written exactly once per row, so no read. */
                *e2p = (int16_t)q;

                tp += dir; ep += dir; bp += dir; e1p += dir; e2p += dir;
            }
            /* e1p has landed on the last pixel's own cell, which has had every
             * contribution it is ever going to get. */
            *e1p = (int16_t)(*e1p + A);
            e1p += dir;
            *e1p = (int16_t)(*e1p + B);     /* padding */
        } else {
            /* Floyd-Steinberg, 7/3/5/1 of 16, mirrored with `dir`. All of the
             * error is passed on, which is what makes this the most tonally
             * accurate of the eight modes and also the one whose pattern is
             * most sensitive to a one-pixel change upstream - see the note
             * about crawling in st7305_mono_t. */
            for (int n = ST7305_W; n; n--) {
                int v = (int)*tp + ((*ep + carry) >> 4);
                if (v < -256) v = -256; else if (v > 511) v = 511;

                const int white = (v > 127);
                *bp = (uint8_t)(white << sh);
                sh  = shflip - sh;

                const int err = v - (white ? 255 : 0);

                carry = err * 7;

                /* The cell one behind has now had all three of its
                 * contributions - 1 from two back, 5 from one back, 3 from
                 * here - so it is final. And nothing else writes this buffer
                 * under Floyd-Steinberg, which is what lets this be a store
                 * rather than an accumulate. */
                *e1p = (int16_t)(A + err * 3);
                A = B + err * 5;
                B = err;

                tp += dir; ep += dir; bp += dir; e1p += dir;
            }
            *e1p = (int16_t)A;
            e1p += dir;
            *e1p = (int16_t)B;              /* padding */
        }

        loop_us += micros() - t1;

        /* Roll the three-row window: the row just finished is spent and becomes
         * the row two ahead.
         *
         * NO memset OF THE RECYCLED ROW, and that is worth 240 KB of stores a
         * frame. It was there because the plain spelling accumulated into a
         * buffer it assumed was clean; both loops above now write every one of
         * the 400 real cells unconditionally - Floyd-Steinberg through
         * e1[x - dir] plus the two flushed cells, Atkinson through e2[x] - so
         * there is nothing stale left to clear. Only the four padding cells
         * still accumulate, and only because the boundary iterations read and
         * write them; nothing ever reads padding into a pixel, but they are
         * zeroed anyway to keep the values bounded and the argument short. */
        int16_t *const spent = e0;
        e0 = e1;
        e1 = e2;
        e2 = spent;
        e2[-2] = e2[-1] = e2[ST7305_W] = e2[ST7305_W + 1] = 0;

        if (r != 3) continue;

        /* A whole 4-row group is decided, so its 200 GRAM bytes can go out. The
         * same layout as the ordered path - one byte per 2 columns x 4 rows -
         * and now the same shape too, the bit positions having been applied
         * upstream. */
        const uint32_t t2 = micros();

        const uint8_t *const b0 = bits_ + 0 * ST7305_W;
        const uint8_t *const b1 = bits_ + 1 * ST7305_W;
        const uint8_t *const b2 = bits_ + 2 * ST7305_W;
        const uint8_t *const b3 = bits_ + 3 * ST7305_W;
        uint8_t *dst = lcd_.fb + (size_t)(ST7305_H / 4 - 1 - (y >> 2));

        for (int dx = 0; dx < ST7305_W; dx += 4) {
            dst[0] = (uint8_t)(b0[dx    ] | b0[dx + 1] |
                               b1[dx    ] | b1[dx + 1] |
                               b2[dx    ] | b2[dx + 1] |
                               b3[dx    ] | b3[dx + 1]);

            dst[ST7305_STRIDE] =
                     (uint8_t)(b0[dx + 2] | b0[dx + 3] |
                               b1[dx + 2] | b1[dx + 3] |
                               b2[dx + 2] | b2[dx + 3] |
                               b3[dx + 2] | b3[dx + 3]);

            dst += 2 * ST7305_STRIDE;
        }

        asm_us += micros() - t2;
    }

    t_tone_us_ = tone_us;
    t_loop_us_ = loop_us;
    t_asm_us_  = asm_us;
}
