# Kenshi performance plugins

[RE_Kenshi](https://github.com/BFrizzleFoShizzle/RE_Kenshi) plugins that cut main-thread frame time in Kenshi, plus the profiling notes behind them. Developed on Linux (Proton Experimental, DXVK) and built there. The plugins only patch code in memory and ship no game files.

Targets Kenshi 1.0.65 (Steam) with RE_Kenshi. Each plugin checks its patch addresses against the exe and does nothing if they don't match.

## Start here (Linux / Proton): the DXVK setting

Set this Steam launch option first. It costs nothing and needs no plugin:

```
DXVK_CONFIG="d3d11.cachedDynamicResources = a" %command%
```

It makes DXVK keep Kenshi's dynamic buffers in host-cached memory, which removes slow read-backs in the UI (MyGUI). Measured effect:

- MyGUI's share of the main thread roughly halved, from 12-20% to about 6%; the UI cost fell from about 3 ms to about 1.5 ms per frame.
- In a heavy, UI-busy save (many name tags, large squad) a single before/after screenshot pair showed about 29 FPS -> 36 FPS with the UI on, and no change with the UI hidden. That one is **not a logged benchmark**.

How it compares with the plugins, from the logged tests: ShadowThrottle (-2.7 to -4.0 ms) is the biggest single gain in the logs, and the other plugins give about 1 ms each. The DXVK setting is the cheapest gain and the biggest one in UI-heavy scenes, but not the biggest overall. It does nothing on Windows, where there is no DXVK; there the plugins are what you get.

## Plugins

| Plugin | What it does | Measured (author's setup) |
|---|---|---|
| `plugins/MyGUIQuadFix` | Replaces MyGUI's `VertexQuad::set`, which reads back from GPU-mapped memory | MyGUI 6-7.6% -> 1.7% of the main thread, about 1 ms per frame |
| `plugins/ShadowThrottle` | Skips Kenshi's shadow update on some frames and corrects the camera-relative shadow matrix | -2.7 ms (light scene) to -4.0 ms (60+ NPCs) |
| `plugins/ShaderConstCache` | Caches D3D11 shader-constant name lookups | -1.0 to -1.4 ms in a 60+ NPC save |
| `plugins/PerfProbe` | Logging only, for profiling | none |

`experimental/` holds plugins that were tested and gave no gain (AnimLOD, BarrierSpin, CharacterDetailCull, CharProbe, MyGUILayoutGuard, RenderQueueTrim, StackProbe). They are kept as working examples of the hooking approach.

Also helps on Proton: set the launch option `DXVK_CONFIG="d3d11.cachedDynamicResources = a"`. It roughly halves the UI's main-thread cost.

Numbers are from one laptop (Ryzen 6800HS, RTX 3070 eGPU) and have **not yet been re-measured on a clean install**. See `docs/results.md` for every test.

## Build

Needs `clang-cl`, `lld-link`, `llvm-dlltool`. No Windows SDK or CRT.

```bash
cd plugins/ShadowThrottle && ./build.sh   # writes build/ShadowThrottle.dll
```

Install: copy the DLL next to the files in that plugin's `mod/` folder, put the folder in `Kenshi/mods/`, and enable it in the mod list (below RE_Kenshi).

## Runtime control

Plugins poll `perfctl.ini` in the Kenshi folder four times a second. `python3 tools/perfctl.py` is a panel for it (`show`, `set key=value`, `ab KEY A B` for a scripted A/B against a MangoHud log). `tools/analysis/` has the perf-sample attribution scripts.

## Docs

- `docs/README.md`: original profiling log and method
- `docs/results.md`: every A/B test
- `docs/scaling-research.md`: what scales with NPC count
- `docs/mangohud/`: raw MangoHud logs

## Status

Licensed under GPL-3.0 (see `LICENSE`), the same as RE_Kenshi. No binary release yet. Numbers above are from the author's modded install; a clean 1.0.65 install benchmark is still to do.
