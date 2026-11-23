// The shadow set: the shadow views' rasterizer states, pipelines, set versions and indirect state.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/Async/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Shading/VertexInput.h"
#include "ShaderCache.h"

#include <Tracy/Tracy.hpp>

#include <OpenRenderGraph/GraphHost.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <string_view>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesImpl.h"
#include "RenderGraph/Fatal.h"

namespace DCLF
{
	namespace
	{
		/** @brief A shadow request's casters, for the on-demand warnings. */
		std::string DescribeShadowCasters(const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
		{
			return fmt::format("for the casters of key technique {:08X}, raster {:X}, vertex layout {:016X} ({})", a_casterKey.technique, a_casterKey.rasterFlags,
				a_casterKey.vertexLayout, a_occlusion == ~0u ? std::string("the shadow views") : fmt::format("occlusion view {}", a_occlusion));
		}
	}

	inline namespace DrawPipelinesDetail
	{
		// The draw's words name its material row. The view's push words and frame record give the rest.
		std::vector<rhi::LayoutBindingRange> ShadowLayoutRanges()
		{
			const auto index = rhi::LayoutRangeSource::IndirectIndex;
			const auto pixelStage = rhi::ShaderStage::Pixel;
			auto fromShadowFrameRecord = [](rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 1, kShadowPushFrameRecord); };
			std::vector<rhi::LayoutBindingRange> shadowRanges;
			for (const bool pixel : { false, true }) {
				const auto stage = pixel ? rhi::ShaderStage::Pixel : rhi::ShaderStage::Vertex;
				for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
					const std::uint32_t b = kBindingShiftB + r;
					if (!pixel && r == 1)
						shadowRanges.push_back(PushedRange(b, stage, 0, kDrawPushMaterialRow));
					else if (r == 0)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushViewBlock));
					else if (r == kPerFrameVertexRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushPerFrame));
					else if (r == kSharedDataRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushSharedData));
					else if (r == kFeatureDataRegister)
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushFeatureData));
					else
						shadowRanges.push_back(PushedRange(b, stage, 1, kShadowPushZeros));
				}
			}
			// The diffuse t0 from the draw's row. Everything else from the frame record.
			shadowRanges.push_back(FromMaterialRow(LayoutRange(kBindingShiftT, 1, pixelStage, index, kShadowRowDiffuseOffset)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kBindingShiftT + 1, kTextureRegisters - 1, pixelStage, index, offsetof(DrawBindings, textures) + 4)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kBindingShiftS, kSamplerRegisters, pixelStage, index, offsetof(DrawBindings, samplers), true)));
			shadowRanges.push_back(fromShadowFrameRecord(LayoutRange(kVertexTextureBinding, kVertexTextureCount, rhi::ShaderStage::Vertex, index,
				offsetof(DrawBindings, textures) + 4 * std::size_t{ kShadingBufferRegister })));
			return shadowRanges;
		}
	}

	void DrawPipelines::RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion)
	{
		impl->completions->shadowRequests.Push({ a_viewKey, a_casterKey, a_occlusion });
		WakePipelineLane();
	}

	void DrawPipelines::InitShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility)
	{
		auto& input = impl->renderInput;
		if (a_format == DXGI_FORMAT_UNKNOWN)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the engine's shadow map has no declared format");
		// D3D11 mode runs CS's setup again after a resize. The format is the same then, so that call does nothing.
		if (input.shadowFormat != DXGI_FORMAT_UNKNOWN) {
			if (input.shadowFormat != a_format)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the shadow map format changed ({} -> {}); it is fixed for the session",
					static_cast<int>(input.shadowFormat), static_cast<int>(a_format)));
			return;
		}
		if (!Enabled())
			return;
		input.shadowFormat = a_format;
		input.utility = &a_utility;
		impl->PostInput(*this);
	}

	std::string DrawPipelines::Impl::NearestShadowKey(const ShadowPipelineKey& a_key) const
	{
		const ShadowPipelineKey* nearest = nullptr;
		KeyDifference best;
		for (const auto& [key, entry] : lane.shadow.entries) {
			if (key == a_key)
				continue;
			KeyDifference difference;
			difference.Field("technique", key.technique, a_key.technique);
			difference.Field("raster", key.rasterFlags, a_key.rasterFlags);
			difference.Field("vertex layout", key.vertexLayout, a_key.vertexLayout);
			difference.Field("view state", key.viewState, a_key.viewState);
			if (!nearest || difference.bits < best.bits) {
				nearest = &key;
				best = std::move(difference);
			}
		}
		if (!nearest)
			return std::string("the first shadow pipeline");
		return fmt::format("nearest requested shadow pipeline differs in {}", best.fields);
	}

	void DrawPipelines::Impl::TryRequestShadow(const ShadowPipelineKey& a_key)
	{
		auto& s = lane.shadow;
		if (s.entries.contains(a_key))
			return;  // requested, or failed for good
		// An inexpressible rasterizer state has id 0, so its views stay native.
		if (a_key.viewState == 0) {
			s.FailForGood(a_key);
			lane.dirty = true;
			return;
		}
		// The view's rasterizer state reaches the lane with the inputs after the render thread registers it.
		const auto* input = lane.input.get();
		if (!input || !input->utility || input->shadowFormat == DXGI_FORMAT_UNKNOWN || !input->shadowStates || a_key.viewState > input->shadowStates->size() ||
			!shadowLayout || !shadowDrawSignature || !shadowViewDrawSignature) {
			s.blocked.push_back(a_key);
			return;
		}
		const auto asked = s.asked.find(a_key);
		const std::string casters = asked != s.asked.end() ? DescribeShadowCasters(asked->second.casterKey, asked->second.occlusion) : std::string("for no recorded caster");
		// The startup pipeline step's keys build before play, so they are not logged as on demand.
		const bool startup = precompile.shadow.contains(a_key);
		std::uint8_t onDemand = 0;
		bool programFailed = false;
		const auto program = ShaderPrograms::Get().FindShadow(a_key.technique, *input->utility, true, nullptr, &onDemand, &programFailed);
		if (onDemand && startup)
			++precompile.stageCompiles;
		else if (onDemand)
			logger::warn("[DCLF] on-demand SPIR-V compile: Utility {} of technique {:08X} outside the derived stage batch, unexpected ({}), {}", ShaderPrograms::OnDemandStages(onDemand),
				a_key.technique, SIE::ShaderCache::Instance().IsCompiling() ? "Community Shaders' compile workers still busy" : "Community Shaders' compile workers idle",
				casters);
		const ShadowRasterState rasterState = (*input->shadowStates)[a_key.viewState - 1];
		const DXGI_FORMAT depthFormat = input->shadowFormat;
		auto manifestIdentity = ShadowManifestIdentity(a_key, depthFormat, rasterState);
		if (programFailed) {
			// Its casters stay native in this key's views until a shader reload makes its program again.
			s.FailForGood(a_key);
			lane.dirty = true;
			PipelineManifest::Get().Forget(std::string(kShadowPipelineTag), manifestIdentity);
			return;
		}
		if (!program) {
			s.programWaiters[a_key.technique].push_back(a_key);
			return;
		}
		if (s.Full()) {
			s.blocked.push_back(a_key);
			return;
		}
		const std::string id = fmt::format("dclf.shadow.{:08X}.{:X}.{:016X}.{}", a_key.technique, a_key.rasterFlags, a_key.vertexLayout, a_key.viewState);
		const auto build = [device = device, layout = ShadowLayout(), key = a_key, depthFormat, rasterState](const ShaderPrograms::ShadowProgram& a_program) {
			return BuildShadow(device, layout, key, &a_program, depthFormat, rasterState);
		};
		auto live = MakeLive(id, ShaderPrograms::SourcePathOf(*input->utility), *program,
			[technique = a_key.technique, utility = input->utility](ShaderPrograms::Rebuilt<ShaderPrograms::ShadowProgram> a_done) {
				ShaderPrograms::Get().RebuildShadow(technique, *utility, std::move(a_done));
			},
			build);
		s.entries.try_emplace(a_key);
		// A built key is listed in the pipeline manifest for the next startup, and a failed one is dropped from it.
		RequestBuild([build, program] { return build(*program); },
			[completions = completions, key = a_key, generation = s.generation, live = std::move(live), manifestIdentity = std::move(manifestIdentity)](Artifact a_artifact) {
				if (a_artifact)
					PipelineManifest::Get().Record(std::string(kShadowPipelineTag), manifestIdentity, {});
				else
					PipelineManifest::Get().Forget(std::string(kShadowPipelineTag), manifestIdentity);
				completions->shadow.Push({ key, generation, a_artifact, live });
				WakePipelineLane();
			});
		s.counters.requested.fetch_add(1, std::memory_order_relaxed);
		if (!startup)
			logger::warn("[DCLF] on-demand pipeline build: shadow technique {:08X}, raster {:X}, vertex layout {:016X}, view state {}, format {}, {}; {}", a_key.technique,
				a_key.rasterFlags, a_key.vertexLayout, a_key.viewState, static_cast<int>(depthFormat), casters, NearestShadowKey(a_key));
	}

	void DrawPipelines::Impl::AdmitShadow()
	{
		auto& s = lane.shadow;
		completions->shadow.Drain([&](ShadowDone&& a_done) {
			if (a_done.generation != s.generation)
				return;
			const auto& key = a_done.key;
			const auto& artifact = a_done.artifact;
			const auto found = s.entries.find(key);
			if (found == s.entries.end())
				return;
			auto& entry = found->second;
			lane.dirty = true;
			if (!artifact) {
				entry.failed = true;
				++s.failedEntries;
				s.counters.failed.fetch_add(1, std::memory_order_relaxed);
				if (s.loggedFailures++ < kMaxLoggedFailures)
					logger::warn("[DCLF] shadow pipeline technique {:08X} layout {:016X} failed: {}", key.technique, key.vertexLayout, artifact.error);
				return;
			}
			const auto* built = static_cast<const Built*>(artifact.payload.get());
			entry.slot = static_cast<std::uint32_t>(s.setPipelines.size());
			s.setPipelines.push_back(artifact.payload);
			s.setSlots.push_back(RegisterLive(a_done.live, artifact.payload));
			s.layouts.push_back(key.vertexLayout);
			s.discards.push_back(built->discards ? 1 : 0);
		});
		const auto admitted = static_cast<std::uint32_t>(s.setPipelines.size());
		if (!admitted)
			return;
		PublishAdmitted(s, s.versions, admitted, "shadow pipeline set", [&](ShadowSetVersion& a_version) {
			a_version.pipelines.assign(s.setPipelines.begin(), s.setPipelines.begin() + admitted);
			a_version.layouts.assign(s.layouts.begin(), s.layouts.begin() + admitted);
			a_version.discards.assign(s.discards.begin(), s.discards.begin() + admitted);
			return true;
		}, [](const ShadowPipelineKey&) {});
	}

	org::services::PipelinePayload DrawPipelines::Impl::BuildShadow(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, ShadowPipelineKey a_key,
		const ShaderPrograms::ShadowProgram* a_program, DXGI_FORMAT a_depthFormat, ShadowRasterState a_state)
	{
		auto [vertex, pixel] = Parse<2>({ &a_program->vertex, &a_program->pixel });
		CheckBindings(vertex, false);
		CheckBindings(pixel, true);
		auto built = std::make_shared<Built>();
		AddUsage(vertex, false, built->usage[kColorVariant]);
		AddUsage(pixel, true, built->usage[kColorVariant]);
		// No vertex input state. It is bound by its draws, not from an execution set.
		PipelineDesc desc(a_layout, a_program->vertex, a_program->pixel, a_depthFormat);
		desc.raster.rs.cull = (a_key.rasterFlags & kRasterTwoSided) ? rhi::CullMode::None : a_state.cull;
		desc.raster.rs.frontCCW = a_state.frontCCW;
		// The integer DepthBias passes through as the constant factor. Shadow maps are D16_UNORM, where Vulkan's default
		// representation matches what D3D11 and DXVK mean by it.
		desc.raster.rs.depthBias = a_state.depthBias;
		desc.raster.rs.depthBiasClamp = a_state.depthBiasClamp;
		desc.raster.rs.slopeScaledDepthBias = a_state.slopeScaledDepthBias;
		desc.raster.rs.depthClip = a_state.depthClip;
		desc.depth.ds.depthEnable = true;
		desc.depth.ds.depthWrite = true;
		desc.depth.ds.depthFunc = rhi::CompareOp::Less;
		desc.Create(a_device, built->pipelines[kColorVariant], "shadow");
		built->discards = FragmentDefersDepth(a_program->pixel);
		return built;
	}

	std::uint32_t DrawPipelines::ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode)
	{
		// The epoch's pass is bounded by the viewport, so no scissor. Depth clipping off becomes depth clamp, as DXVK draws it.
		// Setup fails on a device without depth clamp.
		const bool clampable = Enabled();
		if (a_desc.FillMode != D3D11_FILL_SOLID || (!a_desc.DepthClipEnable && !clampable)) {
			if (impl->loggedShadowRasterFailures++ < kMaxLoggedFailures)
				logger::warn("[DCLF] shadow view rasterizer state not expressible (fill {}, depth clip {}; pipelines {}); the view stays native",
					static_cast<int>(a_desc.FillMode), a_desc.DepthClipEnable, clampable ? "ready" : "not ready");
			return 0;
		}
		Impl::ShadowRasterState state;
		state.depthClip = a_desc.DepthClipEnable != FALSE;
		state.depthBias = static_cast<float>(a_desc.DepthBias);
		state.depthBiasClamp = a_desc.DepthBiasClamp;
		state.slopeScaledDepthBias = a_desc.SlopeScaledDepthBias;
		state.cull = a_desc.CullMode == D3D11_CULL_NONE ? rhi::CullMode::None : a_desc.CullMode == D3D11_CULL_FRONT ? rhi::CullMode::Front : rhi::CullMode::Back;
		state.frontCCW = a_desc.FrontCounterClockwise != FALSE;
		bool added = false;
		const std::uint32_t id = impl->ShadowRasterStateIdOf(state, added);
		if (!added)
			return id;
		logger::info("[DCLF] shadow view rasterizer state {} (mode {:#x}): depth bias {} (clamp {}, slope {}), cull {}, front {}, depth {}", id, a_renderMode,
			state.depthBias, state.depthBiasClamp, state.slopeScaledDepthBias, static_cast<int>(a_desc.CullMode), a_desc.FrontCounterClockwise ? "CCW" : "CW",
			state.depthClip ? "clipped" : "clamped");
		// Keys of this id wait in the lane until it arrives with the frame's inputs.
		if (Enabled())
			impl->PostInput(*this);
		return id;
	}

	std::uint32_t DrawPipelines::Impl::ShadowRasterStateIdOf(const ShadowRasterState& a_state, bool& a_added)
	{
		a_added = false;
		for (std::size_t i = 0; i < shadowRasterStates.size(); ++i)
			if (shadowRasterStates[i] == a_state)
				return static_cast<std::uint32_t>(i + 1);
		shadowRasterStates.push_back(a_state);
		renderInput.shadowStates = std::make_shared<const std::vector<ShadowRasterState>>(shadowRasterStates);
		a_added = true;
		return static_cast<std::uint32_t>(shadowRasterStates.size());
	}

	std::uint32_t DrawPipelines::ShadowRasterStateCount() const
	{
		return static_cast<std::uint32_t>(impl->shadowRasterStates.size());
	}

	ShadowIndirectState GetShadowIndirectState(const PipelineCatalog& a_catalog, DXGI_FORMAT a_shadowFormat)
	{
		// Invalid once the shadow map format differs from the set's.
		const auto& pipelines = DrawPipelines::Get();
		const auto& impl = *pipelines.impl;
		ShadowIndirectState state;
		if (!a_catalog.shadowSetVersion || a_catalog.shadowFormat != a_shadowFormat || !impl.layout || !impl.shadowLayout)
			return state;
		auto version = std::static_pointer_cast<const DrawPipelines::Impl::ShadowSetVersion>(a_catalog.shadowSetVersion);
		state.layout = impl.ShadowLayout();
		state.pipelines.reserve(version->pipelines.size());
		for (const auto& payload : version->pipelines)
			state.pipelines.push_back(static_cast<const DrawPipelines::Impl::Built*>(payload.get())->pipelines[kColorVariant]->GetHandle());
		state.vertexLayouts = version->layouts;
		state.discards = version->discards;
		state.drawSignature = impl.shadowViewDrawSignature->GetHandle();
		state.version = std::move(version);
		state.valid = true;
		return state;
	}

	ShadowIndirectState FrameShadowIndirectState()
	{
		const auto* catalog = FrameCatalog();
		return catalog ? GetShadowIndirectState(*catalog, DrawPipelines::Get().ShadowFormat()) : ShadowIndirectState{};
	}

	DXGI_FORMAT DrawPipelines::ShadowFormat() const
	{
		return impl->renderInput.shadowFormat;
	}
}
