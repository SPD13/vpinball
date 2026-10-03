// license:GPLv3+

#include "core/stdafx.h"
#include "VRDevice.h"

#include "core/VPApp.h"
#include "core/vpversion.h"
#include "parts/primitive.h"
#include "renderer/MeshBuffer.h"
#include "renderer/IndexBuffer.h"
#include "renderer/RenderTarget.h"
#include "renderer/Sampler.h"
#include "renderer/VertexBuffer.h"

// MSVC Concurrency Viewer support
// This requires to add the MSVC Concurrency SDK to the project
//#define MSVC_CONCURRENCY_VIEWER
#ifdef MSVC_CONCURRENCY_VIEWER
#include <cvmarkersobj.h>
using namespace Concurrency::diagnostic;
extern marker_series series;
#endif

#if defined(ENABLE_XR) && defined(ENABLE_BGFX)
   #include "bgfx/bgfx.h"
   #ifdef _far
      #undef _far
   #endif
   #ifdef _near
      #undef _near
   #endif
   #include "bx/math.h"
   #include "renderer/Renderer.h"
   #include <map>
   #include <set>
   #include <vector>
   #include <time.h>

static inline const char* GetXRErrorString(XrInstance xrInstance, XrResult result)
{
   static char string[XR_MAX_RESULT_STRING_SIZE];
   xrResultToString(xrInstance, result, string);
   return string;
}

#define OPENXR_CHECK(x, y)                                                                                                                                                                   \
   {                                                                                                                                                                                         \
      if (const XrResult res = (x); !XR_SUCCEEDED(res))                                                                                                                                      \
      {                                                                                                                                                                                      \
         PLOGE << "ERROR: OPENXR: " << int(res) << " (" << (m_xrInstance ? GetXRErrorString(m_xrInstance, res) : "") << ") " << (y);                                                         \
      }                                                                                                                                                                                      \
   }

inline static float XrRcpSqrt(const float x)
{
   constexpr float SMALLEST_NON_DENORMAL = FLT_MIN; // ( 1U << 23 )
   const float rcp = (x >= SMALLEST_NON_DENORMAL) ? 1.0f / sqrtf(x) : 1.0f;
   return rcp;
}

inline static void XrVector3f_Lerp(XrVector3f* const result, const XrVector3f* const a, const XrVector3f* const b, const float fraction) {
   result->x = a->x + fraction * (b->x - a->x);
   result->y = a->y + fraction * (b->y - a->y);
   result->z = a->z + fraction * (b->z - a->z);
}

inline static void XrVector3f_Scale(XrVector3f* const result, const XrVector3f* const a, const float scaleFactor) {
   result->x = a->x * scaleFactor;
   result->y = a->y * scaleFactor;
   result->z = a->z * scaleFactor;
}

inline static void XrQuaternionf_Lerp(XrQuaternionf* const result, const XrQuaternionf* const a, const XrQuaternionf* const b, const float fraction) {
   const float s = a->x * b->x + a->y * b->y + a->z * b->z + a->w * b->w;
   const float fa = 1.0f - fraction;
   const float fb = (s < 0.0f) ? -fraction : fraction;
   const float x = a->x * fa + b->x * fb;
   const float y = a->y * fa + b->y * fb;
   const float z = a->z * fa + b->z * fb;
   const float w = a->w * fa + b->w * fb;
   const float lengthRcp = XrRcpSqrt(x * x + y * y + z * z + w * w);
   result->x = x * lengthRcp;
   result->y = y * lengthRcp;
   result->z = z * lengthRcp;
   result->w = w * lengthRcp;
}

inline static void XrPosef_ToMatrix3D(Matrix3D* result, const XrPosef* pose)
{
   const bx::Quaternion orientation(pose->orientation.x, pose->orientation.y, pose->orientation.z, pose->orientation.w);
   const bx::Quaternion invertOrientation = bx::conjugate(orientation);
   bx::mtxFromQuaternion(&result->m[0][0], invertOrientation);
   result->Transpose();
   const bx::Quaternion position(pose->position.x, pose->position.y, pose->position.z, 0.f);
   bx::Quaternion invertPosition = bx::conjugate(position);
   invertPosition = bx::mul(invertPosition, orientation);
   invertPosition = bx::mul(invertOrientation, invertPosition);
   result->m[3][0] += invertPosition.x;
   result->m[3][1] += invertPosition.y;
   result->m[3][2] += invertPosition.z;
}


#ifdef XR_USE_GRAPHICS_API_D3D11
#include "XRD3D11Backend.h"
#endif

#ifdef XR_USE_GRAPHICS_API_D3D12
#include "XRD3D12Backend.h"
#endif

#ifdef XR_USE_GRAPHICS_API_VULKAN
#include "XRVulkanBackend.h"
#endif

#endif



#if defined(ENABLE_XR)
bgfx::RendererType::Enum VRDevice::SelectRendererType(const Settings& settings)
{
   // VRDevice is created before bgfx initialization (since it creates the graphic context expected by OpenXR), so bgfx::getRendererType() is not defined at this point.
   // Renderer is determined at compile time based on platform: D3D11 for Windows, Vulkan for Android.
   #if BX_PLATFORM_WINDOWS && !defined(__STANDALONE__)
      const string gfxBackend = settings.GetPlayer_GfxBackend();
      if (gfxBackend == "Vulkan"sv)
      #ifdef _DEBUG
         return bgfx::RendererType::Enum::Vulkan;
      #else
      {
         PLOGI << "Renderer backend enforced to Direct3D11 as Vulkan is still experimental and not enabled in release builds";
         return bgfx::RendererType::Enum::Direct3D11;
      }
      #endif
      else if (gfxBackend == "Direct3D12"sv)
         return bgfx::RendererType::Enum::Direct3D12;
      else
         return bgfx::RendererType::Enum::Direct3D11; // Default to Direct3D 11
   #elif BX_PLATFORM_ANDROID || BX_PLATFORM_LINUX || BX_PLATFORM_WINDOWS // Standalone Windows build included, to render like the headsets
      return bgfx::RendererType::Enum::Vulkan;
   #else
      #error "Unsupported platform for OpenXR"
   #endif
}
#endif

void VRDevice::ApplyTableSettings(const Settings& settings)
{
   // Scene offset (vertical rotation and horizontal shift)
   m_orientation = settings.GetPlayerVR_Orientation();
   m_tablePos.x = settings.GetPlayerVR_TableX();
   m_tablePos.y = settings.GetPlayerVR_TableY();
   // Offset of the playfield from the room ground is defined as an offset from the lockbar, minus bottom glass height and custom adjustment
   m_tablePos.z = settings.GetPlayerVR_TableZ();
   m_worldDirty = true;

   #if defined(ENABLE_XR)
      // Relative scale factor and positioning
      m_lockbarWidth = settings.GetPlayer_LockbarWidth();
      m_lockbarHeight = settings.GetPlayer_LockbarHeight();
      m_lockFeetToGround = settings.GetPlayerVR_LockFeetToGround();

      m_showControllers = settings.GetPlayerVR_ShowControllers();
      m_displayRefreshRateMode = settings.GetPlayerVR_DisplayRefreshRate();
      m_foveationMode = settings.GetPlayerVR_Foveation();
      m_foveationEyeTracked = settings.GetPlayerVR_FoveationEyeTracked();
      m_foveationFlipX = settings.GetPlayerVR_FoveationFlipX();
      m_foveationFlipY = settings.GetPlayerVR_FoveationFlipY();
      m_dynamicResolution = settings.GetPlayerVR_DynamicResolution();
      m_dynamicResolutionTarget = clamp(settings.GetPlayerVR_DynamicResolutionTarget(), 0.5f, 1.f);
      m_dynamicResolutionMinScale = clamp(settings.GetPlayerVR_DynamicResolutionMinScale(), 0.5f, 1.f);
      m_dynamicRenderScale = 1.f; // Each table starts at full size
      m_frameBudgetMs = 0.f; // Learned again from the session's frame periods

      // State of the previous table
      m_headsetViewCentering = false;
      m_controllerViewCentering = false;
      m_uiPanelPlaced = false;
      m_tableCaptureView = TableCaptureView::None;
      m_tableCaptureBounds.clear();

      if (IsOpenXRHMDReady())
         UpdateEyeResolution(settings);
   #endif
}

VRDevice::VRDevice(const Settings& settings)
{
   #if defined(ENABLE_XR)
      // Fill out an XrApplicationInfo structure detailing the names and OpenXR version.
      // The application/engine name and version are user-defined. These may help IHVs or runtimes.
      XrApplicationInfo AI;
      strncpy_s(AI.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "Visual Pinball X");
      constexpr uint32_t ver = (VP_VERSION_MAJOR / 10) << 12 | (VP_VERSION_MAJOR % 10) << 8 | (VP_VERSION_MINOR) << 4 | VP_VERSION_REV; // e.g. 0x1081 for 10.8.1
      AI.applicationVersion = ver;
      strncpy_s(AI.engineName, XR_MAX_ENGINE_NAME_SIZE, "");
      AI.engineVersion = 0;
      AI.apiVersion = XR_MAKE_VERSION(1, 0, 0); //XR_CURRENT_API_VERSION;

      // Get all the API Layers from the OpenXR runtime.
      uint32_t apiLayerCount = 0;
      std::vector<XrApiLayerProperties> apiLayerProperties;
      OPENXR_CHECK(xrEnumerateApiLayerProperties(0, &apiLayerCount, nullptr), "Failed to enumerate ApiLayerProperties.");
      apiLayerProperties.resize(apiLayerCount, { XR_TYPE_API_LAYER_PROPERTIES });
      OPENXR_CHECK(xrEnumerateApiLayerProperties(apiLayerCount, &apiLayerCount, apiLayerProperties.data()), "Failed to enumerate ApiLayerProperties.");

      // Check the requested API layers against the ones from the OpenXR. If found add it to the Active API Layers.
      m_activeAPILayers.clear();
      for (const auto& requestLayer : m_apiLayers)
      {
         for (const auto& layerProperty : apiLayerProperties)
         {
            if (requestLayer == layerProperty.layerName)
            {
               m_activeAPILayers.push_back(requestLayer.c_str());
               break;
            }
         }
      }

      // Get all the Instance Extensions from the OpenXR instance.
      uint32_t extensionCount = 0;
      std::vector<XrExtensionProperties> extensionProperties;
      OPENXR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr), "Failed to enumerate InstanceExtensionProperties.");
      extensionProperties.resize(extensionCount, { XR_TYPE_EXTENSION_PROPERTIES });
      OPENXR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensionProperties.data()), "Failed to enumerate InstanceExtensionProperties.");
      #ifdef DEBUG
         for (const auto& extensionProperty : extensionProperties)
         {
            PLOGD << "OpenXR supported extension: " << extensionProperty.extensionName << ", version " << extensionProperty.extensionVersion;
         }
      #endif
      m_activeInstanceExtensions.clear();
      // Add a specific extension to the list of extensions to be enabled, if it is supported.
      auto EnableExtensionIfSupported = [&](const char* extensionName)
      {
         for (uint32_t i = 0; i < extensionCount; i++)
         {
            if (strcmp(extensionProperties[i].extensionName, extensionName) == 0)
            {
               m_activeInstanceExtensions.push_back(extensionName);
               return true;
            }
         }
         return false;
      };
      m_rendererType = SelectRendererType(settings);
      bool hasGraphicBackend = false;
      switch (m_rendererType)
      {
      #ifdef XR_USE_GRAPHICS_API_VULKAN
         case bgfx::RendererType::Enum::Vulkan:
            // According to https://github.khronos.org/OpenXR-Inventory/runtime_extension_support.html all runtimes that support XR_KHR_VULKAN_ENABLE_EXTENSION_NAME do support XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME
            hasGraphicBackend = EnableExtensionIfSupported(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
            break;
      #endif
      #ifdef XR_USE_GRAPHICS_API_D3D11
         case bgfx::RendererType::Enum::Direct3D11: hasGraphicBackend = EnableExtensionIfSupported(XR_KHR_D3D11_ENABLE_EXTENSION_NAME); break;
      #endif
      #ifdef XR_USE_GRAPHICS_API_D3D12
         case bgfx::RendererType::Enum::Direct3D12: hasGraphicBackend = EnableExtensionIfSupported(XR_KHR_D3D12_ENABLE_EXTENSION_NAME); break;
      #endif
      }
      assert(hasGraphicBackend);
      if (!hasGraphicBackend)
         return;

      m_depthExtensionSupported = EnableExtensionIfSupported(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
      m_colorSpaceExtensionSupported = EnableExtensionIfSupported(XR_FB_COLOR_SPACE_EXTENSION_NAME);
      m_visibilityMaskExtensionSupported = EnableExtensionIfSupported(XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
      #if BX_PLATFORM_WINDOWS
         m_win32PerfCounterExtensionSupported = EnableExtensionIfSupported(XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME);
      #elif BX_PLATFORM_ANDROID || BX_PLATFORM_LINUX
         m_convertTimespecTimeExtensionSupported = EnableExtensionIfSupported(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
      #endif
      m_passthroughExtensionSupported = EnableExtensionIfSupported(XR_FB_PASSTHROUGH_EXTENSION_NAME);
      m_displayRefreshRateExtensionSupported = EnableExtensionIfSupported(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
      // Needed to bind the Valve Frame controller interaction profile (without it, the runtime emulates an Oculus Touch controller which lacks the d-pad, view and bumper inputs)
      EnableExtensionIfSupported("XR_VALVE_frame_controller_interaction");
      // Controller models of the runtime (see GetControllerModels). XR_EXT_render_model needs XR_EXT_uuid since the instance asks for OpenXR 1.0
      #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
      {
         bool supported = false;
         for (uint32_t i = 0; i < extensionCount; i++)
            if (strcmp(extensionProperties[i].extensionName, XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME) == 0)
               supported = true;
         m_renderModelExtensionSupported = supported && EnableExtensionIfSupported(XR_EXT_UUID_EXTENSION_NAME) && EnableExtensionIfSupported(XR_EXT_RENDER_MODEL_EXTENSION_NAME)
            && EnableExtensionIfSupported(XR_EXT_INTERACTION_RENDER_MODEL_EXTENSION_NAME);
      }
      #endif
      // Foveated rendering by the runtime: the profile needs XR_FB_foveation and XR_FB_foveation_configuration, applying it to the swapchain needs
      // XR_FB_swapchain_update_state, and the runtime only builds Vulkan density maps with XR_FB_foveation_vulkan
      m_foveationExtensionSupported = EnableExtensionIfSupported(XR_FB_FOVEATION_EXTENSION_NAME) && EnableExtensionIfSupported(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME)
         && EnableExtensionIfSupported(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
      #ifdef XR_USE_GRAPHICS_API_VULKAN
      if (m_rendererType == bgfx::RendererType::Enum::Vulkan)
         m_foveationExtensionSupported = EnableExtensionIfSupported(XR_FB_FOVEATION_VULKAN_EXTENSION_NAME) && m_foveationExtensionSupported;
      #endif
      m_foveationEyeTrackedExtensionSupported = m_foveationExtensionSupported && EnableExtensionIfSupported(XR_META_FOVEATION_EYE_TRACKED_EXTENSION_NAME);
      m_eyeGazeExtensionSupported = EnableExtensionIfSupported(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
      m_performanceMetricsExtensionSupported = EnableExtensionIfSupported(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
      #ifdef DEBUG
         m_debugUtilsExtensionSupported = EnableExtensionIfSupported(XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
      #endif

      // Fill out an XrInstanceCreateInfo structure and create an XrInstance.
      XrInstanceCreateInfo instanceCI { XR_TYPE_INSTANCE_CREATE_INFO };
      instanceCI.createFlags = 0;
      instanceCI.applicationInfo = AI;
      instanceCI.enabledApiLayerCount = static_cast<uint32_t>(m_activeAPILayers.size());
      instanceCI.enabledApiLayerNames = m_activeAPILayers.data();
      instanceCI.enabledExtensionCount = static_cast<uint32_t>(m_activeInstanceExtensions.size());
      instanceCI.enabledExtensionNames = m_activeInstanceExtensions.data();
      OPENXR_CHECK(xrCreateInstance(&instanceCI, &m_xrInstance), "Failed to create Instance.");
      if (m_xrInstance == XR_NULL_HANDLE)
      {
         assert(false);
         return;
      }

      #if BX_PLATFORM_WINDOWS
      if (m_win32PerfCounterExtensionSupported)
      {
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrConvertTimeToWin32PerformanceCounterKHR", (PFN_xrVoidFunction*)&m_xrConvertTimeToWin32PerformanceCounterKHR),
            "Failed to get xrConvertTimeToWin32PerformanceCounterKHR.");
      }
      #elif BX_PLATFORM_ANDROID || BX_PLATFORM_LINUX
      if (m_convertTimespecTimeExtensionSupported)
      {
         OPENXR_CHECK(
            xrGetInstanceProcAddr(m_xrInstance, "xrConvertTimeToTimespecTimeKHR", (PFN_xrVoidFunction*)&m_xrConvertTimeToTimespecTimeKHR),
            "Failed to get xrConvertTimeToTimespecTimeKHR.");
      }
      #endif
      if (m_displayRefreshRateExtensionSupported)
      {
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&m_xrGetDisplayRefreshRateFB), "Failed to get xrGetDisplayRefreshRateFB.");
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&m_xrRequestDisplayRefreshRateFB), "Failed to get xrRequestDisplayRefreshRateFB.");
      }
      if (m_visibilityMaskExtensionSupported)
      {
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrGetVisibilityMaskKHR", (PFN_xrVoidFunction*)&xrGetVisibilityMaskKHR), "Failed to get xrGetVisibilityMaskKHR.");
      }
      if (m_foveationExtensionSupported)
      {
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrCreateFoveationProfileFB", (PFN_xrVoidFunction*)&m_xrCreateFoveationProfileFB), "Failed to get xrCreateFoveationProfileFB.");
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrDestroyFoveationProfileFB", (PFN_xrVoidFunction*)&m_xrDestroyFoveationProfileFB), "Failed to get xrDestroyFoveationProfileFB.");
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrUpdateSwapchainFB", (PFN_xrVoidFunction*)&m_xrUpdateSwapchainFB), "Failed to get xrUpdateSwapchainFB.");
      }
      if (m_foveationEyeTrackedExtensionSupported)
      {
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrGetFoveationEyeTrackedStateMETA", (PFN_xrVoidFunction*)&m_xrGetFoveationEyeTrackedStateMETA), "Failed to get xrGetFoveationEyeTrackedStateMETA.");
      }
      if (m_performanceMetricsExtensionSupported)
      {
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrEnumeratePerformanceMetricsCounterPathsMETA", (PFN_xrVoidFunction*)&m_xrEnumeratePerformanceMetricsCounterPathsMETA), "Failed to get xrEnumeratePerformanceMetricsCounterPathsMETA.");
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrSetPerformanceMetricsStateMETA", (PFN_xrVoidFunction*)&m_xrSetPerformanceMetricsStateMETA), "Failed to get xrSetPerformanceMetricsStateMETA.");
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrQueryPerformanceMetricsCounterMETA", (PFN_xrVoidFunction*)&m_xrQueryPerformanceMetricsCounterMETA), "Failed to get xrQueryPerformanceMetricsCounterMETA.");
      }
      #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
      if (m_renderModelExtensionSupported)
      {
         const auto getProc = [this](const char* name, auto& fn)
         {
            if (!XR_SUCCEEDED(xrGetInstanceProcAddr(m_xrInstance, name, reinterpret_cast<PFN_xrVoidFunction*>(&fn))) || fn == nullptr)
            {
               PLOGE << "OpenXR: failed to get " << name;
               m_renderModelExtensionSupported = false;
            }
         };
         getProc("xrCreateRenderModelEXT", m_xrCreateRenderModelEXT);
         getProc("xrDestroyRenderModelEXT", m_xrDestroyRenderModelEXT);
         getProc("xrGetRenderModelPropertiesEXT", m_xrGetRenderModelPropertiesEXT);
         getProc("xrCreateRenderModelSpaceEXT", m_xrCreateRenderModelSpaceEXT);
         getProc("xrCreateRenderModelAssetEXT", m_xrCreateRenderModelAssetEXT);
         getProc("xrDestroyRenderModelAssetEXT", m_xrDestroyRenderModelAssetEXT);
         getProc("xrGetRenderModelAssetDataEXT", m_xrGetRenderModelAssetDataEXT);
         getProc("xrGetRenderModelAssetPropertiesEXT", m_xrGetRenderModelAssetPropertiesEXT);
         getProc("xrGetRenderModelStateEXT", m_xrGetRenderModelStateEXT);
         getProc("xrEnumerateInteractionRenderModelIdsEXT", m_xrEnumerateInteractionRenderModelIdsEXT);
      }
      #endif
      PLOGI << "OpenXR controller models: " << (m_renderModelExtensionSupported ? "supported by the runtime" : "not supported by the runtime");
      PLOGI << "OpenXR foveated rendering: " << (m_foveationExtensionSupported ? "supported by the runtime" : "not supported by the runtime") << ", eye-tracked: "
            << (m_foveationEyeTrackedExtensionSupported ? "extension present" : "no extension");
      if (m_debugUtilsExtensionSupported)
      {
         // Fill out a XrDebugUtilsMessengerCreateInfoEXT structure specifying all severities and types.
         // Set the userCallback to OpenXRMessageCallbackFunction().
         XrDebugUtilsMessengerCreateInfoEXT debugUtilsMessengerCI { XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
         debugUtilsMessengerCI.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
         debugUtilsMessengerCI.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
         debugUtilsMessengerCI.userCallback = (PFN_xrDebugUtilsMessengerCallbackEXT)OpenXRMessageCallbackFunction;
         debugUtilsMessengerCI.userData = nullptr;

         // Load xrCreateDebugUtilsMessengerEXT() function pointer as it is not default loaded by the OpenXR loader.
         PFN_xrCreateDebugUtilsMessengerEXT xrCreateDebugUtilsMessengerEXT;
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrCreateDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)&xrCreateDebugUtilsMessengerEXT), "Failed to get InstanceProcAddr.");

         // Finally create and return the XrDebugUtilsMessengerEXT.
         OPENXR_CHECK(xrCreateDebugUtilsMessengerEXT(m_xrInstance, &debugUtilsMessengerCI, &m_debugUtilsMessenger), "Failed to create DebugUtilsMessenger.");
      }

      // Get the instance's properties and log the runtime name and version.
      XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
      OPENXR_CHECK(xrGetInstanceProperties(m_xrInstance, &instanceProperties), "Failed to get InstanceProperties.");
      PLOGI << "OpenXR Runtime: " << instanceProperties.runtimeName << " - "
                                 << XR_VERSION_MAJOR(instanceProperties.runtimeVersion) << '.'
                                 << XR_VERSION_MINOR(instanceProperties.runtimeVersion) << '.'
                                 << XR_VERSION_PATCH(instanceProperties.runtimeVersion);
   #endif
}

VRDevice::~VRDevice()
{
   #if defined(ENABLE_XR)
      // The session is released by the render thread before BGFX is shut down (see ReleaseSession), and the device is only destroyed after it
      if (m_session != XR_NULL_HANDLE)
      {
         PLOGE << "OpenXR session still alive when destroying the VR device";
         OPENXR_CHECK(xrDestroySession(m_session), "Failed to destroy Session.");
         m_session = XR_NULL_HANDLE;
      }

      // The graphics device (given to BGFX, which never destroys a device it did not create) is destroyed with the backend
      m_backend = nullptr;

      if (m_debugUtilsExtensionSupported)
      {
         PFN_xrDestroyDebugUtilsMessengerEXT xrDestroyDebugUtilsMessengerEXT;
         OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrDestroyDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)&xrDestroyDebugUtilsMessengerEXT), "Failed to get InstanceProcAddr.");
         OPENXR_CHECK(xrDestroyDebugUtilsMessengerEXT(m_debugUtilsMessenger), "Failed to destroy DebugUtilsMessenger.");
      }

      // Destroy the XrInstance.
      OPENXR_CHECK(xrDestroyInstance(m_xrInstance), "Failed to destroy Instance.");
   #endif
}

