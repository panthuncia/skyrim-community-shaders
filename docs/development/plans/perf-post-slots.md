# perf/post-slots: the native post chain's slot effects, SE ids, the FSR shot

Lead `post-slots`, 2026-11-25. Builds on [perf-post-native.md](./perf-post-native.md) and
[native-post-chain.md](./native-post-chain.md) ("Milestone 1, as built", "Slot effects, as built").

## What changed

- **Slot effects native.** The native end-of-frame family no longer calls the engine's `RenderEffect` for refraction (slot 1), the
  depth of field and distant blur (slot 3: downsample, compute blur H and V, the composite of its mode) or the radial blur (slot 5).
  CS stages their steps from rows it computes from the snapshot (`NativePost.inl`: `RefractionSteps`, `DofSteps`, `DofBlurStep`,
  `RadialSteps`, `RunSlot`). Compute steps stage their CS b0 rows and thread groups natively (`StageStep`'s new branch,
  `NativeSteps::State::computeRows`).
- **Write-back.** Refraction's drawn flags (AE 415291/415292) are cleared, as its Render does. After a depth of field composite with
  the underwater mask flag (AE 391616) set, CS clears rt12 through the skeleton's clear pass (`Sites::TakeColourClear`, lead
  reflections' pass; D3D11 clear when none takes it) and clears the flag.
- **Legacy Graphics Compatibility's selector** (pre-1.7.99 shaders) is written into the native rows as its Render hooks write it
  (`SetLegacySelector`), including the hook's quirk: it compares the float index with the param's row count, so the radial blur's
  selector (index 7, two rows) is never set.
- **Foreign hooks** (decision 4): a slot whose Render or UpdateParams vtable entry leads outside the game and CS stays on the
  engine's `RenderEffect`, one log line.
- **Observe mode** predicts the native slots' steps in the engine's order (pre-tonemap slots, HDR, TAA, screenshot, LDR slots, the
  upsample) and compares rows; compute steps compare the engine's CS b0 capture and the thread groups.
