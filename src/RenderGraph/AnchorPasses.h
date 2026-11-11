#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"

#	include <array>
#	include <cstdint>
#	include <functional>
#	include <memory>
#	include <optional>
#	include <string>
#	include <string_view>
#	include <utility>
#	include <vector>

namespace org
{
	class Buffer;
	class LatchBlock;
	class RenderPass;
	class Resource;
}

struct ComputeProgram;

namespace RenderGraph
{
	class RunGate;
}

/**
 * @brief The common passes of a frame anchor client, made from plain descriptions: a fullscreen draw, a compute dispatch, a whole
 * texture copy, a UAV clear and a buffer-to-texture upload. Generalised from Post Processing's chain builder (ORGPostEffect.h), with
 * the anchor, the pass names and the order keys as parameters, so a port lists its steps instead of writing a pass class for each.
 *
 * Each pass is made from an immutable spec when its anchor's graph is built; its revision is that spec, so its recording is kept.
 * Per-frame values reach a pass through the client's latch (Bind::Latch) and its run gate (Draw::gate, Dispatch::gate): the commit
 * writes them, the recording stays the same.
 *
 * Use: a client's Setup makes a Builder, adds its steps, and registers ClientDesc{ .passes = builder.Take(), ... } with
 * .sharedConstants = builder.ReadsSharedConstants() and its gate in .gates. Render thread, at setup or a rebuild.
 *
 * Translation units that include this header include <rhi_interop_vulkan.h> first (volk before any Vulkan header).
 */
namespace RenderGraph::AnchorPasses
{
	/** @brief A Bind or target that covers every mip of its texture (the texture's default view). */
	inline constexpr std::uint32_t kAllMips = UINT32_MAX;

	/**
	 * @brief One entry of a pass's push block, in the shader's resource list order (ShaderBindings.h): Srv and Uav are declared
	 * resources (the whole texture or one mip), Published a read of a published resource by its id (PublishedResources.h: the
	 * default view, ordered after its producer's writes in any epoch), Handle a sampler or another descriptor index, Latch a LATCH
	 * entry's two words (an offset into the builder's latch), Shared a SHARED entry's four words (SharedConstants.h).
	 */
	struct Bind
	{
		enum class Kind : std::uint8_t
		{
			Srv,
			Uav,
			Published,
			Handle,
			Latch,
			Shared,
		};

		Kind kind = Kind::Handle;
		std::shared_ptr<org::Resource> resource;
		std::uint32_t value = 0;       // Handle: the descriptor index; Latch: the region's offset
		std::uint32_t mip = kAllMips;  // Srv and Uav: the one mip the view covers, or the default view
		std::string id;                // Published: the id (PublishedResources::IsDeclarable at the build)

		static Bind Srv(std::shared_ptr<org::Resource> a_resource, std::uint32_t a_mip = kAllMips) { return { Kind::Srv, std::move(a_resource), 0, a_mip }; }
		static Bind Uav(std::shared_ptr<org::Resource> a_resource, std::uint32_t a_mip = kAllMips) { return { Kind::Uav, std::move(a_resource), 0, a_mip }; }
		static Bind Published(std::string_view a_id) { return { Kind::Published, nullptr, 0, kAllMips, std::string(a_id) }; }
		static Bind Handle(std::uint32_t a_index) { return { Kind::Handle, nullptr, a_index }; }
		static Bind Unused() { return Handle(0); }
		static Bind Latch(std::uint32_t a_offset) { return { Kind::Latch, nullptr, a_offset }; }
		static Bind Shared() { return { Kind::Shared, nullptr, 0 }; }
	};

	/** @brief A fullscreen raster pass: a_program's pixel stage over the viewport, into one target (or one mip of it). */
	struct Draw
	{
		std::string name;  // the step: the pass is "<prefix>.<name>"
		std::shared_ptr<const RasterProgram> program;
		std::vector<Bind> bindings;
		std::shared_ptr<org::Resource> target;
		std::uint32_t mip = kAllMips;
		// SV_Target1 and up (whole textures, the same load op): for a program with several targets.
		std::vector<std::shared_ptr<org::Resource>> extraTargets;
		RasterProgram::Viewport viewport;  // the render area (required): the target's (or the mip's) size, or a part of it
		rhi::LoadOp load = rhi::LoadOp::Load;
		std::array<float, 4> clear{};  // with LoadOp::Clear
		std::optional<std::uint32_t> gate;  // the builder's run gate entry: the draw happens only on runs whose commit opened it
	};

