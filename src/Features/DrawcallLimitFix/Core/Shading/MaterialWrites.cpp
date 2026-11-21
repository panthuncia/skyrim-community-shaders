// MaterialWrites: hooks on controller and material writes, which run on the engine's update thread, and their lock-free queues.
// Also defines the material-write sink in Api/MaterialWrites.h.

#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "RenderGraph/Async/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "RenderGraph/Fatal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>

namespace DCLF::MaterialWrites
{
	namespace
	{
		// ---- The write queue, only for CS_DCLF_CAPTURE_PARITY diagnostics. The render thread drains it once a frame and producers never block.
		RenderGraph::Frame::EventQueue<const RE::BSShaderMaterial*, 8192>& GetQueue()
		{
			static RenderGraph::Frame::EventQueue<const RE::BSShaderMaterial*, 8192> queue;
			return queue;
		}
		bool WritesQueued()
		{
			return Diagnostics::Observe().capture;
		}
		RenderGraph::Frame::EventQueue<const void*, 8192>& ShadingQueue()
		{
			// The scene pass drains this for the next frame's shading sample. It is per frame, so it wakes no pass.
			static RenderGraph::Frame::EventQueue<const void*, 8192> queue;
			return queue;
		}
		// The transform moves, for the kept shadow build.
		RenderGraph::Frame::EventQueue<const RE::BSShaderMaterial*, 8192>& TransformMoves()
		{
			static RenderGraph::Frame::EventQueue<const RE::BSShaderMaterial*, 8192> queue;
			return queue;
		}
		// The kept shadow build only runs while DCLF draws shadows. Without a drain a full queue allocates on every push,
		// so pushes stop after kTransformConsumerIdle and the next drain is told moves were lost.
		constexpr auto kTransformConsumerIdle = std::chrono::seconds(2);
		std::atomic<std::int64_t> transformConsumerSeen{ 0 };  // steady_clock ticks of the last drain, 0 before the first
		std::atomic<bool> transformMovesLost{ false };
		std::atomic<std::uint64_t> transformMovesDropped{ 0 };
		void PushTransformMove(const RE::BSShaderMaterial* a_material)
		{
			const auto seen = transformConsumerSeen.load(std::memory_order_relaxed);
			const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
			if (seen && now - seen < std::chrono::duration_cast<std::chrono::steady_clock::duration>(kTransformConsumerIdle).count())
				TransformMoves().Push(a_material);
			else {
				transformMovesDropped.fetch_add(1, std::memory_order_relaxed);
				if (!transformMovesLost.load(std::memory_order_relaxed))
					transformMovesLost.store(true, std::memory_order_relaxed);
			}
		}

		// ---- Hooks.
		// A controller's type field at +0x50: which member of the property or material it writes.
		std::uint32_t ControllerType(const RE::NiTimeController* a_controller)
		{
			return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(a_controller) + 0x50);
		}

		void NoteTarget(const RE::NiTimeController* a_controller)
		{
			// NiTimeController::target. These controllers only attach to a BSLightingShaderProperty and write through its material at +0x78.
			auto* property = static_cast<const RE::BSShaderProperty*>(a_controller->target);
			if (property && property->material)
				NoteWritten(property->material);
		}

		// Controller destination tables for AE 1.6.1170 and SE 1.5.97: float Update 0x14150DDE0 and colour Update 0x14150EA50,
		// SE 0x141321310 and 0x141321d90. Float tables hold indices, colour tables byte offsets. Other runtimes keep the conservative notification.
		const std::uint32_t* FloatDestinations()
		{
			return reinterpret_cast<const std::uint32_t*>(REL::Offset(Engine::Address::kFloatControllerTargets).address());
		}
		bool ControllerDestinationTablesKnown()
		{
			const auto version = REL::Module::get().version();
			static const bool known = (version == REL::Version{ 1, 6, 1170, 0 } || version == REL::Version{ 1, 5, 97, 0 }) &&
			                          Engine::Address::kFloatControllerTargets.Known() && Engine::Address::kColorControllerTargets.Known();
			return known;
		}
		const std::uint32_t* ColourDestinations()
		{
			return reinterpret_cast<const std::uint32_t*>(REL::Offset(Engine::Address::kColorControllerTargets).address());
		}

		// The writes are SceneWriterFramework call hooks. Each controller handler records its case on PRE, with scratch[0] the case,
		// [6] the type and [7] the property, and finishes that case on POST.
		enum ControllerCase : std::uintptr_t
		{
			kEmissive = 1,  // the property's emissive multiplier or colour (per-object shading)
			kTransforms,    // the texture transforms (float controller types above 0x13)
			kDestination,   // a material member the AE destination tables name
			kOther,         // anything else: the conservative notification
		};

