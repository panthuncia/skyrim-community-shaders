#include "ORGScreenshot.h"
#include "RenderGraph/Engine/Native/Native.h"

#include "RenderGraph/Switches.h"

#include <DirectXTex.h>

#include <cstring>

bool ORGScreenshot::Requested()
{
#if defined(CS_HAS_RENDER_GRAPH)
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
#else
	return false;
#endif
}

ORGScreenshot& ORGScreenshot::Get()
{
	static ORGScreenshot s_screenshot;
	return s_screenshot;
}

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

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/HDRDisplay/ORGHDRDisplay.h"
#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameStaging.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/Buffers/Buffer.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <atomic>
#	include <mutex>
#	include <string>
#	include <utility>

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
		// Render thread. The source's name and resource when the graph owns it (the clean scene), for the client's graph.
		std::pair<std::string, std::shared_ptr<org::Resource>> own;
	};

	/** @brief The clean scene's copy, shared with its client's callbacks. Render thread. */
	struct CleanCopy
	{
		bool armed = false;            // SnapshotCleanScene is running the anchor for it
		std::uint32_t copied = ~0u;    // the frame whose run's commit opened the copy
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
		if (a_source.engineTarget >= 0) {
			auto* renderer = globals::game::renderer;
			// Use the engine's own import so the graph orders it with other anchors' uses.
			if (renderer && renderer->GetRuntimeData().renderTargets[static_cast<std::size_t>(a_source.engineTarget)].texture == a_source.texture)
				return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_source.engineTarget));
		}
		// HDR Display's ORG passes write these, so use their import to order the copy after those writes.
		if (auto hdr = ORGHDRDisplay::Get().ImportOf(a_source.texture))
			return hdr;
		return a_source.importName ? FrameAnchors::D3D11Texture(a_source.importName, a_source.texture, a_source.srv, nullptr) : nullptr;
	}
}

struct ORGScreenshot::Impl
{
	std::shared_ptr<Shared> shared = std::make_shared<Shared>();
	bool registered = false;
	bool importWarned = false;
	std::uint64_t nextId = 0;
	std::uint64_t points = 0;
	std::vector<std::shared_ptr<Request>> outstanding;  // taken, not read back yet
	std::uint64_t built = 0;                            // the registration the client's passes were made for

	// The pre-UI clean scene: kFRAMEBUFFER's copy at the FrameGeneration anchor.
	std::unique_ptr<GraphResources::Texture2D> clean;
	FrameAnchors::Texture cleanSource;
	std::shared_ptr<RenderGraph::RunGate> cleanGate;
	std::shared_ptr<CleanCopy> cleanCopy = std::make_shared<CleanCopy>();
	bool cleanRegistered = false;
	bool cleanWarned = false;
};

ORGScreenshot::~ORGScreenshot() = default;

void ORGScreenshot::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Capture))
		logger::warn("[ORG] Screenshot: CS_ORG_SCREENSHOT=1, but the Capture anchor could not be reserved; captures use D3D11");
}

namespace
{
	/**
	* @brief Whether the run copies with the current registration. Without the stages the anchor runs whatever graph it built,
	* so a capture taken before the new build switches in would read back an unwritten, black buffer.
	*/
	bool CopyBuilt()
	{
		return FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) || FrameAnchors::Scheduled(kClientId, FrameAnchors::Anchor::Capture);
	}

	/** @brief In native mode with the UI stage the Capture anchor is its step after the overlay. Otherwise it runs its own epoch. */
	bool RegisterClient(const std::shared_ptr<Shared>& a_shared)
	{
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Capture, kClientId, 100);
		builder.Add("copy", FrameAnchors::PassKind::Copy, [a_shared, source = a_shared->source, buffers = a_shared->buffers]() -> std::shared_ptr<org::RenderPass> {
			return source ? std::make_shared<CapturePass>(a_shared, source, buffers) : nullptr;
		});
		FrameAnchors::ClientDesc client{};
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
			if (own.second)
				a_graph.RegisterResource(org::ResourceIdentifier(own.first), own.second);
		};
		client.commit = [weak = std::weak_ptr<Shared>(a_shared)](const FrameAnchors::Point&) {
			const auto owner = weak.lock();
			if (!owner || !owner->armed || !CopyBuilt())
				return;
			auto request = std::exchange(owner->armed, nullptr);
			request->taken = true;
			request->takenAt = owner->points;
			owner->runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), request);
		};
		// No executed callback, since in a stage it would run before the commit submits. The next capture point takes the GPU point instead.
		return FrameAnchors::Register(std::move(client));
	}
}

