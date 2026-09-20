// license:GPLv3+

#pragma once

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <string>
#include <queue>
#include <deque>
#include <filesystem>
#include <mongoose/mongoose.h>

class WebServer {
public:
   WebServer();
   ~WebServer();

   static void EventHandler(struct mg_connection *c, int ev, void *ev_data);
   static void LogAppender(const string& formattedLog);
   static void BroadcastStatus();
   void Update();
   void Start();
   void Stop();
   bool IsRunning() { return m_run; }
   string GetUrl();

   // When pairing is required, the API is only served to browsers that entered the code displayed by the application.
   // This is the default for the desktop application, as the server has no other access control and listens on the local network.
   void SetPairingRequired(bool required) { m_pairingRequired = required; }
   string GetPairingCode();

private:
   void SetLastUpdate();
   void Info(struct mg_connection *c, struct mg_http_message* hm);
   void Status(struct mg_connection *c, struct mg_http_message* hm);
   void Assets(struct mg_connection *c, struct mg_http_message* hm);
   void Files(struct mg_connection *c, struct mg_http_message* hm);
   void Download(struct mg_connection *c, struct mg_http_message* hm);
   void Upload(struct mg_connection *c, struct mg_http_message* hm);
   void Delete(struct mg_connection *c, struct mg_http_message* hm);
   void Rename(struct mg_connection *c, struct mg_http_message* hm);
   void Move(struct mg_connection *c, struct mg_http_message* hm);
   void Folder(struct mg_connection *c, struct mg_http_message* hm);
   void Extract(struct mg_connection *c, struct mg_http_message* hm);
   void Command(struct mg_connection *c, struct mg_http_message* hm);
   void LogStream(struct mg_connection *c, struct mg_http_message* hm);

   void AddLogEntry(const string& formattedLog);
   void BroadcastLogEntry(const string& formattedLog);

   void Pair(struct mg_connection *c, struct mg_http_message* hm);
   bool IsPaired(struct mg_http_message* hm);
   void GeneratePairingCode();

   string GetIPAddress();
   bool ValidatePathParameter(struct mg_connection *c, struct mg_http_message* hm, const char* paramName, string& outValue);
   std::filesystem::path BuildTablePath(const char* relativePath);
   bool Unzip(const char* pSource);

   struct mg_mgr m_mgr;
   std::atomic<bool> m_run;
   std::unique_ptr<std::thread> m_pThread;
   string m_url;
#ifdef __LIBVPINBALL__
   bool m_pairingRequired = false;
#else
   bool m_pairingRequired = true;
#endif
   std::mutex m_pairingMutex;
   string m_pairingCode;
   vector<string> m_pairedTokens;
   int m_failedPairings = 0;
   static std::mutex s_logMutex;
   static vector<unsigned long> s_logConnections;
   static vector<unsigned long> s_statusConnections;
   static std::deque<string> s_recentLogs;
   static const size_t MAX_RECENT_LOGS = 1000;
   static WebServer* s_instance;
   static int64_t s_lastUpdateTimestamp;
};
