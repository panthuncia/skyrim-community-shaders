# snap0-ui: the native UI renderer (roadmap phase 4)

Lead `ui`, branch `snap0/ui`, based on `org_integration` 8fb9b1dc5. Roadmap: `render-thread-snapshots.md`, "Phase 4". Design
and older RE: `docs/development/engine/scaleform.md`.

## Milestones

| # | Milestone | State |
|---|---|---|
| 1 | Research: vtable, shaders, states, formats, textures, masks, targets | done (below; shaders were already transcribed) |
| 2 | Recorder and UI stream | done |
| 3 | Replay as passes, observe parity | done: 0 differences in 4,000 displays (HUD, main menu, loading, inventory, map, console) |
| 4 | The switch: native draws the replay, the game renderer not called | done for draws and state; GFx's render target stack, EndDisplay's queued releases and texture creation still call the game (counted) |
| 5 | Loading screen and logo | partly: loading-screen displays already record and replay through the same stream (the serving thread is the render owner); no separate snapshot kind yet |

## What was built

**The UI stream** (`src/RenderGraph/Engine/Scaleform/UiStream.h/.cpp`). `UiStream` is one present's GRenderer calls in call
order: `UiCommand { op, five ints, texture key, payload offset/size }` plus a byte payload (16-byte aligned), a texture table
(`TextureInfo`: the raw view with a reference, format, size, plain/has-data flags) and a target table (`DisplayTargets`:
target and depth textures with references, formats). It is finished when `FrameClock::PresentSerial` moves and published as
`std::shared_ptr<const UiStream>` (`UiStream::Latest()`). Plugging it into `FrameSnapshot` is one line: take `Latest()` at
the snapshot's capture point.

**The recorder** (native mode, render owner): every slot GFx calls becomes a command, including the pure setters (CS takes
slots 14-16, 21-23, 27-28 and 32-37 in native only). Copies:

- vertex and index arrays at the first draw after each `SetVertexData`/`SetIndexData` (GFx's memory is valid while set, and the
  D3D11 renderer reads it at the draw), and again at the first draw of a new stream;
- the world 3D matrix (`SetWorld3D` passes a pointer) at each draw when its values changed;
- bitmap lists, matrices, colour transforms, fill textures, blur parameters, colour matrices, push frames, inline;
- texture info (`Texture` command) when a draw first reads a texture in a stream or its info changed, by `ApplyFillStyle`'s
  rules (stage 1 only for two-texture gouraud fills), so a stale pointer in an unused stage is never read;