#ifdef ENABLE_XR

XrBool32 VRDevice::OpenXRMessageCallbackFunction(XrDebugUtilsMessageSeverityFlagsEXT messageSeverity, XrDebugUtilsMessageTypeFlagsEXT messageType, const XrDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData)
{
   // Lambda to covert an XrDebugUtilsMessageSeverityFlagsEXT to std::string. Bitwise check to concatenate multiple severities to the output string.
   auto GetMessageSeverityString = [](XrDebugUtilsMessageSeverityFlagsEXT messageSeverity) -> std::string
   {
      bool separator = false;

      std::string msgFlags;
      if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT)
      {
         msgFlags += "VERBOSE"sv;
         separator = true;
      }
      if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
      {
         if (separator)
            msgFlags += ',';
         msgFlags += "INFO"sv;
         separator = true;
      }
      if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
      {
         if (separator)
            msgFlags += ',';
         msgFlags += "WARN"sv;
         separator = true;
      }
      if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
      {
         if (separator)
            msgFlags += ',';
         msgFlags += "ERROR"sv;
      }
      return msgFlags;
   };
   // Lambda to covert an XrDebugUtilsMessageTypeFlagsEXT to std::string. Bitwise check to concatenate multiple types to the output string.
   auto GetMessageTypeString = [](XrDebugUtilsMessageTypeFlagsEXT messageType) -> std::string
   {
      bool separator = false;

      std::string msgFlags;
      if (messageType & XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT)
      {
         msgFlags += "GEN"sv;
         separator = true;
      }
      if (messageType & XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT)
      {
         if (separator)
            msgFlags += ',';
         msgFlags += "SPEC"sv;
         separator = true;
      }
      if (messageType & XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT)
      {
         if (separator)
            msgFlags += ',';
         msgFlags += "PERF"sv;
      }
      return msgFlags;
   };

   std::string functionName = (pCallbackData->functionName) ? pCallbackData->functionName : "";
   std::string messageSeverityStr = GetMessageSeverityString(messageSeverity);
   std::string messageTypeStr = GetMessageTypeString(messageType);
   std::string messageId = (pCallbackData->messageId) ? pCallbackData->messageId : "";
   std::string message = (pCallbackData->message) ? pCallbackData->message : "";
   std::stringstream errorMessage;
   errorMessage << functionName << '(' << messageSeverityStr << " / " << messageTypeStr << "): msgNum: " << messageId << " - " << message;
   if (messageSeverity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
   {
      PLOGE << errorMessage.str();
      // assert(false);
   }
   else
   {
      PLOGI << errorMessage.str();
   }
   return XrBool32();
}

void* VRDevice::GetGraphicContext() const { return m_backend->GetGraphicContext(); }

bgfx::RendererType::Enum VRDevice::GetGraphicContextType() const { return m_backend->GetRendererType(); }

void VRDevice::SetupHMD()
{
   assert(m_backend == nullptr);
   assert(m_xrInstance != XR_NULL_HANDLE);

   // Get the XrSystemId from the instance and the supplied XrFormFactor.
   XrSystemGetInfo systemGI{XR_TYPE_SYSTEM_GET_INFO};
   systemGI.formFactor = m_formFactor;
   OPENXR_CHECK(xrGetSystem(m_xrInstance, &systemGI, &m_systemID), "Failed to get SystemID.");
   // if failed with XR_ERROR_FORM_FACTOR_UNAVAILABLE, then the headset is just not detected, we delay/retry
   // FIXME handle other error return codes
   if (m_systemID == XR_NULL_SYSTEM_ID)
      return;

   // Get the System's properties for some general information about the hardware and the vendor.
   XrSystemColorSpacePropertiesFB colorSpaceProperties { XR_TYPE_SYSTEM_COLOR_SPACE_PROPERTIES_FB };
   XrSystemFoveationEyeTrackedPropertiesMETA foveationEyeTrackedProperties { XR_TYPE_SYSTEM_FOVEATION_EYE_TRACKED_PROPERTIES_META };
   XrSystemEyeGazeInteractionPropertiesEXT eyeGazeProperties { XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT };
   void** next = &m_systemProperties.next;
   if (m_eyeGazeExtensionSupported)
   {
      *next = &eyeGazeProperties;
      next = &eyeGazeProperties.next;
   }
   if (m_colorSpaceExtensionSupported)
   {
      *next = &colorSpaceProperties;
      next = &colorSpaceProperties.next;
   }
   if (m_foveationEyeTrackedExtensionSupported)
   {
      *next = &foveationEyeTrackedProperties;
      next = &foveationEyeTrackedProperties.next;
   }
   OPENXR_CHECK(xrGetSystemProperties(m_xrInstance, m_systemID, &m_systemProperties), "Failed to get SystemProperties.");
   m_systemProperties.next = nullptr;
   m_foveationEyeTrackedSystemSupported = m_foveationEyeTrackedExtensionSupported && foveationEyeTrackedProperties.supportsFoveationEyeTracked;
   m_eyeGazeSystemSupported = m_eyeGazeExtensionSupported && eyeGazeProperties.supportsEyeGazeInteraction;
   if (m_eyeGazeExtensionSupported)
      PLOGI << "Eye gaze interaction " << (m_eyeGazeSystemSupported ? "supported" : "not supported") << " by this headset";
   if (m_foveationEyeTrackedExtensionSupported)
      PLOGI << "Eye-tracked foveation " << (m_foveationEyeTrackedSystemSupported ? "supported" : "not supported") << " by this headset";
   if (m_colorSpaceExtensionSupported)
   {
      PLOGI << "Native XR device colorspace: " << colorSpaceProperties.colorSpace;

      // Set color space to get the same rendering on all HMD and usual desktop play
      PFN_xrEnumerateColorSpacesFB xrEnumerateColorSpacesFB;
      OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrEnumerateColorSpacesFB", (PFN_xrVoidFunction*)&xrEnumerateColorSpacesFB), "Failed to get xrEnumerateColorSpacesFB.");
      PFN_xrSetColorSpaceFB xrSetColorSpaceFB;
      OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrSetColorSpaceFB", (PFN_xrVoidFunction*)&xrSetColorSpaceFB), "Failed to get xrSetColorSpaceFB.");
      uint32_t colorSpaceCount;
      xrEnumerateColorSpacesFB(m_session, 0, &colorSpaceCount, nullptr);
      XrColorSpaceFB* colorSpaces = new XrColorSpaceFB[colorSpaceCount];
      xrEnumerateColorSpacesFB(m_session, colorSpaceCount, &colorSpaceCount, colorSpaces);
      for (uint32_t i = 0; i < colorSpaceCount; i++)
      {
         if (colorSpaces[i] == XR_COLOR_SPACE_REC709_FB)
         {
            xrSetColorSpaceFB(m_session, XR_COLOR_SPACE_REC709_FB);
            break;
         }
      }
   }

   // Gets the View Configuration Types. The first call gets the count of the array that will be returned. The next call fills out the array.
   uint32_t viewConfigurationCount = 0;
   OPENXR_CHECK(xrEnumerateViewConfigurations(m_xrInstance, m_systemID, 0, &viewConfigurationCount, nullptr), "Failed to enumerate View Configurations.");
   m_viewConfigurations.resize(viewConfigurationCount);
   OPENXR_CHECK(xrEnumerateViewConfigurations(m_xrInstance, m_systemID, viewConfigurationCount, &viewConfigurationCount, m_viewConfigurations.data()), "Failed to enumerate View Configurations.");

   // Pick the first application supported View Configuration Type con supported by the hardware.
   for (const XrViewConfigurationType& viewConfiguration : m_applicationViewConfigurations)
   {
      if (std::ranges::find(m_viewConfigurations.begin(), m_viewConfigurations.end(), viewConfiguration) != m_viewConfigurations.end())
      {
         m_viewConfiguration = viewConfiguration;
         break;
      }
   }
   if (m_viewConfiguration == XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM)
   {
      PLOGE << "Failed to find a view configuration type. Defaulting to XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO.";
      m_viewConfiguration = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
   }

   // Gets the View Configuration Views. The first call gets the count of the array that will be returned. The next call fills out the array.
   uint32_t viewConfigurationViewCount = 0;
   OPENXR_CHECK(xrEnumerateViewConfigurationViews(m_xrInstance, m_systemID, m_viewConfiguration, 0, &viewConfigurationViewCount, nullptr), "Failed to enumerate ViewConfiguration Views.");
   m_viewConfigurationViews.resize(viewConfigurationViewCount, { XR_TYPE_VIEW_CONFIGURATION_VIEW });
   OPENXR_CHECK(xrEnumerateViewConfigurationViews(m_xrInstance, m_systemID, m_viewConfiguration, viewConfigurationViewCount, &viewConfigurationViewCount, m_viewConfigurationViews.data()), "Failed to enumerate ViewConfiguration Views.");

   // Retrieves the available blend modes. The first call gets the count of the array that will be returned. The next call fills out the array.
   uint32_t environmentBlendModeCount = 0;
   OPENXR_CHECK(xrEnumerateEnvironmentBlendModes(m_xrInstance, m_systemID, m_viewConfiguration, 0, &environmentBlendModeCount, nullptr), "Failed to enumerate EnvironmentBlend Modes.");
   m_environmentBlendModes.resize(environmentBlendModeCount);
   OPENXR_CHECK(xrEnumerateEnvironmentBlendModes(m_xrInstance, m_systemID, m_viewConfiguration, environmentBlendModeCount, &environmentBlendModeCount, m_environmentBlendModes.data()), "Failed to enumerate EnvironmentBlend Modes.");
   #ifdef DEBUG
      for (const auto environmentBlendMode : m_environmentBlendModes)
      {
         static const char* blendModeNames[] = { "Opaque", "Additive", "Alpha" };
         PLOGD << "OpenXR supported blend mode: " << (1 <= environmentBlendMode && environmentBlendMode < 4 ? blendModeNames[environmentBlendMode - 1] : std::to_string(environmentBlendMode).c_str());
      }
   #endif
   // Pick the first application supported blend mode supported by the hardware.
   for (const XrEnvironmentBlendMode& environmentBlendMode : m_applicationEnvironmentBlendModes)
   {
      if (std::ranges::find(m_environmentBlendModes.begin(), m_environmentBlendModes.end(), environmentBlendMode) != m_environmentBlendModes.end())
      {
         m_environmentBlendMode = environmentBlendMode;
         break;
      }
   }
   if (m_environmentBlendMode == XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM)
   {
      PLOGE << "Failed to find a compatible blend mode. Defaulting to XR_ENVIRONMENT_BLEND_MODE_OPAQUE.";
      m_environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
   }

   // Since we are using texture array rendering, we need target to be stereo views with the same setup
   assert(m_viewConfigurationViews.size() == 2);
   assert(m_viewConfigurationViews[0].recommendedImageRectWidth == m_viewConfigurationViews[1].recommendedImageRectWidth);
   assert(m_viewConfigurationViews[0].recommendedImageRectHeight == m_viewConfigurationViews[1].recommendedImageRectHeight);
   assert(m_viewConfigurationViews[0].recommendedSwapchainSampleCount == m_viewConfigurationViews[1].recommendedSwapchainSampleCount);

   PLOGI << "Headset recommended resolution: " << m_viewConfigurationViews[0].recommendedImageRectWidth << 'x' << m_viewConfigurationViews[0].recommendedImageRectHeight;
   PLOGI << "Headset maximum resolution: " << m_viewConfigurationViews[0].maxImageRectWidth << 'x' << m_viewConfigurationViews[0].maxImageRectHeight;

   // Create graphics backend early so GetGraphicContext() can provide Vulkan handles to BGFX
   #if BX_PLATFORM_WINDOWS || BX_PLATFORM_ANDROID || BX_PLATFORM_LINUX
   if (m_rendererType == bgfx::RendererType::Vulkan)
   {
      PLOGI << "Creating Vulkan backend for OpenXR (before BGFX initialization)";
      m_backend = std::make_unique<XRVulkanBackend>(m_xrInstance, m_systemID);
   }
   #endif
   #if BX_PLATFORM_WINDOWS && !defined(__STANDALONE__)
   if (m_rendererType == bgfx::RendererType::Direct3D11)
   {
      PLOGI << "Creating DX11 backend for OpenXR (before BGFX initialization)";
      m_backend = std::make_unique<XRD3D11Backend>(m_xrInstance, m_systemID);
   }
   if (m_rendererType == bgfx::RendererType::Direct3D12)
   {
      PLOGI << "Creating DX12 backend for OpenXR (before BGFX initialization)";
      m_backend = std::make_unique<XRD3D12Backend>(m_xrInstance, m_systemID);
   }
   #endif

   assert(m_backend != nullptr);
}

// Resolution of the eye images (and of the swapchains, created with each session), from the resolution factor of the table settings
void VRDevice::UpdateEyeResolution(const Settings& settings)
{
   assert(IsOpenXRHMDReady() && !m_viewConfigurationViews.empty());

   // Let the user choose the down/super sampling
   const float resFactor = settings.GetPlayerVR_ResFactor();
   if (resFactor <= 0.1f || resFactor > 10.f)
   {
      m_eyeWidth = m_viewConfigurationViews[0].recommendedImageRectWidth;
      m_eyeHeight = m_viewConfigurationViews[0].recommendedImageRectHeight;
   }
   else
   {
      m_eyeWidth = static_cast<unsigned int>((float)m_viewConfigurationViews[0].maxImageRectWidth * resFactor);
      m_eyeHeight = static_cast<unsigned int>((float)m_viewConfigurationViews[0].maxImageRectHeight * resFactor);
   }

   // Limit to OpenXR declared limits
   const uint32_t maxWidth = std::min(m_viewConfigurationViews[0].maxImageRectWidth, m_systemProperties.graphicsProperties.maxSwapchainImageWidth);
   const uint32_t maxHeight = std::min(m_viewConfigurationViews[0].maxImageRectHeight, m_systemProperties.graphicsProperties.maxSwapchainImageHeight);
   if (m_eyeWidth == 0 || m_eyeHeight == 0 || m_eyeWidth > maxWidth || m_eyeHeight > maxHeight)
   {
      PLOGI << "Requested resolution exceeds OpenXR swapchain limits, defaulting to headset recommended resolution";
      m_eyeWidth = m_viewConfigurationViews[0].recommendedImageRectWidth;
      m_eyeHeight = m_viewConfigurationViews[0].recommendedImageRectHeight;
   }

   // Limit to a resolution, under the maximum texture size supported by the GPU
   // This is called before BGFX is initialized (the graphics backend is given to BGFX when it is initialized), so the caps are all zero for the
   // first table, then they are the ones of the last BGFX initialization (same graphics device)
   const bgfx::Caps* caps = bgfx::getCaps();
   if (caps->limits.maxTextureSize != 0 && ((static_cast<uint32_t>(m_eyeWidth) >= caps->limits.maxTextureSize) || (static_cast<uint32_t>(m_eyeHeight) >= caps->limits.maxTextureSize)))
   {
      PLOGI << "Requested resolution exceed the GPU capability, defaulting to headset recommended resolution";
      m_eyeWidth = std::min(m_viewConfigurationViews[0].recommendedImageRectWidth, caps->limits.maxTextureSize);
      m_eyeHeight = std::min(m_viewConfigurationViews[0].recommendedImageRectHeight, caps->limits.maxTextureSize);
   }

   PLOGI << "Selected resolution: " << m_eyeWidth << 'x' << m_eyeHeight;
}

void VRDevice::SetDisplayRefreshRateMode(int mode)
{
   m_displayRefreshRateMode = mode;
   if (m_sessionRunning)
      ApplyDisplayRefreshRate();
}

void VRDevice::ApplyDisplayRefreshRate()
{
   if (!m_displayRefreshRateExtensionSupported)
      return;
   static constexpr float rates[] = { 0.f, 72.f, 80.f, 90.f, 120.f };
   const float requested = rates[clamp(m_displayRefreshRateMode, 0, static_cast<int>(std::size(rates)) - 1)];
   float rate = 0.f;
   if (m_xrGetDisplayRefreshRateFB(m_session, &rate) == XR_SUCCESS && rate > 0.f && g_pplayer && g_pplayer->m_playfieldWnd)
      g_pplayer->m_playfieldWnd->SetRefreshRate(rate);
   if (requested > 0.f && requested != rate)
   {
      const XrResult result = m_xrRequestDisplayRefreshRateFB(m_session, requested);
      if (result != XR_SUCCESS)
         PLOGW << "Headset refused refresh rate " << requested << " Hz (result: " << result << ')';
   }
}

void VRDevice::SetFoveationMode(int mode)
{
   m_foveationMode = clamp(mode, 0, 3);
   ApplyFoveation();
   FillOwnFoveationMap();
}

void VRDevice::SetFoveationEyeTracked(bool eyeTracked)
{
   m_foveationEyeTracked = eyeTracked;
   ApplyFoveation();
}

