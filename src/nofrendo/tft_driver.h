#ifndef TFT_DRIVER_H
#define TFT_DRIVER_H

#include <Arduino.h>
#include <SPI.h>
#include "hw_config.h"

/* ---------------------------------------------------------------------------
 * Alternate display back end.
 *
 * With NES_USE_ST7305 set in the sketch's hw_config.h, TFTDriver becomes the
 * ST7305Tft wrapper from st7305_gfx - a 400x300 1-bit reflective panel instead
 * of the 280x240 colour ST7789 below. The wrapper exposes the same methods this
 * class does, so osd.cpp compiles against either one.
 *
 * st7305_tft.h and st7305_gfx.h live in the SKETCH ROOT, one level up from this
 * src/nofrendo/ folder. The Arduino builder puts the sketch root on the include
 * path for every translation unit it compiles - the same mechanism that lets
 * this file include hw_config.h.
 *
 * One semantic difference the callers have to know about: the wrapper buffers.
 * Drawing does not reach the glass until flush(). See st7305_tft.h.
 * ------------------------------------------------------------------------- */
#if defined(NES_USE_ST7305) && NES_USE_ST7305

#include "st7305_tft.h"
typedef ST7305Tft TFTDriver;

#else

#define TFT_SWRESET   0x01
#define TFT_SLPOUT    0x11
#define TFT_COLMOD    0x3A
#define TFT_MADCTL    0x36
#define TFT_CASET     0x2A
#define TFT_RASET     0x2B
#define TFT_RAMWR     0x2C
#define TFT_DISPON    0x29
#define TFT_DISPOFF   0x28
#define TFT_INVON     0x21
#define TFT_INVOFF    0x20
 
#define DISPLAY_WIDTH  280
#define DISPLAY_HEIGHT 240
 
#define ST7789_COL_OFFSET 20
 
#define TFT_BLACK   0x0000
#define TFT_WHITE   0xFFFF
#define TFT_RED     0xF800
#define TFT_GREEN   0x07E0
#define TFT_BLUE    0x001F
 
