#include <Arduino.h>
#include <SPI.h>
#include <FS.h>
#include "hw_config.h"
#include "tft_driver.h"

/* Defaulted, not required, so this file still builds against an older
 * hw_config.h. Placed here rather than at the first use: a fallback that lands
 * after the #if it protects protects nothing. */
#ifndef NES_PPU_DIAGNOSTICS
#define NES_PPU_DIAGNOSTICS 0
#endif

/* Card access goes through NES_FS so the same code reaches either bus.
 * The ESP32-S3-RLCD-4.2's TF slot is wired to the SDMMC peripheral, not to SPI,
 * so SD.h cannot see it at all - it is not a speed preference. The two objects
 * present the same open()/File API, which is why one macro is enough. */
#if defined(NES_USE_SD_MMC) && NES_USE_SD_MMC
#include <SD_MMC.h>
#define NES_FS SD_MMC
#else
#include <SD.h>
#define NES_FS SD
#endif

/* The splash bitmap is 280x240 of RGB565 fading in and out - meaningless on a
 * 1-bit panel that refreshes in the low tens of Hz, where the fade is 60 flushes
 * of dither noise. Not compiled in for ST7305. */
#if !(defined(NES_USE_ST7305) && NES_USE_ST7305)
#include "turning.h"
#endif

#if ENABLE_SOUND
#include <driver/i2s.h>
/* Board-specific codec bring-up. Lives in the sketch folder, not in this
 * library: which codec sits on the I2S bus is a property of the board, and the
 * emulator core has no business knowing. The !NES_USE_ES8311 half of that file
 * is a set of no-ops, so this include is safe for a plain amplifier too. */
#include "nes_codec.h"
#endif

#include <string.h>
#include <stdarg.h>

extern "C" {
#include "noftypes.h"
#include "bitmap.h"
#include "osd.h"
#include "nofrendo.h"
#include "nesinput.h"
#include "event.h"
#include "nofconfig.h"
/* For the emulated PC in the periodic log line. Small and self-contained,
 * unlike nes.h - see the NES_VISIBLE_Y0 note below for why that one is kept
 * out. */
#include "nes6502.h"

#if NES_PPU_DIAGNOSTICS
/* Defined in nes_ppu.c, for the same log line. Declared here rather than by
 * including nes_ppu.h, which drags in the whole PPU context type for three
 * symbols. */
extern unsigned long nes_spr0_hits;
extern unsigned long nes_ppustat_reads;
extern unsigned long nes_spr0_scanned;
extern unsigned long nes_spr0_opaque;
extern unsigned long nes_spr0_colmask;
extern unsigned long nes_spr0_bgmask;
extern unsigned long nes_bg_vaddr;
extern unsigned long nes_bg_xofs;
extern unsigned long nes_bg_tail;
extern unsigned long nes_bg_pat;
extern unsigned long nes_bg_pataddr;
extern unsigned long nes_chr_writes;
extern unsigned long nes_chr_max;
extern int nes_bg_nt;
extern int ppu_rendering_state(void);
extern void ppu_sprite0(int *y, int *x, int *tile, int *attr);
#endif /* NES_PPU_DIAGNOSTICS */
}

#define NES_SCREEN_WIDTH 256
#define NES_SCREEN_HEIGHT 240

/* THE RENDERER DRAWS OUTSIDE THE 256, IN BOTH DIRECTIONS, AND ALWAYS HAS.
 *
 * ppu_renderbg() starts at `vidbuf - ppu.tile_xofs` and lays down 33 tiles -
 * 264 pixels - because a background scrolled by a fine amount shows part of a
 * 33rd tile. So it writes up to 7 pixels before the line and up to 8 past it.
 * ppu_renderoam() does the same at the right edge: a sprite at x=252 is drawn
 * through x=259, and one at x=254 - which is where Battletoads parks sprite 0 -
 * reaches x=261.
 *
 * With no margin those writes land in the neighbouring lines, because the lines
 * are contiguous. The overshoot past the end is harmless, since the next line
 * is drawn immediately afterwards and overwrites it. The undershoot is not:
 * line N's leading 7 pixels land on the TAIL of line N-1, which was finished a
 * moment ago, and the result is a column of the wrong tile down the right-hand
 * edge of a scrolling picture - visible in Super Mario Bros. whenever the fine
 * scroll is non-zero, which is most of the time.
 *
 * Upstream nofrendo passes 8 here; the line is still in nes.c, commented out,
 * one call site above the one this port replaced it with. Four bmp_create()
 * calls in this file lost the 8 on the way across. It costs 16 bytes a line,
 * 3.8 KB a bitmap, 11.5 KB across the pool - against 8 MB of PSRAM. */
#define NES_BITMAP_OVERDRAW 8

/* What a television actually showed of those 240 rows. NTSC sets cropped about
 * 8 rows top and bottom, and games knew it - the hidden rows carry scroll seams
 * and half-drawn tiles, not picture. nes.h carries the same 224 under the same
 * name for the emulator core; osd.cpp cannot include it (it drags in the whole
 * C core API), hence the #ifndef, which also lets a build flag override both. */
#ifndef NES_VISIBLE_Y0
#define NES_VISIBLE_Y0 8
#endif
#ifndef NES_VISIBLE_HEIGHT
#define NES_VISIBLE_HEIGHT 224
#endif

/* tft_driver.h already defines these to match whichever panel is selected.
 * Only fall back to the ST7789 numbers if it somehow did not. */
#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH 280
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT 240
#endif

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_GAIN 100
#define TARGET_FRAME_MICROS 16666

/* How often nes_audio_service() runs: once per emulated NES frame, which
 * nofrendo drives at NES_REFRESH_RATE. Not the panel's rate - audio is serviced
 * on every frame, including the three in four the ST7305 never displays. */
#define NES_AUDIO_FRAME_RATE 60
 
int master_volume = 100;
bool show_fps = false;
bool select_pressed = false;
static bool runtime_sound_enabled = false;

extern TFTDriver tft;
extern "C" int nes_get_gamepad_state();

#if !(defined(NES_USE_ST7305) && NES_USE_ST7305)

static uint16_t scale_color_565(uint16_t color, uint8_t alpha) {
  uint8_t r = (color >> 11) & 0x1F;
  uint8_t g = (color >> 5) & 0x3F;
  uint8_t b = color & 0x1F;

  r = (r * alpha) / 255;
  g = (g * alpha) / 255;
  b = (b * alpha) / 255;

  return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t swap_rb_565(uint16_t color) {
  uint16_t r = (color >> 11) & 0x1F;
  uint16_t g = (color >> 5) & 0x3F;
  uint16_t b = color & 0x1F;
  return (uint16_t)((b << 11) | (g << 5) | r);
}

static void draw_magic(uint8_t alpha) {
  static uint16_t line[hakki_W];

  for (int y = 0; y < hakki_H; y++) {
    int row = y * hakki_W;
    for (int x = 0; x < hakki_W; x++) {
      uint16_t c = pgm_read_word(&hakki[row + x]);
      c = swap_rb_565(c);
      line[x] = scale_color_565(c, alpha);
    }
    tft.pushImage(0, y, hakki_W, 1, line);
  }
}

static void show_magic() {
  static bool shown = false;
  if (shown) {
    return;
  }
  shown = true;

  const int fade_in_ms = 300;
  const int hold_ms = 500;
  const int fade_out_ms = 500;
  const int steps = 30;
  const int step_delay_in = fade_in_ms / steps;
  const int step_delay_out = fade_out_ms / steps;

  tft.fillScreen(0x0000);
  for (int i = 0; i <= steps; i++) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_in);
  }

  delay(hold_ms);

  for (int i = steps; i >= 0; i--) {
    uint8_t alpha = (uint8_t)((i * 255) / steps);
    draw_magic(alpha);
    delay(step_delay_out);
  }

  tft.fillScreen(0x0000);
}

#endif /* !NES_USE_ST7305 - splash */

