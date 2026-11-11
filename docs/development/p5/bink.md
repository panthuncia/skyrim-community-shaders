# P5.11: Bink video on the render graph

RE results and the design for drawing Bink movies with ORG ([org-full-port.md](../org-full-port.md) P5.11, R10 in
[engine-d3d11-surface.md](../engine-d3d11-surface.md)). The code is `src/RenderGraph/Engine/Bink/` and
`package/Shaders/Bink/BinkCopy.hlsl`, behind `CS_ORG_BINK=1`. Addresses were checked in Ghidra on SE 1.5.97 and AE 1.6.1170;
the functions below are named there.

## RE

### Functions

| Function | SE ID / addr | AE ID / addr | What it does |
| --- | --- | --- | --- |
| `BSMoviePlayer::Open` | 87888 / 0x1410a0f80 | 90257 / 0x141195a10 | `BinkOpen("Data\Video\<name>")`. Off the main thread it adds flag 0x8000000 and, after the open, releases the MoviePlayer's semaphore (+0x48) |
| `BSMoviePlayer::OpenAndCreateTiles` | 87889 / 0x1410a10f0 | 90258 / 0x141195b80 | Open, then the scale (`MoviePlayer::CalculateScale`), the x and y offsets, then `CreateTileTextures` into player +0x08 |
| `BSMoviePlayer::PlayBlocking` | 87887 / 0x1410a0e40 | 90256 / 0x141195830 | Mode 1; `do { Tick } while`; then BinkClose and FreeTiles (SE), or Close (AE) |
| `BSMoviePlayer::Tick` | 87890 / 0x1410a1210 | 90259 / 0x141195ca0 | One frame: decode, clear, draw, present (below) |
| `BSMoviePlayer::Close` | 87891 / 0x1410a13a0 | 90260 / 0x141195f30 | BinkClose, FreeTiles (inlined on AE), reset the player |
| `BSMovie::CreateTileTextures` | 87904 / 0x1410a1910 | 90275 / 0x1411965e0 | The tile set (below) |
| `BSMovie::FreeTiles` | 87905 / 0x1410a20e0 | 90276 / 0x141196dc0 | Releases textures, quads, shaders; frees the set. On AE nothing calls it: Close inlines it |
| `BSMovie::DrawTiles` | 87907 / 0x1410a2370 | 90279 / 0x141197050 | The tile draw (below). HDR Display already detours it |
| `BSMovie::DecodeFrameToTiles` | 87911 / 0x1410a2840 | inlined in 90259 | `BinkDoFrame`, then per tile: lock, `BinkCopyToBufferRect`, unlock |
| `BSMovie::CreateTileQuad` | 87910 / 0x1410a2720 | inlined | One tile's TriShape: 4 vertices, indices 0,1,2,0,2,3 |
| `BSMovie::CreateCornerTileTexture` | 87903 / 0x1410a1870 | inlined | The bottom-right tile's texture |
| `BSGraphics::Renderer::LockTexture` | 75514 / 0x140d6e4c0 | 77306 / 0x140e48c10 | `(renderer, texture block, D3D11_MAP, D3D11_MAPPED_SUBRESOURCE*)`: `context->Map` of block +0x00. Two other callers |
| `BSGraphics::Renderer::UnlockTexture` | 75515 / 0x140d6e510 | 77307 / 0x140e48c60 | `context->Unmap` |
| `MoviePlayer::StartSequenceThread` | 87897 / 0x1410a14f0 | 90266 / 0x1411961b0 | `CreateThread(PlaySequenceThread)`, named "MoviePlayer", priority from 0x141e0c208, then waits for the semaphore |
| `MoviePlayer::PlaySequenceThread` | 87896 / 0x1410a14d0 | 90265 / 0x141196190 | Calls `PlaySequence` |
| `MoviePlayer::PlaySequence` | 87923 / 0x1410a3070 | 90298 / 0x141197bb0 | `Renderer::Lock` (BGSMoviePlayer vfunc +0x38), each comma-separated `.bik` through `MoviePlayer::PlayMovie`, `Renderer::Unlock` (+0x40) |
| `MoviePlayer::PlayMovie` | 87918 / 0x1410a2c40 | 90291 / 0x1411977a0 | Pauses a playing movie, makes a player, `PlayBlocking` |
| `MoviePlayer::TickInFrame` | 87920 / 0x1410a2f10 | 90293 / 0x141197a60 | A tick of the playing movie inside another frame (mode 2) |
| `Console::PlayBink` | 21915 / 0x140301360 | `GameFunc::handler::PlayBink` (calls PlayMovie at 0x140357e1b) | Console command `PlayBink <file>`: `PlayMovie` on the console's thread |
| `Papyrus::Game::PlayBink` | 54855 / 0x1409736a0 | `papyrus::Game::PlayBink` (calls PlayMovie at 0x140a0d10e) | Papyrus `Game.PlayBink` |

