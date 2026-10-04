// license:GPLv3+

#pragma once

#include "Settings.h"
#include "FileLocator.h"

namespace VPinballLib { class TableLibrary; class ScoreStore; }
class WebServer; // Only with VPX_TABLE_WEBSERVER, defined by the macOS, Linux and windows-mingw builds (mobile builds have their own instance)
namespace VPX { class Window; }
class VRDevice;


class VPApp final
{
public:
   VPApp();
   ~VPApp();

   void SetCommandLineCustomSettingsFileName(const std::filesystem::path& path) { m_commandLineCustomSettingsFileName = path; } // Must be defined before InitInstance() is called, otherwise it will be ignored
   void InitInstance(bool isPlay = false);

   // overall app settings
   Settings m_settings;

   FileLocator m_fileLocator;

   void LimitMultiThreading();
   int GetLogicalNumberOfProcessors() const;

   // Global custom parameters that can be set through command line and accessed in the script via GetCustomParam(X)
   wstring m_customParameters[MAX_CUSTOM_PARAM_INDEX];

#ifndef ENABLE_BGFX
   // FIXME Deprecated command line options (supposed to be handled through INI nowadays)
   bool m_bgles = false; // override global emission scale by m_fgles below
   float m_fgles = 0.f;
#endif

   // Script security level
   int m_securitylevel;

   // Display resources kept from one table to the next when tables are played one after another (m_keepDisplayBetweenTables), so that
   // switching tables neither closes the window nor makes the VR runtime see the application quit and start again. They are released
   // when the application closes. Otherwise, they are released when the table which used them is closed (editor, mobile builds).
   bool m_keepDisplayBetweenTables = false;
   // The VR device (OpenXR instance) is not kept, only the windows are: SteamVR (2.17.10 on the Steam Frame) ties its Vulkan objects to the
   // OpenXR instance and reuses them for the next session after destroying them with the previous one (crash in the driver at xrEndFrame),
   // and creating a new Vulkan device under the same instance makes it call the destroyed VkInstance (abort in the loader). Found on the
   // Frame on 2026-10-02 with the Vulkan API dump layer. A runtime fixing this could set it back to true.
   static constexpr bool m_keepVRDeviceBetweenTables = false;
   // Window kept from the previous table if it was created with the same settings, otherwise a new one
   VPX::Window* AcquireWindow(int windowId, const string& title, const Settings& settings);
   void ReleaseWindow(VPX::Window* wnd);
#ifdef ENABLE_XR
   // VR device (OpenXR instance) kept from the previous table, or a new one (which may not be ready, see VRDevice::IsOpenXRReady)
   VRDevice* AcquireVRDevice(const Settings& settings);
   void ReleaseVRDevice(bool discard); // discard: destroy it even if devices are kept, for example when the headset was not found
#endif
   void ReleaseDisplayResources();
   uint64_t m_lastTableCloseTime = 0; // When the last table started to close (steady clock, in microseconds), to log how long switching tables takes

#ifndef __STANDALONE__
   static CComModule m_module;
   class WinApp final : public CWinApp
   {
   public:
      WinApp() = default;
   protected:
      BOOL OnIdle(LONG) override;
      BOOL PreTranslateMessage(MSG& msg) override;
   } m_winApp;
   HINSTANCE GetInstanceHandle() const { return m_winApp.GetInstanceHandle(); }
#else
   HINSTANCE GetInstanceHandle() const { return nullptr; }

   // Tables managed by the application (in-game table picker), created on first use
   VPinballLib::TableLibrary& GetTableLibrary();

   // Player profiles and the scores of their games (profiles.json, scores.json), created on first use
   VPinballLib::ScoreStore& GetScoreStore();
   // Scores recorded while playing the last table, shown by the lobby when it gets back (see ScoreTracker), then cleared
   struct SessionResult
   {
      string tableUuid;
      vector<string> scoreIds;
   };
   std::optional<SessionResult> m_lastSessionResult;

#ifdef VPX_TABLE_WEBSERVER
   // Web server to manage the table library from a browser. It runs from its activation in the table picker until the application is closed,
   // across the lobby and the tables, and is started with the application when set to be always on (Standalone.WebServerAlwaysOn).
   WebServer& GetWebServer();
   void StartWebServerIfAlwaysOn();
#endif

   // When no PinMAME folder is defined, use a 'pinmame' folder shared by all tables inside the tables folder (created if needed),
   // so that ROMs can be added like tables, for example from a browser. Must be called before the plugins are loaded.
   void SetupSharedPinMAMEFolder();
   bool IsSharedPinMAMEFolderUsed() const { return m_isSharedPinMAMEFolderApplied; }

   // Table to play after the running one is closed, which allows to switch table without leaving the application
   std::filesystem::path m_nextTableFilename;

   // Launcher mode ('-Launcher' command line option): the application starts on a lobby table with the table picker opened,
   // and gets back to it when a table is closed, until the user quits from the lobby or the picker
   bool m_launcherMode = false;
   std::filesystem::path GetLobbyTablePath() const;

   // The VR room of a table can replace the default room of the lobby: the lobby then loads that table, and only keeps the parts of its room
   // which were visible when it was chosen (kept in lobby-room.json). Without that table, the lobby gets back to the default room.
   struct LobbyRoom
   {
      std::filesystem::path tablePath;
      string tableName;
      vector<string> parts;
   };
   std::optional<LobbyRoom> GetLobbyRoom();
   int UseTableRoomInLobby(PinTable* table); // Returns the number of room parts kept, 0 if the table has no VR room (then nothing changes)
   void ResetLobbyRoom();
   static vector<IEditable*> GetRoomParts(PinTable* table); // All the parts of its VR room, visible or not
   // The 'Visible' property that scripts use, for the parts which have one
   static std::optional<bool> GetPartVisible(IEditable* part);
   static void SetPartVisible(IEditable* part, bool visible);
   bool m_playingLobby = false; // Set while the lobby is played

   // What the images of the tables captured in VR show (Standalone.TableImageFocus), for the menus that choose it
   static string GetTableImageFocusLabel();
   static void NextTableImageFocus();
   bool m_reloadLobby = false; // Load the lobby again when it closes, for example to apply a new room

   // Message of the loading screen shown while switching tables (see LiveUI::SetLoadingText): for a table (its name in the library), or for what follows
   // the table being closed (the next table, or the lobby in launcher mode; empty when the application quits)
   string GetLoadingText(const std::filesystem::path& tablePath, bool lobby);
   string GetNextLoadingText(bool playingLobby);
#endif

private:
   struct KeptWindow
   {
      VPX::Window* window;
      int windowId;
      string config; // Settings the window was created with (see VPX::Window::GetConfigKey)
   };
   vector<KeptWindow> m_keptWindows; // Windows not used by the current table, kept for the next one
   vector<KeptWindow> m_usedWindows; // Windows acquired by the current table
   VRDevice* m_vrDevice = nullptr;

#ifdef __STANDALONE__
   std::unique_ptr<VPinballLib::TableLibrary> m_tableLibrary;
   std::unique_ptr<VPinballLib::ScoreStore> m_scoreStore;
   bool m_isSharedPinMAMEFolderApplied = false;
#ifdef VPX_TABLE_WEBSERVER
   std::unique_ptr<WebServer> m_webServer;
#endif
#endif
   std::filesystem::path m_commandLineCustomSettingsFileName; // Override default ini filename, must be defined before InitInstance
   int m_logicalNumberOfProcessors = -1;
};
