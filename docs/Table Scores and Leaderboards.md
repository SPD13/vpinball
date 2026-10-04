# Table scores and leaderboards

The standalone player records the score of every game played on a table of its library, under the name of the player wearing the headset, and keeps a leaderboard per table. Players are chosen in the lobby; the scores are shown in the lobby after each table and in a Scores tab, and are managed from a browser through the web server (Scores page). This is part of the `steam-frame` work (branch `leaderboards`), written for the Steam Frame but built into every desktop standalone build (macOS, Linux, windows-mingw).

Nothing has to be entered by hand: the score is read from the table while it runs. That is the hard part, because tables keep their scores in very different places. This document explains how it is done, how the scores are stored and shown, and how to fix a table whose scores are not found.

## 1. What the player gets

- **Players (profiles).** "Player: <name>" at the top of the table picker opens the list of players: pick who is playing, or add a new one with the keyboard (virtual keyboard in VR). The first time the lobby starts with no player, it opens this list. Players can also be added, renamed, deleted and made active from the browser.
- **Automatic score capture.** At the end of each game on a library table, the scores are saved. Several games in one session give several scores. A game still running when the table is left is not recorded.
- **Result screen.** Back in the lobby after a table, a page shows the score of each game, its rank on the table ("rank 3 of 17 (2 of 5 players)"), "New personal best!" or "New record of the table!", and the leaderboard around it, the session's scores highlighted. Without an active player, "Save under a new player..." creates one and gives it the scores just played.
- **Scores tab** in the table picker: every table with scores, the last played first, with its best score and holder, and the active player's best and rank. Selecting one opens its leaderboard (all scores, or the best of each player), from which the table can be started.
- **Table page** in the picker: the 10 best scores of the table under its actions ("No score yet: be the first!"), with a link to the whole leaderboard.
- **Scores page in the browser** (`scores.html`): every score with filters by table and by player, "All scores" / "Best per player" views, give a score to another player, delete a score, clear the scores of a table or all of them, and manage the players. It updates by itself while games are played. The tables page gets "Show scores" in each table's "..." menu.
- **Multiplayer.** In a game of several players, player 1 is the one wearing the headset: their score goes to the active player. The scores of the other players are kept as "Unassigned" and can be given to a player from the browser.

## 2. Decisions

These were chosen with the user before the implementation and shape the rest:

| Question | Decision |
|---|---|
| Who gets the score of a multiplayer game | Player 1 → the active profile; players 2 and up → "Unassigned", reassignable in the web page |
| A game left in progress | Discarded: only games whose end was seen are recorded |
| No source finds a score | Nothing is recorded, the miss is logged, and the table gets an entry in `score-rules.json`. There is no manual score entry |
| What a leaderboard groups | One leaderboard per table file of the library (its uuid), not per ROM: two tables running the same ROM have separate leaderboards |
| Tables outside the library (`-play` of a file elsewhere), the lobby, the editor | Not tracked |

## 3. Capturing the scores: `ScoreTracker`

Files: `src/core/ScoreTracker.h/.cpp`.

### 3.1 Life of a tracker

`Player`'s constructor creates a `ScoreTracker` when the player is in play mode, is not the lobby, and the table file belongs to the library (`TableLibrary::FindTable`, which matches the file's path relative to the tables folder). It does so after the script was initialized, then calls `Start()`, which loads the rules, reads the high scores the table had saved before (`VPReg.ini`, section 3.3), and probes the script's variables.

- **Every frame**, `Player::PrepareFrame` calls `Update()`, which polls the sources every **250 ms** (`POLL_PERIOD`).
- **When the table is left**, `Player::~Player` calls `Finish()` *before* the plugins are told the game ended: PinMAME and the script still run, so a last poll reads them. A game still in progress is discarded here ("The game in progress (P1 1,234,560) is not recorded: it was not finished").
- The script then runs its `Exit` event, where many tables save their high scores; `LateFinish()` comes after it, records the high-score fallback (section 3.3) if nothing else showed a score, and writes the session line of the log.
- The ids of the scores recorded in the session go to `VPApp::m_lastSessionResult`, which the lobby turns into the result page (section 6.3).

Everything runs on the main thread, which is also the thread of the script and of the plugin messages, so the tracker has no locking of its own; the store it writes to is thread safe (section 5).

### 3.2 Sources

Each source gives a *reading*: the scores of the players (player 1 first), the number of players when known, and whether a game is running when the source can tell. Sources, from the most reliable:

