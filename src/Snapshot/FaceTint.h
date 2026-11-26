#pragma once

#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "Snapshot/FrameSnapshot.h"
#include "Snapshot/Retire.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;

/**
* @brief The face tint bakes (side effect S18) in native mode, split into a game half and a render half (decision 8).
*
* The engine queues a job when an actor's head needs a tint texture (BSFaceGenManager::PrepareHeadPartForShaders, the race menu).
* Its FaceGen::UpdatePending (SE 26455, AE 27041), called by Main::DrawFrame and the loading loop, draws each job's sixteen masks with
* ISAlphaBlend into kPLAYER_FACEGEN_TINT, copies that into a new texture, hands the texture to the head's NiSourceTexture (+0x48) and
* frees the job. In native the engine's two calls are gone:
* - Game half, in the capture: the queue is drained under its lock. Each job becomes a Bake (its masks, its rows, a new texture
*   for the result) and is freed as the engine frees it. A bake stays in the block until the render side reports it done; the
*   next capture then hands its texture to the head and releases the old one, as UpdatePending did.
* - Render half, at each world frame's start: one bake at a time is drawn as the face tint step (ImageSpace::StageFaceTint).
*   The next world frame copies kPLAYER_FACEGEN_TINT into the bake's texture, once the step's stage has been submitted, and
*   reports it done. No engine call.
*
* Bakes are identified by serial, so a repeated snapshot draws nothing new and a dropped one loses no bake (it stays in the next
* block until reported). Guest and plain D3D11 install nothing and keep the engine's calls.
*/
namespace Snapshot::FaceTint
{
	/** @brief One queued job's bake. Made and let go on the main thread; the render side reads it through the adopted snapshot. */
	struct Bake
	{
		Bake() = default;
		Bake(const Bake&) = delete;
		Bake& operator=(const Bake&) = delete;
		~Bake();

		std::uint64_t serial = 0;
		std::array<EngineRef, 16> masks;                       // the masks' NiSourceTextures, kept alive with their views
		std::array<ID3D11ShaderResourceView*, 16> views{};     // their views (unwrapped), null for an unset or depth slot
		RenderGraph::Engine::ImageSpace::TintRows rows;        // ISAlphaBlend's pixel rows from the job's param
		EngineRef head;                                        // the head's NiSourceTexture the result goes to
		void* texture = nullptr;                               // the result's BSGraphics::Texture, ours until handed to head
		ID3D11Texture2D* destination = nullptr;                // its texture (unwrapped)
		ID3D11Texture2D* source = nullptr;                     // kPLAYER_FACEGEN_TINT's texture (unwrapped)
	};

	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::FaceTint;
		std::vector<std::shared_ptr<const Bake>> bakes;  // serial order, not yet reported done
		bool operator==(const Block& a_other) const
		{
			return bakes.size() == a_other.bakes.size() && (bakes.empty() || (bakes.front() == a_other.bakes.front() && bakes.back() == a_other.bakes.back()));
		}
	};

	/** @brief Native only, at hook install: removes the engine's two UpdatePending calls and registers the capture. */
	void InstallHooks();

	/** @brief Render owner, at each Renderer::Begin after the engine's Begin. Native world frames only. */
	void RenderFrame();

	/** @brief The counters since the last call, as part of the side effects line. Empty outside native. */
	std::string TakeStatsLine();
}
