// license:GPLv3+

#include "core/stdafx.h"
#include "WebServer.h"

#include "core/FileLocator.h"
#include "core/VPApp.h"
#include "core/vpversion.h"
#include "parts/pintable.h"
#include "ui/live/LiveUI.h"
#include "ui/win/WinEditor.h"

#ifdef __LIBVPINBALL__
#include "VPinballLib.h"
#else
#include "TableLibrary.h"
#include "ScoreStore.h"
#endif
#include "ZipUtils.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#endif
#include <filesystem>
#include <map>
#include <set>
#include <algorithm>

using json = nlohmann::json;

namespace {
   constexpr const char* HEADER_JSON = "Content-Type: application/json\r\n";

   constexpr const char* RESPONSE_OK = "OK";
   constexpr const char* RESPONSE_BAD_REQUEST = "Bad request";
   constexpr const char* RESPONSE_UNAUTHORIZED = "Pairing required";
   constexpr const char* RESPONSE_NOT_FOUND = "File not found";
   constexpr const char* RESPONSE_METHOD_NOT_ALLOWED = "Method Not Allowed";
   constexpr const char* RESPONSE_CONFLICT = "Conflict";
   constexpr const char* RESPONSE_INTERNAL_SERVER_ERROR = "Server error";

   constexpr int STATUS_OK = 200;
   constexpr int STATUS_BAD_REQUEST = 400;
   constexpr int STATUS_UNAUTHORIZED = 401;
   constexpr int STATUS_NOT_FOUND = 404;
   constexpr int STATUS_METHOD_NOT_ALLOWED = 405;
   constexpr int STATUS_CONFLICT = 409;
   constexpr int STATUS_INTERNAL_SERVER_ERROR = 500;

   constexpr size_t MAX_UPLOAD_SIZE = size_t(2) * 1024 * 1024 * 1024; // 2GB

   constexpr const char* PAIRING_COOKIE = "vpx_pairing";
   constexpr int MAX_FAILED_PAIRINGS = 5;

   // Mongoose and the browser use UTF-8 file names (on Windows, std::filesystem::path::string() uses the ANSI code page instead)
   string PathToUTF8(const std::filesystem::path& path)
   {
      const std::u8string s = path.u8string();
      return string(s.begin(), s.end());
   }

   std::filesystem::path UTF8ToPath(const string& utf8)
   {
      return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
   }
}

// The mobile launchers are notified through the library events, while the desktop application owns its table library
static void NotifyWebServerUrl(const string& url)
{
#ifdef __LIBVPINBALL__
   if (url.empty())
      VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_SERVER, nullptr);
   else {
      VPinballLib::WebServerData webServerData = { url };
      VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_WEB_SERVER, &webServerData);
   }
#endif
}

static void NotifyTablesChanged()
{
#ifdef __LIBVPINBALL__
   VPinballLib::CommandData commandData = { "reloadTables", "" };
   VPinballLib::VPinballLib::SendEvent(VPINBALL_EVENT_COMMAND, &commandData);
#else
   g_app->GetTableLibrary().RescanAsync();
#endif
}

std::mutex WebServer::s_logMutex;
vector<unsigned long> WebServer::s_logConnections;
vector<unsigned long> WebServer::s_statusConnections;
std::deque<string> WebServer::s_recentLogs;
WebServer* WebServer::s_instance = nullptr;
int64_t WebServer::s_lastUpdateTimestamp = 0;

