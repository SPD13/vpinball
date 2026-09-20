// license:GPLv3+

#include "core/stdafx.h"
#include "AppCommands.h"

#include "extern.h"
#include "core/TournamentFile.h"
#include "core/VPApp.h"
#include "core/editablereg.h"
#include "lib/src/TableLibrary.h"
#include "parts/Material.h"
#include "parts/pintable.h"
#include "parts/PartGroup.h"
#include "parts/primitive.h"
#include "ui/VPXFileFeedback.h"
#include "ui/live/LiveUI.h"
#include "ui/win/WinEditor.h"
#include "utils/BiffReader.h"

#include "pole/pole.h"

#include <iostream>
#include <fstream>


ShowInfoAndExitCommand::ShowInfoAndExitCommand(const string& title, const string& message, int exitCode)
   : m_title(title)
   , m_message(message)
   , m_exitCode(exitCode)
{
}

void ShowInfoAndExitCommand::Execute()
{
   if (!m_title.empty())
      std::cout << m_title << "\n\n";
   if (!m_message.empty())
      std::cout << m_message << "\n\n";
#ifndef __STANDALONE__
   ::MessageBox(nullptr, m_message.c_str(), m_title.empty() ? "Visual Pinball" : m_title.c_str(), MB_OK);
#endif
   exit(m_exitCode);
}


TableBasedCommand::TableBasedCommand(const std::filesystem::path& tableFilename)
   : m_tableFilename(tableFilename)
{
}

CComObject<PinTable>* TableBasedCommand::LoadTable()
{
   CComObject<PinTable>* table;
   CComObject<PinTable>::CreateInstance(&table);
   table->AddRef();
   VPXFileFeedback feedback;
   table->LoadGameFromFilename(m_tableFilename.string(), feedback);
   if (!m_tableIniFileName.empty() && FileExists(m_tableIniFileName))
      table->SetSettingsFileName(m_tableIniFileName);
   return table;
}


ExportVBSCommand::ExportVBSCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

void ExportVBSCommand::Execute()
{
   string script;
   POLE::Storage rootStorage(m_tableFilename.string().c_str());
   rootStorage.open();
   if (rootStorage.result() == POLE::Storage::Ok && rootStorage.exists("GameStg/Version") && rootStorage.exists("GameStg/GameData"))
   {
      int loadfileversion = 0;
      POLE::Stream versionStream(&rootStorage, "GameStg/Version");
      versionStream.read(reinterpret_cast<unsigned char*>(&loadfileversion), sizeof(int));
      bool isProtected = false;
      POLE::Stream gameStream(&rootStorage, "GameStg/GameData");
      BiffReader reader(&gameStream, loadfileversion, 0, 0);
      reader.AsObject(
         [&script, &isProtected](int tag, IObjectReader& reader)
         {
            switch (tag)
            {
            case FID(SECB): // old protection/encryption data
            {
               struct ProtectionData
               {
                  int32_t fileversion;
                  int32_t size;
                  uint8_t paraphrase[16 + 8];
                  uint32_t flags;
                  int32_t keyversion;
                  int32_t spare1;
                  int32_t spare2;
               } protectionData;
               reader.AsRaw(&protectionData, sizeof(ProtectionData));
               isProtected = ((protectionData.flags & DISABLE_EVERYTHING) == DISABLE_EVERYTHING) || ((protectionData.flags & DISABLE_SCRIPT_EDITING) == DISABLE_SCRIPT_EDITING);
               break;
            }
            case FID(CODE): script = reader.AsScript(isProtected); break;
            }
            return true;
         });
   }

   //CComObject<PinTable>* table = LoadTable();
   std::filesystem::path scriptFilename = m_tableFilename;
   scriptFilename.replace_extension(".vbs");
   if (std::ofstream outFile(scriptFilename, std::ios::binary); outFile)
   {
      //outFile.write(table->m_original_table_script.data(), table->m_original_table_script.size());
      outFile << script;
      outFile.close();
   }
   //table->Release();
}


ExportPOVCommand::ExportPOVCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

void ExportPOVCommand::Execute()
{
   CComObject<PinTable>* table = LoadTable();
   for (int i = 0; i < 3; i++)
      table->mViewSetups[i].SaveToTableOverrideSettings(table->m_settings, (ViewSetupID)i);
   table->m_settings.Save();
   table->Release();
}


PlayTableCommand::PlayTableCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

