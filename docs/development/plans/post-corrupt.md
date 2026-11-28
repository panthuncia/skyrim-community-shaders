# post-corrupt: one-frame corruption after loads, and the white squares

Lead `post-corrupt`, branch `post/corrupt` (base `org_integration` 5f8108e70; `snap7/mvec2` merged twice for testing, the second
time with b95514dcb; both are in `org_integration` now), stage `post-corrupt`. RTX 5080, Dev-Fast throughout. Runs are in
`C:\Modding\CS-AB\runs`, labelled `post-corrupt-*`.

The user's request: fix any other real corruption found, including one-frame flickers seen after loads (maybe related to the
device loss), and find what the white squares in a native cadence-drop-cells shot are.

## 1. Commits

| Commit | What |
|---|---|
| aef85be41, a113824b1, 5154937ca | test(cs-test): load and sweep bursts (section 2) |
| 8be5cc0b5 | fix(menu): no compile card without counted tasks (F4) |
| 4e766306c | fix(dclf): hold guest presents under the kept menu (F1) |
| 3c988fb42 | fix(dclf): texture transform rows per render (F2) |
| 427ef19d0 | fix(dclf): skin material rows wait before reuse (F3) |
| (this file, README) | docs |

No submodule change. snap7-devloss8 cherry-picked 3c988fb42 unchanged (f155cd672 on snap7/devloss8): the same mismatch can leave
copy rows unwritten or past the backing's end, which matches the float heap indices of the device loss. With it and the interim wait
deleted, its native resize stress passed 5 of 5 (it lost the device about every second run before). Keep 3c988fb42 as it is so the
two merge cleanly.

## 2. Bursts in the configs

- cadence-drop-cells: a 180-frame burst after each coc, from `held-frame` (the first world frame under the kept loading menu), or
  `load-start` for the nearby `coc Whiterun` (no loading menu: the Presents are held, so the burst starts at the release). Plus a
  120-frame burst after the save's load, once the pinned steps stopped the fight.
- native-interior: after its coc in and out, and after the save's load. land-cocs: after the first two rounds' cocs.
- native-tour: after both loads. first-person-switch: through both pov switches and after its coc. menu-scenes: back in the world
  after each menu. update-cap240: through a quarter-turn camera cut at the cap.
- All with `[expect.flicker] max_spike = 20, max_tiles = 8`. Plain D3D11 has no load hold and no `held-frame`, so a d3d11 run of
  these stops at the first held-frame burst.

Learned while placing them:

- The save's guard fight is no flicker scene: swings, hits and camera shake flicker in every mode. A 240-frame burst in the fight
  scored guest 54.9 (149 tiles), d3d11 17.6 (9 tiles), native 13.2 (`post-corrupt-f-*`). So quick keeps no burst, and the save's load
  is burst after the pinned steps stop the fight, about 10 frames after world-ready. Decided by the agent, to confirm.
- A step after a burst waits until its files are written (120 PNGs take about 20 s). A burst before god mode let the guard bring up
  the Health Low tutorial, which paused the game and swallowed the next coc. A burst over about 250 frames with PNGs needs `wait_s`
  on the next step.

## 3. Findings

### F1. Guest showed the world half drawn for about 30 frames after every load (fixed, 4e766306c)

Guest's main thread draws the world under the loading menu that the load hold keeps open, and the menu does not cover it. So the
hold's frames showed the world with DCLF's classes withheld: black land, a pale untextured player, the previous cell's LOD from the
new camera (with white squares), a white-fogged interior. Native draws no world while its snapshot's loading menu is open and was fine.

Fix: guest holds the presents while the hold keeps the menu (Holding and Releasing), as a load without a menu already does. The last
loading screen frame stays until the menu closes. A burst skips frames whose present is held (never on screen; also native's nearby
cocs).

Guest native-interior load-out: spike 127.2 with 81 flickering tiles (`g-nig`) -> 0.77 and 0.96 (`h-nig`, `m-nig`).

### F2. Repeats drew animated effects from another material row (fixed, 3c988fb42)

