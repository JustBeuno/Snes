/* Minimal WebSocket client over plain TCP or mbedTLS. See ap_ws.h. */
#include "ap_ws.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#ifndef _3DS
#include <netinet/tcp.h>
#endif
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "ap_cacert.h"
#include "ap_platform.h"

#include <zlib.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define AP_WS_CONNECT_TIMEOUT_MS 10000
#define AP_WS_IO_TIMEOUT_MS      15000
#define AP_WS_MAX_MESSAGE        (64u * 1024u * 1024u)

struct ap_ws
{
   int fd;
   int tls;
   volatile int *stop;

   mbedtls_ssl_context ssl;
   mbedtls_ssl_config conf;
   mbedtls_x509_crt ca;
   mbedtls_entropy_context entropy;
   mbedtls_ctr_drbg_context drbg;

   char *msg;       /* assembled message */
   size_t msg_len;
   size_t msg_cap;
   int in_message;  /* a fragmented data message is in progress */

   /* permessage-deflate (RFC 7692), receive side */
   int deflate_on;        /* negotiated with the server */
   int server_no_ctx;     /* server resets its compression per message */
   int msg_compressed;    /* current message has RSV1 set */
   int inf_ready;
   z_stream inf;
   char *plain;           /* decompressed output buffer */
   size_t plain_cap;
};

static int msg_reserve(ap_ws_t *ws, size_t extra);

static long long now_ms(void)
{
   return ap_now_ms();
}

#ifdef _3DS
/* mbedTLS entropy on the 3DS (MBEDTLS_ENTROPY_HARDWARE_ALT): the system's
 * hardware random number service. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen);
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
   static int ps_ready = 0;
   (void)data;
   *olen = 0;
   if (!ps_ready)
   {
      if (R_FAILED(psInit()))
         return -1;
      ps_ready = 1;
   }
   if (R_FAILED(PS_GenerateRandomBytes(output, len)))
      return -1;
   *olen = len;
   return 0;
}

/* MBEDTLS_PLATFORM_MS_TIME_ALT: millisecond clock for mbedTLS. */
mbedtls_ms_time_t mbedtls_ms_time(void);
mbedtls_ms_time_t mbedtls_ms_time(void)
{
   return (mbedtls_ms_time_t)osGetTime();
}
#endif

static int stopped(ap_ws_t *ws)
{
   return ws->stop && *ws->stop;
}

/* ---------------------------------------------------------------- */
/* Raw socket helpers                                                */

static int set_nonblocking(int fd)
{
   int flags = fcntl(fd, F_GETFL, 0);
   if (flags < 0)
      return -1;
   return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int tcp_connect(const char *host, int port, volatile int *stop,
                       char *err, size_t err_len)
{
   struct addrinfo hints, *res = NULL, *ai;
   char port_str[16];
   int fd = -1;
   int rc;

   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_STREAM;
   hints.ai_protocol = IPPROTO_TCP;
   snprintf(port_str, sizeof(port_str), "%d", port);

   rc = getaddrinfo(host, port_str, &hints, &res);
   if (rc != 0 || !res)
   {
      snprintf(err, err_len, "Could not look up %s", host);
      return -1;
   }

   for (ai = res; ai; ai = ai->ai_next)
   {
      long long deadline;
      int one = 1;

      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
         continue;
      set_nonblocking(fd);
#ifdef TCP_NODELAY
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#endif
      (void)one;
#ifdef SO_NOSIGPIPE
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
         break;
      if (errno != EINPROGRESS && errno != EWOULDBLOCK)
      {
         close(fd);
         fd = -1;
         continue;
      }

      deadline = now_ms() + AP_WS_CONNECT_TIMEOUT_MS;
      for (;;)
      {
         struct pollfd p;
         int so_err = 0;
         socklen_t so_len = sizeof(so_err);

         if (stop && *stop)
         {
            close(fd);
            freeaddrinfo(res);
            snprintf(err, err_len, "Stopped");
            return -1;
         }
         p.fd = fd;
         p.events = POLLOUT;
         p.revents = 0;
         rc = poll(&p, 1, 200);
         if (rc == 0)
         {
            if (now_ms() > deadline)
            {
               close(fd);
               fd = -1;
               break;
            }
            continue;
         }
         if (rc < 0 && errno == EINTR)
            continue;
         if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len) < 0 || so_err != 0)
         {
            close(fd);
            fd = -1;
         }
         break;
      }
      if (fd >= 0)
         break;
   }

   freeaddrinfo(res);
   if (fd < 0)
      snprintf(err, err_len, "Could not connect to %s:%d", host, port);
   return fd;
}

