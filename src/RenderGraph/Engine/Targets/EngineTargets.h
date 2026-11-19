#pragma once

#include <d3d11.h>

#include <cstdint>
#include <memory>

#include "RenderGraph/FrameAnchors.h"

/**
* @brief The engine's render targets as ORG resources, with CS_ORG_ENGINE_TARGETS=1.
*
* The engine's target creation functions are detoured so ORG makes each image. The engine's fields get DXVK's D3D11 objects
* over the same memory, so D3D11 readers keep working. Each target is a versioned slot published by id, such as "Engine::kMAIN".
*
* The first set is made with D3D11 before the graph exists, and TakeOver replaces it. Later sets are made on ORG directly.
* The engine still frees targets through D3D11. A token in each texture's private data retires the image once DXVK destroys it.
*
* Kept on D3D11: kFRAMEBUFFER, mip-generating targets, mip views of other targets, non-DEFAULT 3D targets, and failures.
* Everything but Enabled runs on the render thread.
*/
namespace RenderGraph::Engine::Targets
{
	/** @brief Whether CS_ORG_ENGINE_TARGETS=1 is set. */
	bool Enabled();

	void Install();

	/** @brief Call before BSShaderRenderTargets::Create. */
	void BeginSet();

	/** @brief Call after BSShaderRenderTargets::Create. Publishes the set and moves the resize generation once. */
	void EndSet();

	/** @brief Remakes the targets the engine made on D3D11 before the graph existed, right after the graph initializes. */
	void TakeOver();

	/**
	* @brief Wraps every ORG target's fields in tripwire handles once the guest device and context exist.
	* Only with CS_ORG_ENGINE_TARGET_HANDLES=1. Targets made later get handles at creation.
	*/
	void InstallHandles();

	/** @brief How a CS feature remakes an engine target. ORG uses it from the first version so the feature's Recreate finds its shape. */
	struct Plan
	{
		const char* owner = "?";                   // for the log
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // UNKNOWN keeps the engine's
		UINT bindFlags = 0;                        // 0 keeps the engine's
		bool mainSize = false;                     // kMAIN's size instead of the engine's own
		bool dropCopy = false;                     // otherwise the copy keeps the engine's format, as HDR leaves it
	};

	/** @brief Call before the render graph is set up. A later call for the same target replaces the plan. */
	void PlanTarget(std::uint32_t a_target, const Plan& a_plan);

	bool Owns(std::uint32_t a_target);

	/**
	* @brief Remakes an ORG-owned target with another description until the engine's next Create. True with nothing done if it already has that shape.
	* False when ORG does not own it or a_desc is not a DEFAULT 2D texture with one slice and one mip. The caller then uses D3D11.
	*/
	bool Recreate(std::uint32_t a_target, const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool a_keepCopy = false);

	/** @brief Undoes Recreate. False when ORG does not own the target. */
	bool Restore(std::uint32_t a_target);

	/** @brief The published slot whose current D3D11 texture, or a tripwire handle of it, is a_texture. Empty if none. */
	FrameAnchors::Texture Find(const ID3D11Resource* a_texture);

	/** @brief Find by the VkImage DXVK reports for a texture. */
	FrameAnchors::Texture FindImage(std::uint64_t a_image);

	/** @brief Render thread, each frame. Hands the images of destroyed D3D11 textures to ReleaseAfterD3D11. */
	void ServiceRetired();
}
