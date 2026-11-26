# snap4-ui2: the native UI renderer, finished (roadmap phase 4)

Lead `ui2`, branch `snap4/ui2` (base `org_integration` 1c6773873), stage `s4-ui2`. Follows [snap0-ui.md](snap0-ui.md) (the
stream, recorder and replay). Roadmap: [render-thread-snapshots.md](render-thread-snapshots.md), "Phase 4"; decisions 4, 16, 19.

## 1. No game renderer call in native

Native mode now takes every GRendererD3D1x slot but the destructor (shutdown only, still counted). The census line reads
`[ORG] Scaleform: the game's renderer was called for nothing; every other call was CS's` in every native run below.

**CS-owned textures and targets** (`src/RenderGraph/Engine/Scaleform/UiResources.{h,cpp}`). CreateTexture (slot 2) and
CreateRenderTarget (slot 6) return CS objects with GTextureD3D1x's (0x80) and GRenderTargetD3D1x's (0x70) field layout, so code
that reads those fields (CS's `ReadTextureInfo`, `ReadPushedTarget`) keeps working. Their vtables are copies of the D3D1x ones
(the target's sits 0x110 bytes after the texture's on SE and AE; GFx has no RTTI words) with the D3D1x functions replaced by CS
ports, read in Ghidra (SE):

| Slot | D3D1x (SE) | CS |
|---|---|---|
| texture 1 InitTexture | 0x140f602c0 | ARGB as is, RGB to RGBA with opaque alpha (one level), A8 with the full chain (2x2 box filter when both sides halve exactly, else the bytes as they are, as SE 0x140f60df0), DXT1/3/5 and images with levels through GetMipMapLevelData's port (0x140f65eb0); all levels as initial data |
| texture 2 InitDynamicTexture | 0x140f5f4f0 | ARGB/A8/D24S8, filled with 0x40, DYNAMIC for usage 0x20, render-target or depth bind for 0x40 |
| texture 3 Update | 0x140f63b10 | each rectangle copied into an upload (below) |
| texture 4/5 Map/Unmap | 0x140f60ef0 / 0x140f63a60 | a CPU copy per texture; Unmap queues whole-level uploads |
| texture 12 InitTexture(ID3D11Texture2D) | 0x140f601d0 | keeps the engine texture with a reference, makes a view |
| texture 14 ReleaseTextureData, 16 Bind | | releases; Bind is fatal (only the D3D1x renderer calls it) |
| target 1 InitRenderTarget(GTexture x3) | 0x140f5f920 | references the textures; no views (the replay draws pushed targets into scratch images) |
| target 7 InitRenderTarget(views) | 0x140f5faa0 | the engine's views, borrowed as D3D1x keeps them |

GFx base-class slots (user data, change handlers, getters at compatible offsets) stay GFx's; `CallHandlers` and the base
destructor's handler-array free are ported. Textures are made through the device SetDependentVideoMode gave (the engine's
device handle, as GTextureD3D1x did), from any thread.

**Renderer slots now CS's** (`Renderer.cpp`): GetRenderCaps (same caps), CreateTextureYUV (null, logged), CreateRenderTarget,
SetDisplayRenderTarget, Push/PopRenderTarget, PushTempRenderTarget and ReleaseTempRenderTargets (ports of SE 82887, 82882, 82888,
82895: CS's target stack, the temp pool and stencil pool with their refcount tests, unsigned budget arithmetic and the pooled
flag whose depth Pop lets go), EndDisplay (no release queue: nothing is queued), CheckFilterSupport, GetRenderStats (zeros),
ReleaseResources (nothing to release), SetDependentVideoMode / ResetVideoMode / GetDisplayStatus (CS's video mode flag replaces
+0xC0), GetDevice/GetContext. The push makes the D3D1x push's vtable calls the same way (GFx's MakeViewAndPersp3D with fov 55 and
the Y flip, then SetView3D, SetPerspective3D, SetWorld3D(null)), and the pop sets the saved matrices back, so the stream gets
the same commands. The display-target reads at BeginDisplay are tagged capture-point (DrawInterface is a producer).

**Lifetimes.** Objects live while GFx holds them (GFx refcount at +0x08; slot 0 deletes). A stream's texture table holds its
own view references, uploads hold texture references, and Passes' commit keeps views in the frame's owners until the GPU work
retires. So a texture GFx releases stays valid for every snapshot that still names it, rendered once, again or never. Phase 6's
retire queue: these references are the snapshot-held references it generalises; nothing here returns objects to the game.

## 2. A state header per stream (decision 19)

`StreamHeader` (UiStream.h): the RendererState (array pointers dropped, the recorder copies arrays again at a stream's first
draw), blend mode and stack. A stream's header is the previous stream's `EndState()`: its header advanced over its commands by
the replay's pure setters, BeginDisplay's state changes, and the cached 3D product a 3D draw brings up to date. The replay starts
every stream from its header with texgen rows zero. Observe step (removed): at each new stream the continued replay state was
compared field by field with the header: **0 of ~5,000 differ** (runs `nvk-menus2`, `nvk-repeat1`, `nvk-dropcells`).