		// BSLightingShaderPropertyFloatController::Update (AE 14150dde0, vtable slot 0x27). Type 0xb writes the emissive multiplier,
		// types above 0x13 the texture-transform buffers, every other type a material field.
		// While a feature patches the frame VS variable, a write that moved only the transforms captures nothing.
		// The kept shadow build still hears of it, because its rows read the transform themselves.
		void OnFloatController(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* controller = SceneWriter::Arg<RE::NiTimeController*>(a_event, 0);
			if (SceneWriter::IsPre(a_event)) {
				const auto type = ControllerType(controller);
				auto* property = static_cast<RE::BSLightingShaderProperty*>(controller->target);
				a_scratch[6] = type;
				a_scratch[7] = reinterpret_cast<std::uintptr_t>(property);
				if (type == 0xb && property) {
					a_scratch[0] = kEmissive;
					std::memcpy(&a_scratch[1], &property->emissiveMult, sizeof(property->emissiveMult));
				} else if (type > 0x13 && property && property->material) {
					auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(property->material);
					a_scratch[0] = kTransforms;
					a_scratch[1] = reinterpret_cast<std::uintptr_t>(material);
					static_assert(sizeof(material->texCoordOffset) + sizeof(material->texCoordScale) <= 4 * sizeof(std::uintptr_t));
					std::memcpy(&a_scratch[2], material->texCoordOffset, sizeof(material->texCoordOffset));
					std::memcpy(&a_scratch[4], material->texCoordScale, sizeof(material->texCoordScale));
				} else if (type <= 0x13 && type != 0xb && property && property->material && ControllerDestinationTablesKnown()) {
					auto* material = property->material;
					const auto offset = std::size_t(FloatDestinations()[type]) * sizeof(float);
					a_scratch[0] = kDestination;
					a_scratch[1] = reinterpret_cast<std::uintptr_t>(material);
					std::memcpy(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + offset, sizeof(std::uint32_t));
				} else
					a_scratch[0] = kOther;
				return;
			}
			const auto type = static_cast<std::uint32_t>(a_scratch[6]);
			auto* property = reinterpret_cast<RE::BSLightingShaderProperty*>(a_scratch[7]);
			switch (a_scratch[0]) {
			case kEmissive:
				if (std::memcmp(&a_scratch[1], &property->emissiveMult, sizeof(property->emissiveMult)) != 0)
					ShadingQueue().Push(property);
				break;
			case kTransforms:
				{
					auto* material = reinterpret_cast<RE::BSLightingShaderMaterialBase*>(a_scratch[1]);
					// A swap during the update belongs to the property, so capture its current material, which the property keeps alive.
					if (property->material != material && property->material)
						MaterialPort::PushCapture(property->material);
					const bool moved = property->material != material ||
					                   std::memcmp(&a_scratch[2], material->texCoordOffset, sizeof(material->texCoordOffset)) != 0 ||
					                   std::memcmp(&a_scratch[4], material->texCoordScale, sizeof(material->texCoordScale)) != 0;
					// Unless a feature patches the frame's variable, the transforms belong to the records, so capture.
					if (moved && property->material == material && !Extensions::FrameVSVariablePatched())
						NoteWritten(material);
					if (moved)
						PushTransformMove(material);
					break;
				}
			case kDestination:
				{
					auto* material = reinterpret_cast<RE::BSShaderMaterial*>(a_scratch[1]);
					const auto offset = std::size_t(FloatDestinations()[type]) * sizeof(float);
					// A member's shading takes the material's alpha, so resample its dependents too.
					if (property->material != material ||
						std::memcmp(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + offset, sizeof(std::uint32_t)) != 0) {
						NoteWritten(material);
						ShadingQueue().Push(property);
					}
					break;
				}
			default:
				if (type != 0xb && type <= 0x13) {
					NoteTarget(controller);
					if (property)
						ShadingQueue().Push(property);
				}
				break;
			}
		}

