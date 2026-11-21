// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "ORGUpscaling.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "Features/HDRDisplay/ORGHDRDisplay.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "RenderGraph/Switches.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/VersionedTexture.h>
#include <rhi_helpers.h>

#include <array>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace
{
	using RenderGraph::AnchorPasses::Bind;

	constexpr const char* kClientId = "cs.upscaling";
	constexpr const char* kHudlessClientId = "cs.upscaling.hudless";
	// Frame generation's inputs, after the hudless copy in the same step.
	constexpr const char* kFrameGenInputsClientId = "cs.upscaling.fg-inputs";
	// On only while an SDK upscaler is the method, so the post stage's build leaves the pass out with TAA or no upscaler.
	constexpr const char* kEvaluateClientId = "cs.upscaling.evaluate";
	constexpr const char* kUpscaledName = "Upscaling::UpscaledTexture";
	// One texture per format, in ORGUpscaling::kHudlessFormats' order.
	constexpr std::array<const char*, ORGUpscaling::kHudlessFormats.size()> kHudlessNames{ "Upscaling::HudlessTexture.RGBA8", "Upscaling::HudlessTexture.BGRA8",
		"Upscaling::HudlessTexture.RGB10A2" };
	constexpr const char* kDepthShader = "Upscaling/DepthRefractionUpscalePS.hlsl";
	constexpr const char* kUnderwaterShader = "Upscaling/UnderwaterMaskUpscalePS.hlsl";
	// A plain Load and write, for the hudless copy and the upscaled colour's copy into kMAIN.
	constexpr const char* kCopyShader = "Upscaling/CopyHudlessPS.hlsl";
	constexpr std::int32_t kOrderBase = 100;

	// What a frame queues for the Upscale anchor.
	constexpr std::uint32_t kCopyToMain = 1u << 0;
	constexpr std::uint32_t kDepthUpscale = 1u << 1;
	constexpr std::uint32_t kEvaluate = 1u << 2;

	// The copy into kMAIN, and the depth upscale's two draws.
	constexpr std::uint32_t kCopyEntry = 0;
	constexpr std::uint32_t kDepthEntry = 1;
	constexpr std::uint32_t kUpscaleEntries = 2;

	// Push blocks in list order.
	struct DepthBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle refractionNormals, depth, linearSampler;
		ShaderBindings::Latch jitter;
	};

	struct UnderwaterBlock
	{
		SharedConstants::Handles shared;
		ShaderBindings::Handle underwaterMask, linearSampler;
		ShaderBindings::Latch jitter;
	};

	struct CopyBlock
	{
		ShaderBindings::Handle colorInput;
	};

	static_assert(sizeof(Upscaling::JitterCB) == 16, "the latch struct is one 16-byte row, as the shaders' JitterConstants");

	namespace Targets = RenderGraph::Engine::Targets;

	/** @brief The format of an engine target's view. The same for the session. */
	rhi::Format FormatOf(Targets::TargetRef a_target)
	{
		return rhi::helpers::ToRHI(Targets::ViewFormat(a_target));
	}

	/** @brief The engine targets the Upscale anchor's passes use. */
	struct DepthInputs
	{
		FrameAnchors::Texture main;
		rhi::Format mainFormat = rhi::Format::Unknown;  // the copy's target format
		FrameAnchors::Texture mainDepth, motion;        // read by the evaluate where it is a stage's pass
		FrameAnchors::Texture depth, depthCopy;
		FrameAnchors::Texture refraction, refractionCopy, saoCameraZ;
		FrameAnchors::Texture underwater, underwaterCopy;
		std::uint32_t width = 0, height = 0;  // the depth upscale's render area, set per key

		bool operator==(const DepthInputs&) const = default;

		bool DepthComplete() const
		{
			return depth && depthCopy && refraction && refractionCopy && saoCameraZ && underwater && underwaterCopy;
		}

		/** @brief A copy with the depth upscale's render area: the key's display. */
		DepthInputs SizedFor(std::uint32_t a_width, std::uint32_t a_height) const
		{
			auto sized = *this;
			sized.width = a_width;
			sized.height = a_height;
			return sized;
		}
	};

	/** @brief The hudless copy's source as the last copy found it. */
	struct HudlessSource
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> view;

		bool operator==(const HudlessSource& a_other) const { return texture.get() == a_other.texture.get() && view.get() == a_other.view.get(); }
	};

	/** @brief What frame generation's inputs client is made for: the depth it reads, the motion vectors and the hudless texture. */
	struct InputsKey
	{
		FrameAnchors::Texture depth, motion, hudless;
		bool operator==(const InputsKey&) const = default;
	};

	/** @brief The engine's view formats. */
	struct DepthFormats
	{
		rhi::Format refraction = rhi::Format::Unknown, saoCameraZ = rhi::Format::Unknown, underwater = rhi::Format::Unknown;
		rhi::Format depth = rhi::Format::Unknown;

		bool operator==(const DepthFormats&) const = default;
	};

	/**
	* @brief kMAIN, and the depth and motion vectors the post stage's evaluate reads. Once, at setup: the engine targets are
	* declared slots that never change, and their formats are the session's.
	*/
	void CurrentMain(DepthInputs& a_inputs)
	{
		a_inputs.main = FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kMAIN));
		a_inputs.mainFormat = a_inputs.main ? FormatOf(Targets::Colour(RE::RENDER_TARGETS::kMAIN, Targets::View::Rtv)) : rhi::Format::Unknown;
		a_inputs.mainDepth = FrameAnchors::EngineTexture(FrameAnchors::Depth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
		a_inputs.motion = FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kMOTION_VECTOR));
	}

	/**
	* @brief The depth upscale's engine targets and their formats. Once, at setup, as CurrentMain. The copies are the declared copy
	* targets (Engine::<name> Copy). The copies follow each frame's versions; the render area is set per key (DepthInputs::SizedFor). Empty fields when one is not backed.
	*/
	void CurrentDepthInputs(DepthInputs& a_inputs, DepthFormats& a_formats)
	{
		constexpr auto kDepth = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
		constexpr auto kDepthCopy = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY);
		constexpr auto kRefraction = static_cast<std::uint32_t>(RE::RENDER_TARGET::kREFRACTION_NORMALS);
		constexpr auto kSaoCameraZ = static_cast<std::uint32_t>(RE::RENDER_TARGET::kSAO_CAMERAZ);
		constexpr auto kUnderwater = static_cast<std::uint32_t>(RE::RENDER_TARGET::kUNDERWATER_MASK);
		a_inputs.depth = FrameAnchors::EngineTexture(FrameAnchors::Depth(kDepth));
		a_inputs.depthCopy = FrameAnchors::EngineTexture(FrameAnchors::Depth(kDepthCopy));
		a_inputs.refraction = FrameAnchors::EngineTexture(FrameAnchors::Colour(kRefraction));
		a_inputs.refractionCopy = Targets::Slot(Targets::ColourCopy(kRefraction));
		a_inputs.saoCameraZ = FrameAnchors::EngineTexture(FrameAnchors::Colour(kSaoCameraZ));
		a_inputs.underwater = FrameAnchors::EngineTexture(FrameAnchors::Colour(kUnderwater));
		a_inputs.underwaterCopy = Targets::Slot(Targets::ColourCopy(kUnderwater));
		a_formats = { FormatOf(Targets::Colour(kRefraction, Targets::View::Rtv)), FormatOf(Targets::Colour(kSaoCameraZ, Targets::View::Rtv)),
			FormatOf(Targets::Colour(kUnderwater, Targets::View::Rtv)), FormatOf(Targets::Depth(kDepth, Targets::View::Dsv)) };
	}

	RasterProgram::Desc DepthDesc(const DepthFormats& a_formats)
	{
		// As the D3D11 states: no blending, depth written with the test always passing, no culling.
		return { .pixelSource = kDepthShader,
			.constantWords = ShaderBindings::Words<DepthBlock>(),
			.targets = { { .format = a_formats.refraction }, { .format = a_formats.saoCameraZ } },
			.depthFormat = a_formats.depth,
			.depth = { .depthEnable = true, .depthWrite = true, .depthFunc = rhi::CompareOp::Always } };
	}

	RasterProgram::Desc UnderwaterDesc(const DepthFormats& a_formats)
	{
		return { .pixelSource = kUnderwaterShader, .constantWords = ShaderBindings::Words<UnderwaterBlock>(), .targets = { { .format = a_formats.underwater } } };
	}

	RasterProgram::Desc CopyDesc(rhi::Format a_format)
	{
		return { .pixelSource = kCopyShader, .constantWords = ShaderBindings::Words<CopyBlock>(), .targets = { { .format = a_format } } };
	}

	struct EvaluateWork;
	struct EvaluateShared;

	/** @brief Default usage, no CPU access, no misc flags, as the graph twins require. */
	D3D11_TEXTURE2D_DESC Sanitized(D3D11_TEXTURE2D_DESC a_desc, UINT a_bindFlags)
	{
		a_desc.Usage = D3D11_USAGE_DEFAULT;
		a_desc.CPUAccessFlags = 0;
		a_desc.MiscFlags = 0;
		a_desc.BindFlags = a_bindFlags;
		return a_desc;
	}
}

