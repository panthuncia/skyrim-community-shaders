# Build speed

How the plugin's incremental builds were made fast, what it costs, and the rules that keep them fast.
The settings live in `cmake/BuildSpeed.cmake`.

## What was measured

Machine: Ryzen 7 9800X3D (8 cores, 16 threads), Ninja, MSVC 14.51, preset `Perf-Fast` (`/O2 /Ob2 /Z7`, no LTO).
Each scenario touches one file and builds the `CommunityShaders` target. Wall time includes vcvars,
CMake's checks and the link. CPU-seconds and object counts come from `.ninja_log`.

Where the time went, before:

- **Header parsing.** A typical Drawcall Limit Fix (DCLF) file compiled alone takes 7.4 s in the front
  end and 0.8 s in the optimizer. `/d1reportTime` puts about 4.6 s of that in headers after the PCH:
  DCLF's own headers (it includes most of its tree in every file, about 570 headers), then
  OpenRenderGraph, ORGModuleServices, flecs, BasicRHI and the Vulkan headers. The rest is template
  instantiation (std, DCLF, ORG, magic_enum). `/Od` would not help.
  An ordinary feature file takes about 1 s in the front end, a Menu file 2 s.
- **Module scanning.** With `/std:c++latest`, CMake scanned every source for C++20 module imports
  before compiling it: one more preprocessor pass per file. In a full build that was 3,250 of
  about 10,800 CPU-seconds. The plugin has no modules.
- **Fan-out.** `FrameAnchors.h` reached 224 files, `RenderGraphRuntime.h` 245, `Frame/FrameDescription.h`
  251 (all but 29 through `RenderGraphRuntime.h`). `ninja -t deps` counts each file twice (the
  object and the module-scan output), so its counts are double these.
- **Contention.** 16 compiles on 8 cores run about three times slower each than one alone.

## What changed

1. **Module scanning off** (`CXX_SCAN_FOR_MODULES OFF`, all presets). It does not change the
   compiled code.
2. **Unity builds for Drawcall Limit Fix and the render graph** (`SC_UNITY_BUILD`, on in `Dev-Fast`
   and presets that inherit it, off in the shipping, PR and Release presets). Sources under
   `src/Features/DrawcallLimitFix/` and `src/RenderGraph/` compile in batches of 8
   (`SC_UNITY_BATCH_SIZE`), in sorted path order, cut per directory two levels under `src/`.
   536 files become 67 objects. The rest of `src/` compiles one file per object.
