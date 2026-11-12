#pragma once

// What every DCLF family that draws the engine's own draws on the graph shares (the sky, the water, the screen blood, the shadow masks):
// the one DrawTriShape detour they latch through, their SetupGeometry watch, the batch's pass count, the geometry, viewport, constant,
// texture and header reads of a latched draw, the leases a run keeps, and the setup point where their clients register. The families
// are the primary path for their draws (docs/development/p5/primary-path.md): a client registers at setup with every input declared,
// a batch is withheld when its anchor can run, and a draw the family cannot take ends the latched part of its batch: the latched draws
// run at that point and the engine draws the rest, in order. Nothing a family hands back is vouched for under the flush latch: the
// latch skips it and the residue census counts it.
//
// Render thread (the render owner) unless a function says otherwise.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/FrameAnchors.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct ID3D11Buffer;
struct ID3D11ShaderResourceView;

namespace org
{
	class RenderPassBuilder;
}

namespace DCLF::Family
{
	/** @brief Leases and imports a latched draw holds until its run's GPU work is done. */
	using Owners = std::vector<std::shared_ptr<const void>>;

	namespace Address
	{
		// BSGraphics::Renderer::DrawTriShape(renderer, TriShape*, startIndex, triangleCount): a static TriShape's draw (R1).
		inline constexpr Engine::Offset kDrawTriShape{ { 77263 }, { 75477 } };
	}

