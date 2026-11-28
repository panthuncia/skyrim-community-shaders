// The sun's inputs at native's world masks, logged when they change between world renders. A rare fresh render drew the world
// with no sunlight (post-sun.md); in a still scene nothing here changes, so a logged line names the input that did.

#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/NativeCasters.h"

#include "RenderGraph/Engine/Native/RenderScene.h"
#include "Snapshot/CoreFrame.h"
#include "Snapshot/FrameSnapshot.h"
#include "Snapshot/Publication.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace DCLF::Features::ShadowMask
{
	namespace
	{
		/** @brief What one world render's masks read of the sun. */
		struct SunInputs
		{
			std::uint64_t snapshot = 0, renderFrame = 0;
			std::uint32_t renders = 0;
			bool valid = false;
			// The scene's mask flags.
			bool sceneShadows = false, sunShadowsOff = false;
			std::int32_t keptCasters = 0;
			// The set: lights, whether the sun is kept, its values, and whether its maps were drawn.
			bool haveSet = false;
			std::uint32_t lights = 0;
			bool sunKept = false, sunDrawn = false, sunHidden = false, sunCoverageLeft = false;
			std::uint32_t sunMask = 0xFF, sunMaps = 0, sunVerdict = 0, sunDraws = 0;
			float sunFade = 0.0f, sunDiffuse = 0.0f;
			// The cascades the masks and Deferred read.
			bool splitsLight = false;
			std::uint32_t splitsMaps = 0, descriptors = 0, badTransforms = 0;
			float splits = 0.0f, transforms = 0.0f;
			std::int32_t target = -1;
			std::uint32_t slices = 0;
			// The mask run and the frame's light.
			std::uint32_t maskDraws = 0;
			float nearPlane = 0.0f, farPlane = 0.0f;
			float dirColour = 0.0f, dirFade = 0.0f, sunlightScale = 0.0f;
			bool noSky = false;
			// DCLF's frame globals: what picks the draws' shadow passes and their sun colour, and the mask view they bind.
			bool interior = false, loading = false, deferredShadow = false, noSunShadowDir = false, pipeline = false;
			float pipelineColour = 0.0f;
			std::uintptr_t maskView = 0;
		};

		float Sum(const float* a_values, std::size_t a_count, std::uint32_t* a_bad = nullptr)
		{
			float sum = 0.0f;
			bool bad = false;
			for (std::size_t i = 0; i < a_count; ++i) {
				bad |= !std::isfinite(a_values[i]);
				sum += std::abs(a_values[i]);
			}
			if (a_bad && (bad || sum == 0.0f))
				++*a_bad;
			return sum;
		}

		SunInputs Read(const Shadows::Native::LightSet* a_set, const Run& a_run)
		{
			SunInputs in;
			const auto& render = Snapshot::Render();
			in.renderFrame = render.frame;
			in.renders = render.rendersOfSnapshot;
			if (const auto& snapshot = Snapshot::Current()) {
				in.snapshot = snapshot->id;
				const auto& frustum = snapshot->cameras.Current(Snapshot::ViewId::Main).frustum;
				in.nearPlane = frustum.fNear;
				in.farPlane = frustum.fFar;
			}
			const auto& scene = ::RenderGraph::Engine::Native::Scene::Current();
			in.valid = scene.valid;
			in.sceneShadows = scene.sceneShadows;
			in.sunShadowsOff = scene.sunShadowsOff;
			in.keptCasters = scene.keptCasters;
			if (a_set) {
				in.haveSet = true;
				in.lights = static_cast<std::uint32_t>(a_set->lights.size());
				const auto& sun = a_set->sun;
				in.splitsLight = sun.light != nullptr;
				in.splitsMaps = sun.maps;
				in.splits = Sum(sun.start.data(), sun.start.size()) + Sum(sun.end.data(), sun.end.size());
				in.descriptors = static_cast<std::uint32_t>(sun.descriptors.size());
				for (const auto& descriptor : sun.descriptors) {
					in.transforms += Sum(descriptor.lightTransform.data(), descriptor.lightTransform.size(), &in.badTransforms);
					in.target = descriptor.target;
					in.slices = in.slices * 4 + descriptor.slice;
				}
				if (const auto* kept = sun.light ? a_set->Find(sun.light) : nullptr) {
					in.sunKept = true;
					in.sunDrawn = Shadows::Native::DrewMaps(kept->light);
					in.sunHidden = kept->hidden;
					in.sunCoverageLeft = kept->coverageLeft;
					in.sunMask = kept->maskIndex;
					in.sunMaps = kept->maps;
					in.sunVerdict = kept->verdict;
					in.sunDraws = static_cast<std::uint32_t>(kept->draws.size());
					in.sunFade = kept->fade;
					in.sunDiffuse = Sum(kept->diffuse.data(), kept->diffuse.size());
				}
			}
			in.maskDraws = static_cast<std::uint32_t>(a_run.draws.size());
			const auto& core = Snapshot::Core();
			in.dirColour = std::abs(core.dirLightColor.red) + std::abs(core.dirLightColor.green) + std::abs(core.dirLightColor.blue);
			in.dirFade = core.dirLightFade;
			in.sunlightScale = core.sunlightScale;
			in.noSky = core.noSky;
			const auto& globals = FrameGlobals::Current();
			in.interior = globals.interior;
			in.loading = globals.loading;
			in.deferredShadow = globals.accumulator && globals.accumulatorDeferredShadow;
			in.noSunShadowDir = globals.noSunShadowDir != 0;
			in.maskView = reinterpret_cast<std::uintptr_t>(globals.technique.shadowMask);
			if (globals.pipeline) {
				in.pipeline = globals.pipeline->sun;
				in.pipelineColour = Sum(globals.pipeline->dirLightColor, 3);
			}
			return in;
		}

		/** @brief Whether a value moved by more than a_share of the larger magnitude. */
		bool Moved(float a_old, float a_new, float a_share)
		{
			const float scale = (std::max)(std::abs(a_old), std::abs(a_new));
			return std::abs(a_new - a_old) > a_share * scale && scale > 1e-6f;
		}

		std::string Changes(const SunInputs& a_old, const SunInputs& a_new)
		{
			std::string out;
			const auto discrete = [&](const char* a_name, auto a_was, auto a_now) {
				if (a_was != a_now)
					out += std::format(" {} {}->{}", a_name, a_was, a_now);
			};
			const auto real = [&](const char* a_name, float a_was, float a_now, float a_share) {
				if (Moved(a_was, a_now, a_share))
					out += std::format(" {} {:.4g}->{:.4g}", a_name, a_was, a_now);
			};
			discrete("valid", a_old.valid, a_new.valid);
			discrete("sceneShadows", a_old.sceneShadows, a_new.sceneShadows);
			discrete("sunShadowsOff", a_old.sunShadowsOff, a_new.sunShadowsOff);
			discrete("keptCasters", a_old.keptCasters > 0, a_new.keptCasters > 0);
			discrete("set", a_old.haveSet, a_new.haveSet);
			discrete("sunKept", a_old.sunKept, a_new.sunKept);
			discrete("sunDrawn", a_old.sunDrawn, a_new.sunDrawn);
			discrete("sunHidden", a_old.sunHidden, a_new.sunHidden);
			discrete("sunCoverageLeft", a_old.sunCoverageLeft, a_new.sunCoverageLeft);
			discrete("sunMask", a_old.sunMask, a_new.sunMask);
			discrete("sunMaps", a_old.sunMaps, a_new.sunMaps);
			discrete("sunVerdict", a_old.sunVerdict, a_new.sunVerdict);
			discrete("sunDraws", a_old.sunDraws, a_new.sunDraws);
			discrete("splitsLight", a_old.splitsLight, a_new.splitsLight);
			discrete("splitsMaps", a_old.splitsMaps, a_new.splitsMaps);
			discrete("descriptors", a_old.descriptors, a_new.descriptors);
			discrete("badTransforms", a_old.badTransforms, a_new.badTransforms);
			discrete("target", a_old.target, a_new.target);
			discrete("slices", a_old.slices, a_new.slices);
			discrete("noSky", a_old.noSky, a_new.noSky);
			discrete("interior", a_old.interior, a_new.interior);
			discrete("loading", a_old.loading, a_new.loading);
			discrete("deferredShadow", a_old.deferredShadow, a_new.deferredShadow);
			discrete("noSunShadowDir", a_old.noSunShadowDir, a_new.noSunShadowDir);
			discrete("pipelineSun", a_old.pipeline, a_new.pipeline);
			discrete("maskView", a_old.maskView, a_new.maskView);
			real("pipelineColour", a_old.pipelineColour, a_new.pipelineColour, 0.05f);
			real("sunFade", a_old.sunFade, a_new.sunFade, 0.05f);
			real("sunDiffuse", a_old.sunDiffuse, a_new.sunDiffuse, 0.05f);
			real("splits", a_old.splits, a_new.splits, 0.05f);
			real("transforms", a_old.transforms, a_new.transforms, 0.3f);
			real("near", a_old.nearPlane, a_new.nearPlane, 0.1f);
			real("far", a_old.farPlane, a_new.farPlane, 0.1f);
			real("dirColour", a_old.dirColour, a_new.dirColour, 0.05f);
			real("dirFade", a_old.dirFade, a_new.dirFade, 0.05f);
			real("sunlightScale", a_old.sunlightScale, a_new.sunlightScale, 0.05f);
			// A mask draw count change alone is a local light coming or going; it is listed only beside another change.
			if (!out.empty() && a_old.maskDraws != a_new.maskDraws)
				out += std::format(" maskDraws {}->{}", a_old.maskDraws, a_new.maskDraws);
			return out;
		}

		SunInputs s_last;     // render thread
		std::uint32_t s_lines = 0;
		constexpr std::uint32_t kMaxLines = 400;
	}

	void WatchSun(const Shadows::Native::LightSet* a_set, const Run& a_run)
	{
		const auto now = Read(a_set, a_run);
		if (s_last.renderFrame && s_lines < kMaxLines) {
			if (const auto changes = Changes(s_last, now); !changes.empty()) {
				++s_lines;
				logger::info("[DCLF] sun watch: render {} (snapshot {}, render {} of it) after render {} (snapshot {}):{}{}", now.renderFrame, now.snapshot,
					now.renders, s_last.renderFrame, s_last.snapshot, changes, s_lines == kMaxLines ? " (last line)" : "");
			}
		}
		s_last = now;
	}
}
