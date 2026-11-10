#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenshot.h"

#	include "Globals.h"
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
#	include <cstring>
#	include <mutex>
#	include <utility>

namespace
{
	constexpr const char* kClientId = "cs.screenshot";
	// Capture points a readback may take before it is reported lost (a dropped ticket takes its copy with it).
	constexpr std::uint64_t kPatience = 600;

	/** @brief One capture: armed at a capture point, taken by one preparation of the pass, answered by the readback. */
	struct Request
	{
		std::uint64_t id = 0;
		std::uint64_t armedAt = 0;  // the capture point's number
		ORGScreenshot::OnPixels onPixels;
		std::atomic<bool> submitted = false;  // a submission held its copy
		std::atomic<bool> withdrawn = false;  // its capture point passed without the copy: D3D11 took it
	};

	void OnCopySubmitted(Request& a_request, org::SubmissionContext) { a_request.submitted.store(true, std::memory_order_release); }

	/** @brief What the render thread, the graph host's thread and the readback callback share. */
	struct Shared
	{
		std::mutex mutex;
		std::shared_ptr<Request> armed;                                                     // guarded: the latest armed capture
		std::uint64_t takenId = 0;                                                          // guarded: the latest capture a preparation took
		std::vector<std::pair<std::shared_ptr<Request>, org::ReadbackCaptureResult>> done;  // guarded: finished readbacks
		std::atomic<std::uint64_t> armedId = 0;                                             // armed->id, for the pass's revision
		std::shared_ptr<org::runtime::IReadbackService> service;
		// Render thread: the source the next build's pass copies from (builds run on the render thread).
		std::shared_ptr<org::ExternalTextureResource> source;
	};

	struct CaptureFrame
	{
		org::PreparedResourceReference source;
		org::runtime::ReadbackCapturePlan plan;
	};

	/** @brief The source's first subresource into a readback buffer, at the frames that armed a capture. */
	class CapturePass final : public org::TypedRenderGraphPass<CapturePass, CaptureFrame>
	{
	public:
		// At the build, on the render thread: the source as it is now.
		explicit CapturePass(std::shared_ptr<Shared> a_shared) :
			shared(std::move(a_shared)), source(shared->source) {}

		void Declare(org::PassBuilder& a_builder)
		{
			a_builder.CopySource(source);
			a_builder.PreferQueue(org::QueueKind::Graphics);
		}

		// The capture it was prepared for, while it is the one armed. A ticket prepared before the hook armed another one is
		// prepared again (CloudShadows' copy pass does the same).
		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			const std::uint64_t used = prepared.load(std::memory_order_acquire);
			a_out.push_back(used);
			if (used != shared->armedId.load(std::memory_order_acquire))
				a_out.push_back(mismatch.fetch_add(1, std::memory_order_relaxed) + 1);
		}

		CaptureFrame Prepare(const org::PassPrepareContext& a_preparation)
		{
			std::shared_ptr<Request> request;
			{
				std::scoped_lock lock(shared->mutex);
				const std::uint64_t armedId = shared->armed ? shared->armed->id : 0;
				prepared.store(armedId, std::memory_order_release);
				// Taken once: tickets prepared later for the same armed capture copy nothing.
				if (armedId && armedId != shared->takenId) {
					request = shared->armed;
					shared->takenId = armedId;
				}
			}
			if (!request || !source || !shared->service)
				return {};
			CaptureFrame data{};
			data.source = a_preparation.CaptureResource(source->GetGlobalResourceID());
			// The readback service's thread (any submitting or host thread): Poll hands it on.
			org::ReadbackCaptureCallback onDone = [weak = std::weak_ptr<Shared>(shared), request](org::ReadbackCaptureResult&& a_result) {
				if (request->withdrawn.load(std::memory_order_acquire))
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
		std::atomic<std::uint64_t> prepared = UINT64_MAX;  // nothing prepared yet: differs from every armed id
		mutable std::atomic<std::uint64_t> mismatch = 0;
	};

	std::shared_ptr<org::ExternalTextureResource> Import(const ORGScreenshot::Source& a_source)
	{
		if (a_source.engineTarget >= 0) {
			auto* renderer = globals::game::renderer;
			// The engine's own import when the source is the slot's texture, so the graph orders it with other anchors' uses.
			if (renderer && renderer->GetRuntimeData().renderTargets[static_cast<std::size_t>(a_source.engineTarget)].texture == a_source.texture)
				return FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_source.engineTarget));
		}
		return a_source.importName ? FrameAnchors::D3D11Texture(a_source.importName, a_source.texture, a_source.srv, nullptr) : nullptr;
	}
}

