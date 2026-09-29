# Gottlieb System 3: no credits after inserting coins

Sep 28, 2026 · @Sebastien Bock

## Summary

Street Fighter II (`sfight2`) stays at CREDITS 0 whatever coins are inserted because its game settings in the NVRAM are all zero, so every price is 0. The coins reach the ROM and are counted; VPX, the PinMAME plugin and the table script work correctly. Loading the factory settings from the ROM's service menu fixes it: two coins then give 1 credit. Super Mario Bros (`smb`), another Gottlieb System 3 game, has the same problem.

## The issue

Inserting a coin in JP's Street Fighter 2 VR Room 1.1 makes the display show "GAME OVER – CREDITS 0", and the credit count never goes up. Everything between the coin key and the ROM was checked and works:

| Checked | Result |
| --- | --- |
| VPX log of the sessions | No script or plugin error; ROM `sfight2` loads and runs |
| Table script and `gts3.vbs` | Coin keys pulse switches 0 to 3 for about 40 ms, as with VPinMAME |
| PinMAME plugin switch mapping | Switch 0 is published and mapped (offset of 16 for negative cabinet switches) |
| PinMAME keyboard handling | Off, so the keyboard input ports do not overwrite the coin switches |
| DIP switches | System 3 has none; the table's `Dip()` settings have no effect |

To rule out VPX entirely, a small test program ran `sfight2` directly through `pinmame64.dll`, on a copy of the ROM and NVRAM, and printed the DMD as text:

- A coin switch press is seen by the ROM: attract mode switches to the GAME OVER screen, and the coin counters in the NVRAM (bytes 26 to 32) change, which they don't in runs without coins.
- No credit is added on any of the 4 coin switches, with pulses from 20 to 400 ms, with 6 coins in a row, or with the coin door switch (6) closed.
- Deleting the NVRAM does not help: a new one gives the same result.
- The same test on `smb` (Super Mario Bros) also gives CREDITS 0 after 4 coins.

## Root cause

The ROM's adjustments (prices, replay levels, credit settings) live in its NVRAM, and in this NVRAM they are all zero. When PinMAME creates the NVRAM of a Gottlieb System 3 game, it fills the 8 KB of battery-backed RAM with zeros (`NVRAM_HANDLER(gts3)` in `src/wpc/gts3.c`). The ROM does not check that memory: it takes the zeros as its settings instead of loading its factory defaults, as it would after the random contents of real RAM.

Three observations confirm it:

- In the service menu, "Game adjustment 1: first replay level" reads 00; after loading the factory settings it reads 80,000,000.
- With an NVRAM filled with 0xFF instead of zeros, the display reads "CREDITS FF": the ROM shows the raw memory as its credit count.
- After loading the factory settings, 2 coins give CREDITS 1.

VPinMAME uses the same PinMAME code, so this is not specific to VPX standalone or to this branch. Any System 3 game started without a saved NVRAM is affected until its factory settings are loaded.

## Manual solution

Load the factory settings once from the ROM's service menu, while playing the table in VPX. The keys below are the VPX defaults; on VR controllers, the Self Test action (key 7) may need to be mapped first.

1. Start the table and wait for the attract mode.
2. Press **7** (Self Test) twice: the service menu lists Bookkeeping, Game Adjustments, Self Test and Utilities, with Self Test selected.
3. Press the **left flipper** three times to move down to Game Adjustments (the list wraps around).
4. Press the **right flipper** to select it. The screen reads "Credit button to load factory settings".
5. Press **Start** (the credit button). The screen reads "Credit button to load English"; the right flipper changes the language.
6. Press **Start** again: the factory settings load, and Game adjustment 1 shows a first replay level of 80,000,000.
7. Leave the table. PinMAME saves the NVRAM, and coins add credits from the next start.

In the adjustment screens, the flippers change the value shown, so avoid pressing them after step 6.

## Fix applied

The installed `sfight2.nv` is the previous NVRAM with the factory settings loaded by the ROM itself, through the same menu steps as the manual solution, run by a test program instead of a player. The ROM and all other files are unchanged.

How it was produced:

1. A test program (`test.cpp`, built with MinGW against `pinmame64.dll` from the VPX build) runs a ROM with a copy of the `pinmame` folder, waits 20 s for the boot, presses switches in sequence and prints the DMD as text.
2. Starting from a copy of the previous `sfight2.nv`, it pressed: −8 (Self Test), −8, 143, 143, 143 (left flipper, down to Game Adjustments), 141 (right flipper, select), 4 (Start: load factory settings), 4 (Start: English). Each press lasts 100 ms, followed by 1.2 s.
3. The display then showed Game adjustment 1 at 80,000,000, and stopping the emulation saved the NVRAM.
4. Verification: a new session started from that NVRAM, 2 coins on switch 1, and the display read CREDITS 1. The same check passed again after installing it.

Where it is:

| File | Content |
| --- | --- |
| `Documents/VPinballX/Tables/pinmame/nvram/sfight2.nv` | Fixed NVRAM, factory settings, English |
| `Documents/VPinballX/Tables/pinmame/nvram/sfight2.nv.bak-2026-09-28-zero-settings` | The previous NVRAM, to restore by renaming it |

PinMAME's System 3 switch numbers used by the test: coins 0 to 3, Start 4, coin door 6, diagnostic −8, flipper buttons 141 (right) and 143 (left).

## Follow-ups

- [x] Super Mario Bros (`smb.nv`) had the same zero settings, fixed the same way on 2026-09-28 (previous file kept as `smb.nv.bak-2026-09-28-zero-settings`). Its older menu is shorter: one press of 7 shows "Test mode", one left flipper press reaches "Credit button to load factory settings", then Start twice. The first replay level becomes 60,000,000; checked by inserting 2 coins and pressing Start, which starts a game (Start alone does not).
- [ ] Other Gottlieb System 3 tables added later will start with zero settings too.
- [ ] Launcher idea, not implemented: detect a System 3 ROM whose adjustments are still zero and run the factory reset on its first start, or warn the player.
