#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

struct ImDrawData;
namespace org::services
{
	struct RasterProgram;
}
using RasterProgram = org::services::RasterProgram;

namespace org
{
	class RenderPass;
}

namespace FrameAnchors
{
	class Texture;
}

namespace RenderGraph::Frame
{
	class Inputs;
}

/**
* @brief A Dear ImGui renderer backend on BasicRHI, run as render-graph passes. imgui_impl_dx11's work without D3D11.
*
* It implements ImGui 1.92's texture protocol, owning the font atlas pages and other ImTextureData. Other ImTextureIDs go to the
* caller's resolver. Each frame Commit stages vertices, indices and texture uploads in one buffer for the run. The upload pass
* copies, and the draw pass draws one ImDrawCmd at a time with ImGui's blending.
* Both passes are static. ImGui's textures are versions of a fixed set of atlas pages, so new textures need no graph build.
* A texture is restaged whole each frame until an upload of its bound version was recorded.
* Commit runs on the render thread inside the epoch. Draw callbacks are D3D11 code and are skipped.
*/
namespace ImGuiBasicRHI
{
	inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

	/** @brief A texture made by MakeTexture: a full mip chain, a view of all of it and one per level. */
	class Texture;

	/** @brief What a draw samples: a descriptor-heap index or one of this backend's textures, plus what keeps it alive until the frame retires. */
	struct Binding
	{
		std::uint32_t index = kNoIndex;
		std::shared_ptr<const void> owner;
		std::shared_ptr<Texture> texture;  // set: one of this backend's textures, read through the passes' declarations
		int mip = -1;                      // with texture: the level, or -1 for the whole chain
	};

	/** @brief Turns an ImTextureID the backend does not own into a binding; kNoIndex skips the draws that use it. */
	using Resolver = std::function<Binding(std::uint64_t a_texture)>;

	/** @brief What a frame draws onto: the size of the pass's target, its colour format (rhi::Format, as its value) and its pipeline. */
	struct Target
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;
		std::shared_ptr<const RasterProgram> program;  // ProgramFor, for the revision the registration was made for
	};

	/** @brief Becomes the context's renderer backend. Call with no other renderer backend installed. */
	void Init();

	/** @brief Stops being the backend. Every texture is destroyed, and ImGui recreates them for the next backend. */
	void Shutdown();

	/** @brief Whether Init ran and Shutdown did not. */
	bool Installed();

	/** @brief Destroys every ImGui texture, so ImGui recreates them next frame, such as after a font reload. */
	void InvalidateTextures();

	/** @brief Whether an ImTextureID is one of this backend's textures (ImGui's, or MakeTexture's). */
	bool IsOwnTexture(std::uint64_t a_texture);

	/** @brief An RGBA8 texture from CPU pixels, one span per level, uploaded with the next Commit. Render thread. Null, logged, on failure. */
	std::shared_ptr<Texture> MakeTexture(std::uint32_t a_width, std::uint32_t a_height, std::span<const std::span<const std::byte>> a_levels,
		const char* a_name);

	/** @brief A texture's binding. a_mip -1 means the whole chain, sampled at its top level. */
	Binding BindingOf(const std::shared_ptr<Texture>& a_texture, int a_mip);

	/**
	 * @brief Render owner, before the overlay's client is declared. Declares the pipeline on the interface's target format
	 * (Engine::UiTargets), so on HdrEnabled. A revision starts once its format's pipeline is built.
	 */
	void DeclareProgram();

	/** @brief Render thread, in a client's key or make that depends on HdrEnabled. The pipeline for a revision's values. */
	std::shared_ptr<const RasterProgram> ProgramFor(const RenderGraph::Frame::Inputs& a_in);

	/** @brief Answers texture requests, stages geometry and uploads, and stores the draws for the current run. Inside the pass's epoch.
	*  False, logged, when the frame cannot be drawn, so the caller falls back. */
	bool Commit(ImDrawData* a_drawData, const Target& a_target, const Resolver& a_resolve);

	/** @brief A new draw pass onto a_target, reading every atlas page. Records only what Commit stored for its run. Null without atlas pages. */
	std::shared_ptr<org::RenderPass> MakePass(FrameAnchors::Texture a_target);

	/** @brief A new upload pass, ordered before the draw pass. Records nothing on a run with nothing to upload. Null without atlas pages. */
	std::shared_ptr<org::RenderPass> MakeUploadPass();
}
