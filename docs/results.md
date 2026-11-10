# Settings A/B results

Older save with more characters, same camera spot, UI on, 1× speed. Each row is a 30 s MangoHud log (Left Shift+F2); raw CSVs are in `mangohud/`. Settings are cumulative: each row keeps the changes above it.

| Test | Change | Avg FPS | Frame time | 1% low | 0.1% low | GPU load |
|---|---|---|---|---|---|---|
| baseline | settings.cfg as of 2026-10-18, `cachedDynamicResources = a` | 52.2 | 19.1 ms | 38.5 | 33.7 | 55% |
| test1 | `npc range` 4096 → 3000 | 57.2 | 17.5 ms | 44.1 | 43.4 | 59% |
| test2 | `objects view range` 4096 → 3000 | 55.7 | 18.0 ms | 47.5 | 44.2 | 58% |
| test3 | `Shadow Range` 5000 → 3000 | 56.3 | 17.8 ms | 36.4 | 29.4 | 58% |
| test4 | `Show names` + `floaters` 1 → 0 | 56.6 | 17.7 ms | 46.0 | 43.2 | 59% |
| test5 | `grass range` 3 → 1.55, `grass density` 0.5 → 0.25 (`foliage range` left at 1) | 57.0 | 17.5 ms | 47.2 | 44.8 | 58% |

| test6 | **0 mods** (`data/mods.cfg` emptied), settings back to baseline | 52.0 | 19.2 ms | 44.5 | 44.0 | 54% |

test6 vs baseline: same average (52.0 vs 52.2). Main-thread profile is also nearly identical (OgreMain 40.6%, d3d11 12.7%, RenderSystem 10.7%, Kenshi 9.3%, MyGUI 7.6%, ParticleUniverse 6.4%; ~82% busy). **The ~89-entry mod list costs nothing measurable here.** The cost is vanilla Kenshi/Ogre. Better lows only reflect the baseline run's 2 isolated hitches.

## Engine patch: MyGUIQuadFix plugin

| Test | Change | Avg FPS | Frame time | 1% low | 0.1% low | GPU load |
|---|---|---|---|---|---|---|
| test7 | All mods + **MyGUI Quad Fix** plugin, settings at baseline | 53.5 | 18.7 ms | 43.7 | 41.9 | 55% |

- Plugin log: `patched: VertexQuad::set`. UI renders normally.
- Main thread: **MyGUI 6.0–7.6% → 1.65%**. The `VertexQuad::set` hot spot is gone from the profile; MyGUI's remaining samples are spread out (top bucket 0x2c4c0).
- FPS: +1.3 over baseline/no-mods (52.2 / 52.0 → 53.5), about 0.5 ms per frame. Modest, but consistent with removing ~5% of a thread that's ~82% busy.
- The user hid the UI for ~5 s during this session. Neither the MangoHud log (2 s windows) nor the perf timeline shows a step anywhere, so **UI on vs off is now within noise**. Before the patch it was 3 ms (first save) / 1.2 ms (this save).

## Conclusion (2026-10-18)
**Only `npc range` made a real difference** (52.2 → 57.2 FPS, 1% low 38.5 → 44.1). Tests 2–5 all land at 55.7–57.2 FPS, inside run-to-run noise. At this spot, objects range, shadow range, names/floaters and grass cost nothing measurable, so those can go back to their original values for visuals.

The game stays main-thread bound (GPU ~58%). Remaining cost is mostly Ogre's scene work, which scales with the number of characters in range, which is why `npc range` was the one lever that mattered. Next: try `npc range` 2500 and `Global population multiplier` 1.19 → 1.0, and test a busy town, not just this spot.

Notes:
- test2: average within run-to-run noise (~±1.5 FPS); 1% low slightly better. No clear gain at this spot.
- test3: average unchanged. The worse lows come from 2 isolated hitches (max 34 ms) in 300 rows, likely one-off, not steady cost. No clear gain.
- test4: average unchanged, no hitches (max 23 ms). Names/floaters cost nothing measurable at this spot (few NPCs in view after npc range 3000).

## Investigation: RenderSystem_Direct3D11 hot spots (with quad fix, ~11.8% of main thread)

Function-level breakdown from `.pdata` (`scripts/funcs.py`):

| Share of main thread | RVA | Function |
|---|---|---|
| 2.8% | 0x50a10 | `D3D11VertexDeclaration::getILayoutByShader`: `std::map<program*, ID3D11InputLayout*>` find; `CreateInputLayout` only on a miss |
| 2.0% | 0x36f50 | `D3D11RenderSystem::bindGpuProgramParameters` |
| 1.7% | 0x34170 | `D3D11RenderSystem::_render` |
| ≤0.9% each | … | many small functions |

Inside `getILayoutByShader`, nearly all samples sit right after the loads of `this->map head`, the head node, the root node and child nodes. It's **cache misses on cold per-declaration map nodes**, not a deep tree. Each draw touches a different vertex declaration.

Assessment: a lookup cache could save at most ~2% of the main thread (~0.4 ms, ~+1 FPS). It also has a stale-pointer risk: layouts are released when a declaration changes, so a cache must be invalidated with them. All RenderSystem, DXVK (`d3d11.dll` ~13%) and much of OgreMain cost is **per draw call**, so the draw-call count is the real lever.

## Draw calls vs frame time (2026-10-18, DXVK_HUD)

A different save/spot, quad fix on. Logs recorded unpaused; screenshots taken paused (same draw counts either way). Raw log: `mangohud/test8-drawcalls-zoom_*` (ignore its summary line, which is corrupted by a stall; the per-frame rows are fine).

| View | Draw calls | Render passes | FPS | Frame time |
|---|---|---|---|---|
| Zoomed out | 1515 | 25 | 53.7 | ~19.3 ms |
| Zoomed in | 556 | 21 | 59.7 | ~16.7 ms |

- ~959 fewer draws save ~2.6 ms, so **~2.7 µs per draw** (Ogre + RenderSystem + DXVK together). At 1500 draws that's ~4 ms, about 21% of the frame.
- **~15 ms is left even zoomed in on a paused game.** The main-thread profile barely changes with view (OgreMain ~43%, d3d11 ~13.5%, RenderSystem ~11%, Kenshi ~10.6%, ParticleUniverse ~6.8%, still ~79% busy). The cost is spread thin: no single function above ~3% (`SceneManager::_setPass` 3.1%, `InstanceBatchHW::updateVertexBuffer` 2.3%, `Material::getBestTechnique` 2.2%, …).
- Conclusion: there's no big single hot spot left to patch. Most of the frame is per-pass scene processing. Kenshi renders the scene for several cameras (shadow cascades, water reflection, main), and each one walks the scene on the CPU.

## Render passes: shadows (2026-10-18)

Same session as the draw-call test, quad fix on. The only change between runs is `shadow mode` 2 → 0 in-game (saved at 19:16:14).

| Test | Change | Avg FPS | Frame time | 1% low | GPU load |
|---|---|---|---|---|---|
| test9 | shadows on (`shadow mode=2`) | 51.0 | 19.6 ms | 39.8 | 53% |
| test10 | **shadows off** (`shadow mode=0`) | 60.4 | 16.6 ms | 51.8 | 37% |

- **Shadows off saves ~3 ms per frame (+9 FPS)**, the largest single gain found. GPU load also drops 53% → 37%, but the game is CPU-bound, so the FPS gain is from the main thread: each shadow cascade is another full scene traversal plus draw submission.
- `Shadow Range` 5000 → 3000 (test3) did nothing; the number of shadow *passes* is what costs, not their distance.
- test10's 0.1% low (15 FPS) is a single 66 ms hitch, likely the settings change itself.
- DXVK_HUD, zoomed-out view (similar but not identical camera): shadows on **1515 draws / 25 render passes / 53.7 FPS**; shadows off **795 draws / 17 render passes / 59.6 FPS**. Shadows cost ~720 draw calls and 8 render passes per frame. At ~2.7 µs per draw that's ~2 ms, plus per-pass scene traversal, which matches the ~3 ms measured.
- test11, `shadow mode=1`, same camera as the shadows-off shot: **47.6 FPS, 21.0 ms, 1856 draws / 21 passes** (vs mode 0: 795 draws / 17 passes / 59.6 FPS). Mode 1 is **not** a cheaper middle ground; it's the most draw calls of any run. All logs were recorded **unpaused** (only the screenshots were taken paused; draw counts are the same either way), so the runs compare cleanly: mode 1 (21.0 ms) is ~1.4 ms *slower* than mode 2 (19.6 ms).
- Bottom line: shadows are a ~3 ms CPU cost; `shadow mode=0` is the one setting that rivals `npc range`.

## Shadow cost breakdown and patch feasibility (2026-10-18)

Captures `captures/shon.*` (shadow mode 2) vs `captures/shoff.*` (mode 0), same spot, unpaused, pid 20241. Normalized per frame using MyGUI as a frame counter (ratio 1.17, matching 19.6 → 16.6 ms). Script: `scripts/diff.py`.