static const uint8_t font_5x7[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, // 0x20 ' '
    0x00, 0x00, 0x5F, 0x00, 0x00, // 0x21 '!'
    0x00, 0x07, 0x00, 0x07, 0x00, // 0x22 '"'
    0x14, 0x7F, 0x14, 0x7F, 0x14, // 0x23 '#'
    0x24, 0x2A, 0x7F, 0x2A, 0x12, // 0x24 '$'
    0x23, 0x13, 0x08, 0x64, 0x62, // 0x25 '%'
    0x36, 0x49, 0x55, 0x22, 0x50, // 0x26 '&'
    0x00, 0x05, 0x03, 0x00, 0x00, // 0x27 '''
    0x00, 0x1C, 0x22, 0x41, 0x00, // 0x28 '('
    0x00, 0x41, 0x22, 0x1C, 0x00, // 0x29 ')'
    0x14, 0x08, 0x3E, 0x08, 0x14, // 0x2A '*'
    0x08, 0x08, 0x3E, 0x08, 0x08, // 0x2B '+'
    0x00, 0x50, 0x30, 0x00, 0x00, // 0x2C ','
    0x08, 0x08, 0x08, 0x08, 0x08, // 0x2D '-'
    0x00, 0x60, 0x60, 0x00, 0x00, // 0x2E '.'
    0x20, 0x10, 0x08, 0x04, 0x02, // 0x2F '/'
    0x3E, 0x51, 0x49, 0x45, 0x3E, // 0x30 '0'
    0x00, 0x42, 0x7F, 0x40, 0x00, // 0x31 '1'
    0x42, 0x61, 0x51, 0x49, 0x46, // 0x32 '2'
    0x21, 0x41, 0x45, 0x4B, 0x31, // 0x33 '3'
    0x18, 0x14, 0x12, 0x7F, 0x10, // 0x34 '4'
    0x27, 0x45, 0x45, 0x45, 0x39, // 0x35 '5'
    0x3C, 0x4A, 0x49, 0x49, 0x30, // 0x36 '6'
    0x01, 0x71, 0x09, 0x05, 0x03, // 0x37 '7'
    0x36, 0x49, 0x49, 0x49, 0x36, // 0x38 '8'
    0x06, 0x49, 0x49, 0x29, 0x1E, // 0x39 '9'
    0x00, 0x36, 0x36, 0x00, 0x00, // 0x3A ':'
    0x00, 0x56, 0x36, 0x00, 0x00, // 0x3B ';'
    0x08, 0x14, 0x22, 0x41, 0x00, // 0x3C '<'
    0x14, 0x14, 0x14, 0x14, 0x14, // 0x3D '='
    0x00, 0x41, 0x22, 0x14, 0x08, // 0x3E '>'
    0x02, 0x01, 0x51, 0x09, 0x06, // 0x3F '?'
    0x32, 0x49, 0x79, 0x41, 0x3E, // 0x40 '@'
    0x7E, 0x11, 0x11, 0x11, 0x7E, // 0x41 'A'
    0x7F, 0x49, 0x49, 0x49, 0x36, // 0x42 'B'
    0x3E, 0x41, 0x41, 0x41, 0x22, // 0x43 'C'
    0x7F, 0x41, 0x41, 0x22, 0x1C, // 0x44 'D'
    0x7F, 0x49, 0x49, 0x49, 0x41, // 0x45 'E'
    0x7F, 0x09, 0x09, 0x09, 0x01, // 0x46 'F'
    0x3E, 0x41, 0x49, 0x49, 0x7A, // 0x47 'G'
    0x7F, 0x08, 0x08, 0x08, 0x7F, // 0x48 'H'
    0x00, 0x41, 0x7F, 0x41, 0x00, // 0x49 'I'
    0x20, 0x40, 0x41, 0x3F, 0x01, // 0x4A 'J'
    0x7F, 0x08, 0x14, 0x22, 0x41, // 0x4B 'K'
    0x7F, 0x40, 0x40, 0x40, 0x40, // 0x4C 'L'
    0x7F, 0x02, 0x0C, 0x02, 0x7F, // 0x4D 'M'
    0x7F, 0x04, 0x08, 0x10, 0x7F, // 0x4E 'N'
    0x3E, 0x41, 0x41, 0x41, 0x3E, // 0x4F 'O'
    0x7F, 0x09, 0x09, 0x09, 0x06, // 0x50 'P'
    0x3E, 0x41, 0x51, 0x21, 0x5E, // 0x51 'Q'
    0x7F, 0x09, 0x19, 0x29, 0x46, // 0x52 'R'
    0x46, 0x49, 0x49, 0x49, 0x31, // 0x53 'S'
    0x01, 0x01, 0x7F, 0x01, 0x01, // 0x54 'T'
    0x3F, 0x40, 0x40, 0x40, 0x3F, // 0x55 'U'
    0x1F, 0x20, 0x40, 0x20, 0x1F, // 0x56 'V'
    0x7F, 0x20, 0x18, 0x20, 0x7F, // 0x57 'W'
    0x63, 0x14, 0x08, 0x14, 0x63, // 0x58 'X'
    0x03, 0x04, 0x78, 0x04, 0x03, // 0x59 'Y'
    0x61, 0x51, 0x49, 0x45, 0x43, // 0x5A 'Z'
    0x00, 0x7F, 0x41, 0x41, 0x00, // 0x5B '['
    0x02, 0x04, 0x08, 0x10, 0x20, // 0x5C '\'
    0x00, 0x41, 0x41, 0x7F, 0x00, // 0x5D ']'
    0x04, 0x02, 0x01, 0x02, 0x04, // 0x5E '^'
    0x40, 0x40, 0x40, 0x40, 0x40, // 0x5F '_'
    0x00, 0x01, 0x02, 0x04, 0x00, // 0x60 '`'
    0x20, 0x54, 0x54, 0x54, 0x78, // 0x61 'a'
    0x7F, 0x48, 0x44, 0x44, 0x38, // 0x62 'b'
    0x38, 0x44, 0x44, 0x44, 0x20, // 0x63 'c'
    0x38, 0x44, 0x44, 0x48, 0x7F, // 0x64 'd'
    0x38, 0x54, 0x54, 0x54, 0x18, // 0x65 'e'
    0x08, 0x7E, 0x09, 0x01, 0x02, // 0x66 'f'
    0x0C, 0x52, 0x52, 0x52, 0x3E, // 0x67 'g'
    0x7F, 0x08, 0x04, 0x04, 0x78, // 0x68 'h'
    0x00, 0x44, 0x7D, 0x40, 0x00, // 0x69 'i'
    0x20, 0x40, 0x44, 0x3D, 0x00, // 0x6A 'j'
    0x7F, 0x10, 0x28, 0x44, 0x00, // 0x6B 'k'
    0x00, 0x41, 0x7F, 0x40, 0x00, // 0x6C 'l'
    0x7C, 0x04, 0x18, 0x04, 0x78, // 0x6D 'm'
    0x7C, 0x08, 0x04, 0x04, 0x78, // 0x6E 'n'
    0x38, 0x44, 0x44, 0x44, 0x38, // 0x6F 'o'
    0x7C, 0x14, 0x14, 0x14, 0x08, // 0x70 'p'
    0x08, 0x14, 0x14, 0x18, 0x7C, // 0x71 'q'
    0x7C, 0x08, 0x04, 0x04, 0x08, // 0x72 'r'
    0x48, 0x54, 0x54, 0x54, 0x20, // 0x73 's'
    0x04, 0x3F, 0x44, 0x40, 0x20, // 0x74 't'
    0x3C, 0x40, 0x40, 0x20, 0x7C, // 0x75 'u'
    0x1C, 0x20, 0x40, 0x20, 0x1C, // 0x76 'v'
    0x3C, 0x40, 0x30, 0x40, 0x3C, // 0x77 'w'
    0x44, 0x28, 0x10, 0x28, 0x44, // 0x78 'x'
    0x0C, 0x50, 0x50, 0x50, 0x3C, // 0x79 'y'
    0x44, 0x64, 0x54, 0x4C, 0x44, // 0x7A 'z'
    0x00, 0x08, 0x36, 0x41, 0x00, // 0x7B '{'
    0x00, 0x00, 0x7F, 0x00, 0x00, // 0x7C '|'
    0x00, 0x41, 0x36, 0x08, 0x00, // 0x7D '}'
    0x10, 0x08, 0x08, 0x10, 0x08, // 0x7E '~'
    0x00, 0x00, 0x00, 0x00, 0x00  // 0x7F DEL
};

