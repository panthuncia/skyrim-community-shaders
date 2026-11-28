# post-flicker: consecutive-frame bursts and a flicker score in validation

Lead `post-flicker`, branch `post/flicker` (base `org_integration` 19f55f279), stage `post-flicker`. The user's request: the water
flickers badly, and the frame-to-frame change metrics used before to find intermittent flicker should come back as part of
validation. The old method (DCLF's `dclf:shot=fNNNN` frames saved one after another, a region's mean per shot) and its scoring
script are gone; the old `flicker*.toml` configs took 60 shots that were in fact 12-14 frames apart, since each shot waited for its
readback.

RTX 5080, Dev-Fast. Runs are in `C:\Modding\CS-AB\runs`, labelled `post-flicker-*`.

## 1. The capture: the `burst` step

`do = "burst"`, `arg = "<name>:<count>[:grid]"` (3-600 frames). It captures that many consecutive rendered frames on the render side.

- `ScreenshotFeature::RequestBurst` queues it like a shot. At each Present, `StepBurst` takes one capture (`CaptureInto`, the shot's
  own path with a burst tag). It starts at the first Present that renders the step's snapshot or newer, once the readback ring is
  built.
- Graph modes (guest, native): `ORGScreenshot` copies the frame into a readback buffer at the Capture anchor, as a shot does. A
  readback takes about 8 capture points, so a burst asks for a ring of 12 buffers (`SetRing`, never fewer after that). `Ready`
  holds the start until the source's registration and build are in. A frame that cannot be taken (ring full, a capture still armed)
  is skipped and listed. It is never copied with D3D11, since native has no D3D11 content. Plain D3D11 uses a staging copy per frame.
- Labels: the commit that copies a frame records its Present (`State::frameCount`), render frame, snapshot, renders of that snapshot
  and the repeat flag (`ORGScreenshot::Frame`, carried in `Pixels`). So repeats and fresh renders are known.
- The screenshot worker reduces each frame to a 240x135 grid of cell lumas (`ScreenshotBurst::Grid`, Rec. 601, times 256) and saves
  a PNG unless `grid`. At the end it writes `burst.json` and `grid.bin` into `<run>/bursts/<name>/`. The burst counts as one
  scripted capture, so the next step waits for its files.
- The frame time before the burst (the median of the last 31 Present intervals) goes into `burst.json`. The harness compares it with
  the median interval during the burst.

Cost of capturing (median frame time during the burst / before; Dev-Fast): native Vulkan x1.01-1.16, native D3D12 x1.01-1.05,
d3d11 x1.02-1.09, guest x1.0-1.6 (guest's Dragonsreach 8.4 to 13.2 ms, Meadery 7.3 to 11.8 ms). The guest cost is the 8 MB copy out
of the readback buffer on the main thread each frame, probably; not measured further (open). Native rendering paces about the same
during a burst, so its repeats still happen; no frame was skipped in any run.

## 2. The metric: `cstest/flicker.py`

Tiles of 5x5 cells (40x40 pixels, 48x27 tiles at 1080p), 8-bit luma.

- change: a tile's mean absolute cell change between consecutive frames.
- spike: per cell and frame, the value held for 1 or 2 frames against the frames before (b) and after (a). When it is above both or
  below both: min(|s-b|, |s-a|) - |a-b|, else 0. A tile's spike is its cells' mean. Motion and eye adaptation move one way and score
  0. Noise scores a little.
- Events: one-frame spikes over 6, plus two-frame holds over 6 that are not made of one-frame spikes. A tile with two or more events
  flickers.
- Reported per burst: worst spike with tile and frame label, flickering tiles, events, noise floor, frame-mean spike, change
  statistics, the worst 8 tiles, a tile map, frames, repeats, snapshots, skipped frames, frame times. `expect.flicker`: `max_spike`,
  `max_tiles`, `min_frames`. The PNGs are cut to the first frame and those around the three worst tiles' spikes.
- `python tools/cs-test/cstest/flicker.py <run>` scores again.

Decided by the agent, to confirm: one global limit (`max_spike = 20`, `max_tiles = 8`) for every scene instead of per-scene
references. Clean modes are far below it everywhere but at the turning water wheel, and the failures seen so far are far above it.

## 3. Calibration (flicker-scenes, 60 frames per scene)

| Scene | d3d11 (d1) | guest (g1) | native Vulkan (n1, cap 60) |
|---|---|---|---|
| meadery (grass, trees, shadows) | 3.31 / 0 | 0.33 / 0 | 0.41 / 0 |
| sky | 2.82 / 0 | 0.21 / 0 | 0.33 / 0 |
| river | 14.84 / 3 | 4.51 / 0 | **53.84 / 68** |
| river-east | 2.42 / 0 | 0.58 / 0 | **56.57 / 138** |
| whiterun | 1.11 / 0 | 0.63 / 0 | 0.49 / 0 |
| dragonsreach | 0.68 / 0 | 0.49 / 0 | 0.60 / 0 |

(worst tile spike / flickering tiles). Noise floor 0.01 everywhere: with time stopped the scenes are nearly static. d3d11's 14.8 is
Riverwood's turning water wheel (x 1480-1600, y 200-280): real periodic motion at d3d11's 25-40 frames a second (the CPU was
saturated by builds for 101 s of that run). Faster modes see it move less per frame.

Runs: d3d11 1128-014338-post-flicker-d3d11-flicker-scenes-post-flicker-d1, guest 1128-014959-post-flicker-guest-flicker-scenes-post-flicker-g1,
native 1128-013519-post-flicker-native-vulkan-flicker-scenes-post-flicker-n1.

## 4. Configs

- New `flicker-scenes`: the suite's pinned prelude, then 60-frame bursts at the save (Meadery), up at the sky, Riverwood's river
  down and along it, Whiterun, Dragonsreach. New `cap30` (the update cap at 30, no steps) to compose with it.
- `flicker`, `flicker-taa`, `flicker-dlss`, `flicker-none`: rewritten. Each is the pinned prelude, the upscale method, and one 120-frame
  burst instead of 60 shots. Their `native-*` and `native-d3d12-*` wrappers are unchanged.
- `river-flicker`: river.toml's scene with two 120-frame bursts.
- `river` (native-river, native-d3d12-river), `taa-water`, `update-cap30`, `native-interior`: one 60-frame burst after a shot.
- Every one has `[expect.flicker] max_spike = 20, max_tiles = 8`. native-river and native-d3d12-river fail now, on the known water
  flicker. That is intended.

## 5. Findings (flicker beyond d3d11)

- **F1 (high): native water flickers at Riverwood with no resize at all**, in Vulkan and D3D12 and at every update cap tried (cap 30,
  cap 60, uncapped): worst spike 48-58, 68-140 flickering tiles. The river switches between a bright reflective state (mean about 83
  over x 1400-1600, y 520-640, blocky stair-step reflections) and a dark see-through one (about 58, the riverbed visible). At cap 30
  every other snapshot's fresh render is dark and its repeat bright. native-vulkan+taa-water renders one snapshot per frame (1 repeat
  in 60) and does not flicker (2.79), so the repeats are what cause it. snap7-water2 found two causes on snap7/water2: the water blend's
  parity comes from the live frame counter while the DCLF water mask uses the snapshot's (dark frames), and Dynamic Cubemaps
  turns off its reflections on repeats (reflection flicker, b7417fb4d). Runs: n1 (frames 47-50 show it), 1128-015358-...-n30 (cap 30),
  1128-015939-...-nu (uncapped), 1128-020905-...-x1 (D3D12).
