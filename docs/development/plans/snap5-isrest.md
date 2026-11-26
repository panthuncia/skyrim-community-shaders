# Snapshot roadmap phase 5: the ImageSpace rest (lead `isrest`)

Branch `snap5/isrest` (base `org_integration` f23e4f779), stage `s5-isrest`. Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md),
phase 5. Follows [snap5-imagespace.md](snap5-imagespace.md). Decisions 15, 50 and 54-59 in
[render-thread-decisions.md](render-thread-decisions.md).

## 1. `taaReset`

**Who sets it.** `taaReset` (SE 513663 / AE 391591) has three references in AE: TAA's helper (AE 107270) reads and clears it, and
`LoadingMenu::ProcessMessage` sets it on kShow and on kHide. Nothing in CS set it. When TAA's helper sees it, it sets TAA+0x30 to
0x3f and calls AE 0x1414d8e20, which clears the accumulation targets (0x51-0x57: TAA history, the water pair, ...) to zero.

**HistoryBreak does not quite cover it.** Decision 50's `Snapshot::HistoryBreak()` fires for `LoadStarted` (a loading frame, no TAA)
and `FirstAfterLoad`. The loading menu's kHide can come frames after `FirstAfterLoad` (the menu fades out over world frames), and
vanilla resets again then. So the flag goes through the snapshot itself:

- Game side (`CaptureBlock`, NativeWorld.inl): in any native snapshot, unless another mod keeps the end-of-frame effects on the engine,
  the capture takes the flag (clears it) and bumps `Block::taaResets`.
- Render side (`NativePost::TaaResetDue`, exposed as `ImageSpace::TaaResetDue`): the first native world render after the count moves
  returns true, once. `State::ShouldResetHistory()` ORs it in, so `SharedData::ResetHistory` is set for that render: CS's
  ISTemporalAA takes its reset branch (current frame as history, feedback luma reset) and the upscalers reset too.
- The render side no longer clears the engine flag in native (guest still does).

Native TAA already reset on `HistoryBreak` through `SharedData::ResetHistory`; snap5-imagespace's note that native TAA applied no
reset was about the engine flag only.

Run: `s5-isrest-a-nvk-save` logs `TAA history reset from the game's request (count 3, snapshot 60, render frame 805)` after the
first load and `(count 5, snapshot 945, ...)` after the save's load.

## 2. The remaining world steps taken from the engine

Not ported. What I found, so the next lead can start from it:

- **The no-raster steps (ISMap 42, ISNoiseScrollAndBlend 95, ISNoiseNormalmap 96).** Their vanilla vertex shaders write `POSITION`,
  not `SV_Position`, so their draws rasterize nothing; CS's ISNoise.hlsl reproduces that. There is nothing to draw and no constants to
  compute: the port is that native never calls them. Their callers are the water update (TESWaterSystem, the engine's render half,
  phase 3) and the map menu (menu scenes). Nothing to do in ImageSpace.