bool ORGScreenshot::Setup()
{
	if (!Requested() || impl)
		return impl && impl->registered;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Capture)) {
		logger::warn("[ORG] Screenshot: CS_ORG_SCREENSHOT=1, but the render graph or its Capture anchor is not available; captures use D3D11");
		return false;
	}
	impl = std::make_unique<Impl>();
	// No pass until the first capture names a source.
	if (!RegisterClient(impl->shared)) {
		logger::warn("[ORG] Screenshot: the Capture anchor refused the client; captures use D3D11");
		return false;
	}
	impl->registered = true;
	logger::info("[ORG] Screenshot: captures are read back through the render graph ({})",
		FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) ? "a pass of the UI stage" : "the Capture anchor");
	return true;
}

bool ORGScreenshot::IsActive() const
{
	return impl && impl->registered && RenderGraphRuntime::Get().IsActive();
}

bool ORGScreenshot::SetSource(const Source& a_source)
{
	auto& state = *impl;
	auto& shared = *state.shared;
	const bool clean = a_source.cleanScene && state.clean;
	if (a_source.cleanScene && !clean)
		return false;
	FrameAnchors::Texture source = clean ? FrameAnchors::Texture(state.clean->Graph()) : Import(a_source);
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	if (clean)
		desc = state.clean->Desc();
	else
		a_source.texture->GetDesc(&desc);
	const auto bytes = rhi::FormatByteSize(source.Format());
	if (!bytes)
		return false;
	const auto rowPitch = (desc.Width * bytes + 255u) & ~255u;
	if (source == shared.source && shared.width == desc.Width && shared.height == desc.Height && shared.texelBytes == bytes)
		return true;
	shared.source = std::move(source);
	shared.own = clean ? std::pair{ state.clean->Name(), state.clean->Graph() } : std::pair<std::string, std::shared_ptr<org::Resource>>{};
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
	// A capture waits for the client's next build.
	if (!RegisterClient(state.shared))
		return false;
	logger::info("[ORG] Screenshot: the copy now reads {} ({}x{}, {} bytes a texel); its build switches in before the next capture copies",
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
		if (FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) ? !FrameAnchors::CanTap(kClientId) : !CopyBuilt())
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
	/** @brief The clean scene's client: one gated copy, opened only by a commit SnapshotCleanScene armed. */
	bool RegisterCleanClient(const std::shared_ptr<RenderGraph::RunGate>& a_gate, const GraphResources::Texture2D& a_clean, const FrameAnchors::Texture& a_source,
		const std::shared_ptr<CleanCopy>& a_copy)
	{
		RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::FrameGeneration, kCleanClientId, 100, nullptr, a_gate);
		builder.Add(RenderGraph::AnchorPasses::Copy{
			.name = "copy", .source = a_source, .target = a_clean.Graph(), .width = a_clean.Desc().Width, .height = a_clean.Desc().Height, .gate = 0u });
		if (builder.Failed()) {
			logger::error("[ORG] Screenshot: the clean scene's copy could not be built: {}", builder.Failure());
			return false;
		}
		FrameAnchors::ClientDesc client{};
		client.id = kCleanClientId;
		client.passes = builder.Take();
		client.gates = { a_gate };
		client.registerResources = [name = a_clean.Name(), resource = a_clean.Graph()](org::RenderGraph& a_graph) {
			a_graph.RegisterResource(org::ResourceIdentifier(name), resource);
		};
		client.active = [weak = std::weak_ptr<CleanCopy>(a_copy)] {
			const auto copy = weak.lock();
			return copy && copy->armed;
		};
		client.commit = [weak = std::weak_ptr<CleanCopy>(a_copy), gate = a_gate](const FrameAnchors::Point&) {
			const auto copy = weak.lock();
			if (!copy || !copy->armed)
				return;
			gate->OpenDraw(0);
			copy->copied = globals::state->frameCount;
		};
		// The UI stage's frame generation step also takes the hudless copy's tap in the same frame.
		client.repeats = true;
		return FrameAnchors::Register(std::move(client));
	}
}

