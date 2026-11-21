#pragma once

// The engine draw families' programs: CS's shader for an engine draw with the defines ShaderCache gives its descriptor, plus the
// pipeline state the engine's state objects describe.
//
// Every program builds on the program service (RenderGraph/ProgramService.h). Stages compile when CS loads one of the family's
// descriptors. Every program key a session draws with is written to a manifest, and the next session queues those keys in CS's
// setup batch, so they are built before the first frame. A key first seen in a draw is posted to the service and logged once as
// "not prebuilt"; its draws stay the engine's until it is built. The render thread never builds or waits for a program.

#include <array>
#include <compare>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace org::services
{
	struct RasterProgram;
}
using RasterProgram = org::services::RasterProgram;
struct ID3D11BlendState;
struct ID3D11DepthStencilState;
struct ID3D11RasterizerState;

namespace RE
{
	class BSShader;
}

namespace DCLF::Family
{
	/** @brief Which stage's descriptor the program's defines come from. */
	enum class DefineStage : std::uint8_t
	{
		Vertex,
		Pixel,
	};

	/** @brief The words of a D3D11 state description in a ProgramKey, one per field in order. */
	inline constexpr std::size_t kBlendWords = 2 + 8 * 8;
	inline constexpr std::size_t kDepthStencilWords = 14;
	inline constexpr std::size_t kRasterWords = 10;

	/**
	 * @brief An engine draw's program as values, never addresses, so equal keys share one program across sessions.
	 * The states are the D3D11 descriptions of the engine's state objects, field by field.
	 */
	struct ProgramKey
	{
		std::uint32_t descriptor = 0;  // the engine descriptor whose defines the program takes
		DefineStage defineStage = DefineStage::Pixel;
		std::uint32_t variant = 0;  // the family's own permutation
		std::array<std::uint32_t, kBlendWords> blend{};
		std::uint32_t hasDepth = 0;  // 0: no depth attachment
		std::array<std::uint32_t, kDepthStencilWords> depthStencil{};
		std::array<std::uint32_t, kRasterWords> raster{};
		std::array<std::uint32_t, 4> colourFormats{};  // rhi::Format values, 0 past the targets
		std::uint32_t depthFormat = 0;
		std::uint32_t maskedTargets = 0;  // bound but never written, a bit each
		auto operator<=>(const ProgramKey&) const = default;
	};

	/** @brief Fills a_key's states from the engine's state objects. Null a_depthStencil means no depth. False when an object has no description. */
	bool DescribeStates(ProgramKey& a_key, ID3D11BlendState* a_blend, ID3D11DepthStencilState* a_depthStencil, ID3D11RasterizerState* a_raster);

	/** @brief One program's stages a loaded descriptor needs. */
	struct StageRequest
	{
		std::uint32_t descriptor = 0;
		DefineStage stage = DefineStage::Pixel;
		std::uint32_t variant = 0;
		auto operator<=>(const StageRequest&) const = default;
	};

	using Defines = std::vector<std::pair<std::wstring, std::wstring>>;

	/** @brief The family's shader source and how its programs follow the engine's descriptors. */
	struct ProgramSource
	{
		std::string source;  // under Data/Shaders, both stages
		std::wstring languageVersion = L"2021";  // DXC -HV
		std::uint32_t constantWords = 0;  // the push block, in 32-bit words
		std::uint32_t shaderType = 0;  // RE::BSShader::Type whose descriptors the family draws with
		/** @brief The family's defines for a variant, or null for no variants. */
		Defines (*variantDefines)(std::uint32_t a_variant) = nullptr;
		/** @brief Appends the programs a loaded descriptor can be drawn with. None means the family never draws with it. */
		void (*stagesOf)(bool a_pixel, std::uint32_t a_descriptor, std::vector<StageRequest>& a_out) = nullptr;
	};

	/** @brief A family's programs by ProgramKey. Get on the render thread, Prebuild at CS's setup. */
	class EngineDrawPrograms
	{
	public:
		/** @brief Main thread, at install, before the engine loads its shaders. Registers the family for PrecompileStages. */
		EngineDrawPrograms(std::string a_family, ProgramSource a_source);
		~EngineDrawPrograms();

		/**
		 * @brief Render thread. The key's program once built. Null while it builds, and for a key whose states have no ORG form
		 * (logged once). Never builds and never waits: an unseen key is posted to the program service and listed in the manifest.
		 * A failed build is fatal. The defines follow from the family's shader type, so a_shader is not read.
		 */
		std::shared_ptr<const RasterProgram> Get(const ProgramKey& a_key, const RE::BSShader& a_shader);

		/**
		 * @brief Render thread, once at CS's setup, before the setup batch is joined. Queues the manifest's keys on the program
		 * service. The descriptors' defines need only the shader type, so the engine need not have loaded the family's shader.
		 */
		void Prebuild();

		/** @brief Programs built so far. */
		std::size_t Built() const;

		struct Impl;

	private:
		std::unique_ptr<Impl> impl;
	};

	/**
	 * @brief CS's compile threads. Compiles every family's ORG stages for a descriptor CS loads. Requests before the render graph has
	 * a compiler wait for StartStagePrecompile.
	 */
	void PrecompileStages(const RE::BSShader& a_shader, bool a_pixel, std::uint32_t a_descriptor);

	/** @brief Main thread, once at setup. Compiles the stage requests kept before the runtime had a compiler. */
	void StartStagePrecompile();
}