// Build the foveation profile for the current settings and apply it to the color swapchain. The runtime keeps the profile in use until the next update,
// so the previous one can be destroyed right after. Called when the swapchain is created and whenever the settings change.
void VRDevice::ApplyFoveation()
{
   if (!m_foveationExtensionSupported || m_colorSwapchainInfo.swapchain == XR_NULL_HANDLE)
      return;

   static constexpr XrFoveationLevelFB levels[] = { XR_FOVEATION_LEVEL_NONE_FB, XR_FOVEATION_LEVEL_LOW_FB, XR_FOVEATION_LEVEL_MEDIUM_FB, XR_FOVEATION_LEVEL_HIGH_FB };
   const bool eyeTracked = m_foveationEyeTracked && m_foveationEyeTrackedSystemSupported && m_foveationMode != 0;
   // When the scene is foveated with our own image (shading rate image or density map with offsets) the runtime's level is NONE: the
   // eye-tracked profile alone keeps the gaze coming, and the runtime has nothing to do with the images it hands us. The level is only
   // given to the runtime where its own density maps are the fallback.
   const bool ownFoveation = m_backend->IsFragmentShadingRateSupported() || m_backend->IsFragmentDensityMapOffsetSupported();

   XrFoveationEyeTrackedProfileCreateInfoMETA eyeTrackedInfo { XR_TYPE_FOVEATION_EYE_TRACKED_PROFILE_CREATE_INFO_META };
   eyeTrackedInfo.flags = 0;
   XrFoveationLevelProfileCreateInfoFB levelInfo { XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB };
   levelInfo.level = ownFoveation ? XR_FOVEATION_LEVEL_NONE_FB : levels[clamp(m_foveationMode, 0, 3)];
   levelInfo.verticalOffset = 0.f;
   levelInfo.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
   if (eyeTracked)
      levelInfo.next = &eyeTrackedInfo;
   XrFoveationProfileCreateInfoFB createInfo { XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB };
   createInfo.next = &levelInfo;

   XrFoveationProfileFB profile = XR_NULL_HANDLE;
   if (const XrResult result = m_xrCreateFoveationProfileFB(m_session, &createInfo, &profile); !XR_SUCCEEDED(result))
   {
      PLOGW << "Foveation profile refused by the runtime (result: " << result << ')';
      return;
   }
   XrSwapchainStateFoveationFB state { XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB };
   state.flags = 0;
   state.profile = profile;
   if (const XrResult result = m_xrUpdateSwapchainFB(m_colorSwapchainInfo.swapchain, reinterpret_cast<const XrSwapchainStateBaseHeaderFB*>(&state)); !XR_SUCCEEDED(result))
   {
      PLOGW << "Foveation profile not applied to the swapchain (result: " << result << ')';
      OPENXR_CHECK(m_xrDestroyFoveationProfileFB(profile), "Failed to destroy foveation profile");
      return;
   }
   if (m_foveationProfile != XR_NULL_HANDLE)
      OPENXR_CHECK(m_xrDestroyFoveationProfileFB(m_foveationProfile), "Failed to destroy foveation profile");
   m_foveationProfile = profile;
   m_foveationApplied = true;
   PLOGI << "Foveated rendering applied: level " << m_foveationMode << " (profile level " << static_cast<int>(levelInfo.level) << ')' << (eyeTracked ? ", eye-tracked" : ", fixed");
   UpdateFoveationState();
}

// Radius of the full quality area and of the half quality ring per level, as a fraction of the half width (about 55 degrees of field of view
// on the Steam Frame, so 0.2 is 11 degrees around the gaze, which eye tracking affords); beyond is quarter quality. With the area moved to
// the gaze, a tighter profile costs nothing in what is looked at: the levels differ by how much of the periphery is coarse.
static constexpr float kFoveationFullRadius[] = { 1.f, 0.45f, 0.30f, 0.20f };
static constexpr float kFoveationHalfRadius[] = { 1.f, 0.80f, 0.55f, 0.38f };
// Tighter rings for the shading rate image that follows the gaze: a single image serves both eyes, so each eye's disc is also shaded at full
// rate in the other eye's image, where it sits about 0.3 of the half width away (the eyes' asymmetric views); Low here is the fixed Medium
static constexpr float kGazeFullRadius[] = { 1.f, 0.30f, 0.20f, 0.15f };
static constexpr float kGazeHalfRadius[] = { 1.f, 0.55f, 0.38f, 0.28f };

// A map of 32 pixel texels (the minimum of the driver) covering the eye image, 8-bit density in R (x) and G (y) identical on both eyes.
// The hardware uses 1, 1/2 or 1/4 shading rate per axis, so the rings are flat values; the gaze offsets translate the whole map.
void VRDevice::CreateOwnFoveationMap()
{
   #ifdef BGFX_TEXTURE_FRAGMENT_DENSITY_MAP
   if (bgfx::isValid(m_ownFoveationMap) || !m_backend->IsFragmentDensityMapSupported() || !m_backend->IsFragmentDensityMapOffsetSupported())
      return;
   constexpr uint32_t texel = 32;
   m_ownFoveationMapWidth = (m_eyeWidth + texel - 1) / texel;
   m_ownFoveationMapHeight = (m_eyeHeight + texel - 1) / texel;
   const uint32_t layers = static_cast<uint32_t>(m_viewConfigurationViews.size());
   // Created without data: bgfx makes a texture created with data immutable and silently drops the updates FillOwnFoveationMap relies on
   m_ownFoveationMap = bgfx::createTexture2D(static_cast<uint16_t>(m_ownFoveationMapWidth), static_cast<uint16_t>(m_ownFoveationMapHeight), false, static_cast<uint16_t>(layers),
      bgfx::TextureFormat::RG8, BGFX_TEXTURE_FRAGMENT_DENSITY_MAP, nullptr);
   if (!bgfx::isValid(m_ownFoveationMap))
   {
      PLOGW << "Foveated rendering: failed to create the density map";
      return;
   }
   bgfx::setName(m_ownFoveationMap, "Foveation density map");
   PLOGI << "Foveated rendering: own density map of " << m_ownFoveationMapWidth << 'x' << m_ownFoveationMapHeight << ", " << layers << " layers, with gaze offsets";
   FillOwnFoveationMap();
   #endif
}

void VRDevice::FillOwnFoveationMap()
{
   #ifdef BGFX_TEXTURE_FRAGMENT_DENSITY_MAP
   if (!bgfx::isValid(m_ownFoveationMap))
      return;
   static constexpr uint8_t outerDensity[] = { 255, 64, 64, 64 };
   const int level = clamp(m_foveationMode, 0, 3);
   const float cx = 0.5f * static_cast<float>(m_ownFoveationMapWidth), cy = 0.5f * static_cast<float>(m_ownFoveationMapHeight);
   const uint32_t layers = static_cast<uint32_t>(m_viewConfigurationViews.size());
   for (uint32_t layer = 0; layer < layers; layer++)
   {
      const bgfx::Memory* mem = bgfx::alloc(m_ownFoveationMapWidth * m_ownFoveationMapHeight * 2);
      for (uint32_t y = 0; y < m_ownFoveationMapHeight; y++)
         for (uint32_t x = 0; x < m_ownFoveationMapWidth; x++)
         {
            const float dx = (static_cast<float>(x) + 0.5f - cx) / cx, dy = (static_cast<float>(y) + 0.5f - cy) / cy;
            const float r = sqrtf(dx * dx + dy * dy);
            const uint8_t density = r < kFoveationFullRadius[level] ? 255 : r < kFoveationHalfRadius[level] ? 128 : outerDensity[level];
            mem->data[(y * m_ownFoveationMapWidth + x) * 2 + 0] = density;
            mem->data[(y * m_ownFoveationMapWidth + x) * 2 + 1] = density;
         }
      bgfx::updateTexture2D(m_ownFoveationMap, static_cast<uint16_t>(layer), 0, 0, 0, static_cast<uint16_t>(m_ownFoveationMapWidth), static_cast<uint16_t>(m_ownFoveationMapHeight), mem);
   }
   #endif
}

// The gaze of each eye from the runtime (normalized, -1..1), valid when the headset tracks the eyes and the setting asks for it
// The gaze as a ray in the reference space (XR_EXT_eye_gaze_interaction), turned into the point each eye's image looks at for this frame.
// Head and eyes are taken at the gaze sample time, so the ray of a fixated point stays put while the head moves, and the point is projected
// with the views the frame is rendered with; the small vergence between the eyes comes from an assumed fixation distance. The point is
// smoothed with a short filter (the samples jitter), which a saccade resets.
void VRDevice::LocateGaze(const std::vector<XrView>& views, const XrTime displayTime)
{
   m_gazeRayValid = false;
   if (m_gazeSpace == XR_NULL_HANDLE || !m_foveationEyeTracked || m_foveationMode == 0 || views.size() < 2)
      return;
   // Runtimes may only vouch for the orientation of the gaze pose: the ray then starts between the eyes
   constexpr XrSpaceLocationFlags valid = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
   XrEyeGazeSampleTimeEXT sampleTime { XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT };
   XrSpaceLocation location { XR_TYPE_SPACE_LOCATION, &sampleTime };
   const XrResult locateResult = xrLocateSpace(m_gazeSpace, m_referenceSpace, displayTime, &location);
   if (locateResult != XR_SUCCESS || (location.locationFlags & valid) != valid)
   {
      static double nextLog = 0.;
      if (const double now = static_cast<double>(usec()) * 1e-6; now > nextLog)
      {
         nextLog = now + 5.;
         XrActionStateGetInfo getInfo { XR_TYPE_ACTION_STATE_GET_INFO };
         getInfo.action = m_xrInputHandler ? m_xrInputHandler->GetAction("/user/eyes_ext/input/gaze_ext/pose") : XR_NULL_HANDLE;
         XrActionStatePose poseState { XR_TYPE_ACTION_STATE_POSE };
         const XrResult stateResult = getInfo.action != XR_NULL_HANDLE ? xrGetActionStatePose(m_session, &getInfo, &poseState) : XR_ERROR_HANDLE_INVALID;
         XrPath eyesPath = XR_NULL_PATH;
         xrStringToPath(m_xrInstance, "/user/eyes_ext", &eyesPath);
         XrInteractionProfileState profile { XR_TYPE_INTERACTION_PROFILE_STATE };
         const XrResult profileResult = xrGetCurrentInteractionProfile(m_session, eyesPath, &profile);
         char profileName[XR_MAX_PATH_LENGTH] = "none";
         uint32_t length = 0;
         if (profileResult == XR_SUCCESS && profile.interactionProfile != XR_NULL_PATH)
            xrPathToString(m_xrInstance, profile.interactionProfile, sizeof(profileName), &length, profileName);
         PLOGW << "Eye gaze ray unavailable: locate result " << locateResult << ", flags 0x" << std::hex << location.locationFlags << std::dec << ", sample time " << sampleTime.time
               << ", action active " << (stateResult == XR_SUCCESS ? (poseState.isActive ? "yes" : "no") : "unknown") << " (result " << stateResult << "), profile " << profileName << " (result " << profileResult << ')';
      }
      return;
   }
   if (sampleTime.time != 0 && sampleTime.time < displayTime)
   {
      XrSpaceLocation atSample { XR_TYPE_SPACE_LOCATION };
      if (xrLocateSpace(m_gazeSpace, m_referenceSpace, sampleTime.time, &atSample) == XR_SUCCESS && (atSample.locationFlags & valid) == valid)
         location = atSample;
   }
   const bx::Quaternion orientation(location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z, location.pose.orientation.w);
   const bx::Vec3 dir = bx::mul(bx::Vec3(0.f, 0.f, -1.f), orientation);
   const bx::Vec3 origin = (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
      ? bx::Vec3(location.pose.position.x, location.pose.position.y, location.pose.position.z)
      : bx::Vec3(0.5f * (views[0].pose.position.x + views[1].pose.position.x), 0.5f * (views[0].pose.position.y + views[1].pose.position.y), 0.5f * (views[0].pose.position.z + views[1].pose.position.z));
   constexpr float fixationDistance = 1.f; // meters, about the table
   bx::Vec3 point(origin.x + dir.x * fixationDistance, origin.y + dir.y * fixationDistance, origin.z + dir.z * fixationDistance);
   if (m_gazePointValid)
   {
      constexpr float tau = 0.035f; // seconds
      constexpr float saccade = 0.08f; // meters, about 4.5 degrees at the fixation distance
      const bx::Vec3 previous(m_gazePoint.x, m_gazePoint.y, m_gazePoint.z);
      if (bx::length(bx::sub(point, previous)) < saccade)
      {
         const float dt = clamp(static_cast<float>(static_cast<double>(displayTime - m_gazePointTime) * 1e-9), 0.f, 0.1f);
         point = bx::lerp(previous, point, 1.f - expf(-dt / tau));
      }
   }
   m_gazePoint = { point.x, point.y, point.z };
   m_gazePointTime = displayTime;
   m_gazePointValid = true;
   XrVector2f centers[2];
   for (int eye = 0; eye < 2; eye++)
   {
      // Into the eye's view space (OpenXR: x right, y up, looking down -z)
      const XrPosef& eyePose = views[eye].pose;
      const bx::Quaternion eyeOrientation(eyePose.orientation.x, eyePose.orientation.y, eyePose.orientation.z, eyePose.orientation.w);
      const bx::Vec3 p = bx::mul(bx::sub(point, bx::Vec3(eyePose.position.x, eyePose.position.y, eyePose.position.z)), bx::conjugate(eyeOrientation));
      if (p.z >= -0.01f) // Behind the eye
      {
         static double nextLog = 0.;
         if (const double now = static_cast<double>(usec()) * 1e-6; now > nextLog)
         {
            nextLog = now + 5.;
            PLOGW << "Eye gaze ray behind eye " << eye << ": point " << point.x << ',' << point.y << ',' << point.z << " eye " << eyePose.position.x << ',' << eyePose.position.y << ',' << eyePose.position.z << " view " << p.x << ',' << p.y << ',' << p.z;
         }
         return;
      }
      const XrFovf& fov = views[eye].fov;
      const float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight), tanD = tanf(fov.angleDown), tanU = tanf(fov.angleUp);
      const float tx = p.x / -p.z, ty = p.y / -p.z;
      // Image space of the eye (-1..1, x to the right, y down), the convention of the runtime's foveation center
      centers[eye].x = (2.f * tx - (tanR + tanL)) / (tanR - tanL);
      centers[eye].y = -(2.f * ty - (tanU + tanD)) / (tanU - tanD);
   }
   m_foveationCenter[0] = centers[0];
   m_foveationCenter[1] = centers[1];
   m_gazeRayValid = true;
}

bool VRDevice::ReadGaze()
{
   if (m_gazeRayValid)
      return true;
   if (!m_foveationEyeTracked || !m_foveationEyeTrackedExtensionSupported || m_session == XR_NULL_HANDLE)
      return false;
   XrFoveationEyeTrackedStateMETA state { XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META };
   if (!XR_SUCCEEDED(m_xrGetFoveationEyeTrackedStateMETA(m_session, &state)) || (state.flags & XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META) == 0)
      return false;
   for (int eye = 0; eye < 2; eye++)
      m_foveationCenter[eye] = state.foveationCenter[eye];
   return true;
}

// The shading rate image for this frame: full rate inside a disc around each eye's gaze (one image serves both eyes, so the union of the
// two discs), 2x2 in a ring around it, 4x4 beyond, with the radii of the level. Without a valid gaze, the Low rings at the center of the
// view, wide enough for where a player looks. Sized in the pixels the frame renders (dynamic resolution renders a sub rectangle at the
// origin). The image is only uploaded when a disc moved by more than half a texel, or the level or scale changed.
bool VRDevice::UpdateShadingRateMap(bool gazeValid)
{
   #ifdef BGFX_TEXTURE_FRAGMENT_SHADING_RATE
   if (!m_backend->IsFragmentShadingRateSupported() || g_pplayer->m_renderer == nullptr)
      return false;
   const RenderTarget* const scene = g_pplayer->m_renderer->GetBackBufferTexture();
   const uint32_t texel = m_backend->GetFragmentShadingRateTexelSize();
   const uint32_t w = (static_cast<uint32_t>(scene->GetWidth()) + texel - 1) / texel, h = (static_cast<uint32_t>(scene->GetHeight()) + texel - 1) / texel;
   if (!bgfx::isValid(m_shadingRateMap) || w != m_shadingRateMapWidth || h != m_shadingRateMapHeight)
   {
      if (bgfx::isValid(m_shadingRateMap))
         bgfx::destroy(m_shadingRateMap);
      // Created without data (a texture created with data is immutable for bgfx, which then drops the updates below), filled right after
      m_shadingRateMap = bgfx::createTexture2D(static_cast<uint16_t>(w), static_cast<uint16_t>(h), false, 1, bgfx::TextureFormat::R8U, BGFX_TEXTURE_FRAGMENT_SHADING_RATE, nullptr);
      m_shadingRateMapWidth = w;
      m_shadingRateMapHeight = h;
      m_shadingRateMapKey[0] = -1.f;
      if (!bgfx::isValid(m_shadingRateMap))
      {
         PLOGW << "Foveated rendering: failed to create the shading rate image";
         return false;
      }
      bgfx::setName(m_shadingRateMap, "Foveation shading rate image");
      PLOGI << "Foveated rendering: shading rate image of " << w << 'x' << h << " (texel " << texel << ") for the " << scene->GetWidth() << 'x' << scene->GetHeight() << " scene buffer";
   }

   const int level = gazeValid ? clamp(m_foveationMode, 1, 3) : 1;
   // VPX_FOVEATION_DEBUG=1 shrinks the full quality area to a small spot (and makes the rest coarse) to check in the headset where the
   // spot sits against the gaze, which settles the sign conventions (FoveationFlipX/Y) of a runtime
   // (1: a spot for each eye's gaze, 2: a spot for the left eye's gaze only, 3: for the right eye's only)
   static const int debugSpot = getenv("VPX_FOVEATION_DEBUG") != nullptr ? atoi(getenv("VPX_FOVEATION_DEBUG")) : 0;
   const float fullRadius = debugSpot ? 0.06f : gazeValid ? kGazeFullRadius[level] : kFoveationFullRadius[level];
   const float halfRadius = debugSpot ? 0.10f : gazeValid ? kGazeHalfRadius[level] : kFoveationHalfRadius[level];
   const int firstEye = debugSpot == 3 ? 1 : 0, lastEye = debugSpot == 2 ? 0 : 1;
   const float scale = m_dynamicRenderScale;
   const float halfW = 0.5f * static_cast<float>(scene->GetWidth()) * scale / static_cast<float>(texel);
   const float halfH = 0.5f * static_cast<float>(scene->GetHeight()) * scale / static_cast<float>(texel);
   float cx[2], cy[2];
   for (int eye = 0; eye < 2; eye++)
   {
      // Same sign conventions as the density map offsets
      const float gx = gazeValid ? (m_foveationFlipX ? -1.f : 1.f) * m_foveationCenter[eye].x : 0.f;
      const float gy = gazeValid ? (m_foveationFlipY ? -1.f : 1.f) * m_foveationCenter[eye].y : 0.f;
      cx[eye] = halfW + gx * halfW;
      cy[eye] = halfH + gy * halfH;
   }
   if (debugSpot)
   {
      const float ndcX[2] = { cx[0] / halfW - 1.f, cx[1] / halfW - 1.f }, ndcY[2] = { cy[0] / halfH - 1.f, cy[1] / halfH - 1.f };
      UpdateGazeMarker(ndcX, ndcY, fullRadius);
   }
   const float key[6] = { static_cast<float>(level), scale, cx[0], cy[0], cx[1], cy[1] };
   bool changed = key[0] != m_shadingRateMapKey[0] || key[1] != m_shadingRateMapKey[1];
   for (int i = 2; i < 6 && !changed; i++)
      changed = fabsf(key[i] - m_shadingRateMapKey[i]) > 0.5f;
   if (!changed)
      return true;
   memcpy(m_shadingRateMapKey, key, sizeof(key));

   // Rate code: log2 of the fragment width in bits 2-3, of its height in bits 0-1; the driver's largest fragment bounds the coarse one
   const uint8_t coarseLog2 = m_backend->GetFragmentShadingRateMaxFragmentSize() >= 4 ? 2 : 1;
   const uint8_t codes[3] = { 0, 5, static_cast<uint8_t>((coarseLog2 << 2) | coarseLog2) };
   const bgfx::Memory* mem = bgfx::alloc(w * h);
   for (uint32_t y = 0; y < h; y++)
      for (uint32_t x = 0; x < w; x++)
      {
         float r = FLT_MAX;
         for (int eye = firstEye; eye <= lastEye; eye++)
         {
            const float dx = (static_cast<float>(x) + 0.5f - cx[eye]) / halfW, dy = (static_cast<float>(y) + 0.5f - cy[eye]) / halfH;
            r = min(r, sqrtf(dx * dx + dy * dy));
         }
         mem->data[y * w + x] = codes[r < fullRadius ? 0 : r < halfRadius ? 1 : 2];
      }
   bgfx::updateTexture2D(m_shadingRateMap, 0, 0, 0, 0, static_cast<uint16_t>(w), static_cast<uint16_t>(h), mem);
   return true;
   #else
   return false;
   #endif
}