bool ORGScreenshot::SnapshotCleanScene()
{
	if (!IsActive() || !globals::game::renderer)
		return false;
	auto& state = *impl;
	// Only its description is read. With the engine's targets on the graph the slot has no texture, so take it from a view.
	const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
	winrt::com_ptr<ID3D11Texture2D> viewed;
	ID3D11Texture2D* framebuffer = target.texture;
	if (ID3D11View* view = target.SRV ? static_cast<ID3D11View*>(target.SRV) : static_cast<ID3D11View*>(target.RTV); !framebuffer && view) {
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		if (resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(viewed.put()))))
			framebuffer = viewed.get();
	}
	auto source = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kFRAMEBUFFER));
	if (!framebuffer || !source) {
		if (!std::exchange(state.cleanWarned, true))
			logger::warn("[ORG] Screenshot: the clean scene cannot be copied ({}); captures read kFRAMEBUFFER at the capture point",
				!framebuffer ? "kFRAMEBUFFER has no texture" : "kFRAMEBUFFER cannot be imported");
		return false;
	}
	D3D11_TEXTURE2D_DESC desc{};
	framebuffer->GetDesc(&desc);
	const bool same = state.clean && state.clean->Desc().Width == desc.Width && state.clean->Desc().Height == desc.Height &&
	                  state.clean->Desc().Format == desc.Format && state.cleanSource == source;
	if (!same) {
		// A new size or format makes a new texture and client. Its build switches in at a later frame, so this frame does not copy.
		D3D11_TEXTURE2D_DESC cleanDesc{};
		cleanDesc.Width = desc.Width;
		cleanDesc.Height = desc.Height;
		cleanDesc.MipLevels = 1;
		cleanDesc.ArraySize = 1;
		cleanDesc.Format = desc.Format;
		cleanDesc.SampleDesc = { 1, 0 };
		cleanDesc.Usage = D3D11_USAGE_DEFAULT;
		cleanDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		auto clean = std::make_unique<GraphResources::Texture2D>(cleanDesc, "Screenshot::CleanScene", GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		if (!state.cleanGate)
			state.cleanGate = RenderGraph::RunGate::Create("cs.screenshot.clean.gate");
		state.clean.reset();
		state.cleanRegistered = false;
		if (*clean && state.cleanGate) {
			state.clean = std::move(clean);
			state.cleanSource = std::move(source);
			state.cleanRegistered = RegisterCleanClient(state.cleanGate, *state.clean, state.cleanSource, state.cleanCopy);
		}
		if (!state.cleanRegistered && !std::exchange(state.cleanWarned, true))
			logger::error("[ORG] Screenshot: the clean scene's copy could not be set up ({}x{}, format {}); captures read kFRAMEBUFFER at the capture point",
				desc.Width, desc.Height, static_cast<int>(desc.Format));
		return false;
	}
	if (!state.cleanRegistered)
		return false;
	state.cleanCopy->armed = true;
	const bool ran = FrameAnchors::Scheduled(kCleanClientId, FrameAnchors::Anchor::FrameGeneration) && FrameAnchors::Run(FrameAnchors::Anchor::FrameGeneration);
	state.cleanCopy->armed = false;
	return ran && HasFreshCleanScene();
}

bool ORGScreenshot::HasFreshCleanScene() const
{
	return impl && impl->clean && impl->cleanCopy->copied == globals::state->frameCount;
}

const D3D11_TEXTURE2D_DESC* ORGScreenshot::CleanSceneDesc() const
{
	return impl && impl->clean ? &impl->clean->Desc() : nullptr;
}

#else  // !CS_HAS_RENDER_GRAPH

// Without OpenRenderGraph screenshots always copy with D3D11.
struct ORGScreenshot::Impl
{};

ORGScreenshot::~ORGScreenshot() = default;

void ORGScreenshot::Reserve() {}
bool ORGScreenshot::Setup() { return false; }
bool ORGScreenshot::IsActive() const { return false; }
bool ORGScreenshot::SetSource(const Source&) { return false; }
bool ORGScreenshot::Capture(const Source&, OnPixels) { return false; }
void ORGScreenshot::Poll() {}
bool ORGScreenshot::SnapshotCleanScene() { return false; }
bool ORGScreenshot::HasFreshCleanScene() const { return false; }
const D3D11_TEXTURE2D_DESC* ORGScreenshot::CleanSceneDesc() const { return nullptr; }

#endif