void WebServer::EventHandler(struct mg_connection *c, int ev, void *ev_data)
{
   WebServer* webServer = (WebServer*)c->fn_data;

   if (ev == MG_EV_HTTP_MSG) {
      struct mg_http_message *hm = (struct mg_http_message *) ev_data;

      // Everything but the static web page needs a paired browser
      static constexpr const char* apiRoutes[] = { "/info", "/status", "/files", "/download", "/upload", "/delete", "/folder", "/extract", "/command", "/log-stream", "/rename", "/move", "/missing-roms",
         "/tables", "/table-image", "/table-favorite", "/table-name", "/table-delete",
         "/scores", "/score-delete", "/scores-clear", "/score-assign", "/profile-add", "/profile-rename", "/profile-delete", "/profile-active" };
      const bool isApi = std::any_of(std::begin(apiRoutes), std::end(apiRoutes), [hm](const char* route) { return mg_match(hm->uri, mg_str(route), NULL); });
      if (mg_match(hm->uri, mg_str("/pair"), NULL))
         webServer->Pair(c, hm);
      else if (isApi && !webServer->IsPaired(hm))
         mg_http_reply(c, STATUS_UNAUTHORIZED, "", "%s", RESPONSE_UNAUTHORIZED);
      else if (mg_match(hm->uri, mg_str("/info"), NULL))
         webServer->Info(c, hm);
      else if (mg_match(hm->uri, mg_str("/status"), NULL))
         webServer->Status(c, hm);
      else if (mg_match(hm->uri, mg_str("/assets/*"), NULL))
         webServer->Assets(c, hm);
      else if (mg_match(hm->uri, mg_str("/files"), NULL))
         webServer->Files(c, hm);
      else if (mg_match(hm->uri, mg_str("/download"), NULL))
         webServer->Download(c, hm);
      else if (mg_match(hm->uri, mg_str("/upload"), NULL))
         webServer->Upload(c, hm);
      else if (mg_match(hm->uri, mg_str("/delete"), NULL))
         webServer->Delete(c, hm);
      else if (mg_match(hm->uri, mg_str("/folder"), NULL))
         webServer->Folder(c, hm);
      else if (mg_match(hm->uri, mg_str("/extract"), NULL))
         webServer->Extract(c, hm);
      else if (mg_match(hm->uri, mg_str("/command"), NULL))
         webServer->Command(c, hm);
      else if (mg_match(hm->uri, mg_str("/log-stream"), NULL))
         webServer->LogStream(c, hm);
      else if (mg_match(hm->uri, mg_str("/rename"), NULL))
         webServer->Rename(c, hm);
      else if (mg_match(hm->uri, mg_str("/move"), NULL))
         webServer->Move(c, hm);
      else if (mg_match(hm->uri, mg_str("/missing-roms"), NULL))
         webServer->MissingRoms(c, hm);
      else if (mg_match(hm->uri, mg_str("/tables"), NULL))
         webServer->Tables(c, hm);
      else if (mg_match(hm->uri, mg_str("/table-image"), NULL))
         webServer->TableImage(c, hm);
      else if (mg_match(hm->uri, mg_str("/table-favorite"), NULL))
         webServer->TableFavorite(c, hm);
      else if (mg_match(hm->uri, mg_str("/table-name"), NULL))
         webServer->TableName(c, hm);
      else if (mg_match(hm->uri, mg_str("/table-delete"), NULL))
         webServer->TableDelete(c, hm);
      else if (mg_match(hm->uri, mg_str("/scores"), NULL))
         webServer->Scores(c, hm);
      else if (mg_match(hm->uri, mg_str("/score-delete"), NULL))
         webServer->ScoreDelete(c, hm);
      else if (mg_match(hm->uri, mg_str("/scores-clear"), NULL))
         webServer->ScoresClear(c, hm);
      else if (mg_match(hm->uri, mg_str("/score-assign"), NULL))
         webServer->ScoreAssign(c, hm);
      else if (mg_match(hm->uri, mg_str("/profile-add"), NULL))
         webServer->ProfileAdd(c, hm);
      else if (mg_match(hm->uri, mg_str("/profile-rename"), NULL))
         webServer->ProfileRename(c, hm);
      else if (mg_match(hm->uri, mg_str("/profile-delete"), NULL))
         webServer->ProfileDelete(c, hm);
      else if (mg_match(hm->uri, mg_str("/profile-active"), NULL))
         webServer->ProfileActive(c, hm);
      else {
         struct mg_http_serve_opts opts = {};

         string uri(hm->uri.buf, hm->uri.len);
         if (!uri.empty() && uri.front() == '/') uri.erase(0, 1);

         // The home page is the tables page, the file manager (vpx.html) being linked from it. The mobile launchers own their table
         // library, so their home page is the file manager.
#ifdef __LIBVPINBALL__
         constexpr const char* homePage = "vpx.html";
#else
         constexpr const char* homePage = "tables.html";
#endif
         std::filesystem::path webBase = std::filesystem::path(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets)) / "web";
         std::filesystem::path asset = webBase / uri;

         std::error_code ec;
         if (!uri.empty() && std::filesystem::exists(asset, ec))
            mg_http_serve_file(c, hm, asset.string().c_str(), &opts);
         else
            mg_http_serve_file(c, hm, (webBase / homePage).string().c_str(), &opts);
      }
   }
   else if (ev == MG_EV_WAKEUP) {
      const struct mg_str* data = (struct mg_str*)ev_data;
      if (c->is_websocket)
         mg_ws_send(c, data->buf, data->len, WEBSOCKET_OP_TEXT);
      else
         mg_send(c, data->buf, data->len);
   }
   else if (ev == MG_EV_CLOSE) {
      std::lock_guard<std::mutex> lock(s_logMutex);
      auto logIt = std::find(s_logConnections.begin(), s_logConnections.end(), c->id);
      if (logIt != s_logConnections.end())
         s_logConnections.erase(logIt);

      auto statusIt = std::find(s_statusConnections.begin(), s_statusConnections.end(), c->id);
      if (statusIt != s_statusConnections.end())
         s_statusConnections.erase(statusIt);
   }
}

void WebServer::LogAppender(const string& formattedLog)
{
   static std::queue<string> pendingLogs;
   static std::mutex pendingMutex;
   static bool processingPending = false;

   if (!s_instance) {
      std::lock_guard<std::mutex> lock(pendingMutex);
      pendingLogs.push(formattedLog);

      while (pendingLogs.size() > 100)
         pendingLogs.pop();

      return;
   }

   s_instance->AddLogEntry(formattedLog);

   if (!processingPending) {
      std::lock_guard<std::mutex> lock(pendingMutex);
      processingPending = true;
      while (!pendingLogs.empty()) {
         s_instance->AddLogEntry(pendingLogs.front());
         pendingLogs.pop();
      }
      processingPending = false;
   }
}

void WebServer::SetLastUpdate()
{
   s_lastUpdateTimestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()
   ).count();
   PLOGD.printf("Web interface last update timestamp set to: %lld", (long long)s_lastUpdateTimestamp);

#ifndef __LIBVPINBALL__
   // The desktop table picker follows the changes made from the browser
   if (m_run)
      NotifyTablesChanged();
#endif

   BroadcastStatus();
}

WebServer::WebServer()
{
   m_run = false;
   s_instance = this;
}

WebServer::~WebServer()
{
   m_run = false;
   s_instance = nullptr;

   {
      std::lock_guard<std::mutex> lock(s_logMutex);
      s_logConnections.clear();
      s_statusConnections.clear();
      s_recentLogs.clear();
   }

   if (m_pThread && m_pThread->joinable())
      m_pThread->join();
}

