#pragma once

#include <ankerl/unordered_dense.h>

#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/TextureParity.h"
#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/Diagnostics/WetnessParity.h"
#include "Features/DrawcallLimitFix/Integrations/LightLimitFix/LightLimitFix.h"
#include "Features/DrawcallLimitFix/Integrations/TruePBR/Diagnostics/TextureParity.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"

namespace DCLF
{
	/**
	 * @brief Compares what the native main pass draws against what the tables say DCLF would draw.
	 * Also counts statically eligible geometry under a drawn category node that is not tracked. Render thread.
	 */
	class CaptureParity
	{
	public:
		static bool Enabled();
		static CaptureParity& Get();

		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);
		/** @brief The same comparisons for a Lighting draw of a water reflection cube face, under "face" labels. */
		void OnFaceLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

		/** @brief Keeps a copy of the native constants as the engine unmaps them. */
		void OnMap(ID3D11Resource* a_resource, void* a_data);
		void OnUnmap(ID3D11Resource* a_resource);

		/** @brief Checks the draw that follows a checked SetupGeometry. */
		void OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);

		/** @brief Logs and resets the counters every a_interval frames. */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);
		/** @brief Takes the materials written since the last call. Render thread, EarlyPrepass. */
		void DrainWrittenMaterials();

	private:
		ankerl::unordered_dense::set<const RE::BSShaderMaterial*> writtenMaterials;
		struct ConstantSnapshot
		{
			ID3D11Resource* buffer = nullptr;
			void* mapped = nullptr;
			void* lastMapped = nullptr;  // still valid after Unmap, since DXVK keeps dynamic buffers persistently mapped
			std::uint32_t size = 0;
			std::array<std::uint8_t, 1024> bytes{};
			bool valid = false;
		};

		// Stage 0 is VS and 1 is PS. Levels are PerTechnique, PerMaterial and PerGeometry.
		ConstantSnapshot& Slot(std::uint32_t a_stage, std::uint32_t a_level) { return snapshots[a_stage][a_level]; }

		void NoteMismatch(std::string a_message);
		static void Snapshot(ConstantSnapshot& a_snapshot, ID3D11Resource* a_resource);
		bool CompareBlock(const RE::BSGeometry* a_geometry, const char* a_what, const ConstantBlock& a_expected, const StageLayout& a_layout,
			const void* a_nativeTable, std::size_t a_nativeTableSize, const ConstantSnapshot& a_native, ID3D11Resource* a_boundBuffer,
			std::uint32_t a_firstVariable, std::uint64_t a_variables, std::uint64_t a_tolerant);
		bool CompareMaterial(const RE::BSGeometry* a_geometry, std::uint32_t a_materialIndex);
		bool CompareGeometry(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags);
		bool CompareTechnique(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex);
		void ComparePermutation(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex);

		std::array<std::array<ConstantSnapshot, 3>, 2> snapshots;
		// Label prefix while the face comparisons run, and their counts.
		std::string labelPrefix;
		std::uint64_t faceDraws = 0, faceChecked = 0, faceMismatched = 0, faceUntracked = 0;

		void InstallDrawHook();
		bool drawHookInstalled = false;
		std::int32_t pendingObject = -1;  // its draw is next
		// Slots the pending object's draws should bind, in order. A multi-partition skin has one per partition DCLF draws.
		std::array<std::uint32_t, 8> pendingSlots{};
		std::uint32_t pendingSlotCount = 0;
		std::uint32_t pendingSlotCursor = 0;

		// Render state the native draw used per property-derived key, learned and then encoded.
		struct RenderStateKey
		{
			bool twoSided, zTest, zWrite, alphaTest;
			std::uint8_t alphaThreshold;
			bool operator==(const RenderStateKey&) const = default;
		};
		struct RenderStateKeyHash
		{
			std::size_t operator()(const RenderStateKey& k) const noexcept
			{
				return (std::size_t(k.twoSided) << 0) | (std::size_t(k.zTest) << 1) | (std::size_t(k.zWrite) << 2) | (std::size_t(k.alphaTest) << 3) |
				       (std::size_t(k.alphaThreshold) << 4);
			}
		};
		struct RenderStateValue
		{
			std::uint32_t cull, depth, stencil, blend, depthBias;
			bool alphaTestEnabled;
			float alphaTestRef;
			bool operator==(const RenderStateValue&) const = default;
		};
		ankerl::unordered_dense::map<RenderStateKey, std::vector<RenderStateValue>, RenderStateKeyHash> renderStates;
		// How often each block was compared, and how often there was no snapshot, which is not a passing check.
		ankerl::unordered_dense::map<std::string, std::pair<std::uint64_t, std::uint64_t>> blockCoverage;
		std::uint64_t drawMismatches = 0;  // draw arguments or bound buffers differ
		std::uint64_t drawsChecked = 0;
		std::uint32_t treeSamples = 0;  // tree amplitude diagnostics logged this session
		std::uint64_t boneChecks = 0;
		std::uint64_t boneMismatches = 0;
		std::map<ID3D11Resource*, void*> lastMappedAny;

		std::uint64_t nativeDraws = 0;         // native lighting draws in the main pass
		std::uint64_t checkedDraws = 0;        // ... of geometry in the DCLF tables
		std::uint64_t mismatchedDraws = 0;     // ... whose state differs
		// By technique, pass descriptor bits 24-29.
		std::array<std::uint64_t, 64> checkedByTechnique{}, mismatchedByTechnique{};
		std::array<std::string, 64> firstByTechnique;  // the first mismatch of each, whatever the sample limit
		std::string lastNote;                          // the last NoteMismatch
		std::uint64_t materialMismatches = 0;  // ... in PerMaterial constants or textures
		// Materials a draw mismatched on, followed into later write drains to see whether a write event arrived late.
		struct MaterialMismatch
		{
			std::uint32_t firstFrame = 0;
			std::uint32_t lastFrame = 0;
			std::uint32_t frames = 0;
			bool keyDiffers = false;  // the record's material is not the drawn pass's
			bool writtenBefore = false;  // this frame's drain, before the draw, held it
		};
		ankerl::unordered_dense::map<const RE::BSShaderMaterial*, MaterialMismatch> materialMismatchFollow;
		std::map<std::string, std::uint32_t> materialMismatchResolved;
		std::uint64_t geometryMismatches = 0;  // ... in PerGeometry constants
		// Draws whose shading changed between the frame-start sample and the native draw. An accepted difference.
		// Their PerGeometry is compared with the engine's current shading, which checks the sampling.
		std::uint64_t shadingSinceStart = 0;
		std::uint64_t techniqueMismatches = 0; // ... in PerTechnique constants or filter modes
		std::uint64_t inheritedFilters = 0;    // filters neither SetupTechnique nor SetupMaterial sets
		Integrations::LightLimitFix::DrawLightDataParity lightData;
		Integrations::AdvancedSkin::DrawSkinTextureParity skinTextures;
		Integrations::AdvancedSkin::DrawWetnessParity skinWetness;
		Integrations::TruePBR::DrawTextureParity truePBRTextures;
		std::uint64_t permutationChecks = 0;
		std::uint64_t permutationMismatches = 0;
		// Bindings Community Shaders owns must match for every eligible draw of a frame, so DCLF can bind them once per pass.
		// The baseline is the frame's first eligible draw.
		void CompareFeatureBindings(ID3D11DeviceContext* a_context);
		void CompareSkin(ID3D11DeviceContext* a_context, const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex);
		static constexpr std::uint32_t kFirstFeatureConstantBuffer = 3;
		static constexpr std::uint32_t kConstantBufferSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
		static constexpr std::uint32_t kResourceSlots = 128;
		struct FeatureBindings
		{
			std::array<ID3D11Buffer*, kConstantBufferSlots> vsBuffers{}, psBuffers{};
			std::array<ID3D11ShaderResourceView*, kResourceSlots> vsResources{}, psResources{};
		};
		FeatureBindings baseline;
		bool baselineValid = false;
		// Baseline constant buffers mapped since the last eligible draw, VS in bits 0-15 and PS in 16-31.
		// Counted as rewritten only when another eligible draw follows.
		std::uint32_t pendingRewrites = 0;
		// Keyed by "<what> <slot>".
		std::map<std::string, std::uint64_t> bindingChanges;

		// Statically eligible geometry drawn natively outside the tracked category nodes.
		std::uint64_t outsideCategories = 0;
		std::set<std::string> outsideChains;

		// Variables in a native group that DCLF leaves unwritten, as "<group> <variable>".
		std::map<std::string, std::uint64_t> unevaluated;
		// A standing difference fills the capped sample list, so per-variable counts show where a new class goes wrong.
		std::map<std::string, std::uint32_t> mismatchByVariable;
		// Keyed by field index << 32 | differing bits.
		ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> permutationDiffs;
		std::uint64_t untrackedEligible = 0;   // eligible geometry under a drawn category node, not tracked
		// Untracked eligible geometry followed until tracked. Identity only, never dereferenced after its draw.
		struct Untracked
		{
			std::uint32_t firstFrame = 0;
			std::uint32_t lastFrame = 0;
			std::uint32_t frames = 0;
			std::string chain;  // its parents at first sight, with the category node's discovery
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, Untracked> untracked;
		void ResolveUntracked(std::uint32_t a_frame);
		std::map<std::string, std::uint32_t> untrackedResolved;  // "<source> after <n> frames" -> geometries
		std::vector<std::string> untrackedStuck;                 // chains of those still untracked after kStuckFrames
		std::string compareContext;  // appended to a block's mismatch samples (CompareGeometry)
		std::uint64_t notInTables = 0;         // tracked geometry drawn natively but excluded this frame
		std::map<std::string, std::pair<std::uint64_t, std::string>> multiIndexDraws;  // by hint and technique
		std::map<std::string, std::uint64_t> outsideRoots;  // the root and the portal graph lists holding it
		std::uint64_t layerDraws = 0;  // multi-index layer draws, hint 12
		std::string firstLayerDraw;
		std::map<std::uint8_t, std::uint64_t> untrackedBy;  // by verdict
		std::map<std::uint8_t, std::string> untrackedFirst;
		std::map<std::uint8_t, std::uint64_t> notInTablesBy;  // by current verdict, with the first of each named
		std::map<std::uint8_t, std::string> notInTablesFirst;
		std::uint64_t nativeOnlyPasses = 0;    // hint 10 passes DCLF does not model
		std::uint64_t perDrawNormalised = 0;   // descriptors differ only in PerDrawBits, so only descriptors are compared
		ankerl::unordered_dense::set<std::uint32_t> renderFlagsSeen;
		std::vector<std::string> samples;  // first few mismatches per report
	};
}
