#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "LightingDescriptors.h"

namespace DCLF
{
	/**
	 * @brief Captures each lighting pass as the engine registers it with a batch renderer.
	 *
	 * DCLF reads the passes the main camera will draw out of the accumulator's batch renderer
	 * (`SceneStore::CollectAccumulatedPasses`). That works only while the passes are in there, so it
	 * cannot survive static ownership, whose whole point is to keep them out. Capturing at registration
	 * gives the same information one step earlier, from the call that puts a pass into a group.
	 *
	 * `BSBatchRenderer::RegisterPass` (vfunc 0x02) is the right place rather than
	 * `BSLightingShaderProperty::GetRenderPasses`: the two fields DCLF depends on do not exist yet when
	 * `GetRenderPasses` returns. `technique` is the batch group's key, which the caller computes and
	 * passes in as `techniqueID` (it differs from the pass's own `passEnum` - the accumulator adds
	 * DoAlphaTest), and `subPass` is chosen inside registration.
	 *
	 * **Registration is threaded.** The engine's own classifier takes a mutex, and the scene lists are
	 * built on the `gJobList_SceneListAccumCulling` job list, so this is called concurrently. Entries go
	 * into a fixed-capacity buffer with an atomic cursor; the render thread drains it once per frame and
	 * is the only reader. Overflow is counted and makes the frame fall back to the accumulator walk.
	 */
	class PassCapture
	{
	public:
		struct Entry
		{
			const RE::BSGeometry* geometry = nullptr;
			const RE::BSRenderPass* pass = nullptr;
			const RE::BSBatchRenderer* batch = nullptr;  // which renderer it was registered with
			std::uint32_t technique = 0;                 // techniqueID, the batch group's key
			std::uint32_t subPass = 0;                   // derived; see SubPassOf
			std::uint32_t passEnum = 0;
		};

		struct Stats
		{
			std::uint32_t captured = 0;   // entries the last drain saw
			std::uint32_t overflowed = 0;
			std::uint32_t threads = 0;    // distinct threads seen registering
			// Step B gate: the captured set against what the accumulator walk finds.
			std::uint32_t compared = 0;
			std::uint32_t missing = 0;     // in the accumulator, not captured
			std::uint32_t extra = 0;       // captured, not in the accumulator
			std::uint32_t techniqueDiffers = 0;
			std::uint32_t subPassDiffers = 0;
			std::uint32_t withheld = 0;  // passes kept out of the main camera's batch renderer
			std::uint32_t claimed = 0;   // objects DCLF said it owns
			// Claimed but not drawn this frame. Withholding means nothing else will draw them either, so
			// any of these is a visible hole - the one failure mode static ownership introduces.
			std::uint32_t holes = 0;
			// Claim churn. A claim that goes away hands the object back to the native loop, so if culling
			// an object unclaims it the engine simply draws it again and the culling saves nothing.
			std::uint32_t claimsAdded = 0;
			std::uint32_t claimsDropped = 0;
			std::uint32_t droppedAfterCull = 0;  // dropped while the engine still had a pass for it
		};

		/** @brief The set of geometries DCLF owns; registration is withheld for these. */
		using ClaimSet = ankerl::unordered_dense::set<const RE::BSGeometry*>;

		static PassCapture& Get();

		void Install();
		bool Installed() const { return installed; }

		/**
		 * @brief Publishes the claim set for the frames that follow; render thread only.
		 *
		 * Immutable and swapped whole, never mutated in place: the hook reads it from whatever thread the
		 * engine registers on, and it is consulted before the frame's own BuildFrame has run. That is the
		 * right way round - a claim is meant to be a standing statement that DCLF owns an object, not a
		 * per-frame decision, which is exactly what the old `drawnFrame` skip was and why culling could
		 * not be trusted.
		 */
		void PublishClaims(std::shared_ptr<const ClaimSet> a_claims);

		/** @brief The claim set the registration hook is currently using, for the hole detector. */
		std::shared_ptr<const ClaimSet> CurrentClaims() const { return std::atomic_load(&claims); }

		/** @brief Which batch renderers belong to the main camera; withholding applies only to these. */
		void SetMainBatchRenderers(std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> a_renderers);

		/** @brief CS_DCLF_OWNERSHIP=static: withhold claimed passes from the main camera's batch renderer. */
		static bool WithholdingEnabled();

		/** @brief Takes everything registered since the last call; render thread only. */
		std::span<const Entry> Drain();

		const Stats& GetStats() const { return stats; }
		Stats& MutableStats() { return stats; }

		/**
		 * @brief The batch group list a pass lands in, from the property flags alone.
		 *
		 * The engine's classifier reads only the shader property's flags and the geometry's alpha
		 * property: flag bit 54 puts a pass in list 4, otherwise the list is bit 36 (as value 2) plus
		 * whether alpha testing is on. Nothing per-frame goes into it, which is what lets DCLF keep the
		 * field once the passes stop reaching a batch renderer at all.
		 */
		static std::uint32_t SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags);

	private:
		static constexpr std::size_t kCapacity = 32768;

		bool installed = false;
		std::vector<Entry> entries{ kCapacity };
		std::atomic<std::size_t> cursor{ 0 };
		std::atomic<std::uint32_t> overflow{ 0 };
		std::atomic<std::uint32_t> lastThread{ 0 };
		std::atomic<std::uint32_t> threadCount{ 0 };
		Stats stats;
		std::atomic<std::uint32_t> withheld{ 0 };
		// Published whole by the render thread, read by the registering thread. shared_ptr's atomic
		// load/store keeps the readers safe while the next one is being built.
		std::shared_ptr<const ClaimSet> claims;
		std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> mainRenderers;

		struct Hook;
		friend struct Hook;
		void Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique);
		bool Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass);

	public:
		/** @brief CS_DCLF_REGISTER_PROBE: registrations by shader type, and how many into a main renderer. */
		static constexpr std::size_t kShaderTypes = 16;
		std::array<std::atomic<std::uint32_t>, kShaderTypes> probeCounts{};
		std::array<std::atomic<std::uint32_t>, kShaderTypes> probeMain{};

	private:
	};
}
