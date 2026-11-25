# perf2/native: native post chain families 5 and 6, submodule switches

Lead `native`, 2026-11-25. Plan: [native-post-chain.md](./native-post-chain.md) ("Families 5 and 6, as built").

## Commits

CS branch `perf2/native` (on org_integration 67fd3f3cc):

- `19a11568c` perf(post): native reflections, SAO and water blend. `NativeWorld.inl` (new), the hooks in `ImageSpace.cpp` (a new
  detour on the water blend, SE 100550 / AE 107285), `StageWorldKey` split out of `TakeWorldStep`, the compute blur's rows shared
  with the depth of field (`SetBlurCs`).
- `7b71cd7cc` refactor: drop three submodule switches. SwitchRegistry and switches.json rows, doc mentions, and the pointer bumps:
  - ORGModuleServices `perf2/native` `ded0e73` refactor: drop the ORG_BOUND_PUSH_TABLES switch.
  - BasicRHI `perf2/native` `e9d3abb` refactor(vulkan): drop the BASICRHI_VK_BOUND_UBO switch; `4728be0` refactor(vulkan): drop
    BASICRHI_VK_FORCE_GENERAL.
- docs commit: this file and native-post-chain.md.

## Item 1: families 5 and 6

The reflections pass, both RenderSSAO calls and the water blend no longer run in graph modes. What each native form computes and
writes back is in native-post-chain.md. A container still runs when another mod hooks it (decision 4) or before the world steps have
a build; no switch.

Parity, in place of an observe mode: a development build (not committed) ran the containers and compared each step CS predicted
from the engine's data with the step the engine drew (key, explicit rows bit for bit or within 1e-5, compute thread groups). Run
p2-native-cmp3, guest, frozen river with iReflectionResolutionDivider 2 (default, raster mask), 1 (none) and 4 (compute mask), and
ToggleFog (copy form): 18k steps, 0 differed, 0 key mismatches. Excluded: the raw AO's random angle (CS's own generator) and fields
the engine leaves as stack garbage (CS writes zero; listed in the plan). Then shots of the same steps, engine path (cmp3) vs native
(p2-native-nat1): mean channel difference 1.23-1.45, differences only on animated water ripples, the player and edges. A kSSR readback
(`CS_ORG_SHOT_TARGET=109`, p2-native-v-ssr) shows blurred reflections. kSAO (46) was not read back (known crash).

### Measured (Dev-Fast, guest, frozen river, CS_ENGINE_ZONES=1)

