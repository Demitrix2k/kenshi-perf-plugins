# What grows with NPC count (offline research, 2026-10-19)

Question: which main-thread work grows with the number of characters, and which of it a small RE_Kenshi plugin could cut. This is read-only analysis of the existing captures plus static disassembly. Nothing was run in-game, and no game files were changed. Skeletal animation LOD is out of scope (designed separately in `plugin-src/AnimLOD/`). MyGUI and particle distance culling are also out of scope (covered elsewhere).

## Method

- Captures: `captures/baseline.*` (settings-A/B save "with more characters", main thread tid 12457, 15 s, ~19.1 ms/frame → ~785 frames, **no** quad fix or Shadow Throttle) vs `captures/npc60.*` (60+ NPC save, main thread tid 26260 = the busiest thread and the game PID, 15 s, ~26 ms/frame → ~577 frames, quad fix + Shadow Throttle on).
- Per-function cost = leaf samples × 1.001 ms / frames, attributed with `.pdata` bounds. Chained unwind entries (MSVC split function bodies) are merged into their primary function. Scripts are in the scratchpad (`scal/cmp2.py`, `scal/prim.py`, `scal/ins.py`).
- Caveats:
  - "baseline" is not a 5-NPC scene. The two saves differ in more than NPC count (spot, buildings, lights), so ratios are only indicative.
  - npc60 has Shadow Throttle on, so its shadow-pass work is *under*-represented. Anything that still grows a lot is real growth.
  - Frame counts come from MangoHud averages (±5–10%).
  - perf call chains are useless here: MSVC code has no frame pointers (checked: 197 of 311 memcmp samples have no parent, and the rest are garbage). So "who calls X" comes from static analysis only.

