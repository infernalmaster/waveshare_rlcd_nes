/* pgmspace.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * On the ESP32 flash is memory-mapped, so PROGMEM is a section attribute and
 * the pgm_read_* macros are plain dereferences - which is exactly what they are
 * here. Sketches carrying AVR-era PROGMEM tables therefore behave the same way
 * on the host as they do on the target, unlike most of these stubs.
 */
#ifndef PGMSPACE_STUB_H
#define PGMSPACE_STUB_H

#include <stdint.h>
#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif
#define PGM_P const char *
#define PSTR(s) (s)

#define pgm_read_byte(addr)   (*(const uint8_t  *)(addr))
#define pgm_read_word(addr)   (*(const uint16_t *)(addr))
#define pgm_read_dword(addr)  (*(const uint32_t *)(addr))
#define pgm_read_ptr(addr)    (*(void * const *)(addr))

#define pgm_read_byte_near(addr) pgm_read_byte(addr)
#define pgm_read_word_near(addr) pgm_read_word(addr)

#define strlen_P  strlen
#define strcpy_P  strcpy
#define memcpy_P  memcpy

#endif /* PGMSPACE_STUB_H */
