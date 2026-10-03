# Eye-tracked foveated rendering in Visual Pinball on the Steam Frame — technical reference

*2026-10-01, updated 2026-10-03 (the earlier article kept here, about the density map and the tiled render path, is superseded by sections 7, 8 and 11; its driver analysis survives in section 7). Describes what is implemented on the `foveated-rendering` branch of the fork (on top of `steam-frame`), how it works end to end, how it was verified, and what is still open. The evaluation that preceded it is `foveated-rendering-plan.md`; the file-by-file summary is section 9 of `vpinball/docs/Steam Frame Branch.md`. Paths are relative to the `vpinball` clone; bgfx references are to the bundled fork source `external/macos-arm64/Release/bgfx/bgfx.cmake/bgfx/src/renderer_vk.cpp` as patched by `platforms/linux-aarch64/bgfx-fragment-density-map.patch`; Turnip references are to Mesa `src/freedreno/vulkan/`.*

## 1. What foveated rendering is, and why it matters here

A VR headset renders two images per frame, one per eye, and each image is far larger than the part of it the player actually looks at. The human eye resolves fine detail only in the fovea, roughly the central 2°–5° of the visual field; acuity falls off quickly beyond 10°–15°. A display that covers about 110° per eye therefore spends most of its pixels on regions the eye cannot resolve at full detail. **Foveated rendering** shades the image at full quality only around the point of regard and progressively coarser away from it, so that the GPU work per frame drops without a visible loss.

Two variants exist:

- **Fixed foveation**: the high quality area is anchored at the center of each eye's image (where the lens is sharpest and where people look most of the time). No eye tracking is needed, but the high quality zone must be large because the player may look anywhere, so the gain is modest and the periphery is visibly coarse when looked at.
- **Eye-tracked (dynamic) foveation**: the headset's eye tracker reports where each eye looks, and the high quality area follows it every frame. The zone can be small (a few degrees), so the gain is larger and nothing coarse is ever seen, provided the gaze reaches the renderer with low latency (eye saccades are fast; a zone of ~10° radius hides the tracker's latency and noise).

The Steam Frame has eye tracking, and Visual Pinball is a good candidate: at the runtime's recommended 1728×1728 per eye the game runs well, but the display's native 2160×2160 (56 % more pixels) is "significantly sharper" and heavy tables then fall below 72 fps. Foveation is the tool meant to keep native resolution (and later MSAA or reflections) affordable.

### 1.1 How a GPU implements it

There are three mechanisms a modern GPU offers, and the choice is dictated by what the driver and the engine support:

| Mechanism | What it is | On the Steam Frame |
|---|---|---|
| **Fragment density map** (`VK_EXT_fragment_density_map`, FDM) | A small texture (one texel per block of e.g. 32×32 pixels) attached to the render pass; each texel holds a density in x and y (0..1 as R8G8). The rasterizer shades blocks of the target at the density given by the map: a density of 0.5 in both axes means one fragment per 2×2 pixels, which the hardware then expands. Everything is driver-managed; the application only supplies the map. | Yes: Turnip (the Mesa Vulkan driver for Adreno) implements it, including Valve's own additions (see §2). Used by this implementation. |
| **Variable rate shading** (`VK_KHR_fragment_shading_rate`) | A per-draw, per-primitive or per-image shading rate (1×1, 2×2, 4×4…). The attachment variant is the equivalent of FDM with coarser semantics. | Present, but bgfx only supports the per-draw rate (uniform per view), which is useless for foveation. |
| **Multi-resolution / multi-view rendering** | Render the scene several times at different resolutions and composite. | No driver help needed but costly in engine changes (geometry submitted several times). Not considered. |

An FDM is the natural choice on a tiled mobile GPU: Adreno renders in bins (tiles) that fit in its on-chip GMEM, and Turnip applies the density by rendering each bin at a reduced resolution then scaling it up on the way out to memory. This is also what defines the limits of the gain, which §7 comes back to.

### 1.2 What the OpenXR runtime provides