static int16_t stereo_buffer[1024];
static int16_t mono_buffer[512];
static void (*emulator_audio_callback)(void *buffer, int length) = NULL;
 
static uint16_t myPalette565[256];
static uint16_t myPalette565_swapped[256];
static bitmap_t *game_bitmap = NULL;
static bool video_ready = false;
static uint16_t *frame_buffer = NULL;  
static bool low_mem_video_mode = false;
 
unsigned long frame_start_time = 0;
unsigned long frame_count = 0;
unsigned long last_fps_update = 0;
int current_fps = 0;
/* TE edges in the last second, i.e. the panel's own scan rate MEASURED rather
 * than restated from NES_PANEL_FPS. Worth its own counter because it is the
 * only thing that reports the one failure nothing else shows: if TE goes silent
 * the flush task falls back to a timer, the tearing comes back, and every other
 * number on screen carries on looking exactly as healthy as before. */
int current_panel_hz = 0;
 
#define INP_JOYPAD0 0x0001
static nesinput_t joypad_p1;

#define HW_MASK_A 0x01
#define HW_MASK_B 0x02
#define HW_MASK_SELECT 0x04
#define HW_MASK_START 0x08
#define HW_MASK_UP 0x10
#define HW_MASK_DOWN 0x20
#define HW_MASK_LEFT 0x40
#define HW_MASK_RIGHT 0x80

#define INP_PAD_A 0x01
#define INP_PAD_B 0x02
#define INP_PAD_SELECT 0x04
#define INP_PAD_START 0x08
#define INP_PAD_UP 0x10
#define INP_PAD_DOWN 0x20
#define INP_PAD_LEFT 0x40
#define INP_PAD_RIGHT 0x80
 
esp_timer_handle_t nes_timer_handle = NULL;
void (*emu_timer_callback)(void) = NULL;

void IRAM_ATTR timer_callback_handler(void *arg) {
  if (emu_timer_callback) emu_timer_callback();
}
 
extern char *global_rom_data;
extern int global_rom_size;

extern "C" bool vid_preload_rom(const char *path);
extern "C" int osd_rom_open(const char *path);
extern "C" int osd_rom_read(void *dst, int len);
extern "C" void osd_rom_close(void);

/* ---------------------------------------------------------------------------
 * ROM browser.
 *
 * Compiled out for the ST7305, where the sketch supplies its own show_menu()
 * and get_selected_game() (nes_menu.cpp). Not a stylistic preference: this menu
 * signals selection with a red bar behind white text, and red and black have
 * near-identical luminance, so on a 1-bit panel the highlighted entry is
 * indistinguishable from the rest - the UI stops working, it does not merely
 * look different. Laying out for 400x300 instead of 280x240 is the smaller half
 * of the reason.
 * ------------------------------------------------------------------------- */
#if !(defined(NES_USE_ST7305) && NES_USE_ST7305)

#define MAX_GAMES 50
struct {
  char names[MAX_GAMES][32];
  int count;
  int selected;
} game_list = {{}, 0, 0};

int menu_volume = 100;
bool menu_brightness = true;
 
void scan_games() {
  File root = NES_FS.open("/");
  game_list.count = 0;
  
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    if (!entry.isDirectory() && game_list.count < MAX_GAMES) {
      String name = entry.name();
      if (name.endsWith(".nes") || name.endsWith(".NES")) {
        strncpy(game_list.names[game_list.count], entry.name(), 31);
        game_list.names[game_list.count][31] = '\0';
        game_list.count++;
      }
    }
    entry.close();
  }
  root.close();
}
 
void draw_menu() {
  tft.fillScreen(0x0000);  
  delay(100);
   
  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 50, 0xF800);  
  delay(50);
   
  Serial.println("[MENU] Drawing title...");
  tft.drawString(20, 10, "NES EMULATOR", 0xFFFF, 0xF800, 2);
   
  Serial.println("[MENU] Drawing game list...");
  int start_y = 70;
  for (int i = 0; i < game_list.count && i < 4; i++) {
    uint16_t color = (i == game_list.selected) ? 0xF800 : 0xFFFF;  
    char display_name[32];
    strncpy(display_name, game_list.names[i], 31);
    display_name[31] = '\0';
     
    char *dot = strchr(display_name, '.');
    if (dot) *dot = '\0';
    
    Serial.printf("  [%d] %s\n", i, display_name);
    tft.drawString(20, start_y + (i * 30), display_name, color, 0x0000, 1);
  }
  
  Serial.println("[MENU] Drawing controls...");
  tft.drawString(10, 220, "UP/DOWN:Select  A:Play  SELECT:Settings", 0xFFFF, 0x0000, 1);
  delay(100);
}

void draw_settings() {
  tft.fillScreen(0x0000);
  delay(50);
   
  tft.drawFilledRect(0, 0, DISPLAY_WIDTH, 50, 0x001F); 
  
  tft.drawString(20, 10, "SETTINGS", 0xFFFF, 0x001F, 2);
  
  tft.drawString(20, 80, "Volume: ", 0xFFFF, 0x0000, 1);
  char vol_str[10];
  snprintf(vol_str, 10, "%d%%", menu_volume);
  tft.drawString(100, 80, vol_str, 0x07E0, 0x0000, 1);
  
  tft.drawString(10, 220, "UP/DOWN:Volume  A:Save  B:Back", 0xFFFF, 0x0000, 1);
}

