# Feature-port survey (condensed notes, integ/hard-ports b1b39842d)

Scope: about 33k lines in `src/Features/*/ORG*`, `PostProcessing/ORG`, `Effects11/ORG`, and the
feature-side hooks.

## (a) Weaknesses
- W1 "Return to D3D11" paths: ORGTerrainBlending.cpp:428 Fail (per frame from Observe :529), ORGHDRDisplay.cpp:390, ORGScreenSpaceShadows.cpp:423-435 (Prepass->Stop), ORGIBL.cpp:281,302, ORGUpscaling.cpp:511 MarkFailed + Upscaling.cpp:733 DrainCommandRing. Each unregisters mid-frame = LateChange. Native D3D11 path is a tripwire no-op (Tripwire.h:24). 10 other ports use a [[noreturn]] fatal helper. Dir: one fatal helper; delete Fail/Stop/onFailed/failed, IBL CreateSHTextures re-create, ReleaseAfterFailure. M.
- W2 Runtime fault = global D3D11 fallback (RenderGraphRuntime.cpp:3471, IsActive :2707); each port handles differently. Dir: report_and_fail; IsActive = set up only. S.
- W3 ~60 Setup "stays on D3D11" returns. Dir: fatal (RequireDclf model). S.
- W4 Per-frame D3D11 frames: EHF 12 reasons (ORGExponentialHeightFog.cpp:1087-1205); post chain Update holds (ColorGrading preview/save ORGColorGrading.cpp:110-112, LUT not loaded, LensFlare/PhysicalGlare debug), no stage, StagePassed, before first plan; FX/rays no stage, missing inputs, reload after plan; HDR between registration and adoption; TerrainBlending until first registration; SSS until first commit; CloudShadows.cpp:69-104 queued layer ops flushed to D3D11, engine-drawn faces keep D3D11, :309 occlusion copy when QueueOcclusionCopy refuses. Dir: first adoption before first world frame; holds -> declared choices; missing inputs fatal; stage for every frame kind. L.
- W5 Cloud maps: ORGCloudMaps.cpp:572-680 Submit false "work waits", callers retry (Ndf.cpp:534, CloudNoiseGenerator.cpp:245, Storm.cpp:235); kStaleFrames=600 (:60), EndFrame prune (:696) builds client on render thread + Invalidate. Dir: generators declare steps at setup, declared client keyed on map set. M.
- W6 Engine targets observed + Invalidate in ~19 files (SSS z-prepass :146, post kMAIN/kMAIN_COPY :1183-1222, shadow maps in EHF/clouds/Skylighting/PhysicalSky/VolumetricShadows, DynamicCubemaps forwardRenderTargets[0] :997, Upscaling motion/depth :1095, HDR back buffer). Dir: CS owns engine targets; ~30 Observe sites go. L.
- W7 Cross-feature state via D3D11 export identity: IsExportOf/IsPublished/ORGX::Get().IsActive() (EHF 9, PhysicalSky 4, Clouds/SSS/CloudShadows 2). Dir: existence rule; read owner's adopted Setting. M.
- W8 GPU constants from live settings, shape from adopted (ExponentialHeightFog.cpp:102-140, DynamicCubemaps, TerrainBlending, ViewAtmosphere, GpuBendSettings). Dir: GetCommonBufferData reads Adopted; GPU block from field map. M.
- W9 Render-thread compiles: ORGUpscaling.cpp:519-528 CopyProgram from QueueCopyToMain/QueueFrameGenInputs (:1072,:1134); :1157-1170 depth programs in QueueDepthUpscale. Known: FX parse/lower in key fn, post chain replanned each adoption. Dir: DeclareProgram on engine-format vars; planning on programs lane. M.
- W10 LLF culling own epoch via host extension (ORGLightCulling.cpp:509,584 ExecuteEpoch(Segment::LightCulling)); not a revision member; resize binding unchecked. Dir: stage client at anchor. M.
- W11 FrameAnchors::Scheduled at ORGCloudShadows.cpp:741, ORGScreenshot.cpp:265, ORGUpscaling.cpp:694,1116,1215. Dir: declared clients. S.
- W12 Written/first-clear bookkeeping (see P3). M.
- W13 Stale CS_ORG_<FEATURE>=1 docs; Requested() = GraphMode(); 17 dead org.<feature> keys in schema.toml (e.g. org.ibl :2625); ORGSubsurfaceScattering.cpp:451 reads CS_ORG_SSGI_DEBUG. S.
- W14 Tests: hand-tuned frame counts after setting steps; no_log substrings (false failures items 47, 63); only effects11 checks D3D11 frames. Dir: await="adopted"; per-client graph frame counter, expect.graph_frames. M.
- W15 ORGLocalShadows (645 lines) blocked when DCLF loaded (LightLimitFix.cpp:645); EHF fallback (:1180) dead. User decided 2026-11-20: rework the cache to follow DCLF shadow views.

