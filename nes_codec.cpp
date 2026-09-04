/* nes_codec.cpp - see nes_codec.h for why this file exists.
 *
 * A reduced port of Espressif's es8311.c (esp_codec_dev), playback only, over
 * Arduino Wire instead of the ESP-IDF i2c_master driver. The vendor's own copy
 * of the original is in this repository at
 *   02_Example/Arduino/07_Audio_Test/src/ExternLib/esp_codec_dev/device/es8311/
 * and every register write below corresponds to one there. Where this deviates
 * it says so.
 *
 * Pulling in esp_codec_dev whole was the alternative and was rejected: it wants
 * the ESP-IDF i2c_master and i2s_std drivers, and the emulator's audio path is
 * built on the legacy driver/i2s.h. Mixing the two I2S APIs in one binary does
 * not work. This is ~200 lines against that.
 */
#include <Arduino.h>
#include <Wire.h>

#include "hw_config.h"
#include "nes_codec.h"

#if defined(NES_USE_ES8311) && NES_USE_ES8311

/* From the vendor board table: "Board: S3_RLCD_4_2 ... i2c_addr" defaults to
 * the ES8311's 7-bit address with CE tied low. */
#define ES8311_I2C_ADDR 0x18

/* Register names as in es8311_reg.h. Only the ones this path touches. */
#define REG_RESET       0x00
#define REG_CLK_MGR01   0x01
#define REG_CLK_MGR02   0x02
#define REG_CLK_MGR03   0x03
#define REG_CLK_MGR04   0x04
#define REG_CLK_MGR05   0x05
#define REG_CLK_MGR06   0x06
#define REG_CLK_MGR07   0x07
#define REG_CLK_MGR08   0x08
#define REG_SDPIN09     0x09    /* DAC serial port */
#define REG_SDPOUT0A    0x0A    /* ADC serial port */
#define REG_SYSTEM0B    0x0B
#define REG_SYSTEM0C    0x0C
#define REG_SYSTEM0D    0x0D    /* power up/down */
#define REG_SYSTEM0E    0x0E
#define REG_SYSTEM10    0x10
#define REG_SYSTEM11    0x11
#define REG_SYSTEM12    0x12    /* enable DAC */
#define REG_SYSTEM13    0x13
#define REG_SYSTEM14    0x14
#define REG_ADC15       0x15
#define REG_ADC16       0x16
#define REG_ADC17       0x17
#define REG_ADC1B       0x1B
#define REG_ADC1C       0x1C
#define REG_DAC32       0x32    /* DAC volume */
#define REG_DAC37       0x37    /* DAC ramp rate */
#define REG_GPIO44      0x44
#define REG_GP45        0x45
#define REG_CHIP_ID1    0xFD    /* 0x83 */
#define REG_CHIP_ID2    0xFE    /* 0x11 */

/* MCLK is always 256 x Fs here, which is esp_codec_dev's MCLK_DEFAULT_DIV and
 * what osd_init_sound() asks the I2S peripheral for. That collapses the
 * driver's 90-row coefficient table to these six lines - every other row
 * describes an MCLK ratio this board never generates.
 *
 * Transcribed from coeff_div[] in es8311.c, taking the row whose mclk equals
 * 256 x rate. Do not "simplify" the 8 kHz row: its pre_multi really is 2 while
 * every other rate uses 1. That is what the table says. */
struct es_coeff {
    uint32_t rate;
    uint8_t  pre_div, pre_multi, adc_div, dac_div;
    uint8_t  fs_mode, lrck_h, lrck_l, bclk_div, adc_osr, dac_osr;
};

static const es_coeff k_coeffs[] = {
    /* rate   pre_div multi adc_div dac_div fs lrck_h lrck_l bclk adc_osr dac_osr */
    {  8000,  0x01, 0x02, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x20 },
    { 16000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x20 },
    { 24000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10 },
    { 32000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10 },
    { 44100,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10 },
    { 48000,  0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x04, 0x10, 0x10 },
};

static bool s_ready = false;

/* --- I2C ------------------------------------------------------------------- */

static bool es_write(uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    Wire.write(val);
    const uint8_t err = Wire.endTransmission();
    if (err != 0) {
        Serial.printf("[CODEC] I2C write reg 0x%02X failed (%u)\n", reg, err);
        return false;
    }
    return true;
}

/* Returns -1 on a bus error, so a failed read is distinguishable from a
 * register that legitimately holds 0. */
