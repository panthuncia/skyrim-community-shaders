# Snapshot roadmap: three older bugs (lead `bugs`)

Branch `snap5/bugs` (base `org_integration` e2168742a), stage `s5-bugs`. Found by snap5-isrest ([snap5-isrest.md](snap5-isrest.md)),
listed in decision 80 of [render-thread-decisions.md](render-thread-decisions.md).

## 1. Guest PrintScreen was fatal

**Cause.** Main::DrawFrame (AE 36555) consumes PrintScreen's request and copies kFRAMEBUFFER into kSCREENSHOT (rt76) with two
`RenderEffect(ISCopy)` calls before the engine's screenshot function (AE 36853) saves rt76. No CS pass covers those copies, so
in guest the first one was a fatal tap error ("ISCopy ... RenderEffect 34 from rt0 into rt76 ... has no CS pass"). snap5-isrest had
replaced this path in native only.

**Fix.** Guest takes native's route, which fits its model: guest runs the same snapshot captures at Main::Update's call of DrawFrame
and adopts at Renderer::Begin on the same thread.
- `CaptureScreenshots` (Screenshots.inl) runs in both graph modes: it takes the handler's request before DrawFrame sees it and calls
  36853 itself; the files go into the ImageSpace block. The save screenshot handshake in it stays native only (guest's end-of-frame
  effects still complete +0x211/+0x218 at render).
- The SaveRenderTargetToFile thunk in 36853 is installed in both graph modes, so PrintScreen, the capture mode and Papyrus's
  `TakeScreenshot` all go to the Screenshot feature in guest too.
- `RenderScreenshots` reads the block from the adopted snapshot in either mode (not `NativeBlock`).
- `vanilla-screenshot` now composes with guest and expects no `frame tap error`.

## 2. The save pump wrote only the first of two saves

**Not the pump, not CS.** The save in `save-screenshot` has a hostile Whiterun guard. A few seconds in, the "Health Low" tutorial
message box opens and pauses the game (`[FGTEST] ... paused true` from then on; the `saved` shot shows the box). `Save_Impl`
(AE 0x14060fd40) refuses to save while it is up (its UI check), silently for an autosave and with "can't save" for a quicksave.
Whichever save came second fell after the box opened. The screenshot handshake was fine: native completed all its save
screenshots (3 round trips for a quicksave, which counts to 2 before saving, 2 for an autosave).

RE notes (AE): the pump 35772 re-queues a bit-29 request while the Mist Menu is open, else asks for a shot (36857 sets a flag) until
ready (36858); DrawFrame's 36859 turns the flag into +0x211 and marks ready once +0x218 is set. Ready is cleared each DrawFrame
unless a menu with kFreezeFrameBackground is open.

**Fix.** `save-screenshot` starts with `tgm` (the standing rule for hostile saves).

## 3. Window resize lost the GPU device (native)

Not specific to `screenshot-clean`: plain `native-vulkan+window-resize` lost the device too on the base (it last passed on
2026-11-20). Validation (`--set org.validation=true`, with the command buffer labels printed) showed render areas of 1920x1080 on
1280x720 attachments. Since engine targets moved into placed/aliased memory (the frame transient and placement work of
2026-11-21/22), those writes past a target corrupt its neighbours, which shows up as a fault or a hang in a later pass
(`sao.minify`, `cs.dclf.main-opaque`). Before that they landed in a target's own allocation and went unnoticed.

Causes found and fixed (all in the ImageSpace step groups, commit "size ImageSpace steps for their revision"):
1. **Step extents from the adopted version.** `MakeStepSpec` sized an engine output from the import's current version, and the
   builds read kMAIN's size from the engine's D3D11 texture. A registration is made for the revision that brings the new targets,
   so after a resize every world scope kept 1920x1080 render areas on 1280x720 targets, for good. Outputs now take the
   revision's `EngineTargetShapes` (HDR Display's scene texture: the revision's display size); `ContrastLevel`, the chain's and
   the menu's sizes and `MakeTransientLike` too.
2. **Kept and owned textures from the engine's texture.** `OwnedDescLike` read the engine's D3D11 texture, which is the adopted
   version. Growing back to 1920x1080, SAO, TAA history and bloom were made at 1280x720. It now takes the shape from the
   revision's values (the declaration's inputs, or the build's).
3. **Kept builds never re-imported the engine's own targets.** kSAO_CAMERAZ (and its mip chain) is engine-owned: an import that
   takes the engine's new texture only when imported inside the adoption point. A new step build imports it as it makes its
   specs; a kept build (`StepGroup::For`, keyed by display size, so growing back to the first size reuses the first build) made
   no import, so kSAO_CAMERAZ stayed 1280x720 while the passes wrote 1920x1080. `For` now imports a kept build's engine targets.
