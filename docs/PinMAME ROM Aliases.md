# PinMAME ROM aliases: table does not start

Oct 3, 2026 · @Sebastien Bock

## Summary

VR Room – The Simpsons (Data East 1990) v1.04 loads, but its backglass stays dark and Start never begins a game. The table script asks PinMAME for the game `simp`, which is a short alias, not a PinMAME game name. On Windows, VPinMAME maps `simp` to `simp_a27` through its built-in alias list; the PinMAME plugin used by VPX standalone reads that list from `pinmame/alias.txt` instead, and the Steam Frame had no such file. The alias list is now installed on the Frame. The table also needs the ROM `simp_a27.zip`, which is not on the device yet.

## The issue

On the Steam Frame, the table loads and coins add credits, but the backglass stays off and Start does nothing, so no ball is served. The credits come from the table script, not from the ROM: the ROM never runs.

The VPX log (`~/.local/share/VPinballX/10.8/vpinball.log`) of the session shows the error:

```
ERROR [VPXPluginAPIImpl::OnScriptError] Script error reported by plugin (Failure): Game name not found: 'simp'.
```

The table script hides it: it sets `.GameName` and calls `.Run` under `On Error Resume Next`, so the table keeps running without a ROM.

```vb
Const cGameName="simp", ...
With Controller
   .GameName = cGameName
   ...
   On Error Resume Next
   .Run GetPlayerHWnd
```

## Root cause

PinMAME's game names for The Simpsons are `simp_a27` (2.7) and its clone `simp_a20` (2.0), in `src/wpc/degames.c`. `simp` is an alias, one of about 120 that VPinMAME defines in `src/win32com/Alias.cpp`, so the same table works on Windows. Table authors often use these older or shorter names.

The PinMAME plugin does not use VPinMAME's built-in list. `Controller::SetGameName` (`plugins/pinmame/Controller.cpp`) calls `PinmameGetGame`, and libpinmame resolves aliases in `CheckGameAlias` (`src/libpinmame/libpinmame.cpp`) by reading `<vpmPath>/alias.txt`, where `vpmPath` is the shared `pinmame` folder in the tables folder. The file holds one `alias,romname` pair per line, with `#` for comments. Without that file, no alias is known and the game name is rejected.

## Fix applied

PinMAME ships its alias list as `release/VPMAlias.txt`, in the format libpinmame reads (`simp,simp_a27` is line 98). It was copied to the Frame as the plugin's alias file:

| File | Content |
| --- | --- |
| `Documents/VPinballX/Tables/pinmame/alias.txt` | Copy of PinMAME's `release/VPMAlias.txt` (the macOS build has it in `external/macos-arm64/Release/pinmame/pinmame/release/`) |

The other file in that folder, `release/alias.txt`, is a text description of the aliases (`simp = simp_a27`), not the format libpinmame parses: do not use it as `alias.txt`.

The ROM is still missing: `pinmame/roms/` on the Frame has no `simp_a27.zip`. Once it is added (web upload or `scp`), the table resolves `simp` to `simp_a27` and starts. Not verified on the device yet, for lack of the ROM.

A check of every table on the Frame (its `cGameName`, resolved through `alias.txt`, against `pinmame/roms/`) found no other PinMAME table blocked by an alias. The other names without a ROM belong to original tables that do not use PinMAME.

## Follow-ups

- [ ] Add `simp_a27.zip` to the Frame and check that The Simpsons starts.
- [ ] Have the release installer (`Doc/steam-frame-build/release/install.sh`) create `pinmame/alias.txt` from `VPMAlias.txt` when it is missing, so that new installs get the aliases.
- [ ] The web upload's list of missing ROMs could resolve aliases too, so that it asks for `simp_a27` rather than `simp`.