extern "C" int show_menu() {
  Serial.println("\n[MENU] Starting menu system...");

  show_magic();
  
  scan_games();
  Serial.printf("[MENU] Found %d games\n", game_list.count);
  
  if (game_list.count == 0) {
    Serial.println("[MENU] No games found!");
    tft.fillScreen(0xF800); 
    delay(3000);
    return -1;
  }
  
  game_list.selected = 0;
  int top_index = 0; 
  bool in_menu = true;
  unsigned long last_input = 0;
  int last_selected = -1;
  
  bool needs_full_redraw = true;
  bool needs_list_redraw = false;

  auto draw_menu_item = [&](int screen_idx, int game_idx, bool selected) {
    int y_pos = 50 + (screen_idx * 40);
    uint16_t bg = selected ? 0xF800 : 0x0000;
    uint16_t fg = 0xFFFF;
    
    char display_name[32];
    strncpy(display_name, game_list.names[game_idx], 31);
    display_name[31] = '\0';
    char *dot = strchr(display_name, '.');
    if (dot) *dot = '\0';

    tft.drawFilledRect(0, y_pos - 5, DISPLAY_WIDTH, 35, bg);
    tft.drawString(20, y_pos, display_name, fg, bg, 1);
  };
  
  while (in_menu) {
    if (needs_full_redraw) {
      tft.fillScreen(0x0000); 
      tft.drawString(10, 10, "GAME MENU", 0xFFFF, 0x0000, 2);
      
      for (int i = 0; i < 4 && (top_index + i) < game_list.count; i++) {
        draw_menu_item(i, top_index + i, (top_index + i) == game_list.selected);
      }
      
      tft.drawString(10, DISPLAY_HEIGHT - 40, "UP/DN: Select", 0x07E0, 0x0000, 1);
      tft.drawString(10, DISPLAY_HEIGHT - 20, "A: Play  SEL: Settings", 0x07E0, 0x0000, 1);
      
      last_selected = game_list.selected;
      needs_full_redraw = false;
      needs_list_redraw = false;
    } 
    else if (needs_list_redraw) {
      tft.drawFilledRect(0, 45, DISPLAY_WIDTH, 155, 0x0000); 
      
      for (int i = 0; i < 4 && (top_index + i) < game_list.count; i++) {
        draw_menu_item(i, top_index + i, (top_index + i) == game_list.selected);
      }
      
      last_selected = game_list.selected;
      needs_list_redraw = false;
    } 
    else if (last_selected != game_list.selected) {
      if (last_selected >= top_index && last_selected < top_index + 4) {
        draw_menu_item(last_selected - top_index, last_selected, false);
      }
      draw_menu_item(game_list.selected - top_index, game_list.selected, true);
      last_selected = game_list.selected;
    }
    
    int hw = nes_get_gamepad_state();
    
    if ((millis() - last_input) > 150) {
      if (hw & HW_MASK_UP) {
        if (game_list.selected > 0) {
          game_list.selected--;
          if (game_list.selected < top_index) {
            top_index = game_list.selected;
            needs_list_redraw = true; 
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_DOWN) {
        if (game_list.selected < game_list.count - 1) {
          game_list.selected++;
          if (game_list.selected >= top_index + 4) {
            top_index = game_list.selected - 3;
            needs_list_redraw = true;
          }
          last_input = millis();
        }
      }
      if (hw & HW_MASK_A) {
        tft.fillScreen(0x0000);  
        delay(300);
        return game_list.selected;
      }
      if (hw & HW_MASK_SELECT) {
        bool in_settings = true;
        unsigned long settings_input = 0;
        int last_menu_volume = -1;
        bool settings_full_redraw = true;
        
        while (in_settings) {
          if (settings_full_redraw) {
            tft.fillScreen(0x0000);
            tft.drawString(10, 10, "VOLUME SETTINGS", 0xFFFF, 0x0000, 2);
            tft.drawString(10, DISPLAY_HEIGHT - 40, "UP/DN: Adjust", 0x07E0, 0x0000, 1);
            tft.drawString(10, DISPLAY_HEIGHT - 20, "A: Save  B: Cancel", 0x07E0, 0x0000, 1);
            settings_full_redraw = false;
            last_menu_volume = -1;
          }

          if (last_menu_volume != menu_volume) {
            char vol_str[20];
            sprintf(vol_str, "Vol: %d%%", menu_volume);
            tft.drawFilledRect(20, 60, 200, 20, 0x0000);
            tft.drawString(20, 60, vol_str, 0x07E0, 0x0000, 2);

            int bar_width = (menu_volume * (DISPLAY_WIDTH - 40)) / 200;
            tft.drawFilledRect(20, 120, DISPLAY_WIDTH - 40, 30, 0x4208); 
            tft.drawFilledRect(20, 120, bar_width, 30, 0xF800);

            last_menu_volume = menu_volume;
          }
          
          int hw2 = nes_get_gamepad_state();
          
          if ((millis() - settings_input) > 150) {
            if (hw2 & HW_MASK_UP) {
              menu_volume = min(200, menu_volume + 10);
              master_volume = menu_volume;
              settings_input = millis();
            }
            if (hw2 & HW_MASK_DOWN) {
              menu_volume = max(0, menu_volume - 10);
              master_volume = menu_volume;
              settings_input = millis();
            }
            if (hw2 & HW_MASK_A || hw2 & HW_MASK_B) {
              in_settings = false;
            }
          }
          delay(50);
        }
        last_input = millis();
        needs_full_redraw = true; 
      }
    }
    delay(20);
  }
  return -1;
}

extern "C" const char *get_selected_game() {
  static char game_path[64];
  if (game_list.selected < game_list.count) {
    snprintf(game_path, 64, "/%s", game_list.names[game_list.selected]);
    return game_path;
  }
  return "/mario.nes";  // Default fallback
}

#else  /* NES_USE_ST7305: the sketch supplies the browser */

extern "C" int show_menu();
extern "C" const char *get_selected_game();

#endif /* !NES_USE_ST7305 - ROM browser */

/* ---------------------------------------------------------------------------
 * ROM source.
 *
 * All of it is compiled out for NES_EMBEDDED_ROM, where the sketch's
 * nes_rom_embedded.cpp serves the image from a flash array instead. That has to
 * take over osd_release_romdata() too, not just the readers: this version calls
 * free() on the pointer, and free() on a memory-mapped flash address faults.
 * ------------------------------------------------------------------------- */
#if !(defined(NES_EMBEDDED_ROM) && NES_EMBEDDED_ROM)

extern "C" char *osd_getromdata(void) {
  const char *rom_path = get_selected_game();
  
  Serial.printf("[ROM] osd_getromdata loading: %s\n", rom_path);
  Serial.flush();
  
  if (global_rom_data == NULL) {
    Serial.println("[ROM] global_rom_data is NULL - loading from SD...");
    Serial.flush();
    
    bool loaded = false;
    try {
      loaded = vid_preload_rom(rom_path);
    } catch (...) {
      Serial.println("[ROM] EXCEPTION during ROM preload!");
      loaded = false;
    }
    
    if (!loaded) {
      Serial.printf("[ROM] CRITICAL: Failed to load ROM file: %s\n", rom_path);
      return NULL;
    }
  }
  
  if (global_rom_data == NULL) {
    Serial.println("[ROM] FATAL: Still no ROM data after load attempt!");
    return NULL;
  }

  Serial.printf("[ROM] ROM ready: %d bytes\n", global_rom_size);
  return global_rom_data;
}

extern "C" void osd_release_romdata(void) {
  if (global_rom_data) {
    free(global_rom_data);
    global_rom_data = NULL;
    global_rom_size = 0;
  }
}

#endif /* !NES_EMBEDDED_ROM - whole-image API */

extern "C" int osd_init_sound(void) {
  if (!runtime_sound_enabled) {
    Serial.println("Audio disabled by runtime config (-sound not set)");
    return 0;
  }
#if ENABLE_SOUND
  /* The driver survives a game exit, and installing it twice returns an error
   * that would leave the rest of this function unrun - so the second game would
   * come up silent. Nothing here needs redoing per game. */
  static bool i2s_installed = false;
  if (i2s_installed) {
    Serial.println("I2S audio already up");
    return 0;
  }

  Serial.println("Initializing I2S audio...");
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S_MSB,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512,
    .use_apll = true,
    .tx_desc_auto_clear = true,
#ifdef I2S_MCLK
    /* A codec clocked from MCLK needs a defined ratio, not just "some clock".
     * 256x is what the ES8311 clock coefficients in nes_codec.cpp are
     * transcribed for; changing it here without changing that table gives a
     * codec that runs at the wrong rate rather than one that fails. */
    .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#endif
  };
  i2s_pin_config_t pin_config = {
#ifdef I2S_MCLK
    .mck_io_num = I2S_MCLK,
#endif
    .bck_io_num = I2S_BCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_DO,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  esp_err_t err;
  if ((err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL)) != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", err);
    return -1;
  }
  if ((err = i2s_set_pin(I2S_NUM_0, &pin_config)) != ESP_OK) {
    Serial.printf("I2S set pin failed: %d\n", err);
    return -1;
  }
  i2s_set_clk(I2S_NUM_0, AUDIO_SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
  i2s_installed = true;
  Serial.println("I2S audio initialized successfully");

#if defined(NES_USE_ES8311) && NES_USE_ES8311
  /* Strictly after i2s_driver_install(): the codec is configured while its
   * MCLK is already running, which is the order the vendor's own bring-up
   * uses. Configuring it against a dead clock pin is one of the ways this ends
   * in silence that reports success. */
  if (!nes_codec_begin(AUDIO_SAMPLE_RATE)) {
    Serial.println("[OSD] ES8311 bring-up failed - continuing without sound");
    runtime_sound_enabled = false;
    return -1;
  }
#endif

  return 0;
#else
  Serial.println("Sound disabled in configuration");
  return 0;
#endif
}

extern "C" void osd_getsoundinfo(sndinfo_t *info) {
  info->sample_rate = AUDIO_SAMPLE_RATE;
  info->bps = 16;
}

extern "C" void osd_setsound(void (*playfunc)(void *buffer, int length)) {
  emulator_audio_callback = runtime_sound_enabled ? playfunc : NULL;
}

extern "C" void osd_stopsound(void) {
  if (!runtime_sound_enabled) return;
#if ENABLE_SOUND
  i2s_zero_dma_buffer(I2S_NUM_0);
#endif
}

extern "C" void osd_writesound(void *stream, int len) {}

extern "C" void osd_initvideo(int *lines) {
  *lines = NES_SCREEN_HEIGHT;
  Serial.printf("osd_initvideo called, lines=%d\n", *lines);
}

extern "C" void osd_shutdownvideo() {}

extern "C" void osd_setscreen(int x, int y, int width, int height) {}

extern "C" void osd_setpalette(rgb_t *pal) {
  if (!pal) return;
  for (int i = 0; i < 256; i++) {
    uint16_t c = tft.color565(pal[i].r, pal[i].g, pal[i].b);
    myPalette565[i] = c;
    myPalette565_swapped[i] = __builtin_bswap16(c);
  }
#if defined(NES_USE_ST7305) && NES_USE_ST7305
  /* Feed the mono panel the palette at full 8-bit precision rather than via
   * myPalette565 - luminance is what survives the 1-bit reduction, and the
   * 565 round trip throws away up to 3 bits per channel before we measure it.
   * rebuildDither() bakes the result into the table blitIndexed() reads, so it
   * has to run after the whole palette is in, not per entry. */
  for (int i = 0; i < 256; i++) {
    tft.setPaletteEntry(i, pal[i].r, pal[i].g, pal[i].b);
  }
  tft.rebuildDither();
#endif
}

extern "C" int nes_get_gamepad_state();

extern "C" void osd_getinput(void) {
  int hw = nes_get_gamepad_state();
  int nes_data = 0;

  if (hw & HW_MASK_A) nes_data |= INP_PAD_A;
  if (hw & HW_MASK_B) nes_data |= INP_PAD_B;
  if (hw & HW_MASK_SELECT) nes_data |= INP_PAD_SELECT;
  if (hw & HW_MASK_START) nes_data |= INP_PAD_START;
  if (hw & HW_MASK_UP) nes_data |= INP_PAD_UP;
  if (hw & HW_MASK_DOWN) nes_data |= INP_PAD_DOWN;
  if (hw & HW_MASK_LEFT) nes_data |= INP_PAD_LEFT;
  if (hw & HW_MASK_RIGHT) nes_data |= INP_PAD_RIGHT;
  joypad_p1.data = nes_data;
}

extern "C" int osd_init() {
  Serial.println("[OSD] Initializing...");
  Serial.printf("[OSD] Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  
  /* ONCE PER BOOT, not once per game. osd_init() runs again for every ROM
   * loaded, and input_register() is an unchecked `nes_input[active_entries++]`
   * against a 32-slot array with nothing that ever resets the count - so the
   * 33rd game started in one session would write past the end of it. Nothing
   * about the registration needs redoing anyway; joypad_p1 is a static this
   * file owns and the emulator only ever reads through the pointer. */
  static bool joypad_registered = false;
  if (!joypad_registered) {
    Serial.println("[OSD] Initializing joypad...");
    joypad_p1.type = INP_JOYPAD0;
    joypad_p1.data = 0;

    input_register(&joypad_p1);
    joypad_registered = true;
    Serial.println("[OSD] Input registered successfully");
  }
  
  if (osd_init_sound() != 0) { 
    Serial.println("[OSD] Sound Init FAILED"); 
  }
  
  Serial.printf("[OSD] Post-init Heap=%u, SPIRAM=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.println("[OSD] Initialization complete");
  return 0;
}

extern "C" void osd_shutdown() {
  if (nes_timer_handle) {
    esp_timer_stop(nes_timer_handle);
    esp_timer_delete(nes_timer_handle);
    /* Cleared, or the next osd_installtimer() sees a stale handle and the one
     * after that leaks it. */
    nes_timer_handle = NULL;
  }
}

extern "C" int osd_installtimer(int freq, void *func, int func_param, void *func2, int func2_param) {
  /* Called once per game, so an existing timer is the previous game's and must
   * go before another is created. */
  if (nes_timer_handle) {
    esp_timer_stop(nes_timer_handle);
    esp_timer_delete(nes_timer_handle);
    nes_timer_handle = NULL;
  }
  emu_timer_callback = (void (*)(void))func;
  const esp_timer_create_args_t timer_args = { .callback = &timer_callback_handler, .name = "nes_timer" };
  esp_timer_create(&timer_args, &nes_timer_handle);
  esp_timer_start_periodic(nes_timer_handle, 1000000 / freq);
  return 0;
}

extern "C" int osd_gettime(void) {
  return millis();
}

extern "C" int osd_makesnapname(char *buf, int len) {
  return 0;
}

extern "C" void osd_getmouse(int *x, int *y, int *button) {
  *x = 0;
  *y = 0;
  *button = 0;
}

#if !(defined(NES_EMBEDDED_ROM) && NES_EMBEDDED_ROM)

char *global_rom_data = NULL;
int global_rom_size = 0;
static File rom_stream_file;

extern "C" bool vid_preload_rom(const char *path) {
  if (global_rom_data) {
    free(global_rom_data);
    global_rom_data = NULL;
  }
  
  Serial.printf("[ROM] Opening file: %s\n", path);
  Serial.flush();
  delay(100);
  
  File file;
  try {
    file = NES_FS.open(path, FILE_READ);
  } catch (...) {
    Serial.println("[ROM] EXCEPTION during card open()!");
    return false;
  }
  
  if (!file) {
    Serial.println("[ROM] Failed to open file");
    return false;
  }
  
  global_rom_size = file.size();
  Serial.printf("[ROM] File size: %d bytes\n", global_rom_size);
  
  Serial.println("[ROM] Allocating memory...");
  global_rom_data = (char *)heap_caps_malloc(global_rom_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!global_rom_data) {
    Serial.println("[ROM] SPIRAM allocation failed, trying regular heap...");
    global_rom_data = (char *)malloc(global_rom_size);
  }
  
  if (!global_rom_data) {
    Serial.println("[ROM] Memory allocation failed!");
    file.close();
    return false;
  }
  
  Serial.println("[ROM] Reading file into memory...");
  size_t bytes_read = file.readBytes(global_rom_data, global_rom_size);
  file.close();
  
  Serial.printf("[ROM] Read %d bytes\n", bytes_read);
  if (bytes_read != global_rom_size) {
    Serial.printf("[ROM] Error: Expected %d bytes, got %d\n", global_rom_size, bytes_read);
    free(global_rom_data);
    global_rom_data = NULL;
    return false;
  }
  
  Serial.println("[ROM] ROM loaded successfully!");
  return true;
}

#endif /* !NES_EMBEDDED_ROM - card loader */

extern "C" void IRAM_ATTR osd_blit(bitmap_t *bmp) {
  if (!video_ready || !bmp || !bmp->line || !bmp->line[0] || !frame_buffer) {
    return;
  }
  
  for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
    uint8_t *src = bmp->line[y];
    uint16_t *dst = &frame_buffer[y * NES_SCREEN_WIDTH];
    
    for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
      dst[x] = myPalette565_swapped[src[x]];
    }
  }
}

/* Pulls one frame's worth of samples from the APU and hands them to I2S.
 * Split out of vid_flush so both display back ends run exactly the same audio
 * code - it must keep being called even on frames the panel never shows, or the
 * APU's buffer stops draining and the sound stutters. */
static void nes_audio_service() {
  if (!runtime_sound_enabled || !emulator_audio_callback) return;

  /* 16000 / 60 is 266.67, and truncating it to 266 starves I2S by 40 samples a
   * second. That is not a rounding detail: the DMA runs dry every few seconds
   * and repeats its last block, which is audible as a periodic tick. Carrying
   * the remainder makes the sequence 267,267,266,267,267,266... and the long
   * run average exactly right. */
  static int sample_remainder = 0;
  sample_remainder += AUDIO_SAMPLE_RATE;
  int samples_needed = sample_remainder / NES_AUDIO_FRAME_RATE;
  sample_remainder -= samples_needed * NES_AUDIO_FRAME_RATE;

  if (samples_needed < 1) samples_needed = 1;
  /* mono_buffer is 512 entries and stereo_buffer 1024, i.e. 512 frames. Clamp
   * rather than trust the arithmetic - a sample rate raised in hw_config.h
   * without resizing these would otherwise overrun both. */
  if (samples_needed > (int)(sizeof mono_buffer / sizeof mono_buffer[0]))
    samples_needed = (int)(sizeof mono_buffer / sizeof mono_buffer[0]);

  memset(mono_buffer, 0, samples_needed * 2);
  emulator_audio_callback(mono_buffer, samples_needed);

#if ENABLE_SOUND
  int sample_count = samples_needed;
  for (int i = 0; i < sample_count; i++) {
    int32_t sample = mono_buffer[i];

    sample = (sample * master_volume) / 100;
    sample = (sample * AUDIO_GAIN) / 100;

    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;

    stereo_buffer[i * 2] = (int16_t)sample;
    stereo_buffer[i * 2 + 1] = (int16_t)sample;
  }

  size_t written;
  i2s_write(I2S_NUM_0, (const char *)stereo_buffer, sample_count * 4, &written, 0);
#endif
}

#if defined(NES_USE_ST7305) && NES_USE_ST7305

#if defined(ARDUINO_ARCH_ESP32)
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

/* One symbol for "is there a dither task", so the four blocks that make up the
 * asynchronous path switch together and cannot half-exist. NES_DITHER_ON_CORE0
 * is the knob; ARDUINO_ARCH_ESP32 is the floor, because a host build has no
 * FreeRTOS to put a task on. */
#if defined(ARDUINO_ARCH_ESP32) && \
    defined(NES_DITHER_ON_CORE0) && NES_DITHER_ON_CORE0
#define NES_ASYNC_DITHER 1
#else
#define NES_ASYNC_DITHER 0
#endif

/* Frames the emulator produced but the panel never showed. Worth knowing: the
 * panel scans several times slower than the emulator runs, so most frames are
 * expected to be dropped and this number being large is correct, not a fault.
 * The exact ratio follows NES_PANEL_FPS and is in the log line below, not
 * here. */
unsigned long nes_frames_dropped = 0;

/* --- the dither task --------------------------------------------------------
 *
 * WHY DITHERING IS NOT ON THIS THREAD. Reducing 400x300 to one bit is a fixed
 * cost per SHOWN frame, and it used to be paid by the emulator, which meant the
 * emulator's frame rate was hostage to which reduction was selected. Measured on
 * hardware: an ordered mode costs ~2.9 ms, which against an 18 Hz panel is 5% of
 * the core and left the emulator at a clean 61 fps; Atkinson costs ~37 ms, which
 * is 66% of the core, and dropped it to 26. Same emulator, same panel, and all
 * of the difference was accounting.
 *
 * So it moved to core 0, which this board barely uses: only the BLE keyboard
 * task lives there, and at priority 2 against this task's 1, so a keypress still
 * preempts a frame. Core 1 keeps the emulator and the TE flush task. The dither
 * task is the sole caller of beginFrame()/present(), which is what ST7305Tft
 * requires - a single producer - so the double buffering and the TE pacing
 * underneath it are untouched.
 *
 * WHY THREE BITMAPS AND NOT TWO.
 *
 * Two is the obvious answer and it is what this had first: the PPU writes its
 * 8-bit frame all through the emulated frame, so a dither pass reading that same
 * buffer from another core would mix two emulated frames and lay a moving seam
 * across the picture - the exact artefact TE pacing exists to remove,
 * reintroduced one layer up. One buffer to render into, one to read from.
 *
 * It measured `pushed=9/s` against a panel scanning at 18. Exactly half, and the
 * arithmetic says why. With two buffers the handover can only happen inside
 * vid_flush, i.e. at an emulated frame boundary, once every 16.7 ms. The task
 * finishes a 45 ms frame, releases its buffer, and then has nothing to do until
 * the emulator next reaches vid_flush - up to 16.7 ms of dead time, because the
 * buffer the emulator is rendering into is half-written and cannot be taken.
 * 45 + 16.7 is over the panel's 55.5 ms period, so every other TE edge went by
 * with no new frame ready.
 *
 * The third buffer removes the wait entirely. The emulator publishes each
 * finished frame into bmp_latest and immediately takes a free one; the task
 * takes whatever is published the instant it is free, without waiting to be
 * told. Neither ever blocks on the other, and a frame published while the task
 * is busy simply supersedes the one before it - which is the right thing, since
 * the panel can only show the newest one anyway. 61 KB more of PSRAM, of which
 * this board has 8 MB spare.
 *
 * THE HANDOVER NEEDS A LOCK, unlike the two-buffer version it replaces. Picking
 * a free buffer means reading bmp_latest and bmp_held together and deciding
 * against both, and the task can change either between the two reads. The
 * critical sections are a handful of instructions each and run twice per
 * emulated frame; a lock this small is far cheaper than being clever and wrong.
 *
 * Falls back to dithering inline if the task or the extra bitmaps cannot be had,
 * which is the behaviour this file had before all of this and is still correct -
 * just slower. */
#if NES_ASYNC_DITHER
static bitmap_t          *bmp_pool[3] = { NULL, NULL, NULL };
/* Newest complete frame, free for the task to take. NULL = nothing new. */
static bitmap_t *volatile bmp_latest  = NULL;
/* The one the task is reading. The producer must not hand this one out. */
static bitmap_t *volatile bmp_held    = NULL;
static portMUX_TYPE       video_mux   = portMUX_INITIALIZER_UNLOCKED;
#endif
/* Not guarded: the log line reads it to say which thread did the work. */
static void              *dither_task   = NULL;
/* Written by the task, read by the log line on the emulator thread. */
static volatile unsigned long nes_dither_us = 0;

/* Dithers one finished frame into the panel's back buffer and queues it. The
 * ONLY function that may touch the panel once TE pacing is running, and it runs
 * on the dither task whenever there is one. */
static void nes_present_frame(bitmap_t *bmp) {
  /* beginFrame() says no when one frame is already queued and another is on the
   * wire - a frame there is nowhere to draw into is a frame the panel was never
   * going to show, so it costs nothing to skip. */
  if (!tft.beginFrame()) {
    nes_frames_dropped++;
    return;
  }

  unsigned long t1 = micros();
  /* 256x224 of non-square NES pixels stretched over the whole 400x300
   * panel. The panel is 4:3 and so was the television this picture was
   * drawn for, so filling it is the geometrically correct thing to do -
   * centring it 1:1 was not, it left the image about 22% too tall. */
  tft.blitIndexedStretched(bmp->line, NES_SCREEN_WIDTH,
                           NES_VISIBLE_Y0, NES_VISIBLE_HEIGHT);
  nes_dither_us = micros() - t1;

#if defined(NES_SHOW_FPS) && NES_SHOW_FPS
    /* Bottom-left corner, over the picture. There is no margin to park this
     * in any more and nothing to cache: the blit above rewrites all 15000
     * GRAM bytes, so whatever was here is gone every single frame.
     *
     * Three fields, and each answers a question the other two cannot.
     *
     *   emulated fps   is the emulator keeping up? Not the pushed rate - that
     *                  is pinned at the panel's by construction and would read
     *                  the same whether the emulator ran at 60 or at 25.
     *   panel Hz       is TE pacing still alive? Counted edges, so a 0 here is
     *                  the fallback timer and the tearing that comes with it.
     *                  Nothing else on the device reports that.
     *   mode           which of the eight reductions SELECT + A has landed on,
     *                  read BACK from the driver rather than tracked here, so
     *                  a mode that was refused for want of memory shows as the
     *                  one actually on the glass.
     *
     * Ten characters at scale 1 is 60 px of a 400 px panel and 11 px of 300,
     * i.e. half a percent of the picture. */
    {
      char hud[20];
      snprintf(hud, sizeof hud, "%d %d %s",
               current_fps > 999 ? 999 : current_fps,
               current_panel_hz > 99 ? 99 : current_panel_hz,
               tft.monoModeName());
      /* -1: textWidth() counts the blank spacing column after the last glyph,
       * which would leave the box a pixel wider on the right than the left. */
      const int tw = TFTDriver::textWidth(hud, 1) - 1;
      const int th = TFTDriver::textHeight(1);
      const int bx = 3;
      const int by = DISPLAY_HEIGHT - 3 - (th + 4);
      tft.drawFilledRect(bx, by, tw + 4, th + 4, 0xFFFF);
      tft.drawString(bx + 2, by + 2, hud, 0x0000, 0xFFFF, 1);
    }
#endif

  /* Returns at once. The 6.5 ms of SPI happens on the TE task, phase-locked to
   * the scan; this one goes back to waiting for a frame. */
  tft.present();
}

#if NES_ASYNC_DITHER
static void nes_dither_task_entry(void *arg) {
  (void)arg;
  for (;;) {
    /* CLAIM SOMEWHERE TO DRAW BEFORE TAKING SOMETHING TO DRAW.
     *
     * beginFrame() refuses while the frame presented last is still queued for
     * the panel, which is most of the gap between finishing one frame and the
     * next TE edge. Taking a frame first and discarding it on a refusal - which
     * is what this did - throws away the frame AND the moment: by the time the
     * edge frees the slot this task is asleep again, waiting to be handed the
     * emulator's next frame up to 16.7 ms later. Added to Atkinson's 45 ms that
     * put the cycle past the panel's 55.5 ms period and halved the push rate to
     * 9/s, while Floyd at 38 ms still made 18.
     *
     * beginFrame() is idempotent, so claiming early costs nothing and the claim
     * survives to the next pass round this loop. */
    if (!tft.beginFrame()) {
      vTaskDelay(1);                        /* ~1 ms, then ask again */
      continue;
    }

    /* Take whatever the emulator has published. Doing this BEFORE blocking is
     * the whole point of the third buffer: a frame finished while this task was
     * busy is already waiting, and going back to sleep to be told about it is
     * exactly the dead time that halved the push rate. */
    portENTER_CRITICAL(&video_mux);
    bitmap_t *const take = bmp_latest;
    bmp_latest = NULL;
    bmp_held   = take;
    portEXIT_CRITICAL(&video_mux);

    if (!take) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    nes_present_frame(take);

    portENTER_CRITICAL(&video_mux);
    bmp_held = NULL;
    portEXIT_CRITICAL(&video_mux);
  }
}
#endif

extern "C" void vid_flush() {
  frame_start_time = micros();
  frame_count++;

  /* line[] is a flexible array member, so only line[0] is worth testing - the
   * array itself can never be null. */
  const bool have_frame = video_ready && game_bitmap && game_bitmap->line[0];

#if NES_ASYNC_DITHER
  if (have_frame && dither_task && bmp_pool[2]) {
    /* Publish what was just rendered and take a free buffer for the next one.
     * Never waits: if the task is busy, the frame it had not taken yet is simply
     * superseded and becomes the new render target. */
    portENTER_CRITICAL(&video_mux);
    bitmap_t *const done  = game_bitmap;
    bitmap_t *const stale = bmp_latest;   /* published but never taken */
    /* A superseded frame is free again, so it is the obvious next target.
     * Otherwise exactly one of the three is neither being read nor about to be
     * published, and the loop finds it. */
    bitmap_t *next = stale;
    if (!next) {
      for (int i = 0; i < 3; i++) {
        if (bmp_pool[i] != done && bmp_pool[i] != bmp_held) {
          next = bmp_pool[i];
          break;
        }
      }
    }
    /* NOTHING IS PUBLISHED UNLESS THERE IS SOMEWHERE TO RENDER NEXT. The search
     * cannot fail - latest and held are never the same buffer, so at most two of
     * three are ever spoken for - but writing it as one decision rather than two
     * means a wrong proof costs a dropped frame instead of a null game_bitmap
     * and a PPU writing through it. */
    if (next) {
      bmp_latest  = done;
      game_bitmap = next;
    }
    portEXIT_CRITICAL(&video_mux);

    /* At 60 emulated frames against ~18 shown, most end their life here, and
     * always did - what is new is that the emulator does not stop to find out.
     * Incremented from this core and from core 0 when beginFrame() refuses;
     * it is a diagnostic, and a lost count is cheaper than a lock for it. */
    if (stale || !next) nes_frames_dropped++;

    if (next) xTaskNotifyGive((TaskHandle_t)dither_task);
  } else if (have_frame) {
    nes_present_frame(game_bitmap);       /* no task - dither inline, as before */
  } else {
    nes_frames_dropped++;
  }
#else
  if (have_frame) nes_present_frame(game_bitmap);
  else            nes_frames_dropped++;
#endif

  {
    static unsigned long last_print = 0;
    if (millis() - last_print > 3000) {
      /* te_hz is the diagnostic that matters: if it is 0 the pacing has fallen
       * back to a timer and the tearing is back, and no amount of staring at
       * the other numbers would say so. pushed is the true picture rate and
       * should sit at the panel's scan rate, not at emul_fps.
       *
       * dither= IS NO LONGER TIME TAKEN FROM THE EMULATOR. It is measured on
       * whichever thread did the work, and once the dither task exists that is
       * core 0 - so a 37 ms Atkinson frame and a 3 ms Bayer one should now show
       * the same emul_fps. If they do not, the task did not start; `on=` says
       * which it is.
       *
       * tone/loop/asm are the three phases of a diffusion frame and are zero in
       * an ordered mode. They are printed because a host benchmark mispredicted
       * which of them was expensive on this chip, and the only cure for that is
       * to measure on the chip. Their sum is a little under dither=; the
       * shortfall is the row rotation. */
      static uint32_t last_edges = 0, last_pushes = 0;
      const uint32_t edges = tft.teEdges(), pushed = tft.pushes();

      /* Per second, from two counter samples three seconds apart - except on
       * the first line after a game (re)starts, where the driver's counters
       * have gone back to zero and the baseline still holds the last game's
       * total. That subtraction underflows and printed te_hz=1431655746, which
       * is 0x55555552 and not a frame rate. Nothing downstream was wrong; the
       * number was. A counter that went backwards means "no baseline yet", and
       * the honest answer for that interval is zero. */
      auto rate = [](uint32_t now, uint32_t before) -> unsigned long {
        return now >= before ? (unsigned long)((now - before) / 3) : 0UL;
      };

      /* pc= IS THE ONE THAT SAYS WHETHER THE GAME IS ALIVE. Every other field
       * here describes the emulator, and the emulator carries on at a clean 60
       * fps whether the emulated CPU is running the game or spinning on one
       * instruction - a frozen picture with emul_fps=60 looks identical either
       * way, which is how this gets mistaken for a display or a card fault.
       *
       * Sampled once every three seconds, so it is a lottery ticket rather than
       * a trace: a healthy game lands somewhere different every time. The same
       * address twice running means the 6502 is not going anywhere, and jam=
       * then says which kind of stuck it is - an illegal opcode it can never
       * leave, or a wait loop for something that is never coming. */
      nes6502_context cpu_snapshot;
      nes6502_getcontext(&cpu_snapshot);

      /* spr0 AND $2002 TOGETHER, because neither means anything alone.
       *
       * A game that splits the screen on a sprite 0 hit polls $2002 in a tight
       * loop and expects the strike bit within one frame. If the hit never
       * comes the poll never ends - and from outside that is a perfectly
       * healthy emulator running a game that has stopped, which is the state
       * this log line exists to name.
       *
       * spr0=0 with p2002 in the tens of thousands per second IS that failure:
       * the game is asking, the PPU is never answering. spr0 near the frame
       * rate with p2002 in the hundreds is a normal raster split. Both near
       * zero means the game is not waiting on the PPU at all and the still
       * picture has some other cause - look at ppu= next, since rendering
       * switched off explains a static screen all by itself. */
      char diag[176] = "";
#if NES_PPU_DIAGNOSTICS
      static unsigned long last_spr0 = 0, last_stat = 0;
      static unsigned long last_scan = 0, last_opaq = 0;
      static unsigned long last_chr  = 0;
      const unsigned long spr0 = nes_spr0_hits, stat = nes_ppustat_reads;
      const unsigned long scan = nes_spr0_scanned, opaq = nes_spr0_opaque;
      const int render = ppu_rendering_state();

      /* WHY SPRITE 0 DID NOT STRIKE, narrowed to one of three answers.
       *
       * scan= counts the scanlines sprite 0 was eligible on, opaque= how many
       * of those had a non-transparent sprite pixel, spr0= how many produced a
       * hit. They only ever fall, and where they fall says which layer failed:
       *
       *   scan=0                 sprite 0 is not on screen at all - geometry,
       *                          or OAM never got written
       *   scan>0 opaque=0        it is on screen but its pattern is blank -
       *                          CHR RAM never received the tile
       *   opaque>0 spr0=0        it is on screen and solid, but the background
       *                          under it is transparent - or is being drawn
       *                          out of step with it
       *
       * s0= is the raw OAM entry behind those numbers, because "y=240" answers
       * the first case on its own. */
      int s0y, s0x, s0tile, s0attr;
      ppu_sprite0(&s0y, &s0x, &s0tile, &s0attr);

      /* Built separately and appended, rather than kept as a second copy of the
       * whole printf under an #else. Two format strings that have to be edited
       * in step is how one of them quietly stops matching its arguments. */
      snprintf(diag, sizeof diag,
               " spr0=%lu/s p2002=%lu/s ppu=%s scan=%lu/s opaque=%lu/s "
               "s0=y%d,x%d,t$%02X,a$%02X col=$%02X bg=$%02X vaddr=$%04X "
               "xofs=%lu tail=$%06X pat=$%04X@$%04X chr=%lu/s top=$%04X nt=%d",
               (spr0 - last_spr0) / 3, (stat - last_stat) / 3,
               render == 3 ? "bg+obj" : render == 1 ? "bg"
                           : render == 2 ? "obj" : "OFF",
               (scan - last_scan) / 3, (opaq - last_opaq) / 3,
               s0y, s0x, s0tile, s0attr,
               (unsigned)nes_spr0_colmask, (unsigned)nes_spr0_bgmask,
               (unsigned)nes_bg_vaddr, nes_bg_xofs,
               (unsigned)nes_bg_tail,
               (unsigned)nes_bg_pat, (unsigned)nes_bg_pataddr,
               (nes_chr_writes - last_chr) / 3, (unsigned)nes_chr_max,
               nes_bg_nt);
#endif /* NES_PPU_DIAGNOSTICS */

      Serial.printf("dither=%luus(%s on=%s tone=%lu loop=%lu asm=%lu) "
                    "emul_fps=%d dropped=%lu te_hz=%lu pushed=%lu/s "
                    "pc=$%04X%s%s\n",
                    nes_dither_us, tft.monoModeName(),
                    dither_task ? "core0" : "emu",
                    (unsigned long)tft.diffToneUs(),
                    (unsigned long)tft.diffLoopUs(),
                    (unsigned long)tft.diffAsmUs(),
                    current_fps, nes_frames_dropped,
                    rate(edges, last_edges), rate(pushed, last_pushes),
                    (unsigned)cpu_snapshot.pc_reg,
                    cpu_snapshot.jammed ? " JAMMED" : "",
                    diag);

#if NES_PPU_DIAGNOSTICS
      /* Cleared so each line describes its own three seconds. Written from this
       * thread and OR'd from the PPU, which runs on this thread too. */
      nes_spr0_colmask = 0;
      nes_spr0_bgmask  = 0;
      last_chr    = nes_chr_writes;
      last_spr0   = spr0;
      last_stat   = stat;
      last_scan   = scan;
      last_opaq   = opaq;
#endif
      last_edges  = edges;
      last_pushes = pushed;
      last_print  = millis();
    }
  }

  /* Outside every branch above, and it has to stay that way: it must keep being
   * called even on frames the panel never shows, or the APU's buffer stops
   * draining and the sound stutters. */
  nes_audio_service();

  if (millis() - last_fps_update >= 1000) {
    current_fps = frame_count;
    frame_count = 0;

    /* Edges since the last time round, which is one second give or take a
     * frame - close enough for a two-digit readout, and it costs one
     * subtraction against maintaining a second timebase. Counted here rather
     * than inside the drawing block above so it keeps updating on frames the
     * panel drops, which is most of them. */
    static uint32_t last_hud_edges = 0;
    const uint32_t edges = tft.teEdges();
    current_panel_hz = (int)(edges - last_hud_edges);
    last_hud_edges = edges;

    last_fps_update = millis();
  }
}

#else /* original ST7789 path */

extern "C" void vid_flush() {
  static unsigned long last_display_update = 0;
  static bool tried_frame_buffer_alloc = false;
  static uint16_t line_buffer[NES_SCREEN_WIDTH];

  frame_start_time = micros();

  if (!tried_frame_buffer_alloc && !frame_buffer) {
    tried_frame_buffer_alloc = true;
    frame_buffer = (uint16_t *)heap_caps_malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buffer) {
      frame_buffer = (uint16_t *)malloc(NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT * sizeof(uint16_t));
    }
    if (!frame_buffer) {
      low_mem_video_mode = true;
      Serial.println("Low-memory video mode enabled (line-by-line rendering)");
    }
  }

  if (frame_buffer) {
    osd_blit(game_bitmap);
  }

  unsigned long now = micros();
  if (now - last_display_update > 16666) {
    int x_offset = (DISPLAY_WIDTH - NES_SCREEN_WIDTH) / 2;
    unsigned long t1 = micros();
    
    if (frame_buffer) {
      tft.pushImage(x_offset, 0, NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, frame_buffer, true);
    } else if (low_mem_video_mode && game_bitmap && game_bitmap->line && game_bitmap->line[0]) {
      static bool lowmem_logged = false;
      if (!lowmem_logged) {
        Serial.println("Display mode: low-memory line render (FPS will be lower)");
        lowmem_logged = true;
      }
      for (int y = 0; y < NES_SCREEN_HEIGHT; y++) {
        uint8_t *src = game_bitmap->line[y];
        for (int x = 0; x < NES_SCREEN_WIDTH; x++) {
          line_buffer[x] = myPalette565_swapped[src[x]];
        }
        tft.pushImage(x_offset, y, NES_SCREEN_WIDTH, 1, line_buffer, true);
      }
    }
    
    unsigned long push_t = micros() - t1;
    last_display_update = now;
    
    static unsigned long last_print = 0;
    if (millis() - last_print > 3000) {
      Serial.printf("push=%luus emul_fps=%d\n", push_t, current_fps);
      last_print = millis();
    }
  }

  nes_audio_service();

  frame_count++;
  if (millis() - last_fps_update >= 1000) {
    current_fps = frame_count;
    frame_count = 0;
    last_fps_update = millis();
  }
}

#endif /* NES_USE_ST7305 */

extern "C" int vid_init(int width, int height, viddriver_t *osd_driver) {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, NES_BITMAP_OVERDRAW); 
  frame_buffer = NULL;
  low_mem_video_mode = false;
  video_ready = (game_bitmap && game_bitmap->data);
  Serial.printf("vid_init heap snapshot: heap=%u spiram=%u\n", esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  Serial.printf("vid_init: bitmap %p, frame_buffer %p, ready %d, deferred_fb_alloc=1\n", game_bitmap, frame_buffer, video_ready);

  /* Which osd.cpp is actually in the binary, and when it was compiled.
   *
   * Worth the two lines permanently: this file lives in a library, so the
   * Arduino builder reaches it through the libraries folder rather than through
   * the sketch, and a stale copy or an uninvalidated build cache there produces
   * a working binary that silently ignores every edit made here. That failure
   * looks exactly like "the change did nothing", and nothing else in the log
   * distinguishes the two. __DATE__/__TIME__ are the compiler's, so a line
   * older than your last edit means this translation unit was not rebuilt. */
#if defined(NES_USE_ST7305) && NES_USE_ST7305
  Serial.printf("[VIDEO] osd.cpp built %s %s\n", __DATE__, __TIME__);
  Serial.printf("[VIDEO] stretched: src %dx%d rows %d..%d -> panel %dx%d\n",
                NES_SCREEN_WIDTH, NES_VISIBLE_HEIGHT, NES_VISIBLE_Y0,
                NES_VISIBLE_Y0 + NES_VISIBLE_HEIGHT - 1,
                DISPLAY_WIDTH, DISPLAY_HEIGHT);

  /* From here until vid_shutdown() the TE task owns the SPI bus and nothing on
   * this thread may flush. Started here rather than in the sketch because this
   * is the exact boundary: the menus and the splash are done, the emulator has
   * not drawn a frame yet, and the panel is holding a picture nobody will touch
   * again. NES_FRAME_INTERVAL_MS survives only as the fallback cadence for a
   * board where TE never fires. */
  if (video_ready) {
    Serial.printf("[VIDEO] TE pacing on GPIO %d, fallback %d ms\n",
                  RLCD_TE, NES_FRAME_INTERVAL_MS);
    if (!tft.startTePacing(RLCD_TE, NES_FRAME_INTERVAL_MS))
      Serial.println("[VIDEO] x TE task would not start - unpaced, expect tearing");
  }

#if NES_ASYNC_DITHER
  /* Two more bitmaps and the task that reads them. All or nothing: dithering on
   * another core without somewhere separate for the PPU to write would put a
   * moving seam through the picture, so a failure here falls all the way back to
   * dithering inline rather than half way. See the note above
   * nes_present_frame().
   *
   * Core 0, priority 1. The BLE keyboard is the only other thing there and sits
   * at priority 2, so a keypress still preempts a frame - which is the right way
   * round, a frame being 45 ms of work that nobody is waiting on. */
  /* ONCE, not once per game. vid_init() runs again every time a ROM is loaded,
   * and this used to leak two 61 KB bitmaps and start a second task on each
   * pass. dither_task being set is the record that it has already been done. */
  if (video_ready && !dither_task) {
    bmp_pool[0] = game_bitmap;
    bmp_pool[1] = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, NES_BITMAP_OVERDRAW);
    bmp_pool[2] = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, NES_BITMAP_OVERDRAW);

    if (!bmp_pool[1] || !bmp_pool[1]->data ||
        !bmp_pool[2] || !bmp_pool[2]->data) {
      bmp_pool[1] = bmp_pool[2] = NULL;
      Serial.println("[VIDEO] x no room for the extra frame buffers - "
                     "dithering stays on the emulator thread");
    } else if (xTaskCreatePinnedToCore(nes_dither_task_entry, "nes_dither",
                                       4096, NULL, 1,
                                       (TaskHandle_t *)&dither_task, 0)
               != pdPASS) {
      dither_task = NULL;
      Serial.println("[VIDEO] x dither task would not start - "
                     "dithering stays on the emulator thread");
    } else {
      Serial.println("[VIDEO] dithering on core 0, triple-buffered - emulator "
                     "fps is independent of the mode, and the panel gets a new "
                     "frame as fast as the mode can make one");
    }
  }
