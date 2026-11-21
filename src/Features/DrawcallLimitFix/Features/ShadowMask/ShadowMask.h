#pragma once

#include "RenderGraph/BufferRef.h"

// Shadow mask, in graph mode: the render graph draws the engine's shadow mask passes in the engine's place.
// Each shadow-casting light is one BSUtilityShader screen-quad pass into kSHADOW_MASK. The world's run at Main::Draw (AE 107141,
// SE 100423) and a menu scene's at UI3DSceneManager::Render (AE 52728, SE 51856). Details are in docs/development/p5/shadow-mask.md.
// The engine's CPU code runs unchanged. Its function runs with DrawTriShape latched (ShadowMaskCapture.cpp), and an ORG pass at
// the EngineDraws anchor draws the latched draws in engine order with CS's Utility.hlsl (ShadowMaskDraws.cpp).
// A draw the family cannot take flushes the latched part, and the engine draws the rest.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Features::ShadowMask
{
	namespace Address
	{
		// Main::RenderShadowMasks (AE 107140, SE 100422) calls the world's shadow masks when a light casts.
		inline constexpr Engine::Offset kWorldMasks{ { 107140, 0x7E }, { 100422, 0x90 } };
		// UI3DSceneManager::Render (AE 52727, SE 51855) calls the menu scene's shadow masks.
		inline constexpr Engine::Offset kMenuMasks{ { 52727, 0x461 }, { 51855, 0x466 } };
	}

	enum class View : std::uint8_t
	{
		World,  // Main::Draw's, after the depth pass: the sun's cascades and the world's shadow-casting lights
		Menu,   // a menu scene's (UI3DSceneManager::Render): the menu lights of the model's light scheme
		Count
	};

	/** @brief RE::RENDER_TARGET::kSHADOW_MASK: the one target the passes write. */
	inline constexpr std::uint32_t kShadowMaskTarget = 18;

	inline constexpr std::uint32_t kSlots = 8;

	using Kind = GpuTextures::ViewKind;

	/** @brief Each slot's declared dimension. The shadow maps (t3, t4, t6) are arrays. */
	constexpr Kind SlotKind(std::uint32_t a_slot)
	{
		return a_slot == 3 || a_slot == 4 || a_slot == 6 ? Kind::Texture2DArray : Kind::Texture2D;
	}

	/**
	 * @brief Whether the ORG build reads slot a_slot: only the scene depth (t2) and the light's shadow map (t4).
	 * Other slots latch as null views. Latching the map from t3 left the sun's masks fully in shadow. The engine reads the focus
	 * stencil (t5) from red, where DXVK's view has zero, so it never samples t6, and the ORG build reads 0 the same way.
	 */
	constexpr bool SlotRead(std::uint32_t a_slot) { return a_slot == 2 || a_slot == 4; }

	/** @brief Whether the ORG build reads slot a_slot's sampler. s3 is the filter-0 path's plain sampler. */
	constexpr bool SamplerRead(std::uint32_t a_slot) { return SlotRead(a_slot) || a_slot == 3; }

	/** @brief The rows of PS b0 (PerTechnique) and PS b2 (PerGeometry) the latch takes (Utility.hlsl's packoffsets). */
	inline constexpr std::uint32_t kTechniqueRows = 5;
	inline constexpr std::uint32_t kGeometryRows = 28;

	/** @brief UtilityOrgShadowMaskDraw in Utility.hlsl, 640 bytes. */
	struct DrawConstants
	{
		float psTechnique[kTechniqueRows][4];
		float psGeometry[kGeometryRows][4];
		float alphaTestRef;
		std::uint32_t vertexStride;
		std::uint32_t firstIndex;
		std::uint32_t padding;
		std::uint32_t textures[kSlots];
		std::uint32_t samplers[kSlots];
		RenderGraph::BufferRef vertexRef;  // the vertex buffer view ref the vertex stage pulls through
		RenderGraph::BufferRef indexRef;   // the index buffer's
		std::uint32_t vertexLayout[2];
		std::uint32_t padding2[2];
	};
	static_assert(sizeof(DrawConstants) == 640 && sizeof(DrawConstants) % 16 == 0, "UtilityOrgShadowMaskDraw's layout");

	struct Draw
	{
		Family::ProgramKey key;
		std::shared_ptr<const void> program;  // the RasterProgram (ShadowMaskDraws.cpp)
		std::uint32_t indexCount = 0;
		Family::Viewport viewport;
		DrawConstants constants{};
	};

	/** @brief What a view's client is made for. Slots are read by heap index, resolved per draw. */
	struct Shape
	{
		View view = View::World;
		bool operator==(const Shape&) const = default;
	};

	/** @brief A batch's draws, as the anchor's pass records them. */
	struct Run
	{
		View view = View::World;
		std::vector<Draw> draws;
		std::uint64_t revision = 0;  // what the recording depends on (ShadowMaskDraws.cpp, Run::Hash)
		Family::Owners owners;       // the textures' imports and the buffers' leases

		/** @brief What the recording depends on: everything but the latched values and counts (ShadowMaskDraws.cpp). */
		std::uint64_t Hash() const;
	};

	/** @brief The view's name for logs and client ids. */
	const char* NameOf(View a_view);

	Api::Feature Feature();

	/** @brief Whether the family is on, which is graph mode (read at install). */
	bool Enabled();

	void InstallHooks();
	void PreflightHooks();

	namespace ShadowMaskDraws
	{
		/** @brief Whether the anchor and runtime are up. False for good once a client failed. */
		bool Available();
		/** @brief Makes both views' clients and queues their programs. Main thread. False while the graph has no host. */
		bool Setup();
		/** @brief Whether a_view's batch is taken now. Its client follows kSHADOW_MASK to a new slot after a format change. */
		bool CanTake(View a_view);
		/** @brief How a_view's client reads the published id a bound slot exports. */
		Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index);
		/** @brief The program for a key, or null when it cannot be built. */
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader);
		/** @brief Keeps a batch's imports and leases alive when it ran nothing, so the next batches find them. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor. False means its draws were not drawn, and that is logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