The BGSMoviePlayer singleton is SE 0x143047330 (AE `_gBGSMoviePlayer`). Its vtable (SE 0x141646f48): +0x38 and +0x40 jump to
`Renderer::Lock`/`Unlock` (75458/75459), +0x48/+0x50 return the screen width and height (`BSGraphics::State` +0x24/+0x28),
+0x58 returns the in-frame flag (SE 0x14302c8e0, set by `Renderer::Begin`), +0x60 the main thread's id, +0x68 pumps the
thread's window messages and returns whether the movie goes on (the skip key).

### Structures

Player (0x30 bytes): +0x00 `BINK*` (+0x00 width, +0x04 height, +0x08 frame count), +0x08 tile set, +0x10 frames decoded,
+0x14 frames copied, +0x18 scale (float), +0x1C x and +0x20 y offset (pixels), +0x24 mode (1 blocking, 2 in another frame,
0 failed), +0x28 opening flag.

Tile set (0x70 bytes, then the texture pointers): +0x00 bytes per pixel (4), +0x04 width, +0x08 height, +0x0C the tile being
decoded, +0x14 pitch, +0x18 tile count, +0x1C whole columns, +0x20 whole rows, +0x24 edge width, +0x28 edge height (each the
remainder rounded up to a power of two, 0 when there is none), +0x2C/+0x30 the remainders, +0x34 tile size (256), +0x38 the
texture blocks, +0x40 the quads (a BSTArray), +0x58 the VS, +0x60 the PS.

Tile order: each row of whole tiles followed by its right edge tile, then the bottom row the same way, ending with the corner.
The textures are `Renderer::CreateTexture2D` (75511) blocks: DYNAMIC, R8G8B8A8_UNORM, the tile's size (edge tiles at their
power-of-two size), zero-filled at creation.

### One tick

`BSMoviePlayer::Tick(player, ...)`:

1. If the movie goes on (vfunc +0x68) and is not paused (MoviePlayer +0x31): `BinkWait`. A frame is due: decode it into the
   tiles (below), `BinkNextFrame`, count it. None due: in mode 1 `Sleep(1)` and return (no draw, no present); in mode 2 draw
   the old frame.
2. `SetRenderTarget(0, kFRAMEBUFFER, clear)` with the clear colour set from 0x141e0de88 first, depth MAIN, `ApplyState` (the
   clear runs here).
3. `Renderer::Begin(0)` unless a frame is open (vfunc +0x58).
4. `DrawTiles`.
5. Mode 1: `Renderer::End`, which is the Present.

Decode: per tile i, column `i % (columns + edge?)`, row `i / ...`, source rectangle at (column x 256, row x 256) of the tile's
texture size: `LockTexture(tile, WRITE_DISCARD)`, `BinkCopyToBufferRect(bink, pData, RowPitch, bink height, 0, 0, x, y, w, h,
flags)`, `UnlockTexture`. Flags: SE `0x80000003` (copy all, surface type 3), AE `0x80003`. There is no YUV texture: the CPU
writes 32-bit pixels.

