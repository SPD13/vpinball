// license:GPLv3+

#include "core/stdafx.h"
#include "ScoresPage.h"

#ifdef __STANDALONE__

#include "TablePickerPage.h"
#include "lib/src/ScoreStore.h"
#include "lib/src/TableLibrary.h"
#include "ui/live/LiveUI.h"

#include "fonts/IconsForkAwesome.h"

using VPinballLib::Profile;
using VPinballLib::Score;
using VPinballLib::ScoreStore;

namespace VPX::InGameUI
{

static constexpr size_t MAX_LEADERBOARD_ROWS = 50;
static constexpr int RESULT_ROWS_AROUND = 3; // Scores shown above and below the one of the player in the result page
static constexpr ImU32 HIGHLIGHT_COLOR = IM_COL32(255, 200, 0, 255); // Scores of the active player, and of the games just played

static string FormatDate(int64_t secondsSinceEpoch)
{
   const time_t time = static_cast<time_t>(secondsSinceEpoch);
   std::tm localTime {};
#ifdef _WIN32
   localtime_s(&localTime, &time);
#else
   localtime_r(&time, &localTime);
#endif
   char buffer[32];
   return strftime(buffer, sizeof(buffer), "%d %b %Y", &localTime) > 0 ? string(buffer) : string();
}

static string GetPlayerName(const Score& score, const vector<Profile>& profiles)
{
   const auto it = std::ranges::find_if(profiles, [&score](const Profile& profile) { return profile.id == score.profileId; });
   if (it != profiles.end())
      return it->name;
   return score.playerCount > 1 ? std::format("Unassigned (player {})", score.playerSlot) : "Unassigned"s;
}

static string GetTableName(const string& uuid, const string& fallback)
{
   const std::optional<VPinballLib::Table> table = g_app->GetTableLibrary().GetTable(uuid);
   return table ? table->name : fallback;
}

// One line of a leaderboard: rank, score (right aligned), player and date in columns
static void RenderScoreRow(int rank, const Score& score, const string& playerName, bool highlight)
{
   const float x0 = ImGui::GetCursorPosX();
   const float width = ImGui::GetContentRegionAvail().x;
   if (highlight)
      ImGui::PushStyleColor(ImGuiCol_Text, HIGHLIGHT_COLOR);
   ImGui::Text("%d.", rank);
   const string value = ScoreStore::FormatScore(score.score);
   ImGui::SameLine(x0 + width * 0.40f - ImGui::CalcTextSize(value.c_str()).x);
   ImGui::TextUnformatted(value.c_str());
   ImGui::SameLine(x0 + width * 0.46f);
   ImGui::TextUnformatted(playerName.c_str());
   ImGui::SameLine(x0 + width * 0.76f);
   ImGui::TextUnformatted(FormatDate(score.playedAt).c_str());
   if (highlight)
      ImGui::PopStyleColor();
}

static void AddScoreRow(InGameUIPage& page, int rank, const Score& score, const string& playerName, bool highlight)
{
   page.AddItem(std::make_unique<InGameUIItem>("##score-" + score.id, "Played on " + FormatDate(score.playedAt) + (score.playerCount > 1 ? std::format(", player {} of {}", score.playerSlot, score.playerCount) : ""s),
      [rank, score, playerName, highlight](int, const InGameUIItem*) { RenderScoreRow(rank, score, playerName, highlight); }));
}

static void OpenTableScores(Player* player, const string& uuid)
{
   const string path = "scores/" + uuid;
   player->m_liveUI->m_inGameUI.AddPage(path, [uuid]() { return std::make_unique<TableScoresPage>(uuid); });
   player->m_liveUI->m_inGameUI.Navigate(path);
}

void AddPlayerItem(InGameUIPage& page)
{
   ScoreStore& store = g_app->GetScoreStore();
   vector<std::pair<int, string>> players; // The players which have a profile, and its name
   for (int slot = 1; slot <= ScoreStore::MAX_PLAYERS; slot++)
      if (const std::optional<Profile> profile = store.GetPlayerProfile(slot); profile)
         players.emplace_back(slot, profile->name);
   // Short, as the label is not wrapped: the whole list is in the tooltip
   string label, list;
   if (players.empty())
      label = "Player: nobody (choose who is playing)"s;
   else if (players.size() == 1 && players.front().first == 1)
      label = "Player: " + players.front().second;
   else
   {
      label = "Players:"s;
      for (const auto& [slot, name] : players)
      {
         label += std::format("{} {} {}", label.back() == ':' ? "" : ",", slot, name);
         list += std::format("{}Player {}: {}", list.empty() ? "" : "\n", slot, name);
      }
      list += '\n';
   }
   page.AddItem(std::make_unique<InGameUIItem>(label, list + "The scores of the games are recorded for the profile of each player. Select to change who is playing, or to add a profile.", "profiles"s));
}


///////////////////////////////////////////////////////////////////////////////
// Profiles

// Entry of the name of a new profile, which is given to a player of the games
static void OpenNewProfile(Player* player, int slot)
{
   player->m_liveUI->m_inGameUI.AddPage("profiles/new"s,
      [slot]()
      {
         return std::make_unique<TextEntryPage>("New profile"s, ""s,
            [slot](const string& name)
            {
               const std::optional<Profile> profile = g_app->GetScoreStore().AddProfile(name);
               if (profile)
                  g_app->GetScoreStore().SetPlayerProfile(slot, profile->id);
               g_pplayer->m_liveUI->PushNotification(
                  !profile ? "A profile is already named " + name : slot == 1 ? "Playing as " + profile->name : std::format("{} is player {}", profile->name, slot), 3000);
            },
            false, 32);
      });
   player->m_liveUI->m_inGameUI.Navigate("profiles/new"s);
}

ProfilesPage::ProfilesPage()
   : InGameUIPage("Who is playing?"s,
        "The scores of each player of the games are recorded for the profile given to it. Player 1 is the one wearing the headset, and a profile may be given to several players.\n"
        "Profiles are renamed and deleted from the Scores page of a browser (Wi-Fi upload)."s,
        SaveMode::None)
{
}

void ProfilesPage::Render(float elapsedS)
{
   // Profiles may also be changed from a browser
   if (m_revision != g_app->GetScoreStore().GetRevision())
      RequestRebuild();
   InGameUIPage::Render(elapsedS);
}

void ProfilesPage::BuildPage()
{
   ScoreStore& store = g_app->GetScoreStore();
   m_revision = store.GetRevision();
   const vector<Profile> profiles = store.GetProfiles();

   const auto addNewProfile = [this]() { AddItem(std::make_unique<InGameUIItem>("New profile..."s, "Add a profile, given to player 1"s, [this]() { OpenNewProfile(m_player, 1); })); };
   if (profiles.empty())
   {
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "No profile yet: add yours to get your scores in the leaderboards."s));
      addNewProfile();
   }

   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Players"s));
   for (int slot = 1; slot <= ScoreStore::MAX_PLAYERS; slot++)
   {
      const std::optional<Profile> profile = store.GetPlayerProfile(slot);
      const string path = std::format("profiles/player{}", slot);
      m_player->m_liveUI->m_inGameUI.AddPage(path, [slot]() { return std::make_unique<PlayerProfilePage>(slot); });
      AddItem(std::make_unique<InGameUIItem>(std::format("Player {}: {}", slot, profile ? profile->name : "Unassigned"s),
         (profile ? std::format("The scores of player {} are recorded for {}.", slot, profile->name) : std::format("The scores of player {} belong to nobody.", slot))
            + " Select to give this player a profile, or to make it unassigned.",
         path));
   }

   if (!profiles.empty())
      addNewProfile();
}


