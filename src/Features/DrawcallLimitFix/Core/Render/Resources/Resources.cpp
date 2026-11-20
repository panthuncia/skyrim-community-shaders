// GPU resources for the DCLF draws: the main, scene, shadow and view resources. All are created on the render thread.
// Each versioned buffer is registered with the scene's set, which grows it.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/RefTrace.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/BufferRefs.h"

#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/ScheduleDump.h"

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"

#include <cassert>
#include <mutex>
#include <unordered_map>

namespace DCLF::Draws
{
	namespace
	{
		/** @brief One desc's program load. The program is written before the state turns kLoaded. */
		struct ProgramLoad
		{
			static constexpr int kLoading = 0, kLoaded = 1, kFailed = 2;
			std::atomic<int> state{ kLoading };
			std::shared_ptr<const ComputeProgram> program;
		};

		/** @brief LoadProgram's entries by desc, for one device. A new device starts over. */
		struct ProgramLoads
		{
			std::mutex mutex;
			const void* device = nullptr;
			std::unordered_map<std::string, std::shared_ptr<ProgramLoad>> entries;
		};

		ProgramLoads& Loads()
		{
			static ProgramLoads loads;
			return loads;
		}

		/** @brief Everything in a desc that changes what Load builds. */
		std::string ProgramKey(const ComputeProgram::Desc& a_desc)
		{
			std::string key = a_desc.source + '|' + Util::WStringToString(a_desc.entry) + '|' + std::to_string(a_desc.constantWords) + (a_desc.commonDefines ? "|c" : "|n");
			for (const auto& [name, value] : a_desc.defines)
				key += '|' + Util::WStringToString(name) + '=' + Util::WStringToString(value);
			return key;
		}

		/** @brief A device-local buffer the shaders read by device address. */
		std::shared_ptr<org::Buffer> DeviceBuffer(std::uint64_t a_bytes, const char* a_name)
		{
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		}

		/** @brief A structured buffer the shaders read through its SRV. a_index receives the SRV's descriptor slot. */
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, std::uint32_t& a_index, bool a_unorderedAccess = false)
		{
			auto created = org::Buffer::CreateUnmaterializedStructuredBuffer(a_elements, a_stride, a_unorderedAccess);
			created->SetName(a_name);
			created->Materialize();
			a_index = created->GetSRVInfo(0).slot.index;
			return created;
		}

		/** @brief Gives a view with its own frame constants copies of the main frame buffers. They are remade only when the main shape changes. */
		void MatchViewFrameBuffers(ViewResources& a_view, const std::vector<FrameBuffer>& a_main)
		{
			if (!a_view.frameConstants)
				return;
			const auto held = a_view.frameBuffers.load(std::memory_order_acquire);
			if (held && held->size() == a_main.size() &&
				std::equal(a_main.begin(), a_main.end(), held->begin(), [](const auto& a, const auto& b) { return a.SameShape(b); }))
				return;
			auto copies = std::make_shared<std::vector<FrameBuffer>>(a_main);
			for (auto& frameBuffer : *copies) {
				std::uint32_t index = 0;
				frameBuffer.copy = StructuredBuffer(frameBuffer.elements, frameBuffer.stride,
					fmt::format("{}.frame-buffer.t{}", a_view.traits->prefix, frameBuffer.textureRegister).c_str(), index);
			}
			a_view.frameBuffers.store(std::move(copies), std::memory_order_release);
		}

		/** @brief A D3D11 view of a word buffer's first a_bytes, for readbacks. Words past it read as zero. */
		auto WrapWords(org::Buffer& a_buffer, std::uint64_t a_bytes)
		{
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = static_cast<UINT>(a_bytes);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
		}

		std::uint64_t AddressOf(rhi::Device a_device, org::Buffer& a_buffer)
		{
			// BasicRHI places whole buffers at 256 or more. The descriptor-buffer model's pointer loads assume at least 16.
			const auto address = a_device.GetBufferDeviceAddress({ a_buffer.GetAPIResource().GetHandle(), 0 });
			assert(address % 16 == 0);
			return address;
		}

		/** @brief A per-object buffer's first size. CS_DCLF_TABLE_START=small starts tiny to test growth. */
		std::uint32_t InitialObjectUnits()
		{
			return SwitchValue(Switch::TableStart) == "small" ? 64u : kInitialObjects;
		}
	}
}

namespace DCLF::Draws
{
	ShadowLatchLayout InitialShadowLatchLayout()
	{
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		return { smallSlots ? 2u : kInitialShadowViewSlots, smallSlots ? 16u : kInitialShadowKeySlots, smallSlots ? 1u : kInitialShadowRasterStates,
			smallSlots ? 1u : kInitialSunProcesses };
	}

	MainLayout InitialMainLayout()
	{
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		MainLayout layout;
		layout.sequenceDraws = smallTables ? 64u : kInitialSequenceDraws;
		layout.sequenceDecals = smallTables ? 16u : kInitialDecalDraws;
		return layout;
	}

