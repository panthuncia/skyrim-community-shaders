#include "ORGScreenshot.h"

#include <winrt/base.h>

#include <DirectXTex.h>

#include <cstring>

bool ORGScreenshot::Crop(const Pixels& a_pixels, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format,
	DirectX::ScratchImage& a_image)
{
	const std::size_t bytesPerPixel = DirectX::BitsPerPixel(a_format) / 8;
	if (bytesPerPixel == 0 || a_width == 0 || a_height == 0 || a_x + a_width > a_pixels.width || a_y + a_height > a_pixels.height)
		return false;
	const std::size_t rowBytes = static_cast<std::size_t>(a_width) * bytesPerPixel;
	// The last row's end must be inside the readback.
	const std::size_t lastRowEnd = a_pixels.offset + static_cast<std::size_t>(a_y + a_height - 1) * a_pixels.rowPitch +
	                               static_cast<std::size_t>(a_x) * bytesPerPixel + rowBytes;
	if (lastRowEnd > a_pixels.data.size())
		return false;
	if (FAILED(a_image.Initialize2D(a_format, a_width, a_height, 1, 1)))
		return false;
	const auto* destImage = a_image.GetImage(0, 0, 0);
	if (!destImage || destImage->rowPitch < rowBytes)
		return false;
	const auto* source = reinterpret_cast<const std::uint8_t*>(a_pixels.data.data()) + a_pixels.offset + static_cast<std::size_t>(a_x) * bytesPerPixel;
	for (std::size_t row = 0; row < a_height; ++row)
		std::memcpy(destImage->pixels + row * destImage->rowPitch, source + (a_y + row) * a_pixels.rowPitch, rowBytes);
	return true;
}

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/HDRDisplay/ORGHDRDisplay.h"
#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/Engine/UiTargets.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "State.h"

