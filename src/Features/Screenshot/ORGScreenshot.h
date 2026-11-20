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
* @brief The Screenshot feature's GPU readback on the render graph, with CS_ORG_SCREENSHOT=1.
*
* One copy pass at the Capture anchor copies the source into a readback buffer. It is recorded at execution from the capture its
* run stored, so a kept recording needs nothing new. The bytes come back at a later capture point and the frame never waits.
* D3D11's staging copy is used only when the graph cannot take a capture at all.
* The pre-UI clean scene is a graph copy too: at the UI stage's FrameGeneration step, after the post chain and before the UI, kFRAMEBUFFER
* is copied into a graph texture that a capture of that frame then reads. It needs no D3D11 content, so it also works in native mode.
* The texture and its client are declared once, at setup, on DisplaySize and HdrEnabled, so they switch in with a frame revision.
* Render thread throughout, except Crop. Free of ORG/Vulkan includes.
*/
class ORGScreenshot
{
public:
	/** @brief The SRV may be null. engineTarget is -1 when it is not one. */
	struct Source
	{
		ID3D11Texture2D* texture = nullptr;
		ID3D11ShaderResourceView* srv = nullptr;
		std::int32_t engineTarget = -1;
		const char* importName = nullptr;  // FrameAnchors::D3D11Texture's name when it is no engine target
		bool cleanScene = false;           // this frame's SnapshotCleanScene copy; texture and srv are not used
	};

	/** @brief The first subresource at offset in data, rows rowPitch bytes apart. */
	struct Pixels
	{
		std::vector<std::byte> data;
		std::uint64_t offset = 0;
		std::uint32_t rowPitch = 0;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	/** @brief Called on the render thread from Poll. */
	using OnPixels = std::function<void(Pixels&&)>;

	static bool Requested();

	static ORGScreenshot& Get();

	/** @brief The feature's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	/** @brief At SetupResources. False, logged, keeps every capture on D3D11. */
	bool Setup();

	/** @brief The client is registered and the runtime is still up. */
	bool IsActive() const;

	/**
	* @brief At the capture point. a_onPixels runs from a later Poll once the GPU copy is done.
	* @return False when the graph cannot take it. The caller then copies with D3D11.
	*/
	bool Capture(const Source& a_source, OnPixels a_onPixels);

	/** @brief Every capture point. Takes a waiting capture and hands finished readbacks to their callbacks. */
	void Poll();

	/**
	* @brief After the ImageSpace chain, before the UI: copies kFRAMEBUFFER into the clean scene on the graph.
	* @return False when it did not copy this frame: no adopted revision holds a copy for kFRAMEBUFFER's import at its size yet.
	*/
	bool SnapshotCleanScene();

	/** @brief Whether SnapshotCleanScene copied this frame. */
	bool HasFreshCleanScene() const;

	/** @brief The clean scene's description at the size of its last copy. Null when there is no clean scene on the graph. */
	const D3D11_TEXTURE2D_DESC* CleanSceneDesc() const;

	/** @brief Any thread, since the encoding worker calls it. @return False when the readback does not hold the crop. */
	static bool Crop(const Pixels& a_pixels, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format,
		DirectX::ScratchImage& a_image);

	~ORGScreenshot();

private:
	ORGScreenshot() = default;

	/** @brief The copy pass reads it from its next build. */
	bool SetSource(const Source& a_source);

	struct Impl;
	std::unique_ptr<Impl> impl;
};
