# P5.10: Scaleform on the render graph

The design and RE behind Scaleform on ORG ([org-full-port.md](../org-full-port.md) P5.10). R5 in
[engine-d3d11-surface.md](../engine-d3d11-surface.md) maps the renderer; this file adds what the port needed. Found with Ghidra
and the Address Library on 2026-11-10, SE 1.5.97 and AE 1.6.1170. The code is `src/RenderGraph/Engine/Scaleform/` and the
shaders `package/Shaders/Scaleform/`, behind `CS_ORG_SCALEFORM=1` (default off).

## Status

- **Built:** every display of the HUD and of menus that use solid, bitmap and gouraud fills, glyphs (`DrawBitmaps`), blend
  modes, the blend stack and stencil masks, drawn on the main thread onto the framebuffer or HDR's UI texture.
- **Still D3D11:** a display that uses a filter (blur, glow, drop shadow, bevel, colour matrix), a render target of its own,
  3D, a line strip, a YUV video texture or a 32-bit index list; every display on the loading-screen thread; and any display
  the graph cannot take (below, "Fallback"). Such a display is drawn whole by the D3D11 renderer, at the same point.
- **Shaders:** all 53 embedded sources are transcribed and checked (below, "Shaders"); the filter shaders have no C++ user yet.
- **Not run in game yet** (code only; the main session builds and tests).

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
and every call off the main thread, goes to the D3D11 renderer unchanged. The object's state is the D3D11 renderer's: CS
forwards the pure setters (they write fields and touch no D3D11 state) and reads the fields at each draw.

A display (`BeginDisplay` to `EndDisplay`) is either the graph's or D3D11's, never mixed:

1. `BeginDisplay` on the main thread, with the graph up, starts a capture. Nothing reaches D3D11. The display's calls are
   kept, with copies of what they point at (vertex and index arrays, bitmap lists, matrices, fill textures), and each draw is
   turned into an ORG draw at once: its pipeline (vertex kind, pixel kind, blend, stencil), a record with the D3D11
   constants, its geometry in the display's staging bytes, its textures' SRVs.
2. A call the graph cannot take (the list in Status) **aborts** the capture: the kept calls are replayed into the D3D11
   renderer from its `BeginDisplay`, and the rest of the display goes there directly. The output is the D3D11 output: the
   replay happens at the same point of the D3D11 stream, before anything else is drawn.
3. `EndDisplay` submits the display: the Interface anchor runs its epoch there, drawing the display onto the bound target. If
   the epoch does not run, or the client was not in it, or the commit failed, the kept calls are replayed instead. Then the
   D3D11 `EndDisplay` runs either way: it releases the textures GFx queued for release.

The background rectangle `BeginDisplay` draws is one of the display's draws.

### Slot by slot

"Pure": writes fields, no D3D11 call. "Kept": recorded for a replay. "Abort": ends the capture (replay, then D3D11).