#include <OpenRenderGraph/GraphHost.h>
#include <OpenRenderGraph/Runtime.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/Buffers/Buffer.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace
{
	constexpr const char* kClientId = "cs.screenshot";
	// The clean scene's copy, at the UI stage's FrameGeneration step (RecipeTable.h).
	constexpr const char* kCleanClientId = "cs.screenshot.clean";
	// How many capture points a capture may wait for its copy and readback before it is reported lost.
	constexpr std::uint64_t kPatience = 600;
	// Extra captures in flight wait for a free buffer.
	constexpr std::size_t kBuffers = 2;

	/** @brief Armed at a capture point, copied by the run that took it, read back once that run's work is done. */
	struct Request
	{
		std::uint64_t id = 0;
		std::uint64_t armedAt = 0;  // the capture point's number
		ORGScreenshot::OnPixels onPixels;
		std::size_t buffer = kBuffers;
		std::uint32_t width = 0, height = 0, rowPitch = 0;
		bool taken = false;          // a run committed it, so the copy is in that run's recording
		std::uint64_t takenAt = 0;
		bool submitted = false;      // that run was submitted, so point is set
		org::GraphHost::GpuPoint point;
	};

	/** @brief What the capture copy's registration depends on: the source SetSource last set. */
	struct CaptureKey
	{
		std::uint64_t source = 0;
		bool operator==(const CaptureKey&) const = default;
	};

	struct Shared
	{
		// Render thread. A change rebuilds the client.
		FrameAnchors::Texture source;
		std::uint32_t width = 0, height = 0, texelBytes = 0, rowPitch = 0;
		std::array<std::shared_ptr<org::Buffer>, kBuffers> buffers;
		// Stored by the commit for the pass recorded at execution.
		RenderGraph::RunSlots<Request> runs;
		// Render thread.
		std::shared_ptr<Request> armed;
		// Render thread, read by the commit.
		std::uint64_t points = 0;
		// Render thread. The source's twin when the graph owns it (the clean scene), for the client's graph.
		const GraphResources::Texture2D* own = nullptr;
		// Render thread. SetSource raises wanted for a new source. Each registration keeps the key it was made for.
		std::uint64_t wanted = 0;
		// SetSource observes wanted. The client's key reads it, so a new source is an event.
		RenderGraph::Frame::Observed<std::uint64_t> wantedSeen{ "Screenshot.Source" };
		// The capture copy, declared on the source SetSource observes (an interim until the source is a variable).
		RenderGraph::Frame::DeclaredClientOf<const CaptureKey> client;
	};

	namespace Vars = RenderGraph::Frame::Vars;

	/** @brief What the clean copy's registration depends on. */
	struct CleanKey
	{
		Vars::Extent2D display{};      // the copy's size
		FrameAnchors::Texture source;  // kFRAMEBUFFER's slot (Engine::UiTargets), stable for the session
		bool operator==(const CleanKey&) const = default;
	};

	/** @brief The clean scene: its texture, its client and the copy's state, shared with the client's callbacks. Render thread. */
	struct CleanScene
	{
		RenderGraph::Frame::DeclaredTexture2D texture;
		// Each registration keeps its key. The copy reads the adopted one's (Adopted()), cleared with the client.
		RenderGraph::Frame::DeclaredClientOf<const CleanKey> client;
		std::shared_ptr<RenderGraph::RunGate> gate;
		D3D11_TEXTURE2D_DESC desc{};  // the texture's description at the copy's size, for the capture
		bool armed = false;           // SnapshotCleanScene is running the anchor for it
		std::uint32_t copied = ~0u;   // the frame whose run's commit opened the copy
	};

	struct CaptureBindings
	{
		org::ResourceBindingToken source;
	};

	struct CaptureFrame
	{
		std::shared_ptr<Shared> shared;
	};

	/** @brief Recorded at execution from the capture the commit stored. A run without one skips. The buffers are declared at build, so nothing is re-prepared per capture. */
	class CapturePass final : public org::TypedRenderGraphPass<CapturePass, CaptureFrame, CaptureBindings>
	{
	public:
		CapturePass(std::shared_ptr<Shared> a_shared, FrameAnchors::Texture a_source, std::array<std::shared_ptr<org::Buffer>, kBuffers> a_buffers) :
			shared(std::move(a_shared)), source(std::move(a_source)), buffers(std::move(a_buffers)) {}

		CaptureBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			CaptureBindings bindings{};
			bindings.source = a_builder.CopySource(source);
			for (const auto& buffer : buffers)
				if (buffer)
					a_builder.CopyDestination(buffer);
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		CaptureFrame Prepare(const CaptureBindings&, const org::PassPrepareContext&) const { return { shared }; }

		static bool SkipsAtExecution(const CaptureBindings&, const CaptureFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return !a_frame.shared || !a_frame.shared->runs.Find(a_recording.Run());
		}

		static void Record(const CaptureBindings& a_bindings, const CaptureFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.shared)
				return;
			const auto request = a_frame.shared->runs.Take(a_recording.Run());
			if (!request || request->buffer >= kBuffers || !a_frame.shared->buffers[request->buffer])
				return;
			rhi::BufferTextureCopyFootprint copy{};
			copy.texture = a_recording.Resolve(a_bindings.source).GetHandle();
			copy.buffer = a_frame.shared->buffers[request->buffer]->GetAPIResource().GetHandle();
			copy.mip = 0;
			copy.arraySlice = 0;
			copy.footprint = { 0, request->rowPitch, request->width, request->height, 1 };
			a_recording.Commands().CopyTextureToBuffer(copy);
		}

	private:
		std::shared_ptr<Shared> shared;
		FrameAnchors::Texture source;
		std::array<std::shared_ptr<org::Buffer>, kBuffers> buffers;
	};

	FrameAnchors::Texture Import(const ORGScreenshot::Source& a_source)
	{
		// The swap chain's back buffer is written only by the present copy, after every capture point. Once the target set holds
		// kFRAMEBUFFER, its slot has the frame (HDR's composite included).
		if (auto* field = RenderGraph::Engine::UiTargets::BackBufferTexture(); field && a_source.texture && !a_source.cleanScene) {
			winrt::com_ptr<ID3D11Texture2D> swapChainBuffer;
			if (globals::d3d::swapChain)
				globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(swapChainBuffer.put()));
			if (a_source.texture == field || (swapChainBuffer && a_source.texture == swapChainBuffer.get()))
				return RenderGraph::Engine::UiTargets::Current().backBuffer;
		}
		if (a_source.engineTarget >= 0) {
			auto* renderer = globals::game::renderer;
			// Use the engine's own import so the graph orders it with other anchors' uses.
			if (renderer && renderer->GetRuntimeData().renderTargets[static_cast<std::size_t>(a_source.engineTarget)].texture == a_source.texture)
				return FrameAnchors::EngineTexture(FrameAnchors::Colour(static_cast<std::uint32_t>(a_source.engineTarget)));
		}
		// HDR Display's ORG passes write these, so use their import to order the copy after those writes.
		if (auto hdr = ORGHDRDisplay::Get().ImportOf(a_source.texture))
			return hdr;
		return a_source.importName ? FrameAnchors::ImportTexture(fmt::format("cs.d3d11.{}", a_source.importName), { .texture = a_source.texture, .srv = a_source.srv }) : nullptr;
	}
}

