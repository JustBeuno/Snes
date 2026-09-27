/* Minimal WebSocket client (RFC 6455) over plain TCP or mbedTLS.
 * Built for the Archipelago connection inside the Snes9x libretro core.
 * All socket I/O is non-blocking and polls a stop flag so the owning
 * thread can always be shut down within a fraction of a second. */
#ifndef AP_WS_H
#define AP_WS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ap_ws ap_ws_t;

/* Create a connection. Returns NULL on failure and writes a readable
 * reason into err (err_len bytes).
 *   use_tls     - 1 for wss://, 0 for ws://
 *   verify_tls  - 1 to require a valid certificate chain
 *   ca_extra    - optional path to an extra PEM file of trusted roots
 *   stop        - flag checked while waiting; nonzero aborts */
ap_ws_t *ap_ws_connect(const char *host, int port, int use_tls, int verify_tls,
                       const char *ca_extra, volatile int *stop,
                       char *err, size_t err_len);

/* Send a text frame. Returns 0 on success, -1 on failure. */
int ap_ws_send_text(ap_ws_t *ws, const char *text, size_t len);

/* Wait up to timeout_ms for a complete text message.
 * Returns 1 and sets *msg / *msg_len (NUL-terminated, owned by ws,
 * valid until the next call) when a message arrived, 0 on timeout,
 * -1 when the connection is closed or broken. Ping/pong and close
 * control frames are handled internally. */
int ap_ws_poll(ap_ws_t *ws, int timeout_ms, char **msg, size_t *msg_len);

/* 1 if the server agreed to compress messages. */
int ap_ws_compressed(ap_ws_t *ws);

void ap_ws_close(ap_ws_t *ws);

#ifdef __cplusplus
}
#endif

#endif
