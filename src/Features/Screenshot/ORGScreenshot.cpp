#include "ORGScreenshot.h"

#include "RenderGraph/Switches.h"

#include <DirectXTex.h>

#include <cstring>

bool ORGScreenshot::Requested()
{
#if defined(CS_HAS_RENDER_GRAPH)
	static const bool requested = Switches::On("CS_ORG_SCREENSHOT");
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
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Resources/Buffers/Buffer.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <atomic>
#	include <mutex>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.screenshot";
	// Capture points a capture may wait for its copy and its readback before it is reported lost.
	constexpr std::uint64_t kPatience = 600;
	// The readback buffers: a capture takes a free one; more captures in flight wait for one.
	constexpr std::size_t kBuffers = 2;

	/** @brief One capture: armed at a capture point, copied by the pass of the run that took it, read back once that run's work is done. */
	struct Request
	{
		std::uint64_t id = 0;
		std::uint64_t armedAt = 0;  // the capture point's number
		ORGScreenshot::OnPixels onPixels;
		std::size_t buffer = kBuffers;  // the readback buffer it copies into
		std::uint32_t width = 0, height = 0, rowPitch = 0;
		bool taken = false;      // a run committed it (the copy is in that run's recording)
		bool submitted = false;  // that run was submitted: point is set
		org::GraphHost::GpuPoint point;
	};

	/** @brief What the render thread and the passes' recordings share. */
	struct Shared
	{
		// Render thread: what the pass copies, and the readback buffers sized for it (a change rebuilds the client).
		FrameAnchors::Texture source;
		std::uint32_t width = 0, height = 0, texelBytes = 0, rowPitch = 0;
		std::array<std::shared_ptr<org::Buffer>, kBuffers> buffers;
		// The capture each run copies, stored by the commit for the pass recorded at execution (a frame input).
		RenderGraph::RunSlots<Request> runs;
		// Render thread: the capture waiting for a run, and the one the last commit stored (its run's submission sets its point).
		std::shared_ptr<Request> armed;
		std::shared_ptr<Request> committed;
	};

	struct CaptureBindings
	{
		org::ResourceBindingToken source;
	};

	struct CaptureFrame
	{
		std::shared_ptr<Shared> shared;
	};

	/**
	 * @brief The source's first subresource copied into the capture's readback buffer. Recorded at execution from the capture its run's
	 * commit stored; a run without one skips, its barriers folded. The buffers are CS's, declared as copy destinations at the build, so a
	 * kept recording needs nothing prepared again for a capture.
	 */
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
			// The engine's own import when the source is the slot's texture, so the graph orders it with other anchors' uses.
			if (renderer && renderer->GetRuntimeData().renderTargets[static_cast<std::size_t>(a_source.engineTarget)].texture == a_source.texture)
				return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_source.engineTarget));
		}
		// HDR Display's output or back buffer, which its ORG passes write at the HDROutput anchor: their own import, so the graph orders
		// the copy after those writes.
		if (auto hdr = ORGHDRDisplay::Get().ImportOf(a_source.texture))
			return hdr;
		return a_source.importName ? FrameAnchors::D3D11Texture(a_source.importName, a_source.texture, a_source.srv, nullptr) : nullptr;
	}
}

struct ORGScreenshot::Impl
{
	std::shared_ptr<Shared> shared = std::make_shared<Shared>();
	std::function<void()> onFailed;
	bool registered = false;
	bool importWarned = false;
	std::uint64_t nextId = 0;
	std::uint64_t points = 0;                           // capture points seen
	std::vector<std::shared_ptr<Request>> outstanding;  // taken, not read back yet
	std::uint64_t built = 0;                            // the registration the client's passes were made for
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
	 * @brief Registers the capture client for the shared source and buffers: one copy pass, recorded at execution. In native mode with the
	 * UI stages the Capture anchor is their step (after the overlay); otherwise it runs its own epoch.
	 */
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
		// The anchor's step has work only while a capture waits for a run.
		client.active = [weak = std::weak_ptr<Shared>(a_shared)] {
			const auto owner = weak.lock();
			return owner && owner->armed != nullptr;
		};
		client.commit = [weak = std::weak_ptr<Shared>(a_shared)](const FrameAnchors::Point&) {
			const auto owner = weak.lock();
			if (!owner || !owner->armed)
				return;
			auto request = std::exchange(owner->armed, nullptr);
			request->taken = true;
			owner->runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), request);
			owner->committed = std::move(request);
		};
		client.executed = [weak = std::weak_ptr<Shared>(a_shared)](const FrameAnchors::Point&) {
			const auto owner = weak.lock();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!owner || !owner->committed || !host)
				return;
			owner->committed->point = host->SubmittedPoint();
			owner->committed->submitted = true;
			owner->committed.reset();
		};
		client.onFailed = [] {
			logger::error("[ORG] Screenshot: the render graph failed with the capture's pass in it; captures use D3D11");
			ORGScreenshot::Get().Fail();
		};
		return FrameAnchors::Register(std::move(client));
	}
}