struct ORGScreenshot::Impl
{
	std::shared_ptr<Shared> shared = std::make_shared<Shared>();
	bool importWarned = false;
	std::uint64_t nextId = 0;
	std::uint64_t points = 0;
	std::vector<std::shared_ptr<Request>> outstanding;  // taken, not read back yet
	std::uint64_t built = 0;                            // the registration the client's passes were made for

	// The pre-UI clean scene: kFRAMEBUFFER's copy at the FrameGeneration anchor. Null when kFRAMEBUFFER had no texture at setup.
	std::shared_ptr<CleanScene> clean;
	bool cleanWarned = false;
};

ORGScreenshot::ORGScreenshot() = default;
ORGScreenshot::~ORGScreenshot() = default;

namespace
{
	/**
	* @brief Whether the run copies with the current registration. Without the stages the anchor runs whatever graph it built,
	* so a capture taken before the new build switches in would read back an unwritten, black buffer. A source set since the
	* last registration waits for the registration the next revision makes.
	*/
	bool CopyBuilt(const Shared& a_shared)
	{
		const auto held = a_shared.client.Held(FrameAnchors::Anchor::Capture);
		return held && held->source == a_shared.wanted;
	}

	/**
	* @brief The capture copy's registration for the current source, made when a revision starts. In native mode with the UI
	* stage the Capture anchor is its step after the overlay. Otherwise it runs its own epoch.
	*/
	bool MakeClient(const std::shared_ptr<Shared>& a_shared, FrameAnchors::ClientDesc& a_client)
	{
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Capture, kClientId, 100);
		builder.Add("copy", FrameAnchors::PassKind::Copy, [a_shared, source = a_shared->source, buffers = a_shared->buffers]() -> std::shared_ptr<org::RenderPass> {
			return source ? std::make_shared<CapturePass>(a_shared, source, buffers) : nullptr;
		});
		auto& client = a_client;
		client.id = kClientId;
		client.passes = builder.Take();
		for (auto& pass : client.passes)
			pass.atExecution = true;
		// Work only while a capture waits for a run.
		client.active = [weak = std::weak_ptr<Shared>(a_shared)] {
			const auto owner = weak.lock();
			return owner && owner->armed != nullptr;
		};
		// A graph-owned source is registered with the graph by name.
		client.registerResources = [own = a_shared->own](org::RenderGraph& a_graph) {
			if (own)
				own->Register(a_graph);
		};
		client.commit = [weak = std::weak_ptr<Shared>(a_shared)](const FrameAnchors::Point&) {
			const auto owner = weak.lock();
			if (!owner || !owner->armed || !CopyBuilt(*owner))
				return;
			auto request = std::exchange(owner->armed, nullptr);
			request->taken = true;
			request->takenAt = owner->points;
			owner->runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), request);
		};
		// No executed callback, since in a stage it would run before the commit submits. The next capture point takes the GPU point instead.
		return true;
	}

	/** @brief Setup. The capture copy follows the source SetSource sets. */
	RenderGraph::Frame::DeclaredClientOf<const CaptureKey> DeclareCaptureClient(const std::shared_ptr<Shared>& a_shared)
	{
		const std::weak_ptr<Shared> weak = a_shared;
		return RenderGraph::Frame::DeclareClient<CaptureKey, const CaptureKey>(
			kClientId, RenderGraph::Frame::DependsOn(a_shared->wantedSeen.Var()),
			[weak](const RenderGraph::Frame::Inputs& a_in) -> std::optional<CaptureKey> {
				const auto owner = weak.lock();
				if (!owner)
					return std::nullopt;
				// Null before the first SetSource: no source yet.
				const auto* wanted = owner->wantedSeen.Get(a_in);
				return CaptureKey{ wanted ? *wanted : 0 };
			},
			[weak](const CaptureKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const CaptureKey> {
				const auto owner = weak.lock();
				if (!owner || !MakeClient(owner, a_client))
					return nullptr;
				return std::make_shared<const CaptureKey>(a_key);
			});
	}

	/** @brief Defined with the clean scene's copy, below. */
	std::shared_ptr<CleanScene> DeclareCleanScene();
}