///////////////////////////////////////////////////////////////////////////////
// Profile of a player

PlayerProfilePage::PlayerProfilePage(int slot)
   : InGameUIPage(std::format("Player {}", slot),
        (slot == 1 ? "The one wearing the headset. "s : ""s) + "The scores of this player are recorded for the selected profile. Select it again to make the player unassigned."s,
        SaveMode::None)
   , m_slot(slot)
{
}

void PlayerProfilePage::Render(float elapsedS)
{
   // Profiles may also be changed from a browser
   if (m_revision != g_app->GetScoreStore().GetRevision())
      RequestRebuild();
   InGameUIPage::Render(elapsedS);
}

void PlayerProfilePage::BuildPage()
{
   ScoreStore& store = g_app->GetScoreStore();
   m_revision = store.GetRevision();
   const vector<Profile> profiles = store.GetProfiles();
   const std::optional<Profile> current = store.GetPlayerProfile(m_slot);
   const vector<Score> scores = store.GetScores();

   // Short, as info labels are not wrapped
   if (profiles.empty())
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "No profile yet"s));
   else
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, current ? "Select " ICON_FK_CHECK " again to unassign"s : std::format("Player {} is unassigned", m_slot)));
   for (const Profile& profile : profiles)
   {
      const bool isCurrent = current && current->id == profile.id;
      const auto count = std::ranges::count_if(scores, [&profile](const Score& score) { return score.profileId == profile.id; });
      string tooltip = std::format("{} score{}", count, count == 1 ? "" : "s");
      if (isCurrent)
         tooltip += std::format("\nSelect to make player {} unassigned", m_slot);
      AddItem(std::make_unique<InGameUIItem>((isCurrent ? ICON_FK_CHECK " "s : "     "s) + profile.name, tooltip,
         [this, id = profile.id, name = profile.name, isCurrent]()
         {
            g_app->GetScoreStore().SetPlayerProfile(m_slot, isCurrent ? string() : id);
            m_player->m_liveUI->PushNotification(isCurrent ? std::format("Player {} is unassigned", m_slot) : m_slot == 1 ? "Playing as " + name : std::format("{} is player {}", name, m_slot), 2000);
            m_player->m_liveUI->m_inGameUI.NavigateBack();
         }));
   }

   AddItem(std::make_unique<InGameUIItem>("New profile..."s, std::format("Add a profile, given to player {}", m_slot), [this]() { OpenNewProfile(m_player, m_slot); }));
}


