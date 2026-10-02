// license:GPLv3+

#include "core/stdafx.h"
#include "TablePickerPage.h"

#ifdef __STANDALONE__

#include "core/VPApp.h"
#include "lib/src/TableLibrary.h"
#ifdef VPX_TABLE_WEBSERVER
#include "lib/src/WebServer.h"
#endif
#include "renderer/Renderer.h"
#include "renderer/Texture.h"
#include "renderer/VRDevice.h"
#include "ui/live/LiveUI.h"

#include "fonts/IconsForkAwesome.h"
#include "imgui/imgui_stdlib.h"

using LibraryTable = VPinballLib::Table; // 'Table' alone is ambiguous with VPX parts
using VPinballLib::TableLibrary;

namespace VPX::InGameUI
{

// State of the picker, kept while the application runs (the page is recreated each time it is opened)
enum class PickerTab { All, Recent, NewlyAdded, MostPlayed, Favorites, Menu }; // Menu: the options of the library and the application, instead of a list
static constexpr const char* TAB_NAMES[] = { "All", "Recent", "Newly added", "Most played", "Favorites", "MENU" };
static PickerTab s_tab = PickerTab::All;
static string s_search;
static int s_page = 0;
static char s_filterInitial = 0; // 'All' tab: 0 for all tables, '#' for names which do not start with a letter

// Custom rendered items are identified by their label, which is not displayed
static constexpr const char* TABS_ITEM = "##tabs";
static constexpr const char* SEARCH_ITEM = "##search";
static constexpr const char* PAGER_TOP_ITEM = "##pager-top";
static constexpr const char* PAGER_BOTTOM_ITEM = "##pager-bottom";
static constexpr const char* FILTER_LABEL = "Show: ";

static constexpr int TABLES_PER_PAGE = 12; // Rows of 2, 3, 4 or 6 tiles, depending on the width of the menu

static char GetInitial(const LibraryTable& table)
{
   const unsigned char c = table.name.empty() ? '#' : static_cast<unsigned char>(table.name[0]);
   return std::isalpha(c) ? static_cast<char>(std::toupper(c)) : '#';
}

// Date, and optionally time, in the local time zone
static string FormatTime(int64_t secondsSinceEpoch, bool withTime)
{
   const time_t time = static_cast<time_t>(secondsSinceEpoch);
   std::tm localTime {};
#ifdef _WIN32
   localtime_s(&localTime, &time);
#else
   localtime_r(&time, &localTime);
#endif
   char buffer[64];
   return strftime(buffer, sizeof(buffer), withTime ? "%d %b %Y at %H:%M" : "%d %b %Y", &localTime) > 0 ? string(buffer) : string();
}

// What is displayed when a table is hovered or selected: when it was added, when it was last played and how many times.
// The information area of the pages holds 3 lines: 'compact' uses 2, to leave one for something else.
static string GetTableStatsInfo(const LibraryTable& table, bool compact = false)
{
   const string added = "Added on " + FormatTime(table.createdAt, false);
   if (table.playCount == 0 || table.lastPlayedAt == 0)
      return added + (compact ? ", never played" : "\nNever played");
   const string lastPlayed = "Last played on " + FormatTime(table.lastPlayedAt, true);
   const string played = table.playCount == 1 ? "layed once"s : std::format("layed {} times", table.playCount);
   return compact ? added + ", p" + played + '\n' + lastPlayed : added + '\n' + lastPlayed + "\nP" + played;
}

static bool IsRunningTable(const Player* player, const LibraryTable& table)
{
   // The lobby may be loaded from a table, to use its VR room: that table is not running then
   if (player->m_isLobby)
      return false;
   std::error_code ec;
   return std::filesystem::equivalent(player->m_ptable->m_filename, g_app->GetTableLibrary().GetFullPath(table), ec);
}


TablePickerPage::TablePickerPage()
   : InGameUIPage("Tables"s, "Select a table to play it.\nTables are added by copying their folder to the tables folder."s, SaveMode::None)
{
}

// Each decode keeps a core busy: a few at a time leave the others to the game and rendering threads
static constexpr int MAX_THUMBNAIL_LOADS = 2;

static bool IsReady(const std::future<std::shared_ptr<BaseTexture>>& future)
{
   return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

TablePickerPage::~TablePickerPage()
{
   // Loads still running are waited for by their futures (a few milliseconds)
   for (auto& [uuid, thumbnail] : m_thumbnails)
      if (thumbnail.texture)
         m_player->m_renderer->m_renderDevice->m_texMan.UnloadTexture(thumbnail.texture.get());
}

ImTextureID TablePickerPage::GetThumbnail(const LibraryTable& table)
{
   if (table.image.empty())
      return nullptr;
   Thumbnail& thumbnail = m_thumbnails[table.uuid];
   // First use, or the image was replaced (the library updates the modification date): decode it on a worker thread
   if (thumbnail.modifiedAt != table.modifiedAt && !thumbnail.loading.valid())
   {
      int loads = static_cast<int>(m_abandonedLoads.size());
      for (const auto& [uuid, other] : m_thumbnails)
         loads += other.loading.valid() ? 1 : 0;
      if (loads < MAX_THUMBNAIL_LOADS)
      {
         thumbnail.loadingModifiedAt = table.modifiedAt;
         thumbnail.loading = std::async(std::launch::async, [path = g_app->GetTableLibrary().GetImagePath(table)]() { return BaseTexture::CreateFromFile(path, 512); });
      }
   }
   // Until then, the previous image if it was replaced
   return thumbnail.texture ? m_player->m_renderer->m_renderDevice->m_texMan.LoadTexture(thumbnail.texture.get(), false) : nullptr;
}

bool TablePickerPage::IsThumbnailLoading(const LibraryTable& table) const
{
   if (table.image.empty())
      return false;
   const auto it = m_thumbnails.find(table.uuid);
   return it == m_thumbnails.end() || (it->second.texture == nullptr && it->second.modifiedAt != table.modifiedAt);
}

// Takes the decoded images, one per frame at most as it is uploaded to the GPU when the tile is drawn
void TablePickerPage::UpdateThumbnailLoads()
{
   std::erase_if(m_abandonedLoads, [](const auto& loading) { return IsReady(loading); });
   for (auto& [uuid, thumbnail] : m_thumbnails)
   {
      if (m_thumbnailLoadedThisFrame)
         break;
      if (thumbnail.loading.valid() && IsReady(thumbnail.loading))
      {
         m_thumbnailLoadedThisFrame = true;
         if (thumbnail.texture)
            m_player->m_renderer->m_renderDevice->m_texMan.UnloadTexture(thumbnail.texture.get());
         thumbnail.texture = thumbnail.loading.get(); // Null if the image could not be decoded: not tried again until it changes
         thumbnail.modifiedAt = thumbnail.loadingModifiedAt;
      }
   }
}

void TablePickerPage::Open(bool isBackwardAnimation)
{
   InGameUIPage::Open(isBackwardAnimation);
   if (!isBackwardAnimation)
      g_app->GetTableLibrary().RescanAsync();
}

void TablePickerPage::Render(float elapsedS)
{
   m_thumbnailLoadedThisFrame = false;
   UpdateThumbnailLoads();

   // Scans run on a worker thread (they may import large files) and are also triggered outside of this page
   const TableLibrary& library = g_app->GetTableLibrary();
   if (m_revision != library.GetRevision() || m_scanning != library.IsScanning())
      RequestRebuild();
#ifdef VPX_TABLE_WEBSERVER
   // The pairing code is renewed after failed attempts
   if (m_webServerUrl != g_app->GetWebServer().GetUrl() || m_pairingCode != g_app->GetWebServer().GetPairingCode())
      RequestRebuild();
#endif
   InGameUIPage::Render(elapsedS);

   // Get back on an item which moved as the page was rebuilt (the rebuild happens at the beginning of the render following the request)
   if (!m_reselectItem.empty() && m_reselectDelay-- <= 0)
   {
      for (int i = 0; i < 1000 && GetItem(m_reselectItem) != nullptr && GetSelectedItem() != GetItem(m_reselectItem); i++)
         SelectNextItem();
      m_reselectItem.clear();
   }
}

void TablePickerPage::SetTab(int tab)
{
   const int nTabs = static_cast<int>(std::size(TAB_NAMES));
   s_tab = static_cast<PickerTab>((tab + nTabs) % nTabs);
   s_page = 0;
   RequestRebuild();
}

void TablePickerPage::SetPage(int page, const string& pagerItem)
{
   s_page = clamp(page, 0, max(m_pageCount - 1, 0));
   m_reselectItem = pagerItem; // The number of items above the bottom pager changes with the page
   m_reselectDelay = 1;
   RequestRebuild();
}

void TablePickerPage::SetSearch(const string& search)
{
   s_search = search;
   s_page = 0;
   RequestRebuild();
}

void TablePickerPage::AdjustItem(float direction, bool isInitialPress)
{
   // Actions of this page must not repeat while the button is held
   if (!isInitialPress)
      return;

   // Items which use the direction, or are custom rendered (the base page does not know what to do with these)
   if (const InGameUIItem* item = GetSelectedItem(); item)
   {
      if (item->m_label == TABS_ITEM)
      {
         SetTab(static_cast<int>(s_tab) + (direction < 0.f ? -1 : 1));
         return;
      }
      if (item->m_label == PAGER_TOP_ITEM || item->m_label == PAGER_BOTTOM_ITEM)
      {
         SetPage((s_page + (direction < 0.f ? m_pageCount - 1 : 1)) % max(m_pageCount, 1), item->m_label);
         return;
      }
      if (item->m_label == SEARCH_ITEM)
      {
         // Without a keyboard, the search text is entered with the navigation buttons
         m_player->m_liveUI->m_inGameUI.AddPage("tables/search"s,
            []() { return std::make_unique<TextEntryPage>("Search tables"s, s_search, [](const string& search) { s_search = search; s_page = 0; }, true); });
         m_player->m_liveUI->m_inGameUI.Navigate("tables/search"s);
         return;
      }
      if (item->m_label.starts_with(FILTER_LABEL))
      {
         // Previous/next initial, going through 'All'
         string initials(1, '\0');
         for (const LibraryTable& table : g_app->GetTableLibrary().GetTables())
            if (initials.find(GetInitial(table)) == string::npos)
               initials += GetInitial(table);
         std::sort(initials.begin() + 1, initials.end());
         const size_t pos = initials.find(s_filterInitial);
         const size_t count = initials.size();
         s_filterInitial = initials[pos == string::npos ? 0 : (pos + (direction < 0.f ? count - 1 : 1)) % count];
         s_page = 0;
         RequestRebuild();
         return;
      }
   }

   InGameUIPage::AdjustItem(direction, isInitialPress);
}

void TablePickerPage::RenderTabs()
{
   // One button per tab, large enough to be hit easily with a mouse or a VR pointer, wrapping on a second line if the menu is narrow (VR).
   // With buttons, left/right change the tab.
   const ImGuiStyle& style = ImGui::GetStyle();
   const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
   const float textHeight = ImGui::GetTextLineHeight();
   const ImVec2 padding(0.9f * textHeight, 0.4f * textHeight);
   const float rounding = 0.3f * textHeight;
   ImDrawList* const drawList = ImGui::GetWindowDrawList();
   // The row is not highlighted as a whole (only the hovered button is): with buttons, the selected tab turns green when the row has the focus
   const InGameUIItem* const selectedItem = GetSelectedItem();
   const bool hasFocus = m_player->m_liveUI->m_inGameUI.IsFlipperNav() && selectedItem != nullptr && selectedItem->m_label == TABS_ITEM;
   ImGui::PushID(TABS_ITEM);
   for (int i = 0; i < static_cast<int>(std::size(TAB_NAMES)); i++)
   {
      const ImVec2 textSize = ImGui::CalcTextSize(TAB_NAMES[i]);
      const ImVec2 size = textSize + padding * 2.f;
      if (i > 0)
      {
         ImGui::SameLine(0.f, style.ItemSpacing.x);
         if (ImGui::GetCursorScreenPos().x + size.x > right)
            ImGui::NewLine();
      }
      // MENU is not a list: it stands apart, at the right end of the line, clear of the scroll bar
      if (static_cast<PickerTab>(i) == PickerTab::Menu)
         ImGui::SetCursorScreenPos(ImVec2(right - size.x - style.ScrollbarSize - style.ItemSpacing.x, ImGui::GetCursorScreenPos().y));
      const ImVec2 pos = ImGui::GetCursorScreenPos();
      if (ImGui::InvisibleButton(TAB_NAMES[i], size))
         SetTab(i);
      const bool isActive = static_cast<int>(s_tab) == i;
      const bool isHovered = ImGui::IsItemHovered() || (hasFocus && isActive);
      const ImU32 background = isHovered ? IM_COL32(0, 255, 0, 50) : isActive ? IM_COL32(255, 255, 255, 56) : IM_COL32(255, 255, 255, 16);
      const ImU32 color = isHovered ? IM_COL32(0, 255, 0, 255) : isActive ? IM_COL32(255, 255, 255, 255) : IM_COL32(170, 170, 170, 255);
      drawList->AddRectFilled(pos, pos + size, background, rounding);
      if (isActive)
         drawList->AddRect(pos, pos + size, color, rounding, ImDrawFlags_None, 2.f);
      drawList->AddText(pos + padding, color, TAB_NAMES[i]);
   }
   ImGui::PopID();
}

void TablePickerPage::RenderSearch()
{
   // A text box for keyboards, filtering while typing. With buttons, activating the item opens the text entry page instead.
   const ImGuiStyle& style = ImGui::GetStyle();
   ImGui::PushID(SEARCH_ITEM);
   ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.f)); // Same height as the other items
   ImGui::TextUnformatted(ICON_FK_SEARCH);
   ImGui::SameLine();
   const float clearWidth = s_search.empty() ? 0.f : ImGui::CalcTextSize(ICON_FK_TIMES).x + 2.f * style.FramePadding.x + style.ItemSpacing.x;
   ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - clearWidth - 2.f * style.ItemSpacing.x);
   string search = s_search;
   if (ImGui::InputTextWithHint("##text", "Search: a few letters of the name", &search) && search != s_search)
      SetSearch(search);
   // In VR there is no keyboard: activating the field opens a virtual one under it
   if (ImGui::IsItemActivated() && m_player->m_vrDevice)
      m_virtualKeyboard = true;
   if (!s_search.empty())
   {
      ImGui::SameLine();
      if (ImGui::Button(ICON_FK_TIMES))
         SetSearch(string());
   }
   ImGui::PopStyleVar();
   if (m_virtualKeyboard)
      RenderVirtualKeyboard();
   ImGui::PopID();
}

