# Engine occlusion queries on the graph (Phase 5.5.5)

`CS_ORG_OCCLUSION_QUERIES=1` (default off for now) moves the engine's sun occlusion queries to the render graph. The sun's
SunOcclude draw is drawn by an ORG pass, an ORG occlusion query counts it, and the engine's `GetQueryData` answers from that
count. Neither the draw nor its `BeginQuery`, `EndQuery` and `GetData` reach D3D11. The flush reason `sky: sun occlusion query`
and its `EngineDraws` scope are deleted.

It needs the sky family (`CS_ORG_SKY=1`, `CS_ORG_ENGINE_CONSTANTS=1`). Without the sky family's sun client the queries stay on
D3D11, as with the switch off.

## The engine's queries (RE, AE 1.6.1170 and SE 1.5.97)

| What | SE ID / address | AE ID / address | Notes |
| --- | --- | --- | --- |
| Occlusion ring | 524740 / 0x143025f60 | 411344 / 0x143286800 | 64 `ID3D11Query*`, made by `Renderer::Init` |
| `AcquireOcclusionQuery` | 75565 / 0x140d70150 | 77366 / 0x140e4ae40 | returns `&ring[next]`, `next = (next + 1) & 0x3f`, counts users up |
| `ReleaseOcclusionQuery` | 75566 / 0x140d70180 | 77367 / 0x140e4ae70 | counts users down; the ring entry is not freed |
| `BeginQuery(renderer, entry)` | 75567 / 0x140d70190 | 77368 / 0x140e4ae80 | `context->Begin(*entry)` |
| `EndQuery(renderer, entry)` | 75568 / 0x140d701b0 | 77369 / 0x140e4aea0 | `context->End(*entry)` |
| `GetQueryData(renderer, entry, wait, flush)` | 75569 / 0x140d701d0 | 77370 / 0x140e4aec0 | reads 8 bytes, returns the low 32 bits. `wait` 0: 0xFFFFFFFF when not ready. `wait` 1: spins with `Sleep(0)` (and always flushes) |
| Sun's query draw `(accumulator, geometry, index, camera)` | 99958 / 0x1412ccad0 | 106604 / 0x1414b4c00 | below |
| `Sun::SubmitOcclusionQuery` | 25799 / 0x1403bbf20 | 26353 / 0x1404155e0 | calls the draw at +0xAD / +0xAF |
| `Sun::UpdateOcclusionQueries` | 99967 / 0x1412cd140 | 106605 / 0x1414b4dc0 | polls (`wait` 0, `flush` 1) the three queries |
| One query's poll | | 106614 / 0x1414b5290 | the same for one index; no direct callers |
| Decal occlusion test | 35563 / 0x1405b29f0 | 36562 / 0x1406458a0 | `BeginQuery`, `SubmitAccumulator(camera, accumulator, 0)` into depth 5 (kDECAL_OCCLUSION), `EndQuery` |
| Decal test's read | 29262 / 0x140450260 | 30117 / 0x1404ac6d0 | polls (`wait` 0, `flush` 0), then releases the entry |

The sun's draw (AE 106604): when the accumulator has no result pending (+0x64 clear) and the index is below 3 with its
pending flag (+0x84 + index * 0x18) clear, it takes the index's query (+0x70 + index * 0x18, acquired once and kept),
`BeginQuery`, `SetCameraData`, makes a BSSkyShader pass of the sun's geometry and draws it through `RenderPassImmediately`,
`EndQuery`, and sets the pending flag. `UpdateOcclusionQueries` then polls each pending query. For a count `n` it stores
`1 - min(n, 1500) * k` at +0x78, the count at +0x7C, and clears the pending flag. When all three answered it keeps the largest
value (1.0 when the smallest count is below 10) at +0x68 for the glare. A query that is not ready keeps its pending flag, and
the next submission waits for it.

