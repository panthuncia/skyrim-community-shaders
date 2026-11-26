// The startup pipeline step: every key the pipeline key sources list is built before the main menu, through the lane's normal
// request paths, so play starts with them in the catalog.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawFamilyLaneSlots.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PipelineExpander.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/PipelineKeySource.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Globals.h"
#include "RenderGraph/PipelineCache.h"
#include "Utils/SwitchFile.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesImpl.h"

namespace DCLF
{
	namespace PipelinePrecompileDetail
	{
		// A forward line's identity: the key (5 words), its view's targets (colour, depth, colour 2) and the winding.
		constexpr std::size_t kForwardIdentityWords = 5 + 3 + 1;
		// A draw family line's identity: the main targets (count, 8 formats, depth) and the winding.
		constexpr std::size_t kDrawFamilyIdentityWords = 10 + 1;
		// The refraction line's identity: the normals format, the depth format, the write mask and the winding.
		constexpr std::size_t kRefractionIdentityWords = 4;
		// Enum ranges a line must stay inside.
		constexpr std::uint32_t kLastBlendFactor = static_cast<std::uint32_t>(rhi::BlendFactor::InvSrc1Alpha);
		constexpr std::uint32_t kLastBlendOp = static_cast<std::uint32_t>(rhi::BlendOp::Max);
		constexpr std::uint32_t kLastCullMode = static_cast<std::uint32_t>(rhi::CullMode::Back);

		std::uint32_t FloatWord(float a_value) { return std::bit_cast<std::uint32_t>(a_value); }
		float WordFloat(std::uint32_t a_word) { return std::bit_cast<float>(a_word); }

		/** @brief Main targets as manifest words: the count, 8 formats and the depth format. */
		void AppendTargetWords(PipelineWords& a_words, const TargetFormats& a_targets)
		{
			a_words.push_back(a_targets.colorCount);
			for (const auto format : a_targets.colors)
				a_words.push_back(static_cast<std::uint32_t>(format));
			a_words.push_back(static_cast<std::uint32_t>(a_targets.depth));
		}

		/** @brief Reads AppendTargetWords' words at a_words. False when malformed. */
		bool ReadTargetWords(const std::uint32_t* a_words, TargetFormats& a_targets)
		{
			a_targets.colorCount = a_words[0];
			if (a_targets.colorCount > a_targets.colors.size())
				return false;
			for (std::size_t i = 0; i < a_targets.colors.size(); ++i)
				a_targets.colors[i] = static_cast<DXGI_FORMAT>(a_words[1 + i]);
			a_targets.depth = static_cast<DXGI_FORMAT>(a_words[1 + a_targets.colors.size()]);
			return true;
		}

		/** @brief The draw family whose forward flags a forward key carries, or Count for a Lighting key (as ForwardPipelines.cpp). */
		Api::DrawFamily PrecompileForwardFamilyOf(std::uint32_t a_flags)
		{
			for (const auto& slot : kDrawFamilyLaneSlots)
				if (a_flags & slot.forwardFlags)
					return slot.family;
			return Api::DrawFamily::Count;
		}

		using Clock = std::chrono::steady_clock;
	}

	std::string DrawPipelines::Impl::MainManifestTag(const PipelineKey& a_key)
	{
		const auto family = KeyFamily(a_key.rasterFlags);
		if (family == Api::ProgramFamily::Count)
			return "lighting";
		std::string tag(std::string_view(Extensions::ProgramFamilyOf(family).keyName));
		std::ranges::replace(tag, ' ', '-');
		std::ranges::replace(tag, '\t', '-');
		// Never another kind's tag or Lighting's.
		if (tag.empty() || !IsMainPipelineTag(tag) || tag == "lighting")
			tag.insert(0, "family-");
		return tag;
	}

	PipelineWords DrawPipelines::Impl::MainManifestIdentity(const PipelineKey& a_key, const TargetFormats& a_targets, bool a_frontCCW)
	{
		PipelineWords words{ a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.rasterFlags, a_key.passDescriptor, static_cast<std::uint32_t>(a_key.vertexLayout),
			static_cast<std::uint32_t>(a_key.vertexLayout >> 32), a_key.runtimeVertex, a_key.runtimePixel };
		PipelinePrecompileDetail::AppendTargetWords(words, a_targets);
		words.push_back(a_frontCCW ? 1u : 0u);
		return words;
	}

	PipelineWords DrawPipelines::Impl::EngineStateWords(std::uint32_t a_bits, const EngineState& a_state)
	{
		using namespace PipelinePrecompileDetail;
		const auto& blend = a_state.blend;
		PipelineWords words{ a_bits, FloatWord(a_state.depthBias), FloatWord(a_state.depthBiasClamp), FloatWord(a_state.slopeScaledDepthBias), blend.alphaToCoverage ? 1u : 0u,
			blend.independentBlend ? 1u : 0u, blend.numAttachments };
		for (const auto& attachment : blend.attachments) {
			words.push_back(attachment.enable ? 1u : 0u);
			words.push_back(static_cast<std::uint32_t>(attachment.srcColor));
			words.push_back(static_cast<std::uint32_t>(attachment.dstColor));
			words.push_back(static_cast<std::uint32_t>(attachment.colorOp));
			words.push_back(static_cast<std::uint32_t>(attachment.srcAlpha));
			words.push_back(static_cast<std::uint32_t>(attachment.dstAlpha));
			words.push_back(static_cast<std::uint32_t>(attachment.alphaOp));
			words.push_back(static_cast<std::uint32_t>(attachment.writeMask));
		}
		return words;
	}