struct ORGScreenshot::Impl
{
	std::shared_ptr<Shared> shared = std::make_shared<Shared>();
	bool registered = false;
	bool failed = false;
	bool importWarned = false;
	bool inactiveWarned = false;
	bool ranThisPoint = false;
	bool warm = false;  // a new source or a new registration: the anchor runs once without a capture, to build its graph
	std::uint64_t nextId = 0;
	std::uint64_t points = 0;                           // capture points seen
	std::shared_ptr<Request> pending;                   // this capture point's capture, while its epoch runs
	std::vector<std::shared_ptr<Request>> outstanding;  // submitted, not read back yet
};

bool ORGScreenshot::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_SCREENSHOT", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

bool ORGScreenshot::CompareRequested()
{
	static const bool requested = [] {
		char value[8]{};
		return Requested() && GetEnvironmentVariableA("CS_ORG_SCREENSHOT_COMPARE", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGScreenshot& ORGScreenshot::Get()
{
	static ORGScreenshot s_screenshot;
	return s_screenshot;
}

ORGScreenshot::~ORGScreenshot() = default;

void ORGScreenshot::ReserveAnchor()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Capture))
		logger::warn("[ORG] Screenshot: CS_ORG_SCREENSHOT=1, but the Capture anchor could not be reserved; captures use D3D11");
}

bool ORGScreenshot::EnsureRegistered()
{
	auto& state = *impl;
	if (state.failed)
		return false;
	if (state.registered)
		return true;
	auto& runtime = RenderGraphRuntime::Get();
	auto* host = runtime.Host();
	// Not up yet (Prime runs from the first Present): D3D11 until it is.
	if (!host || !runtime.IsActive()) {
		if (!std::exchange(state.inactiveWarned, true))
			logger::info("[ORG] Screenshot: the render graph is not active yet; captures use D3D11 until it is");
		return false;
	}
	if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::Capture)) {
		logger::warn("[ORG] Screenshot: the Capture anchor has no epoch; captures use D3D11");
		state.failed = true;
		return false;
	}
	auto shared = state.shared;
	shared->service = host->GetRuntime().Readbacks();
	if (!shared->service) {
		logger::warn("[ORG] Screenshot: the render graph has no readback service; captures use D3D11");
		state.failed = true;
		return false;
	}
	FrameAnchors::ClientDesc client{};
	client.id = kClientId;
	client.passes.push_back({ .name = "cs.screenshot.copy",
		.anchor = FrameAnchors::Anchor::Capture,
		.order = 100,
		.kind = FrameAnchors::PassKind::Copy,
		.make = [shared]() -> std::shared_ptr<org::RenderPass> { return shared->source ? std::make_shared<CapturePass>(shared) : nullptr; } });
	// The pending capture armed at the hook; the pass's revision then sends a ticket prepared before back to be prepared again.
	client.commit = [](const FrameAnchors::Point&) {
		auto& impl = *Get().impl;
		if (!impl.pending)
			return;
		std::scoped_lock lock(impl.shared->mutex);
		impl.shared->armed = impl.pending;
		impl.shared->armedId.store(impl.pending->id, std::memory_order_release);
	};
	client.active = [] { return Get().impl->pending != nullptr || Get().impl->warm; };
	client.onFailed = [] {
		auto& impl = *Get().impl;
		logger::error("[ORG] Screenshot: the render graph failed; captures use D3D11 ({} readbacks lost)", impl.outstanding.size());
		impl.failed = true;
		impl.registered = false;
		impl.outstanding.clear();
	};
	if (!FrameAnchors::Register(std::move(client))) {
		state.failed = true;
		return false;
	}
	state.registered = true;
	state.warm = true;
	logger::info("[ORG] Screenshot: captures are read back through the render graph (Capture anchor)");
	return true;
}

