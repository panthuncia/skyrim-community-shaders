#include "Features/Upscaling/PresentationRevision.h"

#include "Features/Upscaling/DxvkPresenter.h"
#include "Features/Upscaling/FramePresenter.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Variables.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
#	include "RenderGraph/Frame/VariablesLane.h"
#endif

#include <format>
#include <mutex>
#include <string>
#include <utility>

namespace FrameGen::PresentationRevision
{
	namespace
	{
		namespace Vars = RenderGraph::Frame::Vars;
		namespace Revision = RenderGraph::Frame::Revision;

		/** @brief The variable's writers and the presenter requests made for it. */
		struct PresentationRequests
		{
			std::once_flag registered;
			bool revisions = false;  // revisions adopt the variable. Without them a request goes to the presenter at once
			// Writers change one field each, so a write is a read, a change and a write under this lock.
			std::mutex writeMutex;
			std::uint32_t requests = 0;
			std::string reasons;  // the requests' reasons since the presenter was last asked
			// Render thread. What the presenter was last asked for, as the variable has it, and the presenter's serial for it.
			PresentationConfig asked{};
			std::uint64_t askedSerial = 0;
			std::uint32_t askedRequest = 0;  // without revisions
		};

		PresentationRequests& Requests()
		{
			static PresentationRequests requests;
			return requests;
		}

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
		/** @brief Whether a and b are the same but for the colour space. */
		bool OnlyHdrDiffers(PresentationConfig a_a, const PresentationConfig& a_b)
		{
			a_a.hdr = a_b.hdr;
			return a_a == a_b;
		}

		/**
		 * @brief Asks the presenter for a_wanted unless it was asked for it last. Returns the presenter's serial to wait for, or 0
		 * when there is nothing to wait for. Render thread.
		 */
		std::uint64_t AskPresenter(const PresentationConfig& a_wanted)
		{
			auto& requests = Requests();
			const bool native = RenderGraph::Engine::Native::Active();
			// A superseded revision's replacement serves the revision that replaces it.
			if (a_wanted == requests.asked)
				return requests.askedSerial;
			// DXVK's colour space follows the adopted revision through SetColorSpace1, with no replacement asked here.
			if (!native && OnlyHdrDiffers(a_wanted, requests.asked)) {
				requests.asked = a_wanted;
				return 0;
			}
			PresentationConfig config = a_wanted;
			// DXVK's colour space transition names the space its next swap chain has. HDR Display changes it at adoption.
			if (!native)
				config.hdr = Vars::AdoptedPresentationConfig().hdr;
			std::string reasons;
			{
				const std::lock_guard lock(requests.writeMutex);
				reasons = std::exchange(requests.reasons, {});
			}
			auto& presenter = ActivePresenter();
			presenter.RequestConfig(config, reasons.empty() ? "presentation change" : reasons.c_str());
			requests.asked = a_wanted;
			requests.askedSerial = presenter.RequestedSerial();
			// DXVK reports a replacement only through its torn-down callback.
			if (!native && !DxvkPresenter::Get().ReportsReplacements())
				return 0;
			return requests.askedSerial;
		}

		/** @brief The revision's input for a change of PresentationConfig: the swap chain made for it. Render thread. */
		Revision::InputWait ProducePresentation(const org::services::VariableSnapshot& a_values, std::uint64_t a_revision)
		{
			const auto wanted = Vars::Lane::PresentationConfigIn(a_values);
			const std::uint64_t serial = AskPresenter(wanted);
			Revision::InputWait wait;
			wait.what = serial ? std::format("the swap chain for {} (presenter request {})", wanted, serial) : std::format("{} (no replacement)", wanted);
			wait.poll = [serial](std::string&) {
				return !serial || ActivePresenter().AdoptedSerial() >= serial ? Revision::InputState::Ready : Revision::InputState::Pending;
			};
			// The frame presents into the swap chain made for the revision it runs.
			if (RenderGraph::Engine::Native::Active())
				wait.adopt = [] { RenderGraph::Engine::Native::AdoptPresentation(); };
			logger::info("[FrameGen] frame revision {} waits for {}", a_revision, wait.what);
			return wait;
		}
#endif

		void EnsureRegistered()
		{
			auto& requests = Requests();
			std::call_once(requests.registered, [&] {
#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)
				requests.revisions = Revision::RegisterInputProducer("PresentationConfig", &ProducePresentation);
#endif
			});
		}
	}

	std::uint32_t Request(const PresentationConfig& a_config, std::string_view a_reason)
	{
		EnsureRegistered();
		auto& requests = Requests();
		PresentationConfig next;
		{
			const std::lock_guard lock(requests.writeMutex);
			next = a_config;
			next.hdr = Vars::WrittenPresentationConfig().hdr;
			next.request = ++requests.requests;
			requests.reasons += std::format("{}{}", requests.reasons.empty() ? "" : ", ", a_reason);
			Vars::SetPresentationConfig(next, a_reason);
		}
		logger::info("[FrameGen] presentation request {} ({}): {}", next.request, a_reason, std::format("{}", next));
		if (!requests.revisions) {
			// Without revisions the presenter replaces the swap chain at once, in the frame's colour space.
			auto config = next;
			config.hdr = Vars::AdoptedHdrEnabled();
			auto& presenter = ActivePresenter();
			{
				const std::lock_guard lock(requests.writeMutex);
				requests.reasons.clear();
			}
			presenter.RequestConfig(config, std::string(a_reason).c_str());
			requests.asked = next;
			requests.askedSerial = presenter.RequestedSerial();
			requests.askedRequest = next.request;
		}
		return next.request;
	}

	void SetHdr(bool a_hdr, std::string_view a_writer)
	{
		EnsureRegistered();
		auto& requests = Requests();
		const std::lock_guard lock(requests.writeMutex);
		auto next = Vars::WrittenPresentationConfig();
		if (next.hdr == a_hdr)
			return;
		next.hdr = a_hdr;
		Vars::SetPresentationConfig(next, a_writer);
	}

	bool Has(std::uint32_t a_request)
	{
		auto& requests = Requests();
		if (requests.revisions)
			return Vars::AdoptedPresentationConfig().request >= a_request;
		return a_request <= requests.askedRequest && ActivePresenter().AdoptedSerial() >= requests.askedSerial;
	}

	bool Pending()
	{
		std::uint32_t written = 0;
		{
			auto& requests = Requests();
			const std::lock_guard lock(requests.writeMutex);
			written = requests.requests;
		}
		return !Has(written);
	}
}