	/** @brief A compute pass: fixed group counts, a run gate entry, or group counts at a latch offset the commit writes. */
	struct Dispatch
	{
		std::string name;
		std::shared_ptr<const ComputeProgram> program;
		std::vector<Bind> bindings;
		std::array<std::uint32_t, 3> groups{ 1, 1, 1 };
		std::optional<std::uint32_t> gate;       // the builder's run gate entry (its counts replace groups)
		std::optional<std::uint32_t> latchArgs;  // three uints at this offset of the builder's latch (replace groups)
	};

	/** @brief A copy of a whole texture's first mip into another of the same size and format. */
	struct Copy
	{
		std::string name;
		std::shared_ptr<org::Resource> source, target;
		std::uint32_t width = 0, height = 0;
	};

	/** @brief A clear of a whole texture through its UAV (UnorderedAccessClear) to a float or uint value. */
	struct Clear
	{
		std::string name;
		std::shared_ptr<org::Resource> target;
		std::array<float, 4> value{};
		std::optional<std::array<std::uint32_t, 4>> uintValue;  // set: a uint clear instead (integer formats)
	};

	/**
	 * @brief A copy from a host-written staging buffer into one subresource of a texture, laid out by a_footprint (rows at the
	 * copy's pitch). The staging buffer is the pass's to keep alive; it is not a graph resource, so the client writes it once, before
	 * registering, and never while a run with the pass may read it.
	 */
	struct Upload
	{
		std::string name;
		std::shared_ptr<org::Buffer> staging;
		std::shared_ptr<org::Resource> target;
		rhi::CopyableFootprint footprint{};
		std::uint32_t mip = 0, slice = 0;
	};

	/**
	 * @brief Collects one client's passes for one anchor: names "<prefix>.<step>", order keys from a_orderBase upward in the order
	 * the steps are added. The latch (Bind::Latch, Dispatch::latchArgs) and the run gate (Draw::gate, Dispatch::gate) are the
	 * client's; either may be null when no step uses it. A step that cannot be made fails the builder (logged by the caller through
	 * Failure); Take returns what was added.
	 */
	class Builder
	{
	public:
		Builder(FrameAnchors::Anchor a_anchor, std::string a_prefix, std::int32_t a_orderBase, std::shared_ptr<const org::LatchBlock> a_latch = {},
			std::shared_ptr<const RunGate> a_gate = {});

		void Add(Draw a_draw);
		void Add(Dispatch a_dispatch);
		void Add(Copy a_copy);

		/** @brief A clear; a_once (the default) makes it a one-shot pass (PassDesc::once), run again by FrameAnchors::Rearm. */
		void Add(Clear a_clear, bool a_once = true);

		/** @brief An upload, always a one-shot pass: it runs at the anchor's next run, and again after FrameAnchors::Rearm. */
		void Add(Upload a_upload);

		/** @brief A custom pass: make is called at each graph build of the anchor, as FrameAnchors::PassDesc::make. */
		void Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make, bool a_once = false);

		/** @brief Marks the builder failed with a reason (the first one is kept). */
		void Fail(std::string_view a_reason);

		bool Failed() const { return !failure.empty(); }
		const std::string& Failure() const { return failure; }

		/** @brief Whether a pass reads CS's shared constants (Bind::Shared): the client then sets ClientDesc::sharedConstants. */
		bool ReadsSharedConstants() const { return sharedConstants; }

		/** @brief The passes added, for ClientDesc::passes. */
		std::vector<FrameAnchors::PassDesc> Take() { return std::move(passes); }

	private:
		bool Check(std::string_view a_name, const std::vector<Bind>& a_bindings, std::uint32_t a_programWords, std::optional<std::uint32_t> a_gate);
		void Push(std::string_view a_step, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make, bool a_once);

		FrameAnchors::Anchor anchor;
		std::string prefix;
		std::int32_t orderBase = 0;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RunGate> gate;
		std::vector<FrameAnchors::PassDesc> passes;
		std::string failure;
		bool sharedConstants = false;
	};

	/** @brief Defines as programs take them (ComputeProgram::Desc, RasterProgram::Desc). Defines are ASCII. */
	std::vector<std::pair<std::wstring, std::wstring>> Defines(const std::vector<std::pair<std::string, std::string>>& a_defines);
}

#endif
