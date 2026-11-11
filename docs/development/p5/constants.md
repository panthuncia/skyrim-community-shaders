# P5.7: the engine's constants, captured per use

RE and design for plan task P5.7 ([org-full-port.md](../org-full-port.md), Phase 5): every engine constant-buffer write,
captured per use into graph memory, so the ORG passes that draw the engine's work (sky, ImageSpace, menus, DCLF) read the
constants of that exact use instead of the engine's pooled D3D11 buffers. Found with Ghidra (GhidraMCP) and the Address
Library on 2026-11-10, SE 1.5.97 and AE 1.6.1170. It extends R2 and R3 of
[engine-d3d11-surface.md](../engine-d3d11-surface.md) and answers its open questions 5 and 6 for the constant buffers.

Code: `src/RenderGraph/Engine/Constants/` (`EngineConstants.{h,cpp}`, `BufferContents.h`). Switch: `CS_ORG_ENGINE_CONSTANTS=1`.

## What the RE settles

- **Every engine constant buffer is one of 146 globals in one block**, made by
  `BSGraphics::Renderer::CreateConstantBufferPools` (SE 75592 `0x140d720d0`, AE 77400 `0x140e4d000`). The layout is the same
  in SE and AE, so one table classifies a buffer on both (below).
- **The PS pools are technique, material and geometry, in that order.** `BSGraphics::Renderer::SavePixelShader` (SE 75582
  `0x140d70e60`, renamed) writes a pixel shader's pool index per level from `0x143027c00` (level 0, 20 classes),
  `0x143027ca0` (level 1, 20) and `0x143027d40` (level 2, 40). That was R3's open question 5.
- **Every write goes through the immediate context's `Map`/`Unmap`** (always WRITE_DISCARD; no `UpdateSubresource` on an
  engine buffer). The Setup* functions, the custom mapper, the compute writer, the alpha test and the per-frame writer all
  map the global, write, unmap, then bind with `VS/PS/CSSetConstantBuffers` right after (or later, for b12 and PS b11).
- **Every engine draw calls `SetDirtyStates(false)` immediately before its draw** (R1), with all of its constants bound and
  the alpha-test buffer just written inside `SetDirtyStates` itself. The one dispatch site, `DispatchCSShader`, binds CS b0
  after its own `SetDirtyStates(true)`. Those two points are where a use is complete.
- **The custom mapper ignores its size argument**, and its callers pass different units (below). Its type argument is not
  set by the ImageSpace callers at all (a register left over); only type 7 changes what it maps.

## Write paths

### The globals (`CreateConstantBufferPools`)

One block of 146 pointers: SE 524753 `0x143027a08`, AE 411369 `0x143288308`. Index 0 of each size-class pool is never made
(size class zero).

| Index   | Offset | SE address     | AE address     | Group             | Size                  | Written by                                   | Bound at                    |
| ------- | ------ | -------------- | -------------- | ----------------- | --------------------- | -------------------------------------------- | --------------------------- |
| 0-3     | +0x000 | `0x143027a08`  | `0x143288308`  | Custom            | 0xf00 each            | `MapCustomConstantBuffer` (its callers)      | the caller's register       |
| 4       | +0x020 | `0x143027a28`  | `0x143288328`  | AlphaTest         | 0x10                  | `SetDirtyStates` (dirty bits 0x100/0x200)    | PS b11, `SetPerFrameBuffers` |
| 5-24    | +0x028 | `0x143027a30`  | `0x143288330`  | VS technique      | 0x10 x index          | Setup* (SetupTechnique)                      | VS b0, by the Setup*        |
| 25-34   | +0x0c8 | `0x143027ad0`  | `0x1432883d0`  | VS material       | 0x10 x index          | Setup* (SetupMaterial)                       | VS b1                       |
| 35-62   | +0x118 | `0x143027b20`  | `0x143288420`  | VS geometry       | 0x10 x index          | Setup* (SetupGeometry)                       | VS b2                       |
| 63-82   | +0x1f8 | `0x143027c00`  | `0x143288500`  | PS technique      | 0x10 x index          | Setup*                                       | PS b0                       |
| 83-102  | +0x298 | `0x143027ca0`  | `0x1432885a0`  | PS material       | 0x10 x index          | Setup*                                       | PS b1                       |
| 103-142 | +0x338 | `0x143027d40`  | `0x143288640`  | PS geometry       | 0x10 x index          | Setup*                                       | PS b2                       |
| 143     | +0x478 | `0x143027e80`  | `0x143288780`  | Compute           | 0x240                 | `SetComputeShaderConstants`                  | CS b0, `DispatchCSShader`   |
| 144     | +0x480 | `0x143027e88`  | `0x143288788`  | PerFrame          | 0x2d0                 | 75472 / 77258 (CS calls it UpdateCameraData) | VS and PS b12               |
| 145     | +0x488 | `0x143027e90`  | `0x143288790`  | InstanceGroup     | 0x10                  | `MapCustomConstantBuffer` type 7             | VS b7 (grass dispatcher)    |

