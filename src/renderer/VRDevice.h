// license:GPLv3+

#pragma once

#if defined(ENABLE_XR)
   #include "bx/platform.h"

   #if defined(__ANDROID__) && BX_PLATFORM_WINDOWS
      // Our setup may lead to this incorrect double definition, so fix it
      #undef BX_PLATFORM_WINDOWS
      #define BX_PLATFORM_WINDOWS 0
      #undef BX_PLATFORM_ANDROID
      #define BX_PLATFORM_ANDROID 1
   #elif defined(__linux__) && BX_PLATFORM_WINDOWS
      // Same on Linux
      #undef BX_PLATFORM_WINDOWS
      #define BX_PLATFORM_WINDOWS 0
      #undef BX_PLATFORM_LINUX
      #define BX_PLATFORM_LINUX 1
   #endif

   #if BX_PLATFORM_WINDOWS && defined(__STANDALONE__)
      // Standalone Windows build (MinGW): Vulkan only, to render like the standalone builds for headsets
      #define XR_USE_PLATFORM_WIN32
      #define XR_USE_GRAPHICS_API_VULKAN
      #define VK_USE_PLATFORM_WIN32_KHR
   #elif BX_PLATFORM_WINDOWS
      #define XR_USE_PLATFORM_WIN32
      #define XR_USE_GRAPHICS_API_VULKAN
      #define XR_USE_GRAPHICS_API_OPENGL
      #define XR_USE_GRAPHICS_API_OPENGL_ES
      #define XR_USE_GRAPHICS_API_D3D11
      #define XR_USE_GRAPHICS_API_D3D12
      #define VK_USE_PLATFORM_WIN32_KHR
   #elif BX_PLATFORM_ANDROID
      #define XR_USE_TIMESPEC
      #define XR_USE_PLATFORM_ANDROID
      #define XR_USE_GRAPHICS_API_VULKAN
      //#define XR_USE_GRAPHICS_API_OPENGL_ES
   #elif BX_PLATFORM_LINUX
      // Headless Vulkan session (no window system binding needed), for example for the SteamVR runtime of SteamOS devices
      #define XR_USE_TIMESPEC
      #define XR_USE_GRAPHICS_API_VULKAN
   #endif


   // OpenXR Dependencies

   #ifdef XR_USE_PLATFORM_ANDROID
   #include <android/native_window.h>
   #include <android/window.h>
   #include <android/native_window_jni.h>
   #endif  // XR_USE_PLATFORM_ANDROID

   #ifdef XR_USE_PLATFORM_WIN32

   #include <winapifamily.h>
   #if !(WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM))
   // Enable desktop partition APIs, such as RegOpenKeyEx, LoadLibraryEx, PathFileExists etc.
   #undef WINAPI_PARTITION_DESKTOP
   #define WINAPI_PARTITION_DESKTOP 1
   #endif

   #ifndef NOMINMAX
   #define NOMINMAX
   #endif  // !NOMINMAX

   #ifndef WIN32_LEAN_AND_MEAN
   #define WIN32_LEAN_AND_MEAN
   #endif  // !WIN32_LEAN_AND_MEAN

   #include <windows.h>
   #include <unknwn.h>

   #endif  // XR_USE_PLATFORM_WIN32

   #ifdef XR_USE_GRAPHICS_API_D3D11
   #include <d3d11.h>
   #endif  // XR_USE_GRAPHICS_API_D3D11

   #ifdef XR_USE_GRAPHICS_API_D3D12
   #include <d3d12.h>
   #endif  // XR_USE_GRAPHICS_API_D3D12

   #ifdef XR_USE_PLATFORM_XLIB
   #include <X11/Xlib.h>
   #include <X11/Xutil.h>
   #endif  // XR_USE_PLATFORM_XLIB

   #ifdef XR_USE_PLATFORM_XCB
   #include <xcb/xcb.h>
   #endif  // XR_USE_PLATFORM_XCB

   #ifdef XR_USE_GRAPHICS_API_OPENGL
   #if defined(XR_USE_PLATFORM_XLIB) || defined(XR_USE_PLATFORM_XCB)
   #include <GL/glx.h>
   #endif  // (XR_USE_PLATFORM_XLIB || XR_USE_PLATFORM_XCB)
   #ifdef XR_USE_PLATFORM_XCB
   #include <xcb/glx.h>
   #endif  // XR_USE_PLATFORM_XCB
   #ifdef XR_USE_PLATFORM_MACOS
   #include <OpenCL/cl_gl_ext.h>
   #endif  // XR_USE_PLATFORM_MACOS
   #endif  // XR_USE_GRAPHICS_API_OPENGL

   #ifdef XR_USE_GRAPHICS_API_OPENGL_ES
   #include <SDL3/SDL_egl.h>
   #endif  // XR_USE_GRAPHICS_API_OPENGL_ES

   #ifdef XR_USE_GRAPHICS_API_VULKAN
   #include <vulkan/vulkan.h>
   #ifdef XR_USE_PLATFORM_ANDROID
   #include <vulkan/vulkan_android.h>
   #endif  // XR_USE_PLATFORM_ANDROID
   #endif  // XR_USE_GRAPHICS_API_VULKAN

   #ifdef XR_USE_PLATFORM_WAYLAND
   #include "wayland-client.h"
   #endif  // XR_USE_PLATFORM_WAYLAND

   #ifdef XR_USE_PLATFORM_EGL
   #include <EGL/egl.h>
   #endif  // XR_USE_PLATFORM_EGL

   #if defined(XR_USE_PLATFORM_XLIB) || defined(XR_USE_PLATFORM_XCB)
   #ifdef Success
   #undef Success
   #endif  // Success

   #ifdef Always
   #undef Always
   #endif  // Always

   #ifdef None
   #undef None
   #endif  // None
   #endif // defined(XR_USE_PLATFORM_XLIB) || defined(XR_USE_PLATFORM_XCB)

   #include <openxr/openxr.h>
   #include <openxr/openxr_platform.h>

   #include "input/XRInputHandler.h"
