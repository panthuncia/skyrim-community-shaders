#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGFxRuntime.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

#include <cstring>

bool ORGFxRuntime::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

ORGFxRuntime& ORGFxRuntime::Get()
{
	static ORGFxRuntime s_runtime;
	return s_runtime;
}

#if defined(CS_HAS_RENDER_GRAPH)

#	include "FxLower.h"
#	include "FxParser.h"

#	include "Features/Effects11.h"
#	include "Features/Effects11/EffectManager.h"
#	include "Features/Effects11/SettingManager.h"
#	include "Features/Effects11/TextureManager.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Engine/TonemapAnchor.h"
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ProgramCache.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <chrono>
#	include <functional>
#	include <map>
#	include <mutex>
#	include <optional>
#	include <set>
#	include <span>
#	include <string_view>
#	include <tuple>
#	include <type_traits>
#	include <unordered_map>
#	include <variant>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.effects11";
	constexpr std::int32_t kFirstOrder = 100;
	constexpr std::uint32_t kRegionAlignment = 16;

	static_assert(OrgFx::kMaxPushWords == ShaderBindings::kMaxPushWords, "OrgFx::kMaxPushWords must match ShaderBindings::kMaxPushWords");

	using Res = FrameAnchors::Texture;
	using Lifetime = GraphResources::Lifetime;
	namespace AP = RenderGraph::AnchorPasses;
	namespace RF = RenderGraph::Frame;

	/** @brief In the order ExecuteEffects runs them. */
	enum class Fx : std::uint8_t
	{
		DepthOfField,
		Bloom,
		Lens,
		Adaptation,
		Effect,
		PostPass,
		Count
	};
	constexpr std::size_t kEffects = static_cast<std::size_t>(Fx::Count);

	/** @brief Graph textures standing in for TextureManager's and the effects' own. Size 0 means the screen's. */
	struct TextureSpec
	{
		std::string_view name;
		std::uint32_t size;
		DXGI_FORMAT format;
		Lifetime lifetime;
		bool uav = false;
	};

	// Textures a later effect reads even when their chain did not run are History, since FX11 then reads last frame's.
	// The others are written before every read and may alias.
	constexpr TextureSpec kTextures[] = {
		// FX11's kMAIN, colour corrected in place. Engine targets need not have a UAV, so use our own.
		{ "Original", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::Transient, true },
		{ "SDRTemp", 0, DXGI_FORMAT_R10G10B10A2_UNORM, Lifetime::Transient },
		{ "SDRTemp2", 0, DXGI_FORMAT_R10G10B10A2_UNORM, Lifetime::Transient },
		{ "Scratch", 0, DXGI_FORMAT_R10G10B10A2_UNORM, Lifetime::Transient },
		{ "HDRTemp", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "HDRTemp2", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "Lens", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "Bloom", 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "BloomTemp", 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		// FX11 binds TextureManager's 1024 texture and its mips only as single-mip views, so they are three textures.
		{ "Downsampled", 1024, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "Downsampled512", 512, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "Downsampled256", 256, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "SunMasked", ENBAdaptation::kSunMaskSize, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "AdaptationCurrent", 16, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "Adaptation", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::Transient },
		{ "AdaptationHistory", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		// FX11 swaps each pair by frame parity. Here the history copies are made after the frame's write.
		// All kept, since a technique the effect lacks leaves its texture as it was.
		{ "DofAperture", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "DofApertureHistory", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "DofReadFocus", 16, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "DofFocus", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "DofFocusHistory", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		// What FX11 reads through a null view.
		{ "Black", 1, DXGI_FORMAT_R8G8B8A8_UNORM, Lifetime::Persistent },
		{ "RenderTargetRGBA32", 0, DXGI_FORMAT_R8G8B8A8_UNORM, Lifetime::History },
		{ "RenderTargetRGBA64", 0, DXGI_FORMAT_R16G16B16A16_UNORM, Lifetime::History },
		{ "RenderTargetRGBA64F", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTargetR16F", 0, DXGI_FORMAT_R16_FLOAT, Lifetime::History },
		{ "RenderTargetR32F", 0, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "RenderTargetRGB32F", 0, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::History },
		{ "RenderTarget1024", 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTarget512", 512, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTarget256", 256, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTarget128", 128, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTarget64", 64, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTarget32", 32, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "RenderTarget16", 16, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
	};

	const TextureSpec* FindSpec(std::string_view a_name)
	{
		for (const auto& spec : kTextures)
			if (spec.name == a_name)
				return &spec;
		return nullptr;
	}

	bool IsRenderTargetName(std::string_view a_name)
	{
		return a_name.starts_with("RenderTarget") && FindSpec(a_name);
	}

	/**
	* @brief Resolves a RenderTarget annotation as Effect::GetRenderTargetView does.
	* Empty when FX11 finds no texture by that name and so draws to the chain's output.
	*/
	std::string_view RenderTargetOf(std::string_view a_name, std::span<const std::pair<std::string_view, std::string_view>> a_own)
	{
		if (IsRenderTargetName(a_name))
			return a_name;
		for (const auto& [fx11, graph] : a_own)
			if (fx11 == a_name)
				return graph;
		// FX11's swaps between these, such as bloom and adaptation, are fixed textures here, so a write lands where the plan names first.
		static constexpr std::pair<std::string_view, std::string_view> kCommon[] = {
			{ "TextureHDRTemp", "HDRTemp" },
			{ "TextureHDRTemp2", "HDRTemp2" },
			{ "TextureSDRTemp", "SDRTemp" },
			{ "TextureSDRTemp2", "SDRTemp2" },
			{ "TextureBloom", "Bloom" },
			{ "TextureBloomTemp", "BloomTemp" },
			{ "TextureLens", "Lens" },
			{ "TextureAdaptation", "Adaptation" },
			{ "TextureAdaptationSwap", "AdaptationHistory" },
		};
		for (const auto& [fx11, graph] : kCommon)
			if (fx11 == a_name)
				return graph;
		return {};
	}

	/** @brief The display size of a revision's values. Before the first display size, the engine's screen. */
	RF::Vars::Extent2D DisplayOf(const RF::Inputs& a_in)
	{
		auto display = a_in.Get(RF::Vars::DisplaySize());
		if (!display.width || !display.height) {
			const auto* graphics = globals::game::graphicsState;
			display = { static_cast<std::uint32_t>(graphics->screenWidth), static_cast<std::uint32_t>(graphics->screenHeight) };
		}
		return display;
	}

	/** @brief A texture's row in kTextures. */
	std::uint32_t RowOf(const TextureSpec& a_spec)
	{
		return static_cast<std::uint32_t>(&a_spec - kTextures);
	}

	constexpr std::uint32_t kTextureRows = static_cast<std::uint32_t>(std::size(kTextures));

	/**
	* @brief The graph textures, declared on first use and kept for the session (frame revision plan, 6.11).
	* A screen-size one is declared on DisplaySize: a new display size is a new version of it, adopted with a frame revision.
	*/
	struct Textures
	{
		std::array<RF::DeclaredTexture2D, kTextureRows> declared{};

		/** @brief Render thread. Null when the texture cannot be made. */
		GraphResources::Texture2D* Get(const TextureSpec& a_spec)
		{
			auto& texture = declared[RowOf(a_spec)];
			if (texture)
				return texture.Get();
			const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (a_spec.uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
			const D3D11_TEXTURE2D_DESC base{ .Width = 1,
				.Height = 1,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = a_spec.format,
				.SampleDesc = { .Count = 1 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = bind };
			texture = RF::DeclareTexture2D(
				fmt::format("Effects11::{}", a_spec.name), RF::DependsOn(RF::Vars::DisplaySize()),
				[base, size = a_spec.size](const RF::Inputs& a_in) {
					const auto display = DisplayOf(a_in);
					auto desc = base;
					desc.Width = size ? size : display.width;
					desc.Height = size ? size : display.height;
					return desc;
				},
				GraphResources::Options{ .lifetime = a_spec.lifetime });
			if (!texture || !*texture) {
				texture = {};
				return nullptr;
			}
			return texture.Get();
		}
	};

	/** @brief Copied into a pass's region every frame. */
	struct ValueCopy
	{
		ID3DX11EffectVariable* variable = nullptr;
		std::uint32_t offset = 0;
		std::uint32_t size = 0;
	};

	/**
	* @brief FX11's Apply sets only the states a pass names, so a pass that names none keeps the last one set.
	* Depth-stencil state does nothing here because Effects11 binds no depth view.
	*/
	struct PassState
	{
		rhi::BlendAttachment blend{};
		bool alphaToCoverage = false;
		std::array<float, 4> blendFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
		std::uint32_t sampleMask = 0xFFFFFFFF;
		rhi::RasterState raster{ .cull = rhi::CullMode::None };

		bool ReadsFactor() const
		{
			const auto reads = [](rhi::BlendFactor a_factor) { return a_factor == rhi::BlendFactor::BlendFactor || a_factor == rhi::BlendFactor::InvBlendFactor; };
			return blend.enable && (reads(blend.srcColor) || reads(blend.dstColor) || reads(blend.srcAlpha) || reads(blend.dstAlpha));
		}

		std::string Key() const
		{
			return fmt::format("{} {} {} {} {} {} {} {} {}|{} {} {} {}", blend.enable, static_cast<int>(blend.srcColor), static_cast<int>(blend.dstColor),
				static_cast<int>(blend.colorOp), static_cast<int>(blend.srcAlpha), static_cast<int>(blend.dstAlpha), static_cast<int>(blend.alphaOp),
				static_cast<int>(blend.writeMask), alphaToCoverage, static_cast<int>(raster.fill), static_cast<int>(raster.cull), raster.frontCCW, raster.depthClip);
		}
	};

	/** @brief One pass's lowered source, kept with the effect so a plan lowers each pass once. */
	struct LoweredText
	{
		std::string source;
		std::size_t hash = 0;  // of source, for the program's key
		std::uint32_t pushWords = 0;
		bool ok = false;
		std::string error;
	};

	struct FxEffect
	{
		EffectBase* fx11 = nullptr;
		winrt::com_ptr<ID3DX11Effect> compiled;
		bool built = false;
		bool ok = false;
		std::string reason;
		OrgFx::EffectDesc desc;
		OrgFx::LoweredEffect lowered;
		std::vector<ValueCopy> values;
		std::int32_t screenSize = -1;
		std::uint32_t screenSizeBytes = 0;
		std::vector<org::OwnedDescriptorBinding> samplers;  // per resource slot, empty for textures
		std::vector<Res> files;                             // per resource slot, its ResourceName texture or null
		std::unordered_map<std::string, std::uint32_t> techniques;
		std::map<std::pair<std::uint32_t, std::uint32_t>, LoweredText> passes;  // by technique and pass

		std::string Name() const { return fx11 ? fx11->GetName() : std::string("?"); }
	};

	// The UAV, then the latch.
	struct ColorCorrectionBlock
	{
		ShaderBindings::Handle outputTexture;
		ShaderBindings::Latch colorCorrection;
	};

	struct ColorCorrectionConstants
	{
		float brightness;
		float gammaCurve;
		std::uint32_t frameCount;
		std::uint32_t pad;
	};

	struct DitherConstants
	{
		std::uint32_t frameCount;
		float amplitude;
		std::uint32_t pad[2];
	};

	constexpr std::uint32_t kDownsampleWords = 2;
	constexpr std::uint32_t kSunMaskWords = 4;
	constexpr std::uint32_t kCopyWords = 3;

	// CS's own programs in a plan, by key. The .fx passes' keys start with "fx|".
	constexpr std::string_view kColorCorrectionKey = "cs|color-correction";
	constexpr std::string_view kDownsampleKey = "cs|downsample";
	constexpr std::string_view kSunMaskKey = "cs|sun-mask";

	ComputeProgram::Desc ColorCorrectionDesc()
	{
		return { .source = "Effects11/ColorCorrectionCS.hlsl", .constantWords = ShaderBindings::Words<ColorCorrectionBlock>() };
	}

	RasterProgram::Desc CsRasterDesc(const char* a_source, std::uint32_t a_words, rhi::Format a_format)
	{
		RasterProgram::Desc desc{};
		desc.pixelSource = a_source;
		desc.constantWords = a_words;
		desc.targets = { { .format = a_format } };
		return desc;
	}

	RasterProgram::Desc DownsampleDesc() { return CsRasterDesc("Effects11/DownsamplePS.hlsl", kDownsampleWords, rhi::Format::R11G11B10_Float); }
	RasterProgram::Desc SunMaskDesc() { return CsRasterDesc("Effects11/AdaptationSunMaskPS.hlsl", kSunMaskWords, rhi::Format::R11G11B10_Float); }
	RasterProgram::Desc CopyDesc(rhi::Format a_format) { return CsRasterDesc("Effects11/CopyPS.hlsl", kCopyWords, a_format); }
	std::string CopyKey(rhi::Format a_format) { return fmt::format("cs|copy|{}", static_cast<int>(a_format)); }

	/** @brief Logs the reason and stops the game: an error of the FX chain on the render graph has no fallback. */
	[[noreturn]] void Fatal(std::string_view a_what)
	{
		const auto message = fmt::format("Community Shaders: Effects11's FX chain failed on the render graph: {}", a_what);
		logger::critical("[ORG] Effects11: {}", message);
		spdlog::default_logger()->flush();
		stl::report_and_fail(message);
	}

	/** @brief Resolved at record time. */
	struct PushWord
	{
		enum class Kind : std::uint8_t
		{
			Literal,
			Input,        // heap index of the pass's input number value
			LatchHandle,
			LatchOffset,  // the run's region offset plus value
		};
		Kind kind = Kind::Literal;
		std::uint32_t value = 0;
	};

	/** @brief Its own pass because it draws Effects11's six-vertex quad, and one texture may fill several slots as one declared use. */
	struct DrawSpec
	{
		std::string name;
		std::shared_ptr<const RasterProgram> program;
		Res target;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::vector<Res> inputs;
		std::vector<PushWord> push;
		std::uint32_t vertexCount = 3;
		std::vector<org::OwnedDescriptorBinding> samplers;  // kept while the pass lives
		std::optional<std::array<float, 4>> blendFactor;    // set before the draw when the blend reads it

		std::uint32_t Input(const Res& a_resource)
		{
			const auto it = std::ranges::find(inputs, a_resource);
			if (it != inputs.end())
				return static_cast<std::uint32_t>(it - inputs.begin());
			inputs.push_back(a_resource);
			return static_cast<std::uint32_t>(inputs.size() - 1);
		}
	};

	/**
	* @brief Cleared through a render target, since these textures have no UAV for AnchorPasses::Clear. Recorded at execution,
	* it clears only on runs whose commit opened its row of the clear gate.
	*/
	struct ClearSpec
	{
		std::string name;
		Res texture;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t row = 0;  // the texture's row in kTextures, its entry in the clear gate
	};

	using Step = std::variant<std::shared_ptr<const DrawSpec>, AP::Draw, AP::Dispatch, AP::Copy, ClearSpec>;

	struct RegionWrite
	{
		enum class Kind : std::uint8_t
		{
			FxValues,         // with the pass's ScreenSize
			ColorCorrection,
			SunMask,
			Dither,
		};
		Kind kind = Kind::FxValues;
		Fx effect = Fx::Effect;
		std::uint32_t offset = 0;
		float screenSize[4]{};
		float amplitude = 0.0f;
	};

	/** @brief Taken at plan time, because a reload may reset the runtime's effects while an older shape still runs. */
	struct EffectValues
	{
		EffectBase* fx11 = nullptr;
		winrt::com_ptr<ID3DX11Effect> compiled;  // keeps the variables below alive
		std::vector<ValueCopy> values;
		std::uint32_t valuesSize = 0;
		std::int32_t screenSize = -1;
		std::uint32_t screenSizeBytes = 0;
	};

	struct Frame
	{
		std::vector<Step> steps;
		std::vector<std::string> stepNames;
		std::vector<RegionWrite> writes;
		std::shared_ptr<org::LatchBlock> latch;
		std::uint32_t latchBytes = 0;
		std::array<bool, kEffects> runs{};
		std::array<EffectValues, kEffects> effects{};
		Res input;
		Res output;
		std::uint32_t width = 0;   // the display size planned for
		std::uint32_t height = 0;
		// Every texture the steps use, by kTextures row. The kept ones are cleared on the first run of each new version.
		std::vector<std::pair<std::uint32_t, GraphResources::Texture2D*>> textures;
		std::vector<std::pair<std::uint32_t, GraphResources::Texture2D*>> clears;
		std::uint32_t fxPasses = 0;
	};

	// ---- passes ----

	struct DrawBindings
	{
		org::DeclaredViewToken target;
		std::vector<org::DeclaredViewToken> inputs;
	};

	struct DrawFrame
	{
		std::shared_ptr<const DrawSpec> spec;
		std::shared_ptr<const org::LatchBlock> latch;
	};

	class DrawPass final : public org::TypedRenderGraphPass<DrawPass, DrawFrame, DrawBindings>
	{
	public:
		DrawPass(std::shared_ptr<const DrawSpec> a_spec, std::shared_ptr<const org::LatchBlock> a_latch) :
			spec(std::move(a_spec)), latch(std::move(a_latch)) {}

		DrawBindings Declare(org::PassBuilder& a_builder)
		{
			DrawBindings bindings{};
			bindings.target = a_builder.RenderTarget(spec->target).View();
			bindings.inputs.reserve(spec->inputs.size());
			for (const auto& input : spec->inputs)
				bindings.inputs.push_back(a_builder.ShaderResource(input).View());
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(spec->program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
		}

		DrawFrame Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { spec, latch }; }

		static void Record(const DrawBindings& a_bindings, const DrawFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.spec || !a_frame.spec->program || !a_frame.latch)
				return;
			const auto& draw = *a_frame.spec;
			std::array<std::uint32_t, ShaderBindings::kMaxPushWords> words{};
			const std::uint32_t count = (std::min)(static_cast<std::uint32_t>(draw.push.size()), ShaderBindings::kMaxPushWords);
			const auto region = a_frame.latch->At(a_recording.Run());
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& word = draw.push[i];
				switch (word.kind) {
				case PushWord::Kind::Literal:
					words[i] = word.value;
					break;
				case PushWord::Kind::Input:
					words[i] = a_recording.Index(a_bindings.inputs[word.value]);
					break;
				case PushWord::Kind::LatchHandle:
					words[i] = region.srvIndex;
					break;
				case PushWord::Kind::LatchOffset:
					words[i] = static_cast<std::uint32_t>(region.offset) + word.value;
					break;
				}
			}
			if (draw.blendFactor)
				a_recording.Commands().SetBlendFactor(draw.blendFactor->data());
			const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = rhi::LoadOp::Load };
			draw.program->Draw(a_recording, { .colours = { &colour, 1 },
												.viewport = { .width = draw.width, .height = draw.height },
												.constants = { words.data(), count },
												.vertexCount = draw.vertexCount,
												.debugName = draw.name.c_str() });
		}

	private:
		std::shared_ptr<const DrawSpec> spec;
		std::shared_ptr<const org::LatchBlock> latch;
	};

	struct ClearBindings
	{
		org::DeclaredViewToken target;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	struct ClearFrame
	{
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t row = 0;
	};

	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		ClearPass(ClearSpec a_spec, std::shared_ptr<const RenderGraph::RunGate> a_gate) :
			spec(std::move(a_spec)), gate(std::move(a_gate)) {}

		ClearBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(spec.texture).View(), spec.width, spec.height }; }

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.texture.Key()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.get()));
			a_out.push_back(spec.row);
			a_out.push_back(spec.width);
			a_out.push_back(spec.height);
		}

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { gate, spec.row }; }

		// Its commands depend on the run only through SkipsAtExecution.
		static constexpr bool kRecordKeyedBySkip = true;

		/** @brief A clear whose row the run's commit left closed is not recorded. */
		static bool SkipsAtExecution(const ClearBindings&, const ClearFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return a_frame.gate && !a_frame.gate->IsOpen(a_recording.Run(), a_frame.row);
		}

		static void Record(const ClearBindings& a_bindings, const ClearFrame&, org::PassRecordContext& a_recording)
		{
			rhi::ColorAttachment attachment{};
			attachment.rtv = a_recording.View(a_bindings.target);
			attachment.resource = a_recording.Resolve(a_bindings.target).GetHandle();
			attachment.loadOp = rhi::LoadOp::Clear;
			attachment.storeOp = rhi::StoreOp::Store;
			attachment.clear.type = rhi::ClearValueType::Color;
			rhi::PassBeginInfo begin{};
			begin.colors = { &attachment, 1 };
			begin.width = a_bindings.width;
			begin.height = a_bindings.height;
			begin.debugName = "Effects11::Clear";
			auto& commands = a_recording.Commands();
			commands.BeginPass(begin);
			commands.EndPass();
		}

	private:
		ClearSpec spec;
		std::shared_ptr<const RenderGraph::RunGate> gate;
	};

	// ---- helpers ----

	/** @brief The D3D11 description FX11 makes of a parsed sampler. */
	D3D11_SAMPLER_DESC D3D11SamplerOf(const OrgFx::SamplerDesc& a_sampler)
	{
		const auto type = [](OrgFx::FilterMode a_mode) { return a_mode == OrgFx::FilterMode::Point ? D3D11_FILTER_TYPE_POINT : D3D11_FILTER_TYPE_LINEAR; };
		const auto address = [](OrgFx::AddressMode a_mode) {
			switch (a_mode) {
			case OrgFx::AddressMode::Wrap:
				return D3D11_TEXTURE_ADDRESS_WRAP;
			case OrgFx::AddressMode::Mirror:
				return D3D11_TEXTURE_ADDRESS_MIRROR;
			case OrgFx::AddressMode::Border:
				return D3D11_TEXTURE_ADDRESS_BORDER;
			case OrgFx::AddressMode::MirrorOnce:
				return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
			default:
				return D3D11_TEXTURE_ADDRESS_CLAMP;
			}
		};
		D3D11_FILTER_REDUCTION_TYPE reduction = D3D11_FILTER_REDUCTION_TYPE_STANDARD;
		switch (a_sampler.reduction) {
		case OrgFx::FilterReduction::Comparison:
			reduction = D3D11_FILTER_REDUCTION_TYPE_COMPARISON;
			break;
		case OrgFx::FilterReduction::Minimum:
			reduction = D3D11_FILTER_REDUCTION_TYPE_MINIMUM;
			break;
		case OrgFx::FilterReduction::Maximum:
			reduction = D3D11_FILTER_REDUCTION_TYPE_MAXIMUM;
			break;
		default:
			break;
		}
		D3D11_SAMPLER_DESC desc{};
		desc.Filter = a_sampler.anisotropic ? D3D11_ENCODE_ANISOTROPIC_FILTER(reduction) :
		                                      D3D11_ENCODE_BASIC_FILTER(type(a_sampler.minFilter), type(a_sampler.magFilter), type(a_sampler.mipFilter), reduction);
		desc.AddressU = address(a_sampler.addressU);
		desc.AddressV = address(a_sampler.addressV);
		desc.AddressW = address(a_sampler.addressW);
		desc.MipLODBias = a_sampler.mipLodBias;
		desc.MaxAnisotropy = a_sampler.maxAnisotropy;
		// The parser's CompareFunc is D3D11_COMPARISON_FUNC minus one.
		desc.ComparisonFunc = static_cast<D3D11_COMPARISON_FUNC>(static_cast<int>(a_sampler.compare) + 1);
		std::copy(a_sampler.borderColor.begin(), a_sampler.borderColor.end(), desc.BorderColor);
		desc.MinLOD = a_sampler.minLod;
		desc.MaxLOD = a_sampler.maxLod;
		return desc;
	}

	rhi::SamplerDesc SamplerOf(const OrgFx::SamplerDesc& a_sampler)
	{
		auto desc = ShaderBindings::SamplerFromD3D11(D3D11SamplerOf(a_sampler));
		// D3D11's default minimum is -FLT_MAX, but there is nothing below mip 0.
		desc.minLod = (std::max)(desc.minLod, 0.0f);
		desc.maxLod = (std::max)(desc.maxLod, desc.minLod);
		return desc;
	}

	rhi::BlendFactor BlendFactorOf(OrgFx::BlendFactor a_factor)
	{
		using F = OrgFx::BlendFactor;
		switch (a_factor) {
		case F::Zero:
			return rhi::BlendFactor::Zero;
		case F::One:
			return rhi::BlendFactor::One;
		case F::SrcColor:
			return rhi::BlendFactor::SrcColor;
		case F::InvSrcColor:
			return rhi::BlendFactor::InvSrcColor;
		case F::SrcAlpha:
			return rhi::BlendFactor::SrcAlpha;
		case F::InvSrcAlpha:
			return rhi::BlendFactor::InvSrcAlpha;
		case F::DestAlpha:
			return rhi::BlendFactor::DstAlpha;
		case F::InvDestAlpha:
			return rhi::BlendFactor::InvDstAlpha;
		case F::DestColor:
			return rhi::BlendFactor::DstColor;
		case F::InvDestColor:
			return rhi::BlendFactor::InvDstColor;
		case F::SrcAlphaSat:
			return rhi::BlendFactor::SrcAlphaSat;
		case F::Factor:
			return rhi::BlendFactor::BlendFactor;
		case F::InvFactor:
			return rhi::BlendFactor::InvBlendFactor;
		case F::Src1Color:
			return rhi::BlendFactor::Src1Color;
		case F::InvSrc1Color:
			return rhi::BlendFactor::InvSrc1Color;
		case F::Src1Alpha:
			return rhi::BlendFactor::Src1Alpha;
		case F::InvSrc1Alpha:
			return rhi::BlendFactor::InvSrc1Alpha;
		}
		return rhi::BlendFactor::One;
	}

	rhi::BlendOp BlendOpOf(OrgFx::BlendOp a_op)
	{
		switch (a_op) {
		case OrgFx::BlendOp::Subtract:
			return rhi::BlendOp::Sub;
		case OrgFx::BlendOp::RevSubtract:
			return rhi::BlendOp::RevSub;
		case OrgFx::BlendOp::Min:
			return rhi::BlendOp::Min;
		case OrgFx::BlendOp::Max:
			return rhi::BlendOp::Max;
		default:
			return rhi::BlendOp::Add;
		}
	}

	/** @brief NULL means D3D11's default state. */
	bool IsNullState(std::string_view a_name)
	{
		return a_name == "NULL" || a_name == "0" || a_name == "nullptr";
	}

	/** @brief Applies a pass's named states on top of a_state. False when a state is missing or the pass sets FX9-style render states. */
	bool ApplyPassStates(const OrgFx::EffectDesc& a_desc, const OrgFx::Pass& a_pass, PassState& a_state, std::string& a_reason)
	{
		if (!a_pass.otherStates.empty()) {
			a_reason = fmt::format("pass '{}' sets render state '{}' in the FX9 form", a_pass.name, a_pass.otherStates.front().key);
			return false;
		}
		if (!a_pass.blendState.empty()) {
			a_state.blend = {};
			a_state.alphaToCoverage = false;
			if (!IsNullState(a_pass.blendState)) {
				const auto* state = a_desc.FindState(a_pass.blendState);
				if (!state || state->kind != OrgFx::StateKind::Blend || state->arraySize) {
					a_reason = fmt::format("pass '{}' sets blend state '{}', which is not a blend state block", a_pass.name, a_pass.blendState);
					return false;
				}
				// With independent blending off D3D11 uses target 0's state anyway.
				const auto& target = state->blend.targets[0];
				a_state.blend.enable = target.enable;
				a_state.blend.srcColor = BlendFactorOf(target.src);
				a_state.blend.dstColor = BlendFactorOf(target.dest);
				a_state.blend.colorOp = BlendOpOf(target.op);
				a_state.blend.srcAlpha = BlendFactorOf(target.srcAlpha);
				a_state.blend.dstAlpha = BlendFactorOf(target.destAlpha);
				a_state.blend.alphaOp = BlendOpOf(target.opAlpha);
				a_state.blend.writeMask = static_cast<rhi::ColorWriteEnable>(target.writeMask & 0xF);
				a_state.alphaToCoverage = state->blend.alphaToCoverage;
			}
			a_state.blendFactor = a_pass.blendFactor;
			a_state.sampleMask = a_pass.sampleMask;
		}
		if (!a_pass.rasterizerState.empty()) {
			// D3D11's default rasterizer culls back faces, and Effects11's quad faces the front.
			a_state.raster = rhi::RasterState{};
			if (!IsNullState(a_pass.rasterizerState)) {
				const auto* state = a_desc.FindState(a_pass.rasterizerState);
				if (!state || state->kind != OrgFx::StateKind::Rasterizer || state->arraySize) {
					a_reason = fmt::format("pass '{}' sets rasterizer state '{}', which is not a rasterizer state block", a_pass.name, a_pass.rasterizerState);
					return false;
				}
				const auto& r = state->rasterizer;
				a_state.raster.fill = r.fill == OrgFx::FillMode::Wireframe ? rhi::FillMode::Wireframe : rhi::FillMode::Solid;
				a_state.raster.cull = r.cull == OrgFx::CullMode::None ? rhi::CullMode::None : (r.cull == OrgFx::CullMode::Front ? rhi::CullMode::Front : rhi::CullMode::Back);
				a_state.raster.frontCCW = r.frontCounterClockwise;
				a_state.raster.depthClip = r.depthClip;
				// Depth bias needs a depth target. Effects11 sets no scissor, and the render area covers the target.
			}
		}
		return true;
	}

	/** @brief $Globals when a_cbuffer is empty. Null when the pass has none. */
	const OrgFx::ValueRegion* RegionOf(const OrgFx::LoweredEffect& a_lowered, std::string_view a_cbuffer)
	{
		for (const auto& region : a_lowered.regions)
			if (region.cbuffer == a_cbuffer)
				return &region;
		return nullptr;
	}

	std::string FirstError(const std::vector<OrgFx::Diagnostic>& a_diagnostics)
	{
		for (const auto& d : a_diagnostics)
			if (d.severity == OrgFx::Severity::Error)
				return fmt::format("line {}: {}", d.line, d.message);
		return "unknown error";
	}

	EffectBase* Fx11Of(Fx a_fx)
	{
		auto& manager = EffectManager::GetSingleton();
		switch (a_fx) {
		case Fx::DepthOfField:
			return &manager.enbDepthOfField;
		case Fx::Bloom:
			return &manager.enbBloom;
		case Fx::Lens:
			return &manager.enbLens;
		case Fx::Adaptation:
			return &manager.enbAdaptation;
		case Fx::Effect:
			return &manager.enbEffect;
		case Fx::PostPass:
			return &manager.enbEffectPostPass;
		default:
			return nullptr;
		}
	}

	bool Runs(Fx a_fx)
	{
		auto& manager = EffectManager::GetSingleton();
		switch (a_fx) {
		case Fx::DepthOfField:
			return manager.WillEffectRun(manager.enbDepthOfField, manager.ids.useDepthOfField);
		case Fx::Bloom:
			return manager.WillEffectRun(manager.enbBloom, manager.ids.useBloom);
		case Fx::Lens:
			return manager.WillEffectRun(manager.enbLens, manager.ids.useLens);
		case Fx::Adaptation:
			return manager.WillEffectRun(manager.enbAdaptation, manager.ids.useAdaptation);
		case Fx::Effect:
			return manager.WillEffectRun(manager.enbEffect, 0xFFFFFFFF);
		case Fx::PostPass:
			return manager.WillEffectRun(manager.enbEffectPostPass, manager.ids.usePostPass);
		default:
			return false;
		}
	}

	/** @brief The selected technique, or DoF's fallback when it has none. */
	std::string ChainOf(Fx a_fx)
	{
		if (a_fx == Fx::DepthOfField)
			return EffectManager::GetSingleton().enbDepthOfField.GetChainTechnique();
		auto* fx11 = Fx11Of(a_fx);
		return fx11 ? fx11->GetSelectedTechnique() : std::string();
	}

	std::string TechniqueName(ID3DX11EffectTechnique* a_technique)
	{
		D3DX11_TECHNIQUE_DESC desc{};
		if (!a_technique || FAILED(a_technique->GetDesc(&desc)) || !desc.Name)
			return {};
		return desc.Name;
	}

	/**
	* @brief What the last tonemap found that is not a variable yet: its targets, kMAIN, the depth and FX11's compiled effects.
	* A change invalidates the client's key, so the next frame revision plans again (an interim, as Exponential Height Fog's
	* inputs, until the engine's targets are variables).
	*/
	struct Observed
	{
		Res input, output, main, depth;
		rhi::Format inputFormat = rhi::Format::Unknown;
		rhi::Format outputFormat = rhi::Format::Unknown;
		rhi::Format mainFormat = rhi::Format::Unknown;  // kMAIN's RTV
		std::uint32_t inputWidth = 0, inputHeight = 0;
		std::uint32_t outputWidth = 0, outputHeight = 0;
		std::uint32_t mainWidth = 0, mainHeight = 0;
		bool mainCopyable = false;  // kMAIN is R16G16B16A16_FLOAT with one sample, so Original copies into it
		std::array<const void*, kEffects> compiled{};  // each effect's ID3DX11Effect, null while it is not compiled
		bool ready = false;                            // the manager is initialized with a preset loaded

		bool operator==(const Observed&) const = default;
	};

	struct ChainInputs
	{
		Res input, main, depth, output;  // main is kMAIN, when it has an RTV for FX11's result
		bool inputCopies = false;        // same format and size as Original, so a plain copy
		bool mainCopies = false;
		rhi::Format mainFormat = rhi::Format::Unknown;
		std::uint32_t mainWidth = 0;
		std::uint32_t mainHeight = 0;
		rhi::Format outputFormat = rhi::Format::Unknown;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::array<bool, kEffects> runs{};
	};

	/** @brief One program of a plan: its key and its desc, with the defines resolved on the render thread. */
	struct PlannedProgram
	{
		std::string key;
		std::variant<RasterProgram::Desc, ComputeProgram::Desc> desc;
	};

	/** @brief The chain's declared program: every program one plan of the chain asks for. Equal keys are one plan. */
	struct FxPlanKey
	{
		std::uint64_t generation = 0;
		std::vector<std::string> keys;  // sorted
		std::shared_ptr<const std::vector<PlannedProgram>> programs;

		bool operator==(const FxPlanKey& a_other) const { return generation == a_other.generation && keys == a_other.keys; }
	};

	/** @brief The programs one plan lists, by key. Built on the programs lane, read by the chain's build. */
	struct FxPrograms
	{
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
	};

	/** @brief Every program built this session, by generation and key, so a plan builds only what is new. Any thread. */
	struct ProgramStore
	{
		std::mutex mutex;
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
	};

	/**
	* @brief The client's key: the chain's shape as text, what the build plans from, and the plan's programs. Equal when the text
	* and the generation are. The programs follow from those, so they are not compared.
	*/
	struct Shape
	{
		std::string text;
		std::uint64_t generation = 0;
		ChainInputs inputs;
		RF::Vars::Extent2D display{};
		std::shared_ptr<const FxPrograms> programs;

		bool operator==(const Shape& a_other) const { return text == a_other.text && generation == a_other.generation; }
	};
}

