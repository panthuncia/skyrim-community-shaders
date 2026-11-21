// The water port's parity observer. For every latched water draw it recomputes VS and PS b0-b2 from the engine objects and
// compares them with what the engine's setup and CS's hooks wrote, bit for bit. Each mismatching field is logged once per
// (field, context) with both values. Components the engine does not write are not compared: they hold whatever the buffer held.
// Writes go through the bound shaders' constant tables, so a variable the shader lacks lands where the engine's write did.

#include "Features/DrawcallLimitFix/Features/Water/Port/WaterParity.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"

#include "Features/UnifiedWater.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Logging.h"

#include <atomic>
#include <bit>
#include <cstring>
#include <format>
#include <string>

namespace DCLF::Features::Water::Port
{
	namespace
	{
		bool parityEnabled = false;  // written once at install

		// How often the counts and the census roots are logged.
		constexpr std::uint32_t kSummaryFrames = 600;

		constexpr const char* kStage[2] = { "VS", "PS" };

		Util::ReasonLog& Mismatches()
		{
			static Util::ReasonLog log("[DCLF] water port: differs", spdlog::level::warn);
			return log;
		}

		Util::ReasonLog& Notes()
		{
			static Util::ReasonLog log("[DCLF] water port");
			return log;
		}

		struct Counts
		{
			std::uint64_t draws = 0;
			std::uint64_t compared = 0;    // floats
			std::uint64_t mismatched = 0;  // floats
			std::uint64_t skipped = 0;     // draws the observer could not check
			std::uint64_t passMismatches = 0;
		};

		struct Observer
		{
			FrameInputs frame;
			bool haveFrame = false;
			Counts period, total;
			std::uint32_t lastSummary = 0;
		};

		Observer& Get()
		{
			static Observer observer;
			return observer;
		}

		/** @brief The name of a group's row in Water.hlsl: the variable it starts or falls in, and its row within it. */
		std::string RowName(bool a_vertex, std::uint32_t a_group, std::uint32_t a_row)
		{
			auto match = [&](auto a_count, auto a_name) -> std::string {
				for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(a_count); ++i) {
					const auto place = PlacementOf(static_cast<decltype(a_count)>(i));
					if (!place.present || place.group != a_group || a_row < place.row || a_row >= place.row + place.rows)
						continue;
					const std::string name = a_name(static_cast<decltype(a_count)>(i));
					return place.rows > 1 ? std::format("{}[{}]", name, a_row - place.row) : name;
				}
				return "unnamed";
			};
			if (a_vertex)
				return match(VsVariable::Count, [](VsVariable v) { return std::string(VsName(v)); });
			return match(PsVariable::Count, [](PsVariable v) { return std::string(PsName(v)); });
		}

		std::uint32_t Bits(float a_value) { return std::bit_cast<std::uint32_t>(a_value); }

		/** @brief Compares one group's predicted rows with the latched ones. */
		void CompareGroup(bool a_vertex, std::uint32_t a_group, std::span<const Row> a_predicted, const float (*a_latched)[4], std::string_view a_context,
			std::string_view a_shape, Counts& a_counts)
		{
			for (std::uint32_t r = 0; r < a_predicted.size(); ++r) {
				const auto& row = a_predicted[r];
				for (std::uint32_t c = 0; c < 4; ++c) {
					if (!(row.written & (1u << c)))
						continue;
					++a_counts.compared;
					const float latched = a_latched[r][c];
					if (Bits(latched) == Bits(row.v[c]))
						continue;
					++a_counts.mismatched;
					const auto field = std::format("{} b{} c{} {}.{}", kStage[a_vertex ? 0 : 1], a_group, r, RowName(a_vertex, a_group, r), "xyzw"[c]);
					Mismatches().Report(std::format("{} ({})", field, a_context),
						std::format("latched {:08X} ({}), port {:08X} ({}), shape '{}' <- WATER PORT", Bits(latched), latched, Bits(row.v[c]), row.v[c], a_shape));
				}
			}
		}

		ConstantTables BoundTables(const RE::BSGraphics::VertexShader& a_vs, const RE::BSGraphics::PixelShader& a_ps)
		{
			ConstantTables tables;
			std::memcpy(tables.vs.data(), a_vs.constantTable.data(), tables.vs.size());
			std::memcpy(tables.ps.data(), a_ps.constantTable.data(), tables.ps.size());
			return tables;
		}

