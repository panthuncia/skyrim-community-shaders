#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "RenderGraph/RenderGraphRuntime.h"

namespace RE
{
	class BSGeometry;
}

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
			NotSkippedNatively,  // Z-prepass only: the native loop still draws this object, so it owns its depth
			CandidateOnly,       // kept for the GPU culling to reject, but not drawable, so it has no bindings
			RecordCapacity,      // too many DISTINCT (material, pipeline) pairs, which is not the draw cap
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
			std::uint32_t records = 0;                       // binding records built, last epoch
			std::uint64_t uploadBytes = 0;                   // last epoch
			double cpuMs = 0.0;                              // last epoch, assembly and epoch
			// Where that time goes, so the per-draw work is optimised against a measurement rather than a
			// guess.
			//
			// Parts 0-2 are PER BINDING RECORD, not per draw: since Step D deduplicated the record to one
			// per (material, pipeline) pair they run ~100 times an epoch, not ~1500. The per-draw work -
			// the loop prologue with its resolvedBindings probe, and the tail that builds the sequence and
			// the draw input - was invisible, folded into "rest" with the uploads and the epoch execution.
			// This is the Stage 0 lesson applied to the epoch.
			std::array<double, 8> partMs{};
			std::uint32_t notReady = 0;                      // epochs skipped (mirrors, targets or pipelines not ready)
			std::uint32_t shortBuffers = 0;        // draws whose vertex or index slice does not cover them
			std::uint32_t cullDrawn = 0;     // sequences BuildDraws wrote, last sampled epoch
			std::uint32_t cullRejected = 0;  // draws its culling rejected (CS_DCLF_CULL)
			std::uint32_t cullTested = 0;    // draws it tested at all: 0 means the culling did not run
			// Against the engine's own culling, which the draw inputs carry per object (kObjectNativeVisible).
			std::uint32_t cullEngineCulled = 0;     // candidates the engine culled and CS_DCLF_CULL_INPUT=native dropped
			std::uint32_t cullFalseNegatives = 0;   // the engine kept it, the culling here rejected it: a defect
			std::uint32_t cullRescued = 0;          // the engine culled it, the culling here kept it
			std::uint32_t cullOccluded = 0;         // rejected by the HZB rather than by the frustum
			std::uint32_t cullOccludedVisible = 0;  // of those, ones the engine's own culling had kept
			// The second phase, which re-tests what the first rejected against the rebuilt HZB.
			std::uint32_t cullDrawnPhaseTwo = 0;      // depth draws it added
			std::uint32_t cullRescuedByPhaseTwo = 0;  // objects it brought back
			// What the HZB held under the tested objects: all-near or all-far means the build is wrong.
			std::uint32_t hzbNear = 0, hzbFar = 0, hzbSampled = 0;
			// One rejection in full, so an implausible count can be read instead of guessed at.
			struct HzbSample
			{
				bool valid = false;
				bool nativeVisible = false;
				float farthest = 0.0f, nearestZ = 0.0f;
				float uvMin[2]{}, uvMax[2]{};
				std::uint32_t mip = 0;
			} hzbSample;
			std::uint32_t buildParityChecks = 0;   // CS_DCLF_BUILD_PARITY
			std::uint32_t buildParityMismatches = 0;
			// CS_DCLF_BINDLESS_PARITY: the per-object record the DCLF_BINDLESS builds read, checked against
			// the constant group the non-bindless path packs for the same object. Counted per variable
			// component, because that is the granularity at which a layout mistake shows.
			std::uint32_t bindlessParityChecks = 0;
			std::uint32_t bindlessParityMismatches = 0;
			// CS_DCLF_DEDUP_PARITY: a binding record rebuilt per draw against the one its (material,
			// pipeline) pair holds. Zero mismatches is what says the deduplication is sound.
			std::uint32_t recordParityChecks = 0;
			std::uint32_t recordParityMismatches = 0;
		};

		static IndirectDraws& Get();

		bool Enabled() const;

		/** @brief CS_DCLF_HYBRID=1: DCLF draws into the main pass's targets and the native loop skips its objects. */
		static bool Hybrid();

		/**
		 * @brief Whether the epoch drew this geometry in the frame before, which is what the native loop
		 * skips: the epoch runs after the native passes, so its decision is one frame old. An object that
		 * has just become ineligible is missing for one frame; one that has just become eligible is drawn
		 * twice for one frame.
		 */
		bool DrewLastFrame(const RE::BSGeometry* a_geometry, std::uint32_t a_frame) const;

		/**
		 * @brief Publishes what DCLF owns, for the registration hook to withhold.
		 *
		 * The claim is what the colour epoch actually drew, not what DCLF would like to draw: withholding
		 * a pass means the native loop will not draw it either, so claiming something DCLF then fails to
		 * draw (a pipeline still compiling, a texture not resolved) leaves a hole. Drawing it once is the
		 * evidence that it can be drawn again.
		 */
		void PublishClaims();

		/** @brief At the first lighting draw of the main pass: what the pass binds (buffers, views, targets, viewport). */
		void CaptureMainPass();


		/**
		 * @brief Before the deferred composite: assemble this frame's draws and execute them. Constant buffer
		 * contents are read here, once the main pass has drawn with them (the engine updates its per-frame
		 * buffers while it applies a draw's state, after the first draw's SetupGeometry).
		 */
		void Execute();

		/**
		 * @brief Hybrid path, at the end of the native depth pass: assemble this frame's draws and write
		 * their depth into the depth buffer the pass just finished.
		 *
		 * It runs here, and not with the colour pass, because everything the rest of the frame does with
		 * depth - the native draws' own depth test, the sky, Terrain Blending's blended depth and every
		 * effect that reads it - is derived from the depth buffer at this point and has to see DCLF's
		 * objects. Only the vertex stage's bindings are needed, which the depth pass has bound; the pixel
		 * stage is the DCLF_DEPTH_ONLY build, which reads nothing that is not bound yet.
		 */
		void CaptureDepthPass();

		/** @brief Hybrid path, before the deferred composite: the colour pass, against the depth above. */
		void ExecuteColour();

		/**
		 * @brief CS_DCLF_GBUFFER_PROBE=<x>x<y>: read one texel of every main-pass target back and log it.
		 *
		 * Called either side of the colour epoch, it says what DCLF changed in the G-buffer the composite
		 * then consumes, in the same frame and at the same texel - which a comparison between two runs
		 * cannot, because the camera never lands in exactly the same place twice.
		 */
		void ProbeTargets(const char* a_label);

		/** @brief Before the deferred composite: with CS_DCLF_DEBUG_VIEW=1, replace the main pass's targets with DCLF's. */
		void ShowDebugView();

		const Stats& GetStats() const { return stats; }

		~IndirectDraws();

	private:
		IndirectDraws();

		/** @brief Assembles this frame's draws, uploads them and runs one epoch of the given segment. */
		void RunEpoch(RenderGraphRuntime::Segment a_segment);

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
		bool failed = false;
	};

	inline constexpr std::array<const char*, 8> kEpochPartNames{
		"textures/samplers", "constant groups", "record push", "per-draw tail", "per-draw prologue",
		"uploads", "epoch prologue", "graph execute"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(IndirectDraws::Skip::Count)> kSkipNames{ "pipeline", "geometry", "texture", "sampler",
		"constants", "capacity", "not-skipped-natively", "candidate-only", "record-capacity" };
}