struct ORGFxRuntime::Impl
{
	std::array<FxEffect, kEffects> effects{};
	// TextureManager's linear clamp sampler.
	org::OwnedDescriptorBinding linearSampler;
	Textures textures;
	// A row per kTextures entry. A commit opens the rows of the kept textures whose bound version no run cleared yet.
	std::shared_ptr<RenderGraph::RunGate> clearGate;
	std::array<const void*, kTextureRows> clearedVersions{};
	std::vector<std::pair<std::uint32_t, const void*>> openedClears;  // by the last commit, cleared once its run is submitted
	// Declared at the first tonemap the graph can take, on the display size and Effects11's shape writes. Both keys read observed.
	RF::DeclaredClient client;
	RF::DeclaredProgram<FxPrograms> programs;
	std::shared_ptr<ProgramStore> store = std::make_shared<ProgramStore>();
	std::uint64_t generation = 0;       // Invalidate's count: a reload lists and builds every program again
	std::uint64_t buildGeneration = 0;  // the chain's builds, which HeldNow names
	// The last plan and the shape text it was made for, so an equal text does not plan again.
	std::optional<std::pair<std::string, FxPlanKey>> lastPlan;
	Observed observed;
	std::string lastReason;
	bool activeNow = false;
	bool declaredOwner = false;  // what the client's declared gave at the last Begin
	std::uint32_t ranFrame = UINT32_MAX;
	Res frameInput;
	Res frameOutput;
	std::vector<std::vector<std::uint8_t>> images = std::vector<std::vector<std::uint8_t>>(kEffects);
	std::array<std::uint32_t, kEffects> updatedFrame = [] {
		std::array<std::uint32_t, kEffects> frames{};
		frames.fill(UINT32_MAX);
		return frames;
	}();

