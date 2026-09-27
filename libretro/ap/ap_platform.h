/* Small platform layer for the Archipelago client: threads, a lock, sleep
 * and a millisecond clock. POSIX everywhere except the 3DS (libctru). */
#ifndef AP_PLATFORM_H
#define AP_PLATFORM_H

#include <stdint.h>

#ifdef _3DS
#include <3ds.h>

typedef LightLock ap_lock_t;
#define AP_LOCK_INIT(l)   LightLock_Init(l)
#define AP_LOCK(l)        LightLock_Lock(l)
#define AP_UNLOCK(l)      LightLock_Unlock(l)

typedef Thread ap_thread_t;

static inline int ap_thread_start(ap_thread_t *t, void (*fn)(void *), void *arg)
{
   s32 prio = 0x30;
   svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
   /* Slightly lower priority than the emulator so networking only uses
    * spare time; 256 KB stack for the TLS handshake. */
   *t = threadCreate(fn, arg, 256 * 1024, prio + 1 > 0x3F ? 0x3F : prio + 1, -2, false);
   return *t ? 0 : -1;
}

static inline void ap_thread_join(ap_thread_t *t)
{
   if (*t)
   {
      threadJoin(*t, U64_MAX);
      threadFree(*t);
      *t = NULL;
   }
}

static inline long long ap_now_ms(void)
{
   return (long long)osGetTime();
}

static inline void ap_sleep_ms(int ms)
{
   svcSleepThread((s64)ms * 1000000LL);
}

#else /* POSIX */
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef pthread_mutex_t ap_lock_t;
#define AP_LOCK_INIT(l)   pthread_mutex_init(l, NULL)
#define AP_LOCK(l)        pthread_mutex_lock(l)
#define AP_UNLOCK(l)      pthread_mutex_unlock(l)

typedef struct
{
   pthread_t id;
   int running;
   void (*fn)(void *);
   void *arg;
} ap_thread_t;

static void *ap_thread_trampoline(void *p)
{
   ap_thread_t *t = (ap_thread_t *)p;
   t->fn(t->arg);
   return NULL;
}

static inline int ap_thread_start(ap_thread_t *t, void (*fn)(void *), void *arg)
{
   t->fn = fn;
   t->arg = arg;
   if (pthread_create(&t->id, NULL, ap_thread_trampoline, t) != 0)
      return -1;
   t->running = 1;
   return 0;
}

static inline void ap_thread_join(ap_thread_t *t)
{
   if (t->running)
   {
      pthread_join(t->id, NULL);
      t->running = 0;
   }
}

static inline long long ap_now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline void ap_sleep_ms(int ms)
{
   usleep((useconds_t)ms * 1000);
}
#endif

#endif
