/* Built-in Archipelago client for SMZ3 (Super Metroid & A Link to the Past
 * Crossover) inside the Snes9x libretro core.
 *
 * Two halves:
 *   - A background network thread that owns the WebSocket connection to the
 *     Archipelago server and speaks the Archipelago protocol.
 *   - Game logic that runs on the emulator thread between frames (ap_frame),
 *     a direct port of worlds/smz3/Client.py from Archipelago. It reads and
 *     writes the ROM's message queues in save RAM.
 * They share a small amount of state under a mutex.
 */
#include "ap_client.h"
#include "ap_ws.h"
#include "cJSON.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ---------------------------------------------------------------- */
/* SMZ3 constants (worlds/smz3/Client.py, TotalSMZ3/Item.py, Location.py) */

#define SRAM_START              0xE00000u
#define WRAM_START              0xF50000u
#define SMZ3_ROMNAME_START      0x00FFC0u
#define ROMNAME_SIZE            0x15
#define SMZ3_RECV_PROGRESS_ADDR (SRAM_START + 0x4000u)
#define SMZ3_ITEMS_START_ID     84000
#define SMZ3_LOCATIONS_START_ID 85000
#define SMZ3_ROM_PLAYER_LIMIT   256
#define SMZ3_GAME_NAME          "SMZ3"
#define CLIENT_GOAL             30
#define ITEMS_HANDLING          5 /* 0b101: remote items + starting inventory */

#define TICK_FRAMES        4    /* run game logic every 4 frames (~15/s) */
#define MSG_QUEUE_SIZE     32
#define MSG_TEXT_SIZE      200

/* ---------------------------------------------------------------- */
/* Shared state                                                      */

typedef struct
{
   int64_t item;
   int64_t location;
   int player;
} net_item_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_thread;
static int g_thread_started;
static volatile int g_stop;

static retro_environment_t g_env;
static retro_log_printf_t g_log;
static int g_msg_ext; /* frontend supports SET_MESSAGE_EXT */

static int g_active;
static int g_msg_only; /* inactive, but still show the startup message */
static char g_overlay[MSG_TEXT_SIZE];
static unsigned g_overlay_until;
#define OVERLAY_FRAMES 420 /* ~7 seconds */
static uint8_t g_rom_name[ROMNAME_SIZE];
static int g_new_message_queue;
static char g_cfg_paths[3][1024];
static int g_cfg_count;

/* guarded by g_lock */
static int g_connected;
static net_item_t *g_items;
static size_t g_items_len, g_items_cap;
static char **g_outbox;
static size_t g_outbox_len, g_outbox_cap;
static int64_t *g_checked;
static size_t g_checked_len, g_checked_cap;
static int g_finished;
static char g_msgs[MSG_QUEUE_SIZE][MSG_TEXT_SIZE];
static int g_msg_head, g_msg_count;

/* game-thread only */
static unsigned g_frame;
static unsigned g_last_msg_frame;

/* ---------------------------------------------------------------- */
/* Helpers                                                           */

static void logf_(const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   if (g_log)
      g_log(RETRO_LOG_INFO, "[Archipelago] %s\n", buf);
   else
      fprintf(stderr, "[Archipelago] %s\n", buf);
}

/* Queue an on-screen message (any thread). */
static void post_msg(const char *fmt, ...)
{
   char buf[MSG_TEXT_SIZE];
   va_list ap;
   int idx;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   logf_("%s", buf);

   pthread_mutex_lock(&g_lock);
   if (g_msg_count == MSG_QUEUE_SIZE)
   {
      g_msg_head = (g_msg_head + 1) % MSG_QUEUE_SIZE;
      g_msg_count--;
   }
   idx = (g_msg_head + g_msg_count) % MSG_QUEUE_SIZE;
   memcpy(g_msgs[idx], buf, sizeof(buf));
   g_msg_count++;
   pthread_mutex_unlock(&g_lock);
}

/* Queue a packet for the server. Caller holds g_lock. */
static void outbox_push_locked(const char *json)
{
   char *copy;
   if (g_outbox_len == g_outbox_cap)
   {
      size_t cap = g_outbox_cap ? g_outbox_cap * 2 : 16;
      char **p = (char **)realloc(g_outbox, cap * sizeof(*p));
      if (!p)
         return;
      g_outbox = p;
      g_outbox_cap = cap;
   }
   copy = strdup(json);
   if (copy)
      g_outbox[g_outbox_len++] = copy;
}

static void outbox_clear_locked(void)
{
   size_t i;
   for (i = 0; i < g_outbox_len; i++)
      free(g_outbox[i]);
   g_outbox_len = 0;
}

static void checked_add_locked(int64_t id)
{
   size_t i;
   for (i = 0; i < g_checked_len; i++)
      if (g_checked[i] == id)
         return;
   if (g_checked_len == g_checked_cap)
   {
      size_t cap = g_checked_cap ? g_checked_cap * 2 : 64;
      int64_t *p = (int64_t *)realloc(g_checked, cap * sizeof(*p));
      if (!p)
         return;
      g_checked = p;
      g_checked_cap = cap;
   }
   g_checked[g_checked_len++] = id;
}

