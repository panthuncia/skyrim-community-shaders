# snap3-pov: first person after a pov switch (snap3-skeleton's open item)

Lead: `pov`, branch `snap3/pov` (base `org_integration` 8900d1f2c), stage `s7-pov`. Decisions 143, 170, 192-194.

## Cause

Engine-side, Main::RenderPlayerView (AE 0x1406444b0) tests the first-person root's hidden bit (`Get3D1(true)` +0xF4 bit 0) on entry,
**before** Main::SetupSunAndSky. Inside SetupSunAndSky the water update (TESWaterSystem update, AE 32174) runs the reflection cube's
update (AE 32160, 0x140520570). When it refreshes the cube, that update hides the player's current 3D (vfunc +0x380, Get3D2: the
first-person root in first person). It shows the root again only when the third person is visible (vfunc +0x388). In first person
the root is left hidden: RenderPlayerView's first-person block shows it later in the same frame.

Native runs the water update's game half in the capture (`WaterEffects::Capture`, inside `Snapshot::Capture`), before Main::DrawFrame.
The skeleton driver then read the live bit after the water update had hidden the root. So after a switch from third person the test
was false, the block never ran, and nothing showed the root again. Each frame stayed "not first person". Main::ClearMasks was not the
cause: it shows a different global (0x14338c8d8), and only from inside the block.

An observe run (`s7-pov-a-obs-native3`; probes at the capture's steps, scaffolding removed) showed the bit go from shown to hidden
across `WaterEffects::Capture` on the switch frame. Guest kept it shown up to DrawFrame (`s7-pov-a-obs-guest`). A load straight into
first person works when the first frames do not refresh the cube.

## Fix (`986fbfb03`)

- `FrameSnapshot::firstPersonShown`: the first-person root's bit, set in `FillCore` before `WaterEffects::Capture`, where
  RenderPlayerView takes it (before SetupSunAndSky). Frame-only snapshots copy it.
- `Native/RenderScene.cpp`: `ReadBranches` takes the snapshot's bit instead of the live one. The main menu and free camera tests are
  still read live, as before.
- DCLF `FirstPersonLatch` (`FirstPersonView.cpp`) takes the same bit, so DCLF's first-person depth and colour epochs latch again.
  In guest the latch runs at SetupSunAndSky's entry, before the water update, so its value is the same there (observed equal).
- New cs-test config `first-person-switch` (first, third, first, third then first, then a coc into Dragonsreach in first person).

DCLF's first-person stats (`CS_DCLF_STATS=1`, 300-frame windows) after the fix, native vs guest:
first-person-switch native 186/156/240/300/300 latched frames with as many depth and colour epochs, guest 210/151/240/300/300
(the gaps are the third-person windows). Before the fix the latch read the hidden bit after the water update, so it stayed false after a switch (inferred from the observe runs; not counted).

## Runs (stage `s7-pov`, Dev-Fast, `env.CS_SNAPSHOT_DETECT=1` on the c- runs)

| Config | Label | Result |
|---|---|---|
| native-vulkan+first-person, observe | `s7-pov-a-obs-native`, `-native2`, `-native3` | passed; cause found |
| guest+first-person, observe | `s7-pov-a-obs-guest` | passed |
| native-vulkan+first-person-switch | `s7-pov-b-native-switch`, `-stats` | passed; first person drawn in every first-person shot |
| guest+first-person-switch | `s7-pov-b-guest-switch`, `-stats` | passed; matches native |
| guest+quick | `s7-pov-c-guest-quick` | passed (65 render calls, guest) |
| native-vulkan+quick | `s7-pov-c-native-vulkan-quick` | passed, 1 render call/world frame, 668 live reads |
| native-d3d12-quick | `s7-pov-c-native-d3d12-quick` | passed, 1 render call |
| native-vulkan+first-person | `s7-pov-c-native-vulkan-first-person` | passed, 1 render call, 567 live reads |
| guest+first-person | `s7-pov-c-guest-first-person` | passed |
| native-vulkan+native-interior | `s7-pov-c-native-vulkan-native-interior` | passed, 1 render call, 509 live reads |
| native-vulkan+screen-blood | `s7-pov-c-native-vulkan-screen-blood` | passed |
| native-vulkan+cadence-drop-cells | `s7-pov-c-native-vulkan-cadence-drop-cells` | passed, all three shots, 3 calls (drop:3, as before) |
| native-vulkan+menu-main-load (load into first person) | `s7-pov-c-native-vulkan-menu-main-load` | passed |

Detector: engine render calls per world frame unchanged (1). Live reads unchanged within scene noise (skeleton: quick 642, interior
510); DCLF's latch no longer reads the root's bit live.

## Decided by the agent, to confirm

- POV-a. The first-person test's root bit lives in the core snapshot (`firstPersonShown`) instead of a game-side static: phase 7's
  split moves the rest of `ReadBranches` there too.
- POV-b. DCLF's `FirstPersonLatch` reads the snapshot's bit in guest too (same value there, observed), rather than branching on mode.

## Proposed decision-log entry

- **Native's first-person test takes the root's bit before the water update:** the reflection cube's update (AE 32160) hides the
  first-person root in first person and leaves RenderPlayerView's first-person block to show it. Native runs that update in the
  capture, so the snapshot takes the bit first (`FrameSnapshot::firstPersonShown`, in FillCore). The skeleton driver and DCLF's
  first-person latch read it. First person after a pov switch runs the first-person block, its colour point and depth range, as
  in guest. Closes 194's open item.

## Open items

- **The third-person root after a switch to first person differs:** guest leaves the third-person root shown, native hides it. A
  probe on PlayerCharacter::SetFirstPerson (AE 40476, 0x140736150) and its callees (`s7-pov-d-tp-guest`, `-d-tp-native-vulkan`):
  the store at 0x140736237 (`MOV [R14+0xF4], ECX`, an SWF-patched hidden store) hides it in native. In guest the bit is already
  clear by the next call (the audio swap, AE 67700). So the difference is between that store and the audio call, which is only the
  SWF stub and its synchronous hidden-event subscribers. Not found. It decides whether the third-person body exists for the
  shadow casters in first person (the player's own shadow in first person), so it is worth its own look.
- SE was not run (the fix adds no address).