// Debug ring (VPX_FOVEATION_DEBUG) around the full quality spot of each eye, in the eye's tangent space like the visibility mask (x, y at
// z = -1, the eye index in the normal), drawn by the renderer with the vr_mask technique on top of the scene
void VRDevice::UpdateGazeMarker(const float* cxNdc, const float* cyNdc, const float radiusNdc)
{
   constexpr int segments = 48;
   constexpr unsigned int nVerts = 2 * 2 * (segments + 1), nIndices = 2 * 6 * segments;
   RenderDevice* const rd = g_pplayer->m_renderer->m_renderDevice;
   if (m_gazeMarker == nullptr)
   {
      std::shared_ptr<IndexBuffer> ib = std::make_shared<IndexBuffer>(rd, nIndices, false, IndexBuffer::FMT_INDEX32);
      uint32_t* indices;
      ib->Lock(indices);
      for (unsigned int eye = 0; eye < 2; eye++)
         for (unsigned int i = 0; i < segments; i++)
         {
            const uint32_t base = eye * 2 * (segments + 1) + 2 * i;
            *indices++ = base; *indices++ = base + 1; *indices++ = base + 2;
            *indices++ = base + 1; *indices++ = base + 3; *indices++ = base + 2;
         }
      ib->Unlock();
      m_gazeMarker = std::make_shared<MeshBuffer>("GazeMarker"s, std::make_shared<VertexBuffer>(rd, nVerts, nullptr, true), ib, true);
   }
   Vertex3D_NoTex2* vertices;
   m_gazeMarker->m_vb->Lock(vertices);
   for (unsigned int eye = 0; eye < 2; eye++)
   {
      const XrFovf& fov = m_viewFov[eye];
      const float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight), tanD = tanf(fov.angleDown), tanU = tanf(fov.angleUp);
      const float halfW = 0.5f * (tanR - tanL), halfH = 0.5f * (tanU - tanD);
      // Image space (y down) to tangent space (y up)
      const float cx = 0.5f * (tanR + tanL) + cxNdc[eye] * halfW, cy = 0.5f * (tanU + tanD) - cyNdc[eye] * halfH;
      for (unsigned int i = 0; i <= segments; i++)
      {
         const float a = static_cast<float>(i) * (2.f * static_cast<float>(M_PI) / segments);
         for (unsigned int k = 0; k < 2; k++)
         {
            const float r = radiusNdc * (k == 0 ? 1.f : 1.12f);
            vertices->x = cx + cosf(a) * r * halfW;
            vertices->y = cy + sinf(a) * r * halfH;
            vertices->z = -1.f;
            vertices->nx = static_cast<float>(eye);
            vertices->ny = vertices->nz = vertices->tu = vertices->tv = 0.f;
            vertices++;
         }
      }
   }
   m_gazeMarker->m_vb->Unlock();
}

void VRDevice::UpdateFoveationState()
{
   if (!m_foveationEyeTrackedExtensionSupported || m_session == XR_NULL_HANDLE)
      return;
   XrFoveationEyeTrackedStateMETA state { XR_TYPE_FOVEATION_EYE_TRACKED_STATE_META };
   if (XR_SUCCEEDED(m_xrGetFoveationEyeTrackedStateMETA(m_session, &state)))
      m_foveationEyeTrackedActive = (state.flags & XR_FOVEATION_EYE_TRACKED_STATE_VALID_BIT_META) != 0;
}

string VRDevice::GetFoveationStatus() const
{
   if (!m_foveationExtensionSupported)
      return "Not supported by the runtime"s;
   if (m_foveationMode == 0 || !m_foveationApplied)
      return "Off"s;
   // Without a valid gaze the shading rate image falls back to the wide Low profile at the center of the view
   const string how = m_foveationShadingRate ? " (shading rate image, Low profile)"s : " (density map)"s;
   if (!m_foveationEyeTracked)
      return "Fixed at the center"s + how;
   if (!m_foveationEyeTrackedSystemSupported)
      return "Fixed: no eye tracking on this headset"s + how;
   return m_foveationEyeTrackedActive ? (m_foveationShadingRate ? "Eye-tracked (shading rate image)"s : "Eye-tracked (density map)"s)
                                      : "Fixed: eye tracking not active (enable it in the headset settings)"s + how;
}

void VRDevice::EnablePerformanceCounters()
{
   if (!m_performanceMetricsExtensionSupported || m_performanceCountersEnabled || m_session == XR_NULL_HANDLE)
      return;
   uint32_t count = 0;
   OPENXR_CHECK(m_xrEnumeratePerformanceMetricsCounterPathsMETA(m_xrInstance, 0, &count, nullptr), "Failed to enumerate performance counters.");
   vector<XrPath> paths(count);
   OPENXR_CHECK(m_xrEnumeratePerformanceMetricsCounterPathsMETA(m_xrInstance, count, &count, paths.data()), "Failed to enumerate performance counters.");
   m_performanceCounters.clear();
   m_gpuFrameTimePath = XR_NULL_PATH;
   for (uint32_t i = 0; i < count; i++)
   {
      char name[XR_MAX_PATH_LENGTH];
      uint32_t length = 0;
      if (XR_SUCCEEDED(xrPathToString(m_xrInstance, paths[i], sizeof(name), &length, name)))
      {
         m_performanceCounters.emplace_back(name, paths[i]);
         if (strcmp(name, "/perfmetrics_meta/app/gpu_frametime") == 0)
            m_gpuFrameTimePath = paths[i];
      }
   }
   XrPerformanceMetricsStateMETA state { XR_TYPE_PERFORMANCE_METRICS_STATE_META };
   state.enabled = XR_TRUE;
   OPENXR_CHECK(m_xrSetPerformanceMetricsStateMETA(m_session, &state), "Failed to enable performance counters.");
   m_performanceCountersEnabled = true;
   string names;
   for (const auto& counter : m_performanceCounters)
      names += ' ' + counter.first;
   PLOGI << "OpenXR performance counters enabled (" << m_performanceCounters.size() << "):" << names;
}

void VRDevice::SetDynamicResolution(bool enabled)
{
   m_dynamicResolution = enabled;
   if (!enabled)
      m_dynamicRenderScale = 1.f;
   // Turned on during a session: the counters are enabled by the render thread at the next frame (see UpdateDynamicResolution)
}

// Render thread, once per frame: the scale of the next frame from the GPU time of the last one. The counter is the runtime's measure of the
// application's GPU work for a frame, so it includes everything the frame renders and follows the GPU clock (the Frame slows its GPU down
// as it heats up, which this absorbs). The scale moves by small steps: down as soon as a frame is over the target, so a heavy view is
// caught within a few frames, up slowly once frames are well under it, so the sharpness does not pump. The lower limit is the runtime's
// recommended size (the headset's own performance target), the upper one the swapchain size chosen with ResFactor.
void VRDevice::UpdateDynamicResolution(const XrDuration predictedDisplayPeriod)
{
   // The budget is the display's frame period: the shortest period the runtime predicted in this session. SteamVR doubles the predicted
   // period when it throttles an application to half rate after missed frames; taking that as the budget would raise the resolution
   // again and keep the application throttled (seen on the Frame: 27.8 ms reported at 72 Hz, the scale climbed back to 100 %)
   if (predictedDisplayPeriod > 0)
   {
      const float periodMs = static_cast<float>(predictedDisplayPeriod) / 1000000.f;
      if (m_frameBudgetMs <= 0.f || periodMs < m_frameBudgetMs)
         m_frameBudgetMs = periodMs;
   }
   if (!m_dynamicResolution)
      return;
   if (!m_performanceCountersEnabled)
      EnablePerformanceCounters();
   if (m_gpuFrameTimePath == XR_NULL_PATH || predictedDisplayPeriod <= 0)
      return;
   XrPerformanceMetricsCounterMETA value { XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
   if (!XR_SUCCEEDED(m_xrQueryPerformanceMetricsCounterMETA(m_session, m_gpuFrameTimePath, &value)) || !(value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META))
      return;
   const float gpuMs = value.floatValue;
   m_lastGpuFrameTimeMs = gpuMs;
   if (gpuMs < 1.f) // Nothing rendered (the headset is not worn): keep the scale
      return;
   const float budgetMs = m_frameBudgetMs;
   const float targetMs = budgetMs * m_dynamicResolutionTarget;
   const float minScale = clamp(m_dynamicResolutionMinScale.load(), 0.5f, 1.f);
   float scale = m_dynamicRenderScale;
   if (gpuMs > targetMs)
      scale -= 0.02f * (1.f + (gpuMs - targetMs) / targetMs); // Faster the further over
   else if (gpuMs < targetMs * 0.9f)
      scale += 0.003f;
   m_dynamicRenderScale = clamp(scale, minScale, 1.f);
}

string VRDevice::GetDynamicResolutionStatus() const
{
   if (!m_performanceMetricsExtensionSupported)
      return "not supported by the runtime (no GPU time counter)";
   if (!m_dynamicResolution)
      return "off";
   const float scale = m_dynamicRenderScale;
   string status = std::format("{}x{} ({:.0f} %)", static_cast<int>(lroundf(static_cast<float>(m_eyeWidth) * scale)), static_cast<int>(lroundf(static_cast<float>(m_eyeHeight) * scale)), scale * 100.f);
   if (m_lastGpuFrameTimeMs > 0.f)
      status += std::format(", GPU {:.1f} ms of {:.1f} ms (target {:.0f} %)", m_lastGpuFrameTimeMs.load(), m_frameBudgetMs.load(), m_dynamicResolutionTarget.load() * 100.f);
   return status;
}

// Every few seconds: the eye-tracked state and the runtime's performance counters (the ones in milliseconds and percents), so settings can be
// compared from the log on the device
void VRDevice::LogRuntimeStatus()
{
   const double now = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.;
   if (now < m_nextStatusLogTime)
      return;
   m_nextStatusLogTime = now + 5.;
   UpdateFoveationState();
   string status = "Foveation: " + GetFoveationStatus();
   if (m_foveationEyeTrackedActive)
      status += std::format(" (gaze{} L {:.2f},{:.2f} R {:.2f},{:.2f})", m_gazeRayValid ? " ray" : "", m_foveationCenter[0].x, m_foveationCenter[0].y, m_foveationCenter[1].x, m_foveationCenter[1].y);
   status += " | Dynamic resolution: " + GetDynamicResolutionStatus();
   for (const auto& counter : m_performanceCounters)
   {
      XrPerformanceMetricsCounterMETA value { XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
      if (!XR_SUCCEEDED(m_xrQueryPerformanceMetricsCounterMETA(m_session, counter.second, &value)) || !(value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_ANY_VALUE_VALID_BIT_META))
         continue;
      if (value.counterUnit != XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META && value.counterUnit != XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META
         && value.counterUnit != XR_PERFORMANCE_METRICS_COUNTER_UNIT_HERTZ_META)
         continue;
      const string name = counter.first.rfind('/') == string::npos ? counter.first : counter.first.substr(counter.first.rfind('/') + 1);
      const char* unit = value.counterUnit == XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META ? " ms" : value.counterUnit == XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META ? " %" : " Hz";
      status += ", " + name + ' ';
      if (value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META)
         status += std::format("{:.2f}", value.floatValue);
      else
         status += std::to_string(value.uintValue);
      status += unit;
   }
   // Controller models: what the runtime gives for each (diagnostics of the placement)
   #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
   for (size_t i = 0; i < m_controllerModels.size() && i < std::size(m_controllerModelsDebug); i++)
   {
      const auto& d = m_controllerModelsDebug[i];
      status += std::format(" | controller {}: locate {} flags 0x{:x} pos {:.2f},{:.2f},{:.2f} state {} visible {}/{} {}", m_controllerModels[i].id, static_cast<int>(d.locateResult),
         static_cast<uint64_t>(d.flags), d.pose.position.x, d.pose.position.y, d.pose.position.z, static_cast<int>(d.stateResult), d.visibleNodes, m_controllerModels[i].nodeStates.size(),
         m_controllerModels[i].located ? "located" : "not located");
      if (!m_controllerModels[i].nodeStates.empty())
      {
         const XrPosef& p = m_controllerModels[i].nodeStates[0].pose;
         status += std::format(", node 0 '{}' q {:.2f},{:.2f},{:.2f},{:.2f} p {:.3f},{:.3f},{:.3f}", m_controllerModels[i].animatableNodes[0], p.orientation.x, p.orientation.y, p.orientation.z,
            p.orientation.w, p.position.x, p.position.y, p.position.z);
      }
   }
   if (!m_controllerModels.empty())
      status += std::format(" | head {:.2f},{:.2f},{:.2f}", m_uiHeadPos.x, m_uiHeadPos.y, m_uiHeadPos.z);
   #endif
   // With VPX_GPU_PROFILE set in the environment (bgfx profiler enabled by the render device), the GPU time of the last frame per render target
   if (getenv("VPX_GPU_PROFILE") != nullptr)
   {
      const bgfx::Stats* const stats = bgfx::getStats();
      if (stats->gpuTimerFreq > 0 && stats->numViews > 0)
      {
         std::map<string, double> perTarget;
         std::map<string, int> viewsPerTarget;
         double total = 0.;
         for (uint16_t i = 0; i < stats->numViews; i++)
         {
            const bgfx::ViewStats& vs = stats->viewStats[i];
            if (vs.gpuTimeEnd <= vs.gpuTimeBegin)
               continue;
            const double ms = static_cast<double>(vs.gpuTimeEnd - vs.gpuTimeBegin) * 1000. / static_cast<double>(stats->gpuTimerFreq);
            string key = (g_pplayer->m_renderer && vs.view < g_pplayer->m_renderer->m_renderDevice->m_viewNames.size()) ? g_pplayer->m_renderer->m_renderDevice->m_viewNames[vs.view] : string(vs.name);
            const size_t rt = key.find("[RT=");
            if (rt != string::npos)
            {
               key = key.substr(rt + 4);
               const size_t end = key.find_first_of(" ]");
               if (end != string::npos)
                  key = key.substr(0, end);
            }
            perTarget[key] += ms;
            viewsPerTarget[key]++;
            total += ms;
         }
         status += std::format(" | GPU by target ({:.2f} ms, {} views):", total, stats->numViews);
         vector<std::pair<string, double>> sorted(perTarget.begin(), perTarget.end());
         std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
         for (size_t i = 0; i < sorted.size() && i < 8; i++)
            status += std::format(" {} {:.2f} ({} views)", sorted[i].first, sorted[i].second, viewsPerTarget[sorted[i].first]);
      }
   }
   PLOGI << status;
}

void VRDevice::CreateSession()
{
   assert(m_xrInstance != XR_NULL_HANDLE);
   assert(m_systemID != XR_NULL_SYSTEM_ID);
   assert(m_session == XR_NULL_HANDLE);
   assert(m_backend != nullptr);

   // Events left by the session of the previous table must not be taken for events of the new one (a new handle may have the same value)
   DrainEvents();
   m_exitRequested = false;
   m_sessionState = XR_SESSION_STATE_UNKNOWN;
   m_sessionRunning = false;

   m_visibilityMaskDirty = true;

   // Evaluate scene size to be able to define a good far plane
   // Adjust near/far plane for each projected bounding box
   vector<Vertex3Ds> bounds;
   bounds.reserve(16);
   Vertex3Ds sceneMin(FLT_MAX, FLT_MAX, FLT_MAX);
   Vertex3Ds sceneMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
   for (IEditable* editable : g_pplayer->m_ptable->GetParts())
   {
      bool prevVisibility;
      Primitive* prim = editable->GetItemType() == ItemTypeEnum::eItemPrimitive ? static_cast<Primitive*>(editable) : nullptr;
      if (prim)
      {
         prevVisibility = prim->m_d.m_visible;
         prim->m_d.m_visible = true;
      }
      editable->GetBoundingVertices(bounds, nullptr);
      if (prim)
         prim->m_d.m_visible = prevVisibility;
      for (const auto& v : bounds)
      {
         sceneMin.x = min(sceneMin.x, v.x);
         sceneMin.y = min(sceneMin.y, v.y);
         sceneMin.z = min(sceneMin.z, v.z);
         sceneMax.x = max(sceneMax.x, v.x);
         sceneMax.y = max(sceneMax.y, v.y);
         sceneMax.z = max(sceneMax.z, v.z);
      }
      bounds.clear();
   }
   m_sceneSize = (sceneMax - sceneMin).Length();

   // Create an XrSessionCreateInfo structure.
   XrSessionCreateInfo sessionCI { XR_TYPE_SESSION_CREATE_INFO };
   sessionCI.next = m_backend->GetGraphicsBinding();
   sessionCI.createFlags = 0;
   sessionCI.systemId = m_systemID;
   OPENXR_CHECK(xrCreateSession(m_xrInstance, &sessionCI, &m_session), "Failed to create Session.");
   assert(m_session);

   // Initialize passthrough if supported (Meta Quest MR feature)
   if (m_passthroughExtensionSupported && g_pplayer && g_pplayer->m_ptable->m_settings.GetPlayerVR_UsePassthroughColor())
   {
      PFN_xrCreatePassthroughFB xrCreatePassthroughFB;
      OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrCreatePassthroughFB", (PFN_xrVoidFunction*)&xrCreatePassthroughFB), "Failed to get xrCreatePassthroughFB.");

      XrPassthroughCreateInfoFB passthroughCI { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
      passthroughCI.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
      OPENXR_CHECK(xrCreatePassthroughFB(m_session, &passthroughCI, &m_passthrough), "Failed to create passthrough.");

      PFN_xrCreatePassthroughLayerFB xrCreatePassthroughLayerFB;
      OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction*)&xrCreatePassthroughLayerFB), "Failed to get xrCreatePassthroughLayerFB.");

      XrPassthroughLayerCreateInfoFB passthroughLayerCI { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
      passthroughLayerCI.passthrough = m_passthrough;
      passthroughLayerCI.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
      passthroughLayerCI.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
      OPENXR_CHECK(xrCreatePassthroughLayerFB(m_session, &passthroughLayerCI, &m_passthroughLayer), "Failed to create passthrough layer.");

      m_passthroughEnabled = true;
      PLOGI << "Meta Quest passthrough initialized successfully";
   }

   // Fill out an XrReferenceSpaceCreateInfo structure and create a reference XrSpace, specifying an identity pose as the origin and a stage space, defaulting to a local space.
   uint32_t referenceSpaceCount;
   xrEnumerateReferenceSpaces(m_session, 0, &referenceSpaceCount, nullptr);
   XrReferenceSpaceType* referenceSpaces = new XrReferenceSpaceType[referenceSpaceCount];
   xrEnumerateReferenceSpaces(m_session, referenceSpaceCount, &referenceSpaceCount, referenceSpaces);
   XrReferenceSpaceCreateInfo referenceSpaceCI { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
   for (uint32_t i = 0; i < referenceSpaceCount; i++)
      if (referenceSpaces[i] == XR_REFERENCE_SPACE_TYPE_STAGE)
         referenceSpaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
      else if ((referenceSpaces[i] == XR_REFERENCE_SPACE_TYPE_LOCAL) && (referenceSpaceCI.referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE))
         referenceSpaceCI.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
   referenceSpaceCI.poseInReferenceSpace = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
   OPENXR_CHECK(xrCreateReferenceSpace(m_session, &referenceSpaceCI, &m_referenceSpace), "Failed to create ReferenceSpace.");
   delete[] referenceSpaces;

   // Get the supported swapchain formats as an array of int64_t and ordered by runtime preference.
   uint32_t formatCount = 0;
   OPENXR_CHECK(xrEnumerateSwapchainFormats(m_session, 0, &formatCount, nullptr), "Failed to enumerate Swapchain Formats");
   std::vector<int64_t> formats(formatCount);
   OPENXR_CHECK(xrEnumerateSwapchainFormats(m_session, formatCount, &formatCount, formats.data()), "Failed to enumerate Swapchain Formats");
   if (m_backend->SelectDepthSwapchainFormat(formats) == 0)
   {
      PLOGE << "Failed to find depth format for Swapchain." ;
      assert(false);
   }

   for (int i = 0; i < 2; i++)
   {
      SwapchainInfo& swapchain = i == 0 ? m_colorSwapchainInfo : m_depthSwapchainInfo;
      swapchain.backendFormat = i == 0 ? m_backend->SelectColorSwapchainFormat(formats) : m_backend->SelectDepthSwapchainFormat(formats);
      swapchain.width = m_eyeWidth;
      swapchain.height = m_eyeHeight;
      swapchain.arraySize = static_cast<uint32_t>(m_viewConfigurationViews.size());

      XrSwapchainCreateInfo swapchainCreateInfo { XR_TYPE_SWAPCHAIN_CREATE_INFO };
      swapchainCreateInfo.arraySize = swapchain.arraySize;
      swapchainCreateInfo.format = swapchain.backendFormat;
      swapchainCreateInfo.width = swapchain.width;
      swapchainCreateInfo.height = swapchain.height;
      swapchainCreateInfo.mipCount = 1;
      swapchainCreateInfo.faceCount = 1;
      swapchainCreateInfo.sampleCount = m_viewConfigurationViews[0].recommendedSwapchainSampleCount;
      swapchainCreateInfo.createFlags = 0;
      swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT
         | (i == 0 ? (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) : (XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT));
      // Let the runtime attach fragment density maps to the color swapchain: the foveation profile is applied to it in ApplyFoveation
      XrSwapchainCreateInfoFoveationFB foveationCreateInfo { XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB };
      foveationCreateInfo.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
      if (i == 0 && m_foveationExtensionSupported)
         swapchainCreateInfo.next = &foveationCreateInfo;
      OPENXR_CHECK(xrCreateSwapchain(m_session, &swapchainCreateInfo, &swapchain.swapchain), "Failed to create Swapchain");

      uint32_t swapchainImageCount;
      OPENXR_CHECK(xrEnumerateSwapchainImages(swapchain.swapchain, 0, &swapchainImageCount, nullptr), "Failed to enumerate Swapchain Images.");
      m_backend->RequestFoveationImages(i == 0 && m_foveationExtensionSupported);
      XrSwapchainImageBaseHeader* swapchainImages = m_backend->AllocateSwapchainImageData(swapchain.swapchain, i == 0 ? SwapchainType::COLOR : SwapchainType::DEPTH, swapchainImageCount);
      OPENXR_CHECK(xrEnumerateSwapchainImages(swapchain.swapchain, swapchainImageCount, &swapchainImageCount, swapchainImages), "Failed to enumerate Swapchain Images.");
      m_backend->CreateImageViews(swapchain);
      if (i == 0 && m_foveationExtensionSupported)
      {
         if (m_backend->CreateFoveationTextures(swapchain))
            PLOGI << "Foveated rendering: " << swapchain.foveationTextures.size() << " density maps of " << swapchain.foveationWidth << 'x' << swapchain.foveationHeight << " received from the runtime";
         else
            PLOGI << "Foveated rendering: no density map from the runtime" << (m_backend->IsFragmentDensityMapSupported() ? "" : " (fragment density maps not supported by this build or driver)");
      }
   }
   m_swapchainRenderTargets.resize(m_colorSwapchainInfo.imageViews.size() * m_depthSwapchainInfo.imageViews.size());
   ApplyFoveation();
   if (m_foveationExtensionSupported)
      CreateOwnFoveationMap();

   // The runtime's performance counters (app GPU frame time on the Steam Frame, logged every 5 s): for the dynamic resolution, and when
   // measuring (VPX_XR_METRICS=1 for the counters alone, VPX_GPU_PROFILE=1 for the counters and bgfx's per-target breakdown). They cost the
   // runtime a timestamp query per frame, and SteamVR mishandles them across sessions (see ReleaseSession)
   if (m_dynamicResolution || getenv("VPX_XR_METRICS") != nullptr || getenv("VPX_GPU_PROFILE") != nullptr)
      EnablePerformanceCounters();

   auto inputHandler = std::make_unique<XRInputHandler>(g_pplayer->m_pininput, m_xrInstance, m_session, m_eyeGazeSystemSupported);
   if (m_eyeGazeSystemSupported)
   {
      if (const XrAction gazeAction = inputHandler->GetAction("/user/eyes_ext/input/gaze_ext/pose"); gazeAction != XR_NULL_HANDLE)
      {
         XrActionSpaceCreateInfo actionSpaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
         actionSpaceInfo.action = gazeAction;
         actionSpaceInfo.poseInActionSpace = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
         OPENXR_CHECK(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_gazeSpace), "Failed to create the eye gaze action space.");
         PLOGI << "Foveated rendering: gaze taken from the eye gaze ray (XR_EXT_eye_gaze_interaction)";
      }
   }
   XrAction leftPoseAction = inputHandler->GetAction("/user/hand/left/input/grip/pose");
   if (leftPoseAction != XR_NULL_HANDLE)
   {
      XrActionSpaceCreateInfo actionSpaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
      actionSpaceInfo.action = leftPoseAction;
      actionSpaceInfo.poseInActionSpace = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
      OPENXR_CHECK(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_leftControllerSpace), "Failed to create Left Controller Action Space.");
   }
   XrAction rightPoseAction = inputHandler->GetAction("/user/hand/right/input/grip/pose");
   if (rightPoseAction != XR_NULL_HANDLE)
   {
      XrActionSpaceCreateInfo actionSpaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
      actionSpaceInfo.action = rightPoseAction;
      actionSpaceInfo.poseInActionSpace = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
      OPENXR_CHECK(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_rightControllerSpace), "Failed to create Right Controller Action Space.");
   }
   for (const auto& [path, space] : { std::pair<const char*, XrSpace*> { "/user/hand/left/input/aim/pose", &m_leftAimSpace }, { "/user/hand/right/input/aim/pose", &m_rightAimSpace } })
   {
      if (XrAction aimAction = inputHandler->GetAction(path); aimAction != XR_NULL_HANDLE)
      {
         XrActionSpaceCreateInfo actionSpaceInfo { XR_TYPE_ACTION_SPACE_CREATE_INFO };
         actionSpaceInfo.action = aimAction;
         actionSpaceInfo.poseInActionSpace = { { 0.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 0.0f } };
         OPENXR_CHECK(xrCreateActionSpace(m_session, &actionSpaceInfo, space), "Failed to create Controller Aim Action Space.");
      }
   }
   m_xrInputHandler = inputHandler.get();
   g_pplayer->m_pininput.AddInputHandler(std::move(inputHandler));
}

