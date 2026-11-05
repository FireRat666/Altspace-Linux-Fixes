# Altspace-Linux-Compat

Fixes for running Altspace on Linux, and for testing your own Unity spaces on the
Linux Unity editor.

## What these are, and what they aren't

These are **Linux compatibility fixes, not mods**. None of them change the game's files, its
servers, or what other players see. Each one supplies something that Windows has and Linux
(Wine/Proton, or the Linux Unity editor) is missing, so the unchanged Windows build behaves on Linux
the way it already does on Windows.

| Fix | Where it applies | What it changes |
|---|---|---|
| [Emoji](proton/README.md#emoji-show-as-boxes) | Your Wine/Proton prefix | Adds a stand-in for Windows' *Segoe UI Emoji* font, which Wine doesn't ship. An environment fix: the game is untouched. |
| [Room joins / IP certificates](proton/README.md#joining-rooms-fails-under-proton-ssl-ca-certificate-error) | Your Wine/Proton setup | Backports a Wine bug fix that is already in upstream Wine (commit `563aa5f6fe`) but not yet in Proton releases. A small `version.dll` beside the game makes Wine's certificate name check accept IP-address certificates the way Windows does; every other check is unchanged. Undo it once Proton ships the fix. |
| [Video files](proton/README.md#video-files-dont-play-on-the-worlds-video-player) | Your launch command | One environment variable that restores the Steam runtime's TLS module for Proton's media streaming. |
| [Ora browser in Play mode](unity-ora-play-mode/README.md) | The Unity **editor**, on your own machine | Lets the in-world browser run during Play mode on the Linux editor, for testing your own local projects. It patches the local package copy in your project's `Library/PackageCache`; nothing goes into builds or the game. |

## Layout

- [`proton/`](proton/): fixes for the game running under Proton or Wine, with any launcher
  (`fix-emoji.sh`, `fix-ipcert.sh`, and the video launch option).
- [`unity-ora-play-mode/`](unity-ora-play-mode/): the Linux backend for the
  `com.sidequest.ora` browser in Unity Play mode (`install.sh <unity project>`).

Each folder's README has the symptoms, the fix, how to undo it, and why the problem happens.