#endif
#endif

  return video_ready ? 0 : -1;
}

#if !(defined(NES_EMBEDDED_ROM) && NES_EMBEDDED_ROM)

extern "C" int osd_rom_open(const char *path) {
  if (rom_stream_file) {
    rom_stream_file.close();
  }

  Serial.printf("[ROM] Streaming open: %s\n", path ? path : "(null)");
  rom_stream_file = NES_FS.open(path, FILE_READ);
  if (!rom_stream_file) {
    Serial.println("[ROM] Streaming open failed");
    return -1;
  }
  return 0;
}

extern "C" int osd_rom_read(void *dst, int len) {
  if (!rom_stream_file || !dst || len <= 0) {
    return -1;
  }
  int n = rom_stream_file.read((uint8_t *)dst, len);
  return n;
}

extern "C" void osd_rom_close(void) {
  if (rom_stream_file) {
    rom_stream_file.close();
  }
}

#endif /* !NES_EMBEDDED_ROM - card reader */

/* Everything the video side has to give back before the menu may draw again.
 *
 * ORDER IS THE WHOLE FUNCTION. video_ready = false stops vid_flush publishing;
 * dropping bmp_latest stops the dither task starting anything new; then we wait
 * for whatever it already holds, because a task halfway through a 45 ms blit
 * when the panel changes hands would be writing GRAM the menu is also writing.
 * Only then does the TE task give the bus back.
 *
 * The dither task itself is NOT stopped and NOT deleted. It blocks on its
 * notification and nothing notifies it while video_ready is false, so it costs
 * a sleeping task and its stack; deleting and recreating it per game would buy
 * nothing and add a shutdown race to a path that currently has none. */
