/* Connects the Archipelago client to Snes9x's memory.
 * Address layout matches what the official SNI-based client uses:
 *   0x000000-0xDFFFFF  ROM, by file offset
 *   0xE00000-0xEFFFFF  cartridge save RAM
 *   0xF50000-0xF6FFFF  work RAM */
#include "snes9x.h"
#include "memmap.h"

#include "ap_client.h"

static uint8_t *resolve(uint32_t addr, size_t len)
{
   if (addr >= 0xF50000u && addr + len <= 0xF50000u + sizeof(Memory.RAM))
      return Memory.RAM + (addr - 0xF50000u);
   if (addr >= 0xE00000u && addr < 0xF00000u)
   {
      size_t off = addr - 0xE00000u;
      if (Memory.SRAM && off + len <= Memory.SRAMStorage.size())
         return Memory.SRAM + off;
      return NULL;
   }
   if (addr < 0xE00000u)
   {
      if (Memory.ROM && addr + len <= (uint32_t)Memory.CalculatedSize)
         return Memory.ROM + addr;
      return NULL;
   }
   return NULL;
}

extern "C" int ap_mem_read(uint32_t addr, uint8_t *out, size_t len)
{
   uint8_t *p = resolve(addr, len);
   if (!p)
      return 0;
   memcpy(out, p, len);
   return 1;
}

extern "C" int ap_mem_write(uint32_t addr, const uint8_t *in, size_t len)
{
   /* ROM is read-only for the client. */
   if (addr < 0xE00000u)
      return 0;
   uint8_t *p = resolve(addr, len);
   if (!p)
      return 0;
   memcpy(p, in, len);
   return 1;
}
