# Snes9x with built-in Archipelago (SMZ3)

A copy of the Snes9x RetroArch core with an Archipelago client built into it,
for **SMZ3 (Super Metroid & A Link to the Past Crossover)**. No second app,
no SNI, no computer: load your patched ROM in RetroArch and it connects to
the room on its own.

Any other SNES game runs exactly like normal Snes9x. The Archipelago part
only switches on when it sees an SMZ3 multiworld ROM.

## Install (Android)

1. On your phone, open the latest release of this repository and download
   `snes9x_libretro_android.so`.
2. In RetroArch: **Main Menu > Load Core > Install or Restore a Core**, and
   pick the downloaded file. It replaces the **Snes9x** core and keeps
   working for every SNES game.
3. Force-stop RetroArch (Android Settings > Apps > RetroArch > Force stop)
   and reopen it. **Quick Menu > Information > Core Information** should
   show a version ending in `AP-`.

Launch SMZ3 with **Snes9x** (not Snes9x 2005 or 2010). If a playlist or
History entry keeps opening another core, use its **Set Core Association**.
Don't update Snes9x with the Core Downloader; that puts the stock core back.

The `snes9x_archipelago_*` files are the same core under its own name. RetroArch
on Android only offers cores it has a core-info file for, so that name is
mostly useful on desktop.

## Install (3DS)

The 3DS can't load cores as separate libraries; each core is a full
RetroArch program. The release has one built with this core.

- **Homebrew Launcher RetroArch:** copy `snes9x_libretro.3dsx` to
  `sd:/retroarch/cores/`, replacing the existing file.
- **Installed (CIA) RetroArch:** install `snes9x_libretro.cia` with FBI. It
  replaces the installed Snes9x core.

Turn on Wi-Fi on the 3DS. The settings file works the same way as on
Android (next to your ROM on the SD card), and is easy to edit with the SD
card in a computer. Expect a couple of seconds of slowdown while it
connects: the secure handshake is heavy work for the 3DS processor.

## Each new room

1. Patch your ROM from the `.apsmz3` file as usual (the Archipelago
   website's patch page, or the desktop Archipelago launcher).
2. Load the patched ROM with the **snes9x_archipelago** core.
3. The first time, an on-screen message tells you where it made the
   settings file, `archipelago.cfg`: the same folder as your ROM. (If that
   folder isn't writable it falls back to RetroArch's system folder.)
   If you see "not an SMZ3 multiworld ROM" instead, the ROM isn't a patched
   SMZ3 multiworld ROM.
4. Open `archipelago.cfg` in any text editor and fill in the room address
   from the room page, for example:

   ```
   server=archipelago.gg:38281
   password=
   ```

5. Save it. Within a few seconds the game shows
   **"Archipelago: connected as <your name>"**. You never type a slot name;
   the ROM itself identifies you.

When a room's port changes, edit the `server=` line. The core re-reads the
file every time it reconnects, so there's no need to restart the game.

## Good to know

- **Sleeping rooms.** archipelago.gg rooms go to sleep when nobody is
  connected. If the core keeps saying it can't connect, open the room's page
  in a browser to wake it up; the core will connect by itself.
- **Messages.** Items you send and receive also show as RetroArch
  notifications. Set `item_messages=false` to hide them and keep only the
  connection messages.
- **Nothing gets lost or doubled.** Progress is kept in the game's own save
  data, so closing RetroArch, reloading, rewinding, run-ahead or a dropped
  connection won't duplicate items or lose checks. Anything found while
  offline is sent once the connection is back.
- **Self-hosted servers** with their own certificate: add
  `ca_file=/path/to/cert.pem`, or as a last resort `tls_verify=false`.
- RetroArch's log (Settings > Logging) shows `[Archipelago]` lines if
  something needs troubleshooting.

## How it works

| File | Role |
| --- | --- |
| `libretro/ap/ap_client.c` | Protocol, settings file, and the SMZ3 game logic, ported line for line from Archipelago's `worlds/smz3/Client.py` |
| `libretro/ap/ap_ws.c` | WebSocket client over TCP/TLS, running on a background thread |
| `libretro/ap/ap_glue.cpp` | Reads and writes Snes9x's ROM, save RAM and work RAM |
| `libretro/ap/mbedtls`, `cJSON.c`, `ap_cacert.h` | TLS library, JSON parser, and the Mozilla root certificates used to verify archipelago.gg |
| `libretro/libretro.cpp` | Three hooks: start after the ROM loads, one tick after each frame, stop on unload |

The game logic runs between frames on the emulator thread, so it never races
the game. Networking runs on its own thread, so a slow connection never
stutters the game.

### Tests

`tests/archipelago/` holds a harness that runs the real client code against
simulated SMZ3 memory, plus `e2e.py`, which drives it against a real
Archipelago server with a second scripted player. It checks login from the
ROM title, receiving items, sending checks, restart and reconnect behavior,
goal reporting, and the TLS path.