void ORGScreenshot::Fail()
{
	if (!impl)
		return;
	auto& state = *impl;
	logger::error("[ORG] Screenshot: {} readbacks lost", state.outstanding.size());
	state.registered = false;
	state.outstanding.clear();
	state.shared->armed.reset();
	if (state.onFailed)
		state.onFailed();
}

bool ORGScreenshot::Setup(std::function<void()> a_onFailed)
{
	if (!Requested() || impl)
		return impl && impl->registered;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	if (!host || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Capture)) {
		logger::warn("[ORG] Screenshot: CS_ORG_SCREENSHOT=1, but the render graph or its Capture anchor is not available; captures use D3D11");
		return false;
	}
	auto state = std::make_unique<Impl>();
	state->onFailed = std::move(a_onFailed);
	impl = std::move(state);
	// No source yet: the client has no pass until the first capture names one (SetSource), and its build switches in before the copy.
	if (!RegisterClient(impl->shared)) {
		logger::warn("[ORG] Screenshot: the Capture anchor refused the client; captures use D3D11");
		return false;
	}
	impl->registered = true;
	logger::info("[ORG] Screenshot: captures are read back through the render graph ({})",
		FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) ? "a pass of the UI and menu UI stages" : "the Capture anchor");
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
	auto source = Import(a_source);
	if (!source)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	a_source.texture->GetDesc(&desc);
	const auto bytes = rhi::FormatByteSize(source.Format());
	if (!bytes)
		return false;
	const auto rowPitch = (desc.Width * bytes + 255u) & ~255u;
	if (source == shared.source && shared.width == desc.Width && shared.height == desc.Height && shared.texelBytes == bytes)
		return true;
	shared.source = std::move(source);
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
	// The pass copies the new source into the new buffers from the client's next build; a capture waits for it.
	if (!RegisterClient(state.shared))
		return false;
	logger::info("[ORG] Screenshot: the copy now reads {} ({}x{}, {} bytes a texel); its build switches in before the next capture copies",
		a_source.importName ? a_source.importName : "the source", desc.Width, desc.Height, bytes);
	return true;
}

namespace
{
	/** @brief A readback buffer no capture in flight holds, or kBuffers. */
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

	/** @brief Taps the Capture anchor for the armed capture when a tap now commits (the UI stage open, or the anchor's own epoch). */
	void TryTake(Shared& a_shared)
	{
		if (!a_shared.armed)
			return;
		if (FrameAnchors::HeldByStage(FrameAnchors::Anchor::Capture) && !FrameAnchors::CanTap(kClientId))
			return;
		(void)FrameAnchors::Run(FrameAnchors::Anchor::Capture);
	}
}

bool ORGScreenshot::Capture(const Source& a_source, OnPixels a_onPixels)
{
	if (!a_source.texture || !a_onPixels || !IsActive())
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
	// Taken now when the stage's schedule has the pass; otherwise at a later capture point (its build switching in, or the stage's next run).
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
	++state.points;
	TryTake(shared);
	if (state.outstanding.empty())
		return;
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

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: screenshots always copy with D3D11.
struct ORGScreenshot::Impl
{};

ORGScreenshot::~ORGScreenshot() = default;

void ORGScreenshot::Reserve() {}
bool ORGScreenshot::Setup(std::function<void()>) { return false; }
bool ORGScreenshot::IsActive() const { return false; }
bool ORGScreenshot::SetSource(const Source&) { return false; }
bool ORGScreenshot::Capture(const Source&, OnPixels) { return false; }
void ORGScreenshot::Poll() {}
void ORGScreenshot::Fail() {}

#endif
