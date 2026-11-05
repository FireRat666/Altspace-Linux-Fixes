# Ora browser in Unity Play mode on Linux

Runs the SideQuest Ora browser (`com.sidequest.ora`, used by the Altspace creator SDK) in Unity
Play mode on the **Linux editor**, including when Unity runs inside the Unity Hub **Flatpak**.

For testing your own projects in the editor only: nothing here goes into builds or the game.

Ora only ships a Windows browser (`ora-windows.exe`, an Electron app). This repo doesn't contain any
of Ora's code. `install.sh` takes Ora's own Electron app from your Unity project's package, applies
small Linux patches, and runs it on the official Linux build of the same Electron version.

## Install

From this folder:

```bash
./install.sh ~/path/to/UnityProject
```

Then focus Unity: the editor script compiles, patches `SideQuest.Ora.dll`, and Play mode uses the
Linux browser. Re-run `install.sh` after an Ora package update.

Needs `curl`, `unzip`, `sha256sum`, `node`/`npx`, and a Unity editor in `~/Unity/Hub/Editor`
(or `UNITY_EDITOR=<.../Editor>`). Installs to `~/.local/share/ora-linux` (or `ORA_LINUX_HOME`).

## How it works

| Windows (stock Ora) | Linux (this repo) |
|---|---|
| Unity starts `Plugins/Windows~/ora-windows.exe` | `OraManager.browserFolderOverride` points at `bin/`, whose `ora-windows.exe` is a bash launcher |
| Electron renders to a D3D11 shared texture; `ora-windows.node` copies it into a Windows named memory mapping | Electron renders CPU bitmaps; `linux-frames.js` writes them to `/tmp/ora-frames/<name>`, rows bottom-first |
| Unity: `MemoryMappedFile.OpenExisting(name)` | Patched DLL: `OraLinuxShm.Open` maps `/tmp/ora-frames/<name>` (Mono's named maps are process-local on Linux) |
| Browser set to RealTime priority | Skipped on Linux (needs root; the exception stopped Unity tracking the browser) |
| Page audio captured into Unity (`ora-audio.dll`) | Not available: audio plays straight from the browser, not spatialised |

**Flatpak:** the launcher starts Electron on the host with `flatpak-spawn --host --watch-bus` (the
browser exits when Unity stops it). The sandbox's `/dev/shm` is private, but its `/tmp` is the host's
`$XDG_RUNTIME_DIR/.flatpak/<app id>/tmp` (tmpfs), so frames go there. The launcher also passes the
editor's in-sandbox PID so the creator SDK's local multiplayer hub check recognises the browser as the editor's.

## Layout

- `install.sh`: sets everything up (see the header for details).
- `launcher/ora-windows.sh`: the launcher. Installed as `bin/ora-windows.exe`, padded past 1 MB because
  Ora treats smaller files as Git LFS pointers.
- `app/linux-frames.js`: Linux stand-in for the `ora-windows.node` frame addon.
- `app/apply-patches.js`: exact-text patches to Ora's app; fails loudly if Ora changes those lines.
- `patcher/OraLinuxPatcher.cs`: Cecil patch for `SideQuest.Ora.dll` (the original is kept as
  `SideQuest.Ora.dll.orig~`, which Unity ignores). Bump `LatestMarker` when the patch changes.
- `unity/OraLinuxSetup.cs`: editor script (Linux editor only). Sets the browser folder, re-applies the
  DLL patch when needed. Menu: *Tools > Ora Linux > Re-apply Ora DLL Patch*.

Tested with com.sidequest.ora 0.1.2 (Electron 37.2.0), Unity 6000.3.21f1, CachyOS, Unity Hub Flatpak.