	bool GrowableRows::Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name, org::services::GrowthPolicy a_policy, org::services::GrowableContents a_contents,
		org::services::GrowableCapture a_capture)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		stride = a_stride;
		name = a_name;
		buffer = MakeVersioned(DeviceBuffer(std::uint64_t(std::max(a_rows, 1u)) * stride, a_name),
			{ .extent = { 0, a_stride, 0 }, .policy = a_policy, .contents = a_contents, .capture = std::move(a_capture) });
		return AddressOf(host->Device(), *buffer->Get()) != 0;
	}

	namespace
	{
		/** @brief One object slot's share of the scene draw bound, and the pipeline slot whose Z-prepass bucket it counts toward.
		 * Decals count toward none, since the depth segment draws no decals. */
		std::pair<std::uint32_t, std::uint32_t> DrawShareOf(const SceneTables& a_tables, std::size_t a_slot, bool a_partitioned)
		{
			const auto& object = a_tables.objects[a_slot];
			if (object.flags & kObjectFree)
				return { 0u, DrawBoundStore::kNoPipeline };
			const std::uint32_t produced = PartitionDraws(a_partitioned ? a_tables.skinPartitions[a_slot] : 0u);
			return { produced, (object.flags & kObjectDecal) ? DrawBoundStore::kNoPipeline : object.pipelineIndex };
		}

		/** @brief What a_slot casts with, given its draws. */
		DrawBoundStore::ShadowShare ShadowShareOf(const SceneTables& a_tables, std::size_t a_slot, std::uint32_t a_produced)
		{
			DrawBoundStore::ShadowShare out;
			const auto& object = a_tables.objects[a_slot];
			if ((object.flags & kObjectFree) || !a_produced)
				return out;
			out.produced = a_produced;
			out.caster = !(object.flags & kObjectNoShadow);
			out.technique = a_slot < a_tables.shadowTechnique.size() ? a_tables.shadowTechnique[a_slot] : 0u;
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				out.occlusion[v] = a_slot < a_tables.occlusionTechnique[v].size() ? a_tables.occlusionTechnique[v][a_slot] : 0u;
			out.rasterFlags = (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u;
			out.vertexLayout = object.geometryIndex < a_tables.geometries.size() ? VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) : 0ull;
			return out;
		}

		/** @brief The caster rows a_share takes, its draws in each view family it has a key in. */
		std::uint64_t CasterRowsOf(const DrawBoundStore::ShadowShare& a_share)
		{
			ShadowPipelineKey key;
			std::uint64_t families = 0;
			bool caster = false;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				if (!a_share.KeyOf(m, key))
					continue;
				if (IsOcclusionMode(m))
					++families;
				else
					caster = true;
			}
			return std::uint64_t(a_share.produced) * (families + (caster ? 1u : 0u));
		}

		/** @brief Adds a_share's draws to its key in each mode it draws in, or takes them out. */
		void CountShadowShare(ModeKeyDraws& a_out, const DrawBoundStore::ShadowShare& a_share, bool a_add)
		{
			if (!a_share.produced)
				return;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				ShadowPipelineKey key;
				if (!a_share.KeyOf(m, key))
					continue;
				if (a_add) {
					a_out[m][key] += a_share.produced;
				} else if (const auto it = a_out[m].find(key); it != a_out[m].end() && (it->second -= a_share.produced) == 0) {
					a_out[m].erase(it);
				}
			}
		}

		/** @brief Adds shadow view slot buffers up to a_slots. Render thread only, since the passes declare them in a graph build. */
		void AddShadowViewSlots(ShadowResources& a_state, std::uint32_t a_slots, std::uint32_t a_draws, std::uint32_t a_words)
		{
			const std::uint32_t draws = a_draws;
			for (auto s = static_cast<std::uint32_t>(a_state.drawArgs.size()); s < a_slots; ++s) {
				// Every slot holds the scene's casting bound, so they grow a quarter past it rather than to a power of two.
				// A large list's interiors can use 32 slots.
				static_assert(kShadowDrawBytes * 3 / 2 <= kShadowDrawUnitBytes && kShadowDrawUnitBytes % 4 == 0);
				if (s < a_state.drawArgsOwner.size() && a_state.drawArgsOwner[s] < s)
					a_state.drawArgs.push_back(a_state.drawArgs[a_state.drawArgsOwner[s]]);
				else
					a_state.drawArgs.push_back(MakeVersioned(CreateWords(std::uint64_t(draws) * kShadowDrawUnitBytes / 4, true,
																  fmt::format("cs.dclf.shadow.draw-args{}", s).c_str()),
						{ .extent = { 0, kShadowDrawUnitBytes, 4 }, .policy = org::services::GrowthPolicy::Headroom(25, 64) }));
				a_state.count.push_back(CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str()));
				// One word per key slot, matching the latch layout.
				a_state.bucketCounts.push_back(MakeVersioned(CreateWords(std::max(a_words, 64u), true, fmt::format("cs.dclf.shadow.bucket-counts{}", s).c_str()),
					{ .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Headroom(0) }));
			}
		}

		/** @brief The pipeline statistics query pool and its per-range readbacks. None without the pool. */
		void CreatePassStats(Resources& a_state, rhi::Device a_device)
		{
			auto passStats = std::make_shared<PassStats>();
			const std::uint32_t slots = PassStats::kRanges;
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::PipelineStatistics;
			desc.count = slots * PassStats::kKinds;
			desc.statsMask = rhi::PipelineStatBits::PS_IAVertices | rhi::PipelineStatBits::PS_IAPrimitives | rhi::PipelineStatBits::PS_VSInvocations |
			                 rhi::PipelineStatBits::PS_PSInvocations;
			desc.requireAllStats = true;
			if (a_device.CreateQueryPool(desc, passStats->pool) != rhi::Result::Ok || !passStats->pool) {
				logger::warn("[DCLF] pass statistics: no pipeline statistics query pool");
			} else {
				for (std::uint32_t i = 0; i < slots; ++i) {
					passStats->readback.push_back(org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(PassStats::kKinds) * PassStats::kFields * sizeof(std::uint64_t)));
					passStats->readback.back()->SetName("cs.dclf.pass-stats-readback");
				}
				passStats->written.resize(slots);
				a_state.passStats = std::move(passStats);
				logger::info("[DCLF] pass statistics on: {} run ranges", slots);
			}
		}

		/** @brief The foliage pixel record at the target's size. */
		void CreateFoliageRecord(Resources& a_state, [[maybe_unused]] org::GraphHost& a_host, rhi::Device a_device, const D3D11_TEXTURE2D_DESC& a_target)
		{
			auto foliage = std::make_shared<Resources::FoliageRecord>();
			foliage->width = a_target.Width;
			foliage->height = a_target.Height;
			const std::uint64_t pixels = std::uint64_t(a_target.Width) * a_target.Height;
			for (std::uint32_t h = 0; h < 2; ++h) {
				foliage->ids[h] = CreateWords(pixels, true, fmt::format("cs.dclf.foliage-ids{}", h).c_str());
				foliage->colours[h] = CreateWords(pixels * kFoliageColourWords, true, fmt::format("cs.dclf.foliage-colours{}", h).c_str());
				foliage->idsRef[h] = RenderGraph::DeclaredRefOf(a_device, *foliage->ids[h]);
				foliage->coloursRef[h] = RenderGraph::DeclaredRefOf(a_device, *foliage->colours[h]);
			}
			foliage->results = CreateWords(kFoliageResultWords, true, "cs.dclf.foliage-results");
			foliage->owners = CreateWords(pixels * 2, true, "cs.dclf.foliage-owners");
			foliage->ownersIndex = foliage->owners->GetUAVShaderVisibleInfo(0).slot.index;
			foliage->resultsRef = RenderGraph::DeclaredRefOf(a_device, *foliage->results);
			// The record only stands if the observers' compare pass accepts it.
			bool ready = false;
			// One readback range per run lease index the colour epoch records at.
			Diagnostics::Notify(Diagnostics::FoliageRecordMade{ *foliage, a_device, 16u, ready });
			if (ready)
				a_state.foliage = std::move(foliage);
		}

		/** @brief Imports the engine's refraction normals (kREFRACTION_NORMALS), bound as RT0 for group 5 by FUN_1414b4860.
		 * Without them DCLF draws no refraction. */
		void ImportRefractionNormals(Resources& a_state, rhi::Device a_device)
		{
			if (auto* renderer = globals::game::renderer) {
				const auto& normals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
				DxvkOrgInteropResourceInfo normalsInfo{};
				if (normals.texture && normals.RTV && RenderGraphRuntime::Get().DescribeResource(normals.texture, normalsInfo) && normalsInfo.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
					D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
					normals.RTV->GetDesc(&rtvDesc);
					D3D11_TEXTURE2D_DESC textureDesc{};
					normals.texture->GetDesc(&textureDesc);
					org::TextureDescription normalsDesc{};
					normalsDesc.imageDimensions.push_back({ textureDesc.Width, textureDesc.Height, 0, 0 });
					normalsDesc.format = rhi::helpers::ToRHI(rtvDesc.Format);
					normalsDesc.channels = 4;
					normalsDesc.hasRTV = true;
					normalsDesc.rtvFormat = normalsDesc.format;
					a_state.refractionNormals = ImportImage(a_device, normals.texture, normalsInfo.image, normalsDesc, "DCLF refraction normals");
					a_state.refractionFormat = a_state.refractionNormals ? rtvDesc.Format : DXGI_FORMAT_UNKNOWN;
				}
				if (!a_state.refractionNormals)
					logger::warn("[DCLF] the refraction normals (kREFRACTION_NORMALS) could not be imported; DCLF draws no refraction");
			}
		}

		/** @brief Imports the engine's screen-space shadow mask (kSHADOW_MASK), which the colour draws read as t14.
		 * The engine draws it whole, DCLF's pixels included. */
		void ImportShadowMask(Resources& a_state, rhi::Device a_device)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return;
			const auto& mask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
			DxvkOrgInteropResourceInfo info{};
			D3D11_TEXTURE2D_DESC textureDesc{};
			if (mask.texture)
				mask.texture->GetDesc(&textureDesc);
			if (!mask.texture || textureDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM || !RenderGraphRuntime::Get().DescribeResource(mask.texture, info) ||
				info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::warn("[DCLF] the shadow mask (kSHADOW_MASK) is not an RGBA8 image (format {}); the colour pass does not declare it", static_cast<int>(textureDesc.Format));
				return;
			}
			org::TextureDescription maskDesc{};
			maskDesc.imageDimensions.push_back({ textureDesc.Width, textureDesc.Height, 0, 0 });
			maskDesc.format = rhi::Format::R8G8B8A8_UNorm;
			maskDesc.channels = 4;
			// Declared so there is a barrier after the engine's mask draws.
			maskDesc.hasSRV = true;
			maskDesc.srvFormat = maskDesc.format;
			a_state.shadowMask = ImportImage(a_device, mask.texture, info.image, maskDesc, "DCLF shadow mask");
		}

		/** @brief The HZB, from the depth pyramid's producer. Without one, culling stays frustum-only with no phase 2 passes. */
		void SetUpHzb(Resources& a_state, rhi::Device a_device)
		{
			Api::DepthPyramid pyramid{};
			a_state.hzbState = Extensions::SetUpDepthPyramid(a_device, a_state.width, a_state.height, pyramid);
			a_state.hzb = std::move(pyramid.texture);
			a_state.hzbWidth = pyramid.width;
			a_state.hzbHeight = pyramid.height;
			a_state.hzbMips = pyramid.mips;
			a_state.hzbScratch = std::move(pyramid.scratch);
		}
	}
}

