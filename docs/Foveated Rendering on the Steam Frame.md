# Why eye-tracked foveated rendering does not speed up Visual Pinball on the Steam Frame

*Technical article, 2026-10-02. Branch `foveated-rendering` of `SPD13/vpinball`. Measurements were taken on a Valve Steam Frame (Snapdragon 8 Gen 3, Adreno 750, SteamOS ARM64, Mesa 26.3.0-devel git-8aa73b4b19 Turnip driver, SteamVR/OpenXR 2.17.10). The implementation itself is described in `docs/Steam Frame Branch.md`, section 9, and in `Doc/foveated-rendering.md`; this article is about the performance question only.*

## Summary

We implemented eye-tracked foveated rendering end to end — OpenXR foveation profile, eye tracking through `XR_META_foveation_eye_tracked`, a Vulkan fragment density map attached to the scene buffer through a bgfx patch, gaze-following through density map offsets — and it works: the sharp zone follows the eyes on both axes, the periphery is visibly shaded at a quarter of the density. Yet on a heavy table at native resolution (2160×2160 per eye) it saves about **0.5 ms of a 14 ms frame**. The reason is not in our implementation but in how the Adreno driver executes it:

1. On Turnip, a fragment density map is only applied in the **tiled (GMEM) render path**, and the driver's autotuner **forces** any render pass that carries a map into that path.
2. In the tiled path, the scene pass of a heavy table is **geometry-bound**: the driver re-processes the visible geometry once per tile (48 tiles at 2160×2160), which costs more than all the fragment shading the map can remove. Fragment density maps reduce fragment work only.
3. The same pass rendered **directly** (no tiles) takes half the time — 4.7 ms instead of 9.5 ms — and brings the frame from **14.0 ms to 9.1 ms**, under the 13.9 ms budget of 72 Hz at native resolution, *without* foveation. The driver did not choose that path on its own because its default autotuner estimates bandwidth and does not model per-tile geometry cost.

The practical consequence for Visual Pinball on the Frame is the opposite of what we set out to do: the gain comes from telling the driver to *measure* instead of estimate (`TU_AUTOTUNE_ALGO=profiled`, set by the application at startup), and foveation is left available but off by default.

## 1. Why we expected a gain

Foveated rendering shades the image at full quality only around the point of regard. The expected benefit is proportional to the share of frame time spent shading fragments and to how much of the image can be shaded coarsely. With a High profile the map we use has full density within a radius of 0.20 of the half width (about 11° around the gaze), half density up to 0.38, and quarter density beyond: 7 % of the image at full, 17 % at half, 76 % at quarter, so the number of shaded fragments falls to roughly a third. Visual Pinball's scene pass at native resolution looked like a fragment-heavy pass: 2160×2160 × 2 eyes of HDR colour with per-pixel lighting, reflections, and transparent layers, and it did scale with resolution — 9.8 ms at 2160², 6.1 ms at 1728² (0.64× the pixels, 0.63× the time). Everything pointed at a pixel-bound pass that foveation would cut substantially.

## 2. What was built and verified first

The pipeline was checked piece by piece before measuring, so that a missing gain could not be a non-working feature:

