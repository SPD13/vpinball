// license:GPLv3+

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace VPinballLib {

// Someone playing on the device. Scores are attributed to the profile active when the game ended.
struct Profile {
   std::string id;
   std::string name;
   int64_t createdAt = 0;
};

// Final score of one player of a game played on a table of the library
struct Score {
   std::string id;
   std::string tableUuid; // Leaderboards are per table of the library
   std::string tablePath; // Relative to the tables folder, with the name and the ROM: to match the scores again if the table gets another uuid
   std::string tableName;
   std::string rom;
   std::string profileId; // Empty for scores that belong to nobody yet (other players of a multiplayer game)
   int playerSlot = 1; // Player of the game (1 to 4, or more)
   int playerCount = 1; // Players of the game
   int64_t score = 0;
   int64_t playedAt = 0; // End of the game, seconds since epoch
   int durationSec = 0;
   std::string source; // How the score was captured (see ScoreTracker)
};

// Position of a score in the leaderboard of its table
struct ScoreRank {
   int rank = 0; // Among all the scores of the table, from 1
   int total = 0;
   int profileRank = 0; // Among the best score of each profile of the table (0 if the score belongs to nobody)
   int profileTotal = 0;
   bool personalBest = false; // Best score of its profile on the table
};

// Player profiles and scores, kept in profiles.json and scores.json. Thread safe, as the web server uses it from its own thread.
//
// profiles.json: {"activeProfileId":"...","profiles":[{id,name,createdAt}]}
// scores.json: {"scores":[{id,tableUuid,tablePath,tableName,rom,profileId,playerSlot,playerCount,score,playedAt,durationSec,source}]}
class ScoreStore final
{
public:
   enum class LogLevel { Info, Warn, Error };
   using LogCallback = std::function<void(LogLevel level, const std::string& message)>;

   struct Config {
      std::filesystem::path profilesPath;
      std::filesystem::path scoresPath;
      LogCallback log;
   };

   explicit ScoreStore(Config config);

   // Profiles, sorted by name (case insensitive)
   std::vector<Profile> GetProfiles() const;
   std::optional<Profile> GetProfile(const std::string& id) const;
   std::optional<Profile> GetActiveProfile() const;
   bool SetActiveProfile(const std::string& id);
   // Fails on blank names and names already used (case insensitive). The first profile becomes the active one.
   std::optional<Profile> AddProfile(const std::string& name);
   bool RenameProfile(const std::string& id, const std::string& name);
   // The scores of the profile are kept, and belong to nobody afterward
   bool DeleteProfile(const std::string& id);

   // Scores of a table (all tables if empty), best first, ties sorted by date (oldest first, as it was reached first)
   std::vector<Score> GetScores(const std::string& tableUuid = std::string()) const;
   std::optional<Score> GetScore(const std::string& id) const;
   // Ids and dates are given to scores without them
   std::vector<Score> AddScores(std::vector<Score> scores);
   bool DeleteScore(const std::string& id);
   // Deletes the scores of a table (all tables if empty), returns how many were deleted
   size_t ClearScores(const std::string& tableUuid = std::string());
   // Empty profileId: the score belongs to nobody
   bool AssignScore(const std::string& id, const std::string& profileId);
   std::optional<ScoreRank> GetRank(const std::string& scoreId) const;
   // Best score of each profile on a table, best first (scores belonging to nobody are left out)
   std::vector<Score> GetBestByProfile(const std::string& tableUuid) const;

   // Incremented at each change, for UIs that poll
   uint64_t GetRevision() const { return m_revision.load(); }

   static std::string FormatScore(int64_t score); // 12,345,670

private:
   void Log(LogLevel level, const std::string& message) const;
   void Load();
   void SaveProfiles() const;
   void SaveScores() const;
   std::string GenerateId() const;
   static bool IsBetter(const Score& a, const Score& b);
   std::vector<Score> SortedScores(const std::string& tableUuid) const;
   std::vector<Score> BestByProfile(const std::string& tableUuid) const;

   const Config m_config;
   mutable std::mutex m_mutex; // Protects everything below
   std::vector<Profile> m_profiles;
   std::string m_activeProfileId;
   std::vector<Score> m_scores;
   std::atomic<uint64_t> m_revision { 1 };
};

}
