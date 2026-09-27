// license:GPLv3+

#pragma once

#include "InGameUIPage.h"

namespace VPX::InGameUI
{

// A message that the player must acknowledge, like a problem that prevents the table from working (see LiveUI::ShowMessage)
class MessagePage final : public InGameUIPage
{
public:
   MessagePage();

private:
   void BuildPage() override;
};

}
