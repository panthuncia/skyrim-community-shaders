#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "ConstantEvaluator.h"

namespace DCLF
{
	/**
	 * @brief Phase 1 gate (CS_DCLF_CAPTURE_PARITY=1): compares what the native main pass actually
	 * draws with what the CPU tables say Drawcall Limit Fix would draw.
	 *
	 * Called after the Lighting shader's SetupGeometry for every native lighting draw of the main
	 * (deferred) pass. For each draw of a geometry in the tables it checks the shader descriptors, the
	 * world transform, the PerMaterial and PerGeometry constants, and the material's textures and
	 * address modes. For each draw of a statically eligible geometry under a drawn category node that
	 * is not tracked, it counts a tracking miss.
	 */
	class CaptureParity
	{
	public:
		static bool Enabled();
		static CaptureParity& Get();

		void OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags);

		/** @brief Map/Unmap detours: keep a copy of the native constants as the engine unmaps them. */
		void OnMap(ID3D11Resource* a_resource, void* a_data);
		void OnUnmap(ID3D11Resource* a_resource);

		/** @brief DrawIndexed detour: checks the draw that follows a checked SetupGeometry. */
		void OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);

		/** @brief Logs and resets the counters every a_interval frames (called at the start of the main pass). */
		void Report(std::uint32_t a_frame, std::uint32_t a_interval);

	private:
		struct ConstantSnapshot
		{
			ID3D11Resource* buffer = nullptr;
			void* mapped = nullptr;
			std::uint32_t size = 0;
			std::array<std::uint8_t, 1024> bytes{};
			bool valid = false;
		};

		// Snapshot slots: [stage][level], stage 0 = VS, 1 = PS; levels PerMaterial (1) and PerGeometry (2).
		ConstantSnapshot& Slot(std::uint32_t a_stage, std::uint32_t a_level) { return snapshots[a_stage][a_level]; }

		void NoteMismatch(std::string a_message);
		static void Snapshot(ConstantSnapshot& a_snapshot, ID3D11Resource* a_resource);
		bool CompareBlock(const RE::BSGeometry* a_geometry, const char* a_what, const ConstantBlock& a_expected, const StageLayout& a_layout,
			const std::int8_t* a_nativeTable, std::size_t a_nativeTableSize, const ConstantSnapshot& a_native, ID3D11Resource* a_boundBuffer,
			std::uint32_t a_firstVariable, std::uint64_t a_skip);
		bool CompareMaterial(const RE::BSGeometry* a_geometry, std::uint32_t a_materialIndex);
		bool CompareGeometry(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags);

		std::array<std::array<ConstantSnapshot, 3>, 2> snapshots;

		void InstallDrawHook();
		bool drawHookInstalled = false;
		std::int32_t pendingObject = -1;  // object whose SetupGeometry just ran; its draw is next

		// Render state the native draw used, per property-derived key, to learn (and then encode) the mapping.
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
		std::uint64_t drawMismatches = 0;  // draw arguments or bound buffers differ
		std::uint64_t drawsChecked = 0;

		std::uint64_t nativeDraws = 0;         // native lighting draws in the main pass
		std::uint64_t checkedDraws = 0;        // ... of geometry in the DCLF tables
		std::uint64_t mismatchedDraws = 0;     // ... whose state differs
		std::uint64_t materialMismatches = 0;  // ... in PerMaterial constants or textures
		std::uint64_t geometryMismatches = 0;  // ... in PerGeometry constants
		std::uint64_t untrackedEligible = 0;   // eligible geometry under a drawn category node, not tracked
		std::uint64_t notInTables = 0;         // tracked geometry drawn natively but excluded this frame
		ankerl::unordered_dense::set<std::uint32_t> renderFlagsSeen;
		std::vector<std::string> samples;  // first few mismatch descriptions per report
	};
}
