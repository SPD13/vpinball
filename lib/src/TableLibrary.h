// license:GPLv3+

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace VPinballLib {

// A table known to the library. 'path' and 'image' are relative to the tables folder and always use '/'.
struct Table {
   std::string uuid;
   std::string name;
   std::string path;
   std::string image;
   int64_t createdAt = 0;
   int64_t modifiedAt = 0;

   // Player statistics, which are not part of tables.json (see TableLibrary::Config::statsPath)
   int64_t lastPlayedAt = 0;
   int playCount = 0;
   bool favorite = false;
};

// Table database shared by the launchers: keeps tables.json in sync with the content of the tables folder.
//
// This is the C++ counterpart of TableManager.kt (Android) and TableManager.swift (iOS) and reads/writes
// the same tables.json: {"tableCount":N,"tables":[{uuid,name,path,image,createdAt,modifiedAt}]}
//
// It only depends on the standard library (zip support and logging are injected) and is thread safe, as
// the web server triggers rescans from its own thread.
class TableLibrary final
{
public:
   enum class LogLevel { Info, Warn, Error };

   using ProgressCallback = std::function<void(int progress, const std::string& message)>;
   using ZipProgressCallback = std::function<void(int current, int total, const char* filename)>;
   using ZipFunction = std::function<bool(const std::filesystem::path& sourcePath, const std::filesystem::path& destPath, ZipProgressCallback callback)>;
   using LogCallback = std::function<void(LogLevel level, const std::string& message)>;

   struct Config {
      std::filesystem::path tablesPath; // Folder owned by the library, one sub folder per table
      std::filesystem::path jsonPath; // Location of tables.json
      std::filesystem::path statsPath; // Play counts, last played dates and favorites, by table uuid. Defaults to 'table-stats.json' along tables.json. This
                                       // is a separate file as the mobile launchers, which share the tables.json format, reject the fields they do not know
      std::vector<std::filesystem::path> excludedPaths; // Folders inside tablesPath that must not be scanned
      ZipFunction zip; // Usually ZipUtils::Zip, needed by Export
      ZipFunction unzip; // Usually ZipUtils::Unzip, needed to import .zip/.vpxz bundles
      LogCallback log;
   };

   explicit TableLibrary(Config config);
   ~TableLibrary();

   // Synchronize with the tables folder: drop entries whose file is gone, register new .vpx files, pick up
   // artwork, and import the .zip/.vpxz bundles dropped at the root of the folder (deleted once imported).
   // Files modified during the last 'settleSeconds' are considered as still being copied and left for the
   // next rescan: use 0 when the caller knows that transfers are done (startup, web server event, button).
   void Rescan(const ProgressCallback& onProgress = nullptr, int settleSeconds = 0);

   // Same, on a worker thread, for callers that must not block like a UI: poll IsScanning/GetRevision for the outcome.
   // A request made while a scan is running triggers another scan afterward, so that no change is missed.
   void RescanAsync(int settleSeconds = 0);
   bool IsScanning() const { return m_scanning.load(); }

   // Tables sorted by name (case insensitive)
   std::vector<Table> GetTables(bool ascending = true) const;
   std::optional<Table> GetTable(const std::string& uuid) const;

   // Incremented each time the table list changes, for UIs that poll
   uint64_t GetRevision() const { return m_revision.load(); }

   // Import a .vpx, .vpxz or .zip located anywhere (the source file is left untouched)
   std::vector<Table> Import(const std::filesystem::path& path, const ProgressCallback& onProgress = nullptr);

   bool Delete(const std::string& uuid);
   bool Rename(const std::string& uuid, const std::string& newName);
   // Empty: remove the image, absolute: copy the image next to the table, relative: reference an existing file
   bool SetImage(const std::string& uuid, const std::string& imagePath);
   // Look again for <table>.png|.jpg, typically after a screenshot was captured
   bool ReloadImage(const std::string& uuid);
   bool ResetIni(const std::string& uuid);
   bool SetFavorite(const std::string& uuid, bool favorite);
   // To be called when a table starts to be played, whether it was selected from the library or not (nothing happens for tables outside of the library)
   bool RecordPlay(const std::filesystem::path& fullPath);
   // Zip the table folder as <destFolder>/<name>.vpxz
   std::optional<std::filesystem::path> Export(const std::string& uuid, const std::filesystem::path& destFolder, const ProgressCallback& onProgress = nullptr);

   const std::filesystem::path& GetTablesPath() const { return m_config.tablesPath; }
   std::filesystem::path GetFullPath(const Table& table) const { return BuildPath(table.path); }
   std::filesystem::path GetImagePath(const Table& table) const { return table.image.empty() ? std::filesystem::path() : BuildPath(table.image); }
   std::filesystem::path GetIniPath(const Table& table) const { return BuildPath(table.path).replace_extension(".ini"); }
   std::filesystem::path GetScriptPath(const Table& table) const { return BuildPath(table.path).replace_extension(".vbs"); }

   static std::string SanitizeName(const std::string& name);

   // Fuzzy search: the characters of each word of the query must all be found in the text, in the same order, ignoring case. Words may be in any order.
   // Returns nothing if they are not, otherwise a score which is higher for matches that are consecutive, at the start of words, or early in the text.
   static std::optional<int> FuzzyScore(const std::string& query, const std::string& text);

private:
   void Log(LogLevel level, const std::string& message) const;
   std::filesystem::path BuildPath(const std::string& relativePath) const;
   std::string RelativePath(const std::filesystem::path& fullPath) const;
   bool IsInsideTables(const std::filesystem::path& fullPath) const;
   bool IsExcluded(const std::filesystem::path& fullPath) const;
   std::vector<std::filesystem::path> ListTableFiles(const std::filesystem::path& folder, int settleSeconds) const;
   std::string FindImage(const std::string& tablePath) const;
   std::string GenerateUUID(const std::vector<Table>& tables) const;
   std::string GetUniqueFolder(const std::string& baseName) const;
   Table CreateTable(const std::filesystem::path& fullPath, const std::vector<Table>& tables) const;

   void LoadJson();
   void SaveJson();
   void Commit(std::vector<Table> tables);
   bool Update(const std::string& uuid, const std::function<bool(Table&)>& change, bool isContentChange = true);
   std::filesystem::path GetStatsPath() const;
   static std::optional<int> FuzzyWordScore(const std::string& word, const std::string& text);
   std::vector<Table> ImportVPX(const std::filesystem::path& path, const std::vector<Table>& tables);
   std::vector<Table> ImportArchive(const std::filesystem::path& path, const std::vector<Table>& tables, const ProgressCallback& onProgress);

   const Config m_config;
   std::mutex m_operationMutex; // Serializes the operations modifying the library (they may be long: unzip, copy...)
   mutable std::mutex m_mutex; // Protects m_tables, never held during file operations
   std::vector<Table> m_tables;
   bool m_loaded = false;
   std::set<std::string> m_rejectedArchives; // Bundles without table, to avoid extracting them at each rescan
   std::atomic<uint64_t> m_revision { 0 };

   std::mutex m_workerMutex; // Protects m_worker and m_rescanPending
   std::thread m_worker;
   std::atomic<bool> m_scanning { false };
   bool m_rescanPending = false;
};

}