// The UI is displayed on a vertical panel standing in the room (the reference space, in meters), placed in front of the head, facing the
// table's forward direction, when it is first displayed. Pointing at it is intersecting the aim ray of a controller with that panel.
void VRDevice::UpdateUIPanel(const std::vector<XrView>& views, XrTime time)
{
   m_uiPointerValid = false;
   if (views.size() < 2)
      return;

   const auto rotate = [](const XrQuaternionf& q, const vec3& v)
   {
      const vec3 u(q.x, q.y, q.z);
      const vec3 t = CrossProduct(u, v) * 2.f;
      return v + t * q.w + CrossProduct(u, t);
   };

   const vec3 head = (vec3(views[0].pose.position.x, views[0].pose.position.y, views[0].pose.position.z) + vec3(views[1].pose.position.x, views[1].pose.position.y, views[1].pose.position.z)) * 0.5f;
   m_uiHeadPos = head;
   if (!m_uiPanelPlaced)
   {
      // The panel covers the whole UI display area, most of it being transparent. The UI windows are laid out for the full field of view
      // of the headset: at the default distance of 0.8m, they get bigger than at 1m (where 2.8m wide gives about 109 degrees).
      // The player chooses the distance (Standalone.VRMenuDistance), keeping the size, so a closer window looks bigger.
      constexpr float width = 2.8f, belowEyes = 0.1f; // meters
      const float distance = clamp(g_app->m_settings.GetStandalone_VRMenuDistance(), 0.3f, 3.f);
      const float height = width * static_cast<float>(m_eyeHeight) / static_cast<float>(m_eyeWidth);
      // Upright panel in the forward direction of the table (the way the player faces it at the lockbar), wherever the head looks, so that
      // the player turns to the table, or moves, to read the menu. The scene orientation is the yaw of that direction (see the table
      // centering in RenderFrame: looking along -Z gives 0, turned left towards -X gives +90 degrees).
      const float orientation = ANGTORAD(m_orientation);
      const vec3 forward(-sinf(orientation), 0.f, -cosf(orientation));
      const vec3 up(0.f, 1.f, 0.f);
      const vec3 right = CrossProduct(forward, up);
      const vec3 center = head + forward * distance - up * belowEyes;
      m_uiPanelRight = right * width;
      m_uiPanelDown = up * -height;
      m_uiPanelOrigin = center - m_uiPanelRight * 0.5f - m_uiPanelDown * 0.5f;
      m_uiPanelPlaced = true;
   }

   // Depth is not used (the shader outputs a constant depth, the UI being drawn over the scene), so near and far planes do not matter
   for (int eye = 0; eye < 2; eye++)
   {
      Matrix3D view, proj;
      XrPosef_ToMatrix3D(&view, &views[eye].pose);
      proj.SetPerspectiveFovRH(views[eye].fov.angleLeft, views[eye].fov.angleRight, views[eye].fov.angleDown, views[eye].fov.angleUp, 0.05f, 100.f);
      m_uiPanelViewProj[eye] = view * proj;
   }

   if (m_xrInputHandler == nullptr)
      return;

   // Right hand first, left hand if the right one is not tracked or does not point at the UI
   const vec3 normal = CrossProduct(m_uiPanelRight, m_uiPanelDown);
   struct Hand { XrSpace space; const char* trigger; const char* thumbstickY; };
   for (const auto& [space, trigger, thumbstickY] : { Hand { m_rightAimSpace, "/user/hand/right/input/trigger/value", "/user/hand/right/input/thumbstick/y" },
                                                      Hand { m_leftAimSpace, "/user/hand/left/input/trigger/value", "/user/hand/left/input/thumbstick/y" } })
   {
      XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
      if (space == XR_NULL_HANDLE || xrLocateSpace(space, m_referenceSpace, time, &location) != XR_SUCCESS)
         continue;
      if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 || (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
         continue;
      const vec3 origin(location.pose.position.x, location.pose.position.y, location.pose.position.z);
      const vec3 direction = rotate(location.pose.orientation, vec3(0.f, 0.f, -1.f));
      const float denom = direction.Dot(normal);
      if (fabsf(denom) < 1e-6f)
         continue;
      const float t = (m_uiPanelOrigin - origin).Dot(normal) / denom;
      if (t <= 0.f)
         continue;
      const vec3 onPanel = origin + direction * t - m_uiPanelOrigin;
      const float x = onPanel.Dot(m_uiPanelRight) / m_uiPanelRight.LengthSquared();
      const float y = onPanel.Dot(m_uiPanelDown) / m_uiPanelDown.LengthSquared();
      if (x < 0.f || x > 1.f || y < 0.f || y > 1.f)
         continue;

      m_uiPointerValid = true;
      m_uiPointerX = x;
      m_uiPointerY = y;
      m_uiPointerRayStart = origin;
      m_uiPointerRayEnd = origin + direction * t;
      m_uiPointerScroll = m_xrInputHandler->GetFloatState(thumbstickY);
      // Hysteresis, as this is an analog trigger
      const float triggerValue = m_xrInputHandler->GetFloatState(trigger);
      m_uiPointerPressed = m_uiPointerPressed ? (triggerValue > 0.4f) : (triggerValue > 0.7f);
      return;
   }
   m_uiPointerPressed = false;
   m_uiPointerScroll = 0.f;
}

void VRDevice::SetTableCaptureViewPoses(std::vector<XrView>& views, float vpuToWorldScale, TableCaptureView captureView) const
{
   // Playfield in the reference space (meters), from the playfield placement of the previous frame
   const PinTable* const table = g_pplayer->m_ptable;
   const float cx = 0.5f * (table->m_left + table->m_right);
   const vec3 front = m_pfWorld.m_toWorld * vec3(cx, table->m_bottom, 0.f) * vpuToWorldScale;
   const vec3 back = m_pfWorld.m_toWorld * vec3(cx, table->m_top, 0.f) * vpuToWorldScale;
   const vec3 center = (front + back) * 0.5f;
   const vec3 width = m_pfWorld.m_toWorld.MultiplyVectorNoTranslate(vec3(table->m_right - table->m_left, 0.f, 0.f)) * vpuToWorldScale;
   const float length = (front - back).Length();
   const vec3 up(0.f, 1.f, 0.f);
   vec3 toPlayer = front - back; // Horizontal direction from the back of the table to the player
   toPlayer.y = 0.f;
   if (toPlayer.LengthSquared() < 1e-8f)
      return;
   toPlayer.Normalize();

   // The captured eye has an asymmetric field of view: its image is centered on a direction which is not the view axis. The framing uses the half
   // extents of the image around that direction (the eye is then shifted so that the target is at the center of the image, see below).
   const XrFovf& fov = views[0].fov;
   const float tanVertical = 0.5f * (tanf(fov.angleUp) - tanf(fov.angleDown));
   const float tanHorizontal = 0.5f * (tanf(fov.angleRight) - tanf(fov.angleLeft));
   const float tanCenterX = 0.5f * (tanf(fov.angleRight) + tanf(fov.angleLeft));
   const float tanCenterY = 0.5f * (tanf(fov.angleUp) + tanf(fov.angleDown));
   const auto fitDistance = [&](float halfWidth, float halfHeight) { return 1.1f * max(halfHeight / tanVertical, halfWidth / tanHorizontal); };

   vec3 eye, target, upHint = up;
   switch (captureView)
   {
   case TableCaptureView::Table:
      // Straight down on the whole playfield, the back of the table at the top of the image
      target = center;
      eye = center + up * fitDistance(0.5f * width.Length(), 0.5f * length);
      upHint = toPlayer * -1.f;
      break;

   case TableCaptureView::Backglass:
   {
      if (!m_tableCaptureBounds.empty())
      {
         // Facing the parts which show the backglass, framed on their bounds as seen from the player
         const auto toWorld = [&](int space, const vec3& p)
         {
            using enum PartGroupData::SpaceReference;
            const PartGroupData::SpaceReference spaceRef = static_cast<PartGroupData::SpaceReference>(space);
            const Matrix3D& m = spaceRef == SR_CABINET ? m_cabWorld.m_toWorld : spaceRef == SR_CABINET_FEET ? m_feetWorld.m_toWorld : spaceRef == SR_ROOM ? m_roomWorld.m_toWorld : m_pfWorld.m_toWorld;
            return (m * p) * vpuToWorldScale;
         };
         vec3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX), boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
         vector<vec3> points;
         for (const auto& [space, p] : m_tableCaptureBounds)
         {
            const vec3 w = toWorld(space, p);
            points.push_back(w);
            boundsMin = vec3(min(boundsMin.x, w.x), min(boundsMin.y, w.y), min(boundsMin.z, w.z));
            boundsMax = vec3(max(boundsMax.x, w.x), max(boundsMax.y, w.y), max(boundsMax.z, w.z));
         }
         target = (boundsMin + boundsMax) * 0.5f;
         const vec3 side = CrossProduct(up, toPlayer);
         float halfWidth = 0.f, halfHeight = 0.f, front = 0.f;
         for (const vec3& w : points)
         {
            const vec3 d = w - target;
            halfWidth = max(halfWidth, fabsf(d.Dot(side)));
            halfHeight = max(halfHeight, fabsf(d.Dot(up)));
            front = max(front, d.Dot(toPlayer));
         }
         eye = target + toPlayer * (front + fitDistance(halfWidth, halfHeight));
         break;
      }
      // Tables that do not name their backglass parts: it is placed like on a real cabinet (scaled like it), the glass centered
      // about 55cm above and 10cm behind the back of the playfield. The framed area, 90 x 80cm, is larger than a usual glass (about 75 x 60cm)
      // so that taller or higher backboxes keep their top
      target = back + up * (0.55f * m_scale) - toPlayer * (0.1f * m_scale);
      eye = target + toPlayer * fitDistance(0.45f * m_scale, 0.4f * m_scale);
      break;
   }

   case TableCaptureView::Cabinet:
   {
      // From the front, 30 degrees to the right and 20 degrees above, the whole cabinet from the floor (reference space ground) to the top of the backbox
      const float top = back.y + 0.85f * m_scale;
      target = vec3(center.x, 0.5f * top, center.z);
      const float yaw = ANGTORAD(30.f), pitch = ANGTORAD(20.f);
      const vec3 side = CrossProduct(up, toPlayer); // Horizontal, to the right of the player
      const vec3 horizontal = toPlayer * cosf(yaw) + side * sinf(yaw);
      const vec3 direction = horizontal * cosf(pitch) + up * sinf(pitch);
      const float radius = 0.5f * sqrtf(length * length + top * top + width.LengthSquared());
      eye = target + direction * (1.1f * radius / sinf(atanf(min(tanVertical, tanHorizontal))));
      break;
   }

   default: return;
   }

   // Look at the target: view basis (-Z of the view is forward), then rotation matrix (columns: right, up, back) to quaternion
   vec3 viewBack = eye - target;
   viewBack.Normalize();
   vec3 viewRight = CrossProduct(upHint, viewBack);
   if (viewRight.LengthSquared() < 1e-8f)
      return;
   viewRight.Normalize();
   const vec3 viewUp = CrossProduct(viewBack, viewRight);
   const float m00 = viewRight.x, m01 = viewUp.x, m02 = viewBack.x;
   const float m10 = viewRight.y, m11 = viewUp.y, m12 = viewBack.y;
   const float m20 = viewRight.z, m21 = viewUp.z, m22 = viewBack.z;
   XrQuaternionf orientation;
   if (const float trace = m00 + m11 + m22; trace > 0.f)
   {
      const float s = 0.5f / sqrtf(trace + 1.f);
      orientation = { (m21 - m12) * s, (m02 - m20) * s, (m10 - m01) * s, 0.25f / s };
   }
   else if (m00 > m11 && m00 > m22)
   {
      const float s = 2.f * sqrtf(1.f + m00 - m11 - m22);
      orientation = { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
   }
   else if (m11 > m22)
   {
      const float s = 2.f * sqrtf(1.f + m11 - m00 - m22);
      orientation = { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
   }
   else
   {
      const float s = 2.f * sqrtf(1.f + m22 - m00 - m11);
      orientation = { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
   }

   // Shift the eye (not its orientation) so that the target lands at the center of the image, which is off the view axis: seen from the shifted eye,
   // the target is in the direction (tanCenterX, tanCenterY, -1) of the view
   const float distance = (eye - target).Length();
   eye = eye - (viewRight * tanCenterX + viewUp * tanCenterY) * distance;

   for (XrView& view : views)
   {
      view.pose.orientation = orientation;
      view.pose.position = { eye.x, eye.y, eye.z };
   }
}

bool VRDevice::GetUIPointerRayTransforms(Matrix3D (&quadToClip)[2]) const
{
   if (!m_uiPointerValid)
      return false;
   // A flat ribbon from the controller to the pointed position, turned towards the head so that it is seen from its wide side
   constexpr float thickness = 0.004f; // meters
   const vec3 along = m_uiPointerRayEnd - m_uiPointerRayStart;
   vec3 across = CrossProduct(along, m_uiHeadPos - m_uiPointerRayStart);
   if (across.LengthSquared() < 1e-10f)
      return false;
   across.Normalize();
   across = across * thickness;
   const vec3 start = m_uiPointerRayStart - across * 0.5f;
   const Matrix3D quadToWorld(
      along.x, along.y, along.z, 0.f,
      across.x, across.y, across.z, 0.f,
      0.f, 0.f, 1.f, 0.f,
      start.x, start.y, start.z, 1.f);
   for (int eye = 0; eye < 2; eye++)
      quadToClip[eye] = quadToWorld * m_uiPanelViewProj[eye];
   return true;
}

bool VRDevice::GetUIPanelTransforms(float width, float height, Matrix3D (&pixelToClip)[2]) const
{
   if (!m_uiPanelPlaced)
      return false;
   // UI pixel to reference space: the panel origin, then its full width and height spread over the UI display size
   const vec3 dx = m_uiPanelRight * (1.f / width), dy = m_uiPanelDown * (1.f / height);
   const Matrix3D pixelToPanel(
      dx.x, dx.y, dx.z, 0.f,
      dy.x, dy.y, dy.z, 0.f,
      0.f, 0.f, 1.f, 0.f,
      m_uiPanelOrigin.x, m_uiPanelOrigin.y, m_uiPanelOrigin.z, 1.f);
   for (int eye = 0; eye < 2; eye++)
      pixelToClip[eye] = pixelToPanel * m_uiPanelViewProj[eye];
   return true;
}

// Asks the runtime which controller models to show (the devices the player holds), keeping the models already loaded and loading the new ones.
// The list is empty until the runtime has bound the controllers, and changes are signaled with XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT.
void VRDevice::UpdateControllerModels()
{
   m_controllerModelsDirty = false;
   m_controllerModelsRetryTime = 0.;
   #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
   if (!m_renderModelExtensionSupported || m_session == XR_NULL_HANDLE || m_xrInputHandler == nullptr) // The input handler attaches the action sets, which the enumeration needs
      return;

   XrInteractionRenderModelIdsEnumerateInfoEXT enumerateInfo { XR_TYPE_INTERACTION_RENDER_MODEL_IDS_ENUMERATE_INFO_EXT };
   uint32_t count = 0;
   if (const XrResult res = m_xrEnumerateInteractionRenderModelIdsEXT(m_session, &enumerateInfo, 0, &count, nullptr); !XR_SUCCEEDED(res))
   {
      PLOGE << "OpenXR: failed to enumerate controller models: " << GetXRErrorString(m_xrInstance, res);
      return;
   }
   vector<XrRenderModelIdEXT> ids(count);
   if (count > 0 && !XR_SUCCEEDED(m_xrEnumerateInteractionRenderModelIdsEXT(m_session, &enumerateInfo, count, &count, ids.data())))
      return;
   ids.resize(count);
   if (ids.empty()) // Controllers not bound yet: the runtime should signal the change, but ask again later in case it does not
      m_controllerModelsRetryTime = static_cast<double>(usec()) * 1e-6 + 2.;

   vector<ControllerModel> models;
   vector<RenderModelHandles> handles;
   for (const XrRenderModelIdEXT id : ids)
   {
      // Already loaded
      if (const auto it = std::ranges::find_if(m_controllerModels, [id](const ControllerModel& model) { return model.id == static_cast<uint64_t>(id); }); it != m_controllerModels.end())
      {
         const size_t index = it - m_controllerModels.begin();
         models.push_back(std::move(*it));
         handles.push_back(m_renderModelHandles[index]);
         m_renderModelHandles[index] = {};
         it->id = 0;
         continue;
      }

      // glTF extensions the asset may require: our loader (VRControllerModels) reads quantized attributes, nothing else
      static const char* const gltfExtensions[] = { "KHR_mesh_quantization" };
      XrRenderModelCreateInfoEXT createInfo { XR_TYPE_RENDER_MODEL_CREATE_INFO_EXT };
      createInfo.renderModelId = id;
      createInfo.gltfExtensionCount = static_cast<uint32_t>(std::size(gltfExtensions));
      createInfo.gltfExtensions = gltfExtensions;
      RenderModelHandles handle;
      if (const XrResult res = m_xrCreateRenderModelEXT(m_session, &createInfo, &handle.renderModel); !XR_SUCCEEDED(res))
      {
         PLOGE << "OpenXR: failed to create controller model " << static_cast<uint64_t>(id) << ": " << GetXRErrorString(m_xrInstance, res);
         continue;
      }
      XrRenderModelPropertiesGetInfoEXT propertiesInfo { XR_TYPE_RENDER_MODEL_PROPERTIES_GET_INFO_EXT };
      XrRenderModelPropertiesEXT properties { XR_TYPE_RENDER_MODEL_PROPERTIES_EXT };
      XrRenderModelSpaceCreateInfoEXT spaceInfo { XR_TYPE_RENDER_MODEL_SPACE_CREATE_INFO_EXT };
      spaceInfo.renderModel = handle.renderModel;
      XrRenderModelAssetCreateInfoEXT assetInfo { XR_TYPE_RENDER_MODEL_ASSET_CREATE_INFO_EXT };
      XrRenderModelAssetEXT asset = XR_NULL_HANDLE;
      XrResult res = m_xrGetRenderModelPropertiesEXT(handle.renderModel, &propertiesInfo, &properties);
      if (XR_SUCCEEDED(res))
         res = m_xrCreateRenderModelSpaceEXT(m_session, &spaceInfo, &handle.space);
      if (XR_SUCCEEDED(res))
      {
         assetInfo.cacheId = properties.cacheId;
         res = m_xrCreateRenderModelAssetEXT(m_session, &assetInfo, &asset);
      }
      ControllerModel model;
      model.id = static_cast<uint64_t>(id);
      if (XR_SUCCEEDED(res))
      {
         // The glTF binary, then the names of the animatable nodes, which are given in the order of the node states
         XrRenderModelAssetDataGetInfoEXT dataInfo { XR_TYPE_RENDER_MODEL_ASSET_DATA_GET_INFO_EXT };
         XrRenderModelAssetDataEXT data { XR_TYPE_RENDER_MODEL_ASSET_DATA_EXT };
         res = m_xrGetRenderModelAssetDataEXT(asset, &dataInfo, &data);
         auto glb = std::make_shared<vector<uint8_t>>(data.bufferCountOutput);
         data.bufferCapacityInput = static_cast<uint32_t>(glb->size());
         data.buffer = glb->data();
         if (XR_SUCCEEDED(res))
            res = m_xrGetRenderModelAssetDataEXT(asset, &dataInfo, &data);
         model.asset = glb;
         vector<XrRenderModelAssetNodePropertiesEXT> nodes(properties.animatableNodeCount);
         XrRenderModelAssetPropertiesGetInfoEXT assetPropertiesInfo { XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_GET_INFO_EXT };
         XrRenderModelAssetPropertiesEXT assetProperties { XR_TYPE_RENDER_MODEL_ASSET_PROPERTIES_EXT };
         assetProperties.nodePropertyCount = properties.animatableNodeCount;
         assetProperties.nodeProperties = nodes.data();
         if (XR_SUCCEEDED(res))
            res = m_xrGetRenderModelAssetPropertiesEXT(asset, &assetPropertiesInfo, &assetProperties);
         for (const auto& node : nodes)
            model.animatableNodes.emplace_back(node.uniqueName, strnlen(node.uniqueName, XR_MAX_RENDER_MODEL_ASSET_NODE_NAME_SIZE_EXT));
         model.nodeStates.resize(properties.animatableNodeCount, { { { 0.f, 0.f, 0.f, 1.f }, { 0.f, 0.f, 0.f } }, true });
         m_xrDestroyRenderModelAssetEXT(asset);
      }
      if (!XR_SUCCEEDED(res))
      {
         // The asset may not be available yet (XR_ERROR_RENDER_MODEL_ASSET_UNAVAILABLE_EXT): try again a bit later
         PLOGE << "OpenXR: failed to load controller model " << model.id << ": " << GetXRErrorString(m_xrInstance, res);
         if (handle.space != XR_NULL_HANDLE)
            xrDestroySpace(handle.space);
         m_xrDestroyRenderModelEXT(handle.renderModel);
         m_controllerModelsRetryTime = static_cast<double>(usec()) * 1e-6 + 2.;
         continue;
      }
      if (getenv("VPX_DUMP_CONTROLLER_MODELS") != nullptr)
      {
         // Diagnostics: the asset as given by the runtime, to inspect it with glTF tools
         const string path = std::format("/tmp/vpx-controller-model-{}.glb", model.id);
         if (FILE* f = fopen(path.c_str(), "wb"); f)
         {
            fwrite(model.asset->data(), 1, model.asset->size(), f);
            fclose(f);
            PLOGI << "OpenXR controller model " << model.id << " written to " << path;
         }
      }
      PLOGI << "OpenXR controller model " << model.id << " loaded: " << model.asset->size() / 1024 << " KB, " << model.animatableNodes.size() << " animatable nodes";
      models.push_back(std::move(model));
      handles.push_back(handle);
   }

   // Models no longer listed
   for (const RenderModelHandles& handle : m_renderModelHandles)
   {
      if (handle.space != XR_NULL_HANDLE)
         xrDestroySpace(handle.space);
      if (handle.renderModel != XR_NULL_HANDLE)
         m_xrDestroyRenderModelEXT(handle.renderModel);
   }
   m_controllerModels = std::move(models);
   m_renderModelHandles = std::move(handles);
   #endif
}

void VRDevice::LocateControllerModels(XrTime time)
{
   #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
   const bool show = m_showControllers && m_tableCaptureView == TableCaptureView::None;
   for (size_t i = 0; i < m_controllerModels.size(); i++)
   {
      ControllerModel& model = m_controllerModels[i];
      model.located = false;
      if (!show)
         continue;
      XrSpaceLocation location { XR_TYPE_SPACE_LOCATION };
      const XrResult locateResult = xrLocateSpace(m_renderModelHandles[i].space, m_referenceSpace, time, &location);
      if (i < std::size(m_controllerModelsDebug))
         m_controllerModelsDebug[i] = { locateResult, location.locationFlags, location.pose, XR_SUCCESS, 0 };
      if (locateResult != XR_SUCCESS)
         continue;
      if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 || (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
         continue;
      model.located = true;
      model.modelToReference = PoseToMatrix(location.pose);
      if (model.nodeStates.empty())
         continue;
      m_renderModelNodeStates.resize(model.nodeStates.size());
      XrRenderModelStateGetInfoEXT stateInfo { XR_TYPE_RENDER_MODEL_STATE_GET_INFO_EXT };
      stateInfo.displayTime = time;
      XrRenderModelStateEXT state { XR_TYPE_RENDER_MODEL_STATE_EXT };
      state.nodeStateCount = static_cast<uint32_t>(m_renderModelNodeStates.size());
      state.nodeStates = m_renderModelNodeStates.data();
      const XrResult stateResult = m_xrGetRenderModelStateEXT(m_renderModelHandles[i].renderModel, &stateInfo, &state);
      if (XR_SUCCEEDED(stateResult))
         for (size_t j = 0; j < model.nodeStates.size(); j++)
            model.nodeStates[j] = { m_renderModelNodeStates[j].nodePose, m_renderModelNodeStates[j].isVisible == XR_TRUE };
      if (i < std::size(m_controllerModelsDebug))
      {
         m_controllerModelsDebug[i].stateResult = stateResult;
         m_controllerModelsDebug[i].visibleNodes = static_cast<int>(std::ranges::count_if(model.nodeStates, [](const ControllerModel::NodeState& n) { return n.visible; }));
      }
   }
   #endif
}

void VRDevice::DestroyControllerModels()
{
   #if defined(XR_EXT_render_model) && defined(XR_EXT_interaction_render_model)
   for (const RenderModelHandles& handle : m_renderModelHandles)
   {
      if (handle.space != XR_NULL_HANDLE)
         xrDestroySpace(handle.space);
      if (handle.renderModel != XR_NULL_HANDLE)
         m_xrDestroyRenderModelEXT(handle.renderModel);
   }
   #endif
   m_renderModelHandles.clear();
   m_controllerModels.clear();
}

// Stop the session the way OpenXR expects it: ask the runtime to stop it and keep the frame loop running (without any layer) until it does,
// then end it. Destroying a running session works too, but some runtimes then consider that the application crashed or quit.
void VRDevice::EndSession()
{
   if (!m_sessionRunning)
      return;
   m_exitRequested = true;
   OPENXR_CHECK(xrRequestExitSession(m_session), "Failed to request the end of the session.");
   const uint64_t start = usec();
   constexpr uint64_t timeout = 2000000; // 2s
   while (usec() - start < timeout)
   {
      PollEvents(); // Ends the session (xrEndSession) when the runtime reports it as stopping
      if (!m_sessionRunning)
         break;
      XrFrameState frameState { XR_TYPE_FRAME_STATE };
      constexpr XrFrameWaitInfo frameWaitInfo { XR_TYPE_FRAME_WAIT_INFO, nullptr };
      if (!XR_SUCCEEDED(xrWaitFrame(m_session, &frameWaitInfo, &frameState)))
         break;
      constexpr XrFrameBeginInfo frameBeginInfo { XR_TYPE_FRAME_BEGIN_INFO, nullptr };
      if (!XR_SUCCEEDED(xrBeginFrame(m_session, &frameBeginInfo)))
         break;
      XrFrameEndInfo frameEndInfo { XR_TYPE_FRAME_END_INFO };
      frameEndInfo.displayTime = frameState.predictedDisplayTime;
      frameEndInfo.environmentBlendMode = m_environmentBlendMode;
      frameEndInfo.layerCount = 0;
      frameEndInfo.layers = nullptr;
      OPENXR_CHECK(xrEndFrame(m_session, &frameEndInfo), "Failed to end the XR frame while stopping the session.");
   }
   if (m_sessionRunning)
   {
      PLOGW << "OpenXR session was not stopped by the runtime in time, it is destroyed while running";
      return;
   }
   // Let the runtime report the following states of this session (idle, then exiting since we asked for it) while it still exists
   while (usec() - start < timeout && m_sessionState != XR_SESSION_STATE_IDLE && m_sessionState != XR_SESSION_STATE_EXITING)
   {
      PollEvents();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
   }
   PLOGI << "OpenXR session stopped in " << ((usec() - start) / 1000) << "ms";
}

// Polls the events of the instance which are not related to the current session (between sessions, or before creating one)
void VRDevice::DrainEvents()
{
   XrEventDataBuffer eventData { XR_TYPE_EVENT_DATA_BUFFER };
   while (xrPollEvent(m_xrInstance, &eventData) == XR_SUCCESS)
   {
      if (eventData.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
      {
         PLOGW << "OPENXR: Instance Loss Pending";
         m_lost = true;
      }
      else if (eventData.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
      {
         PLOGI << "OPENXR: State " << reinterpret_cast<const XrEventDataSessionStateChanged*>(&eventData)->state << " of a previous session ignored";
      }
      eventData = { XR_TYPE_EVENT_DATA_BUFFER };
   }
}

void VRDevice::ReleaseSession()
{
   assert(m_session);

   EndSession();

   // Performance counters off before the session goes: SteamVR keeps their enabled state with the instance, destroys the timestamp query
   // pools it created for them with the session, and reads those pools again from the next session's xrEndFrame when they were left enabled
   // (crash in the driver on the Steam Frame, SteamVR 2.17.10, when a table was played after the lobby)
   if (m_performanceCountersEnabled)
   {
      XrPerformanceMetricsStateMETA state { XR_TYPE_PERFORMANCE_METRICS_STATE_META };
      state.enabled = XR_FALSE;
      OPENXR_CHECK(m_xrSetPerformanceMetricsStateMETA(m_session, &state), "Failed to disable performance counters.");
      m_performanceCountersEnabled = false;
      m_performanceCounters.clear();
      m_gpuFrameTimePath = XR_NULL_PATH;
   }

   // The action spaces of the controllers, then the input handler which owns their actions
   m_gazeRayValid = m_gazePointValid = false;
   for (XrSpace* space : { &m_leftControllerSpace, &m_rightControllerSpace, &m_leftAimSpace, &m_rightAimSpace, &m_gazeSpace })
   {
      if (*space != XR_NULL_HANDLE)
         OPENXR_CHECK(xrDestroySpace(*space), "Failed to destroy Controller Space.");
      *space = XR_NULL_HANDLE;
   }
   g_pplayer->m_pininput.RemoveInputHandler(m_xrInputHandler);
   m_xrInputHandler = nullptr;

   // Destroy the swapchian render targets, and color/depth image views
   m_swapchainRenderTargets.clear();
   {
      // The density map textures (the same handle may serve several images)
      std::set<uint16_t> destroyed;
      for (const auto& texture : m_colorSwapchainInfo.foveationTextures)
         if (bgfx::isValid(texture) && destroyed.insert(texture.idx).second)
            bgfx::destroy(texture);
      m_colorSwapchainInfo.foveationTextures.clear();
      if (bgfx::isValid(m_ownFoveationMap))
      {
         bgfx::destroy(m_ownFoveationMap);
         m_ownFoveationMap = BGFX_INVALID_HANDLE;
      }
      if (bgfx::isValid(m_shadingRateMap))
      {
         bgfx::destroy(m_shadingRateMap);
         m_shadingRateMap = BGFX_INVALID_HANDLE;
      }
      m_shadingRateMapWidth = m_shadingRateMapHeight = 0;
      m_foveationShadingRate = false;
      m_gazeMarker = nullptr;
   }
   for (const auto& imageView : m_colorSwapchainInfo.imageViews)
      bgfx::destroy(imageView);
   for (const auto& imageView : m_depthSwapchainInfo.imageViews)
      bgfx::destroy(imageView);

   // Free the Swapchain Image Data.
   if (m_colorSwapchainInfo.swapchain)
      m_backend->FreeSwapchainImageData(m_colorSwapchainInfo.swapchain);
   if (m_depthSwapchainInfo.swapchain)
      m_backend->FreeSwapchainImageData(m_depthSwapchainInfo.swapchain);

   // Destroy the foveation profile, then the swapchains.
   if (m_foveationProfile != XR_NULL_HANDLE)
   {
      OPENXR_CHECK(m_xrDestroyFoveationProfileFB(m_foveationProfile), "Failed to destroy foveation profile");
      m_foveationProfile = XR_NULL_HANDLE;
      m_foveationApplied = false;
   }
   if (m_colorSwapchainInfo.swapchain)
      OPENXR_CHECK(xrDestroySwapchain(m_colorSwapchainInfo.swapchain), "Failed to destroy Color Swapchain");
   if (m_depthSwapchainInfo.swapchain)
      OPENXR_CHECK(xrDestroySwapchain(m_depthSwapchainInfo.swapchain), "Failed to destroy Depth Swapchain");
   m_colorSwapchainInfo = {};
   m_depthSwapchainInfo = {};

   DiscardVisibilityMask();

   // The objects created with the session: controller models, reference space, passthrough
   DestroyControllerModels();
   m_controllerModelsDirty = true;
   m_controllerModelsRetryTime = 0.;
   if (m_referenceSpace != XR_NULL_HANDLE)
      OPENXR_CHECK(xrDestroySpace(m_referenceSpace), "Failed to destroy Space.");
   m_referenceSpace = XR_NULL_HANDLE;
   if (m_passthroughLayer != XR_NULL_HANDLE)
   {
      PFN_xrDestroyPassthroughLayerFB xrDestroyPassthroughLayerFB;
      OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrDestroyPassthroughLayerFB", (PFN_xrVoidFunction*)&xrDestroyPassthroughLayerFB), "Failed to get xrDestroyPassthroughLayerFB.");
      OPENXR_CHECK(xrDestroyPassthroughLayerFB(m_passthroughLayer), "Failed to destroy passthrough layer.");
      m_passthroughLayer = XR_NULL_HANDLE;
   }
   if (m_passthrough != XR_NULL_HANDLE)
   {
      PFN_xrDestroyPassthroughFB xrDestroyPassthroughFB;
      OPENXR_CHECK(xrGetInstanceProcAddr(m_xrInstance, "xrDestroyPassthroughFB", (PFN_xrVoidFunction*)&xrDestroyPassthroughFB), "Failed to get xrDestroyPassthroughFB.");
      OPENXR_CHECK(xrDestroyPassthroughFB(m_passthrough), "Failed to destroy passthrough.");
      m_passthrough = XR_NULL_HANDLE;
   }
   m_passthroughEnabled = false;

   OPENXR_CHECK(xrDestroySession(m_session), "Failed to destroy Session.");
   m_session = XR_NULL_HANDLE;
   m_sessionState = XR_SESSION_STATE_UNKNOWN;
   m_sessionRunning = false;
   m_uiPanelPlaced = false;
}

// The runtime stopped the VR experience: close the table (without capturing its image, which needs the session). When it asks the application
// to exit, also leave the launcher instead of getting back to the lobby.
static void CloseTableOnRuntimeRequest(bool exitApplication)
{
   #ifdef __STANDALONE__
   if (exitApplication)
      g_app->m_launcherMode = false;
   #endif
   if (g_pplayer)
      g_pplayer->SetCloseState(Player::CS_CLOSE_APP);
}

void VRDevice::PollEvents()
{
   assert(m_session);

   // Poll OpenXR for a new event.
   XrEventDataBuffer eventData { XR_TYPE_EVENT_DATA_BUFFER };
   auto XrPollEvents = [&]() -> bool
   {
      eventData = { XR_TYPE_EVENT_DATA_BUFFER };
      return xrPollEvent(m_xrInstance, &eventData) == XR_SUCCESS;
   };

   while (XrPollEvents())
   {
      switch (eventData.type)
      {
      // Log the number of lost events from the runtime.
      case XR_TYPE_EVENT_DATA_EVENTS_LOST:
      {
         XrEventDataEventsLost* eventsLost = reinterpret_cast<XrEventDataEventsLost*>(&eventData);
         PLOGI << "OPENXR: Events Lost: " << eventsLost->lostEventCount;
         break;
      }
      // Log that an instance loss is pending and shutdown the application.
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      {
         XrEventDataInstanceLossPending* instanceLossPending = reinterpret_cast<XrEventDataInstanceLossPending*>(&eventData);
         PLOGW << "OPENXR: Instance Loss Pending at: " << instanceLossPending->lossTime;
         m_sessionRunning = false;
         m_lost = true;
         CloseTableOnRuntimeRequest(false);
         break;
      }
      // Log that the interaction profile has changed.
      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
      {
         XrEventDataInteractionProfileChanged* interactionProfileChanged = reinterpret_cast<XrEventDataInteractionProfileChanged*>(&eventData);
         PLOGI << "OPENXR: Interaction Profile changed for Session: " << interactionProfileChanged->session;
         if (interactionProfileChanged->session != m_session)
         {
            PLOGI << "XrEventDataInteractionProfileChanged for unknown Session";
            break;
         }
         m_controllerModelsDirty = true;
         break;
      }
      #ifdef XR_EXT_interaction_render_model
      // The devices the player holds changed: ask again which controller models to show
      case XR_TYPE_EVENT_DATA_INTERACTION_RENDER_MODELS_CHANGED_EXT:
      {
         m_controllerModelsDirty = true;
         break;
      }
      #endif
      // Log that there's a reference space change pending.
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
      {
         XrEventDataReferenceSpaceChangePending* referenceSpaceChangePending = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&eventData);
         PLOGI << "OPENXR: Reference Space Change pending for Session: " << referenceSpaceChangePending->session;
         if (referenceSpaceChangePending->session != m_session)
         {
            PLOGI << "XrEventDataReferenceSpaceChangePending for unknown Session";
            break;
         }
         break;
      }
      case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
      {
         const XrEventDataDisplayRefreshRateChangedFB* refreshRateChanged = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&eventData);
         PLOGI << "OPENXR: Headset refresh rate changed from " << refreshRateChanged->fromDisplayRefreshRate << " Hz to " << refreshRateChanged->toDisplayRefreshRate << " Hz";
         if (g_pplayer && g_pplayer->m_playfieldWnd)
            g_pplayer->m_playfieldWnd->SetRefreshRate(refreshRateChanged->toDisplayRefreshRate);
         break;
      }
      // Session State changes:
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
      {
         XrEventDataSessionStateChanged* sessionStateChanged = reinterpret_cast<XrEventDataSessionStateChanged*>(&eventData);
         if (sessionStateChanged->session != m_session)
         {
            PLOGI << "XrEventDataSessionStateChanged for unknown Session";
            break;
         }

         if (sessionStateChanged->state == XR_SESSION_STATE_READY)
         {
            // SessionState is ready. Begin the XrSession using the XrViewConfigurationType.
            XrSessionBeginInfo sessionBeginInfo { XR_TYPE_SESSION_BEGIN_INFO };
            sessionBeginInfo.primaryViewConfigurationType = m_viewConfiguration;
            OPENXR_CHECK(xrBeginSession(m_session, &sessionBeginInfo), "Failed to begin Session.");
            m_sessionRunning = true;
            ApplyDisplayRefreshRate();
         }
         if (sessionStateChanged->state == XR_SESSION_STATE_STOPPING)
         {
            // SessionState is stopping. End the XrSession.
            OPENXR_CHECK(xrEndSession(m_session), "Failed to end Session.");
            m_sessionRunning = false;
         }
         if (sessionStateChanged->state == XR_SESSION_STATE_EXITING)
         {
            // SessionState is exiting: expected after we asked to stop the session (see EndSession), otherwise the runtime asks the
            // application to quit (for example from the dashboard of the headset), so close the table and the application.
            m_sessionRunning = false;
            if (!m_exitRequested)
            {
               PLOGI << "OPENXR: The runtime asked the application to exit";
               CloseTableOnRuntimeRequest(true);
            }
         }
         if (sessionStateChanged->state == XR_SESSION_STATE_LOSS_PENDING)
         {
            // SessionState is loss pending: close the table, the VR device will be created again for the next one (see VPApp::AcquireVRDevice)
            PLOGW << "OPENXR: Session Loss Pending";
            m_sessionRunning = false;
            m_lost = true;
            CloseTableOnRuntimeRequest(false);
         }
         // Store state for reference across the application.
         m_sessionState = sessionStateChanged->state;
         break;
      }
      // Visibility mask changed
      case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
      {
         m_visibilityMaskDirty = true;
         break;
      }
      default:
      {
         break;
      }
      }
   }
}

void VRDevice::UpdateVisibilityMask(RenderDevice* rd)
{
   assert(m_session);

   if (m_visibilityMaskExtensionSupported && m_visibilityMaskDirty)
   {
      DiscardVisibilityMask();
      uint32_t indexCount = 0, vertexCount = 0, maxVertexCount = 0;
      for (size_t view = 0; view < m_viewConfigurationViews.size(); view++)
      {
         XrVisibilityMaskKHR visibilityMask { XR_TYPE_VISIBILITY_MASK_KHR };
         xrGetVisibilityMaskKHR(m_session, m_viewConfiguration, (uint32_t)view, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &visibilityMask);
         indexCount += visibilityMask.indexCountOutput;
         vertexCount += visibilityMask.vertexCountOutput;
         maxVertexCount = max(maxVertexCount, visibilityMask.vertexCountOutput);
      }
      if ((indexCount > 0) && (vertexCount > 0))
      {
         std::shared_ptr<IndexBuffer> indexBuffer = std::make_shared<IndexBuffer>(rd, indexCount, false, IndexBuffer::FMT_INDEX32);
         std::shared_ptr<VertexBuffer> vertexBuffer = std::make_shared<VertexBuffer>(rd, vertexCount);
         XrVector2f* const vert2d = new XrVector2f[maxVertexCount];
         uint32_t* indices;
         indexBuffer->Lock(indices);
         Vertex3D_NoTex2* vertices;
         vertexBuffer->Lock(vertices);
         uint32_t vertexOffset = 0;
         for (size_t view = 0; view < m_viewConfigurationViews.size(); view++)
         {
            XrVisibilityMaskKHR visibilityMask { XR_TYPE_VISIBILITY_MASK_KHR };
            xrGetVisibilityMaskKHR(m_session, m_viewConfiguration, (uint32_t)view, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &visibilityMask);
            visibilityMask.indexCapacityInput = visibilityMask.indexCountOutput;
            visibilityMask.vertexCapacityInput = visibilityMask.vertexCountOutput;
            visibilityMask.vertices = vert2d;
            visibilityMask.indices = indices;
            xrGetVisibilityMaskKHR(m_session, m_viewConfiguration, (uint32_t)view, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &visibilityMask);
            for (uint32_t i = 0; i < visibilityMask.vertexCountOutput; i++)
            {
               vertices->x = vert2d[i].x;
               vertices->y = vert2d[i].y;
               vertices->z = -1.f;
               vertices->nx = static_cast<float>(view);
               vertices->ny = 0.f;
               vertices->nz = 0.f;
               vertices->tu = 0.f;
               vertices->tv = 0.f;
               vertices++;
            }
            for (uint32_t i = 0; i < visibilityMask.indexCountOutput; i++)
            {
               (*indices) += vertexOffset;
               indices++;
            }
            vertexOffset += visibilityMask.vertexCountOutput;
         }
         indexBuffer->Unlock();
         vertexBuffer->Unlock();
         delete[] vert2d;
         m_visibilityMask = std::make_shared<MeshBuffer>("VisibilityMask"s, vertexBuffer, indexBuffer, true);
         m_visibilityMaskDirty = false;
         PLOGI << "Headset visibility mask acquired";
      }
      else
      {
         m_visibilityMask = nullptr; 
         m_visibilityMaskDirty = false;
         PLOGI << "Headset visibility mask defined to none (empty mask returned by headset)";
      }
   }
}

void VRDevice::RenderFrame(RenderDevice* rd, const std::function<void(RenderTarget* vrRenderTarget)>& submitFrame)
{
   assert(m_session);

   bool rendered = true;
   if (!m_sessionRunning)
   {
      // FIXME we should perform preview rendering here
      submitFrame(nullptr);
      return;
   }
   LogRuntimeStatus();

   // Let OpenXR throttle frame submission and get the XrFrameState for timing and rendering info.
   #ifdef MSVC_CONCURRENCY_VIEWER
   span *tagSpanFF = new span(series, 1, _T("xrWaitFrame"));
   #endif
   g_pplayer->m_renderProfiler->EnterProfileSection(FrameProfiler::PROFILE_RENDER_FLIP);
   XrFrameState frameState { XR_TYPE_FRAME_STATE };
   constexpr XrFrameWaitInfo frameWaitInfo { XR_TYPE_FRAME_WAIT_INFO, nullptr };
   OPENXR_CHECK(xrWaitFrame(m_session, &frameWaitInfo, &frameState), "Failed to wait for XR Frame.");
   // Dynamic resolution: the scale of the frame about to be prepared, from the GPU time of the previous ones
   UpdateDynamicResolution(frameState.predictedDisplayPeriod);
   g_pplayer->m_renderProfiler->ExitProfileSection();
   #ifdef MSVC_CONCURRENCY_VIEWER
   delete tagSpanFF;
   #endif

   // Tell the OpenXR compositor that the application is beginning the frame.
   constexpr XrFrameBeginInfo frameBeginInfo { XR_TYPE_FRAME_BEGIN_INFO, nullptr };
   OPENXR_CHECK(xrBeginFrame(m_session, &frameBeginInfo), "Failed to begin the XR Frame.");

   // Variables for rendering and layer composition.
   RenderLayerInfo renderLayerInfo;
   renderLayerInfo.predictedDisplayTime = frameState.predictedDisplayTime;

   m_predictedDisplayTimestamp = static_cast<float>((double)usec() / 1000000.);
   #if BX_PLATFORM_WINDOWS
   if (m_xrConvertTimeToWin32PerformanceCounterKHR)
   {
      LARGE_INTEGER displayTime;
      m_xrConvertTimeToWin32PerformanceCounterKHR(m_xrInstance, frameState.predictedDisplayTime, &displayTime);
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      LARGE_INTEGER TimerFreq;
      QueryPerformanceFrequency(&TimerFreq);
      m_predictedDisplayTimestamp += static_cast<float>(displayTime.QuadPart - now.QuadPart) / static_cast<float>(TimerFreq.QuadPart);
   }
   #elif BX_PLATFORM_ANDROID || BX_PLATFORM_LINUX
   if (m_xrConvertTimeToTimespecTimeKHR)
   {
      timespec displayTime;
      m_xrConvertTimeToTimespecTimeKHR(m_xrInstance, frameState.predictedDisplayTime, &displayTime);
      timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      time_t sec_diff = displayTime.tv_sec - now.tv_sec;
      long long nsec_diff = displayTime.tv_nsec - now.tv_nsec;
      int64_t total_nsec = sec_diff * 1000000000LL + nsec_diff;
      m_predictedDisplayTimestamp += (float)(static_cast<double>(total_nsec) / 1000000000.0);
   }
   #endif

   // Check that the session is active and that we should render.
   const bool sessionActive = (m_sessionState == XR_SESSION_STATE_SYNCHRONIZED || m_sessionState == XR_SESSION_STATE_VISIBLE || m_sessionState == XR_SESSION_STATE_FOCUSED);
   if (!sessionActive || !frameState.shouldRender)
      rendered = false;

   if (rendered)
   {
      // Locate the views from the view configuration within the (reference) space at the predicted display time.
      std::vector<XrView> views(m_viewConfigurationViews.size(), { XR_TYPE_VIEW });

      XrViewState viewState { XR_TYPE_VIEW_STATE }; // Will contain information on whether the position and/or orientation is valid and/or tracked.
      XrViewLocateInfo viewLocateInfo { XR_TYPE_VIEW_LOCATE_INFO };
      viewLocateInfo.viewConfigurationType = m_viewConfiguration;
      viewLocateInfo.displayTime = renderLayerInfo.predictedDisplayTime;
      viewLocateInfo.space = m_referenceSpace;
      uint32_t viewCount = 0;
      const XrResult result = xrLocateViews(m_session, &viewLocateInfo, &viewState, static_cast<uint32_t>(views.size()), &viewCount, views.data());
      if (result != XR_SUCCESS)
      {
         PLOGE << "Failed to locate Views.";
         rendered = false;
      }
      if (rendered)
      {
         UpdateUIPanel(views, renderLayerInfo.predictedDisplayTime);
         if (m_controllerModelsDirty || (m_controllerModelsRetryTime > 0. && static_cast<double>(usec()) * 1e-6 > m_controllerModelsRetryTime))
            UpdateControllerModels();
         LocateControllerModels(renderLayerInfo.predictedDisplayTime);
         LocateGaze(views, renderLayerInfo.predictedDisplayTime);
      }
      if (rendered)
      {
         // The steps that leads to the matrix stack implemented below are the followings, with first matrix being view, 
         // second being projection, i.e. notation is (view) * (projection):
         // - What would be right: (sceneScale * sceneBasis * VPUToWorldScale * viewOffset * view[eye]) * (projection[eye])
         // - Single view matrix:  (sceneScale * sceneBasis * VPUToWorldScale * viewOffset * medianView) * (inv(medianView) * view[eye] * projection[eye])
         //                        (sceneScale * sceneBasis * viewOffsetInVPU * medianViewInVPU) * (inv(medianViewInVPU) * VPUToWorldScale * view[eye] * projection[eye])
         // - Orthonormal shading: (sceneBasis * viewOffsetInVPU * medianViewInVPU) * (inv(sceneBasis * viewOffsetInVPU * medianViewInVPU) * sceneScale * sceneBasis * viewOffsetInVPU * VPUToWorldScale * view[eye] * projection[eye])
         // - Therefore, we define:
         //   . m_pfWorld = sceneBasis * viewOffsetInVPU
         //   . m_pfMatView = m_pfWorld * medianViewInVPU
         //   . m_pfMatProj[eye] = inv(m_pfMatView) * sceneScale * m_pfWorld * VPUToWorldScale * view[eye] * projection[eye]
         // TODO Note that this is not the way it should be done: shading should use each eye view matrix, leading to slightly different shading per eye
         constexpr float vpuToWorldScale = static_cast<float>(0.0254 * 1.0625 / 50.); // VPU to meters
         constexpr float zNear = 0.1f; // 10cm in front of player
         const float zFar = max(5.f, m_sceneSize * vpuToWorldScale); // This could be fairly optimized for better accuracy (as well as use an optimized depth buffer for rendering)

         // Fixed value of 5 cm between playfield bottom and lockbar border
         // We could (should ?) make this a table data but this does not vary that much so this seems fine for the time being
         constexpr float lockbarToPlayfield = 5.f;

         // Table image capture: the eyes are moved to frame the table (the composited frame then matches what was rendered)
         if (const TableCaptureView captureView = m_tableCaptureView; captureView != TableCaptureView::None)
            SetTableCaptureViewPoses(views, vpuToWorldScale, captureView);

         // Continuous space positioning based on controller pose (for setup where controllers can be placed along a VR cabinet and used for XR play)
         if (m_controllerViewCentering)
         {
            bool leftControllerActive = false;
            XrSpaceLocation leftSpaceLocation { XR_TYPE_SPACE_LOCATION };
            if (m_leftControllerSpace != XR_NULL_HANDLE && xrLocateSpace(m_leftControllerSpace, m_referenceSpace, renderLayerInfo.predictedDisplayTime, &leftSpaceLocation) == XR_SUCCESS)
               leftControllerActive = leftSpaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT;

            bool rightControllerActive = false;
            XrSpaceLocation rightSpaceLocation { XR_TYPE_SPACE_LOCATION };
            if (m_rightControllerSpace != XR_NULL_HANDLE && xrLocateSpace(m_rightControllerSpace, m_referenceSpace, renderLayerInfo.predictedDisplayTime, &rightSpaceLocation) == XR_SUCCESS)
               rightControllerActive = rightSpaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT;

            if (leftControllerActive && rightControllerActive)
            {
               const PinTable* const table = g_pplayer->m_ptable;

               const vec3 rightPos = vec3(rightSpaceLocation.pose.position.x, rightSpaceLocation.pose.position.y, rightSpaceLocation.pose.position.z);
               const vec3 leftPos = vec3(leftSpaceLocation.pose.position.x, leftSpaceLocation.pose.position.y, leftSpaceLocation.pose.position.z);
               const vec3 centerPos = -(rightPos + leftPos) * (float)(0.5 * 100.);
               const vec3 lockbarAxis = rightPos - leftPos;
               const float lockbarAngle = atan2f(-lockbarAxis.z, lockbarAxis.x);
               m_headsetViewCentering = false;
               m_lockbarWidth = lockbarAxis.Length() * 100.f * table->m_settings.GetPlayerVR_ControllerLockbarScale();
               
               // Update fixed scaling, considering lockbar size to be the width of the playfield + 2"1/4
               const float tableWidth = VPUTOCM(table->m_right - table->m_left) + 2.25f * 2.54f;
               const float scale = clamp(m_lockbarWidth / tableWidth, 0.1f, 2.0f);

               const float c = cosf(lockbarAngle);
               const float s = sinf(lockbarAngle);
               const float dx = centerPos.x;
               const float dy = centerPos.z;
               m_lockbarHeight = -centerPos.y;
               m_orientation = RADTOANG(lockbarAngle);
               m_tablePos.x = dx * c - dy * s;
               m_tablePos.y = dx * s + dy * c + table->m_settings.GetPlayerVR_ControllerCabYOffset() + lockbarToPlayfield * scale;
               m_tablePos.z = 0.f;
               m_worldDirty = true;
            }
         }

         // Space positioning based on head pose (not continuous)
         if (m_headsetViewCentering)
         {
            // Compute the eye median pose in VPU coordinates
            Matrix3D medianView;
            XrPosef medianPoseInVPU;
            XrVector3f_Lerp(&medianPoseInVPU.position, &views[0].pose.position, &views[1].pose.position, 0.5f);
            XrQuaternionf_Lerp(&medianPoseInVPU.orientation, &views[0].pose.orientation, &views[1].pose.orientation, 0.5f);
            XrVector3f_Scale(&medianPoseInVPU.position, &medianPoseInVPU.position, 1.f / vpuToWorldScale); // Convert position from meters to VPU
            XrPosef_ToMatrix3D(&medianView, &medianPoseInVPU);

            m_headsetViewCentering = false;
            const float angle = atan2f(medianView.m[0][2], medianView.m[0][0]);
            const float c = cosf(angle);
            const float s = sinf(angle);
            const float dx = -VPUTOCM(medianPoseInVPU.position.x);
            const float dy = -VPUTOCM(medianPoseInVPU.position.z);
            const Settings& settings = g_pplayer->m_ptable->m_settings;

            // Rotate the tracking-space translation into the table's yaw frame, just as
            // controller centering does. Player X/Y is the desired standing position
            // relative to the lockbar, so add it after transforming the headset pose.
            m_orientation = RADTOANG(angle);
            m_tablePos.x = dx * c - dy * s + settings.GetPlayer_ScreenPlayerX();
            m_tablePos.y = dx * s + dy * c + settings.GetPlayer_ScreenPlayerY();
            m_tablePos.z = 0.f;
            //m_tablePos.z = abs(m_tablePos.z) > 10.f ? 0.f : m_tablePos.z; // Keep user custom offset except if it seems out of normal range
            m_worldDirty = true;
         }

         // Prepare the view/projection matrices for the 4 space references we use:
         // - Room is the VR room, offseted and rotated to the table orientation (rotation around vertical axis)
         // - Cabinet feet is the same as the room with cabinet scaling applied (to match lockbar width)
         // - Cabinet is the same as the cabinet feet but also applying depth offset (to match lockbar height)
         // - Playfield is the main space reference where the simulation happens (only one to support physics), relative to the cabinet, with inclination and table coordinate system

         // playfield is inclined at a fixed slope that should be defined with the table (it corresponds to the cabinet design),
         // then player adjust the play angle by adjusting the feet casters, so the whole cab rotates.
         const PinTable* const table = g_pplayer->m_ptable;
         if (const float liveSlope = table->GetPlayfieldSlope(); m_worldDirty || m_slope != liveSlope)
         {
            m_worldDirty = false;
            m_slope = liveSlope;

            // Update fixed scaling, considering lockbar size to be the width of the playfield + 2"1/4
            const float tableWidth = VPUTOCM(table->m_right - table->m_left) + 2.25f * 2.54f;
            m_scale = clamp(m_lockbarWidth / tableWidth, 0.1f, 2.0f);
            const Matrix3D sceneScale = Matrix3D::MatrixScale(m_scale);
            const Matrix3D invSceneScale = Matrix3D::MatrixScale(1.f / m_scale);

            // Move table (in VPU coordinates), adjust coord from RH to LH system
            const Matrix3D coords = Matrix3D::MatrixScale(1.f, -1.f, 1.f);
            const Matrix3D rotz = Matrix3D::MatrixRotateZ(ANGTORAD(m_orientation));
            const Matrix3D rotx2 = Matrix3D::MatrixRotateX(ANGTORAD(-90.f));
            const Matrix3D viewOrientation = rotz * rotx2;
            const Matrix3D viewOrientationInv = Matrix3D::MatrixInverse(viewOrientation);

            // The users define in their settings the real world height where they want the top of the lockbar to be.
            // The real world playfield height is then computed by removing the glass distance at the playfield bottom (typically 2 to 3").
            // The m_tablePos allows to slightly adjust this height on a per table basis.
            // In the end Playfield height = m_tablePos.z (User adjustement) + m_lockBatHeight (Real world lockbar height) - glass distance * scale
            const float scaledGlassHeight = m_scale * VPUTOCM(table->m_glassBottomHeight);

            // The table defines the height of the lockbar of its cabinet model, as well as the playfield base inclination.
            // This allows to fit the cabinet & playfield models to the real world space.
            // If user adjust the inclination, then the cab is rotated as it would in real life (still missing the legs stretching a bit using caster adjustments)
            const float groundToPlayfieldHeight = m_scale * (table->m_groundToLockbarHeight - table->m_glassBottomHeight);
            const float baseSlope = lerp(table->m_angletiltMin, table->m_angletiltMax, table->m_difficulty);

            // Before 10.8.1, there weren't multiple space reference, so room used to be inclined to compensate the playfield inclination.
            // This may leads to slight visual artefact for old VR room (that is to say very slightly inclined room).
            const Matrix3D playfieldSlope = Matrix3D::MatrixRotateX(ANGTORAD(liveSlope));
            const Matrix3D playfieldSlopeInv = Matrix3D::MatrixRotateX(-ANGTORAD(liveSlope));
            const Matrix3D tableCoords = Matrix3D::MatrixTranslate(
               - m_scale * (table->m_right - table->m_left) * 0.5f,
               + m_scale * (table->m_bottom - table->m_top),
               0.f);
            const Matrix3D playfieldPos = Matrix3D::MatrixTranslate(
               -CMTOVPU(m_tablePos.x),
                CMTOVPU(m_tablePos.y + lockbarToPlayfield),
                CMTOVPU(m_tablePos.z + m_lockbarHeight - scaledGlassHeight)); 
            const Matrix3D playfieldPosInv = Matrix3D::MatrixInverse(playfieldPos);
            m_pfWorld.m_toWorld = sceneScale * coords * tableCoords * playfieldSlope * playfieldPos * viewOrientation;

            const Matrix3D cabinetSlope = playfieldPosInv * Matrix3D::MatrixRotateX(ANGTORAD(liveSlope - baseSlope)) * playfieldPos;
            const Matrix3D pfToCab = viewOrientationInv // Revert view orientation
               * playfieldPosInv * playfieldSlopeInv // Revert playfield slope
               * Matrix3D::MatrixTranslate(
                  -CMTOVPU(m_tablePos.x),
                   CMTOVPU(m_tablePos.y + lockbarToPlayfield),
                   // Cabinet model has its z origin at the feet level, m_groundToLockbarHeight corresponding to the playfield level, so we move it down (to real world ground) then up to match user seyup (where we placed the playfield)
                   CMTOVPU(m_tablePos.z + m_lockbarHeight - scaledGlassHeight) - groundToPlayfieldHeight)
               * cabinetSlope // Apply cabinet slope
               * viewOrientation; // Reapply view orientation
            m_cabWorld.m_toWorld = m_pfWorld.m_toWorld * pfToCab;

            // Feet are always touching the ground, scaled against the real world vs model defined playfield level
            // Note that since we are rotating the cabinet with its feet, the feet may slightly leave or enter the ground.
            if (m_lockFeetToGround)
            {
               constexpr float cabHeight
                  = 0.4f * INCHESTOVPU(16.25f); // Magic value where the top of the leg should approximately stand (16.25" is the front height of a modern Stern cabinet)
               const float feetScale = (CMTOVPU(m_tablePos.z + m_lockbarHeight) / m_scale - cabHeight) / (table->m_groundToLockbarHeight - cabHeight);
               //const float feetScale = (CMTOVPU(m_tablePos.z + m_lockbarHeight) / m_scale - table->m_glassBottomHeight) / (table->m_groundToLockbarHeight - table->m_glassBottomHeight);
               //const float feetScale = (m_tablePos.z + m_lockbarHeight - scaledGlassHeight) / VPUTOCM(groundToPlayfieldHeight);
               const Matrix3D pfToFeet = viewOrientationInv // Revert view orientation
                  * playfieldPosInv * playfieldSlopeInv // Revert playfield slope
                  * Matrix3D::MatrixTranslate(-CMTOVPU(m_tablePos.x), CMTOVPU(m_tablePos.y + lockbarToPlayfield),
                     CMTOVPU(m_tablePos.z)) // Feets are always at z=0 in real world, that is to say ground
                  * Matrix3D::MatrixScale(1.f, 1.f, feetScale) // Scale feets in order to match feet bottom to real world floor
                  * cabinetSlope // Apply cabinet slope
                  * viewOrientation; // Reapply view orientation
               m_feetWorld.m_toWorld = m_pfWorld.m_toWorld * pfToFeet;
            }
            else
            {
               m_feetWorld.m_toWorld = m_cabWorld.m_toWorld;
            }

            // Room does not apply the cabinet scaling nor any inclination, as it is the real world room
            const Matrix3D pfToRoom = viewOrientationInv // Revert view orientation
               * playfieldPosInv * playfieldSlopeInv // Revert playfield slope
               * Matrix3D::MatrixTranslate( // Apply table coordinate but without table scale
                   - (1.f - m_scale) * (table->m_right - table->m_left) * 0.5f,
                   + (1.f - m_scale) * (table->m_bottom - table->m_top),
                   0.f)
               * Matrix3D::MatrixTranslate(
                  -CMTOVPU(m_tablePos.x), // For the ease of positioning, align the room to the table view setting, except for z which must stay on ground
                   CMTOVPU(m_tablePos.y + lockbarToPlayfield),
                   CMTOVPU(m_tablePos.z))
               * viewOrientation; // Reapply view orientation
            m_roomWorld.m_toWorld = invSceneScale * m_pfWorld.m_toWorld * pfToRoom;

            // The eye views place the room with (room VPU) * m_roomWorld.m_toWorld * (VPU to meters) * (eye view in meters), see below
            m_referenceToRoom = Matrix3D::MatrixScale(1.f / vpuToWorldScale) * Matrix3D::MatrixInverse(m_roomWorld.m_toWorld);
         }

         // As we only have one view matrix for shading, each eye view is integrated in the projection matrix, by reverting the 'shading' view matrix then
         // applying the eye view matrix, we also apply scale in the projection matrix as it would break shading otherwise (it needs an orthonormal view matrix)

         const Matrix3D vpuScale = Matrix3D::MatrixScale(vpuToWorldScale);
         const Matrix3D invVpuScale = Matrix3D::MatrixScale(1.f / vpuToWorldScale);
         const Matrix3D sceneScale = Matrix3D::MatrixScale(m_scale);
         const Matrix3D invSceneScale = Matrix3D::MatrixScale(1.f / m_scale);

         for (unsigned int eye = 0; eye < 2; eye++)
         {
            Matrix3D view;
            XrPosef_ToMatrix3D(&view, &views[eye].pose);
            view = vpuScale * view * invVpuScale;
            m_nextProj[eye].SetPerspectiveFovRH(views[eye].fov.angleLeft, views[eye].fov.angleRight, views[eye].fov.angleDown, views[eye].fov.angleUp, zNear, zFar);
            m_viewFov[eye] = views[eye].fov;

            // View is per eye, must be orthonormal (so every scale must be compensated)
            const Matrix3D viewInvSceneScale = view * invSceneScale;
            m_pfWorld.m_view[eye] = m_pfWorld.m_toWorld * viewInvSceneScale;
            m_cabWorld.m_view[eye] = m_cabWorld.m_toWorld * viewInvSceneScale;
            m_feetWorld.m_view[eye] = m_feetWorld.m_toWorld * viewInvSceneScale;
            m_roomWorld.m_view[eye] = m_roomWorld.m_toWorld * view;

            m_roomProj[eye] = vpuScale * m_nextProj[eye];
            m_sceneProj[eye] = sceneScale * m_roomProj[eye];
         }

         // Swapchain is acquired, rendered to, and released together for all views as a texture array

         // Resize the layer projection views to match the view count. The layer projection views are used in the layer projection.
         renderLayerInfo.layerProjectionViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW });
         if (m_depthExtensionSupported)
            renderLayerInfo.depthInfoViews.resize(viewCount, { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR });

         // Acquire and wait for an image from the swapchains (the timeout is infinite)
         uint32_t colorImageIndex = 0;
         uint32_t depthImageIndex = 0;
         constexpr XrSwapchainImageAcquireInfo acquireInfo { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO, nullptr };
         OPENXR_CHECK(xrAcquireSwapchainImage(m_colorSwapchainInfo.swapchain, &acquireInfo, &colorImageIndex), "Failed to acquire Image from the Color Swapchian");
         OPENXR_CHECK(xrAcquireSwapchainImage(m_depthSwapchainInfo.swapchain, &acquireInfo, &depthImageIndex), "Failed to acquire Image from the Depth Swapchian");
         #if defined(ENABLE_BGFX) && defined(BGFX_RESOLVE_FRAGMENT_DENSITY_MAP)
         // Foveated rendering: the scene is rendered with the density map the runtime keeps for the acquired image (eye-tracked when the headset allows it)
         if (g_pplayer->m_renderer)
         {
            // The shading rate image where the driver has it (see UpdateShadingRateMap), else our own density map when offsets are
            // available (the runtime's maps cannot take them), else the runtime's map of the acquired image
            bgfx::TextureHandle map = BGFX_INVALID_HANDLE;
            bool shadingRate = false;
            const bool gazeValid = m_foveationMode != 0 && ReadGaze();
            if (m_foveationMode != 0)
            {
               if (UpdateShadingRateMap(gazeValid))
               {
                  map = m_shadingRateMap;
                  shadingRate = true;
               }
               else if (bgfx::isValid(m_ownFoveationMap))
                  map = m_ownFoveationMap;
               else if (colorImageIndex < m_colorSwapchainInfo.foveationTextures.size())
                  map = m_colorSwapchainInfo.foveationTextures[colorImageIndex];
            }
            m_foveationShadingRate = shadingRate;
            g_pplayer->m_renderer->SetFragmentDensityMap(map, shadingRate);
            if (!shadingRate)
            {
               // The density map has its high density area in the middle: the gaze moves it through offsets, in pixels of the scene buffer
               int32_t offsets[4] = { 0, 0, 0, 0 };
               if (gazeValid && bgfx::isValid(map) && map.idx == m_ownFoveationMap.idx && m_backend->IsFragmentDensityMapOffsetSupported())
               {
                  const RenderTarget* const scene = g_pplayer->m_renderer->GetBackBufferTexture();
                  const float halfW = 0.5f * static_cast<float>(scene->GetWidth()), halfH = 0.5f * static_cast<float>(scene->GetHeight());
                  for (int eye = 0; eye < 2; eye++)
                  {
                     offsets[eye * 2 + 0] = static_cast<int32_t>(lroundf((m_foveationFlipX ? -1.f : 1.f) * m_foveationCenter[eye].x * halfW));
                     offsets[eye * 2 + 1] = static_cast<int32_t>(lroundf((m_foveationFlipY ? -1.f : 1.f) * m_foveationCenter[eye].y * halfH));
                  }
               }
               g_pplayer->m_renderer->SetFragmentDensityMapOffsets(offsets, 2);
            }
         }
         #endif

         XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
         waitInfo.timeout = XR_INFINITE_DURATION;
         OPENXR_CHECK(xrWaitSwapchainImage(m_colorSwapchainInfo.swapchain, &waitInfo), "Failed to wait for Image from the Color Swapchain");
         OPENXR_CHECK(xrWaitSwapchainImage(m_depthSwapchainInfo.swapchain, &waitInfo), "Failed to wait for Image from the Depth Swapchain");

         // Use the full range of recommended image size to achieve optimum resolution
         const XrRect2Di imageRect = { { 0, 0 }, { (int32_t)m_colorSwapchainInfo.width, (int32_t)m_colorSwapchainInfo.height } };
         assert(m_colorSwapchainInfo.width == m_depthSwapchainInfo.width);
         assert(m_colorSwapchainInfo.height == m_depthSwapchainInfo.height);

         // Fill out the XrCompositionLayerProjectionView structure specifying the pose and fov from the view.
         for (uint32_t i = 0; i < viewCount; i++)
         {
            renderLayerInfo.layerProjectionViews[i] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
            renderLayerInfo.layerProjectionViews[i].pose = views[i].pose;
            renderLayerInfo.layerProjectionViews[i].fov = views[i].fov;
            renderLayerInfo.layerProjectionViews[i].subImage.swapchain = m_colorSwapchainInfo.swapchain;
            renderLayerInfo.layerProjectionViews[i].subImage.imageRect = imageRect;
            renderLayerInfo.layerProjectionViews[i].subImage.imageArrayIndex = i;
            if (m_depthExtensionSupported)
            {
               renderLayerInfo.depthInfoViews[i] = { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR };
               renderLayerInfo.depthInfoViews[i].minDepth = 0;
               renderLayerInfo.depthInfoViews[i].maxDepth = 1;
               renderLayerInfo.depthInfoViews[i].nearZ = zNear;
               renderLayerInfo.depthInfoViews[i].farZ = zFar;
               renderLayerInfo.depthInfoViews[i].subImage.swapchain = m_depthSwapchainInfo.swapchain;
               renderLayerInfo.depthInfoViews[i].subImage.imageRect = imageRect;
               renderLayerInfo.depthInfoViews[i].subImage.imageArrayIndex = i;
               renderLayerInfo.layerProjectionViews[i].next = &renderLayerInfo.depthInfoViews[i];
            }
         }

         // Prepare frame with the acquired views to limit position-visual latency (limit motion sickness)
         // This can't be done earlier since view acquisition is done for the predicted frame display time (which we have only after xrWaitFrame) 
         // and we also need OpenXR to selected color/depth render target from xrAcquireSwapchainImage
         RenderTarget* vrRenderTarget = m_swapchainRenderTargets[colorImageIndex + depthImageIndex * m_colorSwapchainInfo.imageViews.size()].get();
         if (vrRenderTarget == nullptr)
         {
            const uint16_t nViews = static_cast<uint16_t>(m_viewConfigurationViews.size());
            bgfx::Attachment colorAttachment, depthAttachment;
            colorAttachment.init(m_colorSwapchainInfo.imageViews[colorImageIndex], bgfx::Access::Write, 0, nViews, 0, BGFX_RESOLVE_NONE);
            depthAttachment.init(m_depthSwapchainInfo.imageViews[depthImageIndex], bgfx::Access::Write, 0, nViews, 0, BGFX_RESOLVE_NONE);
            const bgfx::Attachment attachments[] = { colorAttachment, depthAttachment };
            const bgfx::FrameBufferHandle fbh = bgfx::createFrameBuffer(2, attachments);
            m_swapchainRenderTargets[colorImageIndex + depthImageIndex * m_colorSwapchainInfo.imageViews.size()]
               = std::make_unique<RenderTarget>(rd, SurfaceType::RT_STEREO, fbh, colorAttachment.handle, m_colorSwapchainInfo.format, depthAttachment.handle, m_depthSwapchainInfo.format,
                  std::format("VRSwapchain [{}/{}]", colorImageIndex, depthImageIndex), m_colorSwapchainInfo.width, m_colorSwapchainInfo.height, colorFormat::RGBA);
            vrRenderTarget = m_swapchainRenderTargets[colorImageIndex + depthImageIndex * m_colorSwapchainInfo.imageViews.size()].get();
            vrRenderTarget->m_dynamicResolution = true; // The frame draws its top left part at the dynamic resolution scale (see above)
         }
         submitFrame(vrRenderTarget);

         // Dynamic resolution: the frame drew the top left part of the swapchain image (see RenderDevice::BeginScaledRendering), tell the
         // compositor to sample that part only. The depth image was copied over the same part.
         if (const float executedScale = rd->GetExecutedRenderScale(); executedScale < 1.f)
         {
            const XrRect2Di drawnRect = { { 0, 0 }, { vrRenderTarget->GetScaledWidth(executedScale), vrRenderTarget->GetScaledHeight(executedScale) } };
            for (uint32_t i = 0; i < viewCount; i++)
            {
               renderLayerInfo.layerProjectionViews[i].subImage.imageRect = drawnRect;
               if (m_depthExtensionSupported)
                  renderLayerInfo.depthInfoViews[i].subImage.imageRect = drawnRect;
            }
         }

         // Fill out the XrCompositionLayerProjection structure for usage with xrEndFrame().
         renderLayerInfo.layerProjection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT;
         renderLayerInfo.layerProjection.space = m_referenceSpace;
         renderLayerInfo.layerProjection.viewCount = static_cast<uint32_t>(renderLayerInfo.layerProjectionViews.size());
         renderLayerInfo.layerProjection.views = renderLayerInfo.layerProjectionViews.data();

         // Give the swapchain image back to OpenXR, allowing the compositor to use the image.
         constexpr XrSwapchainImageReleaseInfo releaseInfo { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO, nullptr };
         OPENXR_CHECK(xrReleaseSwapchainImage(m_colorSwapchainInfo.swapchain, &releaseInfo), "Failed to release Image back to the Color Swapchain");
         OPENXR_CHECK(xrReleaseSwapchainImage(m_depthSwapchainInfo.swapchain, &releaseInfo), "Failed to release Image back to the Depth Swapchain");

         // Add passthrough layer first (background)
         if (m_passthroughEnabled && m_passthroughLayer != XR_NULL_HANDLE)
         {
            renderLayerInfo.layerPassthrough.layerHandle = m_passthroughLayer;
            renderLayerInfo.layerPassthrough.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            renderLayerInfo.layerPassthrough.space = XR_NULL_HANDLE;
            renderLayerInfo.layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&renderLayerInfo.layerPassthrough));
         }

         renderLayerInfo.layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&renderLayerInfo.layerProjection));
      }
   }

   // Tell OpenXR that we are finished with this frame; specifying its display time, environment blending and layers.
   XrFrameEndInfo frameEndInfo { XR_TYPE_FRAME_END_INFO };
   frameEndInfo.displayTime = frameState.predictedDisplayTime;
   frameEndInfo.environmentBlendMode = m_environmentBlendMode;
   frameEndInfo.layerCount = static_cast<uint32_t>(renderLayerInfo.layers.size());
   frameEndInfo.layers = renderLayerInfo.layers.data();
   OPENXR_CHECK(xrEndFrame(m_session, &frameEndInfo), "Failed to end the XR Frame.");

   // Perform preview window rendering only
   if (!rendered)
      submitFrame(nullptr);
}
#endif