struct ORGUpscaling::Impl
{
	// Queued at setup on the programs queue: the formats are the session's. A revision starts only once they are built.
	RenderGraph::Programs::Fixed<RasterProgram> depthProgram;
	RenderGraph::Programs::Fixed<RasterProgram> underwaterProgram;
	std::map<rhi::Format, RenderGraph::Programs::Fixed<RasterProgram>> copyPrograms;  // by target format
	DepthFormats formats;  // the depth upscale targets' formats, fixed at setup
	org::OwnedDescriptorBinding linearSampler;
	std::shared_ptr<org::LatchBlock> latch;
	std::shared_ptr<RenderGraph::RunGate> upscaleGate, hudlessGate;

	// Declared on variables (frame revision plan 6.14). Their extents change only at adoption.
	RenderGraph::Frame::DeclaredTexture2D upscaled;
	std::array<RenderGraph::Frame::DeclaredTexture2D, kHudlessFormats.size()> hudless;
	RenderGraph::Frame::DeclaredClient upscaleClient, hudlessClient;
	// The evaluate's own client, with a constant key while it has one (HasEvaluateClient).
	RenderGraph::Frame::DeclaredClient evaluateClient;
	// Frame generation's inputs, on the depth QueueFrameGenInputs reads (kMAIN's or its copy), observed (B2's interim).
	// Each registration keeps the key it was made for.
	RenderGraph::Frame::DeclaredClientOf<const InputsKey> inputsClient;
	RenderGraph::Frame::Observed<bool> inputsDepthCopy{ "Upscaling.FrameGenDepthCopy" };

	// The engine targets the Upscale anchor uses, found once at setup: their slots and formats never change in a session.
	DepthInputs inputs;
	// The hudless copy's source texture and view, as the last copy found them (B2's interim). The hudless client's key imports
	// them, so the copy makes no import mid-frame.
	RenderGraph::Frame::Observed<HudlessSource> hudlessSource{ "Upscaling.HudlessSource" };

	// Render thread.
	std::uint32_t queued = 0;
	Upscaling::JitterCB jitter{};
	// Native mode: each run's evaluate, stored by the commit.
	std::shared_ptr<const EvaluateShared> evaluateShared;
	std::shared_ptr<const EvaluateWork> evaluate;
	// Frame generation's inputs: each run's prepare, stored by the commit, and the engine targets the pass imports.
	std::shared_ptr<const EvaluateShared> inputsShared;
	std::shared_ptr<const EvaluateWork> inputsWork;
	bool inputsArmed = false;

	// Set only around CopyHudless's tap, so the clean scene's tap of the same step does not copy the hudless.
	bool hudlessArmed = false;
};

namespace
{
	using Impl = ORGUpscaling::Impl;

	/** @brief Immutable once built. */
	struct DepthSpec
	{
		std::shared_ptr<const RasterProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::shared_ptr<const RenderGraph::RunGate> gate;
		std::uint32_t sampler = 0;
		DepthInputs inputs;
	};