- the display targets at `BeginDisplay` (shadow state or the renderer's current target) and after `SetDisplayRenderTarget`.

**RendererState** is the renderer's state as CS keeps it. Its setters reproduce the D3D1x renderer's pure setters bit for bit
(decompiled, SE ids in the header): `TransformColor` (clamping, truncation, NaN to 255), `FillStyle::SetBitmap`, the
gouraud setter's stale stage 1, the vertex stride and index format tables. `ApplyMatrix3D` ports SE 82817's 3D path (the
cached user·view·perspective product with its dirty flag, the 2D-to-3D promotion, the multiply in the renderer's summation
order, the transpose).

**One translation, two state sources.** The translation into graph draws (the existing `Renderer.cpp` code) now reads a
`RendererState` and a `TextureInfo` lookup instead of the D3D11 renderer's fields. Guest fills the state from the fields at each
call (`RendererState::ReadFields`) and reads textures live, so guest's path is the old one. Native's replay applies the stream's
commands to its own state and reads textures from the stream's table. Each display is replayed at its `EndDisplay`, which keeps
the D3D11-era order with menu scenes and the Interface anchor's per-display runs.

**Parity (milestone 3, then deleted).** For one build the native field path kept drawing and copied each segment; the replay
of the same display was compared at `EndDisplay` semantically: draw states, stencil, vertex counts, texture identities,
samplers, the full constant records, and every vertex each draw reads (through its indices). Run `s0-ui-observe1`: 4,000
displays, 3,726 segments, **0 differences**. The scaffolding (field path in native, segment copies, comparison) was removed
with the switch, since it needs the game renderer's state, which native no longer feeds.

**The switch (milestone 4).** Native hooks record and return. The D3D11 renderer gets no setter, draw, mask, blend, display or
filter call. Still called, and counted in the census line `[ORG] Scaleform: the game's renderer was called for ...`:

- `EndDisplay`: its queued releases free GFx textures released from other threads (the stream holds its own view references);
- `PushRenderTarget`, `PopRenderTarget`, `PushTempRenderTarget`, `SetDisplayRenderTarget`: GFx's render target stack and temp
  pool. Pop releases a pooled target's texture by a flag the push set (SE 82882), so skipping one side would leak or free
  early. Their D3D11 binds and clears reach no GPU work in native (tripwire).
- The slots CS never takes (texture creation and `GTextureD3D1x` updates, `CreateRenderTarget`, `GetRenderCaps`,
  `SetDependentVideoMode`, stats). Not counted.

## Research (milestone 1)

GRendererD3D1x: 0x8D8 bytes, vtable AE 243117 (0x141a258c0), SE 0x1417dbf90. 58 slots, the same on SE and AE:

| Slot | AE id | SE id | Function | Native now |
|---|---|---|---|---|
| 0 | 84931 | 82809 | dtor | game |
| 1 | 84973 | 82852 | GetRenderCaps (needs +0xC0) | game |
| 2 | 84953 | 82832 | CreateTexture (GTextureD3D1x, 0x80 bytes, under +0x640's lock) | game |
| 3 | 84954 | 82833 | CreateTextureYUV | game |
| 4, 5 | 84446, 84450 | 82359, 82363 | BeginFrame, EndFrame | game (no-ops) |
| 6 | 84951 | 82830 | CreateRenderTarget | game |
| 7 | 85034 | 82919 | SetDisplayRenderTarget: +0x740, OMSetRenderTargets | game, recorded |
| 8, 9, 10 | 85004, 85001, 85005 | 82887, 82882, 82888 | Push/PopRenderTarget, PushTempRenderTarget | game, recorded |
| 11 | 85012 | 82895 | ReleaseTempRenderTargets | game |
| 12, 13 | 84942, 84962 | 82819, 82841 | BeginDisplay, EndDisplay | CS (End also game) |
| 14, 15, 16 | 85038, 85045, 85031 | 82922, 82929, 82916 | SetMatrix, SetUserMatrix, SetCxform | CS |
| 17, 18 | 85003, 85000 | 82886, 82881 | Push/PopBlendMode | CS |
| 19, 20 | 84455, 84454 | 82368, 82367 | Push/PopUserData | game (no-ops) |
| 21, 22, 23 | 85040, 85051, 85053 | 82924, 82933, 82935 | SetPerspective3D (+0x598), SetView3D (+0x558), SetWorld3D (+0x618 pointer) | CS |
| 24-26 | 84453, 84464, 84463 | 82366, 82377, 82376 | MakeViewAndPersp3D, stereo | game |
| 27, 28 | 85048, 85037 | 82930, 82921 | SetVertexData, SetIndexData | CS |
| 29 | 85006 | 82889 | ReleaseCachedData (no-op) | game |
| 30, 31 | 84959, 84960 | 82838, 82839 | DrawIndexedTriList, DrawLineStrip | CS |
| 32-37 | 84995, 84994, 84966, 84965, 84964, 84967 | 82875, 82874, 82845, 82844, 82843, 82846 | Line/Fill style setters | CS |
| 38 | 84956 | 82835 | DrawBitmaps | CS |
| 39 | 84449 | 82362 | DrawDistanceFieldBitmaps (no-op) | game |
| 40-42 | 84943, 84963, 84955 | 82820, 82842, 82834 | Begin/EndSubmitMask, DisableMask | CS |
| 43 | 84948 | 82825 | CheckFilterSupport | game |
| 44, 45 | 84957, 84958 | 82836, 82837 | DrawBlurRect, DrawColorMatrixRect | CS |
| 46-57 | | | stats, ReleaseResources, handlers, SetDependentVideoMode (85033/82918), ResetVideoMode, display status, GetDevice/Context | game |

Setter details read in Ghidra (SE): `SetVertexData` also clears a buffer-valid byte (+0x6E8), `SetIndexData` +0x6E9;
`LineStyleDisable`/`FillStyleDisable` clear the mode and stage 0's texture only; `FillStyleColor`/`LineStyleColor` store the
colour through the current colour transform (SE 0x140f45060); `FillStyleBitmap` (SE 0x140f632c0) sets mode 2, colour
0xFFFFFFFF, copies the colour transform and sets the add flag when an add exceeds 1.0; `FillStyleGouraud(type, t0, t1, t2)`
(SE 0x140f63580): no texture copies the colour transform only and clears stage 0; otherwise SetBitmap with t0 (or t1), stage 1
from t1 when given (else stale), then type and mode 3. `BeginDisplay` keeps the blend mode (+0x720) and clears the stack, resets
+0x618 and the mask, and draws the background through the vtable's `SetMatrix(identity)`. Blend `Push` stores the mode before
and takes modes above 2; `Pop` takes the topmost stack entry above 2, else 0.

GTextureD3D1x (vtable AE 243114 / SE 292188, 24 slots): 1 InitTexture(GImageBase*), 2 InitDynamicTexture, 3 Update (glyph
cache, `UpdateSubresource` per rect, SE 82940), 4 Map, 5 Unmap, 12 InitTexture from an `ID3D11Texture2D` (external textures),
14 ReleaseTextureData, 16 Bind(stage, wrap, sample). Shaders: the 53 transcriptions in `package/Shaders/Scaleform/` (already
checked against the embedded blobs, scaleform.md "Shaders"); vertex formats, blend states, stencil states, samplers: unchanged
from scaleform.md.

## Tests

New config `tools/cs-test/configs/ui-menus.toml`: `tgm`, a HUD shot, the inventory, the world map and the console opened
through the UI message queue, a shot in each. Pass it by path (the main checkout's cs-test reads its own configs folder):
`native-vulkan+<worktree>/tools/cs-test/configs/ui-menus.toml`. All Dev-Fast, stage `s0-ui`.

| Config | Label | Build | Result |
|---|---|---|---|
| native-vulkan+ui-menus | s0-ui-observe1 | 06e627bfb (recorder + parity) | passed; parity 4,000 displays, 3,726 segments, 0 differ; census 4,000 of 4,000 on the graph |
| native-vulkan+ui-menus | s0-ui-switch1 | bae5adf12 (the switch) | passed; 4,000 of 4,000 on the graph; game renderer called for EndDisplay only (1,000 per 1,000 displays); shots match observe1 |
| native-d3d12+ui-menus | s0-ui-d3d12-menus | bae5adf12 | passed; 5,000 of 5,000 on the graph; EndDisplay only |
| guest+ui-menus | s0-ui-guest-menus | bae5adf12 | passed; 4,000 of 4,000 on the graph (the field path, as before) |

The `[E]` lines in the native runs (engine draws dropped in menu scenes, Grass Optimizations, Effects11) are the same in the
observe run and the switch run (7 each), so they predate this work. Loading-screen displays (the serving thread, thread 8960 in
`s0-ui-switch1`) are recorded and replayed through the same stream.

## Decided by the agent, to confirm

- **Texgen rows carry across segments and displays** (CS state) instead of being re-read from the renderer's +0x200 at each
  segment start. The field was stale anyway (the D3D11 renderer stopped drawing), and texgen rows only matter for the stages a
  draw binds, which it writes. Guest's draws are unchanged by it.
- **A stream needs the streams before it.** The replay's renderer state carries from one present's stream into the next, as
  the renderer's own state does. For phase 7 (repeated and skipped snapshots) a stream needs a state header (the
  RendererState, blend mode and stack, texgen rows) at its start; not built.
- **Streams are per present** (`FrameClock::PresentSerial`), not per `Renderer::Begin`, so loading-screen presents get streams
  of their own.
- **The render target stack and texture creation stay the game's** for now (counted). Porting them needs CS-owned GTexture and
  GRenderTarget objects (the GFx side holds and refcounts them), with uploads in stream order.
- The video mode flag (+0xC0) and the display targets are still read from engine state at record time.

## Open items

- CS-owned textures: implement `CreateTexture`, `GTexture` (Init, InitDynamicTexture, Update, Map/Unmap, Bind is unused), with
  image data and glyph-cache updates copied into the stream as uploads in call order. Then `EndDisplay`'s release queue is CS's.
- CS-owned render targets: `CreateRenderTarget`, `PushTempRenderTarget`'s pool, `ReleaseTempRenderTargets`, with the stack
  refcounts of SE 82882/82887.
- Replay at frame level: once menu scenes are native (phase 5), replay the whole stream in the UI stage instead of per display.
- Blur filters, GFx render targets and 3D draws did not occur in any test (census: 0 scratch segments); their replay is the
  same translation code, but parity for them was not observed.
- Hook point for the snapshot: `MenuManager::DrawInterface`'s end (Hooks.cpp) is where a frame's UI stream is complete; today
  the stream finishes at the next present's first UI call.
