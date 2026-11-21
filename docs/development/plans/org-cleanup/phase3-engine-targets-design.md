# Phase 3 design: CS owns the engine's render targets

Status: design, 2026-11-20. Written for the cleanup plan's phase 3 (A1, A2, B1, B4, B15, B17, and "Investigate: the present
copy"). Based on `integ/hard-ports` at `71c457535`. No code has changed for it yet.

Read with [../org-cleanup-refactor-plan.md](../org-cleanup-refactor-plan.md), section A1. Calls the agent made that need the
director's word are marked **decided by the agent, to confirm**, with one line on why.

Other leads are changing the port skeleton (B7, `PortFatal`), the engine modules (`Engine::On()`, fatal fallbacks), the core
messages and the variable table (`Vars::Set` / `Adopted` / `OnAdopted`, B13) at the same time. This design uses those as the plan
describes them. Where it needs something from them, it says so in "Needs from other lanes".

## 0. The choices in short

1. **Retarget on the CS side, not in DXVK.** The engine's fields already hold CS tripwire handles. A handle gets a new
   operation, `Tripwire::Retarget`, which swaps the object it forwards to and the description it reports. One mechanism serves
   guest and native mode and B15's `D3D11Wrapper`. DXVK is not changed for the retarget.
2. **The D3D11 wrap moves from adoption to the revision lane.** `GraphResources::Resource::Prepare` wraps the new version and
   makes its views. `AdoptPrepared` only swaps pointers. Native `WrapTexture` gets a mode with no initialize and no wait. The
   first contents come from a birth policy instead.
3. **`EngineTargetSet`** replaces `EngineTargets.cpp`'s make-on-create model. After the first set, the four create thunks only
   record the engine's properties and raise the variables. They hand back the same field handles. The revision lane makes the new
   versions, and adoption retargets the handles and writes the engine's size state.
4. **kFRAMEBUFFER is a declared CS slot in both modes.** Present copies it into the swap chain (guest: into DXVK's back buffer;
   native: into the swap chain image, as today). Native's own back-buffer slot (`MakeGraphBackBuffer`, `Rename`, `CurrentOf`)
   folds into the set.
5. **`Targets::Identify` is a hash map keyed by handle pointer, filled once.** Field handles never change identity after the
   first set, so the map is written once and read without a lock.
6. **The unbacked table moves into the set's declaration table and applies to both modes**, guarded by a fatal tap error on any
   GPU use (see 3.6 for why it is not a list of readers).

## 1. The guest handle retarget

### 1.1 What there is today

- A tripwire handle (`src/RenderGraph/Engine/Tripwire/TripwireHandles.h`) holds a `HandleCore` with
  `IUnknown* wrapped`, `std::shared_ptr<org::Resource> graph` and `owner`. Every forwarded method does
  `static_cast<Interface*>(core.wrapped)->Name Args` (the `TW_TRIP` macro). `ResourceHandle`, `ViewHandle` and `DescHandle` keep a
  `const Desc desc` and answer `GetDesc` from it in both modes.
- Guest: `EngineTargets.cpp`'s `Give(...)` wraps the DXVK texture and views that `GraphResources::ExportTexture` made through
  `dxvkCreateTextureFromVkImage`. `wrapped` is fixed for the handle's life.
- Native: the field handle wraps the native handle that `Native::WrapTexture` returned (a handle whose `owner` is a
  `NativeResource`). `Resolve` (NativeDevice.cpp ~2864) already follows `WrappedOf` for "a port's handle around a native one".
  `Rename`/`CurrentOf` (NativeDevice.cpp:481-496) exist only for the back buffer record.
- Unwrapping happens at call time. The guest context's forwarded calls unwrap each argument (`TW_UNWRAP`), so a swapped
  `wrapped` takes effect for every later call.

### 1.2 Decision: CS-side retarget

A DXVK-side retarget would have to swap a `D3D11CommonTexture`'s image, its cached D3D11 description and every
`D3D11View`'s `DxvkImageView`, including for a new extent. DXVK keeps the extent in `DxvkImageCreateInfo`, and its D3D11 views are
made once. That is a large fork change, and native mode would still need its own. The CS handle is already the proxy the plan's
second option asks for. So the retarget is a tripwire operation and DXVK is not touched.

### 1.3 The API (`Tripwire.h`)

```cpp
namespace RenderGraph::Engine::Tripwire
{
	/** @brief What a handle stands for after Retarget. a_wrap gets one reference. */
	template <class Desc>
	struct Target
	{
		IUnknown* wrap = nullptr;                     // DXVK's object (guest) or the native handle (native); null = unbacked
		Desc desc{};                                  // what GetDesc answers from now on
		std::shared_ptr<org::Resource> graph;         // what GraphResourceOf answers
		std::shared_ptr<const void> owner;            // kept alive with it
	};

	/**
	 * @brief Points a_handle at another object. Calls made after it forward to a_target.wrap. The old object, graph resource and
	 * owner go to the retire queue (ServiceRetired), so DXVK and the GPU can finish with them. Fatal for a handle that is not a
	 * resource or view handle, for a kind mismatch, and off the render owner. Only at the frame's adoption point.
	 */
	void Retarget(ID3D11Texture2D* a_handle, Target<D3D11_TEXTURE2D_DESC> a_target);
	void Retarget(ID3D11Texture3D* a_handle, Target<D3D11_TEXTURE3D_DESC> a_target);
	void Retarget(ID3D11ShaderResourceView* a_handle, Target<D3D11_SHADER_RESOURCE_VIEW_DESC> a_target);
	void Retarget(ID3D11RenderTargetView* a_handle, Target<D3D11_RENDER_TARGET_VIEW_DESC> a_target);
	void Retarget(ID3D11DepthStencilView* a_handle, Target<D3D11_DEPTH_STENCIL_VIEW_DESC> a_target);
	void Retarget(ID3D11UnorderedAccessView* a_handle, Target<D3D11_UNORDERED_ACCESS_VIEW_DESC> a_target);

	/** @brief A handle with nothing behind it yet: every call trips and fails harmlessly, GetDesc answers a_desc. */
	winrt::com_ptr<ID3D11Texture2D> CreateUnbackedTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, Options a_options);
	// ... and the view kinds, Texture3D; or the existing factories with a_wrap = null in guest mode too.

	/** @brief Whether the handle has nothing behind it. The guest context filter drops calls that pass one. */
	bool IsUnbacked(const void* a_object);
}
```

### 1.4 Changes inside the tripwire

- `TripwireHandles.h`, `HandleCore`: `IUnknown* wrapped` becomes `std::atomic<IUnknown*> wrapped`. `Enter`, `TW_TRIP`,
  `ViewHandle::GetResource`, `WrappedOf`, `UnwrapObject` load it with `memory_order_acquire`. `graph` and `owner` move behind
  the same swap: keep them in a small immutable `HandleState { std::shared_ptr<org::Resource> graph; std::shared_ptr<const void>
  owner; }` held by `std::atomic<const HandleState*>`. A retarget allocates a new state and queues the old one for
  `ServiceRetired`. Readers never keep a state across a frame boundary, so no reader sees a freed one.
- `ResourceHandle::desc`, `ViewHandle::desc`: from `const Desc` to two copies and an atomic index (a writer fills the spare,
  then flips it). `GetDesc` reads the current one. Only the render owner writes.
