# Kenshi performance profiling notes

Started 2026-10-18. Goal: find out why Kenshi loses FPS, starting with the UI, and build a repeatable way to profile the game on Linux.

## Setup

| | |
|---|---|
| Machine | ASUS laptop, Ryzen 7 6800HS (Rembrandt iGPU) + **ASUS XG Mobile eGPU**: RTX 3070 Laptop GPU (8 GB), NVIDIA 615.78.8. supergfxctl mode `AsusEgpu`. Game runs on an external monitor attached to the XG Mobile (no PRIME copy). Link: PCIe Gen3 x8 under load (verified: `3, 8, P0`; idle drops to Gen2/P5) |
| OS | CachyOS, kernel 7.2.9, KWin Wayland, fish shell |
| Runtime | Proton Experimental, DXVK v3.1.1-97 |
| Launcher | RE_Kenshi relaunches the game. **The real game process is `./RE_Kenshi/kenshi_x64.exe --norestart`**, not the first `pgrep kenshi_x64.exe` hit (those are Steam/Proton wrappers) |
| Other hooks | `_kenshi_fix1.asi`, `RE_Kenshi.dll`, ~40 data mods |
| Engine | Ogre3D (D3D11 render system) + MyGUI for UI, mostly single-threaded |
| UI toggle | F7 |
| Wine sync | ntsync in use (verified 2026-10-18: `lsmod` use count ~14k while the game runs) |

Constraint: **no CPU boosting / performance power profiles.** Gains must come from settings, DXVK config, or mods.

## Results at a glance

Everything below is main-thread CPU time; the game is limited by one thread. Each number comes from a 30 s MangoHud log at a fixed camera, compared inside one log via a toggle where possible. Different rows use different saves and spots, so compare **milliseconds per frame**, not FPS across rows. Raw data and conditions are in [results.md](results.md).

### What is active now

| Change | Type | Before | After | Gain |
|---|---|---|---|---|
| DXVK `d3d11.cachedDynamicResources = a` (launch option) | config | UI cost **~3 ms** (52 vs 62 FPS UI on/off, first save) | UI cost **~1.2 ms** (48 vs 51 FPS, older save) | UI cost roughly halved; MyGUI's share of the main thread 12–20% → ~6% |
| **MyGUI Quad Fix** plugin | engine patch | MyGUI ~6–7.6% of main thread | **~1.7%** | Light save: 52.0–52.2 → 53.5 FPS (~0.5 ms). 60+ NPC save: 27.1 → 26.1 ms (~1 ms, ~+1 FPS). UI on vs off is now within noise |
| **Shadow Throttle** plugin, shadow mode 1 (CSM) | engine patch | 18.9 ms | 16.1 ms | **−2.8 ms, 52.8 → ~62 FPS** |
| **Shadow Throttle** plugin, shadow mode 2 (RTW, default) | engine patch | 19.6 ms | 17.1 ms | **−2.5 ms, 50.9 → 58.5 FPS** |
| **Shadow Throttle**, interval 3, 60+ NPC save (test 25) | engine patch | 29.8 ms | 25.8 ms | **−4.0 ms, 33.5 → 38.9 FPS** |
| **Shadow Throttle**, interval 3, Drycoom ~21:30 (test 26) | engine patch | 21.3 ms | 18.6 ms | **−2.7 ms, 46.9 → 53.7 FPS** |
| **Shader Const Cache** plugin, 60+ NPC save (tests 30–31) | engine patch | 25.9–26.2 ms | 24.6–24.8 ms | **−1.0 to −1.4 ms, ~+4–5%** |

**UI, original vs now:** the UI started at ~3 ms per frame (about 16% of the frame at 62 FPS), mostly from one MyGUI function reading back from GPU-mapped memory. After the DXVK option and the quad fix, toggling the UI (F7) no longer makes a measurable difference.

**Added 2026-10-19:** Shader Const Cache (−1.0 to −1.4 ms in the 60+ NPC save, visually checked) stacks with the plugins below; it cuts render-system bookkeeping, not UI or shadow work.

