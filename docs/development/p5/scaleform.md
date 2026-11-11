# P5.10: Scaleform on the render graph

The design and RE behind Scaleform on ORG ([org-full-port.md](../org-full-port.md) P5.10). R5 in
[engine-d3d11-surface.md](../engine-d3d11-surface.md) maps the renderer; this file adds what the port needed. Found with Ghidra
and the Address Library on 2026-11-10, SE 1.5.97 and AE 1.6.1170. The code is `src/RenderGraph/Engine/Scaleform/` and the
shaders `package/Shaders/Scaleform/`, behind `CS_ORG_SCALEFORM=1` (default off).

## Status

- **Built (wave 1):** every display of the HUD and of menus that use solid, bitmap and gouraud fills, glyphs (`DrawBitmaps`),
  blend modes, the blend stack and stencil masks, drawn on the render owner onto the framebuffer or HDR's UI texture.
- **Built (P5.10 finish, 2026-11-11, code only):**
  - The texture fallback ("a texture's view cannot be resolved", about 1,790 displays a loading-screen run) was **one texture**:
    GFx's glyph cache, an `A8_UNORM` texture, whose view BasicRHI could not describe (`[DCLF] Game texture view not supported:
    format 65, dimension 4`, the only such line in every run). Every display with text fell back. BasicRHI now has `A8_UNorm`
    (`VK_FORMAT_A8_UNORM_KHR`). On a device that cannot sample, store and render to A8, DXVK stores it as R8 read as
    (0, 0, 0, R); BasicRHI stores it the same way (its per-device format remap, `Device::QueryFormatSupport`), so a descriptor
    names A8 and BasicRHI makes the R8 view with that swizzle.
  - GFx's own textures (images, gradients, the glyph cache) are made on ORG at creation (below, "Lifetime of textures").
  - A display no longer falls back whole. What the graph does not draw becomes a **D3D11 part** between graph segments (below,
    "D3D11 parts"): render to a texture, filter sources, blur/shadow/glow/bevel filters, line strips, 3D, 32-bit indices,
    video textures.
  - The colour matrix filter onto a display's target is a graph draw with the transcribed `FilterVS.hlsl`/`FilterPS.hlsl`.
- **Still D3D11:** the blur family's passes (`DrawBlurRect`, a D3D11 part), everything drawn into GFx's own render targets
  (their textures are then read by graph draws), a whole display into a render texture (`kViewportRenderTexture`), and the
  parts above. In the 15 runs before this change (HUD, inventory, map, journal, loads) **no** filter, render target, line strip
  or render-texture display appeared at all; the census line says how often they occur now.
- **Shaders:** all 53 embedded sources are transcribed and checked (below, "Shaders"); `FILTER` 23/24 now have a C++ user.
- **Run in game:** in the last P5.10 run 3,996 of 4,000 displays were drawn on the graph. No menu in the test runs made a D3D11
  part, so that path has not run; `CS_ORG_SCALEFORM_D3D11_PARTS=N` forces every Nth draw into one (scenario
  `scaleform-org1-parts.env`).
- **Phase 5 review fixes (2026-11-11, code only):** GFx's textures are made through the guest device's interception point
  instead of a device vtable patch; A8 goes through BasicRHI's format remap; the D3D11 blend stack is rebuilt for each part;
  a segment starts from the renderer's live arrays; kept calls are plain records that replay from the staging bytes; the
  capture is touched only on the render owner.

## Census

Every 1000 displays, and the first time each reason comes up (`[ORG] Scaleform: the D3D11 renderer draws: <reason>`):

```
[ORG] Scaleform census: N displays: G on the graph, M on the graph with D3D11 parts, D on D3D11; S graph segments; D3D11 parts: <count reason, ...>
```

- **on the graph:** every draw of the display was a graph draw (one or more segments, no D3D11 part).
- **on the graph with D3D11 parts:** graph segments with D3D11 parts between them.
- **on D3D11:** no graph segment: a whole display refused at `BeginDisplay` (off the render thread, before the graph is up, no
  video mode, into a render texture, no render target), or one whose only segment the graph could not draw.
- **D3D11 parts** counts each part by reason; a whole display refused at `BeginDisplay` counts once under its reason ("a whole
  display ..."). Segment failures name the graph's reason (a new surface, a pipeline, a texture's view, the epoch).

Expected after this change: no "a texture's view cannot be resolved"; the remaining whole-display reasons are the first two
displays per surface ("a new surface (drawn from the next display)") and a few "the client was not in the Interface epoch"
right after a registration (6 in a loading run before).

## The renderer object

`GRendererD3D1x` is 0x8D8 bytes; its layout is the same on SE and AE (checked on the pure setters below). What the port reads:

| Offset | What | Written by |
| --- | --- | --- |
| +0xC0 | video mode set (byte): every draw is skipped without it | `SetDependentVideoMode` |
| +0xD8, +0xE0 | the RTV and DSV bound when the display began (`OMGetRenderTargets(1)`), released by `EndDisplay` | `BeginDisplay` |
| +0x1C0..+0x240 | the vertex constants' CPU copy: `mvp[4]`, then `texgen[4]` | `ApplyMatrix`, `ApplyFillTexture` |
| +0x4B8 | user matrix (`GMatrix2D`, 6 floats) | `SetUserMatrix` |
| +0x4D0 | the viewport matrix times the user matrix, made by `BeginDisplay` | `BeginDisplay` |
| +0x51C | current matrix | `SetMatrix` |
| +0x534 | current colour transform (`Cxform`: rMul, rAdd, gMul, gAdd, bMul, bAdd, aMul, aAdd) | `SetCxform` |
| +0x618 | world 3D matrix pointer (null: 2D); `BeginDisplay` resets it | `SetWorld3D` |
| +0x6A0 / +0x6A8 | vertex / index data pointers | `SetVertexData` / `SetIndexData` |
| +0x6B0 / +0x6B4 / +0x6BC | vertex format (0 none, 1 XY16i, 2 XY32f, 3 XY16iC32, 4 XY16iCF32), stride (0, 4, 4, 8, 12), count | `SetVertexData` |
| +0x6B8 / +0x6C0 | index format as DXGI (0, R16_UINT 0x39, R32_UINT 0x2A), count | `SetIndexData` |
| +0x720 | blend mode (GFx's 15); +0x728 the blend stack (`GArray`) | `PushBlendMode` / `PopBlendMode` |
| +0x7D8 | fill style (0x80 bytes): mode (0 off, 1 colour, 2 bitmap, 3 gouraud), gouraud type, colour (BGRA, cxform applied), cxform copy at +0xC, two `FillTexture`s at +0x30 and +0x58 | `FillStyle*` |
| +0x858 | line style, the same layout | `LineStyle*` |

The `VertexFormat` enum here is sequential (1 to 4), not CommonLib's bit flags. `GColor` arguments are passed by pointer.
`GTextureD3D1x` (vtable SE 0x1417dbe40 ID 292188, AE 0x141a25770 ID 243114): +0x68 renderer, +0x70 the texture (null when
its data is lost), +0x78 its SRV, +0x48 its DXGI format; slot 15 is the texture kind (0 plain, 1 YUV, 2 YUVA); slot 16 `Bind(stage,
wrap, sample)` sets t`stage` and sampler `+0x3B8 + 8 * (wrap * 2 + sample)`.

New IDs: `BSScaleformManager::InitRenderer` is AE 82730 (0x140fc31e0). Functions renamed in Ghidra (SE; the slots on AE too):
`Scaleform__GRendererD3D11__*` (BeginDisplay, EndDisplay, DrawIndexedTriList, DrawLineStrip, DrawBitmaps, the masks, the blend
stack, ApplyFillStyle, ApplyMatrix, ApplyColor, ApplyCxform, ApplyFillTexture, SetPixelShader, SetVertexShader,
UploadVertexConstants, FillStyle__SetBitmap/SetCxform/SetGouraud, Buffers__PrepareIndexed/DrawIndexed/PrepareVertices,
ReleaseQueuedResources, Create), `Scaleform__GTextureD3D11__Bind`, `Scaleform__GMatrix2D__MultiplyRight(Linear)`,
`Scaleform__GRenderer__Cxform__TransformColor`, `BSScaleformManager__InitRenderer`, `UI__DrawInterface`.

### What a draw does (D3D11)

- **Matrix** (`ApplyMatrix`): `mvp = (viewport · user) · M`, M the current matrix (the bitmaps' own matrix for `DrawBitmaps`).
  `mvp[0] = (m00, m01, 0, m02)`, `mvp[1] = (m10, m11, 0, m12)`, `mvp[2] = (0, 0, 1, 0)`, `mvp[3] = (0, 0, 0, 1)`. With a world
  3D matrix the four rows are the full projection; the port leaves 3D to D3D11.
- **Fill** (`ApplyFillStyle`), pixel shader index (+0x240 + 8 · index, the PS-A table is indices 1 to 18):
  - colour: index 1, constant = the fill colour / 255 (R from byte 2), and for Multiply or Darken `rgb = (rgb - 1) · a + 1`;
  - bitmap: index 2 (3 for Multiply or Darken), constants `cxmul`, `cxadd / 255` from the fill's cxform copy, stage 0 from
    the first `FillTexture` (texgen rows `(m00, m01, ·, m02)`, `(m10, m11, ·, m12)`); no texture leaves index 0, a null
    shader;
  - gouraud: no texture: 11 (12 for XY16iC32 data); one texture (types 1, 2): 13; two (types 3 to 5): 14 with stage 1; +4 for
    Multiply or Darken;
  - off: nothing is set; the previous shader and constants stay.
- **Vertex shader:** gouraud fills use 3 (XY16iC32), 4 (XY16iCF32) or 5 (XY16iCF32, type 3); every other fill uses 1 (the
  position alone, read as R16G16_SINT whatever the format). `DrawBitmaps` uses 2. A 3D draw adds 6.
- **Blend:** OMSetBlendState(table 0x1417d4468 [mode]) unless a mask is being submitted (then colour writes off, +0x410).
- **Geometry:** `DrawIndexedTriList(base, min, count, start, triangles)` copies the whole vertex and index arrays into its
  dynamic buffers and draws `triangles · 3` indices from `start`, with `base` added. `DrawBitmaps` writes six vertices
  (float x, y, u, v, BGRA colour; triangles LT-RT-LB, LB-RT-RB) per bitmap, linear clamp sampling, pixel shader 4 for an
  A8_UNORM texture (glyphs), else 5 (6 for Multiply or Darken). `DrawLineStrip` is a line strip of `lines + 1` vertices.

**BeginDisplay(background, viewport, x0, x1, y0, y1)** clips the viewport to the buffer (and to the scissor rectangle with
`kUseScissorRect`), moving the frame rectangle with it, sets that viewport, and makes the viewport matrix
`[2 / w, 0, -1 - 2 x0 / w; 0, -2 / h, 1 + 2 y0 / h]` times the user matrix. It resets the blend stack, the world 3D
matrix and the mask state, binds the states and, when the background's alpha is not zero, sets the identity matrix and draws
the frame rectangle in the background colour.

**Masks** use the DSV bound at `BeginDisplay` (kMAIN's, see below), with five states made by `CreateShaders`: off;
clear (ALWAYS, REPLACE, ref 1; `BeginSubmitMask(kClear)` first clears the stencil to 0); increment and decrement (EQUAL,
INCR / DECR wrapping, ref = the current level, which then moves); test (EQUAL, KEEP, write mask 0, after `EndSubmitMask`).
Depth is never tested.

**Blend states** (array 0x1417c1d48, renderer indices 1 to 7): 1 SrcAlpha/InvSrcAlpha (alpha One/InvSrcAlpha); 2 add
(SrcAlpha/One, alpha Zero/One); 3 subtract (the same, REV_SUBTRACT); 4 multiply (DstColor/Zero, alpha DstAlpha/Zero);
5 lighten (MAX); 6 darken (MIN); 7 no colour write. The modes map 0-14 to 1,1,1,4,1,5,6,1,2,3,1,7,7,1,1. A second array
(0x1417c2588, source factor One) is used only by the filters.

**Samplers** (+0x3B8): wrap · 2 + sample, point or MIN_MAG_MIP_LINEAR, WRAP or CLAMP, LOD bias -0.5, MaxLOD 0.

### Where Scaleform draws

`UI::DrawInterface` (SE 79947 / 0x140ebf510, AE 82084; CS's `MenuManagerDrawInterfaceStartHook` sits on it) binds render
target 0 (the framebuffer when none is bound; HDR's UI texture when CS's HDR redirects it), render target 1 = kMOTION_VECTOR,
render target 2 = the TAA mask swap target (none when RT 0 is the framebuffer) and **kMAIN's depth-stencil**, then calls each
visible menu's `PostDisplay` (which calls `GFxMovieView::Display`, one `BeginDisplay`/`EndDisplay` pair per movie). The
renderer reads only RT 0 (`OMGetRenderTargets(1)`); its shaders write one target. The HUD, the menus, the cursor and the
fader are separate displays, several a frame.

## Shaders

The exe holds 55 blobs (51 distinct) and 53 HLSL sources. `tools/check-scaleform-shaders.py` reads both through Ghidra,
compiles each source with FXC and pairs it with its blob: 46 are identical, 5 differ only in register allocation (the game's
compiler is 9.29.952), and two sources (highlight-only shadows) have no blob. CS's transcriptions compile, for each variant, to
the same instructions as the embedded source: 53 of 53.

| File | Variants | D3D11 table |
| --- | --- | --- |
| `Scaleform/FillVS.hlsl` | `VS_KIND` 1-5, `VS_3D` | VS-A slots 0-4, 6-10 (renderer indices 1-5, 7-11) |
| `Scaleform/FillPS.hlsl` | `PS_KIND` 1-18 | PS-A slots 0-17 (renderer indices 1-18) |
| `Scaleform/FilterVS.hlsl` | one | VS-B slot 1 |
| `Scaleform/FilterPS.hlsl` | `FILTER` 1-24 (the file lists each PS-B slot's) | PS-B slots 1-26 |

Under FXC each is the game's shader: the same signature, constant buffer and resources. The ORG build (DXC, `CS_ORG_BINDLESS`)
has no input layout or constant buffer: the vertex stage reads its vertex by `SV_VertexID` from the display's staging buffer
(the index, the base vertex, then the attributes as the input layout described them) and the draw's constants from a record
there; textures and samplers come from the heaps by the indices in the push block (`Scaleform/Fill.hlsli`,
`Scaleform/Filter.hlsli`). The stages' varyings sit at fixed locations, so every fill vertex stage feeds every fill pixel
stage.

## The design

### A wrapping vtable, one display at a time

The D3D11 renderer stays. `Install` (the seam in `Hooks::Install`) detours its factory (`Scaleform__GRendererD3D11__Create`,
SE 82831 / AE 84952) and gives the new object a copy of its 58-slot vtable in which the slots below are CS's. Every other slot,
and every call off the render owner (`FrameAnchors::OnRenderThread`), goes to the D3D11 renderer unchanged. The object's state is the D3D11 renderer's: CS
forwards the pure setters (they write fields and touch no D3D11 state) and reads the fields at each draw.

A display (`BeginDisplay` to `EndDisplay`) is a sequence of graph segments and D3D11 parts, in D3D11's order:

1. `BeginDisplay` on the render owner, with the graph up, starts a capture. Nothing reaches D3D11. The display's calls are
   kept as plain records (the slot, its arguments, and copies of what they point at: matrices, bitmap lists, fill textures,
   in the segment's bytes), and each draw is turned into an ORG draw at once: its pipeline (vertex kind, pixel kind, blend,
   stencil), a record with the D3D11 constants, its geometry in the segment's staging bytes, its textures' SRVs.
2. A call the graph does not take makes a **D3D11 part** (below): the segment captured so far is drawn first, then the D3D11
   renderer draws the part, then the capture goes on with a new segment. The order is the D3D11 order.
3. `EndDisplay` submits the last segment: the Interface anchor runs its epoch there, drawing it onto the bound target. If the
   epoch does not run, or the client was not in it, or the commit failed, the segment's kept calls are replayed instead. Then
   the D3D11 `EndDisplay` runs either way: it releases the textures GFx queued for release, and the targets a D3D11
   `BeginDisplay` took.

### D3D11 parts

A display is a sequence of graph segments and D3D11 parts. Each segment is one `Passes::Submit` (one Interface epoch run).

- **Starting a part** (`BeginD3D11Part`): the segment so far is submitted (or replayed into D3D11 when the graph cannot draw it).
  The first part of a display runs the D3D11 `BeginDisplay` with the saved arguments and a zero background (the graph drew the
  background), puts back the world 3D matrix it resets, and then sets the D3D11 renderer's blend mode (+0x720) and mask state
  (+0x4A0 submitting, +0x4A4 level, +0x4A8 the depth-stencil state from +0x468's five, bound with the mirrored reference) to the
  mirrored state. That is the state the renderer's own mask calls leave, so its draws and `PopRenderTarget` (which rebinds
  +0x4A8 with +0x4A4) behave as in a D3D11 display. Every part also rebuilds the renderer's blend stack (+0x728, a
  `GArray<uint32>`: data, size at +0x730, capacity) from the mirrored one, through its own `PopBlendMode` until empty and a
  `PushBlendMode(0)` per entry with +0x720 set to the entry (a push stores the mode before it), so the display's later
  `PopBlendMode` calls pop what GFx pushed. Pushes and pops inside a part go to D3D11 alone and are assumed balanced.
- **Render targets:** `PushRenderTarget` / `PushTempRenderTarget` at depth 0 start a part; until the matching `PopRenderTarget`
  every call goes to D3D11 directly (nested pushes counted). GFx's render target textures are DXVK's; graph draws later read them
  through GpuTextures like any texture, in-stream after the D3D11 work.
- **Filters onto the display's target:** `DrawBlurRect` (its intermediate passes push and pop temp targets through the same
  vtable, which pass straight through while the filter runs) is a part. `DrawColorMatrixRect` is a graph draw (below); a part
  only when its source cannot be read.
- **Single calls:** a draw the graph does not take (a line strip, a 3D draw, 32-bit indices, XY32f triangles, gouraud types
  4-5, the fill off, a video or lost texture) is a part of one call.
- **Resuming:** after a target or filter part, render target 0 must still be the display's target (GFx's target stack rebinds
  the display render target it was given, +0x740); otherwise the rest of the display is D3D11's. After 8 parts the rest of the
  display is D3D11's too (each part costs an Interface epoch run).
- **Replaying a later segment** (the graph could not draw it): the D3D11 renderer is set to the blend stack, blend and mask
  state the segment began in, then its kept calls run. A segment's first kept calls set the arrays the renderer's fields held
  when it began (+0x6A0 to +0x6C0, read live: `KeepLiveArrays`), so a replay draws from them whatever GFx set later. After a
  replay, GFx's current arrays go back through the setters (which also mark the renderer's dynamic buffers stale) and its world
  3D pointer is put back. The D3D11 renderer's own calls during a replay, an aborted call or a part's `BeginDisplay` are not
  captured (`Capture::replaying`).
- **A display without `EndDisplay`:** the next `BeginDisplay` on the render owner draws its last segment, and runs the D3D11
  `EndDisplay` when the D3D11 renderer began the display (it releases the views +0xD8/+0xE0 hold). Another thread's
  `BeginDisplay` or `EndDisplay` never touches the capture.

### The colour matrix filter

`DrawColorMatrixRect` (SE 0x140f5e840) draws one rectangle: vertex shader VS-B 1 with the current matrix (`ApplyMatrix(+0x51C)`),
pixel shader PS-B 25 (26 for Multiply and Darken), a strip of four vertices `(x, y, u, v)` from the destination rectangle and the
source rectangle over the texture's size (+0x38/+0x3C), the source bound with `Bind(0, clamp, linear)`, and the filters' blend
array (+0x418, `table[mode]`) unless a mask is being drawn. The pixel constants are 80 bytes: `cxadd` (the matrix argument's
floats 16-19) at 0, the column-major matrix (floats 0-15) at 16. The graph draw is a `FilterRecord` (Display.h): six vertices
(float4 position, float2 coordinate), the matrix, the constants, drawn by `FilterVS.hlsl`/`FilterPS.hlsl` with `FILTER` 23/24.

The filters' blend array (SE 0x1417c2588) is the first one with source colour factor One where the first has SrcAlpha
(multiply keeps DstColor; entry 7 writes nothing). Read from memory:

| Index | First array (0x1417c1d48) | Filters' array (0x1417c2588) |
| --- | --- | --- |
| 1 normal | SrcAlpha, InvSrcAlpha, Add; alpha One, InvSrcAlpha | One, InvSrcAlpha, Add; alpha the same |
| 2 add | SrcAlpha, One, Add; alpha Zero, One | One, One, Add |
| 3 subtract | SrcAlpha, One, RevSub; alpha Zero, One | One, One, RevSub |
| 4 multiply | DstColor, Zero, Add; alpha DstAlpha, Zero | the same |
| 5 lighten | SrcAlpha, One, Max; alpha One, One | One, One, Max |
| 6 darken | SrcAlpha, One, Min | One, One, Min |
| 7 none | no colour write | the same |

### The blur family (not on the graph yet)

`DrawBlurRect` (SE 0x140f5db10) runs `passes` passes (params +0xC, doubled when the box is larger than a threshold; flag 0x20000
skips to the last). Each pass but the last pushes a temp target (`PushTempRenderTarget(frame, w, h, false)`, vtable +0x50),
draws the previous pass's texture through PS-B 21/22 (one- or two-dimensional blur), pops it and releases the previous texture;
the last pass draws onto the current target with the current matrix and the filter's own pixel shader (inner/outer shadow,
highlight, knockout, shadow only, chosen from the flags at +0 bits 1, 2, 6, 8, 10; +2 for Multiply and Darken), reading the
original source as `srctex` (t1). Its constants come from a per-shader offset table at SE 0x141e0ac70 (the reflection's offsets
of cxadd, cxmul, fsize, offset, scolor, scolor2, srctexscale, texscale, and the sampler slot). Putting it on the graph needs
GFx's temp targets as graph resources that one display's passes write and read in turn (render passes on ORG images with
barriers between them inside the Interface pass, or a pass per filter pass), and that table transcribed. Until then the census
shows how often it runs.

The background rectangle `BeginDisplay` draws is one of the display's draws.

### Slot by slot

"Pure": writes fields, no D3D11 call. "Kept": recorded for a replay. "Abort": a D3D11 part of that one call (above).

| Slot | Function | D3D11 renderer | ORG path |
| --- | --- | --- | --- |
| 0 | dtor | releases everything | unchanged |
| 1 | GetRenderCaps | caps (filters, render targets, gouraud) | unchanged: GFx keeps using the same features |
| 2, 3 | CreateTexture, CreateTextureYUV | GTextureD3D1x under +0x640's lock (loader threads) | unchanged; the textures' own `InitTexture`/`InitDynamicTexture` make plain ones on ORG (below, "Lifetime of textures"), the rest stay DXVK's |
| 4, 5 | BeginFrame, EndFrame | no-op; event handlers | unchanged |
| 6 | CreateRenderTarget | a GRenderTargetD3D1x | unchanged |
| 7 | SetDisplayRenderTarget | +0x740, OMSetRenderTargets | in a display: a D3D11 part, then the rest of the display is D3D11's |
| 8, 9, 10 | Push/PopRenderTarget, PushTempRenderTarget(frame, w, h, stencil) | render to texture (filters, cached shapes) | a D3D11 part from the push to the matching pop |
| 11 | ReleaseTempRenderTargets | releases pooled targets | unchanged |
| 12 | BeginDisplay | viewport, matrix, states, background | starts a capture (render owner, graph up, video mode set, not a render texture) |
| 13 | EndDisplay | releases RT/DSV, queued releases | submits (or replays), then the D3D11 EndDisplay |
| 14, 15, 16 | SetMatrix, SetUserMatrix, SetCxform | pure | forwarded and kept |
| 17, 18 | PushBlendMode, PopBlendMode | stack, OMSetBlendState | mirrored (the stack's rule) and kept; +0x720 written at the end |
| 19, 20 | PushUserData, PopUserData | no-op | unchanged |
| 21, 22, 23 | SetPerspective3D, SetView3D, SetWorld3D | pure | forwarded and kept; a draw with a world matrix aborts |
| 24, 25, 26 | MakeViewAndPersp3D, stereo | math, pure | unchanged |
| 27, 28 | SetVertexData, SetIndexData | pure | forwarded and kept; the array copied into the segment's staging bytes at its first draw there, which a replay reads |
| 29 | ReleaseCachedData | no cache | unchanged |
| 30 | DrawIndexedTriList | fill, matrix, draw | an ORG draw; aborts for XY32f, 32-bit indices, gouraud types 4-5, fill off, a bitmap fill without texture, 3D |
| 31 | DrawLineStrip | line strip | abort (raster programs draw triangle lists) |
| 32-37 | Line/Fill style setters | pure | forwarded and kept |
| 38 | DrawBitmaps | glyph quads | an ORG draw; aborts for a texture that is not a plain GTextureD3D1x with data |
| 39 | DrawDistanceFieldBitmaps | no-op | unchanged |
| 40, 41, 42 | Begin/EndSubmitMask, DisableMask | stencil states, clear | mirrored and kept: a stencil clear command and each draw's stencil state and reference; a segment with a mask whose depth target the graph cannot import is replayed |
| 43 | CheckFilterSupport | answers the caps | unchanged |
| 44 | DrawBlurRect | filter passes on temp targets | a D3D11 part |
| 45 | DrawColorMatrixRect | one rectangle | a graph draw (FILTER 23/24); a D3D11 part when its source cannot be read |
| 46-57 | no-ops, stats, ReleaseResources, handlers, video mode, display status, GetDevice/Context | | unchanged |

### The ORG pass

- **Anchor:** a new one, `Interface`, run at every `EndDisplay` the graph takes (several times a frame). It is a late
  anchor: frame generation or the present can follow its epoch, so the hazard separator runs after it. Its frame slots are
  frames in flight times its runs a frame (6 in `FrameAnchors.cpp`'s anchor table: the HUD, the open menus, the cursor and the
  fader are each a display, and each D3D11 part adds a run).
  Running each display's epoch at its `EndDisplay` keeps the D3D11 order exact (menu 3D, Bink or another mod's D3D11 drawing
  between two movies stays between them). One epoch per frame at the end of `UI::DrawInterface` would be cheaper and is the
  next step once nothing D3D11 draws between displays (P5 menu 3D).
- **Surface:** the RT 0 texture bound at `BeginDisplay` (read from DXVK's context, so tripwire handles do not matter) is the
  engine target it belongs to (`FrameAnchors::EngineTarget`) or, for HDR's UI texture, an import of its own
  (`cs.scaleform.target.<n>`). The DSV is kMAIN's (`EngineDepth`). Each (target, depth) pair is a surface with a pass of its
  own in the client; a new surface registers the client again and that display goes to D3D11 (a client registered now is in
  the next run). Steady state has one or two surfaces and no rebuilds.
- **Per display:** the commit copies the display's bytes (geometry, then one 192-byte record per draw) into a host-visible
  staging buffer from a pool, resolves each texture's SRV to a descriptor (`DCLF::GpuTextures::ResolveBinding`, DXVK's image
  in its resting layout, untracked), and stores the frame in the surface's slot for this frame slot (and nothing in the
  others). The pass records one render pass: the viewport and scissor of `BeginDisplay`, then per command a stencil clear
  (`ClearDepthStencilView`, which BasicRHI turns into `vkCmdClearAttachments` over the render area inside the pass) or a draw
  (pipeline, stencil reference, push block, `Draw`). The recording keeps the frame, its staging buffer and its texture
  owners until its GPU work retires.
- **Pipelines:** a `RasterProgram` per (vertex kind, pixel kind, blend state, stencil state, colour format, depth format),
  made when a display first needs it (blocking on the shader store at the first use only). A pipeline that cannot be made
  sends that display to D3D11 and is not tried again.

### Fallback

A segment the graph cannot draw is replayed into D3D11 (a target or depth that cannot be imported, a new surface, a pipeline
that cannot be made, a texture whose view cannot be resolved, an epoch that did not run); a display refused at `BeginDisplay`
is D3D11's whole. Each reason is logged once and counted in the census (above). The graph's failure (`onFailed`) stops captures
for the session.

### Lifetime of textures and caches

- **Textures** (`GfxTextures.cpp`): `GTextureD3D1x` (vtable SE 292188 / AE 243114, the same slots on both) makes its texture in
  slot 1 `InitTexture(GImageBase*)` (images, gradients: DEFAULT, SRV, the image's mips as initial data) or slot 2
  `InitDynamicTexture(w, h, format, mips, usage)` (the glyph cache: format 9 = A8_UNORM, filled with 0x40; usage 0x20 makes it
  DYNAMIC, 0x40 adds a render target bind, format 0x12E is a D24S8 depth target). Both call the renderer's device (+0x68, then
  +0x490) `CreateTexture2D` then `CreateShaderResourceView`. That device is the tripwire guest device handle, so with
  `CS_ORG_ENGINE_TEXTURES=1` and `CS_ORG_TRIPWIRE_GUEST=1`, while slot 1 or 2 runs on a thread they set a
  `Tripwire::ScopedCreateTexture2DHook`, and the guest device's `CreateTexture2D` asks it first: it makes a DEFAULT
  shader-resource 2D texture through `Engine::Textures::Create` (an ORG image, DXVK's texture on it with GFx's initial data, a
  tripwire handle in GFx's field). No vtable is patched. Without the guest the renderer holds DXVK's own device, shared with CS,
  and GFx's textures stay DXVK's. GFx's own view of it is found by `Textures::Find` through its texture, so
  GpuTextures writes the descriptor from the record: no DXVK import, no pin, no relocation. Render targets, DYNAMIC and depth
  textures, textures made before the graph can wrap images, and formats the device cannot sample stay DXVK's and are imported
  per view (once each). A8 on a device without it is made as R8 on both sides (BasicRHI's format remap, as DXVK's). The
  external images (`BSScaleformExternalTexture`, 75511) are engine textures already; GFx's view of them is found the same way.
- The glyph cache is updated with `UpdateSubresource` on the immediate context during `Display`, before the draws that use it. The ORG draws run at `EndDisplay`, after every update of that display. The one
  difference: an update made after a draw that read the old texels in the same display (a font cache eviction within one
  display) is seen by that draw too. GFx's font cache keeps the glyphs a display uses until it ends, so this should not
  happen _(inferred, not checked)_.
- A captured draw keeps a reference to its SRV until the commit resolved it; the frame then keeps the descriptor's owner
  and the SRV until the recording's GPU work retires. `EndDisplay`'s queued releases therefore cannot free a texture the
  graph still reads.
- **Vertex and index data** are GFx's memory, valid while they are set (the D3D11 renderer reads them at every draw): the
  capture copies each array once per segment, into the segment's staging bytes, at the first draw there that reads it. A
  replay passes those copies to the D3D11 renderer instead of GFx's pointers, then sets GFx's back.
- The D3D11 renderer's dynamic buffers and constant buffers are not used by the ORG path.

### The loading-screen thread (with P5.12)

**Now (P5.12 merged):** with `CS_ORG_LOADING_THREAD=1` the serving thread is the render owner while it draws, and its displays
are captured like the main thread's. In the last loading run before this change 5,373 of 7,000 displays were on the graph; the
rest were the glyph-cache fallback (1,619), two new-surface displays and six "not in the Interface epoch". The A8 fix and the
ORG-made textures cover the first; GFx's textures made on loader threads go through `Textures::Create` too (any thread). The
original proposal, kept for its reasoning:

The loading screen draws on `JobListManager::ServingThread` (38085 / 39039) under `Renderer::Lock`, while the main thread
loads. Anchors ran only on the main thread (`FrameAnchors::OnMainThread`), so every display there went to D3D11. The proposal
for P5.12:

- **Render ownership instead of the main thread.** The thread allowed to run anchors becomes the render owner. The main
  thread owns rendering, except between `SetLoadingScreenActive(true)` and `(false)` (34552 / 35407), which hand ownership
  to the serving thread and back. The two never render at once: mode 2 starts after the main thread's last frame ended and
  the main thread waits (+0x60) for the loop to stop before it renders again, and both hold `Renderer::Lock` while they draw.
- **What moves with ownership:** the graph's frame upload instance and the frame slot ring belong to the owner. The handoff
  happens with no epoch in progress (outside `Renderer::Lock`), so a release-acquire on the owner id is enough.
- **Then** the loading screen's displays take the ORG path like the main thread's, and its `Present` runs the Present
  anchor; the loading menu's 3D model (MistMenu, `UI3DSceneManager`) is P5's menu 3D work.
- **Built** in P5.12 behind `CS_ORG_LOADING_THREAD=1` ([loading-screen.md](./loading-screen.md)): the capture checks
  `FrameAnchors::OnRenderThread()`. The owner is claimed by the thread holding `Renderer::Lock` rather than at
  `SetLoadingScreenActive(true)`, which only starts the loop; `SetLoadingScreenActive(false)` hands it back.

### The switch

`CS_ORG_SCALEFORM=1` (read once) installs the factory detour and reserves the Interface anchor at `PostPostLoad`. Off, nothing
is hooked and the D3D11 renderer is the only one. The ORG path starts at the first display after the graph is up, and stops for
good on a graph failure.

## Testing

Scenarios `tools/cs-ab/scenarios/scaleform-org0.env` (D3D11) and `scaleform-org1.env` (ORG), both with the tripwire guest, the
P4 engine switches and `CS_ORG_LOADING_THREAD=1`, shoot the main menu (frame 400), the loading screen of `coc Whiterun` (730; it
may land after the load), the HUD, the inventory, the magic menu, the map and the journal. Compare the shots. In the -org1 log:

1. `[ORG] Scaleform: drawing displays on the render graph`, `GTextureD3D1x's InitTexture and InitDynamicTexture hooked`,
   `GFx's textures go through Engine::Textures::Create` and `GFx's textures are made on ORG (first: ...)`.
2. No `a texture's view cannot be resolved`, no `[DCLF] Game texture view not supported: format 65`.
3. The census lines; every `the D3D11 renderer draws: <reason>` line (each reason once) says what is left.
4. No `[ORG] Scaleform: pipeline ... could not be made` (the filter pipelines are new: `filter 23` / `filter 24`).

`scaleform-org1-parts.env` is the same tour with `CS_ORG_SCALEFORM_D3D11_PARTS=5`: the census should count `a draw made a D3D11
part by CS_ORG_SCALEFORM_D3D11_PARTS` and `too many D3D11 parts in one display`, and the shots should still match.

What a build must confirm first: no `[E]` from the anchor epoch, glyphs (now A8 on the graph), masks (the compass, the map's
frame), blend modes, and any menu with a D3D11 part look as on D3D11.