- `ViewHandle::resource` (the texture handle that `GetResource` answers) does not change: it is the field's texture handle,
  which is also stable.
- `Tripwire.cpp`: `Retarget` implementations, plus the retire queue entry (the old `wrapped` gets its `Release` in
  `ServiceRetired`, after `ReleaseAfterD3D11` says D3D11 is past it).
- `TripwireSelfTest.cpp`: a retarget case: two DXVK textures, a handle over the first, bind it, retarget, bind again, check
  `Unwrap`, `GetDesc` and `GraphResourceOf` give the second, and the first is released only after `ServiceRetired`.
- The guest context filter (`SetContextFilter`, used by the residue census) drops a call whose handle argument is unbacked and
  counts it. A draw, clear or copy that reaches an unbacked handle is `TapError::EngineDraw`-class: fatal in development builds.

### 1.5 The engine's bindings after a retarget

The engine's shadow state caches bound objects by pointer (`RendererShadowState::PSTexture[]`, `CSTexture[]`, `CSUAV[]`) and
skips a bind whose pointer did not change. After a retarget the pointer is the same, so DXVK (guest) or the native context would
keep the old object bound. At adoption, after the retargets, the set marks everything dirty:

```cpp
auto& s = globals::game::shadowState->GetRuntimeData();
s.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET, RE::BSGraphics::ShaderFlags::DIRTY_VIEWPORT);
s.PSResourceModifiedBits = 0xFFFF;
s.CSResourceModifiedBits = 0xFFFF;
s.CSUAVModifiedBits = 0xFF;
```

The engine's next flush binds the new objects. The old ones stay alive until `ServiceRetired`. **Decided by the agent, to
confirm:** dirty bits rather than `ClearState` on DXVK's context, because `ClearState` would also drop CS's own D3D11 bindings
and leave the engine's shadow state out of step. A fall-through engine draw between the adoption point and the next flush is
the only way to write an old version, and the census already flags fall-through draws.

## 2. Native wraps at adoption (Q2)

### 2.1 Where the time goes today

`GraphResources::Resource::AdoptPrepared` (GraphResources.cpp ~830-905) calls `RenderGraphRuntime::WrapTexture` and
`CreateTextureViews` at the adoption point. Native `WrapTexture` (NativeDevice.cpp ~4790-4870) calls
`NativeState::Initialize`, which runs an immediate submission and waits on it (`immediate.RunUntracked` then `immediate.Wait`,
NativeDevice.cpp ~1782-1797), then a second immediate `RunOp` to zero the image. Those two are the 19-75 ms (Vulkan) and
41-102 ms (D3D12) per twin. Guest `dxvkCreateTextureFromVkImage` (extern/dxvk `src/d3d11/d3d11_org_interop.cpp` ~180) only
queues the zero-fill on DXVK's initializer and calls `FlushInitCommands`. It is cheap and safe on any thread.

### 2.2 The change

1. **Wrap on the lane.** `Resource::Prepare(const PreparePlan&)` (lane thread) does what `AdoptPrepared` does now for an exported
   version: `WrapTexture`, the shape check, `CreateTextureViews`. `PreparedExport` gains `winrt::com_ptr<ID3D11Resource> d3d11`
   and the four view pointers. `AdoptPrepared` keeps its checks and only moves the pointers in. The `exportMs` log line then
   measures nothing and goes.
2. **No initialize, no wait in native.** `Native::WrapTexture` gets a flags argument:
   ```cpp
   enum class WrapFlags : std::uint32_t { None = 0, DeferInitialize = 1 };   // Native.h
   winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource&, const DxvkOrgInteropTextureWrap&, DxvkOrgInteropResourceInfo&,
       const char* a_name, const D3D11_SUBRESOURCE_DATA* a_data, WrapFlags a_flags);
   ```
   With `DeferInitialize` it fills the record (`FillImage`) and makes the handle, and it does not call `Initialize` or the zero
   `RunOp`. It is then CPU-only and safe on the lane (the registry has its lock). `RenderGraphRuntime::WrapTexture` passes it for
   every prepared version. The first set's wraps keep the old path; they happen before the first frame.
3. **First contents from the birth policy.** For a native exported version, `Prepare` sets `prepared.contents = Contents::Zero`
   and the graph description's `initialLayout = Undefined`. ORG's versioned-texture contents support (OMS
   `VersionedTextureGrowth`, which B5 also uses) clears the version in the first epoch that touches it. The immediate context's
   own use of a new version (an engine clear before any graph work) goes through the ledger (`LedgerResourceOf`), so it orders
   after that clear.

**Decided by the agent, to confirm:** native wrap records must be ledger-tracked (`Ownership::Tracked`) for item 3 to hold.
If `MakeWrapRecord` gives them another ownership, the first engine-side use would see an `Undefined` image. Lane T checks this
first and reports.

**Decided by the agent, to confirm:** OMS clears a version born `Undefined` on first use for exported twins as it does for
`History` ones. If it does not, lane T adds that to OMS on a branch (`ORGModuleServices`, branch `cleanup-p3-birth`).

## 3. `EngineTargetSet`

### 3.1 Files