The custom ring's index is SE 524752 `0x143027a00` / AE 411367 `0x143288300`. A shader's
`constantBuffers[level].buffer` (VS +0x18/+0x28/+0x38, PS +0x10/+0x20/+0x30) points at a pool entry, and the mapped pointer
is kept beside it (VS +0x40, PS +0x38 for level 2), which is how the Setup* functions write through `constantTable` offsets.

### The functions

| Function                                                                 | SE ID / address      | AE ID / address      | What it does                                                                                                                                                                                                                         |
| ------------------------------------------------------------------------ | -------------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `BSGraphics::Renderer::MapCustomConstantBuffer` (was `GetID3D11Resource`) | 75561 `0x140d6ffd0`  | 77362 `0x140e4acc0`  | `ID3D11Buffer** (Renderer*, uint size, void** outData, int type)`: type 7 maps the instance-group buffer, any other the next of the 4 custom buffers (`index = (index + 1) & 3`). Map WRITE_DISCARD; returns the global slot it used (SE 524949 `0x14302ac58` / AE 411437 `0x14328afe8`). |
| `BSGraphics::Renderer::SetComputeShaderConstants`                        | 75562 `0x140d70050`  | 77363 `0x140e4ad40`  | Maps the compute buffer, copies 0x240 bytes from its argument, unmaps.                                                                                                                                                               |
| `BSGraphics::Renderer::DispatchCSShader`                                 | 75532 `0x140d6ef90`  | 77329 `0x140e49cb0`  | `SetDirtyStates(true)`, `CSSetShader`, `CSSetConstantBuffers(0, 1, &compute)`, `Dispatch`.                                                                                                                                         |
| `BSGraphics::SetDirtyStates`                                             | 75580 `0x140d705b0`  | 77386 `0x140e4b5b0`  | The draw-time flush (R2). Writes the alpha-test buffer on dirty bits 0x100/0x200.                                                                                                                                                    |
| `BSGraphics::Renderer::SetPerFrameBuffers`                               | 75570 `0x140d70290`  | 77371 `0x140e4af80`  | Binds PS b11 (alpha test) and VS/PS b12 (per frame).                                                                                                                                                                                  |
| Per-frame writer ("UpdateCameraData")                                    | 75472 `0x140d6b210`  | 77258 `0x140e45680`  | Maps and fills b12 (0x2d0).                                                                                                                                                                                                          |
| `Renderer::Begin`                                                        | 75460 `0x140d6a0c0`  | 77245 `0x140e44590`  | `ClearState`, then binds b12 to VS and PS.                                                                                                                                                                                            |
| `BSGraphics::Renderer::CreateConstantBufferPools`                        | 75592 `0x140d720d0`  | 77400 `0x140e4d000`  | Creates the block above.                                                                                                                                                                                                             |

The custom mapper's callers (SE; 38 call sites):

