// license:GPLv3+

#pragma once

#include "Settings.h"
#include "FileLocator.h"

namespace VPinballLib { class TableLibrary; }
class WebServer; // Only with VPX_TABLE_WEBSERVER, defined by the macOS, Linux and windows-mingw builds (mobile builds have their own instance)


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

#ifdef VPX_TABLE_WEBSERVER
   // Web server to manage the table library from a browser. It is never started automatically, and stopped when a table is launched.
   WebServer& GetWebServer();
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
#endif

private:
#ifdef __STANDALONE__
   std::unique_ptr<VPinballLib::TableLibrary> m_tableLibrary;
   bool m_isSharedPinMAMEFolderApplied = false;
#ifdef VPX_TABLE_WEBSERVER
   std::unique_ptr<WebServer> m_webServer;
#endif
#endif
   std::filesystem::path m_commandLineCustomSettingsFileName; // Override default ini filename, must be defined before InitInstance
   int m_logicalNumberOfProcessors = -1;
};