`DrawTiles(set)`: alpha blend mode 0 (off), PS sampler slot 0 address mode 0 (clamp) and filter mode 2 (min/mag/mip linear,
`CreateSamplerStates` 75831: MaxAnisotropy 1, MaxLOD FLT_MAX, no bias), depth mode 0 (off); the set's VS and PS; per tile
PS t0 = its texture and `DrawTriShape(quad)`; then depth mode 2 and t0 cleared again. It does not set the rasterizer state;
the quads wind counter-clockwise, so the engine's state there culls nothing.

Quad (`CreateTileQuad(left, top, width, height)`, fractions of the screen): corners (left·2−1, 1−top·2) with UV (0,0), then
bottom left (0,1), bottom right (1,1), top right (1,0); z 0. `CreateTileTextures` builds them with `scale / screen width`
steps from the offsets, in the order above; `MakeLayout` (BinkGraph.cpp) repeats that arithmetic in its order.

### The shaders

VS 0x238 bytes (SE 0x141e0bcd0, AE 0x1420300b0) and PS 0x2b8 bytes (SE 0x141e0bf10, AE 0x1420302f0); SE and AE have the same
bytes. No HLSL for them is in the executable.

```
vs_4_0                                   ps_5_0
dcl_input v0.xyz                         dcl_sampler s0 (SampBase)
dcl_input v1.xy                          dcl_resource_texture2d t0 (TexBase)
mov o0.xyz, v0.xyzx                      sample_indexable r0.xyzw, v1.xyxx, t0.xyzw, s0
mov o0.w, l(1.0)                         mov o0.xyzw, r0.zyxw
mov o1.xy, v1.xyxx
```

The PS swaps red and blue (R10 open question 2): surface type 3 writes BGRA bytes into the RGBA texture.
`package/Shaders/Bink/BinkCopy.hlsl` is that pair. Its FXC builds disassemble the same as the engine's (VS at vs_4_0, PS at
ps_5_0: signatures, bindings and instructions); `tools/check-dual-target-shaders.py` builds both stages for FXC, SPIR-V and
DXIL (push block 32 bytes).

### Threads

- **The startup logo** (`sIntroSequence:General`, default `BGS_Logo.bik`, 1920x1080, 166 frames) plays through
  `StartSequenceThread`, from the main init (SE 0x1405aec80) and `Main` (0x1405b5280). The "MoviePlayer" thread runs
  `PlaySequence`, which holds `Renderer::Lock` for the whole sequence. The main thread waits only until `BinkOpen` returns
  (the semaphore), then goes on initializing; it reaches `WaitForSequenceToEnd` later (0x1405af130). Every logo frame is
  decoded, drawn and presented on the MoviePlayer thread.
- **The main menu's intro movie** (`sMainMenuMovieIntro:General`, empty by default) plays from 0x1405af130 with
  `PlayMovie` on the main thread.
- **Console and Papyrus `PlayBink`** call `PlayMovie` on their thread, the main thread for the console.
- **Mode 2** (`TickInFrame`, players set up by 87915 / 0x1410a2a50 and 87919 / 0x1410a2dc0) draws inside the caller's frame and does not
  present.

## Design

### The path

Hooks (`BinkHooks.cpp`, all detours by ID, so SE and AE are the same):

| Hook | Graph tick | Otherwise |
| --- | --- | --- |
| `CreateTileTextures` | records the layout (`MakeLayout`) and the tile blocks | the same (with the switch on) |
| `Tick` | decides once per tick whether it is a graph tick | the engine's |
| `LockTexture` | for the tick's tiles: the tile's place in this tick's staging buffer | the engine's |
| `UnlockTexture` | for the tick's tiles: nothing | the engine's |
| `DrawTiles` | withheld; the frame is queued (`Graph::Queue`) | the engine's, and the graph's textures are marked stale |
| `FreeTiles`, `Close` | the record goes; after a graph movie the client leaves the Present anchor | the same |

