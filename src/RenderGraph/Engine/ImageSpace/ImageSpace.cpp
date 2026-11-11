#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Switches.h"

bool RenderGraph::Engine::ImageSpace::Requested()
{
	static const bool requested = Switches::On("CS_ORG_IMAGESPACE");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/Engine/Constants/EngineConstants.h"
#	include "RenderGraph/Engine/Pipeline/Descriptions.h"
#	include "RenderGraph/Engine/Pipeline/StateConversion.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "ShaderCache.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <array>
#	include <cctype>
#	include <filesystem>
#	include <map>
#	include <optional>
#	include <set>
#	include <string_view>
#	include <tuple>
#	include <unordered_map>
#	include <vector>

namespace RenderGraph::Engine::ImageSpace
{
	namespace
	{
		using FrameAnchors::Anchor;
		namespace Constants = RenderGraph::Engine::Constants;
		namespace Pipeline = RenderGraph::Engine::Pipeline;

		constexpr const char* kClientId = "cs.engine.imagespace";
		// Gate entries (one per pass). Every registration numbers its passes from 0 by their place in the frame; no draw is taken
		// between a registration and the graph that holds it (Phase::Joining), so an older graph never runs with a newer latch.
		constexpr std::uint32_t kGateEntries = 128;
		// The ImageSpace anchor's runs a frame (its kInfo entry in FrameAnchors.cpp). The steady state uses two (the pre-tonemap
		// composites, the HDR effect up to the first draw it does not take); draws are taken while the frame has a run left. The
		// ring holds kGameFramesInFlight times this, and a run waits only for runs three frames old, so four a frame never fill it.
		// (When the anchors' slots would pass 255 the runtime sizes the anchor for 2 and logs "need too many frame slots"; a third
		// run a frame could then meet a full ring: the batch fails and ImageSpace returns to D3D11.)
		constexpr std::uint32_t kRunsPerFrame = 4;
		// Frames without a new shape before the client registers: the luminance chain alternates its last targets every frame, and
		// effects that follow the weather or the camera show up within a few seconds.
		constexpr std::uint32_t kLearnFrames = 120;
		// Registrations a session may make (the first, a cell change with new shapes, a resize). Each one rebuilds the anchor's graph.
		constexpr std::uint32_t kMaxRegistrations = 16;
		// SRV and sampler slots a shader file's ORG push block lists (t0..t(n-1), then s0..s(n-1)).
		constexpr std::uint32_t kMaxSlots = 3;

		/** @brief One effect the port covers: the engine's shader (BSImagespaceShader::name), its CS source and the slots it reads. */
		struct EffectInfo
		{
			std::string_view engineName;  // +0x120
			std::string_view effectName;  // R4's enum name
			std::string_view group;       // for CS_ORG_IMAGESPACE_EFFECTS
			const char* source;           // under Data/Shaders
			std::uint32_t slots;          // the source's SRV and sampler count in its ORG resource list
			std::uint32_t reads;          // bit per texture slot the pixel stage reads with these defines
		};

		// The engine slots and their names are R4's (engine-d3d11-surface.md); the read masks follow each source's code for the
		// effect's defines (GetShaderMacros, p5/imagespace-shaders.md).
		constexpr std::array<EffectInfo, 18> kEffects{ {
			{ "BSImagespaceShaderHDRDownSample16", "ISHDRDownSample16", "hdr", "ISHDR.hlsl", 3, 0b001 },
			{ "BSImagespaceShaderHDRDownSample4", "ISHDRDownSample4", "hdr", "ISHDR.hlsl", 3, 0b001 },
			{ "BSImagespaceShaderHDRDownSample16Lum", "ISHDRDownSample16Lum", "hdr", "ISHDR.hlsl", 3, 0b001 },
			{ "BSImagespaceShaderHDRDownSample4RGB2Lum", "ISHDRDownSample4RGB2Lum", "hdr", "ISHDR.hlsl", 3, 0b001 },
			{ "BSImagespaceShaderHDRDownSample4LumClamp", "ISHDRDownSample4LumClamp", "hdr", "ISHDR.hlsl", 3, 0b001 },
			{ "BSImagespaceShaderHDRDownSample16LumClamp", "ISHDRDownSample16LumClamp", "hdr", "ISHDR.hlsl", 3, 0b001 },
			{ "BSImagespaceShaderHDRDownSample4LightAdapt", "ISHDRDownSample4LightAdapt", "hdr", "ISHDR.hlsl", 3, 0b011 },
			{ "BSImagespaceShaderHDRDownSample16LightAdapt", "ISHDRDownSample16LightAdapt", "hdr", "ISHDR.hlsl", 3, 0b011 },
			{ "BSImagespaceShaderBrightPassBlur3", "ISBrightPassBlur3", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderBrightPassBlur5", "ISBrightPassBlur5", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderBrightPassBlur7", "ISBrightPassBlur7", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderBrightPassBlur9", "ISBrightPassBlur9", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderBrightPassBlur11", "ISBrightPassBlur11", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderBrightPassBlur13", "ISBrightPassBlur13", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderBrightPassBlur15", "ISBrightPassBlur15", "bloom", "ISBlur.hlsl", 2, 0b11 },
			{ "BSImagespaceShaderISCompositeVolumetricLighting", "ISCompositeVolumetricLighting", "composite", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b01 },
			{ "BSImagespaceShaderISCompositeLensFlare", "ISCompositeLensFlare", "composite", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b10 },
			{ "BSImagespaceShaderISCompositeLensFlareVolumetricLighting", "ISCompositeLensFlareVolumetricLighting", "composite", "ISCompositeLensFlareVolumetricLighting.hlsl", 2, 0b11 },
		} };

		/** @brief What one pass is made for: the effect's shader, its output and inputs, its samplers and its blend state. */
		struct ShapeKey
		{
			const void* shader = nullptr;  // the engine's BSImagespaceShader
			std::int32_t output = -1;      // RE::RENDER_TARGET
			std::array<std::int32_t, kMaxSlots> inputs{ -1, -1, -1 };
			std::array<std::uint32_t, kMaxSlots> address{};
			std::array<std::uint32_t, kMaxSlots> filter{};
			const void* blend = nullptr;  // the engine's ID3D11BlendState for the draw (its state table entry)

			auto Tie() const { return std::tie(shader, output, inputs, address, filter, blend); }
			bool operator<(const ShapeKey& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const ShapeKey& a_other) const { return Tie() == a_other.Tie(); }
		};

		/** @brief One pass: its immutable spec, made at a registration. */
		struct Spec
		{
			ShapeKey key;
			std::size_t effect = 0;
			std::uint32_t entry = 0;  // gate entry, latch region (entry * 16) and order key
			std::string name;
			std::shared_ptr<org::ExternalTextureResource> output;
			std::array<std::shared_ptr<org::ExternalTextureResource>, kMaxSlots> inputs;
			std::shared_ptr<const RasterProgram> program;
			std::array<std::uint32_t, kMaxSlots> samplers{};  // sampler heap indices
		};

		/** @brief A taken draw waiting for its batch's run: its pass and where its constants are. */
		struct Pending
		{
			std::uint32_t entry = 0;
			ConstantsAt at;
		};

		/** @brief OrgEngineConstantsAt in Common/OrgEngineConstants.hlsli. */
		struct ConstantsAt
		{
			std::uint32_t srv = 0;  // PerGeometry (PS b2)
			std::uint32_t offset = 0;
			std::uint32_t frameSrv = 0;  // PerFrame (PS b12), read as FrameBuffer
			std::uint32_t frameOffset = 0;
			std::uint32_t padding[4]{};
		};
		static_assert(sizeof(ConstantsAt) == 32);

		enum class Phase : std::uint8_t
		{
			Off,       // not requested, not set up, or failed
			Learning,  // the engine draws; new shapes are collected
			Joining,   // registered; the anchor's graph is being built with the client
			Live,      // the client's passes draw the shapes they were made for
		};

		/** @brief The BSImagespaceShader::Render call in progress. */
		struct EffectScope
		{
			const RE::BSImagespaceShader* shader = nullptr;
			std::size_t effect = 0;
			bool eligible = false;  // a covered, selected effect whose draw may be taken
			bool drew = false;      // its draw reached DrawTriShape
		};

		struct ImageSpaceState
		{
			bool installed = false;
			bool reserved = false;
			bool flushing = false;
			bool logShapes = false;
			Phase phase = Phase::Off;
			std::array<bool, kEffects.size()> enabled{};
			std::unordered_map<const void*, std::int32_t> effectOf;  // engine shader -> kEffects index, or -1
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> latch;
			std::map<std::pair<std::uint32_t, std::uint32_t>, org::OwnedDescriptorBinding> samplers;  // (address, filter)
			std::map<std::tuple<std::size_t, rhi::Format, const void*>, std::shared_ptr<const RasterProgram>> programs;
			std::vector<std::shared_ptr<const Spec>> specs;         // in the current registration, by entry
			std::set<ShapeKey> refusedShapes;                       // shapes no pass can be made for (logged once)
			std::vector<ShapeKey> candidates;                       // seen, not registered yet
			std::map<ShapeKey, std::uint32_t> positions;            // each shape's place in its frame: scope << 16 | draw within the scope
			std::set<std::pair<std::int32_t, std::int32_t>> swaps;  // targets that stand in for each other (ping-pong, alternation)
			std::uint32_t scope = 0;                                // the current scope (0 the pre-tonemap composites, 1 the HDR effect)
			std::uint32_t drawIndex = 0;                            // eligible draws so far in the current scope
			std::uint32_t nextEntry = 0;
			bool paused = false;  // the game was paused at the last frame (a menu)
			std::uint32_t registrations = 0;
			std::uint32_t quietFrames = 0;
			const void* registeredCell = nullptr;
			// The frame and its batches.
			std::uint32_t frame = UINT32_MAX;
			std::uint32_t runsThisFrame = 0;
			std::uint32_t scopeDepth = 0;
			std::optional<EffectScope> current;
			std::vector<Pending> batch;
			std::vector<std::shared_ptr<const org::LatchBlock>> keepAlive;  // the batch's constant chunks, until its run
			std::uint32_t lastEntry = 0;
			bool executed = false;
			// Counters.
			std::uint64_t taken = 0, engineDrawn = 0, batches = 0;
			std::set<std::string> loggedRefusals;
		};

		ImageSpaceState& IS()
		{
			static ImageSpaceState state;
			return state;
		}

		void Refuse(std::string_view a_reason, std::string_view a_effect)
		{
			auto& state = IS();
			++state.engineDrawn;
			const auto key = fmt::format("{}: {}", a_effect, a_reason);
			if (state.loggedRefusals.insert(key).second)
				logger::info("[ORG] ImageSpace: {} stays on D3D11 ({})", a_effect, a_reason);
		}

		/** @brief Every effect to D3D11 for the session. */
		void Fail(std::string_view a_why)
		{
			auto& state = IS();
			if (state.phase == Phase::Off)
				return;
			state.phase = Phase::Off;
			logger::error("[ORG] ImageSpace: {}; every ImageSpace effect returns to D3D11", a_why);
			FrameAnchors::Unregister(kClientId);
		}

		/** @brief CS_ORG_IMAGESPACE_EFFECTS: engine names, effect names or groups, any case; unset, empty or "all" selects every effect. */
		void SelectEffects()
		{
			auto& state = IS();
			const auto value = Switches::Value("CS_ORG_IMAGESPACE_EFFECTS");
			const auto lower = [](std::string_view a_text) {
				std::string out(a_text);
				std::ranges::transform(out, out.begin(), [](unsigned char a_char) { return static_cast<char>(std::tolower(a_char)); });
				return out;
			};
			if (value.empty() || lower(value) == "all") {
				state.enabled.fill(true);
				return;
			}
			std::string list = lower(value);
			std::size_t start = 0;
			while (start <= list.size()) {
				const auto end = std::min(list.find(',', start), list.size());
				std::string name = list.substr(start, end - start);
				std::erase_if(name, [](char a_char) { return a_char == ' '; });
				bool known = false;
				for (std::size_t i = 0; i < kEffects.size(); ++i) {
					const auto& effect = kEffects[i];
					if (!name.empty() && (name == lower(effect.engineName) || name == lower(effect.effectName) || name == lower(effect.group))) {
						state.enabled[i] = true;
						known = true;
					}
				}
				if (!name.empty() && !known)
					logger::warn("[ORG] ImageSpace: CS_ORG_IMAGESPACE_EFFECTS names {}, which is not an effect this port covers", name);
				start = end + 1;
			}
		}

		std::int32_t EffectOf(const RE::BSImagespaceShader* a_shader)
		{
			auto& state = IS();
			if (const auto it = state.effectOf.find(a_shader); it != state.effectOf.end())
				return it->second;
			std::int32_t found = -1;
			if (a_shader && a_shader->name) {
				const std::string_view name(a_shader->name);
				for (std::size_t i = 0; i < kEffects.size(); ++i)
					if (kEffects[i].engineName == name)
						found = static_cast<std::int32_t>(i);
			}
			state.effectOf.emplace(a_shader, found);
			return found;
		}

		/** @brief The engine render target a shader resource view is, or -1. kFRAMEBUFFER (an RTV only) is never one. */
		std::int32_t TargetOfView(const ID3D11ShaderResourceView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return -1;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = RE::RENDER_TARGET::kMAIN; i < RE::RENDER_TARGET::kTOTAL; ++i)
				if (targets[i].SRV == a_view)
					return static_cast<std::int32_t>(i);
			return -1;
		}

		/**
		 * @brief The draw's shape from the shadow state, as SetDirtyStates is about to apply it; false with a reason when no pass can
		 * draw it.
		 */
		bool ReadShape(const RE::BSImagespaceShader* a_shader, const EffectInfo& a_effect, ShapeKey& a_out, std::string_view& a_refusal)
		{
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto* renderer = globals::game::renderer;
			a_out.shader = a_shader;
			const auto output = static_cast<std::int32_t>(shadow.renderTargets[0]);
			if (output < static_cast<std::int32_t>(RE::RENDER_TARGET::kMAIN) || output >= static_cast<std::int32_t>(RE::RENDER_TARGET::kTOTAL)) {
				a_refusal = "its output is not an engine render target";
				return false;
			}
			for (std::size_t i = 1; i < std::size(shadow.renderTargets); ++i)
				if (static_cast<std::int32_t>(shadow.renderTargets[i]) != static_cast<std::int32_t>(RE::RENDER_TARGET::kNONE)) {
					a_refusal = "more than one render target";
					return false;
				}
			// The engine leaves a depth target bound through the chain with the test off: the pass binds none then, and the draw is the same.
			if (shadow.depthStencil != UINT32_MAX &&
				(shadow.depthStencilDepthMode != RE::BSGraphics::DepthStencilDepthMode::kDisabled || shadow.depthStencilStencilMode != 0)) {
				a_refusal = "a depth or stencil test";
				return false;
			}
			// A clear is applied by SetDirtyStates when the target is bound; the pending batch would run after it.
			constexpr std::uint32_t kDirtyRenderTarget = 0x1;
			if ((shadow.stateUpdateFlags.underlying() & kDirtyRenderTarget) && shadow.setRenderTargetMode[0] <= RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR_STENCIL) {
				a_refusal = "its target is cleared at the bind";
				return false;
			}
			auto* texture = renderer ? renderer->GetRuntimeData().renderTargets[output].texture : nullptr;
			if (!texture) {
				a_refusal = "its output has no texture";
				return false;
			}
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const auto& viewport = shadow.viewPort;
			if (viewport.TopLeftX != 0.0f || viewport.TopLeftY != 0.0f || viewport.Width != static_cast<float>(desc.Width) ||
				viewport.Height != static_cast<float>(desc.Height) || viewport.MinDepth != 0.0f || viewport.MaxDepth != 1.0f) {
				a_refusal = "a viewport smaller than its target";
				return false;
			}
			if (shadow.rasterStateScissorMode != 0) {
				a_refusal = "a scissor rectangle";
				return false;
			}
			a_out.output = output;
			for (std::uint32_t slot = 0; slot < a_effect.slots; ++slot) {
				if (!(a_effect.reads & (1u << slot)))
					continue;
				const auto input = TargetOfView(shadow.PSTexture[slot]);
				if (input < 0) {
					a_refusal = "an input that is not an engine render target";
					return false;
				}
				if (input == output) {
					a_refusal = "it reads its own output";
					return false;
				}
				a_out.inputs[slot] = input;
				a_out.address[slot] = shadow.PSTextureAddressMode[slot].underlying();
				a_out.filter[slot] = shadow.PSTextureFilterMode[slot].underlying();
			}
			const auto draw = Pipeline::EngineDrawState::Latch();
			a_out.blend = Pipeline::EngineBlendState(draw.blendMode, draw.alphaToCoverage, draw.writeMode, draw.blendExtra);
			if (!a_out.blend) {
				a_refusal = "a blend state outside the engine's table";
				return false;
			}
			return true;
		}

		std::uint32_t SamplerFor(std::uint32_t a_address, std::uint32_t a_filter)
		{
			auto& state = IS();
			const auto key = std::make_pair(a_address, a_filter);
			if (const auto it = state.samplers.find(key); it != state.samplers.end())
				return it->second ? it->second.Index() : UINT32_MAX;
			org::OwnedDescriptorBinding binding;
			if (auto* sampler = Pipeline::EngineSamplerState(a_address, a_filter)) {
				D3D11_SAMPLER_DESC desc{};
				sampler->GetDesc(&desc);
				binding = ShaderBindings::CreateSamplerFromD3D11(desc);
			}
			state.samplers.emplace(key, binding);
			return binding ? binding.Index() : UINT32_MAX;
		}

		/** @brief The effect's program for a target format and blend state, compiled the first time (it blocks then). Null on failure. */
		std::shared_ptr<const RasterProgram> ProgramFor(std::size_t a_effect, const RE::BSImagespaceShader* a_shader, rhi::Format a_format, ID3D11BlendState* a_blend)
		{
			auto& state = IS();
			const auto key = std::make_tuple(a_effect, a_format, static_cast<const void*>(a_blend));
			if (const auto it = state.programs.find(key); it != state.programs.end())
				return it->second;
			auto* host = RenderGraphRuntime::Get().Host();
			const auto& effect = kEffects[a_effect];
			std::shared_ptr<const RasterProgram> program;
			D3D11_BLEND_DESC blendDesc{};
			a_blend->GetDesc(&blendDesc);
			rhi::BlendState blend{};
			std::string error;
			if (!host) {
				error = "no graph host";
			} else if (!Pipeline::BlendFromD3D11(blendDesc, blend, error) || Pipeline::ReadsBlendFactor(blendDesc) || Pipeline::UsesDualSource(blendDesc)) {
				if (error.empty())
					error = "a blend factor or dual-source blending";
			} else {
				RasterProgram::Desc desc{};
				desc.pixelSource = effect.source;
				// FXC-era sources (vector conditions in ?:).
				desc.languageVersion = L"2018";
				desc.constantWords = 4 + 2 * effect.slots + 2;
				// The engine's macros for the shader and CS's feature defines, as ShaderCache composes them for the D3D11 build; the stage
				// defines are RasterProgram's own.
				for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(*a_shader, SIE::ShaderClass::Pixel, 0)) {
					if (name == "VSHADER" || name == "PSHADER")
						continue;
					desc.defines.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
				}
				desc.targets = { { .format = a_format, .blend = blend.attachments[0] } };
				program = RasterProgram::Load(host->Device(), desc);
				if (!program)
					error = "the program did not build";
			}
			if (program)
				logger::info("[ORG] ImageSpace: built {} for format {}", effect.effectName, static_cast<int>(a_format));
			else
				logger::warn("[ORG] ImageSpace: {} for format {} has no ORG program ({}); it stays on D3D11", effect.effectName, static_cast<int>(a_format), error);
			// Kept even when null: one attempt per key.
			state.programs.emplace(key, program);
			return program;
		}

		/** @brief The pass for a shape, made now (its textures imported, its program built), or null when it cannot be made. */
		std::shared_ptr<const Spec> MakeSpec(const ShapeKey& a_key)
		{
			auto& state = IS();
			const auto* shader = static_cast<const RE::BSImagespaceShader*>(a_key.shader);
			const auto effectIndex = EffectOf(shader);
			if (effectIndex < 0)
				return nullptr;
			const auto& effect = kEffects[static_cast<std::size_t>(effectIndex)];
			auto spec = std::make_shared<Spec>();
			spec->key = a_key;
			spec->effect = static_cast<std::size_t>(effectIndex);
			spec->output = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_key.output));
			std::string_view refusal;
			if (!spec->output)
				refusal = "its output could not be imported";
			for (std::uint32_t slot = 0; refusal.empty() && slot < effect.slots; ++slot) {
				if (!(effect.reads & (1u << slot)))
					continue;
				spec->inputs[slot] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_key.inputs[slot]));
				spec->samplers[slot] = SamplerFor(a_key.address[slot], a_key.filter[slot]);
				if (!spec->inputs[slot])
					refusal = "an input could not be imported";
				else if (spec->samplers[slot] == UINT32_MAX)
					refusal = "a sampler has no ORG form";
			}
			if (refusal.empty()) {
				spec->program = ProgramFor(spec->effect, shader, spec->output->GetFormat(), static_cast<ID3D11BlendState*>(const_cast<void*>(a_key.blend)));
				if (!spec->program)
					refusal = "no program";
			}
			if (refusal.empty() && state.nextEntry >= kGateEntries)
				refusal = "every gate entry is in use";
			if (!refusal.empty()) {
				if (state.refusedShapes.insert(a_key).second)
					logger::info("[ORG] ImageSpace: {} into rt{} stays on D3D11 ({})", effect.effectName, a_key.output, refusal);
				return nullptr;
			}
			spec->entry = state.nextEntry++;
			spec->name = fmt::format("{}.rt{}.{}", effect.effectName, a_key.output, spec->entry);
			return spec;
		}

		/** @brief Registers the client with every pass it has: the old registration, if any, is replaced. */
		void RegisterClient()
		{
			auto& state = IS();
			AnchorPasses::Builder builder(Anchor::ImageSpace, kClientId, 0, state.latch, state.gate);
			for (const auto& spec : state.specs) {
				const auto& effect = kEffects[spec->effect];
				AnchorPasses::Draw draw{};
				draw.name = spec->name;
				draw.program = spec->program;
				draw.bindings.push_back(AnchorPasses::Bind::Shared());
				for (std::uint32_t slot = 0; slot < effect.slots; ++slot)
					draw.bindings.push_back(spec->inputs[slot] ? AnchorPasses::Bind::Srv(spec->inputs[slot]) : AnchorPasses::Bind::Unused());
				for (std::uint32_t slot = 0; slot < effect.slots; ++slot)
					draw.bindings.push_back(spec->inputs[slot] ? AnchorPasses::Bind::Handle(spec->samplers[slot]) : AnchorPasses::Bind::Unused());
				draw.bindings.push_back(AnchorPasses::Bind::Latch(spec->entry * static_cast<std::uint32_t>(sizeof(ConstantsAt))));
				draw.target = spec->output;
				draw.viewport = { .x = 0, .y = 0, .width = spec->output->GetWidth(), .height = spec->output->GetHeight() };
				draw.load = rhi::LoadOp::Load;
				draw.gate = spec->entry;
				builder.Add(std::move(draw));
			}
			if (builder.Failed()) {
				Fail(fmt::format("its passes could not be made ({})", builder.Failure()));
				return;
			}
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			client.passes = builder.Take();
			client.sharedConstants = builder.ReadsSharedConstants();
			client.gates = { state.gate };
			client.active = [] { return !IS().batch.empty(); };
			client.commit = [](const FrameAnchors::Point&) {
				auto& current = IS();
				const std::uint32_t slot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
				for (const auto& pending : current.batch) {
					current.latch->WriteValue(slot, pending.entry * static_cast<std::uint32_t>(sizeof(ConstantsAt)), pending.at);
					current.gate->OpenDraw(pending.entry);
				}
			};
			client.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
			client.onFailed = [] {
				auto& current = IS();
				if (current.phase != Phase::Off) {
					current.phase = Phase::Off;
					logger::error("[ORG] ImageSpace: the ImageSpace anchor failed; every ImageSpace effect returns to D3D11");
				}
			};
			if (!FrameAnchors::Register(std::move(client))) {
				Fail("the ImageSpace anchor refused the client");
				return;
			}
			++state.registrations;
			state.phase = Phase::Joining;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				state.registeredCell = player->GetParentCell();
			logger::info("[ORG] ImageSpace: registered {} passes (registration {}); the graph is built with them before any draw is taken",
				state.specs.size(), state.registrations);
		}

		/** @brief a_key with every mention of one target of a_pair replaced by the other. */
		ShapeKey Swapped(const ShapeKey& a_key, const std::pair<std::int32_t, std::int32_t>& a_pair)
		{
			const auto swap = [&](std::int32_t a_target) { return a_target == a_pair.first ? a_pair.second : a_target == a_pair.second ? a_pair.first :
				                                                                                                                         a_target; };
			ShapeKey out = a_key;
			out.output = swap(out.output);
			for (auto& input : out.inputs)
				input = swap(input);
			return out;
		}

		bool Mentions(const ShapeKey& a_key, std::int32_t a_target)
		{
			return a_key.output == a_target || std::ranges::find(a_key.inputs, a_target) != a_key.inputs.end();
		}

		/**
		 * @brief Registers every shape seen (the current passes' and the new ones), with the shapes their ping-pong partners make, and
		 * numbers the passes from 0 by their place in the frame.
		 *
		 * Partners are targets that stand in for each other from frame to frame: kMAIN and kMAIN_COPY (the chain's source), and any two
		 * outputs of the same shader with the same inputs (the luminance chain's last level, which alternates by frame). A shape seen
		 * with one of them is registered with the other too, so the other frame's draws, and the light adaptation that reads the
		 * other level, are passes before they are first seen.
		 */
		void JoinCandidates()
		{
			auto& state = IS();
			if (state.registrations >= kMaxRegistrations) {
				if (!state.candidates.empty())
					logger::warn("[ORG] ImageSpace: {} registrations made; {} new shapes stay on D3D11 for the session", state.registrations, state.candidates.size());
				state.candidates.clear();
				return;
			}
			std::vector<ShapeKey> keys;
			for (const auto& spec : state.specs)
				keys.push_back(spec->key);
			for (const auto& key : state.candidates)
				if (std::ranges::find(keys, key) == keys.end())
					keys.push_back(key);
			const auto knownCount = state.specs.size();
			state.candidates.clear();
			// Alternating outputs: the same draw but for its output.
			for (std::size_t i = 0; i < keys.size(); ++i)
				for (std::size_t j = i + 1; j < keys.size(); ++j) {
					const auto& a = keys[i];
					const auto& b = keys[j];
					if (a.shader == b.shader && a.inputs == b.inputs && a.address == b.address && a.filter == b.filter && a.blend == b.blend && a.output != b.output)
						state.swaps.insert(std::minmax(a.output, b.output));
				}
			state.swaps.insert({ static_cast<std::int32_t>(RE::RENDER_TARGET::kMAIN), static_cast<std::int32_t>(RE::RENDER_TARGET::kMAIN_COPY) });
			// Each pair applied once to every shape seen: one partner per mention.
			const auto seen = keys.size();
			for (std::size_t i = 0; i < seen; ++i)
				for (const auto& pair : state.swaps) {
					if (!Mentions(keys[i], pair.first) && !Mentions(keys[i], pair.second))
						continue;
					const auto partner = Swapped(keys[i], pair);
					if (std::ranges::find(keys, partner) != keys.end() || state.refusedShapes.contains(partner))
						continue;
					keys.push_back(partner);
					state.positions.emplace(partner, state.positions.contains(keys[i]) ? state.positions[keys[i]] : 0u);
				}
			// Entries (and so the passes' order) follow each shape's place in its scope, so a frame's draws make one batch per scope
			// whichever partners it uses.
			std::ranges::stable_sort(keys, [&](const ShapeKey& a_left, const ShapeKey& a_right) {
				const auto left = state.positions.find(a_left);
				const auto right = state.positions.find(a_right);
				return (left != state.positions.end() ? left->second : 0u) < (right != state.positions.end() ? right->second : 0u);
			});
			std::vector<std::shared_ptr<const Spec>> specs;
			state.nextEntry = 0;
			for (const auto& key : keys)
				if (auto spec = MakeSpec(key))
					specs.push_back(std::move(spec));
			if (specs.size() == knownCount && state.phase != Phase::Learning) {
				// Nothing new could be made: the current registration stands (its entries are unchanged, as the order is).
				state.nextEntry = static_cast<std::uint32_t>(knownCount);
				return;
			}
			state.specs = std::move(specs);
			if (state.specs.empty()) {
				state.phase = Phase::Learning;
				return;
			}
			RegisterClient();
		}

		/** @brief The engine's targets were made again: every pass is dropped and the shapes are learnt again. */
		void Reset(std::string_view a_why)
		{
			auto& state = IS();
			if (state.registrations >= kMaxRegistrations) {
				Fail(fmt::format("{} after {} registrations", a_why, state.registrations));
				return;
			}
			logger::info("[ORG] ImageSpace: {}; the shapes are learnt again", a_why);
			state.specs.clear();
			state.candidates.clear();
			state.positions.clear();
			state.refusedShapes.clear();
			state.quietFrames = 0;
			state.phase = Phase::Learning;
		}

		bool Usable()
		{
			const auto& state = IS();
			return state.phase != Phase::Off && !state.flushing && FrameAnchors::OnRenderThread() && RenderGraphRuntime::Get().IsActive();
		}

		/** @brief Runs the pending batch at the ImageSpace anchor, here in the D3D11 stream. */
		void Flush()
		{
			auto& state = IS();
			if (state.batch.empty() || state.flushing)
				return;
			const auto count = state.batch.size();
			state.executed = false;
			state.flushing = true;
			const bool ran = FrameAnchors::Run(Anchor::ImageSpace);
			state.flushing = false;
			++state.runsThisFrame;
			state.batch.clear();
			state.keepAlive.clear();
			if (!ran || !state.executed) {
				// The engine's draws were withheld already: this frame misses them. Taking no more keeps it to one frame.
				Fail(fmt::format("a batch of {} draws did not run on the graph ({})", count, ran ? "its graph did not hold the client" : "the anchor's run failed"));
				return;
			}
			state.taken += count;
			++state.batches;
			if (state.batches == 1)
				logger::info("[ORG] ImageSpace: the first batch ({} draws) ran on the graph", count);
		}

		/** @brief Once a frame, on the first ImageSpace call of the main thread: the frame's runs, the learning window, cell changes. */
		void BeginFrame()
		{
			auto& state = IS();
			const auto frame = globals::state ? globals::state->frameCount : 0u;
			if (frame == state.frame)
				return;
			Flush();
			state.frame = frame;
			state.runsThisFrame = 0;
			// A menu that pauses the game opening or closing, or a cell change, is a frame the player does not watch for continuity:
			// shapes first seen since the registration join there (the graph is built again once).
			auto* ui = RE::UI::GetSingleton();
			const bool paused = ui && ui->GameIsPaused();
			const bool pauseChanged = paused != state.paused;
			state.paused = paused;
			auto* player = RE::PlayerCharacter::GetSingleton();
			const void* cell = player ? player->GetParentCell() : nullptr;
			switch (state.phase) {
			case Phase::Learning:
				// The window counts only frames with the player in a cell: the main menu has no eye adaptation and its own source.
				if (!state.candidates.empty() && cell && ++state.quietFrames >= kLearnFrames)
					JoinCandidates();
				break;
			case Phase::Live:
				if (!state.candidates.empty() && (pauseChanged || cell != state.registeredCell))
					JoinCandidates();
				break;
			default:
				break;
			}
		}

		/** @brief The graph holds the registration: draws are taken from now. Otherwise the anchor runs once to build it. */
		void Join()
		{
			auto& state = IS();
			if (state.phase != Phase::Joining)
				return;
			if (FrameAnchors::InBuiltGraph(kClientId, Anchor::ImageSpace)) {
				state.phase = Phase::Live;
				logger::info("[ORG] ImageSpace: the graph holds {} passes; their draws are the graph's from now", state.specs.size());
				return;
			}
			if (state.runsThisFrame < kRunsPerFrame && state.batch.empty()) {
				state.flushing = true;
				FrameAnchors::Run(Anchor::ImageSpace);
				state.flushing = false;
				++state.runsThisFrame;
			}
		}

		const Spec* SpecFor(const ShapeKey& a_key)
		{
			for (const auto& spec : IS().specs)
				if (spec->key == a_key)
					return spec.get();
			return nullptr;
		}

		void NoteShape(const ShapeKey& a_key)
		{
			auto& state = IS();
			if (state.refusedShapes.contains(a_key) || SpecFor(a_key) || std::ranges::find(state.candidates, a_key) != state.candidates.end())
				return;
			if (state.specs.size() + state.candidates.size() >= kGateEntries)
				return;
			state.candidates.push_back(a_key);
			state.positions.emplace(a_key, state.scope << 16 | std::min(state.drawIndex, 0xFFFFu));
			state.quietFrames = 0;
			if (state.logShapes) {
				const auto effect = EffectOf(static_cast<const RE::BSImagespaceShader*>(a_key.shader));
				logger::info("[ORG] ImageSpace: new shape {} into rt{} from rt{} rt{} rt{}", effect >= 0 ? kEffects[static_cast<std::size_t>(effect)].effectName : std::string_view("?"), a_key.output,
					a_key.inputs[0], a_key.inputs[1], a_key.inputs[2]);
			}
		}

		/**
		 * @brief The engine's draw of the current effect: taken into the batch (true; the caller skips the engine's draw) or left to the
		 * engine (false, after running any pending batch so the engine's order holds).
		 */
		bool TakeDraw()
		{
			auto& state = IS();
			auto& scope = *state.current;
			scope.drew = true;
			const auto& effect = kEffects[scope.effect];
			++state.drawIndex;
			ShapeKey key{};
			std::string_view refusal;
			if (!ReadShape(scope.shader, effect, key, refusal)) {
				Flush();
				Refuse(refusal, effect.effectName);
				return false;
			}
			if (state.phase != Phase::Live) {
				NoteShape(key);
				Flush();
				++state.engineDrawn;
				return false;
			}
			const Spec* spec = SpecFor(key);
			if (!spec) {
				NoteShape(key);
				Flush();
				Refuse("a shape first seen after the registration; it joins at the next pausing menu or cell change", effect.effectName);
				return false;
			}
			// A target made again (a resize) has a new import: the passes made for the old one are dropped.
			bool current = spec->output == FrameAnchors::EngineTarget(static_cast<std::uint32_t>(key.output));
			for (std::uint32_t slot = 0; current && slot < effect.slots; ++slot)
				current = !spec->inputs[slot] || spec->inputs[slot] == FrameAnchors::EngineTarget(static_cast<std::uint32_t>(key.inputs[slot]));
			if (!current) {
				Flush();
				Reset("the engine's targets were made again");
				++state.engineDrawn;
				return false;
			}
			// A batch runs in its passes' order: a pass at or before the last one taken (the engine's order differs from the first
			// registration's, or the same pass again) starts a new batch.
			if (!state.batch.empty() && spec->entry <= state.lastEntry)
				Flush();
			if (state.phase != Phase::Live)
				return false;
			if (state.batch.empty() && state.runsThisFrame >= kRunsPerFrame) {
				Refuse("the frame's ImageSpace anchor runs are used up", effect.effectName);
				return false;
			}
			// The engine's state for the draw goes to D3D11 as before (the draw would apply it), and the capture records the draw's
			// constants as a use: the draw is withheld, so no draw hook records it.
			static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
			setDirtyStates(false);
			const auto use = Constants::Capture(Constants::kDrawStages);
			const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 2) : nullptr;
			// The ImageSpace setups write PerGeometry into the PS b2 size-class pool (PSGeometry); a custom mapper's ring is accepted too.
			if (!slot || (slot->group != Constants::Group::PSGeometry && slot->group != Constants::Group::Custom)) {
				Flush();
				Refuse("no captured PerGeometry constants", effect.effectName);
				return false;
			}
			// FrameBuffer as the draw had it bound (PS b12, the engine's per-frame buffer), not CS's shared snapshot.
			const auto* frameSlot = Constants::Find(use, Constants::Stage::Pixel, 12);
			if (!frameSlot) {
				Flush();
				Refuse("no captured PerFrame constants", effect.effectName);
				return false;
			}
			const auto at = Constants::Where(*slot);
			const auto frameAt = Constants::Where(*frameSlot);
			if (!at.block || !frameAt.block) {
				Flush();
				Refuse("its constants have no ring chunk", effect.effectName);
				return false;
			}
			state.batch.push_back({ .entry = spec->entry, .at = { .srv = at.srvIndex, .offset = at.offset, .frameSrv = frameAt.srvIndex, .frameOffset = frameAt.offset } });
			state.keepAlive.push_back(at.block);
			state.keepAlive.push_back(frameAt.block);
			state.lastEntry = spec->entry;
			return true;
		}

		/** @brief Why an effect's Render cannot have its draw taken, or empty. */
		std::string_view Ineligible(const void* a_effect)
		{
			if (!Constants::Active())
				return "the engine-constants capture is not active (CS_ORG_ENGINE_CONSTANTS=1)";
			// ImageSpaceEffect+0x88: the effect keeps its own viewport (BSImagespaceShader::Render skips UpdateViewPort).
			if (*reinterpret_cast<const bool*>(static_cast<const std::byte*>(a_effect) + 0x88))
				return "a custom viewport";
			const auto* manager = reinterpret_cast<const std::byte*>(RE::ImageSpaceManager::GetSingleton());
			if (!manager)
				return "no ImageSpaceManager";
			// +0x10/+0x14: the rectangle Render sets as the viewport (menu 3D; -1: none). +0x58: the dynamic-resolution quad.
			if (*reinterpret_cast<const std::int32_t*>(manager + 0x10) != -1 && *reinterpret_cast<const std::int32_t*>(manager + 0x14) != -1)
				return "a sub-rectangle of its target";
			if (*reinterpret_cast<const bool*>(manager + 0x58))
				return "dynamic resolution";
			return {};
		}

		/** @brief A scope begins: a_scope orders its draws' passes after the earlier scopes' (0 the composites, 1 the HDR effect). */
		void EnterScope(std::uint32_t a_scope)
		{
			if (!Usable())
				return;
			BeginFrame();
			auto& state = IS();
			if (state.scopeDepth++ == 0) {
				state.scope = a_scope;
				state.drawIndex = 0;
			}
		}

		void LeaveScope()
		{
			auto& state = IS();
			if (state.scopeDepth == 0)
				return;
			if (--state.scopeDepth == 0 && FrameAnchors::OnRenderThread()) {
				Flush();
				Join();
			}
		}

		/** @brief BSImagespaceShader::Render (100950 / 107732): every raster ImageSpace shader's draw, through DrawTriShape. */
		struct ShaderRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				auto& state = IS();
				if (state.scopeDepth == 0 || !a_effect || !Usable()) {
					func(a_effect, a_quad, a_param);
					return;
				}
				// The ImageSpaceEffect base sits at +0x90 of a BSImagespaceShader (SE and AE: Render's this - 0x90).
				const auto* shader = static_cast<const RE::BSImagespaceShader*>(static_cast<const RE::ImageSpaceEffect*>(a_effect));
				const auto index = EffectOf(shader);
				EffectScope scope{ .shader = shader, .effect = index >= 0 ? static_cast<std::size_t>(index) : 0 };
				if (index >= 0 && state.enabled[static_cast<std::size_t>(index)]) {
					const auto reason = Ineligible(a_effect);
					scope.eligible = reason.empty();
					if (!scope.eligible)
						Refuse(reason, kEffects[scope.effect].effectName);
				}
				const auto previous = state.current;
				state.current = scope;
				func(a_effect, a_quad, a_param);
				if (state.current->eligible && !state.current->drew && state.loggedRefusals.insert("no DrawTriShape").second)
					logger::warn("[ORG] ImageSpace: {} drew without DrawTriShape; its draws stay the engine's", kEffects[scope.effect].effectName);
				state.current = previous;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceEffectHDR::Render (100465 / 107183): the downsample, luminance, adaptation, bloom and tonemap draws. */
		struct HDRRender
		{
			static void thunk(void* a_effect, void* a_quad, void* a_param)
			{
				EnterScope(1);
				func(a_effect, a_quad, a_param);
				LeaveScope();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceManager::RenderPreTonemapComposites (99024 / 105675): SIL, the lens flares, the VL / lens flare composite. */
		struct PreTonemapComposites
		{
			static void thunk(void* a_manager, std::uintptr_t a_source, std::uintptr_t a_destination)
			{
				EnterScope(0);
				func(a_manager, a_source, a_destination);
				LeaveScope();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSGraphics::Renderer::DrawTriShape (75477 / 77263): the current effect's draw is taken; any other runs the batch first. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, void* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				auto& state = IS();
				if (state.phase != Phase::Off && !state.flushing && FrameAnchors::OnRenderThread()) {
					if (state.scopeDepth > 0 && state.current && state.current->eligible && !state.current->drew) {
						if (TakeDraw())
							return;
					} else
						Flush();
				}
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSGraphics::Renderer::DispatchCSShader (75532 / 77329): an engine dispatch runs the batch first. */
		struct DispatchCSShader
		{
			static void thunk(void* a_renderer, void* a_shader, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z)
			{
				const auto& state = IS();
				if (state.phase != Phase::Off && !state.batch.empty() && FrameAnchors::OnRenderThread())
					Flush();
				func(a_renderer, a_shader, a_x, a_y, a_z);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		const char* PhaseName(Phase a_phase)
		{
			switch (a_phase) {
			case Phase::Learning:
				return "learning";
			case Phase::Joining:
				return "joining";
			case Phase::Live:
				return "live";
			default:
				return "off";
			}
		}
	}

	void Install()
	{
		if (!Requested())
			return;
		stl::detour_thunk<ShaderRender>(REL::RelocationID(100950, 107732));
		stl::detour_thunk<HDRRender>(REL::RelocationID(100465, 107183));
		stl::detour_thunk<PreTonemapComposites>(REL::RelocationID(99024, 105675));
		stl::detour_thunk<DrawTriShape>(REL::RelocationID(75477, 77263));
		stl::detour_thunk<DispatchCSShader>(REL::RelocationID(75532, 77329));
		auto& state = IS();
		state.installed = ShaderRender::func.address() && HDRRender::func.address() && PreTonemapComposites::func.address() && DrawTriShape::func.address() &&
		                  DispatchCSShader::func.address();
		if (state.installed)
			logger::info("[ORG] ImageSpace: hooks installed (BSImagespaceShader::Render, the HDR effect, the pre-tonemap composites, DrawTriShape, DispatchCSShader)");
		else
			logger::error("[ORG] ImageSpace: a hook could not be installed; the ImageSpace chain stays on D3D11");
	}

	void Reserve()
	{
		if (Requested())
			IS().reserved = FrameAnchors::Reserve(Anchor::ImageSpace);
	}

	void Setup()
	{
		auto& state = IS();
		if (!Requested() || state.phase != Phase::Off)
			return;
		const auto off = [](std::string_view a_why) { logger::warn("[ORG] ImageSpace: CS_ORG_IMAGESPACE=1, but {}; the ImageSpace chain stays on D3D11", a_why); };
		if (!state.installed)
			return off("its hooks are not installed");
		if (!state.reserved || !FrameAnchors::HasEpoch(Anchor::ImageSpace))
			return off("the ImageSpace anchor has no epoch");
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !RenderGraphRuntime::Get().IsActive())
			return off("the render graph is not active");
		if (!Constants::Requested())
			return off("the engine-constants capture is off (set CS_ORG_ENGINE_CONSTANTS=1)");
		// The ORG builds read PerGeometry through this header. Shaders staged from before the ORG builds (no header) compile their
		// D3D11 declarations under DXC, and every pipeline then fails on an unmapped cbuffer (PerFrame at b12 first).
		if (std::error_code error; !std::filesystem::exists(std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / "Common/OrgEngineConstants.hlsli", error))
			return off("Data/Shaders has no Common/OrgEngineConstants.hlsli: the staged shaders predate the ORG builds of ISHDR, ISBlur and the VL composite");
		state.gate = RunGate::Create("ImageSpace", kGateEntries);
		state.latch = org::LatchBlock::Create("ImageSpace::ConstantsAt", kGateEntries * static_cast<std::uint32_t>(sizeof(ConstantsAt)), host->FrameSlots());
		if (!state.gate || !state.latch)
			return off("its run gate or latch could not be made");
		SelectEffects();
		state.logShapes = Switches::On("CS_ORG_IMAGESPACE_LOG");
		state.phase = Phase::Learning;
		std::string selected;
		for (std::size_t i = 0; i < kEffects.size(); ++i)
			if (state.enabled[i])
				selected += fmt::format("{}{}", selected.empty() ? "" : ",", kEffects[i].effectName);
		logger::info("[ORG] ImageSpace: on (CS_ORG_IMAGESPACE); effects {}; the client registers after {} frames without a new shape",
			selected.empty() ? "none" : selected, kLearnFrames);
	}

	std::string Status()
	{
		const auto& state = IS();
		return fmt::format("ImageSpace: {}, {} passes, {} candidates, {} registrations, {} draws taken in {} batches, {} left to D3D11", PhaseName(state.phase),
			state.specs.size(), state.candidates.size(), state.registrations, state.taken, state.batches, state.engineDrawn);
	}
}

#else  // !CS_HAS_RENDER_GRAPH

namespace RenderGraph::Engine::ImageSpace
{
	void Install()
	{
		if (Requested())
			logger::warn("[ORG] ImageSpace: CS_ORG_IMAGESPACE=1, but this build has no render graph");
	}
	void Reserve() {}
	void Setup() {}
	std::string Status() { return "ImageSpace: no render graph in this build"; }
}

#endif
