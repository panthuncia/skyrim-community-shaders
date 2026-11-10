#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

namespace DirectX
{
	class ScratchImage;
}

/**
 * @brief The Screenshot feature's GPU readback on the render graph (CS_ORG_SCREENSHOT=1).
 *
 * One copy pass at the Capture anchor, at the end of HDRDisplay's present chain, where the D3D11 path copies into its staging
 * texture. The pass copies the capture source (the framebuffer, the back buffer, or HDR's clean composite) into one of ORG's
 * readback buffers. The readback service hands the bytes back once the GPU copy is done; the frame never waits for it.
 *
 * The copy is a one-shot pass (FrameAnchors::PassDesc::once). Other frames do not have it in the anchor's graph, so they submit
 * nothing for it. A capture arms it (FrameAnchors::Rearm) with that capture's request and source, then runs the anchor. A
 * capture whose copy was not in the epoch submitted at its capture point is withdrawn, and the caller copies it with D3D11.
 *
 * On any failure the feature uses its D3D11 staging copy. Render thread throughout, except Crop.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGScreenshot
{
public:
	/** @brief What to copy: the D3D11 texture, its SRV (may be null), and the engine target it is (-1 when it is not one). */
	struct Source
	{
		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		std::int32_t engineTarget = -1;
		const char* importName = nullptr;  // FrameAnchors::D3D11Texture's name when it is no engine target
	};

	/** @brief The copied texture: its first subresource at `offset` in `data`, rows `rowPitch` bytes apart. */
	struct Pixels
	{
		std::vector<std::byte> data;
		std::uint64_t offset = 0;
		std::uint32_t rowPitch = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	/** @brief Called on the render thread, from Poll, with the copied texture. */
	using OnPixels = std::function<void(Pixels&&)>;

	/** @brief Whether CS_ORG_SCREENSHOT=1 is set. */
	static bool Requested();

	static ORGScreenshot& Get();

	/** @brief Reserves the Capture anchor when requested. Call before the render graph initializes (the feature's PostPostLoad). */
	static void Reserve();

	/**
	 * @brief Registers the anchor client (the feature's SetupResources). False, logged, keeps every capture on D3D11.
	 * @param a_onFailed Called when the graph fails later; captures then use D3D11.
	 */
	bool Setup(std::function<void()> a_onFailed);

	/** @brief Whether captures go through the graph: the client is registered and the runtime is still up. */
	bool IsActive() const;

	/**
	 * @brief At the capture point: copies a_source on the graph now, in the Capture anchor's epoch. a_onPixels runs from a later
	 * Poll, once the GPU copy is done.
	 * @return False when the graph cannot take it (inactive, the source cannot be imported, the epoch failed, or the epoch
	 * submitted at this point did not hold the copy): the caller copies with D3D11 instead.
	 */
	bool Capture(const Source& a_source, OnPixels a_onPixels);

	/** @brief Every capture point, after any Capture: hands finished readbacks to their callbacks, and runs the anchor. */
	void Poll();

	/**
	 * @brief Copies the a_width x a_height crop at a_x, a_y of a readback into a_image, in a_format. Any thread: the encoding
	 * worker calls it, off the render thread.
	 * @return False when the readback does not hold the crop.
	 */
	static bool Crop(const Pixels& a_pixels, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format,
		DirectX::ScratchImage& a_image);

	~ORGScreenshot();

private:
	ORGScreenshot() = default;

	/** @brief The graph import of a_source, which the copy pass reads from its next build. */
	bool SetSource(const Source& a_source);

	struct Impl;
	std::unique_ptr<Impl> impl;
};