static int es_read(uint8_t reg)
{
    Wire.beginTransmission(ES8311_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom((uint8_t)ES8311_I2C_ADDR, (uint8_t)1) != 1) return -1;
    return Wire.read();
}

/* Read-modify-write, which most of the clock configuration needs: several of
 * these registers carry bits this path must not disturb. */
static bool es_update(uint8_t reg, uint8_t keep_mask, uint8_t set_bits)
{
    const int old = es_read(reg);
    if (old < 0) return false;
    return es_write(reg, (uint8_t)((old & keep_mask) | set_bits));
}

/* --- configuration --------------------------------------------------------- */

static const es_coeff *find_coeff(int rate)
{
    for (unsigned i = 0; i < sizeof k_coeffs / sizeof k_coeffs[0]; i++)
        if (k_coeffs[i].rate == (uint32_t)rate) return &k_coeffs[i];
    return NULL;
}

/* es8311_config_sample(), with mclk_div fixed at 256. */
static bool es_config_clocks(const es_coeff *c)
{
    bool ok = true;

    /* REG02: pre-divider and pre-multiplier. The multiplier is encoded as
     * log2 - 1,2,4,8 become 0,1,2,3. */
    uint8_t multi_code = 0;
    switch (c->pre_multi) {
        case 1: multi_code = 0; break;
        case 2: multi_code = 1; break;
        case 4: multi_code = 2; break;
        case 8: multi_code = 3; break;
        default: break;
    }
    ok &= es_update(REG_CLK_MGR02, 0x07,
                    (uint8_t)(((c->pre_div - 1) << 5) | (multi_code << 3)));

    ok &= es_write(REG_CLK_MGR05,
                   (uint8_t)(((c->adc_div - 1) << 4) | (c->dac_div - 1)));

    ok &= es_update(REG_CLK_MGR03, 0x80,
                    (uint8_t)((c->fs_mode << 6) | c->adc_osr));
    ok &= es_update(REG_CLK_MGR04, 0x80, c->dac_osr);

    ok &= es_update(REG_CLK_MGR07, 0xC0, c->lrck_h);
    ok &= es_write(REG_CLK_MGR08, c->lrck_l);

    /* The driver's odd-looking bclk encoding: divisors below 19 are stored
     * minus one, 19 and above are stored as-is. Kept rather than cleaned up,
     * because it is the chip's encoding and not a bug in the driver. */
    ok &= es_update(REG_CLK_MGR06, 0xE0,
                    (uint8_t)(c->bclk_div < 19 ? c->bclk_div - 1 : c->bclk_div));
    return ok;
}

