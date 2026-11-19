// The scene's event handlers. Each SceneWriterFramework hook pushes the mirror's update and the scene pass's event on the writer's thread.

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SkinPartitionWatch.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValueMoves.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/Events.h"

#include <bit>

namespace DCLF::Scene
{
	// Each handler takes the values it compares into scratch on PRE and pushes on POST, on the writer's thread.
	namespace
	{
		using SceneWriter::Arg;
		using SceneWriter::IsPre;

		std::uintptr_t Bits(float a_value) { return std::bit_cast<std::uint32_t>(a_value); }
		float FloatOf(std::uintptr_t a_word) { return std::bit_cast<float>(static_cast<std::uint32_t>(a_word)); }

		// BSFadeNode::OnVisible (slot 0x34) and FUN_14147a160, the fade state machine: currentFade and the door state (+0x154).
		void OnFadeValues(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* fade = Arg<RE::BSFadeNode*>(a_event, 0);
			if (IsPre(a_event)) {
				a_scratch[0] = Bits(CurrentFade(fade));
				a_scratch[1] = fade->GetRuntimeData().unk154;
				return;
			}
			const float before = FloatOf(a_scratch[0]);
			const auto door = static_cast<std::uint8_t>(a_scratch[1]);
			if (CurrentFade(fade) != before || fade->GetRuntimeData().unk154 != door)
				PushNodeUpdate(fade, SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeDoor);
		}


		/** @brief FUN_14147a430 (node, level), the only writer of the fade node's LOD level (+0x152 & 0xF). */
		void OnLodLevel(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* fade = Arg<RE::BSFadeNode*>(a_event, 0);
			const std::uint8_t level = fade->GetRuntimeData().unk152 & 0xF;
			if (IsPre(a_event))
				a_scratch[0] = level;
			else if (level != a_scratch[0])
				PushNodeUpdate(fade, SceneCapture::NodeRecord::kFadeLevel);
		}

		// Sky::SetColor (0x14040d970) writes a weather-blend colour in place, and external emittance may point at it.
		void OnSkySetColor(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* colour = Arg<RE::NiColor*>(a_event, 1);
			static_assert(sizeof(RE::NiColor) <= 2 * sizeof(std::uintptr_t));
			if (!colour)
				return;
			if (IsPre(a_event))
				std::memcpy(a_scratch, colour, sizeof(RE::NiColor));
			else if (std::memcmp(a_scratch, colour, sizeof(RE::NiColor)) != 0)
				emittanceEvents.Push(colour);
		}