void TablePickerPage::RenderVirtualKeyboard()
{
   // Keys add to the search (not to the text field, which loses the focus when a key is clicked), which filters the list at once
   static constexpr const char* rows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL'", "ZXCVBNM-&." };
   const ImGuiStyle& style = ImGui::GetStyle();
   const float spacing = style.ItemSpacing.x;
   const float keyWidth = (ImGui::GetContentRegionAvail().x - 9.f * spacing) / 10.f;
   const float keyHeight = 1.5f * ImGui::GetFrameHeight();
   ImGui::PushID("VirtualKeyboard");
   ImGui::Dummy(ImVec2(0.f, 0.5f * keyHeight));
   for (const char* row : rows)
   {
      for (const char* key = row; *key; key++)
      {
         if (key != row)
            ImGui::SameLine(0.f, spacing);
         const char label[2] = { *key, '\0' };
         ImGui::PushID(*key);
         if (ImGui::Button(label, ImVec2(keyWidth, keyHeight)) && s_search.size() < 64)
            SetSearch(s_search + static_cast<char>(std::tolower(static_cast<unsigned char>(*key))));
         ImGui::PopID();
      }
   }
   // Last row: space over 4 keys, then delete, clear and done over 2 keys each
   const float wideKey = 2.f * keyWidth + spacing;
   if (ImGui::Button("Space", ImVec2(2.f * wideKey + spacing, keyHeight)) && s_search.size() < 64)
      SetSearch(s_search + ' ');
   ImGui::SameLine(0.f, spacing);
   if (ImGui::Button(ICON_FK_ARROW_LEFT " Delete", ImVec2(wideKey, keyHeight)) && !s_search.empty())
   {
      // A whole UTF-8 character
      string search = s_search;
      while (!search.empty() && (static_cast<unsigned char>(search.back()) & 0xC0) == 0x80)
         search.pop_back();
      if (!search.empty())
         search.pop_back();
      SetSearch(search);
   }
   ImGui::SameLine(0.f, spacing);
   if (ImGui::Button("Clear", ImVec2(wideKey, keyHeight)))
      SetSearch(string());
   ImGui::SameLine(0.f, spacing);
   if (ImGui::Button(ICON_FK_CHECK " Done", ImVec2(wideKey, keyHeight)))
      m_virtualKeyboard = false;
   ImGui::PopID();
}

