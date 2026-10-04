# Table scores and leaderboards

The standalone player records the score of every game played on a table of its library, under the name of the player wearing the headset, and keeps a leaderboard per table. Players are chosen in the lobby; the scores are shown in the lobby after each table and in a Scores tab, and are managed from a browser through the web server (Scores page). This is part of the `steam-frame` work (branch `leaderboards`), written for the Steam Frame but built into every desktop standalone build (macOS, Linux, windows-mingw).

Nothing has to be entered by hand: the score is read from the table while it runs. That is the hard part, because tables keep their scores in very different places. This document explains how it is done, how the scores are stored and shown, and how to fix a table whose scores are not found. Section 12 is the field guide for that last part: the test harness that plays a library unattended, how to read its results, the probing techniques that find where a table keeps its scores and when its games end, and what was learned on the 46 tables of the Steam Frame library (appendix A).

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
| `pinmame` | The RAM of the emulated machine, through the **memory map** of its ROM (`game_state` of the [Pinball Memory Maps](https://github.com/tomlogic/pinmame-nvram-maps) project): one state per player score, `player_count`, `game_over` | ROM tables whose map has a `game_state` section (WPC, System 11, Bally, Stern, Gottlieb...: 383 395 1534 of the  bundled maps,  ROM names in the index) | `game_over` flag of the machine; the player count back to 0; the 'game on' solenoid named by a rule |
| `b2s` | What the script sends to the B2S backglass server: `B2SSetScorePlayer` (scores of players 1 to 6), `B2SSetCanPlay` (players), `B2SSetGameOver` | Most EM and original tables (`If B2SOn Then Controller.B2SSetScorePlayer...`) | Game over light (`B2SSetGameOver`) |
| `ultradmd` | The scores of the UltraDMD scoreboard (`DisplayScoreboard(cPlayers, highlighted, s1..s4, ...)`), as FlexDMD implements UltraDMD | Original tables built on UltraDMD | none |
| `script` | Global variables of the table script with the usual names (below) | EM and original tables without the above | `bGameInPlay`-like or `GameOver`-like variable |
| `highscore` | Values the script saves with `SaveValue` under score-like keys | Last resort, when no other source showed a score in the session | none |

**How each is read:**

- **pinmame.** The PinMAME plugin loads the memory map of the running ROM and exposes its entries as controller *states* of a "Game States" source, described by their path in the map (`game_state\scores\Player 1`, `game_state\player_count\...`, `game_state\game_over\...`). The tracker gets the ROM name from the controller's `gameId` (`pinmame::<rom>`), enumerates the states with `CTLPI_STATE_GET_SRC_MSG`, keeps those three kinds, and reads them through their `GetState` callbacks (any integer, float or text format). The states are dropped and enumerated again whenever the plugin announces a change (`CTLPI_STATE_ON_SRC_CHG_MSG`), and once more when the ROM is known, as its rule may name a solenoid to read. Maps are looked up next to the table, then in the PinMAME folder, then, new, in the maps bundled with the application (section 5).

  The machine tells whether a game runs in three ways, the later overriding the earlier:
  - the `game_over` entry of the map;
  - the **player count**: once the machine has shown players in the session, no player (0, or a value outside 1..6, as maps with an `offset` read 256 when no game runs) means no game. Some `game_over` entries do not work: the one of Bally Evel Knievel reads the game over lamp, which stays off, while its player count goes back to 0 at the end of the game (section 12.6);
  - the **game on solenoid** named by the rule of the ROM (`gameOnSolenoid`, section 4), read from the "VPinMAME Solenoids" states of the plugin, numbered like in the scripts' `SolCallback`. It is the flipper enable output: on while a ball is played, off between balls (drain, bonus count, tilt: up to 6 s seen) and after the game. The game is over once it stays off **20 s** (`GAME_ON_OFF_DELAY`), which also lets the end of game bonus, counted after the flippers went off, into the score. Stern SAM machines have no game over in their maps; PinMAME emulates their 'GameOn' as solenoid 33 (`SAM_FASTFLIPSOL`) from a per-ROM RAM address (`fastflipaddr` in `sam.c`), so it works for the SAM ROMs that PinMAME knows.
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
| `gameOnSolenoid` | PinMAME solenoid that is on while a game is played (section 3.2), for ROMs whose map has no working game over; 33 on Stern SAM |

A variable named by a rule and not found in the script is logged as a warning.

The bundled rules come from the coverage run over the library (section 12):

| Rule | Table | Why |
|---|---|---|
| `{"file": "masters of the universe", "source": "script", "scores": "nvScore", "scoreBase": 1, "players": "PlayersPlayingGame", "inGame": "VpGameInPlay"}` | Masters of the Universe (custom, UltraDMD) | Its scoreboard always says 2 players, and its game flag and scores have names of their own (`Score` is an unrelated scalar) |
| `{"rom": "twd_160h", "gameOnSolenoid": 33}` | The Walking Dead (Stern SAM) | No game over in the map; the player count is not reset at the end of a game |
| `{"rom": "im_185ve", "gameOnSolenoid": 33}` | Iron Man Vault Edition (Stern SAM) | Same, with the map added locally (section 5) |
| `{"rom": "potc_600as", "gameOnSolenoid": 33}` | Pirates of the Caribbean (Stern SAM) | Same |

Each rule carries a `_table` comment field (ignored by the tracker) saying which table it is for and why.

## 5. Bundled memory maps

`src/assets/pinmame/memmaps/` holds `index.json`, `maps/` and `platforms/` of [tomlogic/pinmame-nvram-maps](https://github.com/tomlogic/pinmame-nvram-maps), revision `7e63610`, 5 MB. They are under the **Open Database License** (database) and the **Database Contents License** (contents), Copyright Tom Collins and contributors; both licenses and a README with the attribution are in the folder and must stay with it.

Four maps and two platforms were added locally, found with the probing techniques of section 12.4, and listed in `index.json`:

| File | ROM | Table |
|---|---|---|
| `maps/stern/sam/im_185ve.map.json` | `im_185ve` | Iron Man Vault Edition (Stern SAM) |
| `maps/stern/sam/potc_600as.map.json` | `potc_600as` | Pirates of the Caribbean (Stern SAM) |
| `maps/peyper/sonstwar.map.json`, `platforms/peyper.json` | `sonstwar` | Star Wars (Sonic 1987) |
| `maps/juegos-populares/faeton.map.json`, `platforms/juegos-populares.json` | `faeton` | Faeton (Juegos Populares 1985), played by Ulysse 31 |

Their `_notes` say how each address was found and what was verified (the Faeton scale of 10 is inferred, section 12.6). They are candidates to send upstream.

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
- **Library coverage** (October 2026, macOS, the 46 tables of the Frame's library with the Frame's ROMs and NVRAMs, section 12): every table was played unattended. The first pass recorded 33 tables; the misses were fixed with two tracker changes (the player count and game on solenoid signals, section 3.2), one script rule, three game on rules and four new memory maps. In the final pass **44 of the 46 tables record their games**, by source:

  | Source | Tables |
  |---|---|
  | `pinmame` | 29, of which 4 with a map added locally and 3 with a game on rule |
  | `script` | 14 (one with a rule) |
  | `b2s` | 1 (Apollo) |

  The two others, James Bond 007 and Street Fighter 2, could not be played to the end by the autoplay (the ball is not launched, or gets stuck); their scores and game start are read, their end of game is not verified. Every recorded score was checked against the final value of its source and, where the table shows it, against the screen (Apollo's reels, "GAME OVER" and "MATCH" at the moment of the record). Two-player games were checked on a ROM table (Attack from Mars: 125,301,850 and 286,554,310) and a script table (Blade Runner 2049: 826,820 and 879,100): player 1 goes to the active profile, player 2 to nobody. Appendix A lists every table.

  The test code is applied to the source only for the runs and is not part of the commits (`Doc/test-hooks/` of the workspace, section 12).
- **Steam Frame**: not yet run there. To check: B2S tables without a `.directb2s` file, the VR keyboard in the name entry, the result page in the headset.

## 10. Known limits

- Only games whose end is seen are recorded; a table without end of game signal records its games only when the script saves a score or the next game resets the scores, so the last game of a session may be lost for such tables.
- B2S and UltraDMD carry 32-bit scores (the tracker stores 64 bits); modern ROM tables go through the memory maps instead.
- The `highscore` fallback only catches games whose score entered the table's high score list, and cannot tell the players apart.
- The `high_scores` section of the memory maps is not used (only `game_state`).
- A game ended by the game on solenoid is recorded 20 s after its end (`GAME_ON_OFF_DELAY`); leaving the table within those 20 s discards it, as a game in progress.
- The scale of 10 of Juegos Populares Faeton (Ulysse 31) is inferred, not read on its display (section 12.6).
- James Bond 007 and Street Fighter 2 could not be played to the end unattended: their scores and game start are read, their end of game is not verified.
- Two players could not always be started on the Stern SAM tables by the autoplay; their player 2 address is verified on Iron Man only (players 3 and 4 follow the stride on every local map).
- The pages are served over plain HTTP, like the rest of the web server; the scores routes are behind the pairing code when it is on.
- The Visual Studio project files were not updated; the CMake build lists the new files (`ScoreStore` and `ScoreTracker` in `VPX_STANDALONE_SOURCES`, `ScoresPage` in `VPX_SOURCES`).

## 11. Files

New:

```
lib/src/ScoreStore.h, ScoreStore.cpp            profiles and scores, profiles.json / scores.json
src/core/ScoreTracker.h, ScoreTracker.cpp       score capture
plugins/plugins/ScoreboardPlugin.h              "Scores","OnScoreboard:1" plugin message
src/ui/live/ingameui/ScoresPage.h, .cpp         profiles page, leaderboards, result page
src/assets/scores/score-rules.json              per table rules (section 4)
src/assets/pinmame/memmaps/                     Pinball Memory Maps (ODbL / DbCL), plus 4 maps and 2 platforms added locally (section 5)
src/assets/web/scores.html, scores.js           Scores page of the web server
docs/Table Scores and Leaderboards.md           this document
```

Test tools, outside the repository (workspace `Doc/test-hooks/`, section 12):

```
score-coverage-hooks.py                         temporary test code (autoplay, logs of every source, screenshots); apply / remove
score-coverage.py                               plays the library unattended, one result per table
score-coverage-report.py                        sums up a run
score-probe.py                                  probe memory maps: make, analyze (scores), states (game state)
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

## 12. Probing tables: how to make a new table record its scores

This section is the field guide for the next tables. It explains how the library was tested, how to tell why a table records nothing, the techniques that find where a table keeps its scores and when its games end, and what was learned on the way. Everything here was used on the 46 tables of the Steam Frame library in October 2026 (results in appendix A).

The tools are test code, kept out of the repository in the workspace folder `Doc/test-hooks/` (next to `vpinball/`):

| Tool | What it does |
|---|---|
| `score-coverage-hooks.py apply\|remove` | Adds (or removes) temporary code to `player.h/.cpp` and `ScoreTracker.h/.cpp`, marked `TEMP-COVERAGE-TEST`: autoplay, the log of every source, screenshots. Never committed: run `remove` before committing, and check `git diff`. `player.cpp` is CRLF, the script keeps it so |
| `score-coverage.py` | Plays tables of the library one after the other (or several side by side) without anyone at the controls and collects, per table, the log, a `result.json` and a screenshot |
| `score-coverage-report.py` | Sums up a run: per table, the status, the source, the end of game signal, the score, and the sources that disagree |
| `score-probe.py make\|analyze\|states` | Builds probe memory maps (12.4), finds the score addresses in a probe run, and the bytes of the game state (player count, ball, game flag) |

### 12.1 The test harness

**Unattended play.** With `VPX_TEST_COVERAGE=1`, the hook plays the table through the input actions (`InputAction::SetDirectState`, as if keys were pressed):

1. waits for the machine to boot (`VPX_TEST_START_AT`, 25 s by default: Data East, Sega and Gottlieb System 3 ROMs need 10 to 20 s),
2. adds 2 credits, presses Start once per player (`VPX_TEST_PLAYERS`, presses 2.5 s apart). If no reading of any source changed 12 s after Start, the machine did not take the credits: credits and Start again, up to 3 times (it cannot add a player, as no game started),
3. then, every 5 s, pulls the plunger (`VPX_TEST_PULL_MS`, 1.2 s by default) and flips both flippers. The ball drains after a while, so games end on their own in 1 to 4 minutes; the flips also answer the high score entry,
4. quits 4 s after the tracker recorded a game, or at the timeout (`VPX_TEST_TIMEOUT`, 480 s), with a screenshot of the window (`end.png` or `timeout.png`, in `VPX_TEST_SHOTS`).

**What the log gets** (on top of the normal `[Scores]` lines):

- `COVERAGE-READ`: the reading of **every** source each time one changes, not only of the primary one: `| pinmame [538000,0,0,0] p1 over | b2s [5] gameover | script [0] p1 | sol on | tracker in game`. `p1` is the player count, `in`/`over` the in game signal of the source, `sol` the game on solenoid when a rule names one, `tracker` the state of the state machine. This is what tells *why* a game was or was not recorded.
- `COVERAGE-SAVE`: every `SaveValue` of the script, with its time (high scores saved at game over, or only at exit).
- `COVERAGE-TEST`: start pressed, game started, recorded, quit.

**The driver** (`score-coverage.py <out> [--jobs N] [--only <uuid8 or name>...] [--players N] [--pull-ms uuid8=ms] [--start-at uuid8=s] [--timeout s]`):

- reads the library of the Frame (`frame-tables.json` in `<out>`: a copy of `tables.json` from the Frame's preferences), so the tables keep the Frame's uuids;
- plays them from `~/Documents/VPinballX/FrameTables`, the Mac copy of the Frame's tables folder. A table missing there is downloaded from the Frame for its test and deleted afterwards (`ssh steamos@frame.local "tar -cf - <entry>" | tar -xf -`: the library is 7.3 GB and the Mac had 8 GB free);
- gives each table its own preferences folder (`-PrefPath`): a copy of the Mac settings with a 800x450 window, `PinMAMEPath` on `FrameTables/pinmame` (a copy of the Frame's `pinmame` folder: ROMs, NVRAMs, `alias.txt`), the Frame's `tables.json`, and a profile "Autotest". The tables never touch the user's own scores or settings;
- mutes the sound (`SDL_AUDIO_DRIVER=dummy`);
- writes `<out>/<uuid8>/{vpinball.log, result.json, end.png|timeout.png}` and `<out>/summary.json`.

Three tables side by side take about 45 minutes for the 46 tables. Run it under `caffeinate -dims`.

### 12.2 Reading a result: why does a table record nothing?

Start from the session line of the log (`score-coverage-report.py <out>` prints them all), then look at the `COVERAGE-READ` lines:

| Session line | `COVERAGE-READ` shows | Cause | Fix |
|---|---|---|---|
| `no score source found (rom: xyz)` | nothing, or only `script [0]` | ROM table without memory map (or a map without `game_state`), no B2S, no script variable | A memory map (12.4) |
| `no score source found (rom: none)` | nothing | Original table with unusual variable names, no B2S | Read the script (12.3), then a `scores`/`inGame` rule |
| `scores seen from X but no finished game`, `No end of game signal was found` | scores rising, then frozen; `tracker in game` forever | The source has no in game signal (UltraDMD, SAM maps), or the signal is not used by this table | A script variable (`inGame` rule), or the game on solenoid (12.5) |
| same, with a signal | the signal never changes, e.g. `in` before Start was even pressed | A broken flag in the map | Check the player count (12.6); a rule with another signal |
| `Game ended without its scores being reset at its start` | the first game shows the previous game's scores | Normal at boot (restored NVRAM); a problem only if it happens to every game | The score source is not reset by the table: rule with another source |
| no session line, `quit: timeout` | the score stops rising early, `tracker in game` | The autoplay could not play (ball stuck, not launched) | Look at `timeout.png`; longer `--pull-ms`; not a tracker problem |
| `start never pressed` in the report | | The app crashed or was closed | `~/Library/Logs/DiagnosticReports/VPinballX_BGFX-*.ips` |
| no finished game, `quit: timeout` | **one** line only, the previous game's scores, never changing | The machine never started a game: most likely the credits were lost (a window took the focus while they were added), or the emulation was frozen (the Mac slept) | Run again (the hook retries Start); `caffeinate` |

And always check that the recorded score is right: `score-coverage-report.py` compares it with the last value of every other source; the screenshot often shows the score (backglass reels, "GAME OVER", "MATCH": the timing of the end too).

### 12.3 Technique: read the table script

For original tables and EM tables, the script tells where the scores are. Extract it without opening the editor:

```
VPinballX_BGFX -PrefPath <scratch prefs> -ExtractVBS <table.vpx>      # writes <table>.vbs next to the table
```

Then look for:

- the declarations: `grep -n -i "^\s*dim\b.*\(game\|score\|player\|ball\|tilt\)"`;
- the subs that start and end a game: `ResetForNewGame`, `EndOfGame`, `GameOver`, and the variable they set (`VpGameInPlay = TRUE` / `false`);
- where points are added: `Score(CurrentPlayer) = Score(CurrentPlayer) + points`, `nvScore(...)`;
- what is sent to displays: `B2SSetScorePlayer`, `DisplayScoreboard`, `PuPlayer`, `DMD`...;
- for ROM tables, `Const cGameName = "xyz"` and `LoadVPM ... "sam.vbs"`: the ROM and the platform. Such a table keeps its scores only in the emulated machine; the script will not help.

The rule then names the variables (`scores`, `scoreBase`, `players`, `inGame` or `gameOver`, `source`). Check the base of the arrays: `Dim nvScore(4)` used from 1 is `scoreBase: 1`.

### 12.4 Technique: find the scores in the memory of a ROM (probe maps)

When a ROM has no memory map, the tracker can be pointed at many addresses at once, and the one that behaves like a score is the score. The trick is that the PinMAME plugin loads any map it is given: a *probe map* lists hundreds of candidate addresses as if they were the scores of players 1 to N. The coverage log then prints all their values through the game.

1. **Know the machine.** The platform file (`platforms/*.json`) gives the CPU, the byte order and where the RAM and NVRAM are. Without one (Peyper, Juegos Populares), read the PinMAME driver (`pinmame/src/wpc/<driver>.c`): the `MEMORY_WRITE_START` table names the RAM range saved as NVRAM (`MWA_RAM, &generic_nvram`), and the `INITGAME`/`CORE_GAMEDEFNV` lines which driver a ROM uses. Write a platform file like the others.
2. **Pick the range and the shape.**
   - 32-bit machines (Stern SAM, Spike) keep scores as 4-byte little endian integers. Probe every 4 bytes (`int4`). On SAM, the scores are in the NVRAM, near `0x021109E4`-`0x02110AA0`; probing `0x021108F0`-`0x02110B7C` (164 candidates) found them.
   - 8-bit machines keep digits: packed BCD, or one digit per byte, in RAM. Probe every byte of the RAM (`byte`, 2048 candidates for 2 KB); the analysis decodes the windows. The hook logs such large probes as changes only (the log rolls at 5 MB).
3. **Build the probe folder and play:**

   ```
   score-probe.py make <folder> <rom> <platform> <first> <last> int4|byte [--platform-file <json>] [--player-count 0x02110900]
   cp -R <folder> ~/Documents/VPinballX/FrameTables/pinmame/memmaps
   caffeinate -dims score-coverage.py <out> --players 2 --only <uuid8>
   rm -rf ~/Documents/VPinballX/FrameTables/pinmame/memmaps        # at once: it replaces the bundled maps of every ROM
   ```

   The folder is a full copy of the bundled maps plus the probe, because a `memmaps` folder with an `index.json` in the PinMAME folder hides the bundled maps of **all** ROMs (section 5). Play 2 players, so that player 1 and player 2 can be told apart.
4. **Analyze the scores:** `score-probe.py analyze <out>/<uuid8>/result.json <first> int4|byte [--any-step]` lists the candidates that were reset to 0 at the start of the game and then only rose (in multiples of 10, unless `--any-step`). For `byte` probes, it decodes every window of 3-4 bytes as packed BCD (both byte orders) and of 6-7 bytes as one digit per byte. The score of player 1 rises during the balls of player 1 only, player 2's during theirs. The value before the reset is the previous game's score, which is a check in itself. When several overlapping windows match, dump the bytes around them over time (as in 12.6, Star Wars) to see where each score starts and ends: two scores that touch tell the length.
5. **Find the game state:** `score-probe.py states <result.json> <first> <before> <game from> <game to> <after>` lists the bytes whose values during the game never occur in attract mode: the player count (0, then 1, 2 at each Start, 0 at the end), the current player and ball, a game in progress flag. Give it the time of the Start press (`before`), a window surely in game, and a time surely after the end. A player count or flag that goes back to 0 at the end is the `game_over` of the map (`"encoding": "bool", "invert": true`) and the end of game signal.
6. **Find the scale:** the score in memory is not always the score on the display. Look at the display layout of the driver (`core_tLCDLayout` in `<driver>games.c`): a digit of the display fed by a constant segment, not by the memory, is a fixed trailing 0, so the map needs `"scale": 10`. The map format has `scale` (501 bundled maps use 10); libpinmame applies it.
7. **Write the map**: the addresses of players 3 and 4 follow the stride found between 1 and 2. Write `maps/<maker>/<platform>/<rom>.map.json` with `_notes` saying how each address was found and what was verified, add the ROM to `index.json` (sorted, as the file is), add a new platform file if needed, copy the assets into the app (the Mac build copies them only when it links), and play a normal game: the session line must say `recorded from pinmame` with the score of the screen.

**Byte order of BCD.** libpinmame reverses `int` and `bcd` values on a platform declared `"endian": "little"`. 8-bit ROMs often keep BCD digits most significant first even on a little endian CPU (both Z80 games here): give such entries `"endian": "big"`.

The NVRAM file is a second, offline check: PinMAME writes `<PinMAMEPath>/nvram/<rom>.nv` when the table is left, and on SAM its offset *n* is the address `0x02100000 + n`. The last game's scores are in it: `python3 -c "import struct;d=open('x.nv','rb').read();print(hex(d.find(struct.pack('<I', 594730))))"` gave `0x10a9c`, the address `0x02110A9C` of the TWD map.

### 12.5 Technique: find the end of the games

A score is only recorded when the end of its game is seen, so a source without an end of game signal records nothing (except when the next game starts). In order of preference:

1. **The `game_over` entry of the memory map**, when it works.
2. **The player count of the map** goes back to 0 at the end of the game on many machines (Bally, Data East, Sega, Gottlieb, Stern Whitestar: checked on 16 tables). The tracker uses it by itself (section 3.2). It does *not* on Stern SAM, which keeps the last game's count.
3. **The game on (flipper enable) solenoid** (`gameOnSolenoid` rule). Every machine cuts the flippers when no ball is played. In the `COVERAGE-READ` lines (`sol on`/`sol off`), it is on during each ball, off for 1 to 6 s between balls, and off for good at the end. On SAM it is solenoid 33; for another platform, look in the PinMAME driver for the GameOn output (`core.h`: "GameOn") and in the table script for the `SolCallback` that drives `vpmFlips` or the flipper relay.
4. **A script variable** (`inGame`/`gameOver` rule), for script and UltraDMD tables: the one `EndOfGame` sets.
5. **The backglass game over light** (`B2SSetGameOver`), automatic for B2S tables that use it.

The session line of the log names the signal that ended the game: `end of game from the game over flag of the machine`, `... from no player on the machine`, `... from the game on solenoid of the machine`, `... from a script variable`, `... from the game over light of the backglass`, `... from the scores saved by the script`, `... from the scores reset by the next game`.

Score saves (`SaveValue` at game over) are a fallback signal for tables without any of these, but many tables save only when they are left (`Table1_Exit`), which is too late (Masters of the Universe).

### 12.6 Discoveries

About the tables and the machines:

- **The same original-table template is everywhere.** 13 of the 14 script tables of the library (JP Salas, nFozzy, Marty02, Ext2k conversions and others) use `Score()`, `PlayersPlayingGame` and `bGameInPlay`. The usual names of section 3.2 cover them with no rule.
- **A memory map flag can be dead.** Bally Evel Knievel (`evelknie`, map `as-2518-17/system-rom-20`) reads `game_over` from the game over lamp (`0x20C` mask `0x4`), which never changed in VPX, so its games never ended. Its player count does go back to 0 at the end: hence the player count signal. 49 bundled maps read a lamp for `game_over`; they may need the same.
- **Stern SAM maps have no game over** at all, and their player count (`0x02110900`, shared by every SAM game) is not reset at the end of a game. Their scores are 4-byte integers in the NVRAM from `0x021109E4` on several ROMs (Tron, Iron Man, Pirates; 4-byte stride), elsewhere on others (TWD `0x02110A9C` with an 8-byte stride): the layout is per ROM. PinMAME's 'GameOn' fast flips solenoid 33 is their end of game signal.
- **The end of game bonus comes after the flippers are cut**: Pirates added 100,000 four seconds after its game on solenoid went off. Hence the 20 s delay before a game on signal ends a game: the score is taken at that moment.
- **Machines boot with garbage or with the last game.** At start, maps show the previous game's scores, sometimes "in game" with a player count, for a fraction of a second (Robocop, Playboy, Attack from Mars). The rule "a game must have been seen at zero" (section 3.4) keeps these out.
- **Attract modes blink the scores.** After a game, Bally Evel Knievel shows its last score on all four displays, blinking with zeros. A tracker without end of game signal would read that as games starting and ending: a trusted signal avoids it.
- **Scoreboards do not always tell the players**: the UltraDMD scoreboard of Masters of the Universe always says 2 players. The script's `PlayersPlayingGame` is right.
- **Some tables cannot be played unattended**: James Bond 007 (Gottlieb 1980, timed play) and Street Fighter 2 (ball stuck after the first points) never finished a game with the autoplay. Their scores and game start are read correctly; their end of game was not checked. Others get a ball stuck now and then (The Simpsons once in four games): one failed run of a table that passed before is not a regression, run it again.
- **Single-player machines** ignore the second Start (Apollo, Williams 1967). The Stern SAM tables sometimes ignored a second Start 2.5 s after the first, and took it 0.7 s after: a 2-player autoplay is not guaranteed.
- **Spanish 8-bit machines** (Peyper/Sonic Star Wars, Juegos Populares Faeton/Ulysse 31) have no maps and no platform files upstream. Both keep their whole state in 2 KB of battery-backed RAM: scores as 3 bytes of BCD per player on a 3-byte stride, a player count that goes back to 0 at the end, and 7-digit displays that show the 6 digits of memory followed by a fixed 0 (`scale: 10`). For Peyper the fixed 0 is in the driver (segment 36); for Juegos Populares it is inferred (6 digits in memory, no room for a 7th) and should be checked against the display on the Frame, where the backglass shows it.
- **The memory map probe needs the machine to be the only thing changing**: in attract mode a machine copies a score to every display (Faeton shows one score on the four players), blinks scores, or shows high scores. Only readings taken after the Start press say which address is whose score.

About the test setup (these cost time; avoid them):

- **VPX pauses a table whose window has no focus** (`Player::IsPlaying` checks `m_playfieldWnd->IsFocused()`). Side by side, only the focused table runs, and the others time out. The hook bypasses the focus when `VPX_TEST_COVERAGE` is set.
- **Machines that never start a game.** Once, the three SAM tables of a run never started a game: their readings did not change from the first one. In the one log looked at, the window had lost the focus to a table starting next to it at the very moment the credits were added, so the credits were most likely dropped (not proven: the same tables started at the first try in the next run). Hence the Start retry of the hook, which had not had to fire yet when this was written.
- **The Mac goes to sleep under a long run** even with VPX's display assertion, freezing the emulation (481 s of game in 23 minutes). Run under `caffeinate -dims`. After one wake, libpinmame crashed with `SIGBUS` in `dcs_speedup` (the DCS sound board of WPC-95 machines like Attack from Mars). That is a PinMAME problem to watch on the Frame, which sleeps when the headset is not worn.
- **macOS `rsync` is openrsync**: no `--protect-args`, and spaces in remote paths break it. Use `ssh ... tar`.
- **`pkill -f "VPinballX_BGFX -PrefPath"` kills every test instance**, including another session's: stop your own process by its PID.
- **`pgrep -f <script>` in a wait loop matches the wait loop itself** (its command line contains the name): wait on the PID.
- **Screenshots**: `RenderDevice::OnScreenshotCaptured` erases the file name but leaves `m_screenshotWindow`, so the render loop asks BGFX for a screenshot again every frame afterwards ("Screenshot capture timed out. Requesting it again"), reading `m_screenshotFilename[i]` of an empty vector. Upstream code; the hook takes its screenshot only just before quitting.
- **A `memmaps` folder in the PinMAME folder hides the bundled maps** of every ROM (12.4).
- **The Mac build copies `src/assets` into the app only when the binary links**: after changing only maps or rules, `cmake -E copy_directory src/assets build/VPinballX_BGFX.app/Contents/Resources/assets`.
- **The log rolls over at 5 MB with no backup kept** (`RollingFileAppender` in `Logger.cpp`). A probe of 2048 bytes printed in full at every change filled it, and the start of the game was lost. The hook therefore prints probe readings (more than 64 values) as the changes since the previous line, `pinmame* [index:value,...]`, which `score-probe.py` rebuilds.

## Appendix A. Coverage of the Steam Frame library (October 2026)

Final pass on macOS, one unattended game per table (section 12). "Score" is the score recorded in that game, checked against the last value of the source. Tables without an entry in the last column needed nothing.

| Table | ROM | Source | End of game | Score | Fix |
|---|---|---|---|---:|---|
| Ace Of Speed 1.1 | mousn_l4 | pinmame | the game over flag of the machine | 147,020 |  |
| Addams Family (Bally 1992) | taf_l7 | pinmame | the game over flag of the machine | 1,031,000 |  |
| Apollo (Williams 1967) | - | b2s | the game over light of the backglass | 2,500 |  |
| Attack from Mars Minimal (Bally 1995) | afm_113b | pinmame | the game over flag of the machine | 307,665,730 |  |
| Austin Powers (Stern 2001) | austin | pinmame | the game over flag of the machine | 16,102,470 |  |
| Blade Runner 2049 | - | script | a script variable | 697,000 |  |
| BleachVR | - | script | a script variable | 8,410,590 |  |
| Bond 60th Dr.No | - | script | a script variable | 35,081,110 |  |
| Bram Stoker's Dracula (Williams 1993) | drac_l1 | pinmame | the game over flag of the machine | 4,409,220 |  |
| Evel Knievel (Bally 1977) | evelknie | pinmame | no player on the machine | 11,740 | Player count end signal (its game over lamp flag never changes) |
| Friday the 13th | - | script | a script variable | 119,150 |  |
| Ghostbusters slimer 2.1 | - | script | a script variable | 353,610 |  |
| Godzilla (Sega 1998) | godzilla | pinmame | the game over flag of the machine | 4,356,820 |  |
| Indiana Jones - The Pinball Adventure (Williams 1993) | ij_l7 | pinmame | the game over flag of the machine | 3,561,000 |  |
| Iron Man Minimal (Stern 2010) | im_185ve | pinmame | the game on solenoid of the machine | 1,513,190 | New map (probe) + game on solenoid rule (SAM) |
| James Bond 007 (Gottlieb 1980) | jamesb | - | - | - | Not played to the end by the autoplay (ball not launched): end of game not verified |
| JAWS 50Th Anniversary (Original 2025) | - | script | a script variable | 1,762,100 |  |
| JohnWick (BABAYAGA Pinball edition, 2023) (VR ROOM Minimal) Sphere | - | script | a script variable | 30,030 |  |
| Masters of the Universe-custom (VR ROOM edition) | - | script | a script variable | 220,000 | Script rule (nvScore, VpGameInPlay) |
| Maverick (Data East 1994) | mav_402 | pinmame | the game over flag of the machine | 50,776,570 |  |
| One Piece | - | script | a script variable | 3,532,740 |  |
| Pirates of the Caribbean | potc_600as | pinmame | the game on solenoid of the machine | 2,045,020 | New map (probe) + game on solenoid rule (SAM) |
| Playboy 35th Anniversary (Data East 1989) v4.3 JP Salas, Ext2k VRROOM | play_a24 | pinmame | the game over flag of the machine | 26,300 |  |
| Pokemon Pinball | - | script | a script variable | 550,440 |  |
| Robocop (Data East 1989) | robo_a34 | pinmame | the game over flag of the machine | 91,200 |  |
| Room Rocky (Gottlieb 1982) | rocky | pinmame | the game over flag of the machine | 20,850 |  |
| Scooby Doo 2022 | - | script | a script variable | 3,818,360 |  |
| South Park (Sega 1999) | sprk_103 | pinmame | the game over flag of the machine | 8,821,040 |  |
| Space Cadet (Original 2021) | - | script | a script variable | 163,250 |  |
| Star Wars (Sonic 1987) | sonstwar | pinmame | the game over flag of the machine | 2,277,000 | New platform and map (RAM probe), scale 10 |
| Star Wars Trilogy | swtril43 | pinmame | the game over flag of the machine | 1,410,290 |  |
| Stargate Minimal (Gottlieb 1995) | stargat5 | pinmame | the game over flag of the machine | 10,269,180 |  |
| Starship Troopers (Sega 1997) | startrp2 | pinmame | the game over flag of the machine | 4,171,910 |  |
| Street Fighter 2 | sfight2 | - | - | - | Not played to the end by the autoplay (ball stuck): end of game not verified |
| Super Mario Bros v 1.2 | smb | pinmame | the game over flag of the machine | 1,571,360 |  |
| Super Mario Bros. Mushroom World (Gottlieb 1992) | smbmush | pinmame | the game over flag of the machine | 4,113,330 |  |
| Terminator 3 Rise of the Machines | term3 | pinmame | the game over flag of the machine | 4,873,670 |  |
| The Flintstones | fs_lx5 | pinmame | the game over flag of the machine | 9,102,310 |  |
| The Lost World Jurassic Park (Sega 1997) | jplstw22 | pinmame | the game over flag of the machine | 624,230 |  |
| The Mandalorian | - | script | a script variable | 191,930 |  |
| The Simpsons (Data East 1990) | simp | pinmame | the game over flag of the machine | 127,180 |  |
| The Walking Dead | twd_160h | pinmame | the game on solenoid of the machine | 1,992,150 | Game on solenoid rule (SAM) |
| Tmnt | tmnt_104 | pinmame | the game over flag of the machine | 312,000 |  |
| Twilight Zone | tz_94ch | pinmame | the game over flag of the machine | 4,800,000 |  |
| Ulysse 31 | faeton | pinmame | the game over flag of the machine | 151,980 | New platform and map for the Faeton ROM (RAM probe), scale 10 inferred |
| Wrath of Olympus (Original 2022) | - | script | a script variable | 3,919,270 |  |