	bool DrawPipelines::Impl::EngineStateFromWords(const PipelineWords& a_inputs, std::uint32_t a_bits, EngineState& a_state)
	{
		using namespace PipelinePrecompileDetail;
		if (a_inputs.size() != kManifestStateWords)
			return false;
		const auto* s = a_inputs.data();
		if (s[0] != a_bits)
			return false;
		a_state = EngineState{};
		a_state.depthBias = WordFloat(s[1]);
		a_state.depthBiasClamp = WordFloat(s[2]);
		a_state.slopeScaledDepthBias = WordFloat(s[3]);
		if (s[4] > 1 || s[5] > 1 || s[6] > std::size(a_state.blend.attachments))
			return false;
		a_state.blend.alphaToCoverage = s[4] != 0;
		a_state.blend.independentBlend = s[5] != 0;
		a_state.blend.numAttachments = s[6];
		s += 7;
		for (auto& attachment : a_state.blend.attachments) {
			if (s[0] > 1 || s[1] > kLastBlendFactor || s[2] > kLastBlendFactor || s[3] > kLastBlendOp || s[4] > kLastBlendFactor || s[5] > kLastBlendFactor ||
				s[6] > kLastBlendOp || s[7] > 0xF)
				return false;
			attachment.enable = s[0] != 0;
			attachment.srcColor = static_cast<rhi::BlendFactor>(s[1]);
			attachment.dstColor = static_cast<rhi::BlendFactor>(s[2]);
			attachment.colorOp = static_cast<rhi::BlendOp>(s[3]);
			attachment.srcAlpha = static_cast<rhi::BlendFactor>(s[4]);
			attachment.dstAlpha = static_cast<rhi::BlendFactor>(s[5]);
			attachment.alphaOp = static_cast<rhi::BlendOp>(s[6]);
			attachment.writeMask = static_cast<rhi::ColorWriteEnable>(s[7]);
			s += kManifestAttachmentWords;
		}
		a_state.valid = true;
		return true;
	}

	bool DrawPipelines::Impl::MainFromManifest(const PipelineWords& a_identity, const PipelineWords& a_inputs, PipelineKey& a_key, TargetFormats& a_targets, bool& a_frontCCW,
		std::uint32_t& a_bits, EngineState& a_state)
	{
		using namespace PipelinePrecompileDetail;
		if (a_identity.size() != kMainManifestIdentityWords || a_inputs.size() != kManifestStateWords)
			return false;
		const auto* w = a_identity.data();
		a_key.vertexDescriptor = w[0];
		a_key.pixelDescriptor = w[1];
		a_key.rasterFlags = w[2];
		a_key.passDescriptor = w[3];
		a_key.vertexLayout = std::uint64_t{ w[4] } | (std::uint64_t{ w[5] } << 32);
		a_key.runtimeVertex = w[6];
		a_key.runtimePixel = w[7];
		w += kMainManifestKeyWords;
		if (!ReadTargetWords(w, a_targets))
			return false;
		const std::uint32_t frontCCW = w[2 + a_targets.colors.size()];
		if (frontCCW > 1)
			return false;
		a_frontCCW = frontCCW != 0;
		// A deferred range key's build reads other states, which no line records.
		const auto family = KeyFamily(a_key.rasterFlags);
		if (family != Api::ProgramFamily::Count && Extensions::FamilyRules(a_key.rasterFlags).deferredRanges)
			return false;
		bool blendOnly = false;
		a_bits = EngineStateBitsOf(a_key, blendOnly);
		return EngineStateFromWords(a_inputs, a_bits, a_state);
	}

	PipelineWords DrawPipelines::Impl::ShadowManifestIdentity(const ShadowPipelineKey& a_key, DXGI_FORMAT a_format, const ShadowRasterState& a_state)
	{
		using namespace PipelinePrecompileDetail;
		return { a_key.technique, a_key.rasterFlags, static_cast<std::uint32_t>(a_key.vertexLayout), static_cast<std::uint32_t>(a_key.vertexLayout >> 32),
			static_cast<std::uint32_t>(a_format), FloatWord(a_state.depthBias), FloatWord(a_state.depthBiasClamp), FloatWord(a_state.slopeScaledDepthBias),
			static_cast<std::uint32_t>(a_state.cull), a_state.frontCCW ? 1u : 0u, a_state.depthClip ? 1u : 0u };
	}