	FxEffect& EffectOf(Fx a_fx) { return effects[static_cast<std::size_t>(a_fx)]; }

	/** @brief The graph cannot take this frame's tonemap, so FX11 runs it. A new reason is logged once. */
	void Inactive(const std::string& a_reason)
	{
		if (!a_reason.empty() && a_reason != lastReason)
			logger::warn("[ORG] Effects11 runs on FX11 this frame: {}", a_reason);
		lastReason = a_reason;
	}

	/** @brief False, with the reason kept, on failure. */
	bool BuildEffect(Fx a_fx)
	{
		auto& e = EffectOf(a_fx);
		e = FxEffect{};
		e.fx11 = Fx11Of(a_fx);
		e.built = true;
		auto fail = [&](std::string a_reason) {
			e.reason = fmt::format("{}: {}", e.Name(), a_reason);
			logger::warn("[ORG] Effects11: {} cannot run on the render graph: {}", e.Name(), a_reason);
			return false;
		};
		auto* fx11 = e.fx11;
		if (!fx11 || !fx11->IsCompiled() || !fx11->GetEffect())
			return fail("FX11 did not compile it");
		const auto& text = fx11->preprocessedSource;
		if (text.empty())
			return fail("no preprocessed text (an encrypted preset, or only FX11's raw-source fallback compiled it)");

		e.desc = OrgFx::Parse(text);
		if (e.desc.Count(OrgFx::Severity::Error))
			return fail("parse error, " + FirstError(e.desc.diagnostics));
		e.lowered = OrgFx::LowerEffect(text, e.desc);
		if (!e.lowered.ok)
			return fail("lowering failed, " + FirstError(e.lowered.diagnostics));

		// FX11 keeps the values, so its $Globals and cbuffers must match the lowered layout. Offsets are from each cbuffer's start.
		auto* effect = fx11->GetEffect();
		e.compiled.copy_from(effect);
		for (const auto& v : e.desc.variables) {
			if (v.bufferOffset < 0 || v.modifiers.isStatic)
				continue;
			const auto region = RegionOf(e.lowered, v.cbuffer);
			if (!region)
				return fail(fmt::format("'{}' is in cbuffer '{}', which the lowering has no region for", v.name, v.cbuffer));
			auto* variable = effect->GetVariableByName(v.name.c_str());
			if (!variable || !variable->IsValid())
				return fail(fmt::format("FX11 has no variable '{}'", v.name));
			D3DX11_EFFECT_VARIABLE_DESC variableDesc{};
			D3DX11_EFFECT_TYPE_DESC typeDesc{};
			if (FAILED(variable->GetDesc(&variableDesc)) || FAILED(variable->GetType()->GetDesc(&typeDesc)))
				return fail(fmt::format("FX11 cannot describe '{}'", v.name));
			if (variableDesc.BufferOffset != static_cast<std::uint32_t>(v.bufferOffset))
				return fail(fmt::format("'{}' is at byte {} of FX11's $Globals, {} of the parser's", v.name, variableDesc.BufferOffset, v.bufferOffset));
			const std::uint32_t offset = region->offset + static_cast<std::uint32_t>(v.bufferOffset);
			if (offset >= e.lowered.valuesSize)
				continue;
			const std::uint32_t size = (std::min)({ typeDesc.UnpackedSize, v.bufferSize, e.lowered.valuesSize - offset });
			if (size)
				e.values.push_back({ variable, offset, size });
		}
		if (const auto* screenSize = e.desc.FindVariable("ScreenSize"); screenSize && screenSize->bufferOffset >= 0 && RegionOf(e.lowered, screenSize->cbuffer)) {
			e.screenSize = static_cast<std::int32_t>(RegionOf(e.lowered, screenSize->cbuffer)->offset) + screenSize->bufferOffset;
			e.screenSizeBytes = (std::min)(screenSize->bufferSize, 16u);
		}

		const auto& slots = e.lowered.resources;
		e.samplers.resize(slots.size());
		e.files.resize(slots.size());
		for (std::size_t i = 0; i < slots.size(); ++i) {
			const auto& slot = slots[i];
			if (slot.kind == OrgFx::ResourceSlot::Kind::Sampler) {
				const OrgFx::SamplerDesc parsed = slot.state >= 0 ? e.desc.states[slot.state].sampler : OrgFx::SamplerDesc{};
				e.samplers[i] = ShaderBindings::CreateSampler(SamplerOf(parsed));
				if (!e.samplers[i])
					return fail(fmt::format("sampler '{}' could not be created", slot.name));
				continue;
			}
			if (slot.variable < 0)
				continue;
			const auto file = OrgFx::AnnotationText(e.desc.variables[slot.variable].annotations, "ResourceName");
			if (file.empty())
				continue;
			const auto cached = fx11->customTextureCache.find(file);
			if (cached == fx11->customTextureCache.end() || !cached->second)
				continue;  // FX11 could not load it either and binds nothing, so black
			winrt::com_ptr<ID3D11Resource> resource;
			cached->second->GetResource(resource.put());
			winrt::com_ptr<ID3D11Texture2D> texture;
			if (!resource || !resource.try_as(texture) || !texture)
				return fail(fmt::format("texture file '{}' is not a 2D texture", file));
			e.files[i] = FrameAnchors::ImmutableTexture(fmt::format("effects11.{}.{}", e.Name(), slot.name), texture.get(), cached->second.get());
			if (!e.files[i])
				return fail(fmt::format("texture file '{}' could not be imported", file));
		}

		for (std::uint32_t t = 0; t < e.desc.techniques.size(); ++t)
			e.techniques.emplace(e.desc.techniques[t].name, t);
		e.ok = true;
		logger::info("[ORG] Effects11: {} parsed and lowered for the render graph ({} techniques, {} resources, {} value bytes)", e.Name(),
			e.desc.techniques.size(), slots.size(), e.lowered.valuesSize);
		return true;
	}