///////////////////////////////////////////////////////////////////////////////
// Leaderboard of a table

static bool s_bestPerPlayer = false; // View of the leaderboards, kept while the application runs

TableScoresPage::TableScoresPage(const string& uuid)
   : InGameUIPage(GetTableName(uuid, "Leaderboard"s), "Scores recorded at the end of the games played on this table"s, SaveMode::None)
   , m_uuid(uuid)
{
}

void TableScoresPage::Render(float elapsedS)
{
   // Scores may be deleted or given to another player from a browser
   if (m_revision != g_app->GetScoreStore().GetRevision())
      RequestRebuild();
   InGameUIPage::Render(elapsedS);
}

void TableScoresPage::AdjustItem(float direction, bool isInitialPress)
{
   // Actions of this page must not repeat while the button is held
   if (isInitialPress)
      InGameUIPage::AdjustItem(direction, isInitialPress);
}

void TableScoresPage::BuildPage()
{
   ScoreStore& store = g_app->GetScoreStore();
   m_revision = store.GetRevision();
   const vector<Profile> profiles = store.GetProfiles();
   const std::optional<Profile> active = store.GetActiveProfile();
   const vector<Score> scores = s_bestPerPlayer ? store.GetBestByProfile(m_uuid) : store.GetScores(m_uuid);

   // The table may be played from here, unless it is the one running
   VPinballLib::TableLibrary& library = g_app->GetTableLibrary();
   if (const std::optional<VPinballLib::Table> table = library.GetTable(m_uuid); table)
   {
      std::error_code ec;
      if (m_player->m_isLobby || !std::filesystem::equivalent(m_player->m_ptable->m_filename, library.GetFullPath(*table), ec))
         AddItem(std::make_unique<InGameUIItem>("Play"s, ""s,
            [this, tablePath = library.GetFullPath(*table)]()
            {
               g_app->m_nextTableFilename = tablePath;
               m_player->SetCloseState(Player::CS_CLOSE_CAPTURE_SCREENSHOT);
            }));
   }

   AddItem(std::make_unique<InGameUIItem>(s_bestPerPlayer ? "Show: best score of each player"s : "Show: all scores"s, "Switch between all the scores and the best score of each player"s,
      [this]()
      {
         s_bestPerPlayer = !s_bestPerPlayer;
         RequestRebuild();
      }));

   if (active)
   {
      const vector<Score> best = store.GetBestByProfile(m_uuid);
      const auto it = std::ranges::find_if(best, [&active](const Score& score) { return score.profileId == active->id; });
      if (it != best.end())
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, std::format("Best of {}: {}, {} of {} player{}", active->name, ScoreStore::FormatScore(it->score),
            it - best.begin() + 1, best.size(), best.size() == 1 ? "" : "s")));
   }

   if (scores.empty())
   {
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info,
         s_bestPerPlayer && !store.GetScores(m_uuid).empty() ? "No score belongs to a player yet"s : "No score yet: scores are recorded at the end of each game"s));
      return;
   }
   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header,
      scores.size() > MAX_LEADERBOARD_ROWS ? std::format("Top {} of {} scores", MAX_LEADERBOARD_ROWS, scores.size()) : std::format("{} score{}", scores.size(), scores.size() == 1 ? "" : "s")));
   for (size_t i = 0; i < scores.size() && i < MAX_LEADERBOARD_ROWS; i++)
      AddScoreRow(*this, static_cast<int>(i) + 1, scores[i], GetPlayerName(scores[i], profiles), active && scores[i].profileId == active->id);
}