void ORGScreenshot::Setup()
{
	if (!BeginSetup())
		return;
	auto state = std::make_unique<Impl>();
	// No pass until the first capture names a source. The client registers at the first revision.
	state->shared->client = DeclareCaptureClient(state->shared);
	// Without it captures still work, but read kFRAMEBUFFER at the capture point.
	state->clean = DeclareCleanScene();
	impl = std::move(state);
	logger::info("[ORG] Screenshot: captures are read back through the render graph (a pass of the UI stage)");
}

bool ORGScreenshot::SetSource(const Source& a_source)
{
	auto& state = *impl;
	auto& shared = *state.shared;
	const bool clean = a_source.cleanScene && state.clean;
	if (a_source.cleanScene && !clean)
		return false;
	FrameAnchors::Texture source = clean ? RenderGraph::AnchorPasses::TextureOf(*state.clean->texture) : Import(a_source);
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	// The clean scene's size is the copy's, so the adopted version's. Its twin's Desc() may be a version still in flight.
	if (clean)
		desc = state.clean->desc;
	else
		a_source.texture->GetDesc(&desc);
	const auto bytes = rhi::FormatByteSize(source.Format());
	if (!bytes)
		return false;
	const auto rowPitch = (desc.Width * bytes + 255u) & ~255u;
	if (source == shared.source && shared.width == desc.Width && shared.height == desc.Height && shared.texelBytes == bytes)
		return true;
	shared.source = std::move(source);
	shared.own = clean ? state.clean->texture.Get() : nullptr;
	shared.width = desc.Width;
	shared.height = desc.Height;
	shared.texelBytes = bytes;
	shared.rowPitch = rowPitch;
	for (std::size_t i = 0; i < kBuffers; ++i) {
		shared.buffers[i] = org::Buffer::CreateShared(rhi::HeapType::Readback, std::uint64_t(rowPitch) * desc.Height);
		if (!shared.buffers[i])
			return false;
		shared.buffers[i]->SetName(fmt::format("cs.screenshot.readback{}", i));
	}
	// A capture waits for the registration the next revision makes, and for its build.
	shared.wantedSeen.Observe(++shared.wanted);
	logger::info("[ORG] Screenshot: the copy now reads {} ({}x{}, {} bytes a texel); its registration and build switch in before the next capture copies",
		clean ? "the clean scene" : (a_source.importName ? a_source.importName : "the source"), desc.Width, desc.Height, bytes);
	return true;
}

namespace
{
	/** @brief kBuffers when none is free. */
	std::size_t FreeBuffer(const std::vector<std::shared_ptr<Request>>& a_outstanding, const std::shared_ptr<Request>& a_armed)
	{
		for (std::size_t i = 0; i < kBuffers; ++i) {
			const bool used = (a_armed && a_armed->buffer == i) ||
			                  std::ranges::any_of(a_outstanding, [&](const std::shared_ptr<Request>& a_request) { return a_request->buffer == i; });
			if (!used)
				return i;
		}
		return kBuffers;
	}