## (b) Reinvented
- P1 D3D11 wrappers following a versioned twin: hand-made (AddRef; new Texture2D; copy views) at ORGCloudShadows.cpp:235, ORGHDRDisplay.cpp:431, ORGPhysicalSky.cpp:635, ORGVolumetricClouds.cpp:1112,1141,1304, ORGSkylighting.cpp:648,677, ORGCloudMaps.cpp:436,466; PhysSkyFollowTwin (:177), CloudFollowTwin (:1250, byD3D11 map), HDR NewExport (:441). -> GraphResources::D3D11Wrapper<T> via MakeWrapper(declared), re-pointed in Revision::AdoptAll; ByD3D11 lookup.
- P2 Observed + Invalidate: 19 files, 34 calls (struct compare, pointer word vectors, counters inputsWanted/Twins().generation).
- P3 Written/first clear: EHF Written() weak_ptr (:746-770), PhysicalSky shadowCleared (:353), Clouds s_outputsWritten (:15,525,1322), Skylighting mapCleared/committed/resetsSeen + native branch (:690,715), SSGI lastReset+FirstFrame+queuedResetHistory (:686), HDR uiCleared, TerrainBlending TerrainDepthCleared, FX first-run clear, FirstRun in DynamicCubemaps/TerrainShadows/ViewAtmosphere. -> Birth::{Zero,Value,Undefined} + Adopted().Written(slot).
- P4 Held registration: matchesById+generation+HeldNow+deque in post chain (ORGPostProcessing.cpp:1054-1080), FX (:1710,:2147), SunRays FindHeld (:651), HDR held, PhysicalSky deferred, VolumetricClouds, ViewAtmosphere, Screenshot. -> DeclaredClient<Key>::HeldKey().
- P5 Per-frame adopted copies: TerrainBlending frameEnabled/LastFrameEnabled (TerrainBlending.h:65), PhysicalSky frameEnabled, ORGSunRays AdoptedToggles, AdoptedSpecularGI, Upscaling adopted method. -> CS::Setting<T>::Adopted() atomic.
- P6 CPU settings -> GPU struct field copies: EHF, DynamicCubemaps, TerrainBlending, ViewAtmosphere, SSS. -> CS::GpuBlock<Gpu> field map + colour-space hook (src/Utils).
- P7 DisplayOr / width?:setup at ORGHDRDisplay.cpp:75, ORGScreenshot.cpp:526, ORGUpscaling.cpp:540, ORGScreenSpaceGI.cpp:336, ORGScreenSpaceShadows.cpp:111,345, ORGSubsurfaceScattering.cpp:447, ORGTerrainBlending.cpp:472, clouds screenFallback (:354). -> DisplaySize set before first declaration; Frame::DisplaySized(desc).
- P8 One-texel off versions (~15): EHF, SunRays, PhysicalSky clouds, ViewAtmosphere, SSGI, Upscaling, LLF, LocalExposure, MotionBlur, PhysicalGlare, LensFlare, ColorGrading, TerrainShadows. -> ExistsWhen(var...).
- P9 Engine depth slot from D3D11 view: ORGPhysicalSky.cpp:128, ORGVolumetricClouds.cpp:364, ORGSkylighting.cpp:349, ORGExponentialHeightFog.cpp:625.
- P10 Port skeleton: Requested() x24, Reserve x26, Get singleton, IsActive, #else stubs x25 (20-40 lines). -> RenderGraph::Port / CS_ORG_PORT(Name); ORG files only with CS_HAS_RENDER_GRAPH; null port.
- P11 Pass styles: 38 hand-written TypedRenderGraphPass with 35 Revision() pointer hashes (SSS, EHF, Skylighting, SSGI, Subsurface, TerrainShadows...), vs AnchorPasses (VL, PhysicalSky, Upscaling, HDR); own builders: post Builder, cloud-map StepDesc, FX FxLower.
- P12 11 [[noreturn]] Fail* copies (CloudShadows, DynamicCubemaps, FX, LLF, SSGI, SSS, TerrainShadows, ViewAtmosphere, VolumetricLighting, VolumetricShadows, PhysicalSky); reason logs fallbackReason/refusal/SetReason/Inactive/call_once. -> PortFatal + Util::ReasonLog.
- P13 26 CreateSampler in 20 files. -> ShaderBindings::Sampler(Preset).
- P14 ssgi-preset command, CS_LLF_LOCAL_SHADOWS switch. -> cs-test call step + settings injection.

## (c) Delete
- ORGLocalShadows only if reworked differently (decision: rework, not delete).
- ORGPostProcessing.cpp:1170 "has no ORG implementation" branch (all 14 effects ported).
- 17 org.<feature> schema keys, CS_ORG_<FEATURE>=1 mentions.
- D3D11 recovery: IBL onFailed/CreateSHTextures, TerrainBlending onFailed, ORGUpscaling::ReleaseAfterFailure + Upscaling.cpp:733, HDR Fail, SSS mid-frame Stop.
- Per-port IsActive() runtime checks after W2.
- CS_ORG_SSGI_DEBUG in Subsurface -> own switch.