void TablePickerPage::RenderPager(const char* item)
{
   ImGui::PushID(item);
   ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.f));
   if (ImGui::Button(ICON_FK_ANGLE_LEFT))
      SetPage((s_page + m_pageCount - 1) % m_pageCount, item);
   ImGui::SameLine();
   ImGui::Text("Page %d of %d", s_page + 1, m_pageCount);
   ImGui::SameLine();
   if (ImGui::Button(ICON_FK_ANGLE_RIGHT))
      SetPage((s_page + 1) % m_pageCount, item);
   ImGui::PopStyleVar();
   ImGui::PopID();
}

void TablePickerPage::BuildPage()
{
   TableLibrary& library = g_app->GetTableLibrary();
   m_revision = library.GetRevision();
   m_scanning = library.IsScanning();
   const bool gridView = g_app->m_settings.GetStandalone_TablePickerGridView();
   const bool sortAscending = g_app->m_settings.GetStandalone_TablePickerSortAscending();
   vector<LibraryTable> tables = library.GetTables(s_tab != PickerTab::All || sortAscending);
   const size_t tableCount = tables.size();

   // Names are displayed, and identify the menu items: tables sharing a name are told apart by their folder
   ankerl::unordered_dense::map<string, int> nameCounts;
   for (const LibraryTable& table : tables)
      nameCounts[table.name]++;
   const std::filesystem::path runningTable = m_player->m_isLobby ? std::filesystem::path() : m_player->m_ptable->m_filename.lexically_normal(); // See IsRunningTable

   // The tab selects and orders the tables
   constexpr size_t filterMinTables = 9; // With a few tables, going through the list is quicker than selecting a letter
   const bool hasLetterFilter = s_tab == PickerTab::All && tableCount >= filterMinTables;
   if (!hasLetterFilter || std::ranges::none_of(tables, [](const LibraryTable& table) { return GetInitial(table) == s_filterInitial; }))
      s_filterInitial = 0;
   const char* emptyMessage = "";
   switch (s_tab)
   {
   case PickerTab::All:
      std::erase_if(tables, [](const LibraryTable& table) { return s_filterInitial != 0 && GetInitial(table) != s_filterInitial; });
      emptyMessage = "No table yet: copy table folders to the tables folder, or add them from a browser (see MENU)";
      break;
   case PickerTab::Recent:
      std::erase_if(tables, [](const LibraryTable& table) { return table.lastPlayedAt == 0; });
      std::ranges::stable_sort(tables, [](const LibraryTable& a, const LibraryTable& b) { return a.lastPlayedAt > b.lastPlayedAt; });
      emptyMessage = "No table was played yet";
      break;
   case PickerTab::NewlyAdded:
      std::ranges::stable_sort(tables, [](const LibraryTable& a, const LibraryTable& b) { return a.createdAt > b.createdAt; });
      emptyMessage = "No table yet";
      break;
   case PickerTab::MostPlayed:
      std::erase_if(tables, [](const LibraryTable& table) { return table.playCount == 0; });
      std::ranges::stable_sort(tables, [](const LibraryTable& a, const LibraryTable& b) { return a.playCount != b.playCount ? a.playCount > b.playCount : a.lastPlayedAt > b.lastPlayedAt; });
      emptyMessage = "No table was played yet";
      break;
   case PickerTab::Favorites:
      std::erase_if(tables, [](const LibraryTable& table) { return !table.favorite; });
      emptyMessage = "No favorite yet: use the star of a table, or 'Add to favorites' in its page";
      break;
   case PickerTab::Menu:
      break;
   }

   // Fuzzy search on the name. In the 'All' tab the best matches come first, the other tabs keep their order.
   if (!s_search.empty())
   {
      vector<std::pair<int, LibraryTable>> matches;
      for (LibraryTable& table : tables)
         if (const std::optional<int> score = TableLibrary::FuzzyScore(s_search, table.name); score)
            matches.emplace_back(*score, std::move(table));
      if (s_tab == PickerTab::All)
         std::ranges::stable_sort(matches, [](const auto& a, const auto& b) { return a.first > b.first; });
      tables.clear();
      for (auto& [score, table] : matches)
         tables.push_back(std::move(table));
      emptyMessage = "No table matches the search";
   }

   // Pages: a short list is quicker to go through with buttons, and only the images of the page are kept in memory
   const size_t matchCount = tables.size();
   m_pageCount = max(1, static_cast<int>((matchCount + TABLES_PER_PAGE - 1) / TABLES_PER_PAGE));
   s_page = clamp(s_page, 0, m_pageCount - 1);
   if (matchCount > TABLES_PER_PAGE)
   {
      const size_t first = static_cast<size_t>(s_page) * TABLES_PER_PAGE;
      tables = vector<LibraryTable>(tables.begin() + static_cast<std::ptrdiff_t>(first), tables.begin() + static_cast<std::ptrdiff_t>(min(first + TABLES_PER_PAGE, matchCount)));
   }

   AddItem(std::make_unique<InGameUIItem>(TABS_ITEM, "Left/Right: previous/next list"s, [this](int, const InGameUIItem*) { RenderTabs(); })).m_customHighlight = true;
   if (s_tab == PickerTab::Menu)
   {
      KeepThumbnails({});
      BuildMenuTab();
      return;
   }
   AddItem(std::make_unique<InGameUIItem>(SEARCH_ITEM, "Type a few letters of the name of a table. Without a keyboard, Left/Right open a text entry page."s, [this](int, const InGameUIItem*) { RenderSearch(); }));

   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header,
      m_scanning                   ? "Scanning tables folder..."s
         : matchCount != tableCount ? std::format("{} of {} tables", matchCount, tableCount)
                                    : std::format("{} table{}", tableCount, tableCount == 1 ? "" : "s")));

   if (hasLetterFilter)
      AddItem(std::make_unique<InGameUIItem>(FILTER_LABEL + (s_filterInitial == 0 ? "All tables"s : s_filterInitial == '#' ? "0-9 and others"s : string(1, s_filterInitial)),
         "Left/Right: only show the tables starting with the previous/next letter"s, []() { /* Handled by AdjustItem as it depends on the direction */ }));

   if (m_pageCount > 1)
      AddItem(std::make_unique<InGameUIItem>(PAGER_TOP_ITEM, "Left/Right: previous/next page"s, [this](int, const InGameUIItem*) { RenderPager(PAGER_TOP_ITEM); }));

   if (tables.empty() && !m_scanning)
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, emptyMessage));

   ankerl::unordered_dense::set<string> displayedTables;
   for (const LibraryTable& table : tables)
   {
      string label = table.name;
      if (nameCounts[table.name] > 1)
         label += " [" + table.path + ']';
      if (runningTable == library.GetFullPath(table).lexically_normal())
         label += " (playing)";
      if (!gridView && table.favorite)
         label = ICON_FK_STAR " "s + label;
      const string path = "tables/" + table.uuid;
      const string uuid = table.uuid;
      displayedTables.insert(uuid);
      m_player->m_liveUI->m_inGameUI.AddPage(path, [uuid]() { return std::make_unique<TableActionsPage>(uuid); });
      InGameUIItem& item = AddItem(std::make_unique<InGameUIItem>(label, GetTableStatsInfo(table), path));
      if (gridView)
      {
         item.m_tileImage = [this, table]() { return GetThumbnail(table); };
         item.m_tileLoading = [this, table]() { return IsThumbnailLoading(table); };
         item.m_tileToggleIconOn = ICON_FK_STAR;
         item.m_tileToggleIconOff = ICON_FK_STAR_O;
         item.m_tileToggleState = [favorite = table.favorite]() { return favorite; };
         item.m_tileToggleAction = [uuid, favorite = table.favorite]() { g_app->GetTableLibrary().SetFavorite(uuid, !favorite); }; // The page is rebuilt as the library changes
      }
   }

   if (m_pageCount > 1)
      AddItem(std::make_unique<InGameUIItem>(PAGER_BOTTOM_ITEM, "Left/Right: previous/next page"s, [this](int, const InGameUIItem*) { RenderPager(PAGER_BOTTOM_ITEM); }));

   KeepThumbnails(displayedTables);
}