| Slot | Function | D3D11 renderer | ORG path |
| --- | --- | --- | --- |
| 0 | dtor | releases everything | unchanged |
| 1 | GetRenderCaps | caps (filters, render targets, gouraud) | unchanged: GFx keeps using the same features |
| 2, 3 | CreateTexture, CreateTextureYUV | GTextureD3D1x under +0x640's lock (loader threads) | unchanged: textures stay D3D11; the graph reads their SRVs |
| 4, 5 | BeginFrame, EndFrame | no-op; event handlers | unchanged |
| 6 | CreateRenderTarget | a GRenderTargetD3D1x | unchanged |
| 7 | SetDisplayRenderTarget | +0x740, OMSetRenderTargets | abort in a display, else unchanged |
| 8, 9, 10 | Push/PopRenderTarget, PushTempRenderTarget | render to texture (filters, cached shapes) | abort |
| 11 | ReleaseTempRenderTargets | releases pooled targets | unchanged |
| 12 | BeginDisplay | viewport, matrix, states, background | starts a capture (main thread, graph up, video mode set, not a render texture) |
| 13 | EndDisplay | releases RT/DSV, queued releases | submits (or replays), then the D3D11 EndDisplay |
| 14, 15, 16 | SetMatrix, SetUserMatrix, SetCxform | pure | forwarded and kept |
| 17, 18 | PushBlendMode, PopBlendMode | stack, OMSetBlendState | mirrored (the stack's rule) and kept; +0x720 written at the end |
| 19, 20 | PushUserData, PopUserData | no-op | unchanged |
| 21, 22, 23 | SetPerspective3D, SetView3D, SetWorld3D | pure | forwarded and kept; a draw with a world matrix aborts |
| 24, 25, 26 | MakeViewAndPersp3D, stereo | math, pure | unchanged |
| 27, 28 | SetVertexData, SetIndexData | pure | forwarded; kept with a copy of the array (at its first draw) |
| 29 | ReleaseCachedData | no cache | unchanged |
| 30 | DrawIndexedTriList | fill, matrix, draw | an ORG draw; aborts for XY32f, 32-bit indices, gouraud types 4-5, fill off, a bitmap fill without texture, 3D |
| 31 | DrawLineStrip | line strip | abort (raster programs draw triangle lists) |
| 32-37 | Line/Fill style setters | pure | forwarded and kept |
| 38 | DrawBitmaps | glyph quads | an ORG draw; aborts for a texture that is not a plain GTextureD3D1x with data |
| 39 | DrawDistanceFieldBitmaps | no-op | unchanged |
| 40, 41, 42 | Begin/EndSubmitMask, DisableMask | stencil states, clear | mirrored: a stencil clear command and each draw's stencil state and reference; abort without a depth target the graph has |
| 43 | CheckFilterSupport | answers the caps | unchanged |
| 44, 45 | DrawBlurRect, DrawColorMatrixRect | filter passes on temp targets | abort |
| 46-57 | no-ops, stats, ReleaseResources, handlers, video mode, display status, GetDevice/Context | | unchanged |

### The ORG pass

- **Anchor:** a new one, `Interface`, run at every `EndDisplay` the graph takes (several times a frame). It is a late
  anchor: frame generation or the present can follow its epoch, so the hazard separator runs after it. Its frame slots are
  frames in flight times the epochs, as every anchor's; with about ten displays a frame it uses its ring that much faster.
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

Every step falls back to D3D11 for that display, logged once per reason, with counts every 1000 displays
(`[ORG] Scaleform: ...`): the graph not up or failed, a display off the main thread, an abort reason, a target or depth that
cannot be imported, a new surface, a pipeline that cannot be made, a texture whose view cannot be resolved, an epoch that did
not run. The graph's failure (`onFailed`) stops captures for the session.

### Lifetime of textures and caches

- **Textures** stay the D3D11 renderer's (`CreateTexture` is untouched, and runs on loader threads under +0x640's lock as
  before). The font cache texture is one of them: GFx updates it with `UpdateSubresource` on the immediate context during
  `Display`, before the draws that use it. The ORG draws run at `EndDisplay`, after every update of that display. The one
  difference: an update made after a draw that read the old texels in the same display (a font cache eviction within one
  display) is seen by that draw too. GFx's font cache keeps the glyphs a display uses until it ends, so this should not
  happen _(inferred, not checked)_.
- A captured draw keeps a reference to its SRV until the commit resolved it; the frame then keeps the descriptor's owner
  and the SRV until the recording's GPU work retires. `EndDisplay`'s queued releases therefore cannot free a texture the
  graph still reads.
- **Vertex and index data** are GFx's memory, valid only during the display: the capture copies each array once, at its
  first draw. The copies live until the next display begins; a replay passes them to the D3D11 renderer instead of GFx's
  pointers, then puts GFx's pointers back.
- The D3D11 renderer's dynamic buffers and constant buffers are not used by the ORG path.

### The loading-screen thread (with P5.12)

The loading screen draws on `JobListManager::ServingThread` (38085 / 39039) under `Renderer::Lock`, while the main thread
loads. Anchors run only on the main thread (`FrameAnchors::OnMainThread`), so every display there goes to D3D11 (counted as
"off the main thread"). The proposal for P5.12:

- **Render ownership instead of the main thread.** The thread allowed to run anchors becomes the render owner. The main
  thread owns rendering, except between `SetLoadingScreenActive(true)` and `(false)` (34552 / 35407), which hand ownership
  to the serving thread and back. The two never render at once: mode 2 starts after the main thread's last frame ended and
  the main thread waits (+0x60) for the loop to stop before it renders again, and both hold `Renderer::Lock` while they draw.
- **What moves with ownership:** the graph's frame upload instance and the frame slot ring belong to the owner. The handoff
  happens with no epoch in progress (outside `Renderer::Lock`), so a release-acquire on the owner id is enough.
- **Then** the loading screen's displays take the ORG path like the main thread's, and its `Present` runs the Present
  anchor; the loading menu's 3D model (MistMenu, `UI3DSceneManager`) is P5's menu 3D work.

### The switch

`CS_ORG_SCALEFORM=1` (read once) installs the factory detour and reserves the Interface anchor at `PostPostLoad`. Off, nothing
is hooked and the D3D11 renderer is the only one. The ORG path starts at the first display after the graph is up, and stops for
good on a graph failure.

## Testing

Scenarios `tools/cs-ab/scenarios/scaleform-org0.env` (D3D11) and `scaleform-org1.env` (ORG, with the tripwire guest and the
P4 engine switches) shoot the HUD in Whiterun, then the inventory, the map and the journal, each shown and hidden. Compare the
shots; in the -org1 log look for `[ORG] Scaleform: drawing displays on the render graph`, the per-reason fallback lines and the
1000-display counts. A menu whose displays all fall back (filters) looks identical by construction; the HUD and plain menus are
the ORG path.

What a build must confirm first: the pipelines compile (`[ORG] Scaleform: pipeline ...` errors), no `[E]` from the anchor
epoch, and glyphs, masks (the compass, the map's frame) and blend modes look as on D3D11.