bool ORGScreenshot::SetSource(const Source& a_source)
{
	auto& state = *impl;
	auto source = Import(a_source);
	if (!source)
		return false;
	// A new source: the anchor's graph is built again at its next run, its pass copying from it.
	if (source != state.shared->source) {
		state.shared->source = std::move(source);
		FrameAnchors::Refresh(kClientId);
		state.warm = true;
		logger::info("[ORG] Screenshot: the copy now reads {}", a_source.importName ? a_source.importName : "the source");
	}
	return true;
}

void ORGScreenshot::Prime(const Source& a_source)
{
	if (!Requested() || !a_source.texture)
		return;
	if (!impl)
		impl = std::make_unique<Impl>();
	if (EnsureRegistered())
		(void)SetSource(a_source);
}

bool ORGScreenshot::Capture(const Source& a_source, OnPixels a_onPixels)
{
	if (!Requested() || !a_source.texture || !a_onPixels)
		return false;
	if (!impl)
		impl = std::make_unique<Impl>();
	auto& state = *impl;
	if (!EnsureRegistered())
		return false;
	if (!SetSource(a_source)) {
		if (!std::exchange(state.importWarned, true))
			logger::warn("[ORG] Screenshot: the capture source cannot be imported into the graph; this capture uses D3D11");
		return false;
	}
	auto request = std::make_shared<Request>();
	request->id = ++state.nextId;
	request->armedAt = state.points;
	request->onPixels = std::move(a_onPixels);
	state.pending = request;
	const bool ran = FrameAnchors::Run(FrameAnchors::Anchor::Capture);
	state.pending.reset();
	state.ranThisPoint = true;
	state.warm = false;
	if (!ran || state.failed) {
		logger::warn("[ORG] Screenshot: the Capture anchor's epoch did not run; this capture uses D3D11");
		return false;
	}
	// The submission at this point did not hold the copy (its graph was being built, or its ticket was not prepared for it): no
	// later ticket may copy another frame for it.
	if (!request->submitted.load(std::memory_order_acquire)) {
		request->withdrawn.store(true, std::memory_order_release);
		{
			std::scoped_lock lock(state.shared->mutex);
			if (state.shared->armed == request)
				state.shared->armed.reset();
			state.shared->takenId = (std::max)(state.shared->takenId, request->id);
			state.shared->armedId.store(0, std::memory_order_release);
		}
		logger::warn("[ORG] Screenshot: capture {} was not in the epoch submitted at its capture point; D3D11 copies it", request->id);
		return false;
	}
	logger::info("[ORG] Screenshot: capture {} copied on the render graph", request->id);
	state.outstanding.push_back(std::move(request));
	return true;
}

void ORGScreenshot::Poll()
{
	if (!Requested())
		return;
	if (!impl)
		impl = std::make_unique<Impl>();
	auto& state = *impl;
	// The anchor runs at every capture point (for its other clients, the anchor self-test), once.
	if (!std::exchange(state.ranThisPoint, false))
		FrameAnchors::Run(FrameAnchors::Anchor::Capture);
	state.warm = false;
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
#	include "ORGScreenshot.h"

struct ORGScreenshot::Impl
{};

bool ORGScreenshot::Requested() { return false; }
bool ORGScreenshot::CompareRequested() { return false; }

ORGScreenshot& ORGScreenshot::Get()
{
	static ORGScreenshot s_screenshot;
	return s_screenshot;
}

ORGScreenshot::~ORGScreenshot() = default;
void ORGScreenshot::ReserveAnchor() {}
void ORGScreenshot::Prime(const Source&) {}
bool ORGScreenshot::Capture(const Source&, OnPixels) { return false; }
void ORGScreenshot::Poll() {}

#endif
