# perf3-se: first SE 1.5.97 runtime test (round 3)

Lead `se`, branch `perf3/se` (from org_integration 64374291a).

## Setup

- Game: `D:\Skyrim installations\1.5.97` (SkyrimSE.exe 1.5.97, SKSE 2.0.17 for 1.5.97). MO2: the portable instance
  `D:\MO2 installations\MO2 1.5.97 Dev`, profile Default.
- Runs go through cs-test unchanged, pointed at the SE instance by its path variables. The lab folder is separate so the AE
  lab's mod copies and shader cache are never touched; the locks are the shared ones, so SE runs take an ordinary lab slot:

  ```
  CS_TEST_MO2='D:\MO2 installations\MO2 1.5.97 Dev' CS_TEST_LAB='D:\CSLab-SE' CS_TEST_BACKGROUND=1 \
    python <main>/tools/cs-test/cs_test.py run guest --stage p3-se --wait 3600 --label p3-se-<x> --set 'steps=[...]'
  ```

  The SE instance has no Start On Save, so the steps coc from the main menu (`coc Whiterun`, later `coc WhiterunDragonsreach`),
  hide the CC Survival message box, set 13:00 and clear weather, and take a shot in each. The script is
  `<scratchpad>/se/run.sh`.
- Stage `p3-se` is the mod `CS AB p3-se` in the SE instance's mods (cs-test's stage, base AIO `CS AB nfg`), with nfg's DXVK,
  `sl.fsr.dll`, `sl.xess.dll`, `textures/PhysicalSky`, and the AE instance's `SettingsUser.json` so both runtimes use the same
  settings.
- Decided by the agent, to confirm: the mod is named `CS AB p3-se` (cs-test's naming) rather than `CS ORG SE test`, and it is
  not added to the SE mod list: lab runs build the game's Data from the profile plus the staged mod, so the mod list does not
  need to change. The old `Community Shaders` mod was already disabled.
- Profile change: `*TerrainHelper.esp` added to the SE profile's `plugins.txt` and `loadorder.txt` (the AIO's plugin; without
  it loads hang). Restored at the end.

## Address audit (before running)

Every SE id and offset added since 742af7870 was checked in Ghidra (`/skyrim/se`):

- Shadow masks: the two call sites (SE 100422+0x90 calls 0x1412e3b80, 51855+0x466 calls 0x1408d2d70), and every global
  (scene shadows, camera, filter mode and bits, Poisson radius, map resolution, far parameter, the three bias scales, render
  mode, slice, world node, both blend tables, render targets, SetCurrentRenderTarget/DepthStencilTarget, RestoreClearColors,
  RestoreTechnique) appears in the SE function the port follows.
- Sky: the sky batch (99938+0x247) and cloud group (99938+0x284) calls match AE's instructions; the sun occlusion query
  (99958) calls the three query functions 75565/75567/75568.
- ImageSpace native world and post chain: every `At<T>(se, ae)` and `RelocationID(se, ae)` pair has the same readers and
  writers on both runtimes (xrefs compared by function and access; the differences are names Ghidra gives differently, and
  extra inlined reads).
- Engine zones are AE-only by design (they refuse on other runtimes).

After merging org_integration 69a1d2183 (perf3/shadow): the native shadow maps' new SE pairs were checked the same way.
The three call sites (Main::Draw 35560+0x1C4 calls CalculateAndDrawShadowCasterLights 100414; 100418+0xBD and 100419+0x271
call the keep-light function 99728) hold those CALLs, and the six globals (free shadow-map mask, volumetric copy, focus target
base, current accumulator, graphics state, SetCameraData) have the same readers and writers as AE's.

## What broke and the fixes

1. `ded74146a` fix(pp): the branch did not build without Tracy zones (`ZoneText` is empty there, C4189 on the step name).
2. `18c51a8a6` fix(cs-test): the lab helper `!CSTestLab.dll` exported only `SKSEPlugin_Version`. SKSE for 1.5.97 loads only
   plugins with `SKSEPlugin_Query`, so the helper never loaded: no home redirect, the game read the user's Documents and never
   saw the run's test file (the harness then reports "crashed, no status file"). It now exports both. The main checkout's
   helper build (`tools/cs-test/lab/plugin/build`) must be rebuilt (`build.cmd`) after the merge before SE runs work from it;
   until then point `CS_TEST_LAB_HELPER` at a rebuilt copy.
3. `f80db6f9c` fix(compat): "Legacy FullScreenBlur stage-count opcode contexts do not match" on SE. On 1.5.97 the full-screen
   blur's deleting destructor (101570) calls Shutdown (101562) for its loop; AE's destructor (108568) has its own copy. The check
   looked for the loop in SE's destructor, failed, and installed no blur adapters (the 1.7 blur shaders then ran with 10 CPU
   stages instead of 11). On SE it now checks the call and patches the two loops. Upstream code, older than the rounds.
4. TressFXSSE (enabled in the SE profile) crashed the game at startup, right after kPostPostLoad, executing at an address in no
   module (`R11 = STATUS_DLL_NOT_FOUND`; its log: "Could not acquire MXAO dll", "Installed render startup hooks"). It is a D3D11
   renderer of its own, outside what guest or native mode can carry. Decided by the agent, to confirm: SE runs turn it off
   (`game.mods_off`), like native runs turn off ImprovedCameraSE; nothing in the profile changes.
