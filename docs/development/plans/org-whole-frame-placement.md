# ORG whole-frame placement (plan, 2026-11-22)

Goal (user, 2026-11-22): ORG works out resource lifetimes and placement by itself. CS only declares what each pass uses and
what must persist. No explicit frame transients, and no manual grouping of aliased resources.

Status (2026-11-22, evening): steps 1-6 implemented, step 7 not done; see `org-placement-relocation-log.md` for what is on, what is off and why. Earlier status: plan only. The frame transient work this replaces was reverted (`chore(org): revert frame transients`). The reasons
are below. Companion plan: `org-relocation.md`. A placement change is a relocation of transients, so the two share the
commit point and the follow path.

## 1. Why CS cannot rely on ORG's aliasing today

### Within one graph, ORG is already automatic

`RenderGraph::PlanPersistentAliasPlacement` (`RenderGraphPersistentStructural.cpp:598`) needs no grouping:

1. It ranks passes by schedule order.
2. It takes each candidate's first and last use, and whether the first use writes, from the passes' states and accesses.
3. It packs the alias pools with a sweep line or a beam search (`:793`).

The planner leaves some resources out:

- a resource whose first use is a read: likely a history (`:737`);
- versioned slots (`:657`);
- resources used on two backends;
- in Balanced mode, small resources the score rejects (`:740-746`).

`Options::aliasPool` is now only a diagnostic (`GraphResources.h:84-86`).

### The frame is many graphs, each planned alone

- ORG can compile one graph over the whole frame. In that mode "scheduling and transient alias placement see every
  lifetime" (`SetPersistentEpochOrder`, `RenderGraph.h:269-275`).
- GraphHost never uses that mode. It builds one graph per epoch (`GraphHost.cpp:1164`, `:1184`).
- The split is deliberate: a client change or a DCLF revision must not recompile the whole frame
  (`org-native-frame-graph.md:37-41`). So each planner sees only its own graph. A resource used by two graphs is invisible to
  both, and CS keeps such resources out of transients.

CS's extra layers all come from that split:

| Layer | Where |
|---|---|
| Frame positions | `Frame/Transients.cpp`, `GraphHost::SetFramePositions` |
| The frame alias arena, where CS stages reserve ranges by position | `FrameAliasArena.h`, `RenderGraph::SetFrameAliasRegion` |
| Private pools for some epochs | `SetPrivateAliasEpochs`. In a shared arena every admission found alias activations: 63 µs against 12 µs (`org-native-frame-graph.md:1666-1668`) |
| Frame transients, with spans learned lazily | `GraphHost::AddFrameTransients` |
| Declared transients | `Frame::DeclareTransient2D/3D` |
| Hand-kept lists | `kPreviousFrame` (`RecipeTable.h:418-433`), and the reverted `kFrameScoped` |

### What went wrong with frame transients (2026-11-22)

1. Each frame transient is placed before anything uses it, with span none, and only ever grows in place. All of them landed at
   one offset. The first graph that used two of them could not widen the second, and the run stopped.
2. Fixing that by keeping them apart put each one in its own block, exactly its size. Nothing else could share those bytes,
   so native usage rose by about 90 MB on Vulkan.
3. Shared 64 MB blocks let graph pools lie over frame transients. A binding change then needed a structural alias edit, and
   deferred+water stopped.
4. DCLF declares its whole binding table as read. So its depth epoch "reads" `kSHADOW_MASK` and `Depth::kPOST_ZPREPASS_COPY`
   before the frame writes them, and the read-order check stops native mode.

### Blockers, sorted

**Artifacts of how CS and ORG are put together. These can be removed:**

- Placement per graph instead of per frame. This is the root cause.
- Spans learned lazily, after placement.
- DCLF's over-declared reads.
- Versioned resources are never placement candidates. Every declared texture is versioned, which is why Scaleform's scratch
  needed a workaround.
- Previous-frame reads listed by hand (`kPreviousFrame`). In frame order, a read before the first write is a previous-frame
  read, and that can be inferred.

**Real constraints. Each needs an explicit marker or a rule:**

- **Users outside the graphs.**
  - Guest-mode exports are DXVK's images.
  - Frame generation's SDK reads `kMOTION_VECTOR` and depth `kMAIN`.
  - D3D11 readbacks (`kSAVE_GAME_SCREENSHOT`).
  - Native-context work (the clear of `kRAW_WATER`).

  ORG already has `HostOwned` and external home states for these.
- **Passes gated at run time.** A region graph holds every merged anchor's passes, and one may draw nothing in a given run.
  A declared first write is then not a guaranteed write. ORG cannot infer this.
