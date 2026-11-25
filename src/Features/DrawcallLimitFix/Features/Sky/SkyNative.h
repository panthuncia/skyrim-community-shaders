#pragma once

// The sky batch without draw capture (docs/development/plans/dclf-native-sky.md): its draws are computed from the passes the
// engine registered, the camera and a few engine globals, instead of latched from the engine's draws.

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"

#include <span>
#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
	class BSShaderAccumulator;
}

namespace DCLF::Features::Sky::Native
{
	enum class Mode : std::uint8_t
	{
		Off,      // CS_DCLF_SKY_NATIVE=0: the capture path alone
		Observe,  // 1: the capture path draws; the native draws are computed and compared with it
		Draw,     // 2 (default): the native draws are drawn; the engine's calls draw nothing of the sky
	};

	/** @brief Registers CS_DCLF_SKY_NATIVE. Install. */
	void RegisterSwitch(Api::InstallContext& a_context);

	Mode GetMode();

	/** @brief What the batch's draws share: the view and its targets, from the capture's shape. */
	struct BatchInputs
	{
		View view = View::Main;
		std::array<std::uint32_t, 3> colourFormats{};
		std::uint32_t depthFormat = 0;
		std::uint32_t renderFlags = 0;  // RenderBatches' flags (0x108 keeps the cull mode)
		Integrations::PhysicalSky::OrgHandles physSky{};
	};

	/**
	 * @brief Render thread, at the sky batch's call, before it. Computes the draws of every pass the accumulator registered for the
	 * sky techniques, in the engine's order. False, with a_why, when a pass cannot be computed (the batch then stays the capture's).
	 */
	bool BuildBatch(RE::BSShaderAccumulator* a_accumulator, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners,
		std::string& a_why);

	/**
	 * @brief Render thread, at the cloud group's call, before it. The draws of its cloud layers (geometry group 13's sky passes), in
	 * list order, with Cloud Shadows' parts. a_lastLayer is the face's last cloud layer so far, and is advanced.
	 */
	bool BuildClouds(RE::BSShaderAccumulator* a_accumulator, RE::BSBatchRenderer::GeometryGroup* a_group, const BatchInputs& a_inputs, std::int32_t& a_lastLayer,
		std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why);

	/**
	 * @brief Render thread, in place of the engine's sun query draw (AE 106604), with the query's camera set. The sun's occlusion
	 * draw (raw technique 0, SunOcclude) from the sun geometry. False, with a_why, when it cannot be computed.
	 */
	bool BuildSun(RE::BSShaderAccumulator* a_accumulator, RE::BSGeometry* a_geometry, const BatchInputs& a_inputs, Draw& a_draw, Family::Owners& a_owners,
		std::string& a_why);

	/** @brief A face's sky pass: what the engine's face cull would register for a sky object. */
	struct FacePass
	{
		RE::BSGeometry* geometry = nullptr;
		RE::BSShaderProperty* property = nullptr;
		std::uint32_t passId = 0;
		std::uint32_t sub = 0;  // the batch sub-group (FindTechniqueGroup), or the cloud list
	};

	/**
	 * @brief The passes the engine's face cull of the sky root would register, in the engine's draw order: the sky batch by
	 * technique, sub-group and newest first, then the cloud layers (geometry group 13) in registration order. False, with a_why,
	 * when the face shows something the native face cannot draw (a sky-object effect).
	 */
	bool FacePasses(const FaceView& a_view, std::vector<FacePass>& a_batch, std::vector<FacePass>& a_clouds, std::string& a_why);

	/** @brief A face's draws from FacePasses, with the face's camera. a_lastLayer is advanced over the cloud layers. */
	bool BuildFace(const FaceView& a_view, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why);

	/**
	 * @brief Observe mode, at the engine's face sky batch: compares the native face's passes, camera and starting state with the
	 * engine's registrations and shadow state. Logs each kind of difference once.
	 */
	void CompareFaceBatch(const FaceView& a_view, RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags);
	/** @brief Observe mode, at the engine's face cloud group: the cloud list and the cloud layers' starting state. */
	void CompareFaceClouds(const FaceView& a_view, RE::BSBatchRenderer::GeometryGroup* a_group);

	/** @brief Leaves only the group's sky-object effects linked, so the engine's call draws them and nothing of the clouds. */
	void DetachClouds(RE::BSBatchRenderer::GeometryGroup* a_group);

	/** @brief The sky sub-groups taken out of the batch renderer, so the engine's call finds nothing to draw. */
	struct Detached
	{
		struct Entry
		{
			void* group = nullptr;  // the PassGroup
			std::array<RE::BSRenderPass*, 5> heads{};
			std::uint32_t bits = 0;
		};
		std::vector<Entry> entries;
		bool restore = false;  // the batch renderer keeps its passes (autoClearPasses off)
	};

	/** @brief Takes the sky sub-groups out, as the engine's auto clear would after drawing them. */
	Detached DetachBatch(RE::BSShaderAccumulator* a_accumulator);
	/** @brief Puts them back when the engine would have kept them. After the engine's call. */
	void Reattach(Detached& a_detached);

	/** @brief Observe mode: compares the native draws with the captured ones and logs each kind of difference once. */
	void Compare(View a_view, std::span<const Draw> a_native, std::span<const Draw> a_captured);

	/** @brief The parity counts, for the family's status line. */
	std::string Status();
}