// Only keep the images of the displayed page
void TablePickerPage::KeepThumbnails(const ankerl::unordered_dense::set<string>& displayedTables)
{
   for (auto it = m_thumbnails.begin(); it != m_thumbnails.end();)
   {
      if (displayedTables.contains(it->first))
         ++it;
      else
      {
         if (it->second.texture)
            m_player->m_renderer->m_renderDevice->m_texMan.UnloadTexture(it->second.texture.get());
         if (it->second.loading.valid())
            m_abandonedLoads.push_back(std::move(it->second.loading));
         it = m_thumbnails.erase(it);
      }
   }
}

// The MENU tab: the options of the library and of the application, which are not needed while choosing a table
void TablePickerPage::BuildMenuTab()
{
   TableLibrary& library = g_app->GetTableLibrary();
   const bool gridView = g_app->m_settings.GetStandalone_TablePickerGridView();
   const bool sortAscending = g_app->m_settings.GetStandalone_TablePickerSortAscending();

   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Library"s));

   AddItem(std::make_unique<InGameUIItem>(gridView ? "View: Grid"s : "View: List"s, "Switch between a grid of table images and a list of names"s,
      [this, gridView]()
      {
         g_app->m_settings.SetStandalone_TablePickerGridView(!gridView, false);
         RequestRebuild();
      }));

   AddItem(std::make_unique<InGameUIItem>(VPApp::GetTableImageFocusLabel(), "What the images of the tables captured in VR show: their backglass, their playfield seen from above, or their whole cabinet"s,
      [this]()
      {
         VPApp::NextTableImageFocus();
         RequestRebuild();
      }));

   // Distance of the menu window in VR, changed by steps of 10cm, the window being placed again at once
   if (m_player->m_vrDevice)
      AddItem(std::make_unique<InGameUIItem>("Menu distance"s, "How far from the player the menu window is placed in VR"s,
         [this](int index, const InGameUIItem*)
         {
            const float distance = g_app->m_settings.GetStandalone_VRMenuDistance();
            const auto setDistance = [this](float value)
            {
               g_app->m_settings.SetStandalone_VRMenuDistance(clamp(roundf(value * 10.f) / 10.f, 0.3f, 3.f), false);
               #ifdef ENABLE_XR
               m_player->m_vrDevice->RecenterUIPanel();
               #endif
            };
            ImGui::AlignTextToFramePadding();
            ImGui::Text("Menu distance: %.1f m", distance);
            const float buttonWidth = ImGui::GetFrameHeight() * 1.6f;
            ImGui::SameLine(0.f, ImGui::GetStyle().ItemSpacing.x * 3.f);
            ImGui::BeginDisabled(distance <= 0.3f + 1e-3f);
            if (ImGui::Button(std::format("{}##MenuCloser{}", ICON_FK_MINUS, index).c_str(), ImVec2(buttonWidth, 0.f)))
               setDistance(distance - 0.1f);
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(distance >= 3.f - 1e-3f);
            if (ImGui::Button(std::format("{}##MenuFarther{}", ICON_FK_PLUS, index).c_str(), ImVec2(buttonWidth, 0.f)))
               setDistance(distance + 0.1f);
            ImGui::EndDisabled();
         }));

   AddItem(std::make_unique<InGameUIItem>(sortAscending ? "Sort: A to Z"s : "Sort: Z to A"s, "Change the sort order of the 'All' list"s,
      [this, sortAscending]()
      {
         g_app->m_settings.SetStandalone_TablePickerSortAscending(!sortAscending, false);
         RequestRebuild();
      }));

   AddItem(std::make_unique<InGameUIItem>("Rescan tables folder"s, "Look for tables added to, or removed from, the tables folder"s, []() { g_app->GetTableLibrary().RescanAsync(); }));

   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "Tables folder: " + library.GetTablesPath().string()));
   if (g_app->IsSharedPinMAMEFolderUsed())
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "ROMs (zipped): 'pinmame/roms' in the tables folder, or in a table's folder"s));

   // The lobby may use the room of a table, chosen from the menu of that table while it is played
   if (g_app->m_launcherMode)
   {
      if (const std::optional<VPApp::LobbyRoom> room = g_app->GetLobbyRoom(); room)
      {
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "Lobby room: the room of " + room->tableName));
         AddItem(std::make_unique<InGameUIItem>("Use the default lobby room"s, "Show the table picker in the default room again"s,
            [this]()
            {
               g_app->ResetLobbyRoom();
               if (m_player->m_isLobby)
               {
                  // Load the lobby again, with the default room
                  g_app->m_reloadLobby = true;
                  m_player->SetCloseState(Player::CS_CLOSE_APP);
               }
               else
                  RequestRebuild();
            }));
      }
      else
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "Lobby room: default (the room of a table can be used, from the menu of that table)"s));
   }

   // In launcher mode, this page is the front door of the application: give access to the rest of the menu from it
   if (g_app->m_launcherMode)
      AddItem(std::make_unique<InGameUIItem>("Settings"s, "Controls, VR, graphics, sound and the other settings"s, "homepage"s));

   if (g_app->m_launcherMode)
      AddItem(std::make_unique<InGameUIItem>("Quit Visual Pinball"s, ""s,
         [this]()
         {
            g_app->m_launcherMode = false;
            m_player->SetCloseState(Player::CS_CLOSE_CAPTURE_SCREENSHOT);
         }));