class TFTDriver {
private:
    static constexpr int TFT_DC = 15;
    static constexpr int TFT_RST = 9;
    static constexpr int TFT_CS = 10;
    static constexpr int TFT_SCK = 12;
    static constexpr int TFT_MOSI = 11;
    static constexpr int TFT_MISO = -1;
    
    void spi_write_cmd(uint8_t cmd) {
        digitalWrite(TFT_CS, LOW);
        digitalWrite(TFT_DC, LOW);  
        SPI.write(cmd);
        digitalWrite(TFT_DC, HIGH);
        digitalWrite(TFT_CS, HIGH);
    }
    
    void spi_write_byte(uint8_t data) {
        digitalWrite(TFT_CS, LOW);
        digitalWrite(TFT_DC, HIGH);  
        SPI.write(data);
        digitalWrite(TFT_CS, HIGH);
    }
     
    void spi_write_word(uint16_t data) {
        digitalWrite(TFT_CS, LOW);
        digitalWrite(TFT_DC, HIGH); 
        SPI.write16(data);
        digitalWrite(TFT_CS, HIGH);
    }
  public:
    TFTDriver() {}
    
    void init() {
        pinMode(TFT_DC, OUTPUT);
        pinMode(TFT_RST, OUTPUT);
        pinMode(TFT_CS, OUTPUT);
        
        digitalWrite(TFT_CS, HIGH);
        digitalWrite(TFT_DC, HIGH);
         
        SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);
        SPI.setFrequency(80000000);
        SPI.setDataMode(SPI_MODE0);
        SPI.setBitOrder(MSBFIRST);
        
        digitalWrite(TFT_RST, HIGH);
        delay(30);
        digitalWrite(TFT_RST, LOW);
        delay(30);
        digitalWrite(TFT_RST, HIGH);
        delay(30);

        spi_write_cmd(TFT_SWRESET);
        delay(30);
        
        spi_write_cmd(TFT_SLPOUT);
        delay(50);
        
        spi_write_cmd(TFT_COLMOD);
        spi_write_byte(0x55);  
        delay(5);
        
        spi_write_cmd(TFT_MADCTL);
        spi_write_byte(0x60);  // Landscape: rotate 90°
        delay(5);
        
        spi_write_cmd(TFT_CASET);
        spi_write_cmd(TFT_RASET);
        delay(5);

        fillScreen(TFT_BLACK);
        delay(20);
        
        spi_write_cmd(TFT_DISPON);
        delay(50);