	/** @brief A pass's lowered source, made once per effect build. CPU work only: the plan lists it and the lane compiles it. */
	const LoweredText& LoweredPassOf(FxEffect& a_effect, std::uint32_t a_technique, std::uint32_t a_pass)
	{
		const std::pair<std::uint32_t, std::uint32_t> key{ a_technique, a_pass };
		if (const auto found = a_effect.passes.find(key); found != a_effect.passes.end())
			return found->second;
		auto lowered = OrgFx::LowerPass(a_effect.lowered, a_effect.desc, a_technique, a_pass);
		LoweredText text;
		text.ok = lowered.ok;
		if (lowered.ok) {
			text.hash = std::hash<std::string>{}(lowered.source);
			text.pushWords = lowered.layout.pushWords;
			text.source = std::move(lowered.source);
		} else
			text.error = FirstError(lowered.diagnostics);
		return a_effect.passes.emplace(key, std::move(text)).first->second;
	}

	/**
	* @brief Builds one frame's steps. FX11's ping-pong becomes fixed textures. It runs twice for a plan, on the render thread:
	* listing (a_listing set), where each program is only described for the programs lane, then building (a_programs set), in the
	* client's make, where each program is looked up. Both runs take the same path, so the build finds every program it asks for.
	*/
	class Planner
	{
	public:
		Planner(Impl& a_impl, const ChainInputs& a_inputs, RF::Vars::Extent2D a_display, const FxPrograms* a_programs, std::vector<PlannedProgram>* a_listing) :
			impl(a_impl), inputs(a_inputs), frame(std::make_shared<Frame>()), programs(a_programs), listing(a_listing)
		{
			frame->width = a_display.width;
			frame->height = a_display.height;
			frame->runs = inputs.runs;
			frame->input = inputs.input;
			frame->output = inputs.output;
			for (std::size_t i = 0; i < kEffects; ++i) {
				if (!inputs.runs[i])
					continue;
				const auto& e = impl.effects[i];
				frame->effects[i] = { e.fx11, e.compiled, e.values, e.lowered.valuesSize, e.screenSize, e.screenSizeBytes };
			}
		}

		std::string error;

		Res Tex(std::string_view a_name)
		{
			const auto* spec = FindSpec(a_name);
			auto* texture = spec ? impl.textures.Get(*spec) : nullptr;
			if (!texture) {
				Fail(fmt::format("the texture Effects11::{} could not be created", a_name));
				return nullptr;
			}
			const std::pair<std::uint32_t, GraphResources::Texture2D*> used{ RowOf(*spec), texture };
			if (std::ranges::find(frame->textures, used) == frame->textures.end())
				frame->textures.push_back(used);
			// Passes declare the slot, so a new version needs no build.
			return AP::TextureOf(*texture);
		}

		Res Black() { return Tex("Black"); }

		void Fail(std::string a_reason)
		{
			if (error.empty())
				error = std::move(a_reason);
		}

		/** @brief The effect keeps the reason. A failed plan is fatal. */
		void FailEffect(FxEffect& a_effect, std::string a_reason)
		{
			a_effect.ok = false;
			a_effect.reason = a_reason;
			Fail(std::move(a_reason));
		}

		/** @brief Listing: describes the program and returns null. Building: the plan's program, or null with the build failed. */
		template <class Describe>
		std::shared_ptr<const RasterProgram> Raster(const std::string& a_key, Describe&& a_describe)
		{
			if (listing) {
				if (listed.insert(a_key).second) {
					RasterProgram::Desc desc = a_describe();
					RenderGraph::ProgramCacheDetail::ResolveDefines(desc);
					listing->push_back({ a_key, std::move(desc) });
				}
				return nullptr;
			}
			if (const auto found = programs->raster.find(a_key); found != programs->raster.end() && found->second)
				return found->second;
			Fail(fmt::format("the program {} is not among the plan's programs", a_key));
			return nullptr;
		}

