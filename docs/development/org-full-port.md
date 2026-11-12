# The full ORG port: the whole renderer through ORG, DXVK optional

The plan for moving every GPU command in CS and in the engine onto OpenRenderGraph, so BasicRHI can run it on Vulkan or
D3D12 and DXVK is not needed. RE results live in [engine-d3d11-surface.md](./engine-d3d11-surface.md); what an external
mod API will need is collected in [org-extension-api-notes.md](./org-extension-api-notes.md). A task moves out of this
file when it is built.

## Status

Work happens on `org-port/*` branches, never on `org_integration` itself (only docs land there directly). Validated work
collects on the CS branch `org-port/integration`, which also carries the submodule bumps. Nothing is pushed yet; several
pinned submodule commits exist only locally, so a fresh clone cannot fetch them.

| Task                                                                                                             | State                                                                               | Where                                                    |
| ---------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------- | -------------------------------------------------------- |
| Phase R                                                                                                          | Done                                                                                | `engine-d3d11-surface.md`                                |
| P0.1-P0.4 (stencil, blend constants, dual source, logic op, sample mask, depth clip, dynamic viewport/scissor)   | Done, D3D12 and Vulkan smokes                                                       | BasicRHI `org-port/p0-pipeline-state`                    |
| P0.5-P0.6 (input layouts, keyed pipelines at record time, generate mips, MSAA resolve)                           | Done, ORG tests 32/32                                                               | ORG `org-port/p0-pipelines`                              |
| P0.7, P0.11 (memory budget, native crash markers) and the AMD device-creation fix                                | Done                                                                                | BasicRHI `org-port/p0-device-diagnostics`                |
| P0.8-P0.10 (present without a host wait, HDR colour spaces and metadata, frame latency, tearing)                 | Done; HDR and present_wait not run (no HDR display, no driver support)              | BasicRHI `org-port/p0-present`                           |
| P0.12-P0.13 (on-disk pipeline cache, allocator owners and spillable memory)                                      | Done; defragmentation not built                                                     | BasicRHI `org-port/p0-cache-alloc`                       |
| All of Phase 0 merged                                                                                            | Done, every smoke passes on both backends                                           | BasicRHI `org-port/p0-integration`; CS builds against it |
| P2.1-P2.5 (graph resources, anchors, shader builds, raster programs, GPU timers)                                 | Done, checked in game                                                               | CS `org-port/integration`                                |
| P2.6-P2.7 (shared constants, deferred composite)                                                                 | Done; composite registers once, A/B within noise                                    | CS `org-port/integration`                                |
| P1 (ownership inversion) and its fix (graph in the owned frame, HDR without a copy)                              | Merged; **open:** with DCLF on the owned frame stops presenting                     | CS `org-port/integration`                                |
| P1.6 (ImGui on BasicRHI, Overlay anchor)                                                                         | Done; menu A/B matches D3D11                                                        | CS `org-port/integration`                                |
| P3 W1: IBL, Screen Space/Terrain/Volumetric Shadows, Skylighting, Grass Collision, LLF remainder                 | Merged; A/B within noise, Grass Collision and LLF local shadows included            | CS `org-port/integration`                                |
| P3 W2: SSGI, Subsurface Scattering, Exponential Height Fog                                                       | Merged; fog and Subsurface A/B within noise (fog volumes read back equal)           | CS `org-port/integration`                                |
| DCLF graph textures (publish API, t30 proof)                                                                     | Merged; **open:** switching path mid-scene loses the device                         | CS `org-port/integration`                                |
| P3 W2: Dynamic Cubemaps, Cloud Shadows                                                                           | Merged; A/B within noise, DCLF reads both through the graph                         | CS `org-port/integration`                                |
| P3 W4: UnifiedWater flowmap (built on the CPU, no GPU pass)                                                      | Merged; same file hash on both paths                                                | CS `org-port/integration`                                |
| P3 W5: FX runtime parser and lowering                                                                            | Merged; execution not written yet                                                   | CS `org-port/integration`                                |
| P3 W3: HDR Display output, Screenshot readback, Upscaling's own passes                                           | Merged; A/B within noise, graph screenshots byte-identical to D3D11 copies          | CS `org-port/integration`                                |
| P3 W3: Post Processing framework and its effects                                                                 | Merged; six effect scenarios (bloom, DoF, exposure, flares, glare) A/B within noise | CS `org-port/integration`                                |
| P3 W4: Terrain Blending, TruePBR glints, Effects11 sun and volumetric rays                                       | Merged; A/B within noise, glints noise reads back equal                             | CS `org-port/integration`                                |
| P3 W5: FX runtime execution                                                                                      | Merged behind CS_ORG_FX; matches FX11 in both cells with recording reuse on         | CS `org-port/integration`                                |
| DCLF reads fog, IBL, Skylighting, Terrain/Volumetric Shadows as published resources                              | Merged; A/B within noise                                                            | CS `org-port/integration`                                |
| DCLF reads Terrain Blending's depths, Cloud Shadows' self shadow and the faces' inputs as published resources    | Merged; A/B within noise, no D3D11 export reads left                                | CS `org-port/integration`                                |
| DXVK: placeholder binds after external commands, interop layout order, relocation copies                         | Done; validation clean apart from a known kMAIN GENERAL layout                      | DXVK `org-port/p3-kept-layouts`, bumped on integration   |
| P3.5: consolidation (bugs, DCLF one rule, framework primitives, ports on them, submodule fixes)                  | Done; A/B within noise, validation clean, hot reload checked                        | CS `org-port/integration`                                |
| VRAM Budget: runs on the main thread, purges as pcb does                                                         | Done; the interior grid-detach crash is gone                                        | CS `fix/vram-budget-main-thread`, merged                 |
| P4.1-P4.7: tripwire handles and guest device, targets, textures, streaming, buffers, shaders and states, VRAM    | Merged and run in game; each lane alone and all together match the baseline         | CS `org-port/p4`                                         |
| P4 guest-phase order: every handle after the guest and the objects it wraps; CS readers of engine fields unwrap  | Done; 83 targets, 648 states and 70 queries get handles, shots match                | CS `org-port/p4`                                         |
| P4 verify: tour (menus, map, journal, race menu, load, Blackreach), VRAM totals, streaming retire fix            | Done; shots match, VRAM within 4% of the D3D11 path; SE deferred                    | CS `org-port/p4`                                         |
| P5 wave 1: constants capture, frame skeleton, sky, Bink, Scaleform subset, ImageSpace shader sources, slot rings | Merged and run; each matches its D3D11 reference (details below)                    | CS `org-port/p5`, ORG `org-port/p5-slots`                |
| P5 wave 2: menus probe, loading screen, ImageSpace chain, water, screen blood; crash fixes                       | Merged and run; each matches its D3D11 reference within run noise (details below)   | CS `org-port/p5`, BasicRHI `org-port/p5-crash-basicrhi` |
| P5.2: DCLF coverage (cloud-lit and falloff effects, shadow depth clamp, point lights without LLF, first person)   | Merged and run; each scenario against the engine's own draws (details below)        | CS `org-port/p5`                                         |
| P5.10 finish: the A8 glyph cache, the colour matrix filter, displays split around D3D11 parts                    | Merged and run; the part path runs with `CS_ORG_SCALEFORM_D3D11_PARTS`               | CS `org-port/p5`, BasicRHI (A8_UNorm, format remap)      |
| P5 review and its fix lanes: framework, families, engine, Scaleform passes, DCLF core                            | Merged; `p5-all.env` runs with every port on                                        | CS `org-port/p5`                                         |
| ORG: retire replaced statistics query pools after the in-stream lists that write them                            | Done; the device loss after in-session rebuilds                                     | ORG `org-port/rebuild-retire` (3100d0e)                  |
| P5 wave 3: frame slot budget 512, ImageSpace end-of-frame effects, shadow masks, water's published slots         | Merged and run (2026-11-11); details below                                          | CS `org-port/p5`, ORG `org-port/frame-slots` (9cbc8b3)   |
| P5.13 (the flush and the shader binds as tripwires)                                                              | Census and latch built (`CS_ORG_RESIDUE_CENSUS`, `CS_ORG_ENGINE_FLUSH_LATCH`); not flipped | CS `org-port/p5`                                         |
| P5 primary path: ports precompile at load, pipelines before use, no readiness fallback; latch allowed draws         | Merged and run (2026-11-11); census 40/frame settled; latch shots match with it off  | CS `org-port/p5`, ORG `org-port/fix-retire-2` (b6fb82a)  |
| Phase 5.5 (no engine fall-through): the gate before Phase 6                                                        | Started 2026-11-11; 5.5.0's compile-stall fixes merged                              | CS `org-port/p5`                                         |
| Guest frames without slots: shared command pools, run leases, one frame arena, only published recordings kept    | Shared pools merged (4.30 GB settled, from 9.75); leases and the arena built, not run yet | CS `org-port/frame-contexts`, ORG and BasicRHI `org-port/frame-contexts` |
| P6.0-P6.3: native mode (`CS_ORG_NATIVE=1`): BasicRHI device, native handles, native host, ORG present, startup errors | Built, not run (render-graph.md, "Native mode"; `native.env`) | CS `org-port/native-vk` |

**Root cause worth knowing:** a BasicRHI-created Vulkan device failed on AMD because the driver rejects acceleration
structures in a mutable descriptor list at any count. DXVK never enables acceleration structures, so the adopted device
never hit it. Acceleration-structure SRVs are now dropped from the descriptor-buffer model where this happens.

**Phases 3, 3.5 and 4 are done; Phase 5 runs in game** on `org-port/p5`, all but P5.13. Phase 4's tour (menus, map,
journal, race menu, a load, Blackreach) matches the baseline with every switch on, and VRAM read from ORG is within 4% of
the D3D11 path. SE 1.5.97 is deferred. Phase 5 wave 1, each against its D3D11 run:

- **P5.7 constants:** every engine constant-buffer use is captured into graph memory; a read-back check found 0 differences.
- **P5.1 skeleton:** the engine's nine clear and depth-copy sites run on the graph; no tripwire line from them is left.
- **P5.3 sky:** the main view and the reflection faces draw on the graph (mean diff 1.0-1.6). The cloud layers stay the
  engine's by default (`CS_ORG_SKY_CLOUDS`, off until a run confirms them; p5/sky.md, "Cloud layers").
- **P5.11 Bink:** a main-thread movie draws on the graph (165 of 166 frames), decoded frames identical. The startup logo
  plays on its own thread; with P5.12's `CS_ORG_LOADING_THREAD=1` it draws on the graph too (164 or 165 of 166 frames).
- **P5.10 Scaleform:** about half the displays drew on the graph in wave 1, shots match. The rest were loading-screen
  displays (P5.12), the glyph cache's A8 view and filters or render-to-texture; the P5.10 finish covers them (below).
- **ImageSpace sources:** all 124 slots have CS HLSL. The 82 checked ones build from it by default: the in-game A/B matched
  (2026-11-10), so `CS_IMAGESPACE_SOURCES` was removed. ISMap and the two TAA variants stay on vanilla's DXBC.

Phase 5 wave 2, each against its D3D11 run:

- **P5.8/P5.9 menus:** a probe for the menu 3D scenes and the map (`CS_ORG_MENU3D`, `CS_ORG_MAP`); shots match.
- **P5.12 loading screen:** the serving thread renders under `Renderer::Lock` as the render owner
  (`CS_ORG_LOADING_THREAD`); the Bink logo and the loading-screen Scaleform displays draw on the graph. Shots match.
- **P5.6 ImageSpace:** the HDR, blur and composite effects draw at the ImageSpace anchor (`CS_ORG_IMAGESPACE`); six shots
  match, Riverwood is within its run noise.
- **P5.4/P5.5 water and screen blood:** at the EngineDraws anchor (`CS_ORG_WATER`, `CS_ORG_BLOOD_SPLATTER`). Water matches
  within run noise; the blood draws, but the fight differs per run, so its shots are not comparable.

Crashes in the default-path runs, investigated with dumps and Ghidra:

- An access violation in the engine's navmesh search (`BSNavmeshInfoSearch`, ID 90542) on a freed object during a load.
  **Engine**; CS does not touch that code.
