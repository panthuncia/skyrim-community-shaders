#pragma once

// Core's side of Api/FrameBindings.h: frame bindings, frame textures, frame graph buffers and per-draw buffers.
// Also the texture registers Core keeps for itself, and the one rule that picks a published graph texture over an imported view.

#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

struct ID3D11ShaderResourceView;

namespace org
{
	struct FramePreparationContext;
	class RenderPassBuilder;
	class VersionedBuffer;
	namespace services
	{
		class NamedVersions;
	}
}

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Api
{
	/** @brief What a binding's select gets. It answers with buffer, or with index when called on the render thread. */
	class FrameBindingSelect
	{
	public:
		const Draws::SceneBuffers& scene;
		std::uint32_t frame;
		bool renderThread = false;                                      // only then may index answer
		const std::shared_ptr<org::VersionedBuffer>* buffer = nullptr;  // null means no answer
		std::uint32_t index = ~0u;  // a descriptor index instead, render thread only
	};
}

namespace DCLF::Extensions
{
	/** @brief Most frame bindings, because a build embeds their values. Grow this before registering more. */
	inline constexpr std::size_t kFrameBindingSlots = 5;
	/** @brief A binding value meaning select gave nothing, so the register keeps what the record set. */
	inline constexpr std::uint32_t kNoFrameBinding = ~0u;
	/** @brief The bindings' values for one frame, in registration order. */
	using FrameBindingValues = std::array<std::uint32_t, kFrameBindingSlots>;

	/**
	 * @brief Whether Core keeps a_register for the material rows' pixel textures or the frame record's own resources.
	 * No binding, frame texture or material contributor may take one. Features/Manifest.cpp checks this at compile time too.
	 */
	inline constexpr bool CoreFrameRegister(std::uint32_t a_register)
	{
		return a_register < kPixelTextureSlots || a_register == kObjectBufferRegister || a_register == kExtrasBufferRegister ||
		       a_register == kCharacterLightRegister || a_register == kPlacementBufferRegister || a_register == kPaletteBufferRegister ||
		       a_register == kShadingBufferRegister;
	}

	/** @brief Fills a_out with every binding's descriptor for a_frame. Index answers are only taken on the render thread. */
	void FrameBindingValuesOf(const org::services::NamedVersions& a_sealed, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame, FrameBindingValues& a_out,
		bool a_renderThread);
	/** @brief Writes a_values into a frame record's texture registers. */
	void WriteFrameBindings(const FrameBindingValues& a_values, std::span<std::uint32_t> a_textures);
	/** @brief Whether a_register belongs to a binding. A build counts those as buffers, not captured frame textures. */
	bool FrameBindingRegister(std::uint32_t a_register);
	/** @brief Binding a_binding's buffer for a_frame, or null. Used by the named-address parity. */
	const std::shared_ptr<org::VersionedBuffer>* FrameBindingBuffer(std::size_t a_binding, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame);

	/** @brief Most frame textures: one per frame register from t16 to t127, since no two share a register. */
	inline constexpr std::size_t kFrameTextureSlots = kTextureRegisters - kPixelTextureSlots;
	/** @brief A register with no frame texture. */
	inline constexpr std::uint8_t kNoFrameTexture = 0xFF;
	static_assert(kFrameTextureSlots < kNoFrameTexture, "a frame texture's index (FrameTextureIndices) would reach kNoFrameTexture");
	/** @brief Any thread, after InstallFeatures. Each texture register's frame texture index, or kNoFrameTexture. */
	std::span<const std::uint8_t> FrameTextureIndices();

	/** @brief Who reads a graph texture. Each reader logs its own path changes. */
	enum class GraphTextureReader : std::uint8_t
	{
		Colour,  // a colour commit's frame textures
		Faces,   // the reflection faces' frame textures
		Grass,   // grass's collision texture
		Count,
	};

	/** @brief Any thread, after InstallFeatures. Every frame texture's graph ids, once each, for the colour passes to declare. */
	std::span<const std::string_view> ColourGraphTextureIds();
	/** @brief Any thread, after InstallFeatures. Graph ids of frame textures that have a view. The faces read no others. */
	std::span<const std::string_view> FacesGraphTextureIds();

	/**
	 * @brief Published-id reads a pass declared, with the bindless index each had at graph build. The commit latches that index.
	 * CheckDeclaredReads verifies it still matches the declared view's index in Prepare. Cheap to copy.
	 */
	struct DeclaredReads
	{
		struct Entry;
		std::shared_ptr<std::vector<Entry>> entries;
	};

	/**
	 * @brief Graph build. Declares a read of a_id with the same view whose index the commit latches, and adds it to a_out.
	 * Check PublishedResources::IsDeclarable first.
	 */
	void DeclarePublishedRead(org::RenderPassBuilder& a_builder, std::string_view a_id, DeclaredReads& a_out);

	/** @brief Logs "[ORG] declared view mismatch" once per declared read whose build-time index no longer matches its view. */
	void CheckDeclaredReads(const DeclaredReads& a_reads, const org::FramePreparationContext& a_preparation, std::string_view a_site);

