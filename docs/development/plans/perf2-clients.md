# Render thread round 2: frame-anchor client commits (lead clients)

The commit bodies of the frame anchors' clients (`CS.Anchors.ClientCommit`, client id as zone text). All timings are Dev-Fast
(/Od), guest mode, the frozen river scene (`trun.sh guest+quick ... --set env.CS_ENGINE_ZONES=1`), per frame.

## Baseline and ranking

`p2-clients-base` (b9d745407 plus two zones) ran on a loaded machine (615 frames in 6.3 s against 913-934 for the after
captures). Clients nobody changed ran 0.61 times as long in the after captures (median over 35 clients), so baseline figures
below are also given scaled by 0.61. Scaled, the total (0.20 ms) matches lead frame's quiet capture (0.197 ms).

| Client | base | base x0.61 | What the time was |
|---|---|---|---|
| `cs.pp` | 57.6 us | 35.1 | Colour grading's step: 31.9 of it. Five colour space matrices a frame, each through `std::format` and string-keyed map lookups |
| `cs.scaleform` | 45.0 | 27.4 | Emptying every surface's slot for every commit (about 12 `RunSlots::Store`, each with two locks), then a fresh 53 KB staging region a frame |
| `cs.imgui` | 20.5 | 12.5 | A commit of an empty overlay: 0 vertices, 0 draw lists, every game frame |
| the rest | 0.21 ms | 0.13 | 35 clients at 1-7 us (quiet) of real per-frame values: light directions, dispatch sizes, layer ops |

The HUD in this scene draws 14 commands with no textures, so the per-texture resolves lead frame suspected cost nothing here.

## What changed

1. **Colour space matrices by index** (`PostProcessing/ColorSpace.h`). `getWhiteAdaptedRGBMatrix(int, int)` returns the
   adapted matrix between two listed spaces from a 6x6 table made once. `ColorGrading::UpdateColorSpaceTransforms` and
   `BuildConstants` use it. Colour grading's step went from 31.9 us (19.5 scaled) to 4.0.
2. **Colour grading takes its curve readback only while the preview is open.** The copy still runs every frame, as before.
3. **Scaleform empties only the slots it filled.** Each commit stored null into every surface's colour and depth slot of its
   instance. Only a slot this client stored a frame in can hold one for the run, so it now remembers those per instance and
   empties just them.
4. **Scaleform keeps its staging region while the display's bytes are unchanged.** The GPU only reads it, so frames with the
   same bytes share one region. Per instance; an instance unused for 60 frames lets its region go.
5. **The overlay commits only when it has work** (`ImGuiRenderer`): the client is active when the frame has vertices or a
   texture request. Its stage still takes the Overlay tap; the client's passes record nothing.
6. **`RenderGraph::KeptBindings`** (new, `src/RenderGraph/KeptBindings.*`): a D3D11 view's GpuTextures binding kept across
   frames while its import stands (same generation, not superseded), with one shared reference to the view. Scaleform and the
   overlay resolve their textures through it, so a HUD or menu texture is resolved once rather than once a frame, and Scaleform
   no longer allocates a view holder per texture per commit. While the undeclared access check is on, every call resolves
   again. Not measurable in the river scene (no textures there).
7. **Zone**: `CS.PP.StepCommit`, the post chain's step name as zone text.

## Measured

| Zone | base x0.61 | after1 | after2 (final) |
|---|---|---|---|
| Client commits, total | 0.201 ms | 0.177 | 0.160 |
| `cs.pp` | 35.1 us | 18.3 | 19.6 |
| of which colour grading | 19.5 | 4.0 | |
| `cs.scaleform` | 27.4 | 21.6 | 14.3 |
| `cs.imgui` | 12.5 | 13.6 | not committed |
| `CS.Present.Overlay.RenderDrawData` (overlay tap inclusive) | 17.2 | 17.2 | 4.0 |
| `CS.FrameAnchors.Run` | 0.338 ms | 0.306 | 0.294 |

About 0.05 ms a frame Dev-Fast in all, against a noise of about 0.1 ms in the window; the per-zone drops are well above
their own noise.

## Not done

- The other 35 clients compute real per-frame values; no repeated work stood out. Each tap also calls
  `GraphFrames::NoteGraph` (a mutex and a string-keyed map lookup) per client; a few tenths of a microsecond each in /Od.
- The overlay's commit body is in ORGModuleServices (`ImGui::Commit`, maps and a staging region per frame), outside this lead.
  Skipping empty frames removes it for game frames; menu frames still pay it.
- ImageSpace's part clients (`cs.engine.imagespace.*`, 2-4 us per tap) belong to the post leads.

## Decisions to confirm

- Decided by the agent, to confirm: the overlay client is inactive on frames with no vertices and no texture requests.
  Textures waiting for a first upload wait until a frame draws, which then stages them as before.
- Decided by the agent, to confirm: `KeptBindings` holds a view and its import for up to 60 frames after its last use (a
  closed menu's textures). The old per-frame path released them once the frame retired.
- Decided by the agent, to confirm: `ColorSpace.h` and `ColorGrading.cpp` are post-processing's feature code, changed because
  the post chain's commit calls `BuildConstants`.

## Tests

| Config | Label | Result |
|---|---|---|
| guest+quick, trun capture | p2-clients-base | ran to its exit step (baseline) |
| guest+quick, trun capture | p2-clients-after1 | ran to its exit step (items 1-3, 6) |
| guest+quick, trun capture | p2-clients-after2 | ran to its exit step (all items) |
| guest+quick | p2-clients-v-guest-quick | passed (final build) |
| guest+hdr | p2-clients-v-guest-hdr | hang: the main thread waited in ORG's `StopFrameProduction` (a graph install at a frame revision) while the machine was loaded (2-4 fps before world-ready) |
| guest+hdr | p2-clients-v-guest-hdr-2 | passed, HUD drawn in HDR (final build, rerun) |
| native-vulkan+quick | p2-clients-v-native-vulkan-quick | passed (final build) |
| native-d3d12-quick | p2-clients-v-native-d3d12-quick | passed (final build) |
| guest-interior | p2-clients-v-guest-interior | passed (final build) |
| guest+menu | p2-clients-v-guest-menu | passed; the CS menu draws and its font reload works (checks item 5) |

Open: the one guest+hdr hang did not repeat. Its stack is in ORG's graph install, outside these changes; worth a look if it
comes back.

SE is untested at runtime (the game folder is AE 1.6.1170). Nothing here is runtime specific.
