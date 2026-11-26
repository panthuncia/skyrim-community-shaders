// The view epochs on the render thread: their traits, forward keys, bucket plans and shapes, their cameras' VS_PerFrame and
// their commits. Their passes are in ViewEpochRecord.cpp, and their captures belong to the view features.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Gpu/NamedRefs.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "Snapshot/CoreFrame.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"

#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

namespace DCLF
{

	namespace Draws
	{
		const ViewTraits& TraitsOf(ViewKind a_kind)
		{
			// Made once, the first time a view's traits are read after InstallFeatures.
			static const auto kTraits = [] {
				std::array<ViewTraits, kViews> traits{};
				for (const auto& slot : kViewSlots) {
					const auto& entry = Extensions::ViewEntryOf(slot.view);
					const auto& shape = entry.shape;
					const auto& epochs = ViewEpochsOf(slot.view);
					traits[ViewIndex(slot.view)] = { slot.view, shape.faces, shape.trees, shape.forwardPart, shape.depthPass, shape.viewBits, epochs.epochs,
						{ epochs.segments[0], epochs.segments[1] }, entry.name.c_str(), entry.prefix.c_str(), entry.blocks.c_str(), entry.target.c_str(),
						{ entry.labels[0].c_str(), entry.labels[1].c_str(), entry.labels[2].c_str(), entry.labels[3].c_str(), entry.labels[4].c_str() }, shape.frustumCull };
				}
				return traits;
			}();
			return kTraits[ViewIndex(a_kind)];
		}

		bool ViewForwardKey(ViewKind a_kind, const SceneTables& a_tables, std::size_t a_slot, ForwardPipelineKey& a_key)
		{
			if (a_slot >= a_tables.pipelines.size() || !a_tables.PipelineUsed(a_slot))
				return false;
			const auto& pipelineKey = a_tables.pipelines[a_slot];
			const std::uint32_t twoSided = (pipelineKey.rasterFlags & kRasterTwoSided) ? ForwardPipelineKey::kForwardTwoSided : 0u;
			// First person draws a forward key's slot with the main set's colour pipeline in the forward range, the rest with its own key.
			// The pixel descriptor drops Deferred because the engine draws first person forward, after the composite.
			const auto& keys = Extensions::ViewEntryOf(a_kind).keys;
			if (keys.lodTechniquesOnly && !LodLightingTechnique(pipelineKey.passDescriptor))
				return false;
			if (keys.forwardRasterToForwardPart && RasterForward(pipelineKey.rasterFlags))
				return false;
			a_key = ForwardPipelineKey{ pipelineKey.vertexDescriptor, pipelineKey.pixelDescriptor & ~kLightingPixelDeferred, keys.keyFlags | twoSided,
				pipelineKey.runtimeVertex, pipelineKey.runtimePixel };
			return true;
		}

		void ViewPipelinesOf(ViewKind a_kind, const PipelineCatalog* a_catalog, const SceneTables& a_tables, const ForwardTargets& a_targets, bool a_on,
			std::vector<rhi::PipelineHandle>& a_slots)
		{
			a_slots.assign(a_on ? a_tables.pipelines.size() : 0, rhi::PipelineHandle{});
			for (std::size_t p = 0; p < a_slots.size(); ++p)
				if (ForwardPipelineKey key; ViewForwardKey(a_kind, a_tables, p, key))
					a_slots[p] = ForwardPipelineOf(a_catalog, key, a_targets);
		}

		void ViewForwardSlotsOf(const SceneTables& a_tables, bool a_on, std::vector<std::uint8_t>& a_slots)
		{
			a_slots.assign(a_on ? a_tables.pipelines.size() : 0, 0);
			for (std::size_t p = 0; p < a_slots.size(); ++p)
				a_slots[p] = a_tables.PipelineUsed(p) && RasterForward(a_tables.pipelines[p].rasterFlags) && !RasterRefraction(a_tables.pipelines[p].rasterFlags) ? 1 : 0;
		}

		std::uint32_t ZGroupOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot)
		{
			// A pipeline slot's Z-prepass group is its lookup entry's set index's, as PlanZBuckets takes it.
			if (!a_lookups || a_slot >= a_lookups->pipelines.size())
				return kNoBucket;
			const auto setIndex = a_lookups->pipelines[a_slot].setIndex;
			return setIndex != Lookups::kNone && setIndex < a_indirect.zGroups.size() ? a_indirect.zGroups[setIndex] : kNoBucket;
		}