- New: `src/RenderGraph/Engine/Targets/EngineTargetSet.h/.cpp` (the set, its declarations, adoption), and
  `src/RenderGraph/Engine/Targets/TargetRef.h` (B4's types, no ORG includes, so ports include it cheaply).
- Rewritten: `EngineTargets.cpp/.h` keeps the four detours and becomes the record-and-refill side.
- Deleted: `src/RenderGraph/Frame/EngineTargetUse.cpp/.h` (its table moves into the set, 3.6).

### 3.2 API

```cpp
namespace RenderGraph::Engine::Targets
{
	/** @brief The engine's properties of one target, as its create function got them. */
	struct Shape { std::uint32_t width, height, depthOrSlices, mips; DXGI_FORMAT format; std::uint32_t bindFlags; bool copy; bool cube; bool volume; };

	/** @brief Every target's recorded shape, the value of Vars::EngineTargetShapes. Compared by content. */
	struct ShapeTable { std::array<Shape, kColourSlots> colours; std::array<Shape, kDepthSlots> depths;
	                    std::array<Shape, kCubeSlots> cubes; std::array<Shape, kVolumeSlots> volumes; bool operator==(const ShapeTable&) const = default; };

	/** @brief Once, after the first BSShaderRenderTargets::Create, before State::Setup's graph work. Declares every target. Render thread. */
	void DeclareSet();

	/** @brief The declared texture's slot. Stable for the session, so ports compare nothing. Empty for an unbacked target (fatal in dev). */
	FrameAnchors::Texture Slot(TargetRef a_target);

	/** @brief The adopted description and view formats, for keys and pipeline setup. Reads the adopted revision only. */
	D3D11_TEXTURE2D_DESC Desc(TargetRef a_target);
	DXGI_FORMAT ViewFormat(TargetRef a_target);   // the RTV / DSV / SRV format the field's view uses (a_target.view says which)
}
```

`FrameAnchors::EngineTarget(index)`, `EngineDepth(index)` and `EngineCubemap(index)` keep their signatures and return
`Targets::Slot(...)`. They no longer import, and the kFRAMEBUFFER branch through `RTV->GetResource` (FrameAnchors.cpp:4196-4212)
goes. Every existing caller keeps working, and the slot it gets never changes.

### 3.3 Variables

- `Vars::DisplaySize()`: written only by the set (and once by the first set). `NativeDevice.cpp`'s `Resize` stops writing it
  (`SetDisplaySize(..., "native swap chain resize")`, ~2047).
- New `Vars::EngineTargetShapes()`, a `Var<ShapeTable>`, written at the end of every set. Every engine target's declaration
  depends on it (and on `DisplaySize` for kFRAMEBUFFER).
- `EngineShadowMap`, `ShadowCascades`, `ReflectionCubemap` keep their types. `RaiseTargetVariables` now reads them from the
  recorded shapes, not from `GetDesc` on the fields.
- `Vars::EngineTargets()` (the serial-only event) is deleted. Its readers depend on `EngineTargetShapes` instead.

**Decided by the agent, to confirm with the variables lead:** one table variable, not one variable per target. About 170
targets as separate variables would swamp the table. A 4 KB value compared by content once per set is cheap. This needs the B13
table to accept a non-trivial `T`, or a `std::shared_ptr<const ShapeTable>` compared by pointee.

### 3.4 The lifecycle

**First set (startup).** As today, the engine's first `BSShaderRenderTargets::Create` runs before the graph exists. The thunks
record each target's properties (`record.props`), and the engine makes its D3D11 objects. Then `globals::ReInit()` and
`State::Setup()` run. Inside `RenderGraph::Engine::Setup()`, `DeclareSet()` replaces `TakeOver()`. For each target it:

1. declares the texture (3.5) from the recorded shape and the plans (3.8);
2. makes the field handles once, over version 0 (unbacked ones over nothing), releasing the engine's D3D11 objects;
3. fills `Targets::Identify`'s map (section 4);
4. writes `EngineTargetShapes` and `DisplaySize`.

`InstallHandles` goes: the handles are made in step 2 for every target, and `UseHandles()`'s guest check moves to an assert
(the guest device and context exist before `Engine::Setup`).

**A later set (resize, window drag, display change).** The engine releases the target fields (`WindowSizeChanged`), resizes
the swap chain, and calls `Create` again, which calls the four create functions.

1. `BeginSet()` stops being fatal in graph mode. It records that a set is open.
2. Each create thunk records the new properties in the pending `ShapeTable` and writes the field's existing handles back into
   the field. A field the engine nulled gets the handle with `AddRef`. A field the engine left (the UAV, which
   `WindowSizeChanged` never releases) is left alone. Nothing is made or released. The thunk never calls the engine's function
   in graph mode after the first set.
3. `EndSet()`:
   - saves what the engine wrote into `BSGraphics::State` (`screenWidth`, `screenHeight`, `frameBufferViewport[2]`) as the
     wanted size;
   - writes the adopted values back, so the engine draws at the size of the targets it has until adoption;
   - sets `EngineTargetShapes` and `DisplaySize`. The revision this starts prepares every changed target's next version on the
     lane (wrap included, section 2).
4. `Hooks.cpp`'s `BSShaderRenderTargets_Create::thunk` does not call `globals::ReInit()`, `Vram::TagGameRenderTargets` or
   `globals::state->Setup()` after the first set in graph mode. `HDRDisplay::PreCreateRenderTargets` keeps the session's first
   `bUse64bitsHDRRenderTarget` value, so kMAIN's format cannot change (a format change would be a new shape class, which is
   fatal).
5. At the adoption point, after `Revision::AdoptAll` has adopted the twins, the set's adopted callback runs on the render owner:
   - `Tripwire::Retarget` for every field handle of every changed target, to the new version's export and views (made on the
     lane);
   - writes the wanted `BSGraphics::State` values;
   - marks the engine's bindings dirty (1.5).

Many sets before one adoption (a window drag) are fine: each `EndSet` overwrites the pending table, and the variable lane keeps
the latest value.

`Hooks.cpp`'s `WndProcHandler_Hook` size lock (`WM_WINDOWPOSCHANGING`, ~1094) and phase 0's `SWP_NOSIZE`-while-dragging fix are
skipped in graph modes. They stay in D3D11 mode, where `State::Setup` still runs again.

**Decided by the agent, to confirm:** for the one or two frames between a swap chain resize and the adoption, the present
copies the smaller extent (as native already does, `PresentThroughLedger`), so a border may show the old image. Holding the
swap chain resize until adoption would need the engine's `ResizeBuffers` call deferred, which is a second detour for a
two-frame artefact.

### 3.5 Each target and its declaration

All are `Frame::DeclareTexture2D` / `DeclareTexture3D` with `DependsOn(Vars::EngineTargetShapes())`, `Lifetime::Persistent`,
`versioned`, exported to D3D11 with the views the field needs. The description function reads the target's row of the table and
applies its plan (3.8). Only the extent may change between sets, so a format, mip or slice change is fatal, as `DeclareTexture`
already enforces.

| Engine target | Declaration name | Description | Field handles retargeted |
|---|---|---|---|
| kFRAMEBUFFER (colour 0) | `Engine::kFRAMEBUFFER` | `DisplaySize`, the swap chain's back-buffer format (`BackBufferFormat`), RTV + SRV | `texture` (now filled), `RTV`, `SRV` |
| kMAIN | `Engine::kMAIN` (+ `Engine::kMAIN Copy`) | recorded shape; format from `bUse64bitsHDRRenderTarget` as recorded | `texture`, `RTV`, `SRV`, `UAV`; `textureCopy`, `SRVCopy` |
| Other copyable colour targets | `Engine::<name>` + `Engine::<name> Copy` | recorded shape; the copy keeps the engine's format (HDR plan) | as kMAIN |
| HDR's LDR targets (`kLDR_DOWNSAMPLE0`, `kLDR_BLURSWAP`, `kBLURFULL_BUFFER`, `kIMAGESPACE_TEMP_COPY`, `kIMAGESPACE_TEMP_COPY2`, `kTEMPORAL_AA_UI_ACCUMULATION_1/2`) | `Engine::<name>` | recorded shape, format `R16G16B16A16_FLOAT` when HDR is loaded (the plan) | `texture`, `RTV`, `SRV`, `UAV` |
| Deferred's G-buffer and water (`kDeferredTargets` in Deferred.cpp: albedo, specular, reflectance, normal-roughness, masks, masks2, `kWATER_1/2`) | `Engine::<name>` | recorded extent, Deferred's format and `kDeferredTargetBinds` (the plan) | `texture`, `RTV`, `SRV`, `UAV` |
| Other colour targets the engine makes at a fraction or fixed size | `Engine::<name>` | recorded shape | `texture`, `RTV`, `SRV`, `UAV` |
| Mip views of another target (`props.mipLevel != -1`) | none of their own | views of mip `mipLevel` of the source target's version | `RTV`, `SRV`, `UAV` follow the source's retarget |
| Mip-generating targets (`props.allowMipGeneration`) | `Engine::<name>` | recorded shape with the full mip chain, `MISC_GENERATE_MIPS` | as colour; `GenerateMips` stays an engine call through the context |
| Depth: kMAIN, kMAIN_COPY, kPOST_ZPREPASS_COPY, kPOST_WATER_COPY, kDECAL_OCCLUSION, kCUBEMAP_REFLECTIONS, kBOOK_TEXT, kFOCUS_NEO | `Engine::Depth::<name>` | recorded shape, `FormatsOf(props)` | `texture`, `views[s]`, `readOnlyViews[s]`, `depthSRV`, `stencilSRV` |
| Shadow and occlusion depth: kSHADOWMAPS_ESRAM, kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM, kSHADOWMAPS, kPRECIPITATION_OCCLUSION_MAP | `Engine::Depth::<name>` | recorded shape with its recorded `arraySize` slices (up to 8) | as depth, one DSV pair per slice |
| Cubemap kREFLECTIONS | `Engine::Cubemap::kREFLECTIONS` | recorded shape, 6 slices, `TEXTURECUBE` | `texture`, `cubeSideRTV[0..5]`, `SRV` |
| 3D kVOLUMETRIC_LIGHTING_NOISE | `Engine::3D::VOLUMETRIC_LIGHTING_NOISE` | `iVolumetricLightingNoiseTextureWidth/Height/Depth:Display` (ORGVolumetricLighting.cpp `NoiseSize`), declared with the set | `texture`, `uav`, `srv`, `created = 1` |
| 3D kVOLUMETRIC_LIGHTING_ACCUMULATION, _COPY | unbacked (3.6), in both modes | the descriptor's size (`DescriptorSize`), recorded for `GetDesc` | `texture`, `uav`, `srv` over nothing |
| HDR UI texture | `HDR::UiTexture`, declared by ORGHDRDisplay as now (6.15) | `DisplaySize`, `R8G8B8A8_UNORM` | not an engine field; reached through `Engine::UiTargets` (3.7) |

