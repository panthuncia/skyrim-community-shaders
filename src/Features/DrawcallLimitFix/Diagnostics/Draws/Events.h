#pragma once

// Hook points of the draws area, for the diagnostics observers. Each is announced with Diagnostics::Notify on the thread its
// comment names. An event that hands something back carries a reference the main path pre-fills with its own answer.

#include "DrawsView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/ShadowViewTally.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

namespace DCLF::Diagnostics
{
	// ---- The main epochs (render thread).

	/** @brief A main epoch finished executing. depth is the frame's Z-prepass payload, for the colour epoch only. */
	struct MainEpochDone
	{
		const std::shared_ptr<Draws::Resources>& resources;
		const Draws::MainPayload& payload;
		const Draws::MainPayload* depth;  // or null
		bool depthOnly;
		// The row tables BuildDraws read, or null.
		const Draws::PayloadVersions* payloadVersions;
		IndirectDrawStats& stats;
	};

	/** @brief A main commit wrote its culling latch. */
	struct CullLatchWritten
	{
		const Draws::BuildDrawsLatch& latch;
		bool depthOnly;
	};

	/** @brief The depth commit's fade updates had differing inputs. The first one stands for all. */
	struct FadeInputsVaried
	{
		std::uint32_t frame;
	};

	/** @brief A main commit's culling setup, sent every commit. */
	struct MainCullingSetup
	{
		const Draws::PassFrame& shape;
		const Draws::Capture& capture;
		bool depthOnly, hasViewProj, replay;
		std::uint32_t frame, liveObjects;
		std::span<const std::byte> replayPerFrame;
	};

	/** @brief The main epochs' graph passes are being listed. An observer may append its own at `point`. */
	struct MainGraphPasses
	{
		enum Point : std::uint8_t
		{
			kBeforeColour,  // before the colour draws (the colour epoch)
			kAfterColour,   // after them, before the depth segment's two-phase tail
			kAfterDepth,    // after the depth segment's passes (the Z-prepass epoch)
		};
		Point point;
		const std::shared_ptr<Draws::Resources>& resources;
		std::uint32_t epoch;  // RenderGraphRuntime::EpochOf
		std::vector<org::RenderGraph::ExternalPassDesc>& out;
	};

	// ---- Stores the builds keep, on the builder's thread or the render thread while the builder is idle.

	/**
	 * @brief Extras version `version` is about to be built from `previous`, or written whole when that is 0.
	 * An observer may supply the mirror the delta is copied into.
	 */
	struct ExtrasSending
	{
		std::uint64_t version, previous;
		std::vector<float>*& mirror;
	};

	/**
	 * @brief Extras version `version` was built to `rows`. Rows in blocks no live object owns in `tables` are never read.
	 */
	struct ExtrasSent
	{
		std::uint64_t version;
		const Draws::ExtrasOut& rows;
		const SceneTables* tables = nullptr;
	};

	/** @brief A kept store was brought up to `tables`, before its view is taken. */
	template <class Store>
	struct StoreUpdated
	{
		const Store& store;
		const SceneTables& tables;
		std::uint32_t frame;
	};

	/** @brief A kept shadow build was made from `in`. */
	struct ShadowBuilt
	{
		const Draws::ShadowInputs& in;
		const SceneTables& tables;
		const Lookups& lookups;
		const Draws::ShadowPayload& payload;
		const Draws::ShadowKept& kept;
	};
}
