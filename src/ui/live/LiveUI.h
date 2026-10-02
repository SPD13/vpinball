// license:GPLv3+

#pragma once

#include "input/InputManager.h"
#include "renderer/Renderer.h"

#include "imgui/imgui.h"
#include "imgui_markdown/imgui_markdown.h"

#include "PerfUI.h"
#include "ingameui/InGameUI.h"
#include "EditorUI.h"
#include "NotificationOverlay.h"
#include "PlumbOverlay.h"
#include "BallControl.h"

#include <deque>

class LiveUI final
{
public:
   LiveUI(RenderDevice* const rd);
   ~LiveUI();

   void Render3D(); // Called to contribute to 3D Scene
   void RenderUI(); // Called to render UI overlay

   bool IsOpened() const { return IsEditorUIOpened() || IsInGameUIOpened(); }
   void HideUI();

   void OpenEditorUI() { m_editorUI.Open(); }
   bool IsEditorUIOpened() const { return m_editorUI.IsOpened(); }
   bool IsEditorViewMode() const { return m_editorUI.IsOpened() && !m_editorUI.IsPreview(); }

   void OpenInGameUI(const string& page = "homepage"s);
   bool IsInGameUIOpened() const { return m_inGameUI.IsOpened(); }

   void ToggleFPS() { m_perfUI.NextPerfMode(); }
   bool IsShowingFPSDetails() const { return m_perfUI.GetPerfMode() != PerfUI::PerfMode::PM_DISABLED; }
   
   void ShowTouchOverlay(bool show) { m_showTouchOverlay = show; }

   // Loading screen shown while switching tables (see Player::RenderLoadingFrame): a message with a spinner, drawn instead of the rest of the UI
   // while the text is not empty
   void SetLoadingText(const string& text) { m_loadingText = text; }
   bool IsLoadingScreenShown() const { return !m_loadingText.empty(); }

   unsigned int PushNotification(const string &message, const int lengthMs, const unsigned int reuseId = 0) { return m_notificationOverlay.PushNotification(message, lengthMs, reuseId); }

   // Messages that the player must acknowledge, shown one at a time in the in-game UI (which pauses the game). Lines starting with '!' are
   // shown as errors. ShowMessage can be called from any thread, ShowPendingMessage is called by the game loop.
   void ShowMessage(const string& title, const string& text);
   void ShowPendingMessage();
   const string& GetMessageTitle() const { return m_messageTitle; }
   const string& GetMessageText() const { return m_messageText; }

   // Ball Control
   BallControl m_ballControl;

   // In Game UI
   VPX::InGameUI::InGameUI m_inGameUI;

   // Profiler display data
   PerfUI m_perfUI;

   // UI context (to be moved outside of LiveUI API)
   void SetMarkdownStartId(const unsigned int startId) { markdown_start_id = startId; }
   const ImGui::MarkdownConfig &GetMarkdownConfig() const { return markdown_config; }
   ImFont *GetOverlayFont() const { return m_overlayFont; }
   float GetDPI() const { return m_uiScale; }
   int GetUIOrientation() const { return m_rotate; }
   static ImGuiKey GetImGuiKeyFromSDLScancode(const SDL_Scancode sdlk);
   static void CenteredText(const string &text);

   void HandleSDLEvent(SDL_Event &e) const;

private:
   string m_loadingText;
   void RenderLoadingScreen();

   std::mutex m_messageMutex;
   std::deque<std::pair<string, string>> m_pendingMessages; // Title and text
   string m_messageTitle, m_messageText; // Displayed by the 'misc/message' page

   void SetupImGuiStyle(const bool isEditor) const;
   
   void NewFrame();
   void AddMousePosEvent(bool isTouch, float x, float y) const;
   void UpdateScale();

   vector<std::shared_ptr<MeshBuffer>> m_meshBuffers;
   std::shared_ptr<MeshBuffer> m_vrPointerRayMesh; // Ray from the VR controller to the pointed position on the UI panel

   // Editor UI
   VPX::EditorUI::EditorUI m_editorUI;

   // Touch UI overlay
   void UpdateTouchUI();
   bool m_showTouchOverlay;

   // Emulated plumb overlay
   PlumbOverlay m_plumbOverlay;

   // Notifications
   NotificationOverlay m_notificationOverlay;

   // MarkDown support
   ImGuiID markdown_start_id;
   static ImGui::MarkdownConfig markdown_config;
   static void MarkdownFormatCallback(const ImGui::MarkdownFormatInfo &markdownFormatInfo, bool start);
   static void MarkdownLinkCallback(ImGui::MarkdownLinkCallbackData data);
   static ImGui::MarkdownImageData MarkdownImageCallback(ImGui::MarkdownLinkCallbackData data);

   // UI Context
   Player   *m_player;
   InputManager *m_pininput;
   std::unique_ptr<Renderer>& m_renderer;

   // Rendering
   RenderDevice* const m_rd;
   int m_rotate = 0;
   float m_uiScale = 0.f;

   // VR controller used as a pointer
   bool m_vrPointerVisible = false;
   bool m_vrPointerPressed = false;
   ImVec2 m_vrPointerPos;
   ImVec2 m_vrPointerAnchor; // Pointed position (0..1) when button navigation started, to switch to pointer navigation when the pointer clearly moves
   bool m_vrPointerAnchorValid = false;
   bool m_vrInGameUIWasOpened = false; // To place the VR UI panel in front of the player each time the in-game UI opens
   ImFont *m_baseFont = nullptr;
   ImFont *m_overlayBoldFont = nullptr;
   ImFont *m_overlayFont = nullptr;
};

namespace plog
{
Record &operator<<(Record &record, const ImVec2 &pt);
}