- Mip views and mip-generating targets are kept on D3D11 today (`CanOwnColour`). Lane A logs which indices they are at the first
  set, so the doc's table can name them after the first run. **Decided by the agent, to confirm:** they join the set rather than
  stay engine objects, since an engine-owned target that changes at a resize would need its own retarget anyway.
- `MakeVolume` (mid-frame) goes. `Create3DRenderTarget::thunk` finds the volume declared with the set. The engine calls it when
  VL first renders: the thunk checks the arguments match the declaration (fatal if not) and fills the fields with the existing
  handles. `ORGVolumetricLighting::DeclareVanillaVolume` (:458, mid-frame) moves to setup, sized from `DescriptorSize()`.
  **Decided by the agent, to confirm:** the descriptor (SE 527948-adjacent table read by `DescriptorField`) is filled from the
  INI quality before the first frame. If not, the vanilla size is the INI quality's table entry read at setup.
- The skeleton's slot lending (Skylighting lends the precipitation occlusion slot, Skeleton.cpp:1222) is a field write by a
  feature. In graph mode it becomes a declared read of the lent target by id (lane M), and `DrawFor`'s "another view in its slot"
  branch becomes fatal.

### 3.6 Targets that are never made

Native mode keeps 15 colour targets and 2 volumes without memory (`Frame/EngineTargetUse.cpp`). The plan asks for "a target no
declaration reads is never made, in both modes". Engine-draw families and the skeleton bind engine targets by handle when they
record, so the set of readers cannot be listed at setup today. **Decided by the agent, to confirm:** the table moves into
`EngineTargetSet.cpp` as a per-target `Backing::None{reason}` in the declaration table and applies in both modes. Every other
target is declared and backed. A GPU use of an unbacked target (a graph import, a draw, clear or copy through the guest context)
is a fatal tap error in development builds in both modes. The guest census line counts what the filter drops. After phase 6 (no
D3D11 frames) the list can become a positive list of readers.

The engine's clears of unbacked targets (`dropClears` today) are dropped in both modes by the context filter.

### 3.7 kFRAMEBUFFER and `Engine::UiTargets` (B17)

- kFRAMEBUFFER is declared like any target. Its create thunk never calls the engine's function, so the engine never holds the
  swap chain's back buffer. The field's `texture` is filled too, which removes the `RTV->GetResource` lookups.
- Guest present: `FrameGen::DxvkPresenter::BeforePresent` (DxvkPresenter.cpp) copies the slot's current version (its D3D11
  export, unwrapped) into `swapChain->GetBuffer(0)` with the unwrapped immediate context's `CopyResource`, inside a
  `ScopedPerfEvent("CS present copy")`. DXVK's back buffers rotate storage every present when there is more than one
  (`RotateBackBuffers`, d3d11_swapchain.cpp), so the copy goes through DXVK's stream, not the graph.
- Native present: `NativeDevice.cpp`'s back buffer becomes the set's kFRAMEBUFFER. `MakeBackBuffer`, `MakeGraphBackBuffer`,
  `MakeGraphBackBufferTexture`, `UseGraphBackBuffer`, `PendGraphBackBuffer`, `DropGraphBackBuffer`, `graph.slot`,
  `backBufferRecord`, `Rename` and `CurrentOf` go. `QueuePresent` takes the slot's current version's record when it queues
  (where it calls `CurrentOf` today); `getBuffer` answers with the field's texture handle. **Decided by the agent, to confirm:**
  one back-buffer model for both modes; native's own slot is the same thing made earlier.
- `Engine::UiTargets` (new `src/RenderGraph/Engine/UiTargets.h/.cpp`):
  ```cpp
  struct UiTargets { FrameAnchors::Texture backBuffer, hdrUi, uiTarget; DXGI_FORMAT backBufferFormat, uiFormat; };
  constexpr Vars::Var<UiTargetsValue> UiTargetsVar() { return { "UiTargets" }; }   // HDR raises it on HdrEnabled
  UiTargets Current(const Frame::Inputs& a_in);   // uiTarget = hdrUi when HDR is on, else backBuffer
  ```
  HDR's field swap (`fb.RTV = uiTexture->rtv`, HDRDisplay.cpp:910-1015, 1271) does not run in graph mode. Scaleform, Bink and
  ImGui draw into `uiTarget` by id. Their "undo HDR's RTV swap" code (Passes.cpp:921-930, BinkGraph.cpp:447-453,
  ImGuiRenderer.cpp:134-178) and the four `"hdr.ui"` literals (ORGHDRDisplay.cpp:94, ImGuiRenderer.cpp:141, BinkGraph.cpp:100,
  Passes.cpp:1016) go.

### 3.8 HDR's and Deferred's target changes as plans

- `Targets::PlanTarget` stays and is the only way to change an engine target's shape: it is read by the declaration's description
  function. HDR's plans (HDRDisplay.cpp:696) and Deferred's (new: `kDeferredTargets` formats and `kDeferredTargetBinds`, called
  from `Deferred::PostPostLoad` before setup) are made before `DeclareSet`.
- `Targets::Recreate` and `Targets::Restore` are deleted. In graph mode `HDRDisplay::UpgradeLDRRenderTargets` /
  `RestoreLDRRenderTargets` (HDRDisplay.cpp:1528-1640) and `SetupRenderTarget` (Deferred.cpp:78) return at once (the plans
  already made them so). Their D3D11 branches stay for D3D11 mode.

### 3.9 What gets deleted in `EngineTargets.cpp`