**Combined (estimate):** the two plugins stack because they cut different work (UI quads vs shadow passes). In shadow mode 2 they save about **3 ms per frame**: in the spot measured, roughly 19.6 → ~16.6–17 ms, or **~51 → ~59–60 FPS**, with shadows and UI unchanged visually. Together with the DXVK option, the UI is ~3 ms cheaper than the original setup in UI-heavy scenes. These estimates are added up from separate A/B tests, not measured in one run.

### Measured but not applied (your choice)

| Setting | Gain | Note |
|---|---|---|
| `npc range` 4096 → 3000 | 52.2 → 57.2 FPS (~1.7 ms) | Fewer characters to render |
| `shadow mode` 2 → 0 (off) | 19.6 → 16.6 ms (~3 ms, +9 FPS) | Shadow Throttle now recovers most of this with shadows on |
| `shadow mode` 2 → 1 | ~1.4 ms *slower* | Not a cheaper middle ground |
| Objects range, shadow range, names/floaters, grass | within noise | No measurable cost at the tested spot |
| All ~89 mods disabled | 52.2 → 52.0 FPS | Mods cost nothing measurable; the cost is vanilla Kenshi/Ogre |

### What's left
The remaining frame time is spread across Ogre's per-object rendering (~43% of the main thread, no single function above ~4%), the D3D11 backend and DXVK (~24%, per draw call), and Kenshi's own logic (~12%). It grows with characters × sub-meshes × render passes. Investigated further: with function-accurate attribution, no Ogre function is above ~4% (`SceneManager::_setPass` 3.75%), and the earlier "~8% shader-parameter copying" was a measurement artifact (really <0.5%). **No worthwhile patch target is left in per-object rendering**; see [results.md](results.md). Use `scripts/funcs.py`, not `syms.py`, for function-level numbers.

**Next candidate, built but not enabled or tested:** the **Animation LOD** plugin (`plugin-src/AnimLOD`, staged in `mods/Animation LOD/`, not in mods.cfg). Ogre evaluates every character's skeleton every frame, which is ~33% of worker time. AnimLOD makes Ogre use its own fast path for distant characters on most frames, and its log reports how much animation CPU it removed. The open risk is Kenshi's manually controlled bones, which can block skipping. See [results.md](results.md) "AnimLOD 1.0".