/* mbedTLS BIO callbacks on the non-blocking socket */
static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
   int fd = *(int *)ctx;
   ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
   if (n < 0)
   {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
         return MBEDTLS_ERR_SSL_WANT_WRITE;
      return MBEDTLS_ERR_NET_SEND_FAILED;
   }
   return (int)n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len)
{
   int fd = *(int *)ctx;
   ssize_t n = recv(fd, buf, len, 0);
   if (n < 0)
   {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
         return MBEDTLS_ERR_SSL_WANT_READ;
      return MBEDTLS_ERR_NET_RECV_FAILED;
   }
   return (int)n; /* 0 = peer closed */
}

/* Returns bytes (>0), 0 when it would block, -1 on error/EOF.
 * *want_write is set when TLS needs the socket writable to progress. */
static int raw_read(ap_ws_t *ws, unsigned char *buf, size_t len, int *want_write)
{
   *want_write = 0;
   if (ws->tls)
   {
      int r = mbedtls_ssl_read(&ws->ssl, buf, len);
      if (r > 0)
         return r;
      if (r == MBEDTLS_ERR_SSL_WANT_READ)
         return 0;
      if (r == MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         *want_write = 1;
         return 0;
      }
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
      if (r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
         return 0;
#endif
      return -1;
   }
   else
   {
      ssize_t n = recv(ws->fd, buf, len, 0);
      if (n > 0)
         return (int)n;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         return 0;
      return -1;
   }
}

static int raw_write(ap_ws_t *ws, const unsigned char *buf, size_t len, int *want_read)
{
   *want_read = 0;
   if (ws->tls)
   {
      int r = mbedtls_ssl_write(&ws->ssl, buf, len);
      if (r > 0)
         return r;
      if (r == MBEDTLS_ERR_SSL_WANT_WRITE)
         return 0;
      if (r == MBEDTLS_ERR_SSL_WANT_READ)
      {
         *want_read = 1;
         return 0;
      }
      return -1;
   }
   else
   {
      ssize_t n = send(ws->fd, buf, len, MSG_NOSIGNAL);
      if (n > 0)
         return (int)n;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
         return 0;
      return -1;
   }
}

static int tls_pending(ap_ws_t *ws)
{
   return ws->tls && mbedtls_ssl_get_bytes_avail(&ws->ssl) > 0;
}

static int wait_fd(ap_ws_t *ws, short events, int ms)
{
   struct pollfd p;
   int rc;
   p.fd = ws->fd;
   p.events = events;
   p.revents = 0;
   rc = poll(&p, 1, ms);
   if (rc < 0 && errno == EINTR)
      return 0;
   return rc;
}

static int read_exact(ap_ws_t *ws, unsigned char *buf, size_t len)
{
   long long deadline = now_ms() + AP_WS_IO_TIMEOUT_MS;
   size_t got = 0;
   while (got < len)
   {
      int want_write = 0;
      int r = raw_read(ws, buf + got, len - got, &want_write);
      if (r < 0)
         return -1;
      if (r > 0)
      {
         got += (size_t)r;
         continue;
      }
      if (stopped(ws) || now_ms() > deadline)
         return -1;
      wait_fd(ws, want_write ? POLLOUT : POLLIN, 100);
   }
   return 0;
}

static int write_all(ap_ws_t *ws, const unsigned char *buf, size_t len)
{
   long long deadline = now_ms() + AP_WS_IO_TIMEOUT_MS;
   size_t put = 0;
   while (put < len)
   {
      int want_read = 0;
      int r = raw_write(ws, buf + put, len - put, &want_read);
      if (r < 0)
         return -1;
      if (r > 0)
      {
         put += (size_t)r;
         continue;
      }
      if (stopped(ws) || now_ms() > deadline)
         return -1;
      wait_fd(ws, want_read ? POLLIN : POLLOUT, 100);
   }
   return 0;
}

/* ---------------------------------------------------------------- */
/* TLS setup                                                         */

static int tls_setup(ap_ws_t *ws, const char *host, int verify,
                     const char *ca_extra, char *err, size_t err_len)
{
   static int psa_ready = 0;
   long long deadline;
   int r;

   if (!psa_ready)
   {
      if (psa_crypto_init() != PSA_SUCCESS)
      {
         snprintf(err, err_len, "Crypto init failed");
         return -1;
      }
      psa_ready = 1;
   }

   r = mbedtls_x509_crt_parse(&ws->ca, (const unsigned char *)ap_cacert_pem,
                              sizeof(ap_cacert_pem));
   if (r < 0 && verify)
   {
      snprintf(err, err_len, "Could not load root certificates");
      return -1;
   }
   if (ca_extra && ca_extra[0])
      mbedtls_x509_crt_parse_file(&ws->ca, ca_extra);

   r = mbedtls_ssl_config_defaults(&ws->conf, MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
   if (r != 0)
   {
      snprintf(err, err_len, "TLS config failed");
      return -1;
   }
   mbedtls_ssl_conf_authmode(&ws->conf, verify ? MBEDTLS_SSL_VERIFY_REQUIRED
                                               : MBEDTLS_SSL_VERIFY_NONE);
   mbedtls_ssl_conf_ca_chain(&ws->conf, &ws->ca, NULL);
   mbedtls_ssl_conf_rng(&ws->conf, mbedtls_ctr_drbg_random, &ws->drbg);

   if (mbedtls_ssl_setup(&ws->ssl, &ws->conf) != 0 ||
       mbedtls_ssl_set_hostname(&ws->ssl, host) != 0)
   {
      snprintf(err, err_len, "TLS setup failed");
      return -1;
   }
   mbedtls_ssl_set_bio(&ws->ssl, &ws->fd, bio_send, bio_recv, NULL);

   deadline = now_ms() + AP_WS_CONNECT_TIMEOUT_MS;
   for (;;)
   {
      r = mbedtls_ssl_handshake(&ws->ssl);
      if (r == 0)
         break;
      if (r != MBEDTLS_ERR_SSL_WANT_READ && r != MBEDTLS_ERR_SSL_WANT_WRITE)
      {
         uint32_t flags = mbedtls_ssl_get_verify_result(&ws->ssl);
         if (verify && flags != 0 && flags != (uint32_t)-1)
            snprintf(err, err_len, "Server certificate was rejected");
         else
         {
            char buf[96];
            mbedtls_strerror(r, buf, sizeof(buf));
            snprintf(err, err_len, "Secure connection failed (%s)", buf);
         }
         return -1;
      }
      if (stopped(ws) || now_ms() > deadline)
      {
         snprintf(err, err_len, "Secure connection timed out");
         return -1;
      }
      wait_fd(ws, r == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, 100);
   }
   ws->tls = 1;
   return 0;
}

/* ---------------------------------------------------------------- */
/* WebSocket handshake and framing                                   */

static int ws_handshake(ap_ws_t *ws, const char *host, int port,
                        char *err, size_t err_len)
{
   unsigned char key_raw[16];
   unsigned char key_b64[32];
   size_t key_len = 0;
   char req[512];
   char resp[2048];
   size_t resp_len = 0;
   int n;

   mbedtls_ctr_drbg_random(&ws->drbg, key_raw, sizeof(key_raw));
   mbedtls_base64_encode(key_b64, sizeof(key_b64), &key_len, key_raw, sizeof(key_raw));
   key_b64[key_len] = 0;

   n = snprintf(req, sizeof(req),
                "GET / HTTP/1.1\r\n"
                "Host: %s:%d\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: %s\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"
                "User-Agent: snes9x-archipelago\r\n"
                "\r\n",
                host, port, key_b64);
   if (write_all(ws, (const unsigned char *)req, (size_t)n) != 0)
   {
      snprintf(err, err_len, "Could not send handshake");
      return -1;
   }

   /* Read byte by byte so nothing past the headers is consumed. */
   while (resp_len < sizeof(resp) - 1)
   {
      if (read_exact(ws, (unsigned char *)resp + resp_len, 1) != 0)
      {
         snprintf(err, err_len, "No handshake reply from server");
         return -1;
      }
      resp_len++;
      if (resp_len >= 4 && memcmp(resp + resp_len - 4, "\r\n\r\n", 4) == 0)
         break;
   }
   resp[resp_len] = 0;

   if (strncmp(resp, "HTTP/1.1 101", 12) != 0 && strncmp(resp, "HTTP/1.0 101", 12) != 0)
   {
      char *eol = strchr(resp, '\r');
      if (eol)
         *eol = 0;
      snprintf(err, err_len, "Server refused WebSocket (%.60s)", resp);
      return -1;
   }

   /* Did the server accept compression? (header names are case-insensitive) */
   {
      char *p;
      for (p = resp; *p; p++)
         *p = (char)tolower((unsigned char)*p);
      p = strstr(resp, "\r\nsec-websocket-extensions:");
      if (p)
      {
         char *eol = strstr(p + 2, "\r\n");
         if (eol)
            *eol = 0;
         if (strstr(p, "permessage-deflate"))
         {
            ws->deflate_on = 1;
            ws->server_no_ctx = strstr(p, "server_no_context_takeover") != NULL;
         }
      }
   }
   if (ws->deflate_on)
   {
      memset(&ws->inf, 0, sizeof(ws->inf));
      if (inflateInit2(&ws->inf, -15) != Z_OK)
      {
         snprintf(err, err_len, "Compression setup failed");
         return -1;
      }
      ws->inf_ready = 1;
   }
   return 0;
}

/* Decompress the assembled message in ws->msg (RFC 7692). */
static int inflate_message(ap_ws_t *ws)
{
   static const unsigned char tail[4] = {0x00, 0x00, 0xFF, 0xFF};
   size_t out_len = 0;
   int zr;

   if (!ws->inf_ready || msg_reserve(ws, 4) != 0)
      return -1;
   memcpy(ws->msg + ws->msg_len, tail, 4);

   ws->inf.next_in = (Bytef *)ws->msg;
   ws->inf.avail_in = (uInt)(ws->msg_len + 4);
   do
   {
      if (ws->plain_cap - out_len < 4096 + 1)
      {
         size_t cap = ws->plain_cap ? ws->plain_cap * 2 : 16384;
         char *p;
         if (cap > AP_WS_MAX_MESSAGE)
            return -1;
         p = (char *)realloc(ws->plain, cap);
         if (!p)
            return -1;
         ws->plain = p;
         ws->plain_cap = cap;
      }
      ws->inf.next_out = (Bytef *)ws->plain + out_len;
      ws->inf.avail_out = (uInt)(ws->plain_cap - out_len - 1);
      zr = inflate(&ws->inf, Z_SYNC_FLUSH);
      out_len = (size_t)((char *)ws->inf.next_out - ws->plain);
      if (zr != Z_OK && zr != Z_BUF_ERROR && zr != Z_STREAM_END)
         return -1;
   } while (ws->inf.avail_in > 0 || ws->inf.avail_out == 0);

   if (ws->server_no_ctx)
      inflateReset(&ws->inf);

   /* Swap buffers so the caller keeps reading ws->msg. */
   {
      char *t = ws->msg;
      size_t tc = ws->msg_cap;
      ws->msg = ws->plain;
      ws->msg_cap = ws->plain_cap;
      ws->plain = t;
      ws->plain_cap = tc;
   }
   ws->msg_len = out_len;
   ws->msg[out_len] = 0;
   return 0;
}

static int send_frame(ap_ws_t *ws, int opcode, const unsigned char *data, size_t len)
{
   unsigned char head[14];
   unsigned char mask[4];
   size_t hl = 0;
   unsigned char *frame;
   size_t i;
   int rc;

   head[hl++] = (unsigned char)(0x80 | (opcode & 0x0F));
   if (len < 126)
      head[hl++] = (unsigned char)(0x80 | len);
   else if (len <= 0xFFFF)
   {
      head[hl++] = 0x80 | 126;
      head[hl++] = (unsigned char)(len >> 8);
      head[hl++] = (unsigned char)len;
   }
   else
   {
      int s;
      head[hl++] = 0x80 | 127;
      for (s = 56; s >= 0; s -= 8)
         head[hl++] = (unsigned char)((uint64_t)len >> s);
   }
   mbedtls_ctr_drbg_random(&ws->drbg, mask, 4);
   memcpy(head + hl, mask, 4);
   hl += 4;

   frame = (unsigned char *)malloc(hl + len);
   if (!frame)
      return -1;
   memcpy(frame, head, hl);
   for (i = 0; i < len; i++)
      frame[hl + i] = data[i] ^ mask[i & 3];
   rc = write_all(ws, frame, hl + len);
   free(frame);
   return rc;
}

int ap_ws_send_text(ap_ws_t *ws, const char *text, size_t len)
{
   if (!ws)
      return -1;
   return send_frame(ws, 0x1, (const unsigned char *)text, len);
}

static int msg_reserve(ap_ws_t *ws, size_t extra)
{
   size_t need = ws->msg_len + extra + 1;
   if (need > AP_WS_MAX_MESSAGE)
      return -1;
   if (need > ws->msg_cap)
   {
      size_t cap = ws->msg_cap ? ws->msg_cap : 4096;
      char *p;
      while (cap < need)
         cap *= 2;
      p = (char *)realloc(ws->msg, cap);
      if (!p)
         return -1;
      ws->msg = p;
      ws->msg_cap = cap;
   }
   return 0;
}

/* Read one frame. Returns 1 when a full data message is ready,
 * 0 when the frame was handled but no message is complete yet,
 * -1 on error or close. */
static int read_frame(ap_ws_t *ws)
{
   unsigned char h[2];
   unsigned char mask[4] = {0, 0, 0, 0};
   uint64_t len;
   int fin, opcode, masked;

   if (read_exact(ws, h, 2) != 0)
      return -1;
   fin = (h[0] & 0x80) != 0;
   opcode = h[0] & 0x0F;
   masked = (h[1] & 0x80) != 0;
   len = h[1] & 0x7F;
   if (len == 126)
   {
      unsigned char e[2];
      if (read_exact(ws, e, 2) != 0)
         return -1;
      len = ((uint64_t)e[0] << 8) | e[1];
   }
   else if (len == 127)
   {
      unsigned char e[8];
      int i;
      if (read_exact(ws, e, 8) != 0)
         return -1;
      len = 0;
      for (i = 0; i < 8; i++)
         len = (len << 8) | e[i];
   }
   if (masked && read_exact(ws, mask, 4) != 0)
      return -1;

   if (opcode >= 0x8)
   {
      unsigned char ctl[125];
      uint64_t i;
      if (len > sizeof(ctl))
         return -1;
      if (len && read_exact(ws, ctl, (size_t)len) != 0)
         return -1;
      if (masked)
         for (i = 0; i < len; i++)
            ctl[i] ^= mask[i & 3];
      if (opcode == 0x9) /* ping */
         return send_frame(ws, 0xA, ctl, (size_t)len) == 0 ? 0 : -1;
      if (opcode == 0x8) /* close */
      {
         send_frame(ws, 0x8, ctl, len >= 2 ? 2 : 0);
         return -1;
      }
      return 0; /* pong or unknown control */
   }

   if (opcode == 0x1 || opcode == 0x2)
   {
      ws->msg_len = 0;
      ws->in_message = 1;
      ws->msg_compressed = ws->deflate_on && (h[0] & 0x40) != 0;
   }
   else if (opcode != 0x0 || !ws->in_message)
      return -1;

   if (len > AP_WS_MAX_MESSAGE || msg_reserve(ws, (size_t)len) != 0)
      return -1;
   if (len && read_exact(ws, (unsigned char *)ws->msg + ws->msg_len, (size_t)len) != 0)
      return -1;
   if (masked)
   {
      uint64_t i;
      for (i = 0; i < len; i++)
         ws->msg[ws->msg_len + i] ^= mask[i & 3];
   }
   ws->msg_len += (size_t)len;
   ws->msg[ws->msg_len] = 0;

   if (fin)
   {
      ws->in_message = 0;
      if (ws->msg_compressed && inflate_message(ws) != 0)
         return -1;
      return 1;
   }
   return 0;
}

int ap_ws_poll(ap_ws_t *ws, int timeout_ms, char **msg, size_t *msg_len)
{
   int first = 1;
   if (!ws)
      return -1;
   for (;;)
   {
      int r;
      if (!tls_pending(ws))
      {
         int ready = wait_fd(ws, POLLIN, first ? timeout_ms : 0);
         if (ready < 0)
            return -1;
         if (ready == 0)
            return 0;
      }
      first = 0;
      r = read_frame(ws);
      if (r < 0)
         return -1;
      if (r == 1)
      {
         *msg = ws->msg;
         *msg_len = ws->msg_len;
         return 1;
      }
   }
}

static void ws_free(ap_ws_t *ws)
{
   if (!ws)
      return;
   if (ws->fd >= 0)
      close(ws->fd);
   mbedtls_ssl_free(&ws->ssl);
   mbedtls_ssl_config_free(&ws->conf);
   mbedtls_x509_crt_free(&ws->ca);
   mbedtls_ctr_drbg_free(&ws->drbg);
   mbedtls_entropy_free(&ws->entropy);
   if (ws->inf_ready)
      inflateEnd(&ws->inf);
   free(ws->plain);
   free(ws->msg);
   free(ws);
}

int ap_ws_compressed(ap_ws_t *ws)
{
   return ws && ws->deflate_on;
}

void ap_ws_close(ap_ws_t *ws)
{
   if (!ws)
      return;
   if (ws->fd >= 0)
   {
      /* Best-effort close frame; don't wait long. */
      unsigned char code[2] = {0x03, 0xE8}; /* 1000 normal */
      int one_shot_stop = 1;
      volatile int *saved = ws->stop;
      ws->stop = &one_shot_stop;
      send_frame(ws, 0x8, code, 2);
      if (ws->tls)
         mbedtls_ssl_close_notify(&ws->ssl);
      ws->stop = saved;
   }
   ws_free(ws);
}

ap_ws_t *ap_ws_connect(const char *host, int port, int use_tls, int verify_tls,
                       const char *ca_extra, volatile int *stop,
                       char *err, size_t err_len)
{
   static const char pers[] = "snes9x-archipelago";
   ap_ws_t *ws = (ap_ws_t *)calloc(1, sizeof(*ws));
   if (!ws)
   {
      snprintf(err, err_len, "Out of memory");
      return NULL;
   }
   ws->fd = -1;
   ws->stop = stop;
   mbedtls_ssl_init(&ws->ssl);
   mbedtls_ssl_config_init(&ws->conf);
   mbedtls_x509_crt_init(&ws->ca);
   mbedtls_ctr_drbg_init(&ws->drbg);
   mbedtls_entropy_init(&ws->entropy);

   if (mbedtls_ctr_drbg_seed(&ws->drbg, mbedtls_entropy_func, &ws->entropy,
                             (const unsigned char *)pers, sizeof(pers) - 1) != 0)
   {
      snprintf(err, err_len, "Random number setup failed");
      ws_free(ws);
      return NULL;
   }

   ws->fd = tcp_connect(host, port, stop, err, err_len);
   if (ws->fd < 0)
   {
      ws_free(ws);
      return NULL;
   }

   if (use_tls && tls_setup(ws, host, verify_tls, ca_extra, err, err_len) != 0)
   {
      ws_free(ws);
      return NULL;
   }

   if (ws_handshake(ws, host, port, err, err_len) != 0)
   {
      ws_free(ws);
      return NULL;
   }
   return ws;
}