`RetireToken`, `kRetireTokenKey`, `retired`, `pinned`, `Export` (replaced by `ExportForEngine`, 4.2), `EngineTexture::Adopt` /
`PendVersion` / `AdoptUnbacked` / `Drop`, `Forget`, `Republish`, `Serve`, `Move`, the `Make*` functions' make-and-give logic (the
view making moves into the set's prepare step), `TakeOver`, `InstallHandles`, `Recreate`, `Restore`, `Find`, `FindImage`,
`ServiceRetired`, `CanOwnColour`'s kFRAMEBUFFER exclusion.

## 4. `Targets::Identify` (B4) and the image registry (B15)

### 4.1 `Targets::Identify`

```cpp
namespace RenderGraph::Engine::Targets   // TargetRef.h
{
	enum class Kind : std::uint8_t { Colour, ColourCopy, Depth, Cubemap, Volume };
	enum class View : std::uint8_t { Texture, Rtv, Srv, Uav, Dsv, ReadOnlyDsv, StencilSrv, CubeFace };
	struct TargetRef
	{
		Kind kind; std::uint16_t index; View view = View::Texture; std::uint8_t slice = 0; std::uint8_t mip = 0;
		bool operator==(const TargetRef&) const = default;
	};

	std::optional<TargetRef> Identify(const ID3D11View* a_view);
	std::optional<TargetRef> Identify(const ID3D11Resource* a_resource);
}
```

- Backed by one `std::unordered_map<const void*, TargetRef>` keyed by field handle pointer, filled in `DeclareSet` and frozen.
  Field handles live for the session, so reads need no lock.
- A pointer that is not a field handle returns `nullopt`. A DXVK object (unwrapped) passed in is a caller bug: development builds
  check `Tripwire::IsHandle` and fail.
- It replaces:
  - ImageSpace `InputOfView`, `MipInputOfView`, `TargetOfUav` (ImageSpace.cpp:623-705) and their callers (:1600-1606,
    MenuSteps.inl:318);
  - Skeleton `FindColour`/`FindDepth` (Skeleton.cpp:450-480; callers :1776-1839) and `Targets::Find` at :1330;
  - Scaleform `EngineTargetOf`/`EngineDepthOf` (Passes.cpp:883-930; callers :1030-1044);
  - the engine-depth search loops in ORGPhysicalSky.cpp, ORGVolumetricClouds.cpp, ORGSkylighting.cpp,
    ORGExponentialHeightFog.cpp (their `depthStencils[` loops);
  - `Targets::Find`/`FindImage` (EngineTargets.cpp:1269-1295) and their callers FrameAnchors.cpp:1656 and :4281, DCLF
    FrameBindings.cpp:353 and :398, EngineDrawFamily.cpp:507.

### 4.2 One image registry and one export path (B15)

```cpp
namespace RenderGraph::ImageRegistry   // new src/RenderGraph/ImageRegistry.h/.cpp
{
	struct Entry
	{
		std::shared_ptr<org::Resource> resource;
		std::shared_ptr<org::VersionedTexture> slot;               // a versioned twin's slot, else null
		std::shared_ptr<const org::TextureDescription> views;
		std::string owner, name;
	};
	/** @brief Any thread. A second live entry for one image is fatal (the duplicate check, always on). */
	void Register(std::uint64_t a_image, Entry a_entry);
	std::optional<Entry> Find(std::uint64_t a_image);
	std::optional<Entry> Find(const IUnknown* a_d3d11);          // through dxvkGetInteropResourceInfo or the native record
	void Retire(const org::Resource* a_resource);
}

namespace GraphResources
{
	struct EngineExport { std::shared_ptr<org::ExternalTextureResource> graph; winrt::com_ptr<ID3D11Resource> d3d11; std::uint64_t image = 0; };
	/** @brief An ORG image with its D3D11 export. Any thread (the lane). Registers in ImageRegistry. */
	EngineExport ExportForEngine(const ExportDesc& a_desc, std::string a_name, Native::WrapFlags a_flags);
	/** @brief The one way to retire one: forget it in the registry, then ReleaseAfterD3D11. */
	void Retire(EngineExport&& a_export);

	/** @brief A stable D3D11 object over a declared twin's current export: a tripwire forwarding handle, retargeted at adoption. */
	template <class T> class D3D11Wrapper;   // T: ID3D11ShaderResourceView, ID3D11UnorderedAccessView, ID3D11Texture2D, ...
	template <class T> D3D11Wrapper<T> MakeWrapper(const Frame::DeclaredTexture2D& a_declared, View a_view);
}
```

- `ImageRegistry` replaces DxvkImageImport's `byImage`, `RegisterMadeImage` / `SetMadeImageSlot` / `ForgetMadeImage` /
  `MadeImageOf`, `NoteImageResource` / `RetireImageResource` / `ImageResources` (DxvkImageImport.cpp:163-330),
  `GraphResourceOfImage`; FrameAnchors' `imports` / `loose` / `FindImage` lookup by image (FrameAnchors.cpp:516-551, :1511);
  native `GraphTextureOf` / `GraphTextureOfImage` and `Registry::byImage` (NativeDevice.cpp ~446, ~4495-4510);
  `Targets::FindImage`. FrameAnchors keeps its id map (`imports` by id) for non-engine imports.
- `ExportForEngine` replaces `GraphResources::ExportTexture` and EngineTargets' `RetireToken` / GUID / `pinned`. EngineTextures'
  `AttachGraphResource` / `Abandon` and the DynamicRing tokens move to it in lane D, since they are separate files.
- `D3D11Wrapper<T>` is `Tripwire::Retarget` on a handle CS made. `Revision::AdoptAll` retargets every live wrapper after the
  twins adopt (the same callback as the engine fields). It replaces the hand-made wrappers in Cloud Shadows, HDR, Physical Sky,
  Volumetric Clouds (×3), Skylighting (×2), cloud maps (×2), `PhysSkyFollowTwin`, `CloudFollowTwin` with its `byD3D11` map, and
  HDR's `NewExport` compare. **Decided by the agent, to confirm:** the wrappers' migration is in lane M, but it can slip to
  phase 4 if lane M runs long; it needs only the primitive from lane T.

## 5. Migration of the observing sites

Each site below reads an engine target's identity, format or size each frame and calls `Invalidate()` on a change. After the
set lands, `FrameAnchors::EngineTarget/EngineDepth/...` return stable slots, and formats and sizes come from
`Targets::Desc` / `ViewFormat` in the key function (adopted values). So each site deletes its engine-target fields from the
observed struct, moves them into the key as `Inputs` reads (or drops them, for slots), and deletes the `Invalidate()` when no
other observed field is left. Fields that are not engine targets stay observed and go with B2 in phase 4.