4. The start steps (water, underwater mask) were not keyed on the display size; the underwater mask is display-sized.

None of this is in `Hooks.cpp` (the prep lead's window proc and input hooks).

**Still open: an intermittent hang in `cs.dclf.main-opaque` after a resize** (2 of 9 resize runs with the fixes: native Vulkan
`s5-bugs-i-nvk-wr`, native D3D12 `s5-bugs-k-d3d12-window-resize`; the D3D12 rerun passed). Both hung in the window between the
revision that adopts the new engine targets and DCLF adopting its main resources for the new size (DCLF runs its old-size scene
for about 250 ms there, `FollowDisplaySize`). The device fault lists shader instruction pointers (waves stuck, a TDR), not a page
fault; DRED points at a main-opaque ExecuteIndirect. Validation shows no render-area or bounds error there. Decided by the agent,
to confirm: left open, since it is in DCLF's resize hand-over, not in the three bugs' paths; next step is GPU-assisted validation
or making DCLF's main resources a member of the resize revision.

Other findings, not fixed:
- Before a stage's new build is staged, ORG records the stage's old build against the revision's pending versions (and moves its
  declarations onto them, ORG e20aa19). With validation this shows as render-area errors in `cs.upscaling.*` and `cs.pp.output.*`
  (old 1920x1080 key on 1280x720 versions). Those recordings are dropped when the new build switches in; the runs pass.
- FSR (FidelityFX through Streamline) destroys an image view still in use when it remakes its context on a resize
  (`VUID-vkDestroyImageView-imageView-01026`).
- `window-resize` and `screenshot-clean` have no `tgm` either; their later shots show the Health Low tutorial over a dimmed frame.

## Commits (branch `snap5/bugs`)

- `a0a245792` fix(org): guest PrintScreen takes native's route
- `39a0086cd` test(cs-test): tgm before save-screenshot's saves
- `7f4cc565b` fix(org): size ImageSpace steps for their revision
- notes (this file)

## Runs (stage `s5-bugs`, Dev-Fast)

Configs from this branch were passed by path (the main checkout's copies are older).

| Config | Label | Result |
|---|---|---|
| guest+vanilla-screenshot | s5-bugs-a-guest-vanilla | passed; ScreenShot47.png saved by the feature (1920x1080, means 70/65/50), no tap error |
| guest+save-screenshot | s5-bugs-a-guest-save | passed; Autosave2 and Quicksave0 both written, screenshots 320x192 (means 69.6/64.7/49.7, 70.2/65.3/50.3) |
| native-vulkan+save-screenshot | s5-bugs-a-nvk-save | passed; both written (70.9/65.8/50.6, 72.4/67.3/51.8); save screenshots 1-6 completed |
| native-vulkan+window-resize (base fixes 1-2 only) | s5-bugs-b-, -c-, -d-, -e-, -f-, -g-, -h-* | device lost (diagnosis runs, validation and import logging) |
| native-vulkan+window-resize | s5-bugs-i-nvk-wr | device lost at the first resize: the open DCLF hang |
| native-vulkan+window-resize | s5-bugs-j-nvk-wr, s5-bugs-k-nvk-window-resize | passed (shots 1920, 1280, 1920) |
| native-vulkan+window-resize, validation | s5-bugs-m-nvk-wr-labels | passed |
| native-vulkan+screenshot-clean | s5-bugs-k-nvk-screenshot-clean | passed |
| native-vulkan+resize-drag | s5-bugs-k-nvk-resize-drag | passed |
| guest+window-resize | s5-bugs-k-guest-window-resize | passed |
| native-d3d12+window-resize | s5-bugs-k-d3d12-window-resize | device lost: the open DCLF hang |
| native-d3d12+window-resize | s5-bugs-n-d3d12-window-resize | passed |
| guest+quick, native-vulkan+quick, native-d3d12-quick | s5-bugs-o-guest-quick, -o-nvk-quick, -o-d3d12-quick | passed |
| guest+vanilla-screenshot, native-vulkan+vanilla-screenshot | s5-bugs-o-guest-vanilla, -o-nvk-vanilla | passed |
| native-vulkan+screenshot-clean | s5-bugs-o-nvk-screenshot-clean | passed |
| native-vulkan+save-screenshot | s5-bugs-o-nvk-save | passed |

## Decided by the agent, to confirm

- Guest's vanilla screenshot is saved by the Screenshot feature like native's (its format, the vanilla name), not by the engine's
  copy into kSCREENSHOT. Why: it fits guest's single-thread snapshot model and needs no CS pass for an engine copy.
- The save pump was not changed: the second save was refused by the game (the tutorial menu), so the config gets `tgm`.
- The DCLF main-opaque hang after a resize is left open (above).
