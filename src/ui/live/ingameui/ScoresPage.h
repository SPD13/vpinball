// license:GPLv3+

#pragma once

#include "InGameUIPage.h"

#ifdef __STANDALONE__

#include "core/VPApp.h"

namespace VPX::InGameUI
{

// Leaderboards of the tables of the library (see ScoreTracker and VPinballLib::ScoreStore)

// Who is playing: the players of the games (1 to ScoreStore::MAX_PLAYERS), each opening a PlayerProfilePage to give it a profile, which gets its scores.
// Profiles are added here, and managed from the scores page of the web server.
class ProfilesPage final : public InGameUIPage
{
public:
   ProfilesPage();

   void Render(float elapsedS) override;

private:
   void BuildPage() override;

   uint64_t m_revision = 0;
};

// The profile of one player of the games: selecting a profile gives it to the player, selecting the one it has makes the player unassigned
class PlayerProfilePage final : public InGameUIPage
{
public:
   explicit PlayerProfilePage(int slot);

   void Render(float elapsedS) override;

private:
   void BuildPage() override;

   const int m_slot; // 1 to ScoreStore::MAX_PLAYERS
   uint64_t m_revision = 0;
};

// Leaderboard of a table: all its scores, or the best one of each player
class TableScoresPage final : public InGameUIPage
{
public:
   explicit TableScoresPage(const string& uuid);

   void Render(float elapsedS) override;
   void AdjustItem(float direction, bool isInitialPress) override;

private:
   void BuildPage() override;

   const string m_uuid;
   uint64_t m_revision = 0;
};

// Scores of the games just played, and their rank, shown by the lobby when a table is left
class ScoreResultPage final : public InGameUIPage
{
public:
   ScoreResultPage();

   void AdjustItem(float direction, bool isInitialPress) override;

   // The result shown by the page, kept while the lobby runs as the page is recreated each time it is opened
   static void SetResult(const VPApp::SessionResult& result) { s_result = result; }

private:
   void BuildPage() override;

   static VPApp::SessionResult s_result;
};

// The 'Player: <name>' item which opens the profiles page (with the names of the other players which have a profile)
void AddPlayerItem(InGameUIPage& page);

// The 'Scores' tab of the table picker: the tables with scores, opening their leaderboard
void BuildLeaderboardList(InGameUIPage& page);

// The best scores of a table (at most 'maxRows'), with a link to its whole leaderboard when there are more
void AddTopScores(InGameUIPage& page, const string& tableUuid, size_t maxRows);

}

#endif
