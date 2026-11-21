// The main epochs' commit on the render thread. It writes the frame's values into the selected revision's shape
// and uploads the depth commit's scene streams.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Coordinator.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Draws.h"

#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace DCLF
{
	namespace
	{
		/**
		 * @brief Logs once per register when a colour commit reads a published graph texture's export through a D3D11 view.
		 * Such an import is not ordered after the producer's graph writes, so the producer should list its id in graphResources.
		 */
		void NoteExportRead(std::uint32_t a_register, ID3D11ShaderResourceView* a_view)
		{
			static std::array<std::string, kTextureRegisters> s_logged;
			auto id = PublishedResources::ExportedId(a_view);
			if (id.empty() || s_logged[a_register] == id)
				return;
			logger::warn("[DCLF] t{} reads the D3D11 export of the published '{}': not ordered after its graph writes", a_register, id);
			s_logged[a_register] = std::move(id);
		}
	}

	static_assert(Draws::FrameConstantRowOffset(Extensions::kFrameConstantRowFirst) == Draws::kExtrasPixelFrameOffset + sizeof(ExtrasFrame::projectedGlobals),
		"Core's first frame constant row is not right after the extras' pixel row");

	namespace
	{
		std::string FrameDataWhere(std::string_view a_route, std::optional<std::string_view> a_target, std::uint64_t a_offset)
		{
			std::string where(a_route);
			where += ": ";
			if (!a_target)
				return where + "other";
			if (*a_target == "cs.dclf.frame-constants")
				return where + Draws::FrameConstantsPart(a_offset);
			where += a_target->empty() ? std::string("unnamed") : std::string(*a_target);
			return where;
		}

		// The upload classes' bytes, counted by destination under the stats switch.
		class FrameDataSink final : public org::services::IUploadBytesSink
		{
		public:
			bool Enabled() const override { return FrameData::Enabled(); }
			void Note(std::string_view a_route, std::optional<std::string_view> a_target, std::uint64_t a_offset, std::size_t a_bytes) override
			{
				FrameData::Note(FrameDataWhere(a_route, a_target, a_offset), a_bytes);
			}
		};
	}

	const org::services::CommitUploadHooks& Draws::CommitHooks()
	{
		static FrameDataSink sink;
		static const org::services::CommitUploadHooks hooks{ .onError = ::RenderGraph::FatalHandler("Drawcall Limit Fix"), .bytes = &sink };
		return hooks;
	}

	std::shared_ptr<org::runtime::StagedUploadBatch> Draws::AcquireCommitBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		DCLF_ZONE("CS.DCLF.Stage.AcquireBatch");
		return org::services::AcquireStagedBatch(a_pool, std::size_t{ 1 } << 20);
	}

	std::shared_ptr<const StreamViews> Draws::MakeStreamViews(ObjectRecordStore& a_objects, ExtrasStore& a_extras, GeometryStore& a_geometries, const TablesContentEpochs& a_oldest,
			std::shared_ptr<const SceneTables> a_hold, const SceneTables& a_tables, std::uint32_t a_generation, std::uint32_t a_frame)
		{
			DCLF_ZONE("CS.DCLF.MakeStreamViews");
			auto views = std::make_shared<StreamViews>();
			views->tables = std::move(a_hold);
			views->generation = a_generation;
			// The three stores are independent, so they run as pool tasks.
			GeometryDrawsOut geometries;
			const auto store = [&](std::size_t a_store) {
				switch (a_store) {
				case 0:
					UpdateObjectRecords(&a_objects, a_oldest.objects, a_tables, a_generation, a_frame, views->objects);
					break;
				case 1:
					UpdateExtras(&a_extras, a_oldest.extras, a_tables, a_generation, views->extras);
					break;
				default:
					UpdateGeometryDraws(&a_geometries, a_oldest.geometries, a_tables, a_generation, a_frame, geometries);
					break;
				}
			};
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.MakeStreamViews.Stores", 3, 1, [&](std::size_t a_begin, std::size_t a_end) {
				DCLF_ZONE("CS.DCLF.MakeStreamViews.StoresShards");
				for (std::size_t s = a_begin; s < a_end; ++s)
					store(s);
			});
			views->geometries = std::move(geometries.slots);
			return views;
		}

	namespace
	{
		// Latch regions already holding the snapshot's bucket tables and the revision's class tables. Render thread.
		Draws::LatchRegionsHeld mainBucketRegions;
		Draws::LatchRegionsHeld mainClassRegions;
	}

	// CommitMainPayload's locals, shared by its stages.
	struct Draws::MainCommit
	{
		const Capture& capture;
		const MainInputs& in;
		MainPayload& payload;
		const std::shared_ptr<Resources>& resources;
		IndirectDraws::Stats& stats;
		std::vector<std::shared_ptr<const void>>& bindingOwners;
		const SceneTables& tables;
		const PayloadVersions& parts;
		const NamedVersions& adopted;
		CommitUploads& uploads;
		LatchedUploads& latched;
		GpuTextures& textures;
		ConstantMirror& mirror;
		const PassFrame& shape;
		const std::shared_ptr<const PassFrame>& revisionShape;
		const ZBucketPlan& writePlan;
		const org::LatchBlock& latchBlock;
		const MainLatchLayout& latchLayout;
		std::uint32_t frameNumber;
		bool depthOnly;
		bool replayVertexInputs;
	};

	namespace
	{
		/** @brief A draw input defect: fatal in development builds. Release counts it, logs the first and draws on. */
		void NoteDrawInputs(std::string_view a_what)
		{
			::RenderGraph::Frame::Driver::NoteTapError(::RenderGraph::Frame::Driver::TapError::DrawInputs, ::RenderGraph::Frame::SegmentId::Count, a_what);
		}

		/** @brief What is bound at an unresolved frame texture register, for its tap error. */
		std::string DescribeFrameTexture(ID3D11ShaderResourceView* a_view)
		{
			if (!a_view)
				return "nothing is bound there at the capture";
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			a_view->GetDesc(&viewDesc);
			std::string what = fmt::format("bound natively: view dimension {}, format {}", static_cast<std::uint32_t>(viewDesc.ViewDimension),
				static_cast<std::uint32_t>(viewDesc.Format));
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			if (auto buffer = resource.try_as<ID3D11Buffer>()) {
				D3D11_BUFFER_DESC bufferDesc{};
				buffer->GetDesc(&bufferDesc);
				what += fmt::format(", buffer of {} bytes, stride {}, usage {}, CPU access {:X}, bind {:X}, misc {:X}", bufferDesc.ByteWidth,
					bufferDesc.StructureByteStride, static_cast<std::uint32_t>(bufferDesc.Usage), bufferDesc.CPUAccessFlags, bufferDesc.BindFlags, bufferDesc.MiscFlags);
			}
			char name[128]{};
			UINT size = sizeof(name) - 1;
			if (resource && SUCCEEDED(resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)))
				what += fmt::format(", '{}'", name);
			return what;
		}

		// Unresolved frame textures read zero, like an unbound view. They are counted because the build could not skip their draws.
		void CountMissingFrameTextures(const MainCommit& a_commit, const std::array<std::uint32_t, kTextureRegisters>& a_frameTextures)
		{
			std::uint32_t frameTexturesMissing = 0;
			std::array<std::uint64_t, 2> missingRegisters{};
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				if (!((a_commit.payload.frameRegisters[t / 64] >> (t % 64)) & 1) || a_frameTextures[t] != kInvalidIndex)
					continue;
				++frameTexturesMissing;
				missingRegisters[t / 64] |= 1ull << (t % 64);
				// Described once per register. Later ones only count.
				static std::array<bool, kTextureRegisters> s_described{};
				NoteDrawInputs(std::exchange(s_described[t], true) ? fmt::format("frame texture t{} is read but not resolved", t) :
				                                                        fmt::format("frame texture t{} is read but not resolved: {}", t, DescribeFrameTexture(a_commit.capture.psViews[t])));
			}
			a_commit.stats.frameTexturesMissing = frameTexturesMissing;
			a_commit.stats.frameTexturesMissingRegisters = missingRegisters;
		}

		// The frame record holds every register a draw's rows do not give.
		void WriteFrameRecord(const MainCommit& a_commit, const std::array<std::uint32_t, kTextureRegisters>& a_frameTextures)
		{
			const auto& in = a_commit.in;
			const auto& resources = *a_commit.resources;
			const std::uint32_t nullIndex = a_commit.textures.NullIndex() != kInvalidIndex ? a_commit.textures.NullIndex() : 0u;
			const DrawBindings frameRecord = FrameRecordOf(resources.frameConstantsAddress, resources.frameConstantsRef, *resources.frameBlocks, in.vsFrameMask, in.psFrameMask, a_frameTextures, nullIndex, a_commit.parts,
				{ in.addresses.placementsIndex, in.addresses.palettesIndex, in.addresses.shadingIndex }, a_commit.adopted, *resources.scene, a_commit.frameNumber);
			a_commit.latched(resources.frameConstants, &frameRecord, sizeof(frameRecord), std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
		}

		void LatchFrameSlots(const MainCommit& a_commit, const FrameBlocks& a_blocks)
		{
			const auto& resources = a_commit.resources;
			auto& latched = a_commit.latched;
			auto& uploads = a_commit.uploads;
			// The zeroed light block is what every draw's b3 reads.
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if (!a_blocks.vs[slot].empty())
					latched(resources->frameConstants, a_blocks.vs[slot].data(), a_blocks.vs[slot].size(), FrameSlotOffset(false, slot));
				if (!a_blocks.ps[slot].empty())
					latched(resources->frameConstants, a_blocks.ps[slot].data(), a_blocks.ps[slot].size(), FrameSlotOffset(true, slot));
			}
			ZeroSharedLightOnce(uploads, resources->frameConstants, resources->sharedLightZeroed);
			LatchFrameRows(latched, resources->frameConstants);
		}

		void CopyResidentStats(const MainCommit& a_commit)
		{
			const auto& payload = a_commit.payload;
			auto& stats = a_commit.stats;
			const bool depthOnly = a_commit.depthOnly;
			stats.residentInputs = static_cast<std::uint32_t>(payload.resident.Count());
			if (!depthOnly) {
				stats.residentDraws = payload.body->residentDraws;
				stats.residentPairs = payload.body->residentPairs;
				stats.residentUndrawable = payload.body->residentUndrawable;
			}
			stats.residentEpochs += payload.resident.ContentEpoch() != stats.residentLastEpoch[depthOnly ? 0 : 1] ? 1 : 0;
			stats.residentLastEpoch[depthOnly ? 0 : 1] = payload.resident.ContentEpoch();
			stats.residentResyncs += payload.residentResyncs;
			for (std::size_t k = 0; k < stats.residentResyncBy.size(); ++k)
				stats.residentResyncBy[k] += (payload.residentResyncReasons >> k) & 1u;
			stats.residentDecalRetakes += payload.residentDecalRetakes;
		}

		void CopyBuildStats(const MainCommit& a_commit)
		{
			const auto& payload = a_commit.payload;
			auto& stats = a_commit.stats;
			stats.skipped = payload.body->skipped;
			stats.missingTextures = payload.body->missingTextures;
			stats.missingVertexConstants = payload.body->missingVertexConstants;
			stats.missingPixelConstants = payload.body->missingPixelConstants;
			stats.deferredTextures = payload.body->deferredTextures;
			stats.rowTableConflicts += payload.rowTableConflicts;
			for (std::size_t i = 0; i < payload.partMs.size(); ++i)
				stats.partMs[i] = payload.partMs[i];
			if (!a_commit.depthOnly)
				stats.decalsDrawn = payload.body->decalsDrawn;
			if (payload.body->shortBuffers) {
				const bool first = stats.shortBuffers == 0;
				stats.shortBuffers += payload.body->shortBuffers;
				const auto& tables = a_commit.tables;
				const auto& shortBuffer = payload.body->shortBuffer;
				if (first && shortBuffer.object < tables.objects.size()) {
					const auto& geometry = tables.geometries[tables.objects[shortBuffer.object].geometryIndex];
					const auto* source = tables.objectGeometry[shortBuffer.object];
					NoteDrawInputs(fmt::format("'{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
						source && source->name.c_str() ? source->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride, shortBuffer.vertexNeeded,
						geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, shortBuffer.indexNeeded, geometry.indexBytes));
				} else {
					NoteDrawInputs(fmt::format("{} draws past their buffers", payload.body->shortBuffers));
				}
			}
			// The snapshot builder's payload uploads are counted separately in PayloadVersions.
			stats.uploadBytes = (payload.materialRowsWritten * sizeof(MaterialRow)) + (payload.pipelineRowsWritten * sizeof(PipelineRow)) +
			                      payload.body->inputList.size() * sizeof(DrawInput);
			stats.records = static_cast<std::uint32_t>(payload.body->materialRows.Count());
		}

		struct MainCullInputs
		{
			std::uint32_t inputCount = 0;
			std::array<float, 16> viewProj{};
			bool hasViewProj = false;
		};

		void CountMainDraws(const MainCommit& a_commit, const std::vector<std::byte>& a_prepassPerFrame, MainCullInputs& a_cull)
		{
			const auto& payload = a_commit.payload;
			const auto& shape = a_commit.shape;
			const bool depthOnly = a_commit.depthOnly;
			// The resident region's draws and inputs come first.
			const std::uint32_t drawCount = static_cast<std::uint32_t>(payload.body->sequences.size() + payload.body->residentDraws);
			const auto decalCount = depthOnly ? std::array<std::uint32_t, kDecalGroups>{} : payload.body->decalCount;
			// The depth segment covers more inputs than drawCount because of cull-only candidates.
			a_cull.inputCount = static_cast<std::uint32_t>(payload.body->inputList.size() + (payload.resident.Count()));
			// The revision sized the sequences for the scene's draw bound, so a count past it is a bug in that bound.
			if (drawCount > shape.sequenceDraws)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} draws past the sequence buffer's {} (the scene's draw bound missed them)", drawCount, shape.sequenceDraws));
			for (std::uint32_t group = 0; group < kDecalGroups; ++group)
				if (decalCount[group] > shape.sequenceDecals)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} decals past the sequence buffer's {} per group", decalCount[group], shape.sequenceDecals));
			// The culling must use the draws' ViewProj from VS_PerFrame c8.
			if (auto* buffer = a_commit.capture.vsBuffers[kPerFrameVertexRegister]) {
				a_commit.mirror.Watch(buffer);
				const auto contents = a_commit.mirror.Contents(buffer);
				if (contents.size() >= 48 * sizeof(float)) {
					std::memcpy(a_cull.viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
					a_cull.hasViewProj = true;
				}
			}
			// Both epochs must rasterise identically for the colour EQUAL test, so their viewport and ViewProj are logged.
			Diagnostics::Notify(Diagnostics::MainCullingSetup{ shape, a_commit.capture, depthOnly, a_cull.hasViewProj, a_commit.replayVertexInputs, a_commit.frameNumber,
				a_commit.tables.liveObjects, a_commit.replayVertexInputs ? std::span<const std::byte>(a_prepassPerFrame) : std::span<const std::byte>() });
			a_commit.stats.drawn = drawCount;
			++a_commit.stats.commitEpochs;
		}

		void FillCullLatch(const MainCommit& a_commit, const MainCullInputs& a_cull, BuildDrawsLatch& a_latch)
		{
			const auto& resources = a_commit.resources;
			const auto& shape = a_commit.shape;
			const bool depthOnly = a_commit.depthOnly;
			const std::uint32_t frameNumber = a_commit.frameNumber;
			a_latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			a_latch.fadeSeedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, *resources->scene, frameNumber, a_commit.adopted);
			// The commit hooks have already published this frame's portal programs.
			a_latch.portalProgramsIndex = depthOnly ? CullBindingIndex(Api::CullBindingSlot::Programs, *resources->scene, frameNumber, a_commit.adopted) : 0u;
			// Portal-strict lights gate their receivers. Index 0 gates nothing.
			if (!depthOnly) {
				a_latch.lightContextsIndex = CullBindingIndex(Api::CullBindingSlot::LightContexts, *resources->scene, frameNumber, a_commit.adopted);
				a_latch.strictGatesIndex = CullBindingIndex(Api::CullBindingSlot::StrictGates, *resources->scene, frameNumber, a_commit.adopted);
			}
			a_latch.stageWord = depthOnly ? Draws::PassLatchWord() : 0u;
			PayloadLatch(a_commit.parts, a_latch);
			a_latch.dispatch[0] = (a_cull.inputCount + 63) / 64;
			a_latch.dispatch[1] = 1;
			a_latch.dispatch[2] = 1;
			a_latch.drawCount = a_cull.inputCount;
			a_latch.cullFlags = a_cull.hasViewProj ? shape.cullMode : 0u;
			// The depth segment culls every candidate. The colour segment draws the members and writes every decal slot.
			a_latch.viewBits = depthOnly ? kDepthViewBits : kColourViewBits;
			// The depth segment publishes and the colour segment reads within one frame.
			a_latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
			if (resources->hzb && shape.width && shape.height) {
				// Mip 0 covers twice its size in source pixels. Image UVs scale by the rendered share to reach the HZB.
				const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
					const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
					return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
				};
				a_latch.hzbUvScalePacked = scale(shape.width, resources->hzbWidth * 2) | (scale(shape.height, resources->hzbHeight * 2) << 16);
			}
			LatchCullCamera(a_latch, a_cull.viewProj, a_commit.capture.eye);
			// Set parity projects absolute bounds, so it keeps the folded form.
			float foldedViewProj[16];
			FoldEyeIntoViewProj(a_cull.viewProj, a_commit.capture.eye, foldedViewProj);
			std::copy(std::begin(foldedViewProj), std::end(foldedViewProj), a_commit.payload.cullViewProj.begin());
			a_commit.payload.culled = a_latch.cullFlags != 0;
			if (depthOnly) {
				// Captured at frame start by Features/Fades.
				Extensions::RootStatesLatchGlobals(a_latch.fadeEye, a_latch.treeHeight);
				// The near twin test for terrain twin buckets. Without a test the latch stays zero.
				if (const auto nearTest = Extensions::NearTwin().nearTest)
					nearTest(a_latch.terrainNear);
				Diagnostics::Notify(Diagnostics::CullLatchWritten{ a_latch, true });
			}
			// Both segments read LOD skin levels from the same fade states.
			a_latch.fadeStatesIndex = CullBindingIndex(Api::CullBindingSlot::RootStates, *resources->scene, frameNumber, a_commit.adopted);
		}
	}

	// The colour pass's cascades and local shadow volumes, clamped to the latch size the producer reserved.
	void IndirectDraws::Impl::CommitShadowInputs(bool a_depthOnly, const PassFrame& a_shape, std::uint32_t a_revisionEpoch)
	{
		if (!a_depthOnly) {
			// The colour pass's sun test needs the cascades every frame, whether or not the entry exclusion ran.
			sunCascades.clear();
			Api::SunCascadeSink cascadeSink(sunCascades);
			Extensions::SampleSunCascades(cascadeSink);
			// Frames with no cascades draw sun-lit passes unshadowed. That is only right in interiors.
			const auto* tes = RE::TES::GetSingleton();
			sunTestEmpty += sunCascades.empty() && !(tes && tes->interiorCell) ? 1 : 0;
			if (sunTestWindow.Tick())
				Reports::ColourSunTestLine(std::exchange(sunTestEmpty, 0));
			// The list stays whole as the build parity's reference. The latch takes the clamped count.
			shadowVolumes.clear();
			Api::LightVolumeSink sink(shadowVolumes);
			Extensions::SampleLightVolumes(sink);
			shadowVolumesLatched = static_cast<std::uint32_t>(shadowVolumes.size());
		}
		if (sunCascades.size() > a_shape.latchLayout.cascades || shadowVolumesLatched > a_shape.latchLayout.shadowVolumes) {
			++revisions.latchClamped[a_revisionEpoch];
			sunCascades.resize(std::min<std::size_t>(sunCascades.size(), a_shape.latchLayout.cascades));
			shadowVolumesLatched = std::min(shadowVolumesLatched, a_shape.latchLayout.shadowVolumes);
		}
	}

	// Resolves the frame's textures from t16 up into the frame record.
	void IndirectDraws::Impl::ResolveFrameTextures(const MainCommit& a_commit, std::array<std::uint32_t, kTextureRegisters>& a_frameTextures)
	{
		const auto& capture = a_commit.capture;
		const auto& epochResources = a_commit.resources;
		auto& textures = a_commit.textures;
		auto& mirror = a_commit.mirror;
		const bool depthOnly = a_commit.depthOnly;
		a_frameTextures.fill(kInvalidIndex);
		if (frameTextureGeneration != textures.Generation()) {
			frameTextureBindings = {};
			frameTextureGeneration = textures.Generation();
		}
		// Each registered frame texture view is resolved once per colour commit.
		// A null view with capturedWhenNull keeps the captured one.
		const auto frameTextures = Extensions::RegisteredFrameTextures();
		const auto frameTextureAt = Extensions::FrameTextureIndices();
		std::array<ID3D11ShaderResourceView*, Extensions::kFrameTextureSlots> frameTextureViews{};
		for (std::size_t i = 0; i < frameTextures.size() && !depthOnly; ++i)
			frameTextureViews[i] = frameTextures[i].view ? static_cast<ID3D11ShaderResourceView*>(frameTextures[i].view()) : nullptr;
		// Graph buffers use the published buffers' indices, so the D3D11 views captured there are not imported.
		const auto graphBuffers = Extensions::RegisteredFrameGraphBuffers();
		std::array<bool, kTextureRegisters> onGraph{};
		for (std::size_t b = 0; b < graphBuffers.size() && !depthOnly; ++b)
			if ((epochResources->graphBuffers >> b) & 1)
				for (std::uint32_t r = 0; r < graphBuffers[b].count; ++r)
					if (graphBuffers[b].firstRegister + r < kTextureRegisters)
						onGraph[graphBuffers[b].firstRegister + r] = true;
		// A frame buffer copy is the graph buffer the colour passes declare, so the captured D3D11 buffer is never imported.
		std::array<bool, kTextureRegisters> copied{};
		for (const auto& frameBuffer : depthOnly ? decltype(epochResources->frameBuffers){} : epochResources->frameBuffers)
			if (frameBuffer.textureRegister < kTextureRegisters)
				copied[frameBuffer.textureRegister] = true;
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t) {
			auto& held = frameTextureBindings[t];
			const auto at = frameTextureAt[t];
			const auto* const texture = at != Extensions::kNoFrameTexture ? &frameTextures[at] : nullptr;
			const bool named = texture && texture->view && (frameTextureViews[at] || !texture->capturedWhenNull);
			auto* const view = t == kCharacterLightRegister ? Scene::FrameView::Get().GetFrameCapture().characterLightView :
			                   named                        ? frameTextureViews[at] :
			                                                  capture.psViews[t];
			// A published export the colour passes declared uses the published texture's descriptor. Nothing is imported.
			std::uint32_t graphIndex = 0;
			if (onGraph[t] || copied[t]) {
				HoldFrameTexture(held.view, held.binding, nullptr, t, 0u, a_commit.bindingOwners);
				continue;
			}
			if (texture &&
				Extensions::FrameTextureIndex(Extensions::GraphTextureReader::Colour, t, *texture, view, graphIndex)) {
				HoldFrameTexture(held.view, held.binding, nullptr, t, 0u, a_commit.bindingOwners);
				a_frameTextures[t] = graphIndex;
				continue;
			}
			// The character light's noise is an HDR downsample engine target, which the colour passes declare.
			if (t == kCharacterLightRegister && (Extensions::AdaptedLuminanceTextureIndex(Extensions::GraphTextureReader::Colour, t, view, graphIndex) ||
													Extensions::EngineTargetTextureIndex(Extensions::GraphTextureReader::Colour, t, "character light", view,
														Extensions::CharacterLightGraphTextureIds(), graphIndex))) {
				HoldFrameTexture(held.view, held.binding, nullptr, t, 0u, a_commit.bindingOwners);
				a_frameTextures[t] = graphIndex;
				continue;
			}
			// Wetness Effects' precipitation occlusion at t70 is the engine's depth target, which the colour passes declare.
			if (t == Extensions::kPrecipitationOcclusionRegister &&
				Extensions::PrecipitationOcclusionTextureIndex(Extensions::GraphTextureReader::Colour, t, view, graphIndex)) {
				HoldFrameTexture(held.view, held.binding, nullptr, t, 0u, a_commit.bindingOwners);
				a_frameTextures[t] = graphIndex;
				continue;
			}
			if (view && view != held.view)
				NoteExportRead(t, view);
			a_frameTextures[t] = HoldFrameTexture(held.view, held.binding, view, t, view ? 0u : textures.NullIndex(), a_commit.bindingOwners);
		}
		// The colour passes declared these graph buffers, so the reads are ordered. Only the indices come from this commit.
		for (std::size_t b = 0; b < graphBuffers.size() && !depthOnly; ++b) {
			if (!((epochResources->graphBuffers >> b) & 1))
				continue;
			bool indexed = false;
			{
				RenderGraph::UndeclaredAccess::DeclaredIndexScope declaredIndices;
				indexed = graphBuffers[b].indices(std::span(a_frameTextures).subspan(graphBuffers[b].firstRegister, graphBuffers[b].count));
			}
			// The producer left the graph since the epoch was built. Use the captured views until the next setup.
			for (std::uint32_t r = 0; !indexed && r < graphBuffers[b].count; ++r) {
				const std::uint32_t t = graphBuffers[b].firstRegister + r;
				if (t >= kTextureRegisters)
					continue;
				auto& held = frameTextureBindings[t];
				auto* const view = capture.psViews[t];
				a_frameTextures[t] = HoldFrameTexture(held.view, held.binding, view, t, view ? 0u : textures.NullIndex(), a_commit.bindingOwners);
			}
		}
		for (const auto& frameBuffer : depthOnly ? decltype(epochResources->frameBuffers){} : epochResources->frameBuffers) {
			auto* buffer = BufferOf(capture.psViews[frameBuffer.textureRegister]);
			// The mirror sees only writes after it starts watching, so a buffer's first watched frame reads zero and is no defect.
			const bool firstWatched = mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			// An unfillable copy reads zero but is still written, because the latched copies name it.
			if (!LatchFrameBuffer(a_commit.latched, frameBuffer, contents) && !firstWatched)
				NoteDrawInputs(fmt::format("frame buffer t{} ({} elements of {} bytes) is not filled: the mirror holds {} bytes of it", frameBuffer.textureRegister,
					frameBuffer.elements, frameBuffer.stride, contents.size()));
			a_frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
		}
	}

	// Runs ahead of every pass in this epoch.
	void IndirectDraws::Impl::UploadSceneStreams(const MainCommit& a_commit)
	{
		const auto& adopted = a_commit.adopted;
		const auto& epochResources = a_commit.resources;
		const bool depthOnly = a_commit.depthOnly;
		Extensions::UploadVertexStream(adopted, a_commit.payload.body->streamRows, *epochResources->scene, a_commit.uploads);
		Extensions::RunCommitHooks({ depthOnly ? Api::CommitKind::Depth : Api::CommitKind::Colour, adopted, Scene::FrameView::Get().GetTables(),
			Scene::FrameView::Get().SceneStep(), *epochResources->scene, a_commit.uploads, &a_commit.latched, a_commit.bindingOwners });
	}

	void IndirectDraws::Impl::ZeroCommitCounts(const MainCommit& a_commit)
	{
		const auto& epochResources = a_commit.resources;
		const auto& payload = a_commit.payload;
		auto& latched = a_commit.latched;
		auto& uploads = a_commit.uploads;
		const bool depthOnly = a_commit.depthOnly;
		// The colour segment must clear only its own draw word, or it would erase the depth phases' counters before anything read them.
		const std::size_t zeroBytes = depthOnly ? sizeof(kZeroCounts) : sizeof(std::uint32_t);
		latched(epochResources->count, kZeroCounts, zeroBytes, 0);
		// Phase 2 dispatches no groups until phase 1 appends to the retest list.
		if (depthOnly && epochResources->retestDispatch)
			latched(epochResources->retestDispatch, kRetestDispatchReset, sizeof(kRetestDispatchReset), 0);
		// The sort's counts start at zero. The scan clears them afterwards.
		if (epochResources->sort)
			epochResources->sort->ZeroCountsOnce(uploads);
		// The HZB's group counter likewise only needs zeroing once.
		if (depthOnly && epochResources->hzbScratch && !epochResources->hzbScratchZeroed) {
			static constexpr std::uint32_t kZero = 0;
			uploads(epochResources->hzbScratch, &kZero, sizeof(kZero), 0);
			epochResources->hzbScratchZeroed = true;
		}
		if (!depthOnly) {
			decalWords = { payload.body->decalCount[0], payload.body->decalCount[1], 0u, 0u, payload.body->decalCount[2] };
			latched(epochResources->count, decalWords.data(), 4 * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
			latched(epochResources->count, &decalWords[4], sizeof(std::uint32_t), kCountDecalLayerWord * sizeof(std::uint32_t));
			latched(epochResources->count, kZeroCounts, kTerrainGroups * sizeof(std::uint32_t), kCountTerrainGroupWord * sizeof(std::uint32_t));
			latched(epochResources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountForwardWord * sizeof(std::uint32_t));
			latched(epochResources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountRefractionWord * sizeof(std::uint32_t));
			latched(epochResources->count, kZeroCounts, 2 * sizeof(std::uint32_t), kCountClassUnplannedWord * sizeof(std::uint32_t));
			latched(epochResources->count, kZeroCounts, 4 * sizeof(std::uint32_t), kCountDeferredTestedWord * sizeof(std::uint32_t));
			// A forward bucket's count word follows the classes'.
			if (const auto words = a_commit.shape.classPlan ? a_commit.shape.classPlan->Words() : static_cast<std::uint32_t>(a_commit.shape.classes.size()) * kClassRanges;
				words && epochResources->classCounts) {
				classCountZeros.assign(words, 0u);
				latched(epochResources->classCounts, classCountZeros.data(), std::size_t(words) * sizeof(std::uint32_t), 0);
			}
		}
	}

	// Writes the depth segment's bucket map and phase tables into the latch. Returns the index pool's copies.
	// A slot without a published pipeline maps to none, and BuildDraws drops its draws.
	std::uint32_t IndirectDraws::Impl::WriteDepthBuckets(const MainCommit& a_commit, const org::RunRef& a_latchRun, BuildDrawsLatch& a_latch)
	{
		const auto& epochResources = a_commit.resources;
		const auto& writePlan = a_commit.writePlan;
		const auto& latchBlock = a_commit.latchBlock;
		const auto& layout = a_commit.latchLayout;
		const std::uint32_t poolCopies = UpdateIndexPool(a_commit.adopted, *epochResources->pool, Scene::FrameView::Get().GetTables(),
			Scene::FrameView::Get().GetTablesGeneration(), latchBlock, a_latchRun, layout.PoolOffset(), a_commit.uploads);
		// Slots added since the plan map to none.
		const auto planned = static_cast<std::uint32_t>(writePlan.map.size());
		auto slots = std::max(planned, InstalledPipelineSlots());
		if (slots > layout.buckets)
			slots = layout.buckets;
		const std::uint32_t buckets = writePlan.Buckets();
		// These belong to the snapshot, so only write them when this frame slot's region holds another snapshot's.
		auto& held = mainBucketRegions.Of(a_latchRun, a_commit.shape.latch, a_commit.revisionShape, slots);
		if (!Draws::LatchRegionsHeld::Hold(held, 0)) {
			WriteBucketMap(latchBlock, "main bucket map", a_latchRun, layout.BucketMapOffset(), writePlan.map, slots);
			if (buckets) {
				const std::size_t words = std::size_t(buckets) * 2;
				LatchWrite(latchBlock, "main bucket tables", a_latchRun, layout.BucketTableOffset(), std::as_bytes(std::span(writePlan.table.data(), words)));
				LatchWrite(latchBlock, "main bucket tables", a_latchRun, layout.PhaseTwoBucketTableOffset(), std::as_bytes(std::span(writePlan.table.data() + words, words)));
			}
		}
		if (buckets) {
			zBucketZeros.assign(buckets, 0u);
			for (const auto& counts : epochResources->zBucketCounts)
				a_commit.latched(counts, zBucketZeros.data(), std::size_t(buckets) * sizeof(std::uint32_t), 0);
		}
		const auto region = static_cast<std::uint32_t>(latchBlock.Offset(a_latchRun));
		a_latch.bucketTableOffset = region + layout.BucketTableOffset();
		a_latch.phaseTwoBucketTableOffset = region + layout.PhaseTwoBucketTableOffset();
		a_latch.bucketMapOffset = region + layout.BucketMapOffset();
		return poolCopies;
	}

	void IndirectDraws::Impl::WriteShadowLatch(const MainCommit& a_commit, const org::RunRef& a_latchRun, BuildDrawsLatch& a_latch)
	{
		const auto& latchBlock = a_commit.latchBlock;
		const auto& layout = a_commit.latchLayout;
		const std::uint32_t header[4] = { static_cast<std::uint32_t>(sunCascades.size()), 0, 0, 0 };
		LatchWrite(latchBlock, "main sun cascades", a_latchRun, MainLatchLayout::CascadeOffset(), std::as_bytes(std::span(header)));
		if (!sunCascades.empty())
			LatchWrite(latchBlock, "main sun cascades", a_latchRun, MainLatchLayout::CascadeOffset() + kSunRegionHeader, std::as_bytes(std::span(sunCascades)));
		const std::uint32_t volumeHeader[4] = { shadowVolumesLatched, 0, 0, 0 };
		LatchWrite(latchBlock, "main shadow volumes", a_latchRun, layout.ShadowVolumeOffset(), std::as_bytes(std::span(volumeHeader)));
		if (shadowVolumesLatched)
			LatchWrite(latchBlock, "main shadow volumes", a_latchRun, layout.ShadowVolumeOffset() + kSunRegionHeader,
				std::as_bytes(std::span(shadowVolumes).first(shadowVolumesLatched)));
		a_latch.sunState = kSunTestOn;
		a_latch.sunCascadeOffset = static_cast<std::uint32_t>(latchBlock.Offset(a_latchRun)) + MainLatchLayout::CascadeOffset();
		a_latch.localShadowOffset = static_cast<std::uint32_t>(latchBlock.Offset(a_latchRun)) + layout.ShadowVolumeOffset();
		Diagnostics::Notify(Diagnostics::CullLatchWritten{ a_latch, false });
	}

	// Writes the revision's class table and map, which BuildDraws routes every colour draw by.
	// Only written when this frame slot's region holds another revision's. Without a plan every colour draw is dropped.
	void IndirectDraws::Impl::WriteClassRegion(const MainCommit& a_commit, const org::RunRef& a_latchRun, BuildDrawsLatch& a_latch)
	{
		const auto& latchBlock = a_commit.latchBlock;
		const auto& layout = a_commit.latchLayout;
		const auto& plan = a_commit.shape.classPlan;
		if (!plan || plan->classes.size() > layout.classes || plan->map.size() > kClassHeaderWords + layout.classMapWords)
			return;  // never: the revision reserved its layout for its plan (RequireMainClasses)
		auto& held = mainClassRegions.Of(a_latchRun, a_commit.shape.latch, a_commit.revisionShape, 0);
		if (!Draws::LatchRegionsHeld::Hold(held, 0)) {
			LatchWrite(latchBlock, "main class table", a_latchRun, layout.ClassTableOffset(), std::as_bytes(std::span(plan->table)));
			LatchWrite(latchBlock, "main class map", a_latchRun, layout.ClassMapOffset(), std::as_bytes(std::span(plan->map)));
		}
		const auto region = static_cast<std::uint32_t>(latchBlock.Offset(a_latchRun));
		a_latch.classTableOffset = region + layout.ClassTableOffset();
		a_latch.classMapOffset = region + layout.ClassMapOffset();
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, const MainInputs& a_frame, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats,
		std::vector<std::shared_ptr<const void>>& a_bindingOwners, const EpochRevision& a_revision)
	{
		DCLF_ZONE("CS.DCLF.CommitMainPayload");
		// Held until this execution retires.
		for (auto& owner : Scene::FrameView::Get().TakeRetiredImports())
			a_bindingOwners.push_back(std::move(owner));
		// Per-frame values come from a_frame because a payload built ahead is a frame old.
		const auto& in = a_frame;
		const auto& tables = Scene::FrameView::Get().GetTables();
		const bool depthOnly = in.depthOnly;
		// The installed PayloadVersions already hold the committed payload. Nothing of it is uploaded here.
		const PayloadVersions& parts = *InstalledPayload();
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && prepassInputs;
		const auto& adopted = Adopted();
		CommitUploads uploads(commitStagedPool, adopted, &a_resources->scene->content);
		const std::size_t latchedShape = depthOnly ? kDepthShape : kColourShape;
		// The selected revision's shape is trusted. Nothing is checked against it.
		const auto revisionEpoch = EpochOfShape(ShapeKind::Main, static_cast<std::uint32_t>(latchedShape));
		const auto revisionShape = a_revision.shape->Value<PassFrame>();
		const PassFrame& shape = *revisionShape;
		CommitShadowInputs(depthOnly, shape, revisionEpoch);
		static const ZBucketPlan kNoPlan;
		const ZBucketPlan& writePlan = shape.zPlan ? *shape.zPlan : kNoPlan;
		const org::LatchBlock& latchBlock = *shape.latch;
		const MainLatchLayout& latchLayout = shape.latchLayout;
		// Per-frame values go through the latch. The epoch's first pass copies them.
		auto latched = LatchedUploads(shape.latched, uploads, RenderGraphRuntime::Get().Host()->CurrentRun());
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		const MainCommit commit{ a_capture, in, a_payload, a_resources, a_stats, a_bindingOwners, tables, parts, adopted, uploads, latched, textures, mirror, shape,
			revisionShape, writePlan, latchBlock, latchLayout, frameNumber, depthOnly, replayVertexInputs };

		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		TracyCZoneN(cTex, "CS.DCLF.CMP.FrameTextures", true);
		ResolveFrameTextures(commit, frameTextures);
		CountMissingFrameTextures(commit, frameTextures);
		// Unresolved registers read zero through a null descriptor of their view's kind.
		if (!depthOnly)
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t)
				if (frameTextures[t] == kInvalidIndex)
					frameTextures[t] = textures.NullIndexFor(frameTextureBindings[t].view);
		WriteFrameRecord(commit, frameTextures);
		lap(2);
		TracyCZoneEnd(cTex);

		TracyCZoneN(cSlots, "CS.DCLF.CMP.LatchFrameSlots", true);
		LatchFrameSlots(commit, a_blocks);
		TracyCZoneEnd(cSlots);

		lap(3);
		TracyCZoneN(cStreams, "CS.DCLF.CMP.SceneStreamsAndHooks", true);
		// The visibility readback reads the root states the depth epoch steps, so it is armed after the commit hooks.
		UploadSceneStreams(commit);
		TracyCZoneEnd(cStreams);
		TracyCZoneN(cCounts, "CS.DCLF.CMP.CountsAndStats", true);
		if (depthOnly)
			ArmVisibilityReadback(*a_resources->scene, uploads);
		ZeroCommitCounts(commit);
		CopyResidentStats(commit);

		lap(4);
		CopyBuildStats(commit);
		lap(6);
		lap(5);

		TracyCZoneEnd(cCounts);
		TracyCZoneN(cLatch, "CS.DCLF.CMP.CullLatch", true);
		MainCullInputs cull;
		CountMainDraws(commit, prepassVS[kPerFrameVertexRegister], cull);

		BuildDrawsLatch latch{};
		FillCullLatch(commit, cull, latch);
		const org::RunRef latchRun = RenderGraphRuntime::Get().Host()->CurrentRun();
		std::uint32_t poolCopies = 0;
		if (depthOnly && a_resources->pool)
			poolCopies = WriteDepthBuckets(commit, latchRun, latch);
		if (!depthOnly) {
			WriteShadowLatch(commit, latchRun, latch);
			WriteClassRegion(commit, latchRun, latch);
		}
		LatchWriteValue(latchBlock, "main culling latch", latchRun, 0, latch);
		// A view drawing this frame's depth commit checks against these.
		if (depthOnly)
			a_resources->depthCommitted = { frameNumber, cull.inputCount, adopted.Serial() };

		{
			DCLF_ZONE("CS.DCLF.CMP.LatchedFinish");
			(void)latched.Finish();
		}
		TracyCZoneEnd(cLatch);
		revisions.latchedMisses += latched.Misses();
		// Commit the view epochs this one hosts, such as reflection faces and first person depth. Their latches and owners join this execution.
		std::uint32_t hostedDrawn = 0;
		if (const auto hosts = EpochTraitsOf(revisionEpoch).hostsMask; hosts && hostedWork && *hostedWork) {
			hostedCommit = { revisionEpoch, &a_bindingOwners, &a_payload, 0 };
			(*hostedWork)();
			hostedDrawn = std::exchange(hostedCommit, {}).drawn & hosts;
			for (std::uint32_t e = 0, epochs = EpochCount(); e < epochs; ++e)
				revisions.submitted[e] += (hostedDrawn >> e) & 1;
		}
		// Without the index pool's dispatch when this commit gave out no range, since it would be zero groups.
		// The CPU pipeline's graph has no recording. Its site draws the frame from this commit.
		if (a_revision.direct) {
			// The site executes the CPU pipeline passes in DXVK's buffer ahead of its draws.
			if (a_revision.recordings && !a_revision.recordings->recordings.empty()) {
				const auto& recordings = *a_revision.recordings;
				const std::size_t variant = depthOnly && !poolCopies && recordings.Variants() > 1 ? 1 : 0;
				SubmitRevisionRecording(revisionEpoch, recordings, recordings.IndexOf(variant, hostedDrawn));
			}
			const Direct::MainCommitInputs site{ revisionShape, a_resources, &adopted, &latch, sunCascades, std::span(shadowVolumes.data(), shadowVolumesLatched), latchRun,
				poolCopies };
			if (depthOnly)
				Direct::DepthCommitted(site);
			else
				Direct::ColourCommitted(site);
			lap(6);
			return true;
		}
		// The GPU-driven graph's forward families draw on the CPU pipeline's sites, from this commit.
		const Direct::MainCommitInputs forwardSite{ revisionShape, a_resources, &adopted, &latch, sunCascades, std::span(shadowVolumes.data(), shadowVolumesLatched), latchRun,
			poolCopies };
		if (depthOnly)
			Direct::DepthCommitted(forwardSite);
		else
			Direct::ColourCommitted(forwardSite);
		const auto& recordings = *a_revision.recordings;
		const std::size_t variant = depthOnly && !poolCopies && recordings.Variants() > 1 ? 1 : 0;
		SubmitRevisionRecording(revisionEpoch, recordings, recordings.IndexOf(variant, hostedDrawn));
		lap(6);
		return true;
	}

}

