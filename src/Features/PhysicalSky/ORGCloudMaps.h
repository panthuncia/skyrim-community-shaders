#pragma once

#include "Buffer.h"
#include "RenderGraph/Port.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

struct PhysicalSky;

/**
* @brief Physical Sky's cloud map generators on the render graph (docs/development/plans/physical-sky-port.md, 6a).
*
* One client, cs.physical-sky.cloud-maps, at the Prepass anchor, ahead of the cloud renderer (cs.physical-sky.clouds).
* The managers keep their CPU change detection and describe each dispatch in D3D11 terms (views, constants, group
* counts). This class turns them into gated steps of the client and opens the steps at the commit. The client's key is the
* step set (the steps and the twins they use), observed as a variable, so a changed step registers the client again with
* the next revision. Work waits in a queue until a registration holds its steps; no manager asks twice.
* Generated maps are GraphResources twins, Persistent, exported to D3D11 so guest D3D11 code and the debug UI read the same
* memory. File textures are imports: an immutable ImportTexture in 2D, ImmutableTextureOf for arrays and volumes in native mode, and
* a twin filled once by CopyResource for arrays and volumes in guest mode.
* In the graph modes the managers never dispatch on D3D11, and any setup failure is fatal. Render thread throughout. Free of
* ORG/Vulkan includes.
*/
class ORGCloudMaps : public RenderGraph::Port<ORGCloudMaps>
{
public:
	static constexpr std::string_view kName = "Physical Sky cloud maps";

	/**
	* @brief A texture for a manager: a graph twin with D3D11 views in the graph modes, else a plain D3D11 texture.
	* The twin is Persistent, exported (SRV and UAV) and cleared once before its first use. Bind flags become SRV and UAV,
	* misc flags are dropped. In graph mode a failure is fatal.
	*/
	static eastl::unique_ptr<Texture2D> MakeTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool a_array = false);
	static eastl::unique_ptr<Texture3D> MakeTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name);

	/** @brief At SetupResources, after the managers made their textures. Queues the programs. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup(const PhysicalSky& a_feature);

	/** @brief The programs, one per shader entry. */
	enum class Program : std::uint32_t
	{
		NdfNoise,
		NdfGenerate,
		MidCloud,
		Cirrus,
		LocalNdf,
		StormNdf,
		Shape,
		ShapeMip,
		Adjustment,
		AdjustmentMip,
		CrossSection,
		Count
	};

	/** @brief The samplers the generators read. */
	enum class Sampler : std::uint32_t
	{
		WrapLinear,     // MIN_MAG_MIP_LINEAR, wrap: NDF, middle, cirrus, storm
		ClampLinear,    // MIN_MAG_MIP_LINEAR, clamp: local NDF
		Noise,          // the feature's sampNoise
		Transmittance,  // the feature's sampTr
		Count
	};

	/** @brief One entry of a step's push block, in the shader's resource list order. */
	struct Input
	{
		enum class Kind : std::uint8_t
		{
			Srv,       // a view of a twin, or a file texture; null reads zero
			Uav,       // a twin, one mip or every mip
			UavArray,  // a twin's whole-array view
			Sampler,
			Latch,  // the step's constants
			Word,   // a plain push word
		};
		Kind kind = Kind::Srv;
		ID3D11ShaderResourceView* srv = nullptr;
		ID3D11Resource* texture = nullptr;
		std::uint32_t mip = UINT32_MAX;  // Srv of a twin and Uav: one mip, or UINT32_MAX for the default view
		std::uint32_t value = 0;         // Sampler: the Sampler; Word: the word

		static Input Srv(ID3D11ShaderResourceView* a_srv) { return { Kind::Srv, a_srv }; }
		/** @brief One mip of a twin, for a shader that reads the previous mip of a chain. */
		static Input SrvMip(ID3D11Resource* a_texture, std::uint32_t a_mip) { return { Kind::Srv, nullptr, a_texture, a_mip }; }
		static Input Uav(ID3D11Resource* a_texture, std::uint32_t a_mip = UINT32_MAX) { return { Kind::Uav, nullptr, a_texture, a_mip }; }
		static Input UavArray(ID3D11Resource* a_texture) { return { Kind::UavArray, nullptr, a_texture }; }
		static Input SamplerOf(Sampler a_sampler) { return { Kind::Sampler, nullptr, nullptr, UINT32_MAX, static_cast<std::uint32_t>(a_sampler) }; }
		static Input Latch() { return { Kind::Latch }; }
		static Input Word(std::uint32_t a_value) { return { Kind::Word, nullptr, nullptr, UINT32_MAX, a_value }; }
	};

	/** @brief One dispatch. The step name is stable across frames; its inputs decide its registration. */
	struct Dispatch
	{
		std::string step;
		std::int32_t order = 0;  // the steps' order inside the client: producers before readers
		Program program = Program::Count;
		std::vector<Input> inputs;
		std::vector<std::byte> constants;  // the latch region's bytes, in cbuffer packing
		std::array<std::uint32_t, 3> groups{ 1, 1, 1 };

		/** @brief Copies a constant struct into constants. */
		template <class T>
		void Constants(const T& a_value)
		{
			const auto bytes = std::as_bytes(std::span(&a_value, 1));
			constants.assign(bytes.begin(), bytes.end());
		}
	};

	/** @brief Mips 1 to N-1 of a 2D twin from mip 0, through the graph's GenerateMips. */
	struct Mips
	{
		std::string step;
		std::int32_t order = 0;
		ID3D11Resource* texture = nullptr;
	};

	/** @brief One manager update's work. Its steps run together, in one commit. */
	struct Work
	{
		std::vector<Dispatch> dispatches;
		std::vector<Mips> mips;
		// The manager. Its updates run in order, one per commit.
		std::string owner;
		// Every update must run (the noise volume's slices). Otherwise an update with every step of the one queued before it
		// replaces that one, since every step rewrites its whole output.
		bool sequential = false;
	};

	/**
	* @brief Takes a manager update's work. It always takes it, so the manager moves on at once. The work runs at the commit
	* of the first run whose registration holds its steps: this frame's when they are known, otherwise the run of the revision
	* that registers the new step set. A texture that is not a twin, or a file texture the graph cannot import, is fatal.
	*/
	void Submit(Work a_work);

	/** @brief At the feature's Prepass, after the managers updated. Observes a new step set when a step or a twin changed. */
	void EndFrame();

	/** @brief The order bands of the managers' steps. */
	static constexpr std::int32_t kNoiseOrder = 0;
	static constexpr std::int32_t kNdfOrder = 1000;
	static constexpr std::int32_t kWeatherOrder = 2000;
	static constexpr std::int32_t kLocalOrder = 3000;
	static constexpr std::int32_t kStormOrder = 4000;
	static constexpr std::int32_t kCrossSectionOrder = 5000;
	~ORGCloudMaps();

	struct Impl;

private:
	friend class RenderGraph::Port<ORGCloudMaps>;
	ORGCloudMaps();

	std::shared_ptr<Impl> impl;
};
