// license:GPLv3+

#include "ScoreStore.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <random>
#include <set>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using std::string;
using namespace std::string_literals;

namespace VPinballLib {

namespace {

int64_t Now() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

string ToLower(string value)
{
   std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
   return value;
}

string Trim(const string& value)
{
   const size_t start = value.find_first_not_of(" \t\r\n");
   if (start == string::npos)
      return string();
   const size_t end = value.find_last_not_of(" \t\r\n");
   return value.substr(start, end - start + 1);
}

}

ScoreStore::ScoreStore(Config config)
   : m_config(std::move(config))
{
   Load();
}

void ScoreStore::Log(LogLevel level, const string& message) const
{
   if (m_config.log)
      m_config.log(level, message);
}

///////////////////////////////////////////////////////////////////////////////
// Persistence

void ScoreStore::Load()
{
   std::lock_guard lock(m_mutex);
   if (std::ifstream file(m_config.profilesPath); file.is_open()) {
      try {
         const nlohmann::json json = nlohmann::json::parse(file);
         m_activeProfileId = json.value("activeProfileId", ""s);
         for (const auto& entry : json.at("profiles")) {
            Profile profile;
            profile.id = entry.value("id", ""s);
            profile.name = entry.value("name", ""s);
            profile.createdAt = entry.value("createdAt", int64_t(0));
            if (!profile.id.empty())
               m_profiles.push_back(profile);
         }
      } catch (const std::exception& e) {
         Log(LogLevel::Error, "Failed to parse " + m_config.profilesPath.string() + ": " + e.what());
      }
   }
   if (std::ranges::none_of(m_profiles, [this](const Profile& profile) { return profile.id == m_activeProfileId; }))
      m_activeProfileId = m_profiles.empty() ? string() : m_profiles.front().id;

   if (std::ifstream file(m_config.scoresPath); file.is_open()) {
      try {
         const nlohmann::json json = nlohmann::json::parse(file);
         for (const auto& entry : json.at("scores")) {
            Score score;
            score.id = entry.value("id", ""s);
            score.tableUuid = entry.value("tableUuid", ""s);
            score.tablePath = entry.value("tablePath", ""s);
            score.tableName = entry.value("tableName", ""s);
            score.rom = entry.value("rom", ""s);
            score.profileId = entry.value("profileId", ""s);
            score.playerSlot = entry.value("playerSlot", 1);
            score.playerCount = entry.value("playerCount", 1);
            score.score = entry.value("score", int64_t(0));
            score.playedAt = entry.value("playedAt", int64_t(0));
            score.durationSec = entry.value("durationSec", 0);
            score.source = entry.value("source", ""s);
            if (!score.id.empty() && !score.tableUuid.empty())
               m_scores.push_back(score);
         }
      } catch (const std::exception& e) {
         Log(LogLevel::Error, "Failed to parse " + m_config.scoresPath.string() + ": " + e.what());
      }
   }
}

// Write then rename, so that a crash or a full disk never leaves a truncated file
static bool SaveJson(const fs::path& path, const nlohmann::ordered_json& content, string& error)
{
   std::error_code ec;
   fs::create_directories(path.parent_path(), ec);
   fs::path tempPath = path;
   tempPath += ".tmp";
   {
      std::ofstream file(tempPath, std::ios::trunc);
      file << content.dump(2) << '\n';
      if (!file) {
         error = "Failed to write " + tempPath.string();
         return false;
      }
   }
   fs::rename(tempPath, path, ec);
   if (ec) {
      error = "Failed to save " + path.string() + ": " + ec.message();
      return false;
   }
   return true;
}

void ScoreStore::SaveProfiles() const
{
   nlohmann::ordered_json profiles = nlohmann::ordered_json::array();
   for (const Profile& profile : m_profiles)
      profiles.push_back({ { "id", profile.id }, { "name", profile.name }, { "createdAt", profile.createdAt } });
   string error;
   if (!SaveJson(m_config.profilesPath, { { "activeProfileId", m_activeProfileId }, { "profiles", profiles } }, error))
      Log(LogLevel::Error, error);
}

void ScoreStore::SaveScores() const
{
   nlohmann::ordered_json scores = nlohmann::ordered_json::array();
   for (const Score& score : m_scores)
      scores.push_back({ { "id", score.id }, { "tableUuid", score.tableUuid }, { "tablePath", score.tablePath }, { "tableName", score.tableName }, { "rom", score.rom },
         { "profileId", score.profileId }, { "playerSlot", score.playerSlot }, { "playerCount", score.playerCount }, { "score", score.score },
         { "playedAt", score.playedAt }, { "durationSec", score.durationSec }, { "source", score.source } });
   string error;
   if (!SaveJson(m_config.scoresPath, { { "scores", scores } }, error))
      Log(LogLevel::Error, error);
}

string ScoreStore::GenerateId() const
{
   static std::mt19937_64 generator { std::random_device {}() };
   static constexpr char HEX[] = "0123456789abcdef";
   for (;;) {
      string id;
      uint64_t value = generator();
      for (int i = 0; i < 16; i++, value >>= 4)
         id += HEX[value & 0xF];
      if (std::ranges::none_of(m_profiles, [&id](const Profile& profile) { return profile.id == id; })
         && std::ranges::none_of(m_scores, [&id](const Score& score) { return score.id == id; }))
         return id;
   }
}

///////////////////////////////////////////////////////////////////////////////
// Profiles

std::vector<Profile> ScoreStore::GetProfiles() const
{
   std::vector<Profile> profiles;
   {
      std::lock_guard lock(m_mutex);
      profiles = m_profiles;
   }
   std::ranges::sort(profiles, [](const Profile& a, const Profile& b) { return ToLower(a.name) < ToLower(b.name); });
   return profiles;
}

std::optional<Profile> ScoreStore::GetProfile(const string& id) const
{
   std::lock_guard lock(m_mutex);
   const auto it = std::ranges::find_if(m_profiles, [&id](const Profile& profile) { return profile.id == id; });
   return it == m_profiles.end() ? std::nullopt : std::optional<Profile>(*it);
}

std::optional<Profile> ScoreStore::GetActiveProfile() const
{
   std::lock_guard lock(m_mutex);
   const auto it = std::ranges::find_if(m_profiles, [this](const Profile& profile) { return profile.id == m_activeProfileId; });
   return it == m_profiles.end() ? std::nullopt : std::optional<Profile>(*it);
}

bool ScoreStore::SetActiveProfile(const string& id)
{
   std::lock_guard lock(m_mutex);
   if (std::ranges::none_of(m_profiles, [&id](const Profile& profile) { return profile.id == id; }))
      return false;
   if (m_activeProfileId != id) {
      m_activeProfileId = id;
      SaveProfiles();
      m_revision++;
   }
   return true;
}

std::optional<Profile> ScoreStore::AddProfile(const string& name)
{
   const string trimmed = Trim(name);
   if (trimmed.empty())
      return std::nullopt;
   std::lock_guard lock(m_mutex);
   if (std::ranges::any_of(m_profiles, [&trimmed](const Profile& profile) { return ToLower(profile.name) == ToLower(trimmed); }))
      return std::nullopt;
   Profile profile { GenerateId(), trimmed, Now() };
   m_profiles.push_back(profile);
   if (m_activeProfileId.empty())
      m_activeProfileId = profile.id;
   SaveProfiles();
   m_revision++;
   Log(LogLevel::Info, "Profile added: " + trimmed);
   return profile;
}

bool ScoreStore::RenameProfile(const string& id, const string& name)
{
   const string trimmed = Trim(name);
   if (trimmed.empty())
      return false;
   std::lock_guard lock(m_mutex);
   if (std::ranges::any_of(m_profiles, [&](const Profile& profile) { return profile.id != id && ToLower(profile.name) == ToLower(trimmed); }))
      return false;
   const auto it = std::ranges::find_if(m_profiles, [&id](const Profile& profile) { return profile.id == id; });
   if (it == m_profiles.end())
      return false;
   it->name = trimmed;
   SaveProfiles();
   m_revision++;
   return true;
}

bool ScoreStore::DeleteProfile(const string& id)
{
   std::lock_guard lock(m_mutex);
   const auto it = std::ranges::find_if(m_profiles, [&id](const Profile& profile) { return profile.id == id; });
   if (it == m_profiles.end())
      return false;
   Log(LogLevel::Info, "Profile deleted: " + it->name);
   m_profiles.erase(it);
   if (m_activeProfileId == id)
      m_activeProfileId = m_profiles.empty() ? string() : m_profiles.front().id;
   bool scoresChanged = false;
   for (Score& score : m_scores)
      if (score.profileId == id) {
         score.profileId.clear();
         scoresChanged = true;
      }
   SaveProfiles();
   if (scoresChanged)
      SaveScores();
   m_revision++;
   return true;
}

///////////////////////////////////////////////////////////////////////////////
// Scores

bool ScoreStore::IsBetter(const Score& a, const Score& b)
{
   if (a.score != b.score)
      return a.score > b.score;
   if (a.playedAt != b.playedAt)
      return a.playedAt < b.playedAt;
   return a.id < b.id;
}

std::vector<Score> ScoreStore::SortedScores(const string& tableUuid) const
{
   std::vector<Score> scores;
   for (const Score& score : m_scores)
      if (tableUuid.empty() || score.tableUuid == tableUuid)
         scores.push_back(score);
   std::ranges::sort(scores, IsBetter);
   return scores;
}

std::vector<Score> ScoreStore::BestByProfile(const string& tableUuid) const
{
   std::vector<Score> best;
   std::set<string> seen;
   for (const Score& score : SortedScores(tableUuid))
      if (!score.profileId.empty() && seen.insert(score.profileId).second)
         best.push_back(score);
   return best;
}

std::vector<Score> ScoreStore::GetScores(const string& tableUuid) const
{
   std::lock_guard lock(m_mutex);
   return SortedScores(tableUuid);
}

std::optional<Score> ScoreStore::GetScore(const string& id) const
{
   std::lock_guard lock(m_mutex);
   const auto it = std::ranges::find_if(m_scores, [&id](const Score& score) { return score.id == id; });
   return it == m_scores.end() ? std::nullopt : std::optional<Score>(*it);
}

std::vector<Score> ScoreStore::AddScores(std::vector<Score> scores)
{
   std::lock_guard lock(m_mutex);
   for (Score& score : scores) {
      if (score.id.empty())
         score.id = GenerateId();
      if (score.playedAt == 0)
         score.playedAt = Now();
      m_scores.push_back(score);
   }
   if (!scores.empty()) {
      SaveScores();
      m_revision++;
   }
   return scores;
}

bool ScoreStore::DeleteScore(const string& id)
{
   std::lock_guard lock(m_mutex);
   if (std::erase_if(m_scores, [&id](const Score& score) { return score.id == id; }) == 0)
      return false;
   SaveScores();
   m_revision++;
   return true;
}

size_t ScoreStore::ClearScores(const string& tableUuid)
{
   std::lock_guard lock(m_mutex);
   const size_t count = std::erase_if(m_scores, [&tableUuid](const Score& score) { return tableUuid.empty() || score.tableUuid == tableUuid; });
   if (count == 0)
      return 0;
   SaveScores();
   m_revision++;
   return count;
}

bool ScoreStore::AssignScore(const string& id, const string& profileId)
{
   std::lock_guard lock(m_mutex);
   if (!profileId.empty() && std::ranges::none_of(m_profiles, [&profileId](const Profile& profile) { return profile.id == profileId; }))
      return false;
   const auto it = std::ranges::find_if(m_scores, [&id](const Score& score) { return score.id == id; });
   if (it == m_scores.end())
      return false;
   if (it->profileId != profileId) {
      it->profileId = profileId;
      SaveScores();
      m_revision++;
   }
   return true;
}

std::optional<ScoreRank> ScoreStore::GetRank(const string& scoreId) const
{
   std::lock_guard lock(m_mutex);
   const auto found = std::ranges::find_if(m_scores, [&scoreId](const Score& score) { return score.id == scoreId; });
   if (found == m_scores.end())
      return std::nullopt;
   const Score score = *found;
   ScoreRank rank;
   const std::vector<Score> sorted = SortedScores(score.tableUuid);
   rank.total = static_cast<int>(sorted.size());
   rank.rank = static_cast<int>(std::ranges::find_if(sorted, [&scoreId](const Score& other) { return other.id == scoreId; }) - sorted.begin()) + 1;
   if (!score.profileId.empty()) {
      // The profile is ranked with this score, even if it did better before, to tell where this game places it among the others
      const std::vector<Score> best = BestByProfile(score.tableUuid);
      rank.profileTotal = static_cast<int>(best.size());
      rank.profileRank = 1;
      for (const Score& other : best) {
         if (other.profileId == score.profileId)
            rank.personalBest = other.id == score.id;
         else if (IsBetter(other, score))
            rank.profileRank++;
      }
   }
   return rank;
}

std::vector<Score> ScoreStore::GetBestByProfile(const string& tableUuid) const
{
   std::lock_guard lock(m_mutex);
   return BestByProfile(tableUuid);
}

string ScoreStore::FormatScore(int64_t score)
{
   string digits = std::to_string(score < 0 ? -score : score);
   for (int pos = static_cast<int>(digits.size()) - 3; pos > 0; pos -= 3)
      digits.insert(static_cast<size_t>(pos), ",");
   return score < 0 ? '-' + digits : digits;
}

}