- **Shadows = 21% of the main thread's per-frame work.** Split: OgreMain 47% (per-camera culling/render queue: `InstanceBatchHW::updateVertexBuffer`, `Material::getBestTechnique`, `Entity::_updateRenderQueue`, `QueuedRenderableCollection::merge` …), RenderSystem 23% + DXVK 20% (submitting the extra draws), Kenshi's own code only ~7%.
- Kenshi does **not** use Ogre's built-in shadow system (no `setShadowTechnique`/PSSM imports). It has its own `DeferredLightRenderer` with `CsmShadowMap` (cascaded) and `RTWShadow` (likely what `shadow mode=1` selects). KenshiLib has no symbols for any of it. The engine is an **Ogre 2.0-era fork** (`ObjectMemoryManager`, `_updateViewportCullPhase01`).
- Shadow code lives at `Kenshi_x64.exe` 0x140860000–0x140869000 (`scripts/calls.py` lists each function's Ogre calls):
  - `0x140862f60` (3.4 KB): cascade setup. Light near/far, fits an AABB, `Vector3::makeCeil` (looks like texel snapping = stabilized cascades), `Camera::setPosition`, writes the shadow matrix into `GpuSharedParameters`.
  - **`0x140863cb0`** (492 B): renders one CSM pass: calls the setup above, `_setCurrentRenderStage`, `_setViewport`, `Viewport::clear`, `_updateViewportCullPhase01`, then the scene render. Reached only through a vtable (no direct calls).
  - `0x140865010`: similar per-pass renderer with `_setShadowCasterTechnique` + visibility mask (probably RTW or a second caster pass).
- **Patch idea:** hook `0x140863cb0` and skip it for far cascades on some frames. Because the matrix setup is *inside* that call, skipping it keeps the old shadow map and its old matrix together. They stay consistent; distant shadows just update at a lower rate. Characters' shadows in far cascades would lag a frame or two. Unknowns: argument layout (cascade index), number of cascades, whether the shared-params slots are per cascade, and whether anything else clears the maps.

## Engine patch: Shadow Throttle plugin (2026-10-18)

`mods/Shadow Throttle/` (source: `plugin-src/ShadowThrottle/`). It re-renders shadow maps every Nth frame by skipping the whole shadow update on other frames: RTW prepare/setup/pass for mode 2, CSM render for mode 1. Texture, matrices and shader params stay consistent. Hotkeys: **F10** on/off, **F11** cycles N 2→3→4; changes are timestamped in `ShadowThrottle.log`.

test13: **shadow mode 1 (CSM)**, interval 2, camera still, unpaused. A/B within one 30 s log (F10 at 11.4 s and 24.0 s, from `ShadowThrottle.log`; ±1 s around each switch excluded):

| Segment | Throttle | Avg frame time | FPS |
|---|---|---|---|
| 0–11 s | ON (every 2nd frame) | 16.3 ms | 61.3 |
| 12–23 s | OFF (every frame) | 18.9 ms | 52.8 |
| 25–30 s | ON | 15.9 ms | 62.8 |

- **Throttle saves ~2.8 ms per frame (~+9 FPS) with shadows kept on**, close to the ~3 ms that shadows-off saved. Clean steps at both toggles.
- MangoHud's summary line for this log is corrupted again (a stall at the end); per-row data is fine.
- test12 (F11 cycling 2/3/4, no OFF segment) was too noisy to rank intervals.
- Still to check: visual artifacts (shadow lag on moving characters, edges while panning), and mode 2 (RTW).

### Shadow Throttle 1.1 → 1.2: fixing shadow lag (2026-10-18)

- **1.1 problem:** with throttle on, shadows visibly lagged behind the camera while panning. Cause, from `data/materials/deferred/deferred.hlsl` + `common/shadowFunctions.hlsl`: the deferred shader rebuilds positions camera-relative (`worldPos = viewPos * inverseView`, camera at origin) and looks shadows up via `shadowViewMat` (CSM) / `shadow_matrix` (RTW). Those matrices have the camera position baked in, so a stale matrix plus a moved camera shifts the shadows.
- **1.1 mode 2 measurement (test14):** throttle ON was *slower* (19.1 vs 17.7 ms) even though GPU load fell 58% → 41%. Not yet explained; see the 1.2 numbers below.
- **1.2 fix:** hooks Kenshi's imports of `GpuSharedParameters::getConstantDefinition`/`getFloatPointer` to find where each matrix lives. It saves the matrix and camera position on rendered frames; on skipped frames it writes `M' = M_saved · Translate(camNow − camAtRender)` and calls `_markDirty`. Log: both matrices captured (`RTW ok CSM ok`), 0 out-of-band fetches.
- **User test: no shadow lag while panning in either mode.**
- test15-b (1.2, mostly shadow mode 2, includes panning, so not a clean A/B): ON 16.3 / 17.4 ms vs OFF 20.1 ms (5 s segment). Points to ~3 ms saved; a clean still-camera A/B in mode 2 is still needed.

test16, **Shadow Throttle 1.2, shadow mode 2 (RTW)**, camera still, interval 2. F10 at 12.2 s and 23.1 s (±1 s around each switch excluded):

| Segment | Throttle | Avg frame time | FPS | GPU load |
|---|---|---|---|---|
| 0–11 s | OFF | 19.6 ms | 50.9 | 54% |
| 13–22 s | ON | 17.1 ms | 58.5 | 55% |
| 24–30 s | OFF | 19.6 ms | 51.1 | 54% |

- **Mode 2: ~2.5 ms saved (+7.6 FPS)**, with clean steps at both toggles. The 1.1 mode-2 slowdown (test14) does not reproduce with 1.2.
- Combined with test13 (mode 1: ~2.8 ms), Shadow Throttle recovers most of the ~3 ms shadow cost in both modes while keeping shadows on and lag-free.

## MyGUI Quad Fix 1.1: heavy UI test (2026-10-18)

1.1 adds **F6** to switch between the fix and MyGUI's original `VertexQuad::set` at runtime (timestamped in `MyGUIQuadFix.log`). The hot path stays a leaf; polling and the original call sit out of line behind a tail jump.

test17: save with **60+ controllable NPCs**, UI on, camera still, shadow mode 2 + Shadow Throttle on. F6 at 11.1 s and 21.4 s (±1 s excluded):

| Segment | Quad fix | Avg frame time | FPS |
|---|---|---|---|
| 0–10 s | ON | 26.3 ms | 38.1 |
| 12–20 s | OFF | 27.1 ms | 37.0 |
| 22–30 s | ON | 26.0 ms | 38.5 |

- **~0.9 ms saved (~+1 FPS)** with a full squad bar. Per-second noise here is ±1.5 ms, so this is a small but consistent gain, not a big one.
- This save is much heavier overall (26 ms per frame vs ~17–19 ms in the earlier spot), and the UI isn't what makes it heavy.

## Heavy save profile: 60+ controllable NPCs (2026-10-18)

Capture `captures/npc60.*`, all threads, 15 s, camera still, ~26 ms frames (38 FPS), both plugins active.

- Main thread (tid 26260) ~84% busy, so still single-thread bound. Next busiest: an unnamed Kenshi worker (~19% of a core) and `dxvk-cs` (~13%). Neither is a limit.
- Main-thread mix is **the same as in the light save**: OgreMain 43%, Kenshi 12.5%, DXVK 12%, RenderSystem 11.8%, CRT 6.8%, ParticleUniverse 5.3%, MyGUI 2.2%. More characters means more of the same per-renderable work, not a new hot spot.
- Top OgreMain exports are all per-renderable overhead: `SceneManager::_setPass` 3.8%, `GpuProgramParameters::_copySharedParams` 2.3%, `Material::getBestTechnique` 2.0%, `Entity::_updateRenderQueue` 1.5%, render-queue add/merge/clear ~3.5%, `updateAllOldAnimations` 1.2%. Shader-parameter handling together (`_setPass`, `_copySharedParams`, `_updateAutoParams`, `getParameters`, `_copySharedParamsToTargetParams`) is ~8%.
- Kenshi's own code is spread thin (top function 0.8%).
- Interpretation: cost scales with characters × sub-meshes (body, head, hair, each armour piece) × passes. The structural lever is draw count. The only sizeable patch candidate left is the shader-parameter copying (~8%), and it is risky: Kenshi writes shared params through raw pointers without marking them dirty, so Ogre can't tell when they change.

## Investigation: is Ogre's per-object rendering worth patching? (2026-10-18)

**Answer: no.** Function-accurate attribution (`scripts/funcs.py`, using `.pdata` bounds) on the 60+ NPC capture shows OgreMain (43% of the main thread) as a long flat tail: `SceneManager::_setPass` 3.75% is the largest, then a run of 0.5–2% functions (`updateAllOldAnimations`, `renderSingleObject`, SubEntity getters, render-queue adds, node updates, texture-unit setup …).

**Correction to earlier notes:** the "~8% shader-parameter copying" cluster was mostly a measurement artifact. `scripts/syms.py` names each sample after the nearest *exported* symbol, so unexported code that follows an export got counted under it. With `.pdata` bounds, `GpuProgramParameters::_copySharedParams` is 0.06% and `GpuSharedParametersUsage::_copySharedParamsToTargetParams` 0.27%.

How the copy works (disassembled): `_copySharedParamsToTargetParams` compares the shared-params version (`+0x110`) only to rebuild its copy map, then **always copies** every entry and flags the source dirty. That's stock Ogre 1.10 behaviour. The version only changes when constants are added or removed, not when values change, and Kenshi writes values through raw `getFloatPointer` without `_markDirty`, so there is no cheap "unchanged" signal. Skipping copies would need value comparisons that cost about as much as the copy, for <0.5% at stake.

Takeaway: use `scripts/funcs.py` (not `syms.py`) for any function-level numbers. The remaining main-thread cost is structural (characters × sub-meshes × passes). The only levers left are fewer objects/passes (settings, Shadow Throttle) or engine-level batching, which is out of reach for a binary patch.

## PerfProbe: per-pass CPU time, 60+ NPC save (2026-10-18)

`mods/PerfProbe/` (source `plugin-src/PerfProbe/`, active only with `KENSHI_PERF_PROBE=1`). It hooks OgreMain `Camera::_cullScenePhase01`/`_renderScenePhase02` with trampolines and times every scene pass by camera and render-queue range (exclusive time). Log: `captures/PerfProbe-npc60-2026-10-18.log`. Shadow mode 2 + Shadow Throttle (interval 2), MyGUI Quad Fix on, camera still.

The `016-017` pass runs twice per real frame, so a real frame ≈ 2 × the probe's "frame" (~28 ms, ~35 FPS).

| Pass (camera, rq range) | Share of wall | ≈ ms per real frame |
|---|---|---|
| **mainCamera 020–080**: opaque geometry (render 33–35%, cull 4%) | **37–40%** | **~10.5** |
| **rtwcamera 010–080**: shadows, throttled to every 2nd frame | **11%** | **~3** (≈6 ms per update) |
| mainCamera 082–085: transparent/particle range | 5.5–7.5% | ~1.5–2 |
| mainCamera 016–017 (×2): sky/background | 3.3% | ~0.9 |
| mainCamera 001–010, 081–082, 087–089 | ~4.5% | ~1.2 |
| **Outside scene passes**: game logic, animation, scene graph, UI, submission | **~36%** | **~10** |

- **No water-reflection camera and no portrait camera rendered** in this scene. Squad portraits are not live 3D renders, and water reflection costs nothing here.
- The opaque main pass dominates the scene work, and it is render-phase work (per-object draw submission), not culling.

### npc range A/B with PerfProbe (60+ NPC save)

Sequence (per the user): 4096 → **1000** (slider minimum) → ~3000 (saved as 3003 at 20:24:46). Log: `captures/PerfProbe-npc60-npcrange-2026-10-18.log`. Probe units are ms per `016-017` pass; a real frame ≈ 2× the probe frame.

| Windows | npc range | Main opaque pass (020–080) | rtw shadows | Probe frame → real frame |
|---|---|---|---|---|
| W20–30 | 4096 | 5.0–5.1 | 1.45–1.55 | ~12.6 → ~25 ms |
| **W31–35** | **1000** | **~3.5** | **~1.05** | ~10.9 → **~22 ms** |
| W36–41 | ~3000 | 4.9–5.1 | 1.45–1.55 | ~12.9 → ~26 ms |

- 4096 vs 3000: no difference in this save.
- **3000 → 1000: main pass −30%, shadows −30%, frame ≈26 → 22 ms (~38 → ~46 FPS).** Characters between 1000 and 3000 cost ≈3–4 ms per frame here.
- (An earlier version of this note wrongly put the W31–35 dip down to the options menu.)

## PerfProbe 1.2/1.3: what the draws are (60+ NPC save, 2026-10-18)

PerfProbe 1.2 hooks `SceneManager::renderSingleObject` (one call per object draw) and groups its time by mesh name (SubEntity) or RTTI class name (everything else). 1.3 prints the full list every 4th window. Logs: `captures/PerfProbe12-npc60-2026-10-18.log`, `captures/PerfProbe13-npc60-2026-10-18.log`. Categories are name heuristics (`scripts/objcat.py`).

- Object draws are **~33% of wall time** (main passes 26%, shadow passes 7%), ~80k draws/s (~2,300 per frame at ~35 FPS), across 368 mesh names/types.
- Printed per-row percentages lose the long tail to rounding, so the category split uses **draws/s**, which is exact. Per-draw cost is roughly uniform (~2.7 µs incl. submission).

| Category | Mesh names/types | Draws/s main | Draws/s shadow | Share of all draws |
|---|---|---|---|---|
| **Characters** (bodies, heads/morphs, hair, clothing, armour, weapons, backpacks, robot limbs, animals) | 192 | 19,088 | 9,552 | **35.9%** |
| Buildings/props (houses, interiors, farms, benches, turrets, furniture) | 72 | 9,720 | 4,889 | 18.3% |
| **Lights** (`DeferredLightRenderable` light volumes + light-fixture meshes) | 7 | 10,022 | 2,379 | **15.6%** |
| Terrain (patches, decals, distant LOD, water, sky) | 12 | 6,633 | 1,914 | 10.7% |
| Foliage (forest batches, grass) | 38 | 4,344 | 1,487 | 7.3% |
| Instanced props (`InstanceBatchHW`) | 1 | 3,273 | 2,101 | 6.7% |
| FX/2D (billboards, points, fullscreen quads, manual objects) | 5 | 4,324 | 0 | 5.4% |

- **Characters are the largest category: ~36% of draws, ≈12% of wall time, ≈3.4 ms per frame** in draw submission alone, plus their share of culling, queueing and animation outside the draw. Each character is ~10–15 separate draws (body, head + morph meshes, hair, beard, each clothing/armour piece, weapons, backpack) and again in shadow passes.
- **Deferred lights are ~15.6% of draws**: ~160 light volumes per frame plus light-fixture meshes.

### Ranked plugin candidates (from PerfProbe data)

| # | Idea | Targets | Est. gain (60+ NPC save) | Visual / gameplay risk | Engineering risk |
|---|---|---|---|---|---|
| 1 | **Character detail culling:** skip small character parts (hair/beards, hats, backpacks, sheathed weapons, morph sub-meshes) beyond a distance, and in shadow passes | 36% of draws (≈3.4 ms + queue/cull overhead); `npc range` 3000→1000 alone saved ≈3–4 ms | ~1–2 ms | Details pop at distance; small-part shadows missing | Low: reuses the proven `renderSingleObject` hook; skipping a draw is local |
| 2 | **Distant light culling:** skip deferred light volumes beyond a distance or below a screen size | 15.6% of draws (~160 lights/frame) | ~0.7–1 ms | Distant torches/lamps stop lighting surroundings (mostly at night) | Medium: needs each light's position from Kenshi's own `DeferredLightRenderable` |
| 3 | Shadow interval 3 (`KENSHI_SHADOW_INTERVAL=3`) | RTW update ≈6 ms each | ~1 ms | Shadows of moving characters update at 1/3 rate | None: config only |
| 4 | Investigate the ~36% outside scene passes (game logic/AI/animation) | ~10 ms per frame, not yet broken down | unknown | AI throttling would affect gameplay | High |

Best impact per effort: **#3 now (free), then #1 as a configurable "draw filter" plugin** (rules by mesh-name pattern, distance and pass type) built on PerfProbe's per-object hook. #2 can be a rule in the same plugin once the light position is found.

## Character Detail Cull 1.0: first A/B (2026-10-18)

`mods/Character Detail Cull/` (source `plugin-src/CharacterDetailCull/`). It hooks `SceneManager::renderSingleObject` and skips SubEntity draws whose mesh name matches a `small=` pattern in `rules.txt` (hair, beards, hats, masks, backpacks, weapons…), but only on skinned or bone-attached entities, and only beyond `main_distance` (1000) from the main camera, or always in shadow passes (`shadow_distance=0`). Insert toggles it with `KENSHI_PERF_DEBUG=1`.

test18: 60+ NPC save, camera still, Insert at 10.4 s and 21.8 s (±1 s excluded). Plugin log: ~4,500 main + ~2,300 shadow draws/s skipped, ≈7.5% of ~90k draws/s.

| Segment | Filter | Avg frame time | FPS |
|---|---|---|---|
| 0–9 s | ON | 24.4 ms | 41.0 |
| 11–21 s | OFF | 24.3 ms | 41.2 |
| 23–30 s | ON | 26.9 ms | 37.1 |

- **No measurable gain.** ON and OFF are equal within noise; the last ON segment is slower, which looks like scene drift, not the filter.
- Expected effect was small anyway: ~170 skipped draws/frame × ~2.7 µs ≈ 0.5 ms, below the ±1.5 ms noise here. Skipping at `renderSingleObject` only saves draw submission. The object has already been culled and queued, and every draw now pays the hook's classification cost.
- The earlier "~2 ms" (test18a, filter on all run, 24.0 ms) came from comparing separate sessions and isn't supported by the A/B.
- Why `npc range` 3000→1000 saved ~3–4 ms but this doesn't: removing whole characters also removes their culling, queueing, animation/skeleton updates and all their parts. Skipping a few small draws late in the pipeline removes very little of that.

## Investigation: the ~36% outside scene passes (offline, 2026-10-18)

Source: `captures/npc60.*` (all threads, 15 s, 60+ NPC save) plus static analysis of `Kenshi_x64.exe`.

**1. A big part of it isn't work, it's waiting.** The main thread (tid 26260) has 12,526 samples in 15 s at 999 Hz, so it is **on-CPU only ~84%** of the time. With scene passes at ~63–64% of wall time, the rest splits into roughly **~20% non-render work** and **~16% blocked/idle**.

**2. Non-render work on the main thread** (function-accurate, `funcs.py`): animation is small (`SceneManager::updateAllOldAnimations` 1.2%, `AnimationState::setWeight`, `TagPoint::updateNode`, `Node::updateFromParentImpl`, skeleton code ≈ 3% total), ParticleUniverse 5.3%, Kenshi's own code 12.5% (flat; top function 0.8%), MyGUI ~2%, PhysX 1.4%.

**3. Ogre already runs 16 worker threads.** Threads 26484–26498 + 26559 spend 70–75% of their samples in OgreMain (plus ParticleUniverse), each only **~5–6% of a core** busy: `LodStrategy::lodSet`, `AnimationTrack::getKeyFramesAtTime`, `MovableObject::cullFrustum`, `Skeleton::reset`, `Quaternion::nlerp`, `Entity::updateAnimation`, node-animation tracks. Combined ≈ one core of work spread over 16 threads. Other threads: tid 26561 (~19% of a core, ~84% Kenshi code + PhysX, game simulation/AI), `dxvk-cs` (~13%, D3D11 → Vulkan + NVIDIA driver).

**4. Where the worker count comes from:** `Kenshi_x64.exe 0x140818540`: `workers = max(GetSystemInfo().dwNumberOfProcessors − 1, 2)` → `Root::createSceneManager(ST_GENERIC, workers, INSTANCING_CULLING_THREADED, "…")`. On this 8-core/16-thread CPU that is **15 workers**, plus the main thread as participant.

**Hypothesis:** with only ~1 core of parallel work, waking and synchronising 15 workers at every parallel phase (culling runs per pass, ~8 passes/frame, plus animation/LOD/node updates) costs more than it saves under Wine. SMT siblings also share a physical core with the main thread. Fewer workers could shrink the main thread's ~16% wait.

**Test without any patch:** Proton's `PROTON_CPU_TOPOLOGY` makes `GetSystemInfo` report fewer CPUs *and* pins the game to them. Topology: logical CPUs `2n`/`2n+1` are siblings on core `n`.
- A (baseline): no variable → 15 workers
- B: `PROTON_CPU_TOPOLOGY=8:0,2,4,6,8,10,12,14` → 7 workers, one logical CPU per physical core (no SMT sharing)
- C: `PROTON_CPU_TOPOLOGY=4:0,2,4,6` → 3 workers

Each needs a restart, so compare 30 s MangoHud logs at the same still camera (2 runs each, since cross-session noise is ±1.5 ms). If B or C wins, a small plugin could patch the worker count at `0x140818540` instead of restricting the whole process.

**Follow-up if waiting stays high:** `sudo perf sched record -p <pid>` then `perf sched timehist -w`. This shows which thread wakes the main thread after each block (Ogre workers, `dxvk-cs`, the simulation thread), i.e. what it waits on.

### Test: fewer Ogre workers via PROTON_CPU_TOPOLOGY (2026-10-18)

60+ NPC save, same spot, camera still, Shadow Throttle 1.4 (interval 3), no PerfProbe. Separate sessions.

| Run | Setting | Ogre worker threads | Avg frame | FPS |
|---|---|---|---|---|
| A (test19a) | none (16 logical CPUs) | 15 (+1 pool) | 24.0 ms | 41.6 |
| B (test19b) | `PROTON_CPU_TOPOLOGY=8:0,2,4,6,8,10,12,14` | 7 (+1 pool), verified | 24.7 ms | 40.6 |

- **No gain.** B is within cross-session noise of A, if anything slightly slower. The main thread is still ~80% on-CPU (5 s perf check under B). Cutting the worker count and removing SMT sharing does not shrink the wait, so worker-barrier overhead is not what the main thread is waiting on. Test C (4 CPUs) was skipped. A worker-count patch plugin is not worthwhile.
- Next step to find what the main thread waits on: off-CPU/wakeup analysis (`sudo perf sched record` + `perf sched timehist -w`).

### Off-CPU analysis: what the main thread waits on (2026-10-18)

`sudo perf sched record` for 5 s while in the 60+ NPC save, then `perf sched timehist -w` (script output analysed inline). The session still had `PROTON_CPU_TOPOLOGY=8:…` (7 Ogre workers), which performed the same as the default. Main thread tid 41099.

- Main thread: **run 82.6%**, **off-CPU 17.0%** (852 ms in 5 s), of which 3.3% was runnable but not scheduled (likely the 8-CPU limit). ~14,900 blocks in 5 s ≈ **~70 per frame**; median block 16 µs, p90 74 µs, p99 0.93 ms.
- **Who ended those waits:** Ogre worker threads **~757 ms (~15% of wall time)**; wineserver 36 ms (1,267 round trips); everything else negligible. The main thread in turn wakes Kenshi threads ~29,600 times and `dxvk-cs` ~6,300 times in 5 s.
- So the waiting is the main thread **sitting at Ogre's worker barrier**, ~70 parallel phases per frame (culling per pass, LOD, animation, node updates). The workers only had ~370 ms of work each in 5 s (~7%). Phases are tiny, so sleeping and waking cost a large share of each phase.
- `Ogre::Barrier::sync` (`OgreMain 0x1803dff40`, exported) is a pure sleeping barrier: atomic arrival counter; non-last threads `WaitForSingleObject(semaphore)`; last thread flips the generation index and `ReleaseSemaphore(N−1)`. Under Wine every wait and release is a syscall plus a scheduler wake-up.
- This also explains why fewer workers (test B) didn't help: each phase still pays a full sleep/wake round trip.

**Patch idea ("barrier spin"):** replace `Barrier::sync` with spin-then-sleep. Threads that aren't last spin briefly on the generation index before falling back to the semaphore wait. Once the index flips, they still consume their semaphore count with the original wait, which then returns at once, so the semaphore stays consistent. This removes wake-up latency from short phases; long gaps still sleep. Upper bound on gain: the ~15% the main thread spends at the barrier (≈3.5 ms/frame here), realistically a fraction of that. Cost: CPU time spent spinning on up to 15 worker threads, which means more power draw. It should be configurable (spin length, on/off hotkey) and measured with a frame-time A/B and CPU power reading.

## Barrier Spin: A/B (2026-10-18)

`mods/Barrier Spin/` (source `plugin-src/BarrierSpin/`). It replaces `Ogre::Barrier::sync` with a protocol-identical version that spins up to 50 µs (TSC-timed `pause` loop) watching the generation index before the original semaphore wait. Insert toggles it (with `KENSHI_PERF_DEBUG=1`; 1.1 fixed a polling bug where 1.0 missed key presses). Log: `captures/BarrierSpin-2026-10-18.log`.

- It works mechanically: **~60% of barrier waits complete during the spin** (e.g. 18–19k of ~31–33k counted syncs per 10 s avoided sleeping; ~2–3k spins timed out).
- test21, 60+ NPC save, camera still, Insert at 8.9 s and 19.0 s (±1 s excluded):

| Segment | Spin | Avg frame | CPU power |
|---|---|---|---|
| 0–8 s | ON | 25.1 ms | 6.10 W |
| 10–18 s | OFF | 24.6 ms | 5.88 W |
| 20–30 s | ON | 27.1 ms (one 40 ms hitch at 23 s) | 6.38 W |

- test20 (spin on whole run): 24.3 ms and 25.5 ms, against 24.0 ms baseline (test19a) at the same ~5.7 W.
- **No gain, slightly more CPU power.** Avoiding the sleep/wake round trip doesn't shorten the frame. The main thread's ~15% at the barrier is waiting for the workers' real work (critical path/imbalance of the parallel phases), not for OS wake-ups. **Disabled** (removed from `data/mods.cfg`); files kept.
- Conclusion for the "outside scene passes" share: neither worker count (test B) nor barrier latency (Barrier Spin) is the lever. The parallel work itself (LOD, animation, culling per pass) bounds it, and it scales with characters × passes, which comes back to drawing/simulating fewer characters.

## Research: what the Ogre workers spend time on → animation LOD idea (2026-10-18, offline)

Source: `captures/npc60.*`, all 16 Ogre worker threads combined (14,023 samples), grouped by function. `.pdata` bounds; unexported chunks named after the preceding export.

| Group | Share of worker time | Main contents |
|---|---|---|
| **Skeletal animation** | **~33%** | `AnimationTrack::getKeyFramesAtTime`, `Quaternion::nlerp`, `Skeleton::reset/setAnimationState/_getAnimationImpl`, `Animation::apply`, `OldNodeAnimationTrack::applyToNode`, `OldNode::_update/needUpdate/rotate`, `Entity::updateAnimation`, `updateAllOldAnimations02Thread` |
| Culling/bounds | ~13.5% | `MovableObject::cullFrustum`, `updateAllBounds`, `InstanceBatch` threaded culling |
| Kernel/Wine | ~11% | thread wake/wait |
| Particles | ~9% | ParticleUniverse `_processParticles`, `Particle::_process` |
| LOD selection | ~8.4% | `LodStrategy::lodSet` |
| Other/unknown | ~20% | incl. a no-`.pdata` region (7%) |

The main thread waits ~15% of each frame for these workers (off-CPU analysis), and the wait is real work rather than wake-up latency (Barrier Spin), so **cutting animation work is the most direct way to shrink that wait.**

**How `Entity::updateAnimation` (`OgreMain 0x1800c1620`, 4 callers incl. the worker loop) decides:**
- returns if the entity has no skeleton (`+0x308`/`+0x300`);
- if `+0x284` (`mFrameAnimationLastUpdated`) ≠ the animation-state set's dirty frame (`[+0x1a8]+0x18`), or the skeleton reports manual changes (vtable `+0x248`), it does the **full update**: `0x1800c2a30` (evaluate all animation states, keyframes, blending, bone hierarchy) and stores the frame;
- otherwise it takes the **fast path**: only checks whether the node's world transform changed (`0x1800cc0b0`) and, if so, recombines the *existing* bone pose with it (`0x1800c16af…`).

**Animation-LOD plugin design:** hook `Entity::updateAnimation`. For characters beyond a distance, on N−1 of every N frames (staggered per entity by pointer hash), set `+0x284` to the current dirty frame before calling the original. Ogre then takes its own fast path: the mesh still follows its node (no lag while walking), the pose updates at 1/N rate, and Kenshi keeps advancing animation time, so playback stays in sync. Manual bone changes still force a full update via the `+0x248` check. Each entity is processed by one worker, so the write is thread-safe. Camera position for the distance test can come from a `Camera::_renderScenePhase02` hook (previous frame's position).

**Expected effect:** if distant characters skip 1/2–2/3 of evaluations, animation work (≈33% of worker time) could drop by roughly a third to a half, shortening the parallel phases the main thread waits on. Rough estimate ≈0.5–1.5 ms in the 60+ NPC save, to be measured. **Visual risk:** distant characters animate at 1/2–1/3 rate, which is mostly unnoticeable at range. Close characters are untouched.

## AnimLOD 1.0: design verified, plugin built, NOT enabled (2026-10-19, offline)

Disassembly checks that changed or confirmed the design above:
- The full update called from `Entity::updateAnimation` is `Entity::cacheBoneMatrices` (`0x1800c2a30`): `Skeleton::setAnimationState` (keyframes, blending) + skeleton vtable `+0x1f8` (`_getBoneMatrices`, bone hierarchy). It is guarded by the shared `mFrameBonesLastUpdated` (`+0x288`) vs `Root+0x190` (next frame number). The fast path skips both.
- All per-frame skeletal animation comes from **two worker call sites**: `updateAllOldAnimations01Thread` (`0x1802ca89d`) and `02Thread` (`0x1802ca8fd`). Ogre animates **every** skinned entity every frame, visible or not, near or far. Kenshi also calls `_updateAnimation`/`_updateAnimationForce` itself (main thread, e.g. `0x1407d2b38`, a bone-aiming routine). Those calls are left alone: the plugin only redirects the two worker call sites, via a 14-byte stub in int3 padding at `0x1802a1ec0`.
- **Shared skeletons:** Kenshi uses `shareSkeletonInstanceWith` (body, armour, head and so on share one skeleton, bone matrices and animation-state set). Skipping one part but not another would tear the character apart for a frame. So the frame stagger is keyed by the animation-state set pointer, not the entity: all parts of one character skip on the same frames.
- **Manually controlled bones:** Kenshi calls `OldBone::setManuallyControlled` in about 6 places (`0x1407d1ce0…0x1407d2cf7`, which also use `nlerp`, `disableBone` and `_updateAnimation`, so it looks like head or aim control). Ogre forces a full update when manual bones are dirty, so those frames can't be skipped. **How often this happens is unknown until measured**, and the plugin logs it ("skips blocked by manual bones/frame"). If it is most characters every frame, AnimLOD can't help.
- **World units:** human meshes are ~19.5 units tall, so **1 unit ≈ 10 cm** (npc range 3000 ≈ 300 m).
- **Camera:** no camera hook, so no conflict with PerfProbe. Once per frame, the first worker to see a new frame reads the `"mainCamera"` cached derived position (`Camera+0x564`) from `SceneManager+0x4c8` (`getCameras`), while the main thread waits at the barrier.

**Plugin:** `plugin-src/AnimLOD/animlod.cpp` (built: `build/AnimLOD.dll`), staged in `mods/Animation LOD/` but **not in mods.cfg**.
- **Defaults:** full rate under 400 units (40 m), 1/2 rate from 400 to 1200, 1/4 rate beyond 1200.
- **Settings (environment variables):** `KENSHI_ANIMLOD=0` starts it off; `KENSHI_ANIMLOD_MID`, `_FAR`, `_MIDN` and `_FARN` set the distances and rates.
- **Hotkey:** with `KENSHI_PERF_DEBUG=1`, Home toggles it.
- **Log (`AnimLOD.log`, every 10 s):** skinned entities per frame, skipped pose updates, skips blocked by manual bones, `updateAnimation` CPU ms per frame summed over workers (direct A/B of the work removed), and a distance histogram. That gives a useful first run even if the fps gain is small.

**Test plan:** 60+ NPC save, camera still, `KENSHI_PERF_DEBUG=1`, Shift+F2 MangoHud log, press Home at ~10 s and ~20 s, then read AnimLOD.log. Afterwards, watch distant characters walking and fighting for visual problems.

## RE_Kenshi source review (2026-10-19)

Source: https://github.com/BFrizzleFoShizzle/RE_Kenshi (GPL-3.0). Plugins can use **KenshiLib**, which offers `QueueHook`, `Escort::AllocateRWXNear` and `Escort::WriteProtected`. That is cleaner than our hand-rolled padding stubs and worth considering before publishing.

- **Ogre hooks:** RE_Kenshi itself only installs a texture-loading listener (mip skipping). No overlap with our Ogre/MyGUI patches.
- **Misc hooks:** ShowCursor de-duplication, mod-list scroll, splash skip, free camera, and the RNG fix. With `FixRNG: true` (on in this install), every CRT `rand()` takes a boost mutex and draws from mt19937. That is cheap when uncontended, so it's only worth a look if `rand` shows up in profiles.
- **PhysX collider caching** happens at load time only, not per frame.

## External research: Micromanner/kenshi-performance-research (2026-10-19)

Source: https://github.com/Micromanner/kenshi-performance-research (MIT, abandoned 2026-10-18). Windows, RX 7900 XTX + 5800X3D, the same Steam 1.0.65 build. Their method: uProf, x64dbg with Ogre symbols, PresentMon. A clone is in the session scratchpad only.

**1. MyGUI: same hotspot as ours, fixed one level higher.**
- They traced *why* `VertexQuad::set` (`0x199460`) runs every frame. Kenshi's per-frame UI loops call `Widget::setPosition` (`0x1BCF30`) and `setRealSize` → `setSize` (`0x1BD080`) on resting widgets with unchanged values. This fork has no equality guard, so each call cascades through `_updateAbsolutePoint` → `LayerNode::outOfDate` → `RenderItem::outOfDate` (`0x154980`), and the whole batched RenderItem (433–561 draw items) rebuilds every frame.
- Their plugin adds "skip if new value == mCoord" to both setters. It cut UI quad rebuilds per frame from ~296 to ~1 and gained **+17–35% FPS (2.5–3.5 ms)** in Shem and Black Desert City.
- **Known unfixed bug:** some buttons don't render, e.g. the vendor window's X close button. They suspect a path that writes `mCoord` before calling the setter.
- **Relation to ours:** our Quad Fix plus DXVK `= a` makes each rebuild cheap (the write-combined read-back is gone); theirs avoids the rebuild. They stack, but on our setup most of their 2.5–3.5 ms is already recovered. In npc60 all of MyGUIEngine is now **2.2%** of the main thread, plus Kenshi's own loop and the cascade.
- **A safer variant to try if UI-heavy cities still cost a lot:** guard only the call sites inside Kenshi's per-frame floating-widget loop (exe, a frame listener) instead of the generic setters. That leaves dialog and window layout, where the X-button bug lives, untouched.

**2. Particles: a content-dependent cost we haven't hit in our test spots.**
- In Black Desert City with rain and fog, they measured ~195 particle systems and ~26,000 billboards per frame. `ParticleUniverse::BillboardRenderer::_updateRenderQueue` → `Ogre::BillboardSet::injectBillboard/genVertices` (OgreMain `0x7c590`, hot chunk `0x7c6b0`) ran on the main thread.
- With all particle rendering disabled (a `ret` at the top of `ParticleSystem::_updateRenderQueue`), the frame went **22.2 → 14.6 ms**. That's a ceiling, not a shippable result. Their distance cull showed obvious pop-in.
- **In our captures:** ParticleUniverse is **5.3%** of the main thread in npc60 and **6.3%** in the baseline save, with billboard code ~1% (`0x7c6b0` 0.37–0.78%). So particles scale with weather, fires and smoke, not with NPC count, and rain in a city can make them the biggest cost. **Worth a test in rain.**
- Their planned next step (not started): spread `genVertices` across idle cores.
- **Our setup differs:** with DXVK `= a`, dynamic buffers are host-cached, so their Windows cost (write-combined vertex writes) may be lower here. Measure before building anything.

**3. Other notes.** Kenshi already creates the main SceneManager with threaded culling (`createSceneManager` at `0x140819146`, workers = cores − 1, matching what we saw). There are 3 extra single-threaded SceneManagers (UI, portraits). Their OgreMain function shares match ours: `_setPass` is the top function, and `0xf250f` is an inlined std::string compare.

## Next test session plan (written 2026-10-19, nothing run yet)

1. **Quick counts with `bpcount.py`** (about 10 s each, game running, camera still; the counting run itself slows the game):
   - `python3 -I scripts/bpcount.py OgreMain_x64.dll:0xc1620 MyGUIEngine_x64.dll:0x154980`: skinned-entity updates per frame and MyGUI `RenderItem::outOfDate` per frame. The first also checks that the frame counter matches MangoHud fps (unverified until then).
   - If `outOfDate` fires every frame in a city, the Micromanner-style setter guard is worth a safer version for us (guard only Kenshi's per-frame loop).
2. **AnimLOD ceiling test** (60+ NPC save): enable the mod, launch with `KENSHI_PERF_DEBUG=1 KENSHI_ANIMLOD_MID=0 KENSHI_ANIMLOD_FAR=0 KENSHI_ANIMLOD_FARN=16`, start a Shift+F2 log, press Home at ~10 s and ~20 s.
   - If frame time barely moves, AnimLOD can't pay off; stop there.
   - Otherwise relaunch with the default distances, A/B again and do the visual check.
3. **Render Queue Trim A/B:** enable `Render Queue Trim.mod`, then use End as the toggle (see its section).
4. **Particles in rain:** a city with rain or fires and a Shift+F2 log, plus a `perf record` of the main thread. Compare the ParticleUniverse plus `BillboardSet` share with the ~6% seen so far.

## Correction: which exe is running (2026-10-19)

**Confirmed:** the game runs **`RE_Kenshi/Kenshi_x64.exe`** (md5 `df4a5a7e…`), not the Steam `kenshi_x64.exe` (md5 `8a03c256…`). The two builds match at some addresses and differ at others.
- **Need re-checking:** exe addresses quoted from the Steam file, including the manual-bone sites `0x1407d1ce0…0x1407d2cf7` in the AnimLOD notes, must be checked against the RE_Kenshi exe before anything patches them.
- **Not affected:** OgreMain, MyGUIEngine and RenderSystem addresses (same DLLs) and our plugins, which check bytes at runtime.
- **Disassemble this one:** `objdump -d --no-show-raw-insn -M intel RE_Kenshi/Kenshi_x64.exe`.

## Scaling survey (background agent, 2026-10-19, offline estimates only)

Full report: [scaling-research.md](scaling-research.md). It compares the main thread of npc60 against baseline: 16.1 → 21.7 ms/frame of CPU. Most of the growth is per-draw cost. Ranked candidates, none measured:
1. **`Root::setRemoveRenderQueueStructuresOnClear(true)`.** The setter exists (OgreMain `0x180043370`, writes `Root+0x198`); Kenshi never calls it. It would stop the render queue from walking empty pass groups. Estimated 0–0.4 ms; could also be slower because the groups are reallocated every frame. A minutes-long A/B with almost no risk.
2. **Cache the D3D11 render system's shader-constant name lookups.** These are string-map searches on every draw and stage, about 1.2 ms/frame in npc60 vs 0.63 ms in baseline. Estimated 0.5–0.9 ms. Risk: a stale cache after shaders reload.
3. **"StackProbe" diagnostic** to find who calls the Wine heap and PhysX on the main thread. Both grow 5–7× with NPCs.

Other findings:
- AI and pathing run on their own thread, which the main thread doesn't wait for.
- Kenshi's own +1 ms is spread thin, with no O(n²) hot spot.
- Particles: no cheap lever for rain in the city. ParticleUniverse already skips empty systems, and `nonvisible_update_timeout` would save < 0.2 ms.

## Render Queue Trim 1.0: built, NOT enabled (2026-10-19)

**Confirmed:** `SceneManager::prepareRenderQueue` reads `Root+0x198` (`0x1802bccb6`) and calls `RenderQueue::clear(flag)` (`0x1802bccc3`). Kenshi leaves the flag false.

**Plugin** (`plugin-src/RenderQueueTrim/rqtrim.cpp`, staged in `mods/Render Queue Trim/`, not in mods.cfg):
- Patches no code. A small background thread waits for `Root` to exist, then calls Ogre's own exported setter.
- `KENSHI_RQ_TRIM=0` starts it off. With `KENSHI_PERF_DEBUG=1`, **End** toggles it.
- `RenderQueueTrim.log` records Kenshi's original value and every toggle.

**Test:** 60+ NPC save, camera still, Shift+F2 log, press End at ~10 s and ~20 s.
- Expected 0–0.4 ms either way. It can also come out slower, because the per-pass structures are rebuilt every frame.
- Then a quick visual check of the world, the UI windows and shadows. A pure queue-bookkeeping change shouldn't alter anything visible.

## Test 22–23: AnimLOD ceiling A/B (2026-10-19, 60+ NPC save, camera still)

Settings: `KENSHI_ANIMLOD_MID=0 KENSHI_ANIMLOD_FAR=0 KENSHI_ANIMLOD_FARN=16`, i.e. every character skips 15 of 16 pose updates. Home toggled it at ~10 s and ~20 s. Render Queue Trim was loaded but off.

| Run | On (0–9 s) | Off (10–19 s) | On (20–30 s) |
|---|---|---|---|
| test22, mean / median ms | 24.24 / 23.07 | 25.06 / 23.65 | 26.22 / 24.37 |
| test23 (relaunch), mean / median ms | 23.83 / 22.62 | 24.01 / 23.01 | 24.54 / 23.81 |

`AnimLOD.log` (measured):
- Animation work: **~340–500 skinned entities per frame** go through the worker loops. AnimLOD skipped ~210–295 of them per frame; manual bones blocked only ~10–24.
- Animation CPU: `updateAnimation` time summed over all workers fell from **3.7 ms/frame (off) to ~1.0–1.9 ms (on)**.
- Camera distance: every skinned entity was 80–320 m from the camera.

`bpcount.py` (measured, live):
- `Entity::updateAnimation` ran ~610 times per frame in total, so Kenshi makes extra calls of its own beyond the worker loops.
- MyGUI `RenderItem::outOfDate` ran **~222 times per frame**: the UI is re-dirtied every frame, as Micromanner found on Windows.

**Conclusion (measured):** the ceiling is within noise (≤ ~0.5 ms, with drift larger than the on/off difference). Removing ~2 ms of animation CPU spread over 15 workers doesn't shorten the main thread's wait noticeably. **AnimLOD is shelved:** disabled, source kept. The worker barrier wait seen in the off-CPU analysis is dominated by something other than animation; animation's ~33% share of *samples* didn't translate into wall time on the critical path.

Render Queue Trim was **not** tested in this run: the End key was never pressed. That kind of mix-up is the reason for the planned control panel.

## Tooling: perfctl control panel (2026-10-19)

Hotkeys are replaced by `perfctl.ini` plus `perfctl.py` (README "Control panel"). The reasons:
- The End/Home mix-up in test 23 cost a test run.
- The two-key pattern (F10 for on/off, F11 for interval) was easy to get wrong.
- Scripted A/B gives exact switch times: the plugin confirms each switch within ~0.25 s, and the script analyses the log itself.

Plugin versions with perfctl: Quad Fix 1.3, Shadow Throttle 1.5, Render Queue Trim 1.1. **Animation LOD** is disabled, removed from mods.cfg and shelved, so it keeps its Home hotkey.

**Checked offline:**
- The parser handles comments, CRLF, spaces and negative values (native test harness).
- The panel toggles `rqtrim` and writes the file (pty test).
- `analyze` reproduces the test 22 numbers.

**Not yet verified in game:** that the plugins pick up file changes under Wine.

## Test 24: Render Queue Trim A/B, scripted (2026-10-19, 60+ NPC save, camera still)

First scripted run with `perfctl.py ab rqtrim 0 1`. The plugin confirmed both switches, in `RenderQueueTrim.log` at +10.0 s and +20.0 s.

| Segment | rqtrim | Mean | Median | p95 |
|---|---|---|---|---|
| 0–10 s | 0 (Kenshi default) | 25.70 ms | 24.70 ms | 32.34 ms |
| 10–20 s | 1 | 26.26 ms | 25.13 ms | 32.18 ms |
| 20–30 s | 0 | 26.43 ms | 25.30 ms | 32.60 ms |

**Conclusion (measured):** no gain. The "on" segment sits between the two "off" segments, so the difference is drift, not the setting (|effect| ≤ ~0.3 ms). Render Queue Trim stays off (`rqtrim=0`) and comes out of mods.cfg at the next game exit.

The scripted A/B itself worked end to end. The one bug, moving the log across filesystems, is fixed (`shutil.move`).

## Test 25: Shadow Throttle at interval 3 vs off, scripted (2026-10-19, 60+ NPC save, shadow mode 2, camera still)

Ran `perfctl.py ab shadow_interval 3 1`. The plugin confirmed both switches in `ShadowThrottle.log`, at +10.1 s and +20.1 s.

| Segment | Interval | Mean | Median | p95 | FPS |
|---|---|---|---|---|---|
| 0–10 s | 3 | 26.01 ms | 24.83 ms | 32.18 ms | 38.5 |
| 10–20 s | 1 (off) | 29.82 ms | 29.39 ms | 33.89 ms | 33.5 |
| 20–30 s | 3 | 25.53 ms | 24.31 ms | 32.10 ms | 39.2 |

**Measured:** Shadow Throttle at interval 3 saves **~4.0 ms per frame** in the 60+ NPC save (29.8 → 25.8 ms, **+5.4 FPS, ~+16%**). Both "on" segments agree, so drift doesn't explain it. This is the first direct A/B of interval 3; earlier gains of 2.5–2.8 ms were measured at interval 2 in a lighter scene.

## Test 26: Shadow Throttle at interval 3 vs off, Drycoom at ~21:30 in-game (2026-10-19, shadow mode 2, camera still)

Second location, same scripted A/B (`ab shadow_interval 3 1`). The plugin confirmed both switches, at +10.1 s and +20.1 s.

| Segment | Interval | Mean | Median | p95 | FPS |
|---|---|---|---|---|---|
| 0–10 s | 3 | 18.71 ms | 18.54 ms | 22.45 ms | 53.5 |
| 10–20 s | 1 (off) | 21.31 ms | 20.46 ms | 24.78 ms | 46.9 |
| 20–30 s | 3 | 18.53 ms | 18.02 ms | 22.03 ms | 54.0 |

**Measured:** −2.7 ms per frame (21.3 → 18.6 ms, **+7 FPS, ~+14%**). The saving scales with scene content: about 4.0 ms with 60+ NPCs (test 25) and 2.7 ms in this lighter scene. Shadow Throttle is the largest single gain we have.

## MyGUI Layout Guard 1.0: the safer version of Micromanner's fix, built and NOT enabled (2026-10-19)

**Confirmed from the binaries** (RE_Kenshi exe + MyGUIEngine):
- Kenshi calls MyGUI's non-virtual setter wrappers through its import table. There are ~55 call sites for `setPosition(int,int)` (IAT `0x142245a68`), ~27 for `setSize(int,int)` (`…a38`), ~19 for `setRealSize(float,float)` (`…9e0`), ~9 for `setRealPosition` (`…af0`) and 1 for `setCoord(int×4)` (`…a28`). MyGUI's own layout code never goes through these wrappers.
- The wrappers take the outer `Widget*` and forward to the virtual on `this+8`. Widget `mCoord` sits at outer `+0x20/+0x24/+0x28/+0x2c` (left, top, width, height). The cropped-parent pointer is at `+0x40`; its width and height are at `+0x20/+0x24`.
- `setRealPosition` and `setRealSize` compute `(int)((float)parentSize × f)` with truncation, or use the render-target size when there is no parent.

**Why it's safer than Micromanner's version:**
1. Only Kenshi's own calls are guarded, through the import-table slots. Window, layout and skin code inside MyGUI is never touched.
2. A call is skipped only when the same widget got the **identical** call from the **same call site** in the previous frame or earlier in this frame, **and** its coordinates already match the result.

One-off calls, such as opening a vendor window or creating a widget, always pass through. That's the case Micromanner's X-button bug points at, where something writes `mCoord` and then calls the setter once.

**Plugin:** `plugin-src/MyGUILayoutGuard/layoutguard.cpp`, staged in `mods/MyGUI Layout Guard/`, not in mods.cfg.
- **Control:** perfctl key `uiguard`. 0 = count only (default, everything passes through); 1 = guard on.
- **Log:** `MyGUILayoutGuard.log`, every 10 s. Per setter it shows calls, no-ops and skips per frame, plus the 12 busiest call sites as exe RVAs.

**Test plan:**
1. Enable the mod and run once in count-only mode (60+ NPC save, then a city) to see how many calls are repeated no-ops, and from where.
2. Run `perfctl.py ab uiguard 0 1`.
3. With the guard on, do the visual checklist: inventory, vendor or trade window (X button), dialogue, map, character screen, name tags and health bars, and floating damage numbers.

Our Quad Fix already made each rebuild cheap, so the expected gain here is modest (MyGUI is 2.2% of the main thread in npc60). It may be larger in UI-heavy cities.

## MeitouClient review (background agent, 2026-10-19)

Full report: [meitou-research.md](../Meitou/docs/research.md).
- **What it is:** a from-scratch C#/Vulkan rewrite of Kenshi (GPL-3.0). It doesn't hook the game and doesn't change its performance. No overlap or conflict with our plugins.
- **Useful leads (the author's decompile of 1.0.68, so addresses differ from ours; the class names exist in our exe's RTTI):**
  - **Per-frame MyGUI writers:** `MainBarGUI` (updated every frame), `DatapanelGUI` text panels, portrait flash, `FloorText`, the map marker, and the medical panel's 9×7 life bars. These are candidates for the ~222 UI dirties per frame; Layout Guard's call-site census will show which ones are no-ops.
  - **NPC scaling:** every frame the main-thread world update gives a round-robin tick to 8 characters and a full update to 6 others. Every character with a "priority" flag (`+0xe4`) gets a full update every frame, so that's the per-NPC cost to look at.
  - **Shadows:** stagger the far CSM cascades and update the near one every frame. Watch p95/p99, because staggered passes can cause spikes.
  - **Particles:** ParticleUniverse quotas (default 500 per technique) and `sorting true` in 13 of 93 scripts. A data-only mod is a cheap rain/city test.

**Shadow Throttle spike check (tests 25–26, re-analysed):**
- p95 is not worse with throttling on: 32.2/32.1 ms on vs 33.9 ms off in the 60+ NPC save, and 22.5/22.0 vs 24.8 ms at Drycoom.
- **Caveat:** MangoHud logs averages over its sampling interval, not single frames, so short single-frame spikes are smoothed out. A per-frame spike check needs per-frame logging.
- `perfctl.py analyze` now also prints p99 and max.

## Test 27: MyGUI Layout Guard, census and A/B (2026-10-19, 60+ NPC save, UI on, camera still)

**Census (count-only, measured):**
- Kenshi sends **55 layout calls per frame** through the five setter imports, and **all are no-ops**.
- 54 of them are `setRealSize` from 6 call sites (`exe+0x720ff4…0x721047`), 9 per frame each. That matches Meitou's "medical panel: 9 life bars × layers redrawn every frame".
- 1 is `setPosition` from `exe+0x728a21`, near the main HUD bar's per-frame update.

**A/B** (`ab uiguard 0 1`, both switches confirmed):

| Segment | uiguard | Mean | Median | p95 |
|---|---|---|---|---|
| 0–10 s | 0 | 25.63 ms | 24.20 ms | 31.68 ms |
| 10–20 s | 1 | 26.35 ms | 25.64 ms | 32.41 ms |
| 20–30 s | 0 | 26.80 ms | 24.48 ms | 33.27 ms |

- **Calls skipped:** with the guard on, it skips ~51 of the 55 calls per frame.
- **UI dirties (bpcount):** `RenderItem::outOfDate` only drops from ~222 to ~211 per frame. **About 95% of the UI dirtying in this scene comes from other paths:** text, colour and visibility changes, or MyGUI-internal calls, not the import-table setters.
- **Frame time:** no measurable change (+0.1 ms, within drift).

**Conclusion (measured):** in this scene Micromanner's setter problem is small. Our Quad Fix already makes each rebuild cheap, and the remaining dirties come from elsewhere. Their large gains were in cities (Shem, Black Desert City) full of floating name tags, so **one city run is still worth doing** before shelving this. If a city also shows no gain, remove it. It stays installed with `uiguard=0` (count only; it adds a small per-call cost).

## Test 28: MyGUI Layout Guard in a city (2026-10-19, new game starting in a city, camera still)

- **Census:** the same 55 no-op calls per frame (54 medical-panel `setRealSize` + 1 HUD `setPosition`). UI dirties (`RenderItem::outOfDate`) are only ~70 per frame.
- **A/B:** 24.09 / 24.33 / 24.62 ms (off / on / off). The difference is **−0.02 ms**.

**Conclusion (measured):** no gain in either scene. On our setup the per-frame setter dirtying that Micromanner measured on Windows is small. The likely reasons: our Quad Fix and DXVK `= a` already make each rebuild cheap, the floating name-tag loop they saw doesn't show up here, and most remaining dirties come from paths other than the setters. **Layout Guard is shelved:** it comes out of mods.cfg at the next exit; source kept.

**Context:** the city run had **1 squad member**; the 60+ NPC save has a large squad. UI dirties (70 vs 222 per frame) scale with the player's squad UI (portraits, bars), not with town NPCs. The guardable setter calls were the same 55 per frame in both, so the verdict holds for big squads too.

**Side finding:** the medical panel's 9 life bars are re-sized every frame even while they don't change (54 calls per frame). That's harmless with the current fixes.

## Per-character update: located in our exe, probe built (2026-10-19, offline)

**Confirmed from the RE_Kenshi exe**, matching Meitou's description of 1.0.68 `FUN_140786e30`:
- **Character budget function:** `0x1407862f0`, via thunk `0x14004b628`, called once per frame from the world update at `0x140788467` on the main thread. It walks the active character list.
  - **Logic tick:** 8 characters per frame (round robin) get Character vtable slot 29 (`0x1405cc300`, thunk `0x14000b91a`).
  - **Priority characters** (byte `+0xe4` set) get the **full update**, slot 28 (`0x1405ce6a0`, thunk `0x1400073d8`), **every frame**.
  - **Other characters** get slot 28 only 6 per frame (round robin). In between, the frame's time is added to their accumulator `+0xc0`.
- **Slot 28 catches up on its own:** it starts by adding `+0xc0` to its time step and then zeroes it. So skipping a full update and adding the frame's time to `+0xc0` is exactly what Kenshi itself does for non-priority characters. That makes an "update LOD" for distant priority characters a natural patch, if the measurements justify it.
- **The `+0xe4` flag** is computed in `0x1405c9xxx` from faction or player relations (vtable `+0x330`, `+0x1e0` checks), a range test, and squad and zone state. It isn't decoded in full.

**CharProbe 1.0 (diagnostic, staged in `mods/CharProbe/`, not in mods.cfg):**
- Times the budget function and every slot 28 / slot 29 call through their thunks, using stubs in int3 padding at exe `0x6c139`. All bytes are checked first. Game behaviour is unchanged.
- `CharProbe.log`, every 10 s: budget time per frame; full updates per frame and their cost, split into priority, round robin and other callers; logic ticks per frame and their cost; the most priority characters in one frame.

**Next:** one run each in the 60+ NPC save and in a town. If priority full updates cost more than ~1–2 ms per frame, design the LOD: skip some full updates for distant priority characters outside the player's squad, adding the time to `+0xc0`.

## Test 29: CharProbe in the 60+ NPC save (2026-10-19, camera still, unpaused)

| Per frame (main thread) | Calls | Time |
|---|---|---|
| Whole character budget function | 1 | **1.28–1.32 ms** |
| Full update, priority characters (every frame) | ~116–121 (max 123) | 0.84–0.88 ms (≈7 µs each) |
| Full update, round robin (non-priority) | ~5.9 | 0.05 ms |
| Full update, other callers | 0–2.5 | ≤ 0.02 ms |
| Logic tick (slot 29) | 9–10 | 0.25 ms |

Slot 28/29 calls from other threads: ~500 in the first window, none afterwards (likely while loading).

**Conclusion (measured):** the per-character update costs **~1.3 ms of a ~25 ms frame (≈5%)**, even with ~120 characters on priority.
- **Best case for an update LOD:** halving full updates for distant non-squad characters would save **≤ ~0.4 ms**, at some gameplay risk to AI responsiveness and fights.
- **Verdict:** not worth a patch. The main-thread time is in rendering (Ogre, D3D11, DXVK), not in per-character game logic. This agrees with Micromanner's "game sim ~8% of the main thread".
- **What's left:** CharProbe stays in place as a diagnostic until the next exit.

## Shader Const Cache 1.0: built, NOT enabled (2026-10-19)

**Confirmed from the binaries:**
- RenderSystem_D3D11 `0x180013eb0` (called 5× from `bindGpuProgramParameters`) loops over a program's reflected variables, which are 0x38-byte records with the name string first. For each one it calls `GpuProgramParameters::getConstantDefinition(name)` through its import slot `+0x717f0`.
- Ogre resolves that through `_findNamedConstantDefinition`: a `std::map<String, GpuConstantDefinition>` in `GpuNamedConstants` (`params+0x128`), with nodes holding the key at `+0x18` and the definition at `+0x40`. Both functions set up big stack frames with security cookies.
- `GpuNamedConstants::~GpuNamedConstants` (`0x1800db0a0`, exported) is reached by tail jumps from 3 places. Its first 15 bytes are a position-independent prologue (`40 53 48 83 ec 30 48 c7 44 24 20 fe ff ff ff`), and nothing jumps into them.

**Plugin** (`plugin-src/ShaderConstCache/constcache.cpp`, staged in `mods/Shader Const Cache/`, not in mods.cfg):
- **Cache:** RenderSystem's import slot points to a direct-mapped cache (16k entries) keyed by (named-constants pointer, name-string address).
- **Hit check:** on every hit, the cached map node's key (`def − 0x28`) must equal the requested name.
- **Reset:** the whole cache is dropped when any `GpuNamedConstants` is destroyed (destructor trampoline), or when the cache is toggled.
- **Miss:** the plugin calls `_findNamedConstantDefinition(name, false)`. If the name is missing, it tail-calls the original, which throws exactly as before.
- **Scope:** Kenshi's own `getConstantDefinition` calls use the exe's own import slot and are untouched.
- **Control:** perfctl key `shadercache` (1 = on, the default; 0 = off). `ShaderConstCache.log` shows lookups, hits, misses and cache resets per frame.

**Expected:** ~0.5–0.9 ms in the 60+ NPC save, per the agent's offline estimate (unmeasured).

**Test plan:**
1. Enable it and check the log: hits should be ~all lookups, with misses only at first.
2. Run `perfctl.py ab shadercache 1 0` in the 60+ NPC save.
3. Visual check: character skin and lighting, shadows, terrain, water, UI. A wrong constant would show as broken shading.

## Tests 30–31: Shader Const Cache A/B, both orders (2026-10-19, 60+ NPC save, camera still)

**Log (measured):**
- **Lookups:** ~19,000–21,000 per frame. With the cache on, 99.9% are hits (~8–19 misses per frame, from slot collisions).
- **Cache resets:** none (no shaders unloaded).

| Run | Order | Segment means (ms) | Cache on vs off |
|---|---|---|---|
| test30 | on / off / on | 24.58 / 25.93 / 25.35 | **−0.97 ms** |
| test31 | off / on / off | 26.37 / 24.75 / 25.99 | **−1.42 ms** |

Medians agree (23.2 / 24.8 / 24.0 and 25.2 / 23.1 / 24.8 ms).

**Conclusion (measured):** **~1.0–1.4 ms per frame saved (≈ +1.5–2.3 FPS at ~39 FPS, ~4–5%)**. The "on" segments win in both orders, so drift doesn't explain it. This is the first gain on the rendering side since Shadow Throttle. It is left **on** (`shadercache=1`) pending a visual check.
**Visual check (2026-10-19, by the user):** characters, lighting, shadows, terrain, water and UI all look normal with the cache on. **Shader Const Cache stays enabled.**

## StackProbe 1.0: real main-thread call stacks, built and NOT enabled (2026-10-19)

**Why:** perf call chains are useless under Wine because MSVC code has no frame pointers. So "who calls the heap allocator, memcmp or PhysX on the main thread" (the scaling survey's open items) can't be answered from perf.

**How it works** (`plugin-src/StackProbe/stackprobe.cpp`, staged in `mods/StackProbe/`, not in mods.cfg):
- **Trigger:** `perfctl.py set stackprobe=1`. A sampler thread then runs for 10 s at ~1 kHz: it suspends the main thread, takes its registers, and unwinds the stack using each module's `.pdata` through `RtlVirtualUnwind`.
- **No deadlock risk:** while the thread is suspended there are no allocations, logging or loader calls. Function lookup uses our own module table, built before the run, and stack reads stay within the TEB stack bounds.
- **Output:** distinct chains with counts go to `StackProbe.log` as module+RVA, leaf first. The status then goes back to 0.
- **Assumption to verify on the first run:** `startPlugin` runs on the main thread (the report's leaf modules will show it).
- **Symbolize** with `scripts/stacksym.py StackProbe.log [--focus msvcr100 --focus ntdll ...]`. It prints self time and inclusive time per function, plus the hottest callers for the focus modules (default: msvcr100, ntdll, PhysX, NxCharacter). Names come from exports and `.pdata` starts, demangled.

**Overhead:** only during the 10 s run (each sample is a suspend + unwind, roughly tens of µs). Idle otherwise.

## StackProbe 1.0 crashed the game; 1.1 fixes it (2026-10-19)

**What happened (measured, from `crashDump1.0.65_x64.dmp`, kept as `captures/stackprobe-1.0-crash.dmp`):**
- About 17 s into the first sampling run, an access violation (read of address 0) hit **Wine's `ntdll!RtlVirtualUnwind2`**, called from our sampler thread (0x384). The main thread (0x178, correctly identified) was still suspended.
- RE_Kenshi's crash handler wrote `save/emergency_save_0`. The user's own save was not touched.
- **Cause:** Wine's unwinder dereferences stack and frame data without checking. Some main-thread frame (probably in Wine's syscall path) fed it a bad frame.

**Fix (StackProbe 1.1):** no longer calls `RtlVirtualUnwind`. It uses its own x64 unwind-code interpreter (PUSH_NONVOL, ALLOC_SMALL/LARGE, SET_FPREG, SAVE_NONVOL(_FAR), PUSH_MACHFRAME, chained entries, partial prologues). Every stack read is checked against the main thread's TEB stack bounds, and any unexpected frame just ends that chain.

**Verified offline under Proton's Wine:** a test program (`plugin-src/StackProbe/test/test.cpp`, sampling a known 5-level chain that includes an 8 KB frame) gave 8,459 samples in 10 s. Every chain was complete and correct, through kernel32 and ntdll, with 0 cut. StackProbe is **out of mods.cfg** until the user decides to try 1.1 in game.

## Test 32: StackProbe 1.1, first real main-thread profile (2026-10-19, 60+ NPC save, camera still)

8,444 samples in 10 s with no crash; 41 stacks cut at code without unwind data. Raw log: `captures/StackProbe-npc60-2026-10-19.log`; symbolize with `scripts/stacksym.py`. Kenshi names refer to `RE_Kenshi/Kenshi_x64.exe`.

**Measured: where the main thread's time goes (inclusive, % of samples, ~25 ms frame):**

| Area | Share | Notes |
|---|---|---|
| Rendering (`Root::_updateAllRenderTargets` / compositor) | **64.6%** | scene passes 52.6%, of which `renderSingleObject` 29.8%, culling 9.8% |
| — `updateGpuProgramParameters` | 12.4% | RenderSystem bind-params `0x36f50` 9.0% (constant copy and lookups, already cached), `Pass::_updateAutoParams` 3.0% |
| — RenderSystem draw/state `0x34170` | 13.1% | d3d11 (DXVK) underneath |
| — Kenshi shadows (`0x2d9500 → 0x2d8500`) | 11.5% | the RTW pass `0x865010` is 7.9% *on average* even at interval 3 (one pass every 3rd frame) |
| Kenshi world update (`frameEnded` → `0x787e70`) | **25.2%** | see below |
| Waiting in `NtWaitForSingleObject` | 16.7% (leaf) | spread over the phases below |

**Kenshi world update, 25.2%, by child:**
- **Character budget `0x7863a9`: 5.2%.** That's ≈1.3 ms, **the same as CharProbe measured**, which cross-validates the sampler.
- **`0x40a4e0`: 4.7%.** It hands a task to Ogre's workers (`SceneManager::executeUserScalableTask`) and **waits 4.5%** (~1.1 ms). ParticleUniverse `_prepareRenderer` shows up under it, so it's likely the parallel particle update.
- Unidentified children: `0xa11da0` 3.8%, `0x7d17e0` 2.6% (physics: the PhysX callers sit under it), `0xa11b70` 2.2%, `0x6e98d0` 1.7%, `0x800a80` 1.3%, `0x44bba0` 1.1%, `0x2e74d3` 1.0%.

**Where the main thread waits (leaf in ntdll, 21.5%):**
- 5.5% in `updateAllOldAnimations`, waiting for the animation workers.
- 4.5% in Kenshi's `executeUserScalableTask`.
- ~3.1% in Ogre's frame-start updates (`0x2c2956` / `0x2c0ed9`).
- 1.2% in cull and 0.9% in render barriers.
- 0.8% in `updateAllLods`.

**Smaller items:**
- **PhysX on the main thread:** only 1.6%.
- **msvcr100:** 4.1% (memcpy for constant buffers, the heap allocator from `updateAllOldAnimations`, memcmp).

**Next leads:**
1. Identify Kenshi's scalable task at `0x40a4e0`: is it the particle update, and can its 1.1 ms wait shrink?
2. The 5.5% animation wait didn't respond to AnimLOD. That suggests it's dominated by something else in the same parallel phase (bounds or node updates), or by a few big skeletons. A StackProbe-style sampler on the worker threads would show what they do in that phase.
3. Kenshi world-update children `0xa11da0` / `0xa11b70` (6% together).

## Test 33: StackProbe in a town (new game, 1 character) + the parallel task identified (2026-10-19)

**Town vs 60+ NPC save (measured, inclusive % of main-thread samples):**

| | 60+ NPC | Town |
|---|---|---|
| Rendering (`_updateAllRenderTargets`) | 64.6% | 66.7% |
| `renderSingleObject` | 29.8% | 29.3% |
| Culling (`_cullPhase01`) | 9.8% | 12.0% |
| Kenshi shadows (`0x2d9500`) | 11.5% | 10.2% |
| Kenshi world update (`0x787e70`) | 25.2% | 21.3% |
| — character budget | 5.2% | 5.0% |
| — **particle update `0x40a4e0`** | **4.7%** | **6.0%** |
| `updateAllOldAnimations` (incl. its wait) | 7.9% | 9.8% |
| Waiting (`NtWaitForSingleObject`) | 16.7% | 20.4% |
| ParticleUniverse (on the main thread) | 3.8% | 4.6% |

**Confirmed from the RE_Kenshi exe:** `0x40a4e0` is Kenshi's **EffectsManager update**. Every frame it:
1. calls `ParticleTechnique::_prepareRenderer` for each technique of each particle system in its list (`this+0xc8`), on the main thread;
2. hands a cached `EffectsManagerUpdateJob` (RTTI; vtable `0x1416d0368`, sized by `getNumWorkerThreads`) to `SceneManager::executeUserScalableTask(job, block = true)`;
3. **waits** until the workers have updated all particle systems.

That wait is **~1.1 ms in the 60+ NPC save and ~1.5 ms in the town**. It's the parallel particle simulation, separate from the particle *rendering* (`BillboardRenderer`, 3.4–4.5%).

(Correction: in the RE_Kenshi exe, IAT `0x142248a18` is `ParticleTechnique::_prepareRenderer`. The earlier "setManuallyControlled" sites came from the Steam exe, whose IAT layout differs.)

**Options (none built):**
- **(a) Don't block:** pass `block = false` and wait later (`waitForPendingUserScalableTask` before rendering). The particle update would then overlap the rest of the world update (~15% of the main thread). **Risk: data races** if the world update creates, moves or destroys effects while the workers run. It needs proof that nothing touches the EffectsManager list in between. Potentially the biggest win (up to ~1–1.5 ms).
- **(b) Do less particle work:** skip or slow updates of off-screen or distant particle systems. ParticleUniverse supports a non-visible update timeout, but only 1 of 93 scripts uses it. Visual risk: effects "freeze" off-screen.
- **(c) Measure first:** sample the worker threads during the job (StackProbe on a worker) to see what dominates the particle update.

## Option (a), a non-blocking particle job: investigated, NOT safe, not built (2026-10-19)

**What the job does (confirmed from the RE_Kenshi exe):**
- `EffectsManagerUpdateJob::execute(thread, numThreads)` (`0x14040ecd0`) walks the EffectsManager's effect list (`job+0x10`) in stride `numThreads`. For each effect it calls `0x1400f9570`:
  - **`ParticleUniverse::ParticleSystem::_update(dt)`**, the full particle simulation (emitters read their scene node's transform; affectors and particle pools are updated);
  - the effect's light fade (`0x1400f9200`) and timers.
- Finished effects (returning −1) go into per-thread "dead" lists (`job+0x28`). The rest of `0x40a4e0`, on the main thread right after the job, processes and destroys them, so it depends on the job having finished.

**What runs after the job in the world update (`0x787e70`):** the job is started at `0x1407882ad`. About 20 calls follow, including **the character budget at `0x140788467`**, i.e. every character's full update. Characters spawn, move and stop effects (blood, dust, footsteps, attached particle systems; Kenshi's `Effect`/`EffectHandler`/`EffectGroup` classes) and move the scene nodes the emitters read.

**Verdict:** letting the job run past `0x40a4e0` would race in at least four ways:
1. Effect creation `push_back`s into the very list the workers iterate, and a reallocation means a crash.
2. Effect destruction would free systems mid-update.
3. ParticleUniverse's managers and pools aren't thread-safe.
4. The dead-list post-processing needs the job finished.

Making it safe would mean guarding every effect create/destroy path in Kenshi. That's far beyond a small patch, and the upside is ≤ ~1–1.5 ms. **Not building it.**

**Why the wait is 1.1–1.5 ms with 15 workers:** the work is split per particle system, so one heavy system (weather, smoke, fire) bounds the whole job while the other workers idle. The way forward is option (c), sampling the workers during the job to see which systems or affectors dominate, then option (b), reducing that work (e.g. update rate or quota for off-screen or distant systems).