		// FUN_141480fc0 (object, &colour) points external emittance at a colour. The colour is a classification input, so this is a property event.
		void OnSetExternalEmittance(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (IsPre(a_event)) {
				auto* object = Arg<RE::NiAVObject*>(a_event, 0);
				auto* geometry = object ? object->AsGeometry() : nullptr;
				auto* shaderProperty = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
				auto* property = shaderProperty ? netimmerse_cast<RE::BSLightingShaderProperty*>(shaderProperty) : nullptr;
				// A port property's emissive re-point is an event too.
				const std::uint8_t port = shaderProperty && !property ? Extensions::PortOfRtti(shaderProperty->GetRTTI()) : std::uint8_t{ 0 };
				const auto emissiveOf = port ? Extensions::MaterialPorts()[port].emissiveOf : nullptr;
				a_scratch[0] = reinterpret_cast<std::uintptr_t>(property);
				a_scratch[1] = reinterpret_cast<std::uintptr_t>(shaderProperty);
				a_scratch[2] = reinterpret_cast<std::uintptr_t>(property ? property->emissiveColor : emissiveOf ? emissiveOf(*shaderProperty) :
																												  nullptr);
				a_scratch[3] = port;
				return;
			}
			auto* property = reinterpret_cast<RE::BSLightingShaderProperty*>(a_scratch[0]);
			auto* shaderProperty = reinterpret_cast<RE::BSShaderProperty*>(a_scratch[1]);
			const void* before = reinterpret_cast<const void*>(a_scratch[2]);
			const auto port = static_cast<std::uint8_t>(a_scratch[3]);
			const auto emissiveOf = port ? Extensions::MaterialPorts()[port].emissiveOf : nullptr;
			if (property && property->emissiveColor != before) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kEmissive);
			} else if (emissiveOf && emissiveOf(*shaderProperty) != before) {
				PushProperty(shaderProperty);
				PushPropertyUpdate(shaderProperty, SceneCapture::PropertyRecord::kEmissive);
			}
		}

		// FUN_1402d1280 and FUN_1402d5090 place a reference's 3D in its cell. They write the LOD type inline at +0x153 and snap only on
		// one branch, so read the statics after the call.
		void OnCellPlaceReference(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* reference = Arg<RE::TESObjectREFR*>(a_event, 1);
			auto* root = reference ? reference->Get3D() : nullptr;
			if (auto* fade = root ? root->AsFadeNode() : nullptr) {
				PushNodeUpdate(fade, kFadeStatics | kFadeState);
				Diagnostics::Notify(Diagnostics::FadeStaticsPlaced{ fade });
			}
		}

		// FUN_140b44490 rebuilds the rigid body's motion in place. Every motion type path ends here.
		void OnRigidBodyMotionType(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* body = Arg<RE::hkpRigidBody*>(a_event, 0))
				PushNodeUpdate(RE::TESHavokUtilities::FindCollidableObject(*body->GetCollidable()), SceneCapture::NodeRecord::kBody);
		}

		// NiObjectNET::RemoveController (0x140d269a0).
		void OnRemoveController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* target = Arg<RE::NiObjectNET*>(a_event, 0);
			if (!target)
				return;
			if (auto* object = netimmerse_cast<RE::NiAVObject*>(target))
				PushNodeUpdate(object, SceneCapture::NodeRecord::kControllers);
			else
				PushPropertyUpdate(netimmerse_cast<RE::BSShaderProperty*>(target), SceneCapture::PropertyRecord::kControllers);
		}

		// NiObjectNET::PrependController (FUN_140d268d0).
		void OnPrependController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* target = Arg<RE::NiObjectNET*>(a_event, 0);
			if (!target)
				return;
			if (auto* object = netimmerse_cast<RE::NiAVObject*>(target)) {
				PushNode(object);
				FrameValueMoves::PushNode(object);
				PushNodeUpdate(object, SceneCapture::NodeRecord::kControllers);
			} else {
				PushProperty(target);
				PushPropertyUpdate(netimmerse_cast<RE::BSShaderProperty*>(target), SceneCapture::PropertyRecord::kControllers);
			}
		}

		// FUN_140ea55a0: a node's transform from its rigid body.
		void OnHavokNodeTransform(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* collision = Arg<RE::NiCollisionObject*>(a_event, 0);
			PushNode(collision->sceneObject);
			FrameValueMoves::PushNode(collision->sceneObject);
		}

		// BSDismemberSkinInstance::UpdateDismemberPartion (0x14021a530), the only writer of editorVisible after load. The mirror finds the geometry by its skin.
		void PushSkinShown(const RE::BSDismemberSkinInstance* a_skin)
		{
			if (!a_skin)
				return;
			SceneCapture::GeometryRecord record;
			record.skin = a_skin;
			const auto& data = a_skin->GetRuntimeData();
			if (data.partitions)
				for (std::int32_t i = 0; i < data.numPartitions; ++i)
					record.shown.push_back(data.partitions[i].editorVisible ? 1 : 0);
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ SceneCapture::GeometryRecord::kDismember, std::move(record) });
		}

		void OnDismemberPartition(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushSkinShown(Arg<RE::BSDismemberSkinInstance*>(a_event, 0));
		}

		// FUN_140218200 stores an armor's shown partitions inline, on a job thread.
		void OnBipedDismemberPartitions(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushSkinShown(Arg<RE::BSDismemberSkinInstance*>(a_event, 2));
		}

		// The four quads' first geometry of a TESObjectLAND's loaded nodes (+0x40).
		template <class F>
		void ForEachLandQuad(const RE::TESObjectLAND* a_land, F&& a_visit)
		{
			const auto* loaded = a_land ? Engine::At<const RE::NiNode* const*>(a_land, 0x40) : nullptr;
			if (!loaded)
				return;
			for (std::uint32_t quad = 0; quad < 4; ++quad) {
				const auto* node = loaded[quad];
				if (!node || node->GetChildren().empty() || !node->GetChildren()[0])
					continue;
				if (auto* geometry = node->GetChildren()[0]->AsGeometry())
					a_visit(*geometry);
			}
		}

		// FUN_1402ad800 stores a land's landscape properties into its four quads (+0x128) after their attach.
		void OnLandSetupProperties(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			ForEachLandQuad(Arg<RE::TESObjectLAND*>(a_event, 0), [](RE::BSGeometry& a_geometry) { PushLeafUpdate(a_geometry); });
		}

		// FUN_1402ad0e0 gives each quad's material its LOD blend textures (+0xF8, +0x100) and landBlendParams (+0x108) after the cell's
		// attach. The record otherwise holds null for t13 and t15.
		void OnLandBlendParams(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			ForEachLandQuad(Arg<RE::TESObjectLAND*>(a_event, 0), [](RE::BSGeometry& a_geometry) {
				const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
				if (property)
					MaterialWrites::NoteWritten(property->material);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kLandBlend);
			});
		}

		RE::BSGeometry* GeometryArg(const SWF_EventHeader* a_event, std::uint32_t a_index)
		{
			auto* object = Arg<RE::NiAVObject*>(a_event, a_index);
			return object ? object->AsGeometry() : nullptr;
		}

		// FUN_1414abd10 sets projected UV flags and params inline on every Lighting or grass property under the object.
		void OnPropertiesProjectedUV(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0))
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(), SceneCapture::PropertyRecord::kProjected);
		}

		// BSGeometry::AttachProperty (0x140267cf0) sets the alpha property (+0x120) after attach, as object LOD blocks do.
		void OnGeometryAttachAlpha(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = Arg<RE::BSGeometry*>(a_event, 0))
				PushLeafUpdate(*geometry);
		}

		// FUN_1414ab770 writes the alpha test threshold (+0x32) inline every frame of a fade, and FUN_1401e95a0 swaps it for harvested flora.
		// A changed threshold is a structural event, since static flora is never rewritten otherwise. Unchanged values push nothing.
		void OnAlphasSetThreshold(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* geometry = GeometryArg(a_event, 0);
			const auto* alpha = geometry ? static_cast<const RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().alphaProperty.get()) : nullptr;
			if (!alpha)
				return;
			if (IsPre(a_event)) {
				a_scratch[0] = alpha->alphaThreshold;
				return;
			}
			if (alpha->alphaThreshold == a_scratch[0])
				return;
			PushProperty(alpha);
			PushAlphaUpdate(alpha, SceneCapture::AlphaRecord::kThreshold);
		}

		// FUN_1414ab580 sets weapon blood: the hidden bit, kWeaponBlood, and alpha blend bits cleared inline (& 0xFE01).
		void OnWeaponBloodGeometry(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0)) {
				if (const auto* alpha = geometry->GetGeometryRuntimeData().alphaProperty.get()) {
					PushProperty(alpha);
					PushAlphaUpdate(alpha, SceneCapture::AlphaRecord::kFlags);
				}
				PushNodeUpdate(geometry, SceneCapture::NodeRecord::kHidden);
			}
		}

		// FUN_140e880e0 releases a node's collision object (+0x40) as an equipped item loses physics. The node is at visit +0x10.
		void OnReleaseCollisionObject(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			if (IsPre(a_event)) {
				auto* visit = Arg<std::byte*>(a_event, 0);
				a_scratch[0] = visit ? *reinterpret_cast<std::uintptr_t*>(visit + 0x10) : 0;
			} else
				PushNodeUpdate(reinterpret_cast<RE::NiAVObject*>(a_scratch[0]), SceneCapture::NodeRecord::kBody);
		}

		// bhkWorld::InitHavok (0x140e86ad0) moves a collision object between nodes and clears the source's +0x40 inline.
		void OnHavokInit(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::NiAVObject*>(a_event, 1), SceneCapture::NodeRecord::kBody);
			PushNodeUpdate(Arg<RE::NiAVObject*>(a_event, 2), SceneCapture::NodeRecord::kBody);
		}

		// The 3D's LOD type (+0x153 & 0xF) after an inline writer: Papyrus Disable with a fade (FUN_140a3db90, type 9) or Enable (0x14034c9f0).
		void PushReferenceFadeType(RE::TESObjectREFR* a_reference)
		{
			auto* root = a_reference ? a_reference->Get3D() : nullptr;
			if (auto* fade = root ? root->AsFadeNode() : nullptr)
				PushNodeUpdate(fade, SceneCapture::NodeRecord::kFadeType);
		}
		void OnReferenceDisable(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushReferenceFadeType(Arg<RE::TESObjectREFR*>(a_event, 0));
		}
		void OnEnableCommand(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushReferenceFadeType(Arg<RE::TESObjectREFR*>(a_event, 2));
		}

		// FUN_140e8bd40, NiAVObject::SetCollisionObject (+0x40). A loader's clutter gets its body after the capture.
		void OnSetCollisionObject(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::NiAVObject*>(a_event, 0), SceneCapture::NodeRecord::kBody);
		}

		// FUN_14147c1e0 sets one shader flag on every property under the object, inline rather than through SetFlags.
		void OnPropertiesSetFlag(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			if (auto* geometry = GeometryArg(a_event, 0))
				PushPropertyUpdate(geometry->GetGeometryRuntimeData().shaderProperty.get(),
					SceneCapture::PropertyRecord::kFlags | SceneCapture::PropertyRecord::kMaterialOther);
		}

		// FUN_14147c690 sets the fade node (+0x60) of every shader property under the object.
		void OnPropertiesSetFadeNode(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			// A leaf update, not a property update, because only the leaf carries a reference. Otherwise an entry attached before the fade node was set would leave its root unlisted.
			if (auto* geometry = GeometryArg(a_event, 1))
				PushLeafUpdate(*geometry);
		}

		// FUN_14147a9b0 is the fade range's only writer (+0x128, +0x12C).
		void OnFadeSetRange(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::BSFadeNode*>(a_event, 0), SceneCapture::NodeRecord::kFadeNear | SceneCapture::NodeRecord::kFadeFar);
		}

		// FUN_14147aa00 sets the fade node's LOD type (+0x153 & 0xF).
		void OnFadeSetLodType(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushNodeUpdate(Arg<RE::BSFadeNode*>(a_event, 0), SceneCapture::NodeRecord::kFadeType);
		}

		// BSLightingShaderProperty::GetRenderPasses (slot 0x2A).
		void OnLodFadeRenderPasses(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSLightingShaderProperty*>(a_event, 0);
			if (IsPre(a_event)) {
				a_scratch[0] = Bits(property->specularLODFade);
				a_scratch[1] = Bits(property->envmapLODFade);
				a_scratch[2] = Bits(property->alpha);
				return;
			}
			// The LOD fades and the alpha left on the property, which non-member records shade with.
			if (Bits(property->specularLODFade) != a_scratch[0] || Bits(property->envmapLODFade) != a_scratch[1] || Bits(property->alpha) != a_scratch[2])
				lodFadeEvents.Push(property);
			if (Bits(property->alpha) != a_scratch[2])
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kAlphaValue);
		}

		// BSShaderProperty::SetFlags (0x14147bee0).
		void OnPropertySetFlags(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSShaderProperty*>(a_event, 0);
			if (IsPre(a_event))
				a_scratch[0] = property->flags.underlying();
			else if (property->flags.underlying() != a_scratch[0]) {
				PushProperty(property);
				// The glints follow kVertexLighting.
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kFlags | SceneCapture::PropertyRecord::kMaterialOther);
			}
		}

		// BSShaderProperty::SetMaterial (0x14147bff0).
		void OnPropertySetMaterial(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSShaderProperty*>(a_event, 0);
			if (IsPre(a_event))
				a_scratch[0] = reinterpret_cast<std::uintptr_t>(property->material);
			else if (reinterpret_cast<std::uintptr_t>(property->material) != a_scratch[0]) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kMaterial | SceneCapture::PropertyRecord::kMaterialAlpha |
												 SceneCapture::PropertyRecord::kMaterialOther);
			}
		}

		// BSLightingShaderProperty::SetMaterialAlpha (vtable slot 0x31).
		void OnPropertySetMaterialAlpha(const SWF_EventHeader* a_event, std::uintptr_t* a_scratch, void*)
		{
			auto* property = Arg<RE::BSLightingShaderProperty*>(a_event, 0);
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			if (IsPre(a_event))
				a_scratch[0] = Bits(material ? material->materialAlpha : 0.0f);
			else if (material && Bits(material->materialAlpha) != a_scratch[0]) {
				PushProperty(property);
				PushPropertyUpdate(property, SceneCapture::PropertyRecord::kMaterialAlpha);
			}
		}

		// Each move key goes to both the scene pass and FrameValues' producer.
		void PushMoved(const void* a_key)
		{
			PushMove(a_key);
			FrameValueMoves::PushKey(a_key);
		}

		// SceneWriterFramework's movers, counted by writer slot. Slots 2-4 are reserved for Features/Actors.
		void OnMoved(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_MovedEvent*>(a_event);
			static constexpr std::size_t kSlots[] = { 0, 1, 5, 6, 9 };
			PushMoved(event.key);
			if (event.writer < std::size(kSlots))
				CountMove(kSlots[event.writer]);
		}

		// A plugin's scene write made through SWF: a property swapped on a geometry, or an alpha property's flags.
		void OnSceneWrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& write = *reinterpret_cast<const SWF_SceneWriteEvent*>(a_event);
			if (write.kind == SWF_WRITE_SHADER_PROPERTY || write.kind == SWF_WRITE_ALPHA_PROPERTY) {
				if (auto* geometry = static_cast<RE::NiAVObject*>(write.object)->AsGeometry())
					PushLeafUpdate(*geometry);
			} else if (write.kind == SWF_WRITE_ALPHA_FLAGS) {
				PushProperty(write.object);
				PushAlphaUpdate(static_cast<const RE::NiAlphaProperty*>(write.object), SceneCapture::AlphaRecord::kFlags);
			}
		}

		// A shader property's reported write: its material captured and its record taken again.
		void PushPropertyWritten(RE::BSShaderProperty* a_property, SWF_Fields a_fields)
		{
			if (!a_property)
				return;
			if ((a_fields & SWF_FIELD_MATERIAL) && a_property->material) {
				MaterialWrites::NoteWritten(a_property->material);
				MaterialWrites::NoteShading(a_property);
			}
			PushProperty(a_property);
			PushPropertyUpdate(a_property, (1u << SceneCapture::PropertyRecord::kFieldCount) - 1);
		}

		// A plugin's report of its own direct store, such as RaceMenu's NiOverride values and body morphs.
		void OnWritten(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& written = *reinterpret_cast<const SWF_WrittenEvent*>(a_event);
			const SWF_Fields fields = written.fields;
			// A material is no NiObject, so tell it by its vtable, never by a virtual call.
			const void* vtable = *static_cast<const void* const*>(written.object);
			if (Extensions::MaterialClassOf(vtable) || Extensions::PortOfVtable(vtable)) {
				if (fields & SWF_FIELD_MATERIAL)
					MaterialWrites::NoteWritten(static_cast<const RE::BSShaderMaterial*>(written.object));
				return;
			}
			auto* object = static_cast<RE::NiObject*>(written.object);
			if (fields & (SWF_FIELD_MATERIAL | SWF_FIELD_SHADER_FLAGS))
				if (auto* property = netimmerse_cast<RE::BSShaderProperty*>(object)) {
					PushPropertyWritten(property, fields);
					return;
				}
			auto* node = netimmerse_cast<RE::NiAVObject*>(object);
			if (!node)
				return;
			auto* geometry = node->AsGeometry();
			if (geometry && (fields & (SWF_FIELD_MATERIAL | SWF_FIELD_SHADER_FLAGS)))
				PushPropertyWritten(geometry->GetGeometryRuntimeData().shaderProperty.get(), fields);
			if (geometry && (fields & SWF_FIELD_PROPERTIES))
				PushLeafUpdate(*geometry);
			if (geometry && (fields & SWF_FIELD_SKIN))
				PushGeometryUpdate(geometry, SceneCapture::GeometryRecord::kSkin | SceneCapture::GeometryRecord::kRenderer |
												 SceneCapture::GeometryRecord::kDismember);
			if (fields & SWF_FIELD_TRANSFORM) {
				PushNode(node);
				FrameValueMoves::PushNode(node);
			}
		}

		// NiSwitchNode's own child edits (its NiNode slots 0x35, 0x37-0x3C).
		void OnSwitchStructural(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			PushSwitchStructural(static_cast<RE::NiNode*>(reinterpret_cast<const SWF_NodeEvent*>(a_event)->node));
		}

		struct CallSubscription
		{
			std::uint32_t hook;
			SWF_EventFn handler;
			std::uint32_t phases;
		};
		constexpr std::uint32_t kPost = SWF_PHASE_POST;
		constexpr std::uint32_t kBoth = SWF_PHASE_PRE | SWF_PHASE_POST;
		constexpr CallSubscription kCalls[] = {
			{ SWF_HOOK_FADE_ON_VISIBLE, &OnFadeValues, kBoth },
			{ SWF_HOOK_FADE_UPDATE, &OnFadeValues, kBoth },
			{ SWF_HOOK_FADE_LOD_LEVEL, &OnLodLevel, kBoth },
			{ SWF_HOOK_FADE_SET_RANGE, &OnFadeSetRange, kPost },
			{ SWF_HOOK_FADE_SET_LOD_TYPE, &OnFadeSetLodType, kPost },
			{ SWF_HOOK_CELL_PLACE_REFERENCE, &OnCellPlaceReference, kPost },
			{ SWF_HOOK_CELL_PLACE_REFERENCE_AT, &OnCellPlaceReference, kPost },
			{ SWF_HOOK_RIGID_BODY_MOTION_TYPE, &OnRigidBodyMotionType, kPost },
			{ SWF_HOOK_RELEASE_COLLISION_OBJECT, &OnReleaseCollisionObject, kBoth },
			{ SWF_HOOK_SET_COLLISION_OBJECT, &OnSetCollisionObject, kPost },
			{ SWF_HOOK_HAVOK_INIT, &OnHavokInit, kPost },
			{ SWF_HOOK_REFERENCE_DISABLE, &OnReferenceDisable, kPost },
			{ SWF_HOOK_ENABLE_COMMAND, &OnEnableCommand, kPost },
			{ SWF_HOOK_PREPEND_CONTROLLER, &OnPrependController, kPost },
			{ SWF_HOOK_REMOVE_CONTROLLER, &OnRemoveController, kPost },
			{ SWF_HOOK_HAVOK_NODE_TRANSFORM, &OnHavokNodeTransform, kPost },
			{ SWF_HOOK_DISMEMBER_PARTITION, &OnDismemberPartition, kPost },
			{ SWF_HOOK_BIPED_DISMEMBER_PARTITIONS, &OnBipedDismemberPartitions, kPost },
			{ SWF_HOOK_PROPERTY_SET_FLAGS, &OnPropertySetFlags, kBoth },
			{ SWF_HOOK_PROPERTY_SET_MATERIAL, &OnPropertySetMaterial, kBoth },
			{ SWF_HOOK_LIGHTING_SET_MATERIAL_ALPHA, &OnPropertySetMaterialAlpha, kBoth },
			{ SWF_HOOK_LIGHTING_GET_RENDER_PASSES, &OnLodFadeRenderPasses, kBoth },
			{ SWF_HOOK_PROPERTIES_SET_FLAG, &OnPropertiesSetFlag, kPost },
			{ SWF_HOOK_PROPERTIES_SET_FADE_NODE, &OnPropertiesSetFadeNode, kPost },
			{ SWF_HOOK_PROPERTIES_PROJECTED_UV, &OnPropertiesProjectedUV, kPost },
			{ SWF_HOOK_SET_EXTERNAL_EMITTANCE, &OnSetExternalEmittance, kBoth },
			{ SWF_HOOK_SKY_SET_COLOR, &OnSkySetColor, kBoth },
			{ SWF_HOOK_GEOMETRY_ATTACH_ALPHA, &OnGeometryAttachAlpha, kPost },
			{ SWF_HOOK_ALPHAS_SET_THRESHOLD, &OnAlphasSetThreshold, kBoth },
			{ SWF_HOOK_FLORA_HARVEST_ALPHAS, &OnAlphasSetThreshold, kBoth },
			{ SWF_HOOK_WEAPON_BLOOD_GEOMETRY, &OnWeaponBloodGeometry, kPost },
			{ SWF_HOOK_LAND_SETUP_PROPERTIES, &OnLandSetupProperties, kPost },
			{ SWF_HOOK_LAND_BLEND_PARAMS, &OnLandBlendParams, kPost },
		};
	}
}

