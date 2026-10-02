# The `steam-frame` branch

This branch of the fork `SPD13/vpinball` prepares Visual Pinball's standalone player for the Valve Steam Frame (SteamOS on ARM64, SteamVR's OpenXR runtime, Vulkan), and adds a table launcher that works from inside the headset. It starts from upstream commit `fd5e18d` (10.8.1 beta).

This document lists everything the branch changes, where, and how far each part has been checked.

## What the player gets

Changes a player sees, in the standalone builds (details in the numbered sections):

- **Table picker** in the in-game menu ("Tables"), which is also the start screen in launcher mode: choose and switch tables without leaving the player (section 3).
- **Lists of tables** as tabs: All, Recent, Newly added, Most played, Favorites, plus a MENU tab for the library and application options. The tabs are large buttons, easy to hit with a mouse or a VR pointer.
- **Thumbnails**: a grid of table images, loaded in the background with a spinner on each tile meanwhile; a list view is also available. A table without an image gets a screenshot when it is closed, and "Replace table image" takes a new one (sections 4 and 8).
- **Favorites**: a star on each thumbnail, or "Add to favorites" in the table's page; the Favorites tab lists them.
- **Pagination**: 12 tables per page, with a pager above and below the list, so large libraries stay fast.
- **Search** as you type (a virtual keyboard in VR), a letter filter and A–Z / Z–A sort in the All tab, and play statistics (added, last played, times played) for the hovered table.
- **Table actions**: play, restart, rename, reset the table settings, delete, and use the table's VR room in the lobby.
- **Lobby** in launcher mode, from which tables are started and to which they return (section 4).
- **Web upload**: add tables and ROMs from a browser on the local network, with a pairing code, missing ROMs listed, and a link to the ROM folder (sections 5 and 8).
- **Missing ROM message** naming the files to add and where (section 8).
- **VR**: the menu on a panel standing in the room, in the table's direction, at an adjustable distance, used with the controllers' pointer; notifications above the menu (sections 7 and 8).
- **No "controller detected" prompt for the VR controllers**: Steam Input also presents the Frame controllers as an Xbox gamepad; in VR, VPX no longer asks to set up a layout for it, since the controllers already work through OpenXR (section 7).
- **Eye-tracked foveated rendering, exclusive to this branch and to the Steam Frame build**: full shading only around the point the eyes look at, following the headset's eye tracker, so native 2160×2160 per eye stays affordable; a "Foveated rendering" level (Off / Low / Medium / High), an "Eye-tracked" switch and a status line in the VR settings page. Not in upstream Visual Pinball, and inert on Windows and macOS, whose builds lack the Vulkan driver support and the bgfx patch (section 9, and `Doc/foveated-rendering.md` for the full technical reference).

## Status

| Part | State |
|---|---|
| Table library, table picker, launcher mode, web upload, shared ROM folder | Built and run on macOS arm64. Checked through automated tests, scripted runs with frame captures of the real application, and `curl` for the web server. On Windows, the table picker was tested by hand (tabs, thumbnail grid, favorite stars, pager arrows, search box); other parts are not yet driven by a person for every feature (see each section). On the Steam Frame (2026-10-01), the lobby, the Wi-Fi upload and uploaded tables were used in the headset. |
| OpenXR, Valve Frame controller profile, Vulkan extension filter, VR menu panel and pointer | **Run in VR on Windows with a PSVR2** (SteamVR 2.17), in the `windows-mingw` build with `ENABLE_XR=ON` (2026-09-27), which is standalone + OpenXR + Vulkan like the Frame build. Linux ARM64: rebuilt with everything on 2026-09-28 and **run in the headset on the Steam Frame on 2026-10-01** after the driver workaround of section 7: OpenXR runtime found, Frame controller profile accepted with its 32 bindings, lobby and tables displayed (details in `Doc/steam-frame-port-plan.md`, section 6). Not yet checked there: launch from the Steam library, pointer and buttons, performance. |
| Eye-tracked foveated rendering (Steam Frame only) | **Works in the headset on the Steam Frame (2026-10-01)**: the sharp zone follows the eyes on both axes, confirmed with the head moving while the eyes stay on an object. Measured gain on a heavy table: ≈ 0.5 ms per frame at High, because on Turnip a density map forces the tiled render path, where the scene pass is geometry-bound (geometry re-processed per bin); rendering the scene pass directly instead takes the frame from 14.0 to 9.1 ms at native 2160×2160 (`TU_AUTOTUNE_ALGO=profiled`/`prefer_sysmem` or `TU_DEBUG=sysmem`). Experiments, trace analysis and the resulting plan (driver autotuner setting at startup, foveation default Off on the Frame, visibility mask drawn LRZ-friendly) in `Doc/foveated-rendering.md`, sections 6.1 and 7. |
| Windows | `windows-mingw` (with or without `ENABLE_XR`): built and used with GCC 16 (MSYS2 UCRT64), Debug. Visual Studio build: built and run in VR in Debug with Vulkan, without the launcher (see "Build variants"). |
| iOS / Android library builds | Not compiled. Shared files they use were changed (`WebServer`, `InGameUIPage`, `VPApp`, `player`); see "Effects on existing builds". |