- **The image-based lens flares (with ISDownsampleCS).** CS forces IBLF off at init; only the console's `ToggleIBLF` turns it on. A
  frame with it runs the composites family on the engine (`CompositesFamily`), whose helper SE 100481 / AE 107199 (0x1414d21b0) draws
  about 30 steps with explicit setters. RE so far (AE):
  - Explicit setters: ISDownsample 108765 (0x44 dwords: row 0 = 1/w, 1/h, count (uint), compensate jitter (uint); rows 1-16
    offset/weight), ISBlur 108764 (0x4c dwords: row 1 x = 1.0, row 2 x = tap count, taps from row 3), ISIBLensFlares 108766 (0x10
    dwords). The sums are `RenderEffect(35)` with a param (local_118, flag 1) and blend 5.
  - Downsample taps (rdata): first downsample rows 1-4 = 0x141aba790, 770, 780, 760 = (-1,-1,.25,0), (-1,1,.25,0), (1,-1,.25,0),
    (1,1,.25,0), count 4, compensate 1; lights -> 16x4 rows = 0x141abbf10, f00, ee0, ef0 = (-1.5,0,.25), (-0.5,0,.25), (0.5,0,.25),
    (1.5,0,.25), count 4; 16x4 blur -> 32x4 rows = 0x141abbf30, f20 = (-0.5,0,.5), (0.5,0,.5), count 2.
  - ISDownsampleCS: the CS b0 is a 0x130-byte struct (FUN_140e4ad40), the downsample struct plus two rows of dynamic-resolution values
    (AE 0x14328cd24-30, lock 0x14328cd38) less an INI offset (0x14202b438 / 0x14202b450); groups (w * ratio * 4 + 0x3ff) >> 10 by
    h * ratio.
  - Flares rows (struct at rsp+0x40): flares0 row 0 z = 391349, row 2 = 1/w, 1/h of the 4x4 ping, row 3 = (1, 391360); flares1 row 0
    z = 0, row 1 x = 415057, row 3 = (0, 391360).
  - Blurs: radius (clamped 1-7) from 391355 / 391356 / 391357, weights from the table 391364 (0xF0 bytes per radius, 0x141abb4f0 for
    radius 8), weight scale sqrt(415056) / sqrt(391358) / sqrt(391359), loop count 391419, the raster/compute switch 391417.
  - Ping-pong by `gFrameCounter & 1`; a first-use flag (+0x31) clears the history targets and creates them (AE 0x1414d3d40).
  - Native needs: these globals captured into the ImageSpace block (SE ids from SE 100481 in reference order), `IblfSteps` with rows,
    the first-use clears as CS clears, then observe (compare with the engine's latched rows in IBLF frames; the `iblf` config) and
    switch. Decided by the agent, to confirm: deferred, since IBLF is off unless the console turns it on and the port is a multi-run
    job; items 1, 3 and 4 came first.
- **Other engine paths in a native world frame** (from the code, for the record): the end-of-frame family goes to the engine while
  the world map (slot 0) or FXAA (slot 2) is active (decision 7); the composites family while SIL is on; a slot effect another mod
  hooks goes through `RenderEffect`; the water displacement (31385 / 32176) runs the engine's container and CS takes its draws with
  the engine's constants; `RunBody` calls the engine's lens flare gather (106988) and VL apply/blur (CS's detours). The containers
  RenderSSAO, RenderReflections, the water blend, the snow copy and RenderPostProcess are entered by the engine's render half (phase 3).

## 3. PrintScreen, frame capture and the save screenshot

**PrintScreen (S24, decision 15).** RE (AE): Main::DrawFrame (36555) consumes MenuControls' screenshot handler +0x10 (52210); if set it
copies kFRAMEBUFFER into 0x4C and back with `RenderEffect(ISCopy)` x2, then calls the screenshot function 36853 (SE 35882), which
names `%s%d.png` from sScreenShotBaseName and iScreenShotIndex, calls `Renderer::SaveRenderTargetToFile(0x4C)`, shows "ScreenShot:
File '%s' created.", moves the index and saves SkyrimPrefs.ini. 36853 is also called by the capture mode (the game timer, AE 36591,
TGA every frame while the toggle 52211 is on) and Papyrus's `Debug.TakeScreenshot`.

Native (Screenshots.inl):
- The capture takes the handler's +0x10 before DrawFrame sees it and calls 36853 itself (PNG). So DrawFrame's two copies and its
  36853 call are not entered.
- 36853's call of SaveRenderTargetToFile (SE +0x73, AE +0x69) is replaced in native: it queues the file (absolute, against the game
  folder). The engine keeps the rest: HUD message, index, INI save. One hook covers PrintScreen, the capture mode and Papyrus.
- The capture moves the queued files into the ImageSpace block (`screenshots` count, the newest four in `screenshotPaths`). At each
  render frame's Begin the render side hands every new one to the Screenshot feature (`RequestCapture`), which saves the next
  Present's frame in the feature's format.
- The capture toggle (52211) stays in DrawFrame: game-side state only (folder, fixed timer delta), no render call.

**Save screenshot (S15).** RE (AE): the request is 36857 (a global), DrawFrame's 36859 moves it to manager +0x211 and marks the shot
ready (36858) once +0x218 is non-zero; the save pump (35772) re-queues a request with bit 29 (quicksave 0xF0000200, autosave
0xF0000040; queued by 35769) until it is ready. Native (Screenshots.inl, NativePost.inl):
- The capture takes +0x211 as `Block::saveShots` (one request in flight; +0x211 stays set meanwhile, so nothing re-asks).
- The render side's end-of-frame effects copy the frame into kSAVE_GAME_SCREENSHOT while the snapshot's count is ahead of the last
  copy, and store the count in `g_saveShotsDone`.
- The next capture sees the count reported back and completes the request as RenderEndOfFrameEffects did (+0x211 cleared, +0x218
  the frame counter). DrawFrame's handshake then marks it ready and the pump writes the save, one game update later than vanilla.
- Guest writes +0x211/+0x218 at render as before.