| Name (stored in `source`) | What is read | Tables covered | In game signal |
|---|---|---|---|
| `pinmame` | The RAM of the emulated machine, through the **memory map** of its ROM (`game_state` of the [Pinball Memory Maps](https://github.com/tomlogic/pinmame-nvram-maps) project): one state per player score, `player_count`, `game_over` | ROM tables whose map has a `game_state` section (WPC, System 11, Bally, Stern, Gottlieb...: 379 of the 391 bundled maps, 1531 ROM names in the index) | `game_over` flag of the machine |
| `b2s` | What the script sends to the B2S backglass server: `B2SSetScorePlayer` (scores of players 1 to 6), `B2SSetCanPlay` (players), `B2SSetGameOver` | Most EM and original tables (`If B2SOn Then Controller.B2SSetScorePlayer...`) | Game over light (`B2SSetGameOver`) |
| `ultradmd` | The scores of the UltraDMD scoreboard (`DisplayScoreboard(cPlayers, highlighted, s1..s4, ...)`), as FlexDMD implements UltraDMD | Original tables built on UltraDMD | none |
| `script` | Global variables of the table script with the usual names (below) | EM and original tables without the above | `bGameInPlay`-like or `GameOver`-like variable |
| `highscore` | Values the script saves with `SaveValue` under score-like keys | Last resort, when no other source showed a score in the session | none |

**How each is read:**

- **pinmame.** The PinMAME plugin loads the memory map of the running ROM and exposes its entries as controller *states* of a "Game States" source, described by their path in the map (`game_state\scores\Player 1`, `game_state\player_count\...`, `game_state\game_over\...`). The tracker gets the ROM name from the controller's `gameId` (`pinmame::<rom>`), enumerates the states with `CTLPI_STATE_GET_SRC_MSG`, keeps those three kinds, and reads them through their `GetState` callbacks (any integer, float or text format). The states are dropped and enumerated again whenever the plugin announces a change (`CTLPI_STATE_ON_SRC_CHG_MSG`). Maps are looked up next to the table, then in the PinMAME folder, then, new, in the maps bundled with the application (section 4).
- **b2s.** The B2S plugins broadcast `"B2S","OnStateChange:1"` for every score (`'C'` events) and data change (`'E'` events, with the ids of the helpers: 31 `B2SSetCanPlay`, 35 `B2SSetGameOver`). The legacy B2S server broadcasts *before* it checks that a backglass is running, so this works for tables without a `.directb2s` file. Values are 32-bit, plenty for EM tables.
- **ultradmd.** New plugin message, `plugins/plugins/ScoreboardPlugin.h`: `"Scores","OnScoreboard:1"` with a `ScoreboardEvent` (source name, players, current player, up to 8 scores of 64 bits). `UltraDMD::DisplayScoreboard` broadcasts it through `FlexDMD::BroadcastScoreboard`. Any other plugin that shows scores on behalf of a script can send it too; its `source` name is what gets stored.
- **script.** The tracker asks the script engine for the dispatch id of each candidate name (`GetIDsOfNames`) and keeps those that read as properties (functions with the same names are left out), then reads them with `DISPATCH_PROPERTYGET`. Names tried, the first found wins:
  - scores, as an array: `Score`, `Scores`, `PlayerScore`, `PlayerScores`, `PScore`, `PlayersScore`; else one variable per player: `Score1..4`, `PlayerScore1..4`, `P1Score..P4Score`; else a single score variable among the first list;
  - players: `PlayersPlayingGame`, `PlayersInGame`, `NumberOfPlayers`, `NumPlayers`, `PlayerCount`, `TotalPlayers`, `Players`, `nPlayers`;
  - in game: `bGameInPlay`, `GameInPlay`, `bGameInProgress`, `GameInProgress`, `bGameActive`, `GameActive`, `bGameStarted`, `GameStarted`, `bInGame`, `InGame`, `bGameOn`, `GameOn`, `bGameRunning`, `GameRunning`;
  - game over (only when no in game variable): `bGameOver`, `GameOver`, `IsGameOver`.

  Score arrays are usually declared with one more entry and used from index 1 (`Dim Score(4)`): index 0 is skipped unless it ever holds a score, in which case the array is taken as filled from 0 (or as the rule says, `scoreBase`). Values may be numbers or text ("1,234,560").
- **highscore.** `ScriptGlobalTable::SaveValue` tells the tracker of every value the script saves. Keys containing `score`, `hiscore`, `highsc` or starting with `hs` count, unless they contain `name`, `init`, `letter` or `char` (initials); values under 100 are ignored. A saved score that was not already among the values of `VPReg.ini` when the table started is new; if no other source showed a score during the session, each new one is recorded as a one-player game at the end of the session. These saves are also an end of game hint for the other sources (section 3.4).

### 3.3 Choosing the source

The *primary* source is the most reliable one that has shown a non-zero score during the session (`pinmame` > `b2s` > `ultradmd` > `script`), unless a rule names one. It can move up during the session when a better source starts showing scores (logged: "Scores now taken from b2s instead of script"); the game being tracked is then restarted from the new source. Every source that shows a score is logged once ("Scores shown by pinmame: P1 1,234,560"), and every stored score keeps the name of its source, so coverage can be measured.

### 3.4 Finding the games

A session can hold several games. The tracker runs a small state machine, *idle* → *in game* → *idle*, fed with the reading of the primary source at each poll.

**With an in game signal.** The signals are tried in an order that depends on the primary source (`pinmame`: machine flag, script variable, backglass light; `b2s`: light, variable; others: variable, light). A signal is only *trusted* once it has said that a game is running: tables often have a variable or a light with a usual name that they never use, which would otherwise read "no game" forever. Changes must last before they count, as signals bounce at ball changes and tilts: 2 polls (0.5 s) to start a game, 6 polls (1.5 s) to end one.

**Without signal**, a game starts when the scores go from none to some, and ends:
- when the script saves a score while the game runs (most tables save their high scores at game over), or
- when the scores are reset by the next game (the previous game's last values are kept).

Scores already shown when the table starts do not start a game: they are often the previous game's, restored by the script.

**Games must start from zero.** A machine whose NVRAM was saved in the middle of a game boots with that game's scores in memory; a script may restore its last scores. So a game is only recorded if its scores were seen at zero within 3 s before its start (`GAME_START_ZERO_WINDOW`) or during it; otherwise: "Game ended without its scores being reset at its start (...): not recorded, as they are from an earlier game".

**Recording** (`RecordGame`): a game whose scores are all zero is skipped. The number of players is what the source says, or up to the last player who scored. One score is stored per player with a non-zero score: player 1 gets the active profile (if any), the others none. The duration of the game and the ROM name are kept. The scores are written at once, so a crash later in the session does not lose them.

### 3.5 What the log says

Every line starts with `[Scores]`. At the end of each session one line sums it up:

- `Session of 'Addams Family': 2 score(s) recorded from pinmame, end of game from the game over flag of the machine (rom: taf_l7)`
- `Session of '...': no score source found (rom: none). Add a rule in score-rules.json to record its scores.` (warning)
- `Session of '...': scores seen from script but no finished game (the last one was left in progress). No end of game signal was found for this table: it may need a rule in score-rules.json` (warning)

Along the way: the ROM, the memory map found ("Memory map of the ROM: 4 player score(s), game over flag, player count"), the script variables found, the rule used, the end of game signal once trusted, each recorded game ("Game recorded from b2s (2 player(s), 312s): P1 120,000, P2 85,000 (player 1: Seb)").

## 4. Rules for the tables the heuristics miss: `score-rules.json`

The long tail of tables is fixed with data, not code. Rules are read from two files, in this order, the later overriding the earlier field by field:

1. `assets/scores/score-rules.json`, shipped with the application (`src/assets/scores/score-rules.json`);
2. `score-rules.json` in the preferences folder, for the user's own rules.

A rule applies to the tables whose **file name contains** `file` (case insensitive), checked when the table starts, or which run the ROM `rom`, checked again once PinMAME reports the ROM.

```json
{"rules": [
  {"file": "my original table", "source": "script", "scores": "PlayerScores", "scoreBase": 1, "inGame": "GameRunning"},
  {"rom": "xyz_l1", "disable": true}
]}
```

| Field | Effect |
|---|---|
| `disable` | Record nothing for this table |
| `source` | Use this source (`pinmame`, `b2s`, `ultradmd`, `script`, `highscore`) instead of the most reliable one seen |
| `scores` | Script variable holding the scores (array or single value); replaces the usual names |
| `scoreBase` | 0 or 1: first index of the scores array |
| `players` | Script variable holding the number of players |
| `inGame` | Script variable true while a game runs |
| `gameOver` | Script variable true when no game runs (used instead of `inGame`) |

A variable named by a rule and not found in the script is logged as a warning. The bundled file has no rule yet: they are to be added from the coverage run over the library (section 9).

## 5. Bundled memory maps

`src/assets/pinmame/memmaps/` holds `index.json`, `maps/` and `platforms/` of [tomlogic/pinmame-nvram-maps](https://github.com/tomlogic/pinmame-nvram-maps), revision `7e63610`, 5 MB. They are under the **Open Database License** (database) and the **Database Contents License** (contents), Copyright Tom Collins and contributors; both licenses and a README with the attribution are in the folder and must stay with it.

`plugins/pinmame/PinMAMEPlugin.cpp` uses them only when no `index.json` is found in the folder configured for the maps or in `<PinMAMEPath>/memmaps`, through the application path given by `GetVpxInfo`. A user can therefore still use a newer copy of the project in their PinMAME folder.

## 6. Storing the scores: `ScoreStore`

Files: `lib/src/ScoreStore.h/.cpp`, owned by `VPApp::GetScoreStore()`. Like `TableLibrary`, it depends only on the standard library and `nlohmann/json`, and logs through a callback.

### 6.1 Files

Both in the preferences folder, next to `tables.json`. They are written to a `.tmp` file then renamed, so a crash or a full disk never leaves a truncated file.

`profiles.json`:

```json
{"activeProfileId": "1f0c...", "profiles": [{"id": "1f0c...", "name": "Seb", "createdAt": 1791100000}]}
```

`scores.json`:

```json
{"scores": [{"id": "9b2e...", "tableUuid": "30baf14c-...", "tablePath": "Addams/Addams Family.vpx", "tableName": "Addams Family",
             "rom": "taf_l7", "profileId": "1f0c...", "playerSlot": 1, "playerCount": 2, "score": 123456780,
             "playedAt": 1791100312, "durationSec": 312, "source": "pinmame"}]}
```

- Ids are 16 random hex digits. Dates are seconds since 1970 (UTC); `playedAt` is the end of the game.
- `profileId` is empty for the scores that belong to nobody.
- `tablePath`, `tableName` and `rom` are kept so that scores can be matched again if a table is imported again under a new uuid, and so that the scores of a table removed from the library still show with its name ("(removed)" in the web page).
- Entries without an id or a table are dropped when loading. A file that does not parse is logged and treated as empty, and is replaced at the next change.

### 6.2 Rules of the store

- **Thread safe**: one mutex protects everything, as the web server calls the store from its own thread. A revision counter is incremented at each change, for the pages that poll (`GetRevision`).
- **Profiles**: names are trimmed, must not be blank and are unique regardless of case (the lobby and the web page limit them to 32 characters). The first profile added becomes the active one. Deleting a profile keeps its scores, which then belong to nobody; if it was the active one, the first remaining profile becomes active.
- **Order of a leaderboard**: higher score first; equal scores by date, the oldest first (it was reached first), then by id so the order is stable.
- **Rank of a score** (`GetRank`): its place among all the scores of the table, and, if it belongs to someone, the place of its player among the best score of each player, counting this score even if the player did better before (it tells where this game places them), and whether it is the player's best on the table.
- **Best per player** (`GetBestByProfile`): the best score of each profile on a table; scores belonging to nobody are left out.
- `AddScores`, `DeleteScore`, `AssignScore` (to a profile, or to nobody), `ClearScores` (of a table, or all of them).

## 7. In the lobby

Files: `src/ui/live/ingameui/ScoresPage.h/.cpp`, `TablePickerPage.h/.cpp`, `InGameUI.cpp`, `src/core/AppCommands.cpp`.

- **Player item.** `AddPlayerItem` puts "Player: <name>" (or "Player: nobody (choose who is playing)") at the top of the picker, on every tab. It opens `profiles`.
- **Profiles page** (`ProfilesPage`, `profiles`): the players with a check mark on the active one and their number of scores; selecting one makes it active ("Playing as Seb"). "New player..." opens a `TextEntryPage` (32 characters) and makes the new player active. Players are renamed and deleted from the browser. The page rebuilds itself when the store changes, so a change made in a browser shows at once.
- **First launch.** When the lobby opens for the first time in a run and there is no profile, it navigates to the profiles page.
- **Scores tab** (`BuildLeaderboardList`): a new tab of the picker between Favorites and MENU (the tabs got shorter names, "New" for Newly added, to stay on one line in VR). One row per table with scores, last played first: "Table: best score (holder)", and as details the number of scores, the last date, and the active player's best and place. A row opens `scores/<uuid>`.
- **Leaderboard of a table** (`TableScoresPage`): "Play" (unless it is the table running), a switch between all the scores and the best of each player (kept for the run), the active player's best and place, then up to 50 rows: rank, score, player, date, the active player's rows highlighted. The details of a row give the date and "player 2 of 3" for multiplayer games.
- **Table page.** Under the actions of a table in the picker, the 10 best scores, and "All N scores..." when there are more (`AddTopScores`). "Reset table settings" and "Delete" are still hidden for the running table; the scores are shown in both cases.
- **Result page** (`ScoreResultPage`, `scores/result`): when `PlayTableCommand::Play` comes back to the lobby with `VPApp::m_lastSessionResult` set, it opens the picker then this page. It lists the table, each game of player 1 ("Game 2: 1,234,560, rank 3 of 17 (2 of 5 players)", or "Your score:" for a single game), the record or personal best line for the best game, the other players' scores as unassigned, "Continue" (first, so that buttons land on it), "Full leaderboard", and the leaderboard from 3 rows above to 3 rows below the best game, with the session's scores highlighted. Without active player it offers "Save under a new player...", which creates the profile, makes it active and gives it the player 1 scores of the session. When several tables are played in a row without the lobby (switching from the in-game menu), only the last one's result is shown.
- **Keyboard.** The VR keyboard of the picker's search (`RenderVirtualKeyboard`) became a reusable function, with words capitalized for names; `TextEntryPage` shows it under its button-driven entry, so names can be typed with the controllers' pointer, and takes a maximum length.

## 8. In the browser

Files: `src/assets/web/scores.html`, `scores.js`, `styles.css`, `tables.html`, `tables.js`, `vpx.html`, `lib/src/WebServer.h/.cpp`.

### 8.1 Scores page

A third page, next to Tables and Files in the header of each page.

- **Filters**: a table (All tables, or one of the tables with scores; a table removed from the library is marked "(removed)"), a player (All players, each player, Unassigned), and two views: **All scores** and **Best per player**. `?uuid=` selects a table, `&view=best` the view; the tables page's "Show scores" opens it this way.
- **List**: rank on its table, score, player (and "Player 2 of 3"), table, date. The rows of the active player are tinted, unassigned ones in italics, and the tooltip tells the source ("Read from the memory of the machine (PinMAME)"). With all tables shown, the leaderboards follow each other by table name.
- **Actions on a score**: give it to another player or to nobody (a dialog with a list), and delete it (a confirmation dialog with its details).
- **Clear the scores.** "Clear table scores", shown when one table is selected and has scores, and "Clear all scores", shown when there are any, at the right of the count. Both open a confirmation dialog telling how many scores, of how many tables, will go, that all the players' scores go whatever the filters, that the players are kept, and that it cannot be undone. For one table, the focus starts on Cancel, so that Enter does not clear by mistake. For all the scores, the Clear button stays disabled until `DELETE` is typed (any case), and the field is emptied each time the dialog opens. The outcome is shown in the status line ("3 scores deleted from Addams Family").
- **Players panel**: each player with their number of scores, "Active" or a "Set active" button, rename, and delete (the dialog says their scores are kept, unassigned). "Add player" opens a name dialog, which stays open to fix a name already taken.
- **Live**: the page polls `/scores` every 3 s while visible and renders again only when the answer changed, so a game ended in the headset or a change made in the lobby shows without reloading.
- The dialogs close only with their buttons, as on the tables page. A page served by a server without the scores (mobile build, older version) says so instead of the list.

### 8.2 Routes

Behind pairing like the other API routes; they answer 404 in the mobile library builds, and `/info` reports `scores: true` on desktop.

| Route | Method | Parameters | Answer |
|---|---|---|---|
| `/scores` | GET | | `{revision, activeProfileId, profiles:[{id, name, createdAt, scoreCount}], tables:[{uuid, name, inLibrary}], scores:[{id, tableUuid, profileId, playerSlot, playerCount, score, playedAt, durationSec, source, rank}]}`, scores best first, `rank` on their table; `tables` lists the library and the removed tables that still have scores |
| `/score-delete` | POST | `id` | 200; 404 unknown score |
| `/score-assign` | POST | `id`, `profile` (empty: nobody) | 200; 404 unknown score or profile |
| `/scores-clear` | POST | `uuid` (a table) **or** `all=1` | `{"deleted": n}`; 400 with neither, both, or an empty `uuid` without `all=1`, so that a page which lost its table can never clear everything |
| `/profile-add` | POST | `name` | `{id, name}`; 400 blank or longer than 32, 409 name taken |
| `/profile-rename` | POST | `id`, `name` | 200; 400, 404, 409 as above |
| `/profile-delete` | POST | `id` | 200 (its scores become unassigned); 404 |
| `/profile-active` | POST | `id` | 200; 404 |

Names are kept on one line (tabs and line breaks become spaces) and trimmed. Deletions and clears are logged. Like the other POST routes, they need a `Content-Length` (browsers send one; with `curl`, use `-d ''`).

## 9. Status and how it was checked

- **macOS**: built and run. The capture was driven with a test table (an empty table with a script sidecar that simulates games through each source in turn: script variables, B2S, UltraDMD scoreboard, saved high scores) and a real ROM table for `pinmame`: two games in a session give two records, a game left in progress gives none, a two-player game gives player 1 to the active profile and player 2 unassigned. The lobby pages were checked from captures of the running application. The web page and its routes were checked with `curl` and in Chrome (headless Chrome for the clear actions, also at phone width).
- **Library coverage**: a run that plays every table of the library unattended (credits, start, ball launches and flips until the game ends) and logs what each source shows is in progress on macOS; its misses will become rules in the bundled `score-rules.json`. The test code it needs is applied to the source only for the run and is not part of the commits (the scripts live in `Doc/test-hooks/` of the workspace).
- **Steam Frame**: not yet run there. To check: B2S tables without a `.directb2s` file, the VR keyboard in the name entry, the result page in the headset.

## 10. Known limits

- Only games whose end is seen are recorded; a table without end of game signal records its games only when the script saves a score or the next game resets the scores, so the last game of a session may be lost for such tables.
- B2S and UltraDMD carry 32-bit scores (the tracker stores 64 bits); modern ROM tables go through the memory maps instead.
- The `highscore` fallback only catches games whose score entered the table's high score list, and cannot tell the players apart.
- The `high_scores` section of the memory maps is not used (only `game_state`).
- The pages are served over plain HTTP, like the rest of the web server; the scores routes are behind the pairing code when it is on.
- The Visual Studio project files were not updated; the CMake build lists the new files (`ScoreStore` and `ScoreTracker` in `VPX_STANDALONE_SOURCES`, `ScoresPage` in `VPX_SOURCES`).

## 11. Files

New:

```
lib/src/ScoreStore.h, ScoreStore.cpp            profiles and scores, profiles.json / scores.json
src/core/ScoreTracker.h, ScoreTracker.cpp       score capture
plugins/plugins/ScoreboardPlugin.h              "Scores","OnScoreboard:1" plugin message
src/ui/live/ingameui/ScoresPage.h, .cpp         profiles page, leaderboards, result page
src/assets/scores/score-rules.json              per table rules (empty for now)
src/assets/pinmame/memmaps/                     Pinball Memory Maps (ODbL / DbCL)
src/assets/web/scores.html, scores.js           Scores page of the web server
docs/Table Scores and Leaderboards.md           this document
```

Modified:

```
src/core/player.h, player.cpp                   tracker created, polled and finished
src/core/ScriptGlobalTable.cpp                  SaveValue forwarded to the tracker
src/core/VPApp.h, VPApp.cpp                     GetScoreStore, m_lastSessionResult
src/core/AppCommands.cpp                        result page or profiles page when the lobby opens
lib/src/TableLibrary.h, .cpp                    FindTable (table of a file)
lib/src/WebServer.h, .cpp                       scores and profiles routes, /info scores flag
plugins/flexdmd/FlexDMD.h, .cpp, UltraDMD.cpp   scoreboard broadcast
plugins/pinmame/PinMAMEPlugin.cpp               bundled memory maps as last resort
src/ui/live/ingameui/InGameUI.cpp               profiles and scores/result pages
src/ui/live/ingameui/TablePickerPage.h, .cpp    player item, Scores tab, top scores, reusable VR keyboard, TextEntryPage keyboard and length
src/assets/web/tables.html, tables.js, vpx.html Scores link, "Show scores"
src/assets/web/styles.css                       scores page
make/CMakeLists_sources.txt                     new sources
```