#ifdef VPX_TABLE_WEBSERVER
   // The web server gives access to the tables folder to the local network, so it only runs on request and asks for a pairing code
   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Add tables from a browser"s));
   WebServer& webServer = g_app->GetWebServer();
   m_webServerUrl = webServer.GetUrl();
   m_pairingCode = webServer.GetPairingCode();
   AddItem(std::make_unique<InGameUIItem>(webServer.IsRunning() ? "Wi-Fi upload: On"s : "Wi-Fi upload: Off"s,
      "Manage tables from the browser of a phone or computer connected to the same network. Stopped when a table is played."s,
      [this]()
      {
         WebServer& webServer = g_app->GetWebServer();
         if (webServer.IsRunning())
            webServer.Stop();
         else
            webServer.Start();
         RequestRebuild();
      }));
   if (webServer.IsRunning())
   {
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, m_webServerUrl.empty() ? "No network address found"s : "Open " + m_webServerUrl));
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "Pairing code: " + m_pairingCode));
   }
#endif
}


static constexpr const char* CHARACTER_LABEL = "Character: ";
static constexpr std::string_view TEXT_ENTRY_CHARACTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz0123456789-'&!().,";

TextEntryPage::TextEntryPage(const string& title, const string& text, const std::function<void(const string&)>& onSave, bool allowEmpty)
   : InGameUIPage(title, "Left/Right select a character, which is then added to the text"s, SaveMode::None)
   , m_text(text)
   , m_onSave(onSave)
   , m_allowEmpty(allowEmpty)
{
}