	struct DepthBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken refractionCopy, depthCopy, refraction, saoCameraZ, depth;
	};

	struct DepthFrame
	{
		std::shared_ptr<const DepthSpec> spec;
	};

	/** @brief Writes two colour targets and depth, which AnchorPasses::Draw does not cover, so it is its own pass. Gated by the depth entry. */
	class DepthUpscalePass final : public org::TypedRenderGraphPass<DepthUpscalePass, DepthFrame, DepthBindings>
	{
	public:
		explicit DepthUpscalePass(std::shared_ptr<const DepthSpec> a_spec) :
			spec(std::move(a_spec)) {}

		DepthBindings Declare(org::PassBuilder& a_builder)
		{
			const auto& inputs = spec->inputs;
			DepthBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			bindings.refractionCopy = a_builder.ShaderResource(inputs.refractionCopy).View();
			bindings.depthCopy = a_builder.ShaderResource(inputs.depthCopy).View();
			bindings.refraction = a_builder.RenderTarget(inputs.refraction).View();
			bindings.saoCameraZ = a_builder.RenderTarget(inputs.saoCameraZ).View();
			bindings.depth = a_builder.DepthReadWrite(inputs.depth).View();
			return bindings;
		}

		// The recording depends only on what the pass was made with.
		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get())); }

		DepthFrame Prepare(const DepthBindings&, const org::PassPrepareContext&) const { return { spec }; }

		static void Record(const DepthBindings& a_bindings, const DepthFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto& spec = *a_frame.spec;
			const DepthBlock block{ SharedConstants::Index(a_recording, a_bindings.shared), a_recording.Index(a_bindings.refractionCopy),
				a_recording.Index(a_bindings.depthCopy), spec.sampler, ShaderBindings::LatchOf(*spec.latch, a_recording.Run()) };
			const std::array<RasterProgram::Colour, 2> colours{ { { .view = &a_bindings.refraction }, { .view = &a_bindings.saoCameraZ } } };
			const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
			spec.program->Draw(a_recording, { .colours = colours,
												.depth = &depth,
												.viewport = { .width = spec.inputs.width, .height = spec.inputs.height },
												.constants = ShaderBindings::Data(block),
												.debugName = "Upscaling::DepthUpscale",
												.indirect = &spec.gate->Latch(),
												.indirectOffset = spec.gate->Offset(kDepthEntry) });
		}

	private:
		std::shared_ptr<const DepthSpec> spec;
	};


	struct EvaluateWork
	{
		std::string label;
		ORGUpscaling::EvaluateRecord record;
	};

	/** @brief Each run's evaluate, stored by the Upscale client's commit. */
	struct EvaluateShared
	{
		RenderGraph::RunSlots<EvaluateWork> runs;
	};

	struct EvaluateBindings
	{
		org::ResourceBindingToken colorIn, colorOut, depth, motion;
	};

	struct EvaluateFrame
	{
		std::shared_ptr<const EvaluateShared> shared;
	};

	/**
	* @brief Records Streamline's tags and evaluate on this pass's command list. Its four images are legacy interop uses in the
	* common state, which the tags name. Recorded at execution from the run's stored evaluate. Rebinds the graph's heaps after.
	*/
	class EvaluatePass final : public org::TypedRenderGraphPass<EvaluatePass, EvaluateFrame, EvaluateBindings>
	{
	public:
		EvaluatePass(std::shared_ptr<const EvaluateShared> a_shared, DepthInputs a_inputs, FrameAnchors::Texture a_output) :
			shared(std::move(a_shared)), inputs(std::move(a_inputs)), output(std::move(a_output)) {}

		EvaluateBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			EvaluateBindings bindings{};
			bindings.colorIn = a_builder.LegacyInteropUse(inputs.main).Resource();
			bindings.colorOut = a_builder.LegacyInteropUse(output).Resource();
			bindings.depth = a_builder.LegacyInteropUse(inputs.mainDepth).Resource();
			bindings.motion = a_builder.LegacyInteropUse(inputs.motion).Resource();
			// The upscaled texture is declared through its slot, so a new version needs no build.
			if (RenderGraph::Backend::IsD3D12() && output.Slot())
				a_builder.InternalTransition(org::ResourceResolverAndRange(*output.Slot()),
					org::ResourceState{ rhi::ResourceAccessType::Common, rhi::ResourceLayout::Common, rhi::ResourceSyncState::All });
			return bindings;
		}

		// Recorded at execution from the run's evaluate, so nothing here prevents reuse.
		void Revision(std::vector<std::uint64_t>&) const {}

		EvaluateFrame Prepare(const EvaluateBindings&, const org::PassPrepareContext&) const { return { shared }; }

		static bool SkipsAtExecution(const EvaluateBindings&, const EvaluateFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return !a_frame.shared || !a_frame.shared->runs.Find(a_recording.Run());
		}

		static void Record(const EvaluateBindings& a_bindings, const EvaluateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto work = a_frame.shared ? a_frame.shared->runs.Take(a_recording.Run()) : nullptr;
			if (!work || !work->record)
				return;
			const std::array<rhi::Resource, 4> images{ a_recording.Resolve(a_bindings.colorIn), a_recording.Resolve(a_bindings.colorOut),
				a_recording.Resolve(a_bindings.depth), a_recording.Resolve(a_bindings.motion) };
			void* nativeList = nullptr;
			std::vector<RenderGraph::Engine::Native::ExternalImageInfo> infos;
			std::shared_ptr<const void> views;
			std::string why;
			const bool described = RenderGraph::Engine::Native::DescribeForCommands(a_recording.Commands(), images, nativeList, infos, views, why);
			if (views)
				a_recording.Retain(std::move(views));
			if (!described)
				RenderGraph::Fatal(ORGUpscaling::kName, fmt::format("{} cannot be recorded ({})", work->label, why));
			const bool evaluated = work->record(nativeList, infos);
			// Streamline may bind its own descriptor heaps, so restore the frame's.
			a_recording.RestoreDescriptorHeaps();
			if (!evaluated) {
				static Util::ReasonLog s_refused("[ORG] Upscaling: the SDK refused, so the frame keeps the previous output", spdlog::level::err);
				s_refused.Report(work->label);
			}
		}

	private:
		std::shared_ptr<const EvaluateShared> shared;
		DepthInputs inputs;
		FrameAnchors::Texture output;
	};

	struct InputsBindings
	{
		org::ResourceBindingToken depth, motion, hudless;
	};

	/**
	* @brief Records FSR-FG's tags and prepare on this pass's command list, after the hudless copy. Its three images are legacy interop
	* uses in the common state, which the tags name, and the hudless stays in that state until the present reads it. Recorded at
	* execution from the run's stored prepare, so a replayed recording records it again. Rebinds the graph's heaps after.
	*/
	class FrameGenInputsPass final : public org::TypedRenderGraphPass<FrameGenInputsPass, EvaluateFrame, InputsBindings>
	{
	public:
		FrameGenInputsPass(std::shared_ptr<const EvaluateShared> a_shared, FrameAnchors::Texture a_depth, FrameAnchors::Texture a_motion,
			FrameAnchors::Texture a_hudless) :
			shared(std::move(a_shared)), depth(std::move(a_depth)), motion(std::move(a_motion)), hudless(std::move(a_hudless)) {}

		InputsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			InputsBindings bindings{};
			bindings.depth = a_builder.LegacyInteropUse(depth).Resource();
			bindings.motion = a_builder.LegacyInteropUse(motion).Resource();
			bindings.hudless = a_builder.LegacyInteropUse(hudless).Resource();
			return bindings;
		}

		// Recorded at execution from the run's prepare, so nothing here prevents reuse.
		void Revision(std::vector<std::uint64_t>&) const {}

		EvaluateFrame Prepare(const InputsBindings&, const org::PassPrepareContext&) const { return { shared }; }

		static bool SkipsAtExecution(const InputsBindings&, const EvaluateFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return !a_frame.shared || !a_frame.shared->runs.Find(a_recording.Run());
		}

		static void Record(const InputsBindings& a_bindings, const EvaluateFrame& a_frame, org::PassRecordContext& a_recording)
		{
			const auto work = a_frame.shared ? a_frame.shared->runs.Take(a_recording.Run()) : nullptr;
			if (!work || !work->record)
				return;
			const std::array<rhi::Resource, 3> images{ a_recording.Resolve(a_bindings.depth), a_recording.Resolve(a_bindings.motion),
				a_recording.Resolve(a_bindings.hudless) };
			void* nativeList = nullptr;
			std::vector<RenderGraph::Engine::Native::ExternalImageInfo> infos;
			std::shared_ptr<const void> views;
			std::string why;
			const bool described = RenderGraph::Engine::Native::DescribeForCommands(a_recording.Commands(), images, nativeList, infos, views, why);
			if (views)
				a_recording.Retain(std::move(views));
			if (!described)
				RenderGraph::Fatal(ORGUpscaling::kName, fmt::format("{} cannot be recorded ({})", work->label, why));
			const bool prepared = work->record(nativeList, infos);
			// Streamline may bind its own descriptor heaps, so restore the frame's.
			a_recording.RestoreDescriptorHeaps();
			if (!prepared) {
				static Util::ReasonLog s_refused("[ORG] Upscaling: the SDK refused, so the frame is presented without generation", spdlog::level::err);
				s_refused.Report(work->label);
			}
		}

	private:
		std::shared_ptr<const EvaluateShared> shared;
		FrameAnchors::Texture depth, motion;
		FrameAnchors::Texture hudless;
	};

	/** @brief Setup. Queues the copy into a_format once on the programs queue. */
	void QueueCopyProgram(Impl& a_impl, rhi::Format a_format)
	{
		if (!a_impl.copyPrograms.contains(a_format))
			a_impl.copyPrograms.emplace(a_format, RenderGraph::Programs::Queue("Upscaling copy", CopyDesc(a_format)));
	}

	/** @brief The copy into a_format. Queued at setup, so it is built once a revision starts. Fatal for a format setup did not queue. */
	std::shared_ptr<const RasterProgram> CopyProgram(const Impl& a_impl, rhi::Format a_format)
	{
		const auto found = a_impl.copyPrograms.find(a_format);
		if (found == a_impl.copyPrograms.end())
			RenderGraph::Fatal(ORGUpscaling::kName, fmt::format("no copy program was queued for format {}", static_cast<int>(a_format)));
		return found->second.Get();
	}

	namespace Vars = RenderGraph::Frame::Vars;
	constexpr const auto& kHudlessFormats = ORGUpscaling::kHudlessFormats;

	bool SdkMethod(Vars::Upscaler a_method)
	{
		return a_method == Vars::Upscaler::FSR || a_method == Vars::Upscaler::DLSS || a_method == Vars::Upscaler::XeSS;
	}

	/** @brief Which hudless texture a revision's values choose: none without frame generation. Inputs or the adopted revision. */
	template <class Values>
	std::optional<std::uint32_t> HudlessIndexIn(const Values& a_values)
	{
		if (!a_values.Get(globals::features::upscaling.settings.frameGeneration))
			return std::nullopt;
		const auto surface = a_values.Get(Vars::PresenterFormat());
		return ORGUpscaling::HudlessFormatIndex(a_values.Get(Vars::HdrEnabled()), static_cast<DXGI_FORMAT>(surface.format), surface.hdr10);
	}

	/** @brief What the Upscale client's registration depends on. */
	struct UpscaleKey
	{
		Vars::Extent2D display{};  // kMAIN's, the copy's render area
		bool depth = false;        // the depth upscale is in the registration: an SDK upscaler. The frame opens it below native.
		bool operator==(const UpscaleKey&) const = default;
	};

	/** @brief What the hudless client's registration depends on. */
	struct HudlessKey
	{
		std::uint32_t format = 0;  // the index in kHudlessFormats
		Vars::Extent2D display{};
		FrameAnchors::Texture source;  // the copy's source, imported by the key
		bool operator==(const HudlessKey&) const = default;
	};

	/**
	* @brief The hudless copy's source as the graph reads it: the back buffer, HDR's twin, or an import of the D3D11 texture.
	* Call it only from the hudless client's key, where a new texture joins the revision.
	*/
	FrameAnchors::Texture HudlessSourceOf(ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_view)
	{
		// HDR's clean composite is its output twin in graph mode, ordered after its writes, else an imported D3D11 texture.
		const auto& framebuffer = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		if (framebuffer.texture == a_texture)
			return FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kFRAMEBUFFER));
		if (auto slot = ORGHDRDisplay::Get().GraphOf(a_texture))
			return slot;
		return FrameAnchors::ImportTexture("cs.d3d11.Upscaling::HudlessSource", { .texture = a_texture, .srv = a_view });
	}

	/** @brief A declared twin as passes declare it: its slot. */
	FrameAnchors::Texture SlotOf(const RenderGraph::Frame::DeclaredTexture2D& a_texture)
	{
		return a_texture ? FrameAnchors::Texture(a_texture->Name(), a_texture->Versioned()) : FrameAnchors::Texture();
	}

	/** @brief Frame generation's inputs for the depth a_depthCopy names and a hudless texture. The engine slots never change. */
	InputsKey InputsKeyFor(bool a_depthCopy, FrameAnchors::Texture a_hudless)
	{
		const auto depth = a_depthCopy ? RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY : RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN;
		return InputsKey{ FrameAnchors::EngineTexture(FrameAnchors::Depth(depth)),
			FrameAnchors::EngineTexture(FrameAnchors::Colour(RE::RENDER_TARGETS::kMOTION_VECTOR)), std::move(a_hudless) };
	}

	/** @brief Whether the evaluate has its own client: a stage holds it and its inputs exist. Fixed at setup. */
	bool HasEvaluateClient(const Impl& a_impl)
	{
		return a_impl.inputs.main && a_impl.inputs.mainDepth && a_impl.inputs.motion;
	}

	/** @brief The evaluate's registration: first at the Upscale anchor, recorded at execution and not gated. */
	bool MakeEvaluateClient(const std::shared_ptr<Impl>& a_impl, FrameAnchors::ClientDesc& a_evaluate)
	{
		auto& impl = *a_impl;
		const std::weak_ptr<Impl> weak = a_impl;
		auto shared = impl.evaluateShared;
		auto inputs = impl.inputs;
		auto output = SlotOf(impl.upscaled);
		a_evaluate.passes.push_back({ .name = std::string(kClientId) + ".evaluate",
			.anchor = FrameAnchors::Anchor::Upscale,
			.order = kOrderBase - 1,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [shared, inputs, output]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<EvaluatePass>(shared, inputs, output); },
			.atExecution = true });
		a_evaluate.declared = [] { return RenderGraph::Frame::Driver::Current().post.upscaler; };
		a_evaluate.active = [weak] {
			const auto current = weak.lock();
			return current && (current->queued & kEvaluate) != 0;
		};
		return true;
	}

	/**
	* @brief The Upscale client for a_key: the copy into kMAIN, plus the depth upscale when the key has it. Registers the evaluate
	* client beside it. False, logged, on failure.
	*/
	bool MakeUpscaleClient(const std::shared_ptr<Impl>& a_impl, const UpscaleKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& impl = *a_impl;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Upscale, kClientId, kOrderBase, impl.latch, impl.upscaleGate);
		const auto sized = impl.inputs.SizedFor(a_key.display.width, a_key.display.height);
		if (impl.upscaled && impl.inputs.main) {
			builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "copy-to-main",
				.program = CopyProgram(impl, impl.inputs.mainFormat),
				.bindings = { Bind::Srv(*impl.upscaled) },
				.target = impl.inputs.main,
				.viewport = { .width = a_key.display.width, .height = a_key.display.height },
				.gate = kCopyEntry });
		}
		const bool depth = a_key.depth && impl.inputs.DepthComplete();
		if (depth) {
			const auto& inputs = sized;
			// The copies run with the depth upscale, so a frame at native quality skips them, as D3D11 does.
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "depth-copy",
				.source = inputs.depth,
				.target = inputs.depthCopy,
				// The copy follows each frame's versions: the targets are not all full size (the underwater mask is half).
				.width = 0,
				.height = 0,
				.gate = kDepthEntry });
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "refraction-normals-copy",
				.source = inputs.refraction,
				.target = inputs.refractionCopy,
				.width = 0,
				.height = 0,
				.gate = kDepthEntry });
			const auto depthProgram = impl.depthProgram.Get();
			if (!depthProgram)
				builder.Fail("the depth upscale's program is missing");
			auto spec = std::make_shared<const DepthSpec>(DepthSpec{ depthProgram, impl.latch, impl.upscaleGate, impl.linearSampler.Index(), inputs });
			builder.Add("depth-upscale", FrameAnchors::PassKind::Render,
				[spec]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DepthUpscalePass>(spec); });
			builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "underwater-mask-copy",
				.source = inputs.underwater,
				.target = inputs.underwaterCopy,
				.width = 0,
				.height = 0,
				.gate = kDepthEntry });
			// At half the screen, as the D3D11 path's viewport.
			builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "underwater-mask-upscale",
				.program = impl.underwaterProgram.Get(),
				.bindings = { Bind::Shared(), Bind::Srv(inputs.underwaterCopy), Bind::Handle(impl.linearSampler.Index()), Bind::Latch(0) },
				.target = inputs.underwater,
				.viewport = { .width = inputs.width / 2, .height = inputs.height / 2 },
				.gate = kDepthEntry });
		}
		if (builder.Failed()) {
			logger::error("[ORG] Upscaling: {}", builder.Failure());
			return false;
		}

		const std::weak_ptr<Impl> weak = a_impl;
		for (auto& pass : builder.Take())
			a_client.passes.push_back(std::move(pass));
		// The depth upscale's own pass reads the shared constants too.
		a_client.sharedConstants = depth;
		a_client.gates = { impl.upscaleGate };
		a_client.registerResources = [weak](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->upscaled)
				impl->upscaled->Register(a_graph);
		};
		a_client.commit = [weak](const FrameAnchors::Point&) {
			const auto impl = weak.lock();
			if (!impl)
				return;
			const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
			impl->latch->WriteValue(run, 0, impl->jitter);
			impl->evaluateShared->runs.Store(run, (impl->queued & kEvaluate) ? impl->evaluate : nullptr);
			// The evaluate is in the post stage's build only while its keyed build for an SDK upscaler has switched in.
			// Until then its output was not written, so do not copy it.
			const bool evaluated = !(impl->queued & kEvaluate) || FrameAnchors::Place(kEvaluateClientId, FrameAnchors::Anchor::Upscale).draws;
			if ((impl->queued & kCopyToMain) && evaluated)
				impl->upscaleGate->OpenDraw(kCopyEntry);
			if (impl->queued & kDepthUpscale)
				impl->upscaleGate->OpenDraw(kDepthEntry);
		};
		a_client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->queued != 0;
		};
		return true;
	}

	/** @brief The hudless client for a_key: the copy into the key's format's texture. False, logged, on failure. */
	bool MakeHudlessClient(const std::shared_ptr<Impl>& a_impl, const HudlessKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& impl = *a_impl;
		const auto index = a_key.format;
		if (index >= impl.hudless.size() || !impl.hudless[index] || !a_key.source) {
			logger::error("[ORG] Upscaling: the hudless copy has no texture or no source for format index {}", index);
			return false;
		}
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::FrameGeneration, kClientId, kOrderBase, nullptr, impl.hudlessGate);
		builder.Add(RenderGraph::AnchorPasses::Draw{ .name = "hudless-copy",
			.program = CopyProgram(impl, rhi::helpers::ToRHI(kHudlessFormats[index])),
			.bindings = { Bind::Srv(a_key.source) },
			.target = SlotOf(impl.hudless[index]),
			.viewport = { .width = a_key.display.width, .height = a_key.display.height },
			.gate = 0 });
		if (builder.Failed()) {
			logger::error("[ORG] Upscaling: {}", builder.Failure());
			return false;
		}

		a_client.passes = builder.Take();
		a_client.gates = { impl.hudlessGate };
		const std::weak_ptr<Impl> weak = a_impl;
		a_client.registerResources = [weak, index](org::RenderGraph& a_graph) {
			if (const auto impl = weak.lock(); impl && impl->hudless[index])
				impl->hudless[index]->Register(a_graph);
		};
		a_client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->hudlessArmed;
		};
		a_client.commit = [weak](const FrameAnchors::Point&) {
			if (const auto impl = weak.lock())
				impl->hudlessGate->OpenDraw(0);
		};
		// The UI stage's frame generation step also takes the clean scene's tap in the same frame.
		a_client.repeats = true;
		return true;
	}

	/** @brief The textures, declared on the variables they follow. Fatal when one has no D3D11 export. */
	void DeclareTextures(const std::shared_ptr<Impl>& a_impl, const D3D11_TEXTURE2D_DESC& a_main)
	{
		using namespace RenderGraph::Frame;
		const auto& settings = globals::features::upscaling.settings;
		// Streamline writes and reads these through their D3D11 images outside the graph, so they are exported and kept.
		const auto upscaledDesc = Sanitized(a_main, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		// An SDK upscaler evaluates into it, and so does guest DLSS-G's evaluate without one. One texel otherwise (the existence rule).
		a_impl->upscaled = DeclareTexture2D(kUpscaledName, DisplaySized(upscaledDesc),
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::UAV },
			ExistsWhen(DependsOn(Vars::UpscaleMethod(), settings.frameGeneration), [](const Inputs& a_in) {
				return SdkMethod(a_in.Get(Vars::UpscaleMethod())) || a_in.Get(globals::features::upscaling.settings.frameGeneration);
			}));
		if (!a_impl->upscaled->D3D11() || !a_impl->upscaled->UAV() || !a_impl->upscaled->Versioned())
			RenderGraph::Fatal(ORGUpscaling::kName, "the upscaled texture has no D3D11 export");
		for (std::uint32_t i = 0; i < kHudlessFormats.size(); ++i) {
			const D3D11_TEXTURE2D_DESC hudlessDesc{ .Width = 1,
				.Height = 1,
				.MipLevels = 1,
				.ArraySize = 1,
				.Format = kHudlessFormats[i],
				.SampleDesc = { .Count = 1 },
				.Usage = D3D11_USAGE_DEFAULT,
				.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
			// FFX derives the UI as the back buffer minus the hudless colour, so the two agree in format and the copy draws through an RTV.
			// The display size while frame generation picks this format, one texel otherwise (the existence rule).
			a_impl->hudless[i] = DeclareTexture2D(kHudlessNames[i], DisplaySized(hudlessDesc),
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV | GraphResources::Export::RTV },
				ExistsWhen(DependsOn(Vars::HdrEnabled(), Vars::PresenterFormat(), settings.frameGeneration),
					[i](const Inputs& a_in) { return HudlessIndexIn(a_in) == i; }));
			if (!a_impl->hudless[i]->D3D11() || !a_impl->hudless[i]->Versioned())
				RenderGraph::Fatal(ORGUpscaling::kName, fmt::format("the hudless texture for format {} has no D3D11 export", static_cast<int>(kHudlessFormats[i])));
		}
	}

	/** @brief What the frame generation inputs' registration depends on: the textures QueueFrameGenInputs last saw. */
	/** @brief Frame generation's inputs pass for the textures QueueFrameGenInputs last saw, made when a revision starts. */
	bool MakeFrameGenInputsClient(const std::shared_ptr<Impl>& a_impl, const InputsKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& impl = *a_impl;
		auto shared = impl.inputsShared;
		auto depth = a_key.depth;
		auto motion = a_key.motion;
		auto hudless = a_key.hudless;
		auto& client = a_client;
		client.id = kFrameGenInputsClientId;
		client.passes.push_back({ .name = kFrameGenInputsClientId,
			.anchor = FrameAnchors::Anchor::FrameGeneration,
			.order = kOrderBase + 1,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [shared, depth, motion, hudless]() -> std::shared_ptr<org::RenderPass> {
				return std::make_shared<FrameGenInputsPass>(shared, depth, motion, hudless);
			},
			.atExecution = true });
		const std::weak_ptr<Impl> weak = a_impl;
		client.active = [weak] {
			const auto impl = weak.lock();
			return impl && impl->inputsArmed;
		};
		client.commit = [weak](const FrameAnchors::Point&) {
			const auto impl = weak.lock();
			if (!impl)
				return;
			impl->inputsShared->runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), impl->inputsWork);
		};
		// The third tap of the UI stage's frame generation step, after the clean scene's and the hudless copy's.
		client.repeats = true;
		return true;
	}

	/** @brief The clients, declared on the variables their registrations read. */
	void DeclareClients(const std::shared_ptr<Impl>& a_impl)
	{
		using namespace RenderGraph::Frame;
		const auto& settings = globals::features::upscaling.settings;
		const std::weak_ptr<Impl> weak = a_impl;
		a_impl->upscaleClient = DeclareClient<UpscaleKey>(
			kClientId, DependsOn(Vars::DisplaySize(), Vars::UpscaleMethod()),
			[weak](const Inputs& a_in) -> std::optional<UpscaleKey> {
				const auto impl = weak.lock();
				if (!impl)
					return std::nullopt;
				// The quality mode is no dependency (decision 6a): the depth upscale is gated, and the frame opens it below native
				// only, as Upscaling::IsUpscalingActive says. So a quality change builds nothing.
				const bool depth = SdkMethod(a_in.Get(Vars::UpscaleMethod()));
				return UpscaleKey{ a_in.Get(Vars::DisplaySize()), depth };
			},
			[weak](const UpscaleKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto impl = weak.lock();
				return impl && MakeUpscaleClient(impl, a_key, a_client);
			});
		a_impl->evaluateClient = DeclareClient<bool>(
			kEvaluateClientId, {},
			[weak](const Inputs&) -> std::optional<bool> {
				const auto impl = weak.lock();
				return impl && HasEvaluateClient(*impl) ? std::optional<bool>(true) : std::nullopt;
			},
			[weak](const bool&, FrameAnchors::ClientDesc& a_client) {
				const auto impl = weak.lock();
				return impl && MakeEvaluateClient(impl, a_client);
			});
		a_impl->hudlessClient = DeclareClient<HudlessKey>(
			kHudlessClientId, DependsOn(Vars::DisplaySize(), Vars::HdrEnabled(), Vars::PresenterFormat(), settings.frameGeneration, a_impl->hudlessSource.Var()),
			[weak](const Inputs& a_in) -> std::optional<HudlessKey> {
				const auto impl = weak.lock();
				if (!impl)
					return std::nullopt;
				const auto index = HudlessIndexIn(a_in);
				// The source comes from the last copy that named one. Before one did, the client waits for it.
				const auto* seen = impl->hudlessSource.Get(a_in);
				if (!index || !seen || !seen->texture)
					return std::nullopt;
				// Imported here, when the revision starts, so a new source texture joins the revision.
				auto source = HudlessSourceOf(seen->texture.get(), seen->view.get());
				if (!source) {
					logger::error("[ORG] Upscaling: the hudless copy's source cannot be imported; the frame has no hudless colour");
					return std::nullopt;
				}
				return HudlessKey{ *index, a_in.Get(Vars::DisplaySize()), std::move(source) };
			},
			[weak](const HudlessKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto impl = weak.lock();
				return impl && MakeHudlessClient(impl, a_key, a_client);
			});
		a_impl->inputsClient = DeclareClient<InputsKey, const InputsKey>(
			kFrameGenInputsClientId, DependsOn(Vars::HdrEnabled(), Vars::PresenterFormat(), settings.frameGeneration, a_impl->inputsDepthCopy.Var()),
			[weak](const Inputs& a_in) -> std::optional<InputsKey> {
				const auto impl = weak.lock();
				if (!impl)
					return std::nullopt;
				const auto* depthCopy = impl->inputsDepthCopy.Get(a_in);
				const auto index = HudlessIndexIn(a_in);
				// Until QueueFrameGenInputs first ran, and while there is no hudless texture, there is no client.
				if (!depthCopy || !index)
					return std::nullopt;
				auto key = InputsKeyFor(*depthCopy, SlotOf(impl->hudless[*index]));
				if (!key.depth || !key.motion || !key.hudless)
					return std::nullopt;
				return key;
			},
			[weak](const InputsKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const InputsKey> {
				const auto impl = weak.lock();
				if (!impl || !MakeFrameGenInputsClient(impl, a_key, a_client))
					return nullptr;
				return std::make_shared<const InputsKey>(a_key);
			});
	}

	/** @brief The hudless texture of the adopted revision, as passes declare it. Empty while that revision has none. */
	FrameAnchors::Texture AdoptedHudless(const Impl& a_impl)
	{
		const auto index = HudlessIndexIn(RenderGraph::Frame::Revision::Adopted());
		return index ? SlotOf(a_impl.hudless[*index]) : FrameAnchors::Texture();
	}

}

