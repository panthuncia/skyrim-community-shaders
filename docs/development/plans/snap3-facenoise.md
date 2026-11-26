# snap3-facenoise: purple noise on the player's face in native

Lead: `facenoise`, branch `snap3/facenoise` (base `org_integration` c4088e6e2), stage `s7-facenoise`. Follows decision 132 (open
item from [snap3-facegen.md](snap3-facegen.md)).

## Cause

The noise is uninitialised video memory, and the face drew a texture nothing ever wrote.

1. `BSFaceGenManager::PrepareHeadPartForShaders` (AE 0x14042bd90) makes a new NiSourceTexture for the head. It gives it an empty
   renderer texture through `Renderer::CreateRenderTexture` (AE 0x140e48190: the tint size, RGBA8, DEFAULT, shader resource,
   no contents). It puts the head in the face material's tint field (+0xa0) and queues the tint job (AE 27040).
2. Drawcall Limit Fix captures the face material when the head attaches. `MaterialPort::Capture` keeps the view each texture
   field names at that moment, which is the empty texture.
3. Then the bake runs. The engine's UpdatePending, CS's earlier engine path, and snap3-facegen's first bake path all gave the
   head a **new** texture holding the result and released the old one. Nothing tells DCLF that a renderer texture changed under
   a material it holds. DCLF keeps drawing the old, empty texture.
4. The native device does not zero a texture made without contents (`NativeDevice.cpp` `InitializeTracked` only moves it out of
   Undefined). So the empty texture holds whatever memory it was given, and the facegen shader's overlay blend shows it as
   coloured noise. DXVK zeroes new images, so guest draws a zero tint instead of noise.

This is why the old engine path showed the same noise: it swapped the texture too.

## Fix (`src/Snapshot/FaceTint.cpp`, native only)

- A bake now writes into the head's own texture, the one the material and DCLF already name. It no longer creates a texture,
  swaps it in or releases the old one (`CreateTexture`, `HandOver`, `ReleaseTexture` and the stats' "handed" count are gone).
  `MakeBake` checks that the head's texture is a 2D RGBA8 texture with one mip at kPLAYER_FACEGEN_TINT's size, as UpdatePending's
  copy needs. Anything else is fatal.
- The render half zeroes each new bake's head texture at the first world frame that sees the bake. It copies from a zero texture
  made once at that size, `FaceTint::Zero`. Until its bake is copied in, the face draws zeros, as it does in guest and on D3D11
  drivers, instead of noise.
- The copy of kPLAYER_FACEGEN_TINT one frame after the step is unchanged.

Guest and D3D11 install nothing from this file, so they are unchanged.

## Runs (stage `s7-facenoise`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+facegen | s7-facenoise-a-nvk-facegen | passed; no noise in the race menu shot, skin tinted; 4 bakes done |
| native-vulkan+quick, NPC steps (`--set steps`, Lydia placed outside) | s7-facenoise-b-nvk-npc, -c-nvk-npc | passed; the NPC was not framed (behind the camera, then combat with a guard) |
| native-vulkan+quick, NPC steps in Dragonsreach | s7-facenoise-d-nvk-npc | passed; two NPC faces on the stairs, mid-distance, no noise |
| guest+facegen | s7-facenoise-e-guest-facegen | passed; the face is flat dark grey (the zero tint, open item below) |
| guest+quick | s7-facenoise-e-guest-quick | passed |
| native-vulkan+quick | s7-facenoise-e-native-vulkan-quick | passed |
| native-d3d12-quick | s7-facenoise-e-native-d3d12-quick | passed; bakes 1-2 done |
| native-vulkan+cadence-drop-cells | s7-facenoise-e-native-vulkan-cadence-drop-cells | passed |

Detector: this change adds and removes no engine calls. The face tint step and its copy were already CS work.

## Decisions (agent, to confirm)

- **FN-a.** A bake fills the head's existing texture instead of swapping in a new one, as vanilla does. Why: DCLF keeps the
  views it captured, so a swap is never drawn. Filling in place also removes a texture allocation and release per bake.
- **FN-b.** New bakes' head textures are zeroed on the render side, so the 1-2 frames before the copy show zeros rather than
  noise (visible on each race menu change otherwise).

## Open items

- **Guest probably draws a zero tint on the player's face.** Guest goes through the same steps: the engine's UpdatePending swaps
  the head's texture and DCLF keeps the empty one, which DXVK zeroed. A zero tint makes the face darker than vanilla (base squared
  instead of the tint's overlay), with no noise. Guest is out of scope and was not changed. A fix would make DCLF capture the
  material again when the engine swaps a texture under it (for example, from Upscaling's UpdatePending detour).
- **The native device leaves textures made without contents uninitialised**, unlike DXVK and D3D11 drivers. Any other engine
  texture that is read before it is written would show noise the same way. Zeroing them in `InitializeTracked` would make the
  two devices behave alike, but it costs GPU work for every such texture. Not changed here; worth a decision.
- NPC close-up: the NPCs seen were mid-distance. Vanilla NPCs use pre-built tint textures and queue no bakes, so the cause above
  cannot reach them.
