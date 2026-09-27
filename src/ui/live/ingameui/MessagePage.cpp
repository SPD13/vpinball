// license:GPLv3+

#include "core/stdafx.h"
#include "MessagePage.h"

#include "core/VPApp.h"
#include "ui/live/LiveUI.h"


namespace VPX::InGameUI
{

MessagePage::MessagePage()
   : InGameUIPage(g_pplayer->m_liveUI->GetMessageTitle(), ""s, SaveMode::None)
{
}

void MessagePage::BuildPage()
{
   std::istringstream lines(g_pplayer->m_liveUI->GetMessageText());
   for (string line; std::getline(lines, line);)
   {
      // Lines starting with '!' are errors (see VPXPluginAPI::ShowMessage)
      if (line.starts_with('!'))
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Error, line.substr(1)));
      else
         AddItem(std::make_unique<InGameUIItem>(InGameUIItem::LabelType::Info, line));
   }

   AddItem(std::make_unique<InGameUIItem>("Continue"s, "Close this message"s, [this]() { m_player->m_liveUI->m_inGameUI.NavigateBack(); }));

#ifdef __STANDALONE__
   const bool toLauncher = g_app->m_launcherMode;
#else
   constexpr bool toLauncher = false;
#endif
   AddItem(std::make_unique<InGameUIItem>(toLauncher ? "Back to the table picker"s : "Quit the table"s, ""s,
      [this]()
      {
         // Same as the Exit Game action
#ifdef __STANDALONE__
         m_player->SetCloseState((g_isMobile || g_app->m_launcherMode) ? Player::CS_CLOSE_CAPTURE_SCREENSHOT : Player::CS_CLOSE_APP);
#else
         m_player->SetCloseState(Player::CS_STOP_PLAY);
#endif
      }));
}

}