	bool DrawPipelines::Impl::ShadowFromManifest(const PipelineWords& a_identity, ShadowPipelineKey& a_key, DXGI_FORMAT& a_format, ShadowRasterState& a_state)
	{
		using namespace PipelinePrecompileDetail;
		if (a_identity.size() != kShadowManifestIdentityWords)
			return false;
		const auto* w = a_identity.data();
		if (w[8] > kLastCullMode || w[9] > 1 || w[10] > 1)
			return false;
		a_key = ShadowPipelineKey{};
		a_key.technique = w[0];
		a_key.rasterFlags = w[1];
		a_key.vertexLayout = std::uint64_t{ w[2] } | (std::uint64_t{ w[3] } << 32);
		a_format = static_cast<DXGI_FORMAT>(w[4]);
		a_state.depthBias = WordFloat(w[5]);
		a_state.depthBiasClamp = WordFloat(w[6]);
		a_state.slopeScaledDepthBias = WordFloat(w[7]);
		a_state.cull = static_cast<rhi::CullMode>(w[8]);
		a_state.frontCCW = w[9] != 0;
		a_state.depthClip = w[10] != 0;
		return true;
	}

	PipelineWords DrawPipelines::Impl::ForwardManifestIdentity(const ForwardPipelineKey& a_key, const ForwardTargets& a_targets, bool a_frontCCW)
	{
		return { a_key.vertexDescriptor, a_key.pixelDescriptor, a_key.flags, a_key.runtimeVertex, a_key.runtimePixel, static_cast<std::uint32_t>(a_targets.colour),
			static_cast<std::uint32_t>(a_targets.depth), static_cast<std::uint32_t>(a_targets.colour2), a_frontCCW ? 1u : 0u };
	}

	bool DrawPipelines::Impl::ForwardFromManifest(const PipelineWords& a_identity, ForwardPipelineKey& a_key, ForwardTargets& a_targets, bool& a_frontCCW)
	{
		using namespace PipelinePrecompileDetail;
		if (a_identity.size() != kForwardIdentityWords || a_identity[8] > 1)
			return false;
		a_key = ForwardPipelineKey{ a_identity[0], a_identity[1], a_identity[2], a_identity[3], a_identity[4] };
		a_targets.colour = static_cast<DXGI_FORMAT>(a_identity[5]);
		a_targets.depth = static_cast<DXGI_FORMAT>(a_identity[6]);
		a_targets.colour2 = static_cast<DXGI_FORMAT>(a_identity[7]);
		a_frontCCW = a_identity[8] != 0;
		return a_targets.colour != DXGI_FORMAT_UNKNOWN && a_targets.depth != DXGI_FORMAT_UNKNOWN;
	}

	std::string DrawPipelines::Impl::DrawFamilyManifestTag(Api::DrawFamily a_family)
	{
		std::string tag(kDrawFamilyPipelineTagPrefix);
		tag += Extensions::DrawFamilyOf(a_family).keyName;
		std::ranges::replace(tag, ' ', '-');
		std::ranges::replace(tag, '\t', '-');
		return tag;
	}

	PipelineWords DrawPipelines::Impl::DrawFamilyManifestIdentity(const TargetFormats& a_targets, bool a_frontCCW)
	{
		PipelineWords words;
		PipelinePrecompileDetail::AppendTargetWords(words, a_targets);
		words.push_back(a_frontCCW ? 1u : 0u);
		return words;
	}

	bool DrawPipelines::Impl::DrawFamilyFromManifest(const PipelineWords& a_identity, TargetFormats& a_targets, bool& a_frontCCW)
	{
		using namespace PipelinePrecompileDetail;
		if (a_identity.size() != kDrawFamilyIdentityWords || !ReadTargetWords(a_identity.data(), a_targets) || a_identity.back() > 1)
			return false;
		a_frontCCW = a_identity.back() != 0;
		return true;
	}

	PipelineWords DrawPipelines::Impl::RefractionManifestIdentity(DXGI_FORMAT a_format, DXGI_FORMAT a_depthFormat, std::uint8_t a_writeMask, bool a_frontCCW)
	{
		return { static_cast<std::uint32_t>(a_format), static_cast<std::uint32_t>(a_depthFormat), a_writeMask, a_frontCCW ? 1u : 0u };
	}