The seam is one call in `Hooks::Install` (`src/Hooks.cpp`): `RenderGraph::Engine::Bink::InstallHooks()`, which also reserves
the Present anchor. With the switch off and no `CS_ORG_BINK_SHOT`, nothing is hooked.

A tick is a graph tick when the switch is on, the player is in mode 1, the tick runs on the main thread
(`FrameAnchors::OnMainThread`), HDR output is off for the frame, the runtime is active with the Present anchor's epoch, the
framebuffer is imported and the program for its format is built, the client is registered (the first eligible tick registers
it and stays the engine's), and the set's ORG textures exist (made at its first graph tick).

The ORG side (`BinkGraph.cpp`), the same shape as ImGui's backend (`ImGuiBasicRHI.cpp`):

- **Tile textures:** one BasicRHI R8G8B8A8_UNORM texture per tile at the engine's size, with its SRV. Not graph resources:
  the pass records their barriers itself (undefined to copy-dest the first time, shader-read after).
- **The upload ring:** host-visible `LatchBlock` staging buffers, one frame each, every tile's rows at a 256-byte pitch and
  512-byte placement. A pool of at most four: a buffer goes back when the last frame that names it retired. With none free,
  the tick decodes on the engine's path. No D3D11 `Map` is involved on a graph tick. The edge tiles' parts beyond the video
  are zeroed, as the engine's zero-filled textures hold.
- **The pass:** client `cs.bink`, pass `cs.bink.tiles` at the Present anchor, order -1000 (before any other Present client),
  kind Render, onto `EngineTarget(kFRAMEBUFFER)`. The commit stores the queued frame in the host's frame slot. `Record`
  takes it, retains it, records the copies (when the tick decoded) with their barriers, then one render pass with load op
  Load (the engine's clear already ran in D3D11, in stream order before the epoch) and one 6-vertex draw per tile, its
  texture and rectangle in the push block. Blending off, no depth, no culling, the framebuffer's size as the viewport.
- **Paused frames:** a tick that decoded nothing draws the textures as they are, when the newest frame is in them.

The anchor runs at `IDXGISwapChain::Present`, which the tick's `Renderer::End` reaches right after `DrawTiles`, so the graph's
draw lands in the frame that Present shows, under CS's overlay and HDR output.

### Fallbacks

- Any of the conditions above false: the engine's path for that tick, D3D11 as before.
- The pipeline, the sampler, the framebuffer import or the registration fails, the client's epoch fails (`onFailed`), or three
  queued frames in a row never run with the pass in the graph (counted from the `executed` signal): logged once
  (`[ORG] Bink: <why>; the engine draws Bink from now on`), the client is unregistered, and every later tick is the engine's.
  The engine's tiles get the next decoded frame, so at most the failing frame is lost.
- A tile set whose layout is not the expected one (bytes per pixel, tile count): logged, that movie stays the engine's.

### Threads, and what P5.12 needs

Everything on the graph side runs on the main thread: the tick, the locks, the queue, and the Present anchor's run that the
tick's Present reaches. Tile sets are made and freed on the playing thread, so their records are under a mutex; the ORG
textures are made only on the main thread.

The startup logo stays on D3D11 for now: it presents from the MoviePlayer thread, where `FrameAnchors::Run` returns false.
The way to put it on the graph, which P5.12 needs for the loading-screen thread too, is an explicit epoch run under
`Renderer::Lock`:

1. `FrameAnchors::OnMainThread` becomes "on the render thread": the main thread, or the thread that owns `Renderer::Lock`
   (the `CRITICAL_SECTION` at Renderer +0x2790 SE / +0x27F0 AE, `OwningThread` equal to the caller) while the main thread is
   not inside a frame. The engine already serializes its frames by that lock: the main frame holds it from Begin to End, the
   logo's `PlaySequence` for the whole sequence, the loading screen per iteration. So two anchor runs never overlap, and the
   lock's acquire and release order the anchors' state and the host's frame upload instance between the threads.
2. The host's thread checks (if any assert the creating thread) accept that thread, and the per-Present work in CS's Present
   hook (`SignalFramePace`, `WaitFramePace`, `SealPresented`, which already run there today) stays as it is.
3. Anything the main thread does with the graph outside a frame while the other thread holds the lock (resource creation,
   `Register` from a feature's setup) must not touch the anchors' per-run state; registration already only publishes a new
   client list, built at the next run.

With that, Bink's eligibility test switches from `FrameAnchors::OnMainThread()` to the new predicate and the logo takes the
graph path unchanged. Until then the logo's ticks log `[ORG] Bink: a movie plays on thread N, not the main thread ...` once
and draw through the engine (FrameAnchors logs its own one-time line about a Present off the main thread too).

### HDR output

With HDR on for the frame, HDR Display's own `DrawTiles` detour clears its UI texture and redirects the framebuffer there, so
the movie goes through its composite. The graph's pass draws the framebuffer, so such ticks stay the engine's. Drawing the
tiles into HDR's UI texture (imported as ImGui imports it) is the follow-up.

## Switches

| Variable | Effect |
| --- | --- |
| `CS_ORG_BINK=1` | Bink on the render graph as above. Logs `Hooking the Bink movie player (CS_ORG_BINK)`, then per movie `[ORG] Bink: drawing onto the framebuffer (...)`, `registered at the Present anchor ...`, `a WxH movie in N tiles, B bytes of staging a frame` and `a WxH movie ended: G frames drawn by the render graph, E by the engine`. |
| `CS_ORG_BINK_SHOT=<n>[,<n>...]` | A diagnostic for either path: the Present of each listed decoded frame (player +0x10) on the main thread is saved through the Screenshot feature as `<log dir>/dclf-shots/bink-<n>.png` (cs-ab copies it to the run's `shots`). Logs `[ORG] Bink: capturing decoded frame n (render graph|engine)`. Hooks the player even with `CS_ORG_BINK` off. |

## Test plan

The instance (C:\Modding\MO2, profile Default) does not skip the logo: no profile INI sets `sIntroSequence`, the game's
`Data\Video\BGS_Logo.bik` is present, and no installed plugin skips it. But the logo plays on the MoviePlayer thread, so it
always takes the engine's path; it shows only that the hooks pass it through (no crash, the logo plays, the line above).

To trigger a Bink on the main thread in a test, run the console's `PlayBink "BGS_Logo.bik"` through
`CS_DCLF_TEST_COMMANDS`. It blocks the main thread for the movie's 7 seconds; the test harness's frame count does not move
meanwhile.

Scenarios (`tools/cs-ab/scenarios/`):

- `bink-org0.env`: the engine's path, `PlayBink` at frame 600, shots `bink-40.png` and `bink-120.png`, a game shot after.
- `bink-org1.env`: the same with `CS_ORG_BINK=1`.

What to check:

1. Both runs: the logo at startup plays as before; no `[E]` lines besides the known one; no device loss.
2. org1's log: the lines listed under the switch; the `movie ended` line with nearly every frame drawn by the render graph
   (0 or 1 by the engine: the registration tick). Its absence, or `the engine draws Bink from now on`, is a failure.
3. The shots: `bink-40.png` and `bink-120.png` of both runs should match byte for byte, or nearly (the same decoded bytes,
   sampler and quads; a difference at tile seams or edges points at the rectangles, a red/blue swap at the shader).
4. After the movie: the game draws again (the `after` shot), and the Present anchor costs nothing (the client unregistered).

Not checked without a build: compilation, the first graph frame after registration (whether the build at the next run
already has the pass; the miss counter tolerates two lost frames), and AE (the IDs and layouts are the same in Ghidra).

## Open points

- The startup logo on the graph waits for P5.12's render-thread rule (above).
- HDR output's UI texture as the target.
- Mode 2 movies (inside another frame) need their draw at the point of `DrawTiles`, not at the Present: an anchor at that
  point, or a draw into the frame's own epoch.
- On AE the console and Papyrus commands' own IDs were not looked up; the port does not hook them.
