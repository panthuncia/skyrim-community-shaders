// The water reflection faces' capture and frame start: each face's camera and targets, the faces' pipelines and resources after
// tree LOD's decision, coverage, and the nodes the update hides. Render thread.

#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF::Features::Reflection
{
	namespace
	{
		/** @brief A render target view's format, its face (the array slice it views) and its texture. */
		struct ViewTarget
		{
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t face = ~0u;
			winrt::com_ptr<ID3D11Texture2D> texture;
			std::uint32_t width = 0, height = 0;
		};

		ViewTarget TargetOf(ID3D11RenderTargetView* a_view)
		{
			ViewTarget out;
			if (!a_view)
				return out;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			out.format = desc.Format;
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY && desc.Texture2DArray.ArraySize == 1)
				out.face = desc.Texture2DArray.FirstArraySlice;
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(out.texture.put())))) {
				D3D11_TEXTURE2D_DESC textureDesc{};
				out.texture->GetDesc(&textureDesc);
				out.width = textureDesc.Width;
				out.height = textureDesc.Height;
			}
			return out;
		}
	}

	void FaceOriented(const ShadowCameras::Camera& a_camera, std::uint32_t a_face)
	{
		auto& draws = IndirectDraws::Get();
		const auto* state = draws.ViewState(kView);
		if (!state)
			return;
		auto& reflection = FrameState();
		// Only a plain face render is DCLF's.
		if (!ReflectionFaces::Plain() || a_face >= kFaces)
			return;
		// The colour target is the engine's cube face. The depth is DCLF's own, cleared each face, at the engine's D24S8 precision.
		auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
		auto target = TargetOf(renderer ? renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[a_face] : nullptr);
		const ForwardTargets targets{ target.format, DXGI_FORMAT_D24_UNORM_S8_UINT };
		if (targets.colour == DXGI_FORMAT_UNKNOWN || !target.texture)
			return;
		if (!(state->targets == targets) && state->targets.colour != DXGI_FORMAT_UNKNOWN)
			logger::info("[DCLF] reflection faces' targets changed: colour {} -> {}, depth {} -> {}", static_cast<int>(state->targets.colour),
				static_cast<int>(targets.colour), static_cast<int>(state->targets.depth), static_cast<int>(targets.depth));
		draws.SetViewTargets(kView, targets);
		++reflection.facesCaptured;
		if (reflection.executedFrame == Scene::FrameView::Get().SceneStep())
			++reflection.lateFaces;
		auto& face = reflection.faces[a_face];
		face = {};
		// VS_PerFrame for the face's camera, with no jitter and no previous camera, as the engine uploads it for a face.
		Draws::PerFrameBlock(a_camera, Draws::FrameBlockNow(), false, nullptr, face.perFrame.data());
		face.perFrameBytes = ShadowCameras::kBlockBytes;
		const auto* floats = reinterpret_cast<const float*>(face.perFrame.data());
		std::memcpy(face.viewProj.data(), floats + 32, sizeof(float) * 16);
		face.eye = { floats[160], floats[161], floats[162] };
		face.captured = true;
		reflection.cube = std::move(target.texture);
		reflection.width = target.width;
		reflection.height = target.height;
	}

	void Prepare(const Api::ViewPointContext& a_context)
	{
		// Request the forward pipelines of every LOD slot the frame uses, for the faces' targets. They are ready a few frames later.
		// A slot without a ready pipeline is drawn by the engine in the faces meanwhile.
		auto& draws = IndirectDraws::Get();
		const auto* state = draws.ViewState(kView);
		if (!state)
			return;
		auto& reflection = FrameState();
		// The faces' targets are known before any face is drawn, so the reflection phase is the set's from the start.
		if (state->targets.colour == DXGI_FORMAT_UNKNOWN)
			if (auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer))
				if (auto* face = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].cubeSideRTV[0]) {
					draws.SetViewTargets(kView, { TargetOf(face).format, DXGI_FORMAT_D24_UNORM_S8_UINT });
					draws.PostViewInputs();
				}
		const bool on = state->targets.colour != DXGI_FORMAT_UNKNOWN && Extensions::ViewRegistered(kView);
		auto& drawPipelines = DrawPipelines::Get();
		if (on)
			drawPipelines.SetViewForwardTargets(kView, state->targets);
		IndirectDraws::ViewPipelineCounts pipelines;
		if (on)
			pipelines = draws.ResolveViewPipelines(kView, *a_context.tables, a_context.catalog, {}, true);
		else
			draws.DropViewPipelines(kView);
		const bool facesWaited = draws.ResolveViewFaces(kView, on, a_context.catalog);
		drawPipelines.NoteForwardWait(pipelines.waited || facesWaited);
		reflection.lodSlots = pipelines.slots;
		reflection.programsReady = pipelines.programs;
		reflection.pipelinesReady = pipelines.ready;
		if (on)
			draws.SetUpView(kView);
		draws.DecideViewFaces(kView);
	}

	void CullHiddenByUpdate(Api::CullHiddenNodes& a_nodes)
	{
		// TESWaterReflections::Update (AE 0x140520570) hides the player's 3D while it renders the faces, then restores it.
		// Main::Draw calls it between the main cull jobs' Begin and Finish.
		if (auto* player = CS_ENGINE_READ(Player, RecordTime, RE::PlayerCharacter::GetSingleton()))
			a_nodes.Add(player->Get3D(false));
		// It hides the water objects' shapes around the faces too.
		if (auto* water = CS_ENGINE_READ(Water, RecordTime, RE::TESWaterSystem::GetSingleton()))
			for (const auto& object : water->waterObjects)
				if (object && object->shape)
					a_nodes.Add(object->shape.get());
	}

	bool Covered(const Api::ViewCoverage& a_coverage)
	{
		// The faces are a snapshot component of this frame with their own frame constants, so no earlier commit has to be held.
		// With the toggle off, the set has no reflection members to withhold.
		const auto& c = a_coverage;
		return c.main && c.epochsCovered && c.targetImported && !c.rebuildRequested;
	}
}