	/** @brief Taps when a tap now commits, with the UI stage open or the anchor's own epoch. */
	void TryTake(Shared& a_shared)
	{
		if (!a_shared.armed)
			return;
		if (!CopyBuilt(a_shared))
			return;
		// In the UI stage, a tap commits only while its window is open with a build that holds the copy.
		if (!FrameAnchors::Place(kClientId).built)
			return;
		(void)FrameAnchors::Run(FrameAnchors::Anchor::Capture);
	}
}

bool ORGScreenshot::Capture(const Source& a_source, OnPixels a_onPixels)
{
	if (!(a_source.cleanScene ? HasFreshCleanScene() : a_source.texture != nullptr) || !a_onPixels || !IsActive())
		return false;
	auto& state = *impl;
	auto& shared = *state.shared;
	if (!SetSource(a_source)) {
		if (!std::exchange(state.importWarned, true))
			logger::warn("[ORG] Screenshot: the capture source cannot be imported into the graph; this capture uses D3D11");
		return false;
	}
	if (shared.armed) {
		logger::warn("[ORG] Screenshot: a capture is still waiting for its copy; this capture uses D3D11");
		return false;
	}
	const auto buffer = FreeBuffer(state.outstanding, nullptr);
	if (buffer >= kBuffers) {
		logger::warn("[ORG] Screenshot: every readback buffer holds a capture in flight; this capture uses D3D11");
		return false;
	}
	auto request = std::make_shared<Request>();
	request->id = ++state.nextId;
	request->armedAt = state.points;
	request->onPixels = std::move(a_onPixels);
	request->buffer = buffer;
	request->width = shared.width;
	request->height = shared.height;
	request->rowPitch = shared.rowPitch;
	shared.armed = request;
	state.outstanding.push_back(request);
	// Otherwise taken at a later capture point.
	TryTake(shared);
	logger::info("[ORG] Screenshot: capture {} {}", request->id, request->taken ? "copied on the render graph" : "waits for a run that holds its copy");
	return true;
}

void ORGScreenshot::Poll()
{
	if (!impl)
		return;
	auto& state = *impl;
	auto& shared = *state.shared;
	// Captures committed from here on get this number.
	const struct Next
	{
		Impl& state;
		~Next() { state.shared->points = ++state.points; }
	} next{ state };
	TryTake(shared);
	if (state.outstanding.empty())
		return;
	// Its run was submitted since, so the point submitted by now covers its copy.
	if (auto* host = RenderGraphRuntime::Get().Host())
		for (const auto& request : state.outstanding)
			if (request->taken && !request->submitted && request->takenAt < state.points) {
				request->point = host->SubmittedPoint();
				request->submitted = true;
			}
	std::vector<std::shared_ptr<Request>> done;
	for (const auto& request : state.outstanding)
		if (request->submitted && request->point.Reached())
			done.push_back(request);
	for (const auto& request : done) {
		std::erase(state.outstanding, request);
		const auto& buffer = shared.buffers[request->buffer];
		void* mapped = nullptr;
		if (buffer)
			buffer->GetAPIResource().Map(&mapped);
		if (!mapped) {
			logger::error("[ORG] Screenshot: capture {}'s readback could not be mapped; it is lost", request->id);
			continue;
		}
		Pixels pixels{};
		pixels.offset = 0;
		pixels.rowPitch = request->rowPitch;
		pixels.width = request->width;
		pixels.height = request->height;
		const auto size = std::size_t(request->rowPitch) * request->height;
		pixels.data.resize(size);
		std::memcpy(pixels.data.data(), mapped, size);
		buffer->GetAPIResource().Unmap(0, 0);
		logger::info("[ORG] Screenshot: capture {} read back ({}x{}, {} bytes) after {} capture points", request->id, pixels.width, pixels.height, size,
			state.points - request->armedAt);
		request->onPixels(std::move(pixels));
	}
	std::erase_if(state.outstanding, [&](const std::shared_ptr<Request>& a_request) {
		if (state.points - a_request->armedAt < kPatience)
			return false;
		logger::error("[ORG] Screenshot: capture {} was not read back after {} capture points; it is lost", a_request->id, kPatience);
		if (shared.armed == a_request)
			shared.armed.reset();
		return true;
	});
}