		template <class Describe>
		std::shared_ptr<const ComputeProgram> Compute(const std::string& a_key, Describe&& a_describe)
		{
			if (listing) {
				if (listed.insert(a_key).second) {
					ComputeProgram::Desc desc = a_describe();
					RenderGraph::ProgramCacheDetail::ResolveDefines(desc);
					listing->push_back({ a_key, std::move(desc) });
				}
				return nullptr;
			}
			if (const auto found = programs->compute.find(a_key); found != programs->compute.end() && found->second)
				return found->second;
			Fail(fmt::format("the program {} is not among the plan's programs", a_key));
			return nullptr;
		}

		/** @brief A .fx pass's program. False, with a_reason, when the pass cannot be lowered. */
		bool FxPass(FxEffect& a_effect, std::uint32_t a_technique, std::uint32_t a_pass, rhi::Format a_format, const PassState& a_state,
			std::shared_ptr<const RasterProgram>& a_program, std::string& a_reason)
		{
			const auto& technique = a_effect.desc.techniques[a_technique];
			const auto& lowered = impl.LoweredPassOf(a_effect, a_technique, a_pass);
			if (!lowered.ok) {
				a_reason = fmt::format("{}: technique '{}' pass {} could not be lowered, {}", a_effect.Name(), technique.name, a_pass, lowered.error);
				return false;
			}
			const auto key = fmt::format("fx|{}|{}|{}|{:016x}|{}|{}", a_effect.Name(), technique.name, a_pass, static_cast<std::uint64_t>(lowered.hash),
				static_cast<int>(a_format), a_state.Key());
			a_program = Raster(key, [&] {
				RasterProgram::Desc desc{};
				desc.pixelSource = fmt::format("Effects11/ORG/{}/{}.{}.hlsl", a_effect.Name(), technique.name, a_pass);
				desc.vertexSource = desc.pixelSource;
				desc.pixelEntry = std::wstring(OrgFx::kPixelEntry, OrgFx::kPixelEntry + std::strlen(OrgFx::kPixelEntry));
				desc.vertexEntry = std::wstring(OrgFx::kVertexEntry, OrgFx::kVertexEntry + std::strlen(OrgFx::kVertexEntry));
				desc.pixelText = lowered.source;
				desc.vertexText = lowered.source;
				// Effects11 builds this code without CS's shader defines.
				desc.commonDefines = false;
				desc.languageVersion = L"2018";
				desc.constantWords = lowered.pushWords;
				desc.targets = { { .format = a_format, .blend = a_state.blend } };
				desc.alphaToCoverage = a_state.alphaToCoverage;
				desc.raster = a_state.raster;
				return desc;
			});
			return true;
		}

		std::uint32_t Region(std::uint32_t a_bytes)
		{
			const std::uint32_t offset = frame->latchBytes;
			frame->latchBytes += (std::max)((a_bytes + kRegionAlignment - 1) / kRegionAlignment * kRegionAlignment, kRegionAlignment);
			return offset;
		}

		void AddStep(Step a_step, std::string a_name)
		{
			frame->steps.push_back(std::move(a_step));
			frame->stepNames.push_back(std::move(a_name));
		}

		void CsDraw(std::string a_step, std::shared_ptr<const RasterProgram> a_program, Res a_target, std::uint32_t a_width, std::uint32_t a_height,
			std::vector<AP::Bind> a_bindings)
		{
			// CS's own draws reset EffectManager's states, so the .fx passes after them start from those.
			state = {};
			AddStep(AP::Draw{ .name = a_step,
						.program = std::move(a_program),
						.bindings = std::move(a_bindings),
						.target = std::move(a_target),
						.viewport = { .width = a_width, .height = a_height } },
				a_step);
		}

		/** @brief With dither for the output copy, without for EffectManager::CopyTexture's false. */
		void Copy(std::string a_step, Res a_source, Res a_target, rhi::Format a_format, std::uint32_t a_width, std::uint32_t a_height, float a_amplitude)
		{
			const std::uint32_t region = Region(sizeof(DitherConstants));
			frame->writes.push_back({ .kind = RegionWrite::Kind::Dither, .offset = region, .amplitude = a_amplitude });
			CsDraw(std::move(a_step), Raster(CopyKey(a_format), [&] { return CopyDesc(a_format); }), std::move(a_target), a_width, a_height,
				{ AP::Bind::Srv(std::move(a_source)), AP::Bind::Latch(region) });
		}

		/** @brief As CopyResource. */
		void CopyTexture(std::string a_step, Res a_source, Res a_target, std::uint32_t a_width = 1, std::uint32_t a_height = 1)
		{
			AddStep(AP::Copy{ .name = a_step, .source = std::move(a_source), .target = std::move(a_target), .width = a_width, .height = a_height }, a_step);
		}

		Res Resolve(const FxEffect& a_effect, std::size_t a_slot, const std::vector<std::pair<std::string_view, Res>>& a_names, const Res& a_color, const Res& a_target)
		{
			const auto& slot = a_effect.lowered.resources[a_slot];
			Res resource;
			if (a_effect.files[a_slot])
				resource = a_effect.files[a_slot];
			else if (slot.name == "TextureColor")
				resource = a_color;
			else if (slot.name == "TextureDepth")
				resource = inputs.depth;
			else if (IsRenderTargetName(slot.name))
				resource = Tex(slot.name);
			else
				for (const auto& [name, named] : a_names)
					if (name == slot.name) {
						resource = named;
						break;
					}
			// The aperture DoF wrote this frame, unless the effect names its own.
			if (!resource && slot.name == "TextureAperture")
				resource = aperture;
			// D3D11 unbinds a view of the bound target, so FX11 reads zero there.
			if (!resource || resource == a_target)
				resource = Black();
			return resource;
		}

		/** @brief As Effect::RenderPasses. */
		void FxTechnique(Fx a_fx, std::uint32_t a_technique, std::string_view a_targetName, const Res& a_color, const std::vector<std::pair<std::string_view, Res>>& a_names)
		{
			auto& e = impl.EffectOf(a_fx);
			const auto* spec = FindSpec(a_targetName);
			const Res target = Tex(a_targetName);
			if (!spec || !target)
				return;
			const std::uint32_t width = spec->size ? spec->size : frame->width;
			const std::uint32_t height = spec->size ? spec->size : frame->height;
			const rhi::Format format = rhi::helpers::ToRHI(spec->format);
			const auto& technique = e.desc.techniques[a_technique];
			for (std::uint32_t p = 0; p < technique.passes.size(); ++p) {
				std::string reason;
				if (!ApplyPassStates(e.desc, technique.passes[p], state, reason)) {
					FailEffect(e, fmt::format("{}: technique '{}': {}", e.Name(), technique.name, reason));
					return;
				}
				// Tessellation needs a patch list and the quad is a triangle list, so D3D11 draws nothing.
				const auto sets = [&](OrgFx::Stage a_stage) {
					const auto& binding = technique.passes[p].shaders[static_cast<std::size_t>(a_stage)];
					return binding.set && !binding.null;
				};
				if (sets(OrgFx::Stage::Hull) || sets(OrgFx::Stage::Domain)) {
					logger::info("[ORG] Effects11: {} technique '{}' pass {} sets tessellation shaders on a quad and draws nothing, as on D3D11", e.Name(), technique.name, p);
					continue;
				}
				// The target has one sample, so a mask without sample 0 writes nothing.
				if (!(state.sampleMask & 1u)) {
					logger::info("[ORG] Effects11: {} technique '{}' pass {} has a sample mask without sample 0 and draws nothing", e.Name(), technique.name, p);
					continue;
				}
				std::shared_ptr<const RasterProgram> program;
				if (!FxPass(e, a_technique, p, format, state, program, reason)) {
					FailEffect(e, std::move(reason));
					return;
				}
				auto draw = std::make_shared<DrawSpec>();
				draw->name = fmt::format("Effects11::{} {} pass {}", e.Name(), technique.name, p);
				draw->program = std::move(program);
				draw->target = target;
				draw->width = width;
				draw->height = height;
				draw->vertexCount = OrgFx::kQuadVertexCount;
				if (state.ReadsFactor())
					draw->blendFactor = state.blendFactor;
				const std::uint32_t region = Region(e.lowered.valuesSize);
				RegionWrite write{ .kind = RegionWrite::Kind::FxValues, .effect = a_fx, .offset = region };
				const float aspect = static_cast<float>(width) / static_cast<float>(height);
				write.screenSize[0] = static_cast<float>(width);
				write.screenSize[1] = 1.0f / static_cast<float>(width);
				write.screenSize[2] = aspect;
				write.screenSize[3] = 1.0f / aspect;
				frame->writes.push_back(write);
				// Entries past the pass's words read black or any sampler.
				const auto layout = OrgFx::LayoutPass(e.lowered, e.desc, a_technique, p);
				if (!layout.ok) {
					FailEffect(e, fmt::format("{}: technique '{}' pass {}: {}", e.Name(), technique.name, p, layout.error));
					return;
				}
				draw->push.assign(layout.pushWords, PushWord{});
				draw->push[0] = { PushWord::Kind::LatchHandle, 0 };
				draw->push[1] = { PushWord::Kind::LatchOffset, region };
				if (layout.shared) {
					draw->push[OrgFx::kUnusedTextureWord] = { PushWord::Kind::Input, draw->Input(Black()) };
					draw->push[OrgFx::kUnusedSamplerWord] = { PushWord::Kind::Literal, impl.linearSampler.Index() };
					draw->samplers.push_back(impl.linearSampler);
				}
				for (std::size_t i = 0; i < e.lowered.resources.size(); ++i) {
					const auto word = layout.words[i];
					if (layout.shared && (word == OrgFx::kUnusedTextureWord || word == OrgFx::kUnusedSamplerWord))
						continue;
					if (e.lowered.resources[i].kind == OrgFx::ResourceSlot::Kind::Sampler) {
						draw->push[word] = { PushWord::Kind::Literal, e.samplers[i].Index() };
						draw->samplers.push_back(e.samplers[i]);
					} else
						draw->push[word] = { PushWord::Kind::Input, draw->Input(Resolve(e, i, a_names, a_color, target)) };
				}
				++frame->fxPasses;
				AddStep(std::shared_ptr<const DrawSpec>(std::move(draw)), fmt::format("{}.{}.p{}", e.Name(), technique.name, p));
			}
		}

		struct ChainResult
		{
			bool executed = false;
			bool inOutput = false;
			bool inTemp = false;
		};

		/** @brief Effect::ExecuteTechniqueSequence, ping-ponging between a_output and a_temp. */
		ChainResult Sequence(Fx a_fx, const std::string& a_chain, const Res& a_input, std::string_view a_output, std::string_view a_temp,
			const std::vector<std::pair<std::string_view, Res>>& a_names)
		{
			ChainResult result;
			auto& e = impl.EffectOf(a_fx);
			auto& fx11 = *e.fx11;
			const auto it = fx11.techniques.find(a_chain);
			if (a_chain.empty() || it == fx11.techniques.end() || it->second.empty())
				return result;
			auto& sequence = it->second;
			std::uint32_t swapCounter = 0;
			for (auto& info : sequence) {
				if (!info.technique || !fx11.IsTechniqueEnabled(info))
					continue;
				const auto name = TechniqueName(info.technique.get());
				const auto technique = e.techniques.find(name);
				if (technique == e.techniques.end()) {
					FailEffect(e, fmt::format("{}: technique '{}' was not parsed", e.Name(), name));
					return result;
				}
				Res input;
				std::string_view output;
				if (sequence.size() == 1 || swapCounter == 0) {
					input = a_input;
					output = a_output;
				} else if ((swapCounter & 1) == 0) {
					input = Tex(a_temp);
					output = a_output;
				} else {
					input = Tex(a_output);
					output = a_temp;
				}
				if (!info.renderTargetName.empty()) {
					// A name FX11 cannot find leaves the output at the chain's, as there.
					if (const auto target = RenderTargetOf(info.renderTargetName, OwnTargets(a_fx)); !target.empty())
						output = target;
				} else
					++swapCounter;
				FxTechnique(a_fx, technique->second, output, input, a_names);
				if (output == a_output || output == a_temp) {
					result.executed = true;
					result.inOutput = output == a_output;
					result.inTemp = !result.inOutput;
				}
			}
			return result;
		}

