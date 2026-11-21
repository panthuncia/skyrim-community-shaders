#pragma once

#include <cstdint>
#include <array>
#include <memory>
#include <string>

namespace RE
{
	class BSBatchRenderer;
	class BSGeometry;
	class BSRenderPass;
	class NiAVObject;
	class NiNode;
}

namespace DCLF::Features::Shadows::Local
{
	struct SunExclusion;

	/**
	 * @brief Point-light shadow culls that skip the entries DCLF draws (dclf-architecture.md). AE and SE.
	 * BSShadowParabolicLight::Accumulate walks the whole scene for every point light each frame, though DCLF's paraboloid views
	 * already draw those casters. Process1 (vtable slot 0x16) skips an entry whose casters are all DCLF's, while the exclusion is
	 * current. Culls run on the render thread, and the detours and registration hook on any thread.
	 */
	namespace LocalLightCull
	{
		/** @brief Installs the Point source's hooks. Main thread, after the Sun source's registration hooks are in. */
		void Install();
		/**
		 * @brief Checks the slots before any hook is written. Accumulate and Process1 replace the engine's, so each must be the engine's.
		 * NiNode::OnVisible is optional and left out when another plugin (Grass Optimizations) holds it.
		 */
		void Preflight();
		/** @brief Whether the skip went in. */
		bool Installed();
		/** @brief Selects this frame's exclusion from the one the shadow epoch published. Render thread, at frame start. */
		void SelectFrame(std::shared_ptr<SunExclusion> a_pending);

		/** @brief The counters since the last Take, reset. */
		inline constexpr std::uint32_t kReachKindCount = 3;  // untested, sphere, portals (and "not reached" after them)
		struct Counters
		{
			std::uint64_t accumulateCalls = 0;
			std::int64_t accumulateTicks = 0;
			bool installed = false, filterInstalled = false;
			// The shadow culls' skip.
			std::uint64_t frames = 0, live = 0, noExclusion = 0, noClaims = 0, stale = 0, masked = 0, visited = 0, skipped = 0;
			// The category filter.
			std::uint64_t lights = 0, filtered = 0, walkedNotLive = 0, nodes = 0, children = 0, cut = 0, dirtyNodes = 0, builds = 0, nodesRebuilt = 0;
			std::int64_t buildTicks = 0;  // on the pool
			std::uint64_t buildsStale = 0, buildsRefused = 0;  // builds landed for an older verdict, or refused a lease
			std::size_t filterNodes = 0;
			// The lights' bits.
			std::uint64_t bitLights = 0, overflow = 0, given = 0, withBits = 0;
			// Empty-cull counters.
			std::uint64_t emptyLights = 0, emptyNotCovered = 0, emptyPortalStrict = 0, emptyGiven = 0, emptyWithBits = 0, emptyUnstamped = 0;
			std::uint64_t emptyLate = 0;    // lights culled empty before DCLF made their views (no shadow that frame)
			std::uint64_t spotCalls = 0, emptySpots = 0, spotsNotCovered = 0, spotsStrict = 0;
			std::int64_t spotTicks = 0;
			// Portal-strict counters. No graph means nothing to gate.
			std::uint64_t strictLights = 0, strictTested = 0, strictRefused = 0, strictNoGraph = 0, strictNoContext = 0;
		};
		Counters Take();

		// The category filter: inside a point light's cull, a category node culls its children less the excluded entries.
		// A node whose children change is walked natively until the next selection rebuilds the filter.
		/** @brief Notes a child attached to or detached from a_parent. Any thread, from SceneTracker's detours. */
		void NoteStructure(const void* a_parent);
		// Cut entries' geometries get each light's bit from DCLF at their main registration instead of from the light's cull.
		// A light whose views DCLF draws entirely is not culled at all, and its sphere bits are applied the same way.
		/** @brief Gives a cut entry's geometry the lights' bits, before a main registration (+0x160 0xFFFF). */
		void NoteMainRegistration(RE::BSGeometry* a_geometry);
		/** @brief Releases retired filters, two Presents later. Render thread, at Present. */
		void EndFrame();
	}
}