## Build variants: what contains what

The launcher lives in the in-game menu and is compiled only in *standalone* builds (`__STANDALONE__`).

| Build | Table picker and launcher | Web upload | OpenXR | Foveated rendering |
|---|---|---|---|---|
| macOS | yes | yes | no (no macOS branch in `VRDevice`) | no |
| Linux x64 / aarch64 | yes | yes (RAR/7z when `external.sh` built libarchive) | yes, with `-DENABLE_XR=ON` (new) | **aarch64 only** (Steam Frame: bgfx patch applied by `platforms/linux-aarch64/external.sh`, Turnip driver) |
| windows-mingw | yes | yes | yes, with `-DENABLE_XR=ON` (new, Vulkan only) | no (compiled out: unpatched bgfx; the OpenXR part is inert without the FB/META extensions) |
| Windows, Visual Studio | no | no | yes (as upstream) | no |
| iOS / Android library | hidden (they have a native launcher) | yes (as upstream) | Android as upstream | no |

## 1. Table library

New files: `lib/src/TableLibrary.h`, `lib/src/TableLibrary.cpp`, `tests/test-table-library.cpp`. Added to `VPX_STANDALONE_SOURCES`.

A C++ port of the Android `TableManager.kt` / iOS `TableManager.swift`, keeping `tables.json` in sync with a tables folder. It depends only on the standard library and the bundled `nlohmann/json`; zip, unzip and logging are passed in by the caller, so it can be tested without the application.

- `tables.json` keeps the mobile format exactly: `{"tableCount":N,"tables":[{uuid,name,path,image,createdAt,modifiedAt}]}`, same key order, paths relative to the tables folder. It is written to a temporary file and renamed.
- On load, old absolute paths are converted; entries with a duplicate uuid or path, a missing file, or a path leaving the tables folder are dropped.
- `Rescan`: registers new `.vpx` files (recursively), drops entries whose file is gone, picks up `<table>.png|.jpg`, and imports `.zip`/`.vpxz` bundles found **at the root** of the tables folder, then deletes them, as well as `.rar` (RAR4 and RAR5) and `.7z` ones in the builds that include libarchive (windows-mingw and Linux, when `external.sh` built it; CMake defines `VPX_ARCHIVE_SUPPORT` then). Archives deeper in the tree are never touched, so PinMAME ROM zips stay zipped. A root bundle without a table is left in place and not extracted again during the run. Hidden files, `._*` and `__MACOSX` are ignored. An optional settle time skips files still being copied.
- Bundles are extracted into a hidden `.import` folder inside the tables folder and moved into place, so a large table is never on disk twice. Several tables in one bundle folder are moved once and all registered.
- `Import`, `Delete`, `Rename`, `SetImage`, `ReloadImage`, `ResetIni`, `Export`. Deleting a table that is alone in its folder removes the folder; otherwise it removes the `.vpx` and its same-name `.ini`, `.vbs`, `.directb2s`, `.png`, `.jpg`.
- `RescanAsync` runs scans on a worker thread and never misses a request. The list read by the UI is locked only for the final swap, never during file work. `GetRevision` lets a UI poll for changes.
- Player statistics: `playCount`, `lastPlayedAt`, `favorite`, with `RecordPlay` and `SetFavorite`. They are stored in a separate **`table-stats.json`**, keyed by uuid, because the mobile launchers reject unknown fields in `tables.json`.
- `FuzzyScore`: each word of the query must be found in the text with its letters in order (not necessarily adjacent); words may be in any order; the best alignment is scored, with bonuses for consecutive letters, word starts and the start of the text.

Tests: doctest, 12 cases, 157 assertions. They pass with AddressSanitizer, UBSan and ThreadSanitizer on macOS, and with GCC 14 on Linux ARM64. They are POSIX-only (they call `zip`/`unzip`) and are not wired into `make/vpx-test.vcxproj`. doctest itself is not in the repository.

## 2. In-game menu framework

Files: `src/ui/live/ingameui/InGameUIItem.h`, `InGameUIPage.h/.cpp`, `InGameUI.h`.

- **Tile layout.** An item with `m_tileImage` (a callback returning an ImGui texture) is drawn as a thumbnail with its label underneath. Consecutive tiles flow into 1 to 6 columns depending on the window width. Navigation remains the menu's linear previous/next, so existing inputs work unchanged. Rendering is in `InGameUIPage::RenderTile`.
- **Tile toggle.** Optional icon in the corner of a tile (`m_tileToggleState`, `m_tileToggleAction`, `m_tileToggleIconOn/Off`), hit-tested separately from the tile. Pointer only.
- `InGameUIPage::GetSelectedItem()`; `InGameUI::GetActivePage()` made public; `InGameUI::UsePointerNav()`.
- Custom-rendered items (`InGameUIItem::Type::CustomRender`), which upstream defines but no page used, are now used by the picker.

