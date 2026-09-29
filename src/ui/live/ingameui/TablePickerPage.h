// license:GPLv3+

#pragma once

#include "InGameUIPage.h"

#ifdef __STANDALONE__

#include <future>

namespace VPinballLib { struct Table; }
class BaseTexture;

namespace VPX::InGameUI
{

// Lists the tables of the application's table library, to switch table without leaving the player
class TablePickerPage final : public InGameUIPage
{
public:
   TablePickerPage();
   ~TablePickerPage() override;

   void Open(bool isBackwardAnimation) override;
   void Render(float elapsedS) override;
   void AdjustItem(float direction, bool isInitialPress) override;

private:
   void BuildPage() override;

   // Table images, loaded when they get visible. Decoding takes a few milliseconds, too long for a frame: it runs on worker threads, a few at
   // a time, and the decoded images are uploaded to the GPU one per frame at most. The tiles show a spinner meanwhile.
   struct Thumbnail
   {
      std::shared_ptr<BaseTexture> texture;
      int64_t modifiedAt = 0;
      std::future<std::shared_ptr<BaseTexture>> loading; // Valid while decoding
      int64_t loadingModifiedAt = 0;
   };
   ImTextureID GetThumbnail(const VPinballLib::Table& table);
   bool IsThumbnailLoading(const VPinballLib::Table& table) const;
   void UpdateThumbnailLoads();
   ankerl::unordered_dense::map<string, Thumbnail> m_thumbnails;
   vector<std::future<std::shared_ptr<BaseTexture>>> m_abandonedLoads; // Of thumbnails no longer displayed, kept until done not to wait for them
   bool m_thumbnailLoadedThisFrame = false;

   void SetTab(int tab);
   void SetPage(int page, const string& pagerItem);
   void SetSearch(const string& search);
   void RenderTabs();
   void BuildMenuTab();
   void KeepThumbnails(const ankerl::unordered_dense::set<string>& displayedTables);
   void RenderSearch();
   void RenderVirtualKeyboard();
   bool m_virtualKeyboard = false; // Shown under the search field in VR, where there is no keyboard, from its activation until 'Done'
   void RenderPager(const char* item);
   int m_pageCount = 1;
   string m_reselectItem;
   int m_reselectDelay = 0;

   uint64_t m_revision = 0;
   bool m_scanning = false;
   string m_webServerUrl;
   string m_pairingCode;
};

// Text entry with the 4 navigation buttons, like the initials of an arcade high score: left/right select a character, which is then added
class TextEntryPage final : public InGameUIPage
{
public:
   TextEntryPage(const string& title, const string& text, const std::function<void(const string&)>& onSave, bool allowEmpty = false);

   void AdjustItem(float direction, bool isInitialPress) override;

private:
   void BuildPage() override;

   string m_text;
   size_t m_charIndex = 0;
   const std::function<void(const string&)> m_onSave;
   const bool m_allowEmpty;
};

// Actions on a table of the library: play, rename, reset settings, delete
class TableActionsPage final : public InGameUIPage
{
public:
   explicit TableActionsPage(const string& uuid);

   void AdjustItem(float direction, bool isInitialPress) override;

private:
   void BuildPage() override;

   const string m_uuid;
   bool m_confirmDelete = false;
};

}

#endif