		/** @brief Effect::effectTextureCache names, FX11's then the graph's. */
		static std::span<const std::pair<std::string_view, std::string_view>> OwnTargets(Fx a_fx)
		{
			static constexpr std::pair<std::string_view, std::string_view> kPostPass[] = { { "TextureScratch", "Scratch" } };
			// FX11's swap pairs. This frame's write is the first of each.
			static constexpr std::pair<std::string_view, std::string_view> kDepthOfField[] = { { "TextureAperture", "DofAperture" },
				{ "TextureApertureSwap", "DofApertureHistory" }, { "TextureReadFocus", "DofReadFocus" }, { "TextureFocus", "DofFocus" },
				{ "TextureFocusSwap", "DofFocusHistory" } };
			if (a_fx == Fx::PostPass)
				return kPostPass;
			if (a_fx == Fx::DepthOfField)
				return kDepthOfField;
			return {};
		}

		/** @brief ExecuteTechnique, when the effect has it. */
		void Technique(Fx a_fx, const char* a_name, std::string_view a_target, const std::vector<std::pair<std::string_view, Res>>& a_names)
		{
			auto& e = impl.EffectOf(a_fx);
			if (const auto it = e.techniques.find(a_name); it != e.techniques.end())
				FxTechnique(a_fx, it->second, a_target, Black(), a_names);
		}

		/** @brief In ExecuteEffects' order. Null with error set on failure. */
		std::shared_ptr<Frame> Build()
		{
			const auto& runs = inputs.runs;
			auto run = [&](Fx a_fx) { return runs[static_cast<std::size_t>(a_fx)]; };
			const std::uint32_t width = frame->width;
			const std::uint32_t height = frame->height;

			Black();

			// FX11 copies the input into kMAIN first. Here that is Original, which has a UAV.
			const Res original = Tex("Original");
			if (inputs.inputCopies)
				CopyTexture("copy-input", inputs.input, original, width, height);
			else
				Copy("copy-input", inputs.input, original, rhi::Format::R16G16B16A16_Float, width, height, 0.0f);

			// 1. Colour correction in place.
			{
				const std::uint32_t region = Region(sizeof(ColorCorrectionConstants));
				frame->writes.push_back({ .kind = RegionWrite::Kind::ColorCorrection, .offset = region });
				AddStep(AP::Dispatch{ .name = "color-correction",
							.program = Compute(std::string(kColorCorrectionKey), ColorCorrectionDesc),
							.bindings = { AP::Bind::Uav(original), AP::Bind::Latch(region) },
							.groups = { (width + 7) / 8, (height + 7) / 8, 1 } },
					"color-correction");
			}
			// 2. Depth of field. Each technique reads what FX11's variables hold at that point. EffectManager skips DoF's TextureAperture.
			if (run(Fx::DepthOfField)) {
				const Res apertureHistory = Tex("DofApertureHistory");
				const Res focusHistory = Tex("DofFocusHistory");
				const Res readFocus = Tex("DofReadFocus");
				const Res adaptationHistory = run(Fx::Adaptation) ? Tex("AdaptationHistory") : Black();
				const bool hasAperture = impl.EffectOf(Fx::DepthOfField).techniques.contains("Aperture");
				std::vector<std::pair<std::string_view, Res>> names{ { "TexturePrevious", apertureHistory }, { "TextureAperture", apertureHistory },
					{ "TextureCurrent", readFocus }, { "TextureFocus", focusHistory }, { "TextureOriginal", original }, { "TextureAdaptation", adaptationHistory } };
				Technique(Fx::DepthOfField, "Aperture", "DofAperture", names);
				names[1].second = Tex("DofAperture");
				Technique(Fx::DepthOfField, "ReadFocus", "DofReadFocus", names);
				names[0].second = focusHistory;
				Technique(Fx::DepthOfField, "Focus", "DofFocus", names);
				names[3].second = Tex("DofFocus");
				const auto chain = Sequence(Fx::DepthOfField, ChainOf(Fx::DepthOfField), original, "HDRTemp", "HDRTemp2", names);
				if (chain.executed && (chain.inOutput || chain.inTemp))
					CopyTexture("dof-to-original", Tex(chain.inOutput ? "HDRTemp" : "HDRTemp2"), original, width, height);
				CopyTexture("dof-aperture-history", Tex("DofAperture"), apertureHistory);
				CopyTexture("dof-focus-history", Tex("DofFocus"), focusHistory);
				// Later effects read this frame's aperture only when the Aperture technique wrote it.
				aperture = hasAperture ? Tex("DofAperture") : Black();
			}

			// FX11 leaves the corrected image in kMAIN. Do it before the output copy, which may be kMAIN.
			if (inputs.main) {
				if (inputs.mainCopies)
					CopyTexture("copy-to-main", original, inputs.main, width, height);
				else
					Copy("copy-to-main", original, inputs.main, inputs.mainFormat, inputs.mainWidth, inputs.mainHeight, 0.0f);
			}

			// 3. The 1024 downsample and its two mips.
			if (run(Fx::Bloom) || run(Fx::Lens) || run(Fx::Adaptation)) {
				const std::pair<std::string_view, std::uint32_t> levels[] = { { "Downsampled", 1024 }, { "Downsampled512", 512 }, { "Downsampled256", 256 } };
				Res source = original;
				for (const auto& [name, size] : levels) {
					const Res target = Tex(name);
					CsDraw(fmt::format("downsample-{}", size), Raster(std::string(kDownsampleKey), DownsampleDesc), target, size, size,
						{ AP::Bind::Srv(source), AP::Bind::Handle(impl.linearSampler.Index()) });
					source = target;
				}
			}

			const Res downsampled = Tex("Downsampled");
			Res bloom = Black();
			Res lens = Black();
			Res adaptation = Black();

			// 4. Bloom and 5. lens, from the 1024 downsample.
			if (run(Fx::Bloom)) {
				const auto chain = Sequence(Fx::Bloom, ChainOf(Fx::Bloom), downsampled, "Bloom", "BloomTemp",
					{ { "TextureDownsampled", downsampled }, { "TextureOriginal", original } });
				bloom = Tex(chain.executed && !chain.inOutput ? "BloomTemp" : "Bloom");
			}
			if (run(Fx::Lens)) {
				const auto chain = Sequence(Fx::Lens, ChainOf(Fx::Lens), downsampled, "Lens", "HDRTemp",
					{ { "TextureDownsampled", downsampled }, { "TextureOriginal", original } });
				lens = Tex(chain.executed && !chain.inOutput ? "HDRTemp" : "Lens");
			}

			// 6. Adaptation: sun-masked mip, down to 16x16, then blended with last frame's value.
			if (run(Fx::Adaptation)) {
				constexpr std::uint32_t kSize = ENBAdaptation::kSunMaskSize;
				const std::uint32_t region = Region(sizeof(ENBAdaptation::SunMaskCB));
				frame->writes.push_back({ .kind = RegionWrite::Kind::SunMask, .offset = region });
				const Res masked = Tex("SunMasked");
				CsDraw("adaptation-sun-mask", Raster(std::string(kSunMaskKey), SunMaskDesc), masked, kSize, kSize,
					{ AP::Bind::Srv(Tex("Downsampled256")), AP::Bind::Handle(impl.linearSampler.Index()), AP::Bind::Latch(region) });
				Technique(Fx::Adaptation, "Downsample", "AdaptationCurrent", { { "TextureCurrent", masked } });
				Technique(Fx::Adaptation, "Draw", "Adaptation", { { "TextureCurrent", Tex("AdaptationCurrent") }, { "TexturePrevious", Tex("AdaptationHistory") } });
				CopyTexture("adaptation-history", Tex("Adaptation"), Tex("AdaptationHistory"));
				adaptation = Tex("Adaptation");
			}

			// 7. enbeffect.fx into the SDR pair.
			std::string_view sdr = "SDRTemp";
			{
				const auto chain = Sequence(Fx::Effect, ChainOf(Fx::Effect), original, "SDRTemp", "SDRTemp2",
					{ { "TextureOriginal", original }, { "TextureBloom", bloom }, { "TextureLens", lens }, { "TextureAdaptation", adaptation } });
				if (chain.executed && !chain.inOutput)
					sdr = "SDRTemp2";
				else if (!chain.executed)
					// Side-target-only sequences leave the SDR texture unwritten, as ENBEffect::Execute.
					Copy("copy-original", original, Tex("SDRTemp"), rhi::Format::R10G10B10A2_UNorm, width, height, 0.0f);
			}

			// 8. The post pass, with Scratch as its temp.
			if (run(Fx::PostPass)) {
				const std::string_view other = sdr == "SDRTemp" ? "SDRTemp2" : "SDRTemp";
				const Res postPassInput = Tex(sdr);
				const auto chain = Sequence(Fx::PostPass, ChainOf(Fx::PostPass), postPassInput, other, "Scratch",
					{ { "TextureOriginal", postPassInput } });
				if (chain.executed && chain.inOutput)
					sdr = other;
				else if (chain.executed && chain.inTemp)
					sdr = "Scratch";
			}

			// 9. Dithered copy into the tonemap's output.
			Copy("copy-to-output", Tex(sdr), inputs.output, inputs.outputFormat, inputs.outputWidth, inputs.outputHeight, 1.0f / 255.0f);

			if (!error.empty())
				return nullptr;
			// A listing needs only the programs.
			if (listing)
				return frame;
			// Kept textures are cleared before anything reads them, on the first run of each version, as DXVK's D3D11 textures
			// start at zero. A version made by a revision is born cleared as well, and its first frame clears it again.
			std::vector<Step> clears;
			std::vector<std::string> clearNames;
			for (const auto& [row, texture] : frame->textures) {
				const auto& spec = kTextures[row];
				if (spec.lifetime == Lifetime::Transient)
					continue;
				const std::uint32_t clearWidth = spec.size ? spec.size : width;
				const std::uint32_t clearHeight = spec.size ? spec.size : height;
				clears.push_back(ClearSpec{ fmt::format("Effects11::Clear {}", spec.name), AP::TextureOf(*texture), clearWidth, clearHeight, row });
				clearNames.push_back(fmt::format("clear-{}", spec.name));
				frame->clears.emplace_back(row, texture);
			}
			frame->steps.insert(frame->steps.begin(), std::make_move_iterator(clears.begin()), std::make_move_iterator(clears.end()));
			frame->stepNames.insert(frame->stepNames.begin(), std::make_move_iterator(clearNames.begin()), std::make_move_iterator(clearNames.end()));
			frame->latch = std::make_shared<org::LatchBlock>("cs.effects11.latch", (std::max)(frame->latchBytes, kRegionAlignment));
			return frame;
		}

	private:
		Impl& impl;
		const ChainInputs& inputs;
		std::shared_ptr<Frame> frame;
		const FxPrograms* programs = nullptr;            // building
		std::vector<PlannedProgram>* listing = nullptr;  // listing
		std::set<std::string, std::less<>> listed;
		PassState state;  // what FX11's context holds for the next .fx pass
		Res aperture;     // null means black, when DoF wrote none
	};