- Device loss in the water runs: the water mask, the water surface and the screen blood share an anchor, and a run that
  one client kept also recorded the others' passes with latch slots never written for that run. Each client's draws now
  go through a run gate (**ours**, fixed). A device loss now stops the graph cleanly, and DCLF's fail-stop names it.
- A pure virtual call on a destroyed shader resource view in DCLF's texture import thread. **Ours**, not reproduced
  since; open.
- Texture streaming registered a one-shot client per batch, about 250 graph rebuilds in a minute. Its windows are now
  made with their mips as initial data, without a pass: 54 rebuilds left, seven per epoch, one per load.
- The crash handler keeps its helper after a crash and reports a failed dump. First-chance faults only log (module and
  offset); the session's one crash dump is written by an unhandled exception filter, for the fault that ends the process.
  Before, the first first-chance access violation used the dump and held its thread for up to 120 s even when a handler
  caught it.

Phase 5.2 and the P5.10 finish, each against the engine's own draws:

- **P5.2 DCLF coverage:** Lighting-variant effects lit by the cloud light and falloff effects without a blending alpha
  property (`CS_DCLF_EFFECT_CLOUD_LIGHT`, `CS_DCLF_EFFECT_FALLOFF`); residue shadow views whose rasterizer state turns depth
  clipping off, drawn with depth clamped (`CS_DCLF_SHADOW_DEPTH_CLAMP`); point lights without Light Limit Fix, from a
  128-row frame table each draw picks from (`dclf-nollf-lights.env`); the precipitation mask without Skylighting
  (`dclf-noskylighting-precip.env`, with its parity); first person on by default (`CS_DCLF_FIRST_PERSON`,
  `dclf-first-person.env`); the particle manager's master particle systems behind `CS_DCLF_MASTER_PARTICLES` (off by
  default; `master-particles-*.env`, `blood-particles-*.env`). `CS_DISABLE_FEATURES` disables features for one run.
- **P5.10 finish:** the texture fallback was one texture, GFx's `A8_UNORM` glyph cache; BasicRHI now has `A8_UNorm` and a
  per-device format remap (`Device::QueryFormatSupport`), so text draws on the graph. The colour matrix filter is a graph
  draw. What the graph does not draw (other filters, render targets, line strips) is a D3D11 part between graph segments
  instead of the whole display (p5/scaleform.md). With every port on, 10,982 of 11,000 displays drew on the graph.

The Phase 5 review (2026-11-11) and its fix lanes:

- **Framework primitives** (render-graph.md, "Joining, running and withholding"): `FrameAnchors::Run` returns a
  `RunResult`, `CanRun` before withholding, `RequestJoin` as the one join gate (builds at DCLF's build point or at frame
  end), `ClientDesc::executed`, `ProgramCache`, `FrameStaging`, `RunSlots`, `MainTick`, `Util::ReasonLog`, and the frame
  slot budget logged at startup.
- **Families helper:** the sky, water and screen blood take the engine's draws through one shared family helper
  (`Core/Engine/Family`); the sky's cloud layers' occlusion is drawn into the later layers.
- **Engine registrations once:** the skeleton imports every target it clears and registers once at Setup; Bink keeps one
  client for the session; ImageSpace joins before it withholds; engine constants are captured on demand. A clear that
  registered again had rebuilt three epochs within 150 ms during a load.
- **Scaleform passes:** the Interface pass draws through a run gate, with `RunSlots` and `FrameStaging`; the client
  registers once with both surfaces and joins through `RequestJoin`; pipelines come from a `ProgramCache`.
- **DCLF core:** drawn roots retire and unheld roots are diffed; master particle observers run with the stats only;
  shadow point light rows go first; first person is left out when it cannot be drawn; DCLF stops after a build failure.
- **ORG's query-pool retirement** (`org-port/rebuild-retire`, 3100d0e): every new pass name replaced ORG's shared
  statistics query pool, and the old one was destroyed while DXVK's pending command buffer still wrote into it. That was
  the device loss after in-session rebuilds. A replaced pool now lives until the last list that recorded with it is
  released, and the retirement frontier covers values given out on the host's shared timelines.

Phase 5 wave 3 and the fixes after it (2026-11-11), each against its D3D11 run with VRAM Budget off:

- **Missing terrain after a save load:** DCLF refused every graph-owned `B8G8R8X8` engine texture (`CS_ORG_ENGINE_TEXTURES`):
  the A8 format-remap change compared the view's identity swizzle with BasicRHI's own alpha-to-one swizzle and called them
  different. Material pairs reading such a texture were skipped while DCLF still withheld their draws, so land quads with a
  BGRX diffuse drew nothing. An identity view now takes BasicRHI's swizzle, and a refused format logs once.
  `terrain-load.env` is a two-minute check.
- **Frame slots and ImageSpace:** ORG's 32-bit frame slots and a budget of 512 let ImageSpace run 13 times a frame. The
  end-of-frame effects (depth of field and its compute blurs, refraction, the radial blur, the snow copy) are their own
  scopes. A draw's viewport is read from the context after the state flush, so effects with their own viewport and the
  dynamic-resolution part of a target under upscaling are taken; the SAO camera-Z levels are mips of one import. The
  graph holds 53 ImageSpace passes in `imagespace-sao-ssr-org1.env`, against 23 before.
- **Family inputs:** water, sky and shadow masks list the published ids they can read when their client is made, and
  each build declares the registered ones, so a target published later no longer keeps a family on the engine for the
  session. Water now draws on the graph after the first frames (`p5-all-census-misc.env`), matching its D3D11 shots.
- **Shadow masks** (`CS_ORG_SHADOW_MASK`): the menu scenes' and the world's masks draw on the graph. The world's never
  registered (t5 was a stencil view the importer rejects; the ORG shader does not read it), then drew black: the filter-0
  path samples the shadow map through t3, whose view the graph saw as null, while t4 holds the same cascade array. The
  ORG build of Utility.hlsl now reads the map through t4's index (p5/shadow-mask.md, "The shadow map at t3 and t4"); the
  four shots' mean diffs went from 16-22 to 2.2-4.7 (NPCs). The engine's filter selector 3 falls back to SHADOWFILTER 0 on
  both paths, a pre-existing ShaderCache bug (dclf-validation.md, "Bugs found by parity").
- **Taken shadow masks feed their readers:** Volumetric Shadows, Exponential Height Fog and Skylighting captured the sun's
  cascades inside the engine's mask draw, which a taken draw never reaches. The family now hands them the view
  (`State::OnShadowMaskDraw`).
- **A hang in target-dump runs:** Upscaling's constant readback waited for the device to go idle mid-frame, a deadlock
  inside DXVK's stream. It now reads after the frames in flight. The hang watchdog also reported a dump it never wrote;
  it now names the helper's result (written, failed with the Win32 error, no helper, no answer).
- **Silent raster skips:** a raster program draw that could not be recorded now logs its reason once.
- **Bink:** the startup logo drew 166 of 166 frames on the graph (`loading-org1.env`).
- **Crashes seen, not ours:** VRAM Budget unloading cells under memory pressure, then a `coc` into an interior faulting in
  EngineFixes (the batches now disable VRAM Budget, see its known grid-detach crash); a pathing job jumping into heap
  memory through `zz_SkyrimEngineTelemetry.dll`'s zone hooks.

The primary-path pass and the host fixes (2026-11-11, `docs/development/p5/primary-path.md`):

- **The ports are the primary path.** Sky, water, screen blood and shadow masks compile their stages when the engine loads
  its shaders and keep the program keys they used in a manifest the next session builds at setup; ImageSpace keeps its
  shapes in a manifest and registers them once at setup; Scaleform, Bink, the skeleton and the Phase 3 ports build their
  programs at setup. The probe, settle, learning and join-polling machinery is deleted; a key never seen builds once on the
  render thread, logged. Settled Whiterun sends 40 engine draws a frame to D3D11, the same as before the rewrite; its shots
  match.
- **The flush latch lets vouched engine draws through** (`Flush::EngineDraws`, `Flush::VouchPass`): DCLF's unclaimed
  objects and each port's listed refusals apply the owed state and draw; anything else is skipped and logged. With the latch
  on, the tour's shots match the latch-off run; the skips left are DCLF objects in load windows (being closed).
- **A deadlock on the first world frame** (found with `p5-all-census-dclf-take.env`, every run): ORG's host thread freed
  retired objects in `PrepareTicket`; WDDM holds a free until the GPU is done with the memory, and that GPU work waited for
  a value only the host thread or the render thread (waiting on the host thread) would signal. Driver frees now run on a
  deleter thread, and the deletion frontier covers DCLF's CPU-frame timeline and the window between taking queue values and
  publishing them. Some frees still wait up to 1.2 s for the GPU; `ORG runtime: freeing ... took` names them.
- **Graph rebuilds during play:** 31.6 s of synchronous rebuilds in a tour fell to about 1 s. Each rebuild now logs its
  cause; DCLF's extensions build only into its own epochs, so one epoch's rebuild no longer gathers every extension; a new
  published id rebuilds only the graphs that read it; Terrain Shadows makes its heightmap textures once per session
  (sized for the largest installed heightmap: about 225 MB with Solstheim's, against 52 MB for Tamriel's before).
- **Hang tooling:** ORG and BasicRHI build with debug info; DCLF's frame-start timeout prints ORG's host state; the staging
  script now ships the current crash-dump helper (an old one answered every hang request without writing).

**Handoff, end of 2026-11-11.** `org-port/p5` (b3cd6afe7, ORG e7733ec) builds and runs the census and latch tours.
Not built yet: the last merge (`dclf-vouch-2`: interior shadow casters vouched at the shadow registration, SE 99932 / AE
106576; the latch tours now stop in Dragonsreach). Verify next: `latch-census.env` and `latch-census-off.env` (identical
commands, clear weather forced) — every latch window should end in `0 engine draws and dispatches skipped` and the shots
should match. Census scenarios force `fw 81a` before Whiterun: compare runs only with matching commands. The water's
geometry group 10 batch (AE 106584 +0x143) is vouched, not taken.

**Open items** (every engine draw that still reaches D3D11 is a Phase 5.5 item, not listed here):

- Without Light Limit Fix, a draw's point lights are selected per pixel from the 128-row table; selecting them once per
  object is dclf-roadmap.md 3.7.
- ORG's `StatisticsManager` still has a race on its shared query pool and no headroom in it: every new pass name replaces
  the pool, and the retirement fix only keeps the old one alive for the lists that recorded with it.
- Dragonsreach is darker with the ImageSpace port on (`imagespace-sao-ssr-org1.env`); likely the eye adaptation chain
  split between the graph and D3D11.
- With DCLF on, the owned frame stops presenting at the first menu or loading screen (P1; task P6.0).
- A latch census run after a shader change (2026-11-11): `CS_DISABLE_FEATURES` had turned the startup precompile off, every
  edited shader compiled on demand in the first world frames, and DCLF's 5 s snapshot wait failed the run. Fixed on
  `org-port/compile-stall` (merged): the precompile stays for features without shader defines, and the wait counts 5 s from
  the last compile or pipeline build (120 s cap).

**Phase 5 findings worth knowing:**

- In the guest phase each anchor is its own epoch in DXVK's stream, with a fixed ring of frame slots. An anchor run more
  than the ring holds in one frame (the skeleton's shadow clears) waited on work DXVK had not submitted: a deadlock. ORG's
  host first grew a listed epoch's ring (`GraphHost::Desc::epochRuns`); run leases (`org-port/frame-contexts`) replaced the
  rings: each run holds a lease of its own and waits for nothing. This goes away once ORG owns the frame and these are passes
  in one graph.
- A client registered while its anchor has no active client never joined the graph: the host builds a graph only when
  it is submitted, and Drawcall Limit Fix makes builds explicit at its per-world-frame build point. A joining client now
  runs the epoch, `FrameAnchors::InBuiltGraph` tells a client when it is in, and a blocking movie builds between frames.
- The engine keeps only kFRAMEBUFFER's RTV (the back buffer); `EngineTarget` takes the texture from it.
- Switching a client's source every batch rebuilds the graph each time; a few hundred rebuilds ran the device out of
  memory. Rebuilds during play are removed at their trigger; one that cannot be avoided builds asynchronously and switches
  in at the next snapshot, as DCLF's epochs do.
- SKSE tasks run on job threads, not the main thread; Tracy now listens on localhost only (no firewall prompt).

**Phase 4 findings worth knowing:**

- The engine's `Renderer::Init` makes the render targets (where CS's `State::Setup` runs) before the blend-table
  builder (where the guest device goes in), and the state tables and queries after it. Handles are installed at the
  end of Init, `BSGraphics::State` setup's call (SE 75445+0x310, AE 77226+0x9C3); see `p4/p4.1-tripwire.md`.