The engine calls the sun's draw twice a frame: from `Main::RenderWorld` (index 1, after the world's accumulator, depth kMAIN)
and from AE 107129 (index 2, `_gUnkBSShaderAccumulator`, target kMAIN with the depth target that function set, the Z-prepass
copy). The census saw both: "into kMAIN with DS kMAIN and with kPOST_ZPREPASS_COPY". No caller uses the blocking form.

## How it works

**The query mirror** (`src/RenderGraph/Engine/Queries/OcclusionQueries.{h,cpp}`). The switch detours `BeginQuery`, `EndQuery`
and `GetQueryData`. A port opens an `OcclusionQueries::Bracket` (a render-thread scope) around the engine call that begins, draws
and ends a query. The first `BeginQuery` inside a bracket takes its ring slot for the graph: that call and its `EndQuery` go
nowhere. A query begun outside any bracket is D3D11's, as before.

**The ORG queries.** A `QueryRing` holds `PerRun()` occlusion queries per frame slot and a readback buffer of their counts. The
client's pass resets its frame slot's range, counts each draw with one query, and resolves the range into the readback:

- `RasterProgram::DrawDesc::occlusionPool` / `occlusionIndex`: `Draw` begins the query after `BeginPass` and ends it before
  `EndPass`, so it counts that draw alone and stays inside one render pass (Vulkan requires a query begun in a render pass to
  end in it). `Draw` now returns whether it recorded the draw.
- `QueryRing::Resolve` begins and ends every query of the range no draw began, with nothing inside, so each query is available
  before `vkCmdCopyQueryPoolResults` with `WAIT_BIT` (a reset query that is never begun would hang the GPU there).
- BasicRHI (`org-port/p55-sun-query`, 1a587d8): on Vulkan an occlusion pool's `ResetQueries` is now `vkCmdResetQueryPool`, not
  a host reset. It runs again on every submission of a kept recording. The host reset ran once, at record time, so a replayed
  recording began queries that were never reset. Occlusion queries are begun with `VK_QUERY_CONTROL_PRECISE_BIT` when the device
  enabled `occlusionQueryPrecise` (DXVK enables it for D3D11). D3D12's `OCCLUSION` queries are exact already and need no reset.

**Answering the engine.** The client's commit names the ring slot its run counts (`QueryRing::Commit`), and its executed
callback marks the run submitted with the host's `SubmittedPoint` and `Seals` (`QueryRing::Executed`). `GetQueryData` on a graph
slot:

- answers the count once it is read,
- reads it when the run's GPU point is reached (polling with no wait),
- returns 0xFFFFFFFF (not ready) until then, as D3D11 does,
- in the blocking form, waits for the run's point, but only once the host sealed it (`Seals()` past the run's). Before that the
  work may still sit in DXVK's stream behind the current frame, and the wait would never end. That case answers not-ready and
  is logged once. No engine caller uses the blocking form.

A frame slot's counts are also read at that slot's next commit, since the host has waited for its last work then. A slot whose
draw the graph did not draw (skipped, below) answers its last count at once.

**The sun's draw** (`Features/DrawcallLimitFix/Features/Sky`). The sky family detours the sun's draw (SE 99958 / AE 106604)
only with the switch on. Inside the bracket the engine's code runs unchanged; its SunOcclude pass reaches the families'
DrawTriShape taker, and the sky's capture latches it as it latches a batch's draws (`TryLatchDraw`: the shadow state, the
engine constants, the geometry). The program is `Sky.hlsl` built for ORG with `SKY_ORG_FACES` (one target), keyed by kMAIN's
format and the depth target's. A third client, `cs.sky.sun` at the new `EngineQueries` anchor, has two passes:
`cs.sky.sun.occlusion` (depth kMAIN, the Z-prepass copy at t17) and `cs.sky.sun.occlusion-copy` (depth kPOST_ZPREPASS_COPY,
a null view at t17, as D3D11 unbinds a view of the bound depth target). The run's depth picks the pass that records. The
headers and Cloud Shadows are null views: SunOcclude samples none of them. The draw goes through the client's run gate.

