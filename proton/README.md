# Altspace under Proton / Wine: emoji, room-join and video fixes

These fix the Linux environment the game runs in; they don't change the game's files.
They work with any launcher (Steam, Faugus, Lutris, Heroic, Bottles, umu-run, plain Wine).
Run the scripts from this folder.

**Finding your install:** with no arguments, the scripts look in the usual prefix locations
(`~/.wine`, `$WINEPREFIX`, Steam's `compatdata`, `~/Faugus`, `~/Games`, Bottles) for prefixes
Altspace has already run in. They recognise one by the `AppData/LocalLow/SideQuest/AltspaceVR`
folder Unity creates there, and read the game folder from that prefix's `Player.log`. So start
Altspace once before running them, or pass the paths yourself.

## Emoji show as boxes

Emoji in world UI (for example 🔊 and 🏆 on game buttons) show as empty boxes when
Altspace runs under Proton. They show correctly on Quest and on Windows.

### Fix

```bash
./fix-emoji.sh
```

The script puts a colour emoji font named **Segoe UI Emoji** (`seguiemj.ttf`) in
`C:\Windows\Fonts` of each prefix Altspace has run in. Restart the game afterwards.

- One specific prefix: `./fix-emoji.sh /path/to/prefix` (the folder that holds `drive_c`).
- Remove it again: `./fix-emoji.sh --undo`.
- Game updates don't undo it, because the font lives in the prefix, not in the game
  folder. Run it again for a new prefix.
- Running it again replaces an older copy of this font with the current version.
- Requirements: Python 3 with fontTools (`python-fonttools` / `python3-fonttools`), and
  Firefox or LibreWolf installed (native, Flatpak or Snap), because the font is made from
  their `TwemojiMozilla.ttf`. To use another Twemoji (COLR) font, run
  `EMOJI_FONT=/path/to/font.ttf ./fix-emoji.sh`. The built font is cached as
  `seguiemj-v2.ttf` in this folder.

### Why it happens (for the devs)

- Unity's Windows player (`UnityPlayer.dll`, checked in Altspace 0.5.190) falls
  back to the OS font **"Segoe UI Emoji"** from `C:\Windows\Fonts` for emoji in UI
  Toolkit text. Every Windows install has that font. Android/Quest has its own system
  emoji font.
- Wine/Proton prefixes don't have Segoe UI Emoji, so the fallback finds nothing and
  every emoji draws as a box.
- Unity seems to size each emoji from the font's plain (non-colour) outline for that
  character. Segoe UI Emoji has a full outline there. Twemoji's is empty, because the
  picture is only in the colour layers. A renamed Twemoji therefore drew emoji at about a
  fifth of their size. The script fixes this by filling each empty outline with that
  emoji's layers (as a composite glyph). A bundled emoji font needs real outlines too.
- Altspace also bundles Noto Color Emoji (the CBDT version, as the dynamic font asset
  `NotoColorEmoji`) for its toasts. Under Proton it fails to load any emoji at startup:
  `[Toasts] Host ready (... emoji font 'NotoColorEmoji' Dynamic, prewarm ok=False missing='🎉🎊✨⭐✅🔔⚠️❌📸👋' ...)`.
  Putting Noto Color Emoji in the prefix doesn't change that line. Unity's FreeType may
  not render CBDT (PNG bitmap) glyphs on desktop; worth checking whether toast emoji
  work on real Windows. A COLR-format emoji font (like Twemoji or Segoe UI Emoji) is the
  safer choice for a bundled fallback.
- Possible client-side fix: add the bundled emoji font asset as an emoji fallback for
  the UI Toolkit panels that SDK spaces use, so emoji don't depend on the OS having
  Segoe UI Emoji.

## Joining rooms fails under Proton ("SSL CA certificate error")

Symptom: Altspace loads the space, then sits on the loading screen. `Player.log` shows:

```
Curl error 60: Cert verify failed. Certificate Common Name(CN) does not match with the expected CN. UnityTls error code: 7
[Networking] Error [http_error] http_error: POST https://158.69.117.174/join failed: SSL CA certificate error
```

### Fix

```bash
./fix-ipcert.sh
```

Close Altspace first. For each Altspace install it finds, the script copies
`ipcert-fix/version.dll` next to `AltspaceVR.exe` and adds a Wine DLL override for
`AltspaceVR.exe` only in that prefix's `user.reg` (the first run saves a copy as
`user.reg.bak-ipcert`). Then start Altspace as usual.

- One install: `./fix-ipcert.sh /path/to/game /path/to/prefix` (the game folder holds
  `AltspaceVR.exe`; the prefix folder holds `drive_c`).
- Remove it: `./fix-ipcert.sh --undo` (or `--undo /path/to/game /path/to/prefix`).
- **Run it again after every game update**, because the update replaces the game folder
  and deletes the DLL. Start the new version once first, so its `Player.log` points at it.
  The prefix setting stays.
- The first run builds the DLL from source (`ipcert-fix/version.c`) with
  `ipcert-fix/build.sh`. It needs only `clang`, `lld` and `llvm`, with no Windows SDK or
  mingw gcc.
- Once Proton ships a Wine with the upstream fix (see below), you can undo this.

