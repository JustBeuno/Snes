/* Tiny libretro frontend: loads the real core, a ROM path, runs frames and
 * prints on-screen messages. Used to test the Archipelago hooks end to end. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libretro.h"

static const char *sysdir;

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (!strstr(fmt, "Archipelago") && level < RETRO_LOG_WARN)
      return;
   va_start(ap, fmt);
   vprintf(fmt, ap);
   va_end(ap);
   fflush(stdout);
}

static bool env(unsigned cmd, void *data)
{
   switch (cmd)
   {
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
   case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = sysdir;
      return true;
   case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
      ((struct retro_log_callback *)data)->log = log_cb;
      return true;
   case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *(unsigned *)data = 1;
      return true;
   case RETRO_ENVIRONMENT_SET_MESSAGE_EXT:
      printf("OSD: %s\n", ((struct retro_message_ext *)data)->msg);
      fflush(stdout);
      return true;
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return true;
   default:
      return false;
   }
}

static int frame_no, dump_at = -1;
static const char *dump_path;
static void video(const void *d, unsigned w, unsigned h, size_t p)
{
   FILE *f;
   unsigned x, y;
   if (++frame_no != dump_at || !d || !dump_path)
      return;
   f = fopen(dump_path, "wb");
   fprintf(f, "P6 %u %u 255\n", w, h);
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         uint16_t px = ((const uint16_t *)((const uint8_t *)d + y * p))[x];
         unsigned char rgb[3] = {(unsigned char)((px >> 11) << 3), (unsigned char)(((px >> 5) & 63) << 2), (unsigned char)((px & 31) << 3)};
         fwrite(rgb, 1, 3, f);
      }
   fclose(f);
}
static void audio(int16_t l, int16_t r) {}
static size_t audio_batch(const int16_t *d, size_t f) { return f; }
static void poll_cb(void) {}
static int16_t input(unsigned p, unsigned d, unsigned i, unsigned id) { return 0; }

int main(int argc, char **argv)
{
   void *h;
   struct retro_game_info info;
   int frames, i;
   if (argc < 5)
   {
      fprintf(stderr, "usage: frontend <core.so> <rom> <sysdir> <seconds>\n");
      return 1;
   }
   sysdir = argv[3];
   if (getenv("DUMP_FRAME")) { dump_at = atoi(getenv("DUMP_FRAME")); dump_path = getenv("DUMP_PATH"); }
   frames = atoi(argv[4]) * 60;
   h = dlopen(argv[1], RTLD_NOW);
   if (!h)
   {
      fprintf(stderr, "%s\n", dlerror());
      return 1;
   }
#define SYM(n) void *n##_p = dlsym(h, #n)
   ((void (*)(retro_environment_t))dlsym(h, "retro_set_environment"))(env);
   ((void (*)(retro_video_refresh_t))dlsym(h, "retro_set_video_refresh"))(video);
   ((void (*)(retro_audio_sample_t))dlsym(h, "retro_set_audio_sample"))(audio);
   ((void (*)(retro_audio_sample_batch_t))dlsym(h, "retro_set_audio_sample_batch"))(audio_batch);
   ((void (*)(retro_input_poll_t))dlsym(h, "retro_set_input_poll"))(poll_cb);
   ((void (*)(retro_input_state_t))dlsym(h, "retro_set_input_state"))(input);
   ((void (*)(void))dlsym(h, "retro_init"))();

   memset(&info, 0, sizeof(info));
   info.path = argv[2];
   if (!((bool (*)(const struct retro_game_info *))dlsym(h, "retro_load_game"))(&info))
   {
      printf("LOAD FAILED\n");
      return 1;
   }
   printf("LOADED\n");
   fflush(stdout);
   for (i = 0; i < frames; i++)
   {
      struct timespec ts = {0, 16666667};
      ((void (*)(void))dlsym(h, "retro_run"))();
      nanosleep(&ts, NULL);
   }
   ((void (*)(void))dlsym(h, "retro_unload_game"))();
   ((void (*)(void))dlsym(h, "retro_deinit"))();
   printf("DONE\n");
   return 0;
}
