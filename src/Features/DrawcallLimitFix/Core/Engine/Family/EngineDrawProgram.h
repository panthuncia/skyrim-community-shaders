#pragma once

// The engine draw families' programs: CS's shader for an engine draw (Sky.hlsl, Water.hlsl, BloodSplatter.hlsl built for ORG) with
// the defines ShaderCache gives the engine's descriptor, and the pipeline state the engine's state objects describe
// (RenderGraph::Engine::Pipeline::FindBlend, FindDepthStencil, FindRaster). Built off the render thread by ProgramCache: a draw whose
// program is not built yet stays the engine's.

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

struct RasterProgram;

namespace RE
{
	class BSShader;
}

namespace DCLF::Family
{
	/** @brief Which stage's descriptor the program's defines come from (ShaderCache::GetCompileDefines' class). */
	enum class DefineStage : std::uint8_t
	{
		Vertex,
		Pixel,
	};

	/** @brief What an engine draw's program is made of. Equal keys share one program. */
	struct ProgramKey
	{
		std::uint32_t descriptor = 0;                  // the engine descriptor whose defines the program takes
		DefineStage defineStage = DefineStage::Pixel;  // the stage that descriptor is
		std::uint32_t variant = 0;        // the family's own permutation (its defines: EngineDrawPrograms::Get's a_variantDefines)
		const void* blend = nullptr;      // the ID3D11BlendState the draw binds
		const void* depthStencil = nullptr;  // its ID3D11DepthStencilState; null: no depth attachment
		const void* raster = nullptr;     // its ID3D11RasterizerState
		std::array<std::uint32_t, 4> colourFormats{};  // rhi::Format values, 0 past the targets
		std::uint32_t depthFormat = 0;
		std::uint32_t maskedTargets = 0;  // colour targets whose writes are off (a bit each): bound, never written
		auto operator<=>(const ProgramKey&) const = default;
	};

	/** @brief The family's shader: its source and what every program of it shares. */
	struct ProgramSource
	{
		std::string source;                       // under Data/Shaders, both stages
		std::wstring languageVersion = L"2021";   // DXC -HV
		std::uint32_t constantWords = 0;          // the push block, in 32-bit words
	};

	enum class ProgramState : std::uint8_t
	{
		Ready,
		Building,  // queued or compiling: the engine draws meanwhile
		Failed,    // its states have no ORG form, or the build failed (logged once)
	};

	/** @brief A family's programs, keyed by ProgramKey, built asynchronously. Render thread. */
	class EngineDrawPrograms
	{
	public:
		EngineDrawPrograms(std::string a_family, ProgramSource a_source);
		~EngineDrawPrograms();

		/**
		 * @brief The key's program once built; null while it builds or when it cannot be (a_state says which). The first call for a key
		 * reads its state descriptions and ShaderCache's defines for a_shader and queues the build. a_variantDefines: the family's
		 * defines for a_key.variant.
		 */
		std::shared_ptr<const RasterProgram> Get(const ProgramKey& a_key, const RE::BSShader& a_shader,
			std::span<const std::pair<std::wstring, std::wstring>> a_variantDefines, ProgramState& a_state);

		/** @brief Programs built so far. */
		std::size_t Built() const { return built; }

	private:
		struct Impl;
		std::unique_ptr<Impl> impl;
		std::size_t built = 0;
	};
}
