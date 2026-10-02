// license:GPLv3+

#pragma once

#if defined(ENABLE_BGFX)
#include "bgfx/bgfx.h"
#endif

#include "typedefs3D.h"
#include "Sampler.h"
class RenderDevice;
class RenderPass;

class RenderTarget final
{
public:
   RenderTarget(RenderDevice* const rd, const SurfaceType type, const int width, const int height, const colorFormat format); // Default output render target
   #if defined(ENABLE_BGFX)
   RenderTarget(RenderDevice* const rd, const SurfaceType type, bgfx::FrameBufferHandle fbh, bgfx::TextureHandle colorTex, bgfx::TextureFormat::Enum colFormat,
      bgfx::TextureHandle depthTex, bgfx::TextureFormat::Enum depthFormat, const string& name, const int width, const int height, const colorFormat format);
   #endif
   RenderTarget(RenderDevice* const rd, const SurfaceType type, const string& name, const int width, const int height, const colorFormat format, bool with_depth, int nMSAASamples, const char* failureMessage, RenderTarget* sharedDepth = nullptr);
   ~RenderTarget();

   // renderScale: dynamic resolution (see RenderDevice::BeginScaledRendering), the viewport covers the top left part of the target when
   // m_dynamicResolution is set, the whole target otherwise
   void Activate(const int layer = -1, const float renderScale = 1.f);
   static RenderTarget* GetCurrentRenderTarget();
   static int GetCurrentRenderLayer();
   static float GetCurrentRenderScale() { return current_render_scale; } // To re-activate a target with the scale it was activated with

   bool IsBackBuffer() const { return m_is_back_buffer; }
   std::shared_ptr<Sampler> GetColorSampler() const { return m_color_sampler; }
   void UpdateDepthSampler(bool insideBeginEnd);
   std::shared_ptr<Sampler> GetDepthSampler() const { return m_depth_sampler; }

   RenderTarget* Duplicate(const string& name, const bool shareDepthSurface = false);
   void CopyTo(RenderTarget* const dest, const bool copyColor = true, const bool copyDepth = true, 
      const int x1 = -1, const int y1 = -1, const int w1 = -1, const int h1 = -1,
      const int x2 = -1, const int y2 = -1, const int w2 = -1, const int h2 = -1,
      const int srcLayer = -1, const int dstLayer = -1);

   void SetSize(const int w, const int h) { assert(m_is_back_buffer); m_width = w; m_height = h; }
   int GetWidth() const { return m_width; }
   int GetHeight() const { return m_height; }
   // Size of the part of the target rendered at a dynamic resolution scale (the top left part, the texture keeps its allocated size)
   int GetScaledWidth(const float renderScale) const { return m_dynamicResolution && renderScale < 1.f ? std::max(1, static_cast<int>(lroundf(static_cast<float>(m_width) * renderScale))) : m_width; }
   int GetScaledHeight(const float renderScale) const { return m_dynamicResolution && renderScale < 1.f ? std::max(1, static_cast<int>(lroundf(static_cast<float>(m_height) * renderScale))) : m_height; }
   // Targets which follow the dynamic resolution of the frame (scene buffers, post-process buffers, probes rendered with the scene projection,
   // the headset swapchain): rendered into their top left part at the scale of the frame, and sampled with scaled texture coordinates
   bool m_dynamicResolution = false;
   bool IsMSAA() const { return m_nMSAASamples > 1; }
   bool HasDepth() const { return m_has_depth; }
   colorFormat GetColorFormat() const { return m_format; }
   RenderDevice* GetRenderDevice() const { return m_rd; }

#if defined(ENABLE_BGFX)
   bgfx::FrameBufferHandle GetCoreFrameBuffer() const { return m_framebuffer; }
   bgfx::TextureFormat::Enum GetCoreColorFormat() const { return m_colorFormat; }
   void ResolveMSAADepth();
   #ifdef BGFX_RESOLVE_FRAGMENT_DENSITY_MAP
   // Foveated rendering: render through a frame buffer that carries this fragment density map (an invalid handle restores the plain one). The
   // variants are cached since the runtime hands a different map for each of its swapchain images.
   void SetFragmentDensityMap(bgfx::TextureHandle map);
   void SetFragmentDensityMapOffsets(const int32_t* offsetsXY, int nLayers); // For the current frame, on the foveated frame buffer in use
   #endif
   static void OnFrameFlushed() { current_render_target = nullptr; current_render_layer = 0; current_render_scale = 1.f; }
#elif defined(ENABLE_OPENGL)
   GLuint GetCoreFrameBuffer() const { return m_framebuffer; }
#elif defined(ENABLE_DX9)
   IDirect3DSurface9* GetCoreColorSurface() { return m_color_surface; }
#endif

   const string m_name;
   const SurfaceType m_type;
   const int m_nLayers;

   RenderPass* m_lastRenderPass = nullptr;

private:
   RenderDevice* const m_rd;
   const bool m_is_back_buffer;
   const colorFormat m_format;
   int m_width;
   int m_height;
   const int m_nMSAASamples;
   const bool m_has_depth;
   const bool m_shared_depth;
   std::shared_ptr<Sampler> m_color_sampler;
   std::shared_ptr<Sampler> m_depth_sampler;

   static RenderTarget* current_render_target;
   static int current_render_layer;
   static float current_render_scale; // Dynamic resolution scale the active view was set up with

#if defined(ENABLE_BGFX)
   bgfx::TextureHandle m_color_tex = BGFX_INVALID_HANDLE;
   bgfx::TextureFormat::Enum m_colorFormat = bgfx::TextureFormat::Count;
   bgfx::TextureHandle m_depth_tex = BGFX_INVALID_HANDLE;
   bgfx::TextureFormat::Enum m_depthFormat = bgfx::TextureFormat::Count;
   bgfx::TextureHandle m_msaaResolveDepthTex = BGFX_INVALID_HANDLE;
   std::shared_ptr<Sampler> m_msaa_depth_sampler;
   bgfx::TextureHandle m_msaaDepthResolveColorTex = BGFX_INVALID_HANDLE;
   bgfx::FrameBufferHandle m_msaaDepthResolveFramebuffer = BGFX_INVALID_HANDLE;
   bgfx::FrameBufferHandle m_framebuffer_layers[6] { BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE, BGFX_INVALID_HANDLE };
   bgfx::FrameBufferHandle m_framebuffer = BGFX_INVALID_HANDLE;
   bool m_needResolve = false;
   #ifdef BGFX_RESOLVE_FRAGMENT_DENSITY_MAP
   bgfx::FrameBufferHandle m_plainFramebuffer = BGFX_INVALID_HANDLE; // m_framebuffer without a fragment density map
   std::map<uint16_t, bgfx::FrameBufferHandle> m_fdmFramebuffers; // Variants of m_plainFramebuffer, keyed by the density map texture
   #endif
#elif defined(ENABLE_OPENGL)
   GLuint m_framebuffer = 0;
   GLenum m_texTarget = 0;
   GLuint m_color_tex = 0;
   GLuint m_depth_tex = 0;
   GLuint m_framebuffer_layers[6];
#elif defined(ENABLE_DX9)
   bool m_use_alternate_depth = false;
   IDirect3DSurface9* m_color_surface = nullptr;
   IDirect3DTexture9* m_color_tex = nullptr;
   IDirect3DSurface9* m_depth_surface = nullptr;
   IDirect3DTexture9* m_depth_tex = nullptr;
#endif
};