#ifdef __STANDALONE__
// A primitive made of a single horizontal quad (the same way the player builds its implicit playfield)
static Primitive* CreateQuad(PinTable* table, const wstring& name, float left, float top, float right, float bottom)
{
   Primitive* quad = static_cast<Primitive*>(EditableRegistry::CreateAndInit(ItemTypeEnum::eItemPrimitive, table, 0, 0));
   quad->SetName(name);
   quad->m_desktopBackdrop = false;
   quad->m_d.m_collidable = false;
   quad->m_d.m_toy = true;
   quad->m_d.m_use3DMesh = true;
   quad->m_d.m_vSize.Set(1.0f, 1.0f, 1.0f);
   quad->m_mesh.m_vertices.resize(4);
   for (unsigned int i = 0; i < 4; i++)
   {
      quad->m_mesh.m_vertices[i].x = (i & 1) ? right : left;
      quad->m_mesh.m_vertices[i].y = (i & 2) ? bottom : top;
      quad->m_mesh.m_vertices[i].z = 0.f;
      quad->m_mesh.m_vertices[i].tu = (i & 1) ? 1.f : 0.f;
      quad->m_mesh.m_vertices[i].tv = (i & 2) ? 1.f : 0.f;
      quad->m_mesh.m_vertices[i].nx = 0.f;
      quad->m_mesh.m_vertices[i].ny = 0.f;
      quad->m_mesh.m_vertices[i].nz = 1.f;
   }
   quad->m_mesh.m_indices = { 0, 1, 2, 2, 1, 3 };
   quad->m_mesh.m_validBounds = false;
   return quad;
}

// The lobby is what is displayed behind the table picker of the launcher mode: an empty room with a floor. A table file can only be
// authored with the Windows editor, so for the time being the lobby is made from the blank table shipped with the application, by
// removing everything it contains (parts, script, playfield) and adding a floor.
static void BuildLobby(PinTable* table)
{
   // Displayed by the home page of the in-game menu
   table->m_tableName = "Visual Pinball"s;
   table->m_blurb.clear();
   table->m_description = "Select 'Tables' to play a table."s;
   table->m_rules.clear();
   table->m_author.clear();
   table->m_version.clear();

   table->m_original_table_script = "Option Explicit\n"s;
   table->m_script_text = table->m_original_table_script;
   for (int i = 0; i < table->m_vcollection.size(); i++)
      table->m_vcollection[i].ClearParts();
   // Parts hold a reference on their group, so groups go last
   for (const bool removeGroups : { false, true })
   {
      const vector<IEditable*> parts = table->GetParts();
      for (IEditable* part : parts)
         if ((part->GetItemType() == ItemTypeEnum::eItemPartGroup) == removeGroups)
            table->RemovePart(part);
   }

   // Outside of VR, look at the room like someone standing in front of a table. The legacy layout of the base table can not be used,
   // as it places the camera by fitting it to the parts that were just removed.
   for (ViewSetup& viewSetup : table->mViewSetups)
   {
      viewSetup = ViewSetup();
      viewSetup.mMode = VLM_CAMERA;
      viewSetup.mLookAt = 300.f; // In percent of the table length: look a few meters ahead instead of down at a playfield, to see the floor up to its far edge
   }

   // An invisible explicit playfield, otherwise the player adds the default one
   Primitive* playfield = CreateQuad(table, L"playfield_mesh"s, table->m_left, table->m_top, table->m_right, table->m_bottom);
   playfield->m_d.m_visible = false;
   table->AddPart(playfield);
   playfield->Release();

   // The floor lies in the room space, which in VR is the real world: z = 0 is the ground, without the playfield inclination
   PartGroup* room = static_cast<PartGroup*>(EditableRegistry::CreateAndInit(ItemTypeEnum::eItemPartGroup, table, 0, 0));
   room->SetName(L"LobbyRoom"s);
   room->m_d.m_spaceReference = PartGroupData::SpaceReference::SR_ROOM;
   table->AddPart(room);

   // Two shades, as a uniform floor gives no sense of distance
   Material* floorMaterials[2];
   for (int i = 0; i < 2; i++)
   {
      floorMaterials[i] = new Material();
      floorMaterials[i]->m_name = i == 0 ? "LobbyFloorDark"s : "LobbyFloorLight"s;
      floorMaterials[i]->m_cBase = i == 0 ? RGB(58, 62, 70) : RGB(78, 83, 92);
      floorMaterials[i]->m_fRoughness = 0.8f;
      table->AddMaterial(floorMaterials[i]);
   }

   // 12 x 12 meters centered on the place where the player stands, made of tiles: outside of VR, the near clipping plane is derived
   // from the corners of the bounding box of each part, so a single large quad would be clipped away as its corners are all far away
   constexpr int nTiles = 6;
   const float tileSize = CMTOVPU(200.f);
   const float left = 0.5f * (table->m_left + table->m_right) - 0.5f * static_cast<float>(nTiles) * tileSize;
   const float top = table->m_bottom - 0.5f * static_cast<float>(nTiles) * tileSize;
   for (int y = 0; y < nTiles; y++)
   {
      for (int x = 0; x < nTiles; x++)
      {
         const float tileLeft = left + static_cast<float>(x) * tileSize, tileTop = top + static_cast<float>(y) * tileSize;
         Primitive* tile = CreateQuad(table, L"LobbyFloor_"s + std::to_wstring(x) + L'_' + std::to_wstring(y), tileLeft, tileTop, tileLeft + tileSize, tileTop + tileSize);
         tile->m_d.m_szMaterial = floorMaterials[(x + y) & 1]->m_name;
         tile->m_d.m_staticRendering = true;
         tile->m_d.m_disableLightingBelow = 1.f;
         tile->SetPartGroup(room);
         table->AddPart(tile);
         tile->Release();
      }
   }
   room->Release();
   table->Undo(true);
}
#endif

