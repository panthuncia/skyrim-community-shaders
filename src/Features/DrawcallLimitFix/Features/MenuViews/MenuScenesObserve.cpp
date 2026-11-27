// Observe step of the native menu scenes (temporary scaffolding, removed at the switch): at each latched menu scene draw, CS
// computes the draw from the scene graph and its own ports, and compares it with what the engine set up. Native mode only.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"
#include "Features/LightLimitFix.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "Snapshot/CoreFrame.h"

#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Switches.h"
#include "ShaderCache.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "State.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string>

namespace DCLF::Features::MenuViews::Scenes::Observe
{
	namespace
	{
		struct Counts
		{
			std::uint64_t draws = 0, differ = 0;
		};

		struct ObserveState
		{
			bool active = false;
			RE::NiCamera* camera = nullptr;
			View view = View::MenuColour;
			std::map<std::string, Counts> fields;
			std::map<std::string, std::uint64_t> others;
			std::uint64_t lighting = 0, logged = 0, reports = 0;
			// The batch's inputs, and the menu scene's last camera.
			Native::Inputs inputs;
			bool inputsMade = false;
			Native::PreviousCamera previous;
			// The batch's draws in the engine's order, and CS's order for the same root.
			std::vector<const RE::BSGeometry*> engineOrder;
			std::vector<std::uint32_t> engineHints;  // each engine draw's accumulation hint, for the order log
			std::vector<Native::ScenePass> mineOrder;
			std::uint64_t orderBatches = 0, orderDiffer = 0;
		};

		ObserveState& Get()
		{
			static ObserveState state;
			return state;
		}

		void Note(const std::string& a_field, bool a_differ, const std::string& a_detail)
		{
			auto& s = Get();
			auto& c = s.fields[a_field];
			++c.draws;
			if (a_differ) {
				// A draw CS cannot compute would be fatal in an owned scheme: each distinct one is logged, past the cap.
				static std::set<std::string> uncomputed;
				if (a_field.ends_with("computed") && uncomputed.insert(a_field + a_detail).second)
					logger::info("[DCLF] menu scenes observe: {} differs: {}", a_field, a_detail);
				else if (c.differ++ < 4 && s.logged++ < 400)
					logger::info("[DCLF] menu scenes observe: {} differs: {}", a_field, a_detail);
			}
		}

		std::string Rows(const float (*a_rows)[4], std::uint32_t a_count)
		{
			std::string text;
			for (std::uint32_t r = 0; r < a_count; ++r)
				text += std::format("[{} {} {} {}]", a_rows[r][0], a_rows[r][1], a_rows[r][2], a_rows[r][3]);
			return text;
		}

		/** @brief Compares two row arrays bit for bit. Names the first differing row. */
		void CompareRows(const std::string& a_field, const float (*a_engine)[4], const float (*a_mine)[4], std::uint32_t a_count, const std::string& a_what)
		{
			for (std::uint32_t r = 0; r < a_count; ++r)
				if (std::memcmp(a_engine[r], a_mine[r], 16) != 0) {
					Note(a_field, true,
						std::format("{} row {}: engine [{} {} {} {}] mine [{} {} {} {}]; engine {} mine {}", a_what, r, a_engine[r][0], a_engine[r][1], a_engine[r][2],
							a_engine[r][3], a_mine[r][0], a_mine[r][1], a_mine[r][2], a_mine[r][3], Rows(a_engine, a_count), Rows(a_mine, a_count)));
					return;
				}
			Note(a_field, false, {});
		}

		/**
		 * @brief The camera the engine's previous-frame rows hold at a menu render: the last one SetCameraData took, the world's in a frame
		 * that drew the world, else the menu scene's last.
		 */
		Native::PreviousCamera PreviousOf(const ObserveState& a_state)
		{
			if (::RenderGraph::Frame::Driver::Current().kind == ::RenderGraph::Frame::Kind::World) {
				const auto& main = Snapshot::FrameCamera::Main();
				const auto& eye = main.data.CameraPosAdjust;
				return { true, { Snapshot::FrameCamera::RowMajor(&main.data.CameraViewProjUnjittered), RE::NiPoint3{ eye.x, eye.y, eye.z } } };
			}
			return a_state.previous;
		}

		/** @brief Whether the engine's menu render renders shadow maps now: its colour accumulator's flag (+0x178), set for scheme 4 with a depth pass. */
		bool ShadowMapsNow(const RE::UI3DSceneManager& a_manager)
		{
			const auto* accumulator = reinterpret_cast<const std::uint8_t*>(a_manager.unk10.get());
			return accumulator && accumulator[0x178] != 0;
		}

		std::string GeometryName(const RE::BSGeometry* a_geometry)
		{
			return a_geometry && a_geometry->name.c_str() ? a_geometry->name.c_str() : "?";
		}
	}

	bool Enabled()
	{
		static const bool enabled = ::RenderGraph::Engine::Native::Active() && Switches::On("CS_MENU_SCENES_OBSERVE");
		return enabled;
	}

