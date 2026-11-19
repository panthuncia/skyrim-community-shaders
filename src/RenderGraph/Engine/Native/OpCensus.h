#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

/**
 * @brief Native mode's census of the immediate context's work. Off by default.
 *
 * CS_ORG_NATIVE_OP_CENSUS=1 counts every Map, upload, copy, clear and mip build per frame by kind and target, and records which
 * graph epoch each fell after. That is the order a single frame graph must keep.
 * CS_ORG_NATIVE_WRITE_CENSUS=1 lists SRV-only textures and VB/IB-only buffers written after creation, which cannot be exempt.
 * When both are off, each operation costs one relaxed load.
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
		UnmapUpload,  // its upload side is copied into the image
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
		std::uint64_t id = 0;         // the record's serial, unique for the session
		std::string_view name;        // the handle's debug name, else the record's own
		std::uint32_t bindFlags = 0;  // D3D11_BIND_*
		std::uint32_t usage = 0;      // D3D11_USAGE
		std::uint32_t dimension = 0;  // BUFFER for a buffer
		bool initialData = false;     // made with initial data
	};

	enum Flag : std::uint8_t
	{
		kOps = 1,
		kWrites = 2,
		kMarkWrites = 4,  // writes only mark their resource
	};

	namespace Detail
	{
		inline std::atomic<std::uint8_t> flags{ 0 };
	}

	/** @brief The switches as Configure read them. 0 before Configure or with all off. Any thread. */
	inline std::uint8_t Flags() { return Detail::flags.load(std::memory_order_relaxed); }

	/** @brief Reads the switches and logs what is on. Called once when native mode makes its device. */
	void Configure();

	bool IsWrite(Op a_op);

	/** @brief Counts one operation on a_target. Call only when Flags() is non-zero. Any thread. */
	void Record(Op a_op, const Target& a_target);

	const char* Name(Op a_op);

	/** @brief Where an operation went instead of the immediate context's own path. */
	enum class Route : std::uint8_t
	{
		GraphPass,
		ExternalWork,
	};

	/** @brief Counts an operation that did not run on the immediate context's own path. Call only when Flags() has kOps. Any thread. */
	void RecordRouted(Op a_op, Route a_route);

	/** @brief Marks a submitted graph epoch, so later operations fall after a_label. Render thread. */
	void NoteEpoch(const char* a_label);

	/** @brief Closes the frame and reports a window when due. */
	void EndFrame();
}