ORGUpscaling::ORGUpscaling() = default;
ORGUpscaling::~ORGUpscaling() = default;

void ORGUpscaling::Setup()
{
	impl.reset();
	if (!On())
		return;
	SetupHost();
	try {
		auto next = std::make_shared<Impl>();
		// The D3D11 path's Deferred::LinearSampler.
		next->linearSampler = ShaderBindings::Sampler(ShaderBindings::SamplerPreset::LinearClamp);
		next->latch = std::make_shared<org::LatchBlock>("cs.upscaling.jitter", static_cast<std::uint32_t>(sizeof(Upscaling::JitterCB)));
		next->upscaleGate = RenderGraph::RunGate::Create("cs.upscaling.gate", kUpscaleEntries);
		next->hudlessGate = RenderGraph::RunGate::Create("cs.upscaling.hudless.gate");
		// The post stage holds the upscaler's evaluate, and the UI stage frame generation's inputs.
		next->evaluateShared = std::make_shared<const EvaluateShared>();
		next->inputsShared = std::make_shared<const EvaluateShared>();
		CurrentDepthInputs(next->inputs, next->formats);
		CurrentMain(next->inputs);
		if (!next->inputs.DepthComplete() || !next->inputs.main)
			Fatal("its engine targets could not be imported");
		// Built on the programs queue while the game loads. The first revision start after this waits for them.
		next->depthProgram = RenderGraph::Programs::Queue("Upscaling depth", DepthDesc(next->formats));
		next->underwaterProgram = RenderGraph::Programs::Queue("Upscaling underwater", UnderwaterDesc(next->formats));
		QueueCopyProgram(*next, next->inputs.mainFormat);
		for (const auto format : kHudlessFormats)
			QueueCopyProgram(*next, rhi::helpers::ToRHI(format));
		if (!next->linearSampler || !next->latch)
			Fatal("its sampler or latch could not be made");
		// kMAIN's declared description, for the upscaled texture.
		const auto mainDesc = Targets::Desc(Targets::Colour(RE::RENDER_TARGETS::kMAIN));
		if (!mainDesc.Width || !mainDesc.Height)
			Fatal("kMAIN is not declared");
		{
			const auto shape = [](Targets::TargetRef a_target) {
				const auto desc = Targets::Desc(a_target);
				return fmt::format("{}x{} format {}", desc.Width, desc.Height, static_cast<int>(desc.Format));
			};
			constexpr auto kRefraction = static_cast<std::uint32_t>(RE::RENDER_TARGET::kREFRACTION_NORMALS);
			constexpr auto kUnderwater = static_cast<std::uint32_t>(RE::RENDER_TARGET::kUNDERWATER_MASK);
			logger::info("[ORG] Upscaling: the depth upscale's copies: refraction normals {} into {}, underwater mask {} into {}", shape(Targets::Colour(kRefraction)),
				shape(Targets::ColourCopy(kRefraction)), shape(Targets::Colour(kUnderwater)), shape(Targets::ColourCopy(kUnderwater)));
		}
		DeclareTextures(next, mainDesc);
		DeclareClients(next);
		impl = std::move(next);
		logger::info("[ORG] Upscaling's own passes run on the render graph ({}x{}); the upscaler's evaluate is the post stage's pass", mainDesc.Width,
			mainDesc.Height);
	} catch (const std::exception& e) {
		Fatal(fmt::format("its setup threw ({})", e.what()));
	}
}