| Caller                                                                 | Size argument                      | Bound to         |
| ---------------------------------------------------------------------- | ---------------------------------- | ---------------- |
| 37 ImageSpace effect setups (`0x1413309e0` to `0x141332970`, e.g. ISCompositeLensFlare `0x141331130`) | floats (0x44 for 0x110 bytes, 0xc for 0x30) | VS b2 and PS b2 |
| Grass per-instance data `0x1412ceb90`                                   | float4s (`(floats + 3) >> 2`)       | VS b8            |
| Effect SetupGeometry `0x1412ff500`                                     | 0x80                               | VS b11           |
| Skinning `NiBoneMatrixSetterI` `0x14131f630`, twice                    | float4s (`bones * 3`)              | VS b10, then b9  |
| The geometry dispatcher, type 10 (grass) `0x141307160`                 | the instance-group index, type 7   | VS b7            |

Since a float count is never smaller than the float4 count of the same data, `size * 16` bytes (at most 0xf00) always covers
what a caller writes. The capture copies that much of a custom buffer.

The Setup* map sites per shader type are R3's table (Lighting, Effect, Utility, Water: 6 each; Sky 2, Grass 1, DistantTree
3, Particle 3, BloodSplatter 2, ImageSpace base 2, WaterFlow 1). Sky's SetupGeometry (`0x1412fabb0`) was decompiled again for
this task: it maps VS and PS level 2, writes through the constant tables, unmaps, and binds VS b2 and PS b2. The capture does
not need a per-function hook for them: they all reach the context.