void WebServer::Start()
{
   if (m_run) {
      PLOGE.printf("Web server already running");
      return;
   }

   // mg_log_set(MG_LL_DEBUG);

   const auto addrPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::StringPropertyDef>("Standalone"s, "WebServerAddr"s, ""s, ""s, false, "0.0.0.0"s));
   const auto portPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::IntPropertyDef>("Standalone"s, "WebServerPort"s, ""s, ""s, false, INT_MIN, INT_MAX, 2112));
   const string addr = g_app->m_settings.GetString(addrPropId);
   const int port = g_app->m_settings.GetInt(portPropId);

   string bindUrl = "http://" + addr + ':' + std::to_string(port);

   PLOGI.printf("Starting web server at %s", bindUrl.c_str());

   mg_mgr_init(&m_mgr);
   if (!mg_wakeup_init(&m_mgr)) {
      PLOGE.printf("Unable to create the web server wakeup pipe, log and status streaming will not be delivered");
   }

   SetLastUpdate();

   if (mg_http_listen(&m_mgr, bindUrl.c_str(), &WebServer::EventHandler, this)) {
      m_run = true;
      {
         std::lock_guard<std::mutex> lock(m_pairingMutex);
         m_pairedTokens.clear();
         GeneratePairingCode();
      }

      PLOGI.printf("Web server started");

      string ip = GetIPAddress();

      if (!ip.empty()) {
         m_url = "http://" + ip + ':' + std::to_string(port);

         PLOGI.printf("To access the web server, in a browser go to: %s", m_url.c_str());
      }
      else
         m_url.clear();

      NotifyWebServerUrl(m_url);

      m_pThread = std::make_unique<std::thread>([this]() {
         while (m_run)
            mg_mgr_poll(&m_mgr, 100);

         mg_mgr_free(&m_mgr);

         PLOGI.printf("Web server closed");
      });
   }
   else {
      PLOGE.printf("Unable to start web server");

      NotifyWebServerUrl(string());
   }
}

void WebServer::Stop()
{
   if (!m_run) {
      PLOGE.printf("Web server is not running");
      return;
   }

   m_run = false;
   m_url.clear();

   if (m_pThread && m_pThread->joinable())
      m_pThread->join();

   {
      std::lock_guard<std::mutex> lock(m_pairingMutex);
      m_pairingCode.clear();
      m_pairedTokens.clear();
   }

   NotifyWebServerUrl(string());
}

void WebServer::Update()
{
   const auto serverPropId = Settings::GetRegistry().Register(std::make_unique<VPX::Properties::BoolPropertyDef>("Standalone"s, "WebServer"s, ""s, ""s, false, false));
   bool enabled = g_app->m_settings.GetBool(serverPropId);

   if (enabled && !m_run)
      Start();
   else if (!enabled && m_run)
      Stop();
}

string WebServer::GetUrl()
{
   return m_run ? m_url : string();
}

///////////////////////////////////////////////////////////////////////////////
// Pairing: the user copies a short code displayed by the application in the browser, which receives a session token (cookie)

string WebServer::GetPairingCode()
{
   std::lock_guard<std::mutex> lock(m_pairingMutex);
   return m_run && m_pairingRequired ? m_pairingCode : string();
}

void WebServer::GeneratePairingCode()
{
   uint32_t value = 0;
   mg_random(&value, sizeof(value));
   m_pairingCode = std::format("{:06}", value % 1000000);
   m_failedPairings = 0;
}

bool WebServer::IsPaired(struct mg_http_message* hm)
{
   if (!m_pairingRequired)
      return true;
   const struct mg_str* cookies = mg_http_get_header(hm, "Cookie");
   if (cookies == nullptr)
      return false;
   const struct mg_str value = mg_http_get_header_var(*cookies, mg_str(PAIRING_COOKIE));
   if (value.len == 0)
      return false;
   const string token(value.buf, value.len);
   std::lock_guard<std::mutex> lock(m_pairingMutex);
   return std::find(m_pairedTokens.begin(), m_pairedTokens.end(), token) != m_pairedTokens.end();
}

void WebServer::Pair(struct mg_connection *c, struct mg_http_message* hm)
{
   char code[16];
   mg_http_get_var(&hm->query, "code", code, sizeof(code));

   std::lock_guard<std::mutex> lock(m_pairingMutex);
   if (!m_pairingRequired) {
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      return;
   }
   if (m_pairingCode.empty() || m_pairingCode != code) {
      // A code is only good for a few attempts, to keep it short while defeating enumeration
      if (++m_failedPairings >= MAX_FAILED_PAIRINGS) {
         PLOGW.printf("Too many failed pairing attempts, a new pairing code was generated");
         GeneratePairingCode();
      }
      mg_http_reply(c, STATUS_UNAUTHORIZED, "", "%s", RESPONSE_UNAUTHORIZED);
      return;
   }

   char token[33];
   mg_random_str(token, sizeof(token));
   m_pairedTokens.emplace_back(token);
   m_failedPairings = 0;
   PLOGI.printf("Web server paired with a new browser");
   mg_http_reply(c, STATUS_OK, std::format("Set-Cookie: {}={}; Path=/; HttpOnly; SameSite=Strict\r\n", PAIRING_COOKIE, token).c_str(), RESPONSE_OK);
}

void WebServer::Info(struct mg_connection *c, struct mg_http_message* hm)
{
   // Archives that can be extracted (and imported as tables when uploaded at the root), which depends on the build
#ifdef __LIBVPINBALL__
   constexpr bool hasTableLibrary = false;
#else
   constexpr bool hasTableLibrary = true; // The tables page manages it
#endif
   json j = {{"version", VP_VERSION_STRING_FULL_LITERAL}, {"extractableExtensions", ZipUtils::GetExtractableExtensions()}, {"tableLibrary", hasTableLibrary},
      {"scores", hasTableLibrary}}; // Leaderboards of the tables of the library
   string response = j.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
}

void WebServer::Status(struct mg_connection *c, struct mg_http_message* hm)
{
   mg_ws_upgrade(c, hm, NULL);

   {
      std::lock_guard<std::mutex> lock(s_logMutex);
      s_statusConnections.push_back(c->id);
   }

   BroadcastStatus();
}