| | base1 (67fd3f3cc) | after2 (final build) |
|---|---|---|
| window | 3.964 ms | 4.324 ms (machine slower, see below) |
| reflections pass, inclusive (engine 107146 zone / CS.PostNative.Reflections) | 0.070 | 0.041 |
| RenderSSAO, both calls, inclusive | 0.104 | 0.057 |
| water blend, inclusive | 0.021 | 0.016 |
| the three together | 0.195 | 0.114 |
| engine self in them (107146, RenderSSAO #1 and #2 with its explicit Render, 107285) | 0.135 | 0 |
| engine self, all zones | 0.915 | 0.853 |

The after capture ran slower everywhere: zones this branch does not touch grew 10-16% (TonemapCallSite 0.062 -> 0.072, CS stage
post 0.120 -> 0.133). A first after capture (after1) was 1.6x slower everywhere and is discarded. So the window cannot be compared;
the three containers' cost dropped by 0.08 ms measured, about 0.1 ms after the machine-wide slowdown. What is left in the native
forms is the step staging (`CS.PostNative.Stage`, 20 steps a frame, 0.051 ms), the scope flushes and SetCameraData, which the
reflections pass keeps for the engine code after it.

## Item 2: submodule switches

- `ORG_BOUND_PUSH_TABLES` (OMS): 0 only made program builds read constant buffers from the heap. Deleted. SPIR-V builds still take
  bound push tables only when BasicRHI binds them (`spirvBoundPushTables`).
- `BASICRHI_VK_BOUND_UBO` (BasicRHI): 0 only brought back heap storage constant buffers on the descriptor-sets model, the slower path
  on Pascal and Maxwell. Deleted. The hardware fallback stays: a device without VK_KHR_push_descriptor, or a layout past the uniform
  buffer limits, still reads them from the heap.
- `BASICRHI_VK_FORCE_GENERAL` (BasicRHI): an experiment keeping every image in GENERAL. Deleted (decided by the agent, to confirm:
  maxwell-raster-handoff.md had kept it as a debug switch, but it only selects a slower layout path).

## Test runs (final build unless noted)

| Config | Label | Result |
|---|---|---|
| guest+quick, Tracy baseline (frozen river) | p2-native-base1 | captured |
| guest+quick, compare build, divider and ToggleFog steps | p2-native-cmp3 | passed; 18k steps, 0 differed |
| guest+quick, native, same steps | p2-native-nat1 | passed; 798 frames native for all three, 0 on the engine |
| guest+quick, Tracy after (frozen river) | p2-native-after1 / after2 | captured (after1 discarded, contention) |
| guest+quick | p2-native-v-guest | passed |
| guest+hdr | p2-native-v-hdr | passed |
| native-vulkan+quick | p2-native-v-nvk | passed; native forms ran, 0 on the engine |
| native-d3d12-quick | p2-native-v-d3d12 | passed |
| guest+quick, kSSR readback (river save: water close up, SSR) | p2-native-v-ssr | passed; blurred reflections in kSSR |

## SE pairs (AE -> SE), matched in Ghidra by the order of references in each SE/AE function pair

| AE | SE | what |
|---|---|---|
| 107285 | 100550 | the water blend |
| 415058, 415059 | 528113, 528114 | SSR camera, SSR blur |
| 391433, 391436, 391442 | 513558, 513560, 513564 | ray march thickness, radius, alpha weight |
| 415060 to 415067 | 528115 to 528122 | SAO settings, half size |
| 415069, 415070, 415072 | 528124, 528125, 528127 | contrast, ToggleFog, reflections flag for the composite |
| 391479 to 391524 (step 3) | 513589 to 513619 (step 2) | snow sparkles |
| 391424, 391445 | 513552, 513566 | the composite's reflection row |
| 77507 | 75698 | State camera data lookup |
| 390951 | 513211 | BSShaderManager::State pointer |

SE's code constants have the same values (the random angle is `rand * 2^-32 * pi` there, in two multiplies). SE runs the native forms
by default; untested at runtime (the game folder is AE).

## Decided by the agent, to confirm

- The reflections pass's native form still calls `State::SetCameraData(world camera, 1)`: engine code after it in RenderPreResolve
  may rely on that camera data. It is state set-up, not the container.
- The raw AO's random angle comes from CS's own generator (the engine's generator is a static inside RenderSSAO).
- Fields the engine leaves as stack garbage are written as zero (the SAO-only composite reads rows 0-3 only with fog, per
  dclf-native-imagespace.md).
- `BASICRHI_VK_FORCE_GENERAL` deleted (above).
- `RE::BSShaderManager::State::GetSingleton()` in CommonLib returns the pointer slot, not the object (390951 holds a pointer; the
  first native build crashed on it). NativeWorld.inl reads the pointer. Other CS users of that accessor were not checked.

## Open

- The SAO composite forms with SAO on (+0x50) were not reached in the test profile (fog-only and copy were); their rows follow the
  decompile.
- The water blend's per-frame clear of the history it writes (RT1, clear mode) is not applied, as before this branch (the explicit
  native step never applied engine state). Its shader discards non-water pixels.
- The snow window between the RenderSSAO calls, start water and lens flares (families 7-8) stay the engine's.
- A clean-machine window comparison (both captures here ran under other leads' load).
- The `CS AB p2-native` MO2 folder is deleted at the end of this item.