| # | File:line | What it observes about engine targets | After phase 3 | Left for B2 |
|---|---|---|---|---|
| 1 | ORGPostProcessing.cpp:1210-1221 | kMAIN/kMAIN_COPY `Key()`, main and copy formats, extent | key reads `Targets::Desc(kMAIN)`; slots stable | `linear`; the count at :1263-1272 |
| 2 | ORG/ORGMotionBlur.cpp:100-108 | depth and motion `Key()` | slots stable: drop | none |
| 3 | ORG/ORGBorder.cpp:45-52 | depth and motion `Key()` | drop | none |
| 4 | ORGScreenSpaceShadows.cpp:426-434 | the z-prepass copy import | drop; its `Stop()` fallback goes with B7 | none |
| 5 | ORGSunRays.cpp:611-632, 963-967 | `main`, `mainFormat`, `depth` | key reads `ViewFormat(kMAIN rtv)` | shadow heights, cloud shadows |
| 6 | ORG/ORGFxRuntime.cpp:875-890, 2137-2140 | input/output/main/depth, their formats and sizes, `mainCopyable` | key reads `Desc` | compiled effects, `ready`; reload at :2100 |
| 7 | ORGExponentialHeightFog.cpp:178-192, 1200-1202 | `inputs[kDepth]` and the other engine `Source`s | key | `farEnabled`, `temporal` |
| 8 | ORGScreenSpaceGI.cpp:308-316, 872-876 | depth, main, normal-roughness, motion vectors | drop | `noise` (an immutable texture) |
| 9 | ORGSubsurfaceScattering.cpp:161-167, 526-531 | main, mask, albedo, normal, depth | drop; delete the "imported for the first time" path | none |
| 10 | ORGSkylighting.cpp:399-407 | occlusion depth, shadow cascades, ESRAM shadows | drop; the lent slot (3.5) becomes a declared read | none |
| 11 | ORGTerrainBlending.cpp:529-537 | main depth, terrain depth | drop for the engine depth | the terrain depth if it is CS's |
| 12 | ORGUpscaling.cpp:914-917, 1105-1113, 1201-1209 | depth (kMAIN vs copy), motion, hudless format and source view | depth choice from a variable; formats from `Desc` | the hudless source if it is not an engine target |
| 13 | ORGViewAtmosphere.cpp:156-163, 551-554 | `main`, `depth` | drop | `program`, `inputs`, `skyObserved` |
| 14 | ORGVolumetricClouds.cpp:385-394, 1405-1407 | `shadowMap` | drop | terrain, cloud layer, twins |
| 15 | ORGPhysicalSky.cpp:909-915, 960-967 | shadow inputs; composite `main`, `depth` | drop | clouds-on-graph |
| 16 | ORGDynamicCubemaps.cpp:998-1000 | `captureMain` = Deferred's `forwardRenderTargets[0]` | a constant Deferred sets at setup | none |
| 17 | ORGDeferredComposite.cpp:516-522 | `ImportTargets` | drop | none |
| 18 | ORGHDRDisplay.cpp:513-519 | the back buffer texture and serial | `UiTargets` / kFRAMEBUFFER slot | none |
| 19 | ORGScreenshot.cpp:147, 696-704 | kFRAMEBUFFER texture | slot stable: drop | the capture request at :395 |
| 20 | Menu/ImGuiRenderer.cpp:96, 134-178 | bound RTV via `OMGetRenderTargets`, its format | `UiTargets::Current().uiTarget` | none |
| 21 | Engine/Scaleform/Passes.cpp:874-878, 921-930, 1016, 1201-1209 | reads, surfaces, HDR's swapped RTV | `Identify` + `UiTargets` | tracked reads of CS textures |
| 22 | ORGVolumetricLighting.cpp:458-468, 489-497 | the vanilla volume declared mid-frame | declared at setup (3.5) | noise gradient, feature inputs |
| 23 | Engine/Bink/BinkGraph.cpp:100, 447-453 | HDR's swapped RTV, `"hdr.ui"` | `UiTargets` | none |

Not engine-target sites (phase 4, B2): ORG/ORGLensFlare.cpp (group counts), ORG/ORGColorGrading.cpp (tonemapper), ORG/ORGPhysicalGlare.cpp
(tear film), ORGCloudMaps.cpp:786, TruePBR/ORGGlintsNoise.cpp, the DCLF Tables/SceneSet/EntrySpaceTable/LightSelection/SkyDraws
`Invalidate`s, CSEditor.

### 5.1 DCLF

| Site | Today | After phase 3 |
|---|---|---|
| Features/MainView/MainViewPoints.cpp:23-71 (`CurrentTargetFormats`, `CaptureMainPass`) | formats from `OMGetRenderTargets` each opaque start, compared, `NoteInputChanged` on change | `TargetFormats` built once at DCLF setup from `Targets::ViewFormat` of Deferred's main-pass targets (new `Deferred::MainPassTargets()`, the ids `StartDeferred` binds, Deferred.cpp:342-364) and the kMAIN DSV. Development builds compare the bound formats at `BeforeOpaque` and fail on a mismatch. `CaptureMainPass` keeps only `CaptureMainPass()` / `CaptureEngineStates()` |
| Core/Gpu/Pipelines/DrawPipelines.cpp:317-340 (`SetTargetFormats`) | bumps `generation` and `stats.targetChanges` on a change | `InitTargetFormats(const TargetFormats&)`, once, before the pipeline lane starts; a second call is fatal |
| Core/Render/Payload/MaterialLookups.cpp:303-324 | shadow format latched from `depthStencils[...].views[0]->GetDesc` | `Targets::ViewFormat({Depth, kSHADOWMAPS_ESRAM, Dsv})`, else `kPRECIPITATION_OCCLUSION_MAP`'s, at setup; the `NoteInputChanged` branch goes |
| Core/Gpu/Pipelines/ShadowPipelines.cpp:99 (`SetShadowInputs`), :200 and DrawPipelines.cpp:408 (`RecreateShadowSet`), DrawPipelinesImpl.h:1016 | posted every frame; a format change recreates the shadow set | `InitShadowInputs(format, utility)` once at setup; `RecreateShadowSet` deleted |
| `targetsGeneration`: DrawPipelines.h:316, DrawPipelinesImpl.h:768, 792, DrawPipelines.cpp:589, DrawPipelinesRhi.h:57-61, 104, FamilyPipelines.cpp:259-283, MainPipelines.cpp:803-867, Revision/Producer.cpp:992-994, Snapshot/ViewBatchesInternal.h:325, Views/ViewEngine.cpp:422, 432 | every catalog read checks it | the parameter and field go; validity is `setVersion && layout` |
| `TapError::DclfInputChanged` for target formats, `IndirectDraws::NoteInputChanged` callers above, the target fields of `SameRevisionInputs` (Coverage.cpp:77-90) | | deleted for targets; main-resource remakes keep it until phase 5 |
| `Targets::Find` in Core/Extensions/FrameBindings.cpp:353, 398 and Core/Engine/Family/EngineDrawFamily.cpp:507 | linear search | `Targets::Identify` |
| `renderTargets[` / `depthStencils[` field reads (Sky, Precipitation, ShadowMask, BloodSplatter captures, Core/Render/Resources/Resources.cpp, Shadow/ShadowCapture.cpp, ShadowCapability.cpp, FirstPersonView.cpp, DecalOcclusionDraws.cpp, MainEpochs.cpp, MenuScenesDraws.cpp) | read handles by index; some call `GetDesc` for shape | handles stay valid (stable); `GetDesc` shape reads become `Targets::Desc` |

`DrawPipelines` built from declared formats at setup: `InitTargetFormats` and `InitShadowInputs` run in DCLF's setup after
`DeclareSet`, so the pipeline lane builds once and never sees a change.

### 5.2 The remaining field reads

There are 176 `renderTargets[` and 98 `depthStencils[` reads. The handles stay valid and stable, so most need no change. Two
kinds do:

