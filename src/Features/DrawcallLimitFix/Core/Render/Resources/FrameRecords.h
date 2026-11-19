#pragma once

// Writes the frame record and the frame-wide rows of a frame constants buffer, using the layout in Resources/FrameSlots.h.
// Every commit that draws with frame constants writes through here. Render thread.

// volk must come before every Vulkan header in this translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "RenderGraph/BufferRef.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"

#include <Resources/Buffers/Buffer.h>

#include <d3d11.h>
#include <winrt/base.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct ID3D11ShaderResourceView;

namespace DCLF::Draws
{
	struct PayloadVersions;
	struct SceneBuffers;

	/** @brief The FrameValues buffers a frame record names, as the commit took them. */
	struct FrameValueIndices
	{
		std::uint32_t placements = 0, palettes = 0, shading = 0;
	};

	/** @brief Writes a frame record's buffer registers. Every other register of a_record is left as is. */
	void WriteRecordBuffers(DrawBindings& a_record, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted,
		const SceneBuffers& a_scene, std::uint32_t a_frame);

	/** @brief Builds a frame record from the frame slots, block views, frame textures and the buffers WriteRecordBuffers writes.
	 * It also fills the backings the stages use to turn device addresses into refs. */
	DrawBindings FrameRecordOf(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef, const FrameBlockViews& a_blocks, std::uint32_t a_vsMask, std::uint32_t a_psMask, std::span<const std::uint32_t, kTextureRegisters> a_frameTextures,
		std::uint32_t a_nullIndex, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted, const SceneBuffers& a_scene,
		std::uint32_t a_frame);

	/** @brief Writes the backings the stages use to turn the record's, push data's and sequences' device addresses into refs. */
	void WriteRecordBackings(DrawBindings& a_record, RenderGraph::BufferRef a_constantsRef, std::uint64_t a_constants, std::uint64_t a_constantsBytes,
		const PayloadVersions& a_parts);

	/** @brief Stages the zero block and the zeroed Light Limit Fix light block into a_target once. */
	void ZeroSharedLightOnce(CommitUploads& a_uploads, const std::shared_ptr<org::Buffer>& a_target, bool& a_zeroed);

	/** @brief Latches this frame's frame-wide rows into a_target, in ascending row order. */
	void LatchFrameRows(LatchedUploads& a_latched, const std::shared_ptr<org::Buffer>& a_target);
	/** @brief Calls a_add for each write LatchFrameRows makes, in its order. */
	void FrameRowWrites(void (*a_add)(void* a_context, std::size_t a_bytes, std::uint64_t a_offset), void* a_context);

	/** @brief Latches a frame buffer's copy from its CPU mirror. Returns false when the mirror holds too little, and the copy then reads zero. */
	bool LatchFrameBuffer(LatchedUploads& a_latched, const FrameBuffer& a_frameBuffer, std::span<const std::byte> a_contents);
	/** @brief The buffer bound now at pixel register a_register, or null. Render thread. */
	winrt::com_ptr<ID3D11Buffer> BoundPixelBuffer(std::uint32_t a_register);

	/** @brief Community Shaders' SharedData (PS b5) and FeatureData (PS b6) as last uploaded, or empty. */
	std::span<const std::byte> SharedDataBlock();
	std::span<const std::byte> FeatureDataBlock();

	/** @brief Holds a frame texture's binding while the register names the view it came from, since the t16+ imports only live that long.
	 * Returns its descriptor index, or a_nullIndex for a null view. */
	std::uint32_t HoldFrameTexture(ID3D11ShaderResourceView*& a_heldView, GpuTextures::Binding& a_held, ID3D11ShaderResourceView* a_view, std::uint32_t a_register,
		std::uint32_t a_nullIndex, std::vector<std::shared_ptr<const void>>& a_owners);
}