## 3. The stream in the snapshot

`Snapshot::UiCommandStream` (Snapshot/UiCommandStream.h), native only: the capture makes an empty holder in
`FrameSnapshot::ui`; CS's detour on `MenuManager::DrawInterface` (SE 79947 / AE 82084) finishes the stream at its end and sets it
once (first render of a game update). A **repeated render** (cadence repeat) lets DrawInterface run (menu scenes and its
ImageSpace effect still run where it calls them) but records nothing: each display is drawn from the snapshot's stream where GFx
ends its own, from the header, the rest at the end. Check (kept, cadence mode only): every repeated segment is hashed against the
first render's: **4,957 segments identical, 0 differ** (`f-nvk-repeat`).

**Uploads** are not replayable commands: they are queued with the sequence of the stream open when GFx made them (or the next),
and applied on the render owner before any draw of that stream, once, in call order. A stream that is never replayed still has
its uploads applied before the next replayed one. Today's drop mode runs no DrawInterface for dropped updates, so no stream is
dropped yet; the queue rule covers phase 7.

## 4. Decision 16: DrawInterface is a producer

EngineZones' `Role::Producer`: DrawInterface's two call sites (DrawFrame, RunLoadingScreen) count as render calls only until
`Scaleform::OwnsRenderer()` (native, every slot CS's). In native runs they log `[producer]` and leave the per-frame total.

## 5. Loading screen and logo

The serving thread's DrawInterface goes through the same detour: its streams are finished per DrawInterface, have headers and
uploads, and call no game renderer function (`nvk-dropcells`: three coc loads). They are not published as snapshots: that needs
a second producer on the snapshot channel, adoption on the serving thread and an id rule for snapshots that are not game updates
(the contract says one id per update; cs-test maps shots to ids). Proposed for phase 5's frame kinds (below). The startup logo is
a Bink movie and calls no GRenderer slot.

## 6. Untested paths

New census fields: 3D draws, blur filters, colour matrices, pushed targets, display-target switches. New config `ui-menus2`
(tween, journal, magic, skills, sleep/wait). None of the nine menus opened in these runs (plus HUD, main menu, loading)
uses any of them: all counts are 0, as are PushTempRenderTarget calls. Still open: the book (render textures into kBOOK_TEXT),
lockpicking and level-up need game state to open.

## Runs (Dev-Fast)

| Config | Label | Result |
|---|---|---|
| native-vulkan+ui-menus | s4-ui2-nvk-menus1 | never loaded (cold shader cache, killed at 120 s) |
| native-vulkan+ui-menus (detect) | s4-ui2-nvk-menus2 | passed; game renderer called for nothing; 184 CS textures, 261 uploads; DrawInterface `[producer]` |
| native-vulkan+ui-menus, repeat:3 | s4-ui2-nvk-repeat1 | passed; 1,592 repeated renders, 4,738 segments same, 0 differ |
| native-vulkan+ui-menus, drop:3 | s4-ui2-nvk-drop1 | passed |
| native-vulkan+cadence-drop-cells | s4-ui2-nvk-dropcells | passed (loading screens) |
| native-vulkan+ui-menus2 | s4-ui2-nvk-menus2b | passed; no 3D, filter or target use |
| guest+ui-menus | s4-ui2-f-guest-menus | passed; 4,000 of 4,000 on the graph |
| native-d3d12+ui-menus | s4-ui2-f-d3d12-menus | passed; nothing called |
| native-vulkan+quick | s4-ui2-f-nvk-quick | passed |
| native-vulkan+menu | s4-ui2-f-nvk-menu | passed |
| native-vulkan+menu-resize | s4-ui2-f-nvk-menu-resize | passed |
| native-vulkan+ui-menus, repeat:3 | s4-ui2-f-nvk-repeat | passed; 4,957 segments same, 0 differ |

The `f-` runs are the final build. Shots were checked by eye (fonts from glyph uploads, icons, skills menu); the save differs
from snap0-ui's baseline runs, so no pixel comparison was possible.

## Decided by the agent, to confirm

- **GFx base-class code stays GFx's** (change handlers, user data, frame and event-handler slots, MakeViewAndPersp3D): it is
  GFx core, not the D3D1x renderer. The D3D1x destructor stays (shutdown only), counted.
- **Textures are made and written through the engine's device and context handles** (CS's own D3D11 layer in native), as
  engine textures are. A direct BasicRHI texture is a later step.
- **Uploads apply at a stream's first replay, before its draws**, not between draws. GFx's glyph cache does not reuse a slot it
  drew in the same frame, and the first render applies them display by display, which is the old timing.
- **Texgen rows start at zero in each stream** (a draw writes the rows of every stage it binds).
- **The snapshot's UI slot is written once after publication** (DrawInterface runs inside the render half until phase 7).
- **Repeated renders still run DrawInterface** (for menu scenes and its ImageSpace effect) but draw the snapshot's stream.
- **Loading screens are not snapshots yet** (section 5).