	/**
	 * @brief Graph build. Declares a read of every declarable id in a_ids.
	 * Ids stay registered once published, and the set only grows on a first Publish, which rebuilds every graph anyway.
	 */
	DeclaredReads DeclareGraphTextures(org::RenderPassBuilder& a_builder, std::span<const std::string_view> a_ids);

	/** @brief Same as above, adding to a_out. */
	void DeclareGraphTextures(org::RenderPassBuilder& a_builder, std::span<const std::string_view> a_ids, DeclaredReads& a_out);

	/**
	 * @brief Render thread. The one rule for reading a published texture.
	 * If a_view is the export of a declared id, writes its bindless index and returns true. Otherwise the caller imports a_view.
	 */
	bool GraphTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, const ID3D11ShaderResourceView* a_view,
		std::span<const std::string_view> a_ids, std::uint32_t& a_index);

	/** @brief Published ids the character light's noise can read: the HDR downsample chain, whose last level the luminance chain alternates. */
	std::span<const std::string_view> CharacterLightGraphTextureIds();

	/**
	 * @brief The character light's read of the adapted luminance. CS's post chain draws that level into its own pair of targets.
	 * Returns the index of CS's target with the same parity if declared. On false, try EngineTargetTextureIndex.
	 */
	bool AdaptedLuminanceTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view, std::uint32_t& a_index);

	/** @brief The published id of the engine's shadow mask, which grass reads from its draw row. */
	std::span<const std::string_view> ShadowMaskGraphTextureIds();

	/**
	 * @brief GraphTextureIndex for a view of an engine render target. The view may be a tripwire handle, so it is matched as the wrapped D3D11 view.
	 * On a miss with a new view, publishes the target if ORG owns it, so the next build declares it.
	 */
	bool EngineTargetTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, ID3D11ShaderResourceView* a_view,
		std::span<const std::string_view> a_ids, std::uint32_t& a_index);

	/** @brief The register where Wetness Effects binds the engine's precipitation occlusion map for every colour draw. */
	inline constexpr std::uint32_t kPrecipitationOcclusionRegister = 70;

	/** @brief The published id of the engine's precipitation occlusion map, a depth target. */
	std::span<const std::string_view> PrecipitationOcclusionGraphTextureIds();

	/**
	 * @brief GraphTextureIndex for the precipitation occlusion map. On a miss with a new view, imports the depth target and publishes it under
	 * the same id so the next colour build declares it. Any other view there is left to the caller's import.
	 */
	bool PrecipitationOcclusionTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view, std::uint32_t& a_index);

	/** @brief Most frame graph buffers registrations, because a commit keeps their indices in a fixed array. Grow this first. */
	inline constexpr std::size_t kFrameGraphBufferSlots = 2;
	inline constexpr std::size_t kFrameGraphBufferRegisters = std::tuple_size_v<decltype(Api::FrameGraphBuffers::resources)>;
	/** @brief Each registration's indices for one commit, by registration then register. */
	using FrameGraphBufferValues = std::array<std::array<std::uint32_t, kFrameGraphBufferRegisters>, kFrameGraphBufferSlots>;
	/** @brief A registered frame graph buffers entry covering registers [firstRegister, firstRegister + count). */
	struct FrameGraphBuffersEntry
	{
		std::string name;
		std::uint32_t firstRegister = ~0u;
		std::uint32_t count = 0;                                            // leading non-empty resources
		std::array<std::string, kFrameGraphBufferRegisters> resources;      // owned so c_str can feed ORG's ResourceIdentifier
		decltype(Api::FrameGraphBuffers::indices) indices = nullptr;
	};
	/** @brief Any thread, after InstallFeatures. */
	std::span<const FrameGraphBuffersEntry> RegisteredFrameGraphBuffers();
	/** @brief Whether a_register belongs to a registered frame graph buffer. */
	bool FrameGraphBufferRegister(std::uint32_t a_register);
	/** @brief Render thread. Writes registration a_buffers's indices and returns whether they are on the graph. */
	bool FrameGraphBufferIndices(std::size_t a_buffers, std::span<std::uint32_t, kFrameGraphBufferRegisters> a_out);

	/** @brief Constant buffers Core treats as bound per draw: b0-b2, b4, VS b9 and b10, and PS b11. */
	inline constexpr std::uint32_t kCorePerDrawVS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 9) | (1u << 10);
	inline constexpr std::uint32_t kCorePerDrawPS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 11);
	inline constexpr std::uint32_t CorePerDrawMask(Api::ShaderStage a_stage) { return a_stage == Api::ShaderStage::Vertex ? kCorePerDrawVS : kCorePerDrawPS; }
	/**
	 * @brief Any thread, after InstallFeatures. a_stage's per-draw constant buffer registers, Core's and declared ones.
	 * Frame block mirroring and capture checks skip these.
	 */
	std::uint32_t PerDrawMask(Api::ShaderStage a_stage);
}