## 3. Table picker

New files: `src/ui/live/ingameui/TablePickerPage.h/.cpp`. Registered as `tables/picker` in `InGameUI.cpp`; "Tables" entry added at the top of `HomePage.cpp` for standalone builds other than the mobile library.

- **Tabs:** All, Recent (by last played), Newly added (by date added), Most played, Favorites, and MENU at the right end (the library and application options, section 8). They are drawn as padded buttons (`RenderTabs`); only the hovered button is highlighted, not the whole row (new `InGameUIItem::m_customHighlight`), and with buttons the current tab turns green when the row has the focus.
- **Search box** filtering as you type, on every tab. In All, results are ordered by score.
- **Pages** of 12 tables, with a pager above and below the tables. Only the tables of the page get menu items, sub-pages and thumbnails; thumbnails of other pages are released.
- **Grid view** with thumbnails (decoded when visible on worker threads, two at a time, with a spinner on the tile meanwhile, uploaded to the GPU at most one per frame, downscaled to 512 px) and a **favorite star** on each thumbnail, or **list view**.
- In the All tab: letter filter (from 9 tables up) and A–Z / Z–A sort.
- Hovering or selecting a table shows when it was added, when it was last played and how many times.
- Per-table page: Play / Restart, Add to / Remove from favorites, Rename, Reset table settings (if an `.ini` exists), Delete with confirmation. Reset and Delete are not offered for the running table. Actions fire once per press (the stock menu repeats an action every frame while a button is held).
- `TextEntryPage`: text entry with four buttons (left/right pick a character, then "Add"), used for rename and for search when there is no keyboard.
- Library section: view mode, sort, rescan, tables folder, ROM folder hint, and in launcher mode "Settings" (opens the menu's home page) and "Quit Visual Pinball".
- Inputs: with a mouse or pointer, tabs, pager arrows, stars and the search box are clicked directly. With buttons, left/right on the tab row, a pager or the letter filter changes them, activating the search row opens the text entry page, and favorites are toggled from the table's page.

New settings (`src/core/Settings_properties.inl`): `Standalone/TablesPath` (empty = `VPinballX/Tables` in the user's documents folder), `Standalone/TablePickerGridView`, `Standalone/TablePickerSortAscending`. The current tab, search text and page are kept for the session only.

The library is owned by `VPApp::GetTableLibrary()` (`src/core/VPApp.h/.cpp`). It uses a dedicated sub folder because upstream's default tables location on desktop is the whole documents folder, which must not be scanned, served by the web server, or have its zips consumed.

Tested by hand on Windows (2026-09-28) and working: the tabs, the grid with its thumbnails, clicking a star, the pager arrows and the search box.

## 4. Launcher mode and lobby

Files: `src/core/AppCommands.h/.cpp`, `src/core/main.cpp`, `src/core/VPApp.h/.cpp`, `src/core/player.h/.cpp`, `src/input/InputManager.cpp`.

- New command line option **`-Launcher`** (`LauncherCommand`). The application starts on a lobby with the picker opened; selecting a table closes the lobby and plays it **in the same process**; leaving a table returns to the lobby; quitting from the lobby or with "Quit Visual Pinball" ends the application.
- `-Play <table>` is unchanged, plus: the picker can switch table from the menu (`VPApp::m_nextTableFilename`, loop in `PlayTableCommand::Execute`).
- A play is recorded in the library when a library table starts (`PlayTableCommand::Play`).
- **Lobby.** `BuildLobby()` loads upstream's `src/assets/blankTable.vpx` and, in memory, replaces the script with an empty one, removes every part, adds an invisible `playfield_mesh`, sets a title and description, switches the views to camera mode, and adds a 12 × 12 m floor made of 36 tiles in two shades. The floor is in a part group using the room space (`SR_ROOM`). It is made of tiles because, outside VR, the near clipping plane is derived from the bounding-box corners of each part, which clips a single large quad away entirely. A table file authored in the editor can replace this by changing `VPApp::GetLobbyTablePath()` and removing the `BuildLobby()` call.
- **Table image on close.** When a library table without an image is closed, a screenshot of the playfield window is saved as `<table>.jpg` (`Player::CaptureTableImageBeforeClosing`, triggered by `CS_CLOSE_CAPTURE_SCREENSHOT`, which the quit action now uses in launcher mode). This mirrors what the mobile library does. If the in-game menu is open (quitting from it, or from the missing ROM message), the capture waits for it to slide out; while closing, `LiveUI::RenderUI` keeps rendering the menu until it is gone but no other overlay, so none ends up in the image. There is no timeout: if a capture never completed, the table would not close.

## 5. Web upload on desktop

Files: `lib/src/WebServer.h/.cpp`, `src/assets/web/app.js`, `src/assets/web/vpx.html`, `make/CMakeLists_sources.txt`, `make/CMakeLists_app.txt`.

The web server and its page are upstream's. Changes:

- `WebServer`, `ZipUtils` and mongoose moved to a new `VPX_WEBSERVER_SOURCES` list, compiled into the macOS and Linux executables (`VPX_TABLE_WEBSERVER` compile definition, `zip` linked, `libzip.so*` copied on Linux). `VPApp::GetWebServer()` owns the instance.
- No longer depends on the mobile library: events go through two functions that raise the mobile events or, on desktop, ask the table library to rescan.
- On desktop it only exposes the table library folder, and any change made from the browser triggers a rescan.
- **Pairing code** (desktop): API routes answer 401 until the browser has posted the 6-digit code shown in the picker to `/pair`; it then holds a random session token in an HttpOnly cookie. Five wrong codes replace the code. Codes and tokens are dropped when the server stops. `SetPairingRequired()` controls it; the mobile builds keep it off. The page asks for the code when it receives a 401.
- Never started automatically on desktop: the picker has "Wi-Fi upload: On/Off", shows the address and the code, and the server is stopped when a table is launched and when the application exits.
- **Staged uploads** (all builds): chunks are written in a hidden `.upload` folder and the file is moved into place when complete. Empty files are created directly.
- **"Upload Folder"** entry in the page's menu (`uploadFolder()`), using the browser's folder picker; skips hidden files. Dropping a folder on the page already worked.
- Uploading `VPinballX.ini` to switch the settings file is limited to the mobile builds.

Known gaps: plain HTTP; the static page and `/assets/*` are served without pairing. Not verified: the pairing prompt and the Upload Folder entry in a real browser.

## 6. Shared ROM folder

`VPApp::SetupSharedPinMAMEFolder()` (`src/core/VPApp.cpp`), called before each table loads because the PinMAME plugin reads its setting when it is loaded.

The plugin looks for ROMs in `pinmame/roms` next to the table, then in its `PinMAMEPath` setting, then in `~/.pinmame`. When the setting is empty (or equal to the default below) and `~/.pinmame/roms` does not exist, the setting is pointed at `<tables folder>/pinmame` and `pinmame/roms` is created, so ROMs can be uploaded from the browser. The value is reset when the application closes, but it can still be written to `VPinballX.ini` if a settings page saves while it is applied. Desktop standalone builds only.

## 7. OpenXR

Files: `CMakeLists.txt`, `make/CMakeLists_app.txt`, `platforms/linux-x64/external.sh`, `platforms/linux-aarch64/external.sh`, `src/renderer/VRDevice.h/.cpp`, `src/renderer/XRVulkanBackend.h`, `src/input/XRInputHandler.h`, `src/ui/live/LiveUI.h/.cpp`.

- **Linux.** `ENABLE_XR` is offered for `PLATFORM=linux` (BGFX renderer only); the Linux dependency scripts build the Khronos OpenXR loader and install its headers; `VRDevice` selects the Vulkan renderer on Linux and shares Android's timespec timing code. As on Android, the Wine headers make bgfx report the Windows platform, which is corrected for Linux too. `XRVulkanBackend` loads `libvulkan.so.1` at run time.
- **Vulkan instance extensions.** The backend now requests only the extensions the driver reports. Upstream always requests `VK_EXT_debug_utils` and `VK_EXT_debug_report`, which fails instance creation on drivers without them. This applies to all platforms.
- **Valve Frame controller.** `XR_VALVE_frame_controller_interaction` is enabled when available and bindings are suggested for `/interaction_profiles/valve/frame_controller_valve`. New inputs appended to the handler's list (indices 32 to 42, existing indices unchanged): right X/Y, left d-pad, left view, both bumpers, both aim poses. Added to the default mapping: view = open menu, right menu = quit table, d-pad = menu navigation, bumpers = nudge. Users with an existing VR mapping will be offered the new layout once.
- **VR menu panel.** The in-game menu is drawn on a panel fixed in the room (`VRDevice::UpdateUIPanel`), placed in front of the player, 10 cm below the eyes, when the menu opens. It faces the table's forward direction (the scene orientation set by centering the table), not where the head looks, so the player turns to the table, or moves, to read it. It is 2.8 m wide, so a closer panel looks bigger.
- **Menu distance.** New setting `Standalone/VRMenuDistance`: distance between the player and the panel, 0.3 m to 3 m, default 0.8 m. In VR, the MENU tab of the picker shows "Menu distance" with − and + buttons that move the panel by 10 cm and place it again at once. The buttons are used with the pointer; flipper-button navigation does not change them.
- **VR pointer.** `VRDevice::UpdateUIPanel` intersects a controller's aim ray with the panel; `LiveUI::NewFrame` feeds the hit point to ImGui as the mouse position, with the trigger as the left button (press at 0.7, release at 0.4), draws the ray and a dot, and scrolls with the thumbstick. The right hand is preferred.
- **Windows standalone.** `ENABLE_XR` is also offered for `PLATFORM=windows-mingw`; standalone builds only use the Vulkan OpenXR backend.
- **VR by default on standalone OpenXR builds.** `PlayerVR/AskToTurnOn` defaults to "Autodetect" when `__STANDALONE__` and `ENABLE_XR` are both defined (Steam Frame, windows-mingw with `ENABLE_XR`); it stays "Enabled" on Android and "Disabled" elsewhere. Without it the Frame ran the launcher in a flat window.
- **No layout prompt for the virtual gamepad (2026-10-01, not yet run in the headset).** Steam Input also exposes the VR controllers as a virtual Xbox gamepad, so every table started with the "new controller detected" dialog, which had to be dismissed although the controllers were already mapped through OpenXR. `SDLInputHandler::OnJoystickAdded` now flags a joystick as a virtual gamepad of the VR controllers (`InputManager::SetDeviceIsVRVirtualGamepad`) when its vendor is Valve (`0x28de`) or its name contains "Steam" or "X-Box 360 pad" (the name of Steam's uinput pad on Linux), and logs every joystick's vendor and product ids. In VR (`STEREO_VR`), `InputManager` skips the layout proposal for flagged devices ("Skipping layout proposal for … virtual gamepad" in the log); outside VR they are still proposed. Side effect: a real wired Xbox 360 pad on Linux has the same name, so in VR it is not proposed either (it can still be mapped by hand in the input settings). To check on the device: that the dialog is gone, and the ids logged for the virtual pad, to match it exactly if needed.
- **Steam Frame Vulkan driver workaround (Linux ARM64 only).** `platforms/linux-aarch64/bgfx-turnip-descriptor-pool.patch`, applied to bgfx by `platforms/linux-aarch64/external.sh`, creates bgfx's descriptor pools without `VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT` (bgfx never frees single sets, it resets whole pools every frame, so the flag was unused). With the flag, Turnip (Mesa 26.3.0-devel on the Frame, 2026-09-30) allocates sets from a free-list heap and places them partly outside the pool's memory: the first `vkUpdateDescriptorSets` of every run crashed inside the driver, and once that was padded around, every sampled texture read as black (the GPU was reading descriptors from unmapped memory), which showed as an empty black view in the headset and a black desktop window. Found with bgfx's own examples on the device: geometry and gradients rendered, every texture did not. The dependency script's cache key carries a `-turnip2` suffix so existing dependency builds rebuild bgfx. The change is harmless on other drivers and could go to bgfx upstream.
- Left as upstream: the desktop preview window created in VR.

## 8. Added on Windows with a PSVR2 (2026-09-26 to 28)

Used in the headset in the `windows-mingw` build with `ENABLE_XR`. Not compiled for Linux yet.

- **Upstream VR fixes:** the eye resolution was 0x0 (`SetupHMD` read the bgfx caps before bgfx was initialized); no bgfx backend fallback with a device created by OpenXR (it crashed); computing the environment irradiance on the GPU and uploading the textures of a table's texture cache submitted frames before the headset requested one (assertions in Debug). The in-game UI ignores the scale of the desktop preview window, which squeezed it to a few pixels.
- **Menu panel and pointer** (section 7): `Standalone/VRMenuDistance`; the ImGui vertex shader has per eye matrices (`vs_imgui.sc`, `bgfx_imgui.h` regenerated).
- **Table picker:** MENU tab (the options formerly below the table list); virtual keyboard for the search field in VR; the menu distance buttons.
- **Table images:** "Replace table image" in the table's menu (`Player::ReplaceTableImage`). Captures wait for the menu to be closed. In VR, `VRDevice::SetTableCaptureView` replaces the eye poses for the capture frames with the view chosen by `Standalone/TableImageFocus`: Backglass (default; framed on the bounding vertices of the parts named, or primitives textured with an image named, "backglass", otherwise on an estimate), Table (playfield from above) or Cabinet. The target is centred for each eye's asymmetric field of view.
- **Missing ROMs:** plugin API `ShowMessage` (a message page the player acknowledges, `MessagePage`, lines starting with `!` shown in red) and `ReportMissingFile`, used by the PinMAME plugin when a ROM cannot start. The library records them in `missing-roms.json` (preferences folder); the web page shows them, grouped by table, when browsing `pinmame/roms`, with a Clear button; an upload of a file with the same name removes the entry (`/missing-roms` route). "Back to the table picker" on the message used to hang when the table had no image (the capture waited for a menu that was no longer rendered, see section 4); fixed.
- **Link to the ROM folder** on the web page: "PinMAME ROMs (pinmame/roms)" above the file list opens the shared ROM folder, which gets lost in a long list of tables; hidden when already there.
- **RAR and 7z** imports and web extraction through libarchive (`ZipUtils::Extract`, `IsExtractable`, `GetExtractableExtensions`), see section 1.
- **Lobby room from a table:** "Use this VR room in the lobby" in a table's menu (`VPApp::UseTableRoomInLobby`, `lobby-room.json`): the lobby is loaded from that table and keeps only the visible parts of its room (room space part groups, layers named after a room, collections named `VR…` that are not the cabinet). "Use the default lobby room" in the MENU tab. The lobby does not use the image or texture cache of that table, and the table itself still shows Play.
- **Notifications** in VR are stacked just above the menu window (`NotificationOverlay`).
- **Web server on Windows** (network address through `GetAdaptersAddresses`, `std::filesystem` listing on all platforms, UTF-8 paths), and an upload progress banner on the web page.

## 9. Eye-tracked foveated rendering (branch `foveated-rendering`, 2026-10-01)

Files: `src/core/Settings_properties.inl`, `src/core/main.cpp`, `src/renderer/VRDevice.h/.cpp`, `src/renderer/XRVulkanBackend.h`, `src/renderer/XRGraphicBackend.h`, `src/renderer/RenderTarget.h/.cpp`, `src/renderer/Renderer.h/.cpp`, `src/renderer/RenderPass.cpp`, `src/renderer/RenderDevice.h/.cpp`, `src/ui/live/ingameui/VRSettingsPage.cpp`, `platforms/linux-aarch64/bgfx-fragment-density-map.patch`, `platforms/linux-aarch64/external.sh`.

Full shading only around the point the eyes look at, through a Vulkan fragment density map on the scene buffer. **Exclusive to this branch, and only built for the Steam Frame** (Linux aarch64 package: the bgfx patch is applied there alone, and the rest is under `#ifdef` on its macros, so Windows and macOS builds compile it out). Evaluation and plan in `Doc/foveated-rendering-plan.md`, full technical reference (mechanism, every layer of the implementation, verification, measurements, open performance analysis) in `Doc/foveated-rendering.md`; everything below was built and checked on the Steam Frame.

- **Settings** (`PlayerVR`): `Foveation` (Off / Low / Medium / High, default Medium on standalone OpenXR builds, Off elsewhere), `FoveationEyeTracked` (default on), and the sign conventions of the gaze offsets `FoveationFlipX` (off) / `FoveationFlipY` (on), which were settled on the device. The VR settings page has the first two and a status line ("Eye-tracked", "Fixed: eye tracking not active (enable it in the headset settings)", "Not supported by the runtime", …). Changes apply live.
- **Runtime side** (`VRDevice`): the color swapchain is created with `XrSwapchainCreateInfoFoveationFB`, a profile (`XrFoveationLevelProfileCreateInfoFB` + `XrFoveationEyeTrackedProfileCreateInfoMETA`) is applied with `xrUpdateSwapchainFB`, the eye-tracked state and gaze come from `xrGetFoveationEyeTrackedStateMETA`. On the Frame this is what turns the eye tracking on (SteamVR/OpenXR 2.17.10; the user must have calibrated eye tracking in the headset settings). The runtime also hands density maps for its swapchain images (`XrSwapchainImageFoveationVulkanFB`, 68×68, one layer per eye), wrapped as bgfx textures by `XRVulkanBackend::CreateFoveationTextures`; they are kept as a fallback only, see below.
- **Why the app builds its own map**: the gaze is applied through fragment density map *offsets* (`VK_EXT_fragment_density_map_offset`, passed at the end of the render pass), and Turnip only honors offsets on density map images created with `VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT` (`tu_enable_fdm_offset` in `tu_cmd_buffer.cc`), which the runtime's maps are not. So `VRDevice::CreateOwnFoveationMap` makes a 68×68 (eye size / 32, the driver's minimum texel), two-layer RG8 map, filled by `FillOwnFoveationMap` with flat rings per level (full density radius 0.45 / 0.30 / 0.20 of the half width, half density up to 0.80 / 0.55 / 0.38, quarter beyond), and every frame the gaze (normalized, y up) becomes pixel offsets (`x · W/2`, `−y · H/2`) for both eyes.
- **Vulkan device** (`XRVulkanBackend.h`): enables `VK_EXT_fragment_density_map` (+ feature), `VK_VALVE_fragment_density_map_layered` (one map layer per eye on our layered scene buffer, Valve's extension, in upstream Mesa), `VK_EXT_fragment_density_map_offset` and `VK_KHR_create_renderpass2`, all when the driver has them; logs "Fragment density maps: supported, one layer per eye, with offsets".
- **bgfx patch** `bgfx-fragment-density-map.patch` (Linux only, applied after the descriptor pool one, cache tag `-turnip3`): `BGFX_RESOLVE_FRAGMENT_DENSITY_MAP` tags a frame buffer attachment as the density map (kept out of the color/depth lists, `VkRenderPassFragmentDensityMapCreateInfoEXT` on the v1 render pass, extra attachment in the `VkFramebuffer`, layout barriers); `BGFX_TEXTURE_FRAGMENT_DENSITY_MAP` for textures that are maps (usage, offset-capable, kept in the density map layout; external images are taken as is); `VkPipelineFragmentDensityMapLayeredCreateInfoVALVE` on pipelines rendering into a layered map; render targets created offset-capable; `bgfx::setFragmentDensityMapOffsets(frameBuffer, offsetsXY, layers)` stored per frame and applied with `vkCmdEndRenderPass2KHR` (offsets rounded to the device granularity); `BGFX_CAPS_FRAGMENT_DENSITY_MAP`. Unpatched bgfx does not define these macros, so all the VPX code is under `#ifdef` and inert on Windows and macOS.
- **Scene buffer** (`RenderTarget::SetFragmentDensityMap/Offsets`, `Renderer::SetFragmentDensityMap/Offsets`): `BackBuffer1` and `BackBuffer2` get a cached frame buffer variant carrying the map; `VRDevice::RenderFrame` selects it right after acquiring the swapchain image and sets the offsets of the frame. Post passes sample the resolved full-resolution image unchanged (non-subsampled image), so bloom, AA, tonemap and the preview need nothing.
- **Measurement**: the runtime's counter `/perfmetrics_meta/app/gpu_frametime` (`XR_META_performance_metrics`) and, with `VPX_GPU_PROFILE=1` in the environment, bgfx's per-view GPU timings grouped by render target (`VRDevice::LogRuntimeStatus`, every 5 s, needs the view names, which release builds now set when profiling — `RenderDevice::m_nameViews`).
- **What the measurements showed (2026-10-02)**, on the heavy Ghostbusters VR-room table at native 2160×2160: foveation High saves ≈ 0.5 ms of a 14 ms frame. Turnip only applies density maps in its tiled render path and forces any pass with a map into it, and in that path the scene pass is geometry-bound (the geometry is reprocessed per tile: 48 tiles, ≈ 6.5 ms of per-tile draws against 4.7 ms for the whole pass rendered directly). Rendering the scene pass directly takes the frame from 14.0 to 9.1 ms. Hence three changes: **`src/core/main.cpp` sets `TU_AUTOTUNE_ALGO=profiled`** in the environment of the Linux standalone build before the Vulkan driver loads (the driver then measures tiled against direct per render pass and keeps the faster; a value set by the user wins), **foveation defaults to Off** (kept for fragment-bound tables and other drivers), and the **VR visibility mask is drawn with `Z_LESSEQUAL`** instead of `Z_ALWAYS` (same result, its shader writes the near plane; an always-pass depth write made the driver disable LRZ, its hierarchical depth rejection, for the rest of the pass) **and first**: it was submitted with a sort key that put it after every opaque part (z = 200000 instead of a depth bias of +200000, the key being `depthBias − z`), so it had never masked anything before the opaque draws; a depth-only draw placed first also keeps LRZ writes enabled on Turnip. Full account in `docs/Foveated Rendering on the Steam Frame.md` (the article: method, every result, the driver's trace and code) and `Doc/foveated-rendering.md`, sections 6.1 and 7.

## 10. Controller models (2026-10-01, not yet run in the headset)

Files: `src/renderer/VRControllerModels.h/.cpp` (new), `third-party/include/cgltf/cgltf.h` (new, cgltf 1.15, MIT, copied from bgfx's `3rdparty`; `third-party/` is ignored by git, so it needs `git add -f`), `src/renderer/VRDevice.h/.cpp`, `src/renderer/Renderer.h/.cpp`, `src/core/Settings_properties.inl`, `src/ui/live/ingameui/VRSettingsPage.cpp`, `make/CMakeLists_sources.txt`, `make/vpx-core.vcxitems(.filters)`.

The controllers the player holds are drawn in the scene as the headset system shows them, with their buttons, triggers and thumbsticks moving, so the button layout can be seen in VR. The meshes come from the runtime, not from files of ours (Valve's recommendation for the Steam Frame), so any headset whose runtime has the extensions gets its own controllers.

- **Runtime side** (`VRDevice`): enables `XR_EXT_render_model`, `XR_EXT_interaction_render_model` and `XR_EXT_uuid` (needed as the instance asks for OpenXR 1.0) when the runtime has them; the log says "OpenXR controller models: supported by the runtime". `UpdateControllerModels` asks for the models of the devices held (`xrEnumerateInteractionRenderModelIdsEXT`, again on `XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT`, on interaction profile changes, and every 2 s while the list is empty or an asset was unavailable), creates each model, its space and its asset, and keeps the glTF binary and the names of its animatable nodes ("OpenXR controller model N loaded: … KB, … animatable nodes"). Only `KHR_mesh_quantization` is declared as a supported glTF extension. Every frame, `LocateControllerModels` locates each model space in the reference space and reads the node poses (`xrGetRenderModelStateEXT`) at the predicted display time. Both run on the render thread before the frame is requested, like the menu panel; the logic thread reads the result while preparing the frame.
- **Drawing** (`VRControllerModels`, owned by `Renderer`, called at the end of `RenderDynamics` in VR, outside reflection passes): the asset is parsed with cgltf once per model (triangle meshes, base color factor and texture, alpha mask and blend, node hierarchy; PNG/JPEG images through `BaseTexture::CreateFromData`), then each node is drawn with the basic shader in the room space reference: reference space to room VPU is `VRDevice::GetReferenceToRoom()`. An animated node takes the pose given by the runtime as its local transform (relative to its parent), keeping its own scale. Materials: metallic-roughness factors to a VPX material (the metallic-roughness texture is ignored, a mesh which has one is shaded as a non metal), half lit / half unlit (`controllerUnlitPart`) so the controllers stay readable on dark tables, both sides drawn (the room space is mirrored compared to glTF).
- **Setting** `PlayerVR/ShowControllers` (default on), in the VR settings page, applied live. The models are also hidden during the table image capture.
- **To check on the device**: that SteamVR lists the Frame controllers once they are bound, their placement against the real controllers, the direction of the texture coordinates, that the animated parts move the right way (the pose is taken as relative to the parent node), and the cost.

## Effects on existing builds

- `-Play`, the editor and the Visual Studio build behave as before, except for the OpenXR changes that apply everywhere: the Vulkan extension filter, the extra controller inputs and default mappings, the menu panel and pointer, and the upstream VR fixes of section 8.
- Standalone desktop builds get a "Tables" entry at the top of the in-game menu. When a table starts they create `VPinballX/Tables/pinmame/roms` in the documents folder, unless a PinMAME folder is already defined or `~/.pinmame/roms` exists (section 6).
- iOS/Android library builds: uploads are staged in `.upload`; `InGameUIPage` has the tile code; no other intended change.
- The Visual Studio project files (`make/*.vcxproj`) were not updated. None of the new files is needed by a non-standalone build; the CMake build lists them.

## Files

New:

```
lib/src/TableLibrary.h
lib/src/TableLibrary.cpp
src/ui/live/ingameui/TablePickerPage.h
src/ui/live/ingameui/TablePickerPage.cpp
src/ui/live/ingameui/MessagePage.h
src/ui/live/ingameui/MessagePage.cpp
tests/test-table-library.cpp
src/renderer/VRControllerModels.h
src/renderer/VRControllerModels.cpp
third-party/include/cgltf/cgltf.h
docs/Steam Frame Branch.md
```

Modified:

```
README.md                               notice about this fork
CMakeLists.txt                          ENABLE_XR option for Linux
make/CMakeLists_sources.txt             new sources, VPX_WEBSERVER_SOURCES
make/CMakeLists_app.txt                 web server, zip, ENABLE_XR and loader on macOS/Linux
platforms/linux-aarch64/external.sh     OpenXR loader, xz + libarchive, bgfx patch for the Frame's driver
platforms/linux-aarch64/bgfx-turnip-descriptor-pool.patch   descriptor pools without the free-set flag (section 7)
platforms/linux-aarch64/bgfx-fragment-density-map.patch     fragment density map attachments and offsets (section 9)
platforms/linux-x64/external.sh         OpenXR loader
lib/src/WebServer.h, WebServer.cpp      desktop use, pairing, staged uploads
src/assets/web/app.js, vpx.html         pairing prompt, Upload Folder, missing ROMs, ROM folder link
src/assets/web/styles.css               missing ROMs, ROM folder link, upload banner
src/core/AppCommands.h, AppCommands.cpp -Launcher, table switching, lobby
src/core/main.cpp                       launcher counts as play mode
src/core/VPApp.h, VPApp.cpp             table library, web server, ROM folder, lobby path
src/core/Settings_properties.inl        Standalone settings, VR autodetect default on standalone OpenXR builds
src/core/player.h, player.cpp           table image on close
src/input/InputManager.h, .cpp          quit action captures the image in launcher mode, no layout prompt for the VR virtual gamepad
src/input/SDLInputHandler.h             VR virtual gamepad detection, joystick vendor/product ids logged
src/input/XRInputHandler.h              Frame controller, aim poses, analog read
src/renderer/VRDevice.h, VRDevice.cpp   Linux, Frame extension, pointer, controller models
src/renderer/Renderer.h, Renderer.cpp   controller models drawn in VR
src/renderer/XRVulkanBackend.h          run-time libvulkan on Linux, extension filter
src/ui/live/LiveUI.h, LiveUI.cpp        pointer fed to ImGui, menu rendered while closing for the table image
src/ui/live/ingameui/HomePage.cpp       "Tables" entry
src/ui/live/ingameui/InGameUI.h, .cpp   page registration, accessors
src/ui/live/ingameui/InGameUIItem.h     tile image and toggle
src/ui/live/ingameui/InGameUIPage.h, .cpp  tile layout
```

## Working on this branch

- The repository mixes LF and CRLF files and has no `.gitattributes`. On Windows, set `git config --global core.autocrlf false` before cloning, and use editors and tools that preserve line endings.
- User data is never part of the repository: tables, ROMs, `VPinballX.ini`, `tables.json` and `table-stats.json` live in the user's documents and preferences folders. Keep it that way: check `git status` before committing, and never add `.vpx` tables other than upstream's own assets, `.directb2s`, ROM zips or `.ini` files.