Foveation is more than a Vulkan feature: the runtime (SteamVR's OpenXR on the Frame) owns the eye tracker and knows the headset's optics. The relevant OpenXR extensions, all present on the Frame (SteamVR/OpenXR 2.17.10):

- `XR_FB_foveation`, `XR_FB_foveation_configuration`: create a **foveation profile** (level Low/Medium/High, dynamic or not) and apply it to a swapchain.
- `XR_FB_swapchain_update_state`: the `xrUpdateSwapchainFB` call that applies the profile.
- `XR_FB_foveation_vulkan`: makes the runtime hand out, per swapchain image, a Vulkan **density map image** it generates from the profile (`XrSwapchainImageFoveationVulkanFB`).
- `XR_META_foveation_eye_tracked`: makes the profile eye-tracked; the runtime turns the eye tracker on and reports its state and the **gaze center per eye** (`xrGetFoveationEyeTrackedStateMETA`, normalized coordinates in [-1, 1]).
- `XR_META_performance_metrics`: counters to measure the result (on the Frame, a single one, `/perfmetrics_meta/app/gpu_frametime`).
- `XR_EXT_eye_gaze_interaction` is also present (raw gaze pose as an action); not used, since `XR_META_foveation_eye_tracked` already gives the gaze center in image space and triggers no separate permission flow.

Enabling eye tracking is **opt-in per user**: eye tracking must be enabled and calibrated in the headset's settings. When it is not, the runtime reports the eye-tracked state as not valid and the implementation falls back to a fixed foveation at the center of each eye.

## 2. The platform, as measured on the device

Facts gathered on the Steam Frame (SteamOS ARM64, Adreno 750 through Turnip) that shaped the design:

- **Vulkan extensions**: `VK_EXT_fragment_density_map` (features: `fragmentDensityMap` yes, `fragmentDensityMapDynamic` no, `fragmentDensityMapNonSubsampledImages` yes; minimum texel size 32×32), `VK_EXT_fragment_density_map_offset` (and the QCOM original), `VK_VALVE_fragment_density_map_layered`, `VK_KHR_fragment_shading_rate`, `VK_KHR_create_renderpass2`.
- **`VK_VALVE_fragment_density_map_layered`** is Valve's extension, upstream in Mesa, that allows one density map **layer per layer of a layered render target without multiview**. Visual Pinball renders both eyes into one 2-layer texture array (instanced stereo with `gl_Layer`), so without it a single map would serve both eyes, which is wrong for eye-tracked foveation (each eye has its own gaze center in its own image).
- **`VK_EXT_fragment_density_map_offset`** lets the application shift the whole density map by a per-layer pixel offset given at the *end* of the render pass (`VkRenderPassFragmentDensityMapOffsetEndInfoEXT` through `vkCmdEndRenderPass2`). This is the designed way to follow the gaze on a tiled GPU: the map itself does not need to be rewritten and re-uploaded every frame (the map must not change while the pass that reads it is in flight, since `fragmentDensityMapDynamic` is not supported), only the offset changes. The device granularity is 8 pixels (`fragmentDensityOffsetGranularity`, Turnip's `TU_FDM_OFFSET_GRANULARITY`).
- **Turnip only honors offsets on density map images created with `VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_EXT`** (`tu_enable_fdm_offset` in `tu_cmd_buffer.cc`). The maps the runtime hands out through `XR_FB_foveation_vulkan` are created without that flag (image create flags = 0, seen with the API dump layer). This single fact is why the application builds its own map (§4.4) instead of using the runtime's: the runtime's maps follow the gaze only if the runtime rewrites them, and whether it does is invisible and, as tested, did not move the sharp zone.
- **Valve's FDM injection layer** (`libVkLayer_VALVE_fdm_injection.so`, an implicit OpenXR API layer plus Vulkan layer present in every process) is the runtime's own foveation implementation for applications that render straight into the swapchain images. It hooks `vkCreateGraphicsPipelines`, `vkCmdBeginRenderPass2`/`vkCmdBeginRendering` and `vkCreateImageView` to inject density maps into render passes targeting a swapchain image. Visual Pinball renders the scene into an **offscreen** stereo buffer and only writes the final tonemapped image into the swapchain, so the layer never matched anything (with `FDM_DEBUG=1` it printed only "XR layer loaded / added swapchain size 2160" in 25 s of rendering) and the application has to do the job itself.
- **Render path of Visual Pinball in VR**: scene → `BackBuffer1`/`BackBuffer2` (2-layer texture array, `RGB16F` color, which Vulkan drivers back with a 16-bit float RGBA image of 8 bytes per pixel, plus D32F depth, MSAA off by default, `AAFactor` 1 so it has the swapchain's size) → post passes (bloom, optional AA/SSR/AO, tonemap) → the OpenXR swapchain framebuffer → compositor. The in-game menu is drawn after tonemap and is never foveated.

## 3. Architecture

```
 Settings (PlayerVR/Foveation, FoveationEyeTracked, FoveationFlipX/Y)
     │
     ▼
 VRDevice (OpenXR) ──── foveation profile ──► SteamVR runtime ──► eye tracker on
     │                  (XR_FB_foveation + XR_META_foveation_eye_tracked)
     │ gaze center per eye, each frame (xrGetFoveationEyeTrackedStateMETA)
     │ own density map (68×68 RG8, 2 layers, flat rings per level)
     ▼
 Renderer ──► RenderTarget BackBuffer1/2: framebuffer variant with the map attached
     │        + per-frame map offsets (gaze × half size, sign convention)
     ▼
 bgfx (patched): VkRenderPassFragmentDensityMapCreateInfoEXT on the render pass,
     │           extra attachment in the VkFramebuffer, VALVE layered pipeline info,
     │           offsets through vkCmdEndRenderPass2 (VK_EXT_fragment_density_map_offset)
     ▼
 Turnip (Mesa): bins rendered at the map's density, scaled up on store to the
                full-resolution scene buffer; post passes see a normal image
```

The design follows two rules:

1. **Everything is under the application's control**: the map, its content, its offsets. The runtime's part is reduced to what only it can do (turn the eye tracker on and report the gaze). The runtime's own density maps are still requested and wrapped, as a fallback for a driver that would not support offsets.
2. **Nothing changes for the other platforms**: the bgfx patch is only applied by the Linux ARM64 build (`platforms/linux-aarch64/external.sh`); its macros (`BGFX_CAPS_FRAGMENT_DENSITY_MAP`, `BGFX_TEXTURE_FRAGMENT_DENSITY_MAP`, `BGFX_RESOLVE_FRAGMENT_DENSITY_MAP`) do not exist in an unpatched bgfx, and all the Visual Pinball code that uses them is under `#ifdef`, so the Windows and macOS builds compile the same source with the feature compiled out. The OpenXR part is gated on the extensions being present and is inert on a PC runtime that lacks them.

## 4. Implementation, layer by layer

### 4.1 Settings and UI

`src/core/Settings_properties.inl`, `PlayerVR` block:

| Setting | Type | Default | Meaning |
|---|---|---|---|
| `Foveation` | enum Off / Low / Medium / High | Medium on `__STANDALONE__ && ENABLE_XR` builds, Off elsewhere | Level; drives both the runtime profile and the application's map rings |
| `FoveationEyeTracked` | bool | on | Follow the eyes when the runtime supports it and eye tracking is active; otherwise fixed at the center |
| `FoveationFlipX` | bool | off | Mirror the horizontal gaze offset (sign convention, settled on the device) |
| `FoveationFlipY` | bool | on | Mirror the vertical gaze offset (idem) |

`src/ui/live/ingameui/VRSettingsPage.cpp` exposes the first two in the in-headset VR settings page, applied live through `VRDevice::SetFoveationMode` / `SetFoveationEyeTracked`, plus a read-only "Foveation status" line (`VRDevice::GetFoveationStatus`): "Not supported by the runtime", "Off", "Fixed (eye tracking disabled)", "Fixed: eye tracking not active (enable it in the headset settings)", or "Eye-tracked".

### 4.2 OpenXR side (`src/renderer/VRDevice.h/.cpp`)

**Instance creation.** Through the existing `EnableExtensionIfSupported` lambda: `XR_FB_foveation` + `XR_FB_foveation_configuration` + `XR_FB_swapchain_update_state` (together `m_foveationExtensionSupported`), plus `XR_FB_foveation_vulkan` on the Vulkan backend, then `XR_META_foveation_eye_tracked` (`m_foveationEyeTrackedExtensionSupported`) and `XR_META_performance_metrics`. The function pointers (`xrCreateFoveationProfileFB`, `xrDestroyFoveationProfileFB`, `xrUpdateSwapchainFB`, `xrGetFoveationEyeTrackedStateMETA`, the three performance metrics entry points) are resolved with `xrGetInstanceProcAddr`. The init log line states what the runtime offers and what the setting asks.

**System properties.** `XrSystemFoveationEyeTrackedPropertiesMETA` is chained to `xrGetSystemProperties`; `supportsFoveationEyeTracked` becomes `m_foveationEyeTrackedSystemSupported` (true on the Frame).

**Swapchain creation.** The color swapchain's `XrSwapchainCreateInfo.next` carries `XrSwapchainCreateInfoFoveationFB { flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB }`, which makes the runtime allocate a density map per swapchain image. The depth swapchain is left alone. Before enumerating the images, `m_backend->RequestFoveationImages(true)` makes the backend chain `XrSwapchainImageFoveationVulkanFB` to each `XrSwapchainImageVulkanKHR`, and after it `m_backend->CreateFoveationTextures(swapchain)` wraps the returned images (68×68, one layer per eye on the Frame) as bgfx textures. They are logged ("N density maps of 68x68 received from the runtime") and kept in `SwapchainInfo::foveationTextures` as the fallback map.

**Profile.** `ApplyFoveation()` (called after the swapchain exists and whenever a setting changes) builds `XrFoveationLevelProfileCreateInfoFB { level = NONE/LOW/MEDIUM/HIGH, verticalOffset = 0, dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB }`, chains `XrFoveationEyeTrackedProfileCreateInfoMETA` when eye tracking is wanted, supported and the level is not Off, creates the profile with `xrCreateFoveationProfileFB` and applies it with `xrUpdateSwapchainFB(XrSwapchainStateFoveationFB { profile })`. The previous profile is destroyed after the new one is applied. **On the Frame, applying an eye-tracked profile is what turns the eye tracker on for the application**; nothing else is needed.

**Gaze.** `xrGetFoveationEyeTrackedStateMETA` returns `flags` (VALID bit) and `foveationCenter[2]`, the point of regard of each eye in its own image, normalized to [-1, 1] with (0, 0) at the center. `UpdateFoveationState()` keeps the validity for the status line; `RenderFrame` reads it every frame for the offsets (§4.5).

**Own density map.** `CreateOwnFoveationMap()` runs once per session after `ApplyFoveation()`, only when the backend reports both FDM and offset support. The size is the eye image divided by the 32-pixel texel, rounded up (2160 → 68, 1728 → 54), `RG8`, one layer per view (2), flag `BGFX_TEXTURE_FRAGMENT_DENSITY_MAP`. `FillOwnFoveationMap()` writes the content, per layer, with `bgfx::updateTexture2D`: concentric flat rings around the map's center,

| Level | Full density radius | Half density radius | Beyond |
|---|---|---|---|
| Low | 0.45 | 0.80 | 1/4 |
| Medium | 0.30 | 0.55 | 1/4 |
| High | 0.20 | 0.38 | 1/4 |

where radii are fractions of the half width (the Frame shows about 55° per half width, so 0.20 is roughly 11° around the gaze, which eye tracking affords), and the density values are 255 (1.0), 128 (0.5) and 64 (0.25) in both channels (the hardware's rates are 1, 1/2 and 1/4 per axis, so intermediate values would be rounded anyway). The map is centered; the gaze moves it through offsets rather than by rewriting it. "Off" is a map full of 255 but is never attached (the plain framebuffer is used).

**Lifetime.** `ReleaseSession` destroys the profile before the swapchains, the wrapped runtime maps and the own map.

**Status and measurement.** `LogRuntimeStatus()`, every 5 s: the foveation status, the gaze centers when valid, the `XR_META_performance_metrics` counters (enumerated and enabled in `CreateSession`), and with `VPX_GPU_PROFILE` set, the per-target GPU time breakdown (§6).

### 4.3 Vulkan device (`src/renderer/XRGraphicBackend.h`, `src/renderer/XRVulkanBackend.h`)

Visual Pinball creates the Vulkan device itself (OpenXR requires it, `xrCreateVulkanDeviceKHR` path) and hands it to bgfx, so the device extensions and features must be enabled here, not in bgfx. `XRVulkanBackend` enumerates the device extensions with `vkEnumerateDeviceExtensionProperties` and, when present, enables:

- `VK_EXT_fragment_density_map` with `VkPhysicalDeviceFragmentDensityMapFeaturesEXT { fragmentDensityMap = TRUE }` → `m_fdmSupported`;
- `VK_VALVE_fragment_density_map_layered` with its feature → `m_fdmLayeredSupported`;
- `VK_EXT_fragment_density_map_offset` + `VK_KHR_create_renderpass2` with `VkPhysicalDeviceFragmentDensityMapOffsetFeaturesEXT { fragmentDensityMapOffset = TRUE }` → `m_fdmOffsetSupported`,

all chained on `VkDeviceCreateInfo.pNext`, logged as "Fragment density maps: supported, one layer per eye, with offsets". The abstract `XRGraphicBackend` gained four virtuals with inert defaults (`RequestFoveationImages`, `CreateFoveationTextures`, `IsFragmentDensityMapSupported`, `IsFragmentDensityMapOffsetSupported`) so `VRDevice` stays backend-agnostic.

`CreateFoveationTextures` wraps the runtime's `VkImage`s as external bgfx textures (`bgfx::createTexture2D(w, h, false, arraySize, RG8, BGFX_TEXTURE_FRAGMENT_DENSITY_MAP, nullptr, (uintptr_t)image)`), deduplicated per image.

### 4.4 bgfx patch (`platforms/linux-aarch64/bgfx-fragment-density-map.patch`)

bgfx has no notion of a density map, and its Vulkan renderer uses Vulkan 1.0 render passes it builds itself (`getRenderPass`), so the support is added as a patch to the fork sources, applied by `platforms/linux-aarch64/external.sh` after the descriptor-pool patch (cache tag `-turnip3`), Linux only. It is generated by a script from pristine sources so it can be regenerated when the fork moves. Additive only; what it adds:

**Public API (`bgfx.h`, `defines.h`)**
- `BGFX_CAPS_FRAGMENT_DENSITY_MAP` (caps bit `0x1000000000`): reported when `VK_EXT_fragment_density_map` is available.
- `BGFX_TEXTURE_FRAGMENT_DENSITY_MAP` (texture flag `0x0002000000000000`): the texture is a density map. An application-created one gets usage `FRAGMENT_DENSITY_MAP_BIT_EXT`, the create flag `VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_EXT` when offsets are supported, and lives in `VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT` (its `m_sampledLayout`, so bgfx's normal transition after an upload lands it in the right layout). An external one (the runtime's) is taken as already in that layout and never transitioned.
- `BGFX_RESOLVE_FRAGMENT_DENSITY_MAP` (attachment resolve flag `0x02`): marks a framebuffer attachment as *the* density map, which `bgfx.cpp` then excludes from the color/size validation.
- `bgfx::setFragmentDensityMapOffsets(FrameBufferHandle, const int32_t* xy, uint8_t layers)`: per-frame offsets, stored in the `Context` (up to 4 framebuffers, 8 layers each), copied into the `Frame` at `bgfx::frame()` like other per-frame state, applied by the renderer.

**Renderer (`renderer_vk.cpp`)**
- Extension table entries for `VK_EXT_fragment_density_map`, `VK_VALVE_fragment_density_map_layered`, `VK_EXT_fragment_density_map_offset`, `VK_KHR_create_renderpass2`; `fragmentDensityOffsetGranularity` queried through `VkPhysicalDeviceFragmentDensityMapOffsetPropertiesEXT`; `m_fragmentDensityMapOffsetSupported` requires an externally created device (the only way the feature can have been enabled) and `vkCmdEndRenderPass2KHR`.
- Barrier tables: the `FRAGMENT_DENSITY_MAP_OPTIMAL_EXT` layout maps to the `FRAGMENT_DENSITY_PROCESS` stage and `FRAGMENT_DENSITY_MAP_READ` access as source and destination.
- `FrameBufferVK::create`: the tagged attachment is pulled out of the color/depth lists (`m_hasFdm`, `m_fdmAttachment`, `m_fdmFormat`, `m_fdmLayers`); `postReset` creates an image view for it (array view over the attached layers) and appends it to the `VkFramebuffer`'s attachments after the color, depth and resolve ones.
- `getRenderPass(..., VkFormat _fdmFormat, uint32_t _fdmLayers)`: the format and layer count enter the cache hash; when set, one more `VkAttachmentDescription` (loadOp LOAD, storeOp DONT_CARE, initial and final layout `FRAGMENT_DENSITY_MAP_OPTIMAL_EXT`) is appended and `VkRenderPassFragmentDensityMapCreateInfoEXT` referencing it is chained on `VkRenderPassCreateInfo.pNext`. Vulkan 1.0 render passes are enough for FDM; render pass 2 is only needed for the offsets at end time.
- Pipelines: when the layered extension is present, `VkPipelineFragmentDensityMapLayeredCreateInfoVALVE { maxFragmentDensityMapLayers }` is chained on the graphics pipeline create info so the fragment shader is compiled to read the map layer of the layer it renders into.
- Render target textures get `VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_EXT` when offsets are supported (the spec requires it on every attachment of a pass that uses offsets).
- `endRenderPass()` replaces every `vkCmdEndRenderPass`: if offsets were set this frame for the framebuffer being rendered, they are rounded down to the granularity and passed in `VkRenderPassFragmentDensityMapOffsetEndInfoEXT` chained on `VkSubpassEndInfo` to `vkCmdEndRenderPass2KHR`; otherwise the plain call.

### 4.5 Scene buffer and per-frame flow (`src/renderer/RenderTarget.h/.cpp`, `src/renderer/Renderer.h/.cpp`, `src/renderer/VRDevice.cpp`)

`RenderTarget` keeps its original framebuffer as `m_plainFramebuffer` and a cache `m_fdmFramebuffers` of variants keyed by the map texture handle. `SetFragmentDensityMap(map)` switches `m_framebuffer` to the variant for that map, creating it on first use with the same color and depth attachments as the plain one plus `{ map, Access::Read, mip 0, layers = m_nLayers, BGFX_RESOLVE_FRAGMENT_DENSITY_MAP }`; an invalid handle restores the plain framebuffer. `SetFragmentDensityMapOffsets` forwards to bgfx when a foveated variant is current. `Renderer::SetFragmentDensityMap/Offsets` apply this to `BackBuffer1` and `BackBuffer2` (the post passes ping-pong between them; both carry the map so any pass that renders into them is foveated). The per-layer sub-framebuffers used elsewhere are untouched, and the swapchain framebuffers never carry a map.

Per frame, in `VRDevice::RenderFrame`, right after the swapchain image is acquired and before anything is drawn:

1. Pick the map: the application's own map if it exists, else the runtime's map of the acquired image (fallback), else none when the level is Off. Hand it to `Renderer::SetFragmentDensityMap`.
2. If the own map is used, the eye-tracked extension is present and offsets are supported: query `xrGetFoveationEyeTrackedStateMETA`; when VALID, for each eye convert the normalized center to pixels of the scene buffer, `offset = (±cx · W/2, ±cy · H/2)`, with the signs from `FoveationFlipX` (off: x as reported) and `FoveationFlipY` (on: y negated, because the runtime reports y up and the image's y axis points down). When not valid (eye tracking off or lost), the offsets stay (0, 0): fixed foveation at the center.
3. `Renderer::SetFragmentDensityMapOffsets(offsets, 2)`.

The scene then renders as usual; Turnip reads the map at the start of each bin, shades at the local density and scales the bin up on store. The post passes sample the resolved full-resolution image unchanged (the image is a normal, non-subsampled one), so bloom, AA, tonemap, the swapchain copy and the preview window need no change.

### 4.6 Render pass view names for profiling (`src/renderer/RenderDevice.h/.cpp`, `src/renderer/RenderPass.cpp`, `src/renderer/RenderTarget.cpp`)

The per-target GPU breakdown needs bgfx view names, which release builds did not set. `RenderDevice::m_nameViews` (true in `_DEBUG` builds and when the `VPX_GPU_PROFILE` environment variable is set, which also enables `BGFX_DEBUG_PROFILER`) now gates the naming in `RenderPass` and `RenderTarget`; the names are kept in `RenderDevice::m_viewNames` so the breakdown can group by render target from the `[RT=…]` part of the name.

## 5. How it was verified on the device

- **Extensions and features**: the init log lines ("OpenXR foveated rendering: supported by the runtime, eye-tracked: extension present", "Eye-tracked foveation supported by this headset", "Fragment density maps: supported, one layer per eye, with offsets", "Foveated rendering: 3 density maps of 68x68 received from the runtime", "own density map of 68x68, 2 layers, with gaze offsets", "Fragment density map attached to BackBuffer1 (2160x2160, 2 layers)").
- **API correctness**: the Vulkan API dump layer (`VK_LAYER_LUNARG_api_dump`) on the device showed the `VkRenderPassFragmentDensityMapCreateInfoEXT` chain, the extra attachment on the scene framebuffers only, and the image create flags of the runtime's maps (0, the reason for the own map); the old Khronos validation layer available on the device raised nothing on the render pass and framebuffer VUIDs.
- **Visible effect**: at High, with the eyes on a fixed object and the head moving, the sharp zone stays on the object; moving the eyes moves it. The sign conventions were found by trial on the device: with FlipX off / FlipY off the vertical axis was wrong; with FlipX on / FlipY off the horizontal was wrong; FlipX off / FlipY on is right on both axes ("the sharp zone follows my eyes on both axes now") and is the default. The periphery shows the expected block pattern when looked at deliberately (the eye tracker then moves the zone, so this is only visible at the edge of a saccade).
- **Eye tracking state**: the 5 s status line reports "Eye-tracked" with the gaze centers moving, after eye tracking was enabled and calibrated in the headset's settings. Without it, "Fixed: eye tracking not active".
- **Performance**: see §6 and §7. The short version is that the feature is functionally complete but the measured gain on the heavy table is nil so far, and the reason is being analyzed.

## 6. Measuring

Two sources, both logged by `VRDevice::LogRuntimeStatus` every 5 s in `~/.local/share/VPinballX/10.8/vpinball.log` on the device:

1. **Runtime counter** `/perfmetrics_meta/app/gpu_frametime` (`XR_META_performance_metrics`, enabled with `VPX_XR_METRICS=1` or `VPX_GPU_PROFILE=1` since 2026-10-02 — SteamVR 2.17.10 crashes the next session of the same instance when the counters were left enabled on the previous one, so they are measurement-only and disabled before a session is destroyed): the application's GPU time per frame as the compositor sees it. On the Frame it saturates at the 72 Hz period (13.9 ms) as soon as the application cannot keep up, so it tells whether the budget is met, not by how much it is missed.
2. **Per-target GPU breakdown**, with `VPX_GPU_PROFILE=1` in the environment: bgfx's profiler (`BGFX_DEBUG_PROFILER`) records GPU timestamps per view, and the status line sums them per render target: e.g. `GPU by target (13.70 ms): BackBuffer1 9.80 VRSwapchain 1.70 Playfield 1.10 BloomBuffer1 1.10`. Caveat: with the profiler on, bgfx ends the Vulkan render pass at every view change (`renderer_vk.cpp`, the `profiler.m_enabled` test in the view-change block), so each view becomes its own render pass with its own tile load/store; the absolute numbers are therefore higher than in normal operation, but comparisons between settings remain valid. The current uncommitted build also prints the number of views per target.

Running on the device from ssh with the profiler: `cd ~/devkit-game/vpx_frame; VPX_GPU_PROFILE=1 LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Launcher`, then `grep "GPU by target" ~/.local/share/VPinballX/10.8/vpinball.log | tail`.

Measurements so far, Ghostbusters VR-room table (heavy), profiler on, the player idle in the room:

| Resolution per eye | Foveation | BackBuffer (scene) | VRSwapchain | Playfield | Bloom | Total |
|---|---|---|---|---|---|---|
| 2160×2160 | Off | 9.6–10.1 ms | 1.7 | 1.1 | 1.1 | ~13.7 ms |
| 2160×2160 | Medium | 9.6–10.1 ms | 1.7 | 1.1 | 1.1 | ~13.7 ms |
| 2160×2160 | High | 9.6–10.1 ms | 1.7 | 1.1 | 1.1 | ~13.7 ms |
| 1728×1728 | Off | 6.0–6.2 ms | 1.05 | 1.0 | 0.6 | ~8.9 ms |

The lobby costs 4.7–5.5 ms in total. The scene pass scales with the pixel count (0.64× pixels → 0.63× time); yet the density map, which demonstrably reduces the shaded fragments to roughly a third at Medium (7 % of the image at full, 17 % at half, 76 % at quarter density), does not reduce it at all. §7 explains why, from the driver experiments below.

### 6.1 Driver experiments (2026-10-02, device Mesa 26.3.0-devel git-8aa73b4b19)

Same table, 2160×2160, the table played directly (`-Play`), 50 s per run. First series with the profiler (scene pass = `BackBuffer`), second series without it (the runtime's `gpu_frametime` only, so bgfx keeps its render passes whole):

| Run | Foveation | Driver setting | Scene pass (profiler) | Frame GPU time (no profiler) |
|---|---|---|---|---|
| default | Off | — | 9.5–10.7 ms | 13.7–14.1 ms (median 14.0) |
| default | High, eye-tracked | — | 4.7–9.4 ms (4.7 only while the gaze was off the table) | 12.4–14.0 (median 13.9) |
| default | Medium, eye-tracked | — | — | 13.5–15.0 (median 13.8) |
| direct rendering forced | Off | `TU_DEBUG=sysmem` | **4.5–4.8 ms** | **9.0–9.7 (median 9.1)** |
| tiled forced | Off | `TU_DEBUG=gmem` | 9.4–9.5 (Playfield 1.1 → 4.2, swapchain 1.7 → 2.9) | — |
| tiled forced | High | `TU_DEBUG=gmem` | 9.2–9.9 | — |
| measured autotuner | Off | `TU_AUTOTUNE_ALGO=profiled` | — | **8.9–10.0 (median 9.3)** |
| sysmem-preferring autotuner | Off | `TU_AUTOTUNE_ALGO=prefer_sysmem` | — | **8.9–9.3 (median 9.1)** |
| no framebuffer compression | Off | `TU_DEBUG=noubwc` | invalid (rendering stopped during the run) | — |

Turnip's own trace (`MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=…`, High) of one frame: the scene is **one render pass of 509 draws, tiled in 48 bins of 288×384** (`tiledRender=true`, no forced reason, i.e. the autotuner's bandwidth estimate chose tiling); inside it the per-bin draws sum to ≈ 6.5 ms, the GMEM stores to ≈ 0.9 ms, the binning pass 0.56 ms; heavy bins (over the table) take up to 650 µs of draws, light ones 80 µs. The Playfield pass (436 draws) runs direct (`Autotune selected sysmem`) in 1.1 ms. LRZ (hierarchical early depth rejection) is **disabled from draw 394 on** with the reason "Depth write + ALWAYS/NOT_EQUAL" / "Depth write + no color writes": that draw is the VR visibility mask (`Renderer.cpp:3012-3025`, depth write, no color write, depth test ALWAYS) drawn after color draws.

### 6.2 Verification of the fixes (2026-10-02, build with `TU_AUTOTUNE_ALGO=profiled` set by the app, foveation default Off, mask with `Z_LESSEQUAL`)

Same table, native 2160×2160, nothing set in the environment: the driver logs `TU_AUTOTUNE_ALGO=1 (profiled)` under `TU_DEBUG=startup`, and the frame GPU time is **8.97–9.18 ms** (previous build: 13.7–14.1 ms). The trace confirms the scene pass (509 draws) now runs direct: `tiledRender=false, tilingDisableReason=Autotune selected sysmem`.

LRZ after the `Z_LESSEQUAL` change: the hard invalidation is gone (`lrz=true`, no disable reason), but LRZ *writes* still stop at draw 394 ("Depth write + no color writes", Turnip's rule for a depth-only draw that comes after colour draws). The mask was still draw 394 because it was never first: a draw's sort key is `depthBias − z` and keys more than 50000 apart are ordered highest first (`RenderPass::SortCommands`, the rule that keeps old tables' playfield first); the mask was submitted with z = 200000 and bias 0, i.e. a key of −200000, which put it *after* every opaque part — so besides the LRZ effect it never worked as an early depth mask either (the hidden area was shaded by all opaque draws, then masked). Fixed by submitting it with a depth bias of +200000 (`Renderer.cpp`). With that build the scene pass trace reads `lrz=true, lrzDisableReason=` and the only write-disable left is "Depth write + blending" at draw 395, i.e. the first transparent part (Visual Pinball's transparent parts write depth, which Turnip's conservative LRZ does not track) — the mask is no longer involved and LRZ is live for every opaque draw. Frame GPU time 9.0–9.2 ms.

## 7. Analysis of the missing gain — resolved on 2026-10-02

*The gain was recovered on 2026-10-03 through the other foveation mechanism, see section 11; the eye tracking is checked in the headset with the debug mode of section 8.*

**Conclusion.** On the Steam Frame's driver, the scene pass of a heavy table is not fragment-bound but **geometry-bound inside the tiled path**: Turnip's tiled rendering re-processes the visible geometry once per bin (48 bins at 2160×2160), which costs ≈ 6.5 ms of per-bin draws against 4.7 ms for the *whole* pass rendered directly ("sysmem"). A fragment density map only reduces fragment work and, on Turnip, only exists in the tiled path (`tu_cmd_render_sysmem` has no density map handling, and the autotuner forces any pass with a map into tiling: "Uses a fragment density map"). So foveation can at best shave the small fragment share (≈ 0.5 ms measured at High), while simply rendering the scene pass directly saves ≈ 5 ms per frame: **14.0 → 9.1 ms at native 2160×2160, foveation off**, comfortably inside the 13.9 ms budget of 72 Hz. The default autotuner (`BANDWIDTH`, a per-pass bandwidth estimate) picks tiling for this pass because it does not model per-bin geometry; the `profiled` algorithm (the driver measures both modes per pass and keeps the faster) finds the right answer by itself (9.3 ms), as does `prefer_sysmem` (9.1 ms).

This also explains the earlier observations: the scene pass scaled with the resolution because the number of bins does (fewer bins at 1728), and foveation changed nothing because it changes neither the bin count nor the geometry per bin.

**What follows for Visual Pinball on the Frame** (all three implemented on 2026-10-02: `src/core/main.cpp`, `src/core/Settings_properties.inl`, `src/renderer/Renderer.cpp`; verification on the device in §6.2):

1. Ask the driver for the measured autotuner (or direct rendering) at startup — `TU_AUTOTUNE_ALGO=profiled` or `prefer_sysmem` set in the environment before the Vulkan loader runs (the driver reads it at device creation; `setenv` without overwrite so a user's own setting wins). This is the 35 % gain, costs nothing, and only affects Turnip.
2. Foveation stays available but should **default to Off** on the Frame: with a map, the pass is forced tiled and the gain is lost. It becomes useful again only if a future Turnip applies density maps in direct rendering, or for a table whose cost really is fragment shading (none measured yet).
3. Fix the visibility mask draw so LRZ survives the whole pass: draw it first and with `Z_LESSEQUAL` (its vertex shader forces `gl_Position.z = 0`, the near plane, so the test passes everywhere against the cleared depth and the result is identical); Turnip keeps LRZ for a depth-only draw at the start of a pass. Gain not yet measured (overdraw of the last 115 draws of the pass, ball and transparents included).

The analysis that led there follows. What FDM reduces on Turnip is the **fragment work inside each bin**: a bin at density 1/4 is rasterized and shaded at half resolution per axis. What it does not reduce:

1. **Tile load and store.** In the GMEM path every render pass loads the attachments it does not clear from memory into GMEM at the start of each bin and stores them back at the end, at full resolution (the scaled bin is expanded on store). bgfx creates its render passes with `loadOp = LOAD` unless the view that begins the pass has a clear, and `storeOp = STORE` always (the discard flags are never used by Visual Pinball). For the scene buffer at 2160×2160 × 2 layers this is 74.6 MB of 16-bit float RGBA color plus 37.3 MB of D32F depth per direction, ≈ 220 MB per load+store of one render pass, roughly 3–4 ms at the memory bandwidth of the Frame's SoC, and it scales with the pixel count exactly as observed, and not with the density map.
2. **Per-bin geometry.** Tiled rendering processes the visible geometry once per bin it touches (after a binning pass that computes the visibility stream). The number of bins is set by the GMEM size and the bytes per pixel of the attachments (16 B/pixel with 2 layers here), so it scales with the resolution, and FDM scales the bins' *content*, not their *count*; a table with heavy geometry pays this per bin whatever the density. Recent Turnip merges adjacent low-density bins (`TU_DEBUG=nobinmerging` to disable), which would recover part of this, but whether the device's Mesa has it is unknown.
3. **Full-resolution passes outside the scene.** VRSwapchain, Playfield and Bloom (≈ 3.9 ms at 2160) are not foveated by construction (post passes read the resolved image; the swapchain copy is a full-screen quad).

Turnip only applies a density map in the GMEM (tiled) path (`tu_cmd_render_sysmem` has no FDM handling; `tu_cmd_render_tiles` does), and picks GMEM or direct ("sysmem") rendering per render pass with an autotuner that compares estimated bandwidths (`tu_autotune.cc`), unless forced. Since the foveation is visible, the scene pass does run tiled.

The experiments that were run (results in §6.1), each a run on the device without rebuilding:

| Run | Tells |
|---|---|
| `TU_DEBUG=sysmem` (direct rendering, no load/store, no per-bin geometry, no FDM) | the cost of the tiled path itself; if the scene pass drops well below 9.8 ms, load/store and per-bin geometry dominate |
| `TU_DEBUG=gmem`, Off vs High | FDM gain with the tiled path forced, free of the autotuner |
| `TU_DEBUG=noubwc` | sensitivity to bandwidth (disables framebuffer compression) |
| `MESA_GPU_TRACES=print` | Turnip's own per-render-pass trace: GMEM or sysmem, the forced-mode reason, draw count, bin layout and GPU timestamps, the most direct view |
| a map at 1/4 density everywhere (debug) | upper bound of what fragment shading can give at this resolution |

The candidate fixes as listed before the experiments (superseded by the conclusion above: the load/store and format items turned out secondary, the tiled path itself is the cost):

- **Clear instead of load, discard instead of store where possible**: the scene render pass should begin with the view that clears color and depth (loadOp CLEAR, no load), and depth should be discarded at the end when no later pass reads it (`BGFX_CLEAR_DISCARD_DEPTH` on the view that begins the pass; AO, SSR and motion blur read the depth, and are off in VR by default). This removes up to half of the 220 MB.
- **Fewer render passes into the scene buffer**: every view change on the same framebuffer that bgfx turns into a new render pass (different view rectangle, a blit, the profiler) costs a full load+store; check the number of passes per frame without the profiler (the API dump counts `vkCmdBeginRenderPass`).
- **Smaller attachments**: a lighter color format for the scene buffer in VR (R11G11B10F halves the color traffic; RGBA8 would also lose the HDR range the tonemapper relies on), and MSAA kept off.
- **Foveate more**: since the shading itself is a smaller share than assumed, a coarser outer ring (1/4 is the minimum the hardware takes) is already in use; the remaining lever is applying the map to the Playfield and swapchain passes, which would need the map on those targets and is not worth it before the scene pass is understood.
- **Turnip version**: bin merging and the autotuner's FDM awareness are recent; a SteamOS update may change the picture. Record the Mesa version with each measurement (`vulkaninfo --summary` on the device).

## 8. Checking the eye tracking in the headset (debug mode)

`VPX_FOVEATION_DEBUG` turns the foveation into something the eye can judge: the full quality area shrinks to a small spot (radius 0.06 of the half width, about 3 degrees), a thin 2×2 ring around it, **4×4 shading everywhere else** (so the whole image is visibly coarse except the spot), and a **magenta ring is drawn around each spot** by the renderer (`VRDevice::UpdateGazeMarker`, a mesh in each eye's tangent space drawn with the `vr_mask` technique on top of the scene, exactly where the rate image puts the spot). The ring is the thing to compare with the gaze: it marks where the application believes the eye looks, whatever the shading does.

| Value | Spots and rings |
|---|---|
| `VPX_FOVEATION_DEBUG=1` | one per eye's gaze; since one rate image serves both eyes, **each eye sees two rings**: its own, and the other eye's about 0.3 of the half width to the side (the asymmetric per eye views, not an error) |
| `VPX_FOVEATION_DEBUG=2` | the left eye's gaze only: close the right eye and the single ring must sit on what is looked at |
| `VPX_FOVEATION_DEBUG=3` | the right eye's gaze only (same test, right eye open) |

On the device, with the headset worn (frames only flow then), `Foveation` not Off and `FoveationEyeTracked` on in the ini:

```
cd ~/devkit-game/vpx_frame
VPX_XR_METRICS=1 VPX_FOVEATION_DEBUG=3 LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE" 2>&1 | grep --line-buffered "Foveation:\|Eye gaze"
```

What to check, in that order:

1. **The ring follows the eye**: look at a point on the right side of the table, the top of the backglass, the flippers; the ring sits on each. Mirrored on an axis → the sign conventions (`FoveationFlipX`, `FoveationFlipY` in the ini, see section 11); sitting at the center and not moving → the gaze is not valid (see the status line below).
2. **Fixation while the head moves**: hold the eyes on an object and turn the head; the ring must stay on the object. If it drifts with the head, the gaze is the runtime's per image center (sampled, not re-projected) rather than the gaze ray.
3. **Jitter**: a small tremble is the tracker's noise through the 35 ms filter; large jumps between neighbouring points are a calibration problem (recalibrate eye tracking in the headset settings).
4. At the edges of the view a slight offset and a stretched ring are expected (tracker accuracy, lens correction); the 2×2 ring around the spot covers it in normal use.

The status line printed every 5 s (`VPX_XR_METRICS=1`, also in the VR settings page) tells which gaze source is in use: `Foveation: Eye-tracked (shading rate image) (gaze ray L x,y R x,y)` is the ray of `XR_EXT_eye_gaze_interaction`; `(gaze L …)` without `ray` is the runtime's foveation center (fallback); `Fixed: eye tracking not active` means the headset is not worn or eye tracking is off in its settings. When the ray cannot be located, `Eye gaze ray unavailable: locate result …, flags …, action active …, profile …` is logged every 5 s with the reason (action inactive and profile `none` while the headset is off the head is normal). The values are each eye's image coordinates, −1..1, x to the right, y down; with the head straight the two eyes differ by ≈ 0.32 in x.

The mode is also the quickest way to feel the cost side: the frame time with it is the 4×4 floor of the table (≈ 11 ms on Addams Family at 2160²), what no foveation profile can go below.

## 9. Limitations and notes

- **Density map not updated per frame**: `fragmentDensityMapDynamic` is false on Turnip; the own map is uploaded on creation and on level changes only, which bgfx orders before the frame's passes. Everything per-frame goes through the offsets. The map is per level, not per table.
- **Offsets granularity**: 8 pixels; the fovea's position is quantized to that, invisible.
- **MSAA**: untested with the map (the Frame build runs with MSAA off; the patch handles the resolve attachments but no measurement was made).
- **Validation**: the device's old validation layer did not know `VK_VALVE_fragment_density_map_layered`; a newer layer built in the container would be needed for a complete check.
- **Other platforms**: the bgfx patch would work on any Vulkan driver with `VK_EXT_fragment_density_map` (AMD and Qualcomm; NVIDIA has only VRS), and the OpenXR part on any runtime with the FB/META extensions (Quest). It is deliberately kept Linux ARM64 only until upstreamed to the bgfx fork.
- **Readability**: text in the scene (DMD, backglass) is coarser when not looked at; with eye tracking this is never seen, with fixed foveation Low is the sensible level.
- **Known unrelated bug**: shutting down while waiting for the headset's first frame hangs.

## 10. Files

`src/core/Settings_properties.inl` (settings) · `src/renderer/VRDevice.h/.cpp` (OpenXR, profile, gaze ray and runtime center, density map and shading rate image, debug ring, status, measurement) · `src/input/XRInputHandler.h` (eye gaze pose action) · `platforms/linux-aarch64/bgfx-fragment-shading-rate.patch` (bgfx, shading rate attachments) · `src/renderer/XRGraphicBackend.h`, `src/renderer/XRVulkanBackend.h` (device extensions and features, runtime map wrapping) · `src/renderer/RenderTarget.h/.cpp`, `src/renderer/Renderer.h/.cpp` (framebuffer variants, offsets) · `src/renderer/RenderDevice.h/.cpp`, `src/renderer/RenderPass.cpp` (view names for profiling) · `src/ui/live/ingameui/VRSettingsPage.cpp` (UI) · `platforms/linux-aarch64/bgfx-fragment-density-map.patch`, `platforms/linux-aarch64/external.sh` (bgfx) · `docs/Steam Frame Branch.md` section 9 (summary) · `Doc/foveated-rendering-plan.md` (evaluation and plan) · this file.

## 11. Shading rate image: the gain recovered (2026-10-03)

**Why it works where the density map did not.** Turnip only applies density maps in its tiled path and forces any pass with one into it (§7). The fragment shading rate attachment (`VK_KHR_fragment_shading_rate`) is programmed at subpass begin in the command stream common to both paths (`tu7_emit_subpass_shading_rate`, a7xx), and `tu_autotune.cc` has no rule forcing a pass with a rate attachment into GMEM: the scene pass stays direct. Device properties on the Frame: `attachmentFragmentShadingRate` and `pipelineFragmentShadingRate` true, texel size 8×8 (min = max), `maxFragmentSize` 4×4, `layeredShadingRateAttachments` false (one rate image for both layers of the stereo scene buffer), `fragmentShadingRateWithShaderDepthStencilWrites` true.

**bgfx** (`platforms/linux-aarch64/bgfx-fragment-shading-rate.patch`, applied after the density map patch, cache tag `-turnip4`): `BGFX_TEXTURE_FRAGMENT_SHADING_RATE` (usage `FRAGMENT_SHADING_RATE_ATTACHMENT`, kept in the `FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL` layout, barrier stages/accesses), `BGFX_RESOLVE_FRAGMENT_SHADING_RATE` tagging the attachment (kept out of the color/depth lists like the density map), `getRenderPass` converting its description to the render pass 2 structures with `VkFragmentShadingRateAttachmentInfoKHR` on the subpass when a rate format is given (`vkCreateRenderPass2KHR`, imported as optional; the attachment is ignored without it), a single layer 2D view appended to the `VkFramebuffer`, `VkPipelineFragmentShadingRateStateCreateInfoKHR { 1x1, KEEP, REPLACE }` on pipelines of such frame buffers and the same combiner in bgfx's own dynamic shading rate call, so the attachment's rate replaces the pipeline's. Pipelines are already keyed by the render pass hash, which includes the rate format and texel size.

**Device** (`XRVulkanBackend.h`): enables `VK_KHR_fragment_shading_rate` with the pipeline and attachment features (and the primitive one when available, which bgfx's own detection requires) plus `VK_KHR_create_renderpass2`, reads the texel size and max fragment size, exposes `IsFragmentShadingRateSupported` / `GetFragmentShadingRateTexelSize` / `GetFragmentShadingRateMaxFragmentSize`. Logs "Fragment shading rate attachments: supported, texel 8, fragments up to 4x4".

**Application** (`VRDevice::UpdateShadingRateMap`, `RenderTarget::SetFragmentDensityMap(map, shadingRate)`): one R8U image of ceil(W/8) × ceil(H/8) texels sized on the scene buffer (270×270 at 2160), a single layer. Each frame, with foveation on: `ReadGaze` (`xrGetFoveationEyeTrackedStateMETA`, the runtime profile applied by `ApplyFoveation` is what turns the eye tracking on), then the image is rewritten with full rate (code 0) inside a disc of radius `kFoveationFullRadius[level]` around each eye's gaze in the frame's rendered pixels (the dynamic resolution scale applies, the discs' union since one image serves both eyes), 2×2 (code 5) up to `kFoveationHalfRadius[level]`, 4×4 (code 10, or 2×2 when the device's max fragment is smaller) beyond; same sign conventions as the density map offsets (`FoveationFlipX/Y`). Without a valid gaze, level Low at the center whatever the setting. The upload (`bgfx::updateTexture2D`, 73 KB) only happens when a disc moved by more than half a texel or the level or scale changed. The density map path is used when the driver has no shading rate attachments; the status line of the settings page says which ("Eye-tracked (shading rate image)").

**Measurements** (fixed images through a temporary hook, before the gaze was wired; native 2160², dynamic resolution off, medians of 9 samples over 40 s): Ghostbusters baseline 11.3 ms → 4×4 everywhere 9.1, 2×2 everywhere 9.7, High rings 9.2, Low rings 10.0, baseline again (hotter) 11.6; Addams baseline 14.9 → 4×4 everywhere 11.0, High rings 11.6, Low rings 12.1, baseline again 15.1. The 4×4 floor (≈ 9 / 11 ms) is the non-fragment cost. Visually: 4×4 everywhere unusable, 2×2 soft, Low rings fixed at the center indistinguishable from the baseline, High rings fixed at the center visibly sharp in the wrong place (hence the gaze). Eye-tracked results with the setting and the gaze ray (Addams Family, same conditions, headset at 67–71 °C, medians of 9 samples): Off 14.3 ms, High 11.2, Medium 11.3, Low 11.5, Off again 14.8 — ≈ 3 ms (22 %) per frame at every level, against the 11.0 ms upper bound of 4×4 shading everywhere. The levels differ in how much of the periphery is coarse, hardly in cost on this table, where the full quality discs of the two eyes are what remains to shade.

**A bug found on the way, which also taints the density map numbers.** bgfx makes a texture created with initial data immutable and silently drops every later `updateTexture2D` on it (`bgfx.cpp`, `immutable = NULL != _mem`; `Context::updateTexture` warns in debug builds only). The own density map of §4 was created with data (full density everywhere) and then filled by `FillOwnFoveationMap` through updates, which never landed: **the density map path always rendered with full density everywhere**, so its measured "≈ 0.5 ms at High" was the cost of the tiled path with no foveation at all, and the gaze offsets moved a uniform map. The forced tiling of §7 is real and measured independently (`TU_DEBUG=sysmem`), but what a density map with real rings would give in the tiled path was never measured. The first eye-tracked build of the shading rate image hit the same trap (created with zeros, then updated: 15 ms, no gain, while the fixed test image of the table above, created with its data, worked). Both textures are now created without data and filled by an update.

**Gaze source** (`VRDevice::LocateGaze`): the runtime's foveation center (`xrGetFoveationEyeTrackedStateMETA`) is a per eye image position as sampled; by the time the frame is displayed the head has turned and the eyes have counter-rotated, so the spot lagged behind a fixated object while the head moved, and it jittered. The gaze is now taken from `XR_EXT_eye_gaze_interaction` as a pose (action `/user/eyes_ext/input/gaze_ext/pose` appended to the input handler's table, index 43, bound on `/interaction_profiles/ext/eye_gaze_interaction`; SteamVR vouches for the orientation only, so the ray starts between the eyes): located at its sample time (`XrEyeGazeSampleTimeEXT`), so head and eyes are consistent and the ray of a fixated point is the right one whatever the head did since, then a fixation point at an assumed 1 m on that ray, smoothed with a 35 ms filter reset on saccades (> 8 cm), is projected with the views of the frame into each eye's image through its FOV. The runtime's center stays as the fallback (the eye-tracked foveation profile at level NONE keeps it valid). Verified in the headset with the debug mode of section 8 (`VPX_FOVEATION_DEBUG`): the spot follows the eyes and stays on a fixated object while the head turns. That check also found the vertical sign of the runtime's center mirrored: `FoveationFlipY` now defaults to false (the earlier "settled" value had been judged against a uniform density map, see above).

**Runtime profile**: with our own image (shading rate or density map with offsets) the profile applied to the swapchain is level NONE with the eye-tracked chain, which keeps the gaze valid (verified) without asking the runtime to do anything with the images; the level is only passed where the runtime's own maps are the fallback.

**Eye-tracked profiles**: one image for both eyes means each eye's disc is also shaded at full rate in the other eye's image, 0.3 of the half width away (the asymmetric per eye views), so the gaze-driven rings are tighter than the fixed ones: full / half radius 0.15 / 0.28 (High), 0.20 / 0.38 (Medium), 0.30 / 0.55 (Low); without a valid gaze the single wide disc of the fixed Low profile (0.45 / 0.80).

**Default**: `Foveation` is Medium on standalone OpenXR builds again.

### 11.1 Challenges met on the way, and what settled them

In the order they came, with the dead ends, since each one cost a build-and-run cycle on the device (about 5 minutes each, the headset worn):

| Challenge | What was suspected or tried | Resolution |
|---|---|---|
| **Which foveation mechanism can work at all on Turnip.** Density maps had given 0.5 ms because they force the tiled path (§7). | Read the driver source for the other mechanism: `tu7_emit_subpass_shading_rate` is emitted in the command stream common to both paths, and `tu_autotune.cc` has no rule forcing a pass with a rate attachment into GMEM. | A bounded upper-bound test first (a fixed 4×4 rate image everywhere, through a temporary `VPX_VRS_TEST` hook) before any real implementation: Addams 14.9 → 11.0 ms. Only then the eye-tracked version. |
| **bgfx has no shading rate attachments**, and its render passes are the v1 structures, which cannot reference one. | — | Third patch: the render pass description is converted to the v2 structures (`vkCreateRenderPass2KHR`) only when a rate format is given; the attachment is tagged like the density map (`BGFX_RESOLVE_FRAGMENT_SHADING_RATE`) and kept out of the color/depth lists; the pipeline combiner is `KEEP, REPLACE` both in the static state and in bgfx's own dynamic `vkCmdSetFragmentShadingRateKHR` (bgfx uses the dynamic state when the device has the pipeline and primitive features, which the application now enables). |
| **The eye-tracked build gave no gain at all** (15 ms, the baseline) while the fixed test image had given 11.6. | 1. Valve's FDM injection layer waking up when a foveation profile is applied: disabled with `DISABLE_VULKAN_FDM_INJECTION_LAYER=1` (verified gone from the log) → still 15 ms; its `FDM_DEBUG=1` output showed it hooks nothing of ours. 2. The runtime profile level itself: forced to NONE and LOW through a temporary hook → still 15 ms. 3. The driver's trace (`MESA_GPU_TRACES=print`): the scene pass direct, no density map anywhere, so the rate attachment was simply not in effect. 4. The only difference left with the test hook was "created with data" versus "created empty then updated". | **bgfx makes a texture created with data immutable and silently drops `updateTexture2D` on it** (`bgfx.cpp`, `immutable = NULL != _mem`; a `BX_WARN` in debug builds only). The rate image stayed 1×1 everywhere; the own density map of the first implementation had stayed uniform the same way (its earlier measurement therefore meant nothing). Both textures are created without data and filled by an update. |
| **The sharp zone sat at the center and did not follow the eyes** once the image did update. | The gaze values moved over a plausible range, so the data was fine; the two spots per eye (one image for both eyes, see below) made it hard to judge by eye. | The debug mode of §8: a small spot, 4×4 everywhere else, a magenta ring around each spot (drawn where the rate image puts it, so ring and spot cannot disagree), and modes to show one eye's spot only. |
| **Vertical axis mirrored.** | The density map "flip" settings had been "settled on the device" against a uniform map (see above), so they had never been validated. | `FoveationFlipY` default false, checked with the ring. The horizontal axis was right: the constant 0.32 difference between the two eyes' x is the asymmetric per eye field of view (straight ahead is at ≈ ±0.17 in each eye's image), not a sign error. |
| **Jitter, and the spot drifting with the head while the eyes held an object.** | The runtime's foveation center is an image position as sampled; by display time the head has turned and the eyes counter-rotated. Smoothing alone would only hide the jitter. | `XR_EXT_eye_gaze_interaction`: the gaze as a pose, located at its own sample time (head and eyes consistent), a fixation point at 1 m on that ray, projected with the views of the frame, smoothed over 35 ms and reset on saccades. The runtime's center stays as the fallback. |
| **The gaze ray was never valid** (first two builds). | 1. The location required `POSITION_VALID`, which SteamVR does not set for the gaze pose: orientation only, origin between the eyes. 2. The point was transformed with `XrPosef_ToMatrix3D`, a helper shaped for VPX's rendering conventions, which put it "behind the eye" every frame, silently. 3. Two edits were lost when an edit script aborted on a later assertion (the status tag), so a build ran without the fix it was meant to test. | Direct quaternion transform into the eye's view space, every exit of `LocateGaze` logged (rate limited), and the status line tagged with the gaze source (`gaze ray` / `gaze`) so the log, not the eye, says which path runs. |
| **Low and Medium gave less than the fixed-image test** (Low: nothing). | One image for both eyes: each eye's full quality disc is also shaded at full rate in the other eye's image, 0.32 of the half width away, so the fixed-profile radii covered most of the image at ≤ 2×2. | Tighter rings for the gaze-driven image (High 0.15/0.28, Medium 0.20/0.38, Low 0.30/0.55), the wide single disc kept for the fixed fallback. With them every level reaches ≈ 11.2–11.5 ms on Addams. |
| **Session hygiene.** | Deploying over a running instance (one measurement run collided with a previous instance still alive), a script's `pkill -f` matching its own ssh command, the headset's `/tmp` cleared by a sleep, runs collecting 0.18 ms frames because the headset was not worn. | Check `pgrep -x VPinballX_BGFX` before every deploy, `pkill -f "[f]ov-setting.sh"`, copy the scripts again after a sleep, and wait for real frame times (> 1 ms) before measuring. |

## 12. How to reproduce the measurements

On the device, from ssh (`cd ~/devkit-game/vpx_frame`), with the table's path in `TABLE` and `ResFactor = 0.2637` in `~/.local/share/VPinballX/10.8/VPinballX.ini`:

```
# frame time only (no profiler), the status line every 5 s in the output (VPX_XR_METRICS enables the runtime's counter)
VPX_XR_METRICS=1 LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE" 2>&1 | grep --line-buffered gpu_frametime

# the same with direct rendering forced / the measured autotuner
VPX_XR_METRICS=1 TU_DEBUG=sysmem LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE"
VPX_XR_METRICS=1 TU_AUTOTUNE_ALGO=profiled LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE"

# per-target breakdown
VPX_GPU_PROFILE=1 LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE" 2>&1 | grep --line-buffered "GPU by target"

# the driver's GPU trace (large: ~4 MB/s), then look for end_render_pass lines: tiledRender, tilingDisableReason, drawCount, lrzStatus
MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=/tmp/gputrace.txt LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE"
```

The headset must be worn: frames (and therefore every number above) only flow while the runtime considers the session visible. A run that prints `gpu_frametime 0.18 ms` with no breakdown is a run during which nothing was rendered.
