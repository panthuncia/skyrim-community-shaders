# Parity: Scaleform on the graph in every window (`org-native/par-scaleform`)

Goal: every Scaleform display draws fully on the graph, in every window. That covers the HUD, the menus, loading screens, the
map, the inventory, the console, the race menu, the journal, and the book and letter menus. In native mode, and in guest
under the flush latch, anything left to the D3D11 renderer is lost.

The code is `src/RenderGraph/Engine/Scaleform/`. The design and the RE are in [p5/scaleform.md](../p5/scaleform.md); the
transforms below were read from the SE 1.5.97 exe (`SkyrimSE_unpacked.exe`), because Ghidra was not running.

The commits come in two groups:
- **Closing commits** (each its own). The D3D11-part path is still present after them.
- **Deletion commit**, last: `refactor(scaleform): delete the D3D11 part machinery`. The main session can merge the first
  group, test, then merge the deletion.

## The items

| Item | State | How |
| --- | --- | --- |
| Whole display into a render texture of GFx's (`kViewportRenderTexture`) | closed | The display draws onto the renderer's current render target (+0x740: its RTV +0x50, DSV +0x58), which `SetDisplayRenderTarget` bound. The book's targets `kBOOK_TEXT_0..3` (depth `kBOOK_TEXT`) are surfaces from setup. |
| Whole display with no render target in the shadow state | closed | The same renderer target. Only a display with neither stays refused (`a whole display with no render target bound`). |
| Whole display without a video mode | closed | The D3D11 `BeginDisplay` returns at once without it (SE 82819 tests +0xC0 first), and so does every draw. The display is counted as on the graph, and nothing is drawn. |
| Whole display off the render thread | not closed (no case left) | Native mode forces `CS_ORG_LOADING_THREAD`, so the loading screen's thread owns rendering while it draws. No other thread draws displays. Still counted: `a whole display off the render thread`. |
| Whole display with the graph path off | inherent | A setup failure or a graph failure. The renderer gets every call, as with the switch off. |
| `PushRenderTarget` / `PushTempRenderTarget` (render to a texture, a filter's source) | closed | Drawn into scratch images on the graph (below). |
| `DrawBlurRect` (blur, drop shadow, glow, bevel) | closed | Graph draws with the transcribed `FilterPS.hlsl` 1-22, its intermediate passes in scratch images (below). |
| Colour matrix filter of a pushed target | closed | It reads the scratch image. |
| Filter sources | closed | A source that is a GFx texture drawn into a scratch image reads that image. |
| YUV video textures, non-`GTextureD3D1x` fills | left out (logged) | Only GFx's video player makes YUV textures (`CreateTextureYUV`), and Skyrim does not link it (inferred: no Skyrim UI plays GFx video). Counted as `... not a plain GTextureD3D1x (video)`. |
| Lost textures | left out (logged) | Their data is gone after a device reset, and GFx makes them again before it draws them again. |
| Two-texture gouraud fill without its second texture | closed | Stage 1 reads what the display bound there last, as the renderer leaves it. Before any, it reads stage 0's texture. Inferred from the fill-off rule, not read. |
| `SetDisplayRenderTarget` inside a display | closed | The segment so far is drawn onto the old target, and the rest of the display onto the new one. |
| The 8-part cap | gone | The anchors allow any number of runs a frame. |
| A render target that is not a surface | partly | The book's targets were added. Any other is still refused, and the census names it (`a render target that is not a surface (engine target N)`). Add it to `kRoles` (`Passes.cpp`). |
| More than 1024 commands | closed | `Submit` draws the segment in runs of `kGateEntries` commands, in order. |
| "Interface ring full" | closed | The label was stale: the anchors' runs are not limited. The real case was a registration for new reads (native) waiting for its build. Segments now run on the graph built before it, and new reads keep their raw indices there. Only new surfaces wait. A role's texture made again that is the same graph texture (the slot's next version) keeps its surface, so nothing is built. |
| A pipeline that fails to build | closed | Only that draw is left out, and its gate entry stays closed. The key is logged once: `[ORG] Scaleform programs: the pipeline for vertex kind ... could not be built; its draws are left out`. |
| Masks on another depth-stencil | inherent | Unchanged. |
| The Phase 6 blocker "private target imports, two graph resources on one image" | closed | `p2-one-resource` already routes Scaleform's imports through `FrameAnchors::ImportTexture`. HDR's UI texture shares Bink's `hdr.ui` slot. GFx's targets are no longer imported at all: scratch images are the client's own graph textures. |
| The three `Flush::Reason`s and the D3D11 parts | deleted (last commit) | Below. |

## GFx render targets on the graph

What the D3D11 renderer does (SE disassembly):
- **`PushRenderTarget(frame, target)`** (82887):
  - saves its state on the +0x748 stack and sets +0x740 to the target;
  - makes the view matrix `[2/w, 0, -1 - 2·left/w; 0, -2/h, 1 + 2·top/h]` from the frame rectangle (w and h at least 1, no
    user matrix);
  - sets the viewport `(0, 0, target+0x60, target+0x64)`;
  - resets the 3D matrices through the vtable;
  - binds the target's RTV and DSV and the depth-stencil state "off";
  - clears the RTV to 0 (the DSV to depth 1, stencil 0).
- **`PushTempRenderTarget(frame, w, h, stencil)`** (82888) finds or makes a pooled target. Its texture is the next power of
  two each way, `InitDynamicTexture(..., usage 0x40)`. It writes w and h to +0x60 and +0x64 and **pushes it through the vtable's
  `PushRenderTarget`**. The old code counted that push twice, so after a temp target the rest of the display stayed D3D11's.
- **`PopRenderTarget`** (82882) restores the saved state.

The graph side:
- **Four scratch images**, `cs.scaleform.scratch.0-3`. Each is R8G8B8A8 at the largest role surface's size, made by the client
  (`org::PixelBuffer`) and registered with the Interface graph. Each has a pass that draws into it. Every pass declares the
  others as shader resources, so the graph moves them between target and read. The passes and resources are static, with no
  build at runtime.
- **A push inside a captured display** does four things:
  1. draws the segment so far;
  2. runs the renderer's own push, for its bookkeeping;
  3. starts a segment onto a free scratch image, which clears it (`LoadOp::Clear`) and uses the transform above;
  4. maps the GFx texture (target+0x38) to that image for the rest of the display.
- **A pop** draws the scratch segment, restores the interrupted target, viewport, matrix and stencil state, and goes on.
- **Draws that read a mapped texture** read the scratch image through the pass's declared view. They are fill bitmaps,
  `DrawBitmaps`, the colour matrix filter and blurs. Their coordinates are scaled from the GFx texture's size to the scratch
  image's.
- **Freeing scratch images.** A mapping ends when its texture is pushed again or the display ends. When none is free, the
  oldest mapping that is not being drawn into gives its image up.
- **The blur family.** Each pass before the last is its own segment into a scratch image (held until the next pass reads it),
  drawn onto {-1,-1,1,1} with the identity matrix. The last pass draws onto the current target. Slots, parameter sets, the
  128-byte constants (with the shadow-only quirk that writes a ratio over cxadd.r), textures and blends follow p5/scaleform.md,
  "The blur family". `DrawStates` now enumerates `FILTER` 1-22 too.

Gaps:
- **Masks inside a pushed target are not drawn.** Scratch images have no stencil. A temp target asked for with a stencil makes a
  D24S8 surface, which is not mirrored.
- A target larger than the scratch images, or one pushed while all four are being drawn into, is left out (`a render target of
  GFx's that no scratch image could take`).
- A GFx texture read in a *later* display than the one that drew it reads GFx's own texture, which the graph never wrote. That
  would be a cached filter result. No case is known.
- The renderer's own push still issues its D3D11 bind and clear of its own texture. Clears go through under the latch; in native
  mode the native context gets them.

## After the deletion commit

These are gone: the three `Flush::Reason`s (`scaleform: display not taken`, `scaleform: D3D11 part`, `scaleform: segment
replayed`), every `Flush::EngineDraws` scope in Scaleform, kept calls and `Replay`, `Abort`, `D3D11Ready`, `SyncD3D11`, the D3D11
filter part, and the setter hooks that only kept calls. The renderer's own setters run again.

What is left:
- A draw or a segment the graph cannot take inside a captured display is **left out** and counted.
- Displays the capture never begins (off the render owner, the graph path off, no target at all) still go to the D3D11 renderer
  unchanged, as with the switch off. Under the latch, or in native mode, they draw nothing.
- Guest without the latch no longer draws refused parts through D3D11.

## Census and log lines

Every 1000 displays (after the deletion commit):

```
[ORG] Scaleform census: N displays: G on the graph, M with draws left out, L not on the graph; S graph segments; left out: <count reason, ...>
```

The target is `1000 displays: 1000 on the graph, 0 with draws left out, 0 not on the graph; ...; left out: none`. Before the
deletion commit the line keeps its old form (`... on the graph with D3D11 parts, ... on D3D11; ... D3D11 parts: ...`), and the
target is the same "N on the graph".

Each reason is logged once: `[ORG] Scaleform: not drawn on the graph: <reason>` (before the deletion: `the D3D11 renderer
draws: <reason>`).

At setup:
- `[ORG] Scaleform: the kBOOK_TEXT_0 surface: an engine target (WxH, DXGI format F), depth an engine depth-stencil`, for each of
  the four book targets;
- `[ORG] Scaleform: 4 scratch images of WxH for GFx's render targets`;
- `[ORG] Scaleform: N pipelines queued for the <role> surface`: more than before, because of `FILTER` 1-22.

At runtime:
- `[ORG] Scaleform: the <role> texture was made again (WxH); its surface draws it as the slot's next version, nothing is built`:
  a resize, with no build.
- `[ORG] Scaleform: the largest segment so far has N commands (drawn in runs of at most 1024)`.

## What to test

Run each mode: guest with the latch on, native Vulkan, native D3D12. In each, `CS_ORG_SCALEFORM=1`, `CS_ORG_LOADING_THREAD=1`,
and `CS_ORG_RESIDUE_CENSUS=1` to see the engine side. Each window, and what it shows:

| Window | What it exercises |
| --- | --- |
| Main menu, a loading screen | kMAIN, the loading thread |
| HUD (compass masks) | masks on kMAIN's depth-stencil |
| Inventory, with item previews | the 3D preview itself is menu 3D, not Scaleform |
| Magic and skills menus | the skills menu draws on top of the sky scene |
| Map | the map's frame masks |
| Journal, console, race menu | sliders, many displays |
| A book and a note or letter | render textures into `kBOOK_TEXT_*`: the pages must show text |

**Filters and render targets.** Vanilla menus showed no filter in earlier runs. To see filters, use a UI mod that uses Flash
drop shadows or glows (SkyUI's menus).

Check in the shots:
- **book pages:** text present, the right way up;
- **filters:** drop shadow and glow offset, colour and softness match the D3D11 build (`CS_ORG_SCALEFORM=0`);
- **masks:** the compass and the map's frame are clipped.

Check in the log:
- the census reaches "1000 on the graph";
- no `could not be built`;
- no `no scratch image could take`;
- no `a render target that is not a surface`. If one appears, add the named engine target to `kRoles`.

## What can trigger a build

- A new tracked texture read in native mode (the client registers again, as before). Displays no longer wait for that build.
- A role's texture made again as another graph resource. This is rare: a role whose import slot changed shape class, or HDR's
  twin made again.

The scratch images and the book surfaces are static.

## Not verified without a build

Everything above is written blind:
- the PixelBuffer scratch registration, and the in-pass `LoadOp::Clear` with a `ClearValue` of format R8G8B8A8;
- the filter variants: `FilterPS.hlsl` 1-22 pass `check-dual-target-shaders.py` (FXC, SPIR-V and DXIL), but none has run in
  game.
