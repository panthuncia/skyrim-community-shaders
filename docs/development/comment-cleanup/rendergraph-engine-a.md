# Comment cleanup notes: rendergraph-engine-a

## 1. Simplification or documentation targets

- Native.h top-of-file: startup stage list. Original listed 8 stages with hook sites: device/swap chain/handles in the game's D3D11CreateDeviceAndSwapChain (SE 75589 / AE 77396), engine stores RendererData +0x38/+0x40, device/context globals SE 524729/524771, AE 411348/411391; engine init in InitD3D hook; graph in RenderGraphRuntime::Initialize; first present copies back buffer. Suggest: docs/development page for native startup; header now only says stages exist.
- Native.h graph-owned texture classes (exempt / promoted / tracked persistent / host-made imported / unbacked / frame transient / stream shell). Six ownership classes each with its own lookup (GraphTextureOf, ImmutableTextureOf, PromotedTextureOf, IsTrackedTexture, IsUnbacked, DescribeResource). The comments were the only explanation of which lookup answers what. Suggest one ownership enum on the native texture and a single lookup returning it; or a doc table.
- Native.h FrameTransientScope: thread-scoped implicit mode change for texture creation (textures have no image until scope ends). Unexpected action at a distance; suggest an explicit creation flag instead.

- NativeDevice.cpp Immediate / RunOp / LedgerResourceOf: the immediate context's operations take one of two routes (ORG external client once the graph is up, own lists before), tracked images move via the ledger while untracked ones move Common->state->Common, and exempt textures additionally leave shader-read around each op. LedgerResourceOf's 7-line comment was the only statement of which records have a ledger (tracked PixelBuffer, promoted+graph-managed exempt, host-made image imported when NativeGraphMovesImages). Suggest: once every texture is graph-owned, drop RunRhi and the Common contract so there is one route; until then a doc table of resource class x route.
- NativeDevice.cpp GraphBackBuffer / MakeGraphBackBuffer / NextGraphVersion / Resolve rename: the engine's back buffer handle is silently redirected (NativeResource::renamed) to a graph-owned version; every Resolve follows it. Version rotation machinery remains though count is fixed at 1 (rotation made prepared tickets stale). Suggest: drop the versioning (single graph-owned back buffer) and the rename indirection.
- NativeDevice.cpp QueuePresent / PresentLocked: presents can run on the render thread or the submission thread, with the version chosen on the render thread and pacing by queued/done counters; plus the loading-screen-from-another-thread case. Hard to follow; suggest documenting the present threading in one place or making present always queued.
- NativeDevice.cpp WaitGraphReaders: CPU map of graph-imported buffers waits only to the last sealed frame boundary; correctness relies on readers of the current frame running after the write. Original: "The current frame's executions are not waited for. A mid-frame wait names only sealed work: later work of the frame may be queued behind this thread. A buffer mapped once a frame before its readers run is covered: its readers of earlier frames are sealed, and this frame's run after the write." Worth a doc.
- NativeDevice.cpp depth-stencil staging (RepackDepthStencil, RepackStaging, UnpackStagingIfNewer, planesNewer, DepthPlaneFloat): the CPU sees D3D11 interleaved texels while the GPU copies separate planes into side areas of the same buffer, synced lazily by a flag. Subtle; suggest isolating into one small type with its own doc.
- NativeDevice.cpp NoteWrite promotion: exempt textures are promoted to tracked on first context write, asynchronously through the build manager at next poll. Unexpected deferred state change; the write census found a single case (loading screen). Suggest making that one texture non-exempt at creation and removing promotion.
- Scaleform/Passes.cpp kInstances / DisplayPass / run gate: the UI stage declares 16 fixed pass instances per surface (8 before a menu's scene, 8 after), each with its own run-gate entries and slots; a draw's vertex count comes from a gate entry that only this client's commit opens, so a stale slot draws zero. The comments were the only explanation of why instances are fixed and how a run without a commit becomes a no-op. Suggest: a doc page for the UI stage and run gates (docs/development/org-native-frame-graph.md "Frame kinds and the UI stage" is the cited home), or a simpler design where the stage records displays directly without fixed instances.
- Scaleform/Passes.cpp declared reads vs raw index (DeclaredRead, Commit, RegisterReads): a newly seen tracked texture is read through its raw bindless index (validator counts it as declared, "interim") until a re-registration rebuilds the graph off-thread. Surfaces vs reads registrations are tracked separately (surfacesRegistration, surfacesGeneration, builtRegistration). Hard to follow; suggest declaring all display textures through one per-frame dynamic read set, or documenting the two-registration lifecycle.
- Scaleform/Passes.cpp MakeClient scratch declarations: scratch images are declared by every instance but the first, relying on "nothing earlier in the frame wrote them and a draw into one clears it first" because the frame arena aliases them. Correctness depends on an ordering argument only in the comment. Suggest making the clear-on-first-use explicit in the graph (a transient that ORG knows is cleared) rather than relying on declaration gaps.
- Scaleform/Passes.cpp DrawStates: the enumeration of every draw state must stay in step with what Renderer.cpp emits; the original 13-line comment listed the valid vertex/pixel/blend/stencil combinations. Original kept here for reference: "vertex and pixel kinds: 1 with 1-3 (solid, bitmap), 2 with 4-6 (bitmaps), 3 with 12-14 and 16-18 (XY16iC32 gouraud), 4 with 11, 13, 14, 15, 17, 18 (XY16iCF32 gouraud), 5 with 11, 14, 15, 18 (gouraud type 3); filters 1 to 24; line strips of kind 1 with 1; blends: the Multiply and Darken kinds (3, 6, 15-18, filter 24) only with the multiply and darken states (4, 6), the other kinds with 1, 2, 3, 5, the solid and glyph kinds (1, 4) with any, the filters from the filters' array (8 + the index); every kind with 7; 3D draws (vertex kinds 7-11) are built at first draw; stencils: off in the pass without masks; off and test with any blend, clear, increment and decrement only with 7 in the pass with them." Suggest generating this table from the capture code or a shared constexpr table both sides use.
- Scaleform/Renderer.cpp PushRenderTarget: original described the renderer's own transform for pushed targets: "the viewport (0, 0, target +0x60, target +0x64), the view matrix mapping the frame rectangle to clip space ([2 / w, 0, -1 - 2 left / w; 0, -2 / h, 1 + 2 top / h], w and h at least 1, no user matrix), the depth-stencil state off." The code reproduces it; worth keeping in docs/development/p5/scaleform.md.
- Tripwire/Tripwire.h + GuestDevice.h: the guest/native/wrapped handle model. Engine fields hold tripwire handles that either forward (guest) or fail or serve (native). CS's globals stay raw DXVK objects, and passing a handle straight to DXVK crashes. ORG resources attach either to a handle (Options::graph) or to a plain DXVK object through private data (AttachGraphResource), and GraphResourceOf searches both. The original top-of-file comment was 35 lines, and the rule "never hand a handle to DXVK directly, use Unwrap" lives only in comments. Suggest: make Unwrap impossible to forget at the type level (engine-field accessors that return unwrapped pointers), and move the handle model into a doc page.
- Flush/Flush.h flush latch: engine flushes run on the CPU but their binds are dropped. The dropped state is "owed" and paid by the next flush CS asks for. Draws are skipped unless vouched for through EngineDraws scopes or per-pass marks. KEEP and UNBIND switches exist to bisect stale-state readers, a write-behind defers engine constant buffer maps inside stage windows, and StateTrace exists only to diagnose DXVK dropping CS binds of stale targets. The original 37-line header was the only full explanation (see HEAD d7aadb11e, Flush.h lines 12-48). Suggest: this is transitional machinery for guest mode. Document it in docs/development/p5/flush.md and retire it once native mode is the only graph path.
- Flush/Flush.cpp Mark (the VouchPass table): vouching relies on BSRenderPass objects being 0x48-byte slots in one 65535-entry arena, indexed by slot modulo table size, with a frame stamp valid for the frame and the next. A fragile engine assumption. Suggest keying by the pass pointer in a small hash set, or vouching at the batch level.
- Bink/BinkHooks.cpp ClaimForBlockingTick: the startup logo plays on the MoviePlayer thread, which only becomes render owner at Renderer::Begin, after Eligible already decided. So ownership is claimed early at tick start, justified by that thread holding Renderer::Lock for the whole sequence. An unexpected cross-thread ownership handoff. Suggest making render ownership an explicit claim the movie player takes before its first tick.
- Bink/BinkGraph.cpp DrawPass::Revision, and the same in Scaleform's DisplayPass: one pass behaves differently (slot read at record time, or a new revision every run) depending on whether the UI stage holds the anchor. Suggest documenting "late batch vs ahead recording" once in the frame graph doc, or removing the anchor-own-epoch path.

## 2. Hazards kept

- Scaleform/Renderer.cpp: engine addresses kept (DAT_141540648 1/255, blend table 0x1417d4468, SE 82819 +0xC0 video mode test, SE 82817/AE 84940 ApplyMatrix, push/pop target IDs, DrawBlurRect SE 82836, filter offset table SE 0x141e0ac70, SE 0x140f5e840).
- Scaleform/Renderer.cpp BindStaleSecond: second texture behaviour inferred from the fill-off rule, not read in Ghidra.
- Scaleform/Renderer.cpp ReadTargets: under the flush latch the D3D11 context state is stale, read the engine's shadow state.
- Scaleform/Renderer.cpp EndDisplay: original always runs, it releases GFx's queued textures.
- Scaleform/Passes.cpp Commit: AddRef the view until GPU work retires, EndDisplay's queued releases may drop GFx's.
- Scaleform/Passes.cpp BlurConstants: slots 17-20 have srctexscale index -1, so the renderer writes ratios into neighbouring constants the shadow shaders read.
- Scaleform/GRendererLayout.h: all offsets, with SE/AE addresses of the functions that confirm them.
- NativeDevice.cpp FeatureHook: do not add 1.1-1.3 feature structs BasicRHI lacks; single-feature structs plus version structs together are invalid.
- NativeDevice.cpp Immediate::Route: commit stages before taking the context lock, re-entrant Route deadlocks ("resource deadlock would occur").
- NativeDevice.cpp: each command pool costs ~2 MB on RX 6600, lists share an allocator.
- NativeDevice.cpp: D3D12 reads UAV clear descriptors at execution (one descriptor pair per clear); RTV/DSV read at record time.
- NativeDevice.cpp: D3D12 resets only a closed list.
- NativeDevice.cpp MakeTexture: D3D12 typeless depth must stay typeless (typed D24/D32 cannot have an SRV).
- NativeDevice.cpp StageInitialData: D3D12 stencil is a second plane, both planes must start defined; 512-byte placement alignment.
- NativeDevice.cpp RecreateSwapchain: the back buffer must survive a swap chain remake, the engine keeps drawing through its views.
- NativeDevice.cpp PresentLocked: D3D12 Present does not block for frame latency, pace on the waitable object.
- NativeDevice.cpp CopyResource/CopySubresourceRegion: D3D11 drops self-copies and Vulkan copies must not overlap.
- NativeDevice.cpp WrapTexture: block-compressed images cannot be cleared.
- NativeDevice.cpp: desc{} zeroes only the union's first member (UAV desc planeSlice).
- NativeDevice.cpp MakeBufferResource: buffer size rounded to words so a raw view reaches an odd index list's last index.
- Native.h PaceHeldPresent: without the wait, CPU runs ahead and per-frame-kept resources (upscaler views) are freed while GPU reads them.
- Native.h OnShaderCompilerReady: must run after the compiler exists or engine GenerateMips makes nothing.
- Native.h DescribeForCommands: caller must Retain a_views until GPU is past them.
- Tripwire.h: never pass a handle straight to DXVK's device or context, DXVK casts to its own class. Use Unwrap.
- Tripwire.cpp GetRetired: never destroyed, a guest object's tag can die after static destruction began.
- Tripwire.cpp: native mode quietly refuses later interface versions (IDXGISwapChain3 and so on). Every CS caller handles the refusal.
- Tripwire.cpp: native handles keep private data bytes as D3D11 does. The icon loader and Landscape Seams Fix tags rely on it.
- Tripwire.cpp: the native swap chain is borderless only, never exclusive fullscreen. Resize follows WM_WINDOWPOSCHANGED.
- TripwireHandles.h: the engine binds no GS/HS/DS, but state backups save and restore them.
- GuestDevice.h/.cpp: engine pointers replaced (device global SE 524729 / AE 411348, context SE 524771 / AE 411391, RendererData +0x38/+0x40). Extra references are intentionally never dropped, so a shutdown Release frees nothing in use.
- Flush.cpp: DispatchCSShader's Dispatch is a tail jump, so the return address is its caller's. Detour install order relative to Hooks.cpp and ImageSpace matters. SE 75488 only calls 75489 while AE 77275 inlines it. RSS dirty words at SE 0x143027eb0 / AE 0x14202ab70.
- Flush.cpp Owed: no lock, relies on only the render owner flushing.
- Flush.cpp guest Map write-behind: DXVK's stream must take no work inside a CS stage window.
- StateTrace.cpp: DXVK drops an SRV bind of a resource bound as an output, and a target bind unbinds same-resource SRVs.
- Bink/BinkHooks.cpp: Renderer::Lock at Renderer +0x2790 SE / +0x27F0 AE. Mode 2 TickInFrame has no caller in SE 1.5.97 / AE 1.6.1170. SE calls FreeTiles directly while AE inlines it into Close.
- Bink/BinkGraph.cpp: D3D12 copy alignment (256-byte rows, 512-byte start). Edge tiles must match the engine's zero-filled texels. Quads follow CreateTileTextures's float arithmetic order to land on the same pixels.
- Bink/Bink.h RequestPlay: the console's PlayBink does nothing as a compiled script and SKSE tasks run on job threads, so plays go through the main-thread tick.

## 3. Possible bugs or dead code

- NativeDevice.cpp MakeGraphBackBuffer: version count is hard-coded to 1, so the multi-version machinery (records vector, NextGraphVersion rotation, NoteNewVersions re-prepare) is effectively dead.
- NativeDevice.cpp Queries: entries are replaced per query but never erased when a query is destroyed (small unbounded growth, by design per original comment).
- Tripwire/TripwireInterfaces.inl is generated by tools/generate-tripwire-interfaces.py. Its comments were left untouched, since an edit would be lost on regeneration. Its header says "TripwireHandle.h expands these", but the file is TripwireHandles.h (fix in the generator).
- Scaleform/Renderer.cpp BindStaleSecond: the behaviour is inferred, not verified in Ghidra. The comment saying so was kept.

## 4. TODO/FIXME lines

- None found in the slice.

## 5. Coverage

All 34 files done: every .cpp, .h and .inl under src/RenderGraph/Engine/ Native, Tripwire, Flush, Scaleform and Bink. TripwireInterfaces.inl was left unchanged on purpose because it is generated.

Comment lines (own-line comment lines plus code lines with a trailing comment): 2701 before, 1485 after. Most of the lines kept are also much shorter. git diff -w --shortstat over the slice: 33 files, 1081 insertions, 2266 deletions.

Every file was checked mechanically: with comments and whitespace removed, each is identical to HEAD (d7aadb11e). Doxygen @{ and @} counts match HEAD in every file.

Scripts (dump, apply, verify): scratchpad comments/rendergraph-engine-a/cc.py.

NativeDevice.cpp was CRLF in the working copy and is now LF. That matches the repo's eol=lf attribute, and it is whitespace only. AllocCounter.cpp, AllocCounter.h and RtSampler.cpp stay CRLF as they were.