#endif

#include "parts/PartGroup.h"

class MeshBuffer;

// The VR device lives as long as the OpenXR instance: when tables are played one after another, the application can keep it from one table
// to the next (see VPApp::AcquireVRDevice), so the runtime does not see the application quit and start again. Each table then creates its
// own session (CreateSession / ReleaseSession, from the render thread) and applies its settings (ApplyTableSettings). SteamVR 2.17.10 does
// not survive a second session on one instance (see VPApp::m_keepVRDeviceBetweenTables), so the device is created again for each table there.
class VRDevice final
{
public:
   VRDevice(const Settings& settings);
   ~VRDevice();

   // Settings that a table may override (scene placement, foveation, resolution, ...), applied each time a table is played with this device
   void ApplyTableSettings(const Settings& settings);

   unsigned int GetEyeWidth() const { return m_eyeWidth; }
   unsigned int GetEyeHeight() const { return m_eyeHeight; }

   #if defined(ENABLE_XR)
   // The UI is displayed on a panel standing in the room, placed in front of the player in the table's forward direction (not where the head looks)
   // when it is first displayed and each time RecenterUIPanel is called
   void RecenterUIPanel() { m_uiPanelPlaced = false; }
   // Transforms from UI pixel coordinates (0..width, 0..height from the top left of the panel) to each eye clip space. Returns false if the panel is not placed yet
   bool GetUIPanelTransforms(float width, float height, Matrix3D (&pixelToClip)[2]) const;
   // Position pointed by a controller on the UI panel, normalized to 0..1 from its top left, and vertical axis of the thumbstick of that
   // controller (-1..1, up is positive). Returns false if no controller points at it
   bool GetUIPointer(float& x, float& y, bool& pressed, float& scroll) const
   {
      x = m_uiPointerX;
      y = m_uiPointerY;
      pressed = m_uiPointerPressed;
      scroll = m_uiPointerScroll;
      return m_uiPointerValid;
   }
   // Transforms from a unit quad (x along the ray from the controller to the pointed position, y across it) to each eye clip space.
   // Returns false if no controller points at the UI panel
   bool GetUIPointerRayTransforms(Matrix3D (&quadToClip)[2]) const;
   // Unless None, both eyes frame the table instead of following the head, to capture the image of a table: its backglass, its whole playfield
   // seen from above, or its whole cabinet seen from the front and a little from the side and above
   enum class TableCaptureView { None, Backglass, Table, Cabinet };
   // backglassBounds: bounding vertices of the parts showing the backglass (see Player::CaptureTableImage), each with the space reference of its
   // part (PartGroupData::SpaceReference). Without them, the backglass is placed like on a real cabinet.
   void SetTableCaptureView(TableCaptureView view, vector<std::pair<int, vec3>> backglassBounds = {})
   {
      if (view != TableCaptureView::None)
         m_tableCaptureBounds = std::move(backglassBounds); // Only read by the render thread while a capture view is set
      m_tableCaptureView = view;
   }
   #endif
   