	/** @brief The client's key: a change registers a new plan. */
	std::string ShapeKey(const ChainInputs& a_inputs, RF::Vars::Extent2D a_display)
	{
		std::string key = fmt::format("{}x{} {} {}{} {} {}", a_display.width, a_display.height, reinterpret_cast<std::uintptr_t>(a_inputs.input.Key()),
			a_inputs.inputCopies ? 'c' : 'd', reinterpret_cast<std::uintptr_t>(a_inputs.main.Key()), static_cast<std::uint32_t>(a_inputs.mainFormat), a_inputs.mainWidth);
		key += fmt::format(" {} {} {} {} {}", a_inputs.mainCopies ? 'c' : 'd', reinterpret_cast<std::uintptr_t>(a_inputs.depth.Key()),
			reinterpret_cast<std::uintptr_t>(a_inputs.output.Key()), static_cast<std::uint32_t>(a_inputs.outputFormat), a_inputs.outputWidth);
		for (std::size_t i = 0; i < kEffects; ++i) {
			if (!a_inputs.runs[i]) {
				key += " -";
				continue;
			}
			auto& fx11 = *effects[i].fx11;
			const auto chain = ChainOf(static_cast<Fx>(i));
			// A reloaded effect is a new plan even with the same techniques, since the plan reads its variables.
			key += fmt::format(" {:x} ", reinterpret_cast<std::uintptr_t>(effects[i].compiled.get())) + chain + ":";
			if (const auto it = fx11.techniques.find(chain); it != fx11.techniques.end())
				for (auto& info : it->second)
					key += fx11.IsTechniqueEnabled(info) ? '1' : '0';
		}
		return key;
	}

	/** @brief The registration for a plan, in a_client. False, with a_failure, when the builder refuses a step. */
	bool MakeClient(const std::shared_ptr<Frame>& a_frame, FrameAnchors::ClientDesc& a_client, std::string& a_failure)
	{
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		const auto latch = std::shared_ptr<const org::LatchBlock>(a_frame->latch);
		const std::shared_ptr<const RenderGraph::RunGate> gate = clearGate;
		AP::Builder builder(FrameAnchors::Anchor::Tonemap, kClientId, kFirstOrder, latch);
		for (std::size_t i = 0; i < a_frame->steps.size(); ++i) {
			// The number keeps names unique and in plan order.
			const auto name = fmt::format("{:03}.{}", i, a_frame->stepNames[i]);
			std::visit(
				[&](const auto& a_step) {
					using T = std::decay_t<decltype(a_step)>;
					if constexpr (std::is_same_v<T, std::shared_ptr<const DrawSpec>>) {
						builder.Add(name, FrameAnchors::PassKind::Render,
							[a_step, latch]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DrawPass>(a_step, latch); });
					} else if constexpr (std::is_same_v<T, ClearSpec>) {
						// Recorded at execution: it clears only on runs whose commit opened its row.
						builder.Add(
							name, FrameAnchors::PassKind::Render,
							[a_step, gate]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ClearPass>(a_step, gate); }, false, true);
					} else {
						auto step = a_step;
						step.name = name;
						builder.Add(std::move(step));
					}
				},
				a_frame->steps[i]);
		}
		if (builder.Failed()) {
			a_failure = builder.Failure();
			return false;
		}
		client.passes = builder.Take();
		client.registerResources = [a_frame](org::RenderGraph& a_graph) {
			for (const auto& [row, texture] : a_frame->textures)
				texture->Register(a_graph);
		};
		// Read at each Begin: while Post Processing or the game owns the tonemap, the passes record only their barriers.
		client.declared = [] {
			const bool owner = globals::state && globals::state->PeekTonemapOwner() == State::TonemapOwner::kEffects11;
			if (auto& self = ORGFxRuntime::Get(); self.impl)
				self.impl->declaredOwner = owner;
			return owner;
		};
		client.active = [] {
			const auto& self = ORGFxRuntime::Get();
			return self.impl && self.impl->activeNow;
		};
		// The commit that runs is the one of the registration the run's graph holds, so it writes its own plan.
		client.commit = [a_frame](const FrameAnchors::Point&) {
			if (auto& self = ORGFxRuntime::Get(); self.impl)
				self.impl->SafeCommit(*a_frame);
		};
		client.executed = [](const FrameAnchors::Point&) {
			if (auto& self = ORGFxRuntime::Get(); self.impl)
				self.impl->Executed();
		};
		client.gates = { clearGate };
		// A schedule that still holds an older plan taps as it, and that plan's commit runs, so a new plan never drops a frame to FX11.
		client.matchesById = true;
		client.generation = ++buildGeneration;
		a_client = std::move(client);
		return true;
	}

	/**
	* @brief What a revision's chain plans from, and its display size in a_display. Nullopt while no preset runs. An input the
	* graph cannot take, or an effect it cannot parse or lower, is fatal: FX11 never takes over. Render thread, no compile.
	*/
	std::optional<ChainInputs> ChainInputsFor(const RF::Inputs& a_in, RF::Vars::Extent2D& a_display)
	{
		// Same conditions as Effects11::RenderTonemap and ExecuteEffects. The tonemap owner is the client's declared.
		auto& manager = EffectManager::GetSingleton();
		if (!observed.ready || !manager.IsInitialized() || !manager.IsPresetLoaded())
			return std::nullopt;
		if (!observed.input || !observed.output)
			Fatal("the tonemap's input or output could not be imported");
		if (!observed.depth)
			Fatal("kMAIN's depth could not be imported");
		if (observed.outputFormat == rhi::Format::Unknown || !observed.outputWidth || !observed.outputHeight)
			Fatal("the tonemap output's format or size is not known");

		ChainInputs inputs{};
		for (std::size_t i = 0; i < kEffects; ++i) {
			const auto fx = static_cast<Fx>(i);
			inputs.runs[i] = Runs(fx);
			if (!inputs.runs[i])
				continue;
			auto& effect = EffectOf(fx);
			if (!effect.built)
				BuildEffect(fx);
			if (!effect.ok)
				Fatal(effect.reason);
		}

		// The textures follow the display size, as TextureManager's follow the screen.
		const auto display = DisplayOf(a_in);
		a_display = display;
		inputs.input = observed.input;
		inputs.depth = observed.depth;
		inputs.output = observed.output;
		// kMAIN also gets FX11's corrected image when it can be drawn to.
		if (observed.main && observed.mainFormat != rhi::Format::Unknown) {
			inputs.main = observed.main;
			inputs.mainFormat = observed.mainFormat;
			inputs.mainWidth = observed.mainWidth;
			inputs.mainHeight = observed.mainHeight;
		}
		inputs.outputFormat = observed.outputFormat;
		inputs.outputWidth = observed.outputWidth;
		inputs.outputHeight = observed.outputHeight;
		// Where CopyResource would apply: same format and size as Original, mip 0 of one slice.
		inputs.inputCopies = observed.inputWidth == display.width && observed.inputHeight == display.height && observed.inputFormat == rhi::Format::R16G16B16A16_Float;
		inputs.mainCopies = inputs.main && observed.mainCopyable && observed.mainWidth == display.width && observed.mainHeight == display.height;
		return inputs;
	}

	/**
	* @brief The declared program's key: every program the chain of a_in's values asks for. A listing run of the planner on the
	* render thread, CPU only. An equal shape text gives the last plan without planning again.
	*/
	FxPlanKey PlanFor(const RF::Inputs& a_in)
	{
		// Nothing may escape a revision's start, and an error is fatal.
		try {
			FxPlanKey plan{ .generation = generation };
			RF::Vars::Extent2D display{};
			const auto inputs = ChainInputsFor(a_in, display);
			if (!inputs)
				return plan;
			auto text = ShapeKey(*inputs, display);
			if (lastPlan && lastPlan->first == text && lastPlan->second.generation == generation)
				return lastPlan->second;
			std::vector<PlannedProgram> listed;
			Planner planner(*this, *inputs, display, nullptr, &listed);
			planner.Build();
			if (!planner.error.empty())
				Fatal(fmt::format("the chain could not be planned: {}", planner.error));
			for (const auto& program : listed)
				plan.keys.push_back(program.key);
			std::ranges::sort(plan.keys);
			plan.programs = std::make_shared<const std::vector<PlannedProgram>>(std::move(listed));
			lastPlan.emplace(std::move(text), plan);
			return plan;
		} catch (const std::exception& e) {
			Fatal(fmt::format("planning threw: {}", e.what()));
		}
	}

	/** @brief The declared program's build, on the programs lane. Null, logged, when a program does not build: that is fatal. */
	static std::shared_ptr<const FxPrograms> BuildPlan(ProgramStore& a_store, rhi::Device a_device, const FxPlanKey& a_plan)
	{
		auto built = std::make_shared<FxPrograms>();
		if (!a_plan.programs)
			return built;
		std::uint32_t compiled = 0;
		for (const auto& planned : *a_plan.programs) {
			const auto stored = fmt::format("{}|{}", a_plan.generation, planned.key);
			if (const auto* raster = std::get_if<RasterProgram::Desc>(&planned.desc)) {
				std::shared_ptr<const RasterProgram> program;
				{
					const std::lock_guard lock(a_store.mutex);
					if (const auto found = a_store.raster.find(stored); found != a_store.raster.end())
						program = found->second;
				}
				if (!program) {
					program = RasterProgram::Load(a_device, *raster);
					if (!program) {
						logger::critical("[ORG] Effects11: {} ({}) did not build on the programs lane (the [ORG] error above has DXC's messages)", raster->pixelSource,
							planned.key);
						return nullptr;
					}
					++compiled;
					const std::lock_guard lock(a_store.mutex);
					a_store.raster.emplace(stored, program);
				}
				built->raster.emplace(planned.key, std::move(program));
			} else {
				const auto& compute = std::get<ComputeProgram::Desc>(planned.desc);
				std::shared_ptr<const ComputeProgram> program;
				{
					const std::lock_guard lock(a_store.mutex);
					if (const auto found = a_store.compute.find(stored); found != a_store.compute.end())
						program = found->second;
				}
				if (!program) {
					program = ComputeProgram::Load(a_device, compute);
					if (!program) {
						logger::critical("[ORG] Effects11: {} ({}) did not build on the programs lane", compute.source, planned.key);
						return nullptr;
					}
					++compiled;
					const std::lock_guard lock(a_store.mutex);
					a_store.compute.emplace(stored, program);
				}
				built->compute.emplace(planned.key, std::move(program));
			}
		}
		logger::info("[ORG] Effects11: the FX chain's {} programs are built on the programs lane ({} new)", a_plan.programs->size(), compiled);
		return built;
	}

	/**
	* @brief The client's key for a revision's values: the shape text, what the build plans from and the plan's programs. Reads
	* values and programs only. The revision started once the programs of its values were built, so they are there.
	*/
	std::optional<Shape> KeyFor(const RF::Inputs& a_in)
	{
		// Nothing may escape a revision's start, and an error is fatal.
		try {
			Shape shape;
			const auto inputs = ChainInputsFor(a_in, shape.display);
			if (!inputs)
				return std::nullopt;
			shape.inputs = *inputs;
			shape.text = ShapeKey(*inputs, shape.display);
			shape.generation = generation;
			shape.programs = programs.For(a_in);
			return shape;
		} catch (const std::exception& e) {
			Fatal(fmt::format("the client's key threw: {}", e.what()));
		}
	}

	/** @brief The make function: builds the chain for a_shape. False is fatal (Frame/Declare.h), so the reason is logged first. */
	bool Make(const Shape& a_shape, FrameAnchors::ClientDesc& a_client)
	{
		std::string failure;
		try {
			if (!a_shape.programs)
				failure = "its programs are not built";
			else {
				const auto started = std::chrono::steady_clock::now();
				Planner planner(*this, a_shape.inputs, a_shape.display, a_shape.programs.get(), nullptr);
				const auto frame = planner.Build();
				if (!frame)
					failure = planner.error;
				else if (MakeClient(frame, a_client, failure)) {
					std::string effectsList;
					for (std::size_t i = 0; i < kEffects; ++i)
						if (frame->runs[i])
							effectsList += fmt::format("{}{}", effectsList.empty() ? "" : ", ", effects[i].Name());
					const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
					logger::info("[ORG] Effects11: FX chain {} built for the next revision: {} passes ({} from .fx: {}), {} programs, {} latch bytes, {}x{}, in {:.0f} ms",
						a_client.generation, frame->steps.size(), frame->fxPasses, effectsList, a_shape.programs->raster.size() + a_shape.programs->compute.size(),
						frame->latchBytes, frame->width, frame->height, ms);
					return true;
				}
			}
		} catch (const std::exception& e) {
			failure = e.what();
		}
		logger::critical("[ORG] Effects11: the FX chain could not be built: {}", failure);
		return false;
	}

	/** @brief What this tonemap's targets and FX11's effects are. */
	static Observed Observe(const RenderGraph::Engine::TonemapTargets& a_targets)
	{
		Observed now;
		auto& manager = EffectManager::GetSingleton();
		now.ready = manager.IsInitialized() && manager.IsPresetLoaded();
		for (std::size_t i = 0; i < kEffects; ++i) {
			auto* fx11 = Fx11Of(static_cast<Fx>(i));
			now.compiled[i] = fx11 && fx11->IsCompiled() ? static_cast<const void*>(fx11->GetEffect()) : nullptr;
		}
		now.input = a_targets.input;
		now.output = a_targets.output;
		now.inputFormat = a_targets.inputFormat;
		now.outputFormat = a_targets.outputFormat;
		if (now.input) {
			now.inputWidth = now.input.Width();
			now.inputHeight = now.input.Height();
		}
		if (now.output) {
			now.outputWidth = now.output.Width();
			now.outputHeight = now.output.Height();
		}
		now.depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
		const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		if (main.texture && main.RTV) {
			now.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
			D3D11_RENDER_TARGET_VIEW_DESC mainView{};
			main.RTV->GetDesc(&mainView);
			D3D11_TEXTURE2D_DESC mainDesc{};
			main.texture->GetDesc(&mainDesc);
			now.mainFormat = rhi::helpers::ToRHI(mainView.Format);
			now.mainWidth = mainDesc.Width;
			now.mainHeight = mainDesc.Height;
			now.mainCopyable = mainDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && mainDesc.SampleDesc.Count == 1;
		}
		return now;
	}

	/** @brief At the first tonemap the graph can take. A part that cannot be made is fatal. */
	void Declare()
	{
		clearGate = RenderGraph::RunGate::Create("cs.effects11.clears", kTextureRows);
		if (!clearGate)
			Fatal("its clear gate could not be made");
		linearSampler = ShaderBindings::CreateSampler({});
		if (!linearSampler)
			Fatal("its linear sampler could not be made");
		// Both keys read Effects11's settings, its effects' techniques and the observed targets. Effects11's writers raise
		// fxShapeWrites, a new observation invalidates both, and a new display size is in the values.
		const auto dependencies = RF::DependsOn(RF::Vars::DisplaySize(), globals::features::effects11.fxShapeWrites);
		// The first plan is empty (nothing is observed yet), so the declaration builds nothing. Later plans build on the lane.
		programs = RF::DeclareProgram(
			"Effects11::FxChain", dependencies, [this](const RF::Inputs& a_in) { return PlanFor(a_in); },
			[store = store, device = RenderGraphRuntime::Get().Host()->Device()](const FxPlanKey& a_plan) { return BuildPlan(*store, device, a_plan); });
		if (!programs)
			Fatal("its programs could not be declared");
		client = RF::DeclareClient<Shape>(
			kClientId, dependencies, [this](const RF::Inputs& a_in) { return KeyFor(a_in); },
			[this](const Shape& a_shape, FrameAnchors::ClientDesc& a_client) { return Make(a_shape, a_client); });
		if (!client)
			Fatal("its client could not be declared");
		logger::info("[ORG] Effects11: the FX chain's programs and client follow the frame revisions");
	}

	/** @brief Prepare catches what it throws. */
	void PrepareFrame(const RenderGraph::Engine::TonemapTargets& a_targets);

	/** @brief Keeps what Commit throws inside the epoch's callback. It is fatal. */
	void SafeCommit(Frame& a_frame)
	{
		try {
			Commit(a_frame);
		} catch (const std::exception& e) {
			Fatal(fmt::format("the commit failed: {}", e.what()));
		}
	}

	/** @brief The run's values for a_frame, the plan the run's graph holds. Opens the clears of kept textures' new versions. */
	void Commit(Frame& a_frame)
	{
		const bool wrote = a_frame.latch && Write(a_frame);
		openedClears.clear();
		for (const auto& [row, texture] : a_frame.clears) {
			const void* version = texture->Graph().get();
			if (clearedVersions[row] == version)
				continue;
			clearGate->OpenDraw(row);
			openedClears.emplace_back(row, version);
		}
		// Only a plan for this frame's targets replaces FX11.
		if (wrote && a_frame.input == frameInput && a_frame.output == frameOutput)
			ranFrame = globals::state->frameCount;
	}

	/** @brief The run was submitted, so its clears ran. */
	void Executed()
	{
		for (const auto& [row, version] : openedClears)
			clearedVersions[row] = version;
		openedClears.clear();
	}

	/** @brief As EffectManager::UpdateEffectValues sets FX11's variables. */
	bool Write(const Frame& a_shape)
	{
		auto& manager = EffectManager::GetSingleton();
		for (std::size_t i = 0; i < kEffects; ++i) {
			if (!a_shape.runs[i])
				continue;
			const auto& e = a_shape.effects[i];
			// FX11 reloaded the effect after the plan, so FX11 renders this frame.
			if (!e.fx11 || !e.compiled || e.fx11->GetEffect() != e.compiled.get())
				return false;
		}
		for (std::size_t i = 0; i < kEffects; ++i) {
			if (!a_shape.runs[i])
				continue;
			const auto& e = a_shape.effects[i];
			// Once a run, whichever shapes it writes.
			if (updatedFrame[i] != globals::state->frameCount) {
				manager.UpdateEffectValues(*e.fx11);
				updatedFrame[i] = globals::state->frameCount;
				// DoF blends with its history from the next frame on, as after FX11's Execute.
				if (static_cast<Fx>(i) == Fx::DepthOfField)
					manager.enbDepthOfField.MarkHistoryValid();
			}
			auto& image = images[i];
			image.assign(e.valuesSize, 0);
			for (const auto& value : e.values)
				value.variable->GetRawValue(image.data() + value.offset, 0, value.size);
		}
		const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
		const auto& latch = *a_shape.latch;
		const std::uint32_t frameCount = manager.frameCount;
		for (const auto& write : a_shape.writes) {
			switch (write.kind) {
			case RegionWrite::Kind::FxValues:
				{
					const auto index = static_cast<std::size_t>(write.effect);
					auto& image = images[index];
					const auto& e = a_shape.effects[index];
					if (e.screenSize >= 0 && static_cast<std::size_t>(e.screenSize) + e.screenSizeBytes <= image.size())
						std::memcpy(image.data() + e.screenSize, write.screenSize, e.screenSizeBytes);
					latch.Write(run, write.offset, std::as_bytes(std::span<const std::uint8_t>(image)));
					break;
				}
			case RegionWrite::Kind::ColorCorrection:
				{
					auto& settings = SettingManager::GetSingleton();
					const ColorCorrectionConstants constants{ settings.GetValue<float>(manager.ids.brightness), settings.GetValue<float>(manager.ids.gammaCurve), frameCount, 0 };
					latch.WriteValue(run, write.offset, constants);
					break;
				}
			case RegionWrite::Kind::SunMask:
				{
					ENBAdaptation::SunMaskCB constants{};
					manager.enbAdaptation.SunMaskConstants(constants);
					latch.WriteValue(run, write.offset, constants);
					break;
				}
			case RegionWrite::Kind::Dither:
				latch.WriteValue(run, write.offset, DitherConstants{ frameCount, write.amplitude, { 0, 0 } });
				break;
			}
		}
		return true;
	}
};

