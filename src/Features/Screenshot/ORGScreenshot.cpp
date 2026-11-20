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
#	include "RenderGraph/Frame/Declare.h"
#	include "RenderGraph/Frame/Variables.h"
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
#	include <optional>
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
		// Render thread. The source's twin when the graph owns it (the clean scene), for the client's graph.
		const GraphResources::Texture2D* own = nullptr;
		// Render thread. SetSource raises wanted for a new source; the registration made for it sets made.
		std::uint64_t wanted = 0, made = 0;
	};

	/** @brief What the capture copy's registration depends on: the source SetSource last set. */
	struct CaptureKey
	{
		std::uint64_t source = 0;
		bool operator==(const CaptureKey&) const = default;
	};

	namespace Vars = RenderGraph::Frame::Vars;

	/** @brief What the clean copy's registration depends on. */
	struct CleanKey
	{
		Vars::Extent2D display{};                // the copy's size
		FrameAnchors::Texture source;            // kFRAMEBUFFER's import
		const ID3D11Texture2D* texture = nullptr;  // the D3D11 texture the key imported
		bool operator==(const CleanKey&) const = default;
	};

	/** @brief The clean scene: its texture, its client and the copy's state, shared with the client's callbacks. Render thread. */
	struct CleanScene
	{
		RenderGraph::Frame::DeclaredTexture2D texture;
		RenderGraph::Frame::DeclaredClient client;
		std::shared_ptr<RenderGraph::RunGate> gate;
		Vars::Extent2D setupDisplay{};  // kFRAMEBUFFER's size at setup, before the first DisplaySize event
		D3D11_TEXTURE2D_DESC desc{};    // the texture's description at the copy's size, for the capture
		// kFRAMEBUFFER's texture as the frame last saw it. Not a variable yet, so a change calls client.Invalidate().
		// The key imports it, so the frame makes no import. Only compared, so it holds no reference to the back buffer.
		const ID3D11Texture2D* observed = nullptr;
		// The key of the registration the adopted revision holds. Set by the client's adopted call, cleared with the client.
		std::optional<CleanKey> held;
		bool armed = false;          // SnapshotCleanScene is running the anchor for it
		std::uint32_t copied = ~0u;  // the frame whose run's commit opened the copy
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
	// The capture copy, declared on the source SetSource observes (an interim until the source is a variable).
	RenderGraph::Frame::DeclaredClient client;
	std::uint64_t nextId = 0;
	std::uint64_t points = 0;
	std::vector<std::shared_ptr<Request>> outstanding;  // taken, not read back yet
	std::uint64_t built = 0;                            // the registration the client's passes were made for

	// The pre-UI clean scene: kFRAMEBUFFER's copy at the FrameGeneration anchor. Null when it could not be declared.
	std::shared_ptr<CleanScene> clean;
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
	* so a capture taken before the new build switches in would read back an unwritten, black buffer. A source set since the
	* last registration waits for the registration the next revision makes.
	*/
	bool CopyBuilt(const Shared& a_shared)
	{
		return a_shared.made == a_shared.wanted &&
		       (FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) || FrameAnchors::Scheduled(kClientId, FrameAnchors::Anchor::Capture));
	}

	/**
	* @brief The capture copy's registration for the current source, made when a revision starts. In native mode with the UI
	* stage the Capture anchor is its step after the overlay. Otherwise it runs its own epoch.
	*/
	bool MakeClient(const std::shared_ptr<Shared>& a_shared, const CaptureKey& a_key, FrameAnchors::ClientDesc& a_client)
	{
		a_shared->made = a_key.source;
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
	RenderGraph::Frame::DeclaredClient DeclareCaptureClient(const std::shared_ptr<Shared>& a_shared)
	{
		const std::weak_ptr<Shared> weak = a_shared;
		return RenderGraph::Frame::DeclareClient<CaptureKey>(
			kClientId, {},
			[weak](const RenderGraph::Frame::Inputs&) -> std::optional<CaptureKey> {
				const auto owner = weak.lock();
				if (!owner)
					return std::nullopt;
				return CaptureKey{ owner->wanted };
			},
			[weak](const CaptureKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto owner = weak.lock();
				return owner && MakeClient(owner, a_key, a_client);
			});
	}

	/** @brief Defined with the clean scene's copy, below. */
	std::shared_ptr<CleanScene> DeclareCleanScene();
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
	// No pass until the first capture names a source. The client registers at the first revision.
	impl->client = DeclareCaptureClient(impl->shared);
	if (!impl->client) {
		logger::warn("[ORG] Screenshot: the capture copy's client could not be declared; captures use D3D11");
		return false;
	}
	impl->registered = true;
	// Without it captures still work, but read kFRAMEBUFFER at the capture point.
	impl->clean = DeclareCleanScene();
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
	++shared.wanted;
	state.client.Invalidate();
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
		if (a_shared.made != a_shared.wanted)
			return;
		if (FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) ? !FrameAnchors::CanTap(kClientId) : !CopyBuilt(a_shared))
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
	/** @brief The display size, or a_setup before the first DisplaySize event. */
	Vars::Extent2D DisplayOr(Vars::Extent2D a_display, Vars::Extent2D a_setup)
	{
		return a_display.width && a_display.height ? a_display : a_setup;
	}

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
		// Every registration shares the gate and the commit. A registration for a new key taps as the one the frame's build holds.
		// SnapshotCleanScene copies only while that one fits the source.
		a_client.matchesById = true;
		logger::info("[ORG] Screenshot: the clean scene's copy is made for the next revision ({}x{})", a_key.display.width, a_key.display.height);
		return true;
	}

	/** @brief kFRAMEBUFFER's texture. With the engine's targets on the graph the slot has no texture, so take it from a view. */
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
	* @brief Declares the clean scene's texture and client, once, at setup. Null, logged, when it cannot be.
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
		clean->setupDisplay = { framebuffer.Width, framebuffer.Height };
		clean->desc = D3D11_TEXTURE2D_DESC{ .Width = framebuffer.Width,
			.Height = framebuffer.Height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = framebuffer.Format,
			.SampleDesc = { .Count = 1 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE };
		clean->gate = RenderGraph::RunGate::Create("cs.screenshot.clean.gate");
		if (!clean->gate) {
			logger::error("[ORG] Screenshot: the clean scene's run gate could not be made");
			return nullptr;
		}
		// The first SnapshotCleanScene takes it again if this one is not the frame's.
		clean->observed = FramebufferTexture().get();
		const auto setup = clean->setupDisplay;
		clean->texture = DeclareTexture2D(
			"Screenshot::CleanScene", DependsOn(Vars::DisplaySize(), Vars::HdrEnabled()),
			[desc = clean->desc, setup](const Inputs& a_in) {
				auto out = desc;
				const bool used = !a_in.Get(Vars::HdrEnabled());
				const auto display = DisplayOr(a_in.Get(Vars::DisplaySize()), setup);
				out.Width = used ? display.width : 1;
				out.Height = used ? display.height : 1;
				return out;
			},
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
		if (!clean->texture || !clean->texture->Versioned()) {
			logger::error("[ORG] Screenshot: the clean scene's texture could not be declared ({}x{}, format {})", framebuffer.Width, framebuffer.Height,
				static_cast<int>(framebuffer.Format));
			return nullptr;
		}
		const std::weak_ptr<CleanScene> weak = clean;
		clean->client = DeclareClient<CleanKey>(
			kCleanClientId, DependsOn(Vars::DisplaySize(), Vars::HdrEnabled()),
			[weak](const Inputs& a_in) -> std::optional<CleanKey> {
				const auto current = weak.lock();
				if (!current)
					return std::nullopt;
				// No client, so none is held from here on.
				if (a_in.Get(Vars::HdrEnabled()) || !current->observed) {
					current->held.reset();
					return std::nullopt;
				}
				// Imported here, when the revision starts, so a new back buffer joins the revision.
				const auto texture = FramebufferTexture();
				auto source = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kFRAMEBUFFER));
				if (!texture || !source) {
					current->held.reset();
					return std::nullopt;
				}
				return CleanKey{ DisplayOr(a_in.Get(Vars::DisplaySize()), current->setupDisplay), std::move(source), texture.get() };
			},
			[weak](const CleanKey& a_key, FrameAnchors::ClientDesc& a_client) {
				const auto current = weak.lock();
				return current && MakeCleanClient(current, a_key, a_client);
			},
			[weak](const CleanKey& a_key) {
				const auto current = weak.lock();
				if (!current)
					return;
				current->held = a_key;
				logger::info("[ORG] Screenshot: the clean scene's copy switches in with the revision adopted at frame {} ({}x{})", globals::state->frameCount,
					a_key.display.width, a_key.display.height);
			});
		if (!clean->client) {
			logger::error("[ORG] Screenshot: the clean scene's client could not be declared");
			return nullptr;
		}
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
	// The back buffer is not a variable yet. A new one registers the copy again when the next revision starts, whose key imports it.
	if (framebuffer.get() != clean.observed) {
		clean.observed = framebuffer.get();
		clean.client.Invalidate();
		return false;
	}
	// The frame's build holds the adopted registration. It copies only while that one was made for this texture at its size.
	// Otherwise the revision with the new size is still on its way.
	D3D11_TEXTURE2D_DESC framebufferDesc{};
	framebuffer->GetDesc(&framebufferDesc);
	const auto& held = clean.held;
	if (!clean.client.Registered() || !held || held->texture != framebuffer.get() || held->display.width != framebufferDesc.Width ||
		held->display.height != framebufferDesc.Height)
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