static void sleep_ms_interruptible(int ms)
{
   while (ms > 0 && !g_stop)
   {
      int step = ms > 100 ? 100 : ms;
      usleep(step * 1000);
      ms -= step;
   }
}

/* ---------------------------------------------------------------- */
/* Config file                                                       */

typedef struct
{
   char server[256];
   char password[256];
   char ca_file[512];
   int tls_verify;
   int item_messages;
} ap_config_t;

static void trim(char *s)
{
   char *p = s;
   size_t n;
   while (*p && isspace((unsigned char)*p))
      p++;
   if (p != s)
      memmove(s, p, strlen(p) + 1);
   n = strlen(s);
   while (n && isspace((unsigned char)s[n - 1]))
      s[--n] = 0;
}

static const char CONFIG_TEMPLATE[] =
   "# Archipelago settings for the SMZ3 core.\n"
   "# Put your room's address here, exactly as shown on the room page,\n"
   "# for example:  server=archipelago.gg:38281\n"
   "server=\n"
   "# Room password, if it has one.\n"
   "password=\n"
   "# Show sent/received item messages on screen (true/false).\n"
   "item_messages=true\n"
   "# Leave this on unless connecting to a self-hosted server with its own certificate.\n"
   "tls_verify=true\n";

/* Returns 1 if a config file with a server was found. */
static int load_config(ap_config_t *cfg, char *found_path, size_t found_len)
{
   int i;
   memset(cfg, 0, sizeof(*cfg));
   cfg->tls_verify = 1;
   cfg->item_messages = 1;
   found_path[0] = 0;

   for (i = 0; i < g_cfg_count; i++)
   {
      char line[1024];
      FILE *f = fopen(g_cfg_paths[i], "r");
      if (!f)
         continue;
      snprintf(found_path, found_len, "%s", g_cfg_paths[i]);
      while (fgets(line, sizeof(line), f))
      {
         char *eq;
         trim(line);
         if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
         eq = strchr(line, '=');
         if (!eq)
            continue;
         *eq = 0;
         trim(line);
         trim(eq + 1);
         if (!strcmp(line, "server"))
            snprintf(cfg->server, sizeof(cfg->server), "%s", eq + 1);
         else if (!strcmp(line, "password"))
            snprintf(cfg->password, sizeof(cfg->password), "%s", eq + 1);
         else if (!strcmp(line, "ca_file"))
            snprintf(cfg->ca_file, sizeof(cfg->ca_file), "%s", eq + 1);
         else if (!strcmp(line, "tls_verify"))
            cfg->tls_verify = !(eq[1] == 'f' || eq[1] == 'F' || eq[1] == '0' || eq[1] == 'n' || eq[1] == 'N');
         else if (!strcmp(line, "item_messages"))
            cfg->item_messages = !(eq[1] == 'f' || eq[1] == 'F' || eq[1] == '0' || eq[1] == 'n' || eq[1] == 'N');
      }
      fclose(f);
      return cfg->server[0] != 0;
   }

   /* No file anywhere: create a template, preferring the ROM's folder
    * (on Android the system folder is usually hidden from file managers). */
   for (i = 0; i < g_cfg_count; i++)
   {
      FILE *f = fopen(g_cfg_paths[i], "w");
      if (f)
      {
         fputs(CONFIG_TEMPLATE, f);
         fclose(f);
         snprintf(found_path, found_len, "%s", g_cfg_paths[i]);
         break;
      }
   }
   return 0;
}

/* Parse "archipelago.gg:38281", "wss://host:port", "/connect host:port".
 * tls: 1 = wss, 0 = ws, -1 = not specified (try secure first). */
static int parse_server(const char *in, char *host, size_t host_len, int *port, int *tls)
{
   char buf[256];
   char *p, *colon, *slash;
   snprintf(buf, sizeof(buf), "%s", in);
   trim(buf);
   p = buf;
   if (!strncmp(p, "/connect", 8))
   {
      p += 8;
      while (*p == ' ')
         p++;
   }
   *tls = -1;
   if (!strncmp(p, "wss://", 6))
   {
      *tls = 1;
      p += 6;
   }
   else if (!strncmp(p, "ws://", 5))
   {
      *tls = 0;
      p += 5;
   }
   slash = strchr(p, '/');
   if (slash)
      *slash = 0;
   *port = 38281;
   colon = strrchr(p, ':');
   if (colon)
   {
      *colon = 0;
      *port = atoi(colon + 1);
   }
   if (!*p || *port <= 0 || *port > 65535)
      return 0;
   snprintf(host, host_len, "%s", p);
   return 1;
}

/* ---------------------------------------------------------------- */
/* Network thread: protocol                                          */