	/** @brief BSGraphics::TriShape's head (R1): the static vertex and index buffers and the VertexDesc. */
	struct TriShape
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		std::uint64_t vertexDesc;
	};

	/**
	 * @brief A family's DrawTriShape taker: true when it took the draw (latched it), so the engine does not make it. Called for every
	 * DrawTriShape on the render owner's thread; a taker whose family has no batch open returns false at once.
	 */
	using DrawTaker = bool (*)(const TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles);

	/**
	 * @brief Adds a_taker to the one DrawTriShape detour, which the first call installs. Takers are asked in the order added; the first
	 * that takes the draw ends it. False, logged, when this runtime has no address for DrawTriShape. Install time (main thread).
	 * ImageSpace keeps its own detour for now (RenderGraph/Engine/ImageSpace); it can move here as a taker.
	 */
	bool AddDrawTaker(std::string_view a_family, DrawTaker a_taker);

	/** @brief Whether the DrawTriShape detour is installed (AddDrawTaker succeeded once). */
	bool DrawTakersInstalled();

	/** @brief A family's SetupGeometry watcher: after the shader's own SetupGeometry (and the hooks chained before this one). */
	using SetupWatcher = void (*)(RE::BSRenderPass* a_pass);

	namespace Detail
	{
		template <RE::BSShader::Type kType>
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				func(a_this, a_pass, a_flags);
				if (watcher)
					watcher(a_pass);
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static inline SetupWatcher watcher = nullptr;
		};
	}

	/**
	 * @brief Watches a shader's SetupGeometry (vtable slot 6 of a_vtable, the shader type's BSShader vtable): a_watcher runs after it,
	 * where the draw's constants are written. One watcher per shader type; install time (main thread).
	 */
	template <RE::BSShader::Type kType>
	void WatchSetupGeometry(REL::VariantID a_vtable, SetupWatcher a_watcher)
	{
		using Thunk = Detail::SetupGeometry<kType>;
		if (Thunk::watcher)
			return;
		Thunk::watcher = a_watcher;
		stl::write_vfunc<0x6, Thunk>(a_vtable);
	}

	/**
	 * @brief Adds a family's setup, which registers its clients and queues its programs. It runs at CS's setup (RunSetups, from
	 * DrawcallLimitFix::PostSetupResources), before the setup's graph build (RenderGraph::Engine::PostSetup, FrameAnchors::BuildPending),
	 * so the clients are in their anchors' graphs before any world frame. A setup that returns false there (no render graph host) is
	 * tried again on the main thread after every frame-job wait (RenderGraph::Engine::MainTick) until it returns true. Install time
	 * (main thread). False, logged, when the tick could not be installed.
	 */
	bool AddSetup(std::string_view a_family, std::function<bool()> a_setup);

	/** @brief CS's setup (main thread, after every feature's SetupResources): every family's setup added so far, once. */
	void RunSetups();

	/**
	 * @brief Once a game frame, from the first family batch: the render thread's buffer registry (GpuResources::Frame) drops the entries
	 * whose leases went and takes DXVK's buffer moves. Grass and tree LOD do the same at their depth commit; the families' leases need
	 * it whether those run or not.
	 */
	void BeginBatch();

	/** @brief The engine's frame counter (Renderer::End increments it). Any thread. */
	std::uint32_t GameFrame();

	/**
	 * @brief A batch's passes against its draws: every pass SetupGeometry set up must draw through DrawTriShape, since a pass that drew
	 * through another function was the engine's, at its place in the batch, ahead of the graph's draws.
	 */
	struct Passes
	{
		static constexpr std::uint32_t kNone = 0xFF;

		std::uint32_t passes = 0;         // SetupGeometry calls
		std::uint32_t drawn = 0;          // DrawTriShape calls
		std::uint32_t pendingType = kNone;  // the last pass's geometry type while its draw has not reached DrawTriShape
		std::uint32_t otherTypes = 0;       // the geometry types of passes that drew elsewhere, a bit each

		void Begin() { *this = {}; }
		/**
		 * @brief A pass set up (its geometry's type, BSGeometry::GetType: R1's dispatcher picks the draw function by it). False when the
		 * pass will not draw through DrawTriShape (DrawsThroughTriShape): the family ends its latched part there, before the
		 * engine draws the pass.
		 */
		bool OnSetup(const RE::BSRenderPass* a_pass);
		/** @brief A DrawTriShape of the batch. */
		void OnDraw()
		{
			++drawn;
			pendingType = kNone;
		}
		/** @brief After the batch: the last pass folded in. True when every pass drew through DrawTriShape. */
		bool End();
		/** @brief The shortfall in words, for a log. */
		std::string Describe(std::string_view a_what) const;
	};

	/** @brief Whether R1's geometry-type dispatcher draws a_pass through DrawTriShape (its geometry type and accumulation hint). */
	bool DrawsThroughTriShape(const RE::BSRenderPass* a_pass);

	/** @brief A latched draw's geometry, read by device address while its leases are held. */
	struct Geometry
	{
		std::uint64_t vertexAddress = 0, indexAddress = 0;
		std::uint64_t layout = 0;  // the engine's input layout key: the vertex shader's mask and the geometry's VertexDesc
		std::uint32_t stride = 0, firstIndex = 0, indexCount = 0;
	};

	enum class GeometryResult : std::uint8_t
	{
		Ok,
		Missing,   // no TriShape, or no buffers
		Topology,  // not a triangle list
		Buffers,   // a buffer could not be imported
		Bounds,    // the index range is past the index buffer, or a vertex is smaller than its position
	};

	const char* ReasonOf(GeometryResult a_result);

	/**
	 * @brief The draw's geometry: both buffers leased (into a_owners), the 16-bit index range checked against the index buffer and the
	 * vertex stride against the position the vertex stage pulls (R32G32B32A32: DCLFPullPosition). a_shaderVertexDesc: the bound vertex
	 * shader's VertexDesc mask; a_topology: the shadow state's. a_detail, when given, receives the numbers of a refusal (the index range,
	 * the buffers' bytes, the stride and the vertex count) for its log line.
	 */
	GeometryResult LatchGeometry(const TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::uint64_t a_shaderVertexDesc,
		std::uint32_t a_topology, Geometry& a_out, Owners& a_owners, std::string* a_detail = nullptr);

	/** @brief The viewport SetDirtyStates would set, and its depth range. */
	struct Viewport
	{
		std::array<std::uint32_t, 4> rect{};  // x, y, width, height
		std::array<float, 2> depthRange{ 0.0f, 1.0f };
	};

	/**
	 * @brief The shadow state's viewport, with the camera state's depth range and the depth bias mode's offset taken off its maximum
	 * (BSGraphics::CreateDepthBiasViewportOffsets' table, SE 0x141794530, AE 0x1419db270; R2, SetDirtyStates' dirty bit 0x40).
	 */
	Viewport LatchViewport(std::uint32_t a_depthBias);

	namespace Constants = ::RenderGraph::Engine::Constants;
	namespace Flush = ::RenderGraph::Engine::Flush;

	/**
	 * @brief The constant buffers bound for the draw about to be latched, as the engine constants capture saw them written and bound
	 * (Constants::Capture of the vertex and pixel stages: it tracks every buffer's writes from startup, so a buffer's first use has its
	 * bytes). Invalid when the capture is not active (CS_ORG_ENGINE_CONSTANTS=1, which the families require).
	 */
	Constants::UseId CaptureConstants();

	/** @brief The bytes of a_slot of a_stage the use bound (0: nothing bound there). */
	std::uint32_t ConstantBytes(Constants::UseId a_use, Constants::Stage a_stage, std::uint32_t a_slot);

	/**
	 * @brief A slot's constants as the draw reads them, copied into a_out (zeros past them: a technique whose vanilla shader declares
	 * fewer rows does not read those). True for nothing bound there; false for an invalid use.
	 */
	bool CopyConstants(Constants::UseId a_use, Constants::Stage a_stage, std::uint32_t a_slot, std::span<std::byte> a_out);

	/** @brief CopyConstants into whole 16-byte rows. */
	inline bool CopyRows(Constants::UseId a_use, Constants::Stage a_stage, std::uint32_t a_slot, float (*a_rows)[4], std::uint32_t a_count)
	{
		return CopyConstants(a_use, a_stage, a_slot, { reinterpret_cast<std::byte*>(a_rows), a_count * std::size_t{ 16 } });
	}

	/** @brief What a texture slot of a latched draw reads. */
	enum class SlotResult : std::uint8_t
	{
		Ok,
		Published,  // a published graph texture's export, which only a declared read orders after its writes
		Failed,     // the view could not be imported (a_failure says why), or the null view could not be made
	};

	/**
	 * @brief A draw's texture slot as the shadow state holds it: a_view's import, or for an empty slot the null view of the slot's
	 * declared kind. a_view is unwrapped (Tripwire). The binding's owner goes into a_owners.
	 */
	SlotResult SlotTexture(ID3D11ShaderResourceView* a_view, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, std::uint32_t& a_index, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure = nullptr);

	/** @brief A set of published ids. */
	using PublishedIds = std::set<std::string, std::less<>>;

	/** @brief The id EngineTargets publishes an engine target under: "Engine::" and the enum's name ("Engine::kSSR"). */
	std::string EngineTargetId(RE::RENDER_TARGET a_target);
	/** @brief An engine depth target's id: "Engine::Depth::" and the enum's name. */
	std::string EngineTargetId(RE::RENDER_TARGET_DEPTHSTENCIL a_target);
	/** @brief An engine cubemap target's id: "Engine::Cubemap::" and the enum's name. */
	std::string EngineTargetId(RE::RENDER_TARGET_CUBEMAP a_target);

	/**
	 * @brief The published ids Drawcall Limit Fix's registry lists at a_registers: its frame textures' graph ids
	 * (Api::FrameTexture::graphResources) and its frame graph buffers' (Api::FrameGraphBuffers). The ported features publish what CS
	 * binds at those registers under these ids. Any thread, after DCLF's install.
	 */
	std::vector<std::string> RegisteredIdsAt(std::span<const std::uint32_t> a_registers);

	/** @brief How a client reads a published export bound at a slot or a header (PublishedInputs::Resolve). */
	enum class InputResult : std::uint8_t
	{
		Graph,          // by the id's bindless index: the client's graph declares the id
		NotListed,      // the id is not among the client's inputs: an inherent refusal (the family's list should name it)
		PublishedLate,  // listed, but first published after the client's graph was built: a producer that publishes during play (an error)
		NoView,         // the id has no whole-resource view (PublishedResources::ShaderResourceIndex)
	};

	/** @brief InputResult in words, for a log. */
	const char* ReasonOf(InputResult a_result);

	/**
	 * @brief The published ids a family client's draws can read, fixed when the client is made at setup.
	 *
	 * The engine targets among them are imported when listed (AddEngine), which publishes them, so the client's first build declares
	 * them. The ported features publish theirs at their own setup (SetupResources), before the families' setup. At each build of its
	 * anchor's graph, the client's pass declares a read of every listed id that every graph registers (PublishedResources::IsRegistered),
	 * for order only, and records it as declared. An id stays registered once published, so the set only grows. A producer that
	 * publishes an id for the first time during play rebuilds every graph by that Publish (not the family); until that rebuild the id is
	 * PublishedLate, an error the family logs once, and that batch is the engine's.
	 *
	 * Filled before the client registers, then shared with the client's passes.
	 */
	class PublishedInputs
	{
	public:
		/** @brief Lists a_id (once; an empty id is ignored). Before the client registers. */
		void Add(std::string_view a_id);
		/** @brief Lists each of a_ids. */
		void Add(std::span<const std::string> a_ids);
		/** @brief Lists an engine target's id and imports the target now, which publishes it (render thread, at setup). */
		void AddEngine(RE::RENDER_TARGET a_target);
		void AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL a_target);
		void AddEngine(RE::RENDER_TARGET_CUBEMAP a_target);

		/** @brief Graph build: declares a read, through no view, of each listed id every graph registers, and records it as declared. */
		void Declare(org::RenderPassBuilder& a_builder);

		/**
		 * @brief Render thread (a batch): how the client reads a_id, the id a bound view exports (PublishedResources::ExportedId). On
		 * Graph, a_index receives the bindless index of the id's whole-resource view (PublishedResources::ShaderResourceIndex).
		 */
		InputResult Resolve(std::string_view a_id, std::uint32_t& a_index) const;

		/** @brief The listed ids, comma separated, for the registration's log line. */
		std::string Describe() const;

		/** @brief The listed ids no graph registers yet (comma separated; empty: none), for the setup's log line. */
		std::string Unregistered() const;

	private:
		std::vector<std::string> ids;
		// The ids a build declared. Replaced whole, and it only grows. Atomic because a graph may be built on the host's owner thread.
		std::atomic<std::shared_ptr<const PublishedIds>> declared;
	};

	/** @brief The engine sampler for a slot's address and filter modes. kInvalid when the graph has none. */
	std::uint32_t SlotSampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode, Owners& a_owners);

	/** @brief What a header register (a resource CS binds for its features' includes) holds for a batch. */
	struct Header
	{
		std::string publishedId;                  // a published graph texture or buffer: read by its index (PublishedInputs::Resolve)
		std::uint32_t index = GpuTextures::kInvalid;  // otherwise its heap index: a D3D11 view's import, or the kind's null view
	};

	enum class HeaderResult : std::uint8_t
	{
		Ok,
		Buffer,   // a D3D11 buffer view the graph cannot import (its feature's ORG path publishes it)
		Texture,  // a D3D11 texture view that could not be imported (a_failure says why), or no null view
	};

	/**
	 * @brief The one header resolver: PS t<a_register> as CS binds it for the batch. A published resource's export resolves to its id
	 * first; an empty register to the null view of a_kind; any other view to its import (a_tag, GpuTextures::Source::FamilyHeader + the
	 * header's index). Imports go into a_owners. a_view, when given, receives the bound view (not kept) for a log.
	 *
	 * The headers are CS's registers (t16 and up; the engine binds t0-t15 only), which CS binds on DXVK's own context
	 * (globals::d3d::context), never through the engine's guest context. The flush latch drops only the guest's binds, so this read stays
	 * true under it (docs/development/p5/primary-path.md, "D3D11 state reads"). A register below 16 is refused.
	 */
	HeaderResult ResolveHeader(std::uint32_t a_register, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, Header& a_out, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure = nullptr, ID3D11ShaderResourceView** a_view = nullptr);

	/**
	 * @brief Leases and imports kept for a few frames: a run's (the GPU may be that far behind) and a batch's that ran nothing (its
	 * imports are found again by the next batch instead of imported again).
	 */
	class KeptOwners
	{
	public:
		/** @brief Keeps a_owners until kKeptFrames game frames have passed. */
		void Keep(Owners&& a_owners);

	private:
		std::deque<std::pair<std::uint32_t, Owners>> kept;  // (game frame, owners)
	};

	/** @brief Game frames a run's owners are kept: the frames the graph may run behind, with a margin. */
	std::uint32_t KeptFrames();

	/**
	 * @brief Runs a_anchor for a client whose latched draws are ready (CanRun said it would draw): a_pending, which the client's active
	 * callback returns, is set for the run. True when the run was submitted (RunStatus::Ran). Any other status is an error: the latched
	 * draws were not drawn this frame, logged once per status as an error (with a_family and a_client). Nothing is retried. The client's
	 * executed callback counts the runs that drew.
	 */
	bool RunWithheld(std::string_view a_family, std::string_view a_client, FrameAnchors::Anchor a_anchor, bool& a_pending);
}