///////////////////////////////////////////////////////////////////////////////
// Result of the last games

VPApp::SessionResult ScoreResultPage::s_result;

ScoreResultPage::ScoreResultPage()
   : InGameUIPage("Game over"s, "Scores of the games you just played, and where they stand among the others"s, SaveMode::None)
{
}

void ScoreResultPage::AdjustItem(float direction, bool isInitialPress)
{
   if (isInitialPress)
      InGameUIPage::AdjustItem(direction, isInitialPress);
}

void ScoreResultPage::BuildPage()
{
   ScoreStore& store = g_app->GetScoreStore();
   const vector<Profile> profiles = store.GetProfiles();
   vector<Score> scores; // Of the session, in the order of the games
   for (const string& id : s_result.scoreIds)
      if (std::optional<Score> score = store.GetScore(id); score)
         scores.push_back(*score);
   if (scores.empty())
   {
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "No score was recorded"s));
      AddItem(std::make_unique<InGameUIItem>("Continue"s, ""s, [this]() { m_player->m_liveUI->m_inGameUI.NavigateBack(); }));
      return;
   }
   const string tableUuid = scores.front().tableUuid;
   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, GetTableName(tableUuid, scores.front().tableName)));

   // The player of the headset is player 1 of each game
   vector<Score> ownScores, otherScores;
   for (const Score& score : scores)
      (score.playerSlot == 1 ? ownScores : otherScores).push_back(score);
   const Score* best = nullptr;
   for (const Score& score : ownScores)
      if (best == nullptr || score.score > best->score)
         best = &score;

   const bool multipleGames = ownScores.size() > 1;
   for (size_t i = 0; i < ownScores.size(); i++)
   {
      const Score& score = ownScores[i];
      const std::optional<VPinballLib::ScoreRank> rank = store.GetRank(score.id);
      string text = (multipleGames ? std::format("Game {}: ", i + 1) : "Your score: "s) + ScoreStore::FormatScore(score.score);
      if (rank)
      {
         text += std::format(", rank {} of {}", rank->rank, rank->total);
         if (rank->profileTotal > 1)
            text += std::format(" ({} of {} players)", rank->profileRank, rank->profileTotal);
      }
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, text));
      if (rank && rank->personalBest && (!multipleGames || &score == best))
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, rank->rank == 1 ? ICON_FK_TROPHY " New record of the table!"s : ICON_FK_STAR " New personal best!"s));
   }
   for (const Score& score : otherScores)
   {
      if (score.profileId.empty())
      {
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info,
            std::format("Player {}: {} (unassigned: give it to a profile from the Scores page of a browser)", score.playerSlot, ScoreStore::FormatScore(score.score))));
         continue;
      }
      const std::optional<VPinballLib::ScoreRank> rank = store.GetRank(score.id);
      string text = std::format("Player {}, {}: {}", score.playerSlot, GetPlayerName(score, profiles), ScoreStore::FormatScore(score.score));
      if (rank)
      {
         text += std::format(", rank {} of {}", rank->rank, rank->total);
         if (rank->personalBest)
            text += rank->rank == 1 ? ", new record of the table!"s : ", new personal best!"s;
      }
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, text));
   }

   // Without profile, the scores of player 1 belong to nobody: they can be given to a new one
   if (!ownScores.empty() && ownScores.front().profileId.empty())
   {
      vector<string> ids;
      for (const Score& score : ownScores)
         ids.push_back(score.id);
      AddItem(std::make_unique<InGameUIItem>("Save under a new profile..."s, "Player 1 had no profile: create one to keep these scores under your name"s,
         [this, ids]()
         {
            m_player->m_liveUI->m_inGameUI.AddPage("profiles/new"s,
               [ids]()
               {
                  return std::make_unique<TextEntryPage>("New profile"s, ""s,
                     [ids](const string& name)
                     {
                        const std::optional<Profile> profile = g_app->GetScoreStore().AddProfile(name);
                        if (!profile)
                        {
                           g_pplayer->m_liveUI->PushNotification("A profile is already named " + name, 3000);
                           return;
                        }
                        g_app->GetScoreStore().SetActiveProfile(profile->id);
                        for (const string& id : ids)
                           g_app->GetScoreStore().AssignScore(id, profile->id);
                        g_pplayer->m_liveUI->PushNotification("Scores saved for " + profile->name, 3000);
                     },
                     false, 32);
               });
            m_player->m_liveUI->m_inGameUI.Navigate("profiles/new"s);
         }));
   }

   // First in the list, to be selected with buttons, as the rows of the leaderboard are not actions
   AddItem(std::make_unique<InGameUIItem>("Continue"s, "Back to the table picker"s, [this]() { m_player->m_liveUI->m_inGameUI.NavigateBack(); }));
   AddItem(std::make_unique<InGameUIItem>("Full leaderboard"s, "All the scores of this table"s, [this, tableUuid]() { OpenTableScores(m_player, tableUuid); }));

   // The leaderboard around the best game
   if (best)
   {
      const vector<Score> leaderboard = store.GetScores(tableUuid);
      const auto found = std::ranges::find_if(leaderboard, [best](const Score& score) { return score.id == best->id; });
      if (found != leaderboard.end())
      {
         const int index = static_cast<int>(found - leaderboard.begin());
         const int first = max(0, index - RESULT_ROWS_AROUND);
         const int last = min(static_cast<int>(leaderboard.size()) - 1, index + RESULT_ROWS_AROUND);
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Leaderboard"s));
         for (int i = first; i <= last; i++)
         {
            const bool isSessionScore = std::ranges::find(s_result.scoreIds, leaderboard[i].id) != s_result.scoreIds.end();
            AddScoreRow(*this, i + 1, leaderboard[i], GetPlayerName(leaderboard[i], profiles), isSessionScore);
         }
      }
   }
}