- reads that call `GetDesc` for a size or format to size CS work: they read `Targets::Desc` (the adopted shape), or a declaration
  on `DisplaySize`;
- writes to a field in graph mode (HDR's swap, Skylighting's lending, Deferred's `SetupRenderTarget`): removed (3.5, 3.7, 3.8).

Lane M greps its files for `->GetDesc` after a field read and lists each changed site in its handoff.

## 6. Lanes, order and tests

### 6.1 Lanes (disjoint files)

| Lane | Scope | Files |
|---|---|---|
| **T: tripwire, wraps, native present** | 1.3-1.5, section 2, the native half of 3.7, the native `Registry::byImage` part of 4.2 | `src/RenderGraph/Engine/Tripwire/*`, `src/RenderGraph/GraphResources.cpp/.h`, `src/RenderGraph/Engine/Native/NativeDevice.cpp`, `Native.h`, `RenderGraphRuntime.cpp/.h` (`WrapTexture` signature), OMS branch if needed |
| **A: the set** | 3.1-3.6, 3.8, 4.1, 4.2's `ImageRegistry` and `ExportForEngine` | `Engine/Targets/*` (new `EngineTargetSet.*`, `TargetRef.h`), `Frame/EngineTargetUse.*` (delete), `ImageRegistry.*` (new), `DxvkImageImport.cpp/.h`, `FrameAnchors.cpp/.h`, `Frame/Variables.*`, `Hooks.cpp`, `Engine/Engine.cpp` (Setup order), `Deferred.cpp/.h`, `Features/HDRDisplay.cpp/.h` (plans, `PreCreateRenderTargets`, the graph-mode returns) |
| **P: present and UI targets** | the guest half of 3.7, B17 | `Features/Upscaling/DxvkPresenter.cpp/.h`, new `Engine/UiTargets.*`, `Features/HDRDisplay/ORGHDRDisplay.cpp/.h`, `Engine/Scaleform/Passes.cpp`, `Engine/Bink/BinkGraph.cpp`, `Menu/ImGuiRenderer.cpp`, `Features/Screenshot/ORGScreenshot.cpp`, `Testing/Commands.cpp` (the drag command), new test configs |
| **D: DCLF and engine modules** | 5.1, the Identify migrations in engine modules, EngineTextures and DynamicRing onto `ExportForEngine` | `src/Features/DrawcallLimitFix/**`, `Engine/ImageSpace/*`, `Engine/Skeleton/*`, `Engine/Textures/*`, the DynamicRing's file |
| **M: ports** | table rows 1-19 and 22 of section 5, the D3D11 wrappers (4.2) | `src/Features/**/ORG*.cpp/.h` (not HDR, Screenshot, DCLF), `src/ORGDeferredComposite.cpp`, the wrapper owners in Cloud Shadows, Physical Sky, Volumetric Clouds, Skylighting, cloud maps |

HDR's own files are split: lane A owns `Features/HDRDisplay.cpp` (the D3D11 feature), lane P owns `Features/HDRDisplay/ORGHDRDisplay.*`.
`Hooks.cpp` is lane A's only. If lane P needs a hook, it asks lane A.

### 6.2 Order

1. **T and A start together.** A codes against section 1.3's API as written. T lands first in the merge.
2. **P starts with A.** It needs `Targets::Slot` for kFRAMEBUFFER and the `UiTargets` shape from this doc only.
3. **D and M start when A's headers (`TargetRef.h`, `EngineTargetSet.h`) are pushed**, and merge after A. They can be written in
   parallel with A's implementation.
4. Merge order: T, A, P, D, M. Build after T+A, after P, after D+M.

### 6.3 Tests

After each merge, the director runs only what the merge touches:

| After | Configs (compose with modes) | What to read |
|---|---|---|
| T | `guest-quick`, `native-quick`, `native-d3d12-quick` with `CS_ORG_TRIPWIRE_SELFTEST=1`; `resize` in native-vulkan and native-d3d12 | the self-test's retarget case passes; the "adopted its prepared version" lines have no export time, and the revision lane log shows the wraps; no `[DCLF] frame N waited` lines from adoption |
| A (+T) | mode smoke in four modes, `menu`, `latch-census-short`; new `window-resize` in guest, native-vulkan, native-d3d12; `hdr-resize` in four modes; `resize`, `dynres`, `upscalers` in guest and native-vulkan | no tap errors; "[ORG] engine target set: adopted WxH" once per resize; shots at the new size; the census counts no draw to an unbacked target |
| P | `guest+hdr`, `guest+fg`, `screenshot-clean`, `bink`, `menu`, `hdr` in four modes | UI present in HDR and SDR; no `"hdr.ui"` import line; the guest present copy timer line |
| D | `guest+dclf-toggles`, `dclf-family`, `native-census`, `shader-reload`, `ibl` | no "Main pass targets changed" line; DCLF builds its pipelines once |
| M | `post-effects`, `post-lens`, `post-programs` (PP batch), `fog`, `ssgi`, `sss`, `terrain-shadows`, `volumetric-shadows`, `physical-sky`, `clouds`, `effects11`, `dynamic-cubemaps`, `upscalers` | no `Invalidate` line for engine targets; the shot set matches |
| all | new `window-drag` in guest and native-vulkan | the game lives; one adoption after the drag ends; no starvation error |

**New configs and command (lane P writes them):**

- `tools/cs-test/configs/window-resize.toml`: at `world-ready` + 60 a shot `start`; `window 1280x720`; 120 frames, shot `small`;
  `window 1920x1080`; 120 frames, shot `large`; exit. Expect shots `start`, `small`, `large`; `no_log` the tap error, "a frame
  revision failed" and "the engine is creating its render targets again". It needs the size lock off in graph modes, which lane A
  does. Today's `hdr-resize` sends the same `window` command, which the lock refuses, so it does not resize.
- `tools/cs-test/configs/window-drag.toml`: `window drag:1280x720:30` then `window drag:1920x1080:30`, a shot after each.
- `src/Testing/Commands.cpp`, `window drag:<W>x<H>:<frames>`: posts `WM_ENTERSIZEMOVE`, then over `<frames>` frames
  `SetWindowPos` steps from the current size to `<W>x<H>` (one per frame), then `WM_EXITSIZEMOVE`. That is the message order a
  real drag sends, so `Upscaling::s_windowModifying` is set as in a real drag.

## 7. The present copy (D-2): findings for A1

This section is written to be pasted into the plan's A1 "Investigate: the present copy". It answers the questions from the code.
The measurements are for the director to run.

**Why the copy is needed.**

- **Guest.** DXVK already copies once. `D3D11SwapChain::PresentImage` (extern/dxvk `src/d3d11/d3d11_swapchain.cpp` ~560-620)
  records `DxvkSwapchainBlitter::present`, a shader blit from DXVK's D3D11 back buffer into the Vulkan swap chain image. The blit
  also converts colour space, draws DXVK's HUD and scales. DXVK's presenter acquires that image in its own stream, so neither CS
  nor the engine can name it. DXVK's D3D11 back buffers also rotate storage at every present when there is more than one
  (`RotateBackBuffers`, `invalidateImage`), so even `GetBuffer(0)` is a different image each frame. Step 7 (kFRAMEBUFFER as a CS
  slot) adds a second copy: CS's slot into DXVK's back buffer, in DXVK's stream at Present.
- **Native Vulkan.** BasicRHI's `Swapchain::Present` presents and then acquires the next image (rhi.h:3345-3349; `AcquirePoint`,
  :3413). CS presents on the submission thread (`QueuePresent` → `QueueOnSubmitThread`, NativeDevice.cpp ~2150-2185) while the
  render thread records the next frame. So when a frame is recorded, its swap chain image is not known yet. Kept recordings
  replay across frames, and `MakeGraphBackBuffer`'s comment records what happened when the back buffer rotated versions every
  frame: "every prepared ticket stale and every kept recording unusable". Naming two or three swap chain images in recordings is
  the same problem. The copy (`PresentThroughLedger`, "native present tail") is already the late-bound step: it is recorded at
  present time through ORG's external client. Formats match by construction: the back buffer is made in the swap chain's format
  without sRGB (`BackBufferFormat`, NativeDevice.cpp ~2537), so the copy is a plain region copy.
