#pragma once

#include <ORGModuleServices/ImGui/ImGuiBackend.h>

#include <cstdint>
#include <memory>
#include <span>

struct ImDrawData;
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
* @brief CS's adapter for OMS's Dear ImGui renderer backend (ORGModuleServices/ImGui/ImGuiBackend.h).
*
* The backend runs ImGui as render-graph passes on BasicRHI. This adapter gives it CS's host on first use: the runtime's graph
* host, graph textures (GraphResources), descriptors (ShaderBindings), frame-revision page versions, the undeclared-access
* check, the "[ORG] " log and RenderGraph::Fatal for errors. CS keeps the pipeline's declaration, keyed on the interface's
* target format, and passes the built program in each Target.
*/
namespace ImGuiBasicRHI
{
	inline constexpr std::uint32_t kNoIndex = org::services::imgui::kNoIndex;

	/** @brief A texture made by MakeTexture: a full mip chain, a view of all of it and one per level. */
	using Texture = org::services::imgui::Texture;

	/** @brief What a draw samples: a descriptor-heap index or one of the backend's textures, plus what keeps it alive. */
	using Binding = org::services::imgui::Binding;

	/** @brief Turns an ImTextureID the backend does not own into a binding; kNoIndex skips the draws that use it. */
	using Resolver = org::services::imgui::Resolver;

	/** @brief What a frame draws onto: the target's size, its colour format (rhi::Format, as its value) and ProgramFor's pipeline. */
	using Target = org::services::imgui::Target;

	/** @brief Becomes the context's renderer backend. Call with no other renderer backend installed. */
	void Init();

	/** @brief Stops being the backend. Every texture is destroyed, and ImGui recreates them for the next backend. */
	void Shutdown();

	/** @brief Whether Init ran and Shutdown did not. */
	bool Installed();

	/** @brief Destroys every ImGui texture, so ImGui recreates them next frame, such as after a font reload. */
	void InvalidateTextures();

	/** @brief Whether an ImTextureID is one of the backend's textures (ImGui's, or MakeTexture's). */
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