**Skips.** With the switch on and the sun's client made, nothing of the sun's draw reaches D3D11. A draw the graph cannot take
is skipped and its query answers its last count, logged once per reason (`[DCLF] sky: a sun occlusion draw is skipped, its
query answers its last count: ...`). Reasons: a view other than kMAIN with kMAIN's depth or the Z-prepass copy, the graph not
able to draw now (not the render owner, the anchor not in the built graph or its ring full), a draw `TryLatchDraw` refuses
(its own reasons), a pass that draws through another function than DrawTriShape (that one still reaches D3D11, logged), more
or fewer than one draw between the query's begin and end, and a run that was not submitted. A sun query inside a sky batch's
capture (never seen) leaves the engine's call out entirely.

## What stays the engine's

- **The decal occlusion test** (SE 35563 / AE 36562). It brackets a whole accumulator render (`SubmitAccumulator` of the decal's
  geometry, render mode 0x1B, into kDECAL_OCCLUSION). Its draws are not a family's, so it opens no bracket: its queries stay on
  D3D11 with their draws. It runs only while decals wait to be placed (BGSDecalManager), so the census tours see it rarely. Taking
  it needs those draws on the graph first (a DCLF or family item); the bracket and the query ring are ready for it.
- With the switch off, or without the sky's sun client (the sky family off, failed, or refused at setup), the sun's draw is the
  engine's on D3D11. There is no vouch for it any more: under the flush latch that draw is skipped as unvouched, and its D3D11
  query counts nothing (no glare).

## Graph rebuilds

The sun's client registers once at CS's setup, with the sky's clients, and joins at the setup's build. Nothing in play registers
or imports again. The run's depth choice changes the recording (its revision), not the graph.

## Switches and log lines

- `CS_ORG_OCCLUSION_QUERIES=1`: the detours, the `EngineQueries` anchor and the sun's client. Logs `[ORG] occlusion queries: on`,
  `[ORG] occlusion queries: cs.sky.sun.queries: 1 queries a run over N frame slots`, `[DCLF] sky: the cs.sky.sun client
  registered at setup`, `[DCLF] sky: the first cs.sky.sun run drawn by the graph`.
- `CS_ORG_OCCLUSION_QUERY_LOG=1` (alone): only the `GetQueryData` detour, which forwards to D3D11 and logs its answers. It is
  the reference for a comparison.
- Both modes log the first 12 answers (`[ORG] occlusion queries: ring slot N read M samples (the graph's count)` or `(D3D11's
  count)`), then a summary per ring slot every 100 answers (`... N reads, samples last, min, max, mean; K not-ready polls`).
- The sky's status line ends with the mirror's counters: answers from the graph and from D3D11, not-ready polls, skips, waits.

## Testing

`tools/cs-ab/scenarios/sun-query-org1.env` (the latch census switches with the queries on) against `sun-query-org0.env` (the
queries off, the flush latch off so D3D11 draws and counts the sun, `CS_ORG_OCCLUSION_QUERY_LOG=1`). Whiterun at 16:00 in clear
weather: shots facing the sun (`sun-240`, `sun-200`, `sun-280`), away from it (`sun-away`) and up (`sun-high`), then
Dragonsreach. Look for:

- the same glare in the shots facing the sun, and none facing away;
- the per-slot summaries: counts of the same order in both runs at the same camera (exact equality is not expected: the
  frames differ), zero or near zero facing away;
- org1's census without the sun's draw (no entry "through AE 106604"), and no `[E] [ORG] flush latch: engine ...` line for it;
- no `a sun occlusion draw is skipped` line in a world frame, no device loss, no `Render graph disabled`.

Not verified without a build and a run: everything here. Risks to look at first: the readback's layout against the resolve's
offsets, the Vulkan query begun inside RasterProgram's pass with a secondary list, the second call's depth target (assumed the
Z-prepass copy from the census), and the sun program's first build at the first sun draw (no manifest entry on a first run).