void WebServer::Assets(struct mg_connection *c, struct mg_http_message* hm)
{
   string uri(hm->uri.buf, hm->uri.len);

   if (uri.length() > 8 && uri.substr(0, 8) == "/assets/") {
      string assetPath = uri.substr(8);

      if (!mg_path_is_sane(mg_str(assetPath.c_str()))) {
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
         return;
      }

      std::filesystem::path fullPath = std::filesystem::path(g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Assets)) / assetPath;

      std::error_code ec;
      if (std::filesystem::is_regular_file(fullPath, ec)) {
         struct mg_http_serve_opts opts = {};
         mg_http_serve_file(c, hm, fullPath.string().c_str(), &opts);
      }
      else
         mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::Files(struct mg_connection *c, struct mg_http_message* hm)
{
   char buffer[1024];
   mg_http_get_var(&hm->query, "q", buffer, sizeof(buffer));

   string q = buffer;
   if (!q.empty() && !mg_path_is_sane(mg_str(buffer))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   PLOGD.printf("Retrieving file list: q=%s", q.c_str());

   const std::filesystem::path path = BuildTablePath(q.c_str());

   std::error_code ec;
   std::filesystem::directory_iterator it(path, ec);
   if (ec) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   json files = json::array();
   for (; it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (ec)
         break;
      const std::filesystem::directory_entry& entry = *it;
      const string name = PathToUTF8(entry.path().filename());
      const bool isDir = entry.is_directory(ec);
      const auto writeTime = entry.last_write_time(ec);
      if (ec)
         continue;
      const std::uintmax_t size = isDir ? 0 : entry.file_size(ec);
      if (ec)
         continue;

      // The cast is needed with libc++ (macOS), where file_clock::to_sys gives a finer duration than the one of system_clock
      const std::time_t mtime = std::chrono::system_clock::to_time_t(std::chrono::time_point_cast<std::chrono::system_clock::duration>(std::chrono::file_clock::to_sys(writeTime)));
      char datebuf[32];
      std::tm tm;
#ifdef _WIN32
      gmtime_s(&tm, &mtime);
#else
      gmtime_r(&mtime, &tm);
#endif
      strftime(datebuf, sizeof(datebuf), "%Y-%m-%dT%H:%M:%SZ", &tm);

      json fileEntry = {
         {"name", name},
         {"ext", isDir ? string() : extension_from_path(name)},
         {"isDir", isDir},
         {"size", static_cast<long long>(size)},
         {"date", datebuf}
      };

      files.push_back(fileEntry);
   }

   string response = files.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
}

void WebServer::Download(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   PLOGI.printf("Downloading file: q=%s", q.c_str());

   const string path = PathToUTF8(BuildTablePath(q.c_str()));

   struct mg_http_serve_opts opts = {};
   mg_http_serve_file(c, hm, path.c_str(), &opts);
}

void WebServer::Upload(struct mg_connection *c, struct mg_http_message* hm)
{
   char q[1024];
   mg_http_get_var(&hm->query, "q", q, sizeof(q));
   if (*q != '\0' && !mg_path_is_sane(mg_str(q))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   string file;
   if (!ValidatePathParameter(c, hm, "file", file))
      return;

   char offsetStr[32];
   mg_http_get_var(&hm->query, "offset", offsetStr, sizeof(offsetStr));
   long offset = offsetStr[0] ? strtol(offsetStr, nullptr, 10) : 0;
   if (offset <= 0) {
      PLOGI.printf("Uploading file: file=%s", file.c_str());
   }

   char lengthStr[32];
   mg_http_get_var(&hm->query, "length", lengthStr, sizeof(lengthStr));
   long length = lengthStr[0] ? strtol(lengthStr, nullptr, 10) : 0;

   const std::filesystem::path path = BuildTablePath(q);

   // Chunks are appended in a hidden staging folder, and the file is moved in place when complete: an interrupted transfer never leaves a truncated table that would be listed (and played)
   std::error_code ec;
   const std::filesystem::path stagingPath = path / ".upload";
   if (hm->body.len == 0) {
      // The web page ends each transfer with an empty request, which is also all it sends for an empty file
      if (length == 0 && offset == 0) {
         std::filesystem::create_directories(path, ec);
         std::ofstream(path / UTF8ToPath(file), std::ios::trunc);
         SetLastUpdate();
      }
      mg_http_upload(c, hm, &mg_fs_posix, PathToUTF8(stagingPath).c_str(), MAX_UPLOAD_SIZE); // Only replies, as there is nothing to write
      return;
   }
   std::filesystem::create_directories(stagingPath, ec);
   if (mg_http_upload(c, hm, &mg_fs_posix, PathToUTF8(stagingPath).c_str(), MAX_UPLOAD_SIZE) == length) {
      std::filesystem::rename(stagingPath / UTF8ToPath(file), path / UTF8ToPath(file), ec);
      if (ec) {
         PLOGE.printf("Failed to finalize upload: file=%s, error=%s", file.c_str(), ec.message().c_str());
         return;
      }
      std::filesystem::remove(stagingPath, ec); // Only succeeds when no other transfer is pending
#ifndef __LIBVPINBALL__
      g_app->GetTableLibrary().OnFileAdded(file); // It may be a missing ROM
#endif
#ifdef __LIBVPINBALL__
      if (*q == '\0' && file == "VPinballX.ini") {
         g_app->m_settings.SetIniPath(path);
         g_app->m_settings.Load(true);
         g_app->m_settings.Save();
      }
#endif
      SetLastUpdate();
   }
}

// GET: the ROMs that tables need and could not find (see TableLibrary::AddMissingRom), POST with 'clear': empty the list
void WebServer::MissingRoms(struct mg_connection *c, struct mg_http_message* hm)
{
   json list = json::array();
#ifndef __LIBVPINBALL__
   VPinballLib::TableLibrary& library = g_app->GetTableLibrary();
   if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
      char clear[8];
      mg_http_get_var(&hm->query, "clear", clear, sizeof(clear));
      if (*clear == '\0') {
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
         return;
      }
      library.ClearMissingRoms();
      PLOGI.printf("Missing ROM list cleared");
   }
   for (const VPinballLib::TableLibrary::MissingRom& missing : library.GetMissingRoms())
      list.push_back({ { "rom", missing.rom }, { "folder", missing.folder }, { "files", missing.files }, { "table", missing.table }, { "reportedAt", missing.reportedAt } });
#endif
   const string response = json { { "missingRoms", list } }.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
}

///////////////////////////////////////////////////////////////////////////////
// Table library: what the table picker of the application shows, with the names and favorites it displays

#ifndef __LIBVPINBALL__
// The table being played holds its files, and saves its settings when closing (the lobby is not a table, even when it is loaded from one to use its VR room)
static bool IsPlayed(const VPinballLib::TableLibrary& library, const VPinballLib::Table& table)
{
   if (g_pplayer == nullptr || g_pplayer->m_isLobby)
      return false;
   std::error_code ec;
   return std::filesystem::equivalent(g_pplayer->m_ptable->m_filename, library.GetFullPath(table), ec);
}

// The table named by the 'uuid' parameter, replying an error if there is none
static std::optional<VPinballLib::Table> GetRequestedTable(struct mg_connection *c, struct mg_http_message* hm)
{
   char uuid[64];
   mg_http_get_var(&hm->query, "uuid", uuid, sizeof(uuid));
   std::optional<VPinballLib::Table> table = *uuid == '\0' ? std::nullopt : g_app->GetTableLibrary().GetTable(uuid);
   if (!table)
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
   return table;
}
#endif

// GET: the tables of the library, with the revision of the list for the page to know when it changed
void WebServer::Tables(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   VPinballLib::TableLibrary& library = g_app->GetTableLibrary();
   json tables = json::array();
   for (const VPinballLib::Table& table : library.GetTables())
      tables.push_back({
         { "uuid", table.uuid },
         { "name", table.name },
         { "defaultName", VPinballLib::TableLibrary::GetDefaultName(table) },
         { "path", table.path },
         { "hasImage", !table.image.empty() },
         { "createdAt", table.createdAt },
         { "modifiedAt", table.modifiedAt },
         { "lastPlayedAt", table.lastPlayedAt },
         { "playCount", table.playCount },
         { "favorite", table.favorite },
         { "playing", IsPlayed(library, table) } });
   const string response = json { { "revision", library.GetRevision() }, { "scanning", library.IsScanning() }, { "tables", tables } }.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
#endif
}

// GET: the image of a table (the page adds its modification date to the URL, so that browsers may cache it)
void WebServer::TableImage(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   const std::optional<VPinballLib::Table> table = GetRequestedTable(c, hm);
   if (!table)
      return;
   const std::filesystem::path imagePath = g_app->GetTableLibrary().GetImagePath(*table);
   std::error_code ec;
   if (imagePath.empty() || !std::filesystem::is_regular_file(imagePath, ec)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }
   struct mg_http_serve_opts opts = {};
   opts.extra_headers = "Cache-Control: max-age=86400\r\n";
   mg_http_serve_file(c, hm, PathToUTF8(imagePath).c_str(), &opts);
#endif
}

// POST with 'uuid' and 'favorite' (1 or 0)
void WebServer::TableFavorite(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
      mg_http_reply(c, STATUS_METHOD_NOT_ALLOWED, "", "%s", RESPONSE_METHOD_NOT_ALLOWED);
      return;
   }
   char favorite[8];
   mg_http_get_var(&hm->query, "favorite", favorite, sizeof(favorite));
   if (*favorite == '\0') {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }
   const std::optional<VPinballLib::Table> table = GetRequestedTable(c, hm);
   if (!table)
      return;
   g_app->GetTableLibrary().SetFavorite(table->uuid, strcmp(favorite, "0") != 0); // False when unchanged, which is not an error
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

// POST with 'uuid' and 'name', the name displayed for the table by the table picker. An empty name gives back the one derived from the file name.
void WebServer::TableName(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
      mg_http_reply(c, STATUS_METHOD_NOT_ALLOWED, "", "%s", RESPONSE_METHOD_NOT_ALLOWED);
      return;
   }
   // The parameter is required, even empty (a name which does not fit in the buffer fails to decode)
   char buffer[512];
   if (mg_http_var(hm->query, mg_str("name")).buf == nullptr || mg_http_get_var(&hm->query, "name", buffer, sizeof(buffer)) < 0) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }
   // One line, without the spaces around it
   string name = buffer;
   std::replace_if(name.begin(), name.end(), [](char ch) { return ch == '\r' || ch == '\n' || ch == '\t'; }, ' ');
   name.erase(0, name.find_first_not_of(' '));
   name.erase(name.find_last_not_of(' ') + 1);

   const std::optional<VPinballLib::Table> table = GetRequestedTable(c, hm);
   if (!table)
      return;
   if (g_app->GetTableLibrary().Rename(table->uuid, name)) // False when unchanged, which is not an error
      PLOGI.printf("Table display name changed: %s -> %s", table->name.c_str(), name.empty() ? VPinballLib::TableLibrary::GetDefaultName(*table).c_str() : name.c_str());
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

// POST with 'uuid': delete the table and its files (the whole folder when it is the only table in it)
void WebServer::TableDelete(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
      mg_http_reply(c, STATUS_METHOD_NOT_ALLOWED, "", "%s", RESPONSE_METHOD_NOT_ALLOWED);
      return;
   }
   const std::optional<VPinballLib::Table> table = GetRequestedTable(c, hm);
   if (!table)
      return;
   VPinballLib::TableLibrary& library = g_app->GetTableLibrary();
   if (IsPlayed(library, *table)) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }
   if (!library.Delete(table->uuid)) {
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      return;
   }
   PLOGI.printf("Table deleted: %s (%s)", table->name.c_str(), table->path.c_str());
   SetLastUpdate(); // Files are gone: the file manager refreshes
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Leaderboards: the scores recorded at the end of the games (see ScoreTracker), and the profiles of the players

