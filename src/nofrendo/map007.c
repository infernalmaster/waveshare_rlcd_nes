#include "noftypes.h"
#include "nes_mmc.h"
#include "nes_ppu.h"
#include "log.h"
 
static void map7_write(uint32 address, uint8 value)
{
   int mirror;
   UNUSED(address);

   /* ONE REGISTER, TWO DECISIONS. Bits 0-2 pick the 32K PRG bank; bit 4 picks
    * which of the two one-screen nametables the whole background is fetched
    * from. Battletoads flips that bit twice a frame with the bank unchanged -
    * a raster effect that draws the top of the picture from one nametable and
    * the rest from the other, with the changeover timed off the sprite 0 hit.
    * Tracing it here printed hundreds of lines a second and buried the log; the
    * state it produces is visible in the nt= field of the periodic line
    * instead. */
   mmc_bankrom(32, 0x8000, value);
   mirror = (value & 0x10) >> 4;
   ppu_mirror(mirror, mirror, mirror, mirror);
}

static void map7_init(void)
{
   mmc_bankrom(32, 0x8000, 0);
}

static map_memwrite map7_memwrite[] =
    {
        {0x8000, 0xFFFF, map7_write},
        {-1, -1, NULL}};

mapintf_t map7_intf =
    {
        7,             /* mapper number */
        "AOROM",       /* mapper name */
        map7_init,     /* init routine */
        NULL,          /* vblank callback */
        NULL,          /* hblank callback */
        NULL,          /* get state (snss) */
        NULL,          /* set state (snss) */
        NULL,          /* memory read structure */
        map7_memwrite, /* memory write structure */
        NULL           /* external sound device */
};
