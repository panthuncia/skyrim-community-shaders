#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

struct ID3D11Texture2D;
namespace RE
{
	class BSRenderPass;
}

namespace DCLF::Diagnostics
{
	/**
	 * @brief Hook points where the main path announces what happened, for parity checks, probes and dumps to observe.
	 *
	 * The main path reaches diagnostics only through Observe(), Policy:: questions and these hook points. It never reads a parity switch.
	 * Observers subscribe at Install, before DCLF's threads start, and the registry is read-only after that, so Notify takes no lock.
	 * Notify costs one flag test while nothing observes the event. An observer runs on its Notify's thread and must not change what
	 * the main path does. Such a change is a Policy question instead.
	 * Switchable observers run only while SetSwitchableArmed has them on. The render thread flips them at frame start.
	 */
	namespace Detail
	{
		template <class Event>
		inline std::atomic<bool> armed{ false };
		template <class Event>
		inline std::vector<void (*)(const Event&)> observers;
		template <class Event>
		inline std::atomic<bool> switchableArmed{ false };
		template <class Event>
		inline std::vector<void (*)(const Event&)> switchableObservers;
		// Per event type with switchable observers. Sets its switchable flag and its armed flag.
		inline std::vector<void (*)(bool)> switchableArms;
	}

	/** @brief Whether anything observes Event. Test this first when the payload is costly to build. */
	template <class Event>
	inline bool Armed()
	{
		return Detail::armed<Event>.load(std::memory_order_relaxed);
	}

	/** @brief Sends a_event to every observer of its type. */
	template <class Event>
	inline void Notify(const Event& a_event)
	{
		if (Detail::armed<Event>.load(std::memory_order_relaxed)) [[unlikely]] {
			for (auto* observer : Detail::observers<Event>)
				observer(a_event);
			if (Detail::switchableArmed<Event>.load(std::memory_order_relaxed))
				for (auto* observer : Detail::switchableObservers<Event>)
					observer(a_event);
		}
	}

	/** @brief A hook point that carries only which point it is. Observers read what they need from the modules. */
	template <auto Point>
	struct On
	{
	};

	/** @brief Announces an On event, for example Notify<FramePoint::Present>(). */
	template <auto Point>
	inline void Notify()
	{
		Notify(On<Point>{});
	}

	/** @brief Install only. a_observer runs at every hook point of Event. */
	template <class Event>
	void Subscribe(void (*a_observer)(const Event&))
	{
		Detail::observers<Event>.push_back(a_observer);
		Detail::armed<Event>.store(true, std::memory_order_relaxed);
	}

	/** @brief Install only. a_observer runs at every hook point of Event while the switchable observers are armed. */
	template <class Event>
	void SubscribeSwitchable(void (*a_observer)(const Event&))
	{
		if (Detail::switchableObservers<Event>.empty())
			Detail::switchableArms.push_back([](bool a_on) {
				Detail::switchableArmed<Event>.store(a_on, std::memory_order_relaxed);
				Detail::armed<Event>.store(a_on || !Detail::observers<Event>.empty(), std::memory_order_relaxed);
			});
		Detail::switchableObservers<Event>.push_back(a_observer);
	}

	/** @brief Install only. a_observer runs at every On<Point> hook point while the switchable observers are armed. */
	template <auto Point>
	void SubscribeSwitchable(void (*a_observer)(const On<Point>&))
	{
		SubscribeSwitchable<On<Point>>(a_observer);
	}

	/** @brief Arms or disarms every switchable observer. Render thread, frame start. */
	inline void SetSwitchableArmed(bool a_on)
	{
		for (auto* arm : Detail::switchableArms)
			arm(a_on);
	}

	/** @brief Install only. a_observer runs at every On<Point> hook point. */
	template <auto Point>
	void Subscribe(void (*a_observer)(const On<Point>&))
	{
		Subscribe<On<Point>>(a_observer);
	}

	/** @brief Registers the observers this run's switches ask for and installs their engine hooks. PostPostLoad, before DCLF's threads start. */
	void Install();

	// Frame events, render thread, in frame order.

	/** @brief Frame points that carry no data, in frame order. */
	enum class FramePoint : std::uint8_t
	{
		LookupsInstalled,   // lookups are the installed publication's, before the scene lane's inputs are posted
		ClaimsInstalled,    // claims installed, before the culls
		ShadowViewsBegin,   // BeforeShadowMaps, with the frame's shadow views already listed
		RegistrationsDone,  // EarlyPrepass, registrations drained
		PrepassEnd,         // Prepass with DCLF running, after the frame constants are refreshed
		Present,            // Present, every frame, first thing
	};

	/** @brief Main::Draw's early hook, every frame. */
	struct FrameBegin
	{
		bool running;
	};

	/** @brief Prepass, every frame, before DCLF's prepass work. */
	struct PrepassBegin
	{
		bool running;
	};

	/** @brief A native Lighting draw after its SetupGeometry, while DCLF runs. */
	struct NativeLightingDraw
	{
		RE::BSRenderPass* pass;
		std::uint32_t renderFlags;
		std::uint32_t frame;
		bool deferred;          // inside the deferred pass
		bool mainPassCaptured;  // the colour epoch's bindings were captured this frame
	};

	/** @brief A native pass about to be drawn while DCLF runs. A set member's pass in the main views is a leak. */
	struct NativePassOffered
	{
		const RE::BSRenderPass* pass;
		std::uint32_t technique;
		bool inDepthPass;
	};

	/** @brief A native pass the batch renderers drew. */
	struct NativePassDrawn
	{
		const RE::BSRenderPass* pass;
		bool inDepthPass;
	};

	/** @brief The engine's reference render of an occlusion map, before it is cleared for DCLF's. Map 0 is Skylighting, 1 the precipitation mask. */
	struct OcclusionReferenceRendered
	{
		std::uint32_t map;
	};

	/** @brief The occlusion maps DCLF's shadow epoch drew, a bit per map. Sent every frame it runs. */
	struct OcclusionMapsDrawn
	{
		std::uint32_t drawn;
	};

	/** @brief A named frame point where the bound targets may be inspected. a_extra is another texture to keep. */
	struct TargetPoint
	{
		const char* label;
		ID3D11Texture2D* extra = nullptr;
	};

}