#ifndef __LIBVPINBALL__
static bool IsPost(struct mg_connection *c, struct mg_http_message* hm)
{
   if (mg_strcmp(hm->method, mg_str("POST")) == 0)
      return true;
   mg_http_reply(c, STATUS_METHOD_NOT_ALLOWED, "", "%s", RESPONSE_METHOD_NOT_ALLOWED);
   return false;
}

// A query parameter, which must be given (even empty) unless 'optional'. Replies an error and returns nothing when it is missing.
static std::optional<string> GetParameter(struct mg_connection *c, struct mg_http_message* hm, const char* name, bool optional = false)
{
   char buffer[512];
   if (mg_http_var(hm->query, mg_str(name)).buf == nullptr) {
      if (optional)
         return string();
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return std::nullopt;
   }
   if (mg_http_get_var(&hm->query, name, buffer, sizeof(buffer)) < 0) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return std::nullopt;
   }
   return string(buffer);
}

// A name of profile on one line, without the spaces around it (callers limit it to 32 characters, as the lobby shows them in lists)
static string CleanProfileName(string name)
{
   std::replace_if(name.begin(), name.end(), [](char ch) { return ch == '\r' || ch == '\n' || ch == '\t'; }, ' ');
   name.erase(0, name.find_first_not_of(' '));
   name.erase(name.find_last_not_of(' ') + 1);
   return name;
}
#endif