	void DrawPipelines::StartPrecompile()
	{
		using namespace PipelinePrecompileDetail;
		auto& p = impl->precompile;
		if (p.started || !Enabled() || !HasTargetFormats())
			return;
		p.started = true;
		if (::Util::SwitchFile::Value("CS_DCLF_PIPELINE_PRECOMPILE") == "0") {
			logger::info("[DCLF] pipeline precompile: off (CS_DCLF_PIPELINE_PRECOMPILE=0); each pipeline builds when first drawn");
			return;
		}
		const auto startedAt = Clock::now();
		// What a world frame's capture posts. No world frame has run yet, so nothing else writes these now. The engine states
		// come from the manifest lines instead, and are checked once the render thread reads them.
		auto& input = impl->renderInput;
		if (!input.lighting) {
			static const REL::Relocation<RE::BSShader**> lightingShader{ REL::Offset(DCLF::Engine::Address::kLightingShader) };
			if (auto* shader = *lightingShader.get(); shader && shader->shaderType.get() == RE::BSShader::Type::Lighting)
				input.lighting = shader;
		}
		if (!input.frontCCW)
			input.frontCCW = impl->EngineFrontCCW();
		for (std::size_t f = 0; f < Extensions::kProgramFamilies; ++f) {
			const auto& family = Extensions::ProgramFamilies()[f];
			if (!input.familySource[f] && family.taken)
				input.familySource[f] = family.contribution.programSource();
		}
		// The draw families' engine shaders, as their frame decisions post them (SetFamilyInputs).
		for (std::size_t f = 0; f < kDrawFamilyLanes; ++f) {
			const auto& family = Extensions::DrawFamilies()[f];
			if (!input.drawFamilySource[f] && family.taken && family.contribution.frame.programSource)
				input.drawFamilySource[f] = static_cast<RE::BSShader*>(const_cast<void*>(family.contribution.frame.programSource()));
		}
		if (!input.states)
			input.states = std::make_shared<const decltype(impl->engineStates)>(impl->engineStates);

		PipelineKeySet keys;
		LocalPipelineManifestSource local;
		ShippedPipelineManifestSource shipped;
		// The expander reads the state bits a main line's build reads, as the lane does.
		PipelineCombinationExpander expander([](const PipelineWords& a_identity) -> std::optional<std::uint32_t> {
			if (a_identity.size() != kMainManifestIdentityWords)
				return std::nullopt;
			PipelineKey key{};
			key.vertexDescriptor = a_identity[0];
			key.pixelDescriptor = a_identity[1];
			key.rasterFlags = a_identity[2];
			key.passDescriptor = a_identity[3];
			bool blendOnly = false;
			return Impl::EngineStateBitsOf(key, blendOnly);
		});
		for (auto* source : std::array<PipelineKeySource*, 3>{ &local, &shipped, &expander })
			CollectPipelineKeys(*source, keys);

		auto job = std::make_unique<Impl::PrecompileJob>();
		std::uint32_t malformed = 0, newRasterStates = 0;
		for (const auto& [tag, tagLines] : keys.Lines()) {
			const bool shadowTag = tag == kShadowPipelineTag;
			for (const auto& [identity, inputs] : tagLines) {
				const bool expanded = keys.IsExpanded(tag, identity);
				if (shadowTag) {
					ShadowPipelineKey key{};
					DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
					Impl::ShadowRasterState raster{};
					if (!Impl::ShadowFromManifest(identity, key, format, raster)) {
						++malformed;
						continue;
					}
					if (format != input.shadowFormat) {
						++job->otherTargets;
						continue;
					}
					// The session's id for the view's rasterizer state. A shadow view with this state later gets the same id.
					bool added = false;
					key.viewState = impl->ShadowRasterStateIdOf(raster, added);
					newRasterStates += added ? 1u : 0u;
					job->shadow.push_back(key);
					if (expanded)
						job->expandedShadow.try_emplace(key, Impl::ManifestLine{ tag, identity, inputs });
				} else if (tag == kForwardPipelineTag) {
					ForwardPipelineKey key{};
					ForwardTargets lineTargets{};
					bool frontCCW = false;
					if (!Impl::ForwardFromManifest(identity, key, lineTargets, frontCCW)) {
						++malformed;
						continue;
					}
					const auto view = Extensions::ForwardKeyView(key.flags);
					const auto family = PrecompileForwardFamilyOf(key.flags);
					const bool sourced = family == Api::DrawFamily::Count ? input.lighting != nullptr : input.drawFamilySource[static_cast<std::size_t>(family)] != nullptr;
					if (view == Api::EngineView::Count || !sourced || !input.frontCCW || frontCCW != *input.frontCCW) {
						++job->otherTargets;
						continue;
					}
					// The view's targets, as its frame would post them. The first line primes them. A line for others is skipped and kept.
					auto& viewTargets = input.viewTargets[static_cast<std::size_t>(view)];
					if (viewTargets.colour == DXGI_FORMAT_UNKNOWN)
						viewTargets = lineTargets;
					else if (!(viewTargets == lineTargets)) {
						++job->otherTargets;
						continue;
					}
					job->forward.push_back(key);
				} else if (tag == kRefractionPipelineTag) {
					if (identity.size() != kRefractionIdentityWords || identity[2] > 0xF || identity[3] > 1) {
						++malformed;
						continue;
					}
					const auto format = static_cast<DXGI_FORMAT>(identity[0]);
					auto* utility = globals::game::utilityShader;
					if (format == DXGI_FORMAT_UNKNOWN || !utility || static_cast<DXGI_FORMAT>(identity[1]) != Targets().depth || !input.frontCCW ||
						(identity[3] != 0) != *input.frontCCW || (input.refractionUtility && (input.refractionFormat != format || input.refractionWriteMask != identity[2]))) {
						++job->otherTargets;
						continue;
					}
					// As the forward epoch posts them (SetRefractionInputs). Other values there later build it again.
					input.refractionUtility = utility;
					input.refractionFormat = format;
					input.refractionWriteMask = static_cast<std::uint8_t>(identity[2]);
					job->refraction = true;
				} else if (tag.starts_with(kDrawFamilyPipelineTagPrefix)) {
					std::size_t f = 0;
					while (f < kDrawFamilyLanes && !(Extensions::DrawFamilies()[f].taken && Impl::DrawFamilyManifestTag(static_cast<Api::DrawFamily>(f)) == tag))
						++f;
					TargetFormats lineTargets{};
					bool frontCCW = false;
					if (f == kDrawFamilyLanes || !input.drawFamilySource[f]) {
						++job->otherTargets;  // a family this session does not draw
						continue;
					}
					Impl::EngineState state{};
					const std::uint32_t bits = std::uint32_t(Extensions::DrawFamilies()[f].contribution.program.writeMode) << kRasterWriteModeShift;
					if (!Impl::DrawFamilyFromManifest(identity, lineTargets, frontCCW) || !Impl::EngineStateFromWords(inputs, bits, state)) {
						++malformed;
						continue;
					}
					if (!(lineTargets == Targets()) || !input.frontCCW || frontCCW != *input.frontCCW) {
						++job->otherTargets;
						continue;
					}
					job->families[f] = true;
					job->states.try_emplace(bits, state);
				} else {
					PipelineKey key{};
					TargetFormats lineTargets{};
					bool frontCCW = false;
					std::uint32_t bits = 0;
					Impl::EngineState state{};
					if (!Impl::MainFromManifest(identity, inputs, key, lineTargets, frontCCW, bits, state) || Impl::MainManifestTag(key) != tag) {
						++malformed;
						continue;
					}
					// A line built for other targets or another winding is skipped and kept, for a session that has them again.
					if (!(lineTargets == Targets()) || !input.frontCCW || frontCCW != *input.frontCCW) {
						++job->otherTargets;
						continue;
					}
					job->main.push_back(key);
					job->states.try_emplace(bits, state);
					if (expanded)
						job->expandedMain.try_emplace(key, Impl::ManifestLine{ tag, identity, inputs });
				}
			}
		}
		if (newRasterStates)
			logger::info("[DCLF] pipeline precompile: {} shadow view rasterizer states registered from the manifest", newRasterStates);
		impl->PostInput(*this);
		for (const auto& [name, count] : keys.Sources()) {
			job->sources += fmt::format("{}{} {}", job->sources.empty() ? "" : ", ", count, name);
			if (name != expander.Name() || expander.RuleCounts().empty())
				continue;
			std::string rules;
			for (const auto& [rule, added] : expander.RuleCounts())
				rules += fmt::format("{}{} {}", rules.empty() ? "" : ", ", added, rule);
			job->sources += fmt::format(" ({})", rules);
		}
		std::uint32_t families = 0;
		for (const bool queued : job->families)
			families += queued ? 1u : 0u;
		const auto queuedKeys = static_cast<std::uint32_t>(job->main.size() + job->shadow.size() + job->forward.size()) + families + (job->refraction ? 1u : 0u);
		logger::info("[DCLF] pipeline precompile: {} main ({} expanded), {} shadow ({} expanded), {} forward, {} draw family and {} refraction keys queued ({}){}{}",
			job->main.size(), job->expandedMain.size(), job->shadow.size(), job->expandedShadow.size(), job->forward.size(), families, job->refraction ? 1 : 0,
			job->sources, job->otherTargets ? fmt::format(", {} for other targets or features skipped", job->otherTargets) : std::string(),
			malformed ? fmt::format(", {} malformed lines skipped", malformed) : std::string());
		if (!queuedKeys)
			return;
		job->started = startedAt;
		job->mainInputs = input.lighting && input.frontCCW && input.states;
		job->shadowInputs = input.utility && input.shadowFormat != DXGI_FORMAT_UNKNOWN && input.shadowStates;
		p.total.store(queuedKeys, std::memory_order_relaxed);
		p.pending.store(true, std::memory_order_release);
		// Nothing else runs yet, so the builds take every preparation worker until the step ends.
		SetBuildFullWidth(true);
		impl->precompileSlot.Post(std::move(job));
		WakePipelineLane();
	}

