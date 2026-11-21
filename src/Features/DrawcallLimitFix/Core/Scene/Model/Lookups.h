#pragma once

// Lookups: the descriptor, sampler and pipeline indices a build reads instead of calling the engine, D3D11 or ORG.
// The scene lane writes them, and each publication carries a copy-on-write snapshot.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	/**
	 * @brief A lookup table: 64 entries a chunk, shared copy-on-write with published copies. Sharing is decided by a stamp that
	 * Snapshot moves, never by a reference count a releasing thread moves, and a replaced chunk goes with its last holder.
	 */
	template <class T>
	using LookupChunks = KeptChunkStore<T, 64, KeptSharing::kByStamp>;

	/**
	 * @brief What a build needs from the render thread's services, resolved ahead of it. A build may not call the engine, D3D11,
	 * DXVK interop or ORG, it reads these plain tables instead. Each entry keeps the key it was resolved for, so a reused slot never
	 * serves a previous tenant's indices.
	 *
	 * `generation` moves whenever an entry a main build may have read changes, and a payload built against an older one is not
	 * committed. `shadowGeneration` does the same for shadow builds, so main-pass churn does not stale shadow jobs.
	 */
	struct Lookups
	{
		static constexpr std::uint32_t kNone = ~0u;
		static constexpr std::uint32_t kTextureSlots = 16;

		/** @brief A copy of DrawPipelines' RegisterUsage. */
		struct RegisterUsageBits
		{
			std::uint32_t vertexConstants = 0;  // b0-b13, bit per register
			std::uint32_t pixelConstants = 0;
			std::array<std::uint64_t, 2> textures{};  // pixel t0-t127
			std::uint32_t samplers = 0;                // pixel s0-s15

			bool UsesTexture(std::uint32_t a_register) const { return (textures[a_register / 64] >> (a_register % 64)) & 1; }
		};

		struct Pipeline
		{
			PipelineKey key{};
			std::uint32_t setIndex = kNone;  // in the published PipelineCatalog; kNone while compiling or unused
			bool requested = false;          // key asked of the pipeline lane (DrawPipelines::RequestLighting), once per key
			std::vector<std::uint8_t> vsTable, psTable;
			std::array<RegisterUsageBits, 2> usage{};  // by variant (kColorVariant, kDepthVariant)
			// The technique's shadow mask (t14), if it binds one.
			std::uint32_t shadowMaskIndex = kNone;
			std::shared_ptr<const void> shadowMaskOwner;
			ID3D11ShaderResourceView* shadowMaskView = nullptr;
			std::uint32_t shadowMaskTextureGeneration = 0;
			std::uint32_t version = 0;                 // new whenever any of the above changes (NextVersion)
		};

		/**
		 * @brief The owners a material entry's indices hold, sealed together and never written after. The lease's last release runs on
		 * the cleanup queue.
		 */
		struct MaterialOwners
		{
			std::array<std::shared_ptr<const void>, kTextureSlots> textures{};
			std::array<std::shared_ptr<const void>, Extensions::kFeatureTextureSlots> features{};
			std::shared_ptr<const void> lease;
		};

		/** @brief Plain values and one owner, so a publication's chunk copy moves one reference count per entry. */
		struct Material
		{
			std::pair<const RE::BSShaderMaterial*, std::uint32_t> key{};
			std::array<std::uint32_t, kTextureSlots> textureIndex{};  // descriptor heap indices; kNone where unresolvable
			// Pins every import named here. A new block whenever an owner changes, so its identity names the owner set.
			std::shared_ptr<const MaterialOwners> bindingBlock;
			bool resolved = false;
			std::array<ID3D11ShaderResourceView*, kTextureSlots> views{};
			std::array<std::uint32_t, Extensions::kFeatureTextureSlots> featureIndex{};
			std::array<ID3D11ShaderResourceView*, Extensions::kFeatureTextureSlots> featureViews{};
			std::uint32_t written = 0;
			std::uint32_t texturesGeneration = 0;
			std::uint64_t recordVersion = 0;  // last material record inspected for texture bindings
			std::uint32_t version = 0;  // new whenever key, resolved, textureIndex or featureIndex changes (NextVersion)
		};

		// A publication copies only the chunks written since the last one.
		LookupChunks<Pipeline> pipelines;
		LookupChunks<Material> materials;
		std::vector<std::uint32_t> materialVersions;
		// Copies keep logGeneration, so a reader continues across publications. A reader on another instance or behind the trim starts again.
		EventLog<std::uint32_t> materialLog;
		std::uint32_t logGeneration = NextLogGeneration();
		static std::uint32_t NextLogGeneration()
		{
			static std::atomic<std::uint32_t> counter{ 0 };
			return counter.fetch_add(1, std::memory_order_relaxed) + 1;
		}
		// Sampler heap index per (address mode, filter mode) of the engine's sampler table. kNone where the engine has no state.
		std::array<std::uint32_t, 4 * 5> samplers;
		std::shared_ptr<const void> sharedBindingBlock;  // null, fixed samplers and projected textures
		bool samplersResolved = false;
		std::uint32_t nullTexture = kNone;
		std::array<std::uint32_t, 4> projectedTextures{ kNone, kNone, kNone, kNone };
		std::array<std::shared_ptr<const void>, 4> projectedOwners{};
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowPipelines;
		// A shadow input names its caster's key slot, not a pipeline, so one slot serves every mode. Append-only.
		// shadowMapRows[mode][state][slot] is that slot's pipeline, or kNone while missing.
		ankerl::unordered_dense::map<ShadowPipelineKey, std::uint32_t, ShadowPipelineKeyHash> shadowSlots;
		std::vector<ShadowPipelineKey> shadowSlotKeys;
		std::array<std::vector<std::vector<std::uint32_t>>, 5> shadowMapRows;  // by mode (kShadowModeCount)
		std::span<const std::uint32_t> ShadowMapRow(std::uint32_t a_mode, std::uint32_t a_state) const
		{
			if (a_mode >= shadowMapRows.size() || a_state >= shadowMapRows[a_mode].size())
				return {};
			return shadowMapRows[a_mode][a_state];
		}
		std::uint32_t ShadowMapPipeline(std::uint32_t a_mode, std::uint32_t a_state, std::uint32_t a_slot) const
		{
			const auto row = ShadowMapRow(a_mode, a_state);
			return a_slot < row.size() ? row[a_slot] : kNone;
		}
		// kNone means rejected. A texture still pending is absent, so its casters wait.
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::uint32_t> shadowTextures;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::shared_ptr<const void>> shadowTextureOwners;
		std::uint32_t pipelineSetGeneration = ~0u;
		std::uint32_t shadowSetGeneration = ~0u;
		// Moving shadowInputsSerial makes CommitSet retake members and waiting slots. It is unique across instances.
		std::shared_ptr<const void> shadowInputs;
		std::uint64_t shadowInputsSerial = 0;
		// Unique across Pipeline::version and Material::version.
		std::uint32_t sharedVersion = 0;
		std::uint32_t versionCounter = 0;
		std::uint32_t NextVersion() { return ++versionCounter; }
		std::uint32_t generation = 0;
		std::uint32_t shadowGeneration = 0;
		// A reset makes a new instance whose generations restart, so state kept against a generation must key on this too.
		std::uint64_t instance = NextInstance();
		// Writes outside the chunks that nothing versions. Part of ChangeKey.
		std::uint64_t changes = 0;

		Lookups() { samplers.fill(kNone); }
		static std::uint64_t NextInstance()
		{
			static std::atomic<std::uint64_t> next{ 0 };
			return next.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		static std::uint32_t SamplerIndex(std::uint32_t a_addressMode, std::uint32_t a_filterMode)
		{
			return a_addressMode < 4 && a_filterMode < 5 ? a_addressMode * 5 + a_filterMode : kNone;
		}
		std::uint32_t Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode) const
		{
			const auto i = SamplerIndex(a_addressMode, a_filterMode);
			return i == kNone ? kNone : samplers[i];
		}

		/** @brief Equal while nothing was written. */
		std::array<std::uint64_t, 6> ChangeKey() const
		{
			return { instance, (std::uint64_t(versionCounter) << 32) | generation, (std::uint64_t(shadowGeneration) << 32) | sharedVersion, materialLog.End(),
				changes, pipelines.Writes() + materials.Writes() };
		}
		/** @brief Scene lane. Shares every chunk, and copies maps and small tables whole. */
		std::shared_ptr<const Lookups> Snapshot()
		{
			auto copy = std::make_shared<const Lookups>(*this);
			pipelines.Freeze();
			materials.Freeze();
			return copy;
		}
	};

	/** @brief The scene lane's unpublished bookkeeping for ResolveLookups, so unchanged resolutions are skipped. */
	struct LookupsResolveState
	{
		ankerl::unordered_dense::set<ShadowPipelineKey, ShadowPipelineKeyHash> shadowRequested;
		std::vector<std::uint64_t> shadowPipelinesResolvedFor;
		// A slot whose witness stands would resolve to what it holds, since a key changes only with a new binding version and a catalog is immutable.
		const void* pipelineCatalog = nullptr;
		std::uint64_t pipelineCatalogRevision = 0;
		std::uint32_t pipelineCatalogGeneration = ~0u;
		std::uint64_t pipelineInstance = 0;
		std::vector<std::uint64_t> pipelineWitness;
		// Kept so an unchanged pass allocates nothing.
		std::vector<std::uint64_t> shadowResolvedScratch;
		std::array<std::vector<std::uint32_t>, 5> shadowModeStates;
		std::uint64_t calls = 0, pipelinesChecked = 0, pipelinesResolved = 0, pipelineSweeps = 0, shadowSkipped = 0, shadowResolved = 0;
	};
}
