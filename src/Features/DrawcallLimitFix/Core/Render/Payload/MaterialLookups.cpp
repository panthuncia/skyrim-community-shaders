// Resolves the Lighting and shadow pipelines a publication's tables use from the pipeline lane's catalog.
// The scene lane does this without reading the engine or any render-thread service.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Snapshot/EngineReads.h"

#include <algorithm>
#include <iterator>

namespace DCLF::Draws
{
	namespace
	{
		bool SameUsage(const Lookups::RegisterUsageBits& a_bits, const RegisterUsage& a_usage)
		{
			return a_bits.vertexConstants == a_usage.vertexConstants && a_bits.pixelConstants == a_usage.pixelConstants && a_bits.textures == a_usage.textures &&
			       a_bits.samplers == a_usage.samplers;
		}

		/** @brief Called when the set was recreated, so every index a build may hold is stale. */
		void BeginPipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups)
		{
			const std::uint32_t setGeneration = a_catalog ? a_catalog->generation : 0u;
			if (a_lookups.pipelineSetGeneration != setGeneration) {
				a_lookups.pipelineSetGeneration = setGeneration;
				++a_lookups.generation;
				++a_lookups.shadowGeneration;
			}
			if (a_lookups.pipelines.size() != a_tables.pipelines.size())
				a_lookups.pipelines.resize(a_tables.pipelines.size());
		}

		void ResolvePipelineSlot(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, std::size_t p)
		{
			// Read before writing. A write copies the entry's shared chunk, so unchanged entries must not be written.
			const auto& current = a_lookups.pipelines[p];
			if (!a_tables.PipelineUsed(p)) {
				if (current.setIndex != Lookups::kNone) {
					auto& entry = a_lookups.pipelines.Mutable(p);
					entry.version = a_lookups.NextVersion();
					entry.setIndex = Lookups::kNone;
				}
				return;
			}
			const auto& key = a_tables.pipelines[p];
			const auto* built = a_catalog ? a_catalog->Find(key) : nullptr;
			const std::uint32_t resolved = built ? built->setIndex : Lookups::kNone;
			bool same = current.key == key && current.requested && current.setIndex == resolved;
			if (same && resolved != Lookups::kNone)
				same = current.vsTable == built->tables->vs && current.psTable == built->tables->ps && SameUsage(current.usage[0], built->usage[0]) &&
				       SameUsage(current.usage[1], built->usage[1]);
			if (same)
				return;
			auto& entry = a_lookups.pipelines.Mutable(p);
			// Write only on change so an entry's version moves only when it changed.
			// A re-keyed entry keeps its technique mask, which belongs to the shared bindings.
			bool changed = false;
			if (!(entry.key == key)) {
				entry.key = key;
				entry.setIndex = Lookups::kNone;
				entry.requested = false;
				changed = true;
			}
			// Ask the lane once per key. It keeps every key.
			if (!entry.requested) {
				DrawPipelines::Get().RequestLighting(key, static_cast<std::uint32_t>(p));
				entry.requested = true;
			}
			if (entry.setIndex != Lookups::kNone && entry.setIndex != resolved)
				++a_lookups.generation;  // a build may hold the old index
			changed |= entry.setIndex != resolved;
			entry.setIndex = resolved;
			if (resolved != Lookups::kNone) {
				// The lane's constant tables, reflected from DCLF's own modules. Copied only where they differ.
				auto assign = [&](std::vector<std::uint8_t>& a_table, const std::vector<std::uint8_t>& a_source) {
					if (a_table != a_source) {
						a_table = a_source;
						changed = true;
					}
				};
				assign(entry.vsTable, built->tables->vs);
				assign(entry.psTable, built->tables->ps);
				for (std::uint32_t variant = 0; variant < 2; ++variant) {
					const auto& usage = built->usage[variant];
					auto& bits = entry.usage[variant];
					changed |= !SameUsage(bits, usage);
					bits.vertexConstants = usage.vertexConstants;
					bits.pixelConstants = usage.pixelConstants;
					bits.textures = usage.textures;
					bits.samplers = usage.samplers;
				}
			}
			if (changed)
				entry.version = a_lookups.NextVersion();
		}

