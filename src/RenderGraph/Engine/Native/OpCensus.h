#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

/**
 * @brief Native mode's census of the immediate context's work (native frame graph plan, Phase 0). Off by default; two switches:
 *
 * - CS_ORG_NATIVE_OP_CENSUS=1: every Map, Unmap upload, UpdateSubresource, copy, clear and GenerateMips the engine or CS makes on the
 *   native immediate context, counted per frame by kind and by target resource (its name, D3D11 bind flags, usage and dimension). Each
 *   operation also records where in the frame it fell: after which graph epoch (an anchor run or a Drawcall Limit Fix segment, with
 *   its run number in the frame). That is the order a single frame graph must keep.
 * - CS_ORG_NATIVE_WRITE_CENSUS=1: the textures made with shader-resource binding only and the buffers made with vertex or index
 *   binding only that are written after creation (an update, a Map for writing, a copy into them, a clear), with their names and
 *   counts. Such a resource cannot be exempt from tracking as an immutable one.
 *
 * Both report per window (CensusWindow), every CS_ORG_CENSUS_FRAMES frames in it and when the game leaves it.
 *
 * When both are off, each operation costs one relaxed load (Flags()).
 */
namespace RenderGraph::Engine::Native::OpCensus
{
	/** @brief An immediate-context operation. The *Source kinds are the read side of a copy. */
	enum class Op : std::uint8_t
	{
		MapRead,
		MapWrite,
		MapWriteDiscard,
		MapWriteNoOverwrite,
		MapReadWrite,
		UnmapUpload,  // a DYNAMIC texture's Unmap: its upload side is copied into the image
		UpdateBuffer,
		UpdateTexture,
		Copy,
		CopySource,
		CopyRegion,
		CopyRegionSource,
		ClearRenderTarget,
		ClearDepthStencil,
		ClearUavUint,
		ClearUavFloat,
		GenerateMips,
		Count
	};

	/** @brief What the census knows of an operation's resource, from its native record. */
	struct Target
	{
		std::uint64_t id = 0;         // the record's serial (unique for the session)
		std::string_view name;        // the handle's debug name, else the record's own
		std::uint32_t bindFlags = 0;  // D3D11_BIND_*
		std::uint32_t usage = 0;      // D3D11_USAGE
		std::uint32_t dimension = 0;  // D3D11_RESOURCE_DIMENSION (BUFFER for a buffer)
		bool initialData = false;     // made with initial data
	};

	enum Flag : std::uint8_t
	{
		kOps = 1,
		kWrites = 2,
		kMarkWrites = 4,  // CS_ORG_VALIDATE_UNDECLARED=1: writes only mark their resource (Native::ResourceFacts::written)
	};

	namespace Detail
	{
		inline std::atomic<std::uint8_t> flags{ 0 };
	}

	/** @brief The switches as Configure read them (Flag bits); 0 before Configure or with all off. Any thread, one load. */
	inline std::uint8_t Flags() { return Detail::flags.load(std::memory_order_relaxed); }

	/** @brief Reads the switches and logs what is on. Native mode calls it once, when it makes its device. */
	void Configure();

	/** @brief Whether a_op writes its target (the write census counts these). */
	bool IsWrite(Op a_op);

	/** @brief Counts one operation on a_target. Call only when Flags() is non-zero. Any thread. */
	void Record(Op a_op, const Target& a_target);

	/** @brief The operation's name as the reports give it ("ClearRenderTargetView"). */
	const char* Name(Op a_op);

	/**
	 * @brief Where an operation went instead of the immediate context's own path (org-native-frame-graph.md, Phase 3): a static
	 * graph pass (the engine skeleton's clears and copies), or ORG's external client (RenderGraph::ExternalWork).
	 */
	enum class Route : std::uint8_t
	{
		GraphPass,
		ExternalWork,
	};

	/**
	 * @brief Counts one operation that did not run on the immediate context's own path; Record counts only those that did. The op
	 * census reports both, per window. Call only when Flags() has kOps. Any thread.
	 */
	void RecordRouted(Op a_op, Route a_route);

	/**
	 * @brief A graph epoch was submitted (RenderGraphRuntime::TryExecuteEpoch): the operations after it fall after a_label. Render
	 * thread; nothing when the census is off.
	 */
	void NoteEpoch(const char* a_label);

	/** @brief The frame's end (RenderGraphRuntime::SealPresented): closes the frame and reports a window when due. */
	void EndFrame();
}
