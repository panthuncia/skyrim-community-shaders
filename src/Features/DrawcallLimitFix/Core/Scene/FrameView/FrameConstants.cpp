// FrameView's Prepass constants: the pipelines' PerGeometry blocks from the frame's sample, the frame lighting and fog,
// the character light's view and the projected textures. Render thread.

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "State.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialSources.h"
#include "Snapshot/EngineReads.h"

namespace DCLF::Scene
{
	static_assert(std::tuple_size_v<decltype(FrameView::FrameCapture::lighting)> == std::tuple_size_v<FrameLighting>);
	static_assert(std::is_same_v<decltype(FrameView::FrameCapture::fog), FrameFog>);

	void FrameView::RefreshFrameConstants()
	{
		const Tables& view = View();
		DCLF_ZONE_NAMED(frameConstantsZone, "CS.DCLF.FrameConstants", true);
		RefreshCharacterLightView();
		// Every frame. Once a ProjectedUV object waits for them the engine draws none, so they cannot come from a native draw alone.
		NoteProjectedTextures();
		if (!EngineShaders::Lighting())
			return;
		// Native: the frame start's game half took it (its eye is zero at native's Prepass, inside RenderWorld). Guest samples it here
		// because only from Prepass is the eye the main camera's; at EarlyPrepass it is still the shadow-map camera's.
		// A sample without a sun makes nothing and is not kept.
		const auto& taken = FrameGlobals::Current().pipeline;
		const GeometryPort::PipelineFrame portFrame = taken ? *taken : GeometryPort::SamplePipelineFrame(false);
		++geometryStats.samples;
		const bool pipelineInputsMoved = portFrame.sun && (!geometryPortFrame || !portFrame.SamePipelineInputs(*geometryPortFrame));
		if (portFrame.sun) {
			geometryPortFrame = portFrame;
			geometryPortFrameNumber = renderStep;
			PostPipelineFrame(portFrame);
		}
		// Versioned only when it differs.
		FrameLighting frameLighting;
		std::memcpy(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting));
		std::uint32_t lightingWritten = 0;
		GeometryPort::MergeFrameLighting(portFrame, frameLighting, lightingWritten);
		// The technique rows keep the fog they were made with, so the time of day versions none of them. No engine read.
		FrameFog frameFog = frameCapture.fog;
		std::uint32_t fogWritten = 0;
		{
			TechniqueConstants fog;
			EvaluateTechnique(0, fog);
			MergeFrameFog(fog, frameFog, fogWritten);
		}
		frameTables.SyncPipelines(view.pipelines, view.pipelineBindingVersion, frameTablesGeneration, view.pipelinesStamp);
		TracyCZoneN(pipelinesZone, "CS.DCLF.FrameConstants.Pipelines", true);
		std::int64_t pipelinesVisited = 0;
		for (std::size_t word = 0; word < view.usedPipelineBits.size(); ++word) {
			for (std::uint64_t remaining = view.usedPipelineBits[word]; remaining; remaining &= remaining - 1) {
				const std::uint32_t i = static_cast<std::uint32_t>(word * 64 + std::countr_zero(remaining));
				if (i >= view.pipelines.size())
					continue;
				++pipelinesVisited;
				// Made whole when the slot is new, at the first refresh, or when the pipeline inputs moved. Otherwise only per-frame values are
				// written in place. Only what a DCLF_BINDLESS draw reads versions and posts the pipeline. Without a sun the held block is kept.
				if (!portFrame.sun)
					continue;
				const std::uint32_t passDescriptor = view.pipelines[i].passDescriptor;
				auto& held = frameTables.geometryConstants[i];
				bool ownChanged = false;
				if (!constantsRefreshed || !frameTables.geometryConstantsValid[i] || pipelineInputsMoved) {
					GeometryConstants constants;
					if (!GeometryPort::PipelineGeometryConstants(passDescriptor, GeometryPort::kMainPassRenderFlags, portFrame, constants))
						continue;
					++geometryStats.full;
					ownChanged = !frameTables.geometryConstantsValid[i] || !SameBindlessGeometry(constants, held);
					held = constants;
				} else {
					GeometryPort::RefreshFrameValues(passDescriptor, portFrame, held);
				}
				if (ownChanged) {
					frameTables.pipelineConstantsVersion[i] = frameTables.NextVersion();
					++geometryStats.changed;
					PostPipelineConstants(i);
				}
				frameTables.geometryConstantsValid[i] = 1;
			}
		}
		TracyCZoneEnd(pipelinesZone);
		TracyPlot("CS.DCLF.FrameConstants.Pipelines", pipelinesVisited);
		// A slot not visited this frame holds an older block. It is made again at the first Prepass that visits it.
		if (pipelineInputsMoved)
			for (std::size_t p = 0; p < frameTables.geometryConstantsValid.size(); ++p)
				if (p / 64 >= view.usedPipelineBits.size() || !((view.usedPipelineBits[p / 64] >> (p % 64)) & 1))
					frameTables.geometryConstantsValid[p] = 0;
		if (fogWritten)
			frameCapture.fog = frameFog;
		if (lightingWritten && std::memcmp(frameLighting.data(), frameCapture.lighting.data(), sizeof(frameLighting)) != 0) {
			std::memcpy(frameCapture.lighting.data(), frameLighting.data(), sizeof(frameLighting));
			++geometryStats.lightingVersions;
		}
		++geometryStats.frames;