   float GetLockbarWidth() const { return m_lockbarWidth; }
   void SetLockbarWidth(float width) { m_lockbarWidth = width; m_worldDirty = true; }
   float GetLockbarHeight() const { return m_lockbarHeight; }
   void SetLockbarHeight(float height) { m_lockbarHeight = height; m_worldDirty = true; }
   bool IsLockFeetToGround() const { return m_lockFeetToGround; }
   void SetLockFeetToGround(bool lock) { m_lockFeetToGround = lock; m_worldDirty = true; }

   void OffsetTable(float dx, float dy, float dz);
   void RecenterTable();
   float GetSceneOrientation() const { return m_orientation; }
   const Vertex3Ds& GetSceneOffset() const { return m_tablePos; }
   void SetSceneOrientation(float orientation) { m_orientation = orientation; m_worldDirty = true; }
   void SetSceneOffset(const Vertex3Ds& pos) { m_tablePos = pos; m_worldDirty = true; }
   void SaveVRSettings(Settings& settings) const;

   void UpdateVRPosition(PartGroupData::SpaceReference spaceRef, ModelViewProj& mvp);

   float GetPredictedDisplayTimestamp() const { return m_predictedDisplayTimestamp; }

private:
   unsigned int m_eyeWidth = 1080;
   unsigned int m_eyeHeight = 1020;

   float m_scale = 1.0f;
   float m_lockbarWidth = 57.0f; // Real world width of the lockbar in cm
   float m_lockbarHeight = 85.0f; // Real world height (from ground) of the lockbar in cm
   bool m_lockFeetToGround = true;
   float m_orientation = 0.0f;
   Vertex3Ds m_tablePos;
   float m_slope = 0.0f;

   float m_predictedDisplayTimestamp = 0.f;

   bool m_worldDirty = true;
   struct Viewpoint
   {
      Matrix3D m_toWorld; // Matrix to transform from this viewpoint to world coordinates
      Matrix3D m_view[2];
   };
   Viewpoint m_pfWorld;
   Viewpoint m_cabWorld;
   Viewpoint m_feetWorld;
   Viewpoint m_roomWorld;
   Matrix3D m_roomProj[2];
   Matrix3D m_sceneProj[2];

#ifdef ENABLE_XR
public:
   int GetDisplayRefreshRateMode() const { return m_displayRefreshRateMode; }
   void SetDisplayRefreshRateMode(int mode);
   // Foveated rendering (see the private members below): level 0 to 3, eye-tracked when the headset allows it
   int GetFoveationMode() const { return m_foveationMode; }
   void SetFoveationMode(int mode);
   bool IsFoveationEyeTracked() const { return m_foveationEyeTracked; }
   void SetFoveationEyeTracked(bool eyeTracked);
   // One line for the settings page: "not supported", "fixed", "eye-tracked", ...
   string GetFoveationStatus() const;
   bool IsOpenXRReady() const { return m_xrInstance != XR_NULL_HANDLE; }
   void SetupHMD();
   bool IsOpenXRHMDReady() const { return m_systemID != XR_NULL_SYSTEM_ID; }
   // Graphics backend the device was created for, and the one the settings ask for (the device must be created again if they differ)
   bgfx::RendererType::Enum GetRendererType() const { return m_rendererType; }
   static bgfx::RendererType::Enum SelectRendererType(const Settings& settings);
   // The runtime lost the session or the instance: the device can't be used anymore and must be created again
   bool IsLost() const { return m_lost; }
   void CreateSession();
   void ReleaseSession(); // Ends the session (letting the runtime stop it) then destroys it and all its objects, the instance is kept
   void* GetGraphicContext() const;
   bgfx::RendererType::Enum GetGraphicContextType() const;
   void PollEvents();
   void RenderFrame(class RenderDevice* rd, const std::function<void(RenderTarget* vrRenderTarget)>& submitFrame);
   void UpdateVisibilityMask(class RenderDevice* rd);
   bool UseDepthBuffer() const { return m_depthExtensionSupported; }
   bgfx::TextureFormat::Enum GetDepthFormat() const { return m_depthSwapchainInfo.format; }