ORGFxRuntime::~ORGFxRuntime() = default;

void ORGFxRuntime::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Tonemap))
		logger::warn("[ORG] Effects11 stays on FX11: the Tonemap anchor could not be reserved");
}

void ORGFxRuntime::Invalidate()
{
	if (!impl)
		return;
	// The current registration keeps its own copy of the variables it reads, and its commit leaves a reloaded effect to FX11.
	impl->activeNow = false;
	for (auto& effect : impl->effects)
		effect = FxEffect{};
	// A reload lists and builds every program again. Registrations and kept plans hold their own programs.
	{
		const std::lock_guard lock(impl->store->mutex);
		impl->store->raster.clear();
		impl->store->compute.clear();
	}
	++impl->generation;
	impl->lastPlan.reset();
	impl->programs.Invalidate();
	impl->client.Invalidate();
}

void ORGFxRuntime::Prepare(const RenderGraph::Engine::TonemapTargets& a_targets)
{
	if (!Requested())
		return;
	if (!impl)
		impl = std::make_unique<Impl>();
	impl->activeNow = false;
	// Nothing may escape the engine's tonemap call, and an error is fatal.
	try {
		impl->PrepareFrame(a_targets);
	} catch (const std::exception& e) {
		Fatal(e.what());
	} catch (...) {
		Fatal("an unknown exception");
	}
}

void ORGFxRuntime::Impl::PrepareFrame(const RenderGraph::Engine::TonemapTargets& a_targets)
{
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.Host() || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Tonemap)) {
		Inactive("the render graph or its Tonemap anchor is not available");
		return;
	}
	if (!client)
		Declare();
	// A tonemap after this frame's stage, such as a menu scene's, is not the world's. No stage takes it, so FX11 runs it, and it
	// leaves the observed state alone.
	if (FrameAnchors::StagePassed(kClientId, FrameAnchors::Anchor::Tonemap)) {
		Inactive("a tonemap after this frame's stage, such as a menu scene's");
		return;
	}
	// Not variables yet: a change of the targets or of FX11's compiled effects plans again at the next revision.
	if (auto now = Observe(a_targets); !(now == observed)) {
		observed = std::move(now);
		programs.Invalidate();
		client.Invalidate();
	}
	frameInput = a_targets.input;
	frameOutput = a_targets.output;
	// Same conditions as Effects11::RenderTonemap and ExecuteEffects. A frame whose Begin left the passes off has no commit either.
	if (globals::state->GetTonemapOwner() != State::TonemapOwner::kEffects11 || !observed.ready || !declaredOwner)
		return;
	// The registration the frame's schedule holds commits, which may be older than the newest one (matchesById).
	const auto hold = FrameAnchors::HeldNow(kClientId, FrameAnchors::Anchor::Tonemap);
	activeNow = hold.held && hold.scheduled;
	if (activeNow)
		lastReason.clear();
	else
		Inactive(hold.held ? "the tonemap's stage has no schedule that holds the chain yet" : "this frame has no stage that holds the chain");
}

bool ORGFxRuntime::RanThisFrame() const
{
	return impl && impl->ranFrame == globals::state->frameCount;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph Effects11 always runs on FX11.
struct ORGFxRuntime::Impl
{};

ORGFxRuntime::~ORGFxRuntime() = default;
void ORGFxRuntime::Reserve() {}
void ORGFxRuntime::Invalidate() {}
void ORGFxRuntime::Prepare(const RenderGraph::Engine::TonemapTargets&) {}
bool ORGFxRuntime::RanThisFrame() const { return false; }

#endif
