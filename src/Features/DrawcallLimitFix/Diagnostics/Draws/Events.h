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
		// The row tables BuildDraws read, or null. The build parity uses their bases.
		const Draws::PayloadVersions* payloadVersions;
		IndirectDrawStats& stats;
	};

	/** @brief A main commit wrote its culling latch. */
	struct CullLatchWritten
	{
		const Draws::BuildDrawsLatch& latch;
		bool depthOnly;
	};

	/** @brief A frame texture register that a main commit could not resolve, so it reads zero. Sent every commit. */
	struct FrameTextureUnresolved
	{
		std::uint32_t textureRegister;
		ID3D11ShaderResourceView* view;
	};

	/** @brief A frame buffer at t`textureRegister` larger than the constant mirror can fill, so its copy reads zero. */
	struct FrameBufferUnfilled
	{
		std::uint32_t textureRegister, elements, stride;
		std::size_t mirrorBytes;
	};

	/** @brief The depth commit's fade updates had differing inputs. The first one stands for all. */
	struct FadeInputsVaried
	{
		std::uint32_t frame;
	};

	/** @brief A main commit drew past a geometry's buffers. Sent once per stats window. */
	struct ShortBuffersDrawn
	{
		const SceneTables& tables;
		const Draws::MainPayload& payload;
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

	/** @brief The depth commit's fade roots before upload. An observer may name the first root of FadeStateCS's log window, ~0u for none. */
	struct FadeLogWindow
	{
		std::uint32_t frame;
		const SceneTables& tables;
		const FadeFrame& inputs;
		std::uint32_t& base;
	};

	/** @brief A main commit's culling ViewProj, relative to latch.cullEye. */
	struct MainCullViewProj
	{
		const Draws::BuildDrawsLatch& latch;
		std::uint32_t width, height, frame;
		bool depthOnly;
		const SceneTables& tables;
	};

	/** @brief The first-person walk of the skeleton's geometry, every 16 frames, with the frame's claims. */
	struct FirstPersonSkeletonWalked
	{
		const SceneTables& tables;
		const ankerl::unordered_dense::set<const RE::BSGeometry*>& skeleton;
		std::size_t walked;
		const SetSnapshot& claims;
	};

	/** @brief The engine drew a reflection face and its targets are still bound. */
	struct ReflectionFaceDrawn
	{
	};

	/** @brief The foliage record was made. It is valid once an observer finds it ready. */
	struct FoliageRecordMade
	{
		Draws::Resources::FoliageRecord& record;
		rhi::Device device;
		std::uint32_t slots;  // readback ranges to make
		bool& ready;
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

	// ---- Adoption, coverage, main epochs and retirement (render thread, in frame order).

	/** @brief The frame adopted a snapshot. Its set and payload, either possibly null, are now the frame's. */
	struct SnapshotAdopted
	{
		std::uint32_t frame;
		std::uint64_t snapshot;  // the snapshot's serial
		const Draws::NamedVersions* set;
		const Draws::ScenePayload* payload;
		bool growth;  // the first versions a growth made current
	};

	/** @brief The frame's main epoch coverage was decided, with what it was decided from. */
	struct EpochCoverageDecided
	{
		std::uint32_t frame;
		std::span<const bool> covered;
		std::uint64_t snapshot;  // 0 for none
		bool drawsComplete, stale;
		const Draws::ScenePayload* payload;  // null for none
		bool versions;  // the active revision names a set
		bool current;   // and it is the adopted one, with the snapshot current and a payload source
	};

	/** @brief A main epoch's outcome, every frame. Why it was not submitted, or whether it failed, committed nothing or was submitted. */
	struct MainEpochOutcome
	{
		std::uint32_t frame;
		const char* outcome;
		const Draws::PayloadVersions* payload;  // null for none
		std::uint64_t snapshot;  // 0 for none
		bool depthOnly;
		bool executed;   // the epoch reached its execution (outcome: "submitted", "FAILED" or "NOT COMMITTED")
		bool notDrawingYet;  // no claims installed yet, the only claims-less frames
	};

	/** @brief The frame bound its adopted set at `point`. A point at or below the completed point binds nothing. */
	struct AdoptedBound
	{
		std::uint32_t frame;
		std::uint64_t point, completed;
	};

	// ---- The shadow epochs (render thread).

	/** @brief The index pool now covers the tables' first `count` geometry slots. */
	struct IndexPoolUpdated
	{
		const Draws::IndexPool& pool;
		const SceneTables& tables;
		std::uint32_t count;
	};

	/** @brief Where DCLF predicts the engine draws a shadow view. The renderer's state holds where it actually did. */
	struct ShadowTargetPredicted
	{
		std::uint32_t viewId;
		const ShadowViews::View* shadowView;
		std::uint32_t occurrence, target, slice;
	};

	/** @brief A view whose viewport and eye are DCLF's, before they replace what the capture read. cascade may be null. */
	struct OwnShadowViewport
	{
		const Draws::PendingView& view;
		const ShadowCameras::Camera* cascade;
		std::uint32_t descriptor, target;
		ShadowView kind;
	};

	/** @brief A view whose block is DCLF's, before it replaces the engine block the capture took. */
	struct OwnShadowCamera
	{
		const Draws::PendingView& view;
		const ShadowCameras::Camera& cascade;
		std::uint32_t descriptor;
		ShadowView kind;
	};

	/** @brief A shadow build took DCLF's full-frustum processes for `frame`, or found none current. */
	struct SunEntryPlanesTaken
	{
		std::uint32_t frame;
		bool taken;
	};

	/** @brief The engine's FinishAccumulating of occlusion map `view`. */
	struct OcclusionCaptured
	{
		std::uint32_t view;
	};

	/** @brief A shadow view's last latch write of the epoch, with the payload it draws. */
	struct ShadowViewLatched
	{
		const Draws::PendingView& view;
		const Draws::BuildDrawsLatch& latch;
		std::uint32_t frame;
		const Draws::ShadowPayload& payload;
		ShadowDrawStats& stats;
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

	/** @brief An exclusion the cache would have reused was built in full. Reports whether the two agree. */
	struct ExclusionRebuilt
	{
		bool parabolic;
		bool same;
	};

	/** @brief A publication's draws were built ahead for `frame`. */
	struct DrawsBuiltAhead
	{
		const DrawsView::Impl::DrawPublication& draws;
		const SceneTables& tables;
		std::uint32_t frame;
	};

	// ---- The sealed set. The snapshot is the only source of a versioned buffer's address and SRV index.

	/** @brief A scene revision was sealed, with the draws its snapshot pairs with it. */
	struct SnapshotSealed
	{
		const Draws::NamedVersions& sealed;
		const DrawsView::Impl::DrawPublication& draws;
		const DrawsView::Impl::AheadContext& context;
	};

	/** @brief A commit staged bytes into a versioned buffer through the adopted set. Render thread. */
	struct CommitTargetStaged
	{
		const Draws::NamedVersions& sealed;
		const Draws::Versioned& target;
	};

	/** @brief The revision producer made shapes from the sealed set, for tables of `geometries` slots. */
	struct RevisionShapesMade
	{
		const Draws::NamedVersions& sealed;
		const Draws::MainLayout& layout;
		const Draws::MadeShapes& made;
		const Draws::RevisionInputs& inputs;
		std::size_t geometries;
	};
}