void TextEntryPage::AdjustItem(float direction, bool isInitialPress)
{
   // The character selector repeats while the button is held, as it is a long list, the other actions do not
   if (const InGameUIItem* item = GetSelectedItem(); item && item->m_label.starts_with(CHARACTER_LABEL))
   {
      static uint32_t lastChangeMs = 0;
      if (!isInitialPress && msec() - lastChangeMs < 150)
         return;
      lastChangeMs = msec();
      const size_t count = TEXT_ENTRY_CHARACTERS.size();
      m_charIndex = (m_charIndex + (direction < 0.f ? count - 1 : 1)) % count;
      RequestRebuild();
      return;
   }
   if (isInitialPress)
      InGameUIPage::AdjustItem(direction, isInitialPress);
}

void TextEntryPage::BuildPage()
{
   const char c = TEXT_ENTRY_CHARACTERS[m_charIndex];
   const string displayedChar = c == ' ' ? "space"s : string(1, c);
   AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, m_text + '_'));
   AddItem(std::make_unique<InGameUIItem>(CHARACTER_LABEL + "< "s + displayedChar + " >", "Left/Right: previous/next character"s, []() { /* Handled by AdjustItem as it depends on the direction */ }));
   AddItem(std::make_unique<InGameUIItem>("Add '" + displayedChar + '\'', ""s,
      [this, c]()
      {
         if (m_text.size() < 64)
            m_text += c;
         // Words usually are capitalized: propose lowercase after a letter, uppercase after a space
         if (c == ' ')
            m_charIndex = TEXT_ENTRY_CHARACTERS.find('A');
         else if (std::isupper(static_cast<unsigned char>(c)))
            m_charIndex = TEXT_ENTRY_CHARACTERS.find(static_cast<char>(std::tolower(c)));
         RequestRebuild();
      }));
   if (!m_text.empty())
      AddItem(std::make_unique<InGameUIItem>("Delete last character"s, ""s,
         [this]()
         {
            // Remove a whole UTF-8 character (existing names may hold some)
            while (!m_text.empty() && (static_cast<unsigned char>(m_text.back()) & 0xC0) == 0x80)
               m_text.pop_back();
            if (!m_text.empty())
               m_text.pop_back();
            RequestRebuild();
         }));
   if (m_allowEmpty || m_text.find_first_not_of(' ') != string::npos)
      AddItem(std::make_unique<InGameUIItem>("Save"s, ""s,
         [this]()
         {
            m_onSave(m_text);
            m_player->m_liveUI->m_inGameUI.NavigateBack();
         }));
}


