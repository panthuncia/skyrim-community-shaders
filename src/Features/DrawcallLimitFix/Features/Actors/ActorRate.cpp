// Keeps off-screen actors animating at full rate when they cast a shadow.
// A high actor the pre-cull left kNotVisible animates at full rate only if its skin property holds this frame's gFrameCounter
// (Actor::ModifyAnimationUpdateData, 0x1406a37ef). Only the engine's depth and shadow registrations write that stamp,
// so it is lost once DCLF replaces those culls. We stamp every high actor whose bound lies in one of the last scene
// frame's shadow views. Runs on the main thread at Main::Update's entry, when no job runs.
// See skyrim-engine-notes.md, "the fade nodes' OnVisible stores and the actors' animation stamp".

#include "Features/DrawcallLimitFix/Features/Actors/Actors.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LightViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace DCLF::Features::Actors
{
	namespace
	{
		// The same on AE 1.6.1170 and SE 1.5.97.
		constexpr std::size_t kMiddleHighSkinProperty = 0x178;  // MiddleHighProcessData: the property Actor::ModifyAnimationUpdateData reads
		constexpr std::size_t kPropertyStamp = 0x80;            // BSShaderProperty::lastAccumulatedFrameCount

		// Written on the main thread. The report reads them on the render thread between frames.
		struct ActorRate
		{
			std::uint64_t entries = 0, actors = 0, noProperty = 0, stamped = 0, inSun = 0, inLocal = 0;
		};
		ActorRate actorRate;

		bool SphereInside(const ShadowCameras::Planes& a_planes, const RE::NiPoint3& a_centre, float a_radius)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!((a_planes.mask >> p) & 1))
					continue;
				const auto& plane = a_planes.plane[p];
				if (!(plane[0] * a_centre.x + plane[1] * a_centre.y + plane[2] * a_centre.z - plane[3] > -a_radius))
					return false;
			}
			return true;
		}

		void UpdateActorRate(bool a_running)
		{
			if (!a_running)
				return;
			auto* lists = RE::ProcessLists::GetSingleton();
			if (!lists)
				return;
			auto& c = actorRate;
			++c.entries;
			const std::uint32_t counter = Engine::Global<std::uint32_t>(Engine::Address::kFrameCounter);
			const std::uint32_t sceneFrame = Scene::FrameView::Get().GetFrame();
			const auto& sun = Shadows::Sun::SunBoard();
			const bool sunViews = sun.valid && sun.sceneFrame == sceneFrame;
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				if (!actor)
					continue;
				const auto* process = actor->GetActorRuntimeData().currentProcess;
				const auto* middleHigh = process ? process->middleHigh : nullptr;
				auto* property = middleHigh ? Engine::At<std::byte*>(middleHigh, kMiddleHighSkinProperty) : nullptr;
				auto* root = actor->Get3D2();
				++c.actors;
				if (!property || !root) {
					++c.noProperty;
					continue;
				}
				const auto& bound = root->worldBound;
				bool inSun = false, inLocal = false;
				// The cascade culls only see what the full-frustum cull kept, so test both.
				bool inFull = sun.fullFrusta.empty();
				for (const auto& planes : sun.fullFrusta)
					inFull = inFull || SphereInside(planes, bound.center, bound.radius);
				if (sunViews && inFull)
					for (std::uint32_t i = 0; i < sun.count && i < Shadows::Sun::SunFrame::kMaxCascades && !inSun; ++i)
						inSun = sun.cascades[i].valid && SphereInside(sun.cascades[i].cull, bound.center, bound.radius);
				if (!inSun)
					Shadows::Local::LightsBoard().ForEachView(sceneFrame, [&](const ShadowCameras::Camera& a_view, const std::array<float, 4>& a_sphere) {
						// Point lights have no cull planes, so test against the light's radius.
						if (inLocal)
							return;
						if (a_view.cull.mask) {
							inLocal = SphereInside(a_view.cull, bound.center, bound.radius);
						} else if (a_sphere[3] > 0.0f) {
							const float dx = bound.center.x - a_sphere[0], dy = bound.center.y - a_sphere[1], dz = bound.center.z - a_sphere[2];
							const float reach = a_sphere[3] + bound.radius;
							inLocal = dx * dx + dy * dy + dz * dz <= reach * reach;
						}
					});
				const bool dclf = inSun || inLocal;
				c.inSun += inSun ? 1 : 0;
				c.inLocal += inLocal ? 1 : 0;
				if (dclf) {
					std::atomic_ref<std::uint32_t>(Engine::At<std::uint32_t>(property, kPropertyStamp)).store(counter, std::memory_order_relaxed);
					++c.stamped;
				}
			}
		}

		void ActorRateText(std::string& a_text)
		{
			const auto c = std::exchange(actorRate, {});
			if (!c.entries)
				return;
			a_text += fmt::format(
				"[DCLF] actor rate (T7f checklist item 6): {} entries, {:.1f} high actors a frame ({:.1f} without a skin property or 3D), {:.1f} in the "
				"sun's cascade culls and {:.1f} in a local shadow view a frame, {:.1f} stamped by DCLF a frame\n",
				c.entries, double(c.actors) / double(c.entries), double(c.noProperty) / double(c.entries),
				double(c.inSun) / double(c.entries), double(c.inLocal) / double(c.entries), double(c.stamped) / double(c.entries));
		}
	}

	void RegisterActorRate(Api::InstallContext& a_context)
	{
		// Runs last at Main::Update's entry, after the fade writes. Nothing calls it without the primary cull.
		a_context.RegisterStandInObserver({ .afterWrites = &UpdateActorRate });
		a_context.RegisterReportLines({ .name = "actor rate", .point = Api::ReportPoint::AccumulatedWrites, .text = &ActorRateText });
	}
}