**Writes after Unmap.** `BSLightingShader::SetupTechnique` writes `VPOSOffset` through the stored pointer after unmapping
PerTechnique (DCLF's notes). DXVK keeps a dynamic buffer's mapping valid until its next Map, and the GPU reads that memory,
so the capture reads the last mapping at the draw, not at the Unmap.

Renamed in Ghidra: SE `MapCustomConstantBuffer`, `SetComputeShaderConstants`, `DispatchCSShader`, `SavePixelShader`; AE
`MapCustomConstantBuffer`, `SetComputeShaderConstants`, `DispatchCSShader`, `SetDirtyStates`, `SetPerFrameBuffers`.

## The per-use model

A **use** is one engine draw or dispatch. Its constants are whatever is bound to its stages' slots at that moment, with each
buffer's contents as the GPU will read them.

- **Writes** (`BufferContents`, shared with DCLF's `ConstantMirror`): each constant buffer's last mapping pointer, or a copy of
  `UpdateSubresource`'s data, and a version bumped by every Map, Unmap and update. Map, Unmap and UpdateSubresource reach it
  from the existing hooks in `Globals.cpp`.
- **Binds:** `VS/PS/CSSetConstantBuffers` and their `*1` forms (CS's windowed binds, GrassOptimizations) and `ClearState`
  are detoured on DXVK's immediate context. Each slot remembers its buffer and window.
- **Capture:** after `SetDirtyStates(false)` (and CS's own per-draw work in the same hook) for VS and PS, and after
  `DispatchCSShader` for CS. Each bound slot's window is copied into the frame's ring unless the same buffer version and
  window was already copied this frame, in which case the use points at the earlier copy. A buffer bound for 500 draws and
  written once is copied once.
- **Ring:** 1 MiB `org::LatchBlock` chunks (one slot each, host-visible, read in shaders as a ByteAddressBuffer). A slot's
  copy starts on 16 bytes. A frame's chunks retire after the Present through `RenderGraphRuntime::ReleaseAfterD3D11` and
  come back to a free list once the GPU is past the D3D11 work issued until then. That work includes every anchor epoch of
  the frame (they run in DXVK's stream before the Present), so a chunk is never rewritten while a pass of its frame can read
  it. Steady state allocates nothing: the chunks, the buffer index, the use and slot vectors are reused.
- **Ids:** a `UseId` is (capture frame, index). A `SlotConstants` is (stage, slot, group, pool index, chunk, offset, size).
  Records of past frames are gone: a lookup with an old id returns nothing.
- **Classification:** a buffer's group (Custom, AlphaTest, VS/PS technique, material, geometry, Compute, PerFrame,
  InstanceGroup, or Other for CS's and other plugins' buffers) and its pool index come from its place in the block.
- **Threads:** the hooks run on whichever thread drives the immediate context (serialized by `Renderer::Lock`). Uses are
  captured, and the frame ends, only on the main thread (`FrameAnchors::OnMainThread`), where the anchors run. A loading
  screen's job-thread frames are not captured (P5.12 decides what they need).

## The consumer API

`#include "RenderGraph/Engine/Constants/EngineConstants.h"`, namespace `RenderGraph::Engine::Constants`. Main thread.

| Call                                   | Use                                                                                                                                  |
| -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `Active()`                             | The capture runs: switch on, render graph active, no ring failure.                                                                   |
| `Current()`                            | The last draw or dispatch captured this frame. Call it right after the engine's draw call (or inside a hook that runs after `SetDirtyStates`). |
| `Capture(stageMask)`                   | Captures what is bound now as a new use: for a consumer that withholds the engine's draw at its own choke point, once the Setup* calls have bound their buffers. `kDrawStages` or `StageBit(Stage::Compute)`. |
| `Slots(use)`, `Find(use, stage, slot)` | The use's slots; one slot's constants, or null when nothing was bound there.                                                          |
| `Where(slot)`                          | `{ block, srvIndex, offset, size }`: the chunk (hold the `shared_ptr` while recording) and where the bytes are.                      |
| `Bytes(slot)`                          | The bytes on the CPU. The ring is write-combined memory: reading it is slow, so this is for diagnostics, not per-draw use.          |
| `GetStats()`                           | Totals since the start.                                                                                                              |

**ImageSpace (P5.6).** An ImageSpace effect's b2 is an ordinary slot of its draw: `Find(Current(), Stage::Pixel, 2)` right
after the effect drew gives its constants (group `Custom`, the same bytes the VS b2 slot points at). An ORG ImageSpace pass
that withholds the D3D11 draw calls `Capture(kDrawStages)` after the effect's setup function returned (its custom buffer is
then written, unmapped and bound to VS and PS b2) and keeps that id.

**Kept recordings.** A pass whose recording is reused must not bake `srvIndex` or `offset`: its commit writes them into its
own LatchBlock at the frame slot (`latch->WriteValue(host->CurrentFrameSlot(), ...)`), and the shader loads the constants
with `ByteAddressBuffer` from `ResourceDescriptorHeap[srvIndex]` at `offset`. A recording made for one frame may use the
location directly and retain the block (`RecordingContext::Retain`).

```cpp
namespace C = RenderGraph::Engine::Constants;
// In a hook that runs after the engine's sky draw (DCLF-style latch at the draw's choke point).
if (const C::UseId use = C::Current(); use.Valid()) {
	if (const auto* geometry = C::Find(use, C::Stage::Vertex, 2)) {
		const C::Location at = C::Where(*geometry);
		latched.push_back({ at.srvIndex, at.offset, at.size });  // written into the pass's latch at commit
		keepAlive.push_back(at.block);                            // until the frame's recording is made
	}
}
```

DCLF's `ConstantMirror` keeps its API (`Watch`, `Contents`): p5-sky and DCLF's epochs read through it as before. Its write
tracking is now `BufferContents`.

## Switches and log lines

| Switch                                  | Effect                                                                                                         |
| --------------------------------------- | -------------------------------------------------------------------------------------------------------------- |
| `CS_ORG_ENGINE_CONSTANTS=1`             | Installs the hooks and captures every use. Off (default): nothing is hooked beyond the existing Map/Unmap seams, which return at once. |
| `CS_ORG_ENGINE_CONSTANTS_LOG_FRAMES=N`  | The summary line's window (default 600 frames).                                                                 |
| `CS_ORG_ENGINE_CONSTANTS_CHECK=1`       | About once a second, one captured slot is compared with a staging read-back of its D3D11 buffer (`CopyResource`, `Map(READ)`: stalls). |

Log lines (all `[ORG] engine constants`):

- `capturing every engine constant-buffer use (CS_ORG_ENGINE_CONSTANTS=1)` at startup.
- `<N> frames: per frame <u> uses, <s> slots, <c> copies, <k> KiB (peak <p> KiB, <n> chunks); copies per frame by group:
  ...; unwritten slots <w>, dropped uses <d>[; checked <x>, <m> differ]` every window.
- `the ring has <n> chunks (<m> MiB)` when the ring grows past 4 chunks (at powers of two).
- Errors: `a ring chunk could not be made (...); the capture stops`, `<n> constant buffers seen; later ones are not
  captured`, and with the check `check: <VS|PS|CS> b<n> (<group>, pool <i>) differs from the D3D11 buffer`.

## Test plan

Scenario `tools/cs-ab/scenarios/constants-org1.env`: the `dclf-publish2-0` tour with `CS_ORG_ENGINE_CONSTANTS=1`, the
tripwire guest and every P4 switch, the summary every 300 frames. `constants-org1-check.env` adds the check.

1. **Build and start:** the startup line; no `[E]` lines beyond the known Effects11 one; no `Render graph disabled`.
2. **It captures:** in Whiterun and Dragonsreach the summary shows uses and copies above zero, the groups `VS geometry`,
   `PS geometry`, `per-frame`, `custom` (ImageSpace, bones) and `compute` (ImageSpace compute effects), and `unwritten
   slots` near zero after the first window (a slot bound before its first write since the hooks went in shows there once).
3. **It is cheap:** copies and KiB per frame stay bounded (expect a few thousand copies and a few MiB per frame in
   Whiterun with DCLF; `dropped uses 0`), the chunk count stops growing after the first windows, and the frame rate matches
   `dclf-publish2-0` with the P4 switches (GPU-bound comparison, one sample each, Dev-Fast is /Od).
4. **It is right:** `constants-org1-check.env` ends every window with `0 differ`. A mismatch names the stage, slot and group.
5. **It changes nothing:** shots match `dclf-publish2-0`'s.
6. **Loading screens and menus:** a `coc` (loading screen on the job thread) and opening the inventory do not stop the
   summary lines or log errors. AE 1.6.1170 as well as SE 1.5.97.

## Risks and limits

- **The capture point is after CS's own per-draw work** in the `SetDirtyStates` hook (`State::Draw`, `VanillaBinding`), so a
  use also records CS's buffers (permutation, shared data) bound at that draw, as group Other. A consumer that wants only the
  engine's slots filters by group.
- **Uses that never draw.** DCLF and GrassOptimizations call `SetDirtyStates(false)` themselves; those calls are uses too.
  They cost a few slot records and no copies when nothing was written since.
- **An anchor epoch on an async queue** is not ordered by `ReleaseAfterD3D11`'s event query. Consumers read the ring on the
  graphics queue (the anchors in DXVK's stream) until that is checked.
- **Loading-screen frames** (the job thread) are not captured, and a long loading screen does not end capture frames: no
  chunk is in use then, so nothing grows.
- **Stale identities:** a released buffer's address can come back as another resource. The type and description are read
  again at every write and first bind, so a reused address is classified again, but an engine global's group tag is kept
  from its first sighting (the block is filled once at init and never released before shutdown).
- **The ring's memory** is host-visible, device-local with resizable BAR. Without it the GPU reads constants over PCIe,
  which is what the engine's DYNAMIC buffers do today on DXVK.
- **Not done here:** the dual write ends at P5.13, when `SetDirtyStates`, `SetShader`, `SetPixelShader` and
  `DispatchCSShader` become latch-only: the engine's Setup* maps then need a target that is not a D3D11 buffer (the
  capture's ring is the natural one: map into the ring and skip D3D11).