typedef struct
{
   ap_ws_t *ws;
   ap_config_t cfg;
   cJSON *version;      /* server version object from RoomInfo */
   cJSON *datapackage;  /* data.games from DataPackage */
   cJSON *players;      /* Connected.players */
   cJSON *slot_info;    /* Connected.slot_info */
   int slot;
   int team;
   int refused;
} net_t;

static int send_json(net_t *n, cJSON *arr)
{
   char *s = cJSON_PrintUnformatted(arr);
   int rc = -1;
   if (s)
   {
      rc = ap_ws_send_text(n->ws, s, strlen(s));
      free(s);
   }
   return rc;
}

static void random_uuid(char *out, size_t len)
{
   static const char hex[] = "0123456789abcdef";
   size_t i;
   for (i = 0; i + 1 < len && i < 32; i++)
      out[i] = hex[rand() & 15];
   out[i] = 0;
}

static void send_connect(net_t *n)
{
   static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   char name[64];
   char uuid[40];
   size_t i, o = 0;
   cJSON *arr = cJSON_CreateArray();
   cJSON *c = cJSON_CreateObject();
   cJSON *ver;

   /* The server identifies SMZ3 players by the base64 of the ROM title. */
   for (i = 0; i < ROMNAME_SIZE; i += 3)
   {
      uint32_t v = (uint32_t)g_rom_name[i] << 16;
      if (i + 1 < ROMNAME_SIZE) v |= (uint32_t)g_rom_name[i + 1] << 8;
      if (i + 2 < ROMNAME_SIZE) v |= g_rom_name[i + 2];
      name[o++] = b64[(v >> 18) & 63];
      name[o++] = b64[(v >> 12) & 63];
      name[o++] = i + 1 < ROMNAME_SIZE ? b64[(v >> 6) & 63] : '=';
      name[o++] = i + 2 < ROMNAME_SIZE ? b64[v & 63] : '=';
   }
   name[o] = 0;
   random_uuid(uuid, sizeof(uuid));

   if (n->version)
   {
      ver = cJSON_Duplicate(n->version, 1);
   }
   else
   {
      ver = cJSON_CreateObject();
      cJSON_AddNumberToObject(ver, "major", 0);
      cJSON_AddNumberToObject(ver, "minor", 6);
      cJSON_AddNumberToObject(ver, "build", 8);
      cJSON_AddStringToObject(ver, "class", "Version");
   }

   cJSON_AddStringToObject(c, "cmd", "Connect");
   cJSON_AddStringToObject(c, "password", n->cfg.password);
   cJSON_AddStringToObject(c, "name", name);
   cJSON_AddItemToObject(c, "version", ver);
   cJSON_AddItemToObject(c, "tags", cJSON_CreateArray());
   cJSON_AddNumberToObject(c, "items_handling", ITEMS_HANDLING);
   cJSON_AddStringToObject(c, "uuid", uuid);
   cJSON_AddStringToObject(c, "game", SMZ3_GAME_NAME);
   cJSON_AddBoolToObject(c, "slot_data", 0);
   cJSON_AddItemToArray(arr, c);
   send_json(n, arr);
   cJSON_Delete(arr);
}

static const char *player_name(net_t *n, int slot)
{
   cJSON *p;
   cJSON_ArrayForEach(p, n->players)
   {
      cJSON *t = cJSON_GetObjectItem(p, "team");
      cJSON *s = cJSON_GetObjectItem(p, "slot");
      if (cJSON_IsNumber(t) && cJSON_IsNumber(s) && t->valueint == n->team && s->valueint == slot)
      {
         cJSON *a = cJSON_GetObjectItem(p, "alias");
         if (!cJSON_IsString(a))
            a = cJSON_GetObjectItem(p, "name");
         if (cJSON_IsString(a))
            return a->valuestring;
      }
   }
   return slot == 0 ? "Server" : NULL;
}

static const char *slot_game(net_t *n, int slot)
{
   char key[16];
   cJSON *info, *game;
   snprintf(key, sizeof(key), "%d", slot);
   info = cJSON_GetObjectItem(n->slot_info, key);
   game = cJSON_GetObjectItem(info, "game");
   return cJSON_IsString(game) ? game->valuestring : NULL;
}

static const char *lookup_name(net_t *n, int slot, const char *table, double id)
{
   const char *game = slot_game(n, slot);
   cJSON *g, *t, *e;
   if (!game || !n->datapackage)
      return NULL;
   g = cJSON_GetObjectItem(n->datapackage, game);
   t = cJSON_GetObjectItem(g, table);
   cJSON_ArrayForEach(e, t)
   {
      if (cJSON_IsNumber(e) && e->valuedouble == id)
         return e->string;
   }
   return NULL;
}

