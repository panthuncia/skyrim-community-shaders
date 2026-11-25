// The shadow epoch, on the render thread. Each frame it commits the installed shadow payload, writes a culling latch
// per shadow view and occlusion map, uploads the arena, and submits the recorded work.
// Also here: the shadow inputs, the occlusion maps' readiness check and the culling counter readback.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/ShadowSite.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ExecutionOwners.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "RenderGraph/Fatal.h"

namespace DCLF
{
	namespace Draws
	{
		/** @brief The latch fields every shadow view shares. The caller sets the culling flags and planes. */
		BuildDrawsLatch ShadowViewLatch(const PendingView& a_view, std::uint32_t a_inputs, std::uint32_t a_frame, std::uint32_t a_seedsIndex)
		{
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (a_inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = a_inputs;
			latch.visibilityStamp = a_frame & 0x0FFFFFFFu;  // 28 bits, BuildDrawsCS keeps flags in the top bits
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = a_seedsIndex;
			LatchCullCamera(latch, a_view.viewProj, a_view.eye);
			return latch;
		}

		namespace
		{
			/**
			 * @brief The smallest caster radius in world units that covers a_minTexels in a sun view, or 0 for no test.
			 * Only orthographic projections have one texel size, so others are not tested.
			 */
			float SunCasterMinRadius(const float (&a_viewProj)[16], std::uint32_t a_width, std::uint32_t a_height, float a_minTexels)
			{
				if (!(a_minTexels > 0.0f) || !a_width || !a_height)
					return 0.0f;
				const auto rowLength = [&](std::uint32_t a_row) {
					const float* row = a_viewProj + a_row * 4;
					return std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
				};
				if (rowLength(3) > 1e-6f)
					return 0.0f;
				const float texelsPerUnit = std::max(rowLength(0) * 0.5f * static_cast<float>(a_width), rowLength(1) * 0.5f * static_cast<float>(a_height));
				return texelsPerUnit > 0.0f ? a_minTexels / texelsPerUnit : 0.0f;
			}
		}

		/** @brief Points the latch at the view's bucket table, and writes the table when a_write is set. */
		void WriteBucketTable(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const org::RunRef& a_latchRun, std::uint32_t a_slot,
			std::span<const ShadowBucket> a_buckets, bool a_write, BuildDrawsLatch& a_latch)
		{
			if (a_buckets.size() > a_layout.keySlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} shadow buckets past the latch's {} key slots", a_buckets.size(), a_layout.keySlots));
			const std::uint32_t offset = a_layout.BucketOffset(a_slot);
			a_latch.bucketTableOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchRun)) + offset;
			if (!a_write || a_buckets.empty())
				return;
			std::vector<std::uint32_t> table;
			table.reserve(a_buckets.size() * 2);
			for (const auto& bucket : a_buckets) {
				table.push_back(bucket.first);
				table.push_back(bucket.capacity);
			}
			LatchWrite(a_latchBlock, "shadow bucket tables", a_latchRun, offset, std::as_bytes(std::span(table)));
		}

		// Which latch regions already hold this revision's tables. Render thread only.
		// Part ids are 2 * view slot for a bucket table and 2 * map row + 1 for a map row.
		LatchRegionsHeld shadowLatchRegions;
		// The arena bytes past the frame record that the constants buffer holds now. Render thread only.
		struct ShadowArenaHeld
		{
			std::weak_ptr<const void> target;
			std::uint64_t serial = 0;
			std::vector<std::byte> bytes;
		} shadowArenaHeld;
		// A new arena mostly repeats the last one, so it is compared in chunks and only differing runs are staged.
		constexpr std::size_t kArenaChunk = 256;