void PlayTableCommand::Play(bool openTablePicker)
{
#ifdef __STANDALONE__
   g_app->SetupSharedPinMAMEFolder();
#endif
   CComObject<PinTable>* table = LoadTable();
#ifdef __STANDALONE__
   if (openTablePicker)
      BuildLobby(table);
   else
      g_app->GetTableLibrary().RecordPlay(m_tableFilename); // For the 'Recent' and 'Most played' lists of the table picker
#endif
   auto player = std::make_unique<Player>(table, Player::PlayMode::Play);
#ifdef __STANDALONE__
   if (openTablePicker)
      player->m_liveUI->OpenInGameUI("tables/picker"s);
#endif
   player->GameLoop();
   player = nullptr;
   table->Release();
}

#ifdef __STANDALONE__
// The in-game table picker selects the next table to play, then closes the player
static void PlaySelectedTables()
{
   while (!g_app->m_nextTableFilename.empty())
   {
      PlayTableCommand nextTable(std::exchange(g_app->m_nextTableFilename, std::filesystem::path()));
      nextTable.Play();
   }
}
#endif

void PlayTableCommand::Execute()
{
   Play();
#ifdef __STANDALONE__
   PlaySelectedTables();
#endif
}


#ifdef __STANDALONE__
LauncherCommand::LauncherCommand()
{
}

void LauncherCommand::Execute()
{
   // Closing a table gets back to the lobby, closing the lobby without selecting a table quits
   g_app->m_launcherMode = true;
   while (g_app->m_launcherMode)
   {
      PlayTableCommand lobby(g_app->GetLobbyTablePath());
      lobby.Play(true);
      if (g_app->m_nextTableFilename.empty())
         break;
      PlaySelectedTables();
   }
}
#endif


AuditTableCommand::AuditTableCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

void AuditTableCommand::Execute()
{
   CComObject<PinTable>* table = LoadTable();
   table->AuditTable(true);
   table->Release();
}


PovEditCommand::PovEditCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

void PovEditCommand::Execute()
{
   CComObject<PinTable>* table = LoadTable();
   auto player = std::make_unique<Player>(table, Player::PlayMode::EditPOV);
   player->GameLoop();
   player = nullptr;
   table->Release();
}


#ifndef __STANDALONE__
Win32EditCommand::Win32EditCommand()
   : Win32EditCommand(""s)
{
}

Win32EditCommand::Win32EditCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

void Win32EditCommand::Execute()
{
   WinEditor vpxEditor(g_app->GetInstanceHandle());
   g_pvp = &vpxEditor;
   vpxEditor.m_open_minimized = m_minimized;
   vpxEditor.m_disable_pause_menu = m_disablePauseMenu;
   vpxEditor.Create(nullptr);
   vpxEditor.LoadEditorSetupFromSettings();
   if (!m_tableFilename.empty() && FileExists(m_tableFilename))
   {
      vpxEditor.LoadFileName(m_tableFilename.string(), true);
      if (!m_tableIniFileName.empty() && FileExists(m_tableIniFileName) && vpxEditor.GetActiveTable())
         vpxEditor.GetActiveTable()->SetSettingsFileName(m_tableIniFileName);
   }
   else if (g_app->m_settings.GetEditor_SelectTableOnStart())
   {
      vpxEditor.m_table_played_via_SelectTableOnStart = vpxEditor.LoadFile(false);
      if (vpxEditor.m_table_played_via_SelectTableOnStart)
         vpxEditor.DoPlay(0);
   }
   g_app->m_winApp.Run();
   g_pvp = nullptr;
}
#endif


LiveEditCommand::LiveEditCommand()
   : LiveEditCommand(""s)
{
}

LiveEditCommand::LiveEditCommand(const std::filesystem::path& tableFilename)
   : TableBasedCommand(tableFilename)
{
}

void LiveEditCommand::Execute()
{
   CComObject<PinTable>* table = LoadTable();
   auto player = std::make_unique<Player>(table, Player::PlayMode::FullEdit);
   player->GameLoop();
   player = nullptr;
   table->Release();
}


CaptureAttractCommand::CaptureAttractCommand(const std::filesystem::path& tableFilename, int nFrames, int framesPerSecond, bool cutToLoop)
   : TableBasedCommand(tableFilename)
   , m_nFrames(nFrames)
   , m_framesPerSecond(framesPerSecond)
   , m_cutToLoop(cutToLoop)
{
}

