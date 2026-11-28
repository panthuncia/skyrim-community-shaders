# snap7-lock: the render thread without Renderer::Lock

Lead `lock`, round `snap7`, branch `snap7/lock` (base `org_integration` 7ea6d47bb, with snap7-devloss5), stage `s7-lock`. Continues
[snap7-pace.md](snap7-pace.md) (P-1, decision 228), [post-perf.md](post-perf.md) D-1 and [snap7-thread.md](snap7-thread.md) TH-b.
Dev-Fast (/Od) throughout: frame rates are Dev-Fast, one sample each.

Status: **done.** A routed render takes Renderer::Lock only when it runs engine code: a window change in Begin's body (resizes),
volumetric lighting (the console's TVL flags) or the interface drawn inline (a menu the UI jobs opened). In steady play, repeats
included, the render thread never takes it.

## 1. Commits

| Commit | What |
|---|---|
| a670b06e3 | feat(native): render without Renderer::Lock |
| (this file) | docs: snap7-lock notes and runs |

No submodule changes.

## 2. What the lock protects

Renderer::Lock (SE 75458, AE 77243) is `EnterCriticalSection(renderer + 0x27F0)`. It is not fair: a thread that releases it and takes
it again at once keeps it. Engine callers (AE xrefs in Ghidra) and what they do inside:

| Caller (AE id) | Thread | Inside the lock |
|---|---|---|
| 36555 Main::DrawFrame | game | the whole engine frame (unrouted frames; parks the render thread first) |
| 39039 RunLoadingScreen | serving | the loading screen frame (parks the render thread first) |
| Bink ticks (BinkHooks) | movie | the movie frame (parks the render thread first) |
| 15220 BGSDecalManager update, 15214, 15215 | game | decal creation and release (geometry, the device) |
| BSTaskPool_HandleTask, task 0x47 | task pool | texture purge (discards mips) |
| 36854-36856 | game | the save screenshot: a texture, a target copy, Map/Unmap on the context |
| 36815 lock guard (TES::PurgeBufferedCells and 2 more) | game | cell purge |
| 26175, Precipitation::RenderMask | render | the precipitation mask (engine render code; not called in routed frames) |
| 82324, 36547, ReloadProjectedUVTextures | game | Scaleform shutdown, startup, a console command |

So the lock keeps the engine's D3D context, targets and device to one thread at a time. None of these callers reads what Begin's or
End's bodies write for CS. They keep working unchanged and now never wait for a routed render.

## 3. Begin's and End's bodies (AE 77245 / 77246, SE 75460 / 75461)

Begin, in order (offsets into the renderer, the same on SE and AE):
1. The swap chain's full screen state (+0x70) and the foreground window.
2. A resize asked (+0x34): the resize callback (+0x8), WindowSizeChanged (AE 0x140e43bc0), the window apply (AE 77239). Or, in full
   screen (+0x30) with the window focused but not full screen, the window apply alone.
3. The frame flag (SE 525009, AE 411490) = 1. Only render code reads it (AE 77258, the Bink player).
4. The jitter call (AE +0xE2, SE +0xE5). Upscaling's, HDR's and LegacyGraphicsCompatibility's Main_UpdateJitter hooks sit on it: CS's
   upscaler and Streamline frame start, CS's jitter, the frame camera.
5. The context's ClearState, the shadow state's reset (SE 75622, AE 77437), constant buffer 12 for VS and PS.
6. The current render window global (SE 524730, AE 411349), renderer +0xA68/+0xA70 from it, and the target manager's screen size
   (SE 524970, AE 411451) from the window's client area (the default size, SE 525002/3, AE 411483/4, when that is empty).

End: the engine's frame timer slot 1 (nothing reads it), bLockFramerate:Display's sleep to about 61 fps (inline on AE, a call on SE),
the present on the current window's swap chain (CS's Present hook), the frame counter (removed by SideEffects S2), the frame flag = 0.

## 4. The render half now

`RenderThread::RenderHalf`:
1. `Frame::Driver::StartRendererFrame`: Begin's CS part (claim, adoption, resize event, menus, window state, OnBegin), split out of
   the Begin detour (`RendererBegin::thunk` now calls it, then the engine body, then FaceTint). No lock: every other producer
   (Main::DrawFrame, the loading screen, Bink, the inline interface) parks the render thread before its own Begin, so Begins and
   adoptions never overlap.
2. Whether the render runs engine code: `Bodies::BeginChanges` (a resize asked, a return to full screen, another window, or window
   fields or a screen size that differ from what Begin last set), volumetric lighting, or the interface drawn inline.
3. If so: the lock around `Frame::Driver::EngineBegin` (the engine's own body) and around volumetric lighting, the interface and End,
   as before.
4. If not: `Bodies::Begin` (the frame flag, the jitter call through its current call target, the shadow state's reset), FaceTint, the
   record, the UI replay and Renderer::End, all without the lock.

The stats line counts the renders that took the lock and why: "N took Renderer::Lock for engine code (window a, volumetric lighting
b, interface c), held X ms, at most Y at once".

## 5. Decided by the agent, to confirm

- L-a. Renderer::End stays the engine's, called without the lock, not ported. Its body touches only render-owned memory (its timer,
  bLockFramerate's sleep, the present through CS's hook, the frame flag). SSE Display Tweaks, installed in the test profile, replaces
  bLockFramerate's limiter. A CS copy of End would bypass whatever it patched there.