   void DiscardVisibilityMask() { m_visibilityMask = nullptr; }
   std::shared_ptr<MeshBuffer> GetVisibilityMask() const { return m_visibilityMask; }

   Matrix3D* GetVisibilityMaskProjs() { return &m_nextProj[0]; }

   void EnableControllerViewCentering(bool enable) { m_controllerViewCentering = enable; }
   bool IsControllerViewCenteringEnabled() const { return m_controllerViewCentering; }

   // Models of the controllers the player holds, as the system shows them (XR_EXT_render_model with XR_EXT_interaction_render_model): the runtime gives
   // their glTF asset, places them and animates their parts (buttons, triggers, thumbsticks). Updated by the render thread before each frame is
   // prepared, so they are read while preparing a frame. A model that is not located (not tracked, hidden by the setting, table capture) is not drawn.
   struct ControllerModel
   {
      struct NodeState
      {
         XrPosef pose; // Relative to the parent node
         bool visible;
      };
      uint64_t id = 0; // Runtime render model id: a model with the same id keeps the same asset
      std::shared_ptr<const vector<uint8_t>> asset; // glTF binary (GLB)
      vector<string> animatableNodes; // Names of the glTF nodes moved by the runtime, in the order of nodeStates
      vector<NodeState> nodeStates;
      bool located = false;
      Matrix3D modelToReference; // glTF model space to the reference space, both in meters
   };
   const vector<ControllerModel>& GetControllerModels() const { return m_controllerModels; }
   // Reference space (meters) to the room space reference of the scene (VPU)
   const Matrix3D& GetReferenceToRoom() const { return m_referenceToRoom; }
   bool IsShowControllers() const { return m_showControllers; }
   void SetShowControllers(bool show) { m_showControllers = show; }
   // Transform from the space of a pose to the space it is given in (rotation then translation, for row vectors)
   static Matrix3D PoseToMatrix(const XrPosef& pose)
   {
      const float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
      return Matrix3D(
         1.f - 2.f * (y * y + z * z), 2.f * (x * y + z * w), 2.f * (x * z - y * w), 0.f,
         2.f * (x * y - z * w), 1.f - 2.f * (x * x + z * z), 2.f * (y * z + x * w), 0.f,
         2.f * (x * z + y * w), 2.f * (y * z - x * w), 1.f - 2.f * (x * x + y * y), 0.f,
         pose.position.x, pose.position.y, pose.position.z, 1.f);
   }

   enum class SwapchainType : uint8_t
   {
      COLOR,
      DEPTH
   };