- **F2 (high): native D3D12 drew one broken frame in Dragonsreach**: frame 42 of the burst (render frame of a repeat, the third
  render of its snapshot) shows the hall's columns and floor in pieces under an exterior sky, without the player and the walls.
  The frames before and after it are correct. Whole-frame spike 19.6, tile spike 135. Run 1128-020905-post-flicker-native-d3d12-flicker-scenes-post-flicker-x1,
  `bursts/dragonsreach/f041-f044.png`. Native Vulkan and guest, in the same scene, were clean in all runs.
- Clean in every native run: grass and shadows at the save, the sky, Whiterun, Dragonsreach (Vulkan).
- Also clean: native-vulkan+flicker-taa (TAA at the save, 120 frames, 66 repeats: 1.13), native-vulkan+update-cap30's burst (0.15),
  native-interior's burst (0.35).

## 6. Test runs

| Config | Label | Result |
|---|---|---|
| native-vulkan+flicker-scenes | post-flicker-n1 | passed before limits were added; river 53.8/68, river-east 56.6/138 |
| d3d11+flicker-scenes | post-flicker-d1 | passed (calibration) |
| guest+flicker-scenes | post-flicker-g1 | passed (calibration) |
| native-vulkan+flicker-scenes+cap30 | post-flicker-n30 | failed on the river bursts (F1) |
| native-vulkan+flicker-scenes, game.uncapped | post-flicker-nu | failed on the river bursts (F1) |
| native-d3d12+flicker-scenes | post-flicker-x1 | failed: river bursts (F1), Dragonsreach (F2) |
| native-vulkan+taa-water | post-flicker-ntw | passed, burst 2.79 |
| native-vulkan+flicker-taa | post-flicker-v-native-vulkan+flicker-taa | passed, 1.13 |
| native-vulkan+quick, guest+quick | post-flicker-v-* | passed |
| native-interior, native-vulkan+update-cap30 | post-flicker-v-* | passed, shot references held |

Skipped: native-d3d12-quick (no ORG, BasicRHI or device change; D3D12 ran flicker-scenes anyway).

## 7. Open items

- Guest pays up to 60% frame time while a burst captures (8 MB copied out of the readback buffer each frame on the main thread,
  probably). A smaller readback (a GPU reduction to the grid) would remove it; not needed for flicker found so far.
- F2 needs an owner: one broken native D3D12 frame in Dragonsreach, seen once.
- The limits are global. A scene with real fast periodic motion (the water wheel, flames up close) may need its own limit; record it
  in the config when one does.