Tested 28 Oct 2026 with Proton-CachyOS `cachyos-11.0-20261015`: joins to
`158.69.117.174` succeed, and other users show up in the room.

### Why it happens

- The client joins rooms at a bare IP (`POST https://158.69.117.174/join`). That server
  has a Let's Encrypt **IP certificate**: the IP is only in the subjectAltName as an
  `iPAddress` entry, and the subject (and so the CN) is empty.
- Unity's Windows player checks the server name with the Windows API:
  `UnityPlayer.dll` imports `CertVerifyCertificateChainPolicy` from `crypt32` and calls it
  with `CERT_CHAIN_POLICY_SSL`.
- Wine's `crypt32` only compared `dNSName` entries, so it returned `CERT_E_CN_NO_MATCH`
  for every IP certificate. Unity reports that as "UnityTls error code: 7" / curl error 60.
  The certificate chain itself is fine.
- Real Windows and Quest (Android) match IP entries, so they're unaffected.
- **Wine fixed this upstream on 19 Oct 2026**, in commit `563aa5f6fe` ("crypt32: Match
  IP address SANs in SSL hostname check"). It's newer than wine-11.0 and every Proton
  branch, including Proton-CachyOS 11.0 built 15 Oct.

### What the DLL does

`version.dll` passes every real `version.dll` call through to the system copy. When it
loads, it points `UnityPlayer.dll`'s import of `CertVerifyCertificateChainPolicy` at a
wrapper. The wrapper calls the real function first. It only changes the result when
all of these are true:

- the result is `CERT_E_CN_NO_MATCH`, which Wine only reports after the chain has
  already passed;
- the server name is an IPv4 or IPv6 address;
- the leaf certificate's subjectAltName contains that exact IP.

Every other failure (untrusted chain, expired, wrong IP) still fails.

### Suggestions for the devs

- Easiest client/server fix: join through a hostname (for example
  `https://<something>.worldspace.host/join`) with a normal DNS certificate, not a bare IP.
  That works with every TLS stack, including current Proton.
- Proton / Proton-CachyOS: pick up Wine commit `563aa5f6fe`.

## Video files don't play on the world's video player

Symptom: MP4 (and other `https://`) files never start on the world's own video player
(Unity's), while the same links play in a browser. `Player.log` shows:

```
WindowsVideoMedia error 0xc00d36bb while reading https://.../movie.mp4
Context: MFCreateSourceReaderFromURL
Error details: Unexpected error.
```

### Fix

Start Altspace with this environment variable:

```
GIO_EXTRA_MODULES=/usr/lib/x86_64-linux-gnu/gio/modules
```

The path is the one inside the Steam runtime container, not on your system, so it's the same
on every machine. Where to put it:

- **Steam** (Altspace added as a non-Steam game): in the game's launch options,
  `GIO_EXTRA_MODULES=/usr/lib/x86_64-linux-gnu/gio/modules %command%`.
- **umu-run**, or a `.desktop` shortcut (for example one Faugus made): put
  `env GIO_EXTRA_MODULES=/usr/lib/x86_64-linux-gnu/gio/modules` in front of the command, or
  of the `Exec=` line.
- **Lutris, Heroic, Bottles**: add it to the game's environment variables.

This applies when the game runs inside the Steam Linux Runtime, which is what Steam's
Proton and umu-run (and the launchers built on it) use.

Tested 30 Oct 2026 with Proton-CachyOS `cachyos-11.0-20261015` and the steamrt4 runtime
`4.0.20261021`: Library of Congress and MDN MP4s play again in Altspace 0.5.202.

### Why it happens

- Unity's Windows player opens video URLs with Media Foundation
  (`MFCreateSourceReaderFromURL`). Proton serves `https:` URLs with a GStreamer scheme handler,
  which downloads them with `souphttpsrc` (libsoup).
- libsoup needs GLib's TLS backend, the GIO module `libgiognutls.so`. The Steam runtime has it
  in `/usr/lib/x86_64-linux-gnu/gio/modules`.
- The runtime's launcher service (`srt-launcher-service`, pressure-vessel 0.20261007.0) sets
  `GIO_MODULE_DIR=/nonexistent` and `GIO_USE_VFS=local` for itself, and the game inherits them.
  With no GIO modules, libsoup has no TLS: the request gets its response headers, then the
  stream fails ("Internal data stream error"), and Media Foundation returns
  `MF_E_UNEXPECTED` (`0xc00d36bb`).
- Reproduced outside Altspace with a small Media Foundation test program: it plays the MP4
  normally, and fails with exactly `0xc00d36bb` when only `GIO_MODULE_DIR=/nonexistent` is
  added. `GIO_EXTRA_MODULES` is read in addition to `GIO_MODULE_DIR`, and the launcher doesn't
  touch it, so it brings the TLS module back.
- WebM (VP8) files are unaffected because Unity decodes those itself, without
  Media Foundation.

### Suggestions for the devs / upstream

- steam-runtime-tools: the launcher service shouldn't pass its own `GIO_MODULE_DIR` and
  `GIO_USE_VFS` on to the programs it launches.
- Until then, Proton (or umu) could set `GIO_EXTRA_MODULES` for the runtime's GIO module folder.