static void handle_print_json(net_t *n, cJSON *cmd)
{
   cJSON *type = cJSON_GetObjectItem(cmd, "type");
   cJSON *recv = cJSON_GetObjectItem(cmd, "receiving");
   cJSON *item = cJSON_GetObjectItem(cmd, "item");
   cJSON *owner = cJSON_GetObjectItem(item, "player");
   cJSON *parts = cJSON_GetObjectItem(cmd, "data");
   cJSON *part;
   char out[MSG_TEXT_SIZE];
   size_t len = 0;
   int to_us, from_us;

   if (!n->cfg.item_messages || !cJSON_IsString(type))
      return;
   if (strcmp(type->valuestring, "ItemSend") && strcmp(type->valuestring, "ItemCheat"))
      return;
   if (!cJSON_IsNumber(recv) || !cJSON_IsNumber(owner))
      return;
   to_us = recv->valueint == n->slot;
   from_us = owner->valueint == n->slot;
   if (to_us == from_us)
      return; /* not ours, or found our own item (the game shows those) */

   out[0] = 0;
   cJSON_ArrayForEach(part, parts)
   {
      cJSON *pt = cJSON_GetObjectItem(part, "type");
      cJSON *tx = cJSON_GetObjectItem(part, "text");
      cJSON *pl = cJSON_GetObjectItem(part, "player");
      const char *s = cJSON_IsString(tx) ? tx->valuestring : "";
      const char *ptype = cJSON_IsString(pt) ? pt->valuestring : "text";
      const char *named = NULL;

      if (!strcmp(ptype, "player_id"))
         named = player_name(n, atoi(s));
      else if (!strcmp(ptype, "item_id") && cJSON_IsNumber(pl))
         named = lookup_name(n, pl->valueint, "item_name_to_id", atof(s));
      else if (!strcmp(ptype, "location_id") && cJSON_IsNumber(pl))
         named = lookup_name(n, pl->valueint, "location_name_to_id", atof(s));
      if (named)
         s = named;
      len += (size_t)snprintf(out + len, len < sizeof(out) ? sizeof(out) - len : 0, "%s", s);
      if (len >= sizeof(out))
         break;
   }
   if (out[0])
      post_msg("%s", out);
}