        spi_write_cmd(TFT_INVON);
        delay(10);
        
    }
    
    void fillScreen(uint16_t color) {
        spi_write_cmd(TFT_CASET);
        spi_write_word(ST7789_COL_OFFSET);
        spi_write_word(ST7789_COL_OFFSET + DISPLAY_WIDTH - 1);
        
        spi_write_cmd(TFT_RASET);
        spi_write_word(0);
        spi_write_word(DISPLAY_HEIGHT - 1);
        
        spi_write_cmd(TFT_RAMWR);
         
        digitalWrite(TFT_CS, LOW);
        digitalWrite(TFT_DC, HIGH);  
        
        uint32_t pixel_count = (uint32_t)DISPLAY_WIDTH * DISPLAY_HEIGHT;
        static uint8_t buffer[512];  
         
        uint8_t high = color >> 8;
        uint8_t low = color & 0xFF;
        for (int i = 0; i < 512; i += 2) {
            buffer[i] = high;
            buffer[i + 1] = low;
        }
         
        for (uint32_t i = 0; i < pixel_count; i += 256) {
            uint32_t chunk_size = (pixel_count - i > 256) ? 256 : (pixel_count - i);
            SPI.writeBytes(buffer, chunk_size * 2);
        }
        
        digitalWrite(TFT_CS, HIGH);
    }
    
    void pushImage(int x, int y, int w, int h, const uint16_t *data, bool preswapped = false) {
 
        spi_write_cmd(TFT_CASET);
        spi_write_word(x + ST7789_COL_OFFSET);
        spi_write_word(x + w - 1 + ST7789_COL_OFFSET);
         
        spi_write_cmd(TFT_RASET);
        spi_write_word(y);
        spi_write_word(y + h - 1);
         
        spi_write_cmd(TFT_RAMWR);
         
        digitalWrite(TFT_CS, LOW);
        digitalWrite(TFT_DC, HIGH);  
         
        uint32_t pixel_count = (uint32_t)w * h;
         
        static uint8_t buffer[1024];  
         
        for (uint32_t i = 0; i < pixel_count; i += 512) {
            uint32_t chunk_size = (pixel_count - i > 512) ? 512 : (pixel_count - i);
            if (preswapped) { 
                SPI.writeBytes((const uint8_t *)&data[i], chunk_size * 2);
            } else {
                for (uint32_t j = 0; j < chunk_size; j++) {
                    uint16_t pixel = data[i + j];
                    buffer[j * 2] = pixel >> 8;      
                    buffer[j * 2 + 1] = pixel & 0xFF;  
                }
                SPI.writeBytes(buffer, chunk_size * 2);
            }
        }
         
        digitalWrite(TFT_CS, HIGH);
    } 
    void drawChar(int x, int y, char c, uint16_t fg, uint16_t bg, int scale = 1) {
        if (scale < 1) {
            scale = 1;
        }

        uint8_t idx = (c >= 0x20 && c <= 0x7F) ? static_cast<uint8_t>(c - 0x20) : static_cast<uint8_t>('?' - 0x20);
        const uint8_t *glyph = &font_5x7[idx * 5];

        for (int cy = 0; cy < 7; cy++) {
            for (int cx = 0; cx < 5; cx++) {
                uint8_t line = pgm_read_byte(&glyph[cx]);
                if (line & (1 << cy)) {
                    drawFilledRect(x + cx * scale, y + cy * scale, scale, scale, fg);
                } else {
                    drawFilledRect(x + cx * scale, y + cy * scale, scale, scale, bg);
                }
            }
        }
    }
      
    void drawFilledRect(int x, int y, int w, int h, uint16_t color) {
        if (w <= 0 || h <= 0) {
            return;
        }

        int x0 = max(0, x);
        int y0 = max(0, y);
        int x1 = min(DISPLAY_WIDTH - 1, x + w - 1);
        int y1 = min(DISPLAY_HEIGHT - 1, y + h - 1);

        if (x1 < x0 || y1 < y0) {
            return;
        }

        int clipped_w = x1 - x0 + 1;
        int clipped_h = y1 - y0 + 1;
         
        spi_write_cmd(TFT_CASET);
        spi_write_word(x0 + ST7789_COL_OFFSET);
        spi_write_word(x1 + ST7789_COL_OFFSET);
         
        spi_write_cmd(TFT_RASET);
         
        spi_write_word(y0);
        spi_write_word(y1);
         
        spi_write_cmd(TFT_RAMWR);
         
        digitalWrite(TFT_CS, LOW);
        digitalWrite(TFT_DC, HIGH);
         
        uint32_t pixels = (uint32_t)clipped_w * clipped_h;
         
        for (uint32_t i = 0; i < pixels; i++) {
             SPI.write16(color);
        }
         
         digitalWrite(TFT_CS, HIGH);
    }
      
    void drawNumber(int x, int y, int num, uint16_t fg, uint16_t bg, int scale = 1) {
        if (num < 0) num = 0;
        char buf[10];
        sprintf(buf, "%d", num);
         
        int px = x;
        for (int i = 0; buf[i] != '\0'; i++) {
            drawChar(px, y, buf[i], fg, bg, scale);
            px += (5 + 1) * scale; 
        }
    }
     
    void drawString(int x, int y, const char *str, uint16_t fg, uint16_t bg, int scale = 1) {
        int px = x;
        for (int i = 0; str[i] != '\0'; i++) {
            drawChar(px, y, str[i], fg, bg, scale);
            px += (5 + 1) * scale;  
            }
        }

        static uint16_t color565(uint8_t r, uint8_t g, uint8_t b) {
        return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        }
   };

#endif /* NES_USE_ST7305 */
#endif /* TFT_DRIVER_H */