5. A cold shader store takes far longer than the 600 s compile timeout with other leads' games and builds on the machine
   (Lighting pixel shaders 8-50 s each, 189 of 3301 in 6 minutes). The AE lab's shared store (`D:\CSLab\ShaderCache`) was
   copied into the SE lab. Most variants still missed: the SE game folder ships its own `d3dcompiler_47.dll` (2019), and the
   FXC variant key holds the compiler's identity. One run with a long compile timeout (`p3-se-guest4-cold`, 120 s of compiling)
   filled the SE store; later runs hit it (the key holds the relative shader path, so every SE slot shares it).
6. Found while testing, not SE-specific, not fixed (outside this item): a fatal "revision N's staged builds ... were refused at its
   adoption: the host's configuration changed since its build started" (`p3-se-guest6`). The profiler's timestamp switch
   (`FollowProfilerTimestamps`: on while the profiler is in use, back to the startup mode after 300 idle frames) bumps the
   graph host's configuration (`GraphHost::SetRecordingTimestampMode`), which refuses every build in flight, and a revision
   that had staged the CS segments' builds (epochs 10-19) was then refused at adoption, which is fatal. It hit when a coc
   started a load right as the profiler went idle (main menu + 120 frames). Every run toggles the profiler at start-up, AE too.
   The SE runs wait 450 frames at the main menu before their coc to stay clear of it.

## Runs (cs-test, stage p3-se, Dev-Fast)

| Config | Label | Result |
|---|---|---|
| guest | p3-se-guest1 | crashed: helper not loaded (fix 2), TressFXSSE crash (4) |
| guest | p3-se-guest2, guest3 | killed: cold shader store (5) |
| guest | p3-se-guest4-cold | passed (Survival prompt over the shots) |
| guest | p3-se-guest5 | passed (prompt still open) |
| guest | p3-se-guest6 | hang: the timestamp-switch fatal (6) |
| guest | p3-se-guest7, diag1 | passed; Whiterun exterior and Dragonsreach |
| guest | p3-se-guest8 | never-loaded (see open items) |
| guest | p3-se-guest9-nots | passed (Texture Streaming off) |
| d3d11 | p3-se-d3d11-1, d3d11-2 | killed: the second coc never started a load in plain D3D11 mode |
| d3d11 | p3-se-mksave | passed; saved SETest |
| guest | p3-se-g10, g11, g12, g13 | passed, from SETest |
| native-vulkan | p3-se-nv1, nv2, nv3-novb, nv4-vb | passed |

Runs from g10 on are the merged build (org_integration 69a1d2183, with perf3/shadow's native shadow maps). No crash, no
device loss, no address or hook refusal in any SE log; the only errors are the ones AE logs too (Effects11's missing
enbeffect.fx, the menu scenes' engine draws while their programs build).

## Shots

- Riverwood from SETest: guest (g11, g12, g13) and native Vulkan (nv3, nv4) match the plain D3D11 shot of the same save
  (`p3-se-mksave`) by eye; the scene is the save's foggy weather. AE's Riverwood (`1122-050842-nfg-native-vulkan-terrain-shadows-st4b`)
  is the same view with the AE profile's grass and tree mods, so only the lighting is comparable; nothing stands out.
- Dragonsreach (guest7, g10, nv1, nv2): lit and complete. It is softer than AE's (`1120-212334-nfg-guest-imagespace-transitions-t5`):
  a depth-of-field-like blur. Decided by the agent, to confirm: left as is; AE's profile has image-space mods, and the SE shot
  comes after the blur adapter fix (3), so the next step is an AE/SE pair with the same image space data.

## Open items

- Exterior "broken world" state, seen in guest (guest7, g10) and native Vulkan (nv1, nv2) on SE, intermittent (g11-g13, nv3,
  nv4 are fine with the same save): a clear sky, the near world gone, the distant terrain unlit black, the HUD showing the LEVEL
  bar instead of the compass. AE's imagespace-transitions exterior shot (t5) shows the same LEVEL bar and missing near world,
  with lit distant terrain, so the state is not SE's own; the black terrain may be. Not tracked down: VRAM Budget off (nv3) and
  on (nv4) both looked fine, and its controller logged no purge in nv2.
- `p3-se-guest8`: after a main-menu coc to Riverwood with Texture Streaming on, DCLF's load hold waited on 2376 objects'
  materials ("a streamed texture without its full window yet") with no progress for 60 s, then a re-entry for the precipitation
  occlusion map let the engine draw it, and its depth clear was a frame tap error (fatal). Loading the SETest save at the same
  place with streaming on worked (g10-g13). Not reproduced a second time.
- Plain D3D11 mode on SE: a console coc during play never started a load (d3d11-1, d3d11-2); the main-menu coc did.
- Terrain Helper on SE: "LandscapeDefault EDID texture set from TerrainHelper.esp not found. Terrain helper is disabled." AE
  finds it (AE has an editor-id cache plugin; SE's po3 Tweaks setup may not load editor ids).
- SE native D3D12 not run (ORG and BasicRHI untouched).

## Modlist and files

- SE profile: `plugins.txt` and `loadorder.txt` restored from the copies taken before the runs; `modlist.txt` never changed.
- Left in place for reruns: the mod folder `CS AB p3-se` (not in the mod list), `D:\CSLab-SE` (lab, shader store, documents
  with SETest). The AE instance, `D:\SteamLibrary` and the user's saves were not touched; the first (helperless) SE run wrote its
  logs into the user's Documents SKSE folder (CommunityShaders.log, CrashLogger) before fix 2.