extern "C" void vid_shutdown() {
  video_ready = false;

#if NES_ASYNC_DITHER
  portENTER_CRITICAL(&video_mux);
  bmp_latest = NULL;
  portEXIT_CRITICAL(&video_mux);

  for (int i = 0; i < 400 && bmp_held; i++) delay(5);      /* up to 2 s */
  if (bmp_held)
    Serial.println("[VIDEO] x dither task still busy - releasing the panel "
                   "anyway, expect one corrupt frame");
#endif

#if defined(NES_USE_ST7305) && NES_USE_ST7305
  /* Hands the bus back before anything on this thread flushes again. */
  tft.stopTePacing();
#endif
}

extern "C" int vid_setmode(int width, int height) {
  return 0;
}

extern "C" void vid_setpalette(rgb_t *pal) {
  osd_setpalette(pal);
}

extern "C" bitmap_t *vid_getbuffer() {
  if (!game_bitmap) game_bitmap = bmp_create(NES_SCREEN_WIDTH, NES_SCREEN_HEIGHT, NES_BITMAP_OVERDRAW);
  video_ready = (game_bitmap && game_bitmap->data);
  return game_bitmap;
}

extern "C" void osd_getvideoinfo(vidinfo_t *info) {
  info->default_width = NES_SCREEN_WIDTH;
  info->default_height = NES_SCREEN_HEIGHT;
  info->driver = 0;
}