static void handle_command(net_t *n, cJSON *cmd)
{
   cJSON *c = cJSON_GetObjectItem(cmd, "cmd");
   const char *name;
   if (!cJSON_IsString(c))
      return;
   name = c->valuestring;

   if (!strcmp(name, "RoomInfo"))
   {
      cJSON *ver = cJSON_GetObjectItem(cmd, "version");
      cJSON *games = cJSON_GetObjectItem(cmd, "games");
      cJSON *arr, *req;
      if (n->version)
         cJSON_Delete(n->version);
      n->version = ver ? cJSON_Duplicate(ver, 1) : NULL;

      /* Names for on-screen messages. Only the games in this room. */
      if (n->cfg.item_messages && cJSON_IsArray(games))
      {
         arr = cJSON_CreateArray();
         req = cJSON_CreateObject();
         cJSON_AddStringToObject(req, "cmd", "GetDataPackage");
         cJSON_AddItemToObject(req, "games", cJSON_Duplicate(games, 1));
         cJSON_AddItemToArray(arr, req);
         send_json(n, arr);
         cJSON_Delete(arr);
      }
      send_connect(n);
   }
   else if (!strcmp(name, "Connected"))
   {
      cJSON *team = cJSON_GetObjectItem(cmd, "team");
      cJSON *slot = cJSON_GetObjectItem(cmd, "slot");
      const char *me;
      size_t i;
      n->team = cJSON_IsNumber(team) ? team->valueint : 0;
      n->slot = cJSON_IsNumber(slot) ? slot->valueint : 0;
      if (n->players)
         cJSON_Delete(n->players);
      if (n->slot_info)
         cJSON_Delete(n->slot_info);
      n->players = cJSON_Duplicate(cJSON_GetObjectItem(cmd, "players"), 1);
      n->slot_info = cJSON_Duplicate(cJSON_GetObjectItem(cmd, "slot_info"), 1);

      pthread_mutex_lock(&g_lock);
      g_connected = 1;
      outbox_clear_locked();
      /* Re-send anything checked while the connection was shaky. */
      if (g_checked_len)
      {
         cJSON *arr = cJSON_CreateArray();
         cJSON *lc = cJSON_CreateObject();
         cJSON *locs = cJSON_CreateArray();
         char *s;
         for (i = 0; i < g_checked_len; i++)
            cJSON_AddItemToArray(locs, cJSON_CreateNumber((double)g_checked[i]));
         cJSON_AddStringToObject(lc, "cmd", "LocationChecks");
         cJSON_AddItemToObject(lc, "locations", locs);
         cJSON_AddItemToArray(arr, lc);
         s = cJSON_PrintUnformatted(arr);
         if (s)
         {
            outbox_push_locked(s);
            free(s);
         }
         cJSON_Delete(arr);
      }
      if (g_finished)
         outbox_push_locked("[{\"cmd\":\"StatusUpdate\",\"status\":30}]");
      pthread_mutex_unlock(&g_lock);

      me = player_name(n, n->slot);
      post_msg("Archipelago: connected as %s", me ? me : "player");
   }
   else if (!strcmp(name, "ConnectionRefused"))
   {
      cJSON *errs = cJSON_GetObjectItem(cmd, "errors");
      cJSON *e = cJSON_GetArrayItem(errs, 0);
      const char *why = cJSON_IsString(e) ? e->valuestring : "unknown reason";
      if (!strcmp(why, "InvalidSlot") || !strcmp(why, "InvalidGame"))
         post_msg("Archipelago: this ROM doesn't belong to that room");
      else if (!strcmp(why, "InvalidPassword"))
         post_msg("Archipelago: wrong room password");
      else if (!strcmp(why, "IncompatibleVersion"))
         post_msg("Archipelago: server version not supported");
      else
         post_msg("Archipelago: login refused (%s)", why);
      n->refused = 1;
   }
   else if (!strcmp(name, "ReceivedItems"))
   {
      cJSON *index = cJSON_GetObjectItem(cmd, "index");
      cJSON *items = cJSON_GetObjectItem(cmd, "items");
      cJSON *it;
      int idx = cJSON_IsNumber(index) ? index->valueint : -1;
      int resync = 0;

      pthread_mutex_lock(&g_lock);
      if (idx == 0)
         g_items_len = 0;
      if (idx >= 0 && (size_t)idx == g_items_len)
      {
         cJSON_ArrayForEach(it, items)
         {
            cJSON *iv = cJSON_GetObjectItem(it, "item");
            cJSON *lv = cJSON_GetObjectItem(it, "location");
            cJSON *pv = cJSON_GetObjectItem(it, "player");
            if (g_items_len == g_items_cap)
            {
               size_t cap = g_items_cap ? g_items_cap * 2 : 128;
               net_item_t *p = (net_item_t *)realloc(g_items, cap * sizeof(*p));
               if (!p)
                  break;
               g_items = p;
               g_items_cap = cap;
            }
            g_items[g_items_len].item = cJSON_IsNumber(iv) ? (int64_t)iv->valuedouble : 0;
            g_items[g_items_len].location = cJSON_IsNumber(lv) ? (int64_t)lv->valuedouble : 0;
            g_items[g_items_len].player = cJSON_IsNumber(pv) ? pv->valueint : 0;
            g_items_len++;
         }
      }
      else
         resync = 1;
      pthread_mutex_unlock(&g_lock);

      if (resync)
      {
         cJSON *arr = cJSON_CreateArray();
         cJSON *s = cJSON_CreateObject();
         cJSON_AddStringToObject(s, "cmd", "Sync");
         cJSON_AddItemToArray(arr, s);
         send_json(n, arr);
         cJSON_Delete(arr);
      }
   }
   else if (!strcmp(name, "DataPackage"))
   {
      cJSON *data = cJSON_GetObjectItem(cmd, "data");
      cJSON *games = cJSON_GetObjectItem(data, "games");
      cJSON *g;
      if (!n->datapackage)
         n->datapackage = cJSON_CreateObject();
      cJSON_ArrayForEach(g, games)
      {
         cJSON_DeleteItemFromObject(n->datapackage, g->string);
         cJSON_AddItemToObject(n->datapackage, g->string, cJSON_Duplicate(g, 1));
      }
   }
   else if (!strcmp(name, "PrintJSON"))
      handle_print_json(n, cmd);
}

static void handle_message(net_t *n, const char *text)
{
   cJSON *root = cJSON_Parse(text);
   cJSON *cmd;
   if (!root)
      return;
   if (cJSON_IsArray(root))
   {
      cJSON_ArrayForEach(cmd, root)
         handle_command(n, cmd);
   }
   cJSON_Delete(root);
}

static int flush_outbox(net_t *n)
{
   for (;;)
   {
      char *s = NULL;
      int rc;
      pthread_mutex_lock(&g_lock);
      if (g_outbox_len)
      {
         s = g_outbox[0];
         memmove(g_outbox, g_outbox + 1, (g_outbox_len - 1) * sizeof(*g_outbox));
         g_outbox_len--;
      }
      pthread_mutex_unlock(&g_lock);
      if (!s)
         return 0;
      rc = ap_ws_send_text(n->ws, s, strlen(s));
      free(s);
      if (rc != 0)
         return -1;
   }
}

static void net_reset(net_t *n)
{
   if (n->version) cJSON_Delete(n->version);
   if (n->players) cJSON_Delete(n->players);
   if (n->slot_info) cJSON_Delete(n->slot_info);
   n->version = n->players = n->slot_info = NULL;
   n->slot = n->team = 0;
   n->refused = 0;
}

