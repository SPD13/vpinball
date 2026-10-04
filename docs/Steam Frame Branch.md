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
- **Web upload**: add tables and ROMs from a browser on the local network, with a pairing code that can be switched off, missing ROMs listed, and a link to the ROM folder. It can be on for the session only or always on (started with the application), and stays on while switching between the lobby and the tables (sections 5 and 8).
- **Tables page in the browser**, the home page of the web server: the library as a grid of thumbnails with the picker's tabs and search, a star to add or remove a favorite, and a "..." menu on each table to set its **display name** (the name the picker shows, the files are not renamed) or **delete** it with all its files. With an empty library it links to the file manager, which stays one click away (section 5).
- **Missing ROM message** naming the files to add and where (section 8).
- **VR**: the menu on a panel standing in the room, in the table's direction, at an adjustable distance, used with the controllers' pointer; notifications above the menu (sections 7 and 8).
- **No "controller detected" prompt for the VR controllers**: Steam Input also presents the Frame controllers as an Xbox gamepad; in VR, VPX no longer asks to set up a layout for it, since the controllers already work through OpenXR (section 7).
- **Eye-tracked foveated rendering, exclusive to this branch and to the Steam Frame build**: full shading only around the point the eyes look at, following the headset's eye tracker, through a fragment shading rate image that the Frame's driver applies in its fast direct render path: 2–4 ms per frame on heavy tables at native 2160×2160 per eye, on by default (Medium); a "Foveated rendering" level (Off / Low / Medium / High), an "Eye-tracked" switch and a status line in the VR settings page. Not in upstream Visual Pinball, and inert on Windows and macOS, whose builds lack the Vulkan driver support and the bgfx patches (section 9, and `Doc/foveated-rendering.md` for the full technical reference).
- **Steadier chrome, smoother edges**: specular anti-aliasing in the material shader stops the shiny parts from sparkling when the image is resampled every frame, on every platform; the headset builds also default to Standard FXAA, the only anti-aliasing within the Frame's budget at native resolution (section 12, `docs/Image Quality on the Steam Frame.md`).
- **Scores and leaderboards**: the score of every game played on a library table is recorded automatically, for the player chosen in the lobby ("Player: <name>"), with a result screen (rank, personal best, table record) when the lobby comes back, a Scores tab with the leaderboard of each table, the 10 best scores in each table's page, and a Scores page in the browser to filter, reassign, delete or clear scores (with confirmation) and manage the players (section 14, `docs/Table Scores and Leaderboards.md`).
- **Dynamic resolution in the headset**: the rendering resolution follows the GPU time of the frames, between a minimum and the table's resolution, so heavy tables and a hot headset keep their frame rate instead of judder on head movements, and light tables keep the full sharpness; a switch, a target and a minimum in the VR settings page, with a status line (section 13).

## Status

| Part | State |
|---|---|
| Table library, table picker, launcher mode, web upload, shared ROM folder | Built and run on macOS arm64. Checked through automated tests, scripted runs with frame captures of the real application, and `curl` for the web server. On Windows, the table picker was tested by hand (tabs, thumbnail grid, favorite stars, pager arrows, search box); other parts are not yet driven by a person for every feature (see each section). On the Steam Frame (2026-10-01), the lobby, the Wi-Fi upload and uploaded tables were used in the headset. The tables page of the web server (2026-10-03) was run on macOS, its routes checked with `curl` and the page driven in Chrome; it is deployed on the Frame but not yet used there. |
| OpenXR, Valve Frame controller profile, Vulkan extension filter, VR menu panel and pointer | **Run in VR on Windows with a PSVR2** (SteamVR 2.17), in the `windows-mingw` build with `ENABLE_XR=ON` (2026-09-27), which is standalone + OpenXR + Vulkan like the Frame build. Linux ARM64: rebuilt with everything on 2026-09-28 and **run in the headset on the Steam Frame on 2026-10-01** after the driver workaround of section 7: OpenXR runtime found, Frame controller profile accepted with its 32 bindings, lobby and tables displayed (details in `Doc/steam-frame-port-plan.md`, section 6). Not yet checked there: launch from the Steam library, pointer and buttons, performance. |
| Eye-tracked foveated rendering (Steam Frame only) | **Works in the headset on the Steam Frame (2026-10-01)**: the sharp zone follows the eyes on both axes, confirmed with the head moving while the eyes stay on an object. With a density map the gain was ≈ 0.5 ms per frame at High, because on Turnip a density map forces the tiled render path, where the scene pass is geometry-bound (geometry re-processed per bin); rendering the scene pass directly instead takes the frame from 14.0 to 9.1 ms at native 2160×2160 (`TU_AUTOTUNE_ALGO=profiled`). **Since 2026-10-03 the scene is foveated through a fragment shading rate image that follows the gaze ray, which the driver applies in the direct path: Addams Family 14.3 → 11.2 ms at High, 11.3 at Medium (the default); Ghostbusters 11.3 → 9.2 with a fixed image**, measurements and mechanism in `Doc/foveated-rendering.md`, sections 7 and 10. |
| Scores and leaderboards (branch `leaderboards`, 2026-10-03/04) | Built and run on macOS arm64: capture checked with a test table simulating each source and with a ROM table, lobby pages from captures of the application, web page and routes with `curl` and Chrome. A coverage run over the whole library is in progress. Not yet run on the Steam Frame. |
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
- `Import`, `Delete`, `Rename`, `SetImage`, `ReloadImage`, `ResetIni`, `Export`. `Rename` sets the display name (the `name` of `tables.json`, files untouched); an empty or blank name gives back the default one, `GetDefaultName`: the file name without extension, underscores replaced by spaces, as given to a new table. Deleting a table that is alone in its folder removes the folder; otherwise it removes the `.vpx` and its same-name `.ini`, `.vbs`, `.directb2s`, `.png`, `.jpg`.
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

