#pragma once

#include "RenderGraph/BufferRef.h"

// Shadow mask, in graph mode: the render graph draws the shadow mask passes in the engine's place.
// Each shadow-casting light is one BSUtilityShader screen-quad pass into kSHADOW_MASK. The world's run at Main::Draw (AE 107141,
// SE 100423) and a menu scene's at UI3DSceneManager::Render (AE 52728, SE 51856). Details are in docs/development/engine/shadow-mask.md.
// At each call the draws are computed from the shadow lights (ShadowMaskNative.cpp), the call's own bookkeeping is done
// (ShadowMaskHooks.cpp), and an ORG pass at the EngineDraws anchor draws them in engine order with CS's Utility.hlsl
// (ShadowMaskDraws.cpp). The engine's call does not run. A batch the native build cannot compute is the engine's, logged once.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawFamily.h"
#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

	/** @brief The most draws a run holds: one per shadow light, which the engine caps far below this. */
	inline constexpr std::uint32_t kMaxDraws = 32;

	using Kind = GpuTextures::ViewKind;

	/** @brief Each slot's declared dimension. The shadow maps (t3, t4, t6) are arrays. */
	constexpr Kind SlotKind(std::uint32_t a_slot)
	{
		return a_slot == 3 || a_slot == 4 || a_slot == 6 ? Kind::Texture2DArray : Kind::Texture2D;
	}

	/** @brief The rows of PS b0 (PerTechnique) and PS b2 (PerGeometry) a draw holds (Utility.hlsl's packoffsets). */
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

	/** @brief One light's draw: built, left out while its program builds (logged once), or not computable. */
	enum class PassResult : std::uint8_t
	{
		Drawn,
		LeftOut,
		Failed,
	};

	namespace Native
	{
		/** @brief Shadows::Native's light values, as the masks read them. */
		using Light = Shadows::Native::ShadowLight;

		/**
		 * @brief Render thread, at a view's shadow masks. The draws of a_lights (the view's caster array, in the engine's order: the
		 * world's from the light set, a menu scene's and guest's read at the call), computed from each light's values, its maps, the
		 * settings and the camera. False, with a_why, when a light cannot be computed (the engine then draws the batch). a_menuEye, for a
		 * menu scene CS renders natively: the menu camera's eye, and the draws start from CS's own states rather than the engine's.
		 */
		bool Build(View a_view, Run& a_run, std::span<const Light> a_lights, const Shadows::Native::MaskSettings& a_settings, const Shadows::Native::SunSplits& a_sun,
			std::string& a_why, const RE::NiPoint3* a_menuEye = nullptr);

		/** @brief FUN_1415221e0: the vertex descriptor of a raw Utility technique (the technique less 0x2B). */
		std::uint32_t VertexDescriptorOf(std::uint32_t a_raw);

		/** @brief The shadow scene node whose caster array a_view's masks walk (read where the view is drawn), or null. */
		const RE::ShadowSceneNode* SceneNodeOf(View a_view);

		/** @brief The world scene node's sun light, which the cascaded mask reads, read now. */
		Shadows::Native::SunSplits ReadSun();

		/** @brief FUN_1414f4350: ends the technique the engine left open, as the masks' call does first. */
		void EndTechnique();

		/** @brief The world's sun cascade map (the map its mask draw binds at t4), or null. */
		ID3D11ShaderResourceView* SunMap(View a_view, std::span<const Light> a_lights);
	}

	/**
	 * @brief Native: the world's shadow masks (Main::RenderShadowMasks' call of SE 100423 / AE 107141), driven by CS with no engine function
	 * called. kSHADOW_MASK is cleared to a_clear through the skeleton, and the draws are built from the light set with the snapshot's main
	 * camera and run at the anchor. The lights' maps were given back on the game side, once the set was made (Shadows::Native::CaptureSet).
	 * Render owner, in a native world frame.
	 */
	void DrawWorldMasks(const float a_clear[4]);

	/**
	 * @brief Native: a menu scene's shadow masks (UI3DSceneManager::Render's call of SE 51856 / AE 52728) for a render CS draws itself, with
	 * no engine function called. kSHADOW_MASK is cleared, the draws are built from the menu scene node's kept shadow lights with the menu
	 * camera's eye a_eye, and they run at the anchor. Fatal when they cannot be built. Render owner.
	 */
	void DrawMenuMasks(const RE::NiPoint3& a_eye);

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
		std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, bool* a_building = nullptr);
		/** @brief Keeps a batch's imports and leases alive when it runs nothing, so the next batches find them. */
		void Keep(Family::Owners&& a_owners);
		/** @brief Runs a_run's view at the anchor. False means its draws were not drawn, and that is logged. */
		bool Submit(std::shared_ptr<Run> a_run);
		std::string Status();
	}
}