bool nes_codec_begin(int sample_rate)
{
    s_ready = false;

    const es_coeff *coeff = find_coeff(sample_rate);
    if (!coeff) {
        Serial.printf("[CODEC] no clock coefficients for %d Hz\n", sample_rate);
        return false;
    }

    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);

    /* Identify before configuring. Without this an unpopulated or misrouted
     * codec produces the same silence as a mis-set register, and there would be
     * nothing in the log to tell them apart. */
    const int id1 = es_read(REG_CHIP_ID1);
    const int id2 = es_read(REG_CHIP_ID2);
    if (id1 != 0x83 || id2 != 0x11) {
        Serial.printf("[CODEC] ES8311 not found at 0x%02X (id %02X %02X, want 83 11)\n",
                      ES8311_I2C_ADDR, id1, id2);
        return false;
    }
    Serial.println("[CODEC] ES8311 found");

    bool ok = true;

    /* Written twice on purpose. The upstream driver notes that the first I2C
     * write after power-on is occasionally dropped by this chip; the second is
     * what actually lands. */
    ok &= es_write(REG_GPIO44, 0x08);
    ok &= es_write(REG_GPIO44, 0x08);

    ok &= es_write(REG_CLK_MGR01, 0x30);
    ok &= es_write(REG_CLK_MGR02, 0x00);
    ok &= es_write(REG_CLK_MGR03, 0x10);
    ok &= es_write(REG_ADC16,     0x24);
    ok &= es_write(REG_CLK_MGR04, 0x10);
    ok &= es_write(REG_CLK_MGR05, 0x00);
    ok &= es_write(REG_SYSTEM0B,  0x00);
    ok &= es_write(REG_SYSTEM0C,  0x00);
    ok &= es_write(REG_SYSTEM10,  0x1F);
    ok &= es_write(REG_SYSTEM11,  0x7F);
    ok &= es_write(REG_RESET,     0x80);

    /* Slave mode: the ESP32 drives BCLK and LRCK. Bit 6 clear. */
    ok &= es_update(REG_RESET, 0xBF, 0x00);

    /* 0x3F with bit 7 clear = take the master clock from the MCLK pin rather
     * than deriving it from BCLK, and do not invert it. use_mclk is 1 for this
     * board, so the BCLK-derived fallback is not an option here. */
    ok &= es_write(REG_CLK_MGR01, 0x3F);
    ok &= es_update(REG_CLK_MGR06, (uint8_t)~0x20, 0x00);   /* SCLK not inverted */

    ok &= es_write(REG_SYSTEM13, 0x10);
    ok &= es_write(REG_ADC1B,    0x0A);
    ok &= es_write(REG_ADC1C,    0x6A);

    /* Upstream writes 0x58 here when no_dac_ref is false, which routes DAC
     * output back into the ADC as a reference for echo cancellation. This is a
     * playback-only path with no recording, so that loop is left open. */
    ok &= es_write(REG_GPIO44, 0x08);

    ok &= es_config_clocks(coeff);

    /* Serial format: I2S (low two bits clear), 16 bit (0x0C). Applied to both
     * ports - the ADC side costs nothing and keeps the two consistent. */
    int dac_iface = es_read(REG_SDPIN09);
    int adc_iface = es_read(REG_SDPOUT0A);
    if (dac_iface < 0 || adc_iface < 0) {
        Serial.println("[CODEC] cannot read serial port registers");
        return false;
    }
    dac_iface = (dac_iface & 0xFC) | 0x0C;
    adc_iface = (adc_iface & 0xFC) | 0x0C;

    /* Bit 6 holds a serial port in reset. Clear it for the DAC, leave it set
     * for the ADC - nothing reads the microphone and an enabled ADC would only
     * add noise to the shared reference. */
    dac_iface &= ~0x40;
    adc_iface |= 0x40;
    ok &= es_write(REG_SDPIN09,  (uint8_t)dac_iface);
    ok &= es_write(REG_SDPOUT0A, (uint8_t)adc_iface);

    /* es8311_start(), DAC half. */
    ok &= es_write(REG_ADC17,    0xBF);
    ok &= es_write(REG_SYSTEM0E, 0x02);
    ok &= es_write(REG_SYSTEM12, 0x00);   /* DAC enabled */
    ok &= es_write(REG_SYSTEM14, 0x1A);
    ok &= es_write(REG_SYSTEM0D, 0x01);   /* power up */
    ok &= es_write(REG_ADC15,    0x40);
    ok &= es_write(REG_DAC37,    0x08);   /* DAC ramp rate, softens the pop */
    ok &= es_write(REG_GP45,     0x00);

    if (!ok) {
        Serial.println("[CODEC] register programming failed - see errors above");
        return false;
    }

    s_ready = true;
    nes_codec_set_volume(NES_CODEC_VOLUME);

    /* Amplifier last, and after a short settle, so the DAC's own power-up
     * transient does not reach the speaker as a click. */
    delay(20);
    nes_codec_enable_amp(true);

    Serial.printf("[CODEC] ES8311 ready: %d Hz, MCLK %d Hz, PA on GPIO %d\n",
                  sample_rate, sample_rate * 256, I2S_PA_EN);
    return true;
}

void nes_codec_set_volume(int percent)
{
    if (!s_ready) return;
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;

    if (percent == 0) {
        es_write(REG_DAC32, 0x00);        /* register 0 is mute, not -95.5 dB */
        return;
    }

    /* REG32 is 0.5 dB per step over -95.5 .. +32.0 dB, so 0 dB is 191. Map the
     * top 40 dB of that onto 1..100 - a linear percent straight onto the
     * register would put "50%" at -48 dB, which is inaudible. */
    const float db  = (percent - 100) * 0.4f;
    int         reg = (int)((db + 95.5f) * 2.0f + 0.5f);
    if (reg < 1)   reg = 1;
    if (reg > 255) reg = 255;
    es_write(REG_DAC32, (uint8_t)reg);
}

void nes_codec_enable_amp(bool on)
{
    pinMode(I2S_PA_EN, OUTPUT);
    digitalWrite(I2S_PA_EN, on ? HIGH : LOW);
}

bool nes_codec_ready(void)
{
    return s_ready;
}

#else /* !NES_USE_ES8311 */

bool nes_codec_begin(int)        { return true; }
void nes_codec_set_volume(int)   {}
void nes_codec_enable_amp(bool)  {}
bool nes_codec_ready(void)       { return false; }

#endif