Native at the cadence test mode's drop:3, right after loads: Dragonsreach's hearth and braziers purple-blue on the repeats of a
snapshot and orange on its fresh render (`i-cdc1` load-dragonsreach f099-f101, `j-whole1`); Riverwood's mountain mist and chimney
smoke missing on one repeat (`a-nvk-cdc` load-riverwood f082-f084, `i-cdc2`; spikes 57-79).

Cause: animated materials' draws read the frame's copy of the animated material rows, indexed by their own payload's animated
slots. The copy pass fills the copy from the texture-transforms rows (which material row each copy row holds, each entry's offset and
values). Those rows were sampled by the ticket's sampling job from the newest installed list (copied from the render thread's
`shared_ptr` on the game thread, a race) and sent as they stood; the copy's backing was sized from that list too. A repeated render
keeps its ticket's payload while later tickets install newer ones, so its draws read copy rows holding other materials, or rows
nobody wrote. Two diagnostics narrowed it: sending every row whole changed nothing (`j-whole1`, not the journal); pointing every draw
at the installed rows removed both (`k-noanim1`, `k-noanim2`).

Fix: the game half reads the entries' values at each ticket's frame start, keyed by material and component. Each render's upload
builds the texture-transforms rows for the list its own payload installed; both pools are per render (the copy's size follows the
render's list). The game half reads the installed list through an atomic. A list newer than the last frame start leaves its new
entries unpatched for one frame (the installed row values). The values are the newest frame start's, so a repeat can draw a UV scroll
up to the newest update (as before the change): decided by the agent, to confirm.

After: 5 of 5 cadence runs with every burst under 5 (`l-tt1..3`, `m-cdc1..2`), against the purple fire or missing mist in 4 of 6
runs before.

### F3. The player's arm drawn black on a fresh render after repeats (fix committed, inferred cause)

Native drop:3: the player's arm skin solid black on the fresh render after two or three repeats; the frames around it right; AI off,
player idle (`a-nvk-cdc` load-save f079, `i-cdc1` load-save f002, spike 98.5).

Cause, inferred: Advanced Skin's material table is a game-side pool sent as it stands, while each slot's shading row naming its table
row is staged per ticket. A row freed by one capture (after a load, texture relocations make every slot resolve again) was reused for
another material at once, while an older ticket's render, or a fresh render whose upload already holds a later ticket's table, still
named it. Fix: a freed row waits 16 captures before reuse (renders lag the captures by at most 8 staged tickets). Decided by the
agent, to confirm: the cause is the mechanism, not caught in the act. It did not show in the 5 cadence runs after F2's fix either
(3 before F3's commit, 2 after), so the evidence is thin both ways.

### F4. The compile card flashed "0/0" for a frame after loads (fixed, 8be5cc0b5)

"Compiling Shaders 0/0 (nan%)" showed for one frame under the loading menu: the compilation pool held tasks the compilation set
does not count. The card now shows only counted work. Shared UI code.

### F5. The white squares

The shot (1128-024517; s7-mvec's runs show the same picture) is the stall snap7-mvec2b fixed (b95514dcb): DCLF kept drawing the
exterior's publication with the interior's camera. It reproduced in 3 of my first 5 cadence runs. The same scene, Tamriel LOD from
the interior camera with small white squares, also showed in guest's held frames (F1, `g-nig` load-in f000-f017): it is the
transitional state after an interior coc that the loading menu normally hides, and the squares are drawn in it in both modes. With the
stall and F1 fixed, no burst or shot in 13 later cadence and interior runs showed a square. What exactly draws them was not identified
(no such state left to look at); open, low priority.

## 4. Seen, not fixed

- **post-f2's F2 in native Vulkan:** a repeat in Dragonsreach draws the hall in pieces over a flat floor with the sky through it
  (`m-ni` burst dragonsreach f058, spike 139.9; `m-fps` load-in f139, first person, 126.7). Both repeats (renders 2 and 3). The new
  bursts catch it: native-interior and first-person-switch fail on it until post-f2's fix.
- **A fresh frame without sunlight:** everything in shadow, about 40% darker, then several frames recovering. Once in a native quick
  burst (`d-native-vulkan+quick` f048, in the fight) and once in post-small2's Whiterun burst (1128-062224-...-ext1, f2-3 f195). Not
  reproduced since; cause open.
- **One frame of the player's pose out of step** in guest (16-18) and native D3D12 (15.8-16.2) load-save bursts, about 2 s after the
  pinned steps, under the limit; native Vulkan 0.4-2.6 and d3d11 (30 fps) 0.85 at the same place. Not chased.
- **drop:3 idle motion** scores up to about 21 (A-B-B-C holds of the breathing pose, `k-noanim1` load-riverwood f054). The cadence
  bursts can fail at the limit rarely; left at 20 so F2 stays caught.
- **update-cap240's bad "turn" shot** (director, 1128-060810): all three shots of that run share one camera, and the player stood at
  31890 -16660, not where the prelude's `player.setpos` puts him. The prelude ran at 9 fps (CPU saturated) and the fight had moved and
  pitched the camera; setpos and setangle did not take. The player is not in view although the camera looks at his position (likely
  the knocked-down camera). Harness timing as far as the evidence goes, not a render corruption.
- `j-whole2` lost the device 115 frames after world-ready (diagnostic build; the known loss, snap7-devloss8's).

## 5. Runs (stage post-corrupt)

| Label | Config | Result |
|---|---|---|
| a-nvk-cdc | native-vulkan+cadence-drop-cells (base) | failed: Health Low (burst before god mode); F2 mist, F3 |
| b-nvk-quick, d-native-vulkan+quick | native-vulkan+quick with a burst | failed: fight flicker |
| c-guest+quick, c-d3d11+quick | quick with a burst | passed 4.4, 3.8 |
| f-* | fight bursts, three modes | guest 54.9, d3d11 17.6, native 13.2 |
| d-, g-cdc | cadence (mvec2 merged) | stall (Dragonsreach drawn as the exterior) |
| g-cdc30, g-ni, g-ni30, g-niu, g-nix | cadence cap 30; native-interior 60, cap 30, uncapped, D3D12 | passed |
| g-nig | native-interior in guest | failed: F1 |
| h-nig, h-ni | after F1 | passed |
| h-nid2 | native-interior in d3d11 | stops at the held-frame burst; load-save 0.85 |
| i-cdc1, i-cdc2 | cadence (stall fix merged) | F2 fire, F3; F2 mist |
| i-lc, i-fs | land-cocs, flicker-scenes | passed |
| j-whole1, j-whole2 | diagnostic: every row sent whole | F2 fire still; device lost |
| k-noanim1, k-noanim2 | diagnostic: draws read installed rows | no F2 |
| l-tt1..3 | F2 fix | passed, every burst under 3.3 |
| m-cdc1, m-cdc2, m-cdc30 | F2 and F3 fixes | passed |
| m-ni | native-interior | failed: post-f2's F2 |
| m-nix, m-nig | native-interior D3D12, guest | passed |
| m-nq, m-gq | native-vulkan+quick, guest+quick | passed, references hold |
| m-fps | native-vulkan+first-person-switch | failed: post-f2's F2 |
| m-ms, m-tour, m-c240, m-lc | menu-scenes, native-tour, update-cap240, land-cocs | passed |

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change; native-interior ran on D3D12), guest runs of the sweep configs other
than native-interior and quick (the guest change is the present hold, which native-interior exercises).

## 6. Decisions to confirm

- PC-a. The save's guard fight is not burst; the save's load is burst after the pinned steps (section 2).
- PC-b. Guest holds the presents while the load hold keeps the loading menu, so the last loading screen frame stays (F1).
- PC-c. Texture transform rows follow the render's installed list; values come from the newest ticket frame start (F2).
- PC-d. Advanced Skin's freed material rows wait 16 captures before reuse (F3; inferred cause).

Proposed decision-log entry: load bursts in the suite found four corruptions. Guest showed the half-drawn world under the kept loading
menu for about 30 frames after every load (its presents are now held, as for a load without a menu). Native repeats drew animated
effects from other material rows (purple fire, missing mist) because the texture-transform rows followed the newest list, not the
render's payload; they are built per render now (also the device-loss trigger, snap7-devloss8). Advanced Skin's freed rows wait
before reuse (black arm on fresh renders). A "0/0" compile card no longer flashes. The white squares belong to the stalled publication
snap7-mvec2b fixed. Open: post-f2's repeat in Dragonsreach (now caught in Vulkan by native-interior and first-person-switch), a rare
fresh frame without sunlight.