void VRDevice::OffsetTable(float dx, float dy, float dz)
{
   m_tablePos.x = clamp(m_tablePos.x + dx, -100.0f, 100.0f);
   m_tablePos.y = clamp(m_tablePos.y + dy, -100.0f, 100.0f);
   m_tablePos.z = clamp(m_tablePos.z + dz, -100.0f, 100.0f);
   m_worldDirty = true;
}

void VRDevice::UpdateVRPosition(PartGroupData::SpaceReference spaceRef, ModelViewProj& mvp)
{
   using enum PartGroupData::SpaceReference;

   const Matrix3D* viewpoint = nullptr;
   const Matrix3D* proj = nullptr;
   switch (spaceRef)
   {
   case SR_PLAYFIELD: viewpoint = m_pfWorld.m_view; proj = m_sceneProj; break;
   case SR_CABINET: viewpoint = m_cabWorld.m_view; proj = m_sceneProj; break;
   case SR_CABINET_FEET: viewpoint = m_feetWorld.m_view; proj = m_sceneProj; break;
   case SR_ROOM: viewpoint = m_roomWorld.m_view; proj = m_roomProj; break;
   default: assert(false); return;
   }
   for (unsigned int eye = 0; eye < 2; eye++)
   {
      mvp.SetView(eye, viewpoint[eye]);
      mvp.SetProj(eye, proj[eye]);
   }
}

void VRDevice::RecenterTable()
{
#if defined(ENABLE_XR)
   m_headsetViewCentering = true;
   m_controllerViewCentering = false;
#endif
}

void VRDevice::SaveVRSettings(Settings& settings) const
{
   settings.SetPlayerVR_Orientation(m_orientation, false);
   settings.SetPlayerVR_TableX(m_tablePos.x, false);
   settings.SetPlayerVR_TableY(m_tablePos.y, false);
   settings.SetPlayerVR_TableZ(m_tablePos.z, false);
}