**Content-dependent cost not yet tested here: particles in rain and in cities.** Separate research ([Micromanner/kenshi-performance-research](https://github.com/Micromanner/kenshi-performance-research)) measured particle billboards at ~1/3 of the frame in Black Desert City with rain, on Windows. In our test spots they are ~6% of the main thread. The same research fixed the MyGUI rebuild at its source, with a button-visibility bug; our Quad Fix already recovers most of that cost. See [results.md](results.md) "External research".

## What we found

### 1. The game is single-thread CPU-bound
MangoHud: GPU sits at ~50% while total CPU shows ~20%. The total CPU number hides one maxed-out core. perf confirms that the main thread (tid = game PID) is busy ~80% of wall time whether the UI is on or off.

### 2. UI cost is MyGUI, and almost all of it is one function
- UI on vs off, first save (1 character): **52 vs 62 FPS** (19.3 ms vs 16.2 ms), so about 3 ms per frame.
- On the main thread, `MyGUIEngine_x64.dll` was 12–20% of samples with the UI on and ~1% with it off.
- **~92% of MyGUI's time is in one unexported function, RVA `0x199460`–`0x19955f`.** It writes one UI quad (6 vertices × 24 bytes: xyz, colour, uv) straight into the mapped vertex buffer, then reads 2 vertices back from that buffer to duplicate them.
- Sampling RCX in that function showed the destination is always memory mapped from `/dev/nvidia0`, i.e. a DXVK-mapped GPU buffer.

### 3. DXVK fix: `cachedDynamicResources = a`
- DXVK **already ships a built-in Kenshi profile** with `d3d11.cachedDynamicResources = v` (vertex buffers only). Setting `= v` yourself does nothing.
- Overriding with `= a` (all dynamic resources) roughly **halved MyGUI's share** of the main thread (~12% → ~6%).
- Older save with more characters: **48 vs 51 FPS** (UI on vs off), about 1.2 ms of UI cost.
- Current launch option:
  ```
  DXVK_CONFIG="d3d11.cachedDynamicResources = a" MANGOHUD=1 %command%
  ```
- Not yet checked: whether `= a` raised GPU load. There was ~50% headroom.

### 4. Where the main thread goes now (UI on, `= a`)
| Module | Share | What it is |
|---|---|---|
| `OgreMain_x64.dll` | ~42% | Scene traversal, render queue, material/pass setup, GPU params, billboards, instancing |
| `d3d11.dll` (DXVK) | ~12% | D3D11 → Vulkan translation of draw calls |
| `RenderSystem_Direct3D11_x64.dll` | ~11% | Ogre D3D11 backend; the hot spot is a `std::map` lookup, not memory stalls |
| `Kenshi_x64.exe` | ~11% | Game logic |
| `Plugin_ParticleUniverse_x64.dll` | ~6% | Particles |
| `MyGUIEngine_x64.dll` | ~6% | UI |
| `msvcr100.dll` | ~5% | CRT (memcpy/alloc) |
| `RE_Kenshi.dll` | <1% | Not a cause |

Hot Ogre exports seen: `SceneManager::_setPass`, `GpuProgramParameters::_copySharedParams`, `BillboardSet::genVertices`, `InstanceBatchHW::updateVertexBuffer`, `Material::getBestTechnique`.

### 5. Engine patch: MyGUIQuadFix (RE_Kenshi plugin)
- Source and build: `plugin-src/MyGUIQuadFix/` (`quadfix.cpp`, `build.sh`). Builds on Linux with clang-cl + lld-link; no Windows SDK or CRT needed (kernel32 import lib is generated from a .def).
- Installed as mod `mods/MyGUI Quad Fix/` (`MyGUIQuadFix.dll`, `RE_Kenshi.json` with `"Plugins"`, a zero-record `.mod` header cloned from Heightmap Fix), enabled at the end of `data/mods.cfg`.
- RE_Kenshi calls the export `?startPlugin@@YAXXZ` (`void startPlugin()`) at "post-load plugins".
- Patch: an absolute jump written into the int3 padding at `MyGUIEngine+0x199560`, plus a retargeted incremental-link thunk at `+0x6320`, sends `VertexQuad::set` (`+0x199460`) to a store-only replacement. Every byte is verified before anything is written. The original function is left intact.
- Writes `MyGUIQuadFix.log` in the game folder. `MYGUI_QUADFIX=0` starts with the fix off. Since 1.3 the control panel switches between the fix and MyGUI's original function at runtime (`quadfix=0/1`, see "Control panel"), timestamped in the log. The F6 hotkey from 1.1–1.2 is gone.
- Result: MyGUI 6–7.6% → 1.65% of the main thread, +1.3 FPS; the UI on/off difference is now within noise. See [results.md](results.md).

### 6. Engine patch: Shadow Throttle (RE_Kenshi plugin)
- Source and build: `plugin-src/ShadowThrottle/` (`shadowthrottle.cpp`, `build.sh`). Installed as mod `mods/Shadow Throttle/`, enabled after MyGUI Quad Fix in `data/mods.cfg`.
- Kenshi's own `DeferredLightRenderer` (not Ogre's shadow system) renders shadows from one call site (`0x1402d8634`) by mode: mode 2 = RTW (`0x140865ca0` prepare, `0x140864d80` setup, `0x140865010` pass), mode 1 = CSM (`0x140863cb0`, all cascades in one atlas). Each target sits behind a single-caller thunk, which is redirected via 16-byte stubs in int3 padding at `0x140860392`.
- It skips the whole shadow update on N−1 of every N frames. On skipped frames it re-aligns the camera-relative shadow matrix (`shadowViewMat` / `shadow_matrix`): `M' = M_saved · Translate(camNow − camAtRender)`. It finds the matrix by hooking Kenshi's imports of `GpuSharedParameters::getConstantDefinition/getFloatPointer`.
- Runtime control (since 1.5, replacing the F10/F11 hotkeys): `shadow_interval=N` (1 = off) and `shadow_correct=0/1` through the control panel. Each change is timestamped in `ShadowThrottle.log`. Env: `KENSHI_SHADOW_INTERVAL` (**default 3 since 1.4**, was 2; 1 = start off), `KENSHI_SHADOW_CORRECT=0`. The measured gains below were taken at interval 2; at 3 the shadow pass runs on a third of frames (≈6 ms per update in the 60+ NPC save, so ~1 ms more saved on average, not separately A/B-tested). Shadows of moving characters update at 1/3 of the frame rate.
- Result (interval 2, camera still): mode 1 **−2.8 ms (+9 FPS)**, mode 2 **−2.5 ms (+7.6 FPS)**, no visible shadow lag when panning. See [results.md](results.md).

### 7. Diagnostics and experiments (2026-10-18)
- **PerfProbe** (`plugin-src/PerfProbe/`, mod `mods/PerfProbe/`, active only with `KENSHI_PERF_PROBE=1`): times every Ogre scene pass by camera and render-queue range, and (1.2+) every object draw by mesh/type. Main findings for the 60+ NPC save: the main opaque pass is ~38% of wall time, shadows ~11%, work outside scene passes ~36%; characters are ~36% of draws, lights ~16%; no reflection or portrait passes. Its object table switches itself off if another plugin already hooks `renderSingleObject`.
- **Character Detail Cull** (`plugin-src/CharacterDetailCull/`, mod `mods/Character Detail Cull/` with `rules.txt`): skipped small character-part draws (hair, hats, weapons…) beyond a distance and in shadows. It worked (~7.5% of draws skipped) but gave **no measurable gain** (24.4 vs 24.3 ms), because skipping at `renderSingleObject` only saves draw submission. **Disabled** (removed from `data/mods.cfg`); files kept.
- **Barrier Spin** (`plugin-src/BarrierSpin/`, mod `mods/Barrier Spin/`): Ogre's worker barrier spins up to 50 µs before sleeping. ~60% of waits avoided sleeping, but **no frame-time gain** and slightly more CPU power (A/B: 25.1/27.1 ms on vs 24.6 ms off). **Disabled**; files kept. Off-CPU analysis showed the main thread waits ~15% of the frame at that barrier for the workers' actual work, not for wake-ups. Fewer workers (`PROTON_CPU_TOPOLOGY=8:…`) didn't help either (24.7 vs 24.0 ms).
- `npc range` 3000 → 1000 saved ~3–4 ms in the 60+ NPC save (whole characters removed: culling, queueing, animation and all parts), but distant characters disappear.

## TODO: publish the plugins

Eventually move each plugin into its own GitHub repository:
- **MyGUI Quad Fix**: `plugin-src/MyGUIQuadFix/` + mod files in `mods/MyGUI Quad Fix/`
- **Shadow Throttle**: `plugin-src/ShadowThrottle/` + mod files in `mods/Shadow Throttle/`

Each repo should carry the source, `build.sh` (clang-cl + lld-link, no SDK/CRT), the mod packaging (`RE_Kenshi.json`, the zero-record `.mod` header), and a README covering: what it patches (RVAs and byte checks against Kenshi 1.0.65 Steam + RE_Kenshi), install steps, env vars and perfctl.ini keys, the measured gains from [results.md](results.md), and known limits. **Release rule:** before releasing any plugin or script, benchmark it on a **clean copy of Kenshi**: a fresh Steam install of 1.0.65 with only RE_Kenshi and the plugin being released, no other mods, and a fresh save made in that copy. The README's numbers must come from that run, with a separate note on how it does alongside common mods. Day-to-day testing stays on the modded install with the test mod list (see "Mod lists" below). Before publishing, decide on a license and check RE_Kenshi's plugin and redistribution terms. The plugins only patch code in memory and ship no game files.

## Next ideas

**Settings A/B tests were run on 2026-10-18; see [results.md](results.md).** Only `npc range` helped (4096 → 3000: 52 → 57 FPS). The rest were noise. The table below is the original list of candidates.

**Settings to try**, one at a time, watching MangoHud frametime. Current values are from `settings.cfg`:

| Setting | Now | Try |
|---|---|---|
| `npc range` | 4096 | 2500–3000 |
| `objects view range` | 4096 | 2500–3000 |
| `Global population multiplier` | 1.19 | 1.0 |
| `Show names` / `floaters` | 1 / 1 | 0 |
| `Shadow Range` (`shadow mode=2`) | 5000 | lower |
| `foliage range` / `grass range` | 1 / 3 | lower |

Back up `settings.cfg` before editing.

**Deeper profiling:**
- Break OgreMain down further with `funcs.py` (function-accurate; `syms.py` mislabels unexported code): which Ogre functions grow with character count or view range.
- Count draw calls with `DXVK_HUD=drawcalls,submissions` across settings changes.
- If MyGUI still matters: find out what calls the quad writer, and how often. perf callchains through MSVC code are broken (no frame pointers). Options: `--call-graph dwarf` plus a manual stack scan, or a RE_Kenshi plugin hook.
- Possible mod-level fix: hook the MyGUI quad writer so it doesn't read back from the mapped buffer.
- Look at the `Kenshi_x64.exe` share as squad size grows (AI/pathing mods: SCAR's).

## How to profile (recipe)

Requires `perf` (installed). `kernel.perf_event_paranoid=2` is fine for profiling your own processes. Commands are fish syntax.

```fish
# 1. find the real game PID
set p (pgrep -f 'RE_Kenshi/kenshi_x64.exe' | head -1)

# 2. save the memory map while the game is running (needed to attribute samples)
cp /proc/$p/maps maps.txt

# 3. record (main thread only: -t $p; whole process: -p $p)
perf record -F 999 -g -t $p -o run.data -- sleep 15

# 4. dump samples (add 'time' for timeline.py)
perf script -i run.data -F tid,ip > samples.txt
perf script -i run.data -F tid,time,ip > samples_t.txt
```

**A/B trick:** record 60 s, keep the UI on for ~30 s, press F7, then run `timeline.py`. The per-second table shows exactly when the toggle happened.

### Why the scripts exist
Under Wine, PE DLL code sections are anonymous mappings, so plain `perf report` labels ~80% of samples `[JIT]`. The scripts rebuild each DLL's address range from its header mapping (file offset 0) plus `SizeOfImage` from the PE header. `perf report` labels are unreliable for Wine DLLs; trust the scripts.

| Script | Usage | Output |
|---|---|---|
| `scripts/attribute.py` | `python3 -I attribute.py maps.txt samples.txt [tid]` | Share per thread and per module, optionally for one thread |
| `scripts/timeline.py` | `python3 -I timeline.py maps.txt samples_t.txt tid` | Per-second samples by module (for UI on/off splits) |
| `scripts/hot.py` | `python3 -I hot.py maps.txt samples.txt tid Module.dll` | Hottest 64-byte RVA buckets in one module + first caller outside it |
| `scripts/syms.py` | `python3 -I syms.py maps.txt samples.txt tid` | Hot spots in MyGUI/OgreMain/RenderSystem resolved to the nearest **exported** symbol. Hits far past the last export are internal code, so use `hot.py` + objdump. **Can mislabel**: any unexported code after an export is counted under that export. Prefer `funcs.py` |
| `scripts/funcs.py` | `python3 -I funcs.py maps.txt samples.txt tid Module.dll [top]` | **Function-accurate** breakdown of one module from its `.pdata` table (works for unexported code), with export names where available |
| `scripts/diff.py` | `python3 -I diff.py A.maps A.txt B.maps B.txt tid ratio` | Per-frame A/B diff by module and by OgreMain export (normalize with the frame-rate ratio B/A) |
| `scripts/calls.py` | `python3 -I calls.py image.exe image.asm lo hi [func ...]` | Calls each function in an address range makes, with imports named through thunks and C++ names demangled (`image.asm` from `objdump -d -M intel`) |
| `scripts/bpcount.py` | `python3 -I bpcount.py [-s 10] OgreMain_x64.dll:0xc1620 [Mod:Export ...]` | **Live call counts per frame** in the running game, with no plugin: perf hardware execute breakpoints (`mem:ADDR:xu`). Max 3 targets; adds a frame counter on `Root::_fireFrameEnded`. Each hit costs a debug trap, so don't measure FPS during a run and keep it short for code that runs 10k+ times per frame |

### Control panel (no hotkeys)
Since 2026-10-19 our plugins (Quad Fix 1.3, Shadow Throttle 1.5, Render Queue Trim 1.1) take runtime settings from **`perfctl.ini`** in the Kenshi folder. They check it 4 times a second and report what they applied in `perfctl-status-*.txt`. No `KENSHI_PERF_DEBUG` launch option or hotkeys are needed; environment variables still set the starting values. Code: `plugin-src/common/perfctl.h`.

```bash
python3 perf-profiling/perfctl.py                 # interactive panel: arrows, space, +/-, q
python3 perf-profiling/perfctl.py show            # requested vs applied
python3 perf-profiling/perfctl.py set rqtrim=1
python3 perf-profiling/perfctl.py ab rqtrim       # scripted A/B, see below
```

**Scripted A/B (`ab KEY [A B] [--at 10,20] [--label NAME]`):**
- **What you do:** start a MangoHud log (Shift+F2) and keep the camera still.
- **What the script does:** sets the starting value and waits for the log to appear. It switches to the other value at +10 s and back at +20 s, confirming each switch through the plugin's status file. When the log ends (MangoHud `log_duration=30`), it moves the log to `mangohud/`, appends the switch times to `captures/ab-marks.log`, and prints per-segment frame times and the difference.

Keys: `quadfix`, `shadow_interval`, `shadow_correct`, `rqtrim`.

### Mod lists
`modlists/modlist.sh test|normal|status` swaps `data/mods.cfg` between your normal list and `modlists/test.cfg`. The test list holds only our plugins: Quad Fix, Shadow Throttle and PerfProbe; add `Animation LOD.mod` when testing it. RE_Kenshi itself always loads. `test` saves the current list to `modlists/normal.cfg` first. Run it only with Kenshi closed, and **don't save the game on the test list.** For release benchmarks, use a clean copy instead (see "Release rule" above).

### Lessons from other research (Micromanner, 2026-10-19)
The [Micromanner repo](https://github.com/Micromanner/kenshi-performance-research) did on Windows, with an AI agent driving x64dbg, much of what we do with perf. Practices we adopted:
- **Ceiling test before building.** Measure the most a change could save before investing: disable the whole thing (a `ret` patch, or a plugin's extreme setting) and compare. For AnimLOD: `KENSHI_ANIMLOD_MID=0 KENSHI_ANIMLOD_FAR=0 KENSHI_ANIMLOD_FARN=16` skips 15 of 16 pose updates for everyone (looks wrong, test only). If the ceiling is small, stop there.
- **Count, don't guess.** "Does X run every frame for resting objects?" is answered by `bpcount.py` (their x64dbg counting breakpoints, the Linux way), e.g. MyGUI `RenderItem::outOfDate` (`MyGUIEngine_x64.dll:0x154980`) calls per frame, or skinned entities per frame (`OgreMain_x64.dll:0xc1620`).
- **Visual checks cover windows, not just the world.** Their UI fix shipped with a hidden vendor-window X button because only in-world UI was checked. After any UI or render patch, open the inventory, a vendor or trade window, dialogue, the map and the character screen, and check close buttons and scrollbars.
- **Tag claims by evidence:** confirmed (read from the binary), measured (profiled or A/B), or to verify. Numbers in [results.md](results.md) come from measurements; design notes that are untested say so.
- **Test the content that hurts:** cities, rain and fires (particles), not only big squads.

Disassemble a hot spot (image base is `0x180000000`):
```fish
objdump -d --no-show-raw-insn -M intel --start-address=0x180199460 --stop-address=0x18019955f MyGUIEngine_x64.dll
```

Find out what memory a hot function writes to (example: RCX):
```fish
perf record -F 2999 --user-regs=cx -t $p -o regs.data -- sleep 5
perf script -i regs.data -F ip,uregs
```
Then look up the register values in `/proc/$p/maps`.

DXVK logging, to confirm which config is effective:
```
DXVK_LOG_LEVEL=info DXVK_LOG_PATH=/tmp ... %command%
```
The effective config appears in `/tmp/kenshi_x64_dxgi.log`.
