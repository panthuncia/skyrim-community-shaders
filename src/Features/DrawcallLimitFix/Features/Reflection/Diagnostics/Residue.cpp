// The reflection residue, under CS_DCLF_SET_PARITY or CS_DCLF_PERSISTENT_PARITY: what the engine registered into the reflection
// faces under the LOD land and objects roots that is not a reflection member. The render thread reads the geometries at the
// capture's drain, and the next scene pass classifies them for the report.

#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"

#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneView.h"

#include <ankerl/unordered_dense.h>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DCLF::Features::Reflection
{
	namespace
	{
		using V = Diagnostics::SceneView;

		inline constexpr std::size_t kResidueUntracked = static_cast<std::size_t>(Ineligible::Count), kResidueUnbound = kResidueUntracked + 1,
									 kResidueNotReflection = kResidueUntracked + 2, kResidueReflection = kResidueUntracked + 3, kResidueKinds = kResidueUntracked + 4;
		struct ResidueClasses
		{
			std::array<std::uint64_t, kResidueKinds> counts{};
			std::array<std::string, kResidueKinds> first;
			struct Seen
			{
				std::uint32_t frame = ~0u, frames = 0;
			};
			ankerl::unordered_dense::map<const void*, Seen> seen;
			std::uint64_t persistent = 0;
		};

		/**
		 * @brief The frame's residue geometries and their names, read at the drain on the render thread.
		 * The scene pass classifies them a frame later, past a Present that may have released them, so it dereferences none untracked.
		 */
		struct ResidueDrain
		{
			struct Residue
			{
				const RE::BSGeometry* geometry = nullptr;
				std::string names;  // "'name' under 'parent' (RTTI)"
			};
			std::vector<Residue> residue;
		};

		struct ResidueState
		{
			std::uint32_t updates = 0;
			// A post no scene pass took is given to the next one, so every residue geometry is classified once.
			std::atomic<std::shared_ptr<const ResidueDrain>> posted;
			std::shared_ptr<const ResidueDrain> drain;
			// Written by the coordinator and taken by the render thread's report, under the lock.
			std::mutex lock;
			ResidueClasses classes;
		} residueState;

		void OnCaptureDrained(const Diagnostics::CaptureDrained&)
		{
			// Taken every frame so it never accumulates. Names are read here, because the pass that classifies them runs past this frame's Present.
			auto& state = residueState;
			const auto residue = PassCapture::Get().TakeViewResidueGeometries(kView);
			if (residue.empty())
				return;  // a post no pass took stays for the next
			auto drain = std::make_shared<ResidueDrain>();
			drain->residue.reserve(residue.size());
			for (const auto* geometry : residue) {
				const auto* parent = geometry ? geometry->parent : nullptr;
				drain->residue.push_back({ geometry, geometry ? fmt::format("'{}' under '{}' ({})", geometry->name.c_str() ? geometry->name.c_str() : "",
																	parent && parent->name.c_str() ? parent->name.c_str() : "",
																	geometry->GetRTTI() && geometry->GetRTTI()->name ? geometry->GetRTTI()->name : "?") :
																std::string() });
			}
			// A post no scene pass took is given to this one, so none is lost or classified twice.
			if (const auto unread = state.posted.exchange(nullptr, std::memory_order_acq_rel))
				drain->residue.insert(drain->residue.begin(), unread->residue.begin(), unread->residue.end());
			state.posted.store(std::shared_ptr<const ResidueDrain>(std::move(drain)), std::memory_order_release);
		}

		void OnInputsTaken(const Diagnostics::On<Diagnostics::ScenePoint::AccumulateInputsTaken>&)
		{
			residueState.drain = residueState.posted.exchange(nullptr, std::memory_order_acq_rel);
		}

		void ClassifyResidue(const Scene::Coordinator& a_store)
		{
			// Classify by pointer and the names the render thread read. A geometry released since is untracked here and never dereferenced.
			auto& state = residueState;
			if (!state.drain)
				return;
			const auto& tracked = V::tracked(a_store);
			const std::uint32_t sceneFrame = V::sceneFrame(a_store);
			std::scoped_lock lock(state.lock);
			auto& classes = state.classes;
			for (const auto& [geometry, names] : state.drain->residue) {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(geometry));
				std::size_t kind;
				if (it == tracked.end())
					kind = kResidueUntracked;
				else if (it->second.candidateReason != Ineligible::None)
					kind = static_cast<std::size_t>(it->second.candidateReason);
				else if (const std::int32_t slot = a_store.FindObject(geometry); !V::ResidentObject(a_store, slot))
					kind = kResidueUnbound;
				else
					kind = (Scene::PhasesIn(V::tables(a_store), static_cast<std::uint32_t>(slot)) & kSetReflection) ? kResidueReflection : kResidueNotReflection;
				++classes.counts[kind];
				if (auto& seen = classes.seen[geometry]; seen.frame != sceneFrame) {
					seen.frame = sceneFrame;
					if (++seen.frames == 10)
						++classes.persistent;
				}
				if (classes.first[kind].empty() && it != tracked.end())
					classes.first[kind] = names;
			}
		}

		void OnJoinsBegin(const Diagnostics::On<Diagnostics::ScenePoint::JoinsBegin>&)
		{
			ClassifyResidue(Scene::Coordinator::Get());
		}
	}

	bool ResidueDue()
	{
		// Every 30th update while the parities observe.
		return Diagnostics::Observe().claimsOrScene && residueState.updates++ % 30 == 0;
	}

	void SubscribeResidue()
	{
		Diagnostics::Subscribe<Diagnostics::CaptureDrained>(&OnCaptureDrained);
		Diagnostics::Subscribe<Diagnostics::ScenePoint::AccumulateInputsTaken>(&OnInputsTaken);
		Diagnostics::Subscribe<Diagnostics::ScenePoint::JoinsBegin>(&OnJoinsBegin);
	}

	std::string ResidueKinds()
	{
		ResidueClasses classes;
		{
			std::scoped_lock lock(residueState.lock);
			classes = std::exchange(residueState.classes, {});
		}
		std::string residueKinds;
		for (std::size_t k = 0; k < kResidueKinds; ++k) {
			if (!classes.counts[k])
				continue;
			const std::string_view name = k < kResidueUntracked ? kIneligibleNames[k] :
			                              k == kResidueUntracked ? "untracked" :
			                              k == kResidueUnbound   ? "eligible, unbound" :
			                              k == kResidueNotReflection ? "bound, not a reflection member" :
			                                                          "a reflection member";
			residueKinds += fmt::format("{}{} {}{}", residueKinds.empty() ? " [" : "; ", classes.counts[k], name, classes.first[k].empty() ? "" : " (" + classes.first[k] + ")");
		}
		if (!residueKinds.empty())
			residueKinds += fmt::format("; {} geometries, {} of them on 10 frames or more]", classes.seen.size(), classes.persistent);
		return residueKinds;
	}
}
