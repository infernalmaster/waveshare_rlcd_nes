/* esp_heap_caps.h - stub, host compilation only. See Arduino.h in this folder.
 *
 * The capability flags are declared so a sketch's MALLOC_CAP_SPIRAM allocation
 * type-checks. On the host every allocation comes from the ordinary heap and
 * the "free PSRAM" queries answer 0, so a sketch compiled here takes its
 * no-PSRAM branch. That branch existing and being reachable is the point.
 */
#ifndef ESP_HEAP_CAPS_STUB_H
#define ESP_HEAP_CAPS_STUB_H

#include <stdint.h>
#include <stddef.h>

#define MALLOC_CAP_EXEC       (1 << 0)
#define MALLOC_CAP_32BIT      (1 << 1)
#define MALLOC_CAP_8BIT       (1 << 2)
#define MALLOC_CAP_DMA        (1 << 3)
#define MALLOC_CAP_SPIRAM     (1 << 10)
#define MALLOC_CAP_INTERNAL   (1 << 11)
#define MALLOC_CAP_DEFAULT    (1 << 12)

#ifdef __cplusplus
extern "C" {
#endif

void  *heap_caps_malloc(size_t size, uint32_t caps);
void  *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void   heap_caps_free(void *ptr);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_total_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);

uint32_t esp_get_free_heap_size(void);
uint32_t esp_get_minimum_free_heap_size(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP_HEAP_CAPS_STUB_H */
