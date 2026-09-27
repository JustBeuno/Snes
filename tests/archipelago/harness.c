/* Test harness: runs the real Archipelago client code (ap_client.c, ap_ws.c)
 * against simulated SMZ3 memory, driven by commands on stdin:
 *   check <smz3 location index>   game "finds" a location (queues it in the outbox)
 *   goal                          game reaches the ending
 *   inbox                         print everything the client has written to the inbox
 *   reload                        simulate stopping and restarting the core (keeps save RAM)
 *   quit
 * Emulates the new (2-byte) message queue layout used by current SMZ3 ROMs. */
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ap_client.h"

static uint8_t rom[0x10000];
static uint8_t sram[0x8000];
static uint8_t wram[0x20000];
static const char *sysdir;

#define Q 0x4000
#define SEND_PTR (Q + 0xD3C)
#define SEND_TABLE (Q + 0xDA0)
#define RECV_PTR (Q + 0xD38)

int ap_mem_read(uint32_t a, uint8_t *o, size_t n)
{
   if (a < sizeof(rom) && a + n <= sizeof(rom)) { memcpy(o, rom + a, n); return 1; }
   if (a >= 0xE00000 && a - 0xE00000 + n <= sizeof(sram)) { memcpy(o, sram + (a - 0xE00000), n); return 1; }
   if (a >= 0xF50000 && a - 0xF50000 + n <= sizeof(wram)) { memcpy(o, wram + (a - 0xF50000), n); return 1; }
   return 0;
}

int ap_mem_write(uint32_t a, const uint8_t *i, size_t n)
{
   if (a >= 0xE00000 && a - 0xE00000 + n <= sizeof(sram)) { memcpy(sram + (a - 0xE00000), i, n); return 1; }
   if (a >= 0xF50000 && a - 0xF50000 + n <= sizeof(wram)) { memcpy(wram + (a - 0xF50000), i, n); return 1; }
   return 0;
}

static bool env(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *(const char **)data = sysdir;
      return true;
   case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *(unsigned *)data = 1;
      return true;
   case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      printf("OSD: %s\n", ((struct retro_message_ext *)data)->msg);
      fflush(stdout);
      return true;
   case RETRO_ENVIRONMENT_SET_MESSAGE:
      printf("OSD: %s\n", ((struct retro_message *)data)->msg);
      fflush(stdout);
      return true;
   default:
      return false;
   }
}

static unsigned rd16(unsigned off) { return sram[off] | (sram[off + 1] << 8); }
static void wr16(unsigned off, unsigned v) { sram[off] = v & 0xFF; sram[off + 1] = (v >> 8) & 0xFF; }

static void game_check(int index)
{
   /* The ROM appends to the outbox: message word = index<<3, Z3 flag in bit 15. */
   unsigned count = rd16(SEND_PTR + 2);
   unsigned word;
   if (index >= 256)
      word = ((unsigned)(index - 256) << 3) | 0x8000;
   else
      word = (unsigned)index << 3;
   wr16(SEND_TABLE + count * 2, word);
   wr16(SEND_PTR + 2, count + 1);
}

int main(int argc, char **argv)
{
   char line[256];
   static char pend[8192];
   size_t pend_len = 0;
   struct pollfd p = {0, POLLIN, 0};
   if (argc < 3)
   {
      fprintf(stderr, "usage: harness <rom title> <config dir>\n");
      return 1;
   }
   sysdir = argv[2];
   memset(rom + 0xFFC0, ' ', 21);
   memcpy(rom + 0xFFC0, argv[1], strlen(argv[1]) > 21 ? 21 : strlen(argv[1]));
   wram[0x10] = 0x07; /* Zelda half, in game */

   ap_start(env, NULL);
   for (;;)
   {
      struct timespec ts = {0, 16666667};
      ap_frame();
      nanosleep(&ts, NULL);
      while (poll(&p, 1, 0) > 0 || strchr(pend, '\n'))
      {
         char *nl = strchr(pend, '\n');
         if (!nl)
         {
            ssize_t r = read(0, pend + pend_len, sizeof(pend) - 1 - pend_len);
            if (r <= 0)
               goto done;
            pend_len += (size_t)r;
            pend[pend_len] = 0;
            continue;
         }
         *nl = 0;
         snprintf(line, sizeof(line), "%s", pend);
         pend_len -= (size_t)(nl + 1 - pend);
         memmove(pend, nl + 1, pend_len + 1);
         if (!strncmp(line, "check ", 6))
            game_check(atoi(line + 6));
         else if (!strncmp(line, "goal", 4))
            wram[0x10] = 0x19;
         else if (!strncmp(line, "inbox", 5))
         {
            unsigned n = rd16(RECV_PTR), i;
            printf("INBOX %u:", n);
            for (i = 0; i < n; i++)
               printf(" %u/%u", sram[Q + i * 2], sram[Q + i * 2 + 1]);
            printf("\nOUTBOX sent=%u queued=%u\n", rd16(SEND_PTR), rd16(SEND_PTR + 2));
            fflush(stdout);
         }
         else if (!strncmp(line, "reload", 6))
         {
            ap_stop();
            ap_start(env, NULL);
            printf("RELOADED\n");
            fflush(stdout);
         }
         else if (!strncmp(line, "quit", 4))
            goto done;
      }
   }
done:
   ap_stop();
   printf("BYE\n");
   return 0;
}
