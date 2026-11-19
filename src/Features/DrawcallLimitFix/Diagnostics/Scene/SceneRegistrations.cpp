// Registration parity. The render thread reads the capture's drain and posts it for the next scene pass, which checks the
// registrations against the residency its walk left. The normal path reads no registration.

#include "SceneObservers.h"
#include "SceneState.h"

#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

namespace DCLF::Diagnostics
{
	namespace
	{
		using V = SceneView;

		bool parityOn = false;

		/** @brief The main camera's batch renderers, used to filter the capture. False before the first latch. */
		bool RefreshMainBatchRenderers()
		{
			auto* accumulator = V::latchedAccumulator() ? V::latchedAccumulator() : *globals::game::currentAccumulator.get();
			auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				return false;
			auto& renderers = SceneDiagnostics().mainBatchRenderers;
			renderers.clear();
			renderers.insert(batch);
			for (auto* group : batch->geometryGroups) {
				if (group && group->batchRenderer)
					renderers.insert(group->batchRenderer);
			}
			return true;
		}

		void OnCaptureDrained(const CaptureDrained& a_event)
		{
			auto& state = SceneDiagnostics();
			auto drain = std::make_shared<RegistrationDrain>();
			drain->observed = parityOn;
			const auto entries = a_event.entries;
			if (RefreshMainBatchRenderers()) {
				// Any Lighting pass registered names the engine instance DCLF's programs are built from.
				if (auto* lighting = EngineShaders::Lighting())
					for (const auto& entry : entries)
						if (entry.pass && entry.pass->shader && entry.pass->shader->shaderType.get() == RE::BSShader::Type::Lighting) {
							if (entry.pass->shader != lighting && state.registrations.lightingShaderDiffers++ == 0)
								logger::warn("[DCLF] the Lighting shader the registrations use ({}) is not the engine's instance DCLF's programs are of ({}) <- LIGHTING SHADER",
									static_cast<const void*>(entry.pass->shader), static_cast<const void*>(lighting));
							break;
						}
				// Eligible objects the engine registered that DCLF has not bound mean a missed scene event. The next scene pass checks them.
				for (const auto& entry : entries)
					if (entry.geometry && state.mainBatchRenderers.contains(entry.batch))
						drain->registrations.push_back({ entry.geometry, entry.hint });
			} else {
				drain->observed = false;
			}
			// Latest wins, so only the newest frame's registrations are checked.
			state.drainPosted.store(std::shared_ptr<const RegistrationDrain>(std::move(drain)), std::memory_order_release);
		}

		void OnInputsTaken(const On<ScenePoint::AccumulateInputsTaken>&)
		{
			// Nothing posted since the last pass leaves none, so no drain is checked twice.
			auto& state = SceneDiagnostics();
			state.drain = state.drainPosted.exchange(nullptr, std::memory_order_acq_rel);
		}

		void CheckRegistrations(const Scene::Coordinator& a_store)
		{
			// Only while the render thread observed the frame's registrations. The newest drain is judged against the residency this pass's
			// walk left, before its joins, so a record bound by the last pass counts as bound.
			auto& state = SceneDiagnostics();
			const auto& drain = state.drain;
			if (!drain || !drain->observed)
				return;
			auto& r = state.registrations;
			const auto& tracked = V::tracked(a_store);
			++r.registrationFrames;
			r.registrationsChecked += drain->registrations.size();
			for (const auto& entry : drain->registrations) {
				const auto it = tracked.find(const_cast<RE::BSGeometry*>(entry.geometry));
				if (it == tracked.end() || it->second.candidateReason != Ineligible::None || V::ResidentObject(a_store, a_store.FindObject(entry.geometry)))
					continue;
				++r.registeredUnbound;
				// The names are the engine's, so read them under a lease and skip without one.
				if (r.registeredUnboundFirst.empty())
					if (const EngineReadWindow::Lease lease; lease)
						r.registeredUnboundFirst = fmt::format("'{}' under '{}' (hint {})", entry.geometry->name.c_str() ? entry.geometry->name.c_str() : "",
							entry.geometry->parent && entry.geometry->parent->name.c_str() ? entry.geometry->parent->name.c_str() : "", entry.hint);
			}
		}

		void OnJoinsBegin(const On<ScenePoint::JoinsBegin>&)
		{
			CheckRegistrations(Scene::Coordinator::Get());
		}
	}

	void InstallRegistrations(bool a_parity)
	{
		parityOn = a_parity;
		if (!parityOn)
			return;
		Subscribe<CaptureDrained>(OnCaptureDrained);
		Subscribe<ScenePoint::AccumulateInputsTaken>(OnInputsTaken);
		Subscribe<ScenePoint::JoinsBegin>(OnJoinsBegin);
	}
}
