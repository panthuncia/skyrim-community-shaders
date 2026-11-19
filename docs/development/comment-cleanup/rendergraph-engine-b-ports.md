# Comment cleanup notes: rendergraph-engine-b-ports

Slice: `src/RenderGraph/Engine/` (ImageSpace, Skeleton, Targets, Pipeline, Constants, Queries, Buffers, Textures,
LoadingScreen, and the files directly in `Engine/`), plus every `ORG*` file under `src/` outside DCLF and `src/RenderGraph/`.
Original comments are in git HEAD for every file listed here, if one needs to be recovered.

## 1. Simplification or documentation targets

- **`Engine/Skeleton/Skeleton.cpp`, `Host`, `Place`, `HostFor`, `StagePlaceFor`, `Take`, `RunCopy`.** The comments were the
  only explanation of where each engine clear and copy runs. That can be a DCLF epoch, an anchor, a CS stage place or a UI
  stage place. They also covered when a clear lags, when it is dropped, and the special cases per frame kind: Bink frames,
  frame 0, the face sky clearing reflection faces, and TAA mask clears inside the post chain. You cannot work these rules out
  from the code. Suggestion: one table keyed by (target, flags, frame kind) that gives the host and the drop rule, plus a
  section in `org-native-frame-graph.md`.
- **`Engine/ImageSpace/ChainSteps.inl`, `ChainRoute`, `ChainGroupsOn`, `OpenThrough`.** TAA routing has two forms. In the
  direct form TAA writes a scratch target and a copy puts it back. In the routed form the tonemap writes the scratch and TAA
  reads it. The build key picks the form, and a pass-through copy is needed in routed frames that have no TAA. Separately,
  several groups are "always on" because the engine decides about them late in the frame: the screenshot, the capture, slot 5
  radial blur, refraction, and both tonemap and luminance forms. Suggestion: a per-group "late-decided, gated" property
  instead of a comment at each site. Document the routing in the post stage doc.
- **`Engine/ImageSpace/ImageSpace.cpp`, ShapeKey name encoding** (`kDepthInput`, `kCubeInput`, `kFlowMapOutput`,
  `kLatchSourceInput`, `kDefaultTextureInput`, `kHdrSceneOutput`, `kMipShift`). The comments were the only explanation of
  this bit-packed int. Also `InnerVariant` in ChainSteps.inl packs its key as 0x8000 blend, occurrence << 12, 0x800 slot 7
  and 0x400 inner. `ChainRoute` uses variant bits 13 to 15. Suggestion: typed structs with named fields.
- **`Engine/ImageSpace/ImageSpace.cpp`, the scope, vouch and census model** (`ScopeKind`, `engineCalls`, `NoteEngineDraw`,
  the flush latch skip, `Refuse` and `Failure`). Which draws are batched, which are left unvouched, and how the census
  counts them is spread over many comments. Suggestion: a short doc section.
- **`Engine/ImageSpace/ImageSpace.cpp`, `g_registrationRatios` and `RegisterAtBoundary`.** Upscaling sets the dynamic
  resolution ratios inside a frame and resets them after it. So a registration has to keep the ratios of the frame that asked
  for it. This is a hidden cross-feature timing dependency. Suggestion: Upscaling publishes the frame's ratios explicitly.
- **`Engine/ImageSpace/ImageSpace.cpp`, `Ineligible`.** Detecting the dynamic-resolution quad depends on the manager offsets
  +0x48, +0x50 and +0x58. The quad itself is refused rather than the flag. Kept as two short lines. A doc entry would help.
- **`Engine/Queries/OcclusionQueries.*`, `QueryRing`.** It uses query ranges per lease index in chunks. Readbacks are
  declared at build time, so a chunk made later is unresolved until the next build. Commits made in a staging run are moved.
  A blocking read may only wait once the host has sealed the run. Suggestion: allocate every chunk up front (it already makes
  4 at setup) and drop late chunk creation. Document the seal rule.
- **`Features/CloudShadows/ORGCloudShadows.*`, layer operations.** The queue resolves chains of ops to untouched, zero or
  copy. Then come the gather and scatter passes, scratch slots shared across the start stage's face taps, gate ranges per
  anchor, and a D3D11 flush before any D3D11 use of the layers. The 44-line header comment was the only design text.
  Suggestion: draw all faces on one side, all graph or all D3D11, so the interleaving goes away. Or write a doc page.
- **`Features/PostProcessing/ORGPostEffect.h`, file header.** A 71-line porting guide with 9 rules (dual-target shaders,
  shared constant builders, the Update/Build/Commit/Executed contracts, gates, mips, names). Cut to 11 lines. The original is
  HEAD lines 3-73. Suggestion: move it to a doc under `docs/development/`.
- **`Features/Effects11/ORG/ORGFxRuntime.cpp`, `Commit`.** It writes the values for both the running shape and the
  registered shape, because which one the run's graph holds is only known after the run. Suggestion: let the run report
  its shape before the commit, or keep a single shape per epoch.
- **`Features/DynamicCubemaps/ORGDynamicCubemaps.h`.** The update runs at DeferredPasses, not at DeferredComposite. The ORG
  composite reads the cubemaps through descriptors the graph does not track, so writing them in the same epoch would race.
  Suggestion: declare those reads in the composite, then the work can move back.