static void *net_thread(void *arg)
{
   net_t n;
   char last_err[160] = "";
   char cfg_path[1024];
   int backoff = 2000;
   int said_setup = 0;
   (void)arg;

   memset(&n, 0, sizeof(n));
   srand((unsigned)time(NULL) ^ (unsigned)(uintptr_t)&n);

   while (!g_stop)
   {
      char host[256];
      int port, tls, attempt;
      char err[160] = "";

      if (!load_config(&n.cfg, cfg_path, sizeof(cfg_path)))
      {
         if (said_setup % 7 == 0)
            post_msg("Archipelago: put your room address in %s",
                     cfg_path[0] ? cfg_path : "archipelago.cfg");
         said_setup++;
         sleep_ms_interruptible(3000);
         continue;
      }
      said_setup = 0;

      if (!parse_server(n.cfg.server, host, sizeof(host), &port, &tls))
      {
         snprintf(err, sizeof(err), "Archipelago: can't read server '%s'", n.cfg.server);
         if (strcmp(err, last_err))
         {
            post_msg("%s", err);
            snprintf(last_err, sizeof(last_err), "%s", err);
         }
         sleep_ms_interruptible(3000);
         continue;
      }

      /* Unspecified scheme: try secure first, then plain (like the official client). */
      for (attempt = 0; attempt < 2 && !n.ws && !g_stop; attempt++)
      {
         int use_tls = tls == -1 ? (attempt == 0) : tls;
         if (tls != -1 && attempt > 0)
            break;
         n.ws = ap_ws_connect(host, port, use_tls, n.cfg.tls_verify,
                              n.cfg.ca_file, &g_stop, err, sizeof(err));
      }
      if (!n.ws)
      {
         char full[200];
         snprintf(full, sizeof(full), "Archipelago: %s - retrying", err);
         if (strcmp(full, last_err))
         {
            post_msg("%s", full);
            snprintf(last_err, sizeof(last_err), "%s", full);
         }
         sleep_ms_interruptible(backoff);
         if (backoff < 15000)
            backoff *= 2;
         continue;
      }
      last_err[0] = 0;
      backoff = 2000;
      logf_("WebSocket open to %s:%d", host, port);

      while (!g_stop && !n.refused)
      {
         char *msg;
         size_t len;
         int r = ap_ws_poll(n.ws, 100, &msg, &len);
         if (r < 0)
            break;
         if (r == 1)
            handle_message(&n, msg);
         if (!n.refused && flush_outbox(&n) != 0)
            break;
      }

      ap_ws_close(n.ws);
      n.ws = NULL;
      pthread_mutex_lock(&g_lock);
      g_connected = 0;
      outbox_clear_locked();
      pthread_mutex_unlock(&g_lock);

      if (g_stop)
         break;
      if (n.refused)
      {
         net_reset(&n);
         sleep_ms_interruptible(20000);
      }
      else
      {
         post_msg("Archipelago: connection lost - reconnecting");
         net_reset(&n);
         sleep_ms_interruptible(2000);
      }
   }

   net_reset(&n);
   if (n.datapackage)
      cJSON_Delete(n.datapackage);
   return NULL;
}

/* ---------------------------------------------------------------- */
/* Game logic (emulator thread) - port of SMZ3SNIClient.game_watcher  */

static int convert_loc_smz3_id_to_ap_id(int value)
{
   return (value >= 256 + 230 && value <= 256 + 236) ? value - 34 : value;
}