	void Begin(View a_view, RE::NiCamera* a_camera)
	{
		auto& s = Get();
		s.active = Enabled();
		s.view = a_view;
		s.camera = a_camera;
		s.inputsMade = false;
		s.engineOrder.clear();
		s.engineHints.clear();
		s.mineOrder.clear();
		if (s.active && (a_view == View::MenuColour || a_view == View::MenuDepth) && a_camera)
			if (auto* manager = RE::UI3DSceneManager::GetSingleton())
				if (auto* root = manager->menuObjects[static_cast<std::uint32_t>(manager->currentlightScheme)].get())
					Native::SceneGeometry(root, *a_camera, Native::SortsByClosestPoint(*manager), s.mineOrder);
	}

	void End()
	{
		auto& s = Get();
		if (s.active && s.view == View::MenuDepth && s.camera) {
			// The depth pass's membership: the engine's draws against the geometry CS gives a depth pass.
			std::vector<const RE::BSGeometry*> mine;
			Native::UtilityView view;
			view.view = View::MenuDepth;
			for (const auto& p : s.mineOrder) {
				Native::UtilityResult result;
				std::string why;
				if (!p.membrane && (Native::ComputeUtility(view, *p.geometry, result, why) || !why.empty()))
					mine.push_back(p.geometry);
			}
			auto engine = s.engineOrder;
			std::ranges::sort(engine);
			engine.erase(std::ranges::unique(engine).begin(), engine.end());
			std::ranges::sort(mine);
			std::string missing, extra;
			for (const auto* g : engine)
				if (!std::ranges::binary_search(mine, g))
					missing += GeometryName(g) + " ";
			for (const auto* g : mine)
				if (!std::ranges::binary_search(engine, g))
					extra += GeometryName(g) + " ";
			Note("menu-depth membership", !missing.empty() || !extra.empty(), std::format("engine only: {}; mine only: {}", missing, extra));
		}
		if (s.active && s.view == View::MenuColour && !s.engineOrder.empty()) {
			++s.orderBatches;
			// The blended list's order (hint 1 in the engine's passes, BlendedList in CS's), and whether the two agree on membership.
			std::vector<const RE::BSGeometry*> mine, engine;
			for (const auto& p : s.mineOrder)
				if (!p.membrane && Native::BlendedList(*p.geometry))
					mine.push_back(p.geometry);
			bool membership = true;
			for (std::size_t i = 0; i < s.engineOrder.size(); ++i) {
				const bool hint1 = s.engineHints[i] == 1;
				membership &= hint1 == Native::BlendedList(*s.engineOrder[i]);
				if (hint1)
					engine.push_back(s.engineOrder[i]);
			}
			Note("blended membership", !membership, "an engine draw's hint 1 and BlendedList disagree");
			if (mine != engine || !membership) {
				++s.orderDiffer;
				std::string pattern;
				for (const auto* g : s.engineOrder)
					pattern += GeometryName(g) + " ";
				static std::map<std::string, int> seen;
				if (seen[pattern]++ == 0 && seen.size() < 20) {
					// Each draw with its accumulation hint and the blended sort's key (bound centre along the camera's view direction, radius).
					const auto& r = s.camera->world.rotate.entry;
					const auto key = [&](const RE::BSGeometry* a_g) {
						const auto& c = a_g->worldBound.center;
						return std::format("{}/{}", r[1][0] * c.y + r[0][0] * c.x + r[2][0] * c.z, a_g->worldBound.radius);
					};
					static const REL::Relocation<const RE::NiRTTI*> orderedRtti{ RE::BSOrderedNode::Ni_RTTI };
					const auto ordered = [&](const RE::BSGeometry& a_g) {
						for (auto* node = a_g.parent; node; node = node->parent)
							if (node->GetRTTI() == orderedRtti.get())
								return true;
						return false;
					};
					std::string e, m;
					for (std::size_t i = 0; i < s.engineOrder.size(); ++i)
						e += std::format("{}(h{} {}{}{}) ", GeometryName(s.engineOrder[i]), s.engineHints[i], key(s.engineOrder[i]), Native::BlendedList(*s.engineOrder[i]) ? " B" : "",
							ordered(*s.engineOrder[i]) ? " O" : "");
					for (const auto* g : mine)
						m += GeometryName(g) + " ";
					const auto* manager = RE::UI3DSceneManager::GetSingleton();
					const auto* accumulator = manager ? reinterpret_cast<const std::uint8_t*>(manager->unk10.get()) : nullptr;
					logger::info("[DCLF] menu scenes observe: order differs (accumulator +0x50..52 {} {} {}): engine {} | mine blended {}", accumulator ? accumulator[0x50] : 0,
						accumulator ? accumulator[0x51] : 0, accumulator ? accumulator[0x52] : 0, e, m);
				}
			}
		}
		s.active = false;
		if (s.inputsMade && s.view == View::MenuColour)
			s.previous = { true, { s.inputs.camera.viewProj, s.inputs.eye } };
		if (++s.reports % 600 == 0)
			Report();
	}