Files: `lib/src/WebServer.h/.cpp`, `src/assets/web/app.js`, `src/assets/web/vpx.html`, `src/assets/web/tables.html`, `src/assets/web/tables.js`, `src/assets/web/styles.css`, `make/CMakeLists_sources.txt`, `make/CMakeLists_app.txt`.

The web server and its page are upstream's. Changes:

- `WebServer`, `ZipUtils` and mongoose moved to a new `VPX_WEBSERVER_SOURCES` list, compiled into the macOS and Linux executables (`VPX_TABLE_WEBSERVER` compile definition, `zip` linked, `libzip.so*` copied on Linux). `VPApp::GetWebServer()` owns the instance.
- No longer depends on the mobile library: events go through two functions that raise the mobile events or, on desktop, ask the table library to rescan.
- On desktop it only exposes the table library folder, and any change made from the browser triggers a rescan.
- **Pairing code** (desktop): API routes answer 401 until the browser has posted the 6-digit code shown in the picker to `/pair`; it then holds a random session token in an HttpOnly cookie. Five wrong codes replace the code. Codes and tokens are dropped when the server stops. `SetPairingRequired()` controls it; the mobile builds keep it off. The page asks for the code when it receives a 401, so with pairing off it opens directly.
- **Pairing switch** (2026-10-02): in the picker's MENU tab, the "Pairing code: 123456" line has a switch on its right ("Required" / "Off"), hit with the pointer or flipped with Left/Right on the selected row (`TablePickerPage::RenderPairing`, `PAIRING_ITEM`). It applies at once to the running server (`m_pairingRequired` is now atomic, as the server thread reads it) and is saved immediately to `Standalone.WebServerPairing` (default on), which `VPApp::GetWebServer()` applies when it creates the server, so a disabled code stays disabled at the next launch. With it off, the line reads "Pairing code: disabled" and any device on the local network can add, change or delete tables (said in the tooltip).
- **Three modes** (2026-10-02): the picker's item cycles "Wi-Fi upload: Off" → "On (ask every time)" → "On (always on)" → "Off", and shows the address and the code while on. "Ask every time" is the earlier behavior: on until the application is closed. "Always on" is saved to `Standalone.WebServerAlwaysOn` (default off, saved immediately) and starts the server with the application in play and launcher modes (`VPApp::StartWebServerIfAlwaysOn()`, called from `main.cpp` after `InitInstance`). "Off" stops it and clears the setting. If the server fails to start, the item shows "Off".
- **Kept across table switches** (2026-10-02): the server is no longer stopped when a table is launched from the picker. `VPApp` owns it and the application process stays up between the lobby and the tables (section 10), so once on it stays on until it is turned off or the application exits. Consequence: tables can be uploaded, renamed or deleted from a browser while one is played, including the running table, and a large upload or extraction may cause stutters during play.
- **Staged uploads** (all builds): chunks are written in a hidden `.upload` folder and the file is moved into place when complete. Empty files are created directly.
- **"Upload Folder"** entry in the page's menu (`uploadFolder()`), using the browser's folder picker; skips hidden files. Dropping a folder on the page already worked.
- Uploading `VPinballX.ini` to switch the settings file is limited to the mobile builds.

### Tables page (2026-10-03)

A second page, `tables.html` with `tables.js`, shows the table library the way the picker does, for desktop builds (the mobile launchers own their library, see below).

- **Home page.** `/`, and any unknown path, now serve `tables.html`; the file manager moves to `/vpx.html` (same page, its links are relative or hashes, so folder browsing and the Back button work unchanged). Both pages have a "Tables | Files" switch at the top right of the header. The file manager hides it only when `/info` reports `tableLibrary: false`, so a page cached from an older server still offers the link.
- **Tiles** like the picker's grid: the table image in a 16:10 frame (the table's initial when it has none), the display name, the file name underneath, statistics in the tooltip, and a "Playing" badge on the running table. Images load lazily, through `/table-image?uuid=&v=<modifiedAt>`, so browsers cache them until the image changes.
- **Tabs and search** as in the picker: All, Recent, Newly added, Most played, Favorites (kept in the URL hash), and the same in-order letter matching for the search. "Upload tables and ROMs here", with "here" linking to the file manager, is shown whenever the library is empty.
- **Star** on each tile: adds or removes the favorite at once, saved in `table-stats.json` like the picker's star.
- **"..." menu** on each tile:
  - **Display name**: a dialog with the current name, and the name derived from the file as a hint; "Use file name" goes back to it. Names are trimmed, kept on one line, limited to 128 characters in the page. The picker shows the new name at once (same library).
  - **Delete**: a confirmation dialog with the table's path, then `TableLibrary::Delete`: the whole folder when the table is alone in it, otherwise the table and its same-name companion files. Disabled for the running table, which the server also refuses.
  - The dialogs close only with their buttons, not with Escape nor a click outside their fields.