static void smz3_tick(void)
{
   uint32_t send_ptr_off = 0x680, send_size = 8, send_msg_byte = 4, send_table_off = 0x700;
   uint32_t recv_ptr_off = 0x600, recv_size = 4, recv_table_off = 0x602;
   uint8_t cur_game[2], mode[1], d[4];
   unsigned recv_index, recv_item, item_out_ptr;
   int connected, finished, is_end = 0;

   pthread_mutex_lock(&g_lock);
   connected = g_connected;
   finished = g_finished;
   pthread_mutex_unlock(&g_lock);
   if (!connected)
      return;

   if (g_new_message_queue)
   {
      send_ptr_off = 0xD3C;
      send_size = 2;
      send_msg_byte = 0;
      send_table_off = 0xDA0;
      recv_ptr_off = 0xD36;
      recv_size = 2;
      recv_table_off = 0xD38;
   }

   /* Goal check: which half of the game is running, then its mode byte. */
   if (ap_mem_read(SRAM_START + 0x33FE, cur_game, 2))
   {
      if (cur_game[0] != 0)
      {
         if (ap_mem_read(WRAM_START + 0x0998, mode, 1))
            is_end = mode[0] == 0x26 || mode[0] == 0x27;
      }
      else if (ap_mem_read(WRAM_START + 0x10, mode, 1))
         is_end = mode[0] == 0x19 || mode[0] == 0x1A;
   }
   if (is_end)
   {
      if (!finished)
      {
         pthread_mutex_lock(&g_lock);
         g_finished = 1;
         outbox_push_locked("[{\"cmd\":\"StatusUpdate\",\"status\":30}]");
         pthread_mutex_unlock(&g_lock);
         post_msg("Archipelago: goal complete!");
      }
      return;
   }

   /* Outgoing: locations the game has checked. */
   if (!ap_mem_read(SMZ3_RECV_PROGRESS_ADDR + send_ptr_off, d, 4))
      return;
   recv_index = d[0] | (d[1] << 8);
   recv_item = d[2] | (d[3] << 8);

   while (recv_index < recv_item)
   {
      uint8_t m[8];
      uint32_t item_address = recv_index * send_size;
      int is_z3, masked, item_index;
      int64_t location_id;
      char json[96];
      uint8_t wb[2];

      if (!ap_mem_read(SMZ3_RECV_PROGRESS_ADDR + send_table_off + item_address, m, send_size))
         return;
      is_z3 = (m[send_msg_byte + 1] & 0x80) != 0;
      masked = is_z3 ? (m[send_msg_byte + 1] & 0x7F) : m[send_msg_byte + 1];
      item_index = ((m[send_msg_byte] | (masked << 8)) >> 3) + (is_z3 ? 256 : 0);

      recv_index++;
      wb[0] = recv_index & 0xFF;
      wb[1] = (recv_index >> 8) & 0xFF;
      ap_mem_write(SMZ3_RECV_PROGRESS_ADDR + send_ptr_off, wb, 2);

      location_id = SMZ3_LOCATIONS_START_ID + convert_loc_smz3_id_to_ap_id(item_index);
      snprintf(json, sizeof(json), "[{\"cmd\":\"LocationChecks\",\"locations\":[%lld]}]",
               (long long)location_id);
      pthread_mutex_lock(&g_lock);
      checked_add_locked(location_id);
      outbox_push_locked(json);
      pthread_mutex_unlock(&g_lock);
      logf_("Checked location %lld", (long long)location_id);
   }

   /* Incoming: give the game the next item it hasn't had yet. */
   if (!ap_mem_read(SMZ3_RECV_PROGRESS_ADDR + recv_ptr_off, d, 4))
      return;
   item_out_ptr = d[2] | (d[3] << 8);

   {
      net_item_t item;
      int have = 0;
      pthread_mutex_lock(&g_lock);
      if (item_out_ptr < g_items_len)
      {
         item = g_items[item_out_ptr];
         have = 1;
      }
      pthread_mutex_unlock(&g_lock);

      if (have)
      {
         int item_id = (int)(item.item - SMZ3_ITEMS_START_ID);
         int player_id = item.player < SMZ3_ROM_PLAYER_LIMIT ? item.player : 0;
         uint8_t wb[4];
         if (g_new_message_queue)
         {
            wb[0] = (uint8_t)player_id;
            wb[1] = (uint8_t)item_id;
            ap_mem_write(SMZ3_RECV_PROGRESS_ADDR + item_out_ptr * recv_size, wb, 2);
         }
         else
         {
            wb[0] = player_id & 0xFF;
            wb[1] = (player_id >> 8) & 0xFF;
            wb[2] = item_id & 0xFF;
            wb[3] = (item_id >> 8) & 0xFF;
            ap_mem_write(SMZ3_RECV_PROGRESS_ADDR + item_out_ptr * recv_size, wb, 4);
         }
         item_out_ptr++;
         wb[0] = item_out_ptr & 0xFF;
         wb[1] = (item_out_ptr >> 8) & 0xFF;
         ap_mem_write(SMZ3_RECV_PROGRESS_ADDR + recv_table_off, wb, 2);
         logf_("Gave item %d from player %d (%u received)", item_id, item.player, item_out_ptr);
      }
   }
}

static void show_messages(void)
{
   char text[MSG_TEXT_SIZE];
   int have = 0;
   unsigned gap = g_msg_count > 3 ? 90 : OVERLAY_FRAMES / 2;

   if (g_last_msg_frame && g_frame - g_last_msg_frame < gap)
      return;
   pthread_mutex_lock(&g_lock);
   if (g_msg_count)
   {
      memcpy(text, g_msgs[g_msg_head], sizeof(text));
      g_msg_head = (g_msg_head + 1) % MSG_QUEUE_SIZE;
      g_msg_count--;
      have = 1;
   }
   pthread_mutex_unlock(&g_lock);
   if (!have)
      return;

   g_last_msg_frame = g_frame;
   /* Draw it on the picture as well: RetroArch's notifications can be off. */
   memcpy(g_overlay, text, sizeof(g_overlay));
   g_overlay_until = g_frame + OVERLAY_FRAMES;
   if (!g_env)
      return;
   if (g_msg_ext)
   {
      struct retro_message_ext m;
      memset(&m, 0, sizeof(m));
      m.msg = text;
      m.duration = 8000;
      m.priority = 1;
      m.level = RETRO_LOG_INFO;
      m.target = RETRO_MESSAGE_TARGET_OSD;
      m.type = RETRO_MESSAGE_TYPE_NOTIFICATION;
      m.progress = -1;
      g_env(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &m);
   }
   else
   {
      struct retro_message m;
      m.msg = text;
      m.frames = 480;
      g_env(RETRO_ENVIRONMENT_SET_MESSAGE, &m);
   }
}

