#include "Features/Upscaling/PresentationRevision.h"

#include "Features/Upscaling/FramePresenter.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/Frame/Variables.h"

#include "RenderGraph/Frame/VariablesLane.h"

#include <format>
#include <functional>
#include <mutex>
#include <optional>
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
			// Render thread. The presentation the revision in preparation switches to inside its adoption point (Presenter::Prepare).
			std::optional<PresentationConfig> switchAtAdoption;
		};

		PresentationRequests& Requests()
		{
			static PresentationRequests requests;
			return requests;
		}

		/** @brief The requests' reasons since the presenter was last asked. */
		std::string TakeReasons()
		{
			auto& requests = Requests();
			const std::lock_guard lock(requests.writeMutex);
			auto reasons = std::exchange(requests.reasons, {});
			return reasons.empty() ? std::string("presentation change") : reasons;
		}

		/**
		 * @brief The revision's input for a change of PresentationConfig: the swap chain made for it, or a change the presenter makes
		 * inside the adoption point. Both modes go through the active presenter's contract. Render thread.
		 */
		Revision::InputWait ProducePresentation(const org::services::VariableSnapshot& a_values, std::uint64_t a_revision)
		{
			const auto wanted = Vars::Lane::ValueIn(a_values, Vars::PresentationConfig());
			auto& requests = Requests();
			requests.switchAtAdoption.reset();
			auto& presenter = ActivePresenter();
			PresentationChange change;
			// A superseded revision's replacement serves the revision that replaces it. Serial 0 there is the swap chain made at startup.
			if (wanted == requests.asked) {
				change.serial = requests.askedSerial;
			} else {
				change = presenter.Prepare(wanted, requests.asked, TakeReasons().c_str());
				if (!change.atAdoption && !change.serial)
					stl::report_and_fail(std::format("Community Shaders: the presenter asked no replacement for {}", wanted));
			}
			if (change.atAdoption) {
				// No work before the adoption. The adoption listener asks for it, so the change lands after the old revision's
				// presents and before the new one's.
				requests.switchAtAdoption = wanted;
				logger::info("[FrameGen] frame revision {} switches the presentation at its adoption: {}", a_revision, std::format("{}", wanted));
				return {};
			}
			requests.asked = wanted;
			requests.askedSerial = change.serial;
			Revision::InputWait wait;
			wait.what = std::format("the swap chain for {} (presenter request {})", wanted, change.serial);
			wait.poll = [serial = change.serial](std::string&) {
				return ActivePresenter().AdoptedSerial() >= serial ? Revision::InputState::Ready : Revision::InputState::Pending;
			};
			// The frame presents into the swap chain made for the revision it runs.
			wait.adopt = [wanted, change] { ActivePresenter().Adopt(wanted, change); };
			logger::info("[FrameGen] frame revision {} waits for {}", a_revision, wait.what);
			return wait;
		}

		void EnsureRegistered()
		{
			auto& requests = Requests();
			std::call_once(requests.registered, [&] {
				requests.revisions = Revision::RegisterInputProducer("PresentationConfig", &ProducePresentation);
				// The colour space switch the producer left for the adoption.
				Vars::OnAdopted(Vars::PresentationConfig(),
					std::function<void(const PresentationConfig&, const PresentationConfig&)>([](const PresentationConfig&, const PresentationConfig& a_now) {
						auto& state = Requests();
						if (!state.switchAtAdoption || !(*state.switchAtAdoption == a_now))
							return;
						state.switchAtAdoption.reset();
						(void)TakeReasons();
						auto& presenter = ActivePresenter();
						presenter.Adopt(a_now, PresentationChange{ .atAdoption = true });
						state.asked = a_now;
						state.askedSerial = presenter.RequestedSerial();
					}));
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
			next.hdr = Vars::Written(Vars::PresentationConfig()).hdr;
			next.request = ++requests.requests;
			requests.reasons += std::format("{}{}", requests.reasons.empty() ? "" : ", ", a_reason);
			Vars::Set(Vars::PresentationConfig(), next, a_reason);
		}
		logger::info("[FrameGen] presentation request {} ({}): {}", next.request, a_reason, std::format("{}", next));
		if (!requests.revisions) {
			// Without revisions the presenter replaces the swap chain at once, in the frame's colour space.
			auto config = next;
			config.hdr = Vars::Adopted(Vars::HdrEnabled());
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
		auto next = Vars::Written(Vars::PresentationConfig());
		if (next.hdr == a_hdr)
			return;
		next.hdr = a_hdr;
		Vars::Set(Vars::PresentationConfig(), next, a_writer);
	}

	bool Has(std::uint32_t a_request)
	{
		auto& requests = Requests();
		if (requests.revisions)
			return Vars::Adopted(Vars::PresentationConfig()).request >= a_request;
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