- Once target fields hold handles, any CS code that hands a field to DXVK's own context crashes in `d3d11_context.cpp`.
  `Tripwire::Unwrap` fixes it. The audit missed indirect indices (SSGI's `forwardRenderTargets[0]`) and the engine's
  shadow-state slot array (GrassOptimizations' material textures); both were found by crash and fixed.
- A loading screen presents from a job thread. Texture streaming's Present-anchor pass ran there, on the graph's upload
  instance off its owner thread. Anchors now run only on the main thread, latched at plugin load (CommonLib's
  `Main::threadID` gives a wrong value on AE 1.6.1170).

**Second root cause worth knowing:** after `vkCmdExecuteCommands`, AMD's Windows driver keeps the vertex and index
buffers the primary bound before it. When DXVK re-binds the same index buffer after the graph's secondaries, the driver
drops the bind as redundant and the next indexed draw reads the graph's index buffer. That made the frames after the
Tonemap epoch black (depth of field read garbage) in the FX and Post Processing chains. Validation saw nothing, since
the API use is correct; an RGP capture found it. DXVK now binds placeholder buffers after any external commands, so its
own binds always reach the driver. Details: "Hazard: bindings the driver keeps across an in-stream epoch" in
`render-graph.md`.

**Bugs found by in-game testing (fixed on `org-port/integration`):**

- VRAM Budget's interior step unloaded the worldspace 3D and detached the grid, then unloaded the old grid cells over later
  ticks. An actor in a detached cell got its 3D attached by the AI job, which read the null worldspace portal graph and
  crashed (SkyrimSE+0x2C4BD6). Its controller also ran on a job thread, not the main thread its comment claimed. It now
  runs after the frame jobs on the main thread and calls the engine's whole `pcb` for the interior step.
- A failed build of one graph epoch ended ORG's host thread, so every epoch stopped and DCLF waited until the GPU timed
  out (HDR Display's UI clear asked for a view its import lacked). A failure now stays with its epoch: that anchor's
  clients return to D3D11 and the rest runs on. Imports with a UAV now also publish the CPU-side UAV a clear needs.

- ORG's alias planner sized a texture with an authored mip chain as one mip, so placements overlapped (SSGI's working
  depth ran into its radiance). Placement now sizes textures from the description the backing creates them with.
- Green streaks on near geometry (Whiterun bridge railings at noon) came from transients of different epochs sharing
  alias memory. `CS_ORG_SSGI_DEBUG=<n>` (render-graph.md, "Switches") narrowed it down: gone with no aliasing (1), at
  full resolution (8), and with SSGI in a pool of its own (17). Every epoch's graph packed its transients from offset 0
  of one shared arena, and Volumetric Lighting's volume (epoch 34) covered SSGI's working depth, radiance and normals
  (epoch 15). Nothing orders two epochs' GPU work against each other: the alias ledger adds no wait on the same queue.
  Each epoch now has pools of its own (ORG). Also fixed on the way: an aliased texture's first-use barrier waited for
  nothing (TOP_OF_PIPE; now sync All, and a Vulkan discard keeps a before sync).
- ORG's descriptor manager made 2D views of 3D textures, so Skylighting's probe update wrote almost nothing readers saw.
  3D textures now get Texture3D views.
- A texture array's default view is its first slice: ports that read `Texture2DArray` (Skylighting's cascades, LLF's
  shadow cache, the fog's shadow maps) now ask for the whole-array view. This is a trap for every port (AGENT-BRIEF).
- Terrain Shadows uploaded its heightmap from an anchor commit, which async epochs cannot record: now a copy pass.
- The composite re-registered every frame (the engine alternates the normals target): both are imported once.
- ImGui's Overlay anchor was reserved after the graph fixed its epochs, and the DX11 backend's shutdown freed Win32's
  main-viewport data. Both fixed.
- DCLF imported a ported feature's D3D11 export as a resource of its own, so nothing ordered its reads after the graph's
  writes: Cloud Shadows came out wrong. DCLF now reads it as a published resource. Fog, IBL, Skylighting, Terrain and
  Volumetric Shadows now do the same.
- A client registered during an anchor's run is not in that run's epoch. Glints marked its one-shot fill done from the
  commit and the noise stayed zero; Screenshot lost its first capture. Both now count only work their pass prepared.
- DXVK moved a render target's layout change into the barriers at the start of its command list, ahead of the graph's
  earlier use of the image in its resting layout (undefined on AMD). It now keeps those changes in order for interop
  images. It also copied relocated interop images from their resting layout; the copy now uses the transfer layout.
- DCLF decided a frame texture's path from the producer's current bind, but resolves textures at the epoch, after the
  main sky has rebound t26. The decision now comes from the view the commit captured, and the passes declare a frame
  texture whenever its resource is published.
- The fog fell back to D3D11 in every interior because Cloud Shadows binds nothing there; the shader does not read it
  without the sky, so the input is now left unbound.

Diagnostics added on the way: `CS_ORG_ALIAS_LOG=1` (alias plans), `CS_SKYLIGHTING_READBACK=1` (probe volume means), and
the alias check now names the overlapping pair.

**Follow-ups found while building Phase 0:**

- BasicRHI's Vulkan backend ignores `ResourceDesc::castableFormats`, so sRGB mip generation cannot get its UNORM view.
- ORG's `PixelBuffer` and `ExternalTextureResource` always report one sample, and no `Texture2DMS` SRVs are made, so
  the MSAA resolve pass is untested on the GPU.
- Vulkan feature info reports UMA whenever a host-visible device-local memory type exists (resizable BAR).
- Vulkan `SetResidencyPriority` does nothing, so the spill pool's low priority has no effect there.
- BasicRHI has no top-level `enable_testing()`, so its smokes are not registered with ctest.
- `EvaluateBindingModels` still reports acceleration structures as usable before a device exists.
- ORG's `GraphHost` fixes its epoch order at creation, so frame anchors must be reserved before `Initialize`.
- ORG's `PersistentVulkanHostTests.Pipelines` hung once in a full ctest run and passed on two reruns.

## Context

CS on `org_integration` runs Skyrim's D3D11 under a forked DXVK (`extern/dxvk`), and OpenRenderGraph adopts DXVK's
`VkDevice` to run graph epochs inside DXVK's command stream (`docs/development/render-graph.md`). DCLF and LLF's
clustered culling already run on ORG. Everything else still goes through `globals::d3d::context`, and the engine
owns the device, swap chain, Present, every render target and texture, state binding, ImageSpace, Scaleform, Bink and
the menu 3D scenes. DCLF itself gets all of its inputs as D3D11 objects imported through about 50 private DXVK
exports.

Goal: every GPU command, engine and CS, is an ORG pass, so BasicRHI can run it on Vulkan or D3D12 and DXVK is not
loaded. Decisions (2026-11-09):

| Decision               | Choice                                                                                                                                                             |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| Engine strategy        | **Native replacement only.** No D3D11-API translation layer. Each engine subsystem becomes ORG passes.                                                             |
| Backend order          | **Vulkan first.** D3D12 follows behind a parity gate.                                                                                                              |
| D3D11 paths            | **Kept until cutover** behind switches, then deleted.                                                                                                              |
| Early step             | **Ownership inversion:** BasicRHI creates the device and swap chain, ORG owns the frame and Present, DXVK becomes a guest.                                         |
| Engine handle fields   | **Tripwire handles:** IUnknown-shaped objects whose refcount works and whose every other method logs the caller and fails.                                         |
| Third-party D3D11 mods | **The DXVK build stays the default.** The full port is a development option. A larger ORG extension API comes later; record what it needs in a notes doc as we go. |
| Effects11              | **Write an ORG FX runtime** (techniques parsed, compiled with DXC) to replace FX11.                                                                                |
| Deliverable            | This plan, plus `docs/development/org-full-port.md` (roadmap style, like `dclf-roadmap.md`) after approval.                                                        |

### Where things stand (from the 2026-11-09 survey)

**Infrastructure:**

- BasicRHI has full D3D12 (`rhi_dx12.cpp`, about 11k lines) and Vulkan (`rhi_vulkan.cpp`, about 14k lines) backends, but CS forces D3D12 off (`cmake/RenderGraph.cmake:34-35`).
- ORG already supports a frame it owns end to end: `RenderGraph::SetSwapchainResource`/`SetPresentPass`, `PassBuilder::Present`. BasicRenderer in SARP runs it on D3D12, Vulkan and both (`SARP/BasicRenderer/.../DeviceManager.cpp`, `RendererGraphConstruction.cpp`).
- BasicRHI gaps against what the engine uses:
    - **Pipeline state:** no stencil (`rhi.h:1795` `// TODO: stencil`); no blend factor, `SrcAlphaSat`, dual-source blending, sample mask or depth clip; no dynamic viewport or scissor; no MSAA resolve or GenerateMips.
    - **Vulkan:** no `VK_EXT_memory_budget` (`rhi_vulkan.cpp:3963`); the CPU waits for the GPU on every present (`sc_present`, `:7397`); vsync is ignored.
    - **Present, both backends:** no HDR colour space or metadata, and no latency control.
    - **ORG:** `GraphicsPipelineDesc` has no input layout, and there is no dynamic-state API in `PassRecordContext`.

**CS:**

- About 20 features run their own passes: SSGI, SSS, DynamicCubemaps, ExpHeightFog, VolumetricShadows, Skylighting, TerrainShadows, IBL, GrassCollision, ScreenSpaceShadows, PostProcessing (13 sub-effects), HDRDisplay, Upscaling, Effects11, Screenshot, LLF's remainder, TerrainBlending, GrassOptimizations, CloudShadows and the deferred composite.
- About 15 only change the engine's shaders and bind PS slots: ExtendedMaterials, Skin, WetnessEffects and others.
- The framework still uses D3D11 in `Buffer.h`, `Util::CompileShader` (FXC), `GpuEventTimers`/`Profiler`, ImGui DX11, and the texture streaming and VRAM tooling (DXVK only).

**Engine (Ghidra, SE 1.5.97 and AE 1.6.1170 laid out the same):**

- The context global has 357 references in 110 functions. Almost all are in the `BSGraphics::Renderer` block (IDs 75445-75654), the shader `Setup*` functions (constant buffer Map/Unmap) and about 40 ImageSpace setup functions.
- `SetDirtyStates` (75580/77386) is the only state flush, `SetShader` (75550) and `SetPixelShader` (75555) the only shader binds, and `DispatchCSShader` (75532) the only dispatch.
- Scaleform (GFx 3.x) keeps private device and context pointers in its renderer object (vtable SE 0x1417dbf90).
- Bink reaches D3D11 only through the Renderer's texture lock and unlock (75514/75515).
- Present has three callers: the main frame (35556), the loading-screen loop on a job thread (38085), and Bink (87890).

**DXVK:**

- It can already run on a device someone else created (`DxvkDeviceImportInfo`, `DxvkAdapter::importDevice`, used by `D3D11On12CreateDevice` in `extern/dxvk/src/d3d11/d3d11_main.cpp:371`). That is what makes the inversion cheap.

## Target architecture

1. **BasicRHI owns the device:** `rhi::CreateVulkanDevice`, later `CreateD3D12Device`, with the Streamline interposer.
2. **ORG owns the frame:** one `org::Runtime` with persistent graphs per engine phase, ending in a present pass on a BasicRHI swap chain.
3. **The engine's CPU code still runs** (scene walk, ImageSpace effect list, menus). Its D3D11 choke points stop issuing work: they latch state into ORG passes, as DCLF's `PassCapture` withholds passes today.
4. **Engine resources are ORG resources.** The engine's `ID3D11*` fields hold tripwire handles that point at ORG records.
5. **One shader path:** HLSL compiled by DXC through ORGModuleServices to SPIR-V or DXIL, with bindless bindings (the existing `DCLF_BINDLESS`/`LLF_ORG_BINDLESS` ABI).
6. **Until cutover the DXVK guest exists** as a "legacy" stage inside ORG's frame. The DXVK-free build is the moment that stage is empty.

Switch names (suggested):

- `CS_ORG_OWNED_FRAME`: the inversion.
- `CS_ORG_NATIVE`: no DXVK, Vulkan.
- `CS_ORG_BACKEND=vulkan|d3d12`.

The default stays DXVK as host until the user says otherwise.

## Execution model

### Who does what

- **Subagents implement.** Each phase's independent tasks go to parallel subagents, each in its own git worktree (`isolation: "worktree"`) on its own branch cut from `org_integration`.
- **The main session builds and validates.** It runs `BuildDevFast.bat`, in-game runs, `tools/cs-ab` A/B and screenshot comparisons, then reviews and merges. Implementing agents do not launch the game or benchmarks while two or more of them are running. An agent working alone may run its own build.

### The brief each agent gets

- The task row from this plan.
- The RE section of `engine-d3d11-surface.md` it depends on.
- The reuse list for its area (LLF template, `ComputeProgram`, `DxvkImageImport`, DCLF `PassDef`).
- Its file boundary.
- What "done" means: it compiles, has a switch, and has a fallback.

It returns:

- a summary;
- the branch name;
- anything it found that belongs in `org-extension-api-notes.md`.

### Lanes per phase

These are the units that run in parallel:

| Phase | Lanes                                                                                                                                                                                                                                                                                                                                                             |
| ----- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| R     | 3 RE agents: {R1, R2, R3, R11}, {R4, R5, R10}, {R6, R7, R8, R9}. R12 goes to whichever finishes first. Read only, so they can run at the same time as anything else.                                                                                                                                                                                              |
| P0    | BasicRHI pipeline state {P0.1-P0.3}; dynamic state and ORG pipeline desc {P0.4, P0.5}; present, HDR and latency {P0.8-P0.10}; small utilities {P0.6, P0.7}; diagnostics {P0.11}; caching and allocator {P0.12, P0.13}. Each lane changes different files in `rhi_vulkan.cpp`/`rhi_dx12.cpp`, so agents name the functions they touch and the main session merges. |
| P1    | DXVK fork export {P1.1, P1.2}; ORG present and swap chain {P1.3, P1.5}; ImGui backend {P1.6}; Streamline device {P1.4} after P1.1. The main session does the integration (P1.7).                                                                                                                                                                                  |
| P2    | {P2.1}, {P2.2, P2.4}, {P2.3}, {P2.5}, {P2.6, P2.7}, all parallel. P2.2 lands first, because P3 builds on its API.                                                                                                                                                                                                                                                 |
| P3    | One agent per feature within a wave (W1 is 7 agents, run 3-4 at a time). PostProcessing is split by sub-effect after the shared raster helper. W5 (FX runtime) is one long-running agent with its own design doc.                                                                                                                                                 |
| P4    | {P4.1} first, then {P4.2}, {P4.3, P4.4}, {P4.5}, {P4.6}, {P4.7} in parallel.                                                                                                                                                                                                                                                                                      |
| P5    | Independent families in parallel: sky, water, BloodSplatter, ImageSpace (split by effect group), menu 3D, map, Scaleform, Bink. P5.1, P5.7, P5.12 and P5.13 stay serial in the main session, because they touch the frame skeleton.                                                                                                                               |

## Keeping the work separate and clean

The aim is that each task can later become its own PR against `dev` without dragging other work with it.

### Separate code from existing features

- New ORG code goes in new files beside the D3D11 code, not interleaved with it. For a feature this is `src/Features/<Name>/ORG<Name>.{h,cpp}`, following LLF's `LightLimitFix/ORGLightCulling.cpp`.
- The existing feature file gains only one seam: a call that picks the ORG path or the D3D11 path, behind the feature's switch.
- Engine takeover code goes in its own tree, `src/RenderGraph/Engine/` (resources, handles, swap chain, frame anchors), and stays out of `Hooks.cpp` and `Deferred.cpp`. Those files get only the hook registrations.
- Changes to BasicRHI, ORG and DXVK stay in their submodules. A CS branch only bumps the pointer.

### One task, one branch

- Each task row is one branch with its own commit series and conventional-commit titles: `feat(org): ...`, `refactor(org): ...`, `fix(...)`.
- A branch does not mix submodule bumps, feature ports and framework changes, unless the task needs them together. If it does, the bump is a separate first commit.
- Shared prerequisites (P2.2's anchor API, P4.1's handles) land before their users, so later branches depend only on merged work.

### Clean as we go

- Reuse what already exists rather than adding parallel helpers:
    - `Util::SetResourceName` and the ORG resource naming;
    - `ComputeProgram`, `LatchBlock`, `DxvkImageImport`, `PrefixSum`;
    - DCLF's `PassDef` and `FrameBindings`.
- No duplicated wrappers, no TODO/FIXME placeholders, Doxygen comments on public methods, and comments in short plain sentences.
- Use graph primitives instead of hand-written bookkeeping.
- The main session reviews each branch for these points before merging.
- When a D3D11 path is finally deleted (P8.1), the deletion is a separate commit from the port.

---

## Phase R: upfront RE (Ghidra, read only, can start now)

**Server:** http://127.0.0.1:8089, `program=/skyrim/se/SkyrimSE.exe.unpacked.exe` and `/skyrim/ae/...`.

- From Git Bash, `export MSYS_NO_PATHCONV=1`.
- Working endpoints: `decompile_function`, `batch_decompile`, `disassemble_function`, `get_xrefs_to` (RAM addresses only), `get_function_xrefs?name=` (for imports), `get_function_callers`, `get_bulk_xrefs`, `search_functions_by_name?name_pattern=`, `search_instructions`, `read_memory`, `list_strings?filter=`.
- The registry is in `BethesdaGhidraScripts/ghidra-mcp/src/main/java/com/xebyte/core/EndpointRegistry.java`.
- Address libraries: `BethesdaGhidraScripts/addresslibrary/sse`.

**Output:** `docs/development/engine-d3d11-surface.md` gets one section per task. Each section has SE and AE IDs, signatures, callers, and the state each function reads and writes. Rename functions in Ghidra as they are confirmed.

**Running it:** the tasks are independent. Run R1-R4 and R5-R8 as two or three parallel subagents. Each gets the endpoint notes above and writes only its own doc section.

| #   | Task                                                                    | What to produce                                                                                                                                                                                                                                                                          | Feeds                           |
| --- | ----------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------- |
| R1  | Name the draw functions 75477-75500 (SE) and their AE twins 77263-77290 | Each one's signature and geometry kind (TriShape, dynamic, particle, line, instanced); which shaders and passes reach each; the inline input layouts built by 75490/75491                                                                                                                | P5 residue draws, P4 buffers    |
| R2  | `SetDirtyStates` (75580/77386) and `RendererShadowState`                | The full dirty-bit table and every state it flushes: VS/PS/CS SRVs and samplers, UAVs, RTs, blend/depth/stencil/raster, viewports, topology, input layout, alpha-test CB. Note any GS/HS/DS use. Stencil ops per pass (which passes use stencil: water? decals? the ImageSpace masks?)   | P0 state coverage, P5           |
| R3  | Constant-buffer contract                                                | For each shader type: its constant groups (`constantBuffers[level]`, IDs 52475x-52476x); the about 65 Map/Unmap sites in `Setup*` and ImageSpace setups; which layouts DCLF's `ConstantMirror` already decodes                                                                           | P5 constant path                |
| R4  | ImageSpaceManager                                                       | The effect list (`SetupEffects` 0x141297340), render functions 99023-99025, the render-target ping-pong, every `BSImagespaceShader` with a CS HLSL file and every one without (the vanilla-only, DXBC-only list), and the order and conditions for interior, exterior, menus and the map | P5 ImageSpace, shader authoring |
| R5  | Scaleform renderer                                                      | The full vtable at SE 0x1417dbf90 (GFx 3.x `GRenderer`): textures, vertex/index caches, mask/stencil use, blend modes, render-to-texture (menus with 3D), the font cache, and its shaders (embedded DXBC in the exe: extract them)                                                       | P5 UI                           |
| R6  | Threads and lifetime                                                    | Every D3D11 user off the render thread: the loading screen (38085 under `Renderer::Lock`), loader threads creating textures and buffers, Bink. Buffer creation (75592/75593) and ownership of `BSGraphics::TriShape`/vertex buffers. The dynamic ring (75484, query fence)               | P4 resource takeover            |
| R7  | Swap chain and window                                                   | `InitD3D` (75595/77226), `D3D11CreateDeviceAndSwapChain` (75589), `CreateSwapChain` (75457), resize, `KillWindow`, the DXGI mode enumeration, fullscreen handling                                                                                                                        | P1 inversion                    |
| R8  | Engine work around DCLF epochs                                          | Shadow-target bind and clears (`BSShadowLight::RenderShadowmap`, AE 0x1414f0cf0), reflection-face bind (AE 0x1414ed920), the `Main::RenderDepth` copy (100421/107139), `BSShaderAccumulator::FinishAccumulating` copies                                                                  | P5 first engine passes          |
| R9  | Menu 3D and map                                                         | UI3DSceneManager (51855/52727), `Inventory3DManager::Render` (0x140887750), RaceMenu's 3D, the local map (`LocalMapCamera`, `BSImagespaceShaderLocalMap` 0x1412b6110), and whether DCLF claims their geometry                                                                            | P5 menus                        |
| R10 | Bink and textures                                                       | `BinkDoFrame` flow (87911), plane textures, YUV conversion shader; AE 90260/90275/90276                                                                                                                                                                                                  | P5 Bink                         |
| R11 | Device-global readers                                                   | The 32 users of the device global (SE 0x143025f08) and the remaining shader, layout, query and state-object creation functions (75547, 75551, 75556, 75583, 75590...) with AE twins                                                                                                      | P4 tripwire coverage            |
| R12 | AE 1.7.104 spot check                                                   | Spot-check the same IDs on 1.7.104 if an exe is available. Otherwise mark them unverified                                                                                                                                                                                                | all                             |

**Also from RE:** extract the vanilla shaders that have no CS source from the game's shader archives (the ImageSpace ones and Scaleform's), and disassemble them with `fxc /dumpbin`. This is what makes "write HLSL for every DXBC-only shader" a task list instead of a surprise.

---

## Phase 0: ORG/BasicRHI capability work (submodules `extern/BasicRHI`, `extern/OpenRenderGraph`)

These repos are separate, so changes land there first, then go into CS through a submodule bump. SARP/BasicRenderer must keep building, so run its tests.

| #     | Task                                                                                                                                                                               | Notes                                                                                                                                                                                                                                                      | Size |
| ----- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---- |
| P0.1  | Stencil in `DepthStencilState`: front/back ops, read/write masks; `SetStencilRef` in the record context                                                                            | D3D12 and Vulkan both. The engine uses stencil (R2 says where)                                                                                                                                                                                             | M    |
| P0.2  | Blend: `BlendFactor`/`InvBlendFactor` with `SetBlendFactor`, `SrcAlphaSat`, dual-source `Src1*`, sample mask, logic op (Vulkan only, gated)                                        |                                                                                                                                                                                                                                                            | S-M  |
| P0.3  | Rasterizer: depth clip enable (`depthClampEnable` / `DepthClipEnable`)                                                                                                             |                                                                                                                                                                                                                                                            | S    |
| P0.4  | Dynamic viewport and scissor in `PassRecordContext` (`SetViewports`, `SetScissors`, several viewports)                                                                             | Engine draws change the scissor per draw (Scaleform, ImageSpace)                                                                                                                                                                                           | M    |
| P0.5  | Input layout in ORG's `GraphicsPipelineDesc` (BasicRHI already has it). Pipeline selection at record time from a permutation cache (the "not covered" item in `docs/pipelines.md`) | Engine permutations are picked per draw                                                                                                                                                                                                                    | M-L  |
| P0.6  | GenerateMips (compute, ORG utility pass) and MSAA resolve                                                                                                                          | DynamicCubemaps uses `GenerateMips`                                                                                                                                                                                                                        | S    |
| P0.7  | Vulkan memory budget: `VK_EXT_memory_budget` in `d_queryVideoMemoryInfo`                                                                                                           | Replaces `dxvkGetMemoryHeapReports`. Keep the AMD clamp caveat                                                                                                                                                                                             | S    |
| P0.8  | Vulkan present                                                                                                                                                                     | Remove the host `vkWaitSemaphores` in `sc_present` (let `Queue::Submit` signal binary semaphores); honour vsync and present-mode switching; fix the swapchain-image UNDEFINED VUID; `swapchain_maintenance1`; `present_id`/`present_wait`                  | M    |
| P0.9  | HDR output                                                                                                                                                                         | Choose the colour space (HDR10 PQ, scRGB); `vkSetHdrMetadataEXT` / `SetColorSpace1` + `SetHDRMetaData`. NVIDIA's profile needs the instance's engine name to be `"DXVK_HDR"` (`extern/dxvk/FORK.md`): carry that trick into BasicRHI's `VkApplicationInfo` | M    |
| P0.10 | Frame latency                                                                                                                                                                      | D3D12: the waitable object and `SetMaximumFrameLatency`. Vulkan: present_wait. A host-owned queue-depth policy to replace `dxvkSetPresentQueueDepth`/`SyncPresent`/`TearingPreference`                                                                     | M    |
| P0.11 | Crash diagnostics on native Vulkan                                                                                                                                                 | Checkpoints or `VK_NV_device_diagnostic_checkpoints` / `VK_AMD_buffer_marker`, `VK_EXT_device_fault`, Aftermath markers. Replaces `DXVK_DEBUG=hang` (every RX 6600 run needs this)                                                                         | M    |
| P0.12 | Pipeline cache on disk for Vulkan and D3D12                                                                                                                                        | Cold SPIR-V compiles took up to 109 s per PSO in SARP; engine permutations number over 1,000                                                                                                                                                               | M    |
| P0.13 | VRAM attribution                                                                                                                                                                   | Owner labels, spill pool and defrag in BasicRHI's allocator (replaces `dxvkPushResourceOwner`, `SetResourcesSpillable`, `ForceResourcePlacement`, `SetDefragPressurePercent`)                                                                              | M-L  |

**Verify:** BasicRHI and ORG unit tests; SARP benchmark; a stencil/blend sample in ORG `samples/` on both backends with validation layers.

---

## Phase 1: ownership inversion (DXVK as guest)

| #    | Task                                                                | Detail                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| ---- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P1.1 | A DXVK fork export to create a D3D11 device on a device it is given | Model it on `D3D11On12CreateDevice`'s `DxvkInstanceImportInfo`/`DxvkDeviceImportInfo` path. BasicRHI creates the instance and device with DXVK's required features and extensions added. `dxvkRequestDeviceFeatures` turns around: DXVK _reports_ what it needs. Queue assignment: DXVK gets the graphics queue plus its transfer queue; ORG keeps compute and copy. This replaces `AdoptVulkanDevice` in `RenderGraphRuntime::Initialize`. |
| P1.2 | Headless guest swap chain                                           | DXVK's DXGI swap chain for the game stops presenting. Its back buffer is an ORG-owned image (imported into DXVK, or DXVK images imported into ORG as now), and `Present` becomes "frame done" for the guest stream. Hook point: `IDXGISwapChain::Present` (`Hooks.cpp:487-600`). The loading-screen loop (38085) and Bink (87890) presents go through the same path.                                                                        |
| P1.3 | ORG present pass                                                    | A BasicRHI swap chain on the game's HWND (R7), `SetSwapchainResource` + `SetPresentPass`, created from `DxvkLoader` at `InitD3D`. Resize and fullscreen follow the engine's window events. Fake fullscreen stays on (the FSR-FG alt-tab deadlock in FORK.md).                                                                                                                                                                               |
| P1.4 | Streamline on BasicRHI's device                                     | `CreateVulkanDevice(..., enableStreamlineInterposer=true)`. Move the frame-generation present ownership (`FrameGenController.cpp`, `dxvkSetSyncPresent`, queue depth, recreate) onto P0.10's policy. Re-check SARP's "Streamline on Vulkan corrupts the heap" first, since that is the largest risk here. The DXVK interop command-buffer ring in `Upscaling/DXVKInterop.cpp` stays while the inputs are DXVK images.                       |
| P1.5 | HDRDisplay on the ORG swap chain                                    | `HDROutputCS` writes the ORG back buffer instead of `CopyResource` to DXVK's. HDR surface state comes from P0.9 instead of `dxvkGetPresenterSurfaceState`.                                                                                                                                                                                                                                                                                  |
| P1.6 | ImGui to ORG                                                        | An `imgui_impl_basicrhi` render pass in the present stage. Replace `imgui_impl_dx11` in `Menu.cpp`, `Menu/OverlayRenderer.cpp`, `Menu/ThemeManager.cpp`; move IconLoader, CursorLoader, `Utils/UI.cpp` and the CSEditor textures to ORG textures. Overlays then sit above the guest's output and survive the cutover.                                                                                                                       |
| P1.7 | Frame structure                                                     | `GraphHost`'s epochs keep running inside DXVK's stream through external commands, which works today. The ORG frame tail (HDR, ImGui, present) waits on the guest's last submission. Record which interop features become unnecessary as each later phase lands: segment splits, queue boundaries, home states.                                                                                                                              |

**Done when:** with `CS_ORG_OWNED_FRAME=1`, a tour in Whiterun and Riverwood on the RX 6600 matches the DXVK host build in screenshots (`dclf:shot`) and frame time (one sample per version). Also check HDR on and off, a loading screen, a Bink logo, menus, and alt-tab. DLSS-G or FSR-FG needs foreground, and DLSS-G needs an NVIDIA card. NVIDIA-only checks are gates for whenever an NVIDIA card is available.

---

## Phase 2: the CS framework on ORG

| #    | Task                                                                   | Detail                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ---- | ---------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P2.1 | Graph-backed resource wrappers                                         | Give `src/Buffer.h` (`Texture2D`, `Buffer`, `ConstantBuffer`, `StructuredBuffer`...) graph-backed twins with the same names (the naming convention carries over to ORG resource names). While a D3D11 reader remains, `RenderGraphRuntime::WrapBuffer` (`dxvkCreateBufferFromVkBuffer`) exposes them; add an image equivalent to the DXVK fork.                                                                                                                                                                                           |
| P2.2 | A general feature-pass API                                             | Today each new feature needs a `Segment` enum entry and an epoch-order edit (`RenderGraphRuntime.h:75-125`). Make the frame's stages into named anchors (ReflectionsPrepass, EarlyPrepass, Prepass, DeferredPasses, Tonemap, PostProcessing, Present) that features attach `ExternalPassDesc`s to. Base it on the LLF template (`LightLimitFix/ORGLightCulling.cpp`: `TypedRenderGraphPass`, `IRenderGraphExtension`, `LatchBlock`) and DCLF's `Api/Passes.h` `PassDef`. Write each new anchor's ordering rule into the notes doc (P8.4). |
| P2.3 | Shader compile                                                         | Move `Util::CompileShader` (FXC `*_5_0`, `Utils/D3D.cpp:206`) for CS's own passes to `ComputeProgram::Load` / the ORGModuleServices shader store (DXC, SPIR-V now, DXIL later). Feature HLSL moves to the bindless ABI (`OrgBindless.hlsli`).                                                                                                                                                                                                                                                                                             |
| P2.4 | Raster passes                                                          | A fullscreen VS/PS helper on ORG render passes, to replace `PostProcessing/RasterPass.{h,cpp}` and the PS passes in Upscaling and Effects11.                                                                                                                                                                                                                                                                                                                                                                                              |
| P2.5 | GPU timers                                                             | Move `GpuEventTimers.cpp` and `Profiler.cpp` (D3D11 timestamp queries) to ORG statistics and query pools; move the GPU-load CS. Replace `dxvkGetSubmissionCounter`/submission trace (`GpuIdleTrace.cpp`) with ORG's queue timelines.                                                                                                                                                                                                                                                                                                      |
| P2.6 | Shared constant buffers                                                | Make `PermutationCB`, `SharedDataCB` and `FeatureDataCB` (`State.cpp:1296`) graph uploads, readable by graph passes as bindless constants. The D3D11 binds at PS b4-6 (`VanillaBinding.cpp`) stay for legacy draws until P5.                                                                                                                                                                                                                                                                                                              |
| P2.7 | The deferred composite (`Deferred.cpp:309-428`, `DeferredCompositeCS`) | Becomes an ORG compute pass on imported GBuffer targets (DCLF already imports them, `Core/Render/Resources/Resources.cpp:494-528`). The GBuffer render-target table entries stay engine-created until P4.                                                                                                                                                                                                                                                                                                                                 |

**Verify:** `BuildDevFast.bat` builds. Composite output in screenshots is identical with `CS_ORG_COMPOSITE=0/1`. GPU timer totals match.

---

## Phase 3: CS feature ports

**Rules for every port:**

- The D3D11 path stays behind a per-feature switch until cutover; on shader failure the feature falls back to D3D11 or disables cleanly (`impl.reset()` pattern).
- Resources opt into aliasing where they are transient (`docs/development/org-aliasing-port-priorities.md`).
- DCLF's matching integration (`DrawcallLimitFix/Integrations/<Feature>`) switches from an imported SRV to the graph resource.
- If the feature also binds PS slots for engine draws, keep exporting it to D3D11 (`WrapBuffer`/image wrap) until P5 removes those draws.
- Bump the feature `.ini` only if settings change.

**Waves** (ordered by dependency, then by memory saved):

| Wave                        | Features                                                                                                                                                                                                                                                                                                                                                                                                 | Notes                                                         |
| --------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------- |
| W1: small compute producers | IBL (2 CS), ScreenSpaceShadows, TerrainShadows (1 CS, persistent heightmap), Skylighting probes (`UpdateProbesCS`), GrassCollision, VolumetricShadows (6 CS; hooked at the RenderShadowmask draw in `State::Draw`), LLF's remainder (lights buffer, local shadow copy `LocalShadowCopyCS`, fallback culling)                                                                                             | Each is a few passes; outputs already go to DCLF integrations |
| W2: lighting                | ExponentialHeightFog (5-10 CS, history copies, LLF lists), SSGI (7 CS, history split for aliasing), SSS (2-3 CS), DynamicCubemaps (about 9 CS + GenerateMips + BC6H, 3-frame state machine), CloudShadows (4th MRT on the sky's cubemap draws: keep the engine draw until P5 sky, port the propagation copies now)                                                                                       | SSGI/SSS/composite order inside the DeferredPasses anchor     |
| W3: post and output         | PostProcessing (13 sub-effects, about 45 dispatch sites + raster passes; about 1.2 GB aliasable at 4K), HDRDisplay remainder, Screenshot (staging readback through ORG readback), Upscaling's own passes (`UpscaleDepth` 2 PS, `CopyHudlessColor`, the copy into kMAIN), with Streamline inputs as graph resources. When no DXVK images remain, `SeparateFromGraphEpochs()` and the DXVK command ring go | Largest single port; split by sub-effect                      |
| W4: engine-coupled          | TerrainBlending (re-issues terrain depth passes: becomes a DCLF terrain depth variant plus the `DepthBlend` CS), UnifiedWater flowmap (was a D3D11 deferred context; now a CPU copy, no pass), TruePBR glints noise (setup CS), Effects11 volumetric/sun rays (raymarch PS, 2 blur CS, apply PS)                                                                                                         |                                                               |
| W5: the FX runtime          | Replace FX11 (`Effects11/Effects/Effect.cpp`, `D3DX11CreateEffectFromMemory`): an `.fx` parser (techniques, passes, annotations, UI variables, render-target and texture declarations), DXC compile per pass, execution as ORG passes in the Tonemap anchor. Test against the presets Effects11 ships with. Keep FX11 behind the switch                                                                  | L; its own design doc first                                   |

**Not ported:** GrassOptimizations. Its HiZ, culling and indirect draws drive the engine's grass, which DCLF's grass family already replaces. Its D3D11 path stays until cutover, then is deleted.

**Shader-injection features:** ExtendedMaterials, Skin, Wetness, WaterEffects, HairSpecular, RainRefraction, TerrainHelper, LinearLighting, LODBlending, TerrainVariation, HorizonFix, InteriorSun, SkySync, GrassLighting, ExtendedTranslucency, VolumetricLighting. They have no passes, so they move with the engine shader they change:

- Their texture binds become `FrameBindings` entries (as DCLF already does for its families).
- They are done when every draw family that uses them is ORG.
- VolumetricLighting's replacement compute shaders move with ImageSpace (P5.6).

**Verify each port:** screenshots on/off by switch at the standard cells, the feature's GPU time from ORG statistics, VRAM from `dclf:vram-json`, a hot-reload test, and a forced shader-failure test.

---

## Phase 3.5: consolidation before engine resources

A review of everything through Phase 3 (2026-11-10, six read-only passes) found the core design sound: graph-backed twins,
anchors with immutable snapshots, the PostProcessing chain builder, the FX parser and lowering, the engine tree under
`src/RenderGraph/Engine`. The weak part is consistency: about 20 agents solved the same few problems in their own ways,
so the ports carry copied workarounds, several setup patterns and leftover debugging. Phase 4 builds directly on these
parts, so they are settled first.

**Lanes** (each its own branch from `org-port/integration`, merged into `org-port/p35`):

| Lane           | Scope                                                                                  |
| -------------- | -------------------------------------------------------------------------------------- |
| L1: bugs       | The local bugs below, CS side, outside DCLF and the framework files.                   |
| L2: DCLF       | One graph-texture rule, declare by registration, retire the stand-in test.             |
| L3: framework  | Shared primitives and lifetime in `src/RenderGraph`, define parity, one switch helper. |
| L4: ports      | Move every port onto L3's primitives and one setup template. Starts after L3 merges.   |
| L5: submodules | BasicRHI fixes, DXVK `FORK.md`.                                                        |

Diagnostic removal goes with whichever lane owns the file. Scenario files are pruned at the end.

### L1: bugs

- **FX runtime:**
    - `Invalidate` leaves the running shape, so the next commit can dereference a null FX11 effect: give each frame an immutable per-effect value snapshot;
    - one stale frame when the output target changes;
    - a failed runtime stays registered;
    - the generated vertex texcoord has w = 0 (D3D11 gives 1);
    - `#pragma pack_matrix` silently transposes;
    - a failed pass re-plans every frame;
    - remove the black-frame readbacks (`CS_ORG_FX_READBACK`, `CS_ORG_FX_INPUT`).
- **Post Processing:**
    - `ran` survives `onFailed`;
    - hot reload never clears the graph programs (also Upscaling, Terrain Blending, glints);
    - the D3D11 textures are created beside the graph chain, so its VRAM saving is lost: create them only on fallback;
    - `CS_PP_ONLY` overwrites saved settings.
- **Glints:** the noise is bound before it is filled.
- **LLF local shadows and Grass Collision:** consume the staged frame in the commit, so a frame that staged nothing replays nothing.
- **LLF:** the ORG cache stays R32 where R16 does.
- **ImGui:** a target that cannot be imported must fall back to D3D11, not leave the menu invisible.
- **UnifiedWater:** the CPU flowmap replaces the deferred-context code (no switch, no "ORG" name); drop the whole-file hash.

### L2: DCLF

- **One rule:** a commit reads the graph resource when the view it reads is a declared, published export. The colour commit, the faces and grass use the same helper.
    - Delete `Api::FrameTexture::graph`, the null `view` stubs and dead callbacks.
    - Allow a registration with no view ("captured view, graph only").
- **Declare every registered id, not every published one.** A producer changing path then changes only the commit's choice, not DCLF's main resources.
- **Faces:** declare only the textures they can read.
- **Retire `CS_DCLF_GRAPH_TEXTURE_TEST`** (`ReflectionsGraphTest`).
- **Producer-owned id constants**, used instead of retyped literals.
- **PublishedResources:**
    - a non-allocating export check;
    - a render-thread assert;
    - clear the view on Withdraw.
- **Docs:** refresh `dclf-architecture.md` "Frame graph textures" and the API notes.

### L3: framework

- **Define parity:** `ProgramBuild` adds CS's common shader defines (`Util::GetCommonShaderDefines`, `COMPUTESHADER`), with an opt-out; delete the per-port copies.
- **Shared helpers in `src/RenderGraph`:**
    - fullscreen, dispatch, copy and clear passes (from `ORGPostChain::Builder`) and `ProgramDefines`;
    - indirect dispatch on `ComputeProgram`;
    - one D3D11-to-rhi sampler converter.
- **Primitives:**
    - a per-run pass gate;
    - a signal that a registration's commit reached the GPU;
    - a one-shot clear or upload.
- **Setup template:** `FrameAnchors::Register` accepts shared-constant clients before State's first upload, so every port can use reserve, setup with `onFailed`, and an `IsActive` that checks the runtime.
- **One switch helper** for `CS_ORG_*` and the diagnostics, listed in `render-graph.md`.
- **Lifetime:**
    - a `Resize` bumps a generation the anchors read, replacing the caller's Refresh and Publish;
    - imports get a release path;
    - one retire mechanism (`ReleaseAfterD3D11` with a correct fallback), not frame counts (the composite);
    - the composite's revision uses a serial, not an address;
    - the tripwire's retired list is serviced each frame;
    - the owned frame checks every export before taking the device, and gets a teardown.
- **Framework core:** no feature or self-test calls; Upscaling's late-epoch separation and the self-tests register as clients.
- **Clean up:** move the tonemap-anchor sequence out of `Hooks.cpp` into one seam, and remove the stream trace, flush experiment and skip-tail switch.

### L4: ports

- Move the twelve Phase 3 W1-W2 ports and HDR Display, Upscaling, Screenshot, Sun Rays, Terrain Blending and glints onto L3's helpers and template.
    - Remove the mismatch counters, `prepared` atomics and fill/clear frame counts.
    - Replace the shader early-outs on constant equality with zero-group dispatches.
- **Skylighting** reads the shared constants instead of mirroring them.
- **Dual-target constants:** one struct plus `LATCH` per shader, with no second cbuffer layout.
- **Lookups:**
    - consumers find other features through published ids, not D3D11 view pointers;
    - LLF's culling buffers are published.
- **Narrower seams:** D3D11 resources are created lazily everywhere, and the ORG staging moves out of the feature files.
- **Names:** published ids are renamed where they clash (`SSS::` for Screen Space Shadows), along with the `CS_ORG_LLF_SHADOWS` switch.
- **Readback switches:** removed from the feature files.

### L5: submodules

- **BasicRHI:**
    - unified memory only without a non-device-local heap;
    - zero-size viewports and scissor clamping on Vulkan;
    - placeholder vertex and index binds after `ExecuteCommands`;
    - an acquire point for presenting from another queue;
    - a feature bit for viewports and scissors in secondaries (D3D12 bundles cannot set them);
    - `ShareVulkanDevice` safe to call twice;
    - device hooks can fail;
    - `enable_testing()`.
- **DXVK:** `FORK.md` for the four undocumented changes; fix `dxvk_device_info.h`'s doc comment; drop guests on device teardown.

**Deferred, tracked:**

- async FX pass compiles (needed before the runtime is on by default);
- dynamic resolution re-recording Post Processing;
- region-only screenshot readback;
- Vulkan memory priority;
- MSAA resolve on ORG resources;
- castable formats for sRGB mips.

**Done when:** batch A/B within noise for every Phase 3 scenario, reuse-on validation clean apart from the known kMAIN
layout, and a hot-reload and a forced shader-failure check on Post Processing and one W1 port.

## Phase 4: engine resources become ORG resources

The engine creates everything through a few functions (R6, R11). Order matters: textures and targets first, because DCLF already imports them and gains the most.

| #    | Task                                  | Hooks (SE / AE)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| ---- | ------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P4.1 | Tripwire handles                      | A handle object per type (Texture2D/SRV/RTV/DSV/UAV/Buffer/shader/state/input layout/query). Its IUnknown refcount works; every other slot logs the caller's return address (resolved with the address library) and returns failure or crashes with a report. `QueryInterface` answers only the IIDs the engine asks for. In the guest phase, handles wrap a real DXVK object made from the ORG resource, so un-ported readers keep working; in native mode they wrap nothing. Release can come from any thread (loader threads create and drop resources without `Renderer::Lock`), so the GPU free is deferred until the frames that used the resource retire. |
| P4.2 | Render targets                        | `BSShaderRenderTargets::Create` 100458/107175 drives `CreateRenderTarget` 75467/77253, `CreateDepthStencil` 75469/77255, 3D 75470, cube 75471, `CreateRenderTexture` 75507/77299. ORG creates them; the CS format upgrades (`Hooks.cpp:766-1220`, `State::ModifyRenderTarget`, HDR LDR upgrades) move into those descriptions. CreationEngineRaytracing's hooks (`external_refs/CreationEngineRaytracing/src/Hooks.cpp:181-540`) give signatures and a stand-in pattern. A window resize releases and recreates every target (114 and 12 depth-stencils on SE, 116 on AE) through `WindowSizeChanged`, so ORG resizes them as a set.                             |
| P4.3 | Textures                              | DDS load 75721/77533, then 75724/77538 (CreateTexture2D + SRV); Tex2D/Tex1D 75511/75513; release 75527/77322. One texture path through the graph-job loader.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| P4.4 | Texture streaming on ORG              | Replace the `dxvkStream*` shells (`TextureStreaming/Streamer.cpp`) with ORG-native partial residency: windows as ORG textures, bound through bindless indices. Keep the budget controller (`Policy::StepPullback`) and the traps in .                                                                                                                                                                                                                                                                                                                                                                                                                            |
| P4.5 | Buffers                               | Vertex/index buffer creation 75592/75593 and `TriShape` ownership; the dynamic ring 75484/75485/75487 (Map + query fence) becomes an ORG upload ring. DCLF's geometry imports (`Core/Gpu`) then read ORG buffers directly.                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| P4.6 | Shaders, input layouts, state objects | VS 75547, PS 75551, CS 75556, InputLayout 75583, Query 75590, blend/depth/raster/sampler creation (`CreateSamplerState`'s anisotropy cap in `Hooks.cpp`). Each records a description (bytecode hash, layout, state) for pipeline building; the handle stands in for the engine. `ShaderCache.cpp` (1506-1640, 2744-2817) gives the description its DXC/SPIR-V binary and reflection.                                                                                                                                                                                                                                                                             |
| P4.7 | VRAM tooling                          | `Vram/*` and `MemoryWindow` read ORG/BasicRHI's allocator (P0.13) instead of DXVK snapshots.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                     |

**Verify:**

- The tripwire log is empty over a full tour: menus, map, inventory, race menu, loading screens, Bink, a worldspace change (`cow Blackreach 0 0`), and save/load.
- VRAM totals match the DXVK build.
- `dclf:tex-stats` shows no regression.
- SE 1.5.97 and AE 1.6.1170 both pass.

---

## Phase 5: the engine's remaining passes as ORG passes

Each item follows DCLF's pattern:

- The engine's CPU code runs.
- Its pass is withheld or its draw choke point latches state.
- An ORG pass draws it with the same shader built for ORG.

Each item carries its RE dependency. They are ordered so the frame's skeleton (targets, clears, copies) goes first and the long tail last.

| #     | Item                           | Notes / dependency                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| ----- | ------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P5.1  | Frame skeleton around DCLF     | Shadow-target bind and clears, reflection-face bind and clear, the `RenderDepth` copy into `kPOST_ZPREPASS_COPY`, `FinishAccumulating` copies, `Renderer::Begin` per-frame CBs and `ClearState`. (R8)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| P5.2  | Finish DCLF coverage           | Unclaimed and `UnstableBuffer` shapes, Lighting without LLF, residue shadow casters and the rasterizer-state shadow view (needs P0.3), the reflection faces' cell-root residue, first person (`CS_DCLF_FIRST_PERSON` default) and first-person `HeatRefraction`, effect variants not ported (roadmap 4.1), master particles (1.4), blood impact particles (4.3), precipitation geometry. Use `dclf-roadmap.md`'s items; this plan does not repeat them.                                                                                                                                                                                                                                                                                               |
| P5.3  | Sky                            | BSSkyShader draw family: main view and reflection faces; CloudShadows' 4th MRT becomes an output of the ORG sky pass.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| P5.4  | Water                          | BSWaterShader family, reflection/refraction inputs, the water displacement simulation (an ImageSpace effect), WaterEffects/UnifiedWater bindings. Stencil if R2 finds it (needs P0.1).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| P5.5  | BloodSplatter (screen blood)   | Small family.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         |
| P5.6  | ImageSpace chain               | From R4's effect list, in the engine's order: HDR/tonemap (`ISHDR`, Effects11 tonemap hook), SSAO composite, DOF, menu blur, water blend, VL composite + VolumetricLighting's replacement CS, refraction, local map, and the rest. Each `BSImagespaceShader` setup's constants (about 40 functions) are latched instead of mapped. HLSL is written for every effect R4 lists as DXBC-only. Upscaling already replaces TAA. The surface doc lists all 159 slots: 39 run CS HLSL today, 68 have a CS source whose descriptor entry is commented out (enable and verify those first), and 17 have none (FXAA, BSLensFlare, CopySubRegionCS, the Downsample CS pair, LightingComposite x3, PerlinNoise CS x2, the SAO compute and raw-AO set, SILRawInd). |
| P5.7  | Constant-buffer path           | Replace every remaining `Setup*` Map/Unmap site (R3) with a latch into graph uploads. DCLF's `ConstantMirror` already decodes many of them. Constant buffers are pooled by size and rewritten before each use, so the latch is per use, never keyed by buffer pointer. About 40 functions go through the engine's 4-buffer custom mapper (ImageSpace b2, grass b7/b8, bones b9/b10, Effect VS b11).                                                                                                                                                                                                                                                                                                                                                   |
| P5.8  | Menu 3D                        | UI3DSceneManager (51855/52727), `Inventory3DManager::Render`, race menu, magic/map previews: their geometry as DCLF families with their own view, their `SetDirtyStates` calls withheld. (R9) DCLF does not claim this geometry today: these accumulators reach `RegisterPass`, but DCLF withholds only main, view and shadow renderers, so each needs its own view. Menu 3D also uses stencil (bit 0, then an ImageSpace NOT_EQUAL).                                                                                                                                                                                                                                                                                                                 |
| P5.9  | Map menu                       | `LocalMapCamera`, `BSImagespaceShaderLocalMap`, world map LOD. (R9)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| P5.10 | Scaleform                      | A new `GRenderer` implementation (vtable SE 0x1417dbf90, constructed by 82790, `SetDependentVideoMode` 82918) that records ORG raster passes: batched triangles, masks (stencil, P0.1), blend modes, render-to-texture, the font cache texture, Scaleform's shaders rewritten in HLSL from R5's DXBC. Swap it in at `BSScaleformManager` init (80603). Largest new subsystem in this phase. (R5) The renderer bypasses BSGraphics (raw device and context from `GetScaleformRendererParameters`). Its 58-slot vtable, 55 SM 4.0 blobs and the embedded HLSL source of its shaders are mapped in the surface doc, so the shaders are transcribed, not rewritten. Masks use its own stencil with INCR/DECR.                                             |
| P5.11 | Bink                           | Bink has no YUV path: `BinkCopyToBufferRect` writes RGBA8 256x256 tiles on the CPU and a copy shader draws them. The port is an RGBA8 upload ring plus a blit, presented through the ORG present stage from the blocking play loop or the sequence thread. (R10)                                                                                                                                                                                                                                                                                                                                                                                                                                                                                      |
| P5.12 | Loading screen                 | The loop is `JobListManager::ServingThread` mode 2 (SE 38085 / AE 39039) under `Renderer::Lock` (recursive critical section, Renderer+0x2790 SE / +0x27F0 AE), drawing Scaleform and the loading-screen 3D model through the full accumulator and ImageSpace path. ORG submission and present must be safe from that thread under the same lock. (R6)                                                                                                                                                                                                                                                                                                                                                                                                 |
| P5.13 | The flush and the shader binds | When the tripwire shows no draws left, `SetDirtyStates` 75580/77386, `SetShader` 75550/77351, `SetPixelShader` 75555/77356 and `DispatchCSShader` 75532/77329 become latch-only or no-ops, and their D3D11 sides are tripwires. Code in `src/RenderGraph/Engine/Flush/`, RE and design in [p5/flush.md](./p5/flush.md). `CS_ORG_RESIDUE_CENSUS=1` counts every engine draw and dispatch that still reaches D3D11 through the guest context (by draw function, callers, last technique and view) and the flushes and binds no draw follows (`p5-all-census.env`). `CS_ORG_ENGINE_FLUSH_LATCH=1` runs the four functions on the CPU only and skips, logging once per site, any engine draw that then reaches D3D11; flushes CS asks for apply the owed state (`p5-all-latch.env`). Both off by default. The latch becomes the default only when the census reads zero over a full tour, the latch run skips nothing, and the D3D11-state readers in flush.md read the shadow state. Census of 2026-11-11 (p5-all tour): engine draws reach D3D11 in every frame, about 95 a frame in Whiterun (up to 199): the cloud layers (P5.3, `CS_ORG_SKY_CLOUDS`) 35-55, the ImageSpace raster and compute effects not yet taken (SAO, SSR, water blend, HDR blur, VL; P5.6) about 35, DCLF residue in `RenderBatches` (prepass Utility, Lighting, instanced DistantTree, shadow casters; P5.2) 20-60, water's raw-water and refraction-normal passes (P5.4), particles and skinned Effect partitions (P5.2), the shadow-mask pass (no owner yet), menu 3D Utility (P5.8), Bink's first frame and off-thread logo (P5.11/P5.12), a little Scaleform (P5.10); flush.md "What is left" has the table. The latch run skipped them all and every shot broke, as expected. |

**Verify each item:**

- Screenshots against the DXVK build at the standard cells, plus the case each item adds (water at Riverwood, rain, a menu with a 3D item, the race menu, the local map, the Bethesda logo Bink, loading screens).
- Set parity for culling changes on the RX 6600.
- The residue census (`CS_ORG_RESIDUE_CENSUS`) reads 0 for the item's draws, and the item's vouch reason is deleted from
  the source.

---

## Phase 5.5: no engine fall-through (the gate before Phase 6)

Phase 5 left every engine subsystem running on ORG, but the latch census (2026-11-11,
`1111-173912-p35-latch-census-stall2`) still shows engine draws reaching D3D11: about 40-70 a frame in settled windows, and
60-1,300 skipped per load window on top of the vouched ones. The flush latch's vouch list (`Flush::VouchPass`,
`Flush::EngineDraws`) is an allow-list of known fallbacks: each vouched draw is still drawn by DXVK, and with DXVK off
nothing draws it. Growing that list does not move the port forward.

