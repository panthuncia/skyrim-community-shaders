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

## The primary path (2026-11-11, `org-port/primary-ui`)

With `CS_ORG_BINK=1` the graph is the only path for the ticks it takes (primary-path.md).

- **The tile program is built at setup**, synchronously, for the framebuffer's format (`ProgramCache::Require` in `UpdateTarget`).
  If a resize changes the format, the new program is built at the first tick that sees it. A tick never waits for a program.
- **The client joins at setup.** It is registered at `Bink::Setup` (from `Engine::Setup`). `Engine::PostSetup` (from `State::Setup`, after the features' `SetupResources`) runs the setup's build point,
`FrameAnchors::BuildPending`: every client registered so far is in its anchors' graphs before the first frame. Log:
`[ORG] frame anchors: graphs built at CS's setup; every registered client is in its anchors' graphs`.
  The startup logo plays after that, so its first tick finds the client in the Present graph.
- **Deleted:** the `joined` flag and the per-tick `RequestJoin`, `the tile program is not built yet`, the `missed` latch (a
  missed frame no longer hands the rest of the movie to the engine), and the build `ClaimForBlockingTick` made at the logo's
  first tick (`OnFrameEnd`). The claim of rendering at that tick's start stays: it is ownership, not readiness.
- **A queued frame that did not draw** (its run did not submit with the passes) is an error: `[ORG] Bink: a movie frame queued
  for the Present anchor did not draw (N so far)`. The next ticks draw on the graph.
- **Staging:** one block per frame in flight plus the one the decoder fills (5), so a tick finds one in normal play.
- **The flush latch:** the engine's `DrawTiles` on a tick the graph does not take runs in `Flush::EngineDraws` (`bink: tick not
  taken`).
- **Rebuild trigger:** a new back buffer (a resize) refreshes the client once. The Present graph is built at the next build point
  between frames, and the ticks until then are the engine's (`the framebuffer was made again`).

### What stays the engine's

- Movies in player mode 2 (drawn inside another frame), and ticks off the render owner (the logo without
  `CS_ORG_LOADING_THREAD=1`). Mode 2 needs its draw at the `DrawTiles` point inside the caller's frame: an anchor there, which
  is FrameAnchors work (a new anchor in its table and the frame's epoch order), not done in 5.5.9.
- A movie larger than 4096x2304, and a tile set whose layout is not the expected one.
- The framebuffer that cannot be imported, the Present anchor's ring full this frame, no staging block to take, and a target
  made again while a frame is open (a mode-1 tick always starts between frames, so in practice never).

### 5.5.9 (2026-11-12, `org-port/ui-movies`)

- **HDR output:** a graph tick no longer refuses HDR. At `DrawTiles` it calls `HDRDisplay::PrepareMovieDraw` (what HDR
  Display's own detour of `DrawTiles` does: the scene cleared, kFRAMEBUFFER pointed at the UI texture) and queues the frame for
  HDR's UI texture when kFRAMEBUFFER's RTV is the UI texture's. The UI texture is imported as `hdr.ui`, ORGHDRDisplay's name,
  with every view (ORGHDRDisplay now passes its RTV too: a name's first import decides its views), so both are one graph
  resource. The Present anchor runs before `HandleSwapChainPresent`, whose `ApplyHDR` composites the UI texture.
- **Any movie up to 4096x2304:** the movie texture stays 2048x1152. A movie is split into chunks, each a band of whole tile
  columns and rows that fits it (`MakeChunks`); a 1920x1080 movie is one chunk, a 3840x2160 one six. One pass per target
  copies a chunk's tiles from the staging buffer and draws them, chunk after chunk. The movie texture is declared as a shader
  resource; the pass moves it to a copy destination and back around each chunk's copies (its own barriers, so the graph finds
  it as declared). The upload pass is gone.
- **Seams as the engine's:** the pixel stage clamps each tile's coordinate to its region's edge texels, as the engine's
  separate clamped textures sample. The atlas's seamless edges are gone, so shots should now match the engine's at the
  256-pixel steps too.
- **Paused frames:** the last decoded staging block is kept (`Movie::last`) and drawn again by a tick that decodes nothing; the
  pool holds one block more for it.
- **A target made again:** the framebuffer or HDR's UI texture made again refreshes the client, and a blocking tick with no frame
  open (`BGSMoviePlayer` vfunc +0x58) builds the graph at once with `FrameAnchors::BuildPending`, between two frames. No tick is
  the engine's for it.
- **The logo's frame 1:** nothing changed; `loading-org1.env` showed 166 of 166 frames on the graph (org-full-port.md). Without
  `CS_ORG_LOADING_THREAD=1` the logo stays the engine's (5.5.11 makes the switch the default).
- `bink: tick not taken` stays: mode 2 and the refusals above still reach the engine's `DrawTiles`.

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
| `FreeTiles`, `Close` | the record goes; the next movie starts from a clean count (the client stays) | the same |

The seam is one call in `Hooks::Install` (`src/Hooks.cpp`): `RenderGraph::Engine::Bink::InstallHooks()`, which also reserves
the Present anchor. With the switch off and no `CS_ORG_BINK_SHOT`, nothing is hooked.

A tick is a graph tick when the switch is on, the player is in mode 1, the tick runs on the render owner
(`FrameAnchors::IsRenderOwner`), the anchor can run this frame (`FrameAnchors::CanRun`: the
client is in the Present graph since setup's build point, and the ring has room), the framebuffer is imported, and the movie is at
most 4096x2304.

The ORG side (`BinkGraph.cpp`), the same shape as ImGui's backend (`ImGuiBasicRHI.cpp`):

- **One client for the session**, registered at `State::Setup` (`Bink::Setup`, from `Engine::Setup`): a movie never registers,
  unregisters or refreshes anything. The framebuffer is `FrameAnchors::EngineTarget(kFRAMEBUFFER)`; a new back buffer (a
  resize) refreshes the client once, and ticks are the engine's until the next build point has built it.
- **The movie texture:** one persistent graph texture (`Bink::Movie`, 2048x1152 R8G8B8A8_UNORM, 9 MB) that holds a chunk of the
  video at a time (5.5.9 above), each tile at its place in its chunk. A 1920x1080 movie's tiles reach 1920x1088: one chunk.
- **The upload ring:** `RenderGraph::FrameStaging` (frames in flight plus two blocks), one frame each, every tile's rows at a 256-byte pitch
  and 512-byte placement. With none free, the tick decodes on the engine's path. No D3D11 `Map` is involved on a graph tick. The
  edge tiles' parts beyond the video are zeroed, as the engine's zero-filled textures hold.
- **The passes:** client `cs.bink` at the Present anchor, before any other Present client: `cs.bink.tiles` (order -1000, onto
  the framebuffer) and `cs.bink.tiles.hdr` (order -999, onto HDR's UI texture, while HDR has one). Each copies a chunk's tiles
  into the movie texture and draws them in a render pass with load op Load, one 6-vertex draw per tile, its rectangle and its
  region of the movie texture in the push block; blending off, no depth, no culling. The commit stores the queued frame for both in `RenderGraph::RunSlots`; each
  pass takes its slot's frame and records nothing when there is none, so they are recorded every run (no revision) and never
  repeat an older run's frame.
- **Paused frames:** a tick that decoded nothing draws the last decoded frame again, from its kept staging block.

The anchor runs at `IDXGISwapChain::Present`, which the tick's `Renderer::End` reaches right after `DrawTiles`, so the graph's
draw lands in the frame that Present shows, under CS's overlay and HDR output.

### Fallbacks

- Any of the conditions above false: the engine's path for that tick, D3D11 as before.
- The movie texture, the staging pool, the sampler, the framebuffer import or the registration fails at Setup, or the client's
  epoch fails (`onFailed`): logged once (`[ORG] Bink: <why>; the engine draws Bink from now on`), and every later tick is the
  engine's.
- A queued frame whose run did not submit with the passes (no `executed` signal; only an epoch failure after `CanRun` allowed
  it): logged as an error, and that frame is lost. The next ticks stay on the graph.
- A tile set whose layout is not the expected one (bytes per pixel, tile count): logged, that movie stays the engine's.

### Threads, and what P5.12 needs

Everything on the graph side runs on the render owner: the tick, the locks, the queue, and the Present anchor's run that the
tick's Present reaches. Tile sets are made and freed on the playing thread, so their records are under a mutex.

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

Built in P5.12 ([loading-screen.md](./loading-screen.md)): Bink's checks call `FrameAnchors::IsRenderOwner()`, and with
`CS_ORG_LOADING_THREAD=1` the logo's MoviePlayer thread owns rendering while it holds the lock, so the logo takes the graph
path unchanged. Without the switch the logo's ticks log `[ORG] Bink: a movie plays on thread N, not the render thread ...`
once and draw through the engine (FrameAnchors logs its own one-time line about a Present off the main thread too).

### HDR output

With HDR on for the frame, HDR Display's own `DrawTiles` detour clears its UI texture and redirects the framebuffer there, so
the movie goes through its composite. A graph tick makes the same preparation (`HDRDisplay::PrepareMovieDraw`) and draws into
the UI texture (5.5.9 above).

## Switches

| Variable | Effect |
| --- | --- |
| `CS_ORG_BINK=1` | Bink on the render graph as above. Logs `Hooking the Bink movie player (CS_ORG_BINK)`, at Setup `[ORG] Bink: drawing onto the framebuffer (...)` and `on (CS_ORG_BINK); registered at the Present anchor for the session`, then per movie `a WxH movie in N tiles, B bytes of staging a frame` and `a WxH movie ended: G frames drawn by the render graph, E by the engine`. Why a tick is the engine's is logged once per reason (`the engine draws this frame: ...`). |
| `CS_ORG_BINK_SHOT=<n>[,<n>...]` | A diagnostic (read with `Switches::List`) for either path: the Present of each listed decoded frame (player +0x10) on the main thread is saved through the Screenshot feature as `<log dir>/dclf-shots/bink-<n>.png` (cs-ab copies it to the run's `shots`). Logs `[ORG] Bink: capturing decoded frame n (render graph|engine)`. Hooks the player even with `CS_ORG_BINK` off. |

## Test plan

The instance (C:\Modding\MO2, profile Default) does not skip the logo: no profile INI sets `sIntroSequence`, the game's
`Data\Video\BGS_Logo.bik` is present, and no installed plugin skips it. But the logo plays on the MoviePlayer thread, so it
always takes the engine's path; it shows only that the hooks pass it through (no crash, the logo plays, the line above).

To trigger a Bink on the main thread in a test, use the harness command `dclf:bink=BGS_Logo.bik` in
`CS_DCLF_TEST_COMMANDS`. The console's `PlayBink` does nothing there (the harness runs console commands as a compiled
script), so `dclf:bink` calls `MoviePlayer::PlayMovie` (SE 87918, AE 90291) on the BGSMoviePlayer singleton (SE 525733, AE
412214) with the console's defaults, on the main thread at its next frame-job wait (SE 34554, AE 35409, through
`RenderGraph::Engine::MainTick`, added at plugin load only while `CS_DCLF_TEST_COMMANDS` is set; SKSE tasks run on job threads
in this setup). It logs `[ORG] Bink: playing <file> on thread <id> (main
thread: true)`. It blocks the main thread for the movie's 7 seconds; the test harness's frame count does not move
meanwhile.

Scenarios (`tools/cs-ab/scenarios/`):

- `bink-org0.env`: the engine's path, `dclf:bink=BGS_Logo.bik` at frame 600, shots `bink-40.png` and `bink-120.png`, a game shot after.
- `bink-org1.env`: the same with `CS_ORG_BINK=1`.

What to check:

1. Both runs: the logo at startup plays as before; no `[E]` lines besides the known one; no device loss.
2. org1's log: the lines listed under the switch; the `movie ended` line with nearly every frame drawn by the render graph
   (a few by the engine only while the tile program builds or the client joins). Its absence, `the engine draws Bink from now
   on`, or `did not draw` is a failure.
3. The shots: `bink-40.png` and `bink-120.png` of both runs should match byte for byte, or nearly (the same decoded bytes,
   sampler and quads, except at the 256-pixel tile seams, which the movie texture no longer shows; a difference elsewhere
   points at the rectangles or texture regions, a red/blue swap at the shader).
4. After the movie: the game draws again (the `after` shot). The client stays registered; with no movie its passes record
   nothing (the movie texture's barriers only).

Not checked without a build: compilation, and AE (the IDs and layouts are the same in Ghidra). The startup logo plays before
`State::Setup` registers the client, so it may stay the engine's even with `CS_ORG_LOADING_THREAD=1`.

## The first frame

With every port on, the logo drew 165 of its 166 frames on the graph (`1111-080217-p35-p5-all-s4`): its first tick decides whether it
is a graph tick (`Eligible`) before its own `Renderer::Begin`, where the MoviePlayer thread takes rendering, and the Present anchor's
graph held the `cs.bink` client (registered at `State::Setup`) only from the build at that frame's end (`graphs built at the frame's
end for clients joining them (build 1)`). So the first frame was the engine's for two reasons.

`org-port/w3-misc` (`BinkHooks.cpp`, `ClaimForBlockingTick`): a tick of a blocking movie (player mode 1) on a thread that holds
`Renderer::Lock` (the renderer's CRITICAL_SECTION, Renderer +0x2790 SE, +0x27F0 AE, owned by the caller) but does not own rendering,
with `CS_ORG_LOADING_THREAD=1`, claims rendering at the tick's start (`FrameAnchors::ClaimRenderOwnership`, the claim its `Begin`
makes a few calls later) and builds the graphs a joining client waits for there (`FrameAnchors::OnFrameEnd`: the build a frame's end
makes, made between frames one frame earlier). PlaySequence holds the lock for the whole sequence, so no other thread renders between
the two points. It happens once per ownership change; the next ticks own rendering already. Logged:
`[ORG] Bink: a blocking movie on thread N under Renderer::Lock takes rendering at its tick's start`.

Expected: `a 1920x1080 movie ended: 166 frames drawn by the render graph, 0 by the engine` in `loading-org1.env` and the p5-all runs,
and no `a movie plays on thread N, not the render thread` line. Since the primary path the claim no longer builds at the tick: the
client and the tile program are ready from setup ("The primary path").

## Open points

- The startup logo on the graph: P5.12's `CS_ORG_LOADING_THREAD=1` (above). It has run in game: with every port on, the logo
  drew 164 of its 166 frames on the graph (`1111-063518-p35-p5-all-r1`; 165 in `1111-064846-p35-p5-all-w2`), the first
  ones the engine's while the client joins. The client registers at `State::Setup`; if that ever comes after the logo, the
  logo is the engine's.
- HDR output's UI texture as the target.
- Mode 2 movies (inside another frame) need their draw at the point of `DrawTiles`, not at the Present: an anchor at that
  point, or a draw into the frame's own epoch.
- On AE the console and Papyrus commands' own IDs were not looked up; the port does not hook them.
