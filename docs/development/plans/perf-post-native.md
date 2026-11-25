# perf/post-native: the native post chain, milestone 1 (families 0-4)

Lead `post-native`, 2026-11-25. Plan: [native-post-chain.md](./native-post-chain.md), section "Milestone 1, as built". AE only.

## What changed

- `CS_ORG_POST_NATIVE` (default 2, native): RenderWorldPostProcess's body, RenderPreTonemapComposites, RenderEndOfFrameEffects and the
  HDR effect's Render are no longer called in world frames. CS reads a snapshot at Renderer::Begin (A) and at the post chain's entry
  (B), stages the steps with rows it computes (HDR downsample, luminance levels, adaptation, bloom bright pass and blur, tonemap, VL
  composite, TAA, the upsample, the screenshot copy), and writes back the state engine code after the chain reads. 1 is observe.
- Slot effects (refraction and the depth of field, active in nearly every river frame) still go through the engine's RenderEffect
  from CS's routing; the world map and FXAA keep the frame on the engine (decision 7).
- CS computes the vanilla TAA jitter itself in graph modes (`PostChain::UpdateJitter`, decision 3).
- The tonemap call site's work is one function, `RenderGraph::Engine::TonemapCallSite`, shared by Hooks.cpp and the native path.
- Post Processing's vanilla-bloom skip is readable (`PostProcessing::SkipsVanillaBloom`) and applied by the native HDR family.

## Measured (Dev-Fast /Od, guest, FSR, frozen river, 6 s Tracy, CS_ENGINE_ZONES=1)

| | base | after |
|---|---|---|
| Window | 6.704 ms | 6.617 ms |
| Engine code (zones' self) | 1.304 | 1.058 |
| Other CS | 0.594 | 0.778 |
| `Main::RenderPostProcess` inclusive | 0.799 | 0.772 |
| Engine self in the post chain: RenderEndOfFrameEffects 0.185, composites 0.030, VL blur 0.018, body 0.011 | 0.244 | 0.034 (the slot effects' RenderEffect) plus the lens flare gather |
| CS stage: post | 0.313 | 0.304 |

After the change the engine-zone names around the call sites CS now calls through (`RenderEndOfFrameEffects (in Main::RenderPostProcess)`)
time CS code: the native end-of-frame effects 0.160 inclusive, of which the tonemap call site 0.100 (the Tonemap anchor and Post
Processing's PreProcess, 0.064 self, which was hidden in the engine's self before), the native HDR effect 0.036, the slot effects
0.041. The native body is 0.035 self. In Dev-Fast the CS replacement costs about what the engine's optimized containers did; in an
optimized build the CS part shrinks about 3x. The larger gain is structural: the Post stage depends only on the snapshot.

## Test runs (all passed)

- Observe, guest: river noon, Upscaling TAA, night (`set gamehour`), weather (`fw 81a`), Upscaling off, teleport to Dragonsreach, FSR:
  18k steps compared, 0 rows or states differed after the fixes (obs5). Post Processing off: 24k steps, 0 differed (obs-nopp). HDR
  Display on (guest+hdr): 0 differed (obs-hdr). Native Vulkan frozen river with TAA: 5.8k steps, 0 differed (nvk-obs).
- Native: the same guest scenario (nat1), guest+hdr (hdr), guest+fg, guest+fg-dlss, guest+fg-hdr, native-vulkan+quick and
  native-d3d12-quick, each also with a frozen-river TAA scenario (nvk4, nd12c). No tap errors.
- Shots: the frozen river, engine path vs native, mean channel difference 13-16 against 17.5 between two engine runs (guest; the water
  still moves at timescale 0); native Vulkan 2.3 / 3.4.
- Fixed by observe: the VL composite's colour and VL's activity are B values; the bloom is skipped under Post Processing's bloom;
  the HDR effects' constant taps; native mode's dynamic resolution, ratios and upsample flag are read at the end-of-frame entry.

## Decided by the agent, to confirm

- Slot effects (refraction, depth of field, blurs) stay on the engine's RenderEffect inside the native end-of-frame family: they are
  in nearly every frame, and their rows (the depth of field's compute blur and its UpdateParams) need their own port and observe runs.
- Native mode is AE only; SE runs every family on the engine with one log line (no SE ids for the inputs yet).
- Post state starts each frame from the engine's copy, which CS writes back; this lets a frame on the engine path continue it.
- Per-step shader PreRender/PostRender, the engine's TAA history clear on a reset (CS's history pair is its own) and ApplyState in
  the body are not replicated.
- One row added to the main checkout's `tools/cs-test/switches.json` to run tests (the director keeps it); the branch has it too.

## Open items

- Port the slot effects' rows (refraction is one row; the depth of field needs its compute blur and UpdateParams), then drop the
  RenderEffect calls.
- The underwater DOF's rt12 clear and the water blend's rt88 clear (lead-engine-skip's report) have no graph pass in native mode;
  both are outside this milestone (the DOF slot and family 6).
- SE ids for the snapshot's globals.
- The native-vulkan FSR shot shows the render-size image in the top-left with or without this change (pre-existing).
