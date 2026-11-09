#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

/**
 * @brief The Screenshot feature's GPU readback on the render graph (CS_ORG_SCREENSHOT=1).
 *
 * One copy pass at the Capture anchor, at the end of HDRDisplay's present chain, where the D3D11 path copies into its staging
 * texture. The pass copies the capture source (the framebuffer, the back buffer, or HDR's clean composite) into one of ORG's
 * readback buffers. The readback service hands the bytes back once the GPU copy is done; the frame never waits for it.
 *
 * Captures are rare, so the client is active only at a capture point that armed one: other frames submit nothing. The epoch's
 * ticket is prepared ahead of the hook, so the pass's revision names the capture it prepared for. A capture armed after that
 * makes the ticket stale and it is prepared again, now with the copy.
 *
 * On any failure the feature uses its D3D11 staging copy. Render thread throughout.
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
	void ReserveAnchor();

	/**
	 * @brief At the capture point: copies a_source on the graph now, in the Capture anchor's epoch. a_onPixels runs from a later
	 * Poll, once the GPU copy is done.
	 * @return False when the graph cannot take it (not requested, inactive, the source cannot be imported, the epoch failed):
	 * the caller copies with D3D11 instead.
	 */
	bool Capture(const Source& a_source, OnPixels a_onPixels);

	/** @brief Every capture point, after any Capture: hands finished readbacks to their callbacks, and runs the anchor. */
	void Poll();

	~ORGScreenshot();

private:
	ORGScreenshot() = default;

	/** @brief Registers the anchor client once the runtime is up. False, for good after a failure, when the graph cannot take captures. */
	bool EnsureRegistered();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