		namespace
		{
			/** @brief Whether the view has a forward class map. Without one every forward draw is dropped and counted. */
			bool HasViewClassMap(const ViewFrame& a_shape)
			{
				return !a_shape.forwardRanges.empty() && a_shape.indirect.classOf.size() <= a_shape.latchLayout.classMapWords;
			}

			/** @brief Writes the view's forward class map into its latch slot, which BuildDrawsCS uses to class each forward draw. */
			void WriteViewClassMap(const ViewFrame& a_shape, std::string_view a_what, const org::RunRef& a_latchRun)
			{
				const auto& classOf = a_shape.indirect.classOf;
				const auto& layout = a_shape.latchLayout;
				if (!HasViewClassMap(a_shape))
					return;
				const std::uint32_t header[kClassHeaderWords] = { static_cast<std::uint32_t>(classOf.size()), 0, 0, 0 };
				LatchWrite(*a_shape.latch, a_what, a_latchRun, layout.ClassMapOffset(), std::as_bytes(std::span(header)));
				if (!classOf.empty())
					LatchWrite(*a_shape.latch, a_what, a_latchRun, layout.ClassMapOffset() + kClassHeaderWords * 4, std::as_bytes(std::span(classOf)));
			}

			// Per view, the latch regions already holding the shape's maps and face tables. Render thread.
			std::array<Draws::LatchRegionsHeld, static_cast<std::size_t>(Api::EngineView::Count)> viewShapeRegions;
		}

