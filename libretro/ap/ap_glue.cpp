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
      uint32_t size = Memory.CalculatedSize;
      uint32_t idx = addr;
      if (!Memory.ROM || addr + len > size)
         return NULL;
      /* Snes9x moves the halves of some large (ExHiROM) files around when
       * loading. Map a file offset to where Snes9x actually put it. */
      if (Memory.ExtendedFormat == CMemory::SMALLFIRST && size > 0x400000u)
      {
         uint32_t small = size - 0x400000u;
         idx = addr < small ? addr + 0x400000u : addr - small;
         if ((addr < small) != (addr + len - 1 < small))
            return NULL; /* read straddles the two halves */
      }
      return Memory.ROM + idx;
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

/* ---------------------------------------------------------------- */
/* On-screen text, drawn straight into the game picture with Snes9x's
 * own 8x9 font. Doesn't depend on RetroArch's notification settings. */

#include "font.h"

#define AP_FONT_W 8
#define AP_FONT_H 9
#define AP_MAX_LINES 4

static void draw_char(uint16_t *screen, int pitch, int width, int height,
                      int x0, int y0, int sx, int sy, unsigned char c)
{
   int line, col;
   if (c < 32)
      c = '?';
   line = ((c - 32) / 16) * AP_FONT_H;
   col = ((c - 32) % 16) * AP_FONT_W;
   if (line + AP_FONT_H > (int)(sizeof(font) / sizeof(font[0])))
      return;
   for (int y = 0; y < AP_FONT_H; y++)
   {
      const char *row = font[line + y] + col;
      for (int x = 0; x < AP_FONT_W; x++)
      {
         uint16_t px;
         if (row[x] == '#')
            px = 0xFFFF;
         else if (row[x] == '.')
            px = 0x0000;
         else
            continue;
         for (int yy = 0; yy < sy; yy++)
            for (int xx = 0; xx < sx; xx++)
            {
               int X = x0 + x * sx + xx, Y = y0 + y * sy + yy;
               if (X >= 0 && X < width && Y >= 0 && Y < height)
                  screen[(size_t)Y * pitch + X] = px;
            }
      }
   }
}

extern "C" void ap_draw_overlay(uint16_t *screen, int pitch, int width, int height)
{
   const char *text = ap_overlay_text();
   char lines[AP_MAX_LINES][80];
   int nlines = 0;
   int sx = width >= 512 ? 2 : 1;
   int sy = height > 256 ? 2 : 1;
   int cols = width / (AP_FONT_W * sx) - 2;
   if (!text || !screen || cols < 8)
      return;
   if (cols > 79)
      cols = 79;

   /* Word-wrap; break long words (like file paths) at the edge. */
   while (*text && nlines < AP_MAX_LINES)
   {
      int len = (int)strlen(text);
      int take = len <= cols ? len : cols;
      if (len > cols)
      {
         int sp = take;
         while (sp > 0 && text[sp] != ' ' && text[sp] != '/')
            sp--;
         if (sp > cols / 2)
            take = text[sp] == '/' ? sp + 1 : sp;
      }
      memcpy(lines[nlines], text, take);
      lines[nlines][take] = 0;
      nlines++;
      text += take;
      while (*text == ' ')
         text++;
   }

   int line_h = (AP_FONT_H + 1) * sy;
   int box_h = nlines * line_h + 4 * sy;
   int y_top = height - box_h - 12 * sy;
   if (y_top < 0)
      y_top = 0;

   /* Darken a band behind the text so it reads over any scene. */
   for (int y = y_top; y < y_top + box_h && y < height; y++)
      for (int x = 0; x < width; x++)
      {
         uint16_t p = screen[(size_t)y * pitch + x];
         screen[(size_t)y * pitch + x] = (uint16_t)(((p & 0xE79C) >> 2));
      }

   for (int i = 0; i < nlines; i++)
   {
      int y = y_top + 2 * sy + i * line_h;
      int x = AP_FONT_W * sx;
      for (const char *c = lines[i]; *c; c++, x += AP_FONT_W * sx)
         draw_char(screen, pitch, width, height, x, y, sx, sy, (unsigned char)*c);
   }
}