bool ORGUpscaling::EvaluatesInStage() const
{
	return IsActive();
}

bool ORGUpscaling::Holds(Target) const
{
	return IsActive();
}

ID3D11Texture2D* ORGUpscaling::Texture(Target a_target, ID3D11Texture2D* a_d3d11) const
{
	if (!Holds(a_target))
		return a_d3d11;
	if (a_target == Target::Upscaled)
		return impl->upscaled->D3D11();
	const auto index = HudlessIndexIn(RenderGraph::Frame::Revision::Adopted());
	return index ? impl->hudless[*index]->D3D11() : nullptr;
}

bool ORGUpscaling::QueueEvaluate(std::string a_label, EvaluateRecord a_record)
{
	if (!IsActive())
		return false;
	// The engine targets are the session's slots, found at setup.
	if (!impl->inputs.main || !impl->inputs.mainDepth || !impl->inputs.motion)
		return false;
	impl->evaluate = std::make_shared<const EvaluateWork>(EvaluateWork{ std::move(a_label), std::move(a_record) });
	impl->queued |= kEvaluate;
	return true;
}

bool ORGUpscaling::RecordsFrameGenInputs() const
{
	return IsActive();
}

bool ORGUpscaling::QueueFrameGenInputs(std::string a_label, EvaluateRecord a_record, bool a_depthCopy)
{
	if (!RecordsFrameGenInputs())
		return false;
	// A switch between kMAIN's depth and its copy is an event: the next revision remakes the client.
	impl->inputsDepthCopy.Observe(a_depthCopy);
	// The adopted revision's hudless texture. Its format follows HDR and the presenter.
	const auto current = InputsKeyFor(a_depthCopy, AdoptedHudless(*impl));
	if (!current.depth || !current.motion || !current.hudless)
		return false;
	// A new registration prepares once the UI stage's schedule holds one made for these textures. Until then the frame is
	// presented without generation.
	if (const auto held = impl->inputsClient.Held(FrameAnchors::Anchor::FrameGeneration); !held || !(*held == current))
		return false;
	impl->inputsWork = std::make_shared<const EvaluateWork>(EvaluateWork{ std::move(a_label), std::move(a_record) });
	impl->inputsArmed = true;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	impl->inputsArmed = false;
	impl->inputsWork.reset();
	return ran;
}