   struct SwapchainInfo
   {
      XrSwapchain swapchain = XR_NULL_HANDLE;
      // Fragment density maps of the runtime (XR_FB_foveation_vulkan), one per swapchain image (invalid handle when the runtime gave none), see CreateFoveationTextures
      vector<bgfx::TextureHandle> foveationTextures;
      uint32_t foveationWidth = 0;
      uint32_t foveationHeight = 0;
      uint32_t width = 0;
      uint32_t height = 0;
      uint32_t arraySize = 0;
      bool isDepth = false;
      int64_t backendFormat = 0;
      bgfx::TextureFormat::Enum format;
      std::vector<bgfx::TextureHandle> imageViews;
   };

private:
   XrInstance m_xrInstance = XR_NULL_HANDLE;
   std::vector<const char*> m_activeAPILayers;
   std::vector<const char*> m_activeInstanceExtensions;
   std::vector<std::string> m_apiLayers;

   XrFormFactor m_formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
   XrSystemId m_systemID = {};
   XrSystemProperties m_systemProperties = { XR_TYPE_SYSTEM_PROPERTIES };

   XrSession m_session = {};
   XrSessionState m_sessionState = XR_SESSION_STATE_UNKNOWN;
   bool m_sessionRunning = false;
   bool m_exitRequested = false; // We asked the runtime to stop the session (see EndSession), so its EXITING state is expected
   std::atomic<bool> m_lost = false;
   void EndSession();
   void DrainEvents();
   void UpdateEyeResolution(const Settings& settings);

   std::vector<XrViewConfigurationType> m_applicationViewConfigurations = { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO };
   std::vector<XrViewConfigurationType> m_viewConfigurations;
   XrViewConfigurationType m_viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM;
   std::vector<XrViewConfigurationView> m_viewConfigurationViews;

   SwapchainInfo m_colorSwapchainInfo = {};
   SwapchainInfo m_depthSwapchainInfo = {};
   std::vector<std::unique_ptr<RenderTarget>> m_swapchainRenderTargets;
   std::vector<XrEnvironmentBlendMode> m_applicationEnvironmentBlendModes = { XR_ENVIRONMENT_BLEND_MODE_OPAQUE, XR_ENVIRONMENT_BLEND_MODE_ADDITIVE };
   std::vector<XrEnvironmentBlendMode> m_environmentBlendModes = {};
   XrEnvironmentBlendMode m_environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM;

   XrSpace m_referenceSpace = XR_NULL_HANDLE;
   
   bool m_headsetViewCentering = false;
   bool m_controllerViewCentering = false;
   XrSpace m_leftControllerSpace = XR_NULL_HANDLE;
   XrSpace m_rightControllerSpace = XR_NULL_HANDLE;

   // UI panel, in the reference space (meters): top left corner, and vectors along its full width and height
   bool m_uiPanelPlaced = false;
   vec3 m_uiPanelOrigin, m_uiPanelRight, m_uiPanelDown;
   Matrix3D m_uiPanelViewProj[2]; // Reference space to each eye clip space
   // UI pointer: the aim ray of a controller, intersected with the UI panel
   XrSpace m_leftAimSpace = XR_NULL_HANDLE;
   XrSpace m_rightAimSpace = XR_NULL_HANDLE;
   bool m_uiPointerValid = false;
   bool m_uiPointerPressed = false;
   float m_uiPointerX = 0.f;
   float m_uiPointerY = 0.f;
   float m_uiPointerScroll = 0.f;
   std::atomic<TableCaptureView> m_tableCaptureView = TableCaptureView::None;
   vector<std::pair<int, vec3>> m_tableCaptureBounds;
   void SetTableCaptureViewPoses(std::vector<XrView>& views, float vpuToWorldScale, TableCaptureView captureView) const;
   vec3 m_uiPointerRayStart, m_uiPointerRayEnd, m_uiHeadPos; // Reference space (meters)
   void UpdateUIPanel(const std::vector<XrView>& views, XrTime time);

   struct RenderLayerInfo
   {
      XrTime predictedDisplayTime = 0;
      std::vector<XrCompositionLayerBaseHeader*> layers;
      XrCompositionLayerProjection layerProjection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
      std::vector<XrCompositionLayerProjectionView> layerProjectionViews;
      std::vector<XrCompositionLayerDepthInfoKHR> depthInfoViews;
      XrCompositionLayerPassthroughFB layerPassthrough = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
   };