**Rule:** the port continues until the residue census reads 0 engine draws and dispatches reaching D3D11 in every window of
the census tours, load windows included. Phase 6 does not start before that. The measure is the census count; vouch
reasons are deleted as their items land, and no new ones are added.

**Gate:**

- `latch-census.env` and the added scenarios (rain, the local map and a menu with a 3D item, a main-thread Bink and the
  startup logo, Riverwood water, first person) read `0 engine draws and dispatches a frame reach D3D11` in every 300-frame
  window, with the latch on, on the RX 6600 with `DXVK_DEBUG=hang`.
- No `[E] [ORG] flush latch: engine ...` line, and no `Flush::Reason` left in the source.
- Census tours run with `zz_SkyrimEngineTelemetry` disabled: it only passes the engine's calls through, so with it off the
  census names the engine's own call sites.

| # | Item | Work | Deletes reason |
| --- | --- | --- | --- |
| 5.5.0 | Prerequisites | Merge `org-port/compile-stall` (done). Disable the telemetry plugin in the census scenarios. A census mode that fails the run on any engine draw, for batch checks. | |
| 5.5.1 | Load windows: DCLF owns every object from the first frame | Below. | `dclf: not claimed (main views, shadow views, reflection faces, occlusion maps)` |
| 5.5.2 | Sky cloud layers | Run `sky-river-org1-clouds{,-main,-faces}.env` with weather forced; fix the texture fault and the weather-blend difference; `CS_ORG_SKY_CLOUDS` on by default. | `sky: cloud layers`, `sky: left to the engine` |
| 5.5.3 | Volumetric Lighting on ORG | `ORGVolumetricLighting.{h,cpp}`: generate, raymarch and both blurs as compute passes, with 3D and 1D graph textures, the shadow cascades and Terrain/Cloud Shadows' published ids. Replaces the 107023 detour's scope and the `Hooks.cpp` shader swap. | `imagespace: volumetric lighting compute` |
| 5.5.4 | ImageSpace remainder | VL apply (3D/1D imports in `FrameAnchors`, reading 5.5.3's volume); the SAO composite (dual-target `IBL.hlsli` and `ExponentialHeightFog.hlsli`, their outputs by published id); `Main::RenderWaterEffects`' copies (a bindless source in the latch); refraction's late shape registered at Setup; the distant blur confirmed at 0. | `imagespace: effect not on the graph`, `shape refused`, `shape not in the manifest`, `failed on the graph` |
| 5.5.5 | Sun occlusion query | An rhi occlusion pool mirroring the engine's ring (75565/77366, Begin/End 75567-8 / 77368-9); an ORG pass replays SunOcclude between them; `GetQueryData` (75569/77370) answers once the frame retires. The second user (35563, read by 29262) the same way. Design in `p4/p4.6-shaders-states.md`. Done (`p5/occlusion-queries.md`): the sun at `EngineQueries` (d89148df8), the decal test at its own `DecalQueries` anchor (DCLF's decal occlusion family); both under `CS_ORG_OCCLUSION_QUERIES`, which 5.5.11 turns on. | `sky: sun occlusion query` (deleted) |
| 5.5.6 | DCLF residue | Refraction normals (name them with `p5-all-census-dclf.env`, then claim them); master particles (parity checks, then `CS_DCLF_MASTER_PARTICLES` on by default); precipitation (a family for the grouped-alphas particle draws, AE 107673); first person behind menus (RE RenderDepth's first-person block, take it in `Features/FirstPerson`); water's group-10 batch (the water family spans it). | `precipitation: no port`, `water: geometry group 10 batch`, the families' `left to the engine` reasons |
| 5.5.7 | Menus (P5.8/P5.9, `p5/menus.md`) | The menu scenes as an engine draw family, not DCLF views (a load has no DCLF members for the loading-screen model): the local map's two batches (`CS_ORG_MAP=1`) and a menu model's depth and colour batches (`CS_ORG_MENU3D=1`), drawn with ORG builds of Utility (base texture, fog of war, depth, shadow maps), Lighting and Effect, the world's CS features off in the last two. Written 2026-11-12. | `menu views: local map left to the engine`, `menu views: menu 3D left to the engine` |
| 5.5.8 | Scaleform remainder | GFx render targets as graph resources; the blur filter family (offset table transcribed); 3D draws, line strips, 32-bit indices, XY32f, gouraud 4-5, YUV textures. `CS_ORG_SCALEFORM_D3D11_PARTS` removed. | `scaleform: display not taken`, `D3D11 part`, `segment replayed` |
| 5.5.9 | Bink remainder | Mode-2 movies at the `DrawTiles` point; HDR output into HDR Display's UI texture; movies over 2048x1152; ticks between a resize and the next build; the logo's frame 1. | `bink: tick not taken` |
| 5.5.10 | D3D11 state readers | `EngineDrawFamily.cpp` `ResolveHeader` reads `RSS.PSTexture[]`; diagnostics read the latch or are listed as DXVK-only. Done on `org-port/state-readers` (p5/flush.md "Readers of the D3D11 state" and "DXVK's hazard rules under the latch"): every `Get*` reader classified; `ResolveHeader` reads only CS's registers (t16 and up), Water, ImageSpace and Scaleform read the shadow state, the diagnostics are DXVK-only. Under the latch DXVK keeps stale targets and drops CS's binds of their views (its hazard rules): traced with the census (`[ORG] state readers:`), and `CS_ORG_ENGINE_FLUSH_LATCH_UNBIND=1` unbinds DXVK's targets at a latched flush that changes them. A likely cause of the green streaks on near geometry with the latch on; `latch-census-short-unbind.env` against `latch-census-short.env` decides. | |
| 5.5.11 | On by default | Every port switch the census tours use becomes the default, and the latch is on by default in ORG builds, so the gate runs on the shipping configuration. | |

**Order:** 5.5.0, then 5.5.1 (the largest, and structural), then 5.5.2-5.5.6, then 5.5.7-5.5.9, then 5.5.10-5.5.11.
Lanes that can run in parallel: {5.5.2}, {5.5.3 then 5.5.4's VL apply}, {5.5.4's rest}, {5.5.5}, {5.5.6 per sub-item},
{5.5.7}, {5.5.8}, {5.5.9}. The main session runs the census after each merge.

### 5.5.1: load windows

**Now:** after a load DCLF clears its slot tables (`Core/Scene/Coordinator/Walk.cpp`, `Slots.cpp`) and rebuilds them only
once the loading screen closes, so the engine draws every object until its claim is installed (`FrameView::InstallClaims`).
Smaller gaps: materials with pending texture imports, pipelines not compiled, group renderers made mid-frame
(`MainRendererNow`), and the grass and tree LOD families, owned all or nothing.

**Decision (user, 2026-11-11):** the loading screen stays up until DCLF's claims settle; no black frames, no pop-in.

1. **Readiness signal:** the coordinator publishes "the load's objects are settled" beside `lackingCount`
   (`Core/Scene/Model/Publication.h`, `SceneSet.cpp`): no live slot waits on a transient reason (`MainReady`/`PhaseReady`
   whys 0, 1, 2, 3, 4, 9, 10), grass and tree LOD are owned or have nothing to draw, and shadow and occlusion coverage is
   in as `StartupHold` judges it.
2. **The loading menu keeps presenting** until that signal and the first adopted snapshot that carries it, while the
   world's first frames run withheld and unpresented. A cap logs what still waits; it never hands frames to the engine.
3. **Everything of DCLF's classes is withheld during the hold**, in every registration hook
   (`Core/Handoff/PassCapture.cpp`), matching `MainRendererNow` as well as `IsMainRenderer`; grass stands down through
   `GrassShared`.
4. **After the hold, an unready object is withheld until it is ready**, as `WaitsForBaseMips` does now. `KeepForEngine`
   then has no DCLF case left.
5. **A `coc`/`cow` with no loading menu** uses the same hold and presents the previous frame.
6. **Shortening the hold:** pipeline keys and texture imports are requested from the mirror's attach records during the
   load, without touching freed buffers.

Files: `Core/Render/StartupHold.{h,cpp}` (generalised to every load), `Core/Scene/FrameView/FrameView.{h,cpp}`,
`Core/Scene/Coordinator/{SceneSet,Walk}.cpp`, `Core/Scene/Model/Publication.h`, `Core/Handoff/PassCapture.{h,cpp}`,
`Core/Render/DrawFamilies/FamilyDecisions.cpp`, `Features/Grass/GrassDraws.cpp`, `src/Features/GrassOptimizations.cpp`,
`Core/Render/FrameDriver.cpp`, and the loading-screen path in `src/RenderGraph/Engine` (P5.12).

**Verify:** the loading menu stays up until the signal and logs its hold time; no frame after it skips or vouches a draw; a
cold-cache run reaches the world without a DCLF watchdog failure.

---

## Phase 6: cutover, a DXVK-free Vulkan build as a development option

**Entry condition:** the Phase 5.5 gate passes.

| #    | Task                                                                                                                                                                                                                                                                           |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| P6.0 | The owned frame (P1) presents with DCLF on: rerun `owned-frame.env` on current ORG (the deleter-thread fix may have changed the stall), then root-cause the lost wake in DXVK's submission queue (render-graph.md, "Status (2026-11-09)"). |
| P6.1 | `CS_ORG_NATIVE=1`: `DxvkLoader` does not load DXVK. `InitD3D` (75595) is replaced: no `D3D11CreateDeviceAndSwapChain`; the RendererData device/context/swap chain fields (+0x38/+0x40/+0x60) and their globals (SE 0x143025f08, 0x143027ea0) hold tripwire handles.            |
| P6.2 | Everything that still needs DXVK becomes a startup error that names it, so it cannot silently fall back. The DXVK build stays the default.                                                                                                                                     |
| P6.3 | Third-party D3D11 users fail with a clear message from the tripwire (the plugin's module name from the caller address), and each one is recorded in the API notes doc (P8.4). SkyrimEngineTelemetry only passes the engine's calls through: it is recorded as such and stays disabled in census tours.                                                                                                  |
| P6.4 | Validation: a full tour on the RX 6600 with Vulkan validation layers (`VK_LOADER_LAYERS_DISABLE='*nsight*'`), hang markers through P0.11, SE and AE, HDR on and off, upscalers, frame generation where hardware allows, and Release-preset frame times against the DXVK build. |

---

## Phase 7: D3D12

| #    | Task                                                                                                                                                                                                                                                                                                                                             |
| ---- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| P7.1 | Turn on `BASICRHI_ENABLE_D3D12` in `cmake/RenderGraph.cmake`; add `CS_ORG_BACKEND=d3d12` device creation (BasicRenderer's `DeviceManager` pattern).                                                                                                                                                                                              |
| P7.2 | DXIL targets in the shader store with the D3D12 binding ABI (SM6.6 `ResourceDescriptorHeap`, root constants instead of push constants); `ShaderCache`'s reflection reads DXIL.                                                                                                                                                                   |
| P7.3 | Streamline on D3D12: DLSS/DLSS-G/Reflex natively; D3D12 builds of CS's `sl.fsr`, `sl.fsr_g`, `sl.xess` plugins (`extern/Streamline/source/plugins`, which are Vulkan-only today: `fsrEntry.cpp:291`, `xessEntry.cpp:275`, `fsrGEntry.cpp:910`); FSR-FG with a DXGI swap chain. BasicRenderer's `UpscalingManager`/`FfxBackendDX12` show the way. |
| P7.4 | HDR (`SetColorSpace1`), latency (waitable object), DRED.                                                                                                                                                                                                                                                                                         |
| P7.5 | Parity gate: the P6.4 tour on D3D12 matches Vulkan in screenshots, and both match the DXVK build. Multi-RHI peer work stays deferred.                                                                                                                                                                                                            |

---

## Phase 8: cleanup and docs

- **P8.1:** Delete the D3D11 paths per feature, the DXVK-only tooling (`GpuIdleTrace`, the `dxvk*` exports in `DxvkOrgInterop.h`), and `CS_NATIVE_D3D11`. Delete ORG's embedding machinery once nothing uses it (external epochs, segment splits, queue boundaries, home states, `HostOwned` imports). This only happens when the user chooses to make the native build the default.
- **P8.2:** Fix the doc drift found in the survey:
    - `dclf-architecture.md` / `dclf-roadmap.md` 1.5 still say the engine draws grass.
    - Roadmap 4.6 and `OcclusionMapsFeature.h` disagreed about the precipitation mask: done in P5.2 (4.6 was built, `9c577fae2`; the
      roadmap item is removed).
- **P8.3:** Update `docs/development/render-graph.md` for the owned frame.
- **P8.4, from the start and ongoing:** `docs/development/org-extension-api-notes.md`. Each phase adds what an external mod would need:
    - an overlay pass after the UI;
    - texture create and upload;
    - render-to-texture (RaceMenu/SKEE overlays);
    - reading named engine targets;
    - a frame anchor to attach passes to (P2.2);
    - Present-hook users (ImGui mods);
    - every third-party D3D11 user the tripwire catches.

---

## Order and parallelism

1. Phase R (subagents, read only) and Phase 0 start together. P0.1-P0.5 gate P5, P0.8-P0.10 gate P1.
2. P1 (inversion) runs after P0.8-P0.10. P2 can start alongside it, since P2.1-P2.4 do not need the inversion.
3. P3 waves W1-W4 start once P2.2-P2.4 exist. W5 (FX runtime) can run in parallel with its own design doc.
4. P4 after R6/R11, alongside late P3.
5. P5 items in the listed order, each after its RE task and P4.2-P4.5.
6. Phase 5.5 until its gate passes: no engine draw reaches D3D11 in any census window.
7. P6, then P7, then P8 when the user decides.

**Commits:** each task's branch (see "Keeping the work separate and clean") is merged into `org_integration` by the main session after it builds and validates. Submodule changes to BasicRHI, ORG and DXVK go to their own repos first; ask before pushing the shared DXVK remote.

## Risks

- **A missed engine or mod call site.** Mitigated by the tripwire handles (P4.1) and by keeping DXVK as the default.
- **Performance.** DXVK batches state and barriers for the engine's residue; a naive ORG pass per engine draw would be slower. Every P5 item is a draw family with batching, not a draw-by-draw translation.
- **Streamline on BasicRHI's Vulkan.** SARP saw heap corruption. Check early in P1.4; DLSS-G needs an NVIDIA card.
- **AMD compiler defects** in new compute shaders (equality-with-constant early-outs). Test every new pass on the RX 6600.
- **Vanilla DXBC-only shaders.** 17 ImageSpace shaders have no CS source and 68 more are unmapped; Bink's copy shaders too. Scaleform's HLSL is embedded in the exe. Sizes are in the surface doc.
- **The loading-screen thread** renders off the render thread under the engine's lock (P5.12).
- **The Scaleform and FX runtimes** are each a subsystem of their own; scope them with their own design docs before building.

## Verification summary

**Builds:** `BuildDevFast.bat` per C++ change; Release only for timing.

**Shaders:** `hlslkit-compile` for shader changes.

**Every phase:**

- `tools/cs-ab` scenarios A/B against the DXVK build: screenshots with `dclf:shot`, frame time one sample per version, VRAM.
- In-game runs in `C:\Modding\MO2`, under 60 s by default, `DXVK_DEBUG=hang` on the RX 6600 while DXVK exists.
- SE 1.5.97 and AE 1.6.1170.
- An empty tripwire log from P4 on.

## Files most touched

- `src/RenderGraph/RenderGraphRuntime.{h,cpp}`, `src/DxvkLoader.cpp`, `src/Hooks.cpp`, `src/Deferred.cpp`, `src/State.cpp`, `src/Buffer.h`, `src/ShaderCache.cpp`, `src/Utils/D3D.cpp`, `src/Menu*.cpp`, `src/GpuEventTimers.cpp`
- `src/Features/*.cpp` and their subdirectories
- `src/Features/DrawcallLimitFix/{Core,Features,Integrations}`, `src/TextureStreaming/*`
- `extern/BasicRHI/rhi.h`, `rhi_vulkan.cpp`, `rhi_dx12.cpp`
- `extern/OpenRenderGraph/include/Render/PipelineDesc.h`, `RenderGraph.h`
- `extern/dxvk/src/d3d11/d3d11_main.cpp`, `d3d11_org_interop.h`
- `extern/Streamline/source/plugins/sl.{fsr,fsr_g,xess}`
- Docs: `docs/development/org-full-port.md` (new), `engine-d3d11-surface.md` (new, Phase R), `org-extension-api-notes.md` (new), and updates to `render-graph.md`, `dclf-roadmap.md`, `dclf-architecture.md`

## Settled decisions

1. **GrassOptimizations is retired** at cutover, not ported. DCLF's grass is the only grass path in the ORG build.
2. **NVIDIA checks are deferred.** Development runs on the RX 6600 and the iGPU. The DLSS-G and NVIDIA Streamline checks in P1.4 and P7.3 are gates to run whenever an NVIDIA card is available. They do not block the other phases.
3. **No engine fall-through before Phase 6** (2026-11-11). The measure is the residue census, not the vouch list: an item
   is done when its draws leave the census and its reason is deleted.
4. **Post-load frames** (2026-11-11): the loading screen stays up until DCLF's claims settle. No black frames, no pop-in,
   and the engine never draws the gap.
