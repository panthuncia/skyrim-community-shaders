#pragma once

#include <array>
#include <cstddef>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawFamilyLaneSlots.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	// The Effect family's slot must match Records.h's effect key bits.
	static_assert(ProgramFamilySlotOf(Api::ProgramFamily::Effect).keyBit == kRasterEffect &&
	                  ProgramFamilySlotOf(Api::ProgramFamily::Effect).spareShift == kRasterEffectBillboardShift,
		"the Effect family's slot is not Records.h's kRasterEffect and kRasterEffectBillboardShift");
	/** @brief Register classes shifted onto separate binding ranges in set 0 (DXC -fvk-{b,t,s,u}-shift), so the layout maps each on its own. */
	inline constexpr std::uint32_t kBindingShiftB = 0;
	inline constexpr std::uint32_t kBindingShiftT = 200;
	inline constexpr std::uint32_t kBindingShiftS = 400;
	inline constexpr std::uint32_t kBindingShiftU = 500;

	/** @brief One stage of a program and its depth-only and pulled builds. */
	struct ShaderStageSpec
	{
		bool pixel = false;
		std::uint32_t descriptor = 0;
		bool depth = false;
		bool pulled = false;
	};

	// DCLF_BINDLESS builds read the per-object values from the record table at t127 instead of the draw's constant buffers.
	// That makes the binding record identical for every draw of a (material, pipeline) pair, so it is deduplicated.

	/**
	 * @brief SPIR-V builds of the shader permutations DCLF draws.
	 *
	 * Each stage is compiled once, asynchronously, from the same source and defines as the D3D11 build plus DCLF_ORG. Results are
	 * content-addressed and cached on disk. Every vertex stage pulls its own attributes.
	 *
	 * Completions push to a lock-free queue and wake the pipeline lane, which owns every program entry. Nothing polls a compile.
	 * For hot reload, every stage, entry and program belongs to one sources generation. When it moves, the lane drops its entries.
	 */
	class ShaderPrograms
	{
	public:
		struct Program
		{
			// Pulls its attributes from the buffers in the draw's push data. Both variants draw with it.
			std::vector<std::byte> vertex;
			std::vector<std::byte> pixel;
			std::vector<std::byte> depthPixel;  // built with DCLF_DEPTH_ONLY
			// The Z-prepass plain draws' stages, which fetch their vertices and rows themselves.
			std::vector<std::byte> pulledVertex;
			std::vector<std::byte> pulledDepthPixel;
			// The plain colour draws' pixel stage.
			std::vector<std::byte> pulledPixel;
			// For a depth pipeline without a pixel stage. Empty for a program family's.
			std::vector<std::byte> pulledDepthVertex;
			// The vertex attributes the stages read. A key whose vertex layout lacks one fails, as the input assembler's layout did.
			std::uint32_t vertexAttributes = 0;
			// The sources generation is the oldest the stages were compiled for.
			std::uint64_t fingerprint = 0, sourcesGeneration = 0;
		};

		struct Stats
		{
			std::uint32_t requested = 0;
			std::uint32_t ready = 0;
			std::uint32_t failed = 0;
			std::uint32_t fromCache = 0;
			std::uint32_t shadowRequested = 0;
			std::uint32_t shadowReady = 0;
			std::uint32_t shadowFailed = 0;
		};

		static ShaderPrograms& Get();

		/** @brief Whether SPIR-V compilation is available. */
		bool Enabled() const;

		/** @brief Stages a runtime request needed that no precompile had asked for, so they compiled on demand. */
		static constexpr std::uint8_t kOnDemandVertex = 1, kOnDemandPixel = 2, kOnDemandDepthPixel = 4, kOnDemandPulledVertex = 8,
									  kOnDemandPulledDepthPixel = 16, kOnDemandPulledPixel = 32, kOnDemandPulledDepthVertex = 64;
		static std::string OnDemandStages(std::uint8_t a_stages);

		/**
		 * @brief The pipeline lane. The key's program once its stages compiled, requesting them on the first call. Null otherwise.
		 * a_failed is set when the program can never be made.
		 */
		std::shared_ptr<const Program> Find(const PipelineKey& a_key, RE::BSShader& a_lighting, std::uint8_t* a_onDemand = nullptr, bool* a_failed = nullptr);
		/** @brief Which Lighting program a key draws with. */
		static std::uint64_t LightingProgramId(const PipelineKey& a_key)
		{
			return (static_cast<std::uint64_t>(a_key.vertexDescriptor) << 32) | a_key.pixelDescriptor;
		}

		/**
		 * @brief SPIR-V builds of one Utility technique, for shadow views. Per-object values come from the record table, because one
		 * pipeline draws every object of a view.
		 */
		struct ShadowProgram
		{
			std::vector<std::byte> vertex;
			std::vector<std::byte> pixel;
			std::uint64_t fingerprint = 0, sourcesGeneration = 0;
		};
		/**
		 * @brief The pipeline lane. The Utility technique's programs, or null while they build. A pipeline build holds what it uses.
		 * a_allowRequest false returns null for an unseen technique without starting builds.
		 */
		std::shared_ptr<const ShadowProgram> FindShadow(std::uint32_t a_technique, RE::BSShader& a_utility, bool a_allowRequest = true, bool* a_requested = nullptr,
			std::uint8_t* a_onDemand = nullptr, bool* a_failed = nullptr);

		/** @brief A draw family's programs: colour and depth, each a pulled vertex and pixel stage, plus an optional second colour program. */
		struct DrawProgram
		{
			std::vector<std::byte> vertex, pixel;  // colour
			std::vector<std::byte> depthVertex, depthPixel;  // depth
			std::vector<std::byte> vertex2, pixel2;  // empty with one colour program
			std::uint64_t fingerprint = 0, sourcesGeneration = 0;
		};
		std::shared_ptr<const DrawProgram> FindDrawFamily(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed = nullptr);

		/** @brief A forward view's program. One forward pass draws colour and depth together with plain draws, so both stages are pulled builds. */
		struct ForwardProgram
		{
			std::vector<std::byte> vertex, pixel;
			std::uint64_t fingerprint = 0, sourcesGeneration = 0;
		};
		/** @brief The Lighting forward program of these descriptors, requesting it. Null until both stages are compiled. */
		std::shared_ptr<const ForwardProgram> FindForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, RE::BSShader& a_lighting, bool* a_failed = nullptr);
		/** @brief Which forward program a pair of descriptors draws with. */
		static std::uint64_t ForwardProgramId(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor)
		{
			return (static_cast<std::uint64_t>(a_vertexDescriptor) << 32) | a_pixelDescriptor;
		}
		/**
		 * @brief A program family key's program, vertex and pixel stages only. A forward object is no depth segment input, so it has no
		 * Z-prepass pipeline. Null until both are compiled.
		 */
		std::shared_ptr<const Program> FindFamily(Api::ProgramFamily a_family, const PipelineKey& a_key, const RE::BSShader& a_shader, bool* a_failed = nullptr);
		/** @brief Which family program a key draws with. Distinct from every LightingProgramId. */
		static std::uint64_t FamilyProgramId(Api::ProgramFamily a_family, const PipelineKey& a_key) { return LightingProgramId(a_key) | ProgramFamilySlotOf(a_family).programIdBit; }
		/** @brief A draw family's forward program. */
		std::shared_ptr<const ForwardProgram> FindDrawFamilyForward(Api::DrawFamily a_family, RE::BSShader& a_shader, bool* a_failed = nullptr);

		/** @brief The programs one UpdateLane made or failed, so the lane retries the keys that waited on them. */
		struct Finished
		{
			std::vector<std::uint64_t> lighting;
			std::vector<std::uint32_t> shadow;
			std::vector<std::uint64_t> forward;
			std::array<std::vector<std::uint64_t>, Extensions::kProgramFamilies> families;
			std::array<bool, kDrawFamilyLanes> drawFamily{}, drawFamilyForward{};

			bool Empty() const
			{
				for (const auto& ids : families)
					if (!ids.empty())
						return false;
				for (std::size_t f = 0; f < kDrawFamilyLanes; ++f)
					if (drawFamily[f] || drawFamilyForward[f])
						return false;
				return lighting.empty() && shadow.empty() && forward.empty();
			}
			void Clear()
			{
				lighting.clear();
				shadow.clear();
				forward.clear();
				for (auto& ids : families)
					ids.clear();
				drawFamily.fill(false);
				drawFamilyForward.fill(false);
			}
		};
		/** @brief The pipeline lane. Admits compilations completed since the last call. */
		void UpdateLane(Finished& a_finished);

		/**
		 * @brief The pipeline lane, before UpdateLane. True when a shader reload moved the sources generation. Every entry is then dropped,
		 * so the next Find* rebuilds from the current files.
		 */
		bool FollowSources();

		/** @brief Any thread. The sources generation the next stage is compiled for. */
		static std::uint64_t CurrentSourcesGeneration();
		/** @brief The shader file a shader's programs are compiled from. */
		static std::string SourcePathOf(const RE::BSShader& a_shader);

		/**
		 * @brief Any thread. A program compiled from the current files with the same requests as the lane's Find*, so caches serve both.
		 * Never blocks. a_done runs once with the program, or null and why.
		 */
		template <class P>
		using Rebuilt = std::function<void(std::shared_ptr<const P>, std::string)>;
		void RebuildLighting(const PipelineKey& a_key, const RE::BSShader& a_lighting, Rebuilt<Program> a_done);
		void RebuildFamily(Api::ProgramFamily a_family, const PipelineKey& a_key, const RE::BSShader& a_shader, Rebuilt<Program> a_done);
		void RebuildShadow(std::uint32_t a_technique, const RE::BSShader& a_utility, Rebuilt<ShadowProgram> a_done);
		void RebuildDrawFamily(Api::DrawFamily a_family, const RE::BSShader& a_shader, Rebuilt<DrawProgram> a_done);
		void RebuildForward(std::uint32_t a_vertexDescriptor, std::uint32_t a_pixelDescriptor, const RE::BSShader& a_lighting, Rebuilt<ForwardProgram> a_done);
		void RebuildDrawFamilyForward(Api::DrawFamily a_family, const RE::BSShader& a_shader, Rebuilt<ForwardProgram> a_done);

		// CS compilation workers request the same stages the runtime uses. Requests before ORG initialization wait for StartPrecompile.
		void Precompile(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);
		void StartPrecompile();

		/** @brief Any thread. The pipeline lane's counters, read relaxed. */
		Stats GetStats() const
		{
			Stats result;
			result.requested = laneStats.requested.load(std::memory_order_relaxed);
			result.ready = laneStats.ready.load(std::memory_order_relaxed);
			result.failed = laneStats.failed.load(std::memory_order_relaxed);
			result.fromCache = laneStats.fromCache.load(std::memory_order_relaxed);
			result.shadowRequested = laneStats.shadowRequested.load(std::memory_order_relaxed);
			result.shadowReady = laneStats.shadowReady.load(std::memory_order_relaxed);
			result.shadowFailed = laneStats.shadowFailed.load(std::memory_order_relaxed);
			return result;
		}

	private:
		ShaderPrograms();
		~ShaderPrograms();

		template <class P, std::size_t N>
		struct ProgramEntry;
		// vertex, pixel, depth pixel, pulled vertex, pulled depth pixel, pulled pixel, pulled depth vertex
		using Entry = ProgramEntry<Program, 7>;
		using ShadowEntry = ProgramEntry<ShadowProgram, 2>;
		// Vertex, pixel, depth vertex, depth pixel, then the second colour pair. With one colour program that pair repeats the first.
		using DrawFamilyEntry = ProgramEntry<DrawProgram, 6>;
		using ForwardEntry = ProgramEntry<ForwardProgram, 2>;
		// Vertex and pixel, then the pulled pair. Program's depth slots repeat the pixel stages and are unread.
		using FamilyEntry = ProgramEntry<Program, 6>;
		struct StageCache;
		std::unique_ptr<StageCache> stages;
		/** @brief The shader files as read for one sources generation. Immutable. */
		struct Sources;
		std::shared_ptr<const Sources> sources;  // guarded by StageCache::requestMutex
		/** @brief Call with StageCache::requestMutex held. Rereads the sources if the generation moved. */
		std::shared_ptr<const Sources> SourcesLocked();
		/** @brief Any thread. SourcesLocked under the lock. */
		std::shared_ptr<const Sources> CurrentSources();
		/** @brief Any thread. Compiles a_stages from the current sources into a program. */
		template <class P, std::size_t N>
		void CompileProgram(const RE::BSShader& a_shader, const std::array<ShaderStageSpec, N>& a_stages, Rebuilt<P> a_done);
		/** @brief The pipeline lane. Makes a program's entry on its first request. */
		template <class E, class MakeStages>
		E& MakeEntry(std::unique_ptr<E>& a_slot, std::uint8_t a_user, std::uint64_t a_id, std::atomic<std::uint32_t>* a_requested, std::atomic<std::uint32_t>& a_failed,
			MakeStages&& a_makeStages);
		/** @brief The pipeline lane. Gives an entry whose stages all completed its program, or fails it at its first failed stage. */
		template <class E, class LogFailure>
		void FinishEntry(E& a_entry, std::atomic<std::uint32_t>* a_ready, std::atomic<std::uint32_t>& a_failed, LogFailure&& a_logFailure);
		void FinishDrawFamily(std::uint8_t a_user, Finished& a_finished);

		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<Entry>> entries;
		ankerl::unordered_dense::map<std::uint32_t, std::unique_ptr<ShadowEntry>> shadowEntries;
		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<ForwardEntry>> forwardEntries;
		std::array<std::unique_ptr<DrawFamilyEntry>, kDrawFamilyLanes> drawFamilyEntries;
		std::array<std::unique_ptr<ForwardEntry>, kDrawFamilyLanes> drawFamilyForwardEntries;
		std::array<ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<FamilyEntry>>, Extensions::kProgramFamilies> familyEntries;
		std::atomic<bool> sourcesMissing{ false };  // Lighting.hlsl could not be read
		std::atomic<std::uint32_t> loggedFailures{ 0 };
		std::uint64_t entriesGeneration = 0;
		// Utility programs count apart. Read by GetStats on any thread.
		struct LaneStats
		{
			std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 }, fromCache{ 0 };
			std::atomic<std::uint32_t> shadowRequested{ 0 }, shadowReady{ 0 }, shadowFailed{ 0 };
		};
		LaneStats laneStats;
	};
}