Test commands (any mode, not in the Release profile): `call imagespace.printscreen`, `call imagespace.save=quick|auto`. Configs:
`save-screenshot`, `vanilla-screenshot` (native only). `tools/cs-test/essshot.py` reads a save's screenshot (size, channel means,
optional PNG).

## 4. `sky-native` hang

Does not reproduce on f23e4f779: `s5-isrest-a-sky-native` passed, `coc Riverwood` with timescale 0 loaded in 4.3 s, every shot
names a new snapshot. In snap5-imagespace's run the main thread sat in that load for 69 s while the steps ran on the serving thread's
presents (every shot "asked at snapshot 683"); steps now count game updates and run at the update's first Present (decision 51), so
they no longer run, and issue console commands, during a load. Decided by the agent, to confirm: not pursued further.

## Runs (stage `s5-isrest`, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+save-screenshot (with PrintScreen then) | s5-isrest-a-nvk-save | passed; TAA reset twice; ScreenShot47.png saved by the feature; quicksave written (the slot was reused before I read its screenshot) |
| sky-native | s5-isrest-a-sky-native | passed; no hang |
| guest+save-screenshot (with PrintScreen then) | s5-isrest-b-guest-save | hang: guest's PrintScreen copy from kFRAMEBUFFER is a fatal tap error ("ISCopy ... RenderEffect 34 from rt0 into rt76 ... has no CS pass", MenuSteps.inl:441), unchanged by this branch; PrintScreen moved to its own native-only config |
| native-vulkan+save-screenshot (quick, then auto) | s5-isrest-b-nvk-save | passed; save screenshots 1-6 completed; the quicksave is written with its screenshot (320x192, means 69.7/65.0/50.0); the autosave was dropped by the pump |
| guest+save-screenshot (quick, then auto) | s5-isrest-b-guest-save2 | passed; both written (means 70.0/65.0/49.8, 70.2/65.2/50.2) |
| native-vulkan+vanilla-screenshot | s5-isrest-b-nvk-vanilla | passed; ScreenShot47.png saved by the feature |
| guest+quick | s5-isrest-c-guest-quick | failed: a second load began 20 s after world-ready and the shot step ran on the serving thread during it (capture lost); not this branch's code (guest runs none of it); passed on the rerun s5-isrest-d-guest-quick |
| native-vulkan+quick, native-d3d12-quick | s5-isrest-c-nvk-quick, -c-d3d12-quick | passed |
| native-vulkan+post-effects | s5-isrest-c-post-effects | passed |
| native-vulkan+imagespace-transitions | s5-isrest-c-transitions | passed |
| native-upscalers | s5-isrest-c-upscalers | passed (dlss, fsr, taa, xess) |
| native-vulkan+screenshot-clean | s5-isrest-c-, -d-screenshot-clean | device lost after the window resize step (Timeline::HostWait); the same on the base f23e4f779 (s5-isrest-e-base-screenshot-clean): pre-existing |
| native-vulkan+vanilla-screenshot, CS_SNAPSHOT_DETECT=1 | s5-isrest-c-nvk-vanilla-detect | passed; with PrintScreen, the detector counts no `RenderEffect #2/#3` and no `AE 36853` in Main::DrawFrame (only the input helpers 52210/52211, game-side) |
| native-vulkan+save-screenshot (auto, then quick) | s5-isrest-d-nvk-save | passed; the autosave is written with its screenshot (means 70.1/65.2/50.2); the second save dropped |
| guest+save-screenshot (auto, then quick) | s5-isrest-d-guest-save | passed; the same: the autosave written, the quicksave dropped. So the pump drops the second save in either order and mode |
| native-vulkan+cadence-repeat | s5-isrest-d-cadence-repeat | passed |

## Proposed decision-log entries

- **The TAA reset is a game-side count** (like 55): the loading menu's flag is taken in the capture, and the first native world render
  after the count moves resets histories through `State::ShouldResetHistory`.
- **The vanilla screenshot is a snapshot event** (15 built): PrintScreen's request is taken in the capture and the engine's screenshot
  function runs there; its SaveRenderTargetToFile only records the file; the Screenshot feature saves the frame, in its own format,
  under the vanilla name. DrawFrame's PrintScreen copies are not entered in native.
- **The save screenshot is a request count with a render-side report** (S15): copy on the render side, completion on the game side at
  the next capture, one update later than vanilla.
- **No-raster ImageSpace steps need no port**: they draw nothing; their callers go with phase 3 and the menu scenes.