// GET: the profiles, the tables (of the library, and the ones which are gone but still have scores), and all the scores, best first, with their rank on their table
void WebServer::Scores(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   VPinballLib::ScoreStore& store = g_app->GetScoreStore();
   const uint64_t revision = store.GetRevision(); // Before reading, so that a change made meanwhile is seen at the next poll
   const std::vector<VPinballLib::Score> scores = store.GetScores();
   const std::optional<VPinballLib::Profile> active = store.GetActiveProfile();

   std::map<string, int> scoreCounts;
   for (const VPinballLib::Score& score : scores)
      scoreCounts[score.profileId]++;
   json profiles = json::array();
   for (const VPinballLib::Profile& profile : store.GetProfiles())
      profiles.push_back({ { "id", profile.id }, { "name", profile.name }, { "createdAt", profile.createdAt }, { "scoreCount", scoreCounts[profile.id] } });

   json tables = json::array();
   std::set<string> knownTables;
   for (const VPinballLib::Table& table : g_app->GetTableLibrary().GetTables()) {
      knownTables.insert(table.uuid);
      tables.push_back({ { "uuid", table.uuid }, { "name", table.name }, { "inLibrary", true } });
   }

   json list = json::array();
   std::map<string, int> ranks;
   for (const VPinballLib::Score& score : scores) {
      if (knownTables.insert(score.tableUuid).second)
         tables.push_back({ { "uuid", score.tableUuid }, { "name", score.tableName }, { "inLibrary", false } });
      list.push_back({
         { "id", score.id },
         { "tableUuid", score.tableUuid },
         { "profileId", score.profileId },
         { "playerSlot", score.playerSlot },
         { "playerCount", score.playerCount },
         { "score", score.score },
         { "playedAt", score.playedAt },
         { "durationSec", score.durationSec },
         { "source", score.source },
         { "rank", ++ranks[score.tableUuid] } });
   }
   const string response = json {
      { "revision", revision },
      { "activeProfileId", active ? active->id : string() },
      { "players", store.GetPlayerProfileIds() }, // Profile id of each player of the games, empty if unassigned
      { "profiles", profiles },
      { "tables", tables },
      { "scores", list } }.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
#endif
}

// POST with 'id'
void WebServer::ScoreDelete(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> id = GetParameter(c, hm, "id");
   if (!id)
      return;
   VPinballLib::ScoreStore& store = g_app->GetScoreStore();
   const std::optional<VPinballLib::Score> score = store.GetScore(*id);
   if (!score || !store.DeleteScore(*id)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }
   PLOGI.printf("Score deleted: %s on %s", VPinballLib::ScoreStore::FormatScore(score->score).c_str(), score->tableName.c_str());
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

// POST with 'uuid' to delete the scores of a table, or with 'all=1' to delete all the scores (an empty uuid is refused, so that a page which lost its table
// can not delete everything): replies the number of deleted scores as JSON
void WebServer::ScoresClear(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> uuid = GetParameter(c, hm, "uuid", true);
   const std::optional<string> all = uuid ? GetParameter(c, hm, "all", true) : std::nullopt;
   if (!all)
      return;
   if (uuid->empty() == (*all != "1")) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }
   const size_t count = g_app->GetScoreStore().ClearScores(*uuid);
   if (uuid->empty())
      PLOGI.printf("All scores cleared: %zu deleted", count);
   else
      PLOGI.printf("Scores of table %s cleared: %zu deleted", uuid->c_str(), count);
   const string response = json { { "deleted", count } }.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
#endif
}

// POST with 'id' and 'profile' (empty: the score belongs to nobody)
void WebServer::ScoreAssign(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> id = GetParameter(c, hm, "id");
   const std::optional<string> profile = id ? GetParameter(c, hm, "profile") : std::nullopt;
   if (!profile)
      return;
   if (!g_app->GetScoreStore().AssignScore(*id, *profile)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

// POST with 'name': replies the new profile as JSON, or 409 if the name is already used
void WebServer::ProfileAdd(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> name = GetParameter(c, hm, "name");
   if (!name)
      return;
   const string cleanName = CleanProfileName(*name);
   if (cleanName.empty() || cleanName.size() > 32) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }
   const std::optional<VPinballLib::Profile> profile = g_app->GetScoreStore().AddProfile(cleanName);
   if (!profile) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }
   const string response = json { { "id", profile->id }, { "name", profile->name } }.dump();
   mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
#endif
}

// POST with 'id' and 'name': 409 if the name is used by another profile
void WebServer::ProfileRename(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> id = GetParameter(c, hm, "id");
   const std::optional<string> name = id ? GetParameter(c, hm, "name") : std::nullopt;
   if (!name)
      return;
   const string cleanName = CleanProfileName(*name);
   if (cleanName.empty() || cleanName.size() > 32) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }
   VPinballLib::ScoreStore& store = g_app->GetScoreStore();
   if (!store.GetProfile(*id)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }
   if (!store.RenameProfile(*id, cleanName)) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