void CaptureAttractCommand::Execute()
{
   PLOGI << "Video capture mode requested for " << m_nFrames << " frames at " << m_framesPerSecond << "FPS from table '" << m_tableFilename << "' " << (m_cutToLoop ? "with " : "without ")
         << "loop truncation";
   CComObject<PinTable>* table = LoadTable();
   auto player = std::make_unique<Player>(table, Player::PlayMode::CaptureAttract);
   player->m_nFrameToCapture = m_nFrames;
   player->m_frameCaptureFPS = m_framesPerSecond;
   player->m_cutCaptureToLoop = m_cutToLoop;
   player->GameLoop();
   player = nullptr;
   table->Release();
}


ValidateTournamentCommand::ValidateTournamentCommand(const std::filesystem::path& tableFilename, const std::filesystem::path& tournamentFilename)
   : TableBasedCommand(tableFilename)
   , m_tournamentFilename(tournamentFilename)
{
}

void ValidateTournamentCommand::Execute()
{
   CComObject<PinTable>* table = LoadTable();
   VPX::TournamentFile::GenerateImageFromTournamentFile(table, m_tournamentFilename);
   table->Release();
}



#ifdef __STANDALONE__
int g_argc;
const char **g_argv;
#endif

enum option_names
{
   OPTION_H,
   OPTION_HELP,
   OPTION_QMARK,
#ifndef __STANDALONE__
   OPTION_UNREGSERVER,
   OPTION_REGSERVER,
   OPTION_MINIMIZED,
   OPTION_EXTMINIMIZED,
   OPTION_EDIT,
#endif
   OPTION_LIVE_EDIT,
   OPTION_PLAY,
#ifdef __STANDALONE__
   OPTION_LAUNCHER,
#endif
   OPTION_POVEDIT,
   OPTION_POV,
   OPTION_EXTRACTVBS,
   OPTION_INI,
   OPTION_TABLE_INI,
   OPTION_TOURNAMENT,
   OPTION_VERSION,
   OPTION_FRONTEND_EXIT,
   OPTION_AUDIT,
   OPTION_CAPTURE_ATTRACT,
   OPTION_CUSTOM1,
   OPTION_CUSTOM2,
   OPTION_CUSTOM3,
   OPTION_CUSTOM4,
   OPTION_CUSTOM5,
   OPTION_CUSTOM6,
   OPTION_CUSTOM7,
   OPTION_CUSTOM8,
   OPTION_CUSTOM9,
   OPTION_PREFPATH,
   OPTION_LESSCPUTHREADS,
#ifndef ENABLE_BGFX
   OPTION_GLES,
#endif
   OPTION_INVALID,
};
struct CommandLineOption
{
   option_names name;
   string arg;
   string desc;
};

static const CommandLineOption options[] = {
   { OPTION_H, "h"s, string() },
   { OPTION_HELP, "help"s, string() },
   { OPTION_QMARK, "?"s, string() },
#ifndef __STANDALONE__
   { OPTION_UNREGSERVER, "UnregServer"s, "Unregister VP functions"s },
   { OPTION_REGSERVER, "RegServer"s,"Register VP functions"s },
   { OPTION_MINIMIZED, "Minimized"s, "Start the windows editor in the 'invisible' minimized window mode"s },
   { OPTION_EXTMINIMIZED, "ExtMinimized"s, "Start the windows editor in the 'invisible' minimized window mode, but with enabled Pause Menu"s },
   { OPTION_EDIT, "Edit"s, "[filename]  Load file into VP"s },
#endif
   { OPTION_LIVE_EDIT, "LiveEdit"s, "[opt filename]  Start in live editor mode. if a filename is provided, loads it as the table to edit. WARNING Unstable feature only provided for early testing"s },
   { OPTION_PLAY, "Play"s, "[filename]  Load and play file"s },
#ifdef __STANDALONE__
   { OPTION_LAUNCHER, "Launcher"s, "Start with the table picker, to select and play tables without leaving the application"s },
#endif
   { OPTION_POVEDIT, "PovEdit"s, "[filename]  Load and run file in live editing mode, then export new pov on exit"s },
   { OPTION_POV, "Pov"s, "[filename]  Load, export pov and close"s },
   { OPTION_EXTRACTVBS, "ExtractVBS"s, "[filename]  Load, export table script and close"s },
   { OPTION_INI, "Ini"s, "[filename]  Use a custom settings file instead of loading it from the default location"s },
   { OPTION_TABLE_INI, "TableIni"s, "[filename]  Use a custom table settings file. This option is only available in conjunction with a command which specifies a table filename like Play, Edit,..."s },
   { OPTION_TOURNAMENT, "TournamentFile"s, "[table filename] [tournament filename]  Load a table and tournament file and convert to .png"s },
   { OPTION_VERSION, "v"s, "Displays the version"s },
   { OPTION_FRONTEND_EXIT, "exit"s, ""s }, // (ab)used by frontend, not handled by us
   { OPTION_AUDIT, "Audit"s, "[table filename] Audit the table"s },
   { OPTION_CAPTURE_ATTRACT, "CaptureAttract"s, "Capture an attract mode video"s },
   { OPTION_CUSTOM1, "c1"s, "Custom value 1"s },
   { OPTION_CUSTOM2, "c2"s, "Custom value 2"s },
   { OPTION_CUSTOM3, "c3"s, "Custom value 3"s },
   { OPTION_CUSTOM4, "c4"s, "Custom value 4"s },
   { OPTION_CUSTOM5, "c5"s, "Custom value 5"s },
   { OPTION_CUSTOM6, "c6"s, "Custom value 6"s },
   { OPTION_CUSTOM7, "c7"s, "Custom value 7"s },
   { OPTION_CUSTOM8, "c8"s, "Custom value 8"s },
   { OPTION_CUSTOM9, "c9"s, "Custom value 9"s },
   { OPTION_PREFPATH, "PrefPath"s, "[path]  Use a custom preferences path instead of default"s },
   { OPTION_LESSCPUTHREADS, "LessCPUthreads"s, "Limit the amount of parallel execution"s },
#ifndef ENABLE_BGFX
   { OPTION_GLES, "GLES"s, "[value]  Overrides the global emission scale (day/night setting, value range: 0.115..0.925) [Deprecated, uses ini serttings instead]"s },
#endif
};

