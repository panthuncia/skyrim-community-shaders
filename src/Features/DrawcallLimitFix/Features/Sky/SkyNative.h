#pragma once

// The sky's draws computed from the passes the engine registered, or from the sky root (native's main view and faces), the camera
// and a few engine globals.

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"

#include <string>
#include <vector>

namespace RE
{
	class BSGeometry;
	class BSShaderAccumulator;
}

namespace DCLF::Features::Sky::Native
{
	/** @brief What the batch's draws share: the view and its targets. */
	struct BatchInputs
	{
		View view = View::Main;
		std::array<std::uint32_t, 3> colourFormats{};
		std::uint32_t depthFormat = 0;
		std::uint32_t renderFlags = 0;  // RenderBatches' flags (0x108 keeps the cull mode)
		Integrations::PhysicalSky::OrgHandles physSky{};
	};

	/** @brief One pass's draw: built, left out while its program builds (logged once), or not computable (a_why). */
	enum class PassResult : std::uint8_t
	{
		Drawn,
		LeftOut,
		Failed,
	};

	/**
	 * @brief Render thread, at the sky batch's call, before it. Computes the draws of every pass the accumulator registered for the
	 * sky techniques, in the engine's order. A draw whose program still builds is left out. False, with a_why, when a pass cannot be
	 * computed (the engine then draws the batch).
	 */
	bool BuildBatch(RE::BSShaderAccumulator* a_accumulator, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners,
		std::string& a_why);

	/** @brief At install: the game half takes the sky root's objects into the frame ticket (TicketPart::SkyRoot), for BuildMain and BuildFace. */
	void RegisterRootCapture();

	/**
	 * @brief Render thread, at the main pass's sky batch, in native. The sky batch's and cloud layers' draws from the sky root culled
	 * with the adopted snapshot's main camera, in the engine's draw order, without the engine's registrations. Sky-object effects are
	 * not the family's and are left out. False, with a_why, when a pass cannot be computed.
	 */
	bool BuildMain(RE::BSShaderAccumulator* a_accumulator, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why);

	/**
	 * @brief Render thread, at the cloud group's call, before it. The draws of its cloud layers (geometry group 13's sky passes), in
	 * list order, with Cloud Shadows' parts. a_lastLayer is the face's last cloud layer so far, and is advanced.
	 */
	bool BuildClouds(RE::BSShaderAccumulator* a_accumulator, RE::BSBatchRenderer::GeometryGroup* a_group, const BatchInputs& a_inputs, std::int32_t& a_lastLayer,
		std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why);

	/**
	 * @brief Render thread, in place of the engine's sun query draw (AE 106604), with the query's camera set. The sun's occlusion
	 * draw (raw technique 0, SunOcclude) from the sun geometry.
	 */
	PassResult BuildSun(RE::BSShaderAccumulator* a_accumulator, RE::BSGeometry* a_geometry, const BatchInputs& a_inputs, Draw& a_draw, Family::Owners& a_owners,
		std::string& a_why);

	/**
	 * @brief A face's draws, with the face's camera: the passes the engine's face cull of the sky root would register, in its draw
	 * order. False, with a_why, when the face shows something the native face cannot draw (a sky-object effect) or a pass cannot be
	 * computed.
	 */
	bool BuildFace(const FaceView& a_view, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why);

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
}