// POST with 'id': its scores are kept, and belong to nobody afterward
void WebServer::ProfileDelete(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> id = GetParameter(c, hm, "id");
   if (!id)
      return;
   if (!g_app->GetScoreStore().DeleteProfile(*id)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

// POST with 'id': the profile of player 1, to which the next scores of the one wearing the headset are given
void WebServer::ProfileActive(struct mg_connection *c, struct mg_http_message* hm)
{
#ifdef __LIBVPINBALL__
   mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
#else
   if (!IsPost(c, hm))
      return;
   const std::optional<string> id = GetParameter(c, hm, "id");
   if (!id)
      return;
   if (!g_app->GetScoreStore().SetActiveProfile(*id)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }
   mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
#endif
}

void WebServer::Delete(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   const std::filesystem::path path = BuildTablePath(q.c_str());

   std::error_code ec;
   if (std::filesystem::is_regular_file(path, ec)) {
      if (std::filesystem::remove(path, ec)) {
         SetLastUpdate();
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else {
         PLOGE.printf("Failed to delete file: q=%s, error=%s", q.c_str(), ec.message().c_str());
         mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      }
   }
   else if (std::filesystem::is_directory(path, ec)) {
      const std::uintmax_t removed = std::filesystem::remove_all(path, ec);
      if (!ec && removed != 0) {
         SetLastUpdate();
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else {
         PLOGE.printf("Failed to delete directory: q=%s, error=%s", q.c_str(), ec.message().c_str());
         mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      }
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::Rename(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   string newName;
   if (!ValidatePathParameter(c, hm, "name", newName))
      return;

   const std::filesystem::path oldPath = BuildTablePath(q.c_str());
   std::filesystem::path oldFile(oldPath);

   std::error_code ec;
   if (!std::filesystem::exists(oldFile, ec)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }

   std::filesystem::path newFile = oldFile.parent_path() / UTF8ToPath(newName);
   if (std::filesystem::exists(newFile, ec)) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }

   std::filesystem::rename(oldFile, newFile, ec);
   if (ec) {
      PLOGE.printf("Failed to rename: q=%s, name=%s, error=%s", q.c_str(), newName.c_str(), ec.message().c_str());
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
   }
   else {
      SetLastUpdate();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
}

void WebServer::Move(struct mg_connection *c, struct mg_http_message* hm)
{
   string q;
   if (!ValidatePathParameter(c, hm, "q", q))
      return;

   string dest;
   if (!ValidatePathParameter(c, hm, "dest", dest))
      return;

   const std::filesystem::path oldPath = BuildTablePath(q.c_str());
   const std::filesystem::path newPath = BuildTablePath(dest.c_str());

   std::filesystem::path oldFile(oldPath);
   std::filesystem::path newFile(newPath);

   std::error_code ec;
   if (!std::filesystem::exists(oldFile, ec)) {
      mg_http_reply(c, STATUS_NOT_FOUND, "", RESPONSE_NOT_FOUND);
      return;
   }

   if (std::filesystem::exists(newFile, ec)) {
      mg_http_reply(c, STATUS_CONFLICT, "", RESPONSE_CONFLICT);
      return;
   }

   if (!newFile.parent_path().empty() && !std::filesystem::exists(newFile.parent_path(), ec)) {
      std::filesystem::create_directories(newFile.parent_path(), ec);
      if (ec) {
         PLOGE.printf("Failed to create directory: dest=%s, error=%s", dest.c_str(), ec.message().c_str());
         mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
         return;
      }
   }

   std::filesystem::rename(oldFile, newFile, ec);
   if (ec) {
      PLOGE.printf("Failed to move: q=%s, dest=%s, error=%s", q.c_str(), dest.c_str(), ec.message().c_str());
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
   }
   else {
      SetLastUpdate();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
}

void WebServer::Folder(struct mg_connection *c, struct mg_http_message* hm)
{
   char q[1024];
   mg_http_get_var(&hm->query, "q", q, sizeof(q));

   if (*q == '\0' || !mg_path_is_sane(mg_str(q))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   const std::filesystem::path path = BuildTablePath(q);

   std::error_code ec;
   if (std::filesystem::create_directory(path, ec)) {
      SetLastUpdate();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
   else {
      PLOGE.printf("Failed to create folder: q=%s, error=%s", q, ec.message().c_str());
      mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
   }
}

void WebServer::Extract(struct mg_connection *c, struct mg_http_message* hm)
{
   char q[1024];
   mg_http_get_var(&hm->query, "q", q, sizeof(q));

   if (*q == '\0' || !mg_path_is_sane(mg_str(q))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   const std::filesystem::path path = BuildTablePath(q);

   const std::filesystem::path filePath(path);
   std::error_code ec;
   if (std::filesystem::is_regular_file(filePath, ec)) {
      if (ZipUtils::IsExtractable(filePath)) {
         if (ZipUtils::Extract(filePath, filePath.parent_path(), nullptr)) {
            PLOGI.printf("File extracted: q=%s", q);
            SetLastUpdate();
            mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
         }
         else
            mg_http_reply(c, STATUS_INTERNAL_SERVER_ERROR, "", RESPONSE_INTERNAL_SERVER_ERROR);
      }
      else
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::Command(struct mg_connection *c, struct mg_http_message* hm)
{
   char cmd[1024];
   mg_http_get_var(&hm->query, "cmd", cmd, sizeof(cmd));

   if (*cmd == '\0') {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return;
   }

   if (!strncmp(cmd, "fps", sizeof(cmd))) {
      if (g_pplayer && g_pplayer->m_liveUI) {
         g_pplayer->m_liveUI->ToggleFPS();
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
   }
   else if (!strncmp(cmd, "shutdown", sizeof(cmd))) {
      if (g_pplayer) {
         g_pplayer->SetCloseState(Player::CS_CLOSE_CAPTURE_SCREENSHOT);
         mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
      }
      else
         mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
   }
   else if (!strncmp(cmd, "cls", sizeof(cmd))) {
      {
         std::lock_guard<std::mutex> lock(s_logMutex);
         s_recentLogs.clear();
      }
      json j = {{"status", "success"}, {"message", "Logs cleared"}};
      string response = j.dump();
      mg_http_reply(c, STATUS_OK, HEADER_JSON, "%s", response.c_str());
   }
   else if (!strncmp(cmd, "refresh_tables", sizeof(cmd))) {
      NotifyTablesChanged();
      mg_http_reply(c, STATUS_OK, "", RESPONSE_OK);
   }
   else
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
}

void WebServer::LogStream(struct mg_connection *c, struct mg_http_message* hm)
{
   mg_printf(c, "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/event-stream\r\n"
              "Cache-Control: no-cache\r\n"
              "Connection: keep-alive\r\n"
              "Access-Control-Allow-Origin: *\r\n"
              "\r\n");

   c->is_resp = 0;

   {
      std::lock_guard<std::mutex> lock(s_logMutex);
      s_logConnections.push_back(c->id);

      for (const auto& logLine : s_recentLogs) {
         string data = "data: " + logLine + "\n\n";
         mg_send(c, data.c_str(), data.length());
      }
   }
}

void WebServer::AddLogEntry(const string& formattedLog)
{
   std::lock_guard<std::mutex> lock(s_logMutex);

   if (!s_recentLogs.empty() && s_recentLogs.back() == formattedLog)
      return;

   s_recentLogs.push_back(formattedLog);

   while (s_recentLogs.size() > MAX_RECENT_LOGS)
      s_recentLogs.pop_front();

   BroadcastLogEntry(formattedLog);
}

void WebServer::BroadcastLogEntry(const string& formattedLog)
{
   if (s_logConnections.empty())
      return;

   const string data = "data: " + formattedLog + "\n\n";

   for (const unsigned long id : s_logConnections)
      mg_wakeup(&m_mgr, id, data.c_str(), data.length());
}

void WebServer::BroadcastStatus()
{
   if (s_instance == nullptr) return;

   // The lobby of the launcher mode is not a table, even when it is loaded from one to use its VR room
   bool running = g_pplayer != nullptr && !g_pplayer->m_isLobby;
   string currentTable = running ? g_pplayer->m_ptable->m_filename.string() : ""s;

   json j = {
      {"running", running},
      {"currentTable", currentTable.empty() ? nullptr : json(currentTable)},
      {"lastUpdate", s_lastUpdateTimestamp}
   };

   const string response = j.dump();

   std::lock_guard<std::mutex> lock(s_logMutex);
   if (s_statusConnections.empty()) return;
   for (const unsigned long id : s_statusConnections)
      mg_wakeup(&s_instance->m_mgr, id, response.c_str(), response.length());
}

string WebServer::GetIPAddress()
{
#ifdef _WIN32
   // First IPv4 address of an adapter that is up and has a gateway (Wi-Fi or Ethernet, not loopback nor virtual adapters without route)
   ULONG size = 16 * 1024;
   std::vector<uint8_t> buffer(size);
   auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
   constexpr ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
   if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) == ERROR_BUFFER_OVERFLOW)
   {
      buffer.resize(size);
      adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
   }
   if (GetAdaptersAddresses(AF_INET, flags, nullptr, adapters, &size) != NO_ERROR)
      return string();
   for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
   {
      if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->FirstGatewayAddress == nullptr)
         continue;
      for (const IP_ADAPTER_UNICAST_ADDRESS* address = adapter->FirstUnicastAddress; address != nullptr; address = address->Next)
      {
         char host[NI_MAXHOST];
         if (address->Address.lpSockaddr->sa_family == AF_INET
            && getnameinfo(address->Address.lpSockaddr, address->Address.iSockaddrLength, host, NI_MAXHOST, nullptr, 0, NI_NUMERICHOST) == 0)
            return host;
      }
   }
   return string();
#else
   struct ifaddrs *ifaddr;
   struct ifaddrs *ifa;

   if (getifaddrs(&ifaddr) == -1)
      return string();

   for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr)
         continue;

      int family = ifa->ifa_addr->sa_family;

      if (family == AF_INET) {
         if (strncmp(ifa->ifa_name, "wlan", 4) == 0 || strncmp(ifa->ifa_name, "eth", 3) == 0 || strncmp(ifa->ifa_name, "en", 2) == 0) {
            char host[NI_MAXHOST];
            int s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
            freeifaddrs(ifaddr);
            if (s != 0)
               return string();
            else
               return host;
         }
      }
   }

   freeifaddrs(ifaddr);

   return string();
#endif
}

bool WebServer::ValidatePathParameter(struct mg_connection *c, struct mg_http_message* hm, const char* paramName, string& outValue)
{
   char buffer[1024];
   mg_http_get_var(&hm->query, paramName, buffer, sizeof(buffer));

   if (*buffer == '\0' || !mg_path_is_sane(mg_str(buffer))) {
      mg_http_reply(c, STATUS_BAD_REQUEST, "", "%s", RESPONSE_BAD_REQUEST);
      return false;
   }

   outValue = buffer;
   return true;
}

std::filesystem::path WebServer::BuildTablePath(const char* relativePath)
{
   // Requests use UTF-8
#ifdef __LIBVPINBALL__
   return g_app->m_fileLocator.GetAppPath(FileLocator::AppSubFolder::Tables) / UTF8ToPath(relativePath);
#else
   // On desktop, the default tables location is the user's documents folder: only expose the folder owned by the table library
   return g_app->GetTableLibrary().GetTablesPath() / UTF8ToPath(relativePath);
#endif
}