		/** @brief Resolves only the slots whose key binding or used bit moved. Every slot when the catalog or lookups instance changed. */
		void ResolveChangedPipelineLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, LookupsResolveState& a_state)
		{
			DCLF_ZONE("CS.DCLF.Scene.PipelineLookups");
			BeginPipelineLookups(a_tables, a_catalog, a_lookups);
			const std::uint64_t revision = a_catalog ? a_catalog->revision : 0;
			const std::uint32_t generation = a_catalog ? a_catalog->generation : ~0u;
			if (a_state.pipelineCatalog != a_catalog || a_state.pipelineCatalogRevision != revision || a_state.pipelineCatalogGeneration != generation ||
				a_state.pipelineInstance != a_lookups.instance) {
				a_state.pipelineCatalog = a_catalog;
				a_state.pipelineCatalogRevision = revision;
				a_state.pipelineCatalogGeneration = generation;
				a_state.pipelineInstance = a_lookups.instance;
				a_state.pipelineWitness.assign(a_tables.pipelines.size(), ~0ull);
				++a_state.pipelineSweeps;
			}
			a_state.pipelineWitness.resize(a_tables.pipelines.size(), ~0ull);
			a_state.pipelinesChecked += a_tables.pipelines.size();
			for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
				const std::uint32_t binding = p < a_tables.pipelineBindingVersion.size() ? a_tables.pipelineBindingVersion[p] : 0u;
				const std::uint64_t witness = (std::uint64_t(binding) << 1) | (a_tables.PipelineUsed(p) ? 1u : 0u);
				if (a_state.pipelineWitness[p] == witness)
					continue;
				ResolvePipelineSlot(a_tables, a_catalog, a_lookups, p);
				a_state.pipelineWitness[p] = witness;
				++a_state.pipelinesResolved;
			}
		}
	}

	void ResolveShadowLookups(const SceneTables& a_tables, const PipelineCatalog* a_catalog, const ShadowLookupInputs& a_inputs, LookupsResolveState& a_state,
		Lookups& a_lookups)
	{
		DCLF_ZONE("CS.DCLF.Scene.ShadowLookups");
		// Shadow pipelines resolve only from the catalog the lookups are published with, so the indices and the set agree.
		if (!a_catalog)
			return;
		// The shadow set was recreated by a shadow map format change, so every index a build may hold is stale.
		if (a_lookups.shadowSetGeneration != a_catalog->shadowGeneration) {
			a_lookups.shadowSetGeneration = a_catalog->shadowGeneration;
			for (auto& found : a_lookups.shadowPipelines)
				found.second = Lookups::kNone;
			for (auto& modeRows : a_lookups.shadowMapRows)
				for (auto& row : modeRows)
					std::ranges::fill(row, Lookups::kNone);
			++a_lookups.shadowGeneration;
		}
		static_assert(std::tuple_size_v<decltype(a_state.shadowModeStates)> == kShadowModeCount);
		auto& modeStates = a_state.shadowModeStates;
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			const auto& states = a_inputs.rasterStates[m];
			modeStates[m].clear();
			std::set_union(states.casters.begin(), states.casters.end(), states.volumetric.begin(), states.volumetric.end(), std::back_inserter(modeStates[m]));
		}
		// A resolution that found every pipeline for the same inputs would find the same ones again, so it is skipped.
		// A found pipeline stays in the set until the set is recreated.
		auto& resolvedFor = a_state.shadowResolvedScratch;
		resolvedFor.clear();
		resolvedFor.push_back(a_lookups.instance);
		resolvedFor.push_back((std::uint64_t(a_catalog->shadowGeneration) << 32) | a_catalog->shadowFormat);
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_inputs.modes[m])
				continue;
			const auto& keys = IsOcclusionMode(m) ? a_tables.occlusionKeysUsed[OcclusionOfMode(m)] : a_tables.shadowKeysUsed;
			resolvedFor.push_back((std::uint64_t(m) << 32) | (IsOcclusionMode(m) ? a_inputs.occlusionFormats[OcclusionOfMode(m)] : a_inputs.dsvFormat));
			resolvedFor.push_back((std::uint64_t(keys.size()) << 32) | modeStates[m].size());
			for (const auto& key : keys) {
				resolvedFor.push_back((std::uint64_t(key.technique) << 32) | key.rasterFlags);
				resolvedFor.push_back(key.vertexLayout);
			}
			for (const std::uint32_t state : modeStates[m])
				resolvedFor.push_back(state);
		}
		if (resolvedFor == a_state.shadowPipelinesResolvedFor) {
			++a_state.shadowSkipped;
			return;
		}
		++a_state.shadowResolved;
		bool resolved = true;
		auto& pipelines = DrawPipelines::Get();
		// Merged view pipelines use the canonical depth-only technique and no layout, so slots of one output share a bucket.
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			if (!a_inputs.modes[m])
				continue;
			// The shadow set is built for one format. A mode whose views draw into another format has no pipeline in it.
			const auto& keys = IsOcclusionMode(m) ? a_tables.occlusionKeysUsed[OcclusionOfMode(m)] : a_tables.shadowKeysUsed;
			const DXGI_FORMAT format = IsOcclusionMode(m) ? a_inputs.occlusionFormats[OcclusionOfMode(m)] : a_inputs.dsvFormat;
			const bool inSet = format != DXGI_FORMAT_UNKNOWN && format == a_catalog->shadowFormat;
			for (const auto& key : keys) {
				const std::uint32_t technique = ViewPipelineTechnique(key.technique, m);
				const ShadowPipelineKey slotKey{ key.technique, key.rasterFlags, key.vertexLayout };
				auto slotIt = a_lookups.shadowSlots.find(slotKey);
				if (slotIt == a_lookups.shadowSlots.end()) {
					// The latch's map rows have room for every slot a publication can name.
					slotIt = a_lookups.shadowSlots.emplace(slotKey, static_cast<std::uint32_t>(a_lookups.shadowSlotKeys.size())).first;
					a_lookups.shadowSlotKeys.push_back(slotKey);
					++a_lookups.shadowGeneration;
				}
				const std::uint32_t slot = slotIt->second;
				const std::uint32_t occlusion = IsOcclusionMode(m) ? OcclusionOfMode(m) : ~0u;
				for (const std::uint32_t state : modeStates[m]) {
					const ShadowPipelineKey viewKey{ technique, slotKey.rasterFlags, 0, state };
					// Ask the lane once per key. It keeps every key and rebuilds them all after a format change.
					if (a_state.shadowRequested.insert(viewKey).second)
						pipelines.RequestShadow(viewKey, key, occlusion);
					const auto* built = inSet ? a_catalog->FindShadow(viewKey) : nullptr;
					const std::uint32_t index = built ? built->setIndex : Lookups::kNone;
					// A pending pipeline is looked up again next pass. A failed key is final until the inputs change.
					resolved &= !inSet || (built && (built->setIndex != PipelineCatalog::kNone || built->failed));
					auto [it, inserted] = a_lookups.shadowPipelines.try_emplace(viewKey, index);
					if (inserted || it->second != index) {
						++a_lookups.shadowGeneration;
						it->second = index;
					}
					auto& rows = a_lookups.shadowMapRows[m];
					if (rows.size() <= state) {
						rows.resize(std::size_t(state) + 1);
						++a_lookups.changes;
					}
					auto& row = rows[state];
					if (row.size() <= slot) {
						row.resize(slot + 1, Lookups::kNone);
						++a_lookups.changes;
					}
					// Two modes with the same techniques share the pipeline but not the row, so the row bumps the generation itself.
					if (row[slot] != index) {
						row[slot] = index;
						++a_lookups.shadowGeneration;
					}
				}
			}
		}
		// Only remember a full resolution. Swapped so the scratch keeps its buffer.
		if (resolved)
			std::swap(a_state.shadowPipelinesResolvedFor, resolvedFor);
		else
			a_state.shadowPipelinesResolvedFor.clear();
	}
}