- **`Features/Terrain*`, `Skylighting`, `TerrainShadows`, "NeedsRefresh" and "cleared this frame" handshakes.** Several
  ports use flags passed between the feature and the port ("did the graph clear X this frame", "has a full refresh run in a
  graph holding this registration"). Each one is a two-sided protocol that is explained only in comments. Suggestion: a
  shared helper for "work done in a graph holding registration N".

## 2. Hazards kept (index)

- ImageSpace.cpp: NiTArray::size counts non-null entries, so use free_idx (AE 0x141481a30).
- ImageSpace.cpp: CS caps sampler anisotropy at 8, and the engine leaves LESS set after the comparison sampler.
- ImageSpace.cpp: staged shaders without the ORG header compile D3D11 cbuffers under DXC and every pipeline fails.
- ImageSpace.cpp: the vtable slot hooks are installed late because Legacy Graphics Compatibility checks the original vfunc.
- ImageSpace.cpp: TriShape VertexDesc is at +0x10, ImageSpaceEffect sits at +0x90 of BSImagespaceShader, and the RenderSSAO composite flag is 528126/415071.
- ImageSpace.cpp: the RenderEffect dynamic quad flag is +0x58 (VR layout differs).
- ChainSteps.inl: FinalSize reads the render target manager static 524970/411451, not CommonLib's singleton.
- ChainSteps.inl / WorldSteps.inl: the luminance targets, water history and kSAO_CAMERAZ stay engine targets because DCLF, the skeleton or Upscaling write or read them.
- Skeleton.cpp: the engine's depth table has kTOTAL entries; the clear programs only write depth 1 and stencil 0.
- Skeleton.cpp: Skylighting lends the precipitation occlusion slot within a frame, so remakes are checked at frame start.
- SkeletonHooks.cpp: a seven-byte call site needs the boundary check skipped and the seventh byte NOPed.
- EngineTargets.cpp: WindowSizeChanged never releases a target's UAV. AE has 116 render targets while CommonLib declares 114.
- EngineTargets.h: DXVK cannot wrap GENERATE_MIPS images. Tripwire handles crash without the guest context.
- EngineConstants.cpp: the Tracker is never destroyed, because retired chunks come back during shutdown.
- BufferContents.h: the engine writes some constants after Unmap (BSLightingShader VPOSOffset).
- DynamicRing.cpp: a full chunk is parked until frame end. In guest mode the release query must not reach DXVK's stream inside a stage window.
- OcclusionQueries.cpp: a blocking GetQueryData must wait only after the host sealed the run, or it deadlocks.
- Descriptions.h: Deferred and ShadowmapRasterizerFix swap state table entries at runtime.
- D3D12Device.cpp: shader-visible heap above 1,000,000 descriptors is refused (NVIDIA drew no world).
- Engine.cpp: Flush hooks must install first for detour nesting.
- MainTick.h: SKSE AddTask runs on job threads in this setup.
- EngineHooks.cpp: hook DXVK's device, not a guest handle, since handle methods are shared code.
- ORGHDRDisplay.cpp: only one back buffer may be copied into; DXVK rotates several on its own thread.
- ORGUpscaling.cpp: Streamline may bind its own descriptor heaps, so the frame's are restored.
- ORGExponentialHeightFog.cpp: the shader samples the terrain heights without a null check.
- ORGCODBloom.cpp: blend factor moved into shader alpha because a reused recording would freeze it.
- ORGSkylighting.cpp / ORGTerrainBlending.cpp / ORGSunRays.cpp: second imports of the same image make separate unordered graph resources. Readers must use the published id or the owner's import.
- ORGLightCulling.cpp: the latch copy stays on the graphics queue to avoid a DXVK command list split.

## 3. Possible bugs or dead code noticed

- `Engine/ImageSpace/ImageSpace.cpp` near the old line 748: an orphaned doc block about mapping an SRV to an engine target
  (including the kFRAMEBUFFER back-buffer case) sat above `kDefaultTextures`, away from its function. Removed. The function
  it described (probably TargetOfView) now has no doc.
- `Features/ScreenSpaceShadows/ORGScreenSpaceShadows.cpp:43` (old): a comment about importing Terrain Blending's D3D11
  blended depth sat above `kMaxDispatches`, with no matching code. It is stale. Removed.
- `Engine/D3D12SelfTest.cpp`: both DeviceRefs and Present were labelled "Step 3", and the header said "three checks" while
  there are four. The comments are fixed. Any log text that says "step 3" twice was not checked (it is code).
- `Features/LightLimitFix/ORGLightCulling.cpp` (old line 591): a comment said "Phase 4 takes them from the declared views".
  That is a future plan for the indices latched at commit. Removed as history.

## 4. TODO/FIXME lines found

None in the slice.

## 5. Coverage

- Part 1: all 42 files done.
- Part 2: all 64 `ORG*` files done.
- Comment lines (whole-line and trailing, counted by script): part 1 2764 -> 1363, part 2 3120 -> 1847, total 5884 -> 3210.
  Many of the remaining lines are short trailing comments and the "volk must precede" include notes, which were kept.
- Every file passes a mechanical check. With comments and whitespace removed, it is identical to HEAD.
- Whitespace: shortened trailing comments keep their original padding. A few lines in touched files may have had their
  alignment padding normalized. Run clang-format check on the diff if alignment matters.
- `ORGScreenshot.h` and `ORGUpscaling.h` have CRLF in the working tree, as they did before. Git normalizes them.