///////////////////////////////////////////////////////////////////////////////
// Top scores of a table, for the page of the table in the picker

void AddTopScores(InGameUIPage& page, const string& tableUuid, size_t maxRows)
{
   ScoreStore& store = g_app->GetScoreStore();
   const vector<Score> scores = store.GetScores(tableUuid);
   if (scores.empty())
   {
      page.AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Leaderboard"s));
      page.AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "No score yet: be the first!"s));
      return;
   }
   const vector<Profile> profiles = store.GetProfiles();
   const std::optional<Profile> active = store.GetActiveProfile();
   page.AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, scores.size() > maxRows ? std::format("Top {} scores", maxRows) : "Leaderboard"s));
   for (size_t i = 0; i < scores.size() && i < maxRows; i++)
      AddScoreRow(page, static_cast<int>(i) + 1, scores[i], GetPlayerName(scores[i], profiles), active && scores[i].profileId == active->id);
   if (scores.size() > maxRows)
      page.AddItem(std::make_unique<InGameUIItem>(std::format("All {} scores...", scores.size()), "The whole leaderboard of this table"s,
         [&page, tableUuid]() { OpenTableScores(page.m_player, tableUuid); }));
}


///////////////////////////////////////////////////////////////////////////////
// Scores tab of the table picker

void BuildLeaderboardList(InGameUIPage& page)
{
   ScoreStore& store = g_app->GetScoreStore();
   const vector<Profile> profiles = store.GetProfiles();
   const std::optional<Profile> active = store.GetActiveProfile();

   // Tables with scores, the last played first
   struct TableScores
   {
      string uuid;
      string name;
      int64_t lastPlayedAt = 0;
      size_t count = 0;
   };
   vector<TableScores> tables;
   for (const Score& score : store.GetScores())
   {
      auto it = std::ranges::find_if(tables, [&score](const TableScores& table) { return table.uuid == score.tableUuid; });
      if (it == tables.end())
      {
         tables.push_back({ score.tableUuid, GetTableName(score.tableUuid, score.tableName) });
         it = tables.end() - 1;
      }
      it->lastPlayedAt = max(it->lastPlayedAt, score.playedAt);
      it->count++;
   }
   std::ranges::stable_sort(tables, [](const TableScores& a, const TableScores& b) { return a.lastPlayedAt > b.lastPlayedAt; });

   if (tables.empty())
   {
      page.AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "No score yet: the scores are recorded at the end of each game, for the player chosen above."s));
      return;
   }
   page.AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Leaderboards"s));
   for (const TableScores& table : tables)
   {
      const vector<Score> best = store.GetBestByProfile(table.uuid);
      const Score top = store.GetScores(table.uuid).front();
      string label = table.name + ": " + ScoreStore::FormatScore(top.score) + " (" + GetPlayerName(top, profiles) + ')';
      string info = std::format("{} score{}, last played on {}", table.count, table.count == 1 ? "" : "s", FormatDate(table.lastPlayedAt));
      if (active)
      {
         const auto it = std::ranges::find_if(best, [&active](const Score& score) { return score.profileId == active->id; });
         if (it != best.end())
            info += std::format("\nBest of {}: {}, {} of {} player{}", active->name, ScoreStore::FormatScore(it->score), it - best.begin() + 1, best.size(), best.size() == 1 ? "" : "s");
         else
            info += "\nNo score of " + active->name + " yet";
      }
      const string path = "scores/" + table.uuid;
      page.m_player->m_liveUI->m_inGameUI.AddPage(path, [uuid = table.uuid]() { return std::make_unique<TableScoresPage>(uuid); });
      page.AddItem(std::make_unique<InGameUIItem>(label, info, path));
   }
}

}

#endif