namespace DCLF
{
	void IndirectDraws::ResolveLookups(const void* a_tables, const PipelineCatalog* a_catalog, Lookups& a_lookups, LookupsResolveState& a_state)
	{
		if (!impl)
			return;
		const auto& tables = *static_cast<const SceneTables*>(a_tables);
		++a_state.calls;
		if (EngineShaders::Lighting() && ShaderPrograms::Get().Enabled()) {
			Draws::ResolveChangedPipelineLookups(tables, a_catalog, a_lookups, a_state);
		}
		auto inputs = impl->shadowEngine->shadowLookupInputs.load(std::memory_order_acquire);
		if (inputs && !inputs->enabled)
			inputs.reset();
		// Replace the held inputs together with the resolution below, so readiness and pipelines always agree.
		// The held one stays alive, so the pointer compare is exact.
		if (a_lookups.shadowInputs.get() != static_cast<const void*>(inputs.get())) {
			a_lookups.shadowInputs = inputs;
			// Unique across lookups instances, so a reset never repeats a value the commit last saw.
			static std::atomic<std::uint64_t> shadowInputsSerials{ 0 };
			a_lookups.shadowInputsSerial = shadowInputsSerials.fetch_add(1, std::memory_order_relaxed) + 1;
			++a_lookups.changes;
		}
		if (!inputs || tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return;
		ResolveShadowLookups(tables, a_catalog, *inputs, a_state, a_lookups);
	}

	void IndirectDraws::PostLookupInputs()
	{
		if (!impl)
			return;
		// Publish the import context and fixed bindings before the scene work runs.
		GpuTextures::Get().PublishImportContext();
		UpdateShadowCapability();
		// The shadow pipelines' format, from the engine target set's declarations at setup (DeclaredTargets). It never changes.
		auto& engine = *impl->shadowEngine;
		engine.pipelineShadowFormat = DrawPipelines::Get().ShadowFormat();
		auto& views = engine.lastShadow;
		ShadowLookupInputs inputs;
		inputs.enabled = views.known && CS_ENGINE_READ(Shaders, RecordTime, globals::game::utilityShader);
		if (inputs.enabled) {
			inputs.modes = views.modes;
			inputs.rasterStates = views.rasterStates;
			inputs.dsvFormat = views.dsvFormat;
			inputs.occlusionFormats = engine.OcclusionFormats();
		}
		// Post only on change. The latest wins.
		if (inputs == engine.postedShadowLookups)
			return;
		engine.postedShadowLookups = inputs;
		engine.shadowLookupInputs.store(std::make_shared<const ShadowLookupInputs>(std::move(inputs)), std::memory_order_release);
	}
}