std::filesystem::path CommandLineProcessor::GetPathFromArg(const string& arg)
{
   string pathString = trim_string(arg);

   // On Windows only, we used to remove leading - or /. This is removed as this is not cross platform and is a bug on the front end side
   //if (!pathString.empty() && ((pathString[0] == '-') || (pathString[0] == '/')))
   //   pathString = pathString.substr(1);

   if (pathString.length() >= 2 && pathString[0] == '"') // Remove " "
      pathString = pathString.substr(1, pathString.size() - 2);

   std::filesystem::path path(pathString);
   path = std::filesystem::absolute(path);
   path = std::filesystem::weakly_canonical(path);
   return path;
}

string CommandLineProcessor::GetCommandLineHelp()
{
   std::stringstream ss;
   for (const auto& opt : options)
   {
      if (opt.name >= OPTION_CUSTOM1 && opt.name <= OPTION_CUSTOM9)
         continue;
      ss << '-' << opt.arg << "  " << opt.desc << '\n';
   }
   ss << "\n\n-c1 [customparam] .. -c9 [customparam]  Custom user parameters that can be accessed in the script via GetCustomParam(X)";
   return ss.str();
}

void CommandLineProcessor::OnCommandLineError(const string& title, const string& message)
{
   #ifndef __STANDALONE__
      MessageBox(nullptr, message.c_str(), title.c_str(), MB_ICONERROR);
   #else
      std::cout << title << "\n\n" << message << "\n\n";
   #endif
}

