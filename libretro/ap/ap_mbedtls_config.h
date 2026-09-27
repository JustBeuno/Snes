/* mbedTLS adjustments for platforms without Unix sockets/timers/entropy.
 * Only used on the 3DS build (see Makefile.common). */
#if defined(_3DS)
#undef MBEDTLS_NET_C            /* we do our own sockets */
#undef MBEDTLS_TIMING_C         /* Unix/Windows only */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT /* mbedtls_hardware_poll in ap_ws.c */
#define MBEDTLS_PLATFORM_MS_TIME_ALT /* mbedtls_ms_time in ap_ws.c */
#endif
