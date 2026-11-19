# Comment cleanup notes: rendergraph-top2

This slice continues `rendergraph-top`. It covers the files that notes file listed as not started. The original text of every
comment cut here is in the worktree's HEAD (`git show HEAD:<file>`).

## 1. Simplification or documentation targets

- `RunGate.h` `RunGate` and `FrameAnchors` closing gates. The class comment (18 lines) was the only place that explained the gate
  model. A pass records an indirect dispatch or draw, its counts sit in a latch block, and the anchors close every gate at the start
  of each run so only entries the commit opened do work. It also explained that a departed client's old gates keep being closed
  until no graph holds its passes, and that `CopyOpen`/`CopyOpenAll`/`ClearOpenAll` move each gate's record from a stage's staging
  run to the run it submits. The staging-run moves happen in three places (`RunGate::CopyOpenAll`, `RunSlotsRegistry::MoveAll` in
  `FrameStaging.h`, the latch's own `CopyStableRun`). Suggest one "stage commit moves run data" step that owns all three, and a
  doc section on run gates in `docs/development/org-native-frame-graph.md`.
- `FrameStaging.h` `RunSlots`. The comment explained that a pass without a revision is never current, so its ticket is prepared
  again after the commit for the same run, and that is why the value it Takes is found. That is an unexpected dependency between
  revision rules and when a value is readable. Suggest documenting it with the ticket preparation rules, or making RunSlots
  independent of that re-preparation.
- `Frame/FrameDriver.cpp` `FreezeTestAtBegin`. Main's frame runs the freeze-background test twice, once at Begin and once right
  after. Another thread can push a menu between the two through the UI message queue, so the description and the engine disagreed
  (described as a world frame, drew the capture). The hook forces the second test to return Begin's answer. This engine quirk is
  worth a line in the doc's "The declared frame" section.
- `Frame/FrameDriver.cpp` `OnBegin` build point. In native mode DCLF's build point runs at Renderer::Begin, in guest mode at
  Main::Draw, because in guest mode anchors' epochs may take a build mid-frame. Two build points for one module, chosen by mode.
  Removing guest mode's mid-frame build path would leave one.
- `GraphResources.cpp` `ExportTexture` (the `Kept` struct). Original: "Resting in Common (native mode). The import is exclusive and
  the graph moves the image between layouts. Its other users follow: the native context's work on the D3D11 wrap goes through ORG's
  external client (this resource is its ledger: MadeImageOf), and DCLF's views of it are made on this resource. The import does not
  own the image, so the graph texture keeps it: the import goes first, then the image." Ownership and ledger rules for an exported
  twin are spread over GraphResources, DxvkImageImport's made-image registry and ExternalWork. Same target as the previous agent's
  "one image-to-resource registry".
- `Frame/Transients.h` frame arena. The file header (25 lines) was the main explanation of the frame alias arena: spans as frame
  positions, per-stage transients vs. `Place`d frame transients whose span ORG derives from declaring graphs, and the hand-over rule
  that a resource whose bytes were reused enters from Undefined, so the first pass of its span must write it. The recipe check
  (`Frame/Recipe.cpp` `Validate` and the frame-transient block) enforces it. Documented in the doc sections the header cited;
  keep those in sync.
- `Frame/Recipe.cpp` `Validate`. The check's rules (reads with no writer are external, reads whose writers are all later must be in
  `kPreviousFrame`, a CS transient read with no writer in its own stage is a finding because absent passes may drop the writer)
  lived only in a nine-line comment. Worth a doc entry next to "The recipe".
- `PublishedResources.cpp` `Store` and the building snapshot (`t_building`, `InBuildingSnapshot`). A new id rebuilds only the
  graphs that looked it up, a replaced resource rebuilds all, and builds on another thread read a thread-local snapshot instead of
  live entries. Three rebuild triggers and a thread-local are easy to get wrong. Suggest documenting in render-graph.md.
