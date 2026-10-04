// license:GPLv3+

#include "core/stdafx.h"
#include "ScoreTracker.h"

#include "core/VPApp.h"
#include "core/player.h"
#include "parts/pintable.h"
#include "lib/src/ScoreStore.h"
#include "plugins/ScoreboardPlugin.h"

#include <cmath>
#include <fstream>
#include <span>

#include <nlohmann/json.hpp>

namespace
{

constexpr auto POLL_PERIOD = std::chrono::milliseconds(250);
constexpr int POLLS_TO_START_GAME = 2; // In game signals bounce (ball changes, tilt...), so changes must last before being trusted
constexpr int POLLS_TO_END_GAME = 6;
constexpr auto GAME_START_ZERO_WINDOW = std::chrono::seconds(3); // Scores must be seen at zero this long before a game starts, or during it
constexpr auto GAME_ON_OFF_DELAY = std::chrono::seconds(20); // The game on solenoid (flipper enable) is also off between balls: the game is over once it stays off

// Usual names of the script variables, the first one found is used
const char* const SCRIPT_SCORES[] = { "Score", "Scores", "PlayerScore", "PlayerScores", "PScore", "PlayersScore" };
const char* const SCRIPT_PLAYER_SCORES[][4] = {
   { "Score1", "Score2", "Score3", "Score4" },
   { "PlayerScore1", "PlayerScore2", "PlayerScore3", "PlayerScore4" },
   { "P1Score", "P2Score", "P3Score", "P4Score" },
};
const char* const SCRIPT_PLAYERS[] = { "PlayersPlayingGame", "PlayersInGame", "NumberOfPlayers", "NumPlayers", "PlayerCount", "TotalPlayers", "Players", "nPlayers" };
const char* const SCRIPT_IN_GAME[] = { "bGameInPlay", "GameInPlay", "bGameInProgress", "GameInProgress", "bGameActive", "GameActive", "bGameStarted", "GameStarted",
   "bInGame", "InGame", "bGameOn", "GameOn", "bGameRunning", "GameRunning" };
const char* const SCRIPT_GAME_OVER[] = { "bGameOver", "GameOver", "IsGameOver" };

string ToLower(string value)
{
   std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   return value;
}

// Scores may be numbers or text ('1,234,560')
std::optional<int64_t> ParseScore(const string& text)
{
   string digits;
   for (const char c : text)
   {
      if (c >= '0' && c <= '9')
         digits += c;
      else if (c == '.' && !digits.empty())
         break; // Decimals, as numbers saved by VBScript may have some
      else if (c != ',' && c != ' ' && c != '\'' && !(c == '-' && digits.empty()))
         return std::nullopt;
   }
   if (digits.empty() || digits.size() > 18)
      return std::nullopt;
   return std::stoll(digits);
}

std::optional<int64_t> VariantToInt64(const VARIANT& value)
{
   CComVariant copy;
   if (SUCCEEDED(VariantChangeType(&copy, &value, 0, VT_R8)))
   {
      const double d = V_R8(&copy);
      if (!std::isfinite(d) || std::abs(d) > 9.0e18)
         return std::nullopt;
      return static_cast<int64_t>(std::llround(d));
   }
   if (SUCCEEDED(VariantChangeType(&copy, &value, 0, VT_BSTR)) && V_BSTR(&copy))
      return ParseScore(MakeString(V_BSTR(&copy)));
   return std::nullopt;
}

std::optional<bool> VariantToBool(const VARIANT& value)
{
   CComVariant copy;
   if (FAILED(VariantChangeType(&copy, &value, 0, VT_BOOL)))
      return std::nullopt;
   return V_BOOL(&copy) != VARIANT_FALSE;
}

std::optional<int64_t> ReadStateInt(const StateDef& state)
{
   if (state.GetState == nullptr)
      return std::nullopt;
   switch (state.dataFormat)
   {
   case CTLPI_STATE_FORMAT_UINT8: { uint8_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_UINT16: { uint16_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_UINT32: { uint32_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_UINT64: { uint64_t v = 0; state.GetState(state.callContext, &v); return static_cast<int64_t>(v); }
   case CTLPI_STATE_FORMAT_INT8: { int8_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_INT16: { int16_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_INT32: { int32_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_INT64: { int64_t v = 0; state.GetState(state.callContext, &v); return v; }
   case CTLPI_STATE_FORMAT_FLOAT: { float v = 0.f; state.GetState(state.callContext, &v); return static_cast<int64_t>(std::llround(v)); }
   case CTLPI_STATE_FORMAT_DOUBLE: { double v = 0.; state.GetState(state.callContext, &v); return static_cast<int64_t>(std::llround(v)); }
   case CTLPI_STATE_FORMAT_STRING:
   {
      const char* v = nullptr;
      state.GetState(state.callContext, &v);
      return v ? ParseScore(v) : std::nullopt;
   }
   default: return std::nullopt;
   }
}

string FormatScores(const vector<int64_t>& scores)
{
   string text;
   for (size_t i = 0; i < scores.size(); i++)
      text += (i ? ", P"s : "P"s) + std::to_string(i + 1) + ' ' + VPinballLib::ScoreStore::FormatScore(scores[i]);
   return text;
}

}

ScoreTracker::ScoreTracker(Player* player, const VPinballLib::Table& table)
   : m_player(player)
   , m_table(table)
{
   const MsgPluginAPI& msgApi = m_player->m_pluginManager.GetMsgAPI();
   const uint32_t endpointId = m_player->m_pluginAPI.GetVPXEndPointId();
   m_getStateSrcMsgId = msgApi.GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_GET_SRC_MSG);
   m_onStateSrcChangedMsgId = msgApi.GetMsgID(CTLPI_NAMESPACE, CTLPI_STATE_ON_SRC_CHG_MSG);
   m_getControllersMsgId = msgApi.GetMsgID(CTLPI_NAMESPACE, CTLPI_CONTROLLERS_GET_MSG);
   m_onB2SStateChangeMsgId = msgApi.GetMsgID("B2S", "OnStateChange:1");
   m_onScoreboardMsgId = msgApi.GetMsgID(SCOREPI_NAMESPACE, SCOREPI_EVT_ON_SCOREBOARD);
   msgApi.SubscribeMsg(endpointId, m_onStateSrcChangedMsgId, OnStateSrcChanged, this);
   msgApi.SubscribeMsg(endpointId, m_onB2SStateChangeMsgId, OnB2SStateChange, this);
   msgApi.SubscribeMsg(endpointId, m_onScoreboardMsgId, OnScoreboard, this);
}

ScoreTracker::~ScoreTracker()
{
   const MsgPluginAPI& msgApi = m_player->m_pluginManager.GetMsgAPI();
   msgApi.UnsubscribeMsg(m_onStateSrcChangedMsgId, OnStateSrcChanged, this);
   msgApi.UnsubscribeMsg(m_onB2SStateChangeMsgId, OnB2SStateChange, this);
   msgApi.UnsubscribeMsg(m_onScoreboardMsgId, OnScoreboard, this);
   msgApi.ReleaseMsgID(m_getStateSrcMsgId);
   msgApi.ReleaseMsgID(m_onStateSrcChangedMsgId);
   msgApi.ReleaseMsgID(m_getControllersMsgId);
   msgApi.ReleaseMsgID(m_onB2SStateChangeMsgId);
   msgApi.ReleaseMsgID(m_onScoreboardMsgId);
}

const char* ScoreTracker::GetSourceName(Source source)
{
   switch (source)
   {
   case Source::PinMAME: return "pinmame";
   case Source::B2S: return "b2s";
   case Source::Scoreboard: return "ultradmd";
   case Source::Script: return "script";
   case Source::HighScore: return "highscore";
   default: return "none";
   }
}

bool ScoreTracker::Reading::HasScore() const
{
   return std::ranges::any_of(scores, [](int64_t score) { return score > 0; });
}

///////////////////////////////////////////////////////////////////////////////
// Session

void ScoreTracker::Start()
{
   PLOGI << "[Scores] Tracking the scores of '" << m_table.name << "' (" << m_table.uuid << ')';
   LoadRules(false);
   LoadSavedValues();
   ProbeScript();
   m_lastPoll = std::chrono::steady_clock::now();
}

void ScoreTracker::Update()
{
   const auto now = std::chrono::steady_clock::now();
   if (now - m_lastPoll < POLL_PERIOD)
      return;
   m_lastPoll = now;
   Poll();
}

void ScoreTracker::Finish()
{
   if (m_disabled)
      return;
   Poll(); // Last look while PinMAME and the script are still running
   if (m_inGame)
   {
      m_discardedGames++;
      PLOGI << "[Scores] The game in progress (" << FormatScores(m_gameScores) << ") is not recorded: it was not finished";
      m_inGame = false;
   }
}

void ScoreTracker::LateFinish()
{
   if (m_disabled)
      return;

   // Tables that showed no score anywhere: the scores they saved as high scores during the session are scores of this session
   if (m_seenSources.empty() && !m_savedScoresNew.empty())
   {
      for (const int64_t score : m_savedScoresNew)
         RecordGame({ score }, 1, Source::HighScore, 0);
   }

   string seen;
   for (const Source source : m_seenSources)
      seen += (seen.empty() ? "" : ", ") + string(GetSourceName(source));
   if (!m_recordedIds.empty())
      PLOGI << "[Scores] Session of '" << m_table.name << "': " << m_recordedIds.size() << " score(s) recorded from " << GetSourceName(m_primary == Source::None ? Source::HighScore : m_primary)
            << (m_endSignal.empty() ? ""s : ", end of game from "s + m_endSignal) << " (rom: " << (m_rom.empty() ? "none"s : m_rom) << ')';
   else if (m_seenSources.empty())
      PLOGW << "[Scores] Session of '" << m_table.name << "': no score source found (rom: " << (m_rom.empty() ? "none"s : m_rom) << "). Add a rule in score-rules.json to record its scores.";
   else
      PLOGW << "[Scores] Session of '" << m_table.name << "': scores seen from " << seen << " but no finished game" << (m_discardedGames ? " (the last one was left in progress)" : "")
            << (m_inGameSignalSeen ? ""s : ". No end of game signal was found for this table: it may need a rule in score-rules.json"s);
}

void ScoreTracker::Poll()
{
   if (m_disabled)
      return;
   if (m_rom.empty())
      RefreshRom();
   if (m_pinmameStatesDirty)
      RefreshPinMAMEStates();

   const std::optional<Reading> pinmame = ReadPinMAME();
   const std::optional<Reading> b2s = ReadB2S();
   const std::optional<Reading> scoreboard = ReadScoreboard();
   const std::optional<Reading> script = ReadScript();
   const auto getReading = [&](Source source) -> const std::optional<Reading>& {
      switch (source)
      {
      case Source::PinMAME: return pinmame;
      case Source::B2S: return b2s;
      case Source::Scoreboard: return scoreboard;
      default: return script;
      }
   };
   for (const Source source : { Source::PinMAME, Source::B2S, Source::Scoreboard, Source::Script })
   {
      const auto& reading = getReading(source);
      if (!reading)
         continue;
      if (!reading->HasScore())
         m_lastNoScoreBySource[source] = std::chrono::steady_clock::now();
      else if (m_seenSources.insert(source).second)
         PLOGI << "[Scores] Scores shown by " << GetSourceName(source) << ": " << FormatScores(reading->scores);
   }

   // The most reliable source that showed a score during the session, unless a rule tells which one to use
   Source primary = m_rule.source;
   if (primary == Source::None)
      for (const Source source : { Source::PinMAME, Source::B2S, Source::Scoreboard, Source::Script })
         if (m_seenSources.contains(source))
         {
            primary = source;
            break;
         }
   if (primary != m_primary)
   {
      if (m_primary != Source::None)
         PLOGI << "[Scores] Scores now taken from " << GetSourceName(primary) << " instead of " << GetSourceName(m_primary);
      m_primary = primary;
      m_inGame = false;
      // Scores shown from the start may be the ones of the previous game (restored by the script), not of a game being played
      const auto lastNoScore = m_lastNoScoreBySource.find(primary);
      m_sawNoScore = lastNoScore != m_lastNoScoreBySource.end();
      m_lastNoScore = m_sawNoScore ? std::optional(lastNoScore->second) : std::nullopt;
      m_pendingInGame.reset();
      m_gameScores.clear();
   }
   if (m_primary == Source::None || m_primary == Source::HighScore)
      return;
   const std::optional<Reading>& reading = getReading(m_primary);
   if (!reading)
      return;

   // End of game signal: the one of the source of the scores first. A signal is only trusted once it said that a game is
   // running, as tables may have variables or lights with the usual names that they do not use.
   struct Signal
   {
      std::optional<bool> inGame;
      bool* trusted;
      const char* name;
   };
   const Signal machine { pinmame ? pinmame->inGame : std::nullopt, &m_machineSignalTrusted, m_pmSignalName };
   const Signal variable { script ? script->inGame : std::nullopt, &m_scriptSignalTrusted, "a script variable" };
   const Signal light { m_b2sGameOver ? std::optional<bool>(!*m_b2sGameOver) : std::nullopt, &m_b2sSignalTrusted, "the game over light of the backglass" };
   vector<Signal> signals;
   switch (m_primary)
   {
   case Source::PinMAME: signals = { machine, variable, light }; break;
   case Source::B2S: signals = { light, variable }; break;
   default: signals = { variable, light }; break;
   }
   std::optional<bool> inGame;
   const char* signal = nullptr;
   for (const Signal& candidate : signals)
   {
      if (candidate.inGame && *candidate.inGame && !*candidate.trusted)
      {
         *candidate.trusted = true;
         PLOGI << "[Scores] End of game signal: " << candidate.name;
      }
      if (!inGame && candidate.inGame && *candidate.trusted)
      {
         inGame = candidate.inGame;
         signal = candidate.name;
      }
   }
   UpdateGame(*reading, inGame, signal);
}

///////////////////////////////////////////////////////////////////////////////
// Games

void ScoreTracker::UpdateGame(const Reading& reading, std::optional<bool> inGame, const char* endSignal)
{
   const bool hasScore = reading.HasScore();
   const auto now = std::chrono::steady_clock::now();
   if (!hasScore)
   {
      m_lastNoScore = now;
      m_gameFromZero |= m_inGame;
   }
   if (inGame)
   {
      m_inGameSignalSeen = true;
      if (*inGame != m_inGame)
      {
         if (m_pendingInGame != inGame)
         {
            m_pendingInGame = inGame;
            m_pendingPolls = 0;
         }
         if (++m_pendingPolls >= (*inGame ? POLLS_TO_START_GAME : POLLS_TO_END_GAME))
         {
            m_pendingInGame.reset();
            if (*inGame)
            {
               m_inGame = true;
               m_gameStart = now;
               // Games start from zero, unlike the state restored with the memory of a machine that was left in the middle of a game
               m_gameFromZero = m_lastNoScore && now - *m_lastNoScore < GAME_START_ZERO_WINDOW;
               m_gameScores.clear();
               m_gamePlayers = 0;
               PLOGD << "[Scores] Game started";
            }
            else
            {
               if (hasScore)
                  m_gameScores = reading.scores;
               EndGame(endSignal);
               return;
            }
         }
      }
      else
         m_pendingInGame.reset();
   }
   else
   {
      // Without signal, a game starts when the scores get from none to some, and ends when the script saves scores, or when the scores
      // are reset by the next game. The scores of the previous game are often shown until then, so they do not start a game.
      if (!hasScore)
      {
         if (m_inGame && !m_gameScores.empty())
            EndGame("the scores reset by the next game");
         m_sawNoScore = true;
         m_scoreSaveHint = false;
         return;
      }
      if (!m_inGame)
      {
         m_scoreSaveHint = false;
         if (!m_sawNoScore)
            return;
         m_inGame = true;
         m_gameStart = now;
         m_gameFromZero = true;
         m_gamePlayers = 0;
         PLOGD << "[Scores] Game started (scores rising)";
      }
   }

   if (!m_inGame)
      return;
   // Latest scores of the game. Scores going back to zero mean that the game was reset (or the scores of the previous game were
   // still shown when the game started).
   if (hasScore)
      m_gameScores = reading.scores;
   else
      m_gameScores.clear();
   if (reading.playerCount > 0)
      m_gamePlayers = reading.playerCount;

   if (!inGame && std::exchange(m_scoreSaveHint, false))
   {
      EndGame("the scores saved by the script");
      m_sawNoScore = false;
   }
}

void ScoreTracker::EndGame(const char* signal)
{
   m_inGame = false;
   const int durationSec = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_gameStart).count());
   if (!std::ranges::any_of(m_gameScores, [](int64_t score) { return score > 0; }))
      ;
   else if (!m_gameFromZero)
      PLOGI << "[Scores] Game ended without its scores being reset at its start (" << FormatScores(m_gameScores) << "): not recorded, as they are from an earlier game";
   else
   {
      m_endSignal = signal ? signal : "";
      RecordGame(m_gameScores, m_gamePlayers, m_primary, durationSec);
   }
   m_gameScores.clear();
   m_gamePlayers = 0;
}

void ScoreTracker::RecordGame(const vector<int64_t>& scores, int playerCount, Source source, int durationSec)
{
   // Players: what the game says, or up to the last one who scored
   int lastScoring = 0;
   for (size_t i = 0; i < scores.size(); i++)
      if (scores[i] > 0)
         lastScoring = static_cast<int>(i) + 1;
   if (lastScoring == 0)
      return;
   const int players = std::clamp(std::max(playerCount, lastScoring), 1, static_cast<int>(scores.size()));

   VPinballLib::ScoreStore& store = g_app->GetScoreStore();
   const std::optional<VPinballLib::Profile> profile = store.GetActiveProfile();
   vector<VPinballLib::Score> records;
   for (int slot = 1; slot <= players; slot++)
   {
      if (scores[slot - 1] <= 0)
         continue;
      VPinballLib::Score record;
      record.tableUuid = m_table.uuid;
      record.tablePath = m_table.path;
      record.tableName = m_table.name;
      record.rom = m_rom;
      // The player wearing the headset is player 1, the scores of the other players belong to nobody until someone gives them to a profile
      record.profileId = (slot == 1 && profile) ? profile->id : string();
      record.playerSlot = slot;
      record.playerCount = players;
      record.score = scores[slot - 1];
      record.durationSec = durationSec;
      record.source = source == Source::Scoreboard && !m_scoreboardSource.empty() ? m_scoreboardSource : GetSourceName(source);
      records.push_back(record);
   }
   for (const VPinballLib::Score& record : store.AddScores(std::move(records)))
      m_recordedIds.push_back(record.id);
   PLOGI << "[Scores] Game recorded from " << GetSourceName(source) << " (" << players << " player(s), " << durationSec << "s): " << FormatScores(scores)
         << (profile ? " (player 1: "s + profile->name + ')' : " (no active profile)"s);
}

///////////////////////////////////////////////////////////////////////////////
// Rules (score-rules.json)
//
// {"rules":[{"file":"part of the table file name","rom":"romname","disable":true,"source":"pinmame|b2s|ultradmd|script|highscore",
//            "scores":"script variable","scoreBase":0,"players":"script variable","inGame":"script variable","gameOver":"script variable",
//            "gameOnSolenoid":33}]}

void ScoreTracker::LoadRules(bool romKnown)
{
   const string fileName = ToLower(m_player->m_ptable->m_filename.filename().string());
   for (const std::filesystem::path& path : { g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets) / "scores" / "score-rules.json",
                                              g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Preferences, "score-rules.json") })
   {
      std::ifstream file(path);
      if (!file.is_open())
         continue;
      try
      {
         const nlohmann::json json = nlohmann::json::parse(file);
         for (const auto& rule : json.value("rules", nlohmann::json::array()))
         {
            const string file = ToLower(rule.value("file", ""s));
            const string rom = ToLower(rule.value("rom", ""s));
            const bool matches = romKnown ? (!rom.empty() && rom == ToLower(m_rom)) : (!file.empty() && fileName.find(file) != string::npos);
            if (!matches)
               continue;
            PLOGI << "[Scores] Using the rule of " << path.filename().string() << " for " << (romKnown ? "ROM " + m_rom : "'" + file + '\'');
            m_rule.disable |= rule.value("disable", false);
            if (const string source = rule.value("source", ""s); !source.empty())
               for (const Source candidate : { Source::PinMAME, Source::B2S, Source::Scoreboard, Source::Script, Source::HighScore })
                  if (source == GetSourceName(candidate))
                     m_rule.source = candidate;
            m_rule.scores = rule.value("scores", m_rule.scores);
            m_rule.players = rule.value("players", m_rule.players);
            m_rule.inGame = rule.value("inGame", m_rule.inGame);
            m_rule.gameOver = rule.value("gameOver", m_rule.gameOver);
            m_rule.scoreBase = rule.value("scoreBase", m_rule.scoreBase);
            m_rule.gameOnSolenoid = rule.value("gameOnSolenoid", m_rule.gameOnSolenoid);
         }
      }
      catch (const std::exception& e)
      {
         PLOGE << "[Scores] Failed to parse " << path.string() << ": " << e.what();
      }
   }
   if (m_rule.disable && !m_disabled)
   {
      m_disabled = true;
      PLOGI << "[Scores] Scores are not recorded for this table (score-rules.json)";
   }
}

///////////////////////////////////////////////////////////////////////////////
// pinmame: game_state of the memory map, exposed by the PinMAME plugin as states whose description is the path of the entry
// in the map, like 'game_state\scores\Player 1' or 'game_state\game_over\Game Over'

void ScoreTracker::RefreshRom()
{
   const MsgPluginAPI* msgApi = &m_player->m_pluginManager.GetMsgAPI();
   for (const ControllerDef& controller : PinballPlugin::Controller::GetCtrlItems<ControllerDef>(msgApi, m_player->m_pluginAPI.GetVPXEndPointId(), m_getControllersMsgId))
   {
      const string gameId = controller.gameId ? controller.gameId : "";
      if (gameId.starts_with("pinmame::"))
      {
         m_rom = gameId.substr(9);
         PLOGI << "[Scores] ROM: " << m_rom;
         LoadRules(true);
         m_pinmameStatesDirty = true; // The rule of the ROM may tell more states to use
         return;
      }
   }
}

void ScoreTracker::OnStateSrcChanged(const unsigned int, void* userData, void*)
{
   ScoreTracker* me = static_cast<ScoreTracker*>(userData);
   me->m_pinmameStatesDirty = true;
   // The states are only valid until this event
   me->m_pmScores.clear();
   me->m_pmPlayerCount.reset();
   me->m_pmGameOver.reset();
   me->m_pmGameOn.reset();
}

void ScoreTracker::RefreshPinMAMEStates()
{
   m_pinmameStatesDirty = false;
   m_pmScores.clear();
   m_pmPlayerCount.reset();
   m_pmGameOver.reset();
   m_pmGameOn.reset();
   const MsgPluginAPI* msgApi = &m_player->m_pluginManager.GetMsgAPI();
   for (const StateSrcId& src : PinballPlugin::Controller::GetCtrlItems<StateSrcId>(msgApi, m_player->m_pluginAPI.GetVPXEndPointId(), m_getStateSrcMsgId))
   {
      for (unsigned int i = 0; i < src.nStates; i++)
      {
         const StateDef& state = src.stateDefs[i];
         if (state.desc == nullptr || state.GetState == nullptr)
            continue;
         const string desc = state.desc;
         if (desc.starts_with("game_state\\scores\\"))
            m_pmScores.push_back(state);
         else if (desc.starts_with("game_state\\player_count\\"))
            m_pmPlayerCount = state;
         else if (desc.starts_with("game_state\\game_over\\"))
            m_pmGameOver = state;
      }
      // Solenoids numbered like in the scripts (SolCallback)
      if (m_rule.gameOnSolenoid > 0 && src.name && string(src.name) == "VPinMAME Solenoids")
         for (unsigned int i = 0; i < src.nStates; i++)
            if (src.stateDefs[i].mappingId == static_cast<uint32_t>(m_rule.gameOnSolenoid) && src.stateDefs[i].GetState)
               m_pmGameOn = src.stateDefs[i];
   }
   if (!m_pmScores.empty())
      PLOGI << "[Scores] Memory map of the ROM: " << m_pmScores.size() << " player score(s)" << (m_pmGameOver ? ", game over flag" : ", no game over flag")
            << (m_pmPlayerCount ? ", player count" : "");
}

std::optional<ScoreTracker::Reading> ScoreTracker::ReadPinMAME()
{
   if (m_pmScores.empty())
      return std::nullopt;
   Reading reading;
   for (const StateDef& state : m_pmScores)
      reading.scores.push_back(ReadStateInt(state).value_or(0));
   if (m_pmPlayerCount)
   {
      // Maps with an offset give 256 when no game is played
      const int64_t players = ReadStateInt(*m_pmPlayerCount).value_or(0);
      reading.playerCount = (players >= 1 && players <= SCOREPI_MAX_PLAYERS) ? static_cast<int>(players) : 0;
   }
   if (m_pmGameOver)
      if (const std::optional<int64_t> gameOver = ReadStateInt(*m_pmGameOver))
         reading.inGame = *gameOver == 0;
   // Some game over flags do not work (the game over lamp of Bally Evel Knievel stays off): machines that tell their players only
   // have players during a game
   m_pmSignalName = "the game over flag of the machine";
   if (reading.playerCount > 0)
      m_pmPlayersSeen = true;
   else if (m_pmPlayersSeen && reading.inGame == true)
   {
      reading.inGame = false;
      m_pmSignalName = "no player on the machine";
   }
   if (m_pmGameOn)
   {
      m_pmSignalName = "the game on solenoid of the machine";
      const auto now = std::chrono::steady_clock::now();
      if (ReadStateInt(*m_pmGameOn).value_or(0) != 0)
         m_pmGameOnLastOn = now;
      reading.inGame = m_pmGameOnLastOn && now - *m_pmGameOnLastOn < GAME_ON_OFF_DELAY;
   }
   return reading;
}

///////////////////////////////////////////////////////////////////////////////
// b2s: events broadcasted by the B2S plugins for B2SSetScorePlayer ('C') and B2SSetData ('E', with the ids of the helpers like B2SSetGameOver)

void ScoreTracker::OnB2SStateChange(const unsigned int, void* userData, void* msgData)
{
   ScoreTracker* me = static_cast<ScoreTracker*>(userData);
   const struct B2SPluginEvent
   {
      uint8_t type;
      int32_t index;
      int32_t value;
   }* event = static_cast<const B2SPluginEvent*>(msgData);
   if (event->type == 'C' && event->index >= 1 && event->index <= 6)
   {
      me->m_b2sActive = true;
      me->m_b2sScores[event->index - 1] = event->value;
   }
   else if (event->type == 'E')
   {
      switch (event->index)
      {
      case 31: // B2SSetCanPlay: players of the game
         if (event->value >= 1 && event->value <= 6)
            me->m_b2sPlayers = event->value;
         break;
      case 35: // B2SSetGameOver
         me->m_b2sGameOver = event->value != 0;
         break;
      default: break;
      }
   }
}

std::optional<ScoreTracker::Reading> ScoreTracker::ReadB2S() const
{
   if (!m_b2sActive)
      return std::nullopt;
   Reading reading;
   int count = 0;
   for (int i = 0; i < 6; i++)
      if (m_b2sScores[i] != 0)
         count = i + 1;
   reading.scores.assign(m_b2sScores, m_b2sScores + std::max(count, 1));
   reading.playerCount = m_b2sPlayers;
   return reading;
}

///////////////////////////////////////////////////////////////////////////////
// ultradmd: scoreboard shown by a plugin (see ScoreboardPlugin.h)

void ScoreTracker::OnScoreboard(const unsigned int, void* userData, void* msgData)
{
   ScoreTracker* me = static_cast<ScoreTracker*>(userData);
   const ScoreboardEvent* event = static_cast<const ScoreboardEvent*>(msgData);
   Reading reading;
   const int nScores = std::clamp(event->nScores, 0, SCOREPI_MAX_PLAYERS);
   reading.scores.assign(event->scores, event->scores + nScores);
   // Scoreboards show the scores of the players of the game only
   if (event->nPlayers > 0 && event->nPlayers < nScores)
      reading.scores.resize(event->nPlayers);
   reading.playerCount = event->nPlayers;
   me->m_scoreboard = reading;
   if (event->source)
      me->m_scoreboardSource = event->source;
}

std::optional<ScoreTracker::Reading> ScoreTracker::ReadScoreboard() const { return m_scoreboard; }

///////////////////////////////////////////////////////////////////////////////
// script: global variables of the table script

std::optional<ScoreTracker::ScriptVar> ScoreTracker::ProbeScriptVar(IDispatch* dispatch, const string& name) const
{
   const wstring wname = MakeWString(name);
   LPOLESTR names = const_cast<LPOLESTR>(wname.c_str());
   DISPID id;
   if (FAILED(dispatch->GetIDsOfNames(IID_NULL, &names, 1, 0, &id)))
      return std::nullopt;
   // Functions are also found: only variables can be read as properties
   ScriptVar var { id, name };
   if (!ReadScriptVar(var))
      return std::nullopt;
   return var;
}

std::optional<CComVariant> ScoreTracker::ReadScriptVar(const ScriptVar& var) const
{
   if (m_player->m_scriptInterpreter == nullptr)
      return std::nullopt;
   CComPtr<IDispatch> dispatch;
   m_player->m_scriptInterpreter->GetScriptDispatch(&dispatch);
   if (dispatch == nullptr)
      return std::nullopt;
   DISPPARAMS noArgs = { nullptr, nullptr, 0, 0 };
   CComVariant result;
   if (FAILED(dispatch->Invoke(var.id, IID_NULL, 0, DISPATCH_PROPERTYGET, &noArgs, &result, nullptr, nullptr)))
      return std::nullopt;
   return result;
}

void ScoreTracker::ProbeScript()
{
   m_scriptProbed = true;
   if (m_player->m_scriptInterpreter == nullptr)
      return;
   CComPtr<IDispatch> dispatch;
   m_player->m_scriptInterpreter->GetScriptDispatch(&dispatch);
   if (dispatch == nullptr)
      return;
   const auto probeFirst = [&](const string& ruleName, std::span<const char* const> names) -> std::optional<ScriptVar> {
      if (!ruleName.empty())
      {
         std::optional<ScriptVar> var = ProbeScriptVar(dispatch, ruleName);
         if (!var)
            PLOGW << "[Scores] Script variable '" << ruleName << "' of score-rules.json not found";
         return var;
      }
      for (const char* name : names)
         if (std::optional<ScriptVar> var = ProbeScriptVar(dispatch, name))
            return var;
      return std::nullopt;
   };

   // Scores: preferably an array of scores, otherwise a variable per player, otherwise a single score
   std::optional<ScriptVar> single;
   if (!m_rule.scores.empty())
      m_scriptScores = probeFirst(m_rule.scores, std::span<const char* const>());
   else
   {
      for (const char* name : SCRIPT_SCORES)
         if (std::optional<ScriptVar> var = ProbeScriptVar(dispatch, name))
         {
            if (const std::optional<CComVariant> value = ReadScriptVar(*var); value && V_ISARRAY(&*value))
            {
               m_scriptScores = var;
               break;
            }
            if (!single)
               single = var;
         }
      if (!m_scriptScores)
         for (const auto& names : SCRIPT_PLAYER_SCORES)
         {
            for (const char* name : names)
               if (std::optional<ScriptVar> var = ProbeScriptVar(dispatch, name))
                  m_scriptPlayerScores.push_back(*var);
               else
                  break;
            if (!m_scriptPlayerScores.empty())
               break;
         }
      if (!m_scriptScores && m_scriptPlayerScores.empty())
         m_scriptScores = single;
   }
   m_scriptPlayers = probeFirst(m_rule.players, SCRIPT_PLAYERS);
   if (m_rule.gameOver.empty())
      m_scriptInGame = probeFirst(m_rule.inGame, SCRIPT_IN_GAME);
   if (!m_scriptInGame)
      m_scriptGameOver = probeFirst(m_rule.gameOver, SCRIPT_GAME_OVER);
   if (m_rule.scoreBase >= 0)
      m_scriptZeroBased = m_rule.scoreBase == 0;

   string found;
   const auto add = [&found](const char* what, const std::optional<ScriptVar>& var) {
      if (var)
         found += (found.empty() ? ""s : ", "s) + what + " '" + var->name + '\'';
   };
   add("scores", m_scriptScores);
   if (!m_scriptPlayerScores.empty())
      add("scores", m_scriptPlayerScores.front());
   add("players", m_scriptPlayers);
   add("in game", m_scriptInGame);
   add("game over", m_scriptGameOver);
   if (!found.empty())
      PLOGI << "[Scores] Script variables: " << found;
}

std::optional<ScoreTracker::Reading> ScoreTracker::ReadScript()
{
   if (!m_scriptScores && m_scriptPlayerScores.empty() && !m_scriptPlayers && !m_scriptInGame && !m_scriptGameOver)
      return std::nullopt;
   Reading reading;
   if (m_scriptScores)
   {
      if (const std::optional<CComVariant> value = ReadScriptVar(*m_scriptScores))
      {
         if (V_ISARRAY(&*value))
         {
            SAFEARRAY* array = (V_VT(&*value) & VT_BYREF) ? *V_ARRAYREF(&*value) : V_ARRAY(&*value);
            VARTYPE type = VT_EMPTY;
            LONG lower = 0, upper = -1;
            if (array && SafeArrayGetDim(array) == 1 && SUCCEEDED(SafeArrayGetVartype(array, &type)) && type == VT_VARIANT
               && SUCCEEDED(SafeArrayGetLBound(array, 1, &lower)) && SUCCEEDED(SafeArrayGetUBound(array, 1, &upper)))
            {
               vector<int64_t> elements;
               for (LONG i = lower; i <= upper && elements.size() < SCOREPI_MAX_PLAYERS + 1; i++)
               {
                  CComVariant element;
                  elements.push_back(SUCCEEDED(SafeArrayGetElement(array, &i, &element)) ? VariantToInt64(element).value_or(0) : 0);
               }
               // Arrays are often declared with one more entry to be used from index 1 (Dim Score(4)): the first entry tells which
               if (m_rule.scoreBase < 0 && !elements.empty() && elements[0] > 0 && !m_scriptZeroBased)
               {
                  m_scriptZeroBased = true;
                  PLOGI << "[Scores] Script scores '" << m_scriptScores->name << "' start at index 0";
               }
               const size_t first = (m_scriptZeroBased || elements.size() < 2) ? 0 : 1;
               reading.scores.assign(elements.begin() + first, elements.end());
               if (reading.scores.size() > SCOREPI_MAX_PLAYERS)
                  reading.scores.resize(SCOREPI_MAX_PLAYERS);
            }
         }
         else if (const std::optional<int64_t> score = VariantToInt64(*value))
            reading.scores.push_back(*score);
      }
   }
   else
      for (const ScriptVar& var : m_scriptPlayerScores)
      {
         const std::optional<CComVariant> value = ReadScriptVar(var);
         reading.scores.push_back(value ? VariantToInt64(*value).value_or(0) : 0);
      }
   if (m_scriptPlayers)
      if (const std::optional<CComVariant> value = ReadScriptVar(*m_scriptPlayers))
         reading.playerCount = static_cast<int>(std::clamp<int64_t>(VariantToInt64(*value).value_or(0), 0, SCOREPI_MAX_PLAYERS));
   if (m_scriptInGame)
   {
      if (const std::optional<CComVariant> value = ReadScriptVar(*m_scriptInGame))
         reading.inGame = VariantToBool(*value);
   }
   else if (m_scriptGameOver)
   {
      if (const std::optional<CComVariant> value = ReadScriptVar(*m_scriptGameOver))
         if (const std::optional<bool> gameOver = VariantToBool(*value))
            reading.inGame = !*gameOver;
   }
   return reading;
}

///////////////////////////////////////////////////////////////////////////////
// highscore: values saved by the script with SaveValue (VPReg.ini)

bool ScoreTracker::IsScoreKey(const string& key)
{
   const string lower = ToLower(key);
   for (const char* excluded : { "name", "init", "letter", "char" })
      if (lower.find(excluded) != string::npos)
         return false;
   return lower.find("score") != string::npos || lower.find("hiscore") != string::npos || lower.starts_with("hs") || lower.find("highsc") != string::npos;
}

void ScoreTracker::LoadSavedValues()
{
   const std::filesystem::path path = g_app->m_fileLocator.GetTablePath(m_player->m_ptable, FileLocator::TableSubFolder::User, false) / "VPReg.ini";
   mINI::INIStructure ini;
   if (!mINI::INIFile(path).read(ini))
      return;
   for (const auto& [section, values] : ini)
      for (const auto& [key, value] : values)
         if (IsScoreKey(key))
            if (const std::optional<int64_t> score = ParseScore(value))
               m_savedScoresBefore.insert(*score);
}

void ScoreTracker::OnSaveValue(const string& key, const string& value)
{
   if (m_disabled || !IsScoreKey(key))
      return;
   const std::optional<int64_t> score = ParseScore(value);
   if (!score || *score < 100) // Small values are rather letters of initials, counts...
      return;
   PLOGD << "[Scores] Score saved by the script: " << key << '=' << value;
   if (m_inGame)
      m_scoreSaveHint = true;
   if (!m_savedScoresBefore.contains(*score) && std::ranges::find(m_savedScoresNew, *score) == m_savedScoresNew.end())
      m_savedScoresNew.push_back(*score);
}
