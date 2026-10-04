// license:GPLv3+

#pragma once

#include "lib/src/TableLibrary.h"
#include "plugins/ControllerPlugin.h"

#include <chrono>
#include <map>
#include <set>

class Player;

// Records the scores of the games played on a table of the library, for the leaderboards (see VPinballLib::ScoreStore).
//
// Tables keep their scores in very different places, so the tracker watches several sources and uses the most reliable one
// that shows a score, by order of preference:
// - 'pinmame': the RAM of the emulated machine, read through the memory map of its ROM (game_state of
//   https://github.com/tomlogic/pinmame-nvram-maps, exposed by the PinMAME plugin as 'Game States' controller states)
// - 'b2s': the scores that the script gives to the B2S backglass server (B2SSetScorePlayer...), which most EM and original tables do
// - 'ultradmd': the scoreboard of UltraDMD (see ScoreboardPlugin.h)
// - 'script': global variables of the table script with the usual names (Score(), PlayersPlayingGame, bGameInPlay...)
// - 'highscore': the high scores that the script saves with SaveValue, when nothing else showed a score
//
// A game is recorded when its end is seen: game over flag of the memory map, game in play variable of the script, game over light
// of the backglass, scores saved by the script, or the scores being reset by the next game. A game still running when the table is
// left is not recorded. Tables that the heuristics get wrong get a rule in score-rules.json (bundled in assets/scores, and in the
// preferences folder): 'disable', a 'source' to use, or the names of their script variables.
//
// Everything happens on the main thread, which is also the one of the script and of the plugin messages.
class ScoreTracker final
{
public:
   ScoreTracker(Player* player, const VPinballLib::Table& table);
   ~ScoreTracker();

   void Start(); // After the script was initialized
   void Update(); // Each frame
   void OnSaveValue(const string& key, const string& value); // The script saved a value (SaveValue)
   void Finish(); // When the table is left, before plugins are told so
   void LateFinish(); // Once the script ran its Exit event (where some tables save their high scores)

   const vector<string>& GetRecordedScoreIds() const { return m_recordedIds; }
   const string& GetTableUuid() const { return m_table.uuid; }

private:
   enum class Source { None, PinMAME, B2S, Scoreboard, Script, HighScore };
   static const char* GetSourceName(Source source);

   // What a source shows at a time
   struct Reading
   {
      vector<int64_t> scores; // Player 1 first
      int playerCount = 0; // 0 if unknown
      std::optional<bool> inGame; // Unknown if the source does not tell
      bool HasScore() const;
   };

   struct Rule
   {
      bool disable = false;
      Source source = Source::None;
      string scores, players, inGame, gameOver;
      int scoreBase = -1;
   };

   void Poll();
   void LoadRules(bool romKnown);
   void RefreshRom();
   void RefreshPinMAMEStates();
   std::optional<Reading> ReadPinMAME();
   std::optional<Reading> ReadB2S() const;
   std::optional<Reading> ReadScoreboard() const;
   std::optional<Reading> ReadScript();
   void ProbeScript();
   void UpdateGame(const Reading& reading, std::optional<bool> inGame, const char* endSignal);
   void EndGame(const char* signal);
   void RecordGame(const vector<int64_t>& scores, int playerCount, Source source, int durationSec);
   void LoadSavedValues();
   static bool IsScoreKey(const string& key);

   static void OnStateSrcChanged(const unsigned int msgId, void* userData, void* msgData);
   static void OnB2SStateChange(const unsigned int msgId, void* userData, void* msgData);
   static void OnScoreboard(const unsigned int msgId, void* userData, void* msgData);

   Player* const m_player;
   const VPinballLib::Table m_table;
   string m_rom;
   Rule m_rule;
   bool m_disabled = false;

   unsigned int m_getStateSrcMsgId, m_onStateSrcChangedMsgId, m_getControllersMsgId, m_onB2SStateChangeMsgId, m_onScoreboardMsgId;

   // pinmame
   bool m_pinmameStatesDirty = true;
   vector<StateDef> m_pmScores;
   std::optional<StateDef> m_pmPlayerCount, m_pmGameOver;

   // b2s
   bool m_b2sActive = false;
   int64_t m_b2sScores[6] {};
   int m_b2sPlayers = 0;
   std::optional<bool> m_b2sGameOver; // Game over light (B2SSetGameOver), if the table uses it

   // ultradmd
   std::optional<Reading> m_scoreboard;
   string m_scoreboardSource;

   // script
   struct ScriptVar
   {
      DISPID id;
      string name;
   };
   std::optional<ScriptVar> m_scriptScores; // Array (or single value) of scores
   vector<ScriptVar> m_scriptPlayerScores; // Score1, Score2...
   std::optional<ScriptVar> m_scriptPlayers, m_scriptInGame, m_scriptGameOver;
   bool m_scriptZeroBased = false; // Arrays of scores filled from index 0
   bool m_scriptProbed = false;
   std::optional<ScriptVar> ProbeScriptVar(IDispatch* dispatch, const string& name) const;
   std::optional<CComVariant> ReadScriptVar(const ScriptVar& var) const;

   // highscore: the score values saved by the script, before and during the session
   std::set<int64_t> m_savedScoresBefore;
   vector<int64_t> m_savedScoresNew;

   // Game in progress
   Source m_primary = Source::None;
   bool m_inGame = false;
   bool m_sawNoScore = false; // Without end of game signal, a game starts when scores get from none to some
   std::optional<bool> m_pendingInGame; // Change of the in game signal waiting to be confirmed (signals bounce at ball changes)
   int m_pendingPolls = 0;
   vector<int64_t> m_gameScores;
   int m_gamePlayers = 0;
   std::chrono::steady_clock::time_point m_gameStart;
   std::optional<std::chrono::steady_clock::time_point> m_lastNoScore; // Last time the scores were seen at zero
   bool m_gameFromZero = false; // The scores of the game were seen at zero: they are not the ones of an earlier game
   bool m_scoreSaveHint = false; // The script saved a score while a game was running: likely the end of the game
   bool m_inGameSignalSeen = false;
   bool m_machineSignalTrusted = false, m_scriptSignalTrusted = false, m_b2sSignalTrusted = false;

   std::chrono::steady_clock::time_point m_lastPoll;
   std::set<Source> m_seenSources; // Sources that showed a score, for the session log
   std::map<Source, std::chrono::steady_clock::time_point> m_lastNoScoreBySource; // Last time each source showed no score
   string m_endSignal; // How the end of the last game was found
   int m_discardedGames = 0;
   vector<string> m_recordedIds;
};
