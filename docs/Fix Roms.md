# Fix ROMs

Oct 3, 2026 · @Sebastien Bock

PinMAME tables that load but cannot be played, and how to fix them. Each fix has its own document with the full investigation.

| Symptom | Cause | Document |
| --- | --- | --- |
| Coins are accepted but the display stays at CREDITS 0 (Gottlieb System 3: Street Fighter II, Super Mario Bros, Stargate) | The ROM's settings in a new NVRAM are all zero, so every price is 0 | [Gottlieb System 3 No Credits](Gottlieb%20System%203%20No%20Credits.md) |
| Backglass stays dark, Start does nothing, no ball (The Simpsons) | The table asks for an alias (`simp`) that PinMAME only knows through `pinmame/alias.txt`, which was missing | [PinMAME ROM Aliases](PinMAME%20ROM%20Aliases.md) |

## First checks

1. Read the VPX log (`~/.local/share/VPinballX/10.8/vpinball.log` on the Steam Frame). `Game name not found: '<name>'` means an unknown ROM name: see the aliases document.
2. Check that the ROM is in the shared `pinmame/roms/` folder of the tables folder, under the name the table asks for (`cGameName` in the script), or under the name its alias points to.
3. If the ROM runs but no credits are added, look at the game's settings in its service menu: see the Gottlieb System 3 document.

## Shared folder

All tables use one `pinmame` folder inside the tables folder (`Documents/VPinballX/Tables/pinmame/` on the Frame):

| Path | Content |
| --- | --- |
| `roms/` | ROM zip files |
| `nvram/` | Saved game state and settings (`<rom>.nv`), written when a table is closed |
| `cfg/` | PinMAME settings per ROM |
| `alias.txt` | ROM aliases, one `alias,romname` per line (copy of PinMAME's `VPMAlias.txt`) |

Close the table (the lobby is fine) before replacing a file in `nvram/`: PinMAME writes the NVRAM of the running game when it stops, which would overwrite the new file.
