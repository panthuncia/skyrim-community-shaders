# snap3-quarter: native world drawn in the top-left render-size corner

Lead: `quarter`, branch `snap3/quarter` (base `org_integration` 2abd9a070), stage `s7-quarter`.

## Symptom

In native Vulkan the world showed only in the top-left 1280x720 of the 1920x1080 frame (the render size of FSR quality 1),
and the scene was much darker. The rest of the frame was the menu's background or empty. It was not only the race menu: the
director's `native-vulkan+quick` shot at 2abd9a070 (`1126-161752-dir-native-vulkan-quick-...`) shows the same corner. cs-test
does not compare shots, so those runs still said "passed".

## Cause

The water merge (fb8387ed3, decision 154). `Upscaling::Main_RenderPrecipitation`'s native branch dropped the whole lock
write, the unlock included:

- `Upscaling::ConfigureUpscaling` sets `dynamicResolutionLock = 1` at the frame's start (with this frame's ratios), so what
  draws before the world is full resolution.
- In guest, two things clear it before the world: Upscaling's detour of FaceGen UpdatePending and the RenderPrecipitation
  call-site hook (1 around the call, then 0).
- In native, FaceTint NOPs both engine calls of UpdatePending (snap3-facegen), so only the RenderPrecipitation hook was left.
  The water merge's native branch made it call through with no write, so the lock stayed 1 for the whole frame.
- The world still draws at the render size (FrameCamera ratios, decision 117), but the native ImageSpace reads the lock
  (`NativePost.inl` `ReadEndOfFrame`, `NativeWorld.inl` `BlurRatios`, `ImageSpace.cpp`'s vertex rows) and so do
  `Util::ConvertToDynamic` and LegacyGraphicsCompatibility. With the lock set they take the frame as full resolution: the
  post chain samples the whole target, so the world lands in the corner, and eye adaptation averages the empty rest, so
  the scene is darker.

No bisect build was needed: the cause shows in the code, and the fix restores the frame (runs below). The menus merge
(479acf7b3) is not involved.

## Fix (`src/Features/Upscaling.cpp`, native branch of `Main_RenderPrecipitation`)

The native branch clears the lock after the call again. It still makes no lock write around the call, since native's
occlusion maps make no engine render call (decision 154 stands). Guest is unchanged.

Decided by the agent, to confirm: the unlock stays at this call site, the point where the world's scaled draws begin, as
before the water merge. A cleaner end state is native readers taking "full resolution" from `FrameCamera`
(`fullResolution` / `ViewportRatios`) instead of the engine's lock, which also removes these live reads; that is a phase 7
item and out of scope for a minimal regression fix.

## Runs (stage `s7-quarter`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+facegen | s7-quarter-a-facegen | passed; full frame, framing and brightness match the good shot |
| guest+quick | s7-quarter-b-guest-quick | passed |
| native-vulkan+quick | s7-quarter-b-native-vulkan-quick | passed; full frame (the director's 2abd9a070 shot had the corner) |
| native-d3d12-quick | s7-quarter-b-native-d3d12-quick | hang at the load's held frame (main thread sleeping in engine load code; the director's 164431 run stalled at the same milestone); not this change, which only acts in world frames |
| native-d3d12-quick | s7-quarter-c-native-d3d12-quick | passed (rerun) |
| native-vulkan+menu-scenes | s7-quarter-b-native-vulkan-menu-scenes | passed; inventory, journal, stats, world full frame |
| native-vulkan+native-river | s7-quarter-b-native-vulkan-native-river | passed |
| native-vulkan+rain | s7-quarter-b-native-vulkan-rain | passed |
| native-vulkan+upscalers | s7-quarter-b-native-vulkan-upscalers | passed; DLSS, FSR, TAA, XeSS full frame |
| native-vulkan+cadence-drop-cells | s7-quarter-b-native-vulkan-cadence-drop-cells | passed (cell changes) |
| native-vulkan+map | s7-quarter-c-native-vulkan-map | passed; map and return to the world full frame |

The detector numbers are not affected: the change is one CS-owned engine-global write on the game side of the frame, no
engine render call.

## Open items

- cs-test passed every broken run because nothing compares shots with a reference. A check that the world fills the frame
  (or a shot diff against a kept reference) in `native-vulkan+quick` would have caught this at the water merge.
- Native readers of `dynamicResolutionLock` (ImageSpace's three, `Util::ConvertToDynamic`, LegacyGraphicsCompatibility) read
  a CS-written engine global at record time. They should take full resolution from the frame (`FrameCamera`) in phase 7.
- Race menu shot: the helmet looked a little softer than in the good shot. Likely texture streaming at shot time; not followed.
