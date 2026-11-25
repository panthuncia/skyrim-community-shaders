# perf2/post: native post chain, round 2

Lead `post`, 2026-11-25. Plan: [native-post-chain.md](./native-post-chain.md) ("Round 2, as built").

## Commits (CS branch perf2/post)

- `ab71ad19a` perf(post): native full-screen blur and get hit. Slots 6 and 7 are staged natively from rows CS computes
  (`FullBlurSteps`, `GetHitSteps`, `SlotBlurStep`); new SE/AE globals 528495/415441, 528476/415422, 527646/414598, 528473/415419,
  528474/415420, 513213/390953; foreign-hook check covers slots 6/7 and blur effects 9-15.
- `a49d953f4` refactor(post): drop observe and capture modes. Deletes `CS_ORG_POST_NATIVE` (engine/observe modes, family list) and
  `CS_ORG_IMAGESPACE_NATIVE` (capture/observe/diagnostic), their observe helpers, SwitchRegistry and switches.json rows. SE gets the
  native chain and CS's jitter by default (`PostChain::OwnsJitter` is graph mode only).
- `3f220899c` docs: native-post-chain.md "Round 2, as built", dclf-native-imagespace.md and render-thread-window.md updated.
- `964055f35` merge of org_integration 719c5d2a0 (BasicRHI a2b3b01). Conflicts in SwitchRegistry.cpp and switches.json resolved by
  dropping both sides' deleted rows. Not built yet.

## Validated

- Observe (before deleting it), slots 6/7 ported: guest, imods 106ca1/101dac/10fbe8/48428, 8.7k steps compared, 0 differed (p2-post-obs1).
- Native with the port (ab71ad19a): p2-post-nat1 passed, slot effects native 1662, through RenderEffect 0 (base: 214).
- Pre-merge stripped build: guest blur p2-post-nat2, guest+hdr, native-vulkan+quick, native-vulkan blur scenario p2-post-nvk-blur1 all
  passed.
- native-d3d12-quick failure (run 1125-135219): not this branch. The same build passed 6 minutes later (1125-135847). The failed
  run was slow throughout (1.4 fps, data load 25 s, world-ready at 114 s instead of 48 s), most likely contention from other
  leads' runs and builds. Its errors (engine-draw census lines in DrawTriShape) also appear in passing runs.
- Merged build (964055f35, Dev-Fast), final: native-d3d12-quick p2-post-d3d12-m1 passed; guest+quick p2-post-guest-m1 passed (post
  chain 105/105 frames native, RenderEffect 0); guest+hdr p2-post-hdr-m1 passed; native-vulkan+quick with the blur scenario
  p2-post-nvk-blur-m1 passed (664/664 frames native, slot effects native 1807, RenderEffect 0, blur shots look right).
- SE: every pair re-checked statically in Ghidra, all match. Untested at runtime (game folder is AE).

## Measured (Dev-Fast, guest, frozen river, CS_ENGINE_ZONES=1)

| | base1 (742af7870) | after2 (964055f35) |
|---|---|---|
| window | 5.194 ms | 4.755 ms |
| CS stage: post | 0.226 | 0.208 |
| engine AE 107146 / RenderSSAO #1 / AE 107285 | 0.055 / 0.040 / 0.024 | 0.050 / 0.037 / 0.021 |
| RenderEndOfFrameEffects self | 0.022 | 0.020 |
| CS DCLF: Z-prepass (untouched) | 0.588 | 0.532 |

Every epoch dropped about 8%, including ones this branch does not touch, so the window change comes from org_integration's merged
work and capture noise, not from this branch. The frozen river scene has no blur or get-hit effect active, so the slot 6/7 port
is not expected to show there. Its effect is that blur/get-hit frames no longer go through the engine's RenderEffect (214 to 0
calls). A fresh org_integration 719c5d2a0 baseline was built and staged but not captured: the director paused runs for the
lab-slot switch and the lead was handed back first.

## Decided by the agent, to confirm

- Get hit's angle uses AE's single constant on SE too (SE multiplies by 1/3600 and 2 pi separately: last-bit difference).
- Mode-2 underwater masked DoF: CS writes rows 4-7 as zero where the engine leaves the last override frame's values.
- Families 5-6 (SSR, SAO, water blend containers, about 0.14 ms engine self) are not ported this round.

## Open

- Fresh org_integration baseline capture (optional; see Measured).
- Families 5-6.
- The `CS AB p2-post` MO2 folder currently holds the 719c5d2a0 baseline DLL: restage or delete it.