static string GetTableTitle(const string& uuid)
{
   const std::optional<LibraryTable> table = g_app->GetTableLibrary().GetTable(uuid);
   return table ? table->name : "Table"s;
}

static string GetTableInfo(const string& uuid)
{
   const std::optional<LibraryTable> table = g_app->GetTableLibrary().GetTable(uuid);
   if (!table)
      return ""s;
   return table->path + '\n' + GetTableStatsInfo(*table, true);
}

TableActionsPage::TableActionsPage(const string& uuid)
   : InGameUIPage(GetTableTitle(uuid), GetTableInfo(uuid), SaveMode::None)
   , m_uuid(uuid)
{
}

void TableActionsPage::AdjustItem(float direction, bool isInitialPress)
{
   // Actions of this page must not repeat while the button is held
   if (isInitialPress)
      InGameUIPage::AdjustItem(direction, isInitialPress);
}

void TableActionsPage::BuildPage()
{
   TableLibrary& library = g_app->GetTableLibrary();
   const std::optional<LibraryTable> table = library.GetTable(m_uuid);
   if (!table)
   {
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, "This table is no longer available"s));
      return;
   }
   const bool isRunning = IsRunningTable(m_player, *table);

   if (m_confirmDelete)
   {
      AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Header, "Delete this table and its files?"s));
      AddItem(std::make_unique<InGameUIItem>("Cancel"s, ""s,
         [this]()
         {
            m_confirmDelete = false;
            RequestRebuild();
         }));
      AddItem(std::make_unique<InGameUIItem>("Delete " + table->name, "This can not be undone"s,
         [this, name = table->name]()
         {
            const bool deleted = g_app->GetTableLibrary().Delete(m_uuid);
            m_player->m_liveUI->PushNotification(deleted ? name + " was deleted" : "Failed to delete " + name, 3000);
            m_player->m_liveUI->m_inGameUI.NavigateBack();
         }));
      return;
   }

   AddItem(std::make_unique<InGameUIItem>(isRunning ? "Restart"s : "Play"s, ""s,
      [this, tablePath = library.GetFullPath(*table)]()
      {
#ifdef VPX_TABLE_WEBSERVER
         if (g_app->GetWebServer().IsRunning())
            g_app->GetWebServer().Stop();
#endif
         g_app->m_nextTableFilename = tablePath;
         m_player->SetCloseState(Player::CS_CLOSE_CAPTURE_SCREENSHOT);
      }));

   AddItem(std::make_unique<InGameUIItem>(table->favorite ? ICON_FK_STAR " Remove from favorites"s : ICON_FK_STAR_O " Add to favorites"s, "Favorites have their own list in the table picker"s,
      [this, favorite = table->favorite]()
      {
         g_app->GetTableLibrary().SetFavorite(m_uuid, !favorite);
         RequestRebuild();
      }));

   AddItem(std::make_unique<InGameUIItem>("Rename..."s, "Change the name displayed for this table (its files are not renamed)"s,
      [this, name = table->name]()
      {
         const string uuid = m_uuid;
         m_player->m_liveUI->m_inGameUI.AddPage("tables/rename"s,
            [uuid, name]() { return std::make_unique<TextEntryPage>("Rename table"s, name, [uuid](const string& newName) { g_app->GetTableLibrary().Rename(uuid, newName); }); });
         m_player->m_liveUI->m_inGameUI.Navigate("tables/rename"s);
      }));

   // The running table holds its files and saves its settings when closing
   if (isRunning)
      return;

   if (FileExists(library.GetIniPath(*table)))
      AddItem(std::make_unique<InGameUIItem>("Reset table settings"s, "Remove the settings saved for this table (its .ini file)"s,
         [this]()
         {
            const bool reset = g_app->GetTableLibrary().ResetIni(m_uuid);
            m_player->m_liveUI->PushNotification(reset ? "Table settings were reset"s : "Failed to reset table settings"s, 3000);
            RequestRebuild();
         }));

   AddItem(std::make_unique<InGameUIItem>("Delete..."s, "Remove this table from the device"s,
      [this]()
      {
         m_confirmDelete = true;
         RequestRebuild();
      }));
}

}

#endif