namespace DCLF::Extensions
{
	// A contributed node's list edit, from a feature's own hooks.
	void PushNodeUpdate(const RE::NiAVObject* a_node, std::uint32_t a_fields)
	{
		Scene::PushNodeUpdate(a_node, a_fields);
	}
}

namespace DCLF::Scene
{
	/**
	 * @brief Fade resets, after SWF made the store: currentFade (+0x130) stored inline as 0 or 1, then frame bit 14 set or cleared.
	 * Covers cell placements, the sky cell, Explosion::Load3D and every other such site.
	 */
	void OnFadeReset(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
	{
		const auto& event = *reinterpret_cast<const SWF_FadeResetEvent*>(a_event);
		PushNodeUpdate(static_cast<RE::NiAVObject*>(event.node), SceneCapture::NodeRecord::kFadeCurrent);
	}

	/** @brief An engine store changed an NiSwitchNode's index. */
	void OnSwitchIndex(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
	{
		const auto& event = *reinterpret_cast<const SWF_SwitchIndexEvent*>(a_event);
		auto* node = static_cast<RE::NiAVObject*>(event.node);
		// The mirror's value is safe on any thread. The tracking event is the render thread's alone.
		PushNodeUpdate(node, SceneCapture::NodeRecord::kSwitch);
		PushSwitch(node, event.before, false);
	}

	void PushHiddenUpdate(const void* a_object, std::uint32_t a_site)
	{
		// The mirror's bit and the hidden event in one update, so both land in the same batch.
		SceneCapture::Update update{ SceneCapture::NodeRecord::kHidden, SceneCapture::CaptureNodeFields(*static_cast<const RE::NiAVObject*>(a_object), SceneCapture::NodeRecord::kHidden) };
		update.hiddenSite = a_site;
		SceneTracker::Get().PushUpdate(std::move(update));
	}

	bool HiddenStoresLive()
	{
		return hiddenEventsInstalled;
	}

	void InstallMoveEvents()
	{
		// Without SWF's movers every mover is placed every frame.
		if (!(SceneWriter::Caps() & SWF_CAP_MOVES) || !SceneWriter::Subscribe(SWF_EVENT_MOVED, &OnMoved)) {
			logger::warn("[DCLF] move events: SceneWriterFramework has no movers (see the [SWF] log lines); every mover is placed every frame");
			return;
		}
		moveEventsInstalled = true;
		logger::info("[DCLF] move events installed (animation, 3D positions, ragdolls, the cells' update passes, projectiles)");
	}

	void InstallSceneEvents()
	{
		static bool installed = false;
		if (installed)
			return;
		installed = true;
		// PostPostLoad required these capabilities before any hook was written, so a refused subscription means SWF contradicted itself.
		for (const auto& call : kCalls)
			if (!SceneWriter::SubscribeCall(call.hook, call.handler, call.phases))
				stl::report_and_fail(fmt::format("Drawcall Limit Fix: the scene writer framework refused engine call hook {} after reporting it installed", call.hook));
		lodFadeEventsInstalled = true;
		// Switch selections are followed by event alone, and currentFade takes the inline resets through SWF's stores.
		if (!SceneWriter::Subscribe(SWF_EVENT_SWITCH_INDEX, &OnSwitchIndex) || !SceneWriter::Subscribe(SWF_EVENT_FADE_RESET, &OnFadeReset) ||
			!SceneWriter::Subscribe(SWF_EVENT_SWITCH_STRUCTURAL, &OnSwitchStructural))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused the switch or fade-reset subscriptions after reporting them installed");
		switchEventsInstalled = true;
		// A provider before ABI minor 2 has no scene writes, and every writer stores itself.
		if (const auto* api = SceneWriter::Api(); api && api->abiMinor >= 2 && !SceneWriter::Subscribe(SWF_EVENT_SCENE_WRITE, &OnSceneWrite))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused its scene write events at ABI minor 2");
		// Every ABI has NotifyWritten reports.
		if (!SceneWriter::Subscribe(SWF_EVENT_WRITTEN, &OnWritten))
			stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused its written events");
		logger::info("[DCLF] scene events subscribed to SceneWriterFramework ({} engine calls: fades, property flags, materials and material alpha, LOD fades, emittance, Havok node transforms, controllers)",
			std::size(kCalls));
		InstallMoveEvents();
		// RaceMenu's skin partition swaps, which no writer reports.
		SkinPartitionWatch::Install();
	}
}

namespace DCLF::Extensions
{
	// Pushes for the features' engine hooks, onto the scene's stack and Core's channels.

	void PushStacked(Api::EngineChannelId a_channel, std::uint64_t a_token, std::span<RE::NiAVObject* const> a_pinned, std::span<RE::NiAVObject* const> a_named)
	{
		const auto channels = RegisteredEngineChannels();
		const bool wakes = a_channel.index < channels.size() && channels[a_channel.index].wake == Api::WakeSource::Pass;
		SceneTracker::Get().PushStacked(a_channel.index, a_token, a_pinned, a_named, wakes);
	}

	void WakeChannel(Api::EngineChannelId a_channel)
	{
		const auto channels = RegisteredEngineChannels();
		if (a_channel.index < channels.size() && channels[a_channel.index].wake == Api::WakeSource::Pass)
			WakeScenePass(SceneWake::Channel);
	}

	void PushReservedHidden(const RE::NiAVObject* a_object, std::uint32_t a_site)
	{
		// A feature's own stores as events, on its reserved site.
		if (Scene::hiddenEventsInstalled && a_object)
			Scene::PushHiddenUpdate(a_object, a_site);
	}

	bool HiddenEventsLive()
	{
		return Scene::hiddenEventsInstalled;
	}

	void PushRangesUpdate(const RE::BSGeometry* a_geometry)
	{
		Scene::PushGeometryUpdate(a_geometry, SceneCapture::GeometryRecord::kRanges);
	}

	namespace
	{
		// Copied, because Scene::moveWriterNames views them. Written on the main thread at PostPostLoad.
		std::array<std::string, kMoveWriterSlots> reservedMoveWriterNames;
	}

	Api::MoveWriterId ReserveMoveWriter(std::string_view a_name)
	{
		if (a_name.empty())
			stl::report_and_fail("Drawcall Limit Fix: a move writer was reserved without a name");
		for (std::uint32_t slot = 0; slot < kMoveWriterSlots; ++slot) {
			if (!Scene::moveWriterNames[slot].empty())
				continue;
			reservedMoveWriterNames[slot] = a_name;
			Scene::moveWriterNames[slot] = reservedMoveWriterNames[slot];
			++Scene::moveWritersPending;
			return { slot };
		}
		stl::report_and_fail(fmt::format("Drawcall Limit Fix: the move writer {} is past Extensions::kMoveWriterSlots", a_name));
	}

	void MoveWriterInstalled(Api::MoveWriterId a_writer)
	{
		if (a_writer && Scene::moveWritersPending)
			--Scene::moveWritersPending;
	}

	bool CoreMoveWritersLive()
	{
		return Scene::moveEventsInstalled;
	}

	void PushReservedMove(Api::MoveWriterId a_writer, const void* a_key, Api::MoveReach a_reach)
	{
		if (a_reach != Api::MoveReach::Placements)
			Scene::PushMove(a_key);
		if (a_reach != Api::MoveReach::ScenePass)
			FrameValueMoves::PushKey(a_key);
		Scene::CountMove(a_writer.index);
	}
}