bool ORGUpscaling::QueueCopyToMain()
{
	if (!IsActive() || !impl->upscaled)
		return false;
	if (!impl->inputs.main)
		return false;
	impl->queued |= kCopyToMain;
	return true;
}

bool ORGUpscaling::QueueDepthUpscale(float2 a_jitter, float a_useWideKernel)
{
	if (!IsActive())
		return false;
	// The targets and their formats are the session's, found at setup, so the programs built there serve every frame.
	if (!impl->inputs.DepthComplete())
		return false;
	impl->jitter.jitter = a_jitter;
	impl->jitter.useWideKernel = a_useWideKernel;
	impl->jitter.pad0 = 0.0f;
	impl->queued |= kDepthUpscale;
	return true;
}

void ORGUpscaling::RunUpscale()
{
	if (!impl || !impl->queued)
		return;
	// The copies have no per-run switch. The client's key holds the depth upscale exactly while the adopted method and quality
	// run it, so the registration the frame's build holds matches what was queued.
	(void)FrameAnchors::Run(FrameAnchors::Anchor::Upscale);
	impl->queued = 0;
	impl->evaluate.reset();
}

bool ORGUpscaling::CopyHudless(ID3D11ShaderResourceView* a_source)
{
	if (!IsActive() || !a_source)
		return false;
	winrt::com_ptr<ID3D11Resource> resource;
	a_source->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))) || !texture)
		return false;
	// A new source is an event: the next revision registers the client again, and its key imports it. No import is made here.
	HudlessSource seen;
	seen.texture = std::move(texture);
	seen.view.copy_from(a_source);
	impl->hudlessSource.Observe(seen);
	// The adopted revision has no hudless texture while frame generation is off or the presenter has no format for it.
	if (!AdoptedHudless(*impl))
		return false;
	// A new registration copies once the UI stage's build holds it. Until then the frame has no hudless.
	if (!FrameAnchors::Place(kHudlessClientId, FrameAnchors::Anchor::FrameGeneration).draws)
		return false;
	impl->hudlessArmed = true;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	impl->hudlessArmed = false;
	return ran;
}