- **Epochs that run several times a frame** (taps, copies, DCLF's epochs). A resource used within one run is a loop-local
  transient. One that crosses runs must span all of them.
- **Kept recordings.** They bind backings and descriptor indices (DCLF latches view indices,
  `FrameBindings.cpp:150-152`). So a placement change can only reach new recordings.
- **Admission cost.** Shared bytes between graphs cost alias activations at every admission. Unless the frame's placement is
  the same every frame, that cost returns.

## 2. Design

### 2.1 Usage summaries

Each epoch graph's structural build publishes a usage summary: one entry per placement candidate. It extends what
`ReportFrameTransientUses` (`RenderGraph.h:901`) reports today.

```
struct PlacementUse {
  ResourceKey key;            // stable across builds: the resource's identity, or the versioned slot plus extent class
  uint32_t firstRank, lastRank;  // pass ranks within the epoch's schedule
  bool firstUseWrites;        // false: a history candidate, kept out (or checked against earlier epochs)
  bool writeGuaranteed;       // false when the first writer is gated (see 2.5)
  uint64_t size, alignment; uint8_t memoryClass;
  bool loopCrossing;          // used in more than one run of a repeated epoch
};
```

- It is built from what `PlanPersistentAliasPlacement` already computes. The per-graph packing step moves to the host.
- Resources used by one graph only are in it too. The host decides whether they share bytes with other graphs.

### 2.2 Host packing (`GraphHost`, new `FramePlacement`)

- The host keeps, per attached build, the summaries of every epoch in `epochOrder`.
- A frame-wide timeline is the epochs in order with each epoch's ranks inside. A repeated epoch is one slot whose resources
  span the whole slot when `loopCrossing`.
- A resource's lifetime is the union of its uses across epochs. A first use that reads is a previous-frame read: either the
  resource is persistent, or this is a finding, the same rule as the planner's "first use is read" and the world recipe's
  read-order check.
- Packing reuses `alias::PackGreedySweepLine` and `PackBeamSearch` over the whole frame. The output is an offset per
  resource in a set of frame blocks, and replaces the arena's ranges and spans.
- Placement is deterministic. The same summaries give the same offsets, so steady frames see the same placement and no
  extra alias activations. The planned activations become part of each graph's predicted plan.

### 2.3 Versioned placements

A new frame placement is a new version of the whole set:

- it is computed off the hot path, when a build is staged or a summary changes;
- it applies at the next attach, through the revision adoption point;
- running and kept recordings keep their backings (`FreezeAliasedBindings`, `RenderGraph.h:303`);
- the old blocks are freed once frames in flight retire;
- resources whose offsets did not change keep their backings, so their recordings stay valid.

This follows the same commit point and follow path as `org-relocation.md` section 1. A transient that moves is a relocation
with no copy: transients hold nothing across frames. Its backing generation bump makes kept recordings stale, so they record
again.

### 2.4 Versioned resources as candidates

Two options. Option (a) is recommended:

- (a) Each version gets its own placement entry. A new version is a new key, packed at the next placement. A
  display-size change is then a placement change.
- (b) Size transients to the display size and treat dynamic resolution as a viewport, so resizes stop making new versions
  (`frame-revision-investigation.md:426-428`).

Then `DeclareTexture*` with `Lifetime::Transient` stops being fatal (`Declare.cpp:315-319`). `DeclareTransient*` folds into
it.

### 2.5 Opt-outs, the only manual part left

| Marker | Meaning | Exists? |
|---|---|---|
| `Lifetime::History` / `Persistent` | content kept across frames | yes |
| `External` / `HostOwned` | a user outside the graphs reads or writes it | yes, in ORG |
| `MayNotWrite`, on a pass's write | a gated writer. A resource whose first writer may not run is not a candidate, unless a guaranteed writer comes first | new |

Everything else is a candidate, chosen by the planner's existing score.

## 3. Steps

Each step stands alone and is tested in native Vulkan, native D3D12 and guest. Measure with:

- the VRAM JSON reconciliation (ORG resources, ORG alias pools, not tracked);
- the `vram-loads` and `vram-riverwood` configs;
- `river`, `ssgi`, `dynres`, `menu`, `screenshot-clean` and `resize` for correctness, with screenshot diffs against a
  baseline;
- admission timing in the Tracy zones `ORG.Persistent.*`, for the steady-state cost.

| # | Step | Measure | Risk |
|---|---|---|---|
| 1 | Narrow DCLF's declared reads to what each epoch's shaders sample. `kPreviousFrame` loses the entries this makes unnecessary | the world recipe reports no read before write for DCLF's epochs | missing a real read: an unbound resource, caught by validation |
| 2 | Usage summaries published by each build (2.1). Logged only, no behaviour change. A diagnostic prints the frame-wide lifetimes and what packing would save | the predicted MB against today | none |
| 3 | Host packing behind a switch, `ORG_FRAME_PLACEMENT`, off by default. Graph transients take host offsets instead of per-graph pools. Frame positions, the arena and private pools stay for the off path | VRAM against today; admission cost against the 12 µs baseline; correctness configs | alias activations every frame if placement is not stable |
| 4 | Versioned placements and their follow path (2.3), shared with relocation step 3 | resize and dynres runs; no stale bindings (D3D12 debug layer, Vulkan validation) | kept recordings missing a backing change |
| 5 | Versioned resources as candidates (2.4a). Engine targets that are now declared twins become candidates unless marked | engine targets leave "not tracked" and appear in host placement | gated writers (step 6 must land with this) |
| 6 | `MayNotWrite`. Mark the gated region passes (`org-native-frame-graph.md:1069-1090`) | the `UnwrittenTransient` finding stays quiet | an unmarked gated writer corrupting a shared resource |
| 7 | Switch on by default, then delete: `Frame/Transients.cpp`; `GraphHost::SetFramePositions`, `AddFrameTransients`, `SetPrivateAliasEpochs` and their CS calls; `RenderGraph::SetFrameAliasRegion` and the frame transient observer; `FrameAliasArena` (its block allocation moves into `FramePlacement`); `Frame::DeclareTransient*`; the frame transient predicate in `Recipe.cpp:143-149`; `kPreviousFrame` if step 2 shows the inference covers it | the same configs; less code | |

**Expected savings in native Riverwood:**

- The candidates the reverted work found, about 125 MB:
  - the G-buffer and other frame-scoped engine targets, about 91 MB;
  - Scaleform's scratch, 33 MB.
- SSGI's full-resolution scratch, about 46 MB.
- The shadow mask and depth copy, once DCLF's reads are narrowed.
- Better packing of what the arena already holds, since positions are only coarse lifetimes.

The packing must beat today's per-graph pools, so step 2's prediction decides whether to go on.

## 4. Open questions (recommendation in each)

1. **Should DCLF's repeated epochs take part, or keep private pools?** Take part, with loop-crossing resources spanning the
   slot. Keep private pools as the fallback while step 3 is behind its switch.
2. **Who computes the placement: the owner thread at staging, or a worker?** A worker, from immutable summaries, adopted at
   the next attach. It must never run on the hot path.
3. **Admission cost.** Accept the alias activations only if step 3 measures under about 20 µs for each submission. Otherwise
   bake the activations into each graph's predicted plan before switching on.
4. **Guest mode.** Exports stay DXVK's and are never candidates. Graph transients take part as in native.
5. **Small resources.** Keep the planner's score, so small resources keep memory of their own.

## 5. Handoff: state and how to work on this

Everything a new session needs. Read `.claude/CLAUDE.md` and `AGENTS.md` in the checkout first. Its rules apply:

- conventional commits, a title of 50 characters or fewer, the body wrapped at 72, no attribution lines;
- PRs target `dev`;
- comments in short plain sentences.

### Where things are (2026-11-22)

| | |
|---|---|
| CS checkout | `C:\Users\matth\source\repos\SARP\external_refs\cs-org-dxvk`, branch `org_integration`, head `0c3066070` plus this doc |
| Pushed? | No. About 20 local commits are ahead of `origin/org_integration` (this session's VRAM fixes). Ask the user before pushing |
| Second worktree | `C:\Users\matth\source\repos\SARP\external_refs\org-port-worktrees\sync17`, detached at `f909ab6ab`, with its own Dev-Fast build. Use it for agent work or baselines. Branch `vram/frame-transients` holds the reverted work |

Submodule pins. Each commit is on the named branch, local only:

| Submodule | Commit | Branch |
|---|---|---|
| OpenRenderGraph | `1496d3f` | `vram/frame-arena` (frame arena: unused frame transients placed apart) |
| ORGModuleServices | `5eb51fb` | `fix/program-compiles-here` (`RevisionAssembler::ReclaimRetired`) |
| BasicRHI | `b02dad1` | `d3d12-small-buffers` (numbered memory dumps) |

- ORG is the library to change for this plan. Commit in `extern/OpenRenderGraph` on a new branch, and bump the pointer in a
  separate CS commit.
- After a merge that moves ORGModuleServices, check its pinned commit out: it has `update = none`.

### The reverted attempt, to learn from

`git show 3db7bfa65` and `git log f909ab6ab..vram/frame-transients` hold:

- the frame-scoped engine target table, with exclusion notes for every target (`EngineTargetSet.cpp`, `kFrameScoped`);
- the declared `frameTransient` option;
- SSGI's full-resolution scratch as transients.

Its candidate list and exclusions are the input for steps 5 and 6.

### Build

- PowerShell, from the checkout: `.\BuildDevFast.bat` (Ninja, /Od, DLL only; incremental in minutes).
- In a worktree, `CMakeUserPresets.json` must be copied from the main checkout, as `sync17` already has.
- **Never link while a cs-test game is running.** The game holds the PDB, and the link fails with LNK1201.

### Test (cs-test, `tools/cs-test`, README there)

```
cd tools/cs-test
python cs_test.py stage ../.. --name nfg     # mirrors the build into the MO2 mod "CS AB nfg"
CS_TEST_BACKGROUND=1 DXVK_DEBUG=hang python cs_test.py run native-vulkan+vram-loads --wait 3600 --stage nfg --label <x> --set game.save=Save91_00000000_0_507269736F6E6572_Tamriel_000027_20261113033158_1_1
```

- **Modes:** `native-vulkan`, `native-d3d12`, `guest` (DXVK). Validate every change in all three.
- **Background runs are required:** the user works on the machine. `CS_TEST_BACKGROUND=1` keeps the game from taking focus
  or the mouse.
- **Frame rates from these runs are not trusted.** Dev-Fast is /Od and the runs are in the background.
- **Always pin the save.** Start On Save loads the newest save, and a stray save stops runs at the main menu. Move a stray
  save to `C:\Modding\CS-AB\saves-aside`.
- **Close MO2 and the game before a run**, with `taskkill`. The run refuses otherwise.
- **A killed run leaves cs-test's journal.** Run `python cs_test.py restore --journal-only` before the next run. Never kill a
  run loop without stopping every `cs_test.py` process it started.
- **Shader changes recompile on the first run**, which can time out. Rerun once; never raise the timeout.
- **New env switches** must be in `src/Testing/SwitchRegistry.cpp`. Then regenerate the schema:
  `python cs_test.py run list-switches --stage nfg`, then `python cs_test.py schema`.
- **Findings without a stop:** `--set env.CS_ORG_MISMATCH_FATAL=0` logs every world-recipe finding instead of stopping at the
  first.
- **Outputs:** a run's folder is `C:\Modding\CS-AB\runs\<MMdd-HHmmss>-nfg-<config>-<label>`, holding:
  - `CommunityShaders.log`;
  - `CommunityShaders-VRAM-*.json` (read the second of each pair; `reconciliation` holds the totals, `rows` each resource);
  - `shots/`.
- **Memory dumps:** `--set 'env.BASICRHI_MEMORY_DUMP=C:\path\mem{n}.csv'` writes every live allocation and resource every
  300 presents (Vulkan).
- **Screenshot diff:** `python tools/cs-test/shotdiff.py a.png b.png`.

### Baselines to compare against

`vram-loads`, at the second dump after Riverwood loads 1, 3 and 4. Driver device-local MB:

| Run | Load 1 | Load 3 | Load 4 |
|---|---|---|---|
| native Vulkan, `1122-011637-nfg-native-vulkan-vram-loads-hs1` | 3,808 | 3,636 | 3,693 |
| native D3D12, `1122-011834-nfg-native-d3d12-vram-loads-hs1` | 3,753 | 3,689 | 3,747 |
| guest, `1122-012847-nfg-guest-vram-loads-gc2` | 4,132 | 4,161 | 4,155 |

The `-ft2` runs of the same configs are the reverted frame transients: no gain on native, and about 90 MB more on Vulkan at
load 4.

### Known issues not caused by this work

- **`native-vulkan+screenshot-clean` loses the GPU device** after its `window 1280x720` step. The build before the frame
  transient merge (`f909ab6ab`) does the same.
- **One replaced `Skylighting::ProbeArray` (64 MB)** stays alive after a reload of the same cell. See `vram-riverwood.md`, "A1,
  third pass".
- **Read-order findings on DCLF's depth epoch** (step 1).

### Related documents

- `docs/development/plans/vram-riverwood.md`: VRAM attribution, the leak fixes, the candidates.
- `docs/development/plans/org-relocation.md`: relocation and defragmentation. It shares the commit point and follow path
  with section 2.3.
- `docs/development/org-native-frame-graph.md`: why the frame is split into region graphs; the admission cost of shared
  arenas.
- `extern/OpenRenderGraph/docs` (`persistent-epochs.md`): epochs, copies, taps.

### Working with agents (the user's standing rules)

- With two or more agents running, the main session runs every game test, and agents only build and report "ready for test".
- One agent alone may build and test itself.
- Each agent works in its own worktree and branch.
- Never resume an agent that has been idle for an hour or more, or whose context is more than half full: brief a new one.