// A Visual Pinball table is an OLE compound file (CFB). Validate the signature so passing a
// non-table file (wrong type, or a path typo resolving to something else) reports a clear error
// instead of loading into a black screen.
static bool IsTableFile(const std::filesystem::path& path)
{
   std::ifstream f(path, std::ios::binary);
   if (!f.is_open())
      return false;
   unsigned char sig[8] = {};
   f.read(reinterpret_cast<char*>(sig), sizeof(sig));
   static constexpr unsigned char cfb[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
   if (f.gcount() != static_cast<std::streamsize>(sizeof(sig)))
      return false;
   for (size_t k = 0; k < sizeof(sig); ++k)
      if (sig[k] != cfb[k])
         return false;
   return true;
}

void CommandLineProcessor::ProcessCommandLine()
{
#ifndef __STANDALONE__
   int nArgs;
   const char** szArglist = CommandLineToArgvA(GetCommandLine(), &nArgs);
   ProcessCommandLine(nArgs, szArglist);
   free(szArglist);
#else
   ProcessCommandLine(g_argc, g_argv);
#endif
}

void CommandLineProcessor::ProcessCommandLine(int nArgs, const char* szArglist[])
{
   g_app->SetCommandLineCustomSettingsFileName(""s);

   std::filesystem::path tableIniFileName;
   bool win32EditorMinimized = false;
   bool win32EditorExtMinimized = false;
   bool defaultToWin32Editor = true;
   std::vector<std::unique_ptr<AppCommand>> commands;
   for (int i = 1; i < nArgs; ++i) // skip szArglist[0], contains executable name
   {
      option_names opt = option_names::OPTION_INVALID;
      for (size_t i2 = 0; i2 < std::size(options); ++i2)
      {
         if (StrCompareNoCase(szArglist[i], '-' + options[i2].arg) || StrCompareNoCase(szArglist[i], '/' + options[i2].arg))
         {
            opt = static_cast<option_names>(i2);
            break;
         }
      }
      switch (opt)
      {
      case OPTION_INVALID:
         // If the only parameter passed is a table, consider it a /play command. Play or fail the
         // same on every platform: a missing, directory or non-table argument is an error rather
         // than falling back to the editor.
         if ((nArgs == 2) && (i == 1))
         {
            std::filesystem::path filename = GetPathFromArg(szArglist[i]);
            if (!std::filesystem::exists(filename))
            {
               OnCommandLineError("Command Line Error"s, "Table file '" + filename.string() + "' was not found");
               exit(1);
            }
            if (!IsTableFile(filename))
            {
               OnCommandLineError("Command Line Error"s, "'" + filename.string() + "' is not a Visual Pinball table (.vpx)");
               exit(1);
            }
            commands.push_back(std::make_unique<PlayTableCommand>(filename));
            i++;
            break;
         }
         // ignore stuff (directories) that is passed in via frontends (not considered as invalid)
         if ((szArglist[i][0] != '-') && (szArglist[i][0] != '/'))
            break;
         // Really an invalid parameter, show help and exit
         OnCommandLineError("Visual Pinball Usage"s, "Invalid Parameter "s + szArglist[i] + "\n\nValid Parameters are:\n\n" + GetCommandLineHelp());
         exit(1);
         break;

      case OPTION_INI:
         if (i + 1 >= nArgs)
         {
            OnCommandLineError("Command Line Error"s, "Option '"s + szArglist[i] + "' must be followed by a valid setting file path");
            exit(1);
         }
         g_app->SetCommandLineCustomSettingsFileName(GetPathFromArg(szArglist[i + 1]));
         i++;
         break;

      case OPTION_LESSCPUTHREADS:
         g_app->LimitMultiThreading();
         break;

#ifndef ENABLE_BGFX
      // FIXME remove as this is now handled by the ini system
      case OPTION_GLES: // global emission scale parameter handling
         if (i + 1 < nArgs)
         {
            const char* lpszStr;
            if ((szArglist[i + 1][0] == '-') || (szArglist[i + 1][0] == '/'))
               lpszStr = szArglist[i + 1] + 1;
            else
               lpszStr = szArglist[i + 1];
            g_app->m_fgles = clamp(sz2f(lpszStr), 0.115f, 0.925f);
            g_app->m_bgles = true;
         }
         else
         {
            OnCommandLineError("Command Line Error"s, "Missing global emission scale adjustment"s);
         }
         break;
#endif

      case OPTION_CUSTOM1:
      case OPTION_CUSTOM2:
      case OPTION_CUSTOM3:
      case OPTION_CUSTOM4:
      case OPTION_CUSTOM5:
      case OPTION_CUSTOM6:
      case OPTION_CUSTOM7:
      case OPTION_CUSTOM8:
      case OPTION_CUSTOM9:
         if ((i + 1 < nArgs) && (opt - OPTION_CUSTOM1) <= 9)
         {
            g_app->m_customParameters[opt - OPTION_CUSTOM1] = MakeWString(szArglist[i + 1]);
            ++i; // two params processed
         }
         else
         {
            OnCommandLineError("Command Line Error"s, "Missing custom option value after /c..."s);
         }
         break;

      #ifndef __STANDALONE__
      case OPTION_UNREGSERVER:
         defaultToWin32Editor = false;
         VPApp::m_module.UpdateRegistryFromResource(IDR_VPINBALL, FALSE);
         if (VPApp::m_module.UnregisterServer(TRUE) != S_OK)
            ShowError("Unregister VP functions failed");
         exit(0);
         break;

      case OPTION_REGSERVER:
         defaultToWin32Editor = false;
         VPApp::m_module.UpdateRegistryFromResource(IDR_VPINBALL, TRUE);
         if (VPApp::m_module.RegisterServer(TRUE) != S_OK)
            ShowError("Register VP functions failed");
         exit(0);
         break;

      case OPTION_MINIMIZED:
         win32EditorMinimized = true;
         break;

      case OPTION_EXTMINIMIZED:
         win32EditorExtMinimized = true;
         break;
      #endif

      case OPTION_PREFPATH:
      {
         if (i + 1 >= nArgs)
         {
            OnCommandLineError("Command Line Error"s, "Option '"s + szArglist[i] + "' must be followed by a valid folder path");
            exit(1);
         }
         std::filesystem::path path = GetPathFromArg(szArglist[i + 1]);
         if (!DirExists(path))
         {
            std::error_code ec;
            if (std::filesystem::create_directories(path, ec))
            {
               PLOGI << "Pref path created: " << path;
            }
            else
            {
               PLOGE << "Unable to create pref path: " << path;
            }
         }
         g_app->m_fileLocator.SetPrefPath(path);
         i++;
         break;
      }

      case OPTION_H:
      case OPTION_HELP:
      case OPTION_QMARK:
         commands.push_back(std::make_unique<ShowInfoAndExitCommand>("Visual Pinball Usage"s, GetCommandLineHelp(), 0));
         break;

      case OPTION_VERSION:
         commands.push_back(std::make_unique<ShowInfoAndExitCommand>("", "Visual Pinball "s + VP_VERSION_STRING_FULL_LITERAL, 0));
         break;

#ifdef __STANDALONE__
      case OPTION_LAUNCHER:
         commands.push_back(std::make_unique<LauncherCommand>());
         break;
#endif

      case OPTION_LIVE_EDIT:
         if (i + 1 < nArgs)
         {
            const std::filesystem::path tableFileName = GetPathFromArg(szArglist[i + 1]);
            if (FileExists(tableFileName))
            {
               commands.push_back(std::make_unique<LiveEditCommand>(tableFileName));
               i++;
            }
            else
            {
               commands.push_back(std::make_unique<LiveEditCommand>());
            }
         }
         else
         {
            commands.push_back(std::make_unique<LiveEditCommand>());
         }
         break;

      case OPTION_POVEDIT:
      case OPTION_PLAY:
      case OPTION_AUDIT:
      case OPTION_POV:
      case OPTION_EXTRACTVBS:
      #ifndef __STANDALONE__
         case OPTION_EDIT:
      #endif
      {
         if (i + 1 >= nArgs)
         {
            OnCommandLineError("Command Line Error"s, "Option '"s + szArglist[i] + "' must be followed by a valid table file path");
            exit(1);
         }
         const std::filesystem::path tableFileName = GetPathFromArg(szArglist[i + 1]);
         i++;

         if (!FileExists(tableFileName))
         {
            OnCommandLineError("Command Line Error"s, "Table file '" + tableFileName.string() + "' was not found");
            exit(1);
         }
         else if (!IsTableFile(tableFileName))
         {
            OnCommandLineError("Command Line Error"s, "'" + tableFileName.string() + "' is not a Visual Pinball table (.vpx)");
            exit(1);
         }
         else
         {
            switch (opt)
            {
            case OPTION_POVEDIT: commands.push_back(std::make_unique<PovEditCommand>(tableFileName)); break;
            case OPTION_PLAY: commands.push_back(std::make_unique<PlayTableCommand>(tableFileName)); break;
            case OPTION_AUDIT: commands.push_back(std::make_unique<AuditTableCommand>(tableFileName)); break;
            case OPTION_POV: commands.push_back(std::make_unique<ExportPOVCommand>(tableFileName)); break;
            case OPTION_EXTRACTVBS: commands.push_back(std::make_unique<ExportVBSCommand>(tableFileName)); break;
            #ifndef __STANDALONE__
            case OPTION_EDIT: commands.push_back(std::make_unique<Win32EditCommand>(tableFileName)); break;
            #endif
            }
         }
         break;
      }

      case OPTION_CAPTURE_ATTRACT:
      {
         if (i + 3 >= nArgs)
         {
            OnCommandLineError("Command Line Error"s, "Option '"s + szArglist[i] + "' must be followed by the number of frames to capture, the framerate and a valid table file path");
            exit(1);
         }
         int captureAttract = 0;
         if (!try_parse_int(szArglist[i + 1], captureAttract) || captureAttract <= 0)
         {
            OnCommandLineError("Command Line Error"s, "Invalid number of frames"s);
            exit(1);
         }
         int captureAttractFPS = 0;
         if (!try_parse_int(szArglist[i + 2], captureAttractFPS) || captureAttractFPS <= 0)
         {
            OnCommandLineError("Command Line Error"s, "Invalid framerate"s);
            exit(1);
         }
         const std::filesystem::path tableFileName = GetPathFromArg(szArglist[i + 3]);
         if (!FileExists(tableFileName))
         {
            OnCommandLineError("Command Line Error"s, "Table file '" + tableFileName.string() + "' was not found");
            exit(1);
         }
         if (!IsTableFile(tableFileName))
         {
            OnCommandLineError("Command Line Error"s, "'" + tableFileName.string() + "' is not a Visual Pinball table (.vpx)");
            exit(1);
         }
         bool captureAttractLoop = true;
         if (i + 4 < nArgs && StrCompareNoCase("noloop"s, szArglist[i + 4]))
         {
            captureAttractLoop = false;
            i += 4;
         }
         else
         {
            captureAttractLoop = true;
            i += 3;
         }
         commands.push_back(std::make_unique<CaptureAttractCommand>(tableFileName, captureAttract, captureAttractFPS, captureAttractLoop));
         break;
      }

      case OPTION_TOURNAMENT:
      {
         if (i + 2 >= nArgs)
         {
            OnCommandLineError("Command Line Error"s, "Option '"s + szArglist[i] + "' must be followed by a valid table file path and a valid tournament file path");
            exit(1);
         }
         const std::filesystem::path tableFileName = GetPathFromArg(szArglist[i + 1]);
         i++;
         if (!FileExists(tableFileName))
         {
            OnCommandLineError("Command Line Error"s, "Table file '" + tableFileName.string() + "' was not found");
            exit(1);
         }
         if (!IsTableFile(tableFileName))
         {
            OnCommandLineError("Command Line Error"s, "'" + tableFileName.string() + "' is not a Visual Pinball table (.vpx)");
            exit(1);
         }
         const std::filesystem::path tournamentFileName = GetPathFromArg(szArglist[i + 2]);
         i++;
         if (!FileExists(tournamentFileName))
         {
            OnCommandLineError("Command Line Error"s, "Tournament file '" + tournamentFileName.string() + "' was not found");
            exit(1);
         }
         commands.push_back(std::make_unique<ValidateTournamentCommand>(tableFileName, tournamentFileName));
         break;
      }

      case OPTION_TABLE_INI:
         if (i + 1 >= nArgs)
         {
            OnCommandLineError("Command Line Error"s, "Option '"s + szArglist[i] + "' must be followed by a valid setting file path");
            exit(1);
         }
         tableIniFileName = GetPathFromArg(szArglist[i + 1]);
         i++;
         break;

      default:
         assert(false);
         break;
      }
   }

   #ifndef __STANDALONE__
   if (defaultToWin32Editor && commands.empty())
      commands.push_back(std::make_unique<Win32EditCommand>());
   #endif

   if (commands.size() > 1)
   {
      OnCommandLineError("Command Line Error"s, "Multiple command options specified. Please specify only one");
      exit(1);
   }
   // With no actionable command (and no editor was opened above), show usage instead of starting up
   // and immediately exiting with no output.
   if (commands.empty())
      commands.push_back(std::make_unique<ShowInfoAndExitCommand>("Visual Pinball Usage"s, GetCommandLineHelp(), 0));
   m_command = std::move(commands[0]);

   #ifndef __STANDALONE__
   if (win32EditorMinimized)
   {
      if (auto win32EditCmd = dynamic_cast<Win32EditCommand*>(m_command.get()); win32EditCmd)
      {
         win32EditCmd->m_minimized = true;
         win32EditCmd->m_disablePauseMenu = true;
      }
      else
      {
         OnCommandLineError("Command Line Error"s, "Option '"s + options[OPTION_MINIMIZED].arg + "' must be used in conjunction with a command that uses the Win32 editor");
         exit(1);
      }
   }

   if (win32EditorExtMinimized)
   {
      if (auto win32EditCmd = dynamic_cast<Win32EditCommand*>(m_command.get()); win32EditCmd)
      {
         win32EditCmd->m_minimized = true;
         win32EditCmd->m_disablePauseMenu = false;
      }
      else
      {
         OnCommandLineError("Command Line Error"s, "Option '"s + options[OPTION_EXTMINIMIZED].arg + "' must be used in conjunction with a command that uses the Win32 editor");
         exit(1);
      }
   }
   #endif

   if (!tableIniFileName.empty())
   {
      auto tableCmd = dynamic_cast<TableBasedCommand*>(m_command.get());
      if (tableCmd == nullptr)
      {
         OnCommandLineError("Command Line Error"s, "Option '"s + options[OPTION_TABLE_INI].arg + "' must be used in conjunction with a command that specifies a table filename");
         exit(1);
      }
      if (!FileExists(tableIniFileName))
      {
         OnCommandLineError("Command Line Error"s, "Table ini file '" + tableIniFileName.string() + "' was not found");
         exit(1);
      }
      tableCmd->SetTableIniFileName(tableIniFileName);
   }
}

const char** CommandLineProcessor::CommandLineToArgvA(const char* const CmdLine, int* const _argc)
{
   const size_t len = strlen(CmdLine);
   size_t i = ((len + 2) / 2) * sizeof(void*) + sizeof(void*);

   const char** argv = (const char**)malloc(i + (len + 2) * sizeof(char));
   char* _argv = (char*)(((unsigned char*)argv) + i);

   int argc = 0;
   argv[argc] = _argv;
   bool in_QM = false;
   bool in_TEXT = false;
   bool in_SPACE = true;
   i = 0;
   size_t j = 0;

   char a;
   while ((a = CmdLine[i])) {
      if (in_QM) {
         if (a == '\"') {
            in_QM = false;
         }
         else {
            _argv[j] = a;
            j++;
         }
      }
      else {
         switch (a) {
         case '\"':
            in_QM = true;
            in_TEXT = true;
            if (in_SPACE) {
               argv[argc] = _argv + j;
               argc++;
            }
            in_SPACE = false;
            break;
         case ' ':
         case '\t':
         case '\n':
         case '\r':
            if (in_TEXT) {
               _argv[j] = '\0';
               j++;
            }
            in_TEXT = false;
            in_SPACE = true;
            break;
         default:
            in_TEXT = true;
            if (in_SPACE) {
               argv[argc] = _argv + j;
               argc++;
            }
            _argv[j] = a;
            j++;
            in_SPACE = false;
            break;
         }
      }
      i++;
   }
   _argv[j] = '\0';
   argv[argc] = nullptr;

   (*_argc) = argc;
   return argv;
}
