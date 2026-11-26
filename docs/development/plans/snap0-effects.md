# snap0-effects: the render-half side-effect inventory (phase 0 item 5)

Lead: `effects`, branch `snap0/effects` on `org_integration` 8fb9b1dc5. Docs only: no build, no game run, no code change.
The inventory is [render-side-effects.md](render-side-effects.md).

## Method

- The render half is `Main::DrawFrame` (AE 36555 / SE 35556) and its call tree (the `kSites` table in
  `src/EngineZones.cpp`), plus the loading-screen loop on the serving thread.
- Five read-only research passes ran in parallel, one per slice: culling and visibility; lights, shadows, camera and
  stealth; water, sky, facegen, decals and ImageSpace; DrawFrame's top level, UI, loading and pacing; CS's own writes into
  engine objects (grep of `src/`).
- Each pass decompiled the writers in Ghidra (AE 1.6.1170), found stores to globals and fields, and followed xrefs to
  readers outside the render half. Field readers have no xref route, so they were found from known accessors and from
  `docs/development/skyrim-engine-notes.md` (fade state, culling side effects).
- I rechecked the highest-impact claims myself: the flow-map producer's `Sleep` loop (AE 32031), the screenshot handshake
  (AE 36859 writes ImageSpaceManager +0x211/+0x218), and the fade enable globals (AE 107143 sets them after 20 frames;
  AE 40438 zeroes them; LocalMapRenderer saves and restores them).
- SE ids come from CommonLib `RelocationID` pairs, matching SE call lists in order, or same-named SE functions. Unresolved
  ones are marked "SE ?".

## What game code reads from the render half

1. **A sync contract.** DrawFrame signals job stage events 5-9 that stage jobs and the "Non render-safe AI" job (player 3D
   update) wait on; the post-render stage (Sky::Update, face morphing, shared particles, grass) hangs off the same
   events.
2. **Counters and flags:** the frame counter, `gIsInMenuMode2`, the render-unsafe thread mask, the fade enable globals.
3. **Visibility for AI and animation:** the actor pre-cull (`kNotVisible` and `Actor+0x27C`), `kAccumulated`, fades, the
   shader-property frame stamp, portal visibility, the tree multibound result.
4. **Stealth:** the light level is computed game-side from ShadowSceneNode light lists and per-geometry light lists that
   only the render half updates.
5. **Queues the game fills and the render half drains:** flow-map tiles (the producer spins until an entry frees), decal
   occlusion tests, facegen tint bakes, the save screenshot, water ripples.
6. **Smaller items:** HUD quest-marker camera globals, sun glare, underwater resets, UI-driven updates (TweenMenu camera,
   inventory 3D item, book pages), PrintScreen.

## Top risks

- S1 stage events: probably a hang the first frame native skips DrawFrame. Must be handled before anything else ships.
- S16 flow maps: the cell loader hangs after about 128 flow-map cells.
- S15 save screenshot: autosave- or quicksave-style saves never complete.
- S12/S13 stealth: light level stops tracking new and removed lights.
- S6 actor pre-cull: CS does not cover it at all today, in any mode (guest still runs the engine's).
- S5 fade enable: fades off for the rest of the session after the first teleport.

## Decided by the agent, to confirm

- **Scope of "game reads".** Engine renderer readers count as "goes away". Game-thread readers that only drive a scene
  node (sun glare) are listed but marked visual only. Why: the user decides visual parity per feature anyway.
- **Suggested fates prefer calling the engine's own game-side logic on the game thread** (for example the high actor
  culler, `ProcessAllQueuedLights`) where it needs only game data. Why: smallest correct change. This may conflict with
  "port first" if the user counts those as engine render functions. The culler and the light queue are not render
  functions (no GPU work), but `ProcessQueuedLights` also runs the light-attach walk, which feeds the renderer.
- **SE ids left unresolved** rather than guessed: AE 40335, 40336, 38054, 41375, 41447, 41246, 36563, 37774, 32174,
  106342's callees, 107123/107124/107121, 107133, 107140.
- **The CS-writes table is grouped, not one row per assignment** for renderer state-tracker resyncs. The individual sites
  are: `State.cpp:386-392`, `Deferred.cpp:257, 360-364, 580-592, 664`, `ImageSpace.cpp:1917, 2157-2170, 2513`,
  `Tripwire.cpp:2539-2543`, `Hooks.cpp:291-296, 1305-1345`, `TerrainBlending.cpp:282, 515-533`,
  `GrassOptimizations.cpp:375-440`, `CloudShadows.cpp:242`, `InteriorSun.h:79`, `PhysicalSky.cpp:2653`,
  `RainRefraction.cpp:246`, `ViewAtmosphere.cpp:727`, `PostProcessing.cpp:783`. They all go away with the engine
  renderer.

## Open questions for the user

1. **Stage events (S1):** signal them from CS on the game thread at the old points, or remove the dependency? Their reset
   semantics and what frozen-menu frames (no event 5) do were not checked.
2. **Stealth (S12/S13):** run the engine's light queue processing on the game thread, or rebuild light membership from
   game data in CS? `ImageSpaceManager+0xE0` (`sunlightScale`), also a light-level input, has no writer found yet; if the
   render half writes it, stealth is affected a third way.
3. **Decal tests (S17):** answer the occlusion test from CS's readback, or accept every decal (skip the test)?
4. **Actor pre-cull (S6):** call the engine culler on the game thread, or compute visibility from DCLF's data? DCLF's GPU
   visibility arrives a frame or more late with an asynchronous render thread.
5. **Coverage (S7/S8):** in native, DCLF writes `kAccumulated` and fades only for roots it stands in. Which roots with
   controllers or managed nodes fall outside, and should CS run a game-side cull for them?
6. **Render-unsafe mask (S4)** and **PrintScreen/capture (S24):** where should CS clear the mask, and does native keep
   the vanilla screenshot key?
7. **UI-driven updates (S23):** TweenMenu's camera update and the inventory 3D item update run from menu draw callbacks.
   Should the UI port call them on the game thread, or should CS hook them out of the draw path in both modes?
8. Unverified readers: `Actor+0x27C` was found to have readers (head tracking, idles, facegen); `partiallyUnderwater` and
   `underwaterHeight` have none found; SSN `cameraPos` readers not checked; `Main+0x1D8` mode meaning unknown; the
   movement controller vfunc +0x78 identity unknown.
9. Not checked: terrain and object LOD fade managers, sound occlusion and havok cloth gating (believed to be game-side
   updates outside DrawFrame).
