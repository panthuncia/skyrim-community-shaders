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
	class VersionedTexture;
}

namespace GraphResources
{
	class Resource;
}

struct ComputeProgram;

namespace RenderGraph
{
	class RunGate;
}

/**
* @brief Common passes for frame anchor clients, made from plain descriptions: fullscreen draw, dispatch, copy, UAV clear and upload.
*
* A port lists its steps instead of writing a pass class for each. Each pass is made from an immutable spec at its anchor's build,
* and its revision is that spec, so its recording is kept. Per-frame values arrive through the client's latch and run gate.
* Use a Builder in the client's Setup and register ClientDesc{ .passes = builder.Take() } with its shared constants and gate.
* Include <rhi_interop_vulkan.h> before this header.
*/
namespace RenderGraph::AnchorPasses
{
	/** @brief A Bind or target that covers every mip of its texture (the texture's default view). */
	inline constexpr std::uint32_t kAllMips = UINT32_MAX;

	/** @brief One entry of a pass's push block, in the shader's resource list order (ShaderBindings.h).
	*  Srv and Uav declare a texture or one mip. Published reads by id. Handle, Latch and Shared pass descriptor indices or offsets.
	*  Table passes the pass's handle table (Dispatch::table, Draw::table) as a LATCH entry: its buffer's index, then offset 0. */
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
			Table,
		};

		Kind kind = Kind::Handle;
		std::shared_ptr<org::Resource> resource;
		std::uint32_t value = 0;             // Handle: the descriptor index; Latch: the region's offset
		std::uint32_t mip = kAllMips;        // Srv and Uav: the one mip the view covers, or the default view
		std::string id;                      // Published: the id (PublishedResources::IsDeclarable at the build)
		// Srv and Published: the view variant (org::SRVViewType); Uav: org::UAVViewType. UINT32_MAX for the default view.
		std::uint32_t variant = UINT32_MAX;
		// A versioned twin's slot, declared in place of resource, so a resize is a new version.
		std::shared_ptr<org::VersionedTexture> slot;

		static Bind Srv(const FrameAnchors::Texture& a_texture, std::uint32_t a_mip = kAllMips) { return Of(Kind::Srv, a_texture, a_mip); }
		/** @brief An Srv of a texture array's whole-array view (Texture2DArrayFull), for a shader that declares Texture2DArray. */
		static Bind SrvArray(const FrameAnchors::Texture& a_texture);
		static Bind Uav(const FrameAnchors::Texture& a_texture, std::uint32_t a_mip = kAllMips) { return Of(Kind::Uav, a_texture, a_mip); }
		/** @brief A Uav of a texture array's or cube's whole-array view (Texture2DArrayFull), for a shader that declares RWTexture2DArray. */
		static Bind UavArray(const FrameAnchors::Texture& a_texture, std::uint32_t a_mip = kAllMips);
		/** @brief An Srv of a cube texture's cube view (TextureCube), for a shader that declares TextureCube. */
		static Bind SrvCube(const FrameAnchors::Texture& a_texture);
		/** @brief An Srv of another client's published resource, declared by its id: the graph orders the read after its producer. */
		static Bind Published(std::string_view a_id, std::uint32_t a_variant = UINT32_MAX)
		{
			return { Kind::Published, nullptr, 0, kAllMips, std::string(a_id), a_variant };
		}
		/** @brief An Srv or Uav of a graph twin, its slot when versioned, else its graph resource. Versioned twins never use Graph(). */
		static Bind Srv(const GraphResources::Resource& a_twin, std::uint32_t a_mip = kAllMips);
		static Bind Uav(const GraphResources::Resource& a_twin, std::uint32_t a_mip = kAllMips);
		static Bind Handle(std::uint32_t a_index) { return { Kind::Handle, nullptr, a_index }; }
		static Bind Unused() { return Handle(0); }
		static Bind Latch(std::uint32_t a_offset) { return { Kind::Latch, nullptr, a_offset }; }
		static Bind Shared() { return { Kind::Shared, nullptr, 0 }; }
		/** @brief The pass's handle table, as a LATCH entry (two words). */
		static Bind Table() { return { Kind::Table, nullptr, 0 }; }

	private:
		/** @brief An Srv or Uav of a handle: its slot, or its plain resource. */
		static Bind Of(Kind a_kind, const FrameAnchors::Texture& a_texture, std::uint32_t a_mip)
		{
			Bind bind{ a_kind, a_texture.Plain(), 0, a_mip };
			bind.slot = a_texture.Slot();
			return bind;
		}
	};

	/** @brief A fullscreen raster pass into one target or mip, with an optional depth-stencil attachment, scissor and viewport. */
	struct Draw
	{
		std::string name;  // the step: the pass is "<prefix>.<name>"
		std::shared_ptr<const RasterProgram> program;
		std::vector<Bind> bindings;
		FrameAnchors::Texture target;  // an import's slot, or any graph resource
		std::uint32_t mip = kAllMips;
		// SV_Target1 and up (whole textures, the same load op): for a program with several targets.
		std::vector<FrameAnchors::Texture> extraTargets;
		RasterProgram::Viewport viewport;  // the render area (required): the target's (or the mip's) size, or a part of it
		rhi::LoadOp load = rhi::LoadOp::Load;
		std::array<float, 4> clear{};  // with LoadOp::Clear
		std::optional<std::uint32_t> gate;  // the builder's run gate entry: the draw happens only on runs whose commit opened it
		// The depth-stencil attachment, for a program with a depth format. Loaded and stored.
		// Read-only unless the program's depth state writes depth or stencil.
		FrameAnchors::Texture depth;
		bool depthReadOnly = true;
		std::uint32_t stencilReference = 0;  // the stencil test's and REPLACE's reference (RasterProgram::DrawDesc)
		// The draw's own viewport and scissor inside the render area. The viewport may reach past the target, as the engine's would.
		std::optional<rhi::Viewport> dynamicViewport;
		std::optional<rhi::ScissorRect> scissor;
		// Ungated draws: the vertex and instance counts (a gated draw takes them from RunGate::OpenDraw).
		std::uint32_t vertexCount = 3;
		std::uint32_t instanceCount = 1;
		// The handle table, read through the Bind::Table entry of bindings (see Dispatch::table).
		std::vector<Bind> table;
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
		// The handle table, for a shader that reads more resources than the push block holds: one uint per entry, in the order
		// of the shader's table struct. Srv, Uav and Published entries are declared as bindings are and hold their views'
		// indices; Handle entries hold their value. The pass writes the indices into a buffer of its own when it is recorded,
		// so the table is fixed for the recording and any replay of it. The bindings pass that buffer as their Bind::Table entry.
		std::vector<Bind> table;
	};

	/** @brief A copy of a texture's first mip into another of the same size and format: depth slices of a 3D texture and
	*  array slices as the copy names them. With a gate entry it copies only on runs whose commit opened it, and is recorded at execution. */
	struct Copy
	{
		std::string name;
		FrameAnchors::Texture source, target;
		std::uint32_t width = 0, height = 0;
		std::optional<std::uint32_t> gate;  // the builder's run gate entry
		std::uint32_t depth = 1;            // a 3D texture's depth slices
		std::uint32_t slices = 1;           // an array's or cube's slices (six for a cube), each copied in turn
	};

	/** @brief A clear of a whole texture through its UAV (UnorderedAccessClear) to a float or uint value. */
	struct Clear
	{
		std::string name;
		FrameAnchors::Texture target;
		std::array<float, 4> value{};
		std::optional<std::array<std::uint32_t, 4>> uintValue;  // set: a uint clear instead (integer formats)
	};

	/** @brief A graph twin as a handle, its slot when versioned, else its graph resource. */
	FrameAnchors::Texture TextureOf(const GraphResources::Resource& a_twin);

	/** @brief A Clear of a graph twin, declared as its slot when versioned. The caller sets the value. */
	Clear ClearOf(std::string a_name, const GraphResources::Resource& a_twin);

	/** @brief A Copy between two graph twins, each declared as its slot when versioned. The size is baked in, so a resize needs a new registration. */
	Copy CopyOf(std::string a_name, const GraphResources::Resource& a_source, const GraphResources::Resource& a_target, std::uint32_t a_width,
		std::uint32_t a_height);

	/** @brief A copy from a host-written staging buffer into one texture subresource. The pass keeps the buffer alive.
	*  It is not a graph resource, so write it once before registering and never while a run may read it. */
	struct Upload
	{
		std::string name;
		std::shared_ptr<org::Buffer> staging;
		FrameAnchors::Texture target;
		rhi::CopyableFootprint footprint{};
		std::uint32_t mip = 0, slice = 0;
	};

	/** @brief Collects one client's passes for one anchor, named "<prefix>.<step>" with rising order keys.
	*  A step that cannot be made fails the builder. Take returns what was added. */
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

		/** @brief A custom pass, made at each graph build of the anchor. a_atExecution records it after the run's commit. */
		void Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make, bool a_once = false,
			bool a_atExecution = false);

		/** @brief Marks the builder failed with a reason (the first one is kept). */
		void Fail(std::string_view a_reason);

		bool Failed() const { return !failure.empty(); }
		const std::string& Failure() const { return failure; }

		/** @brief Whether a pass reads CS's shared constants (Bind::Shared): the client then sets ClientDesc::sharedConstants. */
		bool ReadsSharedConstants() const { return sharedConstants; }

		/** @brief The passes added, for ClientDesc::passes. */
		std::vector<FrameAnchors::PassDesc> Take() { return std::move(passes); }

	private:
		bool Check(std::string_view a_name, const std::vector<Bind>& a_bindings, std::uint32_t a_programWords, std::optional<std::uint32_t> a_gate,
			const std::vector<Bind>& a_table = {});
		void Push(std::string_view a_step, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make, bool a_once,
			bool a_atExecution = false);

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