namespace
{
	/** @brief The clean copy's registration for a_key: one gated copy, opened only by a commit SnapshotCleanScene armed. */
	bool MakeCleanClient(const std::shared_ptr<CleanScene>& a_clean, const CleanKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		auto& clean = *a_clean;
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::FrameGeneration, kCleanClientId, 100, nullptr, clean.gate);
		builder.Add(RenderGraph::AnchorPasses::Copy{ .name = "copy",
			.source = a_key.source,
			.target = RenderGraph::AnchorPasses::TextureOf(*clean.texture),
			.width = a_key.display.width,
			.height = a_key.display.height,
			.gate = 0u });
		if (builder.Failed()) {
			logger::error("[ORG] Screenshot: the clean scene's copy could not be built: {}", builder.Failure());
			return false;
		}
		a_client.passes = builder.Take();
		a_client.gates = { clean.gate };
		a_client.registerResources = [twin = clean.texture.Get()](org::RenderGraph& a_graph) { twin->Register(a_graph); };
		const std::weak_ptr<CleanScene> weak = a_clean;
		a_client.active = [weak] {
			const auto current = weak.lock();
			return current && current->armed;
		};
		a_client.commit = [weak](const FrameAnchors::Point&) {
			const auto current = weak.lock();
			if (!current || !current->armed)
				return;
			current->gate->OpenDraw(0);
			current->copied = globals::state->frameCount;
		};
		// The UI stage's frame generation step also takes the hudless copy's tap in the same frame.
		a_client.repeats = true;
		// Every registration shares the gate and the commit. A registration for a new key taps as the one the frame's build holds
		// (by id). SnapshotCleanScene copies only while that one fits the source.
		logger::info("[ORG] Screenshot: the clean scene's copy is made for the next revision ({}x{})", a_key.display.width, a_key.display.height);
		return true;
	}

	/** @brief kFRAMEBUFFER's texture, for its description: the field (the target set's handle), else the texture behind a view. */
	winrt::com_ptr<ID3D11Texture2D> FramebufferTexture()
	{
		winrt::com_ptr<ID3D11Texture2D> framebuffer;
		if (!globals::game::renderer)
			return framebuffer;
		const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		if (target.texture) {
			framebuffer.copy_from(target.texture);
			return framebuffer;
		}
		if (ID3D11View* view = target.SRV ? static_cast<ID3D11View*>(target.SRV) : static_cast<ID3D11View*>(target.RTV)) {
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			if (resource)
				framebuffer = resource.try_as<ID3D11Texture2D>();
		}
		return framebuffer;
	}

	/** @brief kFRAMEBUFFER's description. */
	bool FramebufferDesc(D3D11_TEXTURE2D_DESC& a_desc)
	{
		const auto framebuffer = FramebufferTexture();
		if (!framebuffer)
			return false;
		framebuffer->GetDesc(&a_desc);
		return true;
	}

	/**
	* @brief Declares the clean scene's texture and client, once, at setup. Null, logged, when kFRAMEBUFFER has no texture.
	* Fatal when the texture or client cannot be declared.
	* kFRAMEBUFFER is the back buffer here. Its format is fixed when the swap chain is made (R10G10B10A2 with HDR Display
	* loaded), so only its size follows a variable. The clean scene is read only in SDR, so with HDR on the texture is one
	* texel and there is no client.
	*/
	std::shared_ptr<CleanScene> DeclareCleanScene()
	{
		using namespace RenderGraph::Frame;
		D3D11_TEXTURE2D_DESC framebuffer{};
		if (!FramebufferDesc(framebuffer)) {
			logger::warn("[ORG] Screenshot: kFRAMEBUFFER has no texture at setup, so there is no clean scene on the graph; captures read kFRAMEBUFFER at the capture point");
			return nullptr;
		}
		auto clean = std::make_shared<CleanScene>();
		clean->desc = D3D11_TEXTURE2D_DESC{ .Width = framebuffer.Width,
			.Height = framebuffer.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = framebuffer.Format,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE };
		clean->gate = RenderGraph::RunGate::Create("cs.screenshot.clean.gate");
		// The display size in SDR, one texel with HDR on (the existence rule).
		clean->texture = DeclareTexture2D("Screenshot::CleanScene", DisplaySized(clean->desc), GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent },
			ExistsWhen(DependsOn(Vars::HdrEnabled()), [](const Inputs& a_in) { return !a_in.Get(Vars::HdrEnabled()); }));
		if (!clean->texture->Versioned())
			RenderGraph::Fatal(ORGScreenshot::kName,
				fmt::format("the clean scene's texture could not be declared ({}x{}, format {})", framebuffer.Width, framebuffer.Height,
					static_cast<int>(framebuffer.Format)));
		const std::weak_ptr<CleanScene> weak = clean;
		clean->client = DeclareClient<CleanKey, const CleanKey>(
			kCleanClientId, DependsOn(Vars::DisplaySize(), Vars::HdrEnabled()),
			[weak](const Inputs& a_in) -> std::optional<CleanKey> {
				const auto current = weak.lock();
				if (!current)
					return std::nullopt;
				// No client, so none is adopted from here on.
				if (a_in.Get(Vars::HdrEnabled()))
					return std::nullopt;
				// kFRAMEBUFFER's slot never changes, so a new display size is only a new version of it.
				auto source = RenderGraph::Engine::UiTargets::Current(a_in).backBuffer;
				if (!source)
					RenderGraph::Fatal(ORGScreenshot::kName, "kFRAMEBUFFER has no graph slot for the clean scene's copy");
				return CleanKey{ a_in.Get(Vars::DisplaySize()), std::move(source) };
			},
			[weak](const CleanKey& a_key, FrameAnchors::ClientDesc& a_client) -> std::shared_ptr<const CleanKey> {
				const auto current = weak.lock();
				if (!current || !MakeCleanClient(current, a_key, a_client))
					return nullptr;
				return std::make_shared<const CleanKey>(a_key);
			},
			[](const CleanKey& a_key, const CleanKey&) {
				logger::info("[ORG] Screenshot: the clean scene's copy switches in with the revision adopted at frame {} ({}x{})", globals::state->frameCount,
					a_key.display.width, a_key.display.height);
			});
		return clean;
	}
}