- **Native D3D12.** The same as Vulkan. DXGI's back buffer index is known only after the previous present, which runs on the
  submission thread. Flip-model swap chains take no sRGB format (sRGB is an RTV view), which the copy already avoids.
- **Frame generation.** In guest, DLSS-G and FSR-FG replace DXVK's swap chain (Streamline's interposer; FSR-FG's replacement
  `vkQueuePresentKHR`, extern/Streamline `source/plugins/sl.fsr_g/fsrGEntry.cpp` ~950-1000, 1390-1400). They take the presented
  image and the hudless image, and FSR-FG requires the hudless format to equal the back buffer's (`fgBackBufferVkFormat`). In
  native, the presentation layer (`desc.presentationLayer`) owns the swap chain images. Neither path needs CS's copy, and neither
  stops CS from writing their images directly. The blocker is the recording, not frame generation.

**Can the last write target the acquired image?**

- **Guest: the added copy can go; DXVK's blit cannot.** A small DXVK fork change removes CS's copy: a new interop export
  (`dxvkSetPresentSource(IDXGISwapChain*, ID3D11Texture2D*)`, interop version 8) makes the next `PresentImage` blit from the
  given texture (the slot's current version's D3D11 export) instead of `m_backBuffers[0]` (`GetBackBufferView`). Nothing in
  any recording changes, and frame generation sees the same blit output. It is a late binding of the slot's version at present.
  Recommended once the copy's cost is measured, not before: the copy is simpler and D-2 accepted it.
- **Native: only for a single last pass, and only by recording it every frame.** The back buffer has several writers in a frame
  (the post chain's output, then Scaleform, Bink, ImGui, and HDR's output composite). They cannot all move out of the kept
  recordings. One narrow case is possible: with HDR on, HDR's output composite is the last full-screen write, and it could be a
  per-frame late-bound pass recorded at present against the acquired image. That needs the swap chain images to allow that pass's
  usage: a raster pass, since storage usage on Vulkan swap chain images depends on the surface's `supportedUsageFlags`. In SDR
  there is no such single pass, so the copy stays. Not worth doing unless the measurement below shows more than about 0.2 ms at
  4K.

**Expected cost.** It is a memory-bound copy. At 1080p RGBA8 (or R10G10B10A2) it moves about 16.6 MB, which takes about
0.08-0.1 ms on the RX 6600 (224 GB/s peak, about 70 percent reached). At 4K it moves about 66 MB, about 0.3-0.4 ms. It runs on
the graphics queue just before Present, so in a frame where the GPU is the bottleneck it adds about that much to the present's
latency as well.

**What the director should measure (RX 6600, 1080p and 4K, one run per mode and resolution).**

- **Guest:** after lane P, the copy is inside `ScopedPerfEvent("CS present copy")`. Run `guest-perf-taa` with
  `CS_GPU_EVENT_TIMERS=600`. Read the "CS present copy" line in the `[GpuEvents]` block between the `start` and `end` marks. DXVK's
  own blit is not covered by an event: read it from an RGP capture (the `DxvkSwapchainBlitter` draw in the present submission),
  using the RX 6600 RGP capture script.
- **Native Vulkan and D3D12:** today no timer covers the copy, because it is external work outside the graph's ranges. Lane T
  adds a GPU timestamp pair around `PresentThroughLedger`'s recorded copy, reported through `GpuTimingSource` as a graph range
  named "native present copy" when `CS_GPU_EVENT_TIMERS` is set. Run `native-perf-taa` and `native-d3d12-perf-taa` with
  `CS_GPU_EVENT_TIMERS=600`, and read the "native present copy" line. Cross-check it with one RGP capture.
- **Whole-frame effect:** lane T also adds `CS_ORG_PRESENT_COPY_SKIP=1` (development only), which keeps the barriers and skips
  the copy (the screen shows garbage, and the timing stays valid). Compare the `[Perf]` lines of `native-perf-taa` with and
  without it, and in guest after lane P (the same switch skips the guest `CopyResource`).
- **Resolution:** no cs-test key sets it. Set `iSize W` / `iSize H` in the profile's `SkyrimPrefs.ini` (1920x1080, then
  3840x2160, borderless) before each pair of runs.

**Conclusion to record.** The copy is needed in every mode because recordings name one back-buffer version and the presentable
image is known only after the previous present, on another thread (and in guest, DXVK owns the acquisition). The copy CS adds in
guest can be removed by binding the slot's version at present through DXVK. Native's copy stays unless a measurement makes the
HDR-only late pass worth it.

## 8. Needs from other lanes, and open items

**Needs from other lanes (running now):**

- Variables (B13): a `Var<ShapeTable>` or `Var<std::shared_ptr<const ShapeTable>>` compared by content; an adopted callback that
  runs after `Revision::AdoptAll` and before the frame's first stage, on the render owner. Lane A registers the set's retarget
  there. If `OnAdopted` runs per variable, lane A needs it ordered after the twins' adoption.
- Core (A6 thread checks): `Tripwire::Retarget` asserts the adoption point and the render owner with the same check.
- Ports (B7): the `Stop()` / "returns to D3D11" branches in the observing sites go with `PortFatal`. Lane M rebases on that work.

**Open items (decided by the agent, to confirm), in one place:**

1. CS-side retarget, no DXVK change (1.2).
2. Dirty bits after a retarget, not `ClearState` (1.5).
3. Native wrap records are ledger-tracked, so a deferred initialize is safe (2.2).
4. OMS clears an `Undefined`-born exported version on first use, or lane T adds it (2.2).
5. One `ShapeTable` variable, not one per target (3.3).
6. A border of old image for one or two frames after a resize (3.4).
7. Mip views and mip-generating targets join the set (3.5).
8. The vanilla VL volume size is readable at setup (3.5).
9. The unbacked list applies to both modes, guarded by a fatal tap error, until phase 6 (3.6).
10. One back-buffer model: native's slot folds into the set (3.7).
11. The D3D11 wrappers may slip to phase 4 (4.2).
12. The guest present-source override is a follow-up after measurement (7).

**Not covered here:** native `Resize`'s `HostWait(…, 2000)` (`WaitPresents`, NativeDevice.cpp ~1995) and `RecreateSwapchain` are
render-thread waits from the engine survey (item 11). They are the presenter-contract work in phase 6.