/* ---------------------------------------------------------------- */
/* Public API                                                        */

static void dir_of(const char *path, char *out, size_t len)
{
   char *slash;
   snprintf(out, len, "%s", path ? path : "");
   slash = strrchr(out, '/');
#ifdef _WIN32
   {
      char *bs = strrchr(out, '\\');
      if (bs && (!slash || bs > slash))
         slash = bs;
   }
#endif
   if (slash)
      *slash = 0;
   else
      out[0] = 0;
}

void ap_start(retro_environment_t env, const char *content_path)
{
   struct retro_log_callback logging;
   const char *sys = NULL;
   unsigned ver = 0;
   char dir[1024];

   ap_stop();

   g_frame = 0;
   g_last_msg_frame = 0;
   g_overlay_until = 0;
   pthread_mutex_lock(&g_lock);
   g_msg_head = g_msg_count = 0;
   pthread_mutex_unlock(&g_lock);

   g_env = env;
   g_log = NULL;
   if (env && env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      g_log = logging.log;
   g_msg_ext = env && env(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION, &ver) && ver >= 1;

   /* The title is stamped twice in SMZ3 ROMs; the official client reads the
    * first copy, the second is the fallback. */
   if (!(ap_mem_read(SMZ3_ROMNAME_START, g_rom_name, ROMNAME_SIZE) &&
         memcmp(g_rom_name, "ZSM", 3) == 0) &&
       !(ap_mem_read(0x400000u + SMZ3_ROMNAME_START, g_rom_name, ROMNAME_SIZE) &&
         memcmp(g_rom_name, "ZSM", 3) == 0))
   {
      char shown[ROMNAME_SIZE + 1];
      int k;
      ap_mem_read(SMZ3_ROMNAME_START, g_rom_name, ROMNAME_SIZE);
      for (k = 0; k < ROMNAME_SIZE; k++)
         shown[k] = (g_rom_name[k] >= 0x20 && g_rom_name[k] < 0x7F) ? (char)g_rom_name[k] : '?';
      shown[ROMNAME_SIZE] = 0;
      g_active = 0;
      g_msg_only = 1;
      post_msg("Archipelago: not an SMZ3 multiworld ROM (title \"%s\")", shown);
      return; /* not an SMZ3 Archipelago ROM - stay out of the way */
   }
   g_new_message_queue = g_rom_name[7] >= '0' && g_rom_name[7] <= '9';

   /* Config search order: next to the ROM, then RetroArch's system folder. */
   g_cfg_count = 0;
   if (content_path && content_path[0])
   {
      dir_of(content_path, dir, sizeof(dir));
      if (dir[0])
         snprintf(g_cfg_paths[g_cfg_count++], sizeof(g_cfg_paths[0]), "%s/archipelago.cfg", dir);
   }
   if (env && env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &sys) && sys && sys[0])
      snprintf(g_cfg_paths[g_cfg_count++], sizeof(g_cfg_paths[0]), "%s/archipelago.cfg", sys);
   sys = NULL;
   if (env && env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &sys) && sys && sys[0])
      snprintf(g_cfg_paths[g_cfg_count++], sizeof(g_cfg_paths[0]), "%s/archipelago.cfg", sys);

   pthread_mutex_lock(&g_lock);
   g_connected = 0;
   g_items_len = 0;
   g_checked_len = 0;
   g_finished = 0;
   g_msg_head = g_msg_count = 0;
   outbox_clear_locked();
   pthread_mutex_unlock(&g_lock);
   g_frame = 0;
   g_last_msg_frame = 0;

   logf_("SMZ3 ROM detected (%.21s)", (const char *)g_rom_name);
   post_msg("Archipelago: SMZ3 detected, connecting...");
   g_active = 1;
   g_stop = 0;
   if (pthread_create(&g_thread, NULL, net_thread, NULL) == 0)
      g_thread_started = 1;
   else
      post_msg("Archipelago: could not start network thread");
}

void ap_frame(void)
{
   if (!g_active)
   {
      if (g_msg_only)
      {
         g_frame++;
         show_messages();
      }
      return;
   }
   g_frame++;
   if (g_frame % TICK_FRAMES == 0)
      smz3_tick();
   show_messages();
}

const char *ap_overlay_text(void)
{
   if (!(g_active || g_msg_only) || !g_overlay[0] || g_frame > g_overlay_until)
      return NULL;
   return g_overlay;
}

void ap_stop(void)
{
   g_overlay[0] = 0;
   g_msg_only = 0;
   if (g_thread_started)
   {
      g_stop = 1;
      pthread_join(g_thread, NULL);
      g_thread_started = 0;
   }
   g_active = 0;
   pthread_mutex_lock(&g_lock);
   g_connected = 0;
   outbox_clear_locked();
   pthread_mutex_unlock(&g_lock);
}
