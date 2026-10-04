// license:GPLv3+

#pragma once

#include "InGameUIPage.h"

#ifdef __STANDALONE__

#include <future>

namespace VPinballLib { struct Table; }
class BaseTexture;

namespace VPX::InGameUI
{

// Keyboard drawn with buttons, for VR where there is no keyboard: keys change 'text' at once (up to 'maxLength' bytes). With 'capitalizeWords',
// letters are uppercase at the start of words and lowercase elsewhere, otherwise always lowercase. Returns true when 'Done' is pressed
// ('withDone' adds this key).
bool RenderVirtualKeyboard(string& text, size_t maxLength, bool capitalizeWords, bool withDone);

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
   bool m_virtualKeyboard = false; // Shown under the search field in VR, where there is no keyboard, from its activation until 'Done'
   void RenderPager(const char* item);
   void RenderPairing();
   int m_pageCount = 1;
   string m_reselectItem;
   int m_reselectDelay = 0;

   uint64_t m_revision = 0;
   uint64_t m_scoresRevision = 0; // Of the profiles and scores, shown by the player item and the Scores tab
   bool m_scanning = false;
   string m_webServerUrl;
   string m_pairingCode;
};

// Text entry with the 4 navigation buttons, like the initials of an arcade high score: left/right select a character, which is then added.
// A virtual keyboard is also shown, for pointers (VR controllers, mouse).
class TextEntryPage final : public InGameUIPage
{
public:
   TextEntryPage(const string& title, const string& text, const std::function<void(const string&)>& onSave, bool allowEmpty = false, size_t maxLength = 64);

   void AdjustItem(float direction, bool isInitialPress) override;

private:
   void BuildPage() override;

   string m_text;
   size_t m_charIndex = 0;
   const std::function<void(const string&)> m_onSave;
   const bool m_allowEmpty;
   const size_t m_maxLength;
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
