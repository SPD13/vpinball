# The `steam-frame` branch

This branch of the fork `SPD13/vpinball` prepares Visual Pinball's standalone player for the Valve Steam Frame (SteamOS on ARM64, SteamVR's OpenXR runtime, Vulkan), and adds a table launcher that works from inside the headset. It starts from upstream commit `fd5e18d` (10.8.1 beta).

This document lists everything the branch changes, where, and how far each part has been checked.

## Status

| Part | State |
|---|---|
| Table library, table picker, launcher mode, web upload, shared ROM folder | Built and run on macOS arm64. Checked through automated tests, scripted runs with frame captures of the real application, and `curl` for the web server. Not yet driven by a person with a mouse, a keyboard or a controller for every feature (see each section). |
| OpenXR, Valve Frame controller profile, Vulkan extension filter, VR menu panel and pointer | **Run in VR on Windows with a PSVR2** (SteamVR 2.17), in the `windows-mingw` build with `ENABLE_XR=ON` (2026-09-27), which is standalone + OpenXR + Vulkan like the Frame build. Linux ARM64: compiled on 2026-09-19 only, **before** the work done on Windows (section 8), which has not been compiled for Linux yet. Nothing has run on a Steam Frame. |
| Windows | `windows-mingw` (with or without `ENABLE_XR`): built and used with GCC 16 (MSYS2 UCRT64), Debug. Visual Studio build: built and run in VR in Debug with Vulkan, without the launcher (see "Build variants"). |
| iOS / Android library builds | Not compiled. Shared files they use were changed (`WebServer`, `InGameUIPage`, `VPApp`, `player`); see "Effects on existing builds". |

## Build variants: what contains what

The launcher lives in the in-game menu and is compiled only in *standalone* builds (`__STANDALONE__`).

| Build | Table picker and launcher | Web upload | OpenXR |
|---|---|---|---|
| macOS | yes | yes | no (no macOS branch in `VRDevice`) |
| Linux x64 / aarch64 | yes | yes (RAR/7z when `external.sh` built libarchive) | yes, with `-DENABLE_XR=ON` (new) |
| windows-mingw | yes | yes | yes, with `-DENABLE_XR=ON` (new, Vulkan only) |
| Windows, Visual Studio | no | no | yes (as upstream) |
| iOS / Android library | hidden (they have a native launcher) | yes (as upstream) | Android as upstream |

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

- **Tabs:** All, Recent (by last played), Newly added (by date added), Most played, Favorites.
- **Search box** filtering as you type, on every tab. In All, results are ordered by score.
- **Pages** of 12 tables, with a pager above and below the tables. Only the tables of the page get menu items, sub-pages and thumbnails; thumbnails of other pages are released.
- **Grid view** with thumbnails (decoded when visible, at most one per frame, downscaled to 512 px) and a **favorite star** on each thumbnail, or **list view**.
- In the All tab: letter filter (from 9 tables up) and A–Z / Z–A sort.
- Hovering or selecting a table shows when it was added, when it was last played and how many times.
- Per-table page: Play / Restart, Add to / Remove from favorites, Rename, Reset table settings (if an `.ini` exists), Delete with confirmation. Reset and Delete are not offered for the running table. Actions fire once per press (the stock menu repeats an action every frame while a button is held).
- `TextEntryPage`: text entry with four buttons (left/right pick a character, then "Add"), used for rename and for search when there is no keyboard.
- Library section: view mode, sort, rescan, tables folder, ROM folder hint, and in launcher mode "Settings" (opens the menu's home page) and "Quit Visual Pinball".
- Inputs: with a mouse or pointer, tabs, pager arrows, stars and the search box are clicked directly. With buttons, left/right on the tab row, a pager or the letter filter changes them, activating the search row opens the text entry page, and favorites are toggled from the table's page.

New settings (`src/core/Settings_properties.inl`): `Standalone/TablesPath` (empty = `VPinballX/Tables` in the user's documents folder), `Standalone/TablePickerGridView`, `Standalone/TablePickerSortAscending`. The current tab, search text and page are kept for the session only.

The library is owned by `VPApp::GetTableLibrary()` (`src/core/VPApp.h/.cpp`). It uses a dedicated sub folder because upstream's default tables location on desktop is the whole documents folder, which must not be scanned, served by the web server, or have its zips consumed.

Not verified: clicking a star, a tab or a pager arrow with a real mouse, and typing in the search box. The scripted checks drove the same code paths without input devices.

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
docs/Steam Frame Branch.md
```

Modified:

```
README.md                               notice about this fork
CMakeLists.txt                          ENABLE_XR option for Linux
make/CMakeLists_sources.txt             new sources, VPX_WEBSERVER_SOURCES
make/CMakeLists_app.txt                 web server, zip, ENABLE_XR and loader on macOS/Linux
platforms/linux-aarch64/external.sh     OpenXR loader
platforms/linux-x64/external.sh         OpenXR loader
lib/src/WebServer.h, WebServer.cpp      desktop use, pairing, staged uploads
src/assets/web/app.js, vpx.html         pairing prompt, Upload Folder, missing ROMs, ROM folder link
src/assets/web/styles.css               missing ROMs, ROM folder link, upload banner
src/core/AppCommands.h, AppCommands.cpp -Launcher, table switching, lobby
src/core/main.cpp                       launcher counts as play mode
src/core/VPApp.h, VPApp.cpp             table library, web server, ROM folder, lobby path
src/core/Settings_properties.inl        three Standalone settings
src/core/player.h, player.cpp           table image on close
src/input/InputManager.cpp              quit action captures the image in launcher mode
src/input/XRInputHandler.h              Frame controller, aim poses, analog read
src/renderer/VRDevice.h, VRDevice.cpp   Linux, Frame extension, pointer
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