- The runtime accepts the foveation profile and reports the eye-tracked state as valid with moving gaze centres (`xrGetFoveationEyeTrackedStateMETA`, logged every 5 s).
- The Vulkan API dump shows the render pass of the scene buffer created with `VkRenderPassFragmentDensityMapCreateInfoEXT`, the map as an extra attachment of its framebuffer, and the per-layer map (one per eye) through `VK_VALVE_fragment_density_map_layered`.
- The gaze offsets reach the driver (`vkCmdEndRenderPass2` with `VkRenderPassFragmentDensityMapOffsetEndInfoEXT`), and after discovering that Turnip honours offsets only on images created with `VK_IMAGE_CREATE_FRAGMENT_DENSITY_MAP_OFFSET_BIT_EXT` (which the runtime's own maps lack), the application builds its own map; the sharp zone was then confirmed in the headset to follow the eyes on both axes, with the head moving while the eyes stayed on an object, and the periphery shows the expected block pattern when deliberately looked at.

So the map is in effect on the pass we meant: shading density does drop outside the fovea.

## 3. Method

All measurements on the same table, Ghostbusters (VR room version), the heaviest one in the library, at `ResFactor = 0.2637` (2160×2160 per eye, the panel's native resolution), the table started directly (`-Play`) from an ssh session on the device, the player idle in the room looking at the table, 50 s of samples per run after the first frame. Two instruments:

- **The runtime's counter** `/perfmetrics_meta/app/gpu_frametime` (`XR_META_performance_metrics`): the application's GPU time per frame as the compositor sees it. It is the ground truth for the frame budget. It saturates near the 72 Hz period when the application cannot keep up, which hides *how much* a configuration misses by; it is reliable under the budget.
- **A per-render-target breakdown** from bgfx's GPU profiler (`VPX_GPU_PROFILE=1`), summed per target (`BackBuffer` = the scene pass, `Playfield` = the playfield reflection probe, `VRSwapchain` = tonemap into the swapchain, `BloomBuffer`). It locates the time but has one distortion: with the profiler on, bgfx ends the Vulkan render pass at every view change, so the absolute numbers are a little higher than in normal operation. All comparisons were therefore repeated without the profiler, reading the runtime counter only.

The driver was steered with its documented environment variables (`TU_DEBUG=sysmem|gmem|noubwc`, `TU_AUTOTUNE_ALGO=profiled|prefer_sysmem`) and one run recorded Turnip's own GPU trace (`MESA_GPU_TRACES=print`, `MESA_GPU_TRACEFILE`), which timestamps every render pass, tile, load, store and clear on the GPU and states, for each render pass, the render mode and why it was chosen.

## 4. Results

### 4.1 Foveation levels, default driver behaviour

| Foveation | Scene pass (profiler) | Frame GPU time (no profiler) |
|---|---|---|
| Off | 9.5–10.7 ms | 13.7–14.1 ms (median 14.0) |
| Medium, eye-tracked | — | 13.5–15.0 ms (median 13.8) |
| High, eye-tracked | 7.9–9.4 ms while looking at the table; 4.7 ms in one sample with the gaze off to the side | 12.4–14.0 ms (median 13.9) |

Three levels, one frame time. The one revealing sample is the 4.7 ms scene pass at High while the gaze was away from the table: the cost is concentrated where the table is, which is also where the player looks, so the high-density zone sits on the expensive region by construction. But even then, a pass that shades a quarter of the periphery should have moved far more than 0.5 ms if shading were the cost.

### 4.2 Forcing the render path

| Driver setting | Foveation | Scene pass (profiler) | Other passes | Frame GPU time (no profiler) |
|---|---|---|---|---|
| default (tiled chosen by the autotuner) | Off | 9.5–10.7 ms | Playfield 1.1, swapchain 1.7, bloom 0.9 | 13.7–14.1 ms |
| `TU_DEBUG=sysmem` — direct rendering forced for every pass | Off | **4.5–4.8 ms** | unchanged | **9.0–9.7 ms (median 9.1)** |
| `TU_DEBUG=gmem` — tiled forced for every pass | Off | 9.4–9.5 ms | Playfield 4.2, swapchain 2.9 | — |
| `TU_DEBUG=gmem` | High | 9.2–9.9 ms | idem | — |
| `TU_AUTOTUNE_ALGO=profiled` — the driver measures both modes per pass | Off | — | — | **8.9–10.0 ms (median 9.3)** |
| `TU_AUTOTUNE_ALGO=prefer_sysmem` | Off | — | — | **8.9–9.3 ms (median 9.1)** |
| `TU_DEBUG=noubwc` (no framebuffer compression) | Off | run invalid: rendering stopped mid-run | | |

Reading this table:

- The scene pass was already tiled by default (forcing tiled changes nothing for it) and the other passes were already direct (forcing tiled makes Playfield 4× and the swapchain pass 1.7× slower). The autotuner got those right and the scene pass wrong.
- Direct rendering halves the scene pass. Nothing else in the frame changes.
- Foveation inside the tiled path (`gmem` + High) changes nothing: the tiled scene pass costs the same with a quarter-density periphery as with full density. Whatever the tiled pass spends its time on, it is not fragment shading.
- The driver's own measured autotuner reaches the direct-rendering result by itself.

### 4.3 What the GPU trace says about the tiled scene pass

One frame from the trace, steady state, foveation High:

| Render pass | Mode | Draws | Time |
|---|---|---|---|
| 540×540, 1 draw (×3) | direct | 1 | 0.2–0.7 ms |
| 2160×2160, depth, 1 draw, 2 loads | direct | 1 | 1.6 ms (swapchain tonemap) |
| 2160×2160, depth, 436 draws | direct — "Autotune selected sysmem" | 436 | 1.1 ms (Playfield) |
| **2160×2160, depth, 509 draws** | **tiled — 48 bins of 288×384** | 509 | **9.5 ms (scene)** |

Inside the tiled scene pass:

| Component | Time |
|---|---|
| Binning pass (visibility stream, geometry processed once) | 0.56 ms |
| Per-bin draws, summed over the 48 bins | ≈ 6.5 ms (heavy bins over the table up to 650 µs, bins of room wall 80 µs) |
| GMEM stores (colour + depth of each bin written to memory) | 0.92 ms |
| Per-bin clears and tail | ≈ 1.5 ms |

The per-bin draw total alone (6.5 ms) exceeds the time of the *whole pass* rendered directly (4.7 ms, which includes all the vertex and all the fragment work once). A tiled renderer runs the vertex stage of every primitive once per bin the primitive touches; a pinball table seen from a VR standpoint is a dense mesh that spans most of the bins, so its geometry is transformed many times over. That is the cost that scales with resolution (more bins at 2160² than at 1728²: this is why the pass looked pixel-bound), and the cost that a density map cannot touch: the map scales the *contents* of a bin (fewer fragments per bin), not the *number* of bins nor the geometry each bin re-processes.

The same trace also explained why the Off/High difference is as small as 0.5 ms rather than, say, 2 ms: it reports `lrzStatus=DISABLED` for the scene pass from draw 394 on, reason "Depth write + ALWAYS/NOT_EQUAL" and "Depth write + no color writes". LRZ is Adreno's low-resolution early depth rejection; without it, occluded fragments of later draws are shaded and then discarded. The draw is Visual Pinball's VR visibility mask (the lens-hidden area), drawn with depth write, no colour write and an always-pass depth test; the driver turns LRZ off for the rest of the pass after it. This is a side finding; it affects fragment cost in both render paths and is fixed separately: drawn with `Z_LESSEQUAL`, which gives the same result because the mask's vertex shader writes the near plane, which removed the hard LRZ invalidation (`lrz=true` in the next trace). That trace still showed LRZ *writes* stopping at the mask draw, because Turnip only tolerates a depth-only draw at the *start* of a pass — and the mask was draw 394, not draw 0. The reason was a second, older bug: a draw's sort key in Visual Pinball is `depthBias − z`, and keys far apart are ordered highest first; the mask was submitted with z = 200000 and no bias, a key of −200000, which sorted it *after* every opaque part. The comment next to it said the opposite. So the mask had never worked as an early depth mask at all: the lens-hidden area was shaded by every opaque draw and only then masked. Submitting it with a positive bias of 200000 puts it first; the trace of that build reads `lrz=true` with no disable reason for the scene pass, and the only remaining write-disable is "Depth write + blending" at the first transparent part (Visual Pinball's transparent parts write depth; Turnip's conservative LRZ stops tracking there, which is inherent), so LRZ now covers every opaque draw.

### 4.4 Why the driver forces tiling with a map, in its own words

Turnip's autotuner (`src/freedreno/vulkan/tu_autotune.cc`):

> If the user is using a fragment density map, then this will cause less FS invocations with GMEM, which has a hard-to-measure impact on performance because it depends on how heavy the FS is in addition to how many invocations there were and the density. Let's assume the user knows what they're doing when they added the map, because if SYSMEM is actually faster then they could've just not used the fragment density map.

```
if (pass->has_fdm)
   return forced("Uses a fragment density map", render_mode::GMEM);
```

And the direct path has no density map handling at all (`tu_cmd_render_sysmem` versus `tu_cmd_render_tiles`, `tu_cmd_buffer.cc`), which is natural: the Adreno implementation of FDM is per-bin scaling — each bin is rasterized at a lower resolution and expanded on its way out of GMEM — and there are no bins in direct rendering.

The default autotuner algorithm, `BANDWIDTH`, compares an estimate of the memory traffic of each mode (attachment bytes loaded and stored for tiled, draw-call write bandwidth for direct) and does not model the per-bin geometry re-processing. For a geometry-heavy pass with 16 bytes per pixel of attachments it estimates tiled as cheaper, and picks it. The `PROFILED` algorithm instead renders a pass in both modes over time, measures, and converges on the faster one, which here is direct rendering.

## 5. Conclusions

1. **Foveated rendering is not broken; it is the wrong lever for this workload on this driver.** Everything from the OpenXR profile to the per-bin density scaling works and is visible. It removes fragment work, and fragment work is not what the scene pass is bound by once the driver tiles it.
2. **The bottleneck is tiled rendering of a geometry-heavy pass.** On a tile-based GPU, Visual Pinball's scene pass — hundreds of draws of dense meshes covering most of the view — pays for its geometry once per tile. Direct rendering pays once. The difference is ≈ 5 ms per frame at native resolution, 35 % of the frame.
3. **Using a density map locks the pass into the expensive path.** So on Turnip, "foveation on" means "tiled", and tiled costs more than foveation can save. Foveation would only pay on a pass whose fragment shading dominates even after the per-tile geometry cost, or on a driver that applies density maps in direct rendering, which Turnip does not.
4. **The fix is to let the driver measure.** `TU_AUTOTUNE_ALGO=profiled` (or `prefer_sysmem`) set in the environment before the Vulkan driver loads gives 9.1–9.3 ms frames at native resolution with the table that was at 14.0 ms. The application now sets it itself for Linux standalone builds (`src/core/main.cpp`, without overwriting a value the user set).
5. **Foveation defaults to Off** on the Frame and stays available in the VR settings page, for tables that would turn out fragment-bound and for other drivers.
6. **The visibility mask no longer disables LRZ and is drawn first**, verified in the driver's trace (`lrz=true` through all the opaque draws). Frame time with everything in place, nothing set in the environment: **9.0–9.2 ms** at native resolution on the heaviest table.

## 6. What this changes in the plans

- Native resolution at 72 Hz is reachable on the heaviest table in the library without foveation, with headroom (≈ 4.5 ms). That headroom is the first candidate for MSAA or higher reflection quality, to be measured the same way (runtime counter, no profiler).
- The per-tile geometry cost means every reduction in geometry (fewer draws, no re-submission for reflection probes that are not needed in VR) is worth more on the Frame than on a desktop GPU. The Playfield reflection pass (436 draws, 1.1 ms) is the next thing to look at.
- The foveation work keeps its value as infrastructure: the bgfx patch (density map attachments, layered maps, offsets) is general, and the measurement tooling (runtime counters, per-target breakdown, Turnip trace parsing) is what found the real bottleneck. Should Turnip gain density map support in direct rendering, or should a table be fragment-bound, turning the setting on is all it takes.

## 7. Appendix: how to reproduce

On the device, from ssh (`cd ~/devkit-game/vpx_frame`), with the table's path in `TABLE` and `ResFactor = 0.2637` in `~/.local/share/VPinballX/10.8/VPinballX.ini`:

```
# frame time only (no profiler), the status line every 5 s in the output
LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE" 2>&1 | grep --line-buffered gpu_frametime

# the same with direct rendering forced / the measured autotuner
TU_DEBUG=sysmem LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE"
TU_AUTOTUNE_ALGO=profiled LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE"

# per-target breakdown
VPX_GPU_PROFILE=1 LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE" 2>&1 | grep --line-buffered "GPU by target"

# the driver's GPU trace (large: ~4 MB/s), then look for end_render_pass lines: tiledRender, tilingDisableReason, drawCount, lrzStatus
MESA_GPU_TRACES=print MESA_GPU_TRACEFILE=/tmp/gputrace.txt LD_LIBRARY_PATH=$PWD DISPLAY=:1 ./VPinballX_BGFX -Play "$TABLE"
```

The headset must be worn: frames (and therefore every number above) only flow while the runtime considers the session visible. A run that prints `gpu_frametime 0.18 ms` with no breakdown is a run during which nothing was rendered.