- **Live list**: the page polls `/tables` every 3 s while it is visible, and renders again only when the answer changed, so favorites set in the headset, tables added by uploads or a rescan, and the running table appear without reloading. Tiles are reused between renders, so images are not reloaded.
- **Routes** (behind pairing like the other API routes; desktop only, 404 in the mobile builds):

  | Route | Method | Parameters | Answer |
  |---|---|---|---|
  | `/tables` | GET | | `{revision, scanning, tables:[{uuid, name, defaultName, path, hasImage, createdAt, modifiedAt, lastPlayedAt, playCount, favorite, playing}]}` |
  | `/table-image` | GET | `uuid` | the image, cacheable for a day; 404 without image |
  | `/table-favorite` | POST | `uuid`, `favorite` (1 or 0) | 200; 400 without `favorite`, 404 unknown table |
  | `/table-name` | POST | `uuid`, `name` (empty: default name) | 200; 400 when `name` is missing or too long, 404 unknown table |
  | `/table-delete` | POST | `uuid` | 200; 409 for the running table, 404 unknown table, 500 when the files could not be removed |

  `/info` also reports `tableLibrary` (true on desktop), which the pages use to tell a desktop server from a mobile one and from a server built before this page. A deletion notifies the file manager like the other file changes; a favorite or a name does not, as no file of the tables folder changes.

Known gaps: plain HTTP; the static pages and `/assets/*` are served without pairing. The tables page serves the table images at full size (a few MB for a screenshot), which a large library may feel over Wi-Fi; a resized thumbnail route would fix it. Not verified on the tables page: refusing to delete the running table (the routes and the page were otherwise checked on macOS), and any use in the headset's network conditions. Not verified: the pairing prompt and the Upload Folder entry in a real browser. The pairing switch, the three modes and the server staying on across tables (2026-10-02) are compiled on macOS but not yet run, on the desktop or in the headset.

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

Files: `src/core/Settings_properties.inl`, `src/core/main.cpp`, `src/renderer/VRDevice.h/.cpp`, `src/renderer/XRVulkanBackend.h`, `src/renderer/XRGraphicBackend.h`, `src/renderer/RenderTarget.h/.cpp`, `src/renderer/Renderer.h/.cpp`, `src/renderer/RenderPass.cpp`, `src/renderer/RenderDevice.h/.cpp`, `src/ui/live/ingameui/VRSettingsPage.cpp`, `platforms/linux-aarch64/bgfx-fragment-density-map.patch`, `platforms/linux-aarch64/bgfx-fragment-shading-rate.patch`, `platforms/linux-aarch64/external.sh`.

Full shading only around the point the eyes look at, through a Vulkan fragment density map on the scene buffer. **Exclusive to this branch, and only built for the Steam Frame** (Linux aarch64 package: the bgfx patch is applied there alone, and the rest is under `#ifdef` on its macros, so Windows and macOS builds compile it out). Evaluation and plan in `Doc/foveated-rendering-plan.md`, full technical reference (mechanism, every layer of the implementation, verification, measurements, open performance analysis) in `Doc/foveated-rendering.md`; everything below was built and checked on the Steam Frame.