   bool m_depthExtensionSupported = false;
   bool m_colorSpaceExtensionSupported = false;
   #if BX_PLATFORM_WINDOWS
   bool m_win32PerfCounterExtensionSupported = false;
   PFN_xrConvertTimeToWin32PerformanceCounterKHR m_xrConvertTimeToWin32PerformanceCounterKHR = nullptr;
   #elif BX_PLATFORM_ANDROID || BX_PLATFORM_LINUX
   bool m_convertTimespecTimeExtensionSupported = false;
   PFN_xrConvertTimeToTimespecTimeKHR m_xrConvertTimeToTimespecTimeKHR = nullptr;
   #endif
   bool m_displayRefreshRateExtensionSupported = false;
   PFN_xrGetDisplayRefreshRateFB m_xrGetDisplayRefreshRateFB = nullptr;
   PFN_xrRequestDisplayRefreshRateFB m_xrRequestDisplayRefreshRateFB = nullptr;
   int m_displayRefreshRateMode = 0;
   void ApplyDisplayRefreshRate();

   // Foveated rendering through the runtime (XR_FB_foveation and XR_META_foveation_eye_tracked, see Settings PlayerVR/Foveation). The runtime builds the
   // fragment density maps and applies them to the color swapchain; which render passes of ours benefit is up to the runtime (on the Steam Frame, Valve's
   // FDM injection layer does it), so the result is checked with the performance counters below rather than assumed.
   bool m_foveationExtensionSupported = false;
   bool m_foveationEyeTrackedExtensionSupported = false;
   bool m_foveationEyeTrackedSystemSupported = false;
   PFN_xrCreateFoveationProfileFB m_xrCreateFoveationProfileFB = nullptr;
   PFN_xrDestroyFoveationProfileFB m_xrDestroyFoveationProfileFB = nullptr;
   PFN_xrUpdateSwapchainFB m_xrUpdateSwapchainFB = nullptr;
   PFN_xrGetFoveationEyeTrackedStateMETA m_xrGetFoveationEyeTrackedStateMETA = nullptr;
   int m_foveationMode = 0;
   bool m_foveationEyeTracked = true;
   bool m_foveationApplied = false; // The profile below is what the color swapchain currently uses
   bool m_foveationEyeTrackedActive = false; // Last state reported by the runtime
   bool m_foveationFlipX = false;
   bool m_foveationFlipY = true;
   XrVector2f m_foveationCenter[2] = { { 0.f, 0.f }, { 0.f, 0.f } }; // Last gaze reported by the runtime, normalized (-1..1), per eye
   XrFoveationProfileFB m_foveationProfile = XR_NULL_HANDLE;
   void ApplyFoveation();
   void UpdateFoveationState();
   // Our own density map, one layer per eye, with the high density area in the middle and moved to the gaze with offsets. Needed because
   // the driver only honors offsets on maps created for them, which the runtime's maps are not; the runtime's profile is still applied
   // to the swapchain, it is what turns the eye tracking on.
   bgfx::TextureHandle m_ownFoveationMap = BGFX_INVALID_HANDLE;
   uint32_t m_ownFoveationMapWidth = 0;
   uint32_t m_ownFoveationMapHeight = 0;
   void CreateOwnFoveationMap();
   void FillOwnFoveationMap();

   // Performance counters of the runtime (XR_META_performance_metrics), logged periodically to compare settings on the device
   bool m_performanceMetricsExtensionSupported = false;
   PFN_xrEnumeratePerformanceMetricsCounterPathsMETA m_xrEnumeratePerformanceMetricsCounterPathsMETA = nullptr;
   PFN_xrSetPerformanceMetricsStateMETA m_xrSetPerformanceMetricsStateMETA = nullptr;
   PFN_xrQueryPerformanceMetricsCounterMETA m_xrQueryPerformanceMetricsCounterMETA = nullptr;
   vector<std::pair<string, XrPath>> m_performanceCounters;
   bool m_performanceCountersEnabled = false; // Enabled on the current session (see CreateSession and ReleaseSession)
   double m_nextStatusLogTime = 0.;
   void LogRuntimeStatus();