		/** @brief Checks GetRenderPasses' port against the pass the engine made. */
		void ComparePasses(const RE::BSRenderPass& a_pass, const ObjectInputs& a_object, std::uint32_t a_descriptor, std::uint32_t a_renderMode,
			std::string_view a_context, std::string_view a_shape, Observer& a_observer)
		{
			const std::uint32_t technique = TechniqueOf(a_descriptor);
			// A light pass's count is its technique, so the port is asked for that many lights plus the first entry.
			const auto passes = DeriveRenderPasses(a_object, a_renderMode, technique >= 1 && technique <= 7 ? technique + 1 : 0, a_observer.frame);
			auto note = [&](std::string_view a_what, std::uint32_t a_latched, std::uint32_t a_port) {
				++a_observer.period.passMismatches;
				++a_observer.total.passMismatches;
				Mismatches().Report(std::format("{} ({})", a_what, a_context), std::format("engine {:#x}, port {:#x}, shape '{}' <- WATER PORT", a_latched, a_port, a_shape));
			};
			if (passes.none || passes.localMap) {
				note("GetRenderPasses makes no main-path passes for a drawn shape", a_pass.passEnum, 0);
				return;
			}
			if (passes.waterFlags != a_object.waterFlags)
				note("the property's flags after kUnderwater follows the camera", a_object.waterFlags, passes.waterFlags);
			const PassSpec* expected = technique == kTechniqueStencil ? &passes.stencil : (technique >= 1 && technique <= 7) ? &passes.lights :
			                                                                                                                   &passes.surface;
			if (!expected->present || expected->passEnum != a_pass.passEnum)
				note(std::format("pass id of technique {}", technique), a_pass.passEnum, expected->present ? expected->passEnum : 0);
			// The engine's group 10 fades change the hint, so only the surface pass's own hints are compared.
			if (expected == &passes.surface && (a_pass.accumulationHint == 0 || a_pass.accumulationHint == 13) && a_pass.accumulationHint != passes.surface.accumulationHint)
				note("the surface pass's accumulation hint", a_pass.accumulationHint, passes.surface.accumulationHint);
			if (expected == &passes.surface && globals::state && globals::state->modifiedVertexDescriptor != passes.lookupDescriptor)
				note("CS's lookup descriptor of the surface pass", globals::state->modifiedVertexDescriptor, passes.lookupDescriptor);
		}

		void Summarise(Observer& a_observer, std::uint32_t a_frame)
		{
			if (a_frame - a_observer.lastSummary < kSummaryFrames)
				return;
			a_observer.lastSummary = a_frame;
			const auto& p = a_observer.period;
			const auto& t = a_observer.total;
			const bool bad = p.mismatched || p.passMismatches;
			logger::info("[DCLF] water port: {} draws checked in the last {} frames ({} floats, {} differ, {} pass ids differ, {} not checked); {} draws, {} floats differ in all{}",
				p.draws, kSummaryFrames, p.compared, p.mismatched, p.passMismatches, p.skipped, t.draws, t.mismatched, bad ? " <- WATER PORT" : " <- OK");
			a_observer.period = {};
		}