- **Settings** (`PlayerVR`): `Foveation` (Off / Low / Medium / High, default Medium on standalone OpenXR builds, Off elsewhere), `FoveationEyeTracked` (default on), and the sign conventions of the gaze offsets `FoveationFlipX` (off) / `FoveationFlipY` (on), which were settled on the device. The VR settings page has the first two and a status line ("Eye-tracked", "Fixed: eye tracking not active (enable it in the headset settings)", "Not supported by the runtime", …). Changes apply live.
- **Runtime side** (`VRDevice`): the color swapchain is created with `XrSwapchainCreateInfoFoveationFB`, a profile (`XrFoveationLevelProfileCreateInfoFB` + `XrFoveationEyeTrackedProfileCreateInfoMETA`) is applied with `xrUpdateSwapchainFB`, the eye-tracked state and gaze come from `xrGetFoveationEyeTrackedStateMETA`. On the Frame this is what turns the eye tracking on (SteamVR/OpenXR 2.17.10; the user must have calibrated eye tracking in the headset settings). The runtime also hands density maps for its swapchain images (`XrSwapchainImageFoveationVulkanFB`, 68×68, one layer per eye), wrapped as bgfx textures by `XRVulkanBackend::CreateFoveationTextures`; they are kept as a fallback only, see below.
- **Why the app builds its own map**: the gaze is applied through fragment density map *offsets* (`VK_EXT_fragment_density_map_offset`, passed at the end of the render pass), and Turnip only honors offsets on density map images created with `VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT` (`tu_enable_fdm_offset` in `tu_cmd_buffer.cc`), which the runtime's maps are not. So `VRDevice::CreateOwnFoveationMap` makes a 68×68 (eye size / 32, the driver's minimum texel), two-layer RG8 map, filled by `FillOwnFoveationMap` with flat rings per level (full density radius 0.45 / 0.30 / 0.20 of the half width, half density up to 0.80 / 0.55 / 0.38, quarter beyond), and every frame the gaze (normalized, y up) becomes pixel offsets (`x · W/2`, `−y · H/2`) for both eyes.
- **Vulkan device** (`XRVulkanBackend.h`): enables `VK_EXT_fragment_density_map` (+ feature), `VK_VALVE_fragment_density_map_layered` (one map layer per eye on our layered scene buffer, Valve's extension, in upstream Mesa), `VK_EXT_fragment_density_map_offset` and `VK_KHR_create_renderpass2`, all when the driver has them; logs "Fragment density maps: supported, one layer per eye, with offsets".
- **bgfx patch** `bgfx-fragment-density-map.patch` (Linux only, applied after the descriptor pool one, cache tag `-turnip4` with the shading rate patch): `BGFX_RESOLVE_FRAGMENT_DENSITY_MAP` tags a frame buffer attachment as the density map (kept out of the color/depth lists, `VkRenderPassFragmentDensityMapCreateInfoEXT` on the v1 render pass, extra attachment in the `VkFramebuffer`, layout barriers); `BGFX_TEXTURE_FRAGMENT_DENSITY_MAP` for textures that are maps (usage, offset-capable, kept in the density map layout; external images are taken as is); `VkPipelineFragmentDensityMapLayeredCreateInfoVALVE` on pipelines rendering into a layered map; render targets created offset-capable; `bgfx::setFragmentDensityMapOffsets(frameBuffer, offsetsXY, layers)` stored per frame and applied with `vkCmdEndRenderPass2KHR` (offsets rounded to the device granularity); `BGFX_CAPS_FRAGMENT_DENSITY_MAP`. Unpatched bgfx does not define these macros, so all the VPX code is under `#ifdef` and inert on Windows and macOS.
- **Scene buffer** (`RenderTarget::SetFragmentDensityMap/Offsets`, `Renderer::SetFragmentDensityMap/Offsets`): `BackBuffer1` and `BackBuffer2` get a cached frame buffer variant carrying the map; `VRDevice::RenderFrame` selects it right after acquiring the swapchain image and sets the offsets of the frame. Post passes sample the resolved full-resolution image unchanged (non-subsampled image), so bloom, AA, tonemap and the preview need nothing.
- **Measurement**: the runtime's counter `/perfmetrics_meta/app/gpu_frametime` (`XR_META_performance_metrics`), enabled with `VPX_XR_METRICS=1` in the environment, and, with `VPX_GPU_PROFILE=1`, that counter plus bgfx's per-view GPU timings grouped by render target (`VRDevice::LogRuntimeStatus`, every 5 s, needs the view names, which release builds now set when profiling — `RenderDevice::m_nameViews`). The counters are off otherwise, and always disabled before a session is destroyed: SteamVR 2.17.10 keeps their enabled state with the OpenXR instance but destroys the timestamp query pools it created for them with the session, then reads those pools again from the next session's `xrEndFrame` — a crash in the Vulkan driver when a table was started after the lobby on the Frame (found with the Vulkan API dump layer: no `vkCreateQueryPool` from `vrclient` for the second session, while its `vkGetQueryPoolResults` followed the begin/wait pattern of every frame).
- **Shading rate image (2026-10-03), the path used on the Frame**: the driver only applies density maps in its tiled render path (next bullet), but programs a fragment shading rate attachment (`VK_KHR_fragment_shading_rate`) in both paths, so the scene pass stays direct. A third bgfx patch (`bgfx-fragment-shading-rate.patch`: `BGFX_TEXTURE_FRAGMENT_SHADING_RATE`, `BGFX_RESOLVE_FRAGMENT_SHADING_RATE`, render pass 2 with the rate attachment, pipeline combiner so the attachment's rate wins) lets `RenderTarget::SetFragmentDensityMap(map, shadingRate = true)` attach a single layer R8U image of one rate code per 8×8 block (270×270 at 2160²) to the scene buffer. `VRDevice::UpdateShadingRateMap` rewrites it whenever the gaze moves by half a texel: full rate in a disc around each eye's gaze (one image for both eyes: the union, hence tighter rings than the fixed profiles), 2×2 in a ring, 4×4 beyond, with the radii of the level; without a valid gaze the wide Low profile at the center. The gaze comes from `XR_EXT_eye_gaze_interaction` as a ray located at its sample time and projected with the frame's views (`LocateGaze`: a fixated object stays sharp while the head turns, which the runtime's per image foveation center could not do), smoothed over 35 ms; `VPX_FOVEATION_DEBUG=1|2|3` shrinks the sharp area to a spot, makes the rest very coarse and circles each spot in magenta, to check the tracking in the headset (`FoveationFlipY` default fixed to false that way): the procedure, what to look for and how to read the status line are in **`docs/Foveated Rendering on the Steam Frame.md`, section 8**, the technical reference of the whole feature (mechanisms, measurements, challenges met; the same document as `Doc/foveated-rendering.md`). Preferred over the density map wherever the driver has it; the status line says which ("Eye-tracked (shading rate image)"). Measured at native 2160×2160, dynamic resolution off, medians of 9 samples over 40 s, eye-tracked with the gaze ray (2026-10-03, headset at 67–71 °C): Addams Family Off 14.3 ms → **High 11.2, Medium 11.3, Low 11.5** (Off again at the end: 14.8), i.e. ≈ 3 ms or 22 % per frame, against an upper bound of 11.0 with 4×4 shading everywhere; with fixed images beforehand, Ghostbusters went 11.3 → 9.2. **`Foveation` defaults to Medium again on standalone OpenXR builds.**
- **What the density map measurements showed (2026-10-02)**, on the heavy Ghostbusters VR-room table at native 2160×2160: foveation High saves ≈ 0.5 ms of a 14 ms frame. Turnip only applies density maps in its tiled render path and forces any pass with a map into it, and in that path the scene pass is geometry-bound (the geometry is reprocessed per tile: 48 tiles, ≈ 6.5 ms of per-tile draws against 4.7 ms for the whole pass rendered directly). Rendering the scene pass directly takes the frame from 14.0 to 9.1 ms. Hence three changes: **`src/core/main.cpp` sets `TU_AUTOTUNE_ALGO=profiled`** in the environment of the Linux standalone build before the Vulkan driver loads (the driver then measures tiled against direct per render pass and keeps the faster; a value set by the user wins), **foveation defaulted to Off** (until the shading rate image of the previous bullet), and the **VR visibility mask is drawn with `Z_LESSEQUAL`** instead of `Z_ALWAYS` (same result, its shader writes the near plane; an always-pass depth write made the driver disable LRZ, its hierarchical depth rejection, for the rest of the pass) **and first**: it was submitted with a sort key that put it after every opaque part (z = 200000 instead of a depth bias of +200000, the key being `depthBias − z`), so it had never masked anything before the opaque draws; a depth-only draw placed first also keeps LRZ writes enabled on Turnip. Full account in `docs/Foveated Rendering on the Steam Frame.md` (the technical reference of the whole feature, a copy of `Doc/foveated-rendering.md`): sections 6 and 7 for the density map and the driver's trace, 8 for the debug mode, 11 for the shading rate image and the challenges met.

## 10. One game session across table switches (2026-10-01/02)

Files: `src/core/VPApp.h/.cpp`, `src/core/player.h/.cpp`, `src/renderer/Window.h/.cpp`, `src/renderer/VRDevice.h/.cpp`, `src/ui/live/LiveUI.h/.cpp`, `src/ui/live/ingameui/TablePickerPage.cpp`, `lib/src/WebServer.cpp`, `src/core/AppCommands.cpp` (details in `Doc/steam-frame-port-plan.md`, section 6).

In launcher and play mode, `VPApp` keeps the OS windows (playfield, VR preview) from one table to the next (`AcquireWindow` / `ReleaseWindow`, matched on the settings they were created with), shows a "Loading <table>..." screen with a spinner while switching (`LiveUI::SetLoadingText`, `Player::RenderLoadingFrame`), and can keep the VR device, i.e. the OpenXR instance, so the runtime does not see the application quit and start again (`AcquireVRDevice` / `ReleaseVRDevice`, `VRDevice::ApplyTableSettings` for the per-table settings, `EndSession` / `ReleaseSession` for a clean session stop, `EXITING` and `LOSS_PENDING` handled).

**On the Steam Frame the VR device is not kept** (`VPApp::m_keepVRDeviceBetweenTables = false`): SteamVR 2.17.10 crashed the second table 0.6 s in, in the Vulkan driver called by `vrclient` from `xrEndFrame`. The Vulkan API dump layer showed that `vrclient` creates its timestamp query pools once per OpenXR instance, destroys them with the first session and reads them again from the second; a new Vulkan device under the same instance made it call the destroyed `VkInstance` (abort in the loader). One instance, one device, one session on this runtime, so a new VR device is created for each table, as before; windows and loading screen are kept.

**Loading screen in the headset (2026-10-02):** it appeared doubled, drifting and ghosting while a table loaded. The loading threads push each texture upload to the render thread as a frame with nothing to present (`RenderDevice::SubmitRenderFrame` from another thread sets `m_frameNoPresent`); the desktop render loop processes such a frame without presenting, but the OpenXR loop (`BGFXOpenXRRenderLoop`) ran it inside an acquired headset frame and gave the swapchain image back to the runtime without having drawn into it, so the headset showed the image's stale content: a loading screen from three frames back, rendered for the head pose of that time. The OpenXR loop now processes upload-only frames in place (bgfx flush, the image stays acquired) and releases the image only once a frame has drawn into it.

## 11. Controller models (2026-10-01, not yet run in the headset)

Files: `src/renderer/VRControllerModels.h/.cpp` (new), `third-party/include/cgltf/cgltf.h` (new, cgltf 1.15, MIT, copied from bgfx's `3rdparty`; `third-party/` is ignored by git, so it needs `git add -f`), `src/renderer/VRDevice.h/.cpp`, `src/renderer/Renderer.h/.cpp`, `src/core/Settings_properties.inl`, `src/ui/live/ingameui/VRSettingsPage.cpp`, `make/CMakeLists_sources.txt`, `make/vpx-core.vcxitems(.filters)`.

The controllers the player holds are drawn in the scene as the headset system shows them, with their buttons, triggers and thumbsticks moving, so the button layout can be seen in VR. The meshes come from the runtime, not from files of ours (Valve's recommendation for the Steam Frame), so any headset whose runtime has the extensions gets its own controllers.

- **Runtime side** (`VRDevice`): enables `XR_EXT_render_model`, `XR_EXT_interaction_render_model` and `XR_EXT_uuid` (needed as the instance asks for OpenXR 1.0) when the runtime has them; the log says "OpenXR controller models: supported by the runtime". `UpdateControllerModels` asks for the models of the devices held (`xrEnumerateInteractionRenderModelIdsEXT`, again on `XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT`, on interaction profile changes, and every 2 s while the list is empty or an asset was unavailable), creates each model, its space and its asset, and keeps the glTF binary and the names of its animatable nodes ("OpenXR controller model N loaded: … KB, … animatable nodes"). Only `KHR_mesh_quantization` is declared as a supported glTF extension. Every frame, `LocateControllerModels` locates each model space in the reference space and reads the node poses (`xrGetRenderModelStateEXT`) at the predicted display time. Both run on the render thread before the frame is requested, like the menu panel; the logic thread reads the result while preparing the frame.
- **Drawing** (`VRControllerModels`, owned by `Renderer`, called at the end of `RenderDynamics` in VR, outside reflection passes): the asset is parsed with cgltf once per model (triangle meshes, base color factor and texture, alpha mask and blend, node hierarchy; PNG/JPEG images through `BaseTexture::CreateFromData`), then each node is drawn with the basic shader in the room space reference: reference space to room VPU is `VRDevice::GetReferenceToRoom()`. An animated node takes the pose given by the runtime as its local transform (relative to its parent), keeping its own scale. Materials: metallic-roughness factors to a VPX material (the metallic-roughness texture is ignored, a mesh which has one is shaded as a non metal), half lit / half unlit (`controllerUnlitPart`) so the controllers stay readable on dark tables, both sides drawn (the room space is mirrored compared to glTF).
- **Found on the Frame** (SteamVR/OpenXR 2.17.10): both controllers are listed and located, but `xrGetRenderModelStateEXT` returns every node hidden with an identity pose, so no part moves; when no node at all is visible the visibility is ignored, and a pose without a unit rotation keeps the node's own transform.
- **Front sticker**: the `status` node is where SteamVR shows the running application's logo when it draws the controllers itself (`visibility.default = false` in `/opt/steamvr/drivers/frame_controller/resources/rendermodels/*/frame_controller_*.json`). The asset maps it on the shared color texture although its texture coordinates span a whole image of its own, which showed a squeezed copy of that texture. No OpenXR function to get or set that logo was found, so the node is drawn with `src/assets/controller-sticker.png` (the ball of the dark iOS app icon, cut out with a round mask on a transparent background, at the 3.8 × 4.1 cm aspect of the sticker, 474 × 512), alpha blended over the body with a white base color.
- **Setting** `PlayerVR/ShowControllers` (default on), in the VR settings page, applied live. The models are also hidden during the table image capture.
- **To check on the device**: that SteamVR lists the Frame controllers once they are bound, their placement against the real controllers, the direction of the texture coordinates, that the animated parts move the right way (the pose is taken as relative to the parent node), and the cost.

## 12. Image quality on the Steam Frame (2026-10-02)

Files: `src/shaders/bgfx/material.sh`, `src/shaders/bgfx/fs_ball.sc`, `src/shaders/bgfx_basic.h`, `src/shaders/bgfx_ball.h` (regenerated), `src/core/Settings_properties.inl`; the analysis and the measurements in `docs/Image Quality on the Steam Frame.md`.

Compared with a PSVR2 on a PC, the Frame showed stair-stepped diagonals and chrome parts that seemed to move while the head was still. No setting or feature is missing on the Frame (all defaults, no platform reduction, anisotropic filtering and mipmaps active): the PC image is supersampled by SteamVR's recommended resolution and downsampled by the compositor, while the Frame renders 1:1 with the panel and no anti-aliasing. Measured at 2160×2160 on a heavy table (9.7 ms baseline, 13.9 ms budget at 72 Hz): Standard FXAA +1.6–2.5 ms, MSAA 4× +11.3 ms (the scene pass runs in direct mode, so the samples go through memory), 1.3× supersampling +5.4 ms. The device heats up over back-to-back runs (GPU 60 °C at rest, 90 °C in a run; a hot run of the same configuration measured 3 ms more), so measurements need a rest between launches. With both changes the table runs at ≈ 11 ms; the specular anti-aliasing itself costs nothing measurable.

- **Standard FXAA by default** in the headset builds (`__STANDALONE__ && ENABLE_XR`), the only option within the budget; it fixes the edges, not the sparkle.
- **Specular anti-aliasing** in the material shader (Kaplanyan 2016, Tokuyoshi & Kaplanyan 2019): the variance of the normal over the pixel, from its screen-space derivatives, widens the glossy lobe (point lights and glossy environment lookup, through the Blinn-Phong exponent ↔ alpha² = 2 / (n + 2) conversion), and the mirror lookup of the specular layer reads the environment map at the mip matching the footprint of the reflection instead of mip 0. Flat surfaces are unchanged. This applies to every backend; the shader headers were regenerated on macOS for SPIR-V, Metal, GLSL and GLSL ES, **the DirectX 11/12 blobs are the previous ones** (bgfx's shaderc needs the Windows HLSL compiler): run `src/shaders/bgfx/shaders.sh` on Windows before relying on it with the Direct3D backends.

## 13. Dynamic resolution in the headset (2026-10-02)

Files: `src/renderer/RenderTarget.h/.cpp`, `RenderPass.h/.cpp`, `RenderFrame.h/.cpp`, `RenderDevice.h/.cpp`, `Renderer.cpp`, `RenderProbe.cpp`, `Sampler.cpp`, `VRDevice.h/.cpp`, `src/core/Settings_properties.inl`, `src/ui/live/ingameui/VRSettingsPage.cpp`, `src/shaders/bgfx/fs_basic.sc`, `fs_ball.sc` (headers regenerated); the design, the controller and the runs in `docs/Image Quality on the Steam Frame.md`, section 8.

Addams Family at 2160×2160 costs 13.5–17 ms on the Frame, over the 13.9 ms period of 72 Hz, and judders on head movements (the compositor reprojects a quarter of the frames). Instead of per-table resolution settings found by trial, the resolution follows the measured GPU time: each frame renders into the top left part of the existing scene buffers at a scale (`RenderDevice::BeginScaledRendering`; the view rect of the flagged targets, the texture coordinates of the full-screen passes and the two clip-space lookups of the shaders follow it), and the runtime is told which part of the swapchain image was drawn (`imageRect`). Nothing is reallocated, so the scale changes every frame without a hitch. The VR device reads the runtime's `gpu_frametime` counter (`XR_META_performance_metrics`) each frame and moves the scale against a target fraction of the display's frame period: down at once when over, up slowly when well under, between a minimum and the table's `ResFactor` size. Settings in the VR page, applied live: `DynamicResolution` (on by default in the headset builds), `DynamicResolutionTarget` (85 %), `DynamicResolutionMinScale` (70 %), plus a status line (size, scale, GPU time against the budget), also logged every 5 s.

Found on the first run: the runtime's recommended size (80 %) is not a low enough floor for Addams on a hot GPU (hence the minimum setting), and SteamVR doubles the predicted display period when it throttles the application to half rate after missed frames, which must not be taken as the budget (the shortest period of the session is).

## 14. Scores and leaderboards (branch `leaderboards`, 2026-10-03/04)

Files: `src/core/ScoreTracker.h/.cpp`, `lib/src/ScoreStore.h/.cpp`, `src/ui/live/ingameui/ScoresPage.h/.cpp` (new), `plugins/plugins/ScoreboardPlugin.h` (new), `src/assets/scores/score-rules.json` (new), `src/assets/pinmame/memmaps/` (new), `src/assets/web/scores.html/.js` (new), `src/core/player.h/.cpp`, `src/core/ScriptGlobalTable.cpp`, `src/core/VPApp.h/.cpp`, `src/core/AppCommands.cpp`, `lib/src/TableLibrary.h/.cpp`, `lib/src/WebServer.h/.cpp`, `plugins/flexdmd/FlexDMD.h/.cpp`, `plugins/flexdmd/UltraDMD.cpp`, `plugins/pinmame/PinMAMEPlugin.cpp`, `src/ui/live/ingameui/InGameUI.cpp`, `TablePickerPage.h/.cpp`, `src/assets/web/tables.html/.js`, `vpx.html`, `styles.css`, `make/CMakeLists_sources.txt`. The full description, from the capture to the routes, is in **`docs/Table Scores and Leaderboards.md`**.

- **Capture** (`ScoreTracker`, one per table of the library played, not for the lobby nor files outside the library): every 250 ms it reads several sources and uses the most reliable one that shows a score: the RAM of the emulated machine through the `game_state` memory map of its ROM (`pinmame`), the scores the script sends to the B2S server (`b2s`, works without a `.directb2s`), the UltraDMD scoreboard (`ultradmd`, through a new `"Scores","OnScoreboard:1"` plugin message broadcast by FlexDMD), global variables of the script with the usual names (`script`), and, when nothing else showed a score, the high scores the script saves with `SaveValue` (`highscore`). A game ends on the machine's game over flag, a game in play variable or the backglass game over light (each trusted only once it said a game runs, and debounced), or, without signal, when the script saves a score or the next game resets the scores. A game is recorded only if its scores were seen at zero near its start (NVRAM and scripts restore the last game's scores at boot); a game left in progress is discarded. Player 1 gets the active profile, the other players' scores are stored unassigned. One `[Scores]` line per session in the log tells what was found, or why nothing was.
- **Rules** for the tables the heuristics miss: `score-rules.json` in `assets/scores` (bundled, still empty) and in the preferences folder, matched on the table file name or the ROM: `disable`, the `source` to use, or the names of the script variables (`scores`, `scoreBase`, `players`, `inGame`, `gameOver`).
- **Memory maps**: the [Pinball Memory Maps](https://github.com/tomlogic/pinmame-nvram-maps) are bundled in `assets/pinmame/memmaps` (ODbL / DbCL, licenses and attribution in the folder; 391 maps, 379 with `game_state`, 1531 ROM names) and used by the PinMAME plugin when no map is found along the table or in the PinMAME folder.
- **Storage** (`ScoreStore`, thread safe, revision counter for polling): `profiles.json` (players, active player) and `scores.json` (one entry per player and game: table uuid, path, name, ROM, profile, slot, players, score, date, duration, source) in the preferences folder, written to a temporary file then renamed. One leaderboard per table file; equal scores ordered by date. Deleting a player keeps their scores, unassigned.
- **Lobby**: "Player: <name>" at the top of the picker opens the profiles page (choose, or add with the keyboard; it opens by itself the first time without any player); a **Scores** tab between Favorites and MENU (tab names shortened to fit on one line in VR) lists the tables with scores and opens their leaderboard (all scores or best per player, up to 50 rows, Play); a table's page shows its 10 best scores; back from a table, a **result page** shows each game's score, rank and place among the players, "New personal best!" or "New record of the table!", and the leaderboard around it, with "Save under a new player..." when nobody was selected. The VR keyboard of the search is now a reusable function, also shown by `TextEntryPage` for names.
- **Web**: a **Scores** page (header link on every page, "Show scores" in each table's menu) with table and player filters, All scores / Best per player views, give a score to another player, delete one, **clear the scores of a table or all of them** (confirmation dialog, typing `DELETE` for all), and the players panel (add, rename, delete, set active); it polls every 3 s. Routes `/scores`, `/score-delete`, `/score-assign`, `/scores-clear` (`uuid` or `all=1`, never an empty uuid), `/profile-add`, `/profile-rename`, `/profile-delete`, `/profile-active`, behind pairing, 404 in the mobile builds; `/info` reports `scores`.
- **To check on the Steam Frame**: B2S tables without backglass file, the name entry with the VR keyboard, the result page in the headset; then rules for the tables the library coverage run finds without score.

## Effects on existing builds

- `-Play`, the editor and the Visual Studio build behave as before, except for the OpenXR changes that apply everywhere: the Vulkan extension filter, the extra controller inputs and default mappings, the menu panel and pointer, and the upstream VR fixes of section 8.
- The dynamic resolution plumbing (render scale on passes and targets) is inert outside VR: the scale stays 1 and every view rect and texture coordinate is what it was.
- Every build with the bgfx renderer gets the specular anti-aliasing of the material shader (section 12), except through Direct3D 11/12 until the shader headers are regenerated on Windows.
- Standalone desktop builds record the scores of the library tables they play (`profiles.json`, `scores.json` in the preferences folder) and their PinMAME plugin falls back to the bundled memory maps (section 14); the table picker gets a Scores tab and a player item.
- Standalone desktop builds get a "Tables" entry at the top of the in-game menu. When a table starts they create `VPinballX/Tables/pinmame/roms` in the documents folder, unless a PinMAME folder is already defined or `~/.pinmame/roms` exists (section 6).
- iOS/Android library builds: uploads are staged in `.upload`; `InGameUIPage` has the tile code; the home page of the web server stays the file manager, the tables routes answer 404 and `/info` reports `tableLibrary: false`, so the file manager hides its link to the tables page; no other intended change.
- The Visual Studio project files (`make/*.vcxproj`) were not updated. None of the new files is needed by a non-standalone build; the CMake build lists them.

## Files

New:

```
lib/src/TableLibrary.h
lib/src/TableLibrary.cpp
lib/src/ScoreStore.h
lib/src/ScoreStore.cpp
src/core/ScoreTracker.h
src/core/ScoreTracker.cpp
plugins/plugins/ScoreboardPlugin.h
src/ui/live/ingameui/ScoresPage.h
src/ui/live/ingameui/ScoresPage.cpp
src/assets/scores/score-rules.json
src/assets/pinmame/memmaps/
src/assets/web/scores.html
src/assets/web/scores.js
src/assets/web/tables.html
src/assets/web/tables.js
src/ui/live/ingameui/TablePickerPage.h
src/ui/live/ingameui/TablePickerPage.cpp
src/ui/live/ingameui/MessagePage.h
src/ui/live/ingameui/MessagePage.cpp
tests/test-table-library.cpp
src/renderer/VRControllerModels.h
src/renderer/VRControllerModels.cpp
third-party/include/cgltf/cgltf.h
docs/Steam Frame Branch.md
docs/Foveated Rendering on the Steam Frame.md
docs/Image Quality on the Steam Frame.md
docs/Table Scores and Leaderboards.md
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
platforms/linux-aarch64/bgfx-fragment-shading-rate.patch   fragment shading rate attachments, the foveation path used on the Frame (section 9)
platforms/linux-x64/external.sh         OpenXR loader
lib/src/WebServer.h, WebServer.cpp      desktop use, pairing, staged uploads, tables routes, tables page as home page, scores and profiles routes
lib/src/TableLibrary.h, .cpp            FindTable (library table of a file, for the scores)
src/assets/web/app.js, vpx.html         pairing prompt, Upload Folder, missing ROMs, ROM folder link, Tables | Scores | Files switch
src/assets/web/tables.html, tables.js   Scores link, "Show scores" in the table menu
src/assets/web/styles.css               missing ROMs, ROM folder link, upload banner, tables page, scores page
src/core/ScriptGlobalTable.cpp          SaveValue forwarded to the score tracker
plugins/flexdmd/FlexDMD.h, .cpp, UltraDMD.cpp   UltraDMD scoreboard broadcast for the scores
plugins/pinmame/PinMAMEPlugin.cpp       bundled memory maps as last resort
src/core/AppCommands.h, AppCommands.cpp -Launcher, table switching, lobby, result or profiles page when the lobby opens
src/core/main.cpp                       launcher counts as play mode, web server started with the application when always on
src/core/VPApp.h, VPApp.cpp             table library, web server (pairing and always-on settings), ROM folder, lobby path, score store, last session result
src/core/Settings_properties.inl        Standalone settings (including WebServerPairing, WebServerAlwaysOn), VR autodetect and Standard FXAA defaults on standalone OpenXR builds
src/shaders/bgfx/material.sh, fs_ball.sc   specular anti-aliasing (section 12)
src/shaders/bgfx/fs_basic.sc, fs_ball.sc   clip-space lookups scaled for the dynamic resolution (section 13)
src/shaders/bgfx_basic.h, bgfx_ball.h   regenerated (DirectX blobs carried over)
src/renderer/RenderTarget.h, .cpp       dynamic resolution flag and scaled view rect
src/renderer/RenderPass.h, .cpp         render scale per pass, scaled scissor
src/renderer/RenderFrame.h, .cpp        scale of the frame for the presentation
src/renderer/RenderDevice.h, .cpp       Begin/EndScaledRendering, scaled full-screen quad
src/renderer/Renderer.cpp               scaled frame, flagged targets, scale uniforms, depth copy
src/renderer/RenderProbe.cpp, Sampler.cpp   flagged probe targets, re-activation with the scale
src/ui/live/ingameui/VRSettingsPage.cpp foveation and dynamic resolution items
src/input/XRInputHandler.h              eye gaze pose action and interaction profile (foveated rendering)
src/core/player.h, player.cpp           table image on close, score tracker
src/input/InputManager.h, .cpp          quit action captures the image in launcher mode, no layout prompt for the VR virtual gamepad
src/input/SDLInputHandler.h             VR virtual gamepad detection, joystick vendor/product ids logged
src/input/XRInputHandler.h              Frame controller, aim poses, analog read
src/renderer/VRDevice.h, VRDevice.cpp   Linux, Frame extension, pointer, controller models
src/renderer/Renderer.h, Renderer.cpp   controller models drawn in VR
src/renderer/XRVulkanBackend.h          run-time libvulkan on Linux, extension filter
src/ui/live/LiveUI.h, LiveUI.cpp        pointer fed to ImGui, menu rendered while closing for the table image
src/ui/live/ingameui/HomePage.cpp       "Tables" entry
src/ui/live/ingameui/InGameUI.h, .cpp   page registration (profiles, scores/result), accessors
src/ui/live/ingameui/TablePickerPage.h, .cpp   player item, Scores tab, top scores, reusable VR keyboard
src/ui/live/ingameui/InGameUIItem.h     tile image and toggle
src/ui/live/ingameui/InGameUIPage.h, .cpp  tile layout
```

## Working on this branch

- The repository mixes LF and CRLF files and has no `.gitattributes`. On Windows, set `git config --global core.autocrlf false` before cloning, and use editors and tools that preserve line endings.
- User data is never part of the repository: tables, ROMs, `VPinballX.ini`, `tables.json`, `table-stats.json`, `profiles.json` and `scores.json` live in the user's documents and preferences folders. Keep it that way: check `git status` before committing, and never add `.vpx` tables other than upstream's own assets, `.directb2s`, ROM zips or `.ini` files.
