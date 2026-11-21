#pragma once

// DCLF's extension API. A feature is a name plus an install function that registers the extension points it fills.
// Api/ uses only Api/, standard and ORG types. The contexts its callbacks receive are defined by Core in Core/Extensions/.

#include "Features/DrawcallLimitFix/Api/AncestorValues.h"
#include "Features/DrawcallLimitFix/Api/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Api/Commits.h"
#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Api/CullInputs.h"
#include "Features/DrawcallLimitFix/Api/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Api/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Api/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Api/LatePoses.h"
#include "Features/DrawcallLimitFix/Api/EngineEvents.h"
#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/FrameBindings.h"
#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Api/LightVolumes.h"
#include "Features/DrawcallLimitFix/Api/MainRanges.h"
#include "Features/DrawcallLimitFix/Api/Materials.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Api/Payload.h"
#include "Features/DrawcallLimitFix/Api/PrimaryView.h"
#include "Features/DrawcallLimitFix/Api/Reports.h"
#include "Features/DrawcallLimitFix/Api/RootStates.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Api/ShadingFields.h"
#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Api/ShadowViews.h"
#include "Features/DrawcallLimitFix/Api/SunVolume.h"
#include "Features/DrawcallLimitFix/Api/TableFamilies.h"
#include "Features/DrawcallLimitFix/Api/VertexStreams.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <string>
#include <string_view>

