// Build parity. Every 300th colour epoch, BuildDraws' output is copied to staging and compared three main epochs later with
// the CPU's templates. Template row addresses are table offsets, placed at the bases recorded when the check is armed.

#include "DrawsState.h"
#include "DrawnLocalShadows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		ankerl::unordered_dense::map<std::uint32_t, DrawnLocalShadows> drawnLocalShadows;
		/**
		 * @brief The colour draws BuildDraws wrote, from each class's main and terrain sub-ranges and plain buckets, up to each count.
		 * Forward draws are sorted elsewhere and left out.
		 */
		std::vector<DrawSequence> ClassSequences(const DrawsState::ParityReadback& a_parity, const DrawSequence* a_gpu, const std::uint32_t* a_classCounts)
		{
			std::vector<DrawSequence> built;
			for (std::uint32_t c = 0; c < a_parity.classes.size(); ++c)
				for (const std::uint32_t r : { Draws::kClassRangeMain, Draws::ClassRangeOfSlot(kDecalGroups), Draws::ClassRangeOfSlot(kDecalGroups + 1) }) {
					const auto& range = a_parity.classes[c].ranges[r];
					const std::uint32_t drawn = std::min(a_classCounts[Draws::ClassCountWord(c, r)], range.capacity);
					built.insert(built.end(), a_gpu + range.first, a_gpu + range.first + drawn);
				}
			if (const auto& plan = a_parity.classPlan) {
				const auto take = [&](const SetClassPlan::PlainBucket& a_bucket, std::uint32_t a_word) {
					const std::uint32_t drawn = std::min(a_classCounts[a_word], a_bucket.capacity);
					built.insert(built.end(), a_gpu + a_bucket.first, a_gpu + a_bucket.first + drawn);
				};
				for (std::size_t b = 0; b < plan->main.size(); ++b)
					take(plan->main[b], plan->MainCountWord(b));
				for (std::size_t b = 0; b < plan->terrain.size(); ++b)
					take(plan->terrain[b], plan->TerrainCountWord(b));
			}
			return built;
		}

		/** @brief The sub-range of its group that draws a decal slot, or null for a culled or blank slot. Other classes hold zero-count fillers there. */
		const DrawSequence* DrawnDecal(const DrawsState::ParityReadback& a_parity, const DrawSequence* a_gpu, std::uint32_t a_group, std::size_t a_slot)
		{
			for (const auto& entry : a_parity.classes) {
				const auto& range = entry.ranges[Draws::ClassRangeOfSlot(a_group)];
				if (a_slot < range.capacity && a_gpu[range.first + a_slot].indexCount != 0)
					return &a_gpu[range.first + a_slot];
			}
			return nullptr;
		}

		/** @brief Converts a template's set index to its index within its state class, as BuildDraws stores it. */
		void ClassIndexOf(DrawSequence& a_template, const std::vector<std::uint32_t>& a_classOf)
		{
			if (a_template.pipelineIndex < a_classOf.size() && a_classOf[a_template.pipelineIndex] != kNoSetClass)
				a_template.pipelineIndex = a_classOf[a_template.pipelineIndex] & kSetClassIndexMask;
		}
	}

	void CheckBuildParity(const MainEpochDone& a_epoch)
	{
		const auto& a_resources = a_epoch.resources;
		const MainPayload& a_payload = a_epoch.payload;
		const auto& shadowVolumes = DrawsView::Get().shadowVolumes;
		auto& state = DrawsState::Get();
		auto& parity = state.parity;
		auto& parityEpochs = state.parityEpochs;
		using ParityReadback = DrawsState::ParityReadback;
		const auto& sequences = a_payload.body->sequences;
		const auto& decalTemplates = a_payload.body->decalTemplates;
		const auto& inputs = a_payload.body->inputList;
		auto* context = globals::d3d::context;
		// Every main epoch's payload, to tell where a row outside its epoch's tables came from.
		if (const auto* seen = a_epoch.payloadVersions; seen && seen->main) {
			auto& history = state.parityPayloads;
			const auto newest = state.parityPayloadsSeen ? &history[(state.parityPayloadsSeen - 1) % history.size()] : nullptr;
			if (!newest || newest->serial != seen->serial) {
				const auto& material = (*seen)[Draws::PayloadPart::MaterialRows];
				const auto& pipeline = (*seen)[Draws::PayloadPart::PipelineRows];
				history[state.parityPayloadsSeen++ % history.size()] = { seen->serial, parityEpochs, material.address, material.units * kMaterialRowBytes,
					pipeline.address, pipeline.units * kPipelineRowBytes };
			}
		}
		if (parity) {
			if (--parity->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE countMap{}, sequencesMap{}, classCountsMap{};
			if (FAILED(context->Map(parity->count.get(), 0, D3D11_MAP_READ, 0, &countMap)) ||
				FAILED(context->Map(parity->sequences.get(), 0, D3D11_MAP_READ, 0, &sequencesMap))) {
				parity.reset();
				return;
			}
			if (FAILED(context->Map(parity->classCounts.get(), 0, D3D11_MAP_READ, 0, &classCountsMap))) {
				context->Unmap(parity->count.get(), 0);
				context->Unmap(parity->sequences.get(), 0);
				parity.reset();
				return;
			}
			const std::uint32_t count = static_cast<const std::uint32_t*>(countMap.pData)[0];
			const std::uint32_t culled = static_cast<const std::uint32_t*>(countMap.pData)[1];
			const auto* gpuSequences = static_cast<const DrawSequence*>(sequencesMap.pData);
			// The colour and terrain draws, from their state class sub-ranges.
			std::vector<DrawSequence> built = ClassSequences(*parity, gpuSequences, static_cast<const std::uint32_t*>(classCountsMap.pData));
			context->Unmap(parity->classCounts.get(), 0);
			// Compares a sequence with its template field by field, rows shown as table indices or "-" when outside.
			// For outside rows it names the recent payload whose tables hold them.
			auto originOf = [&](std::uint64_t a_address) -> std::string {
				const auto& history = state.parityPayloads;
				const std::uint32_t kept = std::min<std::uint32_t>(state.parityPayloadsSeen, static_cast<std::uint32_t>(history.size()));
				for (std::uint32_t k = 1; k <= kept; ++k) {
					const auto& seen = history[(state.parityPayloadsSeen - k) % history.size()];
					const bool material = a_address >= seen.materialBase && a_address < seen.materialBase + seen.materialBytes;
					const bool pipeline = a_address >= seen.pipelineBase && a_address < seen.pipelineBase + seen.pipelineBytes;
					if (material || pipeline)
						return fmt::format(", {} rows of payload {} (seen {} colour epochs before the copy)", material ? "material" : "pipeline", seen.serial,
							static_cast<std::int64_t>(parity->armedEpoch) - static_cast<std::int64_t>(seen.epoch));
				}
				return ", in no recent payload";
			};
			auto rowOf = [&](std::uint64_t a_address, std::uint64_t a_base, std::uint64_t a_bytes) {
				return a_address >= a_base && (a_address - a_base) % a_bytes == 0 ? fmt::format("{}", (a_address - a_base) / a_bytes) :
				                                                                    fmt::format("- ({:#x}{})", a_address, originOf(a_address));
			};
			auto describe = [&](const DrawSequence& a_gpu, const DrawSequence& a_cpu) {
				const auto pipelineRow = [&](const DrawSequence& a) { return rowOf(a.pipelineRowAddress, parity->pipelineRowsBase, kPipelineRowBytes); };
				const auto materialRow = [&](const DrawSequence& a) { return rowOf(a.materialRowAddress, parity->materialRowsBase, kMaterialRowBytes); };
				return fmt::format("GPU vs CPU: pipeline {} vs {}, pipeline row {} vs {}, material row {} vs {}, vertices {}+{:#x}/{} vs {}+{:#x}/{}, indices {:#x}/{} vs {:#x}/{}, "
								   "index count {} vs {}, first index {} vs {}",
					a_gpu.pipelineIndex, a_cpu.pipelineIndex, pipelineRow(a_gpu), pipelineRow(a_cpu), materialRow(a_gpu), materialRow(a_cpu), a_gpu.vertexBufferRef.viewSlot,
					a_gpu.vertexBufferRef.byteOffset, a_gpu.vertexBufferSize, a_cpu.vertexBufferRef.viewSlot, a_cpu.vertexBufferRef.byteOffset, a_cpu.vertexBufferSize, a_gpu.indexBufferAddress, a_gpu.indexBufferSize, a_cpu.indexBufferAddress,
					a_cpu.indexBufferSize, a_gpu.indexCount, a_cpu.indexCount, a_gpu.firstIndex, a_cpu.firstIndex);
			};
			// Local shadow lights from each object word against the CPU's selection, then masked out since templates lack them.
			std::size_t localChecked = 0, localDiffering = 0;
			std::string localFirst;
			drawnLocalShadows.clear();
			auto takeLocalShadows = [&](DrawSequence& a_sequence) {
				const std::uint32_t gpu = (a_sequence.objectIndex & kObjectLocalShadowMask) >> kObjectLocalShadowShift;
				a_sequence.objectIndex &= ~kObjectLocalShadowMask;
				const std::uint32_t object = a_sequence.objectIndex & kObjectIndexMask;
				const auto it = parity->expectedLocalShadows.find(object);
				const std::uint32_t cpu = it != parity->expectedLocalShadows.end() ? it->second : 0u;
				++localChecked;
				auto& drawn = drawnLocalShadows[object];
				drawn = { gpu, parity->gatedLocalShadows };
				drawn.channels = parity->armedChannels;
				if (const auto bound = parity->armedBounds.find(object); bound != parity->armedBounds.end())
					std::memcpy(drawn.bound, bound->second.data(), sizeof(drawn.bound));
				if ((gpu & ~parity->gatedLocalShadows) != (cpu & ~parity->gatedLocalShadows) && localDiffering++ == 0)
					localFirst = fmt::format("object {}: GPU {:X}, CPU {:X}", object, gpu, cpu);
			};
			for (auto& sequence : built)
				takeLocalShadows(sequence);
			// Decal slots are fixed, so they compare in place in their own class's sub-range. A culled slot is the template with
			// index count zero. Any other difference is a defect.
			std::size_t decalDiffering = 0, decalCulled = 0, decalSlots = 0;
			std::string decalFirst;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				const auto& expectedDecals = parity->expectedDecals[group];
				for (std::size_t slot = 0; slot < expectedDecals.size() && slot < parity->sequenceDecals; ++slot, ++decalSlots) {
					// A zero-count slot draws nothing whatever else it holds. It is a culled decal or a blank, so only the count matters.
					const auto* drawn = DrawnDecal(*parity, gpuSequences, group, slot);
					if (!drawn) {
						++decalCulled;
						continue;
					}
					DrawSequence decal = *drawn;
					takeLocalShadows(decal);
					decal.objectIndex &= ~kObjectSunMiss;  // the CPU's template has no cascade test
					if (std::memcmp(&decal, &expectedDecals[slot], sizeof(DrawSequence)) != 0 && decalDiffering++ == 0)
						decalFirst = fmt::format("group {} slot {} (object {}): {}", group, slot, decal.objectIndex & kObjectIndexMask, describe(decal, expectedDecals[slot]));
				}
			}
			context->Unmap(parity->count.get(), 0);
			context->Unmap(parity->sequences.get(), 0);
			// BuildDraws appends in any order, so compare as sets keyed by object index. Record addresses are shared by deduplicated records.
			// Multi-partition skins write one sequence per partition, which can share an index buffer, so the key adds the first index.
			for (auto& sequence : built)
				sequence.objectIndex &= ~kObjectSunMiss;
			// Plain draws are pooled with no pipeline index, pooled indices and the bucket slot as first instance. Those fields are skipped on both sides.
			auto& expected = parity->expected;
			if (const auto& plan = parity->classPlan; plan && (!plan->main.empty() || !plan->terrain.empty())) {
				const auto plainFields = [](DrawSequence& a_sequence) {
					a_sequence.pipelineIndex = 0;
					a_sequence.indexCount = 0;
					a_sequence.firstIndex = 0;
					a_sequence.vertexOffset = 0;
					a_sequence.firstInstance = 0;
				};
				for (auto& sequence : built)
					plainFields(sequence);
				for (auto& sequence : expected)
					plainFields(sequence);
			}
			auto keyOf = [](const DrawSequence& a) { return std::tuple(a.objectIndex, a.indexBufferAddress, a.firstIndex); };
			auto byObject = [&](const DrawSequence& a, const DrawSequence& b) { return keyOf(a) < keyOf(b); };
			std::sort(built.begin(), built.end(), byObject);
			std::sort(expected.begin(), expected.end(), byObject);
			// Culling makes the GPU output a subset of the templates. Every sequence written must match a template of the same object byte for byte.
			std::size_t differing = 0, missing = 0;
			std::size_t first = SIZE_MAX, firstExpected = 0;
			for (std::size_t b = 0, e = 0; b < built.size(); ++b) {
				while (e < expected.size() && byObject(expected[e], built[b]))
					++e;  // a template the culling rejected, or an undrawn partition
				if (e == expected.size() || keyOf(expected[e]) != keyOf(built[b])) {
					++missing;  // no culling can explain a sequence with no template
					continue;
				}
				if (std::memcmp(&built[b], &expected[e], sizeof(DrawSequence)) != 0) {
					if (first == SIZE_MAX) {
						first = b;
						firstExpected = e;
					}
					++differing;
				}
				++e;
			}
			logger::info("[DCLF] BuildDraws local shadow lights {}: {} draws, {} differ from the CPU's selection{}{}", localDiffering ? "MISMATCH" : "OK",
				localChecked, localDiffering, localFirst.empty() ? "" : "; first: ", localFirst);
			if (decalSlots)
				logger::info("[DCLF] BuildDraws decal parity {}: {} slots, {} culled, {} differ{}{}", decalDiffering ? "MISMATCH" : "OK", decalSlots, decalCulled, decalDiffering,
					decalFirst.empty() ? "" : "; first: ", decalFirst);
			if (!differing && !missing) {
				logger::info("[DCLF] BuildDraws parity OK: {} of {} sequences match the CPU templates ({} rejected by the culling)", count, expected.size(),
					expected.size() - count);
			} else {
				logger::warn("[DCLF] BuildDraws parity MISMATCH: GPU wrote {}, CPU templated {} ({} counted culled); {} differ, {} have no template; sequences version {} "
							 "named, {} current{}",
					count, expected.size(), culled, differing, missing, parity->sequencesNamed, parity->sequencesCurrent,
					first != SIZE_MAX ? fmt::format(" (first: object {}, {})", built[first].objectIndex, describe(built[first], expected[firstExpected])) : "");
			}
			parity.reset();
			return;
		}
		// Colour epochs only. The Z-prepass draws into its pipeline buckets, so the plain range and decal slots still hold the last colour epoch's.
		if (a_epoch.depthOnly || (parityEpochs++ % 300) != 0 || !a_resources->countD3D11)
			return;
		// The version the recording wrote is the one the adopted set names, which need not be the buffer's current one.
		auto namedSequences = DrawsView::Get().Adopted().Find(*a_resources->sequences);
		const auto currentSequences = a_resources->sequences->Current();
		const std::uint64_t sequencesNamed = namedSequences ? namedSequences->number : 0;
		const std::uint64_t sequencesCurrent = currentSequences ? currentSequences->number : 0;
		auto* const sequencesView = state.paritySequences.Of(namedSequences ? std::move(namedSequences) : currentSequences);
		if (!sequencesView)
			return;
		// After the epoch in D3D11 stream order, so the copies see what BuildDraws wrote.
		auto staging = [&](ID3D11Buffer* a_source) {
			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Buffer> buffer;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put())))
				return buffer;
			context->CopyResource(buffer.get(), a_source);
			return buffer;
		};
		// None means the epoch drew no payload to compare.
		const auto* payloadVersions = a_epoch.payloadVersions;
		if (!payloadVersions || !payloadVersions->main)
			return;
		ParityReadback readback;
		readback.pipelineRowsBase = (*payloadVersions)[Draws::PayloadPart::PipelineRows].address;
		readback.materialRowsBase = (*payloadVersions)[Draws::PayloadPart::MaterialRows].address;
		readback.sequences = staging(sequencesView);
		readback.count = staging(a_resources->countD3D11.get());
		// The ranges of the adopted revision's shape.
		const auto& active = DrawsView::Get().revisions.active;
		const auto epoch = Draws::EpochOfShape(Draws::ShapeKind::Main, static_cast<std::uint32_t>(a_epoch.depthOnly ? kDepthShape : kColourShape));
		const auto shape = active ? active->Get<PassFrame>(DrawsView::Impl::SceneRevisions::kShapeSlot + epoch) : nullptr;
		if (!shape)
			return;
		readback.sequenceDraws = shape->sequenceDraws;
		readback.sequenceDecals = shape->sequenceDecals;
		readback.classes = shape->classes;
		readback.classPlan = shape->classPlan;
		// Class counts, at the version the adopted set names.
		auto* const classCountsView = state.parityClassCounts.Of(DrawsView::Get().Adopted().Find(*a_resources->classCounts));
		if (!classCountsView)
			return;
		readback.classCounts = staging(classCountsView);
		// The templates cover every candidate. The gate is a pure per-object flag test, so the expectation applies it exactly.
		// Frustum culling cannot be predicted here, which is why parity and culling are separate switches.
		readback.expected.clear();
		readback.expected.reserve(sequences.size());
		// Place the template rows at the payload's tables and convert set indices to class indices.
		// An animated row's draws read the frame's copy of that row, as BuildDrawsCS's MaterialRowAddress does.
		const std::uint64_t animatedRows = Draws::PassAnimatedRows().address;
		auto place = [&](DrawSequence& a_sequence) {
			a_sequence.pipelineRowAddress += readback.pipelineRowsBase;
			const auto* row = animatedRows ? a_payload.body->materialRows.At(a_sequence.materialRowAddress / kMaterialRowBytes) : nullptr;
			const std::uint32_t animatedRow = row ? reinterpret_cast<const MaterialRowHeader*>(row->bytes.data() + kMaterialRowHeader)->animatedRow : ~0u;
			a_sequence.materialRowAddress = animatedRow != ~0u ? animatedRows + std::uint64_t(animatedRow) * kMaterialRowBytes :
			                                                     a_sequence.materialRowAddress + readback.materialRowsBase;
			SetSequenceBlocks(a_sequence);  // as BuildDrawsCS's StoreSequence does
			ClassIndexOf(a_sequence, shape->indirect.classOf);
		};
		for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
			readback.expectedDecals[group] = decalTemplates[group];
			for (auto& decal : readback.expectedDecals[group])
				place(decal);
		}
		{
			// Cull-only inputs carry no sequence, so drawable ones are counted off rather than indexed in step.
			// Decals have their own slots. A multi-partition skin has consecutive templates, one per drawn partition.
			std::size_t sequence = 0;
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || (input.flags & kObjectDecal))
					continue;
				const std::size_t templates = PartitionDraws(input.partitions);
				if (sequence + templates > sequences.size())
					break;
				const std::size_t first = sequence;
				sequence += templates;
				// Forward draws are sorted in their own range and not compared here.
				if (input.flags & kObjectForward)
					continue;
				readback.expected.insert(readback.expected.end(), sequences.begin() + first, sequences.begin() + sequence);
			}
			for (auto& expected : readback.expected)
				place(expected);
		}
		// The CPU's local shadow selection for every drawn object. None on the Z-prepass.
		if (!a_payload.inputs.depthOnly) {
			const auto& tables = Scene::FrameView::Get().GetTables();
			const std::uint64_t landscapeFlags = Extensions::LightVolumes().landscapeFlags;
			readback.gatedLocalShadows = 0;
			readback.armedChannels = ChannelLights();
			for (const auto& volume : shadowVolumes)
				if (volume.portalContext)
					readback.gatedLocalShadows |= volume.maskBit & 0xFu;
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || input.objectIndex >= tables.objects.size() || input.objectIndex >= tables.objectGeometry.size())
					continue;
				const auto* geometry = tables.objectGeometry[input.objectIndex];
				const auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				if (!geometry)
					continue;
				const float center[3]{ geometry->worldBound.center.x, geometry->worldBound.center.y, geometry->worldBound.center.z };
				const bool landscape = property && (property->flags.underlying() & landscapeFlags);
				readback.expectedLocalShadows[input.objectIndex] = ShadowVolumeMaskOf(shadowVolumes, landscape, center, geometry->worldBound.radius) & 0xFu;
				readback.armedBounds[input.objectIndex] = { center[0], center[1], center[2], geometry->worldBound.radius };
			}
		}
		readback.sequencesNamed = sequencesNamed;
		readback.sequencesCurrent = sequencesCurrent;
		readback.armedEpoch = parityEpochs;
		readback.framesLeft = 3;
		if (readback.sequences && readback.count && readback.classCounts)
			parity = std::move(readback);
	}

	std::array<const void*, 4> ChannelLights()
	{
		std::array<const void*, 4> out{};
		const auto* node = Engine::Global<RE::ShadowSceneNode*>(Engine::Address::kShadowSceneNode);
		if (!node)
			return out;
		for (const auto& pointer : node->GetRuntimeData().activeShadowLights)
			if (const auto* light = pointer.get(); light && Engine::At<std::uint32_t>(light, 0x520) < out.size())
				out[Engine::At<std::uint32_t>(light, 0x520)] = light;
		return out;
	}

	bool DrawnLocalShadowsOf(std::uint32_t a_object, DrawnLocalShadows& a_out)
	{
		const auto it = drawnLocalShadows.find(a_object);
		if (it == drawnLocalShadows.end())
			return false;
		a_out = it->second;
		return true;
	}
}