### Important: the exe in memory is `RE_Kenshi/Kenshi_x64.exe`, not `kenshi_x64.exe`
The captures map `RE_Kenshi/Kenshi_x64.exe` (md5 `df4a5a7e…`, 36,713,984 B). That is a different build from the Steam `kenshi_x64.exe` (md5 `8a03c256…`); 24 M bytes differ. The scratchpad `kenshi.asm` was made from the **Steam** exe. It matches the running code at some addresses (e.g. `0x1402d8634`) but **not** at others (e.g. `0x140863cb0`, where Shadow Throttle's byte check only matches the RE_Kenshi file).

**Use `scratchpad/rek.asm`** (Intel syntax, made from `RE_Kenshi/Kenshi_x64.exe`) and `scratchpad/rek_calls.txt` (`calls.py` output for the whole exe). All Kenshi addresses below refer to the RE_Kenshi exe.

## Where the extra ~5.6 ms per frame goes

Main thread on-CPU time per frame: **16.1 ms (baseline) → 21.7 ms (npc60)**.

| Module | baseline µs/frame | npc60 µs/frame | Δ |
|---|---|---|---|
| OgreMain | 6692 | 9359 | **+2667** |
| Kenshi_x64.exe | 1709 | 2715 | **+1006** |
| RenderSystem_Direct3D11 | 1778 | 2559 | +781 |
| d3d11.dll (DXVK) | 2058 | 2621 | +563 |
| msvcr100 (memcmp/memmove) | 940 | 1478 | +538 |
| PhysXCore64 | 54 | 295 | +241 (5.5×) |
| ntdll (Wine heap) | 380 | 524 | +144 |
| ParticleUniverse | 1010 | 1154 | +144 |

Top function-level growth (µs/frame, baseline → npc60):

| Function | baseline | npc60 | ratio | Group |
|---|---|---|---|---|
| `SceneManager::_setPass` (Ogre `0x1802bc290`) | 496 | 815 | 1.6 | per draw (structural) |
| **`memcmp`** (msvcr100) | 255 | 540 | **2.1** | **string-keyed lookups, see #1** |
| `D3D11VertexDeclaration::getILayoutByShader` (RS `0x50a10`) | 446 | 685 | 1.5 | per draw (already assessed) |
| memmove/memcpy (msvcr100) | 578 | 807 | 1.4 | mixed |
| **GpuConstantDefinitionMap lower_bound** (Ogre `0x1800f24f0`, unexported) | 261 | 472 | **1.8** | **#1** |
| **`QueuedRenderableCollection::acceptVisitorGrouped`** (`0x18024aa80`) | 119 | 279 | **2.4** | **#2** |
| `VertexBufferBinding::getHasInstanceData` (`0x180109c80`, leaf, no pdata) | 230 | 368 | 1.6 | per draw, see #5 |
| **`QueuedRenderableCollection::clear`** (`0x18024a460`) | 80 | 210 | **2.6** | **#2** |
| `Material::getBestTechnique` | 319 | 442 | 1.4 | per renderable, cache misses only |
| `SubEntity::getNumWorldTransforms` / `getRenderOperation` / `getWorldTransforms` | 204 | 492 | 2.2–2.6 | per character draw (cache misses) |
| `RenderPriorityGroup::addRenderable` (`0x180249cc0` area) | 168 | 278 | 1.6 | #2 |
| `Entity::_updateRenderQueue` | 232 | 326 | 1.4 | per renderable |
| `RenderSystem` getConstantBuffer (`0x180013eb0`) + string compare (`0x1800086b0`) + `_findNamedConstantDefinition` + `getConstantDefinition` | 235 | 381 | 1.6 | **#1** |
| PhysX `0x1802b3da0` (float ray/shape test code) | 12 | 87 | 7.6 | #4 |
| Wine `heap_allocate_block` (ntdll `0x170051f40`) | 17 | 109 | 6.6 | #4 |
| `removePassGroup` (`0x18024a5a0`) | 5 | 33 | 6.5 | #2 (passes re-hashed every frame) |

Most of the growth is per-draw rendering cost, which the earlier notes already called structural (characters × sub-meshes × passes). Inside it, two items **grow faster than the draw count (≥1.8×)** and turned out to be plain redundant work. They are #1 and #2 below. Kenshi's own code grew +1 ms but is flat: no single function grew more than 0.08 ms (details in #6).

---

## Candidates, ranked by expected gain × safety

### #1 Cache shader-constant name lookups in the D3D11 render system (best gain)

**What it does now.** Every time the render system binds a shader's constants, it does a fresh `std::map<String>` lookup **by name** for every constant in the shader. This happens for every draw and every shader stage (Ogre 1.9-era D3D11 code). `RenderSystem_Direct3D11 0x180013eb0` (single constant buffer) and `0x180014040` (multi-buffer variant) loop over the shader's constant-buffer variables (array at `[cb+0x58..0x60]`, stride 0x38). For each variable they do:
1. `GpuProgramParameters::getConstantDefinition(varName)` (import, IAT slot RVA **0x717f0**).
2. This goes to `_findNamedConstantDefinition` (`0x1800eb230`, 0x240-byte frame with security cookie), then to a string-keyed `std::map` lower_bound (`0x1800f24f0`), which calls memcmp (`0x18055de1c` → msvcr100) and a string compare (`0x1800086b0`).
3. Then `isFloat`, then a memcpy of the value into the locked buffer.

`0x180013eb0` is called 5× from `bindGpuProgramParameters` (`0x180036f50`: call sites `0x36fed, 0x370fd, 0x3720d, 0x37320, 0x37433`). The character shaders have ~10–25 uniforms per stage (`data/materials/deferred/skin.hlsl`, `character.hlsl`), so ~2,300 draws/frame means tens of thousands of string-map lookups per frame. The result never changes: the variable → definition mapping is fixed per (program, `GpuNamedConstants`).

**Cost.**
- In npc60: lower_bound 472 + memcmp 540 + string compare 97 + `_findNamed` 62 + `getConstantDefinition` 45 µs ≈ **1.2 ms/frame (~5.5% of the main thread)**.
- In baseline: ≈0.63 ms.
- It roughly doubles with the character-heavy scene.
- Uncertainty: memcmp has ~630 call sites in OgreMain alone, and Kenshi also calls `setNamedConstant` by name from ~80 functions. So not all of it is this path. lower_bound `0x1800f24f0` is specific to the constant-definition map, though, and its 1:1 ratio with memcmp samples fits memcmp being called inside it.

**Patch** (no code bytes changed; this is a pointer swap):
1. Replace RenderSystem's IAT entry `RenderSystem_Direct3D11_x64.dll + 0x717f0` (`?getConstantDefinition@GpuProgramParameters@Ogre@@QEBAAEBUGpuConstantDefinition@2@AEBV?$basic_string…@@Z`) with a hook. Only RenderSystem's 3 call sites and its one thunk (`0x180057a14`) use this slot. Kenshi's own calls are unaffected.
2. Hook: `nc = *(void**)(params + 0x128)` (the raw `mNamedConstants` pointer; null → call the original, which throws as before). Look up a direct-mapped table keyed by `(nc, &name)`. In this path the name strings live in the program's own variable array, so their addresses are stable.
3. Store `{nc, &name, len, first ≤32 chars, map size, generation, def*}`. On a full match, return `def*`. Otherwise call the original and fill the entry.
4. Definitions live in `std::map` nodes, so `def*` stays valid until that map is destroyed.
5. Invalidation: bump the generation when a GPU program unloads (trampoline on OgreMain's exported `Resource::unload`, or only for GpuProgram instances), and on a hotkey/env. Checking `len`, the prefix and the map size on every hit makes stale hits very unlikely even without that hook.
6. Log hits, misses and lookups per frame.

**Expected gain:** ~0.5–0.9 ms/frame in npc60 (+1.5 to +3 FPS at ~38 FPS), ~0.3–0.5 ms in lighter scenes. It scales with the draw count.

**Risk:**
- Visual/gameplay: none if correct (same definition pointer returned).
- Engineering: low–medium. The only real hazard is a stale cache after a shader reload (graphics-settings change) when a `GpuNamedConstants` object is freed and a new one is allocated at the same address. The generation bump on unload covers it.
- The miss path must let `ItemIdentityException` pass through. Build the hook with unwind info (clang-cl x64 emits .pdata), or tail-jump to the original on a miss.

**A/B:** `KENSHI_PERF_DEBUG=1` hotkey that switches the IAT pointer back and forth (atomic 8-byte store), timestamped in the log. 30 s MangoHud with the camera still in the 60+ NPC save, as in test13/16/17. Also re-run `funcs.py` on a new capture: `0x1800f24f0` and memcmp should drop by most of their share.

### #2 Stop iterating empty render-queue pass groups (`Root::setRemoveRenderQueueStructuresOnClear`)

**What it does now.**
- Each render-queue collection keeps a `std::map<Pass*, vector<Renderable*>>` with one entry for **every Pass ever queued into it**.
- `QueuedRenderableCollection::clear` (`0x18024a460`) walks every entry, every render pass, every frame, just to empty each vector. `acceptVisitorGrouped` (`0x18024aa80`) walks every entry again to skip the empty ones. `RenderPriorityGroup::addRenderable` searches the same, larger maps.
- Main and shadow cameras share the rq ids 10–80. Kenshi also clones materials per character (`Material::clone` from 22 functions; per-character `Mesh::clone`/pose baking in `0x140a274d0`). So these maps fill up with passes that are empty in the current pass.
- The per-instruction samples confirm it: ~90% of these functions' samples sit on the map-node and vector loads in the iteration loops (`0x24a4a8`, `0x24a50a`, `0x24aab8`, `0x24ab10` …). Only ~24 samples are on the visitor call that does real work.
- `removePassGroup` grew 6.5× and `Pass::_dirtyHash` only shows up in npc60, so passes are also being re-hashed every frame.

**Cost:** clear 210 + acceptVisitorGrouped 279 + part of addRenderable 278 + removePassGroup 33 µs ≈ **0.6–0.8 ms in npc60** (≈0.37 ms baseline). These grow 2.4–2.6×, faster than draws (1.5×).

**Patch:**
- Ogre already has an option for exactly this: `Root::setRemoveRenderQueueStructuresOnClear(true)` (exported, `0x180043370`; it writes `Root+0x198`).
- `SceneManager::prepareRenderQueue` (`0x1802bccc3`) passes this flag to `RenderQueue::clear(bool)` before each scene render. With `true`, `RenderQueueGroup::clear` (`0x18003dae0`) deletes the priority groups, so only passes actually used in that render get re-created.
- Kenshi never calls the setter (0 call sites in `rek_calls.txt`), so the default `false` is active.
- Plugin: `Ogre::Root::getSingletonPtr()` (export `0x1802a2a40`) at start, then write the byte (or call the setter). A hotkey can flip it at runtime.
- Variant to test as well: set it `true` for one frame every N frames (periodic purge) instead of every frame.

**Expected gain:** 0–0.4 ms in npc60. **It must be measured:** with `true`, every render re-allocates the priority groups and pass-group map nodes/vectors (Ogre pooled allocator). That can cancel the gain in light scenes.

**Risk:** very low. It is a supported Ogre option, with no code patch and no visual effect. The flag is read once per scene render.

**A/B:** hotkey toggle inside one session, as above. It costs about 15 minutes to try, so **test this first**.

### #3 (Out of scope, noted) Animation LOD
Kenshi calls `AnimationStateSet::_notifyDirty` on characters' animation states from `0x14052bd10` (also calls `Entity::getAllAnimationStates`; reached from `0x1407d3060`/`0x1407d2e90`/`0x1407d14b0`, all new or growing in npc60). If that runs every frame for every character, the dirty-frame counter changes every frame. That is consistent with the AnimLOD design: set `+0x284` *after* Kenshi's dirtying, inside the `Entity::updateAnimation` hook. Worth checking in that work.

### #4 Grows strongly, but the evidence is too thin to patch: Wine heap and main-thread PhysX
- **Wine `heap_allocate_block`** (ntdll `0x170051f40`, the slow path of the Wine heap, not the small-block bins): 17 → 109 µs (6.6×). Plus `RtlWakeAddressAll` 27 → 54 and SRW/critical-section time. ≈ +0.15–0.25 ms. Something allocates more, or larger blocks, per frame with more NPCs. The caller is unknown (no stacks).
- **PhysX on the main thread:** 54 → 295 µs (5.5×), mostly float geometry code (`0x1802b3da0`, `0x1800b96b0`, `0x180170710`, `0x1801de720`). It looks like ray/shape queries Kenshi makes from the main thread (PhysX 2.8 is called through vtables, so the callers can't be found statically). Possibly per-character ground/visibility raycasts.
- Without real call stacks neither can be pinned to a patchable caller. **Enabling step:** a tiny "StackProbe" plugin. A Windows-side sampler thread (~1 kHz) would `SuspendThread(main)` + `GetThreadContext` + `RtlVirtualUnwind` over the exe/Ogre `.pdata` for ~12 frames, then log the leaf → caller chains for 10 s. That gives the real callers of memcmp, heap_allocate_block and the PhysX entries, and confirms the split assumed in #1. Risk: diagnostics only, off by default.

### #5 `VertexBufferBinding::getHasInstanceData` (0.37 ms, per draw)
`0x180109c80` walks the binding's buffer map and reads `buffer+0x58` (isInstanceData) on every call. That is ~2 cache misses per draw, called virtually (likely from `D3D11RenderSystem::_render`). The result only changes when bindings change. But a side-table cache keyed by the binding pointer would itself cost a cache miss per draw, so the realistic gain is ≤0.1–0.2 ms. Low priority.

### #6 Kenshi's own main-thread code: no target
+1.0 ms, spread over ~480 functions. The largest growers are each 40–80 µs:
- `0x140684600`: iterates a per-object vector of event/command codes with a switch
- `0x14083d010`: `std::map<ptr, Vector3>::operator[]`
- `0x14052c170`: linear walk of a linked list calling `0x14005225c` per node; the only O(n)-search shape seen, worth ≤43 µs
- `0x1407d3060`, `0x1404cb5d0` (virtual dispatch over a listener vector), `0x140852bd0`

None is worth a patch alone, and no O(n²) hot spot is visible in the samples.

AI/pathing/simulation runs on a separate thread (tid 26561, ~19% of one core, Kenshi + PhysX). The earlier off-CPU analysis showed the main thread waits on Ogre workers, not on that thread. So **throttling per-NPC AI would not raise FPS at 1× speed.** It might matter at 2–3× game speed (not measured).

### Not worth pursuing (rechecked)
- `Material::getBestTechnique`: samples sit on the scheme-index/LOD map loads (cache misses). No missing-scheme arbitration in the hot path, nothing redundant.
- `_setPass`, `getILayoutByShader`, `SubEntity::*` getters, `Entity::_updateRenderQueue`, `TagPoint::updateNode`: per-draw/per-attachment cache-miss costs. They cost more with more characters only because per-character cloned materials defeat pass batching, which a small patch can't change.
- The constant memcpy into D3D11 buffers can't be skipped: the buffer is per program, so consecutive draws differ (world/bone matrices).

## Particles (separate, per coordinator)
- `BillboardRenderer::_updateRenderQueue` (PU `0x1800ca960`) **already returns early for empty pools**: it compares the pool begin and end at `0x1800ca9b2–0x1800ca9bf` before `beginBillboards`. Systems with 0 billboards cost only the call. The rest is per billboard (`injectBillboard`, then Ogre's per-billboard cull and `genVertices`). Billboards never appear in shadow passes (PerfProbe: FX/2D 0 shadow draws). No per-system sorting or redundant pass was found here.
- One cheap lever that scales with the number of systems: **PU `nonvisible_update_timeout`**. Only 1 of the 93 `.pu` scripts sets it (5 use `fast_forward`, 1 `lod_distances`), so every off-screen system keeps simulating. PU's `_processParticles` is ~9% of Ogre worker time (part of what the main thread waits for), plus `_updateTechniques`/`_prepare` on the main thread (~0.14 ms).
  - Patch: call the exported `ParticleSystem::setNonVisibleUpdateTimeout(float)` (e.g. 1–2 s) on every system as it is created or started, or as a data-mod edit to the scripts.
  - Visible systems are unaffected (no pop-in). Off-screen fires and smoke pause until seen again.
  - Expected gain: small in our captures (<0.2 ms). It does **not** help the Black Desert City rain case, where the billboards are on-screen. There, the only levers are per-billboard cost or the rain quota/emission rate (`Kenshi_*Rain*.pu`: e.g. `visual_particle_quota 5000`, `emission_rate 6400`), which is a visual trade.

## Suggested order
1. #2 flag A/B (minutes, zero risk).
2. #1 IAT-hook cache (largest expected gain; small plugin, same build setup as MyGUI Quad Fix).
3. StackProbe diagnostic (#4), to pin the heap/PhysX/memcmp callers before any further patch.
4. Particle timeout only if particle-heavy scenes matter.
