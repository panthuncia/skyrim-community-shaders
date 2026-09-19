#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Records.h"

namespace DCLF
{
	/**
	 * @brief Register classes of the SPIR-V builds, shifted onto separate binding ranges (DXC
	 * -fvk-{b,t,s,u}-shift, all in set 0) so that the pipeline layout can map each class on its own.
	 */
	inline constexpr std::uint32_t kBindingShiftB = 0;
	inline constexpr std::uint32_t kBindingShiftT = 200;
	inline constexpr std::uint32_t kBindingShiftS = 400;
	inline constexpr std::uint32_t kBindingShiftU = 500;

	/**
	 * @brief SPIR-V builds of the Lighting shader permutations DCLF draws (Phase 2).
	 *
	 * Each pipeline key's vertex and pixel descriptors are compiled once, asynchronously, by the render
	 * graph runtime's ORGModuleServices compiler, from the same source, includes and defines the D3D11
	 * build uses (ShaderCache::GetCompileDefines), plus DCLF_ORG. Artifacts are content-addressed on the
	 * source, every shader file under Data/Shaders and the defines, and cached on disk.
	 *
	 * Render thread only.
	 */
	class ShaderPrograms
	{
	public:
		struct Program
		{
			std::vector<std::byte> vertex;  // SPIR-V
			std::vector<std::byte> pixel;
		};

		struct Stats
		{
			std::uint32_t requested = 0;
			std::uint32_t ready = 0;
			std::uint32_t failed = 0;
			std::uint32_t fromCache = 0;  // artifacts found in the memory or disk cache
		};

		static ShaderPrograms& Get();

		/** @brief Whether SPIR-V compilation is available (render graph active, DXC loaded). */
		bool Enabled() const;

		/** @brief The key's program once both stages compiled; requests them on first call. Null otherwise. */
		const Program* Find(const PipelineKey& a_key, RE::BSShader& a_lighting);

		/** @brief Programs are never freed: the reference stays valid for the process (pipeline builds keep it). */

		/** @brief Collects finished compilations (call once per frame). */
		void Update();

		const Stats& GetStats() const { return stats; }

	private:
		ShaderPrograms();
		~ShaderPrograms();

		struct Entry;  // compilation futures; defined with the compiler (ShaderPrograms.cpp)
		bool LoadSources();

		ankerl::unordered_dense::map<std::uint64_t, std::unique_ptr<Entry>> entries;
		std::vector<std::byte> source;
		std::vector<std::filesystem::path> dependencies;
		bool sourcesLoaded = false;
		bool sourcesMissing = false;
		std::uint32_t loggedFailures = 0;
		Stats stats;
	};
}