namespace DCLF
{
	bool IndirectDraws::Impl::Setup(const Capture& a_capture, bool a_depthOnly)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return NotReady(0, "no render graph");
		if (a_depthOnly) {
			// The native depth pass binds no colour targets, so the Z-prepass reuses the main pass's resources.
			return resources != nullptr;
		}
		if (a_capture.targetCount == 0)
			return NotReady(1, "no render targets bound");
		D3D11_TEXTURE2D_DESC target{};
		a_capture.targets[0]->GetDesc(&target);
		const auto& targets = DrawPipelines::Get().Targets();
		// One bit per frame graph buffer registered on the graph.
		std::uint32_t graphBuffers = 0;
		for (std::size_t b = 0; b < Extensions::RegisteredFrameGraphBuffers().size(); ++b) {
			std::array<std::uint32_t, Extensions::kFrameGraphBufferRegisters> indices;
			if (Extensions::FrameGraphBufferIndices(b, indices))
				graphBuffers |= 1u << b;
		}
		auto frameBuffers = FrameBuffersOf(a_capture, graphBuffers);
		const bool sameFrameBuffers = resources && resources->frameBuffers.size() == frameBuffers.size() &&
		                              std::equal(frameBuffers.begin(), frameBuffers.end(), resources->frameBuffers.begin(), [](const auto& a, const auto& b) { return a.SameShape(b); });
		if (resources && targets == formats && target.Width == width && target.Height == height && sameFrameBuffers &&
			resources->graphBuffers == graphBuffers)
			return true;
		// Once DCLF draws, its graphs and snapshot hold these resources and no revision carries new ones (plan step 10a-0).
		// The colour epoch is skipped instead.
		if (resources && !Scene::FrameView::Get().NotDrawingYet()) {
			std::string what;
			if (!(targets == formats))
				what += "the target formats, ";
			if (target.Width != width || target.Height != height)
				what += fmt::format("the size {}x{} -> {}x{}, ", width, height, target.Width, target.Height);
			if (!sameFrameBuffers)
				what += fmt::format("the CPU structured buffers at t16 and up ({} -> {}), ", resources->frameBuffers.size(), frameBuffers.size());
			if (resources->graphBuffers != graphBuffers)
				what += fmt::format("the frame graph buffers with indices {:#x} -> {:#x}, ", resources->graphBuffers, graphBuffers);
			IndirectDraws::NoteInputChanged(fmt::format("the main-pass graph resources would be made again after DCLF drew; changed: {}", what.substr(0, what.size() - 2)));
			return false;
		}

		auto device = host->Device();
		auto state = std::make_shared<Resources>();
		// Targets first, so a setup retried each frame until they import does not remake its buffers and programs.
		if (!ImportMainTargets(*state, a_capture, device, target, targets))
			return false;
		if (!EnsureSceneBuffers(device)) {
			logger::error("[DCLF] The face positions buffer has no device address");
			return false;
		}
		state->scene = scene;
		if (!CreateMainBuffers(*state, *host, device))
			return false;

		state->width = target.Width;
		state->height = target.Height;
		if (Policy::RecordFoliagePixels())
			CreateFoliageRecord(*state, *host, device, target);

		ImportRefractionNormals(*state, device);
		ImportShadowMask(*state, device);
		state->shadow = shadowEngine->shadow;

		SetUpHzb(*state, device);

