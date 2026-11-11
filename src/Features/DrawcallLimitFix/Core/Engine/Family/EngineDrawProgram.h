#pragma once

// The engine draw families' programs: CS's shader for an engine draw (Sky.hlsl, Water.hlsl, BloodSplatter.hlsl, Utility.hlsl built for
// ORG) with the defines ShaderCache gives the engine's descriptor, and the pipeline state the engine's state objects describe.
//
// A program exists before its first draw needs it (docs/development/p5/primary-path.md, rule 2):
// - Stages: when CS loads one of the family's shader descriptors (ShaderCompilationTask, through DrawcallLimitFix::PrecompileShader),
//   the family's ORG stages for that descriptor are compiled into the shader store (PrecompileStages).
// - Pipelines: a program key is a value (the descriptions of the engine's state objects, never their addresses). Every key a session
//   draws with is written to a manifest next to the shader store, and the next session builds the manifest's keys at setup on
//   ProgramCache's build thread (Prebuild). A draw that needs a key still building waits for it.
// - A key in neither is built on the render thread at its first draw, from the stages already in the store, and logged once as
//   "not prebuilt", so the gaps in the enumeration and the manifest show.
// There is no state where the engine draws while a program builds: a program is built, or it cannot be (an inherent refusal).

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct RasterProgram;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;

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

	/** @brief The words of a D3D11 state description in a ProgramKey (each field one word, in the description's order). */
	inline constexpr std::size_t kBlendWords = 2 + 8 * 8;
	inline constexpr std::size_t kDepthStencilWords = 14;
	inline constexpr std::size_t kRasterWords = 10;

	/**
	 * @brief What an engine draw's program is made of, as values: equal keys share one program, in this session and the next (the
	 * manifest). The states are the D3D11 descriptions of the engine's state objects (RenderGraph::Engine::Pipeline::FindBlend,
	 * FindDepthStencil, FindRaster), field by field.
	 */
	struct ProgramKey
	{
		std::uint32_t descriptor = 0;                  // the engine descriptor whose defines the program takes (CS's lookup: State::modified*)
		DefineStage defineStage = DefineStage::Pixel;  // the stage that descriptor is
		std::uint32_t variant = 0;                     // the family's own permutation (its defines: ProgramSource::variantDefines)
		std::array<std::uint32_t, kBlendWords> blend{};
		std::uint32_t hasDepth = 0;  // 0: no depth attachment (depthStencil is unused)
		std::array<std::uint32_t, kDepthStencilWords> depthStencil{};
		std::array<std::uint32_t, kRasterWords> raster{};
		std::array<std::uint32_t, 4> colourFormats{};  // rhi::Format values, 0 past the targets
		std::uint32_t depthFormat = 0;
		std::uint32_t maskedTargets = 0;  // colour targets whose writes are off (a bit each): bound, never written
		auto operator<=>(const ProgramKey&) const = default;
	};

	/**
	 * @brief Fills a_key's states from the engine's state objects (tripwire handles or D3D11 objects), through their recorded
	 * descriptions. a_depthStencil null: no depth attachment. False when an object has no description.
	 */
	bool DescribeStates(ProgramKey& a_key, ID3D11BlendState* a_blend, ID3D11DepthStencilState* a_depthStencil, ID3D11RasterizerState* a_raster);

	/** @brief One program's stages a loaded descriptor needs: its descriptor, which stage's defines, and the family's variant. */
	struct StageRequest
	{
		std::uint32_t descriptor = 0;
		DefineStage stage = DefineStage::Pixel;
		std::uint32_t variant = 0;
		auto operator<=>(const StageRequest&) const = default;
	};

	using Defines = std::vector<std::pair<std::wstring, std::wstring>>;

	/** @brief The family's shader: its source, what every program of it shares, and how its programs follow the engine's descriptors. */
	struct ProgramSource
	{
		std::string source;                      // under Data/Shaders, both stages
		std::wstring languageVersion = L"2021";  // DXC -HV
		std::uint32_t constantWords = 0;         // the push block, in 32-bit words
		std::uint32_t shaderType = 0;            // RE::BSShader::Type whose descriptors the family draws with
		/** @brief The family's defines for a variant (null: no variants). */
		Defines (*variantDefines)(std::uint32_t a_variant) = nullptr;
		/**
		 * @brief The programs a descriptor CS loads for the family's shader type can be drawn with (a_pixel: a pixel shader descriptor),
		 * appended to a_out. None: the family never draws with it.
		 */
		void (*stagesOf)(bool a_pixel, std::uint32_t a_descriptor, std::vector<StageRequest>& a_out) = nullptr;
	};

	/** @brief A family's programs by ProgramKey. Get on the render thread; Prebuild at setup (main thread). */
	class EngineDrawPrograms
	{
	public:
		/** @brief Registers the family for PrecompileStages. At install (main thread), before the engine loads its shaders. */
		EngineDrawPrograms(std::string a_family, ProgramSource a_source);
		~EngineDrawPrograms();

		/**
		 * @brief The key's program. A prebuilt key still building is waited for; a key never seen is built now, on the render thread, and
		 * logged once ("not prebuilt"). Null, logged once, when the key's states have no ORG form or the build failed: the draw is an
		 * inherent refusal. a_shader: the bound shader, whose descriptor defines the program.
		 */
		std::shared_ptr<const RasterProgram> Get(const ProgramKey& a_key, const RE::BSShader& a_shader);

		/**
		 * @brief The manifest's keys of this family queued on ProgramCache's build thread. Once, at setup on the main thread (the render
		 * owner, with the runtime active), after the engine loaded its shaders.
		 */
		void Prebuild();

		/** @brief Programs built so far. */
		std::size_t Built() const;

		struct Impl;

	private:
		std::unique_ptr<Impl> impl;
	};

	/**
	 * @brief A descriptor CS loads (ShaderCompilationTask, through DrawcallLimitFix::PrecompileShader): every family drawing with a_shader's
	 * type compiles its ORG stages for it into the shader store. Before the render graph has a compiler the requests are kept until
	 * StartStagePrecompile. CS's compile threads.
	 */
	void PrecompileStages(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);

	/** @brief The stage requests kept before the runtime had a compiler, compiled now on CS's compile pool. Setup (main thread), once. */
	void StartStagePrecompile();
}