namespace DCLF::Api
{
	/**
	 * @brief What a feature registers with. Valid only during install, which runs once on the main thread before DCLF's threads start.
	 * Registrations are copied, read-only afterwards, and run in registration order. Their callbacks never run per object.
	 */
	class InstallContext
	{
	public:
		virtual FrameValuePoolId RegisterFrameValuePool(const FrameValuePool& a_pool) = 0;
		/** @brief Registers a pass that the frame schedule in Schedule/Frame.cpp names by key. */
		virtual void RegisterPass(const PassDef& a_pass) = 0;
		/** @brief Finds a schedule trigger that the feature's hook marks active around its DCLF work. */
		virtual TriggerId FindTrigger(std::string_view a_id) = 0;
		virtual void RegisterPayloadProduct(const PayloadProduct& a_product) = 0;
		virtual SceneResourcesId RegisterSceneResources(const SceneResources& a_resources) = 0;
		virtual void RegisterCommitHook(const CommitHook& a_hook) = 0;
		/** @brief Registers a frame binding. Its register must not clash with Core's or another binding's. */
		virtual void RegisterFrameBinding(const FrameBinding& a_binding) = 0;
		virtual SwitchId RegisterSwitch(const FeatureSwitch& a_switch) = 0;
		/** @brief Registers a classify input. */
		virtual ClassifyInputId RegisterClassifyInput(const ClassifyInputContribution& a_input) = 0;
		/**
		 * @brief Looks up a classify input by name, from any feature. It reads false until every install has run, and for good if
		 * nobody registers that name.
		 */
		virtual ClassifyInputId NeedClassifyInput(std::string_view a_name) = 0;
		virtual void RegisterReportLines(const ReportLines& a_lines) = 0;
		/** @brief Returns false, and never calls the observer, when the primary cull's engine hooks failed to install. */
		virtual bool RegisterStandInObserver(const StandInObserver& a_observer) = 0;
		/** @brief Registers an engine channel under a unique name. */
		virtual EngineChannelId RegisterEngineChannel(const EngineChannel& a_channel) = 0;
		/** @brief Finds a channel registered so far under a_name. */
		virtual EngineChannelId FindEngineChannel(std::string_view a_name) const = 0;
		/**
		 * @brief Looks up a channel by name, from any feature, for the feature's own hooks to push to.
		 * Pushes are dropped until every install has run, and for good if no channel by that name accepts pushes.
		 */
		virtual EngineChannelId NeedEngineChannel(std::string_view a_name) = 0;
		/** @brief Registers the one show veto. */
		virtual void RegisterShowVeto(const ShowVeto& a_veto) = 0;
		/** @brief Reserves a hidden-event site for the feature's own hidden events. */
		virtual std::uint32_t ReserveHiddenSite() = 0;
		/** @brief Registers a node class whose ordered children the mirror captures for the feature. */
		virtual void RegisterNodeCapture(const NodeCaptureContribution& a_capture) = 0;
		/** @brief Reserves a named move writer. Early hooks use Extensions::ReserveMoveWriter instead, because they have no context. */
		virtual MoveWriterId ReserveMoveWriter(std::string_view a_name) = 0;
		/**
		 * @brief Declares that the feature writes every material row's frame variables each frame, such as TexcoordOffset.
		 * Captures that only move those bytes then rewrite no record.
		 */
		virtual void RegisterFrameVSVariablePatch() = 0;
		/** @brief Registers a table family. */
		virtual TableFamilyId RegisterTableFamily(const TableFamily& a_family) = 0;
		virtual CoordinatorComponentId RegisterCoordinatorComponent(const CoordinatorComponent& a_component) = 0;
		/** @brief Registers a Lighting technique. Each technique belongs to one feature. */
		virtual void RegisterTechnique(const TechniqueContribution& a_technique) = 0;
		/** @brief Registers a geometry type. Each type belongs to one feature. */
		virtual void RegisterGeometryType(const GeometryTypeContribution& a_type) = 0;
		/** @brief Registers a parent and leaves pair. Each pair belongs to one feature. */
		virtual void RegisterParent(const ParentContribution& a_parent) = 0;
		/** @brief Registers an object class. Each class belongs to one feature. */
		virtual void RegisterObjectClass(const ObjectClassContribution& a_class) = 0;
		/** @brief Registers the one unheld roots contribution. */
		virtual void RegisterUnheldRoots(const UnheldRootsContribution& a_roots) = 0;
		/** @brief Registers an owner entry class. ownMotion requires everyFrame. */
		virtual void RegisterEntryClass(const EntryClassContribution& a_class) = 0;
		virtual void RegisterShading(const ShadingContribution& a_shading) = 0;
		/** @brief Registers frame globals. All registrations share Extensions::kFrameGlobalsFloats floats. */
		virtual FrameGlobalsSlot RegisterFrameGlobals(const FrameGlobalsContribution& a_globals) = 0;
		/** @brief Registers a view. Its phase and view bits must not clash with Core's or another view's. */
		virtual void RegisterView(const ViewContribution& a_view) = 0;
		/** @brief Registers work on a view some feature contributes. */
		virtual void RegisterViewWork(const ViewWork& a_work) = 0;
		/** @brief Registers a view root. A root belongs to one contribution or draw family. */
		virtual void RegisterViewRoot(const ViewRootContribution& a_root) = 0;
		/** @brief Registers a source of nodes hidden from the cull. */
		virtual void RegisterCullHiddenSource(const CullHiddenSource& a_source) = 0;
		/** @brief Registers an occlusion map. Its phase and view bits must not clash with Core's or another view's. */
		virtual void RegisterOcclusionMap(const OcclusionMapContribution& a_map) = 0;
		/** @brief Registers a shadow source. Each source belongs to one feature. */
		virtual void RegisterShadowSource(const ShadowSourceContribution& a_source) = 0;
		/** @brief Registers the one shadow exclusion. */
		virtual void RegisterShadowExclusion(const ShadowExclusionContribution& a_exclusion) = 0;
		/** @brief Registers a draw family. The faces' callbacks, sizes, names and root come all together or not at all. */
		virtual void RegisterDrawFamily(const DrawFamilyContribution& a_family) = 0;
		/** @brief Registers a main range. Slots and orders are unique, and an appended slot has no depth prepass. */
		virtual void RegisterMainRange(const MainRangeContribution& a_range) = 0;
		/** @brief Registers a held group and its main range. The group's slot must be an appended one. */
		virtual void RegisterHeldGroup(const HeldGroupContribution& a_group) = 0;
		/** @brief Registers the one near twin. */
		virtual void RegisterNearTwin(const NearTwinContribution& a_twin) = 0;
		/** @brief Registers a deferred range. Each slot has one registration, with every field set. */
		virtual void RegisterDeferredRange(const DeferredRangeContribution& a_range) = 0;
		/** @brief Registers the one second vertex stream. Its scene resources must be registered first. */
		virtual void RegisterVertexStream(const VertexStreamContribution& a_stream) = 0;
		/** @brief Registers palette rows for an item class. A pow2 rule needs a power-of-two maxUnits. */
		virtual void RegisterPaletteRows(const PaletteRowsContribution& a_rows) = 0;
		/** @brief Registers a material port and returns its material kind. Lighting is kind 0, and ports count up from 1. */
		virtual std::uint8_t RegisterMaterialPort(const MaterialPortContribution& a_port) = 0;
		/** @brief Registers a program family. Its shader type must belong to a material port. */
		virtual void RegisterProgramFamily(const ProgramFamilyContribution& a_family) = 0;
		/** @brief Registers a frame constant row within Core's feature rows. */
		virtual void RegisterFrameConstantRow(const FrameConstantRow& a_row) = 0;
		/** @brief Registers a frame texture in a free register from t16 to t127. */
		virtual void RegisterFrameTexture(const FrameTexture& a_texture) = 0;
		/** @brief Registers graph buffers in free registers from t16 to t127. */
		virtual void RegisterFrameGraphBuffers(const FrameGraphBuffers& a_buffers) = 0;
		/** @brief Registers the one light volumes contribution. */
		virtual void RegisterLightVolumes(const LightVolumesContribution& a_volumes) = 0;
		/** @brief Registers the one sun volume contribution. */
		virtual void RegisterSunVolume(const SunVolumeContribution& a_volume) = 0;
		/** @brief Registers the one ancestor value contribution. */
		virtual void RegisterAncestorValue(const AncestorValueContribution& a_value) = 0;
		/** @brief Registers a late pose under a unique name. */
		virtual void RegisterLatePose(const LatePoseContribution& a_pose) = 0;
		/** @brief Registers a shading field. Each field has one registration. */
		virtual ShadingFieldId RegisterShadingField(const ShadingFieldContribution& a_field) = 0;
		/** @brief Registers the one main cull input. Core's main cull reads a single walk. */
		virtual void RegisterMainCullInput(const MainCullInputContribution& a_input) = 0;
		/** @brief Registers a cull binding. Each slot has one binding. */
		virtual void RegisterCullBinding(const CullBinding& a_binding) = 0;
		/** @brief Registers the one depth pyramid. Core's cull tests a single pyramid. */
		virtual void RegisterDepthPyramid(const DepthPyramidContribution& a_pyramid) = 0;
		/** @brief Registers the one primary view handler. */
		virtual void RegisterPrimaryView(const PrimaryViewContribution& a_view) = 0;
		/** @brief Registers the one shadow epochs handler. */
		virtual void RegisterShadowEpochs(const ShadowEpochsContribution& a_epochs) = 0;
		/** @brief Registers the one root states producer. Its table family must be registered first. */
		virtual void RegisterRootStates(const RootStatesContribution& a_states) = 0;
		/** @brief Registers a material contributor. Its texture registers must ascend and not overlap any other registration's. */
		virtual void RegisterMaterialContributor(const MaterialContributor& a_contributor) = 0;
		/** @brief Registers a reference fact. */
		virtual ReferenceFactId RegisterReferenceFact(const ReferenceFact& a_fact) = 0;
		/** @brief Declares the geometry record's one extra value. */
		virtual void DeclareGeometryExtra(std::string_view a_extraName) = 0;
		/** @brief Registers property bits that no other registration owns. */
		virtual void RegisterPropertyBits(const PropertyBits& a_bits) = 0;
		/** @brief Registers a Lighting object flag. The object and record flags must each be a single unclaimed bit. */
		virtual void RegisterLightingObjectFlag(const LightingObjectFlag& a_flag) = 0;
		/** @brief Claims the Lighting key's spare bits. Only one feature may. */
		virtual void RegisterLightingKeyBits(const LightingKeyBits& a_bits) = 0;
		/** @brief Registers Lighting pass rules. Only one registration may set a raw rule. */
		virtual void RegisterLightingPassRules(const LightingPassRules& a_rules) = 0;
		/** @brief Declares a per-draw constant buffer in a stage register up to b13 that Core does not use. */
		virtual void DeclarePerDrawBuffer(const PerDrawBuffer& a_buffer) = 0;

	protected:
		~InstallContext() = default;
	};

	/**
	 * @brief A feature DCLF installs, listed in Features/FeatureList.cpp.
	 *
	 * - earlyHooks: engine hooks that must be in before Core's engine side reads what they change. Registers nothing.
	 * - sourceHooks: a shadow source's engine hooks. Called once per source in ShadowSource order, so hooks install in source order.
	 * - preflight: checks every engine site the feature will hook, before any hook is written.
	 *   A conflict leaves DCLF off with the site named, instead of half installed.
	 */
	struct Feature
	{
		std::string_view name;
		void (*install)(InstallContext& a_context) = nullptr;
		void (*earlyHooks)() = nullptr;
		void (*sourceHooks)(ShadowSourceHooks& a_hooks) = nullptr;
		void (*preflight)() = nullptr;
		/** @brief Optional, any thread. One short line on whether the feature is in use now. */
		std::string (*status)() = nullptr;
	};
}
