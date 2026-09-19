#pragma once

#include <array>
#include <cstdint>
#include <memory>

namespace DCLF
{
	/**
	 * @brief Phase 2: DCLF's objects drawn by the render graph with one indirect command stream, into
	 * off-screen copies of the main pass's targets (the frame itself is unchanged).
	 *
	 * At the first lighting draw of the main (deferred) pass, CaptureMainPass records what the pass binds;
	 * before the deferred composite, Execute runs the graph's MainOpaque epoch. Inside it the draw data is assembled on the CPU and uploaded:
	 *   - constant blocks packed with the native shaders' constant tables: PerTechnique per pipeline,
	 *     PerMaterial per (material, pipeline), PerGeometry per object, Light Limit Fix's StrictLightData
	 *     per (room, shadow mask), the permutation per (pipeline, object flags), the alpha-test reference
	 *     per threshold, and every other bound constant buffer from its CPU mirror (ConstantMirror);
	 *   - one DrawBindings record per object (constant buffer addresses, texture and sampler heap indices);
	 *   - one DrawSequence per object.
	 * A graph pass then executes the sequences against the main depth buffer (imported from DXVK, read
	 * only, depth test EQUAL) into eight graph-owned targets with the main pass's formats. An object is
	 * drawn only when everything its pipeline's shaders read can be supplied.
	 *
	 * Render thread only.
	 */
	class IndirectDraws
	{
	public:
		enum class Skip : std::uint32_t
		{
			Pipeline,  // not in the pipeline set yet
			Geometry,  // no stable vertex / index buffer
			Texture,   // a texture the shaders read is not resolved
			Sampler,
			Constants,  // a constant buffer the shaders read is not available
			Capacity,
			Count
		};

		struct Stats
		{
			std::uint32_t epochs = 0;
			std::uint32_t drawn = 0;  // last epoch
			std::array<std::uint32_t, static_cast<std::size_t>(Skip::Count)> skipped{};
			std::array<std::uint32_t, 4> missingTextures{};  // registers of the last unresolved textures (diagnostics)
			std::uint32_t missingVertexConstants = 0;       // constant buffer registers without an address (bits)
			std::uint32_t missingPixelConstants = 0;
			std::uint64_t uploadBytes = 0;                   // last epoch
			double cpuMs = 0.0;                              // last epoch, assembly and epoch
			std::uint32_t notReady = 0;                      // epochs skipped (mirrors, targets or pipelines not ready)
			std::uint32_t buildParityChecks = 0;   // CS_DCLF_BUILD_PARITY
			std::uint32_t buildParityMismatches = 0;
		};

		static IndirectDraws& Get();

		bool Enabled() const;

		/** @brief At the first lighting draw of the main pass: what the pass binds (buffers, views, targets, viewport). */
		void CaptureMainPass();

		/**
		 * @brief Before the deferred composite: assemble this frame's draws and execute them. Constant buffer
		 * contents are read here, once the main pass has drawn with them (the engine updates its per-frame
		 * buffers while it applies a draw's state, after the first draw's SetupGeometry).
		 */
		void Execute();

		/** @brief Before the deferred composite: with CS_DCLF_DEBUG_VIEW=1, replace the main pass's targets with DCLF's. */
		void ShowDebugView();

		const Stats& GetStats() const { return stats; }

		~IndirectDraws();

	private:
		IndirectDraws();

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
		bool failed = false;
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(IndirectDraws::Skip::Count)> kSkipNames{ "pipeline", "geometry", "texture", "sampler",
		"constants", "capacity" };
}