extern "C" void osd_togglefullscreen(int code) {}

extern "C" char *osd_newextension(char *string, char *ext) {
  return string;
}

extern "C" void osd_fullname(char *fullname, const char *shortname) {
  strcpy(fullname, shortname);
}

extern "C" int osd_main(int argc, char *argv[]) {
  runtime_sound_enabled = false;
  for (int i = 1; i < argc; i++) {
    if (argv[i] && strcmp(argv[i], "-sound") == 0) {
      runtime_sound_enabled = true;
    }
    if (argv[i] && strcmp(argv[i], "-nosound") == 0) {
      runtime_sound_enabled = false;
    }
  }

  char *rom_name = "rom";
  if (argc > 0) rom_name = argv[argc - 1];
  return main_loop(rom_name, system_nes);
}

extern "C" int nofrendo_log_init(void) {
  return 0;
}

extern "C" void nofrendo_log_shutdown(void) {}

extern "C" int nofrendo_log_print(const char *s) {
  if (s) {
    Serial.print(s);
  }
  return 0;
}

extern "C" int nofrendo_log_printf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  int written = Serial.vprintf(format, args);
  va_end(args);
  return written;
}

extern "C" void nofrendo_log_assert(int expr, int line, const char *file, char *msg) {
  if (!expr) {
    Serial.printf("ASSERT FAILED: %s:%d %s\n", file ? file : "?", line, msg ? msg : "");
  }
}