- `SharedConstants.h` upload points. One upload pass per step of a declared segment (kUploadPoints = 19 matching
  FrameAnchors' kMaxSteps), so each step reads the values its tap saw. The constant must track kMaxSteps by hand. Suggest a
  static_assert or deriving one from the other.
- `RunShapes.h` / `FrameSequence.h` / `CensusWindow.h`. Three census diagnostics for the phase plan, each with a long header about
  what it measures. If the phase plan no longer needs them, they are candidates for removal.

## 2. Hazards kept

- Frame/PostChain.cpp: every engine global and member offset with its SE/AE IDs; the RenderTargetManager is a static object at
  524970/411451, not the pointer CommonLib's GetSingleton reads.
- Frame/PostChain.cpp: Upscaling toggles the engine's TAA flag around the end-of-frame effects, so the flag at Begin says nothing.
- Frame/PostChain.cpp: the sun stage ends before the post chain's D3D11 work reaches the GPU.
- Frame/PostChain.cpp: RenderSSAO increments and wraps its counter before drawing (raw AO target 62 + 2 * counter).
- Frame/PostChain.h: luminance last-level parity is flipped just before the last level draws.
- Frame/FrameDriver.cpp: a second freeze test after Begin can disagree, since another thread can push a menu.
- Frame/FrameDescription.h: a recording binds the slot versions it was recorded with, so a new version needs a new recording.
- BuildManager.cpp: one build at a time on either thread (buildMutex); a change waits for the poll so it cannot meet a held build.
- BuildManager.cpp: superseded builds are attached anyway after three in a row; read BuildMilliseconds before attach takes the graph.
- GraphResources.cpp: DXVK's stream may still initialize an exported image, so release it after D3D11 passes that point.
- GraphResources.cpp: release the D3D11 views first, since nothing of D3D11 may outlive the memory it views.
- GraphResources.cpp: Register (build thread) vs Resize (render thread) under a shared mutex.
- GraphResources.cpp / static TransientNames: never destroyed, since twins die during static destruction too.
- GraphResources.cpp: a typeless depth format names no image format, so depth images use the DSV format.
- ProgramCache.h / ProgramBuild.h / RasterProgram.cpp: CS's common defines read State, so resolve them on the caller's thread.
- ProgramCache.h: never call GetNow after StopProgramBuilds, since a dropped build never finishes.
- RasterProgram.cpp / ComputeProgram.cpp: read the registry's sources generation before reading sources, or a reload is missed.
- RasterProgram.cpp: stencil reference is set every draw since it holds across the list's passes.
- RasterProgram.cpp: Vulkan ends an occlusion query in the pass it began in.
- ProgramBuild.cpp: -fspv-use-unknown-image-format, else DXC types float4 RWTextures as Rgba32f and an RGBA16F view is miswritten.
- ProgramBuild.cpp: CS_GPU_AMD define for AMD shader compiler workarounds.
- AnchorPasses.cpp: depth copies drawn as fullscreen passes, since a transfer copy makes AMD decompress and recompress (~60 us, RX 6600).
- AnchorPasses.cpp: a draw that clears its target is always recorded, even with its gate closed.
- DxvkImageImport.cpp: rhi_interop_dx12.h clashes with this unit's Windows SDK headers, so the D3D12 import is declared by hand.
- DxvkImageImport.cpp: a reused VkBuffer handle value must be imported again, or it reads the old buffer's memory.
- DxvkImageImport.cpp: raw views load whole words, so imported buffers are padded.
- BufferRefs.cpp: a null ref beside a real address makes a shader read heap slot 0 as a buffer.
- BufferRefs.cpp: a latch/version mismatch across frames in flight is a race, counted as a skip, not a mismatch.
- PublishedResources.cpp: builds on another thread read the building snapshot, never the live entries.
- UndeclaredAccess.cpp: D3D11 refuses every write to an IMMUTABLE resource; guest mode treats SRV-only or VB/IB-only DEFAULT as immutable.
- ShaderBindings.h: 128-byte push block (Vulkan's guaranteed minimum), fits D3D12's 64 root constants.
- ShaderBindings.h: an untracked SRV must stay in its shader-read layout whenever a shader may read it.
- ShaderBindings.cpp: the runtime's descriptor service exists before any graph is built.
- SharedConstants.cpp: a CBV range is whole 256-byte rows on D3D12.
- FrameStaging.h: a block must be released last by the recording that used it, so it returns only once the GPU is done.
- RunGate.h: IsOpen returns true when it cannot tell, so a caller that skips on false never drops work.
- Frame/Transients.h: a frame transient whose bytes were reused enters from Undefined, so its span's first pass must write it.
- Frame/Transients.cpp: only CS stages place their own transients in the arena, since DCLF's epochs run a varying number of times.
- Frame/FrameResources.cpp: a new size is the slot's next version at the frame boundary, before the new build can switch in.
- Frame/EngineTargetUse.h: GPU work reaching an unbacked target is fatal in development builds.
- ExternalWork.h: always a full memory barrier first, since the native immediate context shares the queue unseen by the ledgers.
- ExternalWork.cpp: the stages whose commit point this is commit first, and the render owner's queued epochs drain first.
- GpuTimingSource.cpp: native mode's D3D11 queries answer 0, so the graph is the only timing source there.
- PrefixSum.cpp: one pass with a full barrier rather than two passes, since the second dispatch rewrites the first's offsets.
- Switches.h: each switch read takes a lock, so keep hot-path switches in a local static.

## 3. Possible bugs or dead code noticed

- `Frame/FrameDescription.h`: the comment above `KindName` and `SegmentName` (removed) listed fixed strings; no issue, noted only
  because "SegmentId, kSegments and SegmentName are in SegmentId.h" was a stray pointer comment, removed.
- `SharedConstants.h`: `kUploadPoints = 19` must equal FrameAnchors' kMaxSteps by hand; nothing checks it (see targets).
- `GpuTimingSource.h`: `Frame::window` and `Frame::epochs` lost their trailing comments as self-explanatory; no issue.
- `Frame/PostChain.cpp`: the comment for `kSaoContrast` ("draws the first level at most 16 pixels high with ISMinifyContrast") was
  cut as the name plus `WorldFields::saoContrast`'s comment say it. No code issue.

No real bugs found in this slice.

## 4. TODO/FIXME lines found

None.

## 5. Coverage

Done (63 files): Frame/FrameDescription.h, Frame/FrameDriver.cpp, Frame/PostChain.cpp, BuildManager.cpp, GraphResources.cpp,
RunGate.h, ProgramCache.h, GpuTimingSource.h, PublishedResources.cpp, ShaderBindings.h, FrameStaging.h, UndeclaredAccess.cpp,
SharedConstants.h, Frame/Transients.h, Frame/PostChain.h, Frame/Recipe.cpp, ComputeProgram.h, DxvkImageImport.cpp,
AnchorPasses.cpp, RasterProgram.cpp, ExternalWork.h, GenerateMips.h, RunShapes.h, CensusWindow.h, PrefixSum.h,
Frame/FrameResources.h, GpuTimingSource.cpp, FrameSequence.h, Switches.h, BufferRefs.cpp, Backend.h, FrameAnchorSelfTest.cpp,
SharedConstants.cpp, RunShapes.cpp, ProgramBuild.h, Frame/SegmentId.h, Frame/EngineTargetUse.h, MemoryWindow.h, UavClearPass.h,
MemoryWindow.cpp, GraphResourcesSelfTest.cpp, ComputeProgram.cpp, ShaderTarget.h, ScheduleWindow.h, GpuLoadPass.h,
FrameAnchorSelfTest.h, ProgramBuild.cpp, ExternalWork.cpp, GenerateMips.cpp, ShaderBindings.cpp, ScheduleWindow.cpp,
FrameSequence.cpp, BufferRef.h, RunGate.cpp, Frame/EngineTargetUse.cpp, PrefixSum.cpp, GpuLoadPass.cpp, FrameStaging.cpp,
Frame/FrameResources.cpp, UavClearPass.cpp, Frame/Transients.cpp, Switches.cpp, ProgramCache.cpp.

Left as they were (nothing worth changing): Frame/FrameDescription.cpp (one FNV-1a line), CensusWindow.cpp and Backend.cpp (no
comments). Not touched by design: DxvkOrgInterop.h and the 17 files rendergraph-top finished.

Numbers (lines carrying a comment, trailing comments included): 1,574 before, 1,025 after. `git diff -w --stat` over the 63 files:
774 insertions, 1,289 deletions. Every file was checked mechanically: with comments and whitespace stripped, each is identical to
HEAD. Line endings unchanged (all LF). Trailing comments keep their original alignment column.