bool ORGScreenshot::SnapshotCleanScene()
{
	if (!IsActive() || !impl->clean)
		return false;
	auto& state = *impl;
	auto& clean = *state.clean;
	const auto framebuffer = FramebufferTexture();
	if (!framebuffer) {
		if (!std::exchange(state.cleanWarned, true))
			logger::warn("[ORG] Screenshot: kFRAMEBUFFER has no texture, so this capture has no clean scene; captures read kFRAMEBUFFER at the capture point");
		return false;
	}
	// The frame's build holds the adopted registration. It copies only while that one was made for kFRAMEBUFFER's adopted size.
	// Otherwise the revision with the new size is still on its way.
	D3D11_TEXTURE2D_DESC framebufferDesc{};
	framebuffer->GetDesc(&framebufferDesc);
	const auto held = clean.client.Adopted();
	if (!held || held->display.width != framebufferDesc.Width || held->display.height != framebufferDesc.Height)
		return false;
	clean.desc.Width = held->display.width;
	clean.desc.Height = held->display.height;
	clean.armed = true;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	clean.armed = false;
	return ran && HasFreshCleanScene();
}

bool ORGScreenshot::HasFreshCleanScene() const
{
	return impl && impl->clean && impl->clean->copied == globals::state->frameCount;
}

const D3D11_TEXTURE2D_DESC* ORGScreenshot::CleanSceneDesc() const
{
	return impl && impl->clean ? &impl->clean->desc : nullptr;
}

