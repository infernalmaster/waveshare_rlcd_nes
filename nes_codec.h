/* nes_codec.h - ES8311 playback bring-up for the ESP32-S3-RLCD-4.2.
 *
 * WHY THIS EXISTS AT ALL. The emulator's audio path was written for a
 * MAX98357A: a class-D amplifier with no control interface, where pushing I2S
 * samples is the entire driver. This board has an ES8311 codec instead, which
 * is a chip with registers. Until those registers are written it holds its DAC
 * in reset and its output muted, so the I2S stream lands nowhere - the failure
 * mode is perfect silence with no error anywhere, which is why ENABLE_SOUND was
 * off rather than merely unwired.
 *
 * There are three separate things that each independently produce that silence,
 * and all three are handled here or by the caller:
 *
 *   1. The ES8311 register sequence (this file). Ported from Espressif's
 *      esp_codec_dev driver, in the vendor's own copy at
 *      02_Example/Arduino/07_Audio_Test/src/ExternLib/esp_codec_dev/, reduced to
 *      the playback path. Not written from the datasheet and not from memory.
 *
 *   2. MCLK. The codec runs its DAC from a master clock the ESP32 has to
 *      generate on GPIO 16 at 256x the sample rate. The board config says
 *      use_mclk: 1, so there is no internal-oscillator fallback. Installing I2S
 *      without a mclk pin gets you a codec that acknowledges every I2C write and
 *      still makes no sound. osd_init_sound() owns this.
 *
 *   3. The power amplifier enable on GPIO 46. Nothing reaches the speaker until
 *      it is driven high. This pin was missing from hw_config.h entirely - it is
 *      in the vendor board table (S3_RLCD_4_2, "pa: 46"), not in the pin comments
 *      the rest of that file was built from.
 *
 * Everything here is source: pins and I2C address from the vendor's
 * board_cfg.h entry for S3_RLCD_4_2, register writes from es8311.c, clock
 * coefficients transcribed from that driver's own table.
 */
#ifndef NES_CODEC_H
#define NES_CODEC_H

#include <stdbool.h>

/* Brings up I2C, configures the ES8311 for `sample_rate` and enables the
 * amplifier. Call AFTER i2s_driver_install(), because the codec needs MCLK
 * running while it is configured.
 *
 * Only sample rates with an entry in the clock table are accepted - 8000,
 * 16000, 24000, 32000, 44100 and 48000. Returns false and logs the reason on
 * any failure, including the codec not answering on I2C at all. */
bool nes_codec_begin(int sample_rate);

/* 0 = mute, 100 = 0 dBFS on the DAC. Between them the scale is dB, not linear:
 * percent 50 is -20 dB, not half as loud in amplitude.
 *
 * This is a SECOND volume control - the emulator already scales samples by
 * master_volume in software, and that is the one the in-game menu drives. Set
 * this once at startup and leave it; two controls fighting over the same signal
 * only makes the quiet end noisy. */
void nes_codec_set_volume(int percent);

/* Cuts the amplifier without touching the codec. Worth using around anything
 * that stops feeding I2S, since a starved DMA buffer repeats its last block. */
void nes_codec_enable_amp(bool on);

/* False until nes_codec_begin() has succeeded. */
bool nes_codec_ready(void);

#endif /* NES_CODEC_H */
