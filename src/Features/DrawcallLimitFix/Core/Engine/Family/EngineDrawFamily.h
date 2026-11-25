#pragma once

// What every DCLF family that draws the engine's own draws on the graph shares: the DrawTriShape detour they latch through, the
// SetupGeometry watch, and the reads of a latched draw's geometry, constants, textures and headers.
// A batch is withheld when its anchor can run. A draw the family cannot take ends the latched part of the batch, so the latched
// draws run there and the engine draws the rest, in order.
// Render thread unless a function says otherwise.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "RenderGraph/BufferRef.h"

#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/FrameAnchors.h"
#include "Utils/Logging.h"

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
		// BSGraphics::Renderer::DrawTriShape(renderer, TriShape*, startIndex, triangleCount).
		inline constexpr Engine::Offset kDrawTriShape{ { 77263 }, { 75477 } };
		// DrawDynamicTriShape's DrawData copy, which particles use among others. SE 75488 calls 75489. AE 77275 (0x140e46f20) draws
		// itself: stream 0 the static buffer, stream 1 the dynamic ring at the data's offset, 16-bit indices.
		inline constexpr Engine::Offset kDrawDynamicDrawData{ { 77275 }, { 75488 } };
		// The engine's dynamic ring: its three buffers and the current one.
		inline constexpr Engine::Offset kDynamicRing{ { 411350 }, { 524731 } };
		inline constexpr Engine::Offset kDynamicRingCurrent{ { 411351 }, { 524732 } };
	}

	/** @brief BSGraphics::TriShape's head: the static vertex and index buffers and the VertexDesc. */
	struct TriShape
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		std::uint64_t vertexDesc;
	};

	/** @brief A family's DrawTriShape taker. Returns true when it latched the draw, so the engine skips it. */
	using DrawTaker = bool (*)(const TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles);

	/** @brief The data DrawDynamicTriShape's copy draws (AE 0x140e46f20): TriShape's head, then stream 1's offset in the dynamic ring. */
	struct DynamicDrawData
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		std::uint64_t vertexDesc;
		std::uint32_t streamOffset;
	};

	/** @brief A DrawData copy taker, as DrawTaker. The detour has set the VertexDesc and topology before the takers run. */
	using DynamicDrawTaker = bool (*)(const DynamicDrawData* a_data, std::uint32_t a_start, std::uint32_t a_triangles);

	/** @brief AddDrawTaker for the DrawData copy's detour, which the first call installs. */
	bool AddDynamicDrawTaker(std::string_view a_family, DynamicDrawTaker a_taker);

	/**
	 * @brief Adds a taker to the DrawTriShape detour, which the first call installs. Takers are asked in order and the first that
	 * takes the draw ends it. Install time, main thread. False when this runtime has no address for DrawTriShape.
	 */
	bool AddDrawTaker(std::string_view a_family, DrawTaker a_taker);

	/** @brief Whether the DrawTriShape detour is installed. */
	bool DrawTakersInstalled();

	/** @brief A family's SetupGeometry watcher, run after the shader's own SetupGeometry. */
	using SetupWatcher = void (*)(RE::BSRenderPass* a_pass);

	namespace Detail
	{
		/** @brief The most watchers one shader type takes. */
		inline constexpr std::size_t kSetupWatchers = 4;

		template <RE::BSShader::Type kType>
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				func(a_this, a_pass, a_flags);
				for (const auto watcher : watchers) {
					if (!watcher)
						break;
					watcher(a_pass);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
			static inline std::array<SetupWatcher, kSetupWatchers> watchers{};
		};
	}

	/**
	 * @brief Watches a shader type's SetupGeometry (vtable slot 6), where the draw's constants are written. Watchers run in order.
	 * Install time, main thread. False when the type has no room left.
	 */
	template <RE::BSShader::Type kType>
	bool WatchSetupGeometry(REL::VariantID a_vtable, SetupWatcher a_watcher)
	{
		using Thunk = Detail::SetupGeometry<kType>;
		for (auto& watcher : Thunk::watchers) {
			if (watcher == a_watcher)
				return true;
			if (watcher)
				continue;
			const bool first = &watcher == Thunk::watchers.data();
			watcher = a_watcher;
			if (first)
				stl::write_vfunc<0x6, Thunk>(a_vtable);
			return true;
		}
		logger::warn("[DCLF] engine draw families: shader type {} has no room for another SetupGeometry watcher", static_cast<std::uint32_t>(kType));
		return false;
	}

	/**
	 * @brief Adds a family's setup, which registers its clients. It runs at CS's setup, before the setup's graph build, so clients are
	 * in their anchors' graphs before any world frame. A setup that returns false is retried on the main thread after every frame-job
	 * wait until it succeeds. Install time, main thread.
	 */
	bool AddSetup(std::string_view a_family, std::function<bool()> a_setup);

	/** @brief Main thread, after every feature's SetupResources. Runs every family's setup once. */
	void RunSetups();

	/**
	 * @brief Once a game frame, from the first family batch. Lets the render thread's buffer registry drop dead entries and take
	 * DXVK's buffer moves, whether or not grass and tree LOD run.
	 */
	void BeginBatch();

	/** @brief Any thread. The engine's frame counter, incremented by Renderer::End. */
	std::uint32_t GameFrame();

	/**
	 * @brief A batch's passes against its draws. A pass that drew through another function than DrawTriShape was the engine's,
	 * at its place in the batch, ahead of the graph's draws.
	 */
	struct Passes
	{
		static constexpr std::uint32_t kNone = 0xFF;

		std::uint32_t passes = 0;
		std::uint32_t drawn = 0;
		std::uint32_t pendingType = kNone;  // the last pass's geometry type while its draw has not reached DrawTriShape
		std::uint32_t otherTypes = 0;  // a bit each
		// Set when the family also takes DrawData copy draws, so a particles pass is no shortfall.
		bool dynamicDraws = false;

		void Begin() { *this = {}; }
		/**
		 * @brief A pass set up. False when it will not draw through DrawTriShape, so the family ends its latched part before the engine draws it.
		 */
		bool OnSetup(const RE::BSRenderPass* a_pass);
		void OnDraw()
		{
			++drawn;
			pendingType = kNone;
		}
		/** @brief After the batch. True when every pass drew through DrawTriShape. */
		bool End();
		/** @brief The shortfall in words, for a log. */
		std::string Describe(std::string_view a_what) const;
	};

	/** @brief Whether the engine's geometry-type dispatcher draws a_pass through DrawTriShape. */
	bool DrawsThroughTriShape(const RE::BSRenderPass* a_pass);

	/** @brief A latched draw's geometry, read through its buffer view refs while its leases are held. */
	struct Geometry
	{
		RenderGraph::BufferRef vertexRef, indexRef;
		std::uint64_t layout = 0;  // the vertex shader's mask and the geometry's VertexDesc
		std::uint32_t stride = 0, firstIndex = 0, indexCount = 0;
		// A dynamic draw's stream 1. Null for a static draw.
		RenderGraph::BufferRef streamRef;
		std::uint32_t streamStride = 0;
	};

	enum class GeometryResult : std::uint8_t
	{
		Ok,
		Missing,
		Topology,
		Buffers,
		Bounds,  // the index range is past the index buffer, or a vertex is smaller than its position
	};

	const char* ReasonOf(GeometryResult a_result);

	/**
	 * @brief The draw's geometry with both buffers leased into a_owners. Checks the 16-bit index range against the index buffer, and
	 * the stride against the R32G32B32A32 position the vertex stage pulls. a_detail gets a refusal's numbers for the log.
	 */
	GeometryResult LatchGeometry(const TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles, std::uint64_t a_shaderVertexDesc,
		std::uint32_t a_topology, Geometry& a_out, Owners& a_owners, std::string* a_detail = nullptr);

	/** @brief LatchGeometry for a DrawData copy. Call at the draw, while the ring's current buffer is the one the engine would bind. */
	GeometryResult LatchDynamicGeometry(const DynamicDrawData* a_data, std::uint32_t a_start, std::uint32_t a_triangles, std::uint64_t a_shaderVertexDesc,
		std::uint32_t a_topology, Geometry& a_out, Owners& a_owners, std::string* a_detail = nullptr);

	/** @brief The viewport SetDirtyStates would set, and its depth range. */
	struct Viewport
	{
		std::array<std::uint32_t, 4> rect{};
		std::array<float, 2> depthRange{ 0.0f, 1.0f };
	};

	/**
	 * @brief The shadow state's viewport, with the depth bias mode's offset taken off its maximum depth
	 * (CreateDepthBiasViewportOffsets' table, SE 0x141794530, AE 0x1419db270).
	 */
	Viewport LatchViewport(std::uint32_t a_depthBias);

	namespace Constants = ::RenderGraph::Engine::Constants;
	namespace Flush = ::RenderGraph::Engine::Flush;

	/**
	 * @brief The constant buffers bound for the draw about to be latched, as the engine constants capture saw them.
	 * Invalid unless CS_ORG_ENGINE_CONSTANTS=1, which the families require.
	 */
	Constants::UseId CaptureConstants();

	/** @brief The bytes of a_slot of a_stage the use bound, 0 if nothing. */
	std::uint32_t ConstantBytes(Constants::UseId a_use, Constants::Stage a_stage, std::uint32_t a_slot);

	/** @brief Copies a slot's constants into a_out, zero-filled past them. True for nothing bound, false for an invalid use. */
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
		Published,  // only a declared read orders it after its writes
		Failed,
	};

	/** @brief A draw's texture slot: a_view's import, or the null view of the slot's kind. a_view is unwrapped first. */
	SlotResult SlotTexture(ID3D11ShaderResourceView* a_view, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, std::uint32_t& a_index, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure = nullptr);

	/** @brief A set of published ids. */
	using PublishedIds = std::set<std::string, std::less<>>;

	/** @brief The id an engine target is published under, such as "Engine::kSSR". */
	std::string EngineTargetId(RE::RENDER_TARGET a_target);
	/** @brief An engine depth target's id. */
	std::string EngineTargetId(RE::RENDER_TARGET_DEPTHSTENCIL a_target);
	/** @brief An engine cubemap target's id. */
	std::string EngineTargetId(RE::RENDER_TARGET_CUBEMAP a_target);

	/** @brief Any thread, after install. The published ids DCLF's registry lists at a_registers. */
	std::vector<std::string> RegisteredIdsAt(std::span<const std::uint32_t> a_registers);

	/** @brief How a client reads a published export bound at a slot or a header. */
	enum class InputResult : std::uint8_t
	{
		Graph,  // by the id's bindless index, the client's graph declares the id
		NotListed,  // not among the client's inputs, so the family's list should name it
		PublishedLate,  // first published after the client's graph was built, an error
		NoView,  // no whole-resource view
	};

	/** @brief InputResult in words, for a log. */
	const char* ReasonOf(InputResult a_result);

	/**
	 * @brief Why the engine drew work a family should have taken (cleanup plan A3, step 6). Each reason is logged once. In graph
	 * mode CS_ORG_ENGINE_DRAW_TAP=1 makes a report a tap error (TapError::Refused); a texture a later batch takes is never one.
	 */
	class RefusalLog
	{
	public:
		explicit RefusalLog(std::string a_prefix) :
			log(std::move(a_prefix), spdlog::level::warn) {}

		/** @brief Render thread. Reports a_reason, with a_detail when given. */
		void Report(std::string_view a_reason, std::string_view a_detail = {});

	private:
		Util::ReasonLog log;
	};

	/**
	 * @brief The published ids a family client's draws can read, fixed when the client is made at setup.
	 * Each graph build declares a read of every listed id that is registered, for ordering. The set only grows.
	 * An id first published during play is an error until the graphs rebuild, and that batch goes to the engine.
	 */
	class PublishedInputs
	{
	public:
		/** @brief Lists a_id once. Call before the client registers. */
		void Add(std::string_view a_id);
		/** @brief Lists each of a_ids. */
		void Add(std::span<const std::string> a_ids);
		/** @brief Render thread, at setup. Lists an engine target and imports it now, which publishes it. */
		void AddEngine(RE::RENDER_TARGET a_target);
		void AddEngine(RE::RENDER_TARGET_DEPTHSTENCIL a_target);
		void AddEngine(RE::RENDER_TARGET_CUBEMAP a_target);
		/**
		 * @brief Render thread, at setup. Lists a target's copy texture and publishes it when ORG owns the target.
		 * The engine binds the copy where a pass reads the target it draws into.
		 */
		void AddEngineCopy(RE::RENDER_TARGET a_target);

		/** @brief Graph build. Declares a read of each registered listed id and returns them for Prepare to check. */
		Extensions::DeclaredReads Declare(org::RenderPassBuilder& a_builder);

		/** @brief Render thread. How the client reads a_id. On Graph, a_index gets the id's bindless index. */
		InputResult Resolve(std::string_view a_id, std::uint32_t& a_index) const;

		/** @brief The listed ids, for the log. */
		std::string Describe() const;

		/** @brief The listed ids no graph registers yet, for the log. */
		std::string Unregistered() const;

	private:
		std::vector<std::string> ids;
		// Replaced whole and only grows. Atomic because a graph may be built on the host's owner thread.
		std::atomic<std::shared_ptr<const PublishedIds>> declared;
		std::atomic<std::uint64_t> declaredSerial{ 0 };  // bumped with each replacement of declared
		// Resolve's answers, per listed id, for one frame and one declared set: draws of a frame ask the same ids again and again,
		// and an id's index changes only when a version is adopted, at a frame's Begin. Render thread.
		struct Answer
		{
			bool asked = false;
			InputResult result = InputResult::NotListed;
			std::uint32_t index = 0;
		};
		mutable std::vector<Answer> answers;
		// Each id's position in ids, made again when ids grew. Render thread.
		mutable std::unordered_map<std::string_view, std::uint32_t> positions;
		mutable std::size_t positionsOf = 0;
		mutable std::uint64_t answersFrame = ~0ull;
		mutable std::uint64_t answersSerial = ~0ull;
	};

	/** @brief The engine sampler for a slot's address and filter modes, or kInvalid. */
	std::uint32_t SlotSampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode, Owners& a_owners);

	/** @brief What a header register holds for a batch. */
	struct Header
	{
		std::string publishedId;  // read by its index
		std::uint32_t index = GpuTextures::kInvalid;  // otherwise a D3D11 view's import or the null view
	};

	enum class HeaderResult : std::uint8_t
	{
		Ok,
		Buffer,  // a D3D11 buffer view the graph cannot import
		Texture,  // could not be imported, or no null view
	};

	/**
	 * @brief Resolves PS t<a_register> as CS binds it for the batch. Registers below 16 are refused.
	 * CS binds these on DXVK's own context, not the engine's guest context, which keeps only the engine's binds.
	 */
	HeaderResult ResolveHeader(std::uint32_t a_register, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, Header& a_out, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure = nullptr, ID3D11ShaderResourceView** a_view = nullptr);

	/** @brief Resolves a_view as a header would read it: a published id, an import, or the null view of a_kind when a_view is null. */
	HeaderResult ResolveHeaderView(ID3D11ShaderResourceView* a_view, GpuTextures::ViewKind a_kind, std::uint32_t a_tag, Header& a_out, Owners& a_owners,
		GpuTextures::ResolveFailure* a_failure = nullptr);

	/** @brief Keeps leases and imports for a few frames, since the GPU may be that far behind. */
	class KeptOwners
	{
	public:
		/** @brief Keeps a_owners until kKeptFrames game frames have passed. */
		void Keep(Owners&& a_owners);

	private:
		std::deque<std::pair<std::uint32_t, Owners>> kept;  // (game frame, owners)
	};

	/** @brief Game frames a run's owners are kept: how far the graph may run behind, plus a margin. */
	std::uint32_t KeptFrames();

	/**
	 * @brief Runs a_anchor for a client whose latched draws are ready. True when the run was submitted.
	 * Any other status is an error, logged once per status, and nothing is retried.
	 */
	bool RunWithheld(std::string_view a_family, std::string_view a_client, FrameAnchors::Anchor a_anchor, bool& a_pending);

	/**
	 * @brief Whether the graph draws a client's batch now. A batch while no open stage holds the client is a tap error.
	 * One the stage's schedule does not hold yet lags, and nothing is drawn.
	 */
	bool HeldForBatch(std::string_view a_family, std::string_view a_client, std::uint32_t a_segment, FrameAnchors::Anchor a_anchor = FrameAnchors::Anchor::EngineDraws);
}
