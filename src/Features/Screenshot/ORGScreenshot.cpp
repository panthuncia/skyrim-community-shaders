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
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <OpenRenderGraph/Runtime.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/IReadbackService.h>
#	include <Render/Runtime/ReadbackCapturePlan.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>

#	include <atomic>
#	include <mutex>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.screenshot";
	// Capture points a readback may take before it is reported lost (a dropped ticket takes its copy with it).
	constexpr std::uint64_t kPatience = 600;

	/** @brief One capture: armed at a capture point, copied by the pass built for it, answered by the readback. */
	struct Request
	{
		std::uint64_t id = 0;
		std::uint64_t armedAt = 0;  // the capture point's number
		ORGScreenshot::OnPixels onPixels;
		std::atomic<bool> submitted = false;  // a submission held its copy
		std::atomic<bool> closed = false;     // its capture point passed: later preparations copy nothing for it
		std::atomic<bool> withdrawn = false;  // its capture point passed without the copy: D3D11 took it
		std::atomic<bool> delivered = false;  // its first readback arrived: any later one is dropped
	};

	void OnCopySubmitted(Request& a_request, org::SubmissionContext) { a_request.submitted.store(true, std::memory_order_release); }

	/** @brief What the render thread, the graph host's thread and the readback callback share. */
	struct Shared
	{
		std::mutex mutex;
		std::vector<std::pair<std::shared_ptr<Request>, org::ReadbackCaptureResult>> done;  // guarded: finished readbacks
		std::shared_ptr<org::runtime::IReadbackService> service;
		// Render thread: what the next build's pass copies, and for which capture (builds run on the render thread).
		std::shared_ptr<org::ExternalTextureResource> source;
		std::shared_ptr<Request> armed;
	};

	struct CaptureFrame
	{
		org::PreparedResourceReference source;
		org::runtime::ReadbackCapturePlan plan;
	};

	/**
	 * @brief The source's first subresource into a readback buffer, for the one capture it was built for. A readback plan
	 * cannot be part of a kept recording, so the pass has no revision and is prepared for each ticket; AnchorPasses' copy
	 * cannot express it.
	 */
	class CapturePass final : public org::TypedRenderGraphPass<CapturePass, CaptureFrame>
	{
	public:
		// At the build, on the render thread: the source and the request as they are now.
		CapturePass(std::shared_ptr<Shared> a_shared, std::shared_ptr<org::Resource> a_source, std::shared_ptr<Request> a_request) :
			shared(std::move(a_shared)), source(std::move(a_source)), request(std::move(a_request)) {}

		void Declare(org::PassBuilder& a_builder)
		{
			a_builder.CopySource(source);
			a_builder.PreferQueue(org::QueueKind::Graphics);
		}

		CaptureFrame Prepare(const org::PassPrepareContext& a_preparation)
		{
			if (!request || request->closed.load(std::memory_order_acquire) || !shared->service)
				return {};
			CaptureFrame data{};
			data.source = a_preparation.CaptureResource(source->GetGlobalResourceID());
			// The readback service's thread (any submitting or host thread): Poll hands it on.
			org::ReadbackCaptureCallback onDone = [weak = std::weak_ptr<Shared>(shared), request = request](org::ReadbackCaptureResult&& a_result) {
				if (request->withdrawn.load(std::memory_order_acquire) || request->delivered.exchange(true, std::memory_order_acq_rel))
					return;
				if (const auto owner = weak.lock()) {
					std::scoped_lock lock(owner->mutex);
					owner->done.emplace_back(request, std::move(a_result));
				}
			};
			data.plan = org::runtime::PlanReadbackCapture(a_preparation.device, shared->service, *source, a_preparation.ResolveCapturedResource(data.source),
				{}, std::move(onDone), org::QueueKind::Graphics, "cs.screenshot.readback");
			if (!data.plan)
				return {};
			if (data.plan.destinationLease)
				a_preparation.Retain(data.plan.destinationLease);
			a_preparation.Reserve(data.plan.reservation);
			// Whether the submission at the capture point held this copy (ORGScreenshot::Capture checks after the run).
			a_preparation.Reserve(request, &OnCopySubmitted);
			data.plan.destinationLease.reset();
			data.plan.reservation.reset();
			return data;
		}

		static void Record(const CaptureFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.plan)
				return;
			org::runtime::RecordReadbackCapture(a_frame.plan, a_recording.Commands(), a_recording.Resolve(a_frame.source).GetHandle());
		}

	private:
		std::shared_ptr<Shared> shared;
		std::shared_ptr<org::Resource> source;
		std::shared_ptr<Request> request;
	};

	std::shared_ptr<org::ExternalTextureResource> Import(const ORGScreenshot::Source& a_source)
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
	bool ranThisPoint = false;
	std::uint64_t nextId = 0;
	std::uint64_t points = 0;                           // capture points seen
	std::vector<std::shared_ptr<Request>> outstanding;  // submitted, not read back yet
};

ORGScreenshot::~ORGScreenshot() = default;