		constantsRefreshed = true;
		++shadingFrames;
	}

	void FrameView::RefreshCharacterLightView()
	{
		// Kept while the sample has none, since its target is -1 while a cell loads.
		if (auto* view = MaterialSources::FrameCharacterLightView(FrameGlobals::Current().material))
			frameCapture.characterLightView = view;
	}

	void FrameView::NoteProjectedTextures()
	{
		// What BSLightingShader::SetupGeometry binds for a ProjectedUV pass (SE 0x1412f2bb0, AE 0x1414dd040), read from the start of
		// BSGraphics::State (SE 0x14302c890, AE 0x14328cc20). DCLF takes all four even when projected normals are off.
		static_assert(ProjectedTextures::kSlots[0] == 3 && ProjectedTextures::kSlots[1] == 8 && ProjectedTextures::kSlots[2] == 10 && ProjectedTextures::kSlots[3] == 11);
		const auto* engineState = reinterpret_cast<RE::NiSourceTexture* const*>(CS_ENGINE_READ(GraphicsState, RecordTime, RE::BSGraphics::State::GetSingleton()));
		if (!engineState)
			return;
		constexpr std::array<std::size_t, 4> kStateIndex{ 1, 2, 3, 0 };
		ProjectedTextures seen{};
		for (std::size_t i = 0; i < ProjectedTextures::kSlots.size(); ++i) {
			const auto* texture = engineState[kStateIndex[i]];
			const auto* renderer = texture ? texture->rendererTexture : nullptr;
			seen.views[i] = renderer ? reinterpret_cast<ID3D11ShaderResourceView*>(renderer->resourceView) : nullptr;
			if (!seen.views[i])
				return;
		}
		seen.valid = true;
		if (projectedTextures.valid && projectedTextures.views == seen.views)
			return;
		projectedTextures = seen;
		// Each view is referenced here, where the engine has it bound, so it outlives the scene work's request.
		auto capture = std::make_shared<SharedBindings::ProjectedCapture>();
		for (std::size_t i = 0; i < seen.views.size(); ++i)
			capture->views[i].copy_from(seen.views[i]);
		channels.projectedPosted.store(std::move(capture), std::memory_order_release);
	}

	void FrameView::PostPipelineConstants(std::uint32_t a_slot)
	{
		auto post = std::make_unique<PipelineConstantsPost>();
		post->slot = a_slot;
		post->key = frameTables.pipelineKeys[a_slot];
		post->binding = frameTables.pipelineBindings[a_slot];
		post->generation = frameTables.tablesGeneration;
		post->constants = frameTables.geometryConstants[a_slot];
		channels.pipelineConstantsPosted.Push(std::move(post));
		++pipelinesPosted;
	}
}