- **SE ids.** Every global, function, call site and code constant the families use has its SE pair (table below). SE runs the
  families on the engine unless `CS_ORG_POST_NATIVE` is set (then also CS's jitter on SE, State offsets 8 bytes lower).

## Slots left on the engine

- Full-screen blur (6) and get hit (7) keep `RenderEffect`: their Render draws nested blur effects (`ImageSpaceEffectBlur` H/V
  passes with the radius hand-off at AE 415421), get hit's rows take a time-based noise (AE 0x14153b7a0) and gameplay globals
  (0x1435ef4f0/4fc). Their draws are not predicted in observe mode. Triggered in the runs below by `imod 106ca1`, `imod 101dac`,
  `imod 10fbe8`, `imod 48428`.
- The raster DoF blur (compute blur off, AE 391684 false) and a DoF composite whose param UpdateParams did not write this frame keep
  the slot on `RenderEffect` (neither seen in the runs).
- Not reached: the underwater DoF forms (underwater mode 1/2 with manager +0xB8), the LDR slots (manager +0x210), the DoF custom
  range (mode bit 0x80). Their rows are written from the decompile; observe has not seen them.

## Measured (Dev-Fast /Od, guest, FSR, frozen river, 6 s Tracy, CS_ENGINE_ZONES=1)

Baseline: this branch before the change (perf/post-native). After: the change merged with org_integration 094a61908 (tickets,
commit-inputs, reflections), so the window and ORG buckets moved for reasons outside this item; the slot zones are this item's.

| | base | after |
|---|---|---|
| Window | 6.953 ms | 4.988 ms (mostly the merged leads) |
| Slot effects (`CS.PostNative.SlotEffect`, inclusive; base: engine RenderEffect plus CS's per-effect shim) | 0.045 | 0.012 |
| `CS.PostNative.EndOfFrame` inclusive | 0.176 | 0.136 |
| `Main::RenderPostProcess` inclusive | 0.812 | 0.669 |
| Engine self in `RenderEndOfFrameEffects`' zone (CS's native code under it) | 0.022 | 0.021 |

No engine code runs for slots 1, 3 and 5 any more; slots 6 and 7 only run under modifiers.

## Test runs

| Config | Label | Result |
|---|---|---|
| guest+quick, trun baseline / after (frozen river, Tracy) | p-post-slots-base, -after | captured |
| guest+quick, observe scenario (noon, `imod` 106ca1, ef95f, 101dac, 10fbe8, 48428, TAA, night, `fw 81a`, FSR, Dragonsreach) | obs1, obs2, obs3 | differences found and fixed: the legacy selector (DoF c0.z, radial id1.w) |
| same, observe | obs4 | 31.9k steps compared, 0 differed |
| same, native | nat1 | no tap errors; Dragonsreach shot vs observe run: mean channel difference 0.96 |
| native-vulkan+quick, slots and FSR / frozen river 400, 600, 1200 frames | nvk1, nvk2 / frz1, frz2 | no tap errors; full-screen FSR shots |
| native-d3d12-quick, slots and FSR / frozen | nd1 / nd-frz | no tap errors; full-screen shots |
| guest+hdr | guest-hdr | passed |
| guest+fg | guest-fg / fg2 / fg-eng (`CS_ORG_POST_NATIVE=0`) | device lost once after the coc with FG on (intermittent); then passed twice |

Slot activity in the native runs: refraction and the DoF every frame, radial blur, full-screen blur and get hit under the modifiers
(slot effects native 3.5k, through RenderEffect 1.5k for slots 6 and 7).

## Item 3: native Vulkan FSR shot in the top-left

Not reproduced on this branch. Five native Vulkan runs with FSR (the default method), including the previous lead's frozen-river
steps with shots at 400, 600 and 1200 frames (frz1, frz2) and a native D3D12 frozen run, give full-screen upscaled shots. The
previous lead's frozen shots (nvk3, nvk4, nvk-obs, nvk-eng, and nd12c on D3D12) all show it: the render-size image in the top-left
of a kMAIN whose rest is the sky, i.e. the upscaler's output never reached kMAIN (no evaluate, or no copy-to-main). The lock
hypothesis (Upscale reading a locked dynamic resolution, so FSR evaluates at display size) was ruled out by a diagnostic: the lock
is 0 at `Upscaling::Upscale` in native mode. The likeliest cause is that run's stage (missing `sl.fsr.dll` or a stale DXVK copy, the
brief's warning); decided by the agent, to confirm. The stage folder is gone, so it cannot be checked.

## SE map (AE id -> SE id), verified statically

Order of RIP-relative references in each AE/SE function pair plus decompile comparison; INI settings also by their name strings.

| AE | SE | what |
|---|---|---|
| 391339, 391342, 391345 | 513490, 513492, 513494 | filmic flag, Reinhard and filmic white |
| 391312, 391305, 391324, 391309, 391321 | 513472, 513467, 513480, 513470, 513478 | saturation, brightness, map brightness, contrast, map contrast |
| 391330, 391333, 391327 | 513484, 513486, 513482 | eye adapt speed, strength, menu timer |
| 391315, 391318, 391336 | 513474, 513476, 513488 | bloom threshold boost, map form, multiple luminance references |
| 415051..415054 | 528106..528109 | adapt off, world map, adapt reset, luminance parity |
| 390954, 390973 | 513214, 513233 | frame time, framebuffer range |
| 391591, 391592, 391593, 391611 | 513663, 513664, 513665, 513677 | TAA reset, K, blend, threshold |
| 391302, 391307 | 513464, 513468 | adapted texture, blur weight table (-0xE8) |
| 391055, 391301, 414913 | 513299, 513463, 527940 | EOF ImageSpace, HDR, VL enabled |
| 414580, 415032, 415007 | 527667, 528087, 528062 | current camera, world scene graph, DoF camera |
| 411489, 411451 | 525008, 524970 | frame counter, render target manager |
| 415291, 391008, 391684, 391616 | 528345, 513267, 513727, 513680 | refraction flags, underwater mode, DoF compute, underwater mask flag |
| 415121.., 415136.. | 528176.., 528191.. | DoF INI, DoF custom range |
| 107014, 107017, 107193, 107206, 106988, 105675, 77453, 77454 | 100297, 100300, 100475, 100486, 100274, 99024, 75646, 75647 | VL settings/colour, ApplyVL, VL blur, lens flares, composites, SetRenderTarget, depth |
| 107148+0x1E7, 105674+0x178 | 100430+0x1F0, 99023+0x1EA and +0x230 | call sites |
| 77518 UpdateJitter | 75709 | State index/tables at +0xB8/+0xBC/+0xDC on SE (+0xC0/+0xC4/+0xE4 AE) |

Struct offsets used (ImageSpaceManager, TAA object, effects, camera) are the same on SE. SE's filmic, adaptation and TAA weight
formulas are written differently (1/pow(x, 2.2), X/(Y*100), a shifted tent) and can differ from AE's in the last bit; observe's
tolerance (1e-5 relative) covers that. SE's RenderEndOfFrameEffects and RenderPreTonemapComposites inline RenderEffect (they call
99027 with an effect pointer), so CS's RenderEffect detour sees no SE draws; native mode does not depend on it.

## Decided by the agent, to confirm

- Slots 6 and 7 stay on the engine's RenderEffect (nested effects, time noise; rare).
- SE: ids added and verified statically only (the game folder is AE); SE stays on the engine unless `CS_ORG_POST_NATIVE` is set.
- The rt12 clear follows the engine's condition (flag set and any composite drew), not only the masked composite.
- The radial blur's selector stays unset, as Legacy Graphics Compatibility leaves it today (its index check counts rows, not floats).
  Fixing the hook would change the radial blur's look on pre-1.7.99; left for its owner.
- Item 3 not reproduced; no change made.

## Open items

- Port slots 6 and 7 (blur sub-effects H/V with radius 0x1435ef500, get hit's noise and globals).
- Observe coverage of the underwater DoF, the LDR slots and the custom DoF range.
- Run SE once with `CS_ORG_POST_NATIVE=1` (observe) on a 1.5.97 game folder, then make native its default.