void ORGScreenshot::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Capture))
		logger::warn("[ORG] Screenshot: CS_ORG_SCREENSHOT=1, but the Capture anchor could not be reserved; captures use D3D11");
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
	auto shared = state->shared;
	shared->service = host->GetRuntime().Readbacks();
	if (!shared->service) {
		logger::warn("[ORG] Screenshot: the render graph has no readback service; captures use D3D11");
		return false;
	}
	RenderGraph::AnchorPasses::Builder builder(FrameAnchors::Anchor::Capture, kClientId, 100);
	// One-shot: in the graph only from a capture's Rearm until one run with it was submitted.
	builder.Add("copy", FrameAnchors::PassKind::Copy, [shared]() -> std::shared_ptr<org::RenderPass> {
		return shared->source ? std::make_shared<CapturePass>(shared, shared->source, shared->armed) : nullptr;
	}, true);
	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes = builder.Take();
	// No per-run work: the anchor runs the copy only while it is armed.
	client.active = [] { return false; };
	client.onFailed = [] {
		auto& impl = *Get().impl;
		logger::error("[ORG] Screenshot: the render graph failed; captures use D3D11 ({} readbacks lost)", impl.outstanding.size());
		impl.registered = false;
		impl.outstanding.clear();
		if (impl.onFailed)
			impl.onFailed();
	};
	state->onFailed = std::move(a_onFailed);
	impl = std::move(state);
	if (!FrameAnchors::Register(std::move(client))) {
		logger::warn("[ORG] Screenshot: the Capture anchor refused the client; captures use D3D11");
		return false;
	}
	impl->registered = true;
	logger::info("[ORG] Screenshot: captures are read back through the render graph (Capture anchor)");
	return true;
}

bool ORGScreenshot::IsActive() const
{
	return impl && impl->registered && RenderGraphRuntime::Get().IsActive();
}

bool ORGScreenshot::SetSource(const Source& a_source)
{
	auto& state = *impl;
	auto source = Import(a_source);
	if (!source)
		return false;
	if (source != state.shared->source) {
		state.shared->source = std::move(source);
		logger::info("[ORG] Screenshot: the copy now reads {}", a_source.importName ? a_source.importName : "the source");
	}
	return true;
}

bool ORGScreenshot::Capture(const Source& a_source, OnPixels a_onPixels)
{
	if (!a_source.texture || !a_onPixels || !IsActive())
		return false;
	auto& state = *impl;
	if (!SetSource(a_source)) {
		if (!std::exchange(state.importWarned, true))
			logger::warn("[ORG] Screenshot: the capture source cannot be imported into the graph; this capture uses D3D11");
		return false;
	}
	auto request = std::make_shared<Request>();
	request->id = ++state.nextId;
	request->armedAt = state.points;
	request->onPixels = std::move(a_onPixels);
	// The copy pass made again for this request and source: armed when it is not pending, and rebuilt when an earlier capture's
	// withdrawn copy still is.
	state.shared->armed = request;
	FrameAnchors::Rearm(kClientId);
	FrameAnchors::Refresh(kClientId);
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::Capture);
	request->closed.store(true, std::memory_order_release);
	state.shared->armed.reset();
	state.ranThisPoint = true;
	if (!ran || !state.registered) {
		request->withdrawn.store(true, std::memory_order_release);
		logger::warn("[ORG] Screenshot: the Capture anchor's epoch did not run; this capture uses D3D11");
		return false;
	}
	// The submission at this point did not hold the copy: no later readback may stand for it.
	if (!request->submitted.load(std::memory_order_acquire)) {
		request->withdrawn.store(true, std::memory_order_release);
		logger::warn("[ORG] Screenshot: capture {} was not in the epoch submitted at its capture point; D3D11 copies it", request->id);
		return false;
	}
	logger::info("[ORG] Screenshot: capture {} copied on the render graph", request->id);
	state.outstanding.push_back(std::move(request));
	return true;
}

void ORGScreenshot::Poll()
{
	if (!impl)
		return;
	auto& state = *impl;
	// The anchor runs at every capture point (for its other clients, the anchor self-test, a copy still pending), once.
	if (!std::exchange(state.ranThisPoint, false))
		FrameAnchors::Run(FrameAnchors::Anchor::Capture);
	++state.points;
	if (state.outstanding.empty())
		return;
	// Other epochs poll the readbacks at their submissions; this one submits only when it captures, so it polls too.
	if (state.shared->service)
		state.shared->service->ProcessReadbackRequests();
	std::vector<std::pair<std::shared_ptr<Request>, org::ReadbackCaptureResult>> done;
	{
		std::scoped_lock lock(state.shared->mutex);
		done.swap(state.shared->done);
	}
	for (auto& [request, result] : done) {
		std::erase(state.outstanding, request);
		if (result.layouts.empty() || result.data.empty()) {
			logger::error("[ORG] Screenshot: capture {} came back without a texture layout", request->id);
			continue;
		}
		const auto& layout = result.layouts.front();
		Pixels pixels{};
		pixels.offset = layout.offset;
		pixels.rowPitch = layout.rowPitch;
		pixels.width = layout.width;
		pixels.height = layout.height;
		pixels.data = std::move(result.data);
		logger::info("[ORG] Screenshot: capture {} read back ({}x{}, {} bytes) after {} capture points", request->id, pixels.width, pixels.height,
			pixels.data.size(), state.points - request->armedAt);
		request->onPixels(std::move(pixels));
	}
	std::erase_if(state.outstanding, [&](const std::shared_ptr<Request>& a_request) {
		if (state.points - a_request->armedAt < kPatience)
			return false;
		logger::error("[ORG] Screenshot: capture {} was not read back after {} frames; it is lost", a_request->id, kPatience);
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

#endif