		state->graphBuffers = graphBuffers;
		for (auto& frameBuffer : frameBuffers) {
			frameBuffer.copy = org::Buffer::CreateUnmaterializedStructuredBuffer(frameBuffer.elements, frameBuffer.stride, false);
			frameBuffer.copy->SetName(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister));
			frameBuffer.copy->Materialize();
		}
		state->frameBuffers = std::move(frameBuffers);

		resources = state;
		formats = targets;
		width = target.Width;
		height = target.Height;
		// The views draw from the main resources, so they are rebuilt at their next import, before the graph is built.
		for (const auto& slot : kViewSlots)
			if (const auto& view = View(slot.view).resources) {
				view->main = state;
				view->rebuildPending = true;
				MatchViewFrameBuffers(*view, state->frameBuffers);
				state->views[ViewIndex(slot.view)] = view;
			}
		PostRevisionInputs();
		// The main epochs' graphs pick up the new resources at their next build.
		host->AddExtension(kExtensionId, [] { return Diagnostics::ScheduleDumped(kExtensionId, MakeMainOpaqueExtension(BuildingState(kExtensionId))); }, ExtensionEpochs(Api::EpochShape::Main));
		logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
		return true;
	}

	bool IndirectDraws::Impl::CreateMainBuffers(Resources& a_state, [[maybe_unused]] org::GraphHost& a_host, rhi::Device a_device)
	{
		// Programs compile off the render thread. Until they finish nothing is made and DCLF stays off.
		// Every BuildDraws variant is requested here, so they compile together and the other setups find them ready.
		bool programsPending = false;
		auto load = [&](const ComputeProgram::Desc& a_desc) {
			bool loading = false;
			auto program = LoadProgram(a_device, a_desc, loading);
			programsPending = programsPending || loading;
			return program;
		};
		auto buildDraws = load({ .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords, .defines = ::DCLF::Diagnostics::RefTrace::ComputeDefines(2) });
		auto clearPortalQueries = load({ .source = kBuildDrawsShader, .entry = L"ClearPortalQueries", .constantWords = kBuildDrawsConstantWords });
		auto preparePortalQueries = load({ .source = kBuildDrawsShader, .entry = L"PreparePortalQueries", .constantWords = kBuildDrawsConstantWords });
		auto forwardSort = load({ .source = kForwardSortShader, .constantWords = kForwardSortConstantWords });
		(void)load({ .source = kBuildDrawsShader, .entry = L"ShadowCasterRows", .constantWords = kBuildDrawsConstantWords });
		(void)load({ .source = kBuildDrawsShader, .entry = L"ShadowViews", .constantWords = kBuildDrawsConstantWords });
		(void)load({ .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords });
		// Compiled ahead so DrawSort::Create finds it cached.
		(void)load({ .source = kSortSequencesShader, .constantWords = kSortSequencesConstantWords });
		if (programsPending)
			return NotReady(8, "its compute programs are compiling off the render thread");
		// The sequence ranges start at the first layout's sizes. RequireMain grows them to the scene's draw bound.
		const bool smallTables = SwitchValue(Switch::TableStart) == "small";
		const MainLayout firstLayout = InitialMainLayout();
		a_state.sequences = MakeVersioned(CreateWords(SequenceSlots(firstLayout.sequenceDraws, firstLayout.sequenceDecals, firstLayout.decalRanges) * sizeof(DrawSequence) / 4,
											  true, "cs.dclf.sequences"),
			{ .extent = { 0, sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
		a_state.count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
		a_state.classCounts = MakeVersioned(CreateWords(64, true, "cs.dclf.class-counts"), { .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Doubling(64) });
		// The depth segment's culling verdict, one word per object, so the colour segment draws exactly the same set.
		const std::uint32_t objects = InitialObjectUnits();
		a_state.visibility = MakeVersioned(CreateWords(objects, true, "cs.dclf.visibility"), { .extent = { 0, 4, 4 } });
		a_state.frustum = MakeVersioned(CreateWords(objects, true, "cs.dclf.frustum"), { .extent = { 0, 4, 4 } });
		// Phase 1's retest list, one word per object slot.
		a_state.retests = MakeVersioned(CreateWords(objects, true, "cs.dclf.retests"), { .extent = { 0, 4, 4 } });
		if (SwitchEnabled(Switch::SetParity))
			a_state.writers = MakeVersioned(CreateWords(objects * 8, true, "cs.dclf.writers"), { .extent = { 0, 32, 4 } });
		a_state.retestDispatch = CreateWords(4, true, "cs.dclf.retest-dispatch");
		if (PassStats::Enabled())
			CreatePassStats(a_state, a_device);
		a_state.preprocessMain = PreprocessStates::Create(a_device, "the main segment");
		// The forward epoch records in its own epoch, so it needs its own state lists.
		a_state.preprocessForward = PreprocessStates::Create(a_device, "the forward epoch");
		a_state.buildDraws = std::move(buildDraws);
		if (!a_state.buildDraws)
			return NotReady(7, "the BuildDraws program could not be created");
		a_state.clearPortalQueries = std::move(clearPortalQueries);
		a_state.preparePortalQueries = std::move(preparePortalQueries);
		if (!a_state.clearPortalQueries || !a_state.preparePortalQueries)
			return NotReady(7, "the main member portal query programs could not be created");
		a_state.portalQuerySignature = CreateDispatchSignature(a_device, a_state.preparePortalQueries->layout->GetHandle());
		if (!a_state.portalQuerySignature)
			return NotReady(7, "the main member portal query signature could not be created");

		// Without the forward sort the forward epoch draws nothing.
		a_state.forwardSort = std::move(forwardSort);
		if (!a_state.forwardSort)
			logger::warn("[DCLF] the forward sort program could not be created: the forward epoch draws nothing");
		a_state.dispatchSignature = CreateDispatchSignature(a_device, a_state.buildDraws->layout->GetHandle());
		if (!a_state.dispatchSignature)
			return NotReady(7, "the BuildDraws dispatch signature could not be created");
		// BuildDraws appends in thread order, so without a sort by pipeline nearly every sequence would switch pipeline.
		a_state.sort = DrawSort::Create(a_device, firstLayout.sequenceDraws);
		const GrowableSpec bucketWords{ .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Doubling(64) };
		a_state.zBucketCounts[0] = MakeVersioned(CreateWords(64, true, "cs.dclf.z.bucket-counts"), bucketWords);
		a_state.zBucketCounts[1] = MakeVersioned(CreateWords(64, true, "cs.dclf.z.bucket-counts2"), bucketWords);
		a_state.pool = EnsureIndexPool(*scene, a_device, smallTables);
		if (!a_state.pool)
			return NotReady(7, "the index pool's copy program could not be created");
		// The view covers every count word. A short view would read zeros that look clean.
		a_state.countD3D11 = WrapWords(*a_state.count, kCountWords * sizeof(std::uint32_t));
		a_state.frameConstants = DeviceBuffer(kFrameConstantBytes, "cs.dclf.frame-constants");
		a_state.frameConstantsAddress = AddressOf(a_device, *a_state.frameConstants);
		a_state.frameConstantsRef = RenderGraph::DeclaredRefOf(a_device, *a_state.frameConstants);

		if (!a_state.frameConstantsAddress || !a_state.frameConstantsRef) {
			logger::error("[DCLF] Draw data buffers have no device address or buffer view ref");
			return false;
		}
		a_state.frameBlocks = FrameBlockViews::Create(a_device, a_state.frameConstants);
		if (!a_state.frameBlocks)
			return false;
		return true;
	}

	bool IndirectDraws::Impl::ImportMainTargets(Resources& a_state, const Capture& a_capture, rhi::Device a_device, const D3D11_TEXTURE2D_DESC& a_target,
		const TargetFormats& a_targets)
	{
		a_state.targetCount = a_targets.colorCount;
		for (std::uint32_t i = 0; i < a_targets.colorCount; ++i) {
			DxvkOrgInteropResourceInfo info{};
			if (!a_capture.targets[i] || !RenderGraphRuntime::Get().DescribeResource(a_capture.targets[i].get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(5, "a main-pass target cannot be described");
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(a_targets.colors[i]);
			desc.channels = 4;
			desc.hasRTV = true;
			desc.rtvFormat = desc.format;
			a_state.native[i] = ImportImage(a_device, a_capture.targets[i].get(), info.image, desc, "DCLF native target");
			if (!a_state.native[i])
				return NotReady(6, "a main-pass target could not be imported");
		}
		DxvkOrgInteropResourceInfo info{};
		if (!a_capture.depth || !RenderGraphRuntime::Get().DescribeResource(a_capture.depth.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return NotReady(10, "the main-pass depth cannot be described");
		// One import that is both attachable and readable. Two imports would be unrelated to the graph, so there would be
		// no barrier between the depth draws and the HZB read.
		org::TextureDescription depthDesc{};
		depthDesc.imageDimensions.push_back({ a_target.Width, a_target.Height, 0, 0 });
		depthDesc.format = rhi::helpers::ToRHI(a_targets.depth);
		depthDesc.channels = 1;
		depthDesc.hasDSV = true;
		depthDesc.dsvFormat = depthDesc.format;
		depthDesc.hasSRV = true;
		depthDesc.srvFormat = DepthReadFormat(depthDesc.format);
		a_state.nativeDepth = ImportImage(a_device, a_capture.depth.get(), info.image, depthDesc, "DCLF native depth");
		if (!a_state.nativeDepth)
			return NotReady(11, "the main-pass depth could not be imported");
		// The near twin target (Terrain Blending's terrain depth) shares the main depth's description. A graph-owned twin
		// is declared as itself, so its clear and the blend are ordered with these draws.
		const auto& nearTwin = Extensions::NearTwin();
		if (auto owned = nearTwin.graph ? nearTwin.graph() : nullptr) {
			a_state.terrainDepth = std::move(owned);
		} else if (auto* const twinTarget = static_cast<ID3D11Texture2D*>(nearTwin.target ? nearTwin.target() : nullptr)) {
			DxvkOrgInteropResourceInfo terrainInfo{};
			if (!RenderGraphRuntime::Get().DescribeResource(twinTarget, terrainInfo) || terrainInfo.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return NotReady(12, nearTwin.cannotDescribe.c_str());
			a_state.terrainDepth = ImportImage(a_device, twinTarget, terrainInfo.image, depthDesc, nearTwin.debugName.c_str());
			if (!a_state.terrainDepth)
				return NotReady(12, nearTwin.cannotImport.c_str());
		}
		return true;
	}

	ScannedDrawBound Draws::ScanDrawBound(const SceneTables& a_tables)
	{
		ScannedDrawBound out;
		std::uint64_t draws = 0;
		const std::size_t objects = a_tables.objects.size();
		const bool partitioned = a_tables.skinPartitions.size() >= objects;
		out.perPipeline.assign(a_tables.pipelines.size(), 0u);
		for (std::size_t o = 0; o < objects; ++o) {
			const auto [produced, pipeline] = DrawShareOf(a_tables, o, partitioned);
			draws += produced;
			if (pipeline < out.perPipeline.size())
				out.perPipeline[pipeline] += produced;
			CountShadowShare(out.modeKeys, ShadowShareOf(a_tables, o, produced), true);
		}
		out.draws = static_cast<std::uint32_t>(std::min<std::uint64_t>(draws, UINT32_MAX));
		return out;
	}

	void ImplProducer::UpdateDrawBound(const SceneTables& a_tables, std::uint32_t a_generation)
	{
		auto& s = producer.drawBound;
		const std::size_t count = a_tables.objects.size();
		const bool partitioned = a_tables.skinPartitions.size() >= count;
		const std::uint32_t generation = a_generation;
		constexpr auto kNoPipeline = DrawBoundStore::kNoPipeline;
		auto set = [&](std::size_t a_slot) {
			const auto [produced, pipeline] = DrawShareOf(a_tables, a_slot, partitioned);
			// Most logged changes, such as placement or shading, leave the casting share unchanged.
			if (auto share = ShadowShareOf(a_tables, a_slot, produced); !(share == s.shadow[a_slot])) {
				CountShadowShare(s.modeKeyDraws, s.shadow[a_slot], false);
				CountShadowShare(s.modeKeyDraws, share, true);
				s.casterRows += CasterRowsOf(share) - CasterRowsOf(s.shadow[a_slot]);
				s.shadow[a_slot] = share;
				++s.shadowVersion;
			}
			s.draws -= s.produced[a_slot];
			if (s.pipeline[a_slot] != kNoPipeline)
				s.perPipeline[s.pipeline[a_slot]] -= s.produced[a_slot];
			s.produced[a_slot] = produced;
			s.pipeline[a_slot] = pipeline;
			s.draws += produced;
			if (pipeline != kNoPipeline) {
				if (pipeline >= s.perPipeline.size())
					s.perPipeline.resize(std::size_t(pipeline) + 1, 0u);
				s.perPipeline[pipeline] += produced;
			}
		};
		++s.updates;
		// Recount every slot on new tables, a lost log, fewer slots, or the partition column coming or going, which is not logged.
		if (!s.cursor.Continues(a_tables.changeLog, generation) || s.produced.size() > count || s.partitioned != partitioned) {
			s.produced.assign(count, 0u);
			s.pipeline.assign(count, kNoPipeline);
			s.perPipeline.clear();
			s.shadow.assign(count, {});
			for (auto& keys : s.modeKeyDraws)
				keys.clear();
			s.casterRows = 0;
			s.draws = 0;
			s.partitioned = partitioned;
			for (std::size_t o = 0; o < count; ++o)
				set(o);
			s.cursor.Restart(generation);
			++s.resyncs;
			++s.shadowVersion;
		} else {
			if (const std::size_t first = s.produced.size(); first < count) {
				s.produced.resize(count, 0u);
				s.pipeline.resize(count, kNoPipeline);
				s.shadow.resize(count);
				for (std::size_t o = first; o < count; ++o)
					set(o);
			}
			// Recounting an unchanged share costs nothing, so any change kind triggers it.
			for (const auto& change : s.cursor.Unread(a_tables.changeLog)) {
				if (change.slot >= count)
					continue;
				set(change.slot);
				++s.changes;
			}
		}
		s.cursor.Advance(a_tables.changeLog);
		Diagnostics::Notify(Diagnostics::StoreUpdated<DrawBoundStore>{ s, a_tables, producer.frame });
	}

	bool IndirectDraws::Impl::EnsureSceneBuffers(rhi::Device a_device)
	{
		if (scene)
			return true;
		auto buffers = std::make_shared<SceneBuffers>();
		const bool smallStart = SwitchValue(Switch::TableStart) == "small";
		using org::services::GrowableContents;
		const std::uint32_t objects = InitialObjectUnits();
		// Every table grows by doubling. The object records, extras and geometry table belong to the payload.
		std::uint32_t unused = 0;
		// The features' scene resources. The vertex stream comes first, since the sealed set owns its address and capacity.
		// Without a vertex stream the tables are not made.
		buffers->sceneResources = Extensions::SetUpSceneResources(a_device, smallStart);
		buffers->vertexStream = Extensions::SelectVertexStream(buffers->sceneResources);
		if (Extensions::VertexStream().registered && !buffers->vertexStream)
			return false;
		// The visibility readback reads the root states' rows, so it is not made without them.
		if (Extensions::CullBindingBuffer(Api::CullBindingSlot::RootRows, *buffers)) {
			const std::uint32_t roots = smallStart ? 4u : kInitialVisibilityRoots;
			if (auto* host = RenderGraphRuntime::Get().Host()) {
				buffers->visibilityEventsProgram = ComputeProgram::Load(a_device, { .source = kVisibilityEventsShader, .constantWords = kVisibilityEventsConstantWords });
				if (buffers->visibilityEventsProgram) {
					const std::uint32_t events = smallStart ? 4u : kInitialVisibilityEvents;
					buffers->visibilityEvents = MakeVersioned(CreateWords(VisibilityEventBytes(events) / 4, true, "cs.dclf.visibility-events"),
						{ .extent = { kVisibilityEventHeaderWords * 4, 16, 4 } });
					buffers->visibilityRootReported = MakeVersioned(StructuredBuffer(roots, 2 * sizeof(std::uint32_t), "cs.dclf.visibility-root-reported", unused, true),
						{ .extent = { 0, 8, 8 }, .contents = GrowableContents::Zero });
					buffers->visibilityObjectReported = MakeVersioned(StructuredBuffer(objects, sizeof(std::uint32_t), "cs.dclf.visibility-object-reported", unused, true),
						{ .extent = { 0, 4, 4 }, .contents = GrowableContents::Zero });
					auto readback = std::make_shared<VisibilityReadback>();
					readback->armed.resize(readback->ranges.Count());
					readback->AddSet(events);
					buffers->visibilityReadback = std::move(readback);
				} else {
					logger::warn("[DCLF] The visibility events program could not be created; the stood-in roots' kAccumulated is not written (T7b2)");
				}
			}
		}
		// Pass setups run after every table, since a pass may take its feature's scene resources. A null state keeps a pass out of the graph.
		buffers->passStates = Draws::SetUpPassStates(a_device, *buffers);
		scene = std::move(buffers);
		return true;
	}

	void IndirectDraws::Impl::ReserveMainLatch(std::uint32_t a_cascades, std::uint32_t a_shadowVolumes, std::uint32_t a_buckets, std::uint32_t a_classes,
		std::uint32_t a_classMapWords)
	{
		auto& latches = producer.latches;
		auto& layout = latches.main;
		if (!latches.mainLaidOut || (a_cascades <= layout.cascades && a_shadowVolumes <= layout.shadowVolumes && a_buckets <= layout.buckets && a_classes <= layout.classes &&
										a_classMapWords <= layout.classMapWords))
			return;
		if (a_classes > layout.classes) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling(16).NextAs(layout.classes, a_classes);
			logger::info("[DCLF] state classes in the main latch: {} grown to {}", layout.classes, grown);
			layout.classes = grown;
		}
		if (a_classMapWords > layout.classMapWords) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling(256).NextAs(layout.classMapWords, a_classMapWords);
			logger::info("[DCLF] set indices in the main latch's class map: {} grown to {}", layout.classMapWords, grown);
			layout.classMapWords = grown;
		}
		if (a_buckets > layout.buckets) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling(64).NextAs(layout.buckets, a_buckets);
			logger::info("[DCLF] Z-prepass buckets in the main latch: {} grown to {}", layout.buckets, grown);
			layout.buckets = grown;
		}
		if (a_cascades > layout.cascades) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling().NextAs(layout.cascades, a_cascades);
			logger::info("[DCLF] sun cascades in the main latch: {} grown to {}", layout.cascades, grown);
			layout.cascades = grown;
		}
		if (a_shadowVolumes > layout.shadowVolumes) {
			const std::uint32_t grown = org::services::GrowthPolicy::Doubling().NextAs(layout.shadowVolumes, a_shadowVolumes);
			logger::info("[DCLF] local shadow light volumes in the main latch: {} grown to {}", layout.shadowVolumes, grown);
			layout.shadowVolumes = grown;
		}
		// A new layout makes a new block at the next seal. Frames in flight keep the block their set names.
	}

	void ShareDrawArgs(ShadowResources& a_state, std::span<const ShadowViewLayout> a_placements)
	{
		// A light's map slice holds one light a frame, so its views can share draw arguments. Only a pair of paraboloid halves
		// draws together. Cascades, their volumetric copies and the occlusion maps draw together, so they keep their own.
		std::map<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> owners;
		for (const auto& placement : a_placements) {
			if (a_state.drawArgsOwner.size() <= placement.slot) {
				const auto first = static_cast<std::uint32_t>(a_state.drawArgsOwner.size());
				a_state.drawArgsOwner.resize(placement.slot + 1);
				std::iota(a_state.drawArgsOwner.begin() + first, a_state.drawArgsOwner.end(), first);
			}
			if (placement.target != kShadowLightTarget)
				continue;
			const auto owner = owners.try_emplace({ placement.target, placement.slice, placement.y }, placement.slot).first->second;
			a_state.drawArgsOwner[placement.slot] = owner;
			// Slots made before the placements were known.
			if (placement.slot < a_state.drawArgs.size() && owner != placement.slot)
				a_state.drawArgs[placement.slot] = a_state.drawArgs[owner];
		}
	}

	std::shared_ptr<const ComputeProgram> Draws::LoadProgram(rhi::Device a_device, const ComputeProgram::Desc& a_desc, bool& a_pending)
	{
		auto& loads = Loads();
		std::shared_ptr<ProgramLoad> entry;
		bool start = false;
		{
			std::lock_guard lock(loads.mutex);
			if (loads.device != a_device.impl) {
				loads.entries.clear();
				loads.device = a_device.impl;
			}
			auto& slot = loads.entries[ProgramKey(a_desc)];
			if (!slot) {
				slot = std::make_shared<ProgramLoad>();
				start = true;
			}
			entry = slot;
		}
		if (start) {
			// The compile can block in DXC for seconds after a shader change, so it runs on the build executor.
			// It runs inline only when the executor refuses the task.
			auto task = [entry, a_device, desc = a_desc] {
				entry->program = ComputeProgram::Load(a_device, desc);
				entry->state.store(entry->program ? ProgramLoad::kLoaded : ProgramLoad::kFailed, std::memory_order_release);
			};
			if (!BuildSubmitter()(task))
				task();
		}
		const int state = entry->state.load(std::memory_order_acquire);
		a_pending = state == ProgramLoad::kLoading;
		return state == ProgramLoad::kLoaded ? entry->program : nullptr;
	}

	bool ShadowEngine::SetupShadow()
	{
		if (shadow)
			return true;
		if (shadowSetupFailed)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return ShadowNotReady(0, "no render graph");
		auto device = host->Device();
		// Programs first. While they compile nothing is made and the views stay native.
		bool pending = false, rowsPending = false;
		auto assignCasterRows = LoadProgram(device, { .source = kBuildDrawsShader, .entry = L"ShadowCasterRows", .constantWords = kBuildDrawsConstantWords }, rowsPending);
		auto buildDraws = LoadProgram(device, { .source = kBuildDrawsShader, .entry = L"ShadowViews", .constantWords = kBuildDrawsConstantWords }, pending);
		if (pending || rowsPending)
			return ShadowNotReady(0, "the compute programs are compiling off the render thread");
		auto state = std::make_shared<ShadowResources>();
		state->constants = DeviceBuffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		// The shadow commit latches into the constants alone, so this is known before any graph declares the pass.
		state->latchedTargets.store(std::make_shared<const std::vector<const void*>>(1, state->constants.get()), std::memory_order_release);
		auto scene = shadowHost.ShadowSceneBuffers(device);
		if (!scene) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the face positions");
		}
		state->scene = std::move(scene);
		const bool smallSlots = SwitchValue(Switch::TableStart) == "small";
		const auto firstLayout = InitialShadowLatchLayout();
		AddShadowViewSlots(*state, firstLayout.viewSlots, 64u, 64u);
		// One row per view slot, matching the latch layout.
		if (!state->viewBlocks.Create(static_cast<std::uint32_t>(kShadowViewSlotBytes), firstLayout.viewSlots, "cs.dclf.shadow.view-blocks",
				org::services::GrowthPolicy::Headroom(0))) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow view blocks");
		}
		const std::uint32_t objects = InitialObjectUnits();
		state->visibility = MakeVersioned(CreateWords(objects, true, "cs.dclf.shadow.visibility"), { .extent = { 0, 4, 4 } });
		state->casterRows = MakeVersioned(CreateWords(64u * sizeof(DrawSequence) / 4, true, "cs.dclf.shadow.caster-rows"),
			{ .extent = { 0, sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Headroom(25, 64) });
		state->casterFirsts = MakeVersioned(CreateWords(objects, true, "cs.dclf.shadow.caster-firsts"), { .extent = { 0, 4, 4 } });
		state->casterRowsUsed = CreateWords(4u, true, "cs.dclf.shadow.caster-rows-used");
		state->assignCasterRows = std::move(assignCasterRows);
		if (!state->assignCasterRows) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the caster rows' compute program could not be created");
		}
		state->rowsDispatchSignature = CreateDispatchSignature(device, state->assignCasterRows->layout->GetHandle());
		if (!state->rowsDispatchSignature) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the caster rows' dispatch signature could not be created");
		}
		// One dispatch covers every view of an epoch.
		state->buildDraws = std::move(buildDraws);
		if (!state->buildDraws) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws compute program could not be created");
		}
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws dispatch signature could not be created");
		}
		state->pool = EnsureIndexPool(*state->scene, device, smallSlots);
		if (!state->pool) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the index pool's copy program could not be created");
		}
		state->constantsAddress = AddressOf(device, *state->constants);
		state->constantsRef = RenderGraph::DeclaredRefOf(device, *state->constants);
		if (!state->constantsAddress || !state->constantsRef) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address or buffer view ref for the shadow buffers");
		}
		// D3D12 reaches SharedData and FeatureData through constant buffer views. A view covers whole 256-byte rows, at most 64 KiB.
		if (::RenderGraph::Backend::IsD3D12()) {
			const auto arena = ShadowArenaBlocksOf();
			const auto rows = [](std::uint32_t a_bytes) {
				return static_cast<std::uint32_t>(std::min<std::uint64_t>((std::max<std::uint64_t>(a_bytes, 1) + kConstantAlignment - 1) & ~(kConstantAlignment - 1), 65536));
			};
			const std::array<FrameBlockViews::Block, 2> blocks{ { { arena.sharedData, rows(arena.sharedBytes ? arena.sharedBytes : static_cast<std::uint32_t>(kShadowZeroBlockBytes)) },
				{ arena.featureData, rows(arena.featureBytes ? arena.featureBytes : static_cast<std::uint32_t>(kShadowZeroBlockBytes)) } } };
			state->blockViews = FrameBlockViews::CreateFor(device, state->constants, blocks, "shadow block");
			if (!state->blockViews) {
				shadowSetupFailed = true;
				return ShadowNotReady(1, "the shadow views' SharedData and FeatureData views could not be made");
			}
			state->blockViewIndices = { state->blockViews->indices[0], state->blockViews->indices[1] };
		}
		shadow = state;
		shadowHost.ShadowResourcesMade(state);
		host->AddExtension(kShadowExtensionId, [] { return Diagnostics::ScheduleDumped(kShadowExtensionId, MakeShadowExtension(BuildingState(kShadowExtensionId))); }, ExtensionEpochs(Api::EpochShape::Shadow));
		logger::info("[DCLF] shadow view graph resources created");
		shadowHost.PostRevisionInputs();
		return true;
	}

	void IndirectDraws::Impl::ServeRevisionRequests()
	{
		// Makes the requested shadow view slots on the render thread and rebuilds the graph. The revision code waits for it.
		auto* host = RenderGraphRuntime::Get().Host();
		auto& shadow = shadowEngine->shadow;
		if (const auto request = shadowSlotsSlot.Take(); request && host && shadow && request->slots > shadow->drawArgs.size()) {
			AddShadowViewSlots(*shadow, request->slots, request->sequenceDraws, request->bucketCountWords);
			host->AddExtension(kShadowExtensionId, [] { return Diagnostics::ScheduleDumped(kShadowExtensionId, MakeShadowExtension(BuildingState(kShadowExtensionId))); }, ExtensionEpochs(Api::EpochShape::Shadow));
			PostRevisionInputs();
		}
	}

	bool IndirectDraws::Impl::SetupView(ViewKind a_kind)
	{
		// A view's buffers, once the main resources and index pool exist.
		auto& v = View(a_kind);
		if (v.resources)
			return true;
		if (v.setupFailed || !resources || !scene || !scene->pool)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return false;
		const auto& traits = TraitsOf(a_kind);
		auto device = host->Device();
		const auto name = [&](std::string_view a_name) { return fmt::format("{}.{}", traits.prefix, a_name); };
		auto fail = [&](const char* a_what) {
			v.setupFailed = true;
			logger::warn("[DCLF] the {} view's {}; it stays native", traits.name, a_what);
			return false;
		};
		// While the program compiles the view stays native, without failing.
		bool programsPending = false;
		auto buildDraws = LoadProgram(device, { .source = kBuildDrawsShader, .constantWords = kBuildDrawsConstantWords }, programsPending);
		if (programsPending)
			return false;
		auto state = std::make_shared<ViewResources>();
		state->traits = &traits;
		state->main = resources;
		state->buildDraws = std::move(buildDraws);
		if (state->buildDraws)
			state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature)
			return fail("BuildDraws program could not be created");
		state->count = CreateWords(kCountWords, true, name("draw-count").c_str());
		// First sizes. RequireView grows them to hold the view's plan.
		const std::uint32_t buckets = org::services::GrowthPolicy::Doubling(16).NextAs(0u, ViewLatchLayout::Initial(traits).buckets);
		for (std::uint32_t f = 0; f < traits.faces; ++f)
			state->bucketCounts[f] = MakeVersioned(CreateWords(buckets, true, name(traits.faces > 1 ? fmt::format("bucket-counts{}", f) : "bucket-counts").c_str()),
				{ .extent = { 0, 4, 4 }, .policy = org::services::GrowthPolicy::Headroom(0) });
		state->sequences = MakeVersioned(CreateWords(std::uint64_t(traits.faces) * 64u * sizeof(DrawSequence) / 4, true, name("sequences").c_str()),
			{ .extent = { 0, traits.faces * sizeof(DrawSequence), 4 }, .policy = org::services::GrowthPolicy::Doubling(64) });
		// The faces' draw family rows and lists, one per face. The lists grow with the scene tables.
		for (std::size_t d = 0; d < Extensions::kDrawFamilies && traits.trees; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			if (!Extensions::DrawFamilyHasFaces(family))
				continue;
			const auto& entry = Extensions::DrawFamilyOf(family);
			const auto& faces = entry.contribution.faces;
			const std::uint32_t headerWords = faces.headerBytes / 4;
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				state->faceRows[d][f] = CreateWords(faces.rowBytes / 4, false, name(fmt::format("{}{}", entry.rowName, f)).c_str());
				state->faceRowsRef[d][f] = RenderGraph::DeclaredRefOf(device, *state->faceRows[d][f]);
				if (!state->faceRowsRef[d][f])
					return fail(fmt::format("{} rows have no buffer view ref", entry.label).c_str());
				if (faces.lists && FamilySetUp(*scene, family)) {
					Api::DrawFamilyLists lists{ *scene };
					faces.lists(lists);
					const std::uint32_t words = headerWords + lists.current * faces.unitWords;
					state->faceLists[d][f] = MakeVersioned(CreateWords(words, true, name(fmt::format("{}{}", entry.listName, f)).c_str()),
						{ .extent = { faces.headerBytes, faces.unitWords * 4, 4 } });
				}
			}
		}
		state->blocks = DeviceBuffer(std::uint64_t(traits.faces) * kViewBlockBytes, name(traits.blocks).c_str());
		state->blocksAddress = AddressOf(device, *state->blocks);
		if (!state->blocksAddress)
			return fail("blocks have no device address");
		// A view with its own frame constants, laid out like the main ones and written by its commit.
		if (OwnsFrameConstants(traits)) {
			state->frameConstants = DeviceBuffer(kFrameConstantBytes, name("frame-constants").c_str());
			state->frameConstantsAddress = AddressOf(device, *state->frameConstants);
			state->frameConstantsRef = RenderGraph::DeclaredRefOf(device, *state->frameConstants);
			if (!state->frameConstantsAddress || !state->frameConstantsRef)
				return fail("frame constants have no device address or buffer view ref");
			state->frameBlocks = FrameBlockViews::Create(device, state->frameConstants);
			if (!state->frameBlocks)
				return fail("the frame blocks' views could not be made");
			std::vector<FrameBlockViews::Block> faceBlocks;
			for (std::uint32_t f = 0; f < traits.faces; ++f)
				faceBlocks.push_back({ std::uint64_t(f) * kViewBlockBytes, kViewBlockBytes });
			state->faceBlocks = FrameBlockViews::CreateFor(device, state->blocks, faceBlocks, "the faces' camera blocks");
			if (!state->faceBlocks)
				return fail("the faces' camera blocks' views could not be made");
			MatchViewFrameBuffers(*state, resources->frameBuffers);
		}
		// The forward part draws through the main set's colour signatures with an explicit preprocess, like ForwardDrawPass.
		if (traits.forwardPart)
			state->preprocessForward = PreprocessStates::Create(device, fmt::format("the {} forward part", traits.name).c_str());
		resources->views[ViewIndex(a_kind)] = state;
		v.resources = std::move(state);
		logger::info("[DCLF] {} graph resources created", traits.name);
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportViewColourTarget(ViewKind a_kind, ID3D11Texture2D* a_texture)
	{
		// A ColourWithOwnDepth view, such as the reflection's cube target.
		auto* host = RenderGraphRuntime::Get().Host();
		auto& view = View(a_kind);
		if (!view.resources || !host || !a_texture)
			return false;
		const auto& traits = TraitsOf(a_kind);
		auto& r = *view.resources;
		if (r.target && r.targetTexture == a_texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return false;
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(view.targets.colour);
		desc.channels = 4;
		desc.hasRTV = true;
		desc.rtvFormat = desc.format;
		auto cube = ImportImage(host->Device(), a_texture, info.image, desc, fmt::format("DCLF {} {}", traits.name, traits.target).c_str());
		if (!cube) {
			static std::uint32_t logged = 0;
			if (logged++ < 4)
				logger::warn("[DCLF] the {} {} could not be imported (not in the general layout?); the faces stay native", traits.name, traits.target);
			return false;
		}
		// DCLF's own depth for the faces, at the engine's precision. Its depth target 6 is R24G8.
		org::TextureDescription depthDesc{};
		depthDesc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
		depthDesc.format = rhi::helpers::ToRHI(view.targets.depth);
		depthDesc.channels = 1;
		depthDesc.hasDSV = true;
		depthDesc.dsvFormat = depthDesc.format;
		r.depth = org::PixelBuffer::CreateSharedUnmaterialized(depthDesc);
		r.depth->SetName(fmt::format("{}.depth", traits.prefix));
		r.target = std::move(cube);
		r.targetTexture = a_texture;
		r.width = info.image.extent.width;
		r.height = info.image.extent.height;
		host->AddExtension(kViewSlots[ViewIndex(a_kind)].extensionId, [kind = a_kind, id = kViewSlots[ViewIndex(a_kind)].extensionId] { return Diagnostics::ScheduleDumped(id, MakeViewExtension(BuildingState(id), kind)); }, ExtensionEpochs(Api::EpochShape::View, a_kind));
		logger::info("[DCLF] {} {} imported: {}x{}, {} faces, format {}", traits.name, traits.target, r.width, r.height, info.image.arrayLayers, static_cast<int>(view.targets.colour));
		PostRevisionInputs();
		return true;
	}

	bool IndirectDraws::Impl::ImportViewDepthTarget(ViewKind a_kind)
	{
		// A DepthTarget view, such as first person's kPOST_ZPREPASS_COPY (FUN_1414cb6d0), imported in its DSV's format.
		auto* host = RenderGraphRuntime::Get().Host();
		auto* renderer = globals::game::renderer;
		auto& view = View(a_kind);
		if (!view.resources || !host || !renderer)
			return false;
		auto& r = *view.resources;
		const auto& copy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (!copy.texture || !copy.views[0])
			return false;
		if (r.target && r.targetTexture == copy.texture && !r.rebuildPending)
			return true;
		if (!r.target || r.targetTexture != copy.texture) {
			DxvkOrgInteropResourceInfo info{};
			if (!RenderGraphRuntime::Get().DescribeResource(copy.texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
				return false;
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			copy.views[0]->GetDesc(&dsvDesc);
			org::TextureDescription desc{};
			desc.imageDimensions.push_back({ info.image.extent.width, info.image.extent.height, 0, 0 });
			desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
			desc.channels = 1;
			desc.hasDSV = true;
			desc.dsvFormat = desc.format;
			const auto& traits = TraitsOf(a_kind);
			auto imported = ImportImage(host->Device(), copy.texture, info.image, desc, fmt::format("DCLF {} depth (kPOST_ZPREPASS_COPY)", traits.name).c_str());
			if (!imported) {
				static std::uint32_t logged = 0;
				if (logged++ < 4)
					logger::warn("[DCLF] kPOST_ZPREPASS_COPY could not be imported (not in the general layout?); {} stays native",
						kKeyGroupNames[kViewSlots[ViewIndex(a_kind)].keyGroup]);
				return false;
			}
			r.target = std::move(imported);
			r.targetTexture = copy.texture;
			logger::info("[DCLF] {} depth imported: {}x{}, format {}", traits.name, info.image.extent.width, info.image.extent.height, static_cast<int>(dsvDesc.Format));
		}
		r.rebuildPending = false;
		host->AddExtension(kViewSlots[ViewIndex(a_kind)].extensionId, [kind = a_kind, id = kViewSlots[ViewIndex(a_kind)].extensionId] { return Diagnostics::ScheduleDumped(id, MakeViewExtension(BuildingState(id), kind)); }, ExtensionEpochs(Api::EpochShape::View, a_kind));
		PostRevisionInputs();
		return true;
	}

	bool ShadowEngine::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		return ImportDepthTexture(a_index, data.texture, data.views[0], a_target);
	}

	bool ShadowEngine::ImportDepthTexture(std::uint32_t a_index, ID3D11Texture2D* a_texture, ID3D11DepthStencilView* a_view, std::uint32_t a_label,
		std::shared_ptr<org::ExternalTextureResource> a_graph)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !shadow || a_index >= kShadowDepthTargets)
			return false;
		if (!a_texture || !a_view)
			return ShadowNotReady(3, "the shadow map has no texture");
		if (shadow->depth[a_index] && shadow->depthTexture[a_index] == a_texture)
			return true;
		const auto adopt = [&](FrameAnchors::Texture a_resource, std::uint32_t a_layers) {
			shadow->depth[a_index] = std::move(a_resource);
			shadow->depthTexture[a_index] = a_texture;
			shadow->depthLayers[a_index] = a_layers;
			// The passes need to bind a new resource, so the shadow epochs' graphs rebuild at their next build.
			host->AddExtension(kShadowExtensionId, [] { return Diagnostics::ScheduleDumped(kShadowExtensionId, MakeShadowExtension(BuildingState(kShadowExtensionId))); }, ExtensionEpochs(Api::EpochShape::Shadow));
		};
		// A graph-owned target, such as an occlusion map's twin, is one 2D slice.
		if (a_graph) {
			adopt(std::move(a_graph), 1);
			logger::info("[DCLF] shadow map {} is a graph texture: declared as itself, not imported", a_label);
			return true;
		}
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return ShadowNotReady(4, "the shadow map cannot be described");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		a_view->GetDesc(&dsvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
		desc.channels = 1;
		desc.hasDSV = true;
		desc.dsvFormat = desc.format;
		// Readable too, since an occlusion map's readers sample it.
		desc.hasSRV = true;
		desc.srvFormat = DepthReadFormat(desc.format);
		static constexpr const char* kNames[kFirstOcclusionTarget] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)" };
		const char* name = IsOcclusionTarget(a_index) ? Extensions::OcclusionMapOf(a_index - kFirstOcclusionTarget).targetName.c_str() : kNames[a_index];
		auto imported = ImportImage(host->Device(), a_texture, info.image, desc, name);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		adopt(std::move(imported), info.image.arrayLayers);
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_label, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}
}