		/**
		 * @brief Points the latch at its rasterizer state's pipeline-map row, and writes the row when a_write is set.
		 * The offset is into the whole latch block, so it includes the frame slot's base.
		 */
		void UseShadowMapRow(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const RowBuckets& a_buckets, const org::RunRef& a_latchRun,
			std::uint32_t a_mode, std::uint32_t a_rasterState, bool a_write, BuildDrawsLatch& a_latch)
		{
			const auto& layout = a_layout;
			// ReserveShadowLatch gives every registered state a row in every mode.
			if (a_rasterState == 0 || a_rasterState > layout.rasterStates || a_mode >= kShadowModeCount)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("shadow view rasterizer state {} of mode {} past the latch's {} rows", a_rasterState, a_mode,
					layout.rasterStates));
			const std::uint32_t mapRowOffset = layout.MapOffset() + layout.MapRowOf(a_mode, a_rasterState) * layout.MapRowBytes();
			a_latch.pipelineMapOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchRun)) + mapRowOffset;
			if (!a_write)
				return;
			const auto& row = a_buckets.bucketOfSlot;
			if (row.size() > layout.keySlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} shadow key slots past the latch's {}", row.size(), layout.keySlots));
			if (!row.empty())
				LatchWrite(a_latchBlock, "shadow map rows", a_latchRun, mapRowOffset, std::as_bytes(std::span(row)));
		}

		/**
		 * @brief Points the latch at the key slots' vertex layouts, and writes them when a_write is set.
		 * Slots past the table stay zero, so their draws use the bucket's pushed layout.
		 */
		void UseSlotLayouts(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const ShadowFrame& a_frame, const org::RunRef& a_latchRun,
			bool a_write, BuildDrawsLatch& a_latch)
		{
			a_latch.slotLayoutsOffset = static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchRun)) + a_layout.SlotLayoutsOffset();
			if (!a_write || !a_frame.slotLayouts || a_frame.slotLayouts->empty())
				return;
			const auto& layouts = *a_frame.slotLayouts;
			const std::size_t count = std::min<std::size_t>(layouts.size(), a_layout.keySlots);
			LatchWrite(a_latchBlock, "shadow slot layouts", a_latchRun, a_layout.SlotLayoutsOffset(), std::as_bytes(std::span(layouts.data(), count)));
		}

		/** @brief Writes the sun's full-frustum processes into the latch. Returns their offset into the block. */
		std::uint32_t WriteSunEntryRegion(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const org::RunRef& a_latchRun,
			std::span<const SunEntryProcess> a_processes)
		{
			const auto& layout = a_layout;
			if (a_processes.size() > layout.sunProcesses)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} sun full-frustum processes past the latch's {}", a_processes.size(), layout.sunProcesses));
			const std::uint32_t header[4] = { static_cast<std::uint32_t>(a_processes.size()), 0, 0, 0 };
			LatchWrite(a_latchBlock, "shadow sun processes", a_latchRun, layout.SunEntryOffset(), std::as_bytes(std::span(header)));
			if (!a_processes.empty())
				LatchWrite(a_latchBlock, "shadow sun processes", a_latchRun, layout.SunEntryOffset() + kSunRegionHeader, std::as_bytes(a_processes));
			return static_cast<std::uint32_t>(a_latchBlock.Offset(a_latchRun)) + layout.SunEntryOffset();
		}

		/** @brief Writes a view's constant blocks into its latch region, for ShadowLatchedCopiesPass to copy. */
		void WriteViewBlocks(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const org::RunRef& a_latchRun, std::uint32_t a_slot,
			const PendingView& a_view)
		{
			const std::uint32_t offset = a_layout.ViewBlockOffset(a_slot);
			LatchWrite(a_latchBlock, "shadow view blocks", a_latchRun, offset, std::as_bytes(std::span(a_view.viewBlock)));
			// DCLFDepthRange in PerTechnique c3. The shader applies the depth range under a [0, 1] viewport,
			// so a depth range change needs no new recording.
			const float depthRange[4] = { a_view.minDepth, a_view.maxDepth - a_view.minDepth, 0.0f, 0.0f };
			LatchWrite(a_latchBlock, "shadow view blocks", a_latchRun, offset + static_cast<std::uint32_t>(sizeof(a_view.viewBlock)), std::as_bytes(std::span(depthRange)));
			LatchWrite(a_latchBlock, "shadow per-frame view data", a_latchRun, offset + static_cast<std::uint32_t>(kShadowPerFrameOffset), std::span(a_view.perFrame.data(), a_view.perFrameBytes));
		}

		/**
		 * @brief Writes the epoch's single culling dispatch, which covers every view. X spans the whole input list
		 * and Y has one group row per view of the submitted variant.
		 */
		void WriteBuildDispatch(const org::LatchBlock& a_latchBlock, const ShadowLatchLayout& a_layout, const org::RunRef& a_latchRun, std::uint32_t a_inputs,
			std::uint32_t a_inputsIndex, const ShadowFrame& a_shape)
		{
			const std::uint32_t dispatch[4] = { (a_inputs + 63) / 64, ShadowBuildRows(a_shape), 1, 0 };
			LatchWrite(a_latchBlock, "shadow culling dispatch", a_latchRun, a_layout.BuildDispatchOffset(), std::as_bytes(std::span(dispatch)));
			// The caster rows pass runs over the list before the culling reads its output.
			const std::uint32_t rowsDispatch[4] = { (a_inputs + 63) / 64, 1, 1, 0 };
			LatchWrite(a_latchBlock, "shadow caster rows dispatch", a_latchRun, a_layout.RowsDispatchOffset(), std::as_bytes(std::span(rowsDispatch)));
			const std::uint32_t list[4] = { a_inputsIndex, a_inputs, 0, 0 };
			LatchWrite(a_latchBlock, "shadow caster rows list", a_latchRun, a_layout.RowsInputsOffset(), std::as_bytes(std::span(list)));
		}
	}

	bool IndirectDraws::OcclusionReady(std::uint32_t a_view) const
	{
		// Called before the engine draws the map. This frame's view layout is not set up yet, so the check uses last frame's.
		// If the layout changes anyway, LatchOcclusionViews skips the map and the engine renders it.
		const auto& engine = *impl->shadowEngine;
		if (a_view >= kOcclusionViews || !Extensions::OcclusionOn(a_view) || !engine.shadow ||
			!engine.shadow->depth[OcclusionDepthTarget(a_view)])
			return false;
		// The built graph may lack the map's import while a rebuild is pending.
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || RenderGraphRuntime::Get().RebuildRequestedUnstaged())
			return false;
		std::optional<ShadowViewLayout> predicted;
		for (const auto& layout : engine.PredictedOcclusion(true))
			if (layout.slot == OcclusionSlot(a_view))
				predicted = layout;
		bool shaped = false;
		const std::uint32_t epoch = EpochOfShape(ShapeKind::Shadow, kShadowShape);
		if (predicted && impl->EpochCovered(epoch) && impl->installedDraws && impl->installedDraws->shadow)
			if (const auto& active = impl->revisions.active)
				if (const auto& fragment = active->Fragment(Impl::SceneRevisions::kShapeSlot + epoch))
					if (const auto variants = fragment->Value<ShadowVariants>(); variants && !variants->shapes.empty() && variants->shapes.front())
						for (const auto& layout : LayoutOf(*variants->shapes.front()))
							shaped = shaped || layout == *predicted;
		if (!shaped) {
			++impl->occlusionUnrecorded;
			return false;
		}
		return true;
	}

	namespace Draws
	{
		namespace
		{
			/** @brief State shared by the stages of the shadow commit. */
			struct ShadowCommit
			{
				ShadowResources& resources;
				ShadowPayload& payload;
				const PayloadVersions& parts;
				const NamedVersions& adopted;
				CommitUploads& uploads;
				const ShadowFrame& target;
				const ShadowInputs& in;
				const std::vector<PendingView>& pending;
				std::uint32_t frameNumber;
				IndirectDraws::ShadowStats& stats;
			};

			/** @brief Uploads the vertex streams, runs the commit hooks and copies the payload's counts into the stats. */
			void CommitShadowShared(const ShadowCommit& a_commit, const std::array<bool, kShadowModeCount>& a_modeUsed,
				std::vector<std::shared_ptr<const void>>& a_frameOwners)
			{
				auto& payload = a_commit.payload;
				auto& shadowStats = a_commit.stats;
				const auto& adopted = a_commit.adopted;
				auto& uploads = a_commit.uploads;
				auto& scene = *a_commit.resources.scene;
				shadowStats.faceUploads += Extensions::UploadVertexStream(adopted, payload.streamRows, scene, uploads);
				Extensions::RunCommitHooks({ Api::CommitKind::Shadow, adopted, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().SceneStep(), scene, uploads, nullptr,
					a_frameOwners });
				shadowStats.records = static_cast<std::uint32_t>(payload.materialRows.Count());
				shadowStats.skippedTexture = payload.skippedTexture;
				shadowStats.skippedPipeline = payload.skippedPipeline;
				shadowStats.deferredTextures = payload.deferredTextures;
				shadowStats.deferredPipelines = payload.deferredPipelines;
				std::size_t casterInputs = 0;
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					if (a_modeUsed[m])
						casterInputs += payload.ModeInputs(m);
				shadowStats.inputs = static_cast<std::uint32_t>(casterInputs);
			}

			/**
			 * @brief Writes each pending view's blocks and culling latch, and a zero latch for placements with no view this frame.
			 * Returns the drawn views as bits by index into the shape's views.
			 */
			std::uint64_t LatchShadowViews(const ShadowCommit& a_commit, const org::RunRef& a_latchRun, const std::shared_ptr<const ShadowFrame>& a_revisionShape,
				const std::vector<ShadowViewLayout>& a_layouts, std::vector<Direct::ShadowSiteView>* a_direct)
			{
				auto& resources = a_commit.resources;
				auto& payload = a_commit.payload;
				auto& shadowStats = a_commit.stats;
				const auto& adopted = a_commit.adopted;
				const auto& target = a_commit.target;
				const std::uint32_t frameNumber = a_commit.frameNumber;
				const org::RunRef latchRun = a_latchRun;
				const auto& layouts = a_layouts;
				auto& scene = *resources.scene;
				resources.labels.clear();
				const auto& latchBlock = *target.latch;
				const auto& latchLayout = target.latchLayout;
				{
					DCLF_ZONE("CS.DCLF.ShadowInputs.IndexPool");
					UpdateIndexPool(adopted, *resources.pool, Scene::FrameView::Get().GetTables(), Scene::FrameView::Get().GetTablesGeneration(), latchBlock, latchRun,
						latchLayout.PoolOffset(), a_commit.uploads);
				}
				// Bucket tables and map rows are written only where this frame slot's region does not hold them yet.
				auto& held = Draws::shadowLatchRegions.Of(latchRun, target.latch, a_revisionShape, 0);
				std::uint32_t sunEntryOffset = 0;
				std::vector<bool> slotDrawn(layouts.size());
				std::uint64_t active = 0;
				const std::uint32_t seedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, scene, frameNumber, adopted);
				const std::uint32_t statesIndex = CullBindingIndex(Api::CullBindingSlot::RootStates, scene, frameNumber, adopted);
				const auto roots = Extensions::RootListOf(Scene::FrameView::Get().GetTables());
				// Casters of a portal-strict light must pass its gates. An index of 0 means nothing is gated.
				const std::uint32_t contextsIndex = CullBindingIndex(Api::CullBindingSlot::LightContexts, scene, frameNumber, adopted);
				const std::uint32_t gatesIndex = CullBindingIndex(Api::CullBindingSlot::StrictGates, scene, frameNumber, adopted);
				const float minTexels = shadowMinTexels.load(std::memory_order_relaxed);
				for (const auto& view : a_commit.pending) {
					DCLF_ZONE("CS.DCLF.ShadowInputs.View");
					const std::uint32_t slot = view.slot;
					const std::uint32_t index = slot - kFirstShadowViewSlot;
					slotDrawn[index] = true;
					active |= index < 64 ? 1ull << index : 0ull;
					WriteViewBlocks(latchBlock, latchLayout, latchRun, slot, view);
					// Every view culls the whole shadow list. Its view bits pick out its mode's inputs.
					const auto inputCount = static_cast<std::uint32_t>(payload.Inputs());
					// Point-light views do not test the caster class.
					auto latch = ShadowViewLatch(view, inputCount, frameNumber, seedsIndex);
					const std::uint32_t classFlags = view.casterClass ? kCullVolumetricOnly : view.modeIndex == kParabolicShadowMode ? 0u : kCullCastersOnly;
					latch.cullFlags = (view.hasViewProj ? (1u | (view.renderMode == 0xE ? kCullNoNearPlane : 0u)) : 0u) | classFlags |
					                  (view.sunView ? kCullSunEntry : 0u) | KeyWordOf(view.modeIndex);
					// Only sun views drop casters below the texel threshold. Occlusion maps use their own minRadius.
					if (view.sunView && view.hasViewProj)
						if (const float radius = SunCasterMinRadius(latch.viewProj, view.width, view.height, minTexels); radius > 0.0f) {
							latch.cullFlags |= kCullMinTexels;
							latch.minCasterRadius = radius;
						}
					latch.viewBits = ShadowViewBits(view.modeIndex, view.sunView);
					// A focus view draws only its target's members. The latch stores root + 1, and an unlisted target draws nothing.
					if (view.focus) {
						const auto root = view.focusTarget ? roots.Find(view.focusTarget) : kNoFadeRoot;
						latch.focusRoot = root != kNoFadeRoot ? root + 1 : kFocusRootNone;
					}
					latch.cullPlaneMask = view.cullPlaneMask;
					std::memcpy(latch.cullPlanes, view.cullPlanes, sizeof(latch.cullPlanes));
					// A paraboloid culls by the light's sphere and hemisphere plane in the first two plane slots.
					// Its camera frustum bounds nothing the paraboloid draws.
					if (view.parabolic) {
						latch.cullFlags |= kCullParabolic | (view.parabolicBack ? kCullParabolicBack : 0u) | (view.parabolicBothSides ? kCullParabolicBothSides : 0u);
						latch.cullPlaneMask = 0;
						std::memcpy(latch.cullPlanes[0], view.parabolicSphere.data(), sizeof(latch.cullPlanes[0]));
						std::memcpy(latch.cullPlanes[1], view.parabolicPlane.data(), sizeof(latch.cullPlanes[1]));
					}
					latch.fadeStatesIndex = statesIndex;
					if (view.portalContext && contextsIndex && gatesIndex) {
						latch.lightContextsIndex = contextsIndex;
						latch.strictGatesIndex = gatesIndex;
						latch.strictContext = view.portalContext;
					}
					ShadowPayloadLatch(a_commit.parts, latch);
					// BuildDraws tests each input's entry sphere against the sun's full-frustum processes, written once per frame.
					if (view.sunView) {
						if (!sunEntryOffset)
							sunEntryOffset = WriteSunEntryRegion(latchBlock, latchLayout, latchRun, payload.inputs.sunEntryProcesses);
						latch.sunEntryOffset = sunEntryOffset;
					}
					const std::uint32_t mapRow = view.rasterState ? latchLayout.MapRowOf(view.modeIndex, view.rasterState) : ~0u;
					const bool rowHeld = mapRow < latchLayout.MapRows() && Draws::LatchRegionsHeld::Hold(held, std::size_t(mapRow) * 2 + 1);
					UseShadowMapRow(latchBlock, latchLayout, (*target.rows)[index], latchRun, view.modeIndex, view.rasterState, !rowHeld, latch);
					UseSlotLayouts(latchBlock, latchLayout, target, latchRun, !Draws::LatchRegionsHeld::Hold(held, std::size_t(latchLayout.MapRows()) * 2 + 1), latch);
					WriteBucketTable(latchBlock, latchLayout, latchRun, slot, target.views[index].buckets, !Draws::LatchRegionsHeld::Hold(held, std::size_t(slot) * 2), latch);
					LatchWriteValue(latchBlock, "shadow culling latches", latchRun, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
					if (a_direct)
						a_direct->push_back({ view.modeIndex == kParabolicShadowMode ? Direct::ViewType::Point : view.sunView ? Direct::ViewType::Sun : Direct::ViewType::Spot,
							slot, index, latch });
					resources.labels.push_back({ view.viewId, view.renderMode, slot });
				}
				// Placements without a view this frame get a zero latch, so they do no work.
				for (std::size_t index = 0; index < layouts.size(); ++index) {
					if (slotDrawn[index])
						continue;
					LatchWriteValue(latchBlock, "shadow culling latches", latchRun, layouts[index].slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), BuildDrawsLatch{});
					++shadowStats.retainedViews;
				}
				return active;
			}

			/**
			 * @brief Latches each occlusion map whose view in the shape matches the layout set up this frame. Every other map view
			 * gets a zero latch. Returns the maps latched and adds their view bits to a_active.
			 */
			std::uint32_t LatchOcclusionViews(const ShadowCommit& a_commit, const org::RunRef& a_latchRun, const std::shared_ptr<const ShadowFrame>& a_revisionShape,
				const std::array<ShadowEngine::OcclusionState, kOcclusionViews>& a_occlusion, std::uint32_t a_maps, std::uint64_t& a_active,
				std::vector<Direct::ShadowSiteView>* a_direct)
			{
				const auto& target = a_commit.target;
				const std::uint32_t frameNumber = a_commit.frameNumber;
				const org::RunRef latchRun = a_latchRun;
				const auto& latchBlock = *target.latch;
				const auto& latchLayout = target.latchLayout;
				auto& scene = *a_commit.resources.scene;
				auto& held = Draws::shadowLatchRegions.Of(latchRun, target.latch, a_revisionShape, 0);
				const std::uint32_t seedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, scene, frameNumber, a_commit.adopted);
				const std::uint32_t statesIndex = CullBindingIndex(Api::CullBindingSlot::RootStates, scene, frameNumber, a_commit.adopted);
				static const REL::Relocation<const std::uint8_t*> fadesOn{ REL::Offset(Engine::Address::kFadesOn) };
				const auto inputCount = static_cast<std::uint32_t>(a_commit.payload.Inputs());
				const auto layouts = LayoutOf(target);
				std::uint32_t latched = 0;
				for (std::size_t index = 0; index < target.views.size(); ++index) {
					const auto& shapeView = target.views[index];
					if (!IsOcclusionMode(shapeView.modeIndex))
						continue;
					const std::uint32_t v = OcclusionOfMode(shapeView.modeIndex), slot = shapeView.slot, mode = shapeView.modeIndex;
					const auto& view = a_occlusion[v].view;
					if (!((a_maps >> v) & 1) || layouts[index] != LayoutOf(view, OcclusionSlot(v), mode, OcclusionDepthTarget(v))) {
						LatchWriteValue(latchBlock, "shadow culling latches", latchRun, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), BuildDrawsLatch{});
						continue;
					}
					WriteViewBlocks(latchBlock, latchLayout, latchRun, slot, view);
					// Match the engine's occlusion cull. Never draw a root the main camera faded out or has not seen yet.
					auto latch = ShadowViewLatch(view, inputCount, frameNumber, seedsIndex);
					const auto& map = Extensions::OcclusionMapOf(v);
					latch.cullFlags = 1u | (map.minRadius ? kCullMinRadius : 0u) | (*fadesOn.get() ? kCullFadeOnVisible : 0u) | KeyWordOf(mode);
					// Skylighting's probe grid floor, as an upward-facing cull plane at this commit's height.
					if (map.floorHeight) {
						latch.cullPlanes[0][0] = 0.0f;
						latch.cullPlanes[0][1] = 0.0f;
						latch.cullPlanes[0][2] = 1.0f;
						latch.cullPlanes[0][3] = map.floorHeight();
						latch.cullPlaneMask = 1u;
					}
					latch.viewBits = ShadowViewBits(mode, false);
					latch.fadeStatesIndex = statesIndex;
					ShadowPayloadLatch(a_commit.parts, latch);
					const std::uint32_t mapRow = latchLayout.MapRowOf(mode, view.rasterState);
					const bool rowHeld = mapRow < latchLayout.MapRows() && Draws::LatchRegionsHeld::Hold(held, std::size_t(mapRow) * 2 + 1);
					UseShadowMapRow(latchBlock, latchLayout, (*target.rows)[index], latchRun, mode, view.rasterState, !rowHeld, latch);
					UseSlotLayouts(latchBlock, latchLayout, target, latchRun, !Draws::LatchRegionsHeld::Hold(held, std::size_t(latchLayout.MapRows()) * 2 + 1), latch);
					WriteBucketTable(latchBlock, latchLayout, latchRun, slot, shapeView.buckets, !Draws::LatchRegionsHeld::Hold(held, std::size_t(slot) * 2), latch);
					LatchWriteValue(latchBlock, "shadow culling latches", latchRun, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
					// Bits 5 and 6 are the sky occluder and precipitation mask, as ViewBatches.cpp assigns them.
					if (a_direct && (map.viewBit == (1u << 5) || map.viewBit == (1u << 6)))
						a_direct->push_back({ map.viewBit == (1u << 5) ? Direct::ViewType::SkyOccluder : Direct::ViewType::PrecipOccluder, slot,
							static_cast<std::uint32_t>(index), latch });
					a_commit.stats.occlusionInputs[v] = inputCount;
					latched |= 1u << v;
					if (index < 64)
						a_active |= 1ull << index;
				}
				return latched;
			}

			/**
			 * @brief Stages the arena around the frame record and returns the record patched for this frame.
			 * The record itself goes through the latched copies, which run after the staged ones.
			 */
			std::optional<DrawBindings> UploadShadowArena(const ShadowCommit& a_commit)
			{
				auto& resources = a_commit.resources;
				auto& payload = a_commit.payload;
				const auto& parts = a_commit.parts;
				const auto& in = a_commit.in;
				auto& uploads = a_commit.uploads;
				auto& arena = payload.arena;
				if (const auto& bytes = arena.Bytes(); !bytes.empty()) {
					DrawBindings record = payload.frameRecord;
					WriteRecordBuffers(record, parts, { in.addresses.placementsIndex, in.addresses.palettesIndex, in.addresses.shadingIndex }, a_commit.adopted,
						*resources.scene, a_commit.frameNumber);
					WriteRecordBackings(record, resources.constantsRef, resources.constantsAddress, resources.constants->GetBufferSize(), parts);
					constexpr std::size_t recordEnd = kShadowFrameRecordOffset + sizeof(DrawBindings);
					static_assert(recordEnd <= kShadowArenaBlocksOffset, "the frame record overlaps the arena's blocks");
					if (bytes.size() < recordEnd)
						::RenderGraph::Fatal("Drawcall Limit Fix", "a shadow arena without its frame record");
					// A kept publication commits the same build every frame, so stage nothing then.
					// For a new build, stage only the chunks that differ.
					auto& staged = Draws::shadowArenaHeld;
					const std::shared_ptr<const void> arenaTarget = resources.constants;
					const bool sameTarget = Draws::LatchRegionsHeld::Same(staged.target, arenaTarget);
					const bool arenaHeld = sameTarget && staged.serial == payload.arenaSerial;
					if (kShadowFrameRecordOffset && !arenaHeld)
						uploads(resources.constants, bytes.data(), kShadowFrameRecordOffset, 0);
					if (!arenaHeld) {
						const auto tail = std::span<const std::byte>(bytes).subspan(recordEnd);
						if (!sameTarget)
							staged.bytes.clear();
						const std::size_t compared = std::min(staged.bytes.size(), tail.size());
						std::size_t run = 0;
						bool inRun = false;
						for (std::size_t at = 0; at < tail.size(); at += Draws::kArenaChunk) {
							const std::size_t size = std::min(Draws::kArenaChunk, tail.size() - at);
							const bool differs = at + size > compared || std::memcmp(tail.data() + at, staged.bytes.data() + at, size) != 0;
							if (differs && !inRun) {
								run = at;
								inRun = true;
							} else if (!differs && inRun) {
								uploads(resources.constants, tail.data() + run, at - run, recordEnd + run);
								inRun = false;
							}
						}
						if (inRun)
							uploads(resources.constants, tail.data() + run, tail.size() - run, recordEnd + run);
						staged.bytes.assign(tail.begin(), tail.end());
						staged.target = arenaTarget;
						staged.serial = payload.arenaSerial;
					}
					return record;
				}
				return std::nullopt;
			}

			/**
			 * @brief Latches CS's SharedData and FeatureData as they are now, with the frame record. The water reflection
			 * prepasses refresh them after the build. Returns the latch misses.
			 */
			std::uint32_t LatchShadowCsBlocks(const ShadowCommit& a_commit, const org::RunRef& a_latchRun, const std::optional<DrawBindings>& a_record)
			{
				auto& resources = a_commit.resources;
				const auto& in = a_commit.in;
				auto latched = LatchedUploads(a_commit.target.latched[ShadowFrame::kCommitList], a_commit.uploads, a_latchRun);
				if (a_record)
					latched(resources.constants, &*a_record, sizeof(DrawBindings), kShadowFrameRecordOffset);
				const auto blocks = ShadowArenaBlocksOf();
				const auto writeBlock = [&](std::uint64_t a_offset, std::uint32_t a_size, const std::vector<std::byte>& a_bytes) {
					if (!a_size)
						return;
					if (a_bytes.size() != a_size)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("a shadow commit's CS block of {} bytes for its place of {}", a_bytes.size(), a_size));
					latched(resources.constants, a_bytes.data(), a_bytes.size(), a_offset);
				};
				writeBlock(blocks.sharedData, blocks.sharedBytes, in.sharedData);
				writeBlock(blocks.featureData, blocks.featureBytes, in.featureData);
				(void)latched.Finish();
				return latched.Misses();
			}

			/** @brief Collects what the frame's execution must keep alive until it retires. */
			void HoldShadowCommit(const ShadowPayload& a_payload, const std::shared_ptr<const void>& a_snapshot, std::vector<std::shared_ptr<const void>>& a_owners)
			{
				// Copied, because a frame that keeps the publication commits this payload again.
				a_owners = a_payload.bindingOwners;
				if (a_snapshot)
					a_owners.push_back(a_snapshot);
				for (auto& owner : Scene::FrameView::Get().TakeRetiredImports())
					a_owners.push_back(std::move(owner));
			}

			/**
			 * @brief The narrowest variant that covers the active views. Variant 0 always does. Wider ones would
			 * only fetch zero draw counts and load and store unused targets.
			 */
			std::size_t NarrowestShadowVariant(const std::vector<std::shared_ptr<const ShadowFrame>>& a_variants, std::uint64_t a_active,
				IndirectDraws::ShadowStats& a_stats)
			{
				std::size_t variant = 0;
				int narrowest = 65;
				for (std::size_t v = 0; v < a_variants.size(); ++v)
					if (const auto mask = a_variants[v]->activeViews; (a_active & ~mask) == 0 && std::popcount(mask) < narrowest) {
						narrowest = std::popcount(mask);
						variant = v;
					}
				a_stats.narrowedEpochs += variant != 0;
				return variant;
			}

			/** @brief Picks the variant to submit and writes its culling dispatch. */
			std::size_t ShadowVariantToSubmit(const ShadowCommit& a_commit, const org::RunRef& a_latchRun, const std::vector<std::shared_ptr<const ShadowFrame>>& a_variants,
				std::uint64_t a_active)
			{
				const std::size_t variant = NarrowestShadowVariant(a_variants, a_active, a_commit.stats);
				WriteBuildDispatch(*a_commit.target.latch, a_commit.target.latchLayout, a_latchRun, static_cast<std::uint32_t>(a_commit.payload.Inputs()),
					a_commit.parts[PayloadPart::Inputs].srvIndex, *a_variants[variant]);
				return variant;
			}

			/** @brief Logs the shadow epoch once every 600 calls. */
			void LogShadowEpoch(const ShadowCommit& a_commit)
			{
				const auto& target = a_commit.target;
				const auto& payload = a_commit.payload;
				const auto& shadowStats = a_commit.stats;
				static std::uint32_t logged = 0;
				if (logged++ % 600 == 0) {
					std::string views;
					for (const auto& p : a_commit.pending) {
						const auto& view = target.views[p.slot - kFirstShadowViewSlot];
						views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", p.viewId,
							p.renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.ModeInputs(view.modeIndex), view.capacity);
					}
					logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} material rows ({} in the payload): {}",
						target.views.size(), shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, a_commit.parts[PayloadPart::ShadowRows].units, views);
				}
			}
		}
	}

	std::uint32_t IndirectDraws::ExecuteShadowFrame(std::uint32_t a_maps)
	{
		DCLF_ZONE("CS.DCLF.ExecuteShadowFrame");
		auto& engine = *impl->shadowEngine;
		auto& pending = engine.pendingViews;
		const std::uint32_t frameNumber = Scene::FrameView::Get().SceneStep();
		// The engine renders any occlusion map this epoch does not draw. Those are counted here.
		const std::uint32_t maps = engine.OcclusionSetUp(a_maps, frameNumber);
		auto mapsNotDrawn = [&](std::uint32_t a_drawn) {
			for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
				shadowStats.occlusionNotReady[v] += ((a_maps & ~a_drawn) >> v) & 1;
			return a_drawn;
		};
		if (pending.empty() && !maps) {
			pending.clear();
			return mapsNotDrawn(0);
		}
		ScopedPerfEvent event("CS DCLF: shadow views (CPU)");
		const auto start = std::chrono::steady_clock::now();
		auto notReady = [&](ShadowNotReady a_reason) {
			shadowStats.notReady += static_cast<std::uint32_t>(pending.size());
			shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)] += static_cast<std::uint32_t>(pending.size());
			if (!pending.empty())
				engine.ShadowsNotDrawn(shadowStats);
			pending.clear();
			return mapsNotDrawn(0);
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !engine.shadow || engine.shadowPlacements.empty())
			return notReady(ShadowNotReady::Setup);
		const auto indirect = FrameShadowIndirectState();
		if (!indirect.valid) {
			engine.ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		const auto& tables = Scene::FrameView::Get().GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		auto resources = engine.shadow;
		std::array<bool, kShadowModeCount> modeUsed{};
		std::array<ModeRasterStates, kShadowModeCount> modeRasterStates{};
		engine.NoteShadowViewModes(modeUsed, modeRasterStates);
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0, occlusionMs = 0.0;
		std::uint32_t mapsDrawn = 0;

		ShadowInputs in = engine.PrepareShadowInputs(*resources, modeUsed, modeRasterStates);
		// The revision's shape and recording and the installed payload are trusted here without checks.
		const std::vector<ShadowViewLayout>& layouts = engine.shadowPlacements;
		const std::uint32_t epoch = EpochOfShape(ShapeKind::Shadow, kShadowShape);
		const auto revision = impl->RevisionOf(epoch);
		const std::shared_ptr<ShadowPayload> ahead = impl->installedDraws ? impl->installedDraws->shadow : nullptr;
		if (!revision || !ahead || ahead->inputs.addresses.identity != in.addresses.identity) {
			if (revision && impl->shadowUnbuilt++ == 0)
				logger::warn("[DCLF] the shadow epoch of frame {} has no payload for its resources in the installed publication: not submitted <- UNBUILT", frameNumber);
			return notReady(ShadowNotReady::Epoch);
		}
		const auto revisionShape = revision.shape->Value<ShadowVariants>()->shapes.front();
		const ShadowFrame& target = *revisionShape;
		// Clamp to what the latch was sized for.
		if (in.sunEntryProcesses.size() > target.latchLayout.sunProcesses) {
			++impl->revisions.latchClamped[epoch];
			in.sunEntryProcesses.resize(target.latchLayout.sunProcesses);
		}
		auto owners = ExecutionOwners::Make();
		if (!owners)
			return mapsNotDrawn(0);
		const bool ok = Direct::ExecuteEpoch(EpochTraitsOf(epoch).segment, [&] {
			DCLF_ZONE("CS.DCLF.ShadowInputs");
			const auto bodyStart = std::chrono::steady_clock::now();
			TracyCZoneN(shadowPrepareZone, "CS.DCLF.ShadowInputs.Prepare", true);
			const auto prepareStart = std::chrono::steady_clock::now();
			++stats.async[kAsyncShadow].used;
			// The publication was built before this frame's full-frustum cull, so take the current processes.
			ahead->inputs.sunEntryProcesses = in.sunEntryProcesses;
			engine.committedShadow = ahead;
			ShadowPayload& payload = *ahead;
			// The payload's buffers are already uploaded. Only the arena is uploaded here.
			const PayloadVersions& parts = *impl->InstalledPayload();
			Draws::HoldShadowCommit(payload, impl->adoptedSnapshot, owners.List());
			engine.shadowExecutionOwner = owners.Handle();
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();
			TracyCZoneEnd(shadowPrepareZone);

			const auto& adopted = impl->Adopted();
			CommitUploads uploads(impl->commitStagedPool, adopted, impl->scene ? &impl->scene->content : nullptr);
			const Draws::ShadowCommit commit{ *resources, payload, parts, adopted, uploads, target, in, pending, frameNumber, shadowStats };
			TracyCZoneN(shadowCommitZone, "CS.DCLF.ShadowInputs.CommitShared", true);
			const auto inputsStart = std::chrono::steady_clock::now();
			Draws::CommitShadowShared(commit, modeUsed, owners.List());
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();
			TracyCZoneEnd(shadowCommitZone);

			TracyCZoneN(shadowViewsZone, "CS.DCLF.ShadowInputs.BuildViews", true);
			const auto blocksStart = std::chrono::steady_clock::now();
			const org::RunRef latchRun = RenderGraphRuntime::Get().Host()->CurrentRun();
			// On the CPU pipeline, the shadow site culls from the latched views collected here.
			std::vector<Direct::ShadowSiteView> directViews;
			TracyCZoneN(latchViewsZone, "CS.DCLF.ShadowInputs.LatchViews", true);
			std::uint64_t active = Draws::LatchShadowViews(commit, latchRun, revisionShape, layouts, revision.direct ? &directViews : nullptr);
			TracyCZoneEnd(latchViewsZone);
			const auto occlusionStart = std::chrono::steady_clock::now();
			TracyCZoneN(occlusionZone, "CS.DCLF.ShadowInputs.Occlusion", true);
			mapsDrawn = Draws::LatchOcclusionViews(commit, latchRun, revisionShape, engine.occlusion, maps, active, revision.direct ? &directViews : nullptr);
			occlusionMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - occlusionStart).count();
			TracyCZoneEnd(occlusionZone);
			TracyCZoneN(arenaZone, "CS.DCLF.ShadowInputs.Arena", true);
			const auto record = Draws::UploadShadowArena(commit);
			impl->revisions.latchedMisses += Draws::LatchShadowCsBlocks(commit, latchRun, record);
			TracyCZoneEnd(arenaZone);
			engine.lastActiveViews = active;
			// Views that share draw arguments must not draw in the same frame, or they overwrite each other's draws.
			if (revisionShape) {
				std::uint64_t ownersSeen = 0;
				const auto& views = revisionShape->views;
				for (std::size_t v = 0; v < views.size() && v < 64; ++v) {
					if (!((active >> v) & 1) || views[v].slot >= resources->drawArgsOwner.size())
						continue;
					const std::uint64_t bit = 1ull << (resources->drawArgsOwner[views[v].slot] % 64);
					static std::uint32_t reported = 0;
					if ((ownersSeen & bit) && reported++ < 8)
						logger::error("[DCLF] shadow views that share draw arguments drew in one frame (slot {})", views[v].slot);
					ownersSeen |= bit;
				}
			}
			// On the CPU pipeline the shadow site culls and draws. The recording holds only the latched and index pool copies.
			if (revision.direct) {
				if (revision.recordings && !revision.recordings->recordings.empty())
					impl->SubmitRevisionRecording(epoch, *revision.recordings,
						Draws::ShadowVariantToSubmit(commit, latchRun, revision.shape->Value<ShadowVariants>()->shapes, active));
				Direct::ShadowCommitted({ revisionShape, resources, &adopted, directViews, payload.inputs.sunEntryProcesses, latchRun });
			} else
				impl->SubmitRevisionRecording(epoch, *revision.recordings, Draws::ShadowVariantToSubmit(commit, latchRun, revision.shape->Value<ShadowVariants>()->shapes, active));
			owners.Seal(impl->scene.get());
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			{
				DCLF_ZONE("CS.DCLF.ShadowInputs.Log");
				Draws::LogShadowEpoch(commit);
			}
			TracyCZoneEnd(shadowViewsZone);
			bodyMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bodyStart).count();
		}, owners.Handle());
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own overhead
		shadowStats.cpuMs += totalMs;
		if (!ok) {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			return notReady(ShadowNotReady::Epoch);
		}
		engine.NoteShadowEpochDrawn(*ahead, modeUsed, frameNumber, mapsDrawn, occlusionMs, shadowStats);
		impl->revisions.occlusionSubmitted += mapsDrawn != 0;
		return mapsNotDrawn(mapsDrawn);
	}

	void ShadowEngine::NoteShadowViewModes(std::array<bool, kShadowModeCount>& a_modeUsed, std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates)
	{
		auto& modeUsed = a_modeUsed;
		// Inputs and pipelines cover every catalog state per mode, so the views a frame draws do not change the build.
		// A mode or state missing from the catalog is a defect, and it is added now.
		for (const auto& view : pendingViews) {
			modeUsed[view.modeIndex] = true;
			if (!readyModes[view.modeIndex]) {
				readyModes[view.modeIndex] = true;
				NoteCapability(shadowCatalogBuilt, fmt::format("a view of render mode {:#x}", view.renderMode));
			}
			if (const auto& states = readyStates[view.modeIndex].Of(view.casterClass != 0); !std::binary_search(states.begin(), states.end(), view.rasterState)) {
				readyStates[view.modeIndex].Add(view.rasterState, view.casterClass != 0);
				NoteCapability(shadowCatalogBuilt, fmt::format("view {} (mode {:#x}) draws with rasterizer state {}, not in the catalog", view.viewId, view.renderMode, view.rasterState));
			}
		}
		// Keep occluders in the payload even on frames that do not draw the map.
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v)
			if (Extensions::OcclusionOn(v) && readyModes[OcclusionModeOf(v)] && occlusion[v].dsvFormat != DXGI_FORMAT_UNKNOWN)
				modeUsed[OcclusionModeOf(v)] = true;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			if (modeUsed[m])
				a_modeRasterStates[m] = readyStates[m];
		// One pipeline set per shadow map format. Every engine target is D16.
		const DXGI_FORMAT dsvFormat = pendingViews.empty() ? shadowTargetFormat : pendingViews.front().dsvFormat;
		if (dsvFormat != shadowTargetFormat) {
			shadowTargetFormat = dsvFormat;
			NoteCapability(shadowCatalogBuilt, fmt::format("a view draws into format {}", static_cast<int>(dsvFormat)));
		}
		// The shadow pipelines are built for the engine shadow map's declared format, fixed at setup. A view in another format is a defect.
		if (dsvFormat != DXGI_FORMAT_UNKNOWN && pipelineShadowFormat != DXGI_FORMAT_UNKNOWN && dsvFormat != pipelineShadowFormat)
			IndirectDraws::NoteInputChanged(fmt::format("a shadow view draws into depth format {}, not the shadow pipelines' declared {}", static_cast<int>(dsvFormat),
				static_cast<int>(pipelineShadowFormat)));
	}

	void ShadowEngine::NoteShadowEpochDrawn(const ShadowPayload& a_payload, const std::array<bool, kShadowModeCount>& a_modeUsed, std::uint32_t a_frame,
		std::uint32_t a_mapsDrawn, double a_occlusionMs, IndirectDraws::ShadowStats& a_stats)
	{
		const auto& payload = a_payload;
		const auto& modeUsed = a_modeUsed;
		auto& epochStats = a_stats;
		++epochStats.epochs;
		epochStats.viewsDrawn += static_cast<std::uint32_t>(pendingViews.size());
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			if (modeUsed[OcclusionModeOf(v)])
				occlusion[v].inputs = static_cast<std::uint32_t>(payload.ModeInputs(OcclusionModeOf(v)));
			epochStats.occlusionDrawn[v] += (a_mapsDrawn >> v) & 1;
		}
		if (a_mapsDrawn) {
			++epochStats.occlusionEpochs;
			epochStats.occlusionMs += a_occlusionMs;
		}
		ReadShadowCullCounters(a_frame, epochStats);
		for (std::uint32_t m = 0; m < kFirstOcclusionMode && m < epochStats.casters.size(); ++m)
			if (modeUsed[m])
				epochStats.casters[m] = static_cast<std::uint32_t>(payload.ModeInputs(m));
		epochStats.setWaiting += payload.setWaiting;
		if (epochStats.setWaitingFirst.empty())
			epochStats.setWaitingFirst = payload.setWaitingFirst;
		if (PassCapture::ShadowWithholdingEnabled()) {
			// The cascades' casters decide which sun entries the next frame's cascade culls may skip.
			if (modeUsed[kSunShadowMode])
				Extensions::PublishShadowExclusion(Api::ShadowSource::Sun, payload.sunExclusion);
			// Same for point lights. Publish null when none drew, so a new light falls back to the engine.
			if (Extensions::SourceCullsInstalled(Api::ShadowSource::Point))
				Extensions::PublishShadowExclusion(Api::ShadowSource::Point, modeUsed[kParabolicShadowMode] ? payload.parabolicExclusion : nullptr);
		}
		pendingViews.clear();
	}

	ShadowInputs ShadowEngine::PrepareShadowInputs(const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed,
		const std::array<ModeRasterStates, kShadowModeCount>& a_modeRasterStates) const
	{
		DCLF_ZONE("CS.DCLF.PrepareShadowInputs");
		ShadowInputs in;
		in.frameNumber = Scene::FrameView::Get().SceneStep();
		in.modeUsed = a_modeUsed;
		in.modeRasterStates = a_modeRasterStates;
		// The sun's full-frustum planes. At the shadow epoch these are this frame's. For the build ahead they are last frame's,
		// because it is posted before this frame's UpdateCamera.
		Api::SunEntryPlanesSink entryPlanes(in.sunEntryProcesses);
		(void)Extensions::SampleSunEntryPlanes(entryPlanes, in.frameNumber);
		in.sunCandidates = Scene::FrameView::Get().GetSunCandidates();
		in.lightCandidates = Scene::FrameView::Get().GetLightCandidates();
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.placementsIndex = FrameValues::Get().PlacementsIndex();
		in.addresses.palettesIndex = FrameValues::Get().PalettesIndex();
		in.addresses.shadingIndex = FrameValues::Get().ShadingIndex();
		in.addresses.identity = &a_resources;
		in.tablesGeneration = Scene::FrameView::Get().GetTablesGeneration();
		in.lookupGeneration = Scene::FrameView::Get().GetLookups().shadowGeneration;
		if (auto* csState = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&csState->lastSharedData);
			in.sharedData.assign(shared, shared + sizeof(State::SharedDataCB));
			if (!csState->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(csState->lastFeatureData.data());
				in.featureData.assign(feature, feature + csState->lastFeatureData.size());
			}
		}
		return in;
	}

	std::shared_ptr<ShadowPayload> ShadowEngine::AcquireShadowPayload()
	{
		// Reset idle payloads so they keep nothing alive.
		std::shared_ptr<ShadowPayload> free;
		for (auto& payload : shadowPayloadPool)
			if (payload.use_count() == 1) {
				payload->Reset();
				if (!free)
					free = payload;
			}
		return free ? free : shadowPayloadPool.emplace_back(std::make_shared<ShadowPayload>());
	}

	void ShadowEngine::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		// Each execution copies its counters into the host range of its run. A sample is read once its GPU point is reached,
		// unless a later execution already reused that range. Must run right after submission, while the run is still current.
		auto* host = RenderGraphRuntime::Get().Host();
		const auto readback = CullCounterReadback(CullCounterView::kShadow);
		if (!host || !readback)
			return;
		auto& armed = readback->armed;
		const std::uint32_t frameSlot = readback->RangeOf(host->CurrentRun());
		if (frameSlot == org::RunReadback::kNoRegion)
			return;
		const auto epoch = shadowCullEpochs++;
		bool lost = false;
		if (armed.armed) {
			if (frameSlot == armed.range) {
				// This execution overwrites the unread sample, so it is lost. Take a new one now.
				armed.armed = false;
				++readback->missed;
				lost = true;
			} else if (!armed.done.Reached()) {
				return;
			} else {
				armed.armed = false;
				const bool read = readback->Read(armed.range, armed.source, armed.stamp, [&](const std::uint32_t* a_words) {
					a_stats.cullDrawn = a_words[0];
					a_stats.cullRejected = a_words[1];
					a_stats.cullTested = a_words[2];
					a_stats.cullSunEntryOut = a_words[3];
					a_stats.cullMinRadius = a_words[4];
					a_stats.cullRootFading = a_words[5];
					a_stats.cullClass = a_words[16];
					a_stats.cullBelowTexels = a_words[Draws::kCountBelowTexelsWord];
					a_stats.cullRowsLost = a_words[Draws::kCountCasterRowsLostWord];
					a_stats.cullSampledView = armed.view;
					a_stats.cullSampledMode = armed.mode;
				});
				++(read ? readback->harvested : readback->missed);
				return;
			}
		}
		// Sample one epoch in 127, or the one after a lost sample, cycling through the views.
		if (((epoch % 127) != 0 && !lost) || !shadow || shadow->labels.empty())
			return;
		const auto& view = shadow->labels[(epoch / 127) % shadow->labels.size()];
		if (view.slot >= readback->sources)
			return;
		armed = { true, frameSlot, view.slot, CounterReadback::StampOf(a_frame), view.viewId, view.renderMode, host->SubmittedPoint() };
	}

	void ShadowEngine::ShadowsNotDrawn(IndirectDraws::ShadowStats& a_stats)
	{
		if (const auto set = Scene::FrameView::Get().FrameClaims(); set && (set->drawn & (kSetCaster | kSetCasterPoint)) && PassCapture::ShadowWithholdingEnabled())
			++a_stats.notReadyWithheld;
	}
}