		const char* NameOf(View a_view) { return a_view == View::Mask ? "mask" : "surface"; }
	}

	void RegisterSwitches(Api::InstallContext& a_context)
	{
		const auto id = a_context.RegisterSwitch({ .name = "CS_DCLF_WATER_PARITY", .kind = Api::SwitchKind::Parity, .description = "1: every water draw the latch path takes is checked against the water port (VS and PS b0-b2 bit for bit, its pass id), and a census logs where drawn water hangs in the scene graph (dclf-water-indirect.md, Phase 0)", .after = "CS_DCLF_PARITY_BOTH" });
		parityEnabled = SwitchEnabled(id);
		if (parityEnabled)
			logger::info("[DCLF] water port: parity observer and census on (CS_DCLF_WATER_PARITY=1)");
	}

	bool ParityEnabled() { return parityEnabled; }

	void ObserveDraw(const DrawConstants& a_latched, const RE::BSRenderPass* a_pass, const Family::TriShape* a_shape, View a_view)
	{
		auto& observer = Get();
		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		Summarise(observer, frame);
		++observer.period.draws;
		++observer.total.draws;
		auto skip = [&](std::string_view a_why) {
			++observer.period.skipped;
			++observer.total.skipped;
			Notes().Report(std::format("a draw not checked: {}", a_why));
		};

		const auto* geometry = a_pass ? a_pass->geometry : nullptr;
		const auto* property = a_pass ? a_pass->shaderProperty : nullptr;
		if (!geometry || !property)
			return skip("no pass seen at SetupGeometry");
		if (static_cast<const void*>(geometry->GetGeometryRuntimeData().rendererData) != static_cast<const void*>(a_shape))
			return skip("the drawn TriShape is not the last SetupGeometry pass's geometry's");
		if (property->GetRTTI() != globals::rtti::BSWaterShaderPropertyRTTI.get())
			return skip("the pass's property is not a BSWaterShaderProperty");
		const auto& water = static_cast<const RE::BSWaterShaderProperty&>(*property);
		const auto* material = static_cast<const RE::BSWaterShaderMaterial*>(water.material);
		if (!material || material->GetType() != RE::BSShaderMaterial::Type::kWater)
			return skip("the property has no water material");
		const auto& rss = globals::game::shadowState->GetRuntimeData();
		const auto* vs = rss.currentVertexShader;
		const auto* ps = rss.currentPixelShader;
		if (!vs || !ps)
			return skip("no bound shader");

		// Frame values are captured at the frame's first latched draw, so a value that changes within a frame shows as a mismatch.
		static REL::Relocation<RE::BSShaderAccumulator**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
		const auto* accumulator = *mainAccumulator;
		if (!observer.haveFrame || observer.frame.frame != frame) {
			observer.frame = CaptureFrame(accumulator);
			observer.haveFrame = true;
		}
		const auto& f = observer.frame;

		const std::uint32_t descriptor = RawDescriptor(a_pass->passEnum);
		const auto object = CaptureObject(water, *geometry);
		// Vanilla LOD water and Unified Water's tiles both hang under WaterLOD. The LOD water has the property's LOD bit (0x2),
		// Unified Water's tiles never do (UnifiedWater.cpp sets their flags whole).
		const bool underWaterLod = [&] {
			for (const RE::NiAVObject* node = geometry; node && f.waterLodRoot; node = node->parent)
				if (node == f.waterLodRoot)
					return true;
			return false;
		}();
		const bool tile = underWaterLod && !(object.waterFlags & 0x2) && globals::features::unifiedWater.loaded;
		const std::string context = std::format("technique {}, {}, {}Unified Water {}, Linear Lighting {}", TechniqueOf(descriptor), NameOf(a_view),
			tile ? "a tile, " : "", f.unifiedWater.active ? "on" : "off", f.linearLighting ? "on" : "off");
		const char* shapeName = geometry->name.c_str() ? geometry->name.c_str() : "";

		if (globals::state && globals::state->currentPixelDescriptor != descriptor)
			Mismatches().Report(std::format("the bound descriptor is not the pass id's ({})", context),
				std::format("bound {:#x}, from the pass id {:#x} <- WATER PORT", globals::state->currentPixelDescriptor, descriptor));

		const auto bytes = BytesOf(*material);
		const auto predicted = Assemble(EvaluateTechnique(descriptor, f), EvaluateMaterial(bytes, f), EvaluateObject(object, descriptor, f),
			EvaluateGeometry(object, descriptor, f), EvaluateLights(*a_pass, bytes, descriptor, f), BoundTables(*vs, *ps));

		// A group whose buffer the shader does not have is not written by the engine.
		auto check = [&](bool a_vertex, std::uint32_t a_group, std::span<const Row> a_rows, const float (*a_latchedRows)[4]) {
			const bool bound = a_vertex ? vs->constantBuffers[a_group].buffer != nullptr : ps->constantBuffers[a_group].buffer != nullptr;
			if (!bound)
				return;
			CompareGroup(a_vertex, a_group, a_rows, a_latchedRows, context, shapeName, observer.period);
		};
		const auto before = observer.period.mismatched;
		check(true, 0, predicted.vsTechnique, a_latched.vsTechnique);
		check(true, 1, predicted.vsMaterial, a_latched.vsMaterial);
		check(true, 2, predicted.vsGeometry, a_latched.vsGeometry);
		check(false, 0, predicted.psTechnique, a_latched.psTechnique);
		check(false, 1, predicted.psMaterial, a_latched.psMaterial);
		check(false, 2, predicted.psGeometry, a_latched.psGeometry);
		observer.total.mismatched += observer.period.mismatched - before;

		const auto renderMode = accumulator ? static_cast<std::uint32_t>(accumulator->GetRuntimeData()->renderMode) : 0u;
		ComparePasses(*a_pass, object, descriptor, renderMode, context, shapeName, observer);

		Census::Note(*a_pass, frame, tile);
	}
}