		std::uint32_t SetClassOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot)
		{
			// As ZGroupOf, the slot's lookup entry's set index's word.
			if (!a_lookups || a_slot >= a_lookups->pipelines.size())
				return kNoSetClass;
			const auto setIndex = a_lookups->pipelines[a_slot].setIndex;
			return setIndex != Lookups::kNone && setIndex < a_indirect.classOf.size() ? a_indirect.classOf[setIndex] : kNoSetClass;
		}

		void PlanViewBuckets(const ViewTraits& a_traits, const MainLayout& a_main, const Lookups* a_lookups, const IndirectState& a_indirect,
			std::span<const rhi::PipelineHandle> a_slotPipelines, std::span<const std::uint8_t> a_forwardSlots, ViewPlan& a_out)
		{
			const auto slots = static_cast<std::uint32_t>(a_main.zBucketCapacity.size());
			a_out.map.assign(slots, kNoBucket);
			a_out.buckets.clear();
			for (std::uint32_t p = 0; p < slots; ++p) {
				if (p >= a_slotPipelines.size() || !a_slotPipelines[p].valid() || !a_main.zBucketCapacity[p])
					continue;
				rhi::PipelineHandle depth{};
				if (a_traits.depthPass) {
					const std::uint32_t group = ZGroupOf(a_lookups, a_indirect, p);
					if (group >= a_indirect.zPipelines.size() || !a_indirect.zPipelines[group].valid())
						continue;
					depth = a_indirect.zPipelines[group];
				}
				const auto colour = a_slotPipelines[p];
				auto it = std::find_if(a_out.buckets.begin(), a_out.buckets.end(),
					[&](const auto& a_bucket) { return SameHandle(a_bucket.depth, depth) && SameHandle(a_bucket.colour, colour); });
				if (it == a_out.buckets.end())
					it = a_out.buckets.insert(a_out.buckets.end(), ViewFrame::Bucket{ depth, colour, 0, 0 });
				it->capacity += a_main.zBucketCapacity[p];
				a_out.map[p] = static_cast<std::uint32_t>(it - a_out.buckets.begin());
			}
			a_out.draws = 0;
			for (auto& bucket : a_out.buckets) {
				bucket.first = a_out.draws;
				a_out.draws += bucket.capacity;
			}
			// The forward range gets one device-generated call per state class. Each class's entry holds every draw its forward
			// slots can produce, staged then sorted, in two halves after the buckets' ranges.
			a_out.forwardRanges.clear();
			std::vector<std::uint32_t> capacity(a_indirect.classes.size(), 0u);
			std::uint64_t forward = 0;
			for (std::uint32_t p = 0; p < slots && p < a_forwardSlots.size(); ++p) {
				if (!a_forwardSlots[p] || a_out.map[p] != kNoBucket)
					continue;
				const std::uint32_t setClass = SetClassOf(a_lookups, a_indirect, p);
				if (setClass == kNoSetClass || (setClass >> kSetClassShift) >= capacity.size())
					continue;  // no pipeline in the set version, so none of its draws is the view's
				a_out.map[p] = kFirstPersonForwardBucket | static_cast<std::uint32_t>(a_out.buckets.size());
				const std::uint32_t slotCapacity = p < a_main.slotCapacity.size() ? a_main.slotCapacity[p] : 0u;
				capacity[setClass >> kSetClassShift] += slotCapacity;
				forward += slotCapacity;
			}
			if (forward) {
				a_out.forwardRanges.resize(capacity.size());
				for (std::size_t c = 0; c < capacity.size(); ++c) {
					a_out.forwardRanges[c] = { a_out.draws, capacity[c] };
					a_out.draws += 2 * capacity[c];
				}
			} else {
				for (auto& word : a_out.map)
					if (word != kNoBucket && (word & kFirstPersonForwardBucket))
						word = kNoBucket;
			}
		}

		std::shared_ptr<ViewFrame> MakeViewShape(const ViewResources& a_resources, const ViewShapeInputs& a_in)
		{
			const auto& traits = *a_resources.traits;
			const auto& latches = *a_in.latches;
			auto frame = std::make_shared<ViewFrame>();
			frame->kind = traits.kind;
			frame->resourceHeap = a_in.resourceHeap;
			frame->samplerHeap = a_in.samplerHeap;
			frame->indirect = a_in.indirect;
			frame->latch = latches.latch;
			frame->width = a_in.width;
			frame->height = a_in.height;
			frame->depthRange = a_in.depthRange;
			frame->sequenceDraws = a_in.sequenceDraws;
			frame->buckets = a_in.plan.buckets;
			frame->forwardRanges = a_in.plan.forwardRanges;
			frame->forwardEntry = static_cast<std::uint32_t>(a_in.plan.buckets.size());
			frame->latchLayout = latches.layout;
			frame->map = std::make_shared<const std::vector<std::uint32_t>>(a_in.plan.map);
			// Each draw family's faces: a cull thread per word of its lists' units.
			for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
				const auto& in = a_in.faces[d];
				if (!in.pipeline.valid())
					continue;
				const auto& faces = Extensions::DrawFamilies()[d].contribution.faces;
				frame->faces[d].pipeline = in.pipeline;
				frame->faces[d].signature = in.signature;
				frame->faces[d].groups = static_cast<std::uint32_t>((std::uint64_t(in.units) * faces.unitWords + faces.cullGroup - 1) / faces.cullGroup);
			}
			// Per face, the view's frame record (its blocks, VS and PS b12 the face's camera, come through the record).
			const auto base = FramePushWords(a_in.frameConstantsAddress, a_in.frameConstantsRef, a_in.frameBlocks ? a_in.frameBlocks->TableBase(0) : 0u);
			frame->push.assign(traits.faces, base);
			// A view with its own frame constants holds a record and a block table per face, so each face's words name its own.
			const bool faceRecords = a_resources.frameConstants && a_resources.frameBlocks && a_resources.frameBlocks->faces > 1;
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				if (faceRecords && f) {
					auto& words = frame->push[f];
					const std::uint64_t record = (std::uint64_t(words[kFramePushRecord + 1]) << 32 | words[kFramePushRecord]) + std::uint64_t(f) * sizeof(DrawBindings);
					words[kFramePushRecord] = static_cast<std::uint32_t>(record);
					words[kFramePushRecord + 1] = static_cast<std::uint32_t>(record >> 32);
					words[kFramePushRecordRef + 1] += f * static_cast<std::uint32_t>(sizeof(DrawBindings));
					words[kFramePushBlocks] = a_resources.frameBlocks->TableBase(f);
				}
			}
			// The latched copies: each face's block and family rows from the latch, with the counters zeroed.
			auto& fromLatch = frame->latched[0];
			auto& fromZeros = frame->latched[1];
			fromLatch.latch = latches.latch;
			fromZeros.latch = latches.zeros;
			fromZeros.fixed = true;
			fromZeros.copies.push_back({ a_resources.count.get(), 0, static_cast<std::uint32_t>(sizeof(kZeroCounts)), 0 });
			const auto bucketBytes = static_cast<std::uint32_t>(std::max<std::size_t>(frame->buckets.size() + frame->forwardRanges.size(), 1) * sizeof(std::uint32_t));
			for (std::uint32_t f = 0; f < traits.faces; ++f) {
				const std::uint32_t face = latches.layout.FaceOffset(f);
				fromZeros.copies.push_back({ a_resources.bucketCounts[f]->Key(), 0, bucketBytes, 0 });
				fromLatch.copies.push_back({ a_resources.blocks.get(), face, kViewBlockBytes, std::uint64_t(f) * kViewBlockBytes });
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
					if (frame->faces[d].pipeline.valid() && a_resources.faceRows[d][f] && a_resources.faceLists[d][f]) {
						const auto& entry = Extensions::DrawFamilies()[d];
						fromLatch.copies.push_back({ a_resources.faceRows[d][f].get(), face + entry.faceLayout.rowInFace, entry.contribution.faces.rowBytes, 0 });
						fromLatch.copies.push_back({ a_resources.faceLists[d][f]->Key(), face + entry.faceLayout.headerInFace, entry.contribution.faces.headerBytes, 0 });
					}
			}
			if (!a_in.frameCopies.empty()) {
				auto& fromFrame = frame->latched[ViewFrame::kFrameList];
				fromFrame.latch = latches.latch;
				fromFrame.copies = a_in.frameCopies;
			}
			return frame;
		}

		std::span<const std::byte> FrameBlockNow()
		{
			// Native: the frame camera's block. Its rows past the camera (FrameParams, ratios) are what the engine's block held for first
			// person and the reflection faces (observed); the faces' ratios are written after, as before.
			if (Snapshot::FrameCamera::Owned())
				return std::as_bytes(std::span(&Snapshot::FrameCamera::Main().data, 1));
			std::span<const std::byte> frame;
			auto& mirror = ConstantMirror::Get();
			if (auto* perFrame = *CS_ENGINE_READ(BoundState, BoundState, globals::game::perFrame).get()) {
				mirror.Watch(perFrame);
				frame = mirror.Contents(perFrame);
			}
			if (frame.size() < ShadowCameras::kBlockBytes) {
				const auto& cached = CS_ENGINE_READ(BoundState, BoundState, globals::game::frameBufferCached).data;
				static_assert(sizeof(cached) >= ShadowCameras::kBlockBytes);
				frame = std::as_bytes(std::span(&cached, 1));
			}
			return frame;
		}

		void PerFrameBlock(const ShadowCameras::Camera& a_camera, std::span<const std::byte> a_frame, bool a_jitter, const PreviousCamera* a_previous, std::byte* a_out)
		{
			std::memcpy(a_out, a_frame.data(), ShadowCameras::kBlockBytes);
			auto* out = reinterpret_cast<float*>(a_out);
			Snapshot::FrameCamera::Previous previous{};
			if (a_previous)
				previous = { a_previous->viewProjUnjittered, a_previous->eye };
			const RE::NiPoint3 eye{ a_camera.eye[0], a_camera.eye[1], a_camera.eye[2] };
			// The render frame's jitter, as the main view's (render-owned in native).
			const std::array<float, 2> jitter = a_jitter ? Snapshot::FrameCamera::Jitter() : std::array<float, 2>{};
			Snapshot::FrameCamera::WriteCamera(a_camera.view, a_camera.proj, a_camera.viewProj, eye, jitter, a_previous ? &previous : nullptr, out);
			if (a_previous)
				return;
			if (auto* state = CS_ENGINE_READ(GraphicsState, RecordTime, RE::BSGraphics::State::GetSingleton())) {
				const auto& data = state->GetRuntimeData();
				Snapshot::FrameCamera::WriteRatios({ data.dynamicResolutionWidthRatio, data.dynamicResolutionHeightRatio, data.dynamicResolutionPreviousWidthRatio,
													   data.dynamicResolutionPreviousHeightRatio },
					Snapshot::Core().drClampOffset, out);
			}
		}
	}

	std::uint32_t IndirectDraws::Impl::CommitView(const ViewFrame& a_shape, std::span<const ViewFace> a_faces, std::uint32_t a_inputs, const ViewFamilyRows* a_rows)
	{
		const auto& traits = TraitsOf(a_shape.kind);
		const auto& labels = traits.labels;
		const org::RunRef latchRun = RenderGraphRuntime::Get().Host()->CurrentRun();
		const auto& latchBlock = *a_shape.latch;
		const auto& layout = a_shape.latchLayout;
		const auto& map = *a_shape.map;
		const auto region = static_cast<std::uint32_t>(latchBlock.Offset(latchRun));
		// Slots the tables gained since the map was made get no bucket, since none of their draws is the frame's.
		const std::uint32_t slots = std::min(std::max(static_cast<std::uint32_t>(map.size()), InstalledPipelineSlots()), layout.slots);
		// The maps and face tables belong to the shape, whose map is made with it. Written only when this run's region holds another's.
		auto& held = viewShapeRegions[static_cast<std::size_t>(a_shape.kind)].Of(latchRun, a_shape.latch, a_shape.map, slots);
		TracyCZoneN(mapsZone, "CS.DCLF.View.Maps", true);
		TracyCZoneValue(mapsZone, slots);
		if (!Draws::LatchRegionsHeld::Hold(held, 0)) {
			WriteBucketMap(latchBlock, labels.map, latchRun, layout.MapOffset(), map, slots);
			WriteViewClassMap(a_shape, labels.map, latchRun);
		}
		const bool classMap = HasViewClassMap(a_shape);
		TracyCZoneEnd(mapsZone);
		TracyCZoneN(facesZone, "CS.DCLF.View.Faces", true);
		const std::uint32_t frameNumber = Scene::FrameView::Get().SceneStep();
		auto& state = View(a_shape.kind);
		const auto* view = state.resources.get();
		std::vector<std::uint32_t> table;
		std::uint32_t drawn = 0;
		for (std::uint32_t f = 0; f < a_faces.size() && f < traits.faces; ++f) {
			const auto& face = a_faces[f];
			// The face's bucket table, then one forward entry per state class.
			if (!Draws::LatchRegionsHeld::Hold(held, 1 + f)) {
				table.clear();
				for (const auto& bucket : a_shape.buckets) {
					table.push_back(f * a_shape.sequenceDraws + bucket.first);
					table.push_back(bucket.capacity);
				}
				for (const auto& range : a_shape.forwardRanges) {
					table.push_back(f * a_shape.sequenceDraws + range.first);
					table.push_back(range.capacity);
				}
				if (!table.empty())
					LatchWrite(latchBlock, labels.tables, latchRun, layout.TableOffset(f), std::as_bytes(std::span(table)));
			}
			const std::uint32_t inputs = face.drawn ? a_inputs : 0u;
			BuildDrawsLatch latch{};
			latch.dispatch[0] = (inputs + 63) / 64;
			latch.dispatch[1] = 1;
			latch.dispatch[2] = 1;
			latch.drawCount = inputs;
			latch.cullFlags = traits.frustumCull ? 1u : 0u;  // the frustum alone, near plane included, 0 for untested
			latch.viewBits = traits.viewBits;
			latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;
			latch.placementsIndex = FrameValues::Get().PlacementsIndex();
			latch.fadeSeedsIndex = CullBindingIndex(Api::CullBindingSlot::RootSeeds, *scene, frameNumber, Adopted());
			// A covered frame always has a ready installed payload.
			PayloadLatch(*InstalledPayload(), latch);
			LatchCullCamera(latch, face.viewProj, face.eye);
			latch.bucketMapOffset = region + layout.MapOffset();
			latch.bucketTableOffset = region + layout.TableOffset(f);
			latch.classMapOffset = classMap ? region + layout.ClassMapOffset() : 0u;
			LatchWriteValue(latchBlock, labels.latches, latchRun, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
			if (!face.block.empty())
				LatchWrite(latchBlock, labels.blocks, latchRun, layout.FaceOffset(f), face.block);
			for (std::size_t d = 0; a_rows && view && d < Extensions::kDrawFamilies; ++d) {
				if ((*a_rows)[d].empty() || !a_shape.faces[d].pipeline.valid())
					continue;
				// The family's face names nothing when the face is not drawn or the engine draws the family.
				const auto& entry = Extensions::DrawFamilies()[d];
				auto& written = state.faces[d];
				written.faceRow.resize(entry.contribution.faces.rowBytes);
				written.faceHeader.resize(entry.contribution.faces.headerBytes);
				Api::DrawFamilyFaceWrite write{ (*a_rows)[d], RefOf(Adopted().Of(view->faceLists[d][f])), face.drawn, written.faceRow, written.faceHeader };
				entry.contribution.faces.face(write);
				LatchWrite(latchBlock, labels.trees, latchRun, layout.FaceOffset(f) + entry.faceLayout.rowInFace, write.row);
				LatchWrite(latchBlock, labels.trees, latchRun, layout.FaceOffset(f) + entry.faceLayout.headerInFace, write.header);
			}
			drawn += face.drawn ? 1u : 0u;
		}
		TracyCZoneEnd(facesZone);
		ZoneNamedN(recordZone, "CS.DCLF.View.FrameRecord", true);
		if (auto* own = state.resources.get(); own && own->frameConstants) {
			// The view's own frame constants, filled from this frame's sources. The faces' programs read no other frame slot.
			const auto& adopted = Adopted();
			CommitUploads uploads(commitStagedPool, adopted);
			LatchedUploads frame(a_shape.latched[ViewFrame::kFrameList], uploads, latchRun);
			// Copies of the main frame buffers, such as Deferred's sun cascades at t98, filled from each register's CPU mirror now.
			// A copy that cannot be filled reads zero and its register gets a null buffer.
			if (const auto frameBuffers = own->frameBuffers.load(std::memory_order_acquire)) {
				auto& mirror = ConstantMirror::Get();
				for (const auto& frameBuffer : *frameBuffers) {
					std::span<const std::byte> contents;
					const auto buffer = BoundPixelBuffer(frameBuffer.textureRegister);
					if (buffer) {
						mirror.Watch(buffer.get());
						contents = mirror.Contents(buffer.get());
					}
					if (LatchFrameBuffer(frame, frameBuffer, contents))
						state.frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
					else
						state.frameTextures[frameBuffer.textureRegister] = GpuTextures::Get().NullBufferIndex(false);
				}
			}
			const auto shared = SharedDataBlock(), feature = FeatureDataBlock();
			const std::uint32_t vsMask = (1u << kPerFrameVertexRegister) | (1u << kFrameFogRegister);
			const std::uint32_t psMask = (1u << kPerFrameVertexRegister) | (shared.empty() ? 0u : 1u << kSharedDataRegister) | (feature.empty() ? 0u : 1u << kFeatureDataRegister);
			auto& textures = GpuTextures::Get();
			const std::uint32_t nullIndex = textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u;
			const DrawBindings record = FrameRecordOf(own->frameConstantsAddress, own->frameConstantsRef, *own->frameBlocks, vsMask, psMask, state.frameTextures, nullIndex, *InstalledPayload(),
				{ FrameValues::Get().PlacementsIndex(), FrameValues::Get().PalettesIndex(), FrameValues::Get().ShadingIndex() }, adopted, *scene, frameNumber);
			// A record per face, each naming its face's camera block at VS and PS b12.
			std::array<DrawBindings, kMaxViewFaces> records;
			const std::uint32_t faces = std::min(own->frameBlocks->faces, kMaxViewFaces);
			for (std::uint32_t f = 0; f < faces; ++f) {
				records[f] = record;
				const auto indices = own->frameBlocks->FaceIndices(f);
				std::ranges::copy(indices, records[f].frameBlocks);
			}
			frame(own->frameConstants, records.data(), sizeof(DrawBindings) * faces, std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
			frame(own->frameConstants, shared.data(), shared.size(), FrameSlotOffset(true, kSharedDataRegister));
			frame(own->frameConstants, feature.data(), feature.size(), FrameSlotOffset(true, kFeatureDataRegister));
			ZeroSharedLightOnce(uploads, own->frameConstants, own->sharedLightZeroed);
			LatchFrameRows(frame, own->frameConstants, traits.viewBits);
			(void)frame.Finish();
			revisions.latchedMisses += frame.Misses();
			// Frame texture registers left null, for the view's report.
			state.nullTextures = {};
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t)
				if (record.textures[t] == nullIndex)
					state.nullTextures[t / 64] |= 1ull << (t % 64);
		}
		return drawn;
	}

	ExecutionOwners IndirectDraws::Impl::EpochOwners(std::span<const MainPayload* const> a_payloads)
	{
		auto owners = ExecutionOwners::Make();
		if (!owners)
			return owners;
		for (const auto* payload : a_payloads)
			if (payload)
				owners.AddPayload(*payload);
		// Hold the frame textures the colour segment's record names, which the shared epochs and first person read.
		// A view with its own frame constants holds its commit's instead.
		for (const auto& held : frameTextureBindings)
			owners.Add(held.binding.owner);
		owners.Add(adoptedSnapshot);
		return owners;
	}
}
