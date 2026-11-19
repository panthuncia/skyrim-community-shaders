#pragma once

// The shadow sets: each slot's shadow inputs, the texture and key indices and their order, and the emitted lists.
// A private base of Scene::Coordinator. Its functions are declared in Coordinator.h.

namespace DCLF::Scene
{
	struct CoordinatorShadowSets
	{
		struct ShadowInputs
		{
			ID3D11ShaderResourceView* diffuse = nullptr;
			std::uint64_t vertexDesc = 0;
			std::uint32_t technique = 0;
			std::uint32_t flags = 0;
			std::uint32_t reject = 0;
			std::array<std::uint32_t, kOcclusionViews> occlusionTechnique{};
			bool operator==(const ShadowInputs&) const = default;
		};
		bool shadowSetsDirty = true;
		bool shadowIndexNeedsRebuild = true;
		std::vector<std::uint32_t> shadowDirtySlots;
		std::vector<ShadowInputs> shadowIndexedInputs;
		std::vector<std::uint8_t> shadowIndexedLive;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::set<std::uint32_t>> shadowTextureMembers;
		ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash> shadowKeyMembers;
		std::array<ankerl::unordered_dense::map<ShadowPipelineKey, std::set<std::uint32_t>, ShadowPipelineKeyHash>, kOcclusionViews> occlusionKeyMembers;
		// Each index's keys by their lowest member slot, kept in order as members come and go.
		std::map<std::uint32_t, ID3D11ShaderResourceView*> shadowTextureOrder;
		std::map<std::uint32_t, ShadowPipelineKey> shadowKeyOrder;
		std::array<std::map<std::uint32_t, ShadowPipelineKey>, kOcclusionViews> occlusionKeyOrder;
		ankerl::unordered_dense::map<ID3D11ShaderResourceView*, std::int32_t> shadowTextureNet;
		// A list is made again only when its order moved. The tables take copies.
		std::vector<ID3D11ShaderResourceView*> shadowEmittedTextures;
		ankerl::unordered_dense::set<ID3D11ShaderResourceView*> shadowEmittedSeen;
		std::vector<ShadowPipelineKey> shadowEmittedKeys;
		std::array<std::vector<ShadowPipelineKey>, kOcclusionViews> shadowEmittedOcclusionKeys;
		struct ShadowSetStats
		{
			std::uint64_t refreshes = 0, rebuilds = 0;  // calls; of them index rebuilds (an explicit invalidation, or parity's)
			std::uint64_t journaled = 0, applied = 0;   // slots the journal named (deduplicated); of them with inputs or liveness moved
			std::uint64_t unjournaled = 0;              // dirty with nothing journaled: the lists emitted again, the index untouched
			std::uint64_t listsEmitted = 0;             // public lists made again from their order (texture, key, per occlusion view)
		};
		ShadowSetStats shadowSetStats;
		std::uint32_t keptShadowCasters = 0;
		std::array<std::uint32_t, 16> keptShadowRejects{};
	};
}