	void Other(const char* a_shader, const RE::BSRenderPass* a_pass, std::uint32_t a_vertex, std::uint32_t a_pixel)
	{
		auto& s = Get();
		if (!s.active)
			return;
		if (a_pass && a_pass->geometry && (s.engineOrder.empty() || s.engineOrder.back() != a_pass->geometry)) {
			s.engineOrder.push_back(a_pass->geometry);
			s.engineHints.push_back(a_pass->accumulationHint);
		}
		const auto key = std::format("{} {} vs {:#x} ps {:#x} pass {:#x} ({})", NameOf(s.view), a_shader, a_vertex, a_pixel, a_pass ? a_pass->passEnum : 0u,
			a_pass ? GeometryName(a_pass->geometry) : "no pass");
		if (s.others[key]++ == 0)
			logger::info("[DCLF] menu scenes observe: draw {}", key);
	}

	void Lighting(const RE::BSRenderPass* a_pass, const LightingSceneConstants& a_engine, const float (*a_frame)[4], const ID3D11ShaderResourceView* const* a_views,
		const std::uint32_t* a_address, const std::uint32_t* a_filter, std::uint32_t a_vertex, std::uint32_t a_pixel)
	{
		auto& s = Get();
		if (!s.active || !a_pass || !a_pass->geometry || !s.camera)
			return;
		++s.lighting;
		const auto* geometry = a_pass->geometry;
		const auto name = GeometryName(geometry);
		Other("Lighting", a_pass, a_vertex, a_pixel);
		auto* manager = RE::UI3DSceneManager::GetSingleton();
		if (!manager)
			return;
		// Inputs once per batch: the engine's own previous camera is the last menu render's in a menu frame.
		if (!s.inputsMade) {
			s.inputs = Native::SampleInputs(*manager, *s.camera, PreviousOf(s), ShadowMapsNow(*manager));
			s.inputsMade = true;
		}
		const auto& in = s.inputs;
		// The shadow lights' dimmers CS computes against the ones the engine's shadow render just set.
		for (const auto& [light, dimmer] : in.shadowDimmers)
			Note("shadow light dimmer", dimmer != light->lodDimmer,
				std::format("{}: light {} engine {} mine {}", name, static_cast<const void*>(light), light->lodDimmer, dimmer));
		Native::LightingResult mine;
		std::string why;
		const bool made = Native::ComputeLighting(in, *geometry, mine, why);
		Note("computed", !made, std::format("{}: {}", name, why));
		if (!made)
			return;
		const std::uint32_t pass = PassDescriptorOf(a_pass->passEnum);
		Note("pass descriptor", mine.pass != pass, std::format("{}: engine {:#x} mine {:#x}", name, pass, mine.pass));
		Note("shader descriptors", mine.vertex != a_vertex || mine.pixel != a_pixel,
			std::format("{}: engine vs {:#x} ps {:#x}, mine vs {:#x} ps {:#x}", name, a_vertex, a_pixel, mine.vertex, mine.pixel));
		{
			std::string engineLights, mineLights;
			for (std::uint32_t l = 1; l < a_pass->numLights; ++l)
				engineLights += std::format("{} ", static_cast<const void*>(a_pass->sceneLights[l]));
			for (std::uint32_t l = 1; l < mine.lights.count; ++l)
				mineLights += std::format("{} ", static_cast<const void*>(mine.lights.lights[l]));
			const bool differ = engineLights != mineLights || a_pass->numShadowLights != mine.lights.shadow;
			Note("point lights", differ, std::format("{}: engine {}(shadow {}) mine {}(shadow {})", name, engineLights, a_pass->numShadowLights, mineLights, mine.lights.shadow));
			// Once per geometry: every scene node light with its position, radius and distance to the bound.
			static std::map<std::string, int> detailed;
			if (differ && detailed[name]++ == 0 && detailed.size() < 24) {
				std::string text;
				const auto& data = in.sceneNode->GetRuntimeData();
				const auto describe = [&](const char* a_kind, const RE::BSLight* a_light) {
					const auto* l = a_light ? a_light->light.get() : nullptr;
					if (!l)
						return;
					const auto d = l->world.translate - geometry->worldBound.center;
					text += std::format("{} {} at [{} {} {}] radius {} lodDimmer {} distance {}; ", a_kind, static_cast<const void*>(a_light), l->world.translate.x, l->world.translate.y,
						l->world.translate.z, l->GetLightRuntimeData().radius.x, a_light->lodDimmer, std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z));
				};
				for (const auto& light : data.activeShadowLights)
					describe("shadow", light.get());
				for (const auto& light : data.activeLights)
					describe("point", light.get());
				logger::info("[DCLF] menu scenes observe: lights of {} (bound [{} {} {}] radius {}): {}", name, geometry->worldBound.center.x, geometry->worldBound.center.y,
					geometry->worldBound.center.z, geometry->worldBound.radius, text);
			}
		}
		// Rows: only the floats the port writes; the rest of the engine's buffer holds what an earlier draw left.
		const auto compare = [&](const char* a_field, const float (*a_e)[4], const float (*a_m)[4], const float (*a_w)[4], std::uint32_t a_first, std::uint32_t a_count) {
			for (std::uint32_t r = a_first; r < a_first + a_count; ++r)
				for (std::uint32_t i = 0; i < 4; ++i)
					if (a_w[r][i] != 0.0f && std::memcmp(&a_e[r][i], &a_m[r][i], 4) != 0) {
						Note(a_field, true,
							std::format("{} pass {:#x} row {}: engine [{} {} {} {}] mine [{} {} {} {}]; engine {} mine {}", name, pass, r, a_e[r][0], a_e[r][1], a_e[r][2], a_e[r][3],
								a_m[r][0], a_m[r][1], a_m[r][2], a_m[r][3], Rows(a_e + a_first, a_count), Rows(a_m + a_first, a_count)));
						return;
					}
			Note(a_field, false, {});
		};
		{
			const std::uint32_t uploaded = mine.rawPixel & ~mine.pixel;
			const auto permutation = Native::PermutationOf(*geometry, false);
			Note("permutation ui words", permutation.renderToUI != a_engine.renderToUI || permutation.baseTextureIsWorking != a_engine.baseTextureIsWorking,
				std::format("{}: engine ui {} base {}, mine ui {} base {}", name, a_engine.renderToUI, a_engine.baseTextureIsWorking, permutation.renderToUI,
					permutation.baseTextureIsWorking));
			Note("permutation descriptors", a_engine.descriptors[1] != uploaded,
				std::format("{}: engine {:#x} {:#x} {:#x} {:#x} radius {} base {} ui {}; mine pixel {:#x}", name, a_engine.descriptors[0], a_engine.descriptors[1],
					a_engine.descriptors[2], a_engine.descriptors[3], a_engine.effectRadius, a_engine.baseTextureIsWorking, a_engine.renderToUI, uploaded));
		}
		const auto& c = mine.constants;
		const auto& w = mine.written;
		compare("vs technique", a_engine.vsTechnique, c.vsTechnique, w.vsTechnique, 0, kLightingVSTechniqueRows);
		compare("ps technique", a_engine.psTechnique, c.psTechnique, w.psTechnique, 0, kLightingPSTechniqueRows);
		compare("vs material", a_engine.vsMaterial, c.vsMaterial, w.vsMaterial, 0, kLightingVSMaterialRows);
		compare("ps material", a_engine.psMaterial, c.psMaterial, w.psMaterial, 0, kLightingPSMaterialRows);
		compare("vs World", a_engine.vsGeometry, c.vsGeometry, w.vsGeometry, 0, 3);
		compare("vs PreviousWorld", a_engine.vsGeometry, c.vsGeometry, w.vsGeometry, 3, 3);
		compare("vs geometry rest", a_engine.vsGeometry, c.vsGeometry, w.vsGeometry, 6, kLightingVSGeometryRows - 6);
		compare("ps geometry", a_engine.psGeometry, c.psGeometry, w.psGeometry, 0, 15);
		compare("ps point lights", a_engine.psGeometry, c.psGeometry, w.psGeometry, 15, 15);
		// Textures and samplers the material record writes.
		for (std::uint32_t t = 0; t < kLightingSlots; ++t) {
			if (!((mine.record.textureWritten >> t) & 1))
				continue;
			const auto* view = ::RenderGraph::Engine::Tripwire::Unwrap(mine.record.textures[t]);
			Note(std::format("texture t{}", t), view != a_views[t], std::format("{}: engine {} mine {}", name, static_cast<const void*>(a_views[t]), static_cast<const void*>(view)));
			Note(std::format("address t{}", t), mine.record.addressModes[t] != a_address[t], std::format("{}: engine {} mine {}", name, a_address[t], mine.record.addressModes[t]));
			if (mine.record.filterModes[t] != kUnwrittenFilterMode)
				Note(std::format("filter t{}", t), mine.record.filterModes[t] != a_filter[t], std::format("{}: engine {} mine {}", name, a_filter[t], mine.record.filterModes[t]));
		}
		float frame[kFrameRows][4]{};
		Native::FrameRows(in, frame);
		const float (*engineFrame)[4] = a_frame;
		for (std::uint32_t r = 0; r < kFrameRows; ++r)
			if (std::memcmp(engineFrame[r], frame[r], 16) != 0) {
				Note("frame", true, std::format("{} row {}: engine [{} {} {} {}] mine [{} {} {} {}]", name, r, engineFrame[r][0], engineFrame[r][1], engineFrame[r][2], engineFrame[r][3], frame[r][0], frame[r][1], frame[r][2], frame[r][3]));
				break;
			} else if (r + 1 == kFrameRows)
				Note("frame", false, {});
	}

	void Effect(const RE::BSRenderPass* a_pass, const EffectSceneConstants& a_engine, const ID3D11ShaderResourceView* const* a_views, const std::uint32_t* a_address,
		const std::uint32_t* a_filter)
	{
		auto& s = Get();
		if (!s.active || !a_pass || !a_pass->geometry || !s.camera)
			return;
		const auto name = GeometryName(a_pass->geometry);
		if (auto* manager = RE::UI3DSceneManager::GetSingleton(); manager && !s.inputsMade) {
			s.inputs = Native::SampleInputs(*manager, *s.camera, PreviousOf(s), ShadowMapsNow(*manager));
			s.inputsMade = true;
		}
		Native::EffectResult mine;
		std::string why;
		// A Lighting property's extra Effect pass is its membrane.
		const bool membrane = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty) != nullptr;
		const bool made = s.inputsMade && (membrane ? Native::ComputeMembrane(s.inputs, *a_pass->geometry, mine, why) : Native::ComputeEffect(s.inputs, *a_pass->geometry, mine, why));
		Note("effect computed", !made, std::format("{}: {}", name, why));
		if (made) {
			const std::uint32_t pass = a_pass->passEnum - Effects::kEffectPassEnumBase;
			Note("effect pass descriptor", mine.pass != pass, std::format("{}: engine {:#x} mine {:#x}", name, pass, mine.pass));
			const auto compare = [&](const char* a_field, const float (*a_e)[4], const float (*a_m)[4], const float (*a_w)[4], std::uint32_t a_count) {
				for (std::uint32_t r = 0; r < a_count; ++r)
					for (std::uint32_t i = 0; i < 4; ++i)
						if (a_w[r][i] != 0.0f && std::memcmp(&a_e[r][i], &a_m[r][i], 4) != 0) {
							Note(a_field, true, std::format("{} pass {:#x} row {}: engine {} mine {}", name, pass, r, Rows(a_e, a_count), Rows(a_m, a_count)));
							return;
						}
				Note(a_field, false, {});
			};
			const auto& c = mine.constants;
			const auto& w = mine.written;
			const auto permutation = Native::PermutationOf(*a_pass->geometry, true);
			constexpr auto kAlphaBlend = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::SourceAlphaBlend);
			Note("effect permutation", permutation.renderToUI != a_engine.renderToUI || (permutation.extra & kAlphaBlend) != (a_engine.descriptors[2] & kAlphaBlend),
				std::format("{}: engine ui {} extra {:#x}, mine ui {} extra {:#x}", name, a_engine.renderToUI, a_engine.descriptors[2], permutation.renderToUI, permutation.extra));
			// A membrane's rows are counted apart, as their own port.
			const std::string prefix = membrane ? "membrane" : "effect";
			const auto field = [&](const char* a_name) { return prefix + a_name; };
			compare(field(" vs technique").c_str(), a_engine.vsTechnique, c.vsTechnique, w.vsTechnique, kEffectVSTechniqueRows);
			compare(field(" ps technique").c_str(), a_engine.psTechnique, c.psTechnique, w.psTechnique, kEffectPSTechniqueRows);
			compare(field(" vs material").c_str(), a_engine.vsMaterial, c.vsMaterial, w.vsMaterial, kEffectVSMaterialRows);
			compare(field(" ps material").c_str(), a_engine.psMaterial, c.psMaterial, w.psMaterial, kEffectPSMaterialRows);
			compare(field(" vs geometry").c_str(), a_engine.vsGeometry, c.vsGeometry, w.vsGeometry, kEffectVSGeometryRows);
			compare(field(" ps geometry").c_str(), a_engine.psGeometry, c.psGeometry, w.psGeometry, kEffectPSGeometryRows);
			if (membrane) {
				Note("membrane shader descriptors", a_engine.descriptors[0] != mine.vertex || a_engine.descriptors[1] != (mine.rawPixel & ~mine.pixel),
					std::format("{}: engine {:#x} {:#x}, mine {:#x} {:#x}", name, a_engine.descriptors[0], a_engine.descriptors[1], mine.vertex, mine.rawPixel & ~mine.pixel));
				for (std::uint32_t t = 0; t < kEffectSlotsRead; ++t) {
					if (!((mine.record.textureWritten >> t) & 1))
						continue;
					const auto* view = ::RenderGraph::Engine::Tripwire::Unwrap(mine.record.textures[t]);
					const auto* engine = ::RenderGraph::Engine::Tripwire::Unwrap(a_views[t]);
					Note(std::format("membrane texture t{}", t), view != engine, std::format("{}: engine {} mine {}", name, static_cast<const void*>(engine), static_cast<const void*>(view)));
					Note(std::format("membrane sampler t{}", t), mine.record.addressModes[t] != a_address[t] || mine.record.filterModes[t] != a_filter[t],
						std::format("{}: engine {}/{} mine {}/{}", name, a_address[t], a_filter[t], mine.record.addressModes[t], mine.record.filterModes[t]));
				}
			}
		}
		static std::map<std::string, int> dumped;
		if (dumped[name]++ != 0)
			return;
		logger::info("[DCLF] menu scenes observe: effect {} pass {:#x} vs technique {} vs material {} vs geometry {} ps technique {} ps material {} ps geometry {}", name,
			a_pass->passEnum, Rows(a_engine.vsTechnique, kEffectVSTechniqueRows), Rows(a_engine.vsMaterial, kEffectVSMaterialRows), Rows(a_engine.vsGeometry, kEffectVSGeometryRows),
			Rows(a_engine.psTechnique, kEffectPSTechniqueRows), Rows(a_engine.psMaterial, kEffectPSMaterialRows), Rows(a_engine.psGeometry, kEffectPSGeometryRows));
	}

	void State(const RE::BSRenderPass* a_pass, std::uint32_t a_blend, std::uint32_t a_coverage, std::uint32_t a_write, std::uint32_t a_extra, std::uint32_t a_depth,
		std::uint32_t a_stencil, std::uint32_t a_stencilRef, std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_depthBias, float a_alphaRef,
		const Family::Viewport& a_viewport, std::int32_t a_target)
	{
		auto& s = Get();
		if (!s.active || !a_pass || !a_pass->geometry)
			return;
		const std::int32_t rt0 = static_cast<std::int32_t>(a_target & 0xFF) - 1;
		const auto frame = Family::FrameViewport(rt0, static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN), a_depthBias);
		const bool sameViewport = frame.rect == a_viewport.rect && frame.depthRange == a_viewport.depthRange;
		const auto* property = a_pass->shaderProperty;
		const auto* alpha = a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get();
		const auto key = std::format(
			"{} {}: blend {} coverage {} write {} extra {} depth {} stencil {} ref {:#x} fill {} cull {} bias {} alphaRef {} viewport {} {} {} {} "
			"range {} {} targets {} {} {} {} (frame viewport {}); property flags {:#x} alpha property {}",
			NameOf(s.view), GeometryName(a_pass->geometry), a_blend, a_coverage, a_write, a_extra, a_depth, a_stencil, a_stencilRef, a_fill, a_cull, a_depthBias, a_alphaRef,
			a_viewport.rect[0], a_viewport.rect[1], a_viewport.rect[2], a_viewport.rect[3], a_viewport.depthRange[0], a_viewport.depthRange[1], rt0, static_cast<std::int32_t>((a_target >> 8) & 0xFF) - 1, static_cast<std::int32_t>((a_target >> 16) & 0xFF) - 1,
			static_cast<std::int32_t>((static_cast<std::uint32_t>(a_target) >> 24) & 0xFF) - 1,
			sameViewport ? "same" : std::format("{} {} {} {}", frame.rect[0], frame.rect[1], frame.rect[2], frame.rect[3]), property ? property->flags.underlying() : 0ull,
			alpha ? std::format("flags {:#x} threshold {}", alpha->alphaFlags, alpha->alphaThreshold) : std::string("none"));
		if (s.others[key]++ == 0)
			logger::info("[DCLF] menu scenes observe: state {}", key);
		// A membrane is a Lighting property's Effect pass: Lighting pass numbers are above the Effect base too, so the shader decides.
		const bool membrane = netimmerse_cast<const RE::BSLightingShaderProperty*>(property) && a_pass->shader &&
		                      a_pass->shader->shaderType.get() == RE::BSShader::Type::Effect && a_pass->passEnum >= Effects::kEffectPassEnumBase &&
		                      ((a_pass->passEnum - Effects::kEffectPassEnumBase) & Effects::EffectFlag::Membrane) != 0;
		if (membrane) {
			const auto* manager = RE::UI3DSceneManager::GetSingleton();
			Native::DrawState mine;
			const bool made = manager && Native::MembraneStateOf(*a_pass->geometry, Native::SortsByClosestPoint(*manager), mine);
			Note("membrane state", !made || mine.blend != a_blend || mine.write != a_write || mine.depth != a_depth || mine.cull != a_cull || mine.depthBias != a_depthBias || a_alphaRef >= 0.0f || a_pass->accumulationHint != 3,
				std::format("{}: engine blend {} write {} depth {} cull {} bias {} alphaRef {} hint {}; mine blend {} write {} depth {} cull {} bias {}", GeometryName(a_pass->geometry),
					a_blend, a_write, a_depth, a_cull, a_depthBias, a_alphaRef, a_pass->accumulationHint, mine.blend, mine.write, mine.depth, mine.cull, mine.depthBias));
		}
	}

	void Bones(const RE::BSRenderPass* a_pass, std::span<const std::byte> a_engine)
	{
		auto& s = Get();
		if (!s.active || !a_pass || !a_pass->geometry || a_engine.size() < kBonesBytes)
			return;
		std::array<std::byte, kBonesBytes> mine{};
		if (!Native::BoneRows(*a_pass->geometry, mine)) {
			Note("palettes", true, std::format("{}: no palettes computed", GeometryName(a_pass->geometry)));
			return;
		}
		const auto* skin = a_pass->geometry->GetGeometryRuntimeData().skinInstance.get();
		const std::uint32_t rows = skin->numMatrices * 3;
		const auto* e = reinterpret_cast<const float (*)[4]>(a_engine.data());
		const auto* m = reinterpret_cast<const float (*)[4]>(mine.data());
		CompareRows("palette previous", e, m, rows, GeometryName(a_pass->geometry));
		CompareRows("palette current", e + kPaletteRows, m + kPaletteRows, rows, GeometryName(a_pass->geometry));
	}

	void Utility(const RE::BSRenderPass* a_pass, const UtilitySceneConstants& a_engine, const float (*a_frame)[4], std::uint32_t a_vertex, std::uint32_t a_slice,
		const Native::DrawState& a_state, const Family::Viewport& a_viewport, const ID3D11ShaderResourceView* a_t0, std::uint32_t a_address, std::uint32_t a_filter)
	{
		auto& s = Get();
		auto* manager = RE::UI3DSceneManager::GetSingleton();
		if (!s.active || !a_pass || !a_pass->geometry || !manager || (s.view != View::MenuDepth && s.view != View::MenuShadow))
			return;
		const auto name = GeometryName(a_pass->geometry);
		const std::string prefix = NameOf(s.view);
		const auto field = [&](const char* a_name) { return prefix + " " + a_name; };
		const std::uint32_t buffer = MaterialPort::SampleFrame().vanilla.textureTransformBuffer;
		// The views the draw can be from: the depth pass's camera, or each kept shadow light's descriptor drawing into the draw's slice.
		std::vector<Native::UtilityView> views;
		if (s.view == View::MenuDepth) {
			if (!s.camera)
				return;
			if (!s.inputsMade) {
				s.inputs = Native::SampleInputs(*manager, *s.camera, PreviousOf(s), ShadowMapsNow(*manager));
				s.inputsMade = true;
			}
			auto& view = views.emplace_back();
			view.view = View::MenuDepth;
			view.eye = s.camera->world.translate;
			view.textureTransformBuffer = buffer;
			view.viewport = Family::FrameViewport(kMainTarget, static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN), 0);
			view.viewport.depthRange = { 0.0f, 1.0f };
			Native::FrameRows(s.inputs, view.frame);
		} else if (auto* node = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[1]) {
			for (auto* light : node->GetRuntimeData().shadowLightsAccum) {
				if (!light || !light->light)
					continue;
				const auto& descriptors = light->GetRuntimeData().shadowmapDescriptors;
				const bool parabolic = light->GetIsParabolicLight();
				for (std::uint32_t i = 0; i < light->shadowMapCount && i < descriptors.size(); ++i) {
					const auto& bound = parabolic ? descriptors[0] : descriptors[i];
					auto* camera = descriptors[i].camera.get();
					const auto* data = descriptors[i].shaderAccumulator ? descriptors[i].shaderAccumulator->GetRuntimeData() : nullptr;
					if (!camera || !data || bound.shadowmapIndex != a_slice)
						continue;
					auto& view = views.emplace_back();
					view.view = View::MenuShadow;
					view.renderMode = static_cast<std::uint32_t>(data->renderMode);
					view.eye = camera->world.translate;
					if (parabolic)
						view.parabola = { 1.0f / light->light->GetLightRuntimeData().radius.x, i == 1 ? -1.0f : 1.0f };
					view.slice = a_slice;
					view.textureTransformBuffer = buffer;
					std::uint32_t width = 0, height = 0;
					if (auto* texture = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS].texture) {
						D3D11_TEXTURE2D_DESC desc{};
						texture->GetDesc(&desc);
						width = desc.Width;
						height = desc.Height;
					}
					ShadowCameras::Camera matrices;
					ShadowCameras::CameraMatrices(*camera, matrices);
					view.viewport.rect = ShadowCameras::Viewport(matrices.port, width, height);
					view.viewport.depthRange = { 0.0f, 1.0f };
					Native::UtilityFrameRows(*camera, view.frame);
				}
			}
		}
		if (views.empty()) {
			Note(field("view"), true, std::format("{}: no view of slice {}", name, a_slice));
			return;
		}
		// The candidate whose eye (FrameBuffer c40) is the engine's, on the engine's hemisphere.
		const auto* chosen = &views.front();
		for (const auto& view : views)
			if (std::memcmp(view.frame[40], a_frame[40], 12) == 0 && (view.parabola[1] == 0.0f || view.parabola[1] == a_engine.vsTechnique[1][1]))
				chosen = &view;
		Native::UtilityResult mine;
		std::string why;
		const bool made = Native::ComputeUtility(*chosen, *a_pass->geometry, mine, why);
		Note(field("computed"), !made, std::format("{}: {}", name, why.empty() ? "no pass in CS's rule" : why));
		if (!made)
			return;
		Note(field("vertex descriptor"), mine.vertex != a_vertex, std::format("{}: engine {:#x} mine {:#x} (technique {:#x})", name, a_vertex, mine.vertex, mine.technique));
		const auto compare = [&](const std::string& a_field, const float (*a_e)[4], const float (*a_m)[4], const float (*a_w)[4], std::uint32_t a_count) {
			for (std::uint32_t r = 0; r < a_count; ++r)
				for (std::uint32_t i = 0; i < 4; ++i)
					if (a_w[r][i] != 0.0f && std::memcmp(&a_e[r][i], &a_m[r][i], 4) != 0) {
						Note(a_field, true, std::format("{} row {}: engine {} mine {}", name, r, Rows(a_e, a_count), Rows(a_m, a_count)));
						return;
					}
			Note(a_field, false, {});
		};
		const auto& c = mine.constants;
		const auto& w = mine.written;
		compare(field("vs technique"), a_engine.vsTechnique, c.vsTechnique, w.vsTechnique, kVSTechniqueRows);
		compare(field("vs material"), a_engine.vsMaterial, c.vsMaterial, w.vsMaterial, kVSMaterialRows);
		compare(field("vs geometry"), a_engine.vsGeometry, c.vsGeometry, w.vsGeometry, kVSGeometryRows);
		compare(field("ps geometry"), a_engine.psGeometry, c.psGeometry, w.psGeometry, kPSGeometryRows);
		Note(field("alpha test ref"), std::memcmp(&a_engine.alphaTestRef, &c.alphaTestRef, 4) != 0, std::format("{}: engine {} mine {}", name, a_engine.alphaTestRef, c.alphaTestRef));
		const auto& m = mine.state;
		Note(field("state"),
			m.blend != a_state.blend || m.write != a_state.write || m.depth != a_state.depth || m.cull != a_state.cull || m.depthBias != a_state.depthBias ||
				m.stencil != a_state.stencil || m.stencilRef != a_state.stencilRef,
			std::format("{}: engine blend {} write {} depth {} cull {} bias {} stencil {}/{:#x}; mine blend {} write {} depth {} cull {} bias {} stencil {}/{:#x}", name,
				a_state.blend, a_state.write, a_state.depth, a_state.cull, a_state.depthBias, a_state.stencil, a_state.stencilRef, m.blend, m.write, m.depth, m.cull,
				m.depthBias, m.stencil, m.stencilRef));
		Family::Viewport viewport = chosen->viewport;
		viewport.depthRange[1] -= Family::DepthBiasViewportOffset(m.depthBias);
		Note(field("viewport"), viewport.rect != a_viewport.rect || viewport.depthRange != a_viewport.depthRange,
			std::format("{}: engine {} {} {} {} range {} {}; mine {} {} {} {} range {} {}", name, a_viewport.rect[0], a_viewport.rect[1], a_viewport.rect[2], a_viewport.rect[3],
				a_viewport.depthRange[0], a_viewport.depthRange[1], viewport.rect[0], viewport.rect[1], viewport.rect[2], viewport.rect[3], viewport.depthRange[0],
				viewport.depthRange[1]));
		// The frame rows but the previous camera's (c16-c19, c41), which no Utility technique here reads.
		for (std::uint32_t r = 0; r < kFrameRows; ++r) {
			if ((r >= 16 && r < 20) || r == 41)
				continue;
			if (std::memcmp(a_frame[r], chosen->frame[r], 16) != 0) {
				Note(field("frame"), true,
					std::format("{} row {}: engine [{} {} {} {}] mine [{} {} {} {}]", name, r, a_frame[r][0], a_frame[r][1], a_frame[r][2], a_frame[r][3], chosen->frame[r][0],
						chosen->frame[r][1], chosen->frame[r][2], chosen->frame[r][3]));
				break;
			}
			if (r + 1 == kFrameRows)
				Note(field("frame"), false, {});
		}
		if (mine.texture) {
			const auto* view = ::RenderGraph::Engine::Tripwire::Unwrap(MaterialPort::ViewOf(mine.texture));
			Note(field("t0"), view != a_t0 || mine.address != a_address || mine.filter != a_filter,
				std::format("{}: engine {} {}/{} mine {} {}/{}", name, static_cast<const void*>(a_t0), a_address, a_filter, static_cast<const void*>(view), mine.address, mine.filter));
		}
	}

	void Report()
	{
		auto& s = Get();
		std::string text;
		for (const auto& [field, counts] : s.fields)
			text += std::format("{}: {} of {}; ", field, counts.differ, counts.draws);
		logger::info("[DCLF] menu scenes observe: {} Lighting draws; order differs {} of {} batches; {}", s.lighting, s.orderDiffer, s.orderBatches, text);
	}
}