   Matrix3D m_nextProj[2];

   bool m_debugUtilsExtensionSupported = false;
   XrDebugUtilsMessengerEXT m_debugUtilsMessenger = XR_NULL_HANDLE;
   static XrBool32 OpenXRMessageCallbackFunction(XrDebugUtilsMessageSeverityFlagsEXT messageSeverity, XrDebugUtilsMessageTypeFlagsEXT messageType, const XrDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData);

   // Controller models (see GetControllerModels), with the runtime objects of each model at the same index
   bool m_renderModelExtensionSupported = false;
   std::atomic<bool> m_showControllers = true; // Set by the settings page while the render thread reads it
   bool m_controllerModelsDirty = true; // The list of models must be asked again to the runtime
   double m_controllerModelsRetryTime = 0.; // When the list is asked again after an asset was not available
   vector<ControllerModel> m_controllerModels;
   struct RenderModelHandles
   {
      #ifdef XR_EXT_render_model
      XrRenderModelEXT renderModel = XR_NULL_HANDLE;
      #endif
      XrSpace space = XR_NULL_HANDLE;
   };
   vector<RenderModelHandles> m_renderModelHandles;
   Matrix3D m_referenceToRoom;
   void UpdateControllerModels();
   void LocateControllerModels(XrTime time);
   void DestroyControllerModels();
   #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
   PFN_xrCreateRenderModelEXT m_xrCreateRenderModelEXT = nullptr;
   PFN_xrDestroyRenderModelEXT m_xrDestroyRenderModelEXT = nullptr;
   PFN_xrGetRenderModelPropertiesEXT m_xrGetRenderModelPropertiesEXT = nullptr;
   PFN_xrCreateRenderModelSpaceEXT m_xrCreateRenderModelSpaceEXT = nullptr;
   PFN_xrCreateRenderModelAssetEXT m_xrCreateRenderModelAssetEXT = nullptr;
   PFN_xrDestroyRenderModelAssetEXT m_xrDestroyRenderModelAssetEXT = nullptr;
   PFN_xrGetRenderModelAssetDataEXT m_xrGetRenderModelAssetDataEXT = nullptr;
   PFN_xrGetRenderModelAssetPropertiesEXT m_xrGetRenderModelAssetPropertiesEXT = nullptr;
   PFN_xrGetRenderModelStateEXT m_xrGetRenderModelStateEXT = nullptr;
   PFN_xrEnumerateInteractionRenderModelIdsEXT m_xrEnumerateInteractionRenderModelIdsEXT = nullptr;
   vector<XrRenderModelNodeStateEXT> m_renderModelNodeStates; // Scratch buffer for LocateControllerModels
   struct ControllerModelDebug
   {
      XrResult locateResult;
      XrSpaceLocationFlags flags;
      XrPosef pose;
      XrResult stateResult;
      int visibleNodes;
   };
   ControllerModelDebug m_controllerModelsDebug[4] {}; // Last results of LocateControllerModels, logged by LogRuntimeStatus
   #endif

   bool m_visibilityMaskExtensionSupported = false;
   PFN_xrGetVisibilityMaskKHR xrGetVisibilityMaskKHR = nullptr;
   bool m_visibilityMaskDirty = true;
   std::shared_ptr<MeshBuffer> m_visibilityMask;

   bool m_passthroughExtensionSupported = false;
   XrPassthroughFB m_passthrough = XR_NULL_HANDLE;
   XrPassthroughLayerFB m_passthroughLayer = XR_NULL_HANDLE;
   bool m_passthroughEnabled = false;

   bgfx::RendererType::Enum m_rendererType;
   std::unique_ptr<class XRGraphicBackend> m_backend;

   float m_sceneSize = 0.f;

   XRInputHandler* m_xrInputHandler = nullptr;
#endif
};