- L-b. A lock-free render leaves out Begin's ClearState and constant buffer 12: a routed render draws nothing through the engine's
  context. A render that does (volumetric lighting, the inline interface) runs the engine's body under the lock. The shadow state's
  reset stays, since record-time code and the UI replay read its targets; the game thread writes no shadow state in a routed frame.
- L-c. The jitter call goes through the current call target at Begin's site, so whatever hook chain is installed there runs, as it
  does inside Begin.
- L-d. A window change still runs the engine's whole Begin under the lock. It is rare (each resize step), and resizes keep their
  tested path. Porting WindowSizeChanged (swap chain and engine targets) is snap7-thread's package R.

## 6. Measurements

Render thread's Renderer::Lock from the stats line (two 10 s windows after the start mark). Game thread waits from the no-admin
sampler (1 ms for 8 s after the start mark; a wait is a sample whose innermost frame is a kernel wait, grouped by caller).

| Run | Build | fps | updates/s | renders that took the lock | held per 10 s; longest hold | game thread in Renderer::Lock |
|---|---|---|---|---|---|---|
| native-vulkan+perf-dragonsreach, uncapped | base | 203-235 | 59.9-60 | all (1595, 2221) | 3366 / 1978 ms; 20.8 / 10.2 ms | 0 samples |
| same | lock (b-dr) | 195-226 | 60.0 | 0 | 0 | 0 samples |
| native-perf-taa | base | 146-148 | 60 | all | 2675 / 1369 ms; 24.8 / 3.8 ms | 0 samples |
| same | lock (c-taa) | 143.5-144.4 | 60 | 0 | 0 | 0 samples |
| native-perf-taa beside native-tour (lab load, not exclusive) | base | 147 | 60 | all | 2743 / 1340 ms; 19.0 / 13.7 ms | 0 samples |
| same | lock (c-load) | 143.6-144.2 | 60 | 0 | 0 | 0 samples |
| window-resize-stress-taa | lock | 125-151 | 55.6-60 | 8 and 12 (window) | 52 / 82 ms; 11.7 / 12.9 ms | - |
| ui-menus | lock | - | - | 1 in a window (interface) | 24-29 ms, once per menu open | - |

The lock build's perf runs (b-dr, c-taa, c-load) ran while other leads' builds kept 12-13 of 16 CPUs busy (cs-test's cpu_load
clause); the base runs did not. So the 1-3% lower frame rates are that noise, not a cost of the change. The sampled windows caught
no game-thread lock wait even at base: in steady scenes the decal and task pool paths rarely take the lock. The starvation this
removes is snap7-devloss5's e2 (70 s waits after a resize, while the render thread took the lock again at once every repeat): plain
renders and repeats now never take it, so a game-side holder waits at most for one engine-code render's hold.

## 7. Runs (stage s7-lock)

| Label | Config | Build | Result |
|---|---|---|---|
| a-nvq | native-vulkan+quick | a670b06e3 | passed with the shot reference |
| b-dr | native-vulkan+perf-dragonsreach, uncapped, sampled | a670b06e3 | passed |
| base-dr | same | base 7ea6d47bb | passed |
| base-taa, base-load (+ base-loadgen, native-vulkan+native-tour) | native-perf-taa, sampled | base | passed |
| c-taa, c-load (+ c-loadgen, native-vulkan+native-tour) | native-perf-taa, sampled | a670b06e3 | passed; the tour passed with its 3 shots |
| c-nvk-window-resize-stress-taa | native-vulkan+window-resize-stress-taa | a670b06e3 | passed, 13 shots |
| c-nvk-cadence-drop-cells | native-vulkan+cadence-drop-cells | a670b06e3 | passed, 3 shots |
| c-nvk-update-cap30, c-nvk-update-cap240 | native-vulkan+update-cap30 / 240 | a670b06e3 | passed, 3 shots each |
| c-nvk-ui-menus | native-vulkan+ui-menus | a670b06e3 | passed, 4 shots |
| c-nvk-vanilla-screenshot, c-nvk-save-screenshot | native-vulkan+... | a670b06e3 | passed |
| c-native-d3d12-quick | native-d3d12-quick | a670b06e3 | passed |
| c-guest-quick | guest+quick | a670b06e3 | passed (the Begin detour is shared) |

The MO2 stage folder `CS AB s7-lock` is deleted.

## 8. Open items

- Resizes and full screen changes still take the lock for the engine's Begin (holds up to 13 ms). Porting WindowSizeChanged and the
  window apply onto the render thread (snap7-thread package R) removes the last regular use.
- Volumetric lighting under the console's TVL flags and the inline interface still run engine code under the lock (24-29 ms, once per
  menu the UI jobs open). Both go when those draws are CS passes (post-perf D-4).
- Game-thread lock waits are not counted in process; the sampler shows them. A counter would need a hook on Renderer::Lock itself.
