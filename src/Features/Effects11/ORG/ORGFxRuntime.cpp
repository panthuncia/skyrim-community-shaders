#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "ORGFxRuntime.h"

#include <cstring>

namespace
{
	constexpr const char* kEnvSwitch = "CS_ORG_FX";
	constexpr const char* kReadbackSwitch = "CS_ORG_FX_READBACK";
	constexpr const char* kInputSwitch = "CS_ORG_FX_INPUT";

	/** @brief CS_ORG_FX_INPUT=d3d11: the tonemap input imported as a D3D11 texture (FrameAnchors::D3D11Texture), a diagnostic. */
	bool InputFromD3D11()
	{
		static const bool requested = [] {
			char value[16]{};
			return GetEnvironmentVariableA(kInputSwitch, value, sizeof(value)) && _stricmp(value, "d3d11") == 0;
		}();
		return requested;
	}
}

bool ORGFxRuntime::ReadbackRequested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA(kReadbackSwitch, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

bool ORGFxRuntime::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA(kEnvSwitch, value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
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

#	include "Features/Effects11/EffectManager.h"
#	include "Features/Effects11/SettingManager.h"
#	include "Features/Effects11/TextureManager.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
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
#	include <DirectXPackedVector.h>

#	include <array>
#	include <atomic>
#	include <cfloat>
#	include <cmath>
#	include <chrono>
#	include <map>
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

	using Res = std::shared_ptr<org::Resource>;
	using Lifetime = GraphResources::Lifetime;

	/** @brief The .fx effects the runtime runs, in the order ExecuteEffects runs them. */
	enum class Fx : std::uint8_t
	{
		Bloom,
		Lens,
		Adaptation,
		Effect,
		PostPass,
		Count
	};
	constexpr std::size_t kEffects = static_cast<std::size_t>(Fx::Count);

	/** @brief The graph textures that stand in for TextureManager's and the effects' own. Size 0: the screen's. */
	struct TextureSpec
	{
		std::string_view name;
		std::uint32_t size;
		DXGI_FORMAT format;
		Lifetime lifetime;
		bool uav = false;
	};

	// Chain textures that a later effect reads even when their chain did not run this frame (FX11 then reads the last frame's)
	// are History, so they keep their contents; the others are written before every read and may alias.
	constexpr TextureSpec kTextures[] = {
		// FX11's kMAIN: the tonemap input copied in, colour corrected in place. Engine targets need not have a UAV.
		{ "Original", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::Transient, true },
		{ "SDRTemp", 0, DXGI_FORMAT_R10G10B10A2_UNORM, Lifetime::Transient },
		{ "SDRTemp2", 0, DXGI_FORMAT_R10G10B10A2_UNORM, Lifetime::Transient },
		{ "Scratch", 0, DXGI_FORMAT_R10G10B10A2_UNORM, Lifetime::Transient },
		{ "HDRTemp", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "Lens", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "Bloom", 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		{ "BloomTemp", 1024, DXGI_FORMAT_R16G16B16A16_FLOAT, Lifetime::History },
		// TextureManager's 1024 texture and its two mips, as three textures: FX11 binds them only as single-mip views.
		{ "Downsampled", 1024, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "Downsampled512", 512, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "Downsampled256", 256, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "SunMasked", ENBAdaptation::kSunMaskSize, DXGI_FORMAT_R11G11B10_FLOAT, Lifetime::Transient },
		{ "AdaptationCurrent", 16, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		{ "Adaptation", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::Transient },
		{ "AdaptationHistory", 1, DXGI_FORMAT_R32_FLOAT, Lifetime::History },
		// What FX11 reads through a null view: zero.
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

	/** @brief A RenderTarget* name the effects share (Effects11's RenderTarget annotation and global textures). */
	bool IsRenderTargetName(std::string_view a_name)
	{
		return a_name.starts_with("RenderTarget") && FindSpec(a_name);
	}

	/** @brief The graph textures, kept while the screen size stays the same. */
	struct Resources
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::map<std::string, std::unique_ptr<GraphResources::Texture2D>, std::less<>> textures;
		// The kept (History, Persistent) textures a frame that ran has cleared: they start at zero, as DXVK's D3D11 textures.
		std::set<std::string, std::less<>> cleared;

		/** @brief The texture by name, created on first use; null when the name is unknown or the texture cannot be made. */
		GraphResources::Texture2D* Get(std::string_view a_name)
		{
			if (const auto it = textures.find(a_name); it != textures.end())
				return it->second.get();
			const auto* spec = FindSpec(a_name);
			if (!spec)
				return nullptr;
			const UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | (spec->uav ? D3D11_BIND_UNORDERED_ACCESS : 0u);
			const D3D11_TEXTURE2D_DESC desc{ .Width = spec->size ? spec->size : width,
				.Height = spec->size ? spec->size : height,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = spec->format,
				.SampleDesc = { .Count = 1 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = bind };
			auto texture = std::make_unique<GraphResources::Texture2D>(desc, fmt::format("Effects11::{}", a_name), GraphResources::Options{ .lifetime = spec->lifetime });
			if (!*texture)
				return nullptr;
			auto* raw = texture.get();
			textures.emplace(std::string(a_name), std::move(texture));
			return raw;
		}
	};

	/** @brief A variable of FX11's $Globals copied into a pass's region every frame. */
	struct ValueCopy
	{
		ID3DX11EffectVariable* variable = nullptr;
		std::uint32_t offset = 0;
		std::uint32_t size = 0;
	};

	using ProgramKey = std::tuple<std::uint32_t, std::uint32_t, rhi::Format>;  // technique, pass, target format

	/** @brief One .fx effect on the graph: the parse, the lowering, its bindings and its compiled passes. */
	struct FxEffect
	{
		EffectBase* fx11 = nullptr;
		bool built = false;
		bool ok = false;
		std::string reason;
		OrgFx::EffectDesc desc;
		OrgFx::LoweredEffect lowered;
		std::vector<ValueCopy> values;
		std::int32_t screenSize = -1;  // ScreenSize's offset in $Globals
		std::uint32_t screenSizeBytes = 0;
		std::vector<org::OwnedDescriptorBinding> samplers;  // per resource slot; empty for textures
		std::vector<Res> files;                             // per resource slot: its ResourceName texture, or null
		std::unordered_map<std::string, std::uint32_t> techniques;
		std::map<ProgramKey, std::shared_ptr<const RasterProgram>> programs;

		std::string Name() const { return fx11 ? fx11->GetName() : std::string("?"); }
	};

	/** @brief CS's own programs of the chain. */
	struct CsPrograms
	{
		std::shared_ptr<const ComputeProgram> colorCorrection;
		std::shared_ptr<const RasterProgram> downsample;
		std::shared_ptr<const RasterProgram> sunMask;
		std::map<rhi::Format, std::shared_ptr<const RasterProgram>> copy;
		org::OwnedDescriptorBinding linearSampler;
		bool tried = false;
	};

	// ColorCorrectionCS's push block under ORG: the UAV, then the latch.
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

	constexpr std::uint32_t kDownsampleWords = 2;  // SourceTexture, LinearSampler
	constexpr std::uint32_t kSunMaskWords = 4;     // SourceTexture, LinearSampler, SunMask (handle, offset)
	constexpr std::uint32_t kCopyWords = 3;        // SourceTexture, Dither (handle, offset)

	/** @brief One word of a raster pass's push block, resolved at record time. */
	struct PushWord
	{
		enum class Kind : std::uint8_t
		{
			Literal,      // value
			Input,        // the heap index of the pass's input number value
			LatchHandle,  // the latch block's raw view
			LatchOffset,  // the frame slot's region plus value
		};
		Kind kind = Kind::Literal;
		std::uint32_t value = 0;
	};

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
		std::vector<org::OwnedDescriptorBinding> samplers;  // the samplers its push block names, kept while the pass lives
		std::vector<std::string> wordNames;                 // CS_ORG_FX_READBACK: what each push word is, for the logs
		mutable std::atomic<bool> loggedWords = false;      // CS_ORG_FX_READBACK: the record-time words were logged

		std::uint32_t Input(const Res& a_resource)
		{
			const auto it = std::ranges::find(inputs, a_resource);
			if (it != inputs.end())
				return static_cast<std::uint32_t>(it - inputs.begin());
			inputs.push_back(a_resource);
			return static_cast<std::uint32_t>(inputs.size() - 1);
		}
	};

	struct ComputeSpec
	{
		std::string name;
		std::shared_ptr<const ComputeProgram> program;
		Res texture;
		std::uint32_t groupsX = 0;
		std::uint32_t groupsY = 0;
		std::uint32_t region = 0;
	};

	struct CopySpec
	{
		std::string name;
		Res from;
		Res to;
		std::uint32_t width = 1;
		std::uint32_t height = 1;
	};

	struct ClearSpec
	{
		std::string name;
		Res texture;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	using Step = std::variant<std::shared_ptr<const DrawSpec>, std::shared_ptr<const ComputeSpec>, CopySpec, ClearSpec>;

	/** @brief A region of the latch the commit fills. */
	struct RegionWrite
	{
		enum class Kind : std::uint8_t
		{
			FxValues,         // an effect's $Globals, with the pass's ScreenSize
			ColorCorrection,  // ColorCorrectionConstants
			SunMask,          // ENBAdaptation::SunMaskCB
			Dither,           // DitherConstants
		};
		Kind kind = Kind::FxValues;
		Fx effect = Fx::Effect;
		std::uint32_t offset = 0;
		float screenSize[4]{};
		float amplitude = 0.0f;
		std::string label;  // the pass that reads it, for CS_ORG_FX_READBACK
	};

	/** @brief One registered shape: its steps, its latch, and what the commit writes. */
	struct Frame
	{
		std::vector<Step> steps;
		std::vector<std::string> stepNames;
		std::vector<RegionWrite> writes;
		std::shared_ptr<org::LatchBlock> latch;
		std::uint32_t latchBytes = 0;
		std::array<bool, kEffects> runs{};
		std::shared_ptr<Resources> resources;
		std::vector<std::string> clears;  // the kept textures this frame clears first
		std::uint32_t fxPasses = 0;

		/** @brief CS_ORG_FX_READBACK: a copy of one step's target, exported to D3D11 and read back after the epoch. */
		struct Snapshot
		{
			std::string step;
			std::string target;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::unique_ptr<GraphResources::Texture2D> texture;
		};
		std::vector<Snapshot> snapshots;
		std::map<const org::Resource*, std::string> names;  // every resource the plan names, for the logs
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

	/** @brief One fullscreen draw: an .fx pass or one of CS's raster steps. */
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
			const std::uint32_t slot = a_recording.FrameSlot();
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
					words[i] = a_frame.latch->SrvIndex();
					break;
				case PushWord::Kind::LatchOffset:
					words[i] = static_cast<std::uint32_t>(a_frame.latch->Offset(slot)) + word.value;
					break;
				}
			}
			if (ORGFxRuntime::ReadbackRequested() && !draw.loggedWords.exchange(true)) {
				std::string list;
				for (std::uint32_t i = 0; i < count; ++i)
					list += fmt::format("{}[{}] {}={}", i ? ", " : "", i, i < draw.wordNames.size() ? draw.wordNames[i] : std::string("?"), words[i]);
				logger::info("[ORG] Effects11 readback: {} records {}x{}, {} vertices, push: {}", draw.name, draw.width, draw.height, draw.vertexCount, list);
			}
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

	struct ComputeBindings
	{
		org::DeclaredViewToken texture;
	};

	struct ComputeFrame
	{
		std::shared_ptr<const ComputeSpec> spec;
		std::shared_ptr<const org::LatchBlock> latch;
	};

	/** @brief The colour correction of the input's copy in place, as ApplyColorCorrection on kMAIN. */
	class ColorCorrectionPass final : public org::TypedRenderGraphPass<ColorCorrectionPass, ComputeFrame, ComputeBindings>
	{
	public:
		ColorCorrectionPass(std::shared_ptr<const ComputeSpec> a_spec, std::shared_ptr<const org::LatchBlock> a_latch) :
			spec(std::move(a_spec)), latch(std::move(a_latch)) {}

		ComputeBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.UnorderedAccess(spec->texture).View() };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(latch.get()));
		}

		ComputeFrame Prepare(const ComputeBindings&, const org::PassPrepareContext&) const { return { spec, latch }; }

		static void Record(const ComputeBindings& a_bindings, const ComputeFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.spec || !a_frame.spec->program || !a_frame.latch)
				return;
			const ColorCorrectionBlock block{ a_recording.Index(a_bindings.texture),
				ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot(), a_frame.spec->region) };
			a_frame.spec->program->Dispatch(a_recording, ShaderBindings::Data(block), a_frame.spec->groupsX, a_frame.spec->groupsY, 1);
		}

	private:
		std::shared_ptr<const ComputeSpec> spec;
		std::shared_ptr<const org::LatchBlock> latch;
	};

	struct CopyBindings
	{
		org::ResourceBindingToken from, to;
		std::uint32_t width = 1;
		std::uint32_t height = 1;
	};

	/** @brief A whole-texture copy of two textures of one format and size: the adaptation history, and CopyResource's cases. */
	class CopyPass final : public org::TypedRenderGraphPass<CopyPass, org::EmptyPassFrameData, CopyBindings>
	{
	public:
		explicit CopyPass(CopySpec a_spec) :
			spec(std::move(a_spec)) {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopySource(spec.from), a_builder.CopyDestination(spec.to), spec.width, spec.height };
		}

		static void Record(const CopyBindings& a_bindings, const org::EmptyPassFrameData&, org::PassRecordContext& a_recording)
		{
			const rhi::TextureCopyRegion target{ .texture = a_recording.Resolve(a_bindings.to).GetHandle(), .width = a_bindings.width, .height = a_bindings.height };
			const rhi::TextureCopyRegion source{ .texture = a_recording.Resolve(a_bindings.from).GetHandle(), .width = a_bindings.width, .height = a_bindings.height };
			a_recording.Commands().CopyTextureRegion(target, source);
		}

	private:
		CopySpec spec;
	};

	struct ClearBindings
	{
		org::DeclaredViewToken target;
		std::uint32_t width = 0;
		std::uint32_t height = 0;
	};

	/** @brief A kept texture cleared to zero once, by a render pass that only clears. */
	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, org::EmptyPassFrameData, ClearBindings>
	{
	public:
		explicit ClearPass(ClearSpec a_spec) :
			spec(std::move(a_spec)) {}

		ClearBindings Declare(org::PassBuilder& a_builder) { return { a_builder.RenderTarget(spec.texture).View(), spec.width, spec.height }; }

		static void Record(const ClearBindings& a_bindings, const org::EmptyPassFrameData&, org::PassRecordContext& a_recording)
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
	};

	// ---- helpers ----

	rhi::Filter FilterOf(OrgFx::FilterMode a_mode) { return a_mode == OrgFx::FilterMode::Point ? rhi::Filter::Nearest : rhi::Filter::Linear; }

	rhi::AddressMode AddressOf(OrgFx::AddressMode a_mode)
	{
		switch (a_mode) {
		case OrgFx::AddressMode::Wrap:
			return rhi::AddressMode::Wrap;
		case OrgFx::AddressMode::Mirror:
			return rhi::AddressMode::Mirror;
		case OrgFx::AddressMode::Border:
			return rhi::AddressMode::Border;
		case OrgFx::AddressMode::MirrorOnce:
			return rhi::AddressMode::MirrorOnce;
		default:
			return rhi::AddressMode::Clamp;
		}
	}

	rhi::CompareOp CompareOf(OrgFx::CompareFunc a_func)
	{
		constexpr rhi::CompareOp kOps[] = { rhi::CompareOp::Never, rhi::CompareOp::Less, rhi::CompareOp::Equal, rhi::CompareOp::LessEqual, rhi::CompareOp::Greater,
			rhi::CompareOp::NotEqual, rhi::CompareOp::GreaterEqual, rhi::CompareOp::Always };
		return kOps[static_cast<std::size_t>(a_func) % std::size(kOps)];
	}

	/** @brief A parsed sampler state as BasicRHI's. D3D11's custom border colours that are not a preset become Custom. */
	rhi::SamplerDesc SamplerOf(const OrgFx::SamplerDesc& a_sampler)
	{
		rhi::SamplerDesc desc{};
		desc.minFilter = FilterOf(a_sampler.minFilter);
		desc.magFilter = FilterOf(a_sampler.magFilter);
		desc.mipFilter = a_sampler.mipFilter == OrgFx::FilterMode::Point ? rhi::MipFilter::Nearest : rhi::MipFilter::Linear;
		desc.addressU = AddressOf(a_sampler.addressU);
		desc.addressV = AddressOf(a_sampler.addressV);
		desc.addressW = AddressOf(a_sampler.addressW);
		desc.mipLodBias = a_sampler.mipLodBias;
		// D3D11's default minimum is -FLT_MAX; below mip 0 there is nothing to clamp.
		desc.minLod = (std::max)(a_sampler.minLod, 0.0f);
		desc.maxLod = (std::max)(a_sampler.maxLod, desc.minLod);
		desc.maxAnisotropy = a_sampler.anisotropic ? (std::max)(a_sampler.maxAnisotropy, 1u) : 1u;
		switch (a_sampler.reduction) {
		case OrgFx::FilterReduction::Comparison:
			desc.compareEnable = true;
			desc.compareOp = CompareOf(a_sampler.compare);
			desc.reduction = rhi::ReductionMode::Comparison;
			break;
		case OrgFx::FilterReduction::Minimum:
			desc.reduction = rhi::ReductionMode::Min;
			break;
		case OrgFx::FilterReduction::Maximum:
			desc.reduction = rhi::ReductionMode::Max;
			break;
		default:
			break;
		}
		const auto& c = a_sampler.borderColor;
		if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 0.0f)
			desc.borderPreset = rhi::BorderPreset::TransparentBlack;
		else if (c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f && c[3] == 1.0f)
			desc.borderPreset = rhi::BorderPreset::OpaqueBlack;
		else if (c[0] == 1.0f && c[1] == 1.0f && c[2] == 1.0f && c[3] == 1.0f)
			desc.borderPreset = rhi::BorderPreset::OpaqueWhite;
		else {
			desc.borderPreset = rhi::BorderPreset::Custom;
			std::copy(c.begin(), c.end(), desc.borderColor);
		}
		return desc;
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

	/** @brief Whether ExecuteEffects runs the effect this frame. */
	bool Runs(Fx a_fx)
	{
		auto& manager = EffectManager::GetSingleton();
		switch (a_fx) {
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

	std::string TechniqueName(ID3DX11EffectTechnique* a_technique)
	{
		D3DX11_TECHNIQUE_DESC desc{};
		if (!a_technique || FAILED(a_technique->GetDesc(&desc)) || !desc.Name)
			return {};
		return desc.Name;
	}
}

namespace
{
	/** @brief One texel as four floats, for the formats the plan uses. False for a format it does not decode. */
	bool DecodeTexel(DXGI_FORMAT a_format, const std::uint8_t* a_texel, float (&a_out)[4])
	{
		using namespace DirectX::PackedVector;
		a_out[0] = a_out[1] = a_out[2] = 0.0f;
		a_out[3] = 1.0f;
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			std::memcpy(a_out, a_texel, 16);
			return true;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
			{
				const auto* h = reinterpret_cast<const HALF*>(a_texel);
				for (int c = 0; c < 4; ++c)
					a_out[c] = XMConvertHalfToFloat(h[c]);
				return true;
			}
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			{
				const auto* u = reinterpret_cast<const std::uint16_t*>(a_texel);
				for (int c = 0; c < 4; ++c)
					a_out[c] = u[c] / 65535.0f;
				return true;
			}
		case DXGI_FORMAT_R10G10B10A2_UNORM:
			{
				std::uint32_t v;
				std::memcpy(&v, a_texel, 4);
				a_out[0] = (v & 1023u) / 1023.0f;
				a_out[1] = ((v >> 10) & 1023u) / 1023.0f;
				a_out[2] = ((v >> 20) & 1023u) / 1023.0f;
				a_out[3] = (v >> 30) / 3.0f;
				return true;
			}
		case DXGI_FORMAT_R11G11B10_FLOAT:
			{
				XMFLOAT3PK packed;
				std::memcpy(&packed.v, a_texel, 4);
				DirectX::XMFLOAT3 value;
				DirectX::XMStoreFloat3(&value, XMLoadFloat3PK(&packed));
				a_out[0] = value.x;
				a_out[1] = value.y;
				a_out[2] = value.z;
				return true;
			}
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			for (int c = 0; c < 4; ++c)
				a_out[c] = a_texel[c] / 255.0f;
			return true;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			a_out[0] = a_texel[2] / 255.0f;
			a_out[1] = a_texel[1] / 255.0f;
			a_out[2] = a_texel[0] / 255.0f;
			a_out[3] = a_texel[3] / 255.0f;
			return true;
		case DXGI_FORMAT_R16_FLOAT:
			a_out[0] = XMConvertHalfToFloat(*reinterpret_cast<const HALF*>(a_texel));
			return true;
		case DXGI_FORMAT_R32_FLOAT:
			std::memcpy(a_out, a_texel, 4);
			return true;
		default:
			return false;
		}
	}

	std::uint32_t TexelBytes(DXGI_FORMAT a_format)
	{
		switch (a_format) {
		case DXGI_FORMAT_R32G32B32A32_FLOAT:
			return 16;
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R16G16B16A16_UNORM:
			return 8;
		case DXGI_FORMAT_R16_FLOAT:
			return 2;
		default:
			return 4;
		}
	}

	/** @brief The texture's per-channel mean, minimum and maximum (and non-finite count), through a staging copy. Stalls. */
	std::string Stats(ID3D11Texture2D* a_texture, DXGI_FORMAT a_format)
	{
		if (!a_texture)
			return "no D3D11 export";
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		D3D11_TEXTURE2D_DESC staging = desc;
		staging.Usage = D3D11_USAGE_STAGING;
		staging.BindFlags = 0;
		staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		staging.MiscFlags = 0;
		staging.MipLevels = 1;
		winrt::com_ptr<ID3D11Texture2D> copy;
		if (FAILED(globals::d3d::device->CreateTexture2D(&staging, nullptr, copy.put())))
			return "staging texture failed";
		auto* context = globals::d3d::context;
		context->CopySubresourceRegion(copy.get(), 0, 0, 0, 0, a_texture, 0, nullptr);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(copy.get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return "map failed";
		double sum[4]{};
		float low[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
		float high[4] = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
		std::uint64_t nonFinite = 0;
		const std::uint32_t texel = TexelBytes(a_format);
		bool decoded = true;
		for (UINT y = 0; y < desc.Height && decoded; ++y) {
			const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
			for (UINT x = 0; x < desc.Width; ++x) {
				float v[4];
				if (!DecodeTexel(a_format, row + static_cast<std::size_t>(x) * texel, v)) {
					decoded = false;
					break;
				}
				for (int c = 0; c < 4; ++c) {
					if (!std::isfinite(v[c])) {
						++nonFinite;
						continue;
					}
					sum[c] += v[c];
					low[c] = (std::min)(low[c], v[c]);
					high[c] = (std::max)(high[c], v[c]);
				}
			}
		}
		context->Unmap(copy.get(), 0);
		if (!decoded)
			return fmt::format("format {} is not decoded", static_cast<std::uint32_t>(a_format));
		const double count = static_cast<double>(desc.Width) * desc.Height;
		return fmt::format("mean ({:.4g}, {:.4g}, {:.4g}, {:.4g}) min ({:.4g}, {:.4g}, {:.4g}, {:.4g}) max ({:.4g}, {:.4g}, {:.4g}, {:.4g}) non-finite {}",
			sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count, low[0], low[1], low[2], low[3], high[0], high[1], high[2], high[3], nonFinite);
	}

	std::string Floats(std::span<const std::byte> a_bytes)
	{
		std::string out;
		const std::size_t count = (std::min)(a_bytes.size(), std::size_t{ 64 }) / 4;
		for (std::size_t i = 0; i < count; ++i) {
			float v;
			std::memcpy(&v, a_bytes.data() + i * 4, 4);
			out += fmt::format("{}{:.4g}", i ? " " : "", v);
		}
		return out;
	}
}

struct ORGFxRuntime::Impl
{
	std::array<FxEffect, kEffects> effects{};
	CsPrograms cs;
	std::shared_ptr<Resources> resources;
	std::shared_ptr<Frame> frame;  // the registered shape
	std::string shapeKey;
	std::string lastReason;
	bool registered = false;
	bool activeNow = false;
	bool failed = false;  // the graph failed: FX11 for the session
	std::uint32_t ranFrame = UINT32_MAX;
	std::uint32_t ranCount = 0;        // frames the graph ran
	std::uint32_t nextReadback = 600;  // CS_ORG_FX_READBACK: the ran frame whose plan carries the snapshots
	bool readbackNow = false;          // this frame's commit ran a plan with snapshots: AfterRun reads them
	bool bindingsLogged = false;
	std::string lastInputCheck;  // the last logged import and DXVK image of the tonemap input
	ID3D11Texture2D* checkedTexture = nullptr;
	const org::Resource* checkedImport = nullptr;
	std::vector<std::vector<std::uint8_t>> images = std::vector<std::vector<std::uint8_t>>(kEffects);

	FxEffect& EffectOf(Fx a_fx) { return effects[static_cast<std::size_t>(a_fx)]; }

	/** @brief Leaves the frame to FX11: the passes are unregistered, and a new reason is logged once. */
	void Inactive(const std::string& a_reason)
	{
		activeNow = false;
		if (registered) {
			FrameAnchors::Unregister(kClientId);
			registered = false;
			frame.reset();
			shapeKey.clear();
		}
		if (!a_reason.empty() && a_reason != lastReason)
			logger::warn("[ORG] Effects11 runs on FX11: {}", a_reason);
		lastReason = a_reason;
	}

	bool BuildCsPrograms(rhi::Device a_device)
	{
		if (cs.tried)
			return cs.colorCorrection && cs.downsample && cs.sunMask && cs.linearSampler;
		cs.tried = true;
		cs.colorCorrection = ComputeProgram::Load(a_device, { .source = "Effects11/ColorCorrectionCS.hlsl", .constantWords = ShaderBindings::Words<ColorCorrectionBlock>() });
		const std::vector<RasterProgram::Target> r11g11b10{ { .format = rhi::Format::R11G11B10_Float } };
		RasterProgram::Desc downsample{};
		downsample.pixelSource = "Effects11/DownsamplePS.hlsl";
		downsample.constantWords = kDownsampleWords;
		downsample.targets = r11g11b10;
		cs.downsample = RasterProgram::Load(a_device, downsample);
		RasterProgram::Desc sunMask{};
		sunMask.pixelSource = "Effects11/AdaptationSunMaskPS.hlsl";
		sunMask.constantWords = kSunMaskWords;
		sunMask.targets = r11g11b10;
		cs.sunMask = RasterProgram::Load(a_device, sunMask);
		// TextureManager's linear clamp sampler.
		cs.linearSampler = ShaderBindings::CreateSampler({});
		return cs.colorCorrection && cs.downsample && cs.sunMask && cs.linearSampler;
	}

	std::shared_ptr<const RasterProgram> CopyProgram(rhi::Device a_device, rhi::Format a_format)
	{
		if (const auto it = cs.copy.find(a_format); it != cs.copy.end())
			return it->second;
		RasterProgram::Desc desc{};
		desc.pixelSource = "Effects11/CopyPS.hlsl";
		desc.constantWords = kCopyWords;
		desc.targets = { { .format = a_format } };
		auto program = RasterProgram::Load(a_device, desc);
		cs.copy.emplace(a_format, program);
		return program;
	}

	/** @brief Parses and lowers one effect, maps its values onto FX11's and makes its bindings. False, with the reason kept, on failure. */
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
		if (!e.desc.constantBuffers.empty())
			return fail("cbuffer blocks are not supported yet");
		e.lowered = OrgFx::LowerEffect(text, e.desc);
		if (!e.lowered.ok)
			return fail("lowering failed, " + FirstError(e.lowered.diagnostics));

		// FX11 keeps the values; its $Globals must be laid out as the lowered code reads it.
		auto* effect = fx11->GetEffect();
		for (const auto& v : e.desc.variables) {
			if (v.bufferOffset < 0 || !v.cbuffer.empty() || v.modifiers.isStatic)
				continue;
			auto* variable = effect->GetVariableByName(v.name.c_str());
			if (!variable || !variable->IsValid())
				return fail(fmt::format("FX11 has no variable '{}'", v.name));
			D3DX11_EFFECT_VARIABLE_DESC variableDesc{};
			D3DX11_EFFECT_TYPE_DESC typeDesc{};
			if (FAILED(variable->GetDesc(&variableDesc)) || FAILED(variable->GetType()->GetDesc(&typeDesc)))
				return fail(fmt::format("FX11 cannot describe '{}'", v.name));
			if (variableDesc.BufferOffset != static_cast<std::uint32_t>(v.bufferOffset))
				return fail(fmt::format("'{}' is at byte {} of FX11's $Globals, {} of the parser's", v.name, variableDesc.BufferOffset, v.bufferOffset));
			const std::uint32_t offset = static_cast<std::uint32_t>(v.bufferOffset);
			if (offset >= e.lowered.valuesSize)
				continue;
			const std::uint32_t size = (std::min)({ typeDesc.UnpackedSize, v.bufferSize, e.lowered.valuesSize - offset });
			if (size)
				e.values.push_back({ variable, offset, size });
		}
		if (const auto* screenSize = e.desc.FindVariable("ScreenSize"); screenSize && screenSize->bufferOffset >= 0 && screenSize->cbuffer.empty()) {
			e.screenSize = screenSize->bufferOffset;
			e.screenSizeBytes = (std::min)(screenSize->bufferSize, 16u);
		}

		// Samplers from their state blocks (D3D11's default state for a bare SamplerState), textures loaded from files by FX11.
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
				continue;  // FX11 could not load it either and binds nothing: black
			winrt::com_ptr<ID3D11Resource> resource;
			cached->second->GetResource(resource.put());
			winrt::com_ptr<ID3D11Texture2D> texture;
			if (!resource || !resource.try_as(texture) || !texture)
				return fail(fmt::format("texture file '{}' is not a 2D texture", file));
			e.files[i] = FrameAnchors::D3D11Texture(fmt::format("effects11.{}.{}", e.Name(), slot.name), texture.get(), cached->second.get(), nullptr);
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

	/** @brief One pass's program for a target format, compiled on first use. Null, with a_reason, on failure. */
	std::shared_ptr<const RasterProgram> FxProgram(FxEffect& a_effect, std::uint32_t a_technique, std::uint32_t a_pass, rhi::Format a_format, std::string& a_reason)
	{
		const ProgramKey key{ a_technique, a_pass, a_format };
		if (const auto it = a_effect.programs.find(key); it != a_effect.programs.end()) {
			if (!it->second)
				a_reason = fmt::format("{}: technique '{}' pass {} did not compile", a_effect.Name(), a_effect.desc.techniques[a_technique].name, a_pass);
			return it->second;
		}
		const auto& technique = a_effect.desc.techniques[a_technique];
		const auto& pass = technique.passes[a_pass];
		auto remember = [&](std::shared_ptr<const RasterProgram> a_program) {
			a_effect.programs.emplace(key, a_program);
			return a_program;
		};
		if (!pass.blendState.empty() || !pass.depthStencilState.empty() || !pass.rasterizerState.empty() || !pass.otherStates.empty()) {
			a_reason = fmt::format("{}: technique '{}' pass {} sets render states, which are not supported yet", a_effect.Name(), technique.name, a_pass);
			return remember(nullptr);
		}
		const auto lowered = OrgFx::LowerPass(a_effect.lowered, a_effect.desc, a_technique, a_pass);
		if (!lowered.ok) {
			a_reason = fmt::format("{}: technique '{}' pass {} could not be lowered, {}", a_effect.Name(), technique.name, a_pass, FirstError(lowered.diagnostics));
			return remember(nullptr);
		}
		RasterProgram::Desc desc{};
		desc.pixelSource = fmt::format("Effects11/ORG/{}/{}.{}.hlsl", a_effect.Name(), technique.name, a_pass);
		desc.vertexSource = desc.pixelSource;
		desc.pixelEntry = std::wstring(OrgFx::kPixelEntry, OrgFx::kPixelEntry + std::strlen(OrgFx::kPixelEntry));
		desc.vertexEntry = std::wstring(OrgFx::kVertexEntry, OrgFx::kVertexEntry + std::strlen(OrgFx::kVertexEntry));
		desc.pixelText = lowered.source;
		desc.vertexText = lowered.source;
		desc.languageVersion = L"2018";
		desc.constantWords = a_effect.lowered.PushWords();
		desc.targets = { { .format = a_format } };
		auto program = RasterProgram::Load(RenderGraphRuntime::Get().Host()->Device(), desc);
		if (!program)
			a_reason = fmt::format("{}: DXC could not build technique '{}' pass {} (the [ORG] error above has its messages)", a_effect.Name(), technique.name, a_pass);
		return remember(std::move(program));
	}

	/** @brief What a plan needs from the frame. */
	struct Inputs
	{
		Res input, main, depth, output;  // main: kMAIN, when it has an RTV FX11's result is copied into
		bool inputCopies = false;        // the input has Original's format and size: a plain copy, as CopyResource
		bool mainCopies = false;         // kMAIN has Original's format and size
		rhi::Format mainFormat = rhi::Format::Unknown;
		std::uint32_t mainWidth = 0;
		std::uint32_t mainHeight = 0;
		rhi::Format outputFormat = rhi::Format::Unknown;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::array<bool, kEffects> runs{};
		DXGI_FORMAT inputDxgi = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT mainDxgi = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT outputDxgi = DXGI_FORMAT_UNKNOWN;
		std::uint32_t inputWidth = 0;
		std::uint32_t inputHeight = 0;
		bool snapshots = false;  // CS_ORG_FX_READBACK: copy every step's target for a readback after the epoch
	};

	/** @brief Builds one frame's steps; FX11's ping-pong becomes fixed textures. */
	class Planner
	{
	public:
		Planner(Impl& a_impl, const Inputs& a_inputs) :
			impl(a_impl), inputs(a_inputs), frame(std::make_shared<Frame>())
		{
			frame->resources = impl.resources;
			frame->runs = inputs.runs;
			Name(inputs.input, "engine.input", inputs.inputDxgi, inputs.inputWidth, inputs.inputHeight);
			Name(inputs.main, "engine.kMAIN", inputs.mainDxgi, inputs.mainWidth, inputs.mainHeight);
			Name(inputs.output, "engine.output", inputs.outputDxgi, inputs.outputWidth, inputs.outputHeight);
			Name(inputs.depth, "engine.depth", DXGI_FORMAT_UNKNOWN, 0, 0);
		}

		/** @brief A target's name, format and size, for the readback and the binding logs. */
		struct TargetInfo
		{
			std::string name;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
		};
		std::map<const org::Resource*, TargetInfo> targets;

		void Name(const Res& a_resource, std::string a_name, DXGI_FORMAT a_format, std::uint32_t a_width, std::uint32_t a_height)
		{
			if (a_resource)
				targets.emplace(a_resource.get(), TargetInfo{ std::move(a_name), a_format, a_width, a_height });
		}

		std::string NameOf(const Res& a_resource) const
		{
			if (!a_resource)
				return "null";
			const auto it = targets.find(a_resource.get());
			return it != targets.end() ? it->second.name : fmt::format("resource {:#x}", reinterpret_cast<std::uintptr_t>(a_resource.get()));
		}

		std::string error;

		Res Tex(std::string_view a_name)
		{
			auto* texture = impl.resources->Get(a_name);
			if (!texture) {
				Fail(fmt::format("the texture Effects11::{} could not be created", a_name));
				return nullptr;
			}
			const auto& desc = texture->Desc();
			Name(texture->Graph(), std::string(a_name), desc.Format, desc.Width, desc.Height);
			return texture->Graph();
		}

		Res Black() { return Tex("Black"); }

		void Fail(std::string a_reason)
		{
			if (error.empty())
				error = std::move(a_reason);
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

		/** @brief A fullscreen draw of one of CS's programs. */
		std::shared_ptr<DrawSpec> CsDraw(std::string a_name, std::shared_ptr<const RasterProgram> a_program, Res a_target, std::uint32_t a_width, std::uint32_t a_height)
		{
			auto spec = std::make_shared<DrawSpec>();
			spec->name = std::move(a_name);
			spec->program = std::move(a_program);
			spec->target = std::move(a_target);
			spec->width = a_width;
			spec->height = a_height;
			if (!spec->program)
				Fail(fmt::format("{}: its program could not be built", spec->name));
			return spec;
		}

		/** @brief CopyPS from a_source into a_target, with dither (the output copy) or without (EffectManager::CopyTexture's false). */
		void Copy(std::string a_name, Res a_source, Res a_target, rhi::Format a_format, std::uint32_t a_width, std::uint32_t a_height, float a_amplitude)
		{
			auto spec = CsDraw(a_name, impl.CopyProgram(RenderGraphRuntime::Get().Host()->Device(), a_format), std::move(a_target), a_width, a_height);
			const std::uint32_t region = Region(sizeof(DitherConstants));
			frame->writes.push_back({ .kind = RegionWrite::Kind::Dither, .offset = region, .amplitude = a_amplitude, .label = a_name });
			spec->push = { { PushWord::Kind::Input, spec->Input(a_source) }, { PushWord::Kind::LatchHandle, 0 }, { PushWord::Kind::LatchOffset, region } };
			spec->wordNames = { "SourceTexture:" + NameOf(a_source), "Dither latch", fmt::format("Dither offset (region {})", region) };
			AddStep(std::shared_ptr<const DrawSpec>(std::move(spec)), std::move(a_name));
		}

		/** @brief The texture a pass's slot reads: a file, the chain input, an effect-named input, depth, a shared render target, or black. */
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
			// D3D11 unbinds a view of the bound target, so FX11 reads zero there.
			if (!resource || resource == a_target)
				resource = Black();
			return resource;
		}

		/** @brief Every pass of a technique into a_targetName, as Effect::RenderPasses. */
		void FxTechnique(Fx a_fx, std::uint32_t a_technique, std::string_view a_targetName, const Res& a_color, const std::vector<std::pair<std::string_view, Res>>& a_names)
		{
			auto& e = impl.EffectOf(a_fx);
			const auto* spec = FindSpec(a_targetName);
			const Res target = Tex(a_targetName);
			if (!spec || !target)
				return;
			const std::uint32_t width = spec->size ? spec->size : impl.resources->width;
			const std::uint32_t height = spec->size ? spec->size : impl.resources->height;
			const rhi::Format format = rhi::helpers::ToRHI(spec->format);
			const auto& technique = e.desc.techniques[a_technique];
			for (std::uint32_t p = 0; p < technique.passes.size(); ++p) {
				std::string reason;
				auto program = impl.FxProgram(e, a_technique, p, format, reason);
				if (!program) {
					Fail(reason);
					return;
				}
				auto draw = std::make_shared<DrawSpec>();
				draw->name = fmt::format("Effects11::{} {} pass {}", e.Name(), technique.name, p);
				draw->program = std::move(program);
				draw->target = target;
				draw->width = width;
				draw->height = height;
				draw->vertexCount = OrgFx::kQuadVertexCount;
				const std::uint32_t region = Region(e.lowered.valuesSize);
				RegionWrite write{ .kind = RegionWrite::Kind::FxValues, .effect = a_fx, .offset = region };
				const float aspect = static_cast<float>(width) / static_cast<float>(height);
				write.screenSize[0] = static_cast<float>(width);
				write.screenSize[1] = 1.0f / static_cast<float>(width);
				write.screenSize[2] = aspect;
				write.screenSize[3] = 1.0f / aspect;
				write.label = draw->name;
				frame->writes.push_back(write);
				draw->push = { { PushWord::Kind::LatchHandle, 0 }, { PushWord::Kind::LatchOffset, region } };
				draw->wordNames = { "values latch", fmt::format("values offset (region {}, {} bytes)", region, e.lowered.valuesSize) };
				for (std::size_t i = 0; i < e.lowered.resources.size(); ++i) {
					const auto& slot = e.lowered.resources[i];
					if (slot.kind == OrgFx::ResourceSlot::Kind::Sampler) {
						draw->push.push_back({ PushWord::Kind::Literal, e.samplers[i].Index() });
						draw->samplers.push_back(e.samplers[i]);
						draw->wordNames.push_back(fmt::format("{}:sampler", slot.name));
					} else {
						const Res resource = Resolve(e, i, a_names, a_color, target);
						draw->push.push_back({ PushWord::Kind::Input, draw->Input(resource) });
						draw->wordNames.push_back(fmt::format("{}:{}", slot.name, NameOf(resource)));
					}
				}
				while (draw->push.size() < e.lowered.PushWords()) {
					draw->push.push_back({});
					draw->wordNames.push_back("pad");
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

		/** @brief Effect::ExecuteTechniqueSequence: the chain's enabled techniques, ping-ponging between a_output and a_temp. */
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
					Fail(fmt::format("{}: technique '{}' was not parsed", e.Name(), name));
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
					if (IsRenderTargetName(info.renderTargetName))
						output = info.renderTargetName;
					else if (fx11.effectTextureCache.contains(info.renderTargetName) || TextureManager::GetSingleton().FindCommonTexture(info.renderTargetName)) {
						Fail(fmt::format("{}: technique '{}' renders to '{}', which is not supported yet", e.Name(), name, info.renderTargetName));
						return result;
					}
					// Otherwise FX11 falls back to the chain's output too.
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

		/** @brief ExecuteTechnique: every pass of a technique by name, when the effect has it. */
		void Technique(Fx a_fx, const char* a_name, std::string_view a_target, const std::vector<std::pair<std::string_view, Res>>& a_names)
		{
			auto& e = impl.EffectOf(a_fx);
			if (const auto it = e.techniques.find(a_name); it != e.techniques.end())
				FxTechnique(a_fx, it->second, a_target, Black(), a_names);
		}

		/** @brief The frame, in ExecuteEffects' order. Null with error set on failure. */
		std::shared_ptr<Frame> Build()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			const auto& runs = inputs.runs;
			auto run = [&](Fx a_fx) { return runs[static_cast<std::size_t>(a_fx)]; };
			const std::uint32_t width = impl.resources->width;
			const std::uint32_t height = impl.resources->height;

			Black();

			// FX11 works on kMAIN, after copying the input there (CopyToTarget). Here that is Original, a graph texture with a UAV.
			const Res original = Tex("Original");
			if (inputs.inputCopies)
				AddStep(CopySpec{ "Effects11::CopyInput", inputs.input, original, width, height }, "copy-input");
			else
				Copy("Effects11::CopyInput", inputs.input, original, rhi::Format::R16G16B16A16_Float, width, height, 0.0f);

			// 1. Colour correction in place.
			{
				auto spec = std::make_shared<ComputeSpec>();
				spec->name = "Effects11::ColorCorrection";
				spec->program = impl.cs.colorCorrection;
				spec->texture = original;
				spec->groupsX = (width + 7) / 8;
				spec->groupsY = (height + 7) / 8;
				spec->region = Region(sizeof(ColorCorrectionConstants));
				frame->writes.push_back({ .kind = RegionWrite::Kind::ColorCorrection, .offset = spec->region, .label = spec->name });
				AddStep(std::shared_ptr<const ComputeSpec>(std::move(spec)), "color-correction");
			}
			// FX11 leaves the corrected image in kMAIN; nothing here reads kMAIN again. Before the output copy, which may be kMAIN.
			if (inputs.main) {
				if (inputs.mainCopies)
					AddStep(CopySpec{ "Effects11::CopyToMain", original, inputs.main, width, height }, "copy-to-main");
				else
					Copy("Effects11::CopyToMain", original, inputs.main, inputs.mainFormat, inputs.mainWidth, inputs.mainHeight, 0.0f);
			}

			// 3. The 1024 downsample and its two mips, for bloom, lens and adaptation.
			if (run(Fx::Bloom) || run(Fx::Lens) || run(Fx::Adaptation)) {
				const std::pair<std::string_view, std::uint32_t> levels[] = { { "Downsampled", 1024 }, { "Downsampled512", 512 }, { "Downsampled256", 256 } };
				Res source = original;
				for (const auto& [name, size] : levels) {
					auto spec = CsDraw(fmt::format("Effects11::Downsample {}", size), impl.cs.downsample, Tex(name), size, size);
					spec->push = { { PushWord::Kind::Input, spec->Input(source) }, { PushWord::Kind::Literal, impl.cs.linearSampler.Index() } };
					spec->samplers = { impl.cs.linearSampler };
					spec->wordNames = { "SourceTexture:" + NameOf(source), "LinearSampler" };
					source = spec->target;
					AddStep(std::shared_ptr<const DrawSpec>(std::move(spec)), fmt::format("downsample-{}", size));
				}
			}

			const Res downsampled = Tex("Downsampled");
			Res bloom = Black();
			Res lens = Black();
			Res adaptation = Black();

			// 4. enbbloom.fx and 5. enblens.fx, from the 1024 downsample.
			if (run(Fx::Bloom)) {
				const auto chain = Sequence(Fx::Bloom, impl.EffectOf(Fx::Bloom).fx11->GetSelectedTechnique(), downsampled, "Bloom", "BloomTemp",
					{ { "TextureDownsampled", downsampled }, { "TextureOriginal", original } });
				bloom = Tex(chain.executed && !chain.inOutput ? "BloomTemp" : "Bloom");
			}
			if (run(Fx::Lens)) {
				const auto chain = Sequence(Fx::Lens, impl.EffectOf(Fx::Lens).fx11->GetSelectedTechnique(), downsampled, "Lens", "HDRTemp",
					{ { "TextureDownsampled", downsampled }, { "TextureOriginal", original } });
				lens = Tex(chain.executed && !chain.inOutput ? "HDRTemp" : "Lens");
			}

			// 6. enbadaptation.fx: the sun-masked blurry mip, its Downsample to 16x16, its Draw from last frame's value, kept for the next.
			if (run(Fx::Adaptation)) {
				constexpr std::uint32_t kSize = ENBAdaptation::kSunMaskSize;
				auto mask = CsDraw("Effects11::AdaptationSunMask", impl.cs.sunMask, Tex("SunMasked"), kSize, kSize);
				const std::uint32_t region = Region(sizeof(ENBAdaptation::SunMaskCB));
				frame->writes.push_back({ .kind = RegionWrite::Kind::SunMask, .offset = region, .label = "Effects11::AdaptationSunMask" });
				mask->push = { { PushWord::Kind::Input, mask->Input(Tex("Downsampled256")) }, { PushWord::Kind::Literal, impl.cs.linearSampler.Index() },
					{ PushWord::Kind::LatchHandle, 0 }, { PushWord::Kind::LatchOffset, region } };
				mask->samplers = { impl.cs.linearSampler };
				mask->wordNames = { "SourceTexture:Downsampled256", "LinearSampler", "SunMask latch", fmt::format("SunMask offset (region {})", region) };
				const Res masked = mask->target;
				AddStep(std::shared_ptr<const DrawSpec>(std::move(mask)), "adaptation-sun-mask");
				Technique(Fx::Adaptation, "Downsample", "AdaptationCurrent", { { "TextureCurrent", masked } });
				Technique(Fx::Adaptation, "Draw", "Adaptation", { { "TextureCurrent", Tex("AdaptationCurrent") }, { "TexturePrevious", Tex("AdaptationHistory") } });
				AddStep(CopySpec{ "Effects11::AdaptationHistory", Tex("Adaptation"), Tex("AdaptationHistory") }, "adaptation-history");
				adaptation = Tex("Adaptation");
			}

			// 7. enbeffect.fx, from the corrected image into the SDR pair.
			std::string_view sdr = "SDRTemp";
			{
				const auto chain = Sequence(Fx::Effect, impl.EffectOf(Fx::Effect).fx11->GetSelectedTechnique(), original, "SDRTemp", "SDRTemp2",
					{ { "TextureOriginal", original }, { "TextureBloom", bloom }, { "TextureLens", lens }, { "TextureAdaptation", adaptation } });
				if (chain.executed && !chain.inOutput)
					sdr = "SDRTemp2";
				else if (!chain.executed)
					// Side-target-only sequences leave the SDR texture unwritten: this frame's image, as ENBEffect::Execute.
					Copy("Effects11::CopyOriginal", original, Tex("SDRTemp"), rhi::Format::R10G10B10A2_UNorm, width, height, 0.0f);
			}

			// 8. enbeffectpostpass.fx, from the SDR result into the other one, with Scratch as its temp.
			if (run(Fx::PostPass)) {
				const std::string_view other = sdr == "SDRTemp" ? "SDRTemp2" : "SDRTemp";
				const Res postPassInput = Tex(sdr);
				const auto chain = Sequence(Fx::PostPass, impl.EffectOf(Fx::PostPass).fx11->GetSelectedTechnique(), postPassInput, other, "Scratch",
					{ { "TextureOriginal", postPassInput } });
				if (chain.executed && chain.inOutput)
					sdr = other;
				else if (chain.executed && chain.inTemp)
					sdr = "Scratch";
			}

			// 9. The result into the tonemap's output, dithered.
			Copy("Effects11::CopyToOutput", Tex(sdr), inputs.output, inputs.outputFormat, inputs.outputWidth, inputs.outputHeight, 1.0f / 255.0f);

			if (!error.empty())
				return nullptr;
			// Kept textures not cleared yet start at zero, before anything reads them.
			std::vector<Step> clears;
			std::vector<std::string> clearNames;
			for (const auto& [name, texture] : impl.resources->textures) {
				if (texture->GetLifetime() == Lifetime::Transient || impl.resources->cleared.contains(name))
					continue;
				const auto& desc = texture->Desc();
				clears.push_back(ClearSpec{ fmt::format("Effects11::Clear {}", name), texture->Graph(), desc.Width, desc.Height });
				clearNames.push_back(fmt::format("clear-{}", name));
				frame->clears.push_back(name);
			}
			frame->steps.insert(frame->steps.begin(), std::make_move_iterator(clears.begin()), std::make_move_iterator(clears.end()));
			frame->stepNames.insert(frame->stepNames.begin(), std::make_move_iterator(clearNames.begin()), std::make_move_iterator(clearNames.end()));
			if (inputs.snapshots)
				AddSnapshots();
			for (const auto& [resource, info] : targets)
				frame->names.emplace(resource, info.name);
			frame->latch = std::make_shared<org::LatchBlock>("cs.effects11.latch", (std::max)(frame->latchBytes, kRegionAlignment), host->FrameSlots());
			return frame;
		}

		/** @brief CS_ORG_FX_READBACK: after every step but the clears, a copy of its target into an exported texture. */
		void AddSnapshots()
		{
			std::vector<Step> steps;
			std::vector<std::string> names;
			// A copy of a_target into an exported texture of its format and size, after the steps so far.
			auto snapshotOf = [&](const Res& a_target, const std::string& a_step) {
				const auto it = targets.find(a_target.get());
				if (it == targets.end() || it->second.format == DXGI_FORMAT_UNKNOWN || !it->second.width) {
					logger::warn("[ORG] Effects11 readback: step {} writes {}, whose format or size is unknown; not read back", a_step, NameOf(a_target));
					return;
				}
				const auto& info = it->second;
				const D3D11_TEXTURE2D_DESC desc{ .Width = info.width,
					.Height = info.height,
					.MipLevels = 1,
					.ArraySize = 1,
					.Format = info.format,
					.SampleDesc = { .Count = 1 },
					.Usage = D3D11_USAGE_DEFAULT,
					.BindFlags = D3D11_BIND_SHADER_RESOURCE };
				auto snapshot = std::make_unique<GraphResources::Texture2D>(desc, fmt::format("Effects11::Snapshot{:03}", frame->snapshots.size()),
					GraphResources::Options{ .lifetime = Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
				if (!*snapshot || !snapshot->D3D11()) {
					logger::warn("[ORG] Effects11 readback: no snapshot texture for step {}", a_step);
					return;
				}
				steps.push_back(CopySpec{ fmt::format("Effects11::Snapshot {}", a_step), a_target, snapshot->Graph(), info.width, info.height });
				names.push_back(fmt::format("snapshot-{}", frame->snapshots.size()));
				frame->snapshots.push_back({ a_step, info.name, info.format, std::move(snapshot) });
			};
			for (std::size_t i = 0; i < frame->steps.size(); ++i) {
				// The engine's input as the graph sees it, before anything reads it.
				if (frame->stepNames[i] == "copy-input" || frame->stepNames[i] == "Effects11::CopyInput")
					snapshotOf(inputs.input, "before-copy-input");
				Res target = std::visit(
					[](const auto& a_step) -> Res {
						using T = std::decay_t<decltype(a_step)>;
						if constexpr (std::is_same_v<T, std::shared_ptr<const DrawSpec>>)
							return a_step->target;
						else if constexpr (std::is_same_v<T, std::shared_ptr<const ComputeSpec>>)
							return a_step->texture;
						else if constexpr (std::is_same_v<T, CopySpec>)
							return a_step.to;
						else
							return nullptr;
					},
					frame->steps[i]);
				const bool clear = std::holds_alternative<ClearSpec>(frame->steps[i]);
				steps.push_back(std::move(frame->steps[i]));
				names.push_back(frame->stepNames[i]);
				if (clear || !target)
					continue;
				snapshotOf(target, frame->stepNames[i]);
			}
			frame->steps = std::move(steps);
			frame->stepNames = std::move(names);
		}

	private:
		Impl& impl;
		const Inputs& inputs;
		std::shared_ptr<Frame> frame;
	};

	/** @brief The shape the plan follows; a change registers a new plan. */
	std::string ShapeKey(const Inputs& a_inputs)
	{
		std::string key = fmt::format("{}x{} {} {}{} {} {}", resources->width, resources->height, reinterpret_cast<std::uintptr_t>(a_inputs.input.get()),
			a_inputs.inputCopies ? 'c' : 'd', reinterpret_cast<std::uintptr_t>(a_inputs.main.get()), static_cast<std::uint32_t>(a_inputs.mainFormat), a_inputs.mainWidth);
		key += a_inputs.snapshots ? " snapshots" : "";
		key += fmt::format(" {} {} {} {} c{}",
			reinterpret_cast<std::uintptr_t>(a_inputs.depth.get()), reinterpret_cast<std::uintptr_t>(a_inputs.output.get()), static_cast<std::uint32_t>(a_inputs.outputFormat),
			a_inputs.outputWidth, resources->cleared.size());
		for (std::size_t i = 0; i < kEffects; ++i) {
			if (!a_inputs.runs[i]) {
				key += " -";
				continue;
			}
			auto& fx11 = *effects[i].fx11;
			const auto chain = fx11.GetSelectedTechnique();
			key += " " + chain + ":";
			if (const auto it = fx11.techniques.find(chain); it != fx11.techniques.end())
				for (auto& info : it->second)
					key += fx11.IsTechniqueEnabled(info) ? '1' : '0';
		}
		return key;
	}

	void Register(const std::shared_ptr<Frame>& a_frame)
	{
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		for (std::size_t i = 0; i < a_frame->steps.size(); ++i) {
			FrameAnchors::PassDesc pass{};
			pass.name = fmt::format("{}.{:03}.{}", kClientId, i, a_frame->stepNames[i]);
			pass.anchor = FrameAnchors::Anchor::Tonemap;
			pass.order = kFirstOrder + static_cast<std::int32_t>(i);
			const auto latch = std::shared_ptr<const org::LatchBlock>(a_frame->latch);
			std::visit(
				[&](const auto& a_step) {
					using T = std::decay_t<decltype(a_step)>;
					if constexpr (std::is_same_v<T, std::shared_ptr<const DrawSpec>>) {
						pass.kind = FrameAnchors::PassKind::Render;
						pass.make = [a_step, latch]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DrawPass>(a_step, latch); };
					} else if constexpr (std::is_same_v<T, std::shared_ptr<const ComputeSpec>>) {
						pass.kind = FrameAnchors::PassKind::Compute;
						pass.make = [a_step, latch]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ColorCorrectionPass>(a_step, latch); };
					} else if constexpr (std::is_same_v<T, CopySpec>) {
						pass.kind = FrameAnchors::PassKind::Copy;
						pass.make = [a_step]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<CopyPass>(a_step); };
					} else {
						pass.kind = FrameAnchors::PassKind::Render;
						pass.make = [a_step]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<ClearPass>(a_step); };
					}
				},
				a_frame->steps[i]);
			client.passes.push_back(std::move(pass));
		}
		auto heldResources = a_frame->resources;
		std::shared_ptr<const Frame> heldFrame = a_frame;
		client.registerResources = [heldResources, heldFrame](org::RenderGraph& a_graph) {
			for (const auto& [name, texture] : heldResources->textures)
				texture->Register(a_graph);
			for (const auto& snapshot : heldFrame->snapshots)
				snapshot.texture->Register(a_graph);
		};
		client.active = [] {
			const auto& self = ORGFxRuntime::Get();
			return self.impl && self.impl->activeNow;
		};
		client.commit = [](const FrameAnchors::Point&) {
			if (auto& self = ORGFxRuntime::Get(); self.impl)
				self.impl->SafeCommit();
		};
		client.onFailed = [] {
			logger::error("[ORG] Effects11 returns to FX11: the render graph failed");
			if (auto& self = ORGFxRuntime::Get(); self.impl) {
				self.impl->failed = true;
				self.impl->registered = false;
				self.impl->activeNow = false;
				self.impl->ranFrame = UINT32_MAX;
				self.impl->frame.reset();
			}
		};
		if (!FrameAnchors::Register(std::move(client))) {
			// The last shape's client must not run with a latch nothing fills.
			FrameAnchors::Unregister(kClientId);
			registered = false;
			frame.reset();
			return;
		}
		registered = true;
		frame = a_frame;
		std::string effectsList;
		for (std::size_t i = 0; i < kEffects; ++i)
			if (a_frame->runs[i])
				effectsList += fmt::format("{}{}", effectsList.empty() ? "" : ", ", effects[i].Name());
		logger::info("[ORG] Effects11 runs on the render graph: {} passes ({} from .fx: {}), {} latch bytes, {}x{}", a_frame->steps.size(), a_frame->fxPasses, effectsList,
			a_frame->latchBytes, resources->width, resources->height);
		if (ORGFxRuntime::ReadbackRequested() && !bindingsLogged) {
			bindingsLogged = true;
			LogBindings(*a_frame);
		}
	}

	/** @brief The frame's decision and plan (ORGFxRuntime::Prepare, which catches what it throws). */
	void PrepareFrame(std::uint32_t a_input, std::uint32_t a_output);

	/** @brief After an exception: FX11 for the rest of the session. */
	void Fail(const char* a_what)
	{
		logger::error("[ORG] Effects11 returns to FX11 for the session: {}", a_what);
		failed = true;
		activeNow = false;
		ranFrame = UINT32_MAX;
		if (registered)
			FrameAnchors::Unregister(kClientId);
		registered = false;
		frame.reset();
		shapeKey.clear();
	}

	/** @brief Commit, with what it throws kept inside the epoch's callback. */
	void SafeCommit()
	{
		try {
			Commit();
		} catch (const std::exception& e) {
			// The passes already in this epoch run with the latch as it is; the next frames are FX11's.
			failed = true;
			activeNow = false;
			ranFrame = UINT32_MAX;
			logger::error("[ORG] Effects11 returns to FX11 for the session: the commit failed: {}", e.what());
		}
	}

	/** @brief CS_ORG_FX_READBACK: every step's target and inputs, and each draw's push words by name (their values at record time). */
	void LogBindings(const Frame& a_frame) const
	{
		auto nameOf = [&](const Res& a_resource) {
			const auto it = a_frame.names.find(a_resource.get());
			return it != a_frame.names.end() ? it->second : fmt::format("resource {:#x}", reinterpret_cast<std::uintptr_t>(a_resource.get()));
		};
		for (std::size_t i = 0; i < a_frame.steps.size(); ++i) {
			std::visit(
				[&](const auto& a_step) {
					using T = std::decay_t<decltype(a_step)>;
					if constexpr (std::is_same_v<T, std::shared_ptr<const DrawSpec>>) {
						std::string words;
						for (std::size_t w = 0; w < a_step->push.size(); ++w)
							words += fmt::format("{}[{}] {}", w ? ", " : "", w, w < a_step->wordNames.size() ? a_step->wordNames[w] : std::string("?"));
						std::string inputs;
						for (std::size_t n = 0; n < a_step->inputs.size(); ++n)
							inputs += fmt::format("{}{}", n ? ", " : "", nameOf(a_step->inputs[n]));
						logger::info("[ORG] Effects11 bindings: {:03} {} draws {}x{} into {} ({} words), inputs [{}], push [{}]", i, a_step->name, a_step->width,
							a_step->height, nameOf(a_step->target), a_step->program ? a_step->program->constantWords : 0, inputs, words);
					} else if constexpr (std::is_same_v<T, std::shared_ptr<const ComputeSpec>>)
						logger::info("[ORG] Effects11 bindings: {:03} {} dispatches {}x{} groups on {}, latch region {}", i, a_step->name, a_step->groupsX, a_step->groupsY,
							nameOf(a_step->texture), a_step->region);
					else if constexpr (std::is_same_v<T, CopySpec>)
						logger::info("[ORG] Effects11 bindings: {:03} {} copies {} into {} ({}x{})", i, a_step.name, nameOf(a_step.from), nameOf(a_step.to), a_step.width,
							a_step.height);
					else
						logger::info("[ORG] Effects11 bindings: {:03} {} clears {} ({}x{})", i, a_step.name, nameOf(a_step.texture), a_step.width, a_step.height);
				},
				a_frame.steps[i]);
		}
	}

	/**
	 * @brief Diagnostic: the VkImage the input's graph import wraps against the one DXVK reports for the D3D11 texture now. Asks DXVK
	 * (DescribeResource, which synchronizes with its worker) when the import or texture changes and on readback frames; logs a change.
	 */
	void CheckInputImage(std::uint32_t a_target, ID3D11Texture2D* a_texture, const Res& a_import, bool a_readbackFrame)
	{
		if (!a_readbackFrame && a_texture == checkedTexture && a_import.get() == checkedImport)
			return;
		checkedTexture = a_texture;
		checkedImport = a_import.get();
		const VkImage imported = a_import ? rhi::vulkan::get_resource(a_import->GetAPIResource()) : VK_NULL_HANDLE;
		DxvkOrgInteropResourceInfo info{};
		const bool described = RenderGraphRuntime::Get().DescribeResource(a_texture, info) && info.kind == DXVK_ORG_INTEROP_RESOURCE_IMAGE;
		const auto line = fmt::format("render target {} ({}): D3D11 texture {:#x}, graph import {:#x} wraps VkImage {:#x}; DXVK now {} VkImage {:#x} (layout {}, format {}, {}x{})",
			a_target, InputFromD3D11() ? "D3D11Texture import" : "EngineTarget import", reinterpret_cast<std::uintptr_t>(a_texture),
			reinterpret_cast<std::uintptr_t>(a_import.get()), reinterpret_cast<std::uintptr_t>(imported), described ? "reports" : "cannot describe",
			described ? reinterpret_cast<std::uintptr_t>(info.image.image) : 0, described ? static_cast<int>(info.image.layout) : -1,
			described ? static_cast<int>(info.image.format) : -1, described ? info.image.extent.width : 0, described ? info.image.extent.height : 0);
		if (line == lastInputCheck)
			return;
		lastInputCheck = line;
		const bool same = described && imported == info.image.image;
		logger::info("[ORG] Effects11 input check: {}{}", line, same ? "" : ": DIFFERENT");
	}

	/** @brief After the epoch that ran a plan with snapshots: each step's target, read back through D3D11. */
	void ReadSnapshots()
	{
		if (!readbackNow || !frame)
			return;
		readbackNow = false;
		nextReadback = ranCount + 1000;
		logger::info("[ORG] Effects11 readback of ran frame {} (game frame {}), {} snapshots", ranCount, globals::state->frameCount, frame->snapshots.size());
		for (const auto& snapshot : frame->snapshots) {
			const auto& desc = snapshot.texture->Desc();
			logger::info("[ORG] Effects11 readback: {} -> {} ({} {}x{}): {}", snapshot.step, snapshot.target, static_cast<std::uint32_t>(snapshot.format), desc.Width,
				desc.Height, Stats(snapshot.texture->D3D11(), snapshot.format));
		}
	}

	/** @brief Inside the epoch: this frame's values into the latch. FX11's variables hold them (EffectManager::UpdateEffectValues). */
	void Commit()
	{
		if (!frame || !frame->latch)
			return;
		auto& manager = EffectManager::GetSingleton();
		for (std::size_t i = 0; i < kEffects; ++i) {
			if (!frame->runs[i])
				continue;
			auto& e = effects[i];
			manager.UpdateEffectValues(*e.fx11);
			auto& image = images[i];
			image.assign(e.lowered.valuesSize, 0);
			for (const auto& value : e.values)
				value.variable->GetRawValue(image.data() + value.offset, 0, value.size);
		}
		const std::uint32_t slot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
		const auto& latch = *frame->latch;
		const bool logValues = !frame->snapshots.empty();
		const std::uint32_t frameCount = manager.frameCount;
		for (const auto& write : frame->writes) {
			switch (write.kind) {
			case RegionWrite::Kind::FxValues:
				{
					const auto index = static_cast<std::size_t>(write.effect);
					auto& image = images[index];
					const auto& e = effects[index];
					if (e.screenSize >= 0 && static_cast<std::size_t>(e.screenSize) + e.screenSizeBytes <= image.size())
						std::memcpy(image.data() + e.screenSize, write.screenSize, e.screenSizeBytes);
					latch.Write(slot, write.offset, std::as_bytes(std::span<const std::uint8_t>(image)));
					break;
				}
			case RegionWrite::Kind::ColorCorrection:
				{
					auto& settings = SettingManager::GetSingleton();
					const ColorCorrectionConstants constants{ settings.GetValue<float>(manager.ids.brightness), settings.GetValue<float>(manager.ids.gammaCurve), frameCount, 0 };
					latch.WriteValue(slot, write.offset, constants);
					break;
				}
			case RegionWrite::Kind::SunMask:
				{
					ENBAdaptation::SunMaskCB constants{};
					manager.enbAdaptation.SunMaskConstants(constants);
					latch.WriteValue(slot, write.offset, constants);
					break;
				}
			case RegionWrite::Kind::Dither:
				latch.WriteValue(slot, write.offset, DitherConstants{ frameCount, write.amplitude, { 0, 0 } });
				break;
			}
		}
		if (logValues) {
			const auto bytes = latch.Slot(slot);
			logger::info("[ORG] Effects11 readback: latch slot {} at offset {}, {} bytes per slot", slot, latch.Offset(slot), latch.Stride());
			for (const auto& write : frame->writes)
				if (write.offset < bytes.size())
					logger::info("[ORG] Effects11 readback: values of {} (region {}): {}", write.label, write.offset, Floats(bytes.subspan(write.offset)));
			readbackNow = true;
		}
		++ranCount;
		// The clears run with this frame; the next frame's plan leaves them out.
		for (const auto& name : frame->clears)
			frame->resources->cleared.insert(name);
		ranFrame = globals::state->frameCount;
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
	if (impl->registered)
		FrameAnchors::Unregister(kClientId);
	impl->registered = false;
	impl->activeNow = false;
	impl->frame.reset();
	impl->shapeKey.clear();
	for (auto& effect : impl->effects)
		effect = FxEffect{};
}

void ORGFxRuntime::Prepare(std::uint32_t a_input, std::uint32_t a_output)
{
	if (!Requested())
		return;
	if (!impl)
		impl = std::make_unique<Impl>();
	impl->activeNow = false;
	if (impl->failed)
		return;
	// Nothing may leave the engine's tonemap call: an error returns Effects11 to FX11 for the session.
	try {
		impl->PrepareFrame(a_input, a_output);
	} catch (const std::exception& e) {
		impl->Fail(e.what());
	} catch (...) {
		impl->Fail("an unknown exception");
	}
}

void ORGFxRuntime::Impl::PrepareFrame(std::uint32_t a_input, std::uint32_t a_output)
{
	auto& self = *this;

	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Tonemap))
		return self.Inactive("the render graph or its Tonemap anchor is not available");
	// Only frames FX11 would render: the same conditions as Effects11::RenderTonemap and ExecuteEffects.
	auto& manager = EffectManager::GetSingleton();
	if (globals::state->GetTonemapOwner() != State::TonemapOwner::kEffects11 || !manager.IsInitialized() || !manager.IsPresetLoaded())
		return self.Inactive({});
	if (manager.WillEffectRun(manager.enbDepthOfField, manager.ids.useDepthOfField))
		return self.Inactive("enbdepthoffield.fx is not supported on the render graph yet");

	auto* renderer = globals::game::renderer;
	auto& targets = renderer->GetRuntimeData().renderTargets;
	const auto& main = targets[RE::RENDER_TARGETS::kMAIN];
	if (a_input >= static_cast<std::uint32_t>(RE::RENDER_TARGETS::kTOTAL) || a_output >= static_cast<std::uint32_t>(RE::RENDER_TARGETS::kTOTAL))
		return self.Inactive(fmt::format("the tonemap reads render target {} and writes {}, which are not both render targets", a_input, a_output));
	const auto& input = targets[a_input];
	const auto& output = targets[a_output];
	if (!input.texture || !input.SRV || !output.texture || !output.RTV)
		return self.Inactive(fmt::format("the tonemap input {} has no SRV or its output {} has no RTV", a_input, a_output));

	Impl::Inputs inputs{};
	for (std::size_t i = 0; i < kEffects; ++i) {
		const auto fx = static_cast<Fx>(i);
		inputs.runs[i] = Runs(fx);
		if (!inputs.runs[i])
			continue;
		auto& effect = self.EffectOf(fx);
		if (!effect.built)
			self.BuildEffect(fx);
		if (!effect.ok)
			return self.Inactive(effect.reason);
	}
	if (!self.BuildCsPrograms(host->Device()))
		return self.Inactive("Effects11's own shaders (colour correction, downsample, sun mask) could not be built for the render graph");

	inputs.input = InputFromD3D11() ? FrameAnchors::D3D11Texture(fmt::format("effects11.input.{}", a_input), input.texture, input.SRV, nullptr) :
	                                  FrameAnchors::EngineTarget(a_input);
	inputs.depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
	inputs.output = FrameAnchors::EngineTarget(a_output);
	if (!inputs.input || !inputs.depth || !inputs.output)
		return self.Inactive(fmt::format("the tonemap input {}, kMAIN's depth or the output {} could not be imported", a_input, a_output));
	// kMAIN gets FX11's corrected image too when it can be drawn to.
	if (main.texture && main.RTV) {
		inputs.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
		D3D11_RENDER_TARGET_VIEW_DESC mainView{};
		main.RTV->GetDesc(&mainView);
		D3D11_TEXTURE2D_DESC mainDesc{};
		main.texture->GetDesc(&mainDesc);
		inputs.mainFormat = rhi::helpers::ToRHI(mainView.Format);
		inputs.mainDxgi = mainView.Format;
		inputs.mainWidth = mainDesc.Width;
		inputs.mainHeight = mainDesc.Height;
		if (inputs.mainFormat == rhi::Format::Unknown)
			inputs.main = nullptr;
	}
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
	output.RTV->GetDesc(&rtvDesc);
	D3D11_TEXTURE2D_DESC outputDesc{};
	output.texture->GetDesc(&outputDesc);
	inputs.outputFormat = rhi::helpers::ToRHI(rtvDesc.Format);
	inputs.outputDxgi = rtvDesc.Format;
	{
		D3D11_TEXTURE2D_DESC inputDesc{};
		input.texture->GetDesc(&inputDesc);
		inputs.inputDxgi = inputDesc.Format;
		inputs.inputWidth = inputDesc.Width;
		inputs.inputHeight = inputDesc.Height;
	}
	inputs.snapshots = ORGFxRuntime::ReadbackRequested() && self.ranCount >= self.nextReadback;
	if (ORGFxRuntime::ReadbackRequested() || InputFromD3D11())
		self.CheckInputImage(a_input, input.texture, inputs.input, inputs.snapshots);
	inputs.outputWidth = outputDesc.Width;
	inputs.outputHeight = outputDesc.Height;
	if (inputs.outputFormat == rhi::Format::Unknown)
		return self.Inactive(fmt::format("the tonemap output's format {} has no BasicRHI equivalent", static_cast<std::uint32_t>(rtvDesc.Format)));

	// The textures follow the screen, as TextureManager's.
	const std::uint32_t width = globals::game::graphicsState->screenWidth;
	const std::uint32_t height = globals::game::graphicsState->screenHeight;
	if (!self.resources || self.resources->width != width || self.resources->height != height) {
		self.resources = std::make_shared<Resources>();
		self.resources->width = width;
		self.resources->height = height;
	}
	// A texture copy where D3D11's CopyResource would apply: the same format and size as Original, mip 0 of one slice.
	auto copies = [&](ID3D11Texture2D* a_texture) {
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		return desc.Width == width && desc.Height == height && desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && desc.SampleDesc.Count == 1;
	};
	inputs.inputCopies = copies(input.texture);
	inputs.mainCopies = inputs.main && copies(main.texture);

	auto key = self.ShapeKey(inputs);
	if (!self.registered || key != self.shapeKey) {
		const auto started = std::chrono::steady_clock::now();
		Impl::Planner planner(self, inputs);
		auto planned = planner.Build();
		if (!planned) {
			// A pass that failed to build fails its effect until the next reload; FX11 runs it meanwhile.
			return self.Inactive(planner.error);
		}
		self.Register(planned);
		if (!self.registered)
			return self.Inactive("the render graph refused the passes");
		self.shapeKey = std::move(key);
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
		if (ms > 50.0)
			logger::info("[ORG] Effects11: the plan took {:.0f} ms (pass compiles included)", ms);
	}
	if (!self.lastReason.empty())
		self.lastReason.clear();
	self.activeNow = true;
}

bool ORGFxRuntime::RanThisFrame() const
{
	return impl && !impl->failed && impl->ranFrame == globals::state->frameCount;
}

void ORGFxRuntime::AfterRun()
{
	if (!impl || !ReadbackRequested())
		return;
	try {
		impl->ReadSnapshots();
	} catch (const std::exception& e) {
		logger::error("[ORG] Effects11 readback failed: {}", e.what());
	}
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Effects11 always runs on FX11.
struct ORGFxRuntime::Impl
{};

ORGFxRuntime::~ORGFxRuntime() = default;
void ORGFxRuntime::Reserve() {}
void ORGFxRuntime::Invalidate() {}
void ORGFxRuntime::Prepare(std::uint32_t, std::uint32_t) {}
bool ORGFxRuntime::RanThisFrame() const { return false; }
void ORGFxRuntime::AfterRun() {}

#endif
