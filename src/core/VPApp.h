// license:GPLv3+

#pragma once

#include "Settings.h"
#include "FileLocator.h"

namespace VPinballLib { class TableLibrary; }
class WebServer; // Only with VPX_TABLE_WEBSERVER, defined by the macOS and Linux builds (it uses POSIX network functions, and mobile builds have their own instance)


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
