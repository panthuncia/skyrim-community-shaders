# Render thread: fewer tickets and cheaper uploads (plan items 1 and 3)

Lead `tickets`, 2026-11-25. Plan: `render-thread-window.md`, items 1 and 3.

## Commits

- ORG `perf/tickets` cb9f89a, "perf: cheaper ticket uploads, joint executions" (on 522269f).
- CS `perf/tickets`: b1482f212 bumps ORG, 26f5fa488 "perf(org): emit joined stages with their epoch".

## What changed

- **Uploads (item 3).** A ticket without uploads no longer begins, records or closes upload lists. External epochs' lists
  were begun on the host's thread for every ticket and closed on the render thread, but only 2 of 13.5 tickets a frame
  record anything. Lists are now begun at submission, only for a ticket with uploads. Upload planning is skipped when
  every batch of the ticket is on the graphics queue: nothing can be routed and no wait can be spared.
- **Joined stages (item 1).** A CS stage that commits inside a DCLF epoch's `BeforeGpuWork` is submitted at its commit,
  as before. Its execution stays in the host, and the DCLF epoch's ticket appends its own segments to it
  (`GraphHost::TakeExternalExecution`). The epoch emits both as one execution, with one compute exit and one flush of the
  pending submits. Five pairs join: start + shadow view, pre-depth + Z-prepass, pre-main + main opaque,
  deferred+water + water mask, water + water surface. Post-water does not commit inside the forward epoch in this scene,
  so it stays alone. The upscaler's late-stage separator runs after the joint emission (`RenderGraphRuntime::AfterEmission`).
- **Frame rebindings.** The duplicate check uses a flag per resource instead of a search per rebinding
  (`SynchronousAdmission::PrepareFrame`, not the ledger).

## Measured (Dev-Fast, guest, frozen river scene, same session, back-to-back captures)

| | before (base4) | after (after2) |
|---|---|---|
| Window | 6.91 ms | 6.76 ms |
| ORG epochs bucket | 3.77 ms | 3.49 ms |
| Engine self (noise check) | 1.33 ms | 1.39 ms |
| Tickets a frame (FinishTicket) | 13.5 | 13.5 |
| Emissions a frame (EmitExecution) | 13.5 | 8.5 |
| Upload lists planned/recorded/closed a frame | 13.5 | 2 |
| Upload cost (Plan + Record + Begin + Close) | 0.45 ms | 0.19 ms |
| ApplyFrameRebindings | 0.135 ms | 0.115 ms |

The ORG bucket fell 0.29 ms while the engine (unchanged code) read 0.06 ms slower, so the real cut is about 0.3 ms.

## Why tickets stay at 13.5

A ticket's fixed cost is mostly per graph: the admission and commit against the ledgers (about 107 us of the 250 us),
the handoff, the external execution's segments and the rebindings. Two graphs need two admissions and two commits even
inside one joint ticket, so a joint ticket over two graphs saves only the host's and CS's wrapper (about 15-30 us a
pair). One admission per pair needs one graph per pair, which goes against "DCLF's graphs stay their own graphs" and
was tried and dropped as phase epochs. The real per-ticket wins left are item 2's (hazards).

A first version held the stage's commit and finished both tickets together after the DCLF commit. It measured the
same, but moved the stage's admission, skip answers and inline recording after the DCLF epoch's commit, which can read
state that commit changes. The final version keeps every ticket's submission at its own commit and joins only the
emission.

## Test runs

- guest+quick: p-tickets-joint2, joint3b, nojoin, m1c00, m3c00, m7c00, mall (bisect builds), p-tickets-guest-final: passed.
- native-vulkan+quick p-tickets-nvk1: passed, shot good. native-d3d12-quick p-tickets-nd12a: passed, shot good.
- Captures: p-tickets-base4, p-tickets-after2 (Tracy, scratchpad `tickets/perf`).

A blurry, shiny chest plate on the player showed in joint2/joint3b and sent me bisecting. Other leads' unchanged builds
(p-post-native-obs2, p-hazards-t-guest-quick) show the same at the same time, so it is run-to-run variance (texture
residency), not this change.

## Decided by the agent, to confirm

- Join only the emission, not the submission (see above): safer, same measured cost.
- External upload lists are begun on the render thread for the two tickets a frame that have uploads (about 20 us each),
  instead of on the host's thread for every ticket.
- Item 3 as "upload lists only where there are uploads", not "one list a frame": a later ticket's uploads are queued by
  its own commit, after the earlier tickets are submitted, so they cannot go into an earlier list without the earlier
  ticket's passes seeing later data.

## Open items

- The ledger interface is unchanged (no calls to `BackingAccessAdmissionLedger` changed).
- `PlanTicketUploads` still walks every boundary access for the two tickets with uploads (19 us each); a per-backing-set
  cache of destination uses would cut it.
- worktree builds from `build-wt.cmd` have Tracy off (`TRACY_SUPPORT=OFF`); captures need `-DTRACY_SUPPORT=ON`.