	DrawPipelines::PrecompileProgress DrawPipelines::GetPrecompileProgress() const
	{
		const auto& p = impl->precompile;
		PrecompileProgress progress;
		progress.pending = p.pending.load(std::memory_order_acquire);
		progress.total = p.total.load(std::memory_order_relaxed);
		progress.failed = p.failed.load(std::memory_order_relaxed);
		progress.finished = p.built.load(std::memory_order_relaxed) + progress.failed + p.deferred.load(std::memory_order_relaxed);
		return progress;
	}

	void DrawPipelines::Impl::TakePrecompileJob()
	{
		if (!heldPrecompileJob)
			heldPrecompileJob = precompileSlot.Take();
		if (!heldPrecompileJob)
			return;
		auto& l = lane;
		const auto* input = l.input.get();
		const bool mainReady = input && input->lighting && input->frontCCW && input->states && input->targets.colorCount != 0 && layout && zLayout;
		const bool shadowReady = input && input->utility && input->shadowFormat != DXGI_FORMAT_UNKNOWN && input->shadowStates && shadowLayout;
		// The inputs were posted just before the job, so a pass that took the job before them waits for the next.
		if ((heldPrecompileJob->mainInputs && !mainReady) || (heldPrecompileJob->shadowInputs && !shadowReady))
			return;
		const auto job = std::move(heldPrecompileJob);
		auto& p = precompile;
		auto& s = l.shadow;
		p.active = true;
		p.sources = std::move(job->sources);
		p.otherTargets = job->otherTargets;
		p.startedAt = job->started;
		// The render thread reads each recorded state's live one at its first deferred pass, to check the recorded one against.
		for (const auto& [bits, state] : job->states)
			if (p.recordedStates.try_emplace(bits, state).second && l.statesAsked.insert(bits).second)
				completions->stateRequests.Push(bits);
		const auto leave = [&](std::size_t a_count, std::string_view a_what) {
			p.deferred.fetch_add(static_cast<std::uint32_t>(a_count), std::memory_order_relaxed);
			if (a_count)
				logger::warn("[DCLF] pipeline precompile: the {} pipelines' inputs are not in; their {} keys are left for play", a_what, a_count);
		};
		// A key already asked for is the lane's already, and not the step's. An expanded key is marked before its request, so its build
		// is not recorded in the manifest until play asks for it.
		if (!mainReady) {
			std::uint32_t families = 0;
			for (const bool queued : job->families)
				families += queued ? 1u : 0u;
			leave(job->main.size() + job->forward.size() + families, "main, forward and draw family");
		} else {
			for (const auto& key : job->main) {
				if (!l.asked.try_emplace(key, ~0u).second) {
					p.deferred.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				p.main.insert(key);
				if (const auto expanded = job->expandedMain.find(key); expanded != job->expandedMain.end())
					p.expandedMain.insert_or_assign(key, std::move(expanded->second));
				TryRequest(key);
			}
			// Forward keys check their view's targets and program source themselves, and wait or are left for play as main keys are.
			auto& f = l.forward;
			for (const auto& key : job->forward) {
				if (!f.asked.insert(key).second) {
					p.deferred.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				p.forward.insert(key);
				TryRequestForward(key);
			}
			// The draw families are requested by TryRequestFamilies, later in this pass.
			for (std::size_t d = 0; d < kDrawFamilyLanes; ++d) {
				if (!job->families[d])
					continue;
				if (l.families[d].asked) {
					p.deferred.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				l.families[d].asked = true;
				p.families[d] = true;
			}
		}
		// The refraction pipeline is requested by TryRequestRefraction, later in this pass, once its inputs are in.
		if (job->refraction) {
			if (l.refraction.asked) {
				p.deferred.fetch_add(1, std::memory_order_relaxed);
			} else {
				l.refraction.asked = true;
				p.refraction = true;
			}
		}
		if (!shadowReady) {
			leave(job->shadow.size(), "shadow");
		} else {
			for (const auto& key : job->shadow) {
				if (!s.asked.try_emplace(key, ShadowRequest{ key, key, ~0u }).second) {
					p.deferred.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				p.shadow.insert(key);
				if (const auto expanded = job->expandedShadow.find(key); expanded != job->expandedShadow.end())
					p.expandedShadow.insert_or_assign(key, std::move(expanded->second));
				TryRequestShadow(key);
			}
		}
	}

	void DrawPipelines::Impl::UpdatePrecompile()
	{
		auto& p = precompile;
		if (!p.active)
			return;
		auto& l = lane;
		// An expanded key that failed is listed as unbuildable, so the expander does not offer it again. Any other failure is logged.
		const auto listUnbuildable = [&](auto& a_expanded, const auto& a_key) {
			const auto found = a_expanded.find(a_key);
			if (found == a_expanded.end())
				return false;
			PipelineManifest::Unbuildable().Record(found->second.tag, found->second.identity, {});
			a_expanded.erase(found);
			++p.expandedFailed;
			return true;
		};
		const auto settle = [&](auto& a_family, auto& a_keys, auto&& a_built, auto&& a_failed) {
			using Keys = std::remove_cvref_t<decltype(a_keys)>;
			using Key = typename Keys::key_type;
			if (a_keys.empty())
				return;
			// A key parked on its program is on its way. Any other key with no entry waits for an input only play gives.
			Keys onProgram;
			for (const auto& waiter : a_family.programWaiters)
				onProgram.insert(waiter.second.begin(), waiter.second.end());
			std::vector<Key> settled;
			for (const auto& key : a_keys) {
				const auto found = a_family.entries.find(key);
				if (found == a_family.entries.end()) {
					if (onProgram.contains(key))
						continue;
					p.deferred.fetch_add(1, std::memory_order_relaxed);
				} else if (found->second.failed) {
					// The failed build or program already dropped its manifest line.
					p.failed.fetch_add(1, std::memory_order_relaxed);
					a_failed(key);
				} else if (a_built(found->second)) {
					p.built.fetch_add(1, std::memory_order_relaxed);
				} else {
					continue;  // building, or admitted and not yet published
				}
				settled.push_back(key);
			}
			for (const auto& key : settled)
				a_keys.erase(key);
		};
		settle(l, p.main, [](const Entry& a_entry) { return a_entry.index != kNotReady; }, [&](const PipelineKey& a_key) {
			if (!listUnbuildable(p.expandedMain, a_key))
				logger::warn("[DCLF] pipeline precompile: {} failed; dropped from the pipeline manifest", DescribeKey(a_key));
		});
		settle(l.shadow, p.shadow, [](const Entry& a_entry) { return a_entry.index != kNotReady; }, [&](const ShadowPipelineKey& a_key) {
			if (!listUnbuildable(p.expandedShadow, a_key))
				logger::warn("[DCLF] pipeline precompile: shadow technique {:08X}, raster {:X}, vertex layout {:016X}, view state {} failed; dropped from the pipeline manifest",
					a_key.technique, a_key.rasterFlags, a_key.vertexLayout, a_key.viewState);
		});
		settle(l.forward, p.forward, [](const ForwardEntry& a_entry) { return a_entry.built != nullptr; }, [](const ForwardPipelineKey& a_key) {
			logger::warn("[DCLF] pipeline precompile: forward VS {:08X} PS {:08X} flags {:X} failed; dropped from the pipeline manifest", a_key.vertexDescriptor,
				a_key.pixelDescriptor, a_key.flags);
		});
		const auto* input = l.input.get();
		for (std::size_t d = 0; d < kDrawFamilyLanes; ++d) {
			if (!p.families[d])
				continue;
			const auto& t = l.families[d];
			if (t.built) {
				p.built.fetch_add(1, std::memory_order_relaxed);
			} else if (t.failed) {
				p.failed.fetch_add(1, std::memory_order_relaxed);
				logger::warn("[DCLF] pipeline precompile: {} pipelines failed; dropped from the pipeline manifest", Extensions::DrawFamilies()[d].label);
			} else if (t.requested) {
				continue;
			} else {
				// Not requested yet: its program is on its way when every input is in, else it waits for an input only play gives.
				const std::uint32_t bits = std::uint32_t(Extensions::DrawFamilies()[d].contribution.program.writeMode) << kRasterWriteModeShift;
				const bool stateKnown = input && input->states && (input->states->contains(bits) || p.recordedStates.contains(bits));
				if (input && input->drawFamilySource[d] && input->frontCCW && stateKnown && zLayout && FamilySignature(static_cast<Api::DrawFamily>(d)))
					continue;
				p.deferred.fetch_add(1, std::memory_order_relaxed);
			}
			p.families[d] = false;
		}
		if (p.refraction) {
			const auto& r = l.refraction;
			const bool inputs = input && input->refractionUtility && input->refractionFormat != DXGI_FORMAT_UNKNOWN && input->targets.depth != DXGI_FORMAT_UNKNOWN &&
			                    input->frontCCW && shadowLayout;
			if (r.built) {
				p.built.fetch_add(1, std::memory_order_relaxed);
				p.refraction = false;
			} else if (r.failed) {
				p.failed.fetch_add(1, std::memory_order_relaxed);
				logger::warn("[DCLF] pipeline precompile: the refraction pipeline failed; dropped from the pipeline manifest");
				p.refraction = false;
			} else if (!r.requested && !inputs) {
				p.deferred.fetch_add(1, std::memory_order_relaxed);
				p.refraction = false;
			}
		}
		if (!p.main.empty() || !p.shadow.empty() || !p.forward.empty() || p.refraction || std::ranges::find(p.families, true) != p.families.end())
			return;
		p.active = false;
		SetBuildFullWidth(false);
		// The driver cache holds every pipeline built, so the next start reads them instead of compiling.
		::RenderGraph::PipelineCache::RequestSave();
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(PipelinePrecompileDetail::Clock::now() - p.startedAt).count();
		logger::info("[DCLF] pipeline precompile: done in {} ms: {} keys ({}{}), {} built, {} failed{}, {} left for play, {} shader stages compiled for them", ms,
			p.total.load(std::memory_order_relaxed), p.sources, p.otherTargets ? fmt::format("; {} for other targets or features skipped", p.otherTargets) : std::string(),
			p.built.load(std::memory_order_relaxed), p.failed.load(std::memory_order_relaxed),
			p.expandedFailed ? fmt::format(" ({} of them expanded keys, now listed as unbuildable)", p.expandedFailed) : std::string(),
			p.deferred.load(std::memory_order_relaxed), p.stageCompiles);
		p.pending.store(false, std::memory_order_release);
	}

	void DrawPipelines::Impl::ValidateRecordedStates()
	{
		auto& p = precompile;
		auto& l = lane;
		if (p.recordedStates.empty() || !l.input || !l.input->states)
			return;
		const auto& live = *l.input->states;
		std::vector<std::uint32_t> checked, contradicted;
		for (const auto& [bits, recorded] : p.recordedStates) {
			const auto found = live.find(bits);
			if (found == live.end())
				continue;
			checked.push_back(bits);
			if (!found->second.valid || EngineStateWords(bits, found->second) != EngineStateWords(bits, recorded))
				contradicted.push_back(bits);
		}
		if (checked.empty())
			return;
		// The live state is used from here on.
		for (const std::uint32_t bits : checked)
			p.recordedStates.erase(bits);
		const auto in = [](const std::vector<std::uint32_t>& a_list, std::uint32_t a_bits) { return std::ranges::find(a_list, a_bits) != a_list.end(); };
		// A draw family built from a contradicted state is built again (RebuildContradicted).
		for (std::size_t d = 0; d < kDrawFamilyLanes; ++d) {
			if (p.familyRecordedBits[d] == ~0u || !in(checked, p.familyRecordedBits[d]))
				continue;
			p.familyRebuild[d] = p.familyRebuild[d] || in(contradicted, p.familyRecordedBits[d]);
			p.familyRecordedBits[d] = ~0u;
		}
		std::vector<PipelineKey> settled;
		std::uint32_t rebuilt = 0;
		for (const auto& [key, bits] : p.recordedUse) {
			if (!in(checked, bits))
				continue;
			settled.push_back(key);
			if (in(contradicted, bits)) {
				p.rebuild.push_back(key);
				++rebuilt;
			}
		}
		for (const auto& key : settled)
			p.recordedUse.erase(key);
		for (const std::uint32_t bits : contradicted)
			logger::warn("[DCLF] pipeline precompile: the engine's state {:05X} differs from the pipeline manifest's; the keys built from it are built again", bits);
		if (rebuilt)
			logger::warn("[DCLF] pipeline precompile: {} keys are built again with the engine's live states", rebuilt);
	}

	void DrawPipelines::Impl::RebuildContradicted()
	{
		auto& p = precompile;
		auto& l = lane;
		for (std::size_t d = 0; d < kDrawFamilyLanes; ++d) {
			if (!p.familyRebuild[d])
				continue;
			auto& t = l.families[d];
			// A build in flight finishes first, so its completion is never taken for the new build's.
			if (t.requested && !t.built && !t.failed)
				continue;
			p.familyRebuild[d] = false;
			// Kept for the process, because a recording may still draw with it.
			if (t.built)
				t.retired.push_back(std::move(t.built));
			t.built = {};
			t.slot = {};
			t.requested = false;
			t.failed = t.programFailed;
			if (l.input && l.input->frontCCW)
				PipelineManifest::Get().Forget(DrawFamilyManifestTag(static_cast<Api::DrawFamily>(d)), DrawFamilyManifestIdentity(l.input->targets, *l.input->frontCCW));
			l.dirty = true;
			logger::warn("[DCLF] pipeline precompile: {} pipelines are built again with the engine's live state", Extensions::DrawFamilies()[d].label);
		}
		if (p.rebuild.empty())
			return;
		std::vector<PipelineKey> waiting;
		for (const auto& key : std::exchange(p.rebuild, {})) {
			if (const auto found = l.entries.find(key); found != l.entries.end()) {
				// A build in flight finishes first, so its completion is never taken for the new build's.
				if (!found->second.failed && found->second.index == kNotReady) {
					waiting.push_back(key);
					continue;
				}
				if (found->second.failed)
					--l.failedEntries;
				// Its set index stays in the published sets, unused from the next catalog on.
				l.entries.erase(found);
			}
			// Built in play from here on, so it logs as on demand.
			if (p.main.erase(key))
				p.deferred.fetch_add(1, std::memory_order_relaxed);
			if (l.input && l.input->frontCCW)
				PipelineManifest::Get().Forget(MainManifestTag(key), MainManifestIdentity(key, l.input->targets, *l.input->frontCCW));
			l.dirty = true;
			TryRequest(key);
		}
		p.rebuild = std::move(waiting);
	}

	void DrawPipelines::Impl::PromoteExpanded(const PipelineKey& a_key)
	{
		auto& expanded = precompile.expandedMain;
		const auto found = expanded.find(a_key);
		if (found == expanded.end())
			return;
		const auto line = std::move(found->second);
		expanded.erase(found);
		// A failed build stays out. One still building is listed now, and dropped again if it fails.
		if (const auto entry = lane.entries.find(a_key); entry != lane.entries.end() && entry->second.failed)
			return;
		PipelineManifest::Get().Record(line.tag, line.identity, line.inputs);
	}

	void DrawPipelines::Impl::PromoteExpandedShadow(const ShadowPipelineKey& a_key)
	{
		auto& expanded = precompile.expandedShadow;
		const auto found = expanded.find(a_key);
		if (found == expanded.end())
			return;
		const auto line = std::move(found->second);
		expanded.erase(found);
		if (const auto entry = lane.shadow.entries.find(a_key); entry != lane.shadow.entries.end() && entry->second.failed)
			return;
		PipelineManifest::Get().Record(line.tag, line.identity, {});
	}
}