3. **Render-graph headers in the PCH** (`include/PCHRenderGraph.h`, all presets with the render
   graph). BasicRHI's `rhi.h`, OpenRenderGraph's most-included headers and ORGModuleServices'
   `GrowableBufferSet.h` are parsed once per build. `include/PCH.h` (upstream's) is unchanged.
4. **`Frame/SegmentId.h`.** `RenderGraphRuntime.h` needed only the segment count from
   `FrameDescription.h`. The segment list moved to its own header, so an edit to the description's
   fields rebuilds 29 files instead of 251.
5. **The scene settings catalog ignores DCLF's files** as inputs. The generator skips that tree
   (`build_entries`), but every DCLF edit used to rerun it (7 s, before any object compiles).

Clashes the unity batches exposed were fixed by renaming file-local names (for example two files'
anonymous `kSlots`, `Table()`, `Resolved`) and by qualifying `Engine::` as `DCLF::Engine::` where a
batch also declares `DCLF::Features::Shadows::Engine`.

## Before and after

`Perf-Fast`, wall time / objects compiled / compile CPU-seconds. The link (non-incremental in
`Perf-Fast`) went from about 10 s to about 7.5 s with fewer objects.

| Scenario | Before | After |
| --- | --- | --- |
| touch `RenderGraph/FrameAnchors.h` | 262 s / 224 / 3,283 | 104 s / 96 / 1,548 |
| touch `RenderGraph/RenderGraphRuntime.h` | about 280 s / 245 (estimated) | 112 s / 93 / 1,639 |
| touch `RenderGraph/Frame/FrameDescription.h` | 289 s / 251 / 3,715 | 41 s / 20 / 305 |
| touch `RenderGraph/Frame/RecipeTable.h` | (2 files) | 24 s / 2 / 12 |
| touch `Buffer.h` (rebuilds everything) | 650 s / 866 / 7,600 | about 240 s (estimated from the full build) |
| touch one DCLF `.cpp` (`GrassReport.cpp`) | 29 s / 1 / 8 | 15 s / 1 batch / 6 |
| touch one RenderGraph `.cpp` (`RunGate.cpp`) | 25 s / 1 / 4 | 26 s / 1 batch / 8 |
| full rebuild of the plugin's objects | 631 s / 868 / 7,566 | 234 s / 400 / 3,766 |

What each change bought, from the intermediate measurements: unity builds with module scanning off
took the full build from 631 s to 262 s and the `FrameAnchors.h` touch from 262 s to 131 s; the PCH
addition then took them to 234 s and 104 s. The `SegmentId.h` split took the `FrameDescription.h`
touch from 140 s to 48 s (before the PCH addition; 41 s with it).

A one-file edit outside DCLF still pays the catalog generator (about 7 s before any compile) and the
link. A one-file edit now recompiles its batch of 8, which takes about as long as one file did
(the batch shares one header parse).

## Tried and dropped

- **Batches of 16.** It exposed more name clashes, and the estimated gain on a header touch was
  about 15 s while each one-file edit would recompile twice as much.
- **Unity builds for all of `src/`.** It would cut the full build further (the 333 files outside the
  two trees are now about 60% of its CPU time), but it needs renames in upstream files (for example
  `BaselineManager.cpp`'s `using namespace SKSE;` breaks every later file in its batch), which would
  conflict with upstream merges.
- **Taking `Buffer.h` and `Globals.h` out of the PCH.** Hundreds of files rely on the PCH for them,
  most of them upstream's; each would need explicit includes. With the faster full build the gain
  no longer justifies it.
- **Cutting `FrameAnchors.h`'s fan-out.** No single include edge carries much of it: DCLF's headers
  reach it through many paths, and 29 other files include it directly because they use it.

## For shipping builds

The shipping (`ALL`, `Release`), PR and CI presets do not use unity builds; their optimization, LTO
and PDB settings are unchanged. They do get module scanning off and the larger PCH, so they compile
somewhat faster too.

After pulling the PCH change, CMake may leave `cmake_pch.hxx` with its old timestamp, so the PCH is
not rebuilt with the new header until something in it changes. Builds still work (each file includes
the headers itself); to get the speed, touch `include/PCH.h` once or delete
`build/<preset>/CMakeFiles/CommunityShaders.dir/cmake_pch.*`.

## Rules for keeping it fast

- **Headers that DCLF's `Api/*.h` or other widely included headers include must not include
  `FrameDescription.h`, `RecipeTable.h` or other often-edited render-graph headers.** Include the
  narrow header (`Frame/SegmentId.h`) or forward-declare. Check the fan-out with `ninja -t deps`
  (halve its counts) before adding an include to a header included by hundreds of files.
- **Do not add project headers to the PCH.** `include/PCHRenderGraph.h` takes only stable submodule
  headers, and none that include Vulkan's headers (volk must come first, and it hides Vulkan's
  exported functions, which `Features/Upscaling` calls).
- **File-local names in DCLF and `src/RenderGraph` must be unique within their batch.** Prefer names
  that say what they are (`kInputSlots`, `ContributorTable()`) over generic ones (`kSlots`,
  `Table()`, `State()`, `Resolved`) in anonymous namespaces, and qualify namespaces that a sibling
  file could shadow (`DCLF::Engine::`, not `Engine::`, under `DCLF::Features::Shadows`). Adding a
  file reshuffles its subtree's batches, so a clash can appear in files you did not touch: rename
  the newer name.
- **A unity build hides a missing include** (an earlier file in the batch included it). The PR
  preset and CI build without unity and catch it; before pushing, a non-unity build of the files you
  changed is the check (`-DSC_UNITY_BUILD=OFF`).
- **Files that call Vulkan's exported functions directly** must not be batched (each batch starts
  with `volk.h`, which defines `VK_NO_PROTOTYPES`). None in the two trees do today; mark such a file
  `SKIP_UNITY_BUILD_INCLUSION`.