		// BSLightingShaderPropertyColorController::Update (AE 14150ea50, slot 0x27). Type 1 writes the emissive colour, every other type a material colour.
		void OnColorController(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			constexpr std::size_t kColour = 12;
			auto* controller = SceneWriter::Arg<RE::NiTimeController*>(a_event, 0);
			if (SceneWriter::IsPre(a_event)) {
				const auto type = ControllerType(controller);
				auto* property = static_cast<RE::BSLightingShaderProperty*>(controller->target);
				a_scratch[6] = type;
				a_scratch[7] = reinterpret_cast<std::uintptr_t>(property);
				if (type == 1 && property && property->emissiveColor) {
					a_scratch[0] = kEmissive;
					std::memcpy(&a_scratch[1], property->emissiveColor, kColour);
				} else if (type != 1 && property && property->material && ControllerDestinationTablesKnown()) {
					auto* material = property->material;
					a_scratch[0] = kDestination;
					a_scratch[1] = reinterpret_cast<std::uintptr_t>(material);
					std::memcpy(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + ColourDestinations()[type], kColour);
				} else
					a_scratch[0] = kOther;
				return;
			}
			const auto type = static_cast<std::uint32_t>(a_scratch[6]);
			auto* property = reinterpret_cast<RE::BSLightingShaderProperty*>(a_scratch[7]);
			switch (a_scratch[0]) {
			case kEmissive:
				if (!property->emissiveColor || std::memcmp(&a_scratch[1], property->emissiveColor, kColour) != 0)
					ShadingQueue().Push(property);
				break;
			case kDestination:
				{
					auto* material = reinterpret_cast<RE::BSShaderMaterial*>(a_scratch[1]);
					if (property->material != material ||
						std::memcmp(&a_scratch[2], reinterpret_cast<const std::byte*>(material) + ColourDestinations()[type], kColour) != 0)
						NoteWritten(material);
					break;
				}
			default:
				if (type != 1)
					NoteTarget(controller);
				break;
			}
		}

		// The material's in-place rewrites on the 14 Lighting material classes: BSShaderMaterial 02 and BSLightingShaderMaterialBase 08-0A.
		void OnMaterialRewrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			NoteWritten(SceneWriter::Arg<RE::BSShaderMaterial*>(a_event, 0));
		}
	}

	void Install()
	{
		// SceneWriterFramework's material call family, which PostPostLoad requires.
		bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_FLOAT_CONTROLLER_UPDATE, &OnFloatController) &&
		                  SceneWriter::SubscribeCall(SWF_HOOK_COLOR_CONTROLLER_UPDATE, &OnColorController);
		for (const std::uint32_t hook : { SWF_HOOK_MATERIAL_COPY_MEMBERS, SWF_HOOK_MATERIAL_LOAD_TEXTURE_SET, SWF_HOOK_MATERIAL_CLEAR_TEXTURES,
				 SWF_HOOK_MATERIAL_RECEIVE_VALUES })
			subscribed = subscribed && SceneWriter::SubscribeCall(hook, &OnMaterialRewrite, SWF_PHASE_POST);
		if (!subscribed)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the scene writer framework refused the material write hooks after reporting them installed");
		logger::info("[DCLF] material write hooks subscribed: float and colour controllers, 14 material classes");
	}

	void NoteWritten(const RE::BSShaderMaterial* a_material)
	{
		if (!a_material)
			return;
		if (WritesQueued())
			GetQueue().Push(a_material);
		MaterialPort::PushCapture(a_material);
	}

	void NoteShading(const RE::BSShaderProperty* a_property)
	{
		if (a_property)
			ShadingQueue().Push(a_property);
	}

	bool Drain(ankerl::unordered_dense::set<const RE::BSShaderMaterial*>& a_out)
	{
		// The queue spills when full and loses nothing, so a drain is always complete.
		GetQueue().Drain([&](const RE::BSShaderMaterial* a_material) { a_out.insert(a_material); });
		return true;
	}

	void DrainShadingChanges(std::vector<const void*>& a_out)
	{
		ShadingQueue().Drain([&](const void* a_property) { a_out.push_back(a_property); });
	}
}

namespace DCLF::Extensions
{
	// The transform moves' consumer side.
	bool DrainTransformMoves(std::vector<const RE::BSShaderMaterial*>& a_out)
	{
		using namespace MaterialWrites;
		// Store seen first, then read the flag. A push that found the consumer idle has set the flag before this read, or pushes after it.
		transformConsumerSeen.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
		const bool lost = transformMovesLost.exchange(false, std::memory_order_acq_rel);
		TransformMoves().Drain([&](const RE::BSShaderMaterial* a_material) { a_out.push_back(a_material); });
		return lost;
	}

	TransformMoveCounts TransformMoveStats()
	{
		using namespace MaterialWrites;
		return { TransformMoves().Spilled(), transformMovesDropped.load(std::memory_order_relaxed) };
	}
}

namespace DCLF::Api
{
	// The material-write sink a feature's hooks reach the write queues through.
	void NoteMaterialWritten(const RE::BSShaderMaterial* a_material)
	{
		MaterialWrites::NoteWritten(a_material);
	}

	void NotePropertyShading(const RE::BSShaderProperty* a_property)
	{
		MaterialWrites::ShadingQueue().Push(a_property);
	}
}
