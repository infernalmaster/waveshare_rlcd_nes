#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "noftypes.h"
#include "bitmap.h"

#pragma GCC optimize("O3")

static void *esp32_bitmap_malloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = malloc(size);
    }
    return ptr;
}

static void esp32_bitmap_free(void *ptr) {
    free(ptr);
}

void bmp_clear(const bitmap_t *bitmap, uint8 color)
{
    memset(bitmap->data, color, bitmap->pitch * bitmap->height);
}

static bitmap_t *_make_bitmap(uint8 *data_addr, bool hw, int width,
                              int height, int pitch, int overdraw)
{
   bitmap_t *bitmap;
   int i;
 
   if (NULL == data_addr)
      return NULL;
  
   bitmap = (bitmap_t *)malloc(sizeof(bitmap_t) + (sizeof(uint8 *) * height));
   if (NULL == bitmap)
      return NULL;

   bitmap->hardware = hw;
   bitmap->height = height;
   bitmap->width = width;
   bitmap->data = data_addr; 
   bitmap->pitch = pitch + (overdraw * 2);
 
   if (false == bitmap->hardware)
   { 
      bitmap->pitch = (bitmap->pitch + 3) & ~3;
       
      uintptr_t aligned_data = (uintptr_t)bitmap->data + overdraw; 
      aligned_data = (aligned_data + 3) & ~3;
      
      bitmap->line[0] = (uint8 *)aligned_data;
   }
   else
   { 
      bitmap->line[0] = bitmap->data + overdraw;
   }
 
   for (i = 1; i < height; i++)
      bitmap->line[i] = bitmap->line[i - 1] + bitmap->pitch;

   return bitmap;
}
 
bitmap_t *bmp_create(int width, int height, int overdraw)
{
   uint8 *addr;
   int pitch;

   /* SIZED THE WAY _make_bitmap() WILL ACTUALLY LAY IT OUT, which the old
    * expression was not. It asked for `((pitch * height) + 3) & ~7`, and that
    * mask rounds the total DOWN - by up to four bytes - while the layout below
    * needs strictly more than pitch * height:
    *
    *   - pitch is rounded UP to a multiple of 4 there, not here
    *   - line[0] is pushed past the left margin and then to the next 4-byte
    *     boundary, costing up to 3 bytes the old sum never counted
    *   - the last line still has a right margin after it
    *
    * At 256x240 with no margin the two happened to agree exactly, which is why
    * this survived; with the 8-pixel margin the renderer has always needed, the
    * worst case runs three bytes past the end of the allocation. Rounding the
    * right answer up beats rounding a wrong one down. */
   pitch = ((width + (overdraw * 2)) + 3) & ~3;

   size_t total_size = (size_t)overdraw + 3
                     + (size_t)(height - 1) * (size_t)pitch
                     + (size_t)width + (size_t)overdraw;
   total_size = (total_size + 7) & ~(size_t)7;

   addr = (uint8 *)esp32_bitmap_malloc(total_size);
   
   if (NULL == addr)
      return NULL;

   return _make_bitmap(addr, false, width, height, width, overdraw);
}
 
bitmap_t *bmp_createhw(uint8 *addr, int width, int height, int pitch)
{ 
   return _make_bitmap(addr, true, width, height, pitch, 0); 
}
 
void bmp_destroy(bitmap_t **bitmap)
{
   if (*bitmap)
   { 
      if ((*bitmap)->data && false == (*bitmap)->hardware)
         esp32_bitmap_free((*bitmap)->data);
       
      free(*bitmap);
      *bitmap = NULL;
   }
}
