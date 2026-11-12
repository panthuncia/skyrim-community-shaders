#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>

struct ImDrawData;

namespace org
{
	class RenderPass;
	class ExternalTextureResource;
}

/**
 * @brief A Dear ImGui renderer backend on BasicRHI, run as a render-graph pass: imgui_impl_dx11's work without D3D11.
 *
 * It implements ImGui 1.92's texture protocol (ImGuiBackendFlags_RendererHasTextures): the font atlas pages and any other
 * ImTextureData are BasicRHI textures it owns, created, updated by rectangle and destroyed as ImGui asks. Their ImTextureID is
 * the backend's own (IsOwnTexture); any other ImTextureID a draw names goes to the caller's resolver, which turns it into a
 * descriptor-heap index.
 *
 * Each frame, inside the epoch that runs the pass (its client's commit), Commit copies the draw data's vertices and indices, and
 * the texture uploads, into one host-visible staging buffer, and stores what the pass needs for the execution's run. The
 * pass (MakePass) then records the uploads (copies with their barriers; the graph does not track these textures) and one draw per
 * ImDrawCmd onto its target, each with its scissor, with ImGui's blending. The vertex stage pulls the vertices from the staging
 * buffer (package/Shaders/RenderGraph/ImGui.hlsl). The recording keeps the frame's staging buffer, textures and descriptors alive
 * until its GPU work retires.
 *
 * Threads: Init, Shutdown, InvalidateTextures and Commit on the render thread (Commit inside the epoch). The pass's Prepare and
 * Record run where the graph runs them, and read only the frame Commit stored for their run.
 *
 * Draw callbacks (ImDrawList::AddCallback) are D3D11 code in CS: they are skipped (logged once), except
 * ImDrawCallback_ResetRenderState, which needs nothing here.
 */
namespace ImGuiBasicRHI
{
	inline constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;

	/** @brief A descriptor-heap index and what keeps the descriptor and its resource alive (until the frame's GPU work retires). */
	struct Binding
	{
		std::uint32_t index = kNoIndex;
		std::shared_ptr<const void> owner;
	};

	/** @brief Turns an ImTextureID the backend does not own into a binding; kNoIndex skips the draws that use it. */
	using Resolver = std::function<Binding(std::uint64_t a_texture)>;

	/** @brief What a frame draws onto: the size of the pass's target and its colour format (rhi::Format, as its value). */
	struct Target
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t format = 0;
	};

	/**
	 * @brief Becomes the context's renderer backend (io.BackendRendererName, RendererHasTextures, RendererHasVtxOffset). Call it with no
	 * other renderer backend installed. Any textures another backend destroyed are created again on the next frames.
	 */
	void Init();

	/** @brief Stops being the backend: every texture destroyed (ImGui creates them again for the next backend), the io fields cleared. */
	void Shutdown();

	/** @brief Whether Init ran and Shutdown did not. */
	bool Installed();

	/** @brief Every ImGui texture destroyed; ImGui creates them again on the next frame (after a font reload, say). */
	void InvalidateTextures();

	/** @brief Whether an ImTextureID is one of this backend's textures (ImGui's, or MakeTexture's). */
	bool IsOwnTexture(std::uint64_t a_texture);

	/** @brief A texture made by MakeTexture: a full mip chain, a view of all of it and one per level. */
	class Texture;

	/**
	 * @brief An RGBA8 texture from CPU pixels (one span per mip level, tightly packed rows), uploaded with the next frame's Commit.
	 * Render thread, inside an epoch (or with the graph built). Null, logged, when the device or a view cannot be made.
	 */
	std::shared_ptr<Texture> MakeTexture(std::uint32_t a_width, std::uint32_t a_height, std::span<const std::span<const std::byte>> a_levels,
		const char* a_name);

	/** @brief A texture's binding: a_mip = -1 for the whole chain (sampled at its top level), else that level alone. */
	Binding BindingOf(const std::shared_ptr<Texture>& a_texture, int a_mip);

	/**
	 * @brief Loads the pipeline for a target format (rhi::Format, as its value) now (blocking on a compile the cache does not have).
	 * Render thread, with the runtime active. False, logged, when it cannot be made.
	 */
	bool Prepare(std::uint32_t a_format);

	/**
	 * @brief The frame: ImGui's texture requests answered, the geometry and uploads staged, the draws stored for the host's current
	 * run (GraphHost::CurrentRun) for the pass. Inside the pass's epoch (its commit). False, logged, when the frame cannot be drawn (no pipeline, no
	 * staging buffer): the caller's fallback.
	 */
	bool Commit(ImDrawData* a_drawData, const Target& a_target, const Resolver& a_resolve);

	/**
	 * @brief A new instance of the pass, drawing onto a_target (declared as its render target), for a graph build. It records what
	 * Commit stored for its run, and nothing in an execution Commit did not run for.
	 */
	std::shared_ptr<org::RenderPass> MakePass(std::shared_ptr<org::ExternalTextureResource> a_target);
}
