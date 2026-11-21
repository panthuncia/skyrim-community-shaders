// The extension registry: the install context features register with, and the lists it fills.

#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ClassifyInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DepthPyramid.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LatePoses.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MainRanges.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PrimaryView.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ViewCommon.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "RenderGraph/Fatal.h"

#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <vector>

namespace DCLF::Extensions
{
	namespace
	{
		// Defaults the show veto stubs load when no veto is registered. They never open and never veto.
		const std::atomic<std::uint32_t> kShowVetoClosed{ 0 };
		bool NoShowVeto(const void*) { return false; }

		// Written only during InstallFeatures on the main thread, before DCLF's threads exist. Read-only after that.
		struct Lists
		{
			std::vector<FrameValuePoolEntry> pools;
			std::vector<PayloadProductEntry> products;
			std::vector<SceneResourcesEntry> sceneResources;
			std::uint32_t sceneResourceInputs = 0;
			std::vector<CommitHookEntry> commitHooks;
			std::vector<FrameBindingEntry> frameBindings;
			std::vector<ReportLinesEntry> reportLines;
			std::vector<Api::StandInObserver> standInObservers;
			std::vector<EngineChannelEntry> engineChannels;
			// Engine channels needed by name, bound once every install has run.
			std::vector<std::string> engineChannelNeeds;
			// The show veto cells. Their address stays fixed for the session.
			Api::ShowVeto showVeto{ &kShowVetoClosed, &NoShowVeto };
			bool showVetoRegistered = false;
			std::uint32_t reservedHiddenSites = 0;
			std::vector<TableFamilyEntry> tableFamilies;
			std::vector<CoordinatorComponentEntry> coordinatorComponents;
			// Per entry class, whether the component that follows it takes it.
			std::array<bool, static_cast<std::size_t>(Api::EntryClass::Count)> entryClassesTaken{};
			TechniqueEntries techniques{};
			// At most one property flag rule per technique.
			std::array<PropertyFlagRule, kTechniqueSlots> propertyFlagRules{};
			std::size_t propertyFlagRuleCount = 0;
			GeometryTypesPorted geometryTypes{};
			// Per geometry type, its contribution. geometryType 0xFF means none.
			GeometryTypeContributions geometryTypeContributions{};
			EntryClassRules entryClassRules{};
			// Owner classes by form type, and their timing labels.
			OwnerClasses ownerClasses{};
			std::array<bool, static_cast<std::size_t>(Api::EntryClass::Count)> ownerClassRegistered{};
			std::string ownedLabel = "owned", ownedWithoutRecordLabel = "owned without a record";
			ParentsTaken parents{};
			ObjectClassesTaken objectClasses{};
			CategoryRoots categoryRoots{};
			CategoryRootGathers categoryRootGathers{};
			Api::UnheldRootsContribution unheldRoots{};
			std::vector<ShadingContributionEntry> shadingContributions;
			std::vector<FrameGlobalsEntry> frameGlobals;
			std::uint32_t frameGlobalsFloats = 0;
			// Per view, its contribution. EngineView::Count means none.
			std::array<Api::ViewContribution, kEngineViews> views{};
			std::vector<Api::ViewWork> viewWork;
			std::array<Api::ViewRootContribution, kViewRoots> viewRoots{};
			std::vector<Api::CullHiddenSource> cullHiddenSources;
			// Per map and per source, its contribution. Count means none.
			std::array<Api::OcclusionMapContribution, kOcclusionMaps> occlusionMaps{};
			std::array<Api::ShadowSourceContribution, kShadowSources> shadowSources{};
			// An empty name means none.
			Api::ShadowExclusionContribution shadowExclusion{};
			std::array<Api::DrawFamilyContribution, kDrawFamilies> drawFamilies{};
			// Per slot, its range, including held groups' ranges.
			std::array<Api::MainRangeContribution, kMainRangeSlots> mainRanges{};
			// A null near twin target means none.
			std::vector<HeldGroupEntry> heldGroups;
			NearTwinEntry nearTwin;
			// Per slot, its range. Count means none.
			std::array<Api::DeferredRangeContribution, kDeferredRanges> deferredRanges{};
			// An input's index is its id.
			std::vector<ClassifyInputEntry> classifyInputs;
			std::vector<std::string> classifyInputNeeds;
			std::vector<FrameGraphBuffersEntry> frameGraphBuffers;
			// An empty name means none.
			Api::LightVolumesContribution lightVolumes{};
			Api::SunVolumeContribution sunVolume{};
			Api::AncestorValueContribution ancestorValue{};
			std::vector<Api::LatePoseContribution> latePoses;
			// Entry class Count means none.
			Api::VertexStreamContribution vertexStream{};
			Api::MainCullInputContribution mainCullInput{};
			std::array<Api::CullBinding, kCullBindingSlots> cullBindings{};
			Api::DepthPyramidContribution depthPyramid{};
			Api::PrimaryViewContribution primaryView{};
			std::string primaryViewFeature;
			Api::ShadowEpochsContribution shadowEpochs{};
			std::string shadowEpochsFeature;
			Api::RootStatesContribution rootStates{};
			// Indexed by palette rows class minus 1. itemClass Count means none.
			std::array<Api::PaletteRowsContribution, Api::kPaletteRowsClasses> paletteRows{};
			std::vector<Api::ShadingFieldContribution> shadingFields;
			std::array<bool, kShadingRowFields> shadingFieldTaken{};
			// Lighting is kind 0, and the features' ports follow in registration order.
			std::array<Api::MaterialPortContribution, kMaterialKinds> materialPorts{};
			std::size_t materialPortCount = 0;
			std::array<Api::ProgramFamilyContribution, kProgramFamilies> programFamilies{};
			// Rows are sorted by row at the end.
			std::vector<FrameConstantRowEntry> frameConstantRows;
			std::vector<FrameTextureEntry> frameTextures;
			std::array<std::uint8_t, kTextureRegisters> frameTextureIndices = [] {
				std::array<std::uint8_t, kTextureRegisters> indices{};
				indices.fill(kNoFrameTexture);
				return indices;
			}();
			std::vector<MaterialContributorEntry> materialContributors;
			std::uint32_t materialTextureRegisters = 0, materialWitnessWords = 0;
			LightingJoinRules lightingJoinRules;
			// Per shader stage, the per-draw constant buffer registers, Core's and the declared ones.
			std::array<std::uint32_t, 2> perDrawMasks{ kCorePerDrawVS, kCorePerDrawPS };
			bool installed = false;
		};

		/** @brief The material contributor registered so far that binds a_register, or null. */
		const MaterialContributorEntry* MaterialRegisterAt(const Lists& a_lists, std::uint32_t a_register)
		{
			for (const auto& contributor : a_lists.materialContributors)
				if (std::ranges::find(contributor.registers, a_register) != contributor.registers.end())
					return &contributor;
			return nullptr;
		}

		/** @brief Adds a_range by its slot. A held group's range passes no object class. */
		void AddMainRange(Lists& a_lists, std::string_view a_who, const Api::MainRangeContribution& a_range, bool a_heldGroup)
		{
			const bool classValid = a_heldGroup ? a_range.objectClass == Api::ObjectClass::Count :
			                                      a_range.objectClass < Api::ObjectClass::Count && a_lists.objectClasses[static_cast<std::size_t>(a_range.objectClass)];
			if (a_range.slot >= Api::MainRangeSlot::Count || a_range.name.empty() || !classValid ||
				(a_range.depthPrepass && a_range.slot >= Api::MainRangeSlot::Appended0))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete main range", a_who));
			for (const auto& other : a_lists.mainRanges)
				if (other.slot != Api::MainRangeSlot::Count && other.order == a_range.order)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s main range {} takes order {}, which {} has", a_who, a_range.name, static_cast<std::uint32_t>(a_range.order), other.name));
			auto& entry = a_lists.mainRanges[static_cast<std::size_t>(a_range.slot)];
			if (entry.slot != Api::MainRangeSlot::Count)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes main range slot {}, which another registration has", a_who,
					static_cast<std::uint32_t>(a_range.slot)));
			entry = a_range;
		}

		/** @brief Adds a_group and its main range. */
		void AddHeldGroup(Lists& a_lists, std::string_view a_who, const Api::HeldGroupContribution& a_group)
		{
			if (a_group.slot < Api::MainRangeSlot::Appended0 || a_group.slot >= Api::MainRangeSlot::Count || a_group.name.empty() || !a_group.propertyFlags ||
				a_group.heldWhile.index >= a_lists.classifyInputs.size())
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete held group", a_who));
			AddMainRange(a_lists, a_who, { .slot = a_group.slot, .name = a_group.name, .order = a_group.order }, true);
			const auto group = static_cast<std::uint32_t>(a_group.slot) - static_cast<std::uint32_t>(Api::MainRangeSlot::Appended0) + 1;
			a_lists.heldGroups.push_back({ a_group.heldWhile, group, a_group.propertyFlags });
		}

		/** @brief Sets the one near twin. */
		void SetNearTwin(Lists& a_lists, std::string_view a_who, const Api::NearTwinContribution& a_twin)
		{
			if (a_twin.name.empty() || a_twin.debugName.empty() || a_twin.resource.empty() || !a_twin.target || !a_twin.nearTest)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete near twin", a_who));
			if (a_lists.nearTwin.target)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered near twin {}, which {} has", a_who, a_twin.name, a_lists.nearTwin.name));
			const std::string name(a_twin.name);
			a_lists.nearTwin = { name, std::string(a_twin.debugName), std::string(a_twin.resource), name + " cannot be described", name + " could not be imported",
				a_twin.target, a_twin.nearTest, a_twin.graph };
		}

		/** @brief Adds a_port at the next material kind and returns that kind. */
		std::uint8_t AddMaterialPort(Lists& a_lists, std::string_view a_who, const Api::MaterialPortContribution& a_port)
		{
			const auto spaceValid = [](const Api::MaterialStageSpace& a_stage) {
				return a_stage.variables <= kMaxStageVariables && (a_stage.names.empty() || a_stage.names.size() == a_stage.variables);
			};
			const bool classValid = a_port.objectClass < Api::ObjectClass::Count && a_lists.objectClasses[static_cast<std::size_t>(a_port.objectClass)];
			if (a_port.shaderType == RE::BSShader::Type::Lighting || a_port.shaderType >= RE::BSShader::Type::Total || !classValid || a_port.name.empty() || !a_port.propertyRtti ||
				!a_port.materialVtable || !a_port.capture || !a_port.derive || !a_port.evaluate || !a_port.shading || !spaceValid(a_port.vs) || !spaceValid(a_port.ps))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete material port", a_who));
			// Each frame variable must name a variable of its stage and read only bytes its material has.
			const auto readable = [&](const std::array<Api::MaterialFrameComponent, 4>& a_components) {
				return std::all_of(a_components.begin(), a_components.end(), [&](const Api::MaterialFrameComponent& a_component) {
					return a_component.source + 4u <= a_port.materialBytes &&
					       (a_component.scale == Api::MaterialFrameComponent::kNoScale || a_component.scale + 4u <= a_port.materialBytes);
				});
			};
			if (a_port.frameVariables.size() > Api::kMaxMaterialFrameVariables)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material port declares more than kMaxMaterialFrameVariables frame variables: grow it first", a_who));
			for (const auto& variable : a_port.frameVariables)
				if (variable.variable >= (variable.pixel ? a_port.ps.variables : a_port.vs.variables) || variable.first >= variable.end ||
					variable.end > a_port.materialBytes || !readable(variable.components) || !readable(variable.alternate))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material port declares a frame variable outside its space or material", a_who));
			for (std::size_t kind = 1; kind < a_lists.materialPortCount; ++kind)
				if (a_lists.materialPorts[kind].shaderType == a_port.shaderType)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material port takes shader type {}, which another port has", a_who,
						static_cast<std::uint32_t>(a_port.shaderType)));
			if (a_lists.materialPortCount >= kMaterialKinds)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material port is one past kMaterialKinds: grow it first", a_who));
			a_lists.materialPorts[a_lists.materialPortCount] = a_port;
			return static_cast<std::uint8_t>(a_lists.materialPortCount++);
		}

		/** @brief Whether a draw family or program family registered so far builds from a_shaderType. */
		bool ProgramShaderTaken(const Lists& a_lists, std::uint8_t a_shaderType)
		{
			for (const auto& family : a_lists.drawFamilies)
				if (family.family != Api::DrawFamily::Count && family.program.shaderType == a_shaderType)
					return true;
			for (const auto& family : a_lists.programFamilies)
				if (family.family != Api::ProgramFamily::Count && family.shaderType == a_shaderType)
					return true;
			return false;
		}

		/** @brief Whether a_root already has an owner. */
		bool ViewRootTaken(const Lists& a_lists, Api::ViewRoot a_root)
		{
			if (a_lists.viewRoots[static_cast<std::size_t>(a_root)].root != Api::ViewRoot::Count)
				return true;
			for (const auto& family : a_lists.drawFamilies)
				if (family.family != Api::DrawFamily::Count && family.faces.root == a_root)
					return true;
			return false;
		}

		/** @brief Whether a_faces are all set or all unset. */
		bool DrawFamilyFacesComplete(const Api::DrawFamilyContribution::Faces& a_faces)
		{
			const int callbacks = (a_faces.declare ? 1 : 0) + (a_faces.record ? 1 : 0) + (a_faces.row ? 1 : 0) + (a_faces.lists ? 1 : 0) + (a_faces.face ? 1 : 0);
			const int data = (a_faces.rowBytes ? 1 : 0) + (a_faces.headerBytes ? 1 : 0) + (a_faces.unitWords ? 1 : 0) + (a_faces.cullGroup ? 1 : 0) +
			                 (a_faces.rowName.empty() ? 0 : 1) + (a_faces.listName.empty() ? 0 : 1) + (a_faces.root < Api::ViewRoot::Count ? 1 : 0);
			return callbacks == 0 ? data == 0 : callbacks == 5 && data == 7;
		}

		/** @brief Adds a_family by its family. */
		void AddProgramFamily(Lists& a_lists, std::string_view a_who, const Api::ProgramFamilyContribution& a_family)
		{
			if (a_family.family >= Api::ProgramFamily::Count || a_family.shaderType == RE::BSShader::Type::Lighting || a_family.shaderType >= RE::BSShader::Type::Total ||
				a_family.name.empty() || a_family.keyName.empty() || a_family.sourcePath.empty() || !a_family.programSource)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete program family", a_who));
			auto& entry = a_lists.programFamilies[static_cast<std::size_t>(a_family.family)];
			if (entry.family != Api::ProgramFamily::Count)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes program family {}, which {} has", a_who, a_family.name, entry.name));
			if (ProgramShaderTaken(a_lists, a_family.shaderType))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s program family {} takes shader type {}, which another family has", a_who, a_family.name,
					static_cast<std::uint32_t>(a_family.shaderType)));
			entry = a_family;
		}

		/** @brief Adds a_row. */
		void AddFrameConstantRow(Lists& a_lists, std::string_view a_who, const Api::FrameConstantRow& a_row)
		{
			if (a_row.name.empty() || !a_row.sample || a_row.row < kFrameConstantRowFirst || a_row.row >= kFrameConstantRowsEnd)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete frame constant row", a_who));
			for (const auto& row : a_lists.frameConstantRows)
				if (row.row == a_row.row)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame constant row {} takes c{}, which {} has", a_who, a_row.name, a_row.row, row.name));
			a_lists.frameConstantRows.push_back({ std::string(a_row.name), a_row.row, a_row.sample });
		}

		/** @brief The frame graph buffers registered so far that read a_register, or null. */
		const FrameGraphBuffersEntry* GraphBuffersAt(const Lists& a_lists, std::uint32_t a_register)
		{
			for (const auto& buffers : a_lists.frameGraphBuffers)
				if (a_register >= buffers.firstRegister && a_register - buffers.firstRegister < buffers.count)
					return &buffers;
			return nullptr;
		}

		/** @brief Adds a_buffers. */
		void AddFrameGraphBuffers(Lists& a_lists, std::string_view a_who, const Api::FrameGraphBuffers& a_buffers)
		{
			std::uint32_t count = 0;
			while (count < a_buffers.resources.size() && !a_buffers.resources[count].empty())
				++count;
			bool gap = false;
			for (std::size_t r = count; r < a_buffers.resources.size(); ++r)
				gap = gap || !a_buffers.resources[r].empty();
			const auto first = a_buffers.firstRegister;
			if (a_buffers.name.empty() || !a_buffers.indices || !count || gap || first < kPixelTextureSlots || first >= kTextureRegisters ||
				count > kTextureRegisters - first)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete frame graph buffers", a_who));
			for (std::uint32_t r = first; r < first + count; ++r) {
				if (CoreFrameRegister(r))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame graph buffers {} take t{}, which Core keeps", a_who, a_buffers.name, r));
				for (const auto& binding : a_lists.frameBindings)
					if (binding.textureRegister == r)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r, binding.name));
				if (const auto texture = a_lists.frameTextureIndices[r]; texture != kNoFrameTexture)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r,
						a_lists.frameTextures[texture].name));
				if (const auto* other = GraphBuffersAt(a_lists, r))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r, other->name));
				if (const auto* contributor = MaterialRegisterAt(a_lists, r))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame graph buffers {} take t{}, which {} has", a_who, a_buffers.name, r, contributor->name));
			}
			// A commit keeps each registration's indices in a fixed array.
			if (a_lists.frameGraphBuffers.size() >= kFrameGraphBufferSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame graph buffers {} are one past kFrameGraphBufferSlots: grow it first", a_who, a_buffers.name));
			auto& entry = a_lists.frameGraphBuffers.emplace_back();
			entry.name = std::string(a_buffers.name);
			entry.firstRegister = first;
			entry.count = count;
			for (std::uint32_t r = 0; r < count; ++r)
				entry.resources[r] = std::string(a_buffers.resources[r]);
			entry.indices = a_buffers.indices;
		}

		/** @brief Adds a_texture. */
		void AddFrameTexture(Lists& a_lists, std::string_view a_who, const Api::FrameTexture& a_texture)
		{
			const auto r = a_texture.textureRegister;
			const bool graphIds = std::ranges::any_of(a_texture.graphResources, [](std::string_view a_id) { return !a_id.empty(); });
			if (a_texture.name.empty() || (!a_texture.view && !graphIds) || (a_texture.graphId && !graphIds) || r < kPixelTextureSlots || r >= kTextureRegisters)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete frame texture", a_who));
			if (CoreFrameRegister(r))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame texture {} takes t{}, which Core keeps", a_who, a_texture.name, r));
			for (const auto& binding : a_lists.frameBindings)
				if (binding.textureRegister == r)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, binding.name));
			if (const auto other = a_lists.frameTextureIndices[r]; other != kNoFrameTexture)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, a_lists.frameTextures[other].name));
			if (const auto* buffers = GraphBuffersAt(a_lists, r))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, buffers->name));
			if (const auto* contributor = MaterialRegisterAt(a_lists, r))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame texture {} takes t{}, which {} has", a_who, a_texture.name, r, contributor->name));
			// A colour commit samples every view into a fixed array.
			if (a_lists.frameTextures.size() >= kFrameTextureSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame texture {} is one past kFrameTextureSlots: grow it first", a_who, a_texture.name));
			a_lists.frameTextureIndices[r] = static_cast<std::uint8_t>(a_lists.frameTextures.size());
			a_lists.frameTextures.push_back({ std::string(a_texture.name), r, a_texture.view, a_texture.capturedWhenNull, a_texture.graphResources,
				a_texture.facesGraphResources, a_texture.graphId });
		}

		/** @brief Whether a_contributor has everything its stage needs and ascending registers within t16-t127. */
		bool MaterialContributorComplete(const Api::MaterialContributor& a_contributor)
		{
			const bool stageValid = a_contributor.stage == Api::MaterialStage::Replace ? a_contributor.takesPass && !a_contributor.covers :
			                        a_contributor.stage == Api::MaterialStage::After  ? a_contributor.covers && !a_contributor.takesPass :
			                                                                            false;
			const auto& materialClass = a_contributor.materialClass;
			const int classCallbacks = (materialClass.owns ? 1 : 0) + (materialClass.bytes ? 1 : 0) + (materialClass.textureFields ? 1 : 0);
			const auto registers = a_contributor.textureRegisters;
			bool registersValid = true;
			for (std::size_t i = 0; i < registers.size(); ++i)
				registersValid = registersValid && registers[i] >= kPixelTextureSlots && registers[i] < kTextureRegisters && (i == 0 || registers[i] > registers[i - 1]);
			return !a_contributor.name.empty() && a_contributor.sample && a_contributor.evaluate && stageValid && (classCallbacks == 0 || classCallbacks == 3) &&
			       registersValid;
		}

		/** @brief Adds a_contributor. Each contributor's registers take consecutive feature texture slots. */
		void AddMaterialContributor(Lists& a_lists, std::string_view a_who, const Api::MaterialContributor& a_contributor)
		{
			if (!MaterialContributorComplete(a_contributor))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete material contributor", a_who));
			const auto registers = a_contributor.textureRegisters;
			for (const std::uint32_t r : registers) {
				if (CoreFrameRegister(r))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} takes t{}, which Core keeps", a_who, a_contributor.name, r));
				for (const auto& binding : a_lists.frameBindings)
					if (binding.textureRegister == r)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} takes t{}, which {} has", a_who, a_contributor.name, r, binding.name));
				if (const auto texture = a_lists.frameTextureIndices[r]; texture != kNoFrameTexture)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} takes t{}, which {} has", a_who, a_contributor.name, r,
						a_lists.frameTextures[texture].name));
				if (const auto* buffers = GraphBuffersAt(a_lists, r))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} takes t{}, which {} has", a_who, a_contributor.name, r, buffers->name));
			}
			if (!registers.empty())
				for (const auto& other : a_lists.materialContributors)
					if (!other.registers.empty() && registers.front() <= other.registers.back() && other.registers.front() <= registers.back())
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} takes t{}-t{}, which overlap {}'s", a_who, a_contributor.name,
							registers.front(), registers.back(), other.name));
			// The feature texture slots are frozen by the row header and the HLSL decode.
			if (a_lists.materialTextureRegisters + registers.size() > kFeatureTextureSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} takes registers past kFeatureTextureSlots", a_who, a_contributor.name));
			if (a_lists.materialWitnessWords + a_contributor.witnessWords > kMaterialContributorWitnessWords)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} writes witness words past kMaterialContributorWitnessWords: grow it first",
					a_who, a_contributor.name));
			if (a_lists.materialContributors.size() >= kMaterialContributorSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s material contributor {} is one past kMaterialContributorSlots: grow it first", a_who,
					a_contributor.name));
			a_lists.materialTextureRegisters += static_cast<std::uint32_t>(registers.size());
			a_lists.materialWitnessWords += a_contributor.witnessWords;
			auto& entry = a_lists.materialContributors.emplace_back();
			entry.name = std::string(a_contributor.name);
			entry.stage = a_contributor.stage;
			entry.order = a_contributor.order;
			entry.registers = registers;
			entry.witnessWords = a_contributor.witnessWords;
			entry.signatureBits = a_contributor.signatureBits;
			entry.materialClass = a_contributor.materialClass;
			entry.sample = a_contributor.sample;
			entry.takesPass = a_contributor.takesPass;
			entry.covers = a_contributor.covers;
			entry.evaluate = a_contributor.evaluate;
		}

		/** @brief Adds a_fact as the next fact bit and returns it. */
		Api::ReferenceFactId AddReferenceFact(Lists& a_lists, std::string_view a_who, const Api::ReferenceFact& a_fact)
		{
			auto& rules = a_lists.lightingJoinRules;
			if (a_fact.name.empty() || !a_fact.sample)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete reference fact", a_who));
			if (rules.factCount >= kReferenceFactSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s reference fact {} is one past kReferenceFactSlots: grow it first", a_who, a_fact.name));
			const std::uint32_t index = rules.factCount++;
			rules.facts[index] = { std::string(a_fact.name), a_fact.none, a_fact.sample };
			rules.factsNone |= a_fact.none ? 1u << index : 0u;
			return { index };
		}

		/** @brief Adds a_bits. */
		void AddPropertyBits(Lists& a_lists, std::string_view a_who, const Api::PropertyBits& a_bits)
		{
			auto& rules = a_lists.lightingJoinRules;
			if (a_bits.name.empty() || !a_bits.bits || !a_bits.capture)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete property bits", a_who));
			if (a_bits.bits & rules.propertyBitsTaken)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s property bits {} ({:#x}) take bits another registration has", a_who, a_bits.name, a_bits.bits));
			if (rules.propertyBitsCount >= kPropertyBitsSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s property bits {} are one past kPropertyBitsSlots: grow it first", a_who, a_bits.name));
			rules.propertyBits[rules.propertyBitsCount++] = { std::string(a_bits.name), a_bits.bits, a_bits.capture };
			rules.propertyBitsTaken = static_cast<std::uint8_t>(rules.propertyBitsTaken | a_bits.bits);
		}

		/** @brief Adds a_flag. */
		void AddLightingObjectFlag(Lists& a_lists, std::string_view a_who, const Api::LightingObjectFlag& a_flag)
		{
			auto& rules = a_lists.lightingJoinRules;
			if (!std::has_single_bit(a_flag.objectFlag) || !std::has_single_bit(a_flag.recordFlag) || !a_flag.applies)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete Lighting object flag", a_who));
			for (std::uint32_t i = 0; i < rules.objectFlagCount; ++i)
				if (rules.objectFlags[i].objectFlag == a_flag.objectFlag || rules.objectFlags[i].recordFlag == a_flag.recordFlag)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s Lighting object flag {:#x} (record flag {:#x}) is another registration's", a_who,
						a_flag.objectFlag, a_flag.recordFlag));
			if (rules.objectFlagCount >= kLightingObjectFlagSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s Lighting object flag {:#x} is one past kLightingObjectFlagSlots: grow it first", a_who,
					a_flag.objectFlag));
			rules.objectFlags[rules.objectFlagCount++] = a_flag;
		}

		/** @brief Adds a_rules' masks and derivePass. */
		void AddLightingPassRules(Lists& a_lists, std::string_view a_who, const Api::LightingPassRules& a_rules)
		{
			auto& rules = a_lists.lightingJoinRules;
			auto& masks = rules.passMasks;
			const bool empty = !a_rules.pixelKeep && !a_rules.rawWhen && !a_rules.rawSet && !a_rules.rawFlip && !a_rules.derivePass;
			if (empty || (a_rules.pixelKeep & ~kLightingPixelSplitBits) || ((a_rules.rawSet || a_rules.rawFlip) && !a_rules.rawWhen))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete Lighting pass rules", a_who));
			// SetupGeometry's raw technique takes only one rule.
			if (a_rules.rawWhen && masks.rawWhen)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s Lighting pass rules give a raw technique rule, which another registration has", a_who));
			if (a_rules.derivePass && rules.deriveCount >= kLightingPassDeriveSlots)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s Lighting pass rules' derivePass is one past kLightingPassDeriveSlots: grow it first", a_who));
			masks.pixelKeep |= a_rules.pixelKeep;
			if (a_rules.rawWhen)
				masks = { masks.pixelKeep, a_rules.rawWhen, a_rules.rawSet, a_rules.rawFlip };
			if (a_rules.derivePass)
				rules.derives[rules.deriveCount++] = a_rules.derivePass;
		}

		/** @brief Adds a_buffer's register to its stage's per-draw mask. */
		void AddPerDrawBuffer(Lists& a_lists, std::string_view a_who, const Api::PerDrawBuffer& a_buffer)
		{
			const auto stage = static_cast<std::size_t>(a_buffer.stage);
			if (a_buffer.name.empty() || stage >= a_lists.perDrawMasks.size() || a_buffer.constantRegister >= kConstantBufferRegisters)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} declared an incomplete per-draw buffer", a_who));
			const std::uint32_t bit = 1u << a_buffer.constantRegister;
			if (CorePerDrawMask(a_buffer.stage) & bit)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s per-draw buffer {} takes b{}, which Core binds per draw", a_who, a_buffer.name, a_buffer.constantRegister));
			if (a_lists.perDrawMasks[stage] & bit)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s per-draw buffer {} takes b{}, which another declaration has", a_who, a_buffer.name,
					a_buffer.constantRegister));
			a_lists.perDrawMasks[stage] |= bit;
		}

		Lists& Registered()
		{
			static Lists lists;
			return lists;
		}

		class Context final : public Api::InstallContext
		{
		public:
			explicit Context(Lists& a_lists) :
				lists(a_lists) {}

			Api::FrameValuePoolId RegisterFrameValuePool(const Api::FrameValuePool& a_pool) override
			{
				if (a_pool.name.empty() || !a_pool.stride || !a_pool.kick || !a_pool.sample || !a_pool.send)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete frame-value pool", feature));
				lists.pools.push_back({ std::string(a_pool.name), a_pool.stride, a_pool.deviceAddress, a_pool.unorderedAccess, a_pool.frameStart, a_pool.kick, a_pool.sample, a_pool.send });
				return { static_cast<std::uint32_t>(lists.pools.size() - 1) };
			}

			void RegisterPass(const Api::PassDef& a_pass) override { Draws::RegisterPassDef(a_pass, feature); }
			Api::TriggerId FindTrigger(std::string_view a_id) override { return Draws::FindTrigger(*schedule, a_id, feature); }

			void RegisterPayloadProduct(const Api::PayloadProduct& a_product) override
			{
				if (a_product.name.empty() || !a_product.build || !a_product.install)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete payload product", feature));
				lists.products.push_back({ std::string(a_product.name), a_product.build, a_product.carried, a_product.install });
			}

			Api::SceneResourcesId RegisterSceneResources(const Api::SceneResources& a_resources) override
			{
				if (a_resources.name.empty() || !a_resources.setup || !a_resources.postInputs != !a_resources.inputWords)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete scene resources", feature));
				if (lists.sceneResourceInputs + a_resources.inputWords > kSceneResourceInputWords)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s scene resources {} post words past kSceneResourceInputWords: grow it first", feature, a_resources.name));
				lists.sceneResources.push_back({ std::string(a_resources.name), a_resources.setup, a_resources.registerGraph, a_resources.require, a_resources.fit,
					a_resources.postInputs, lists.sceneResourceInputs, a_resources.inputWords, a_resources.latched, a_resources.sealed });
				lists.sceneResourceInputs += a_resources.inputWords;
				return { static_cast<std::uint32_t>(lists.sceneResources.size() - 1) };
			}

			void RegisterCommitHook(const Api::CommitHook& a_hook) override
			{
				if (a_hook.name.empty() || !a_hook.commit)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete commit hook", feature));
				lists.commitHooks.push_back({ std::string(a_hook.name), a_hook.commit });
			}

			void RegisterFrameBinding(const Api::FrameBinding& a_binding) override
			{
				if (a_binding.name.empty() || !a_binding.select || a_binding.textureRegister >= kTextureRegisters)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete frame binding", feature));
				if (CoreFrameRegister(a_binding.textureRegister))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame binding {} takes t{}, which Core keeps", feature, a_binding.name, a_binding.textureRegister));
				for (const auto& binding : lists.frameBindings)
					if (binding.textureRegister == a_binding.textureRegister)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
							binding.name));
				if (const auto texture = lists.frameTextureIndices[a_binding.textureRegister]; texture != kNoFrameTexture)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
						lists.frameTextures[texture].name));
				if (const auto* buffers = GraphBuffersAt(lists, a_binding.textureRegister))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
						buffers->name));
				if (const auto* contributor = MaterialRegisterAt(lists, a_binding.textureRegister))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame binding {} takes t{}, which {} has", feature, a_binding.name, a_binding.textureRegister,
						contributor->name));
				if (lists.frameBindings.size() >= kFrameBindingSlots)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame binding {} is one past kFrameBindingSlots: grow it first", feature, a_binding.name));
				lists.frameBindings.push_back({ std::string(a_binding.name), a_binding.textureRegister, a_binding.select });
			}

			Api::SwitchId RegisterSwitch(const Api::FeatureSwitch& a_switch) override
			{
				return DCLF::RegisterSwitch(a_switch, feature);
			}

			Api::ClassifyInputId RegisterClassifyInput(const Api::ClassifyInputContribution& a_input) override
			{
				if (a_input.name.empty() || !a_input.sample)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete classify input", feature));
				if (lists.classifyInputs.size() >= kClassifyInputSlots)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s classify input {} is one past kClassifyInputSlots: grow it first", feature, a_input.name));
				for (const auto& input : lists.classifyInputs)
					if (input.name == a_input.name)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s classify input {} has a name another input has", feature, a_input.name));
				lists.classifyInputs.push_back({ std::string(a_input.name), a_input.sample });
				return { static_cast<std::uint32_t>(lists.classifyInputs.size() - 1) };
			}

			Api::ClassifyInputId NeedClassifyInput(std::string_view a_name) override
			{
				if (a_name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} needs a classify input without a name", feature));
				// A name needed twice counts as one need.
				std::size_t slot = 0;
				while (slot < lists.classifyInputNeeds.size() && lists.classifyInputNeeds[slot] != a_name)
					++slot;
				if (slot == lists.classifyInputNeeds.size()) {
					if (slot >= kClassifyInputNeedSlots)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s need of classify input {} is one past kClassifyInputNeedSlots: grow it first", feature, a_name));
					lists.classifyInputNeeds.emplace_back(a_name);
				}
				return { kClassifyInputNeedBit | static_cast<std::uint32_t>(slot) };
			}

			void RegisterFrameGraphBuffers(const Api::FrameGraphBuffers& a_buffers) override { AddFrameGraphBuffers(lists, feature, a_buffers); }

			void RegisterLightVolumes(const Api::LightVolumesContribution& a_volumes) override
			{
				if (a_volumes.name.empty() || !a_volumes.sample)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete light volumes contribution", feature));
				if (!lists.lightVolumes.name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes light volumes {}, which {} has", feature, a_volumes.name, lists.lightVolumes.name));
				lists.lightVolumes = a_volumes;
			}

			void RegisterSunVolume(const Api::SunVolumeContribution& a_volume) override
			{
				if (a_volume.name.empty() || !a_volume.cascades || !a_volume.entryPlanes)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete sun volume contribution", feature));
				if (!lists.sunVolume.name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes sun volume {}, which {} has", feature, a_volume.name, lists.sunVolume.name));
				lists.sunVolume = a_volume;
			}

			void RegisterAncestorValue(const Api::AncestorValueContribution& a_value) override
			{
				// Its input must be registered first, so an integration registers its own input before this.
				if (a_value.name.empty() || (!a_value.ancestorRtti[0] && !a_value.ancestorRtti[1]) || !a_value.generation || !a_value.fill ||
					a_value.whileOn.index >= lists.classifyInputs.size())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete ancestor value", feature));
				if (!lists.ancestorValue.name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes ancestor value {}, which {} has", feature, a_value.name, lists.ancestorValue.name));
				lists.ancestorValue = a_value;
			}

			void RegisterLatePose(const Api::LatePoseContribution& a_pose) override
			{
				if (a_pose.name.empty() || !a_pose.root)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete late pose", feature));
				for (const auto& other : lists.latePoses)
					if (other.name == a_pose.name)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered late pose {} twice", feature, a_pose.name));
				lists.latePoses.push_back(a_pose);
			}

			void RegisterReportLines(const Api::ReportLines& a_lines) override
			{
				// Points that compose text take text lines. The others take logged lines.
				const auto point = a_lines.point;
				const bool text = Api::ReportPointComposesText(point);
				// A component's lines replace text or lines at a coordinator point, and the component must be registered first.
				const bool component = Api::ReportPointTakesComponents(point) && a_lines.componentText;
				if (component && (!a_lines.component || a_lines.component.index >= lists.coordinatorComponents.size()))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s report lines {} name no registered component", feature, a_lines.name));
				if (a_lines.name.empty() || (component ? a_lines.text || a_lines.lines :
				                                        text ? !a_lines.text || a_lines.lines || a_lines.componentText : !a_lines.lines || a_lines.text || a_lines.componentText))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete report lines", feature));
				lists.reportLines.push_back({ std::string(a_lines.name), a_lines.point, a_lines.lines, a_lines.text, a_lines.component, a_lines.componentText });
			}

			bool RegisterStandInObserver(const Api::StandInObserver& a_observer) override
			{
				// Without the primary cull nothing stands in, so nothing would call the observer.
				if (!StandInInstalled())
					return false;
				lists.standInObservers.push_back(a_observer);
				return true;
			}

			Api::EngineChannelId RegisterEngineChannel(const Api::EngineChannel& a_channel) override
			{
				// A SceneStack channel's events live on the scene's stack, so there is no queue to drain.
				if (a_channel.name.empty() || (a_channel.kind != Api::ChannelKind::SceneStack && !a_channel.drain))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete engine channel", feature));
				if (FindEngineChannel(a_channel.name))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s engine channel {} has a name another channel has", feature, a_channel.name));
				if (lists.engineChannels.size() >= kEngineChannelSlots)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s engine channel {} is one past kEngineChannelSlots: grow it first", feature, a_channel.name));
				lists.engineChannels.push_back({ std::string(a_channel.name), a_channel.kind, a_channel.apply, a_channel.wake, a_channel.namesKeys,
					a_channel.drain, a_channel.push });
				return { static_cast<std::uint32_t>(lists.engineChannels.size() - 1) };
			}

			Api::EngineChannelId NeedEngineChannel(std::string_view a_name) override
			{
				if (a_name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} needs an engine channel without a name", feature));
				// A name needed twice counts as one need.
				std::size_t slot = 0;
				while (slot < lists.engineChannelNeeds.size() && lists.engineChannelNeeds[slot] != a_name)
					++slot;
				if (slot == lists.engineChannelNeeds.size()) {
					if (slot >= kEngineChannelNeedSlots)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s need of engine channel {} is one past kEngineChannelNeedSlots: grow it first", feature, a_name));
					lists.engineChannelNeeds.emplace_back(a_name);
				}
				return { kEngineChannelNeedBit | static_cast<std::uint32_t>(slot) };
			}

			void RegisterShowVeto(const Api::ShowVeto& a_veto) override
			{
				if (!a_veto.open || !a_veto.vetoes)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete show veto", feature));
				// The stubs load only one veto's cells.
				if (std::exchange(lists.showVetoRegistered, true))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered a show veto, which another registration has", feature));
				lists.showVeto = { a_veto.open, a_veto.vetoes };
			}

			std::uint32_t ReserveHiddenSite() override { return HiddenSourceSites() + lists.reservedHiddenSites++; }
			void RegisterNodeCapture(const Api::NodeCaptureContribution& a_capture) override { AddNodeCapture(feature, a_capture); }
			Api::MoveWriterId ReserveMoveWriter(std::string_view a_name) override { return Extensions::ReserveMoveWriter(a_name); }
			void RegisterFrameVSVariablePatch() override { NoteFrameVSVariablePatch(); }

			Api::EngineChannelId FindEngineChannel(std::string_view a_name) const override
			{
				for (std::uint32_t c = 0; c < lists.engineChannels.size(); ++c)
					if (lists.engineChannels[c].name == a_name)
						return { c };
				return {};
			}

			Api::TableFamilyId RegisterTableFamily(const Api::TableFamily& a_family) override
			{
				if (a_family.name.empty() || !a_family.make || !a_family.assign || !a_family.clear || !a_family.recycle)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete table family", feature));
				if (lists.tableFamilies.size() >= kTableFamilySlots)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s table family {} is one past kTableFamilySlots: grow it first", feature, a_family.name));
				// The row needs a bit above its shift, and a shifted or biased row needs a record word to ride in.
				if (a_family.recordShift >= 32 || ((a_family.recordShift || a_family.recordBias || !a_family.recordTag) && a_family.recordWord == ~0u))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s table family {} packs its record reference past the word, or into none", feature, a_family.name));
				lists.tableFamilies.push_back({ std::string(a_family.name), a_family.make, a_family.assign, a_family.assignRow, a_family.same, a_family.clear, a_family.recycle, a_family.rowsEnd,
					a_family.recordWord, a_family.recordShift, a_family.recordBias, a_family.recordTag, a_family.journaled, a_family.rows });
				return { static_cast<std::uint32_t>(lists.tableFamilies.size() - 1) };
			}

			Api::CoordinatorComponentId RegisterCoordinatorComponent(const Api::CoordinatorComponent& a_component) override
			{
				const auto& followed = a_component.entryClass;
				if (a_component.name.empty() || !a_component.make || followed.follows >= Api::EntryClass::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete coordinator component", feature));
				// Only one component may follow an entry class.
				if (followed.follows != Api::EntryClass::None)
					for (const auto& component : lists.coordinatorComponents)
						if (component.entryClass.follows == followed.follows)
							::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s component {} follows entry class {}, which {} follows", feature, a_component.name,
								static_cast<std::uint32_t>(followed.follows), component.name));
				lists.coordinatorComponents.push_back({ std::string(a_component.name), a_component.make, a_component.slots, a_component.residency, a_component.accumulate, a_component.walk,
					a_component.retirement, followed, a_component.events, a_component.commit, a_component.adoption });
				if (followed.follows != Api::EntryClass::None)
					lists.entryClassesTaken[static_cast<std::size_t>(followed.follows)] = followed.takes;
				return { static_cast<std::uint32_t>(lists.coordinatorComponents.size() - 1) };
			}

			void RegisterTechnique(const Api::TechniqueContribution& a_technique) override
			{
				// A property flag rule must name the geometry type its flagged objects must be.
				if (a_technique.technique >= kTechniqueSlots || (a_technique.propertyFlags && a_technique.geometryType >= kGeometryTypeSlots))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete technique contribution", feature));
				auto& entry = lists.techniques[a_technique.technique];
				if (entry.taken)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes technique {}, which another feature has", feature, a_technique.technique));
				entry = { true, a_technique.objectFlags, a_technique.extras };
				// Techniques that share a rule, like LODLand and LODLandNoise, add it once.
				if (a_technique.propertyFlags) {
					const PropertyFlagRule rule{ a_technique.propertyFlags, a_technique.geometryType };
					const auto rules = std::span(lists.propertyFlagRules).first(lists.propertyFlagRuleCount);
					const bool known = std::ranges::any_of(rules, [&rule](const PropertyFlagRule& a_rule) {
						return a_rule.propertyFlags == rule.propertyFlags && a_rule.geometryType == rule.geometryType;
					});
					if (!known)
						lists.propertyFlagRules[lists.propertyFlagRuleCount++] = rule;
				}
			}

			void RegisterGeometryType(const Api::GeometryTypeContribution& a_type) override
			{
				// A type is ported unless the contribution only names an entry class, which needs its parent kinds.
				const auto& entry = a_type.entry;
				const bool entryValid = entry.entryClass < Api::EntryClass::Count &&
				                        (entry.entryClass == Api::EntryClass::None ? !entry.parentKind && !entry.perFrame && !entry.holdsParent : entry.parentKind != 0);
				const bool classValid = a_type.ported ||
				                        (!a_type.propertyFlags && a_type.itemClass == Api::FrameValueItemClass::None && !a_type.moves && !a_type.ranges &&
				                            entry.entryClass != Api::EntryClass::None);
				if (a_type.geometryType >= kGeometryTypeSlots || a_type.itemClass >= Api::FrameValueItemClass::Count || !entryValid || !classValid)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete geometry type contribution", feature));
				auto& contribution = lists.geometryTypeContributions[a_type.geometryType];
				if (contribution.geometryType != 0xFF)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes geometry type {}, which another feature has", feature, static_cast<std::uint32_t>(a_type.geometryType)));
				contribution = a_type;
				lists.geometryTypes[a_type.geometryType] = a_type.ported;
				// One rule per entry class. A second type naming it must name it the same way.
				if (entry.entryClass != Api::EntryClass::None) {
					if (lists.ownerClassRegistered[static_cast<std::size_t>(entry.entryClass)])
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s geometry type {} names entry class {}, an owner class", feature,
							static_cast<std::uint32_t>(a_type.geometryType), static_cast<std::uint32_t>(entry.entryClass)));
					auto& rule = lists.entryClassRules[static_cast<std::size_t>(entry.entryClass)];
					if (rule.entryClass != Api::EntryClass::None && (rule.perFrame != entry.perFrame || rule.holdsParent != entry.holdsParent))
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s geometry type {} names entry class {} unlike another contribution", feature,
							static_cast<std::uint32_t>(a_type.geometryType), static_cast<std::uint32_t>(entry.entryClass)));
					rule = entry;
				}
			}

			void RegisterParent(const Api::ParentContribution& a_parent) override
			{
				if (a_parent.parent >= Api::EngineParent::Count || a_parent.leaves >= Api::ParentLeaves::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete parent contribution", feature));
				auto& taken = lists.parents[static_cast<std::size_t>(a_parent.parent)][static_cast<std::size_t>(a_parent.leaves)];
				if (taken)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes parent {} leaves {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_parent.parent), static_cast<std::uint32_t>(a_parent.leaves)));
				taken = true;
			}

			void RegisterObjectClass(const Api::ObjectClassContribution& a_class) override
			{
				if (a_class.objectClass >= Api::ObjectClass::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete object class contribution", feature));
				auto& taken = lists.objectClasses[static_cast<std::size_t>(a_class.objectClass)];
				if (taken)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes object class {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_class.objectClass)));
				taken = true;
				lists.categoryRoots[static_cast<std::size_t>(a_class.objectClass)] = a_class.categoryRoot;
				lists.categoryRootGathers[static_cast<std::size_t>(a_class.objectClass)] = a_class.categoryRoots;
			}

			void RegisterUnheldRoots(const Api::UnheldRootsContribution& a_roots) override
			{
				if (!a_roots.owner || !a_roots.enter || !a_roots.leave || !a_roots.gather)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete unheld roots contribution", feature));
				if (lists.unheldRoots.gather)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes unheld roots, which another feature has", feature));
				lists.unheldRoots = a_roots;
			}

			void RegisterEntryClass(const Api::EntryClassContribution& a_class) override
			{
				// ownMotion means no events, so it requires everyFrame.
				if (a_class.entryClass == Api::EntryClass::None || a_class.entryClass >= Api::EntryClass::Count || a_class.name.empty() ||
					(a_class.ownMotion && !a_class.everyFrame))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete entry class contribution", feature));
				const auto index = static_cast<std::size_t>(a_class.entryClass);
				auto& owner = lists.ownerClasses[a_class.ownerFormType];
				if (lists.ownerClassRegistered[index] || owner.ownerClass != Api::EntryClass::None ||
					lists.entryClassRules[index].entryClass != Api::EntryClass::None)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes entry class {} (form type {}), which another contribution has", feature,
						static_cast<std::uint32_t>(a_class.entryClass), static_cast<std::uint32_t>(a_class.ownerFormType)));
				const bool first = std::ranges::none_of(lists.ownerClassRegistered, std::identity{});
				lists.ownerClassRegistered[index] = true;
				owner = { a_class.entryClass, a_class.everyFrame, a_class.ownMotion, a_class.published };
				lists.ownedLabel = first ? std::string(a_class.name) : lists.ownedLabel + "/" + std::string(a_class.name);
				lists.ownedWithoutRecordLabel = lists.ownedLabel + " without a record";
			}

			void RegisterShading(const Api::ShadingContribution& a_shading) override
			{
				// Each variable must lie in the block and take at most a word, because the VS mask has one bit per variable.
				const auto blockValid = [&] {
					if (a_shading.blockWords > Api::kShadingBlockWords || (a_shading.blockWords && !a_shading.objectFlags) ||
						a_shading.vsVariables.size() > a_shading.blockWords)
						return false;
					for (const auto& variable : a_shading.vsVariables)
						if (variable.variable >= 64 || !variable.floats || variable.compared > variable.floats ||
							std::uint32_t(variable.firstWord) + variable.floats > a_shading.blockWords)
							return false;
					return true;
				};
				if (a_shading.name.empty() || !a_shading.propertyFlags || !a_shading.derive || !blockValid())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete shading contribution", feature));
				if (a_shading.blockWords)
					for (const auto& other : lists.shadingContributions)
						if (other.blockWords)
							::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s shading contribution {} claims the feature block, which {} has", feature,
								a_shading.name, other.name));
				lists.shadingContributions.push_back({ std::string(a_shading.name), a_shading.propertyFlags, a_shading.derive, a_shading.objectFlags,
					a_shading.blockWords, a_shading.vsVariables });
			}

			Api::FrameGlobalsSlot RegisterFrameGlobals(const Api::FrameGlobalsContribution& a_globals) override
			{
				if (a_globals.name.empty() || !a_globals.floats || !a_globals.capture)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete frame-globals contribution", feature));
				if (lists.frameGlobalsFloats + a_globals.floats > kFrameGlobalsFloats)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s frame globals {} are past kFrameGlobalsFloats: grow it first", feature, a_globals.name));
				const std::uint32_t first = lists.frameGlobalsFloats;
				lists.frameGlobals.push_back({ std::string(a_globals.name), first, a_globals.floats, a_globals.capture });
				lists.frameGlobalsFloats += a_globals.floats;
				return { first };
			}

			void RegisterView(const Api::ViewContribution& a_view) override
			{
				// The phase and view bit are checked against Core's and other views' once all are registered.
				if (a_view.view >= Api::EngineView::Count || !a_view.phase || !a_view.shape.viewBits || !a_view.covered ||
					(a_view.membership == Api::ViewMembership::CategoryRoot && !a_view.captureRoot) ||
					(a_view.withholding == Api::ViewWithholding::InMainRenderers && !a_view.accumulators))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete view contribution", feature));
				auto& entry = lists.views[static_cast<std::size_t>(a_view.view)];
				if (entry.view != Api::EngineView::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes view {}, which another feature has", feature, static_cast<std::uint32_t>(a_view.view)));
				entry = a_view;
			}

			void RegisterViewWork(const Api::ViewWork& a_work) override
			{
				if (a_work.view >= Api::EngineView::Count || a_work.point >= Api::ViewPoint::Count || !a_work.run)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete view work", feature));
				lists.viewWork.push_back(a_work);
			}

			void RegisterViewRoot(const Api::ViewRootContribution& a_root) override
			{
				if (a_root.root >= Api::ViewRoot::Count || !a_root.owned)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete view root contribution", feature));
				// One owner per root, because a view's owned roots are its owners' answers.
				if (ViewRootTaken(lists, a_root.root))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes view root {}, which another registration has", feature,
						static_cast<std::uint32_t>(a_root.root)));
				lists.viewRoots[static_cast<std::size_t>(a_root.root)] = a_root;
			}

			void RegisterCullHiddenSource(const Api::CullHiddenSource& a_source) override
			{
				if (!a_source.nodes || a_source.bit > Api::CullHiddenBit::Hidden)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete cull hidden source", feature));
				lists.cullHiddenSources.push_back(a_source);
			}

			void RegisterOcclusionMap(const Api::OcclusionMapContribution& a_map) override
			{
				// The bits are checked against Core's and other views' once all are registered.
				if (a_map.map >= Api::OcclusionMap::Count || !a_map.available || !a_map.bits.phase || !a_map.bits.viewBit ||
					!a_map.technique || !a_map.camera || (a_map.target == Api::OcclusionTarget::ImportedTexture && !a_map.importTarget))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete occlusion map contribution", feature));
				auto& entry = lists.occlusionMaps[static_cast<std::size_t>(a_map.map)];
				if (entry.map != Api::OcclusionMap::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes occlusion map {}, which another feature has", feature, static_cast<std::uint32_t>(a_map.map)));
				entry = a_map;
			}

			void RegisterShadowSource(const Api::ShadowSourceContribution& a_source) override
			{
				// Local light sources hear about each kept light. Only the Sun ends the full-frustum window.
				const bool local = a_source.source == Api::ShadowSource::Spot || a_source.source == Api::ShadowSource::Point;
				const bool sun = a_source.source == Api::ShadowSource::Sun;
				if (a_source.source >= Api::ShadowSource::Count || !a_source.camera || (local && !a_source.lightKept) || sun != !!a_source.fullFrustumWindowEnd)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete shadow source contribution", feature));
				auto& entry = lists.shadowSources[static_cast<std::size_t>(a_source.source)];
				if (entry.source != Api::ShadowSource::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes shadow source {}, which another feature has", feature,
						static_cast<std::uint32_t>(a_source.source)));
				entry = a_source;
			}

			void RegisterShadowExclusion(const Api::ShadowExclusionContribution& a_exclusion) override
			{
				if (a_exclusion.name.empty() || !a_exclusion.build || !a_exclusion.publish || !a_exclusion.frameStart || !a_exclusion.listExclusion)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete shadow exclusion contribution", feature));
				if (!lists.shadowExclusion.name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes shadow exclusion {}, which {} has", feature, a_exclusion.name,
						lists.shadowExclusion.name));
				lists.shadowExclusion = a_exclusion;
			}

			void RegisterDrawFamily(const Api::DrawFamilyContribution& a_family) override
			{
				// The faces' callbacks come all together or not at all.
				const auto& main = a_family.main;
				const auto& program = a_family.program;
				// One or two colour programs, a write mode of 0-12, and a forward program only with faces.
				const bool programValid = program.shaderType != RE::BSShader::Type::Lighting && program.shaderType < RE::BSShader::Type::Total && !program.name.empty() &&
				                          !program.label.empty() && !program.keyName.empty() && !program.sourcePath.empty() && a_family.engineShapes.propertyVtable &&
				                          (program.colours == 1 || (program.colours == 2 && program.descriptors.colour2)) && program.writeMode < 13 &&
				                          (a_family.faces.face || !program.descriptors.forward);
				if (a_family.family >= Api::DrawFamily::Count || a_family.objectClass >= Api::ObjectClass::Count ||
					!lists.objectClasses[static_cast<std::size_t>(a_family.objectClass)] || a_family.resources.index >= lists.sceneResources.size() ||
					!a_family.frame.decide || !a_family.frame.programSource || !main.declare || !main.revision || !main.record || !programValid ||
					!DrawFamilyFacesComplete(a_family.faces))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete draw family contribution", feature));
				auto& entry = lists.drawFamilies[static_cast<std::size_t>(a_family.family)];
				if (entry.family != Api::DrawFamily::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes draw family {}, which another registration has", feature,
						static_cast<std::uint32_t>(a_family.family)));
				// Programs are found by shader type, and the faces' root has one owner.
				if (ProgramShaderTaken(lists, program.shaderType))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s draw family {} takes shader type {}, which another family has", feature, program.name,
						static_cast<std::uint32_t>(program.shaderType)));
				if (a_family.faces.root < Api::ViewRoot::Count && ViewRootTaken(lists, a_family.faces.root))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s draw family {} draws view root {}, which another registration has", feature, program.name,
						static_cast<std::uint32_t>(a_family.faces.root)));
				entry = a_family;
			}

			void RegisterMainRange(const Api::MainRangeContribution& a_range) override { AddMainRange(lists, feature, a_range, false); }
			void RegisterHeldGroup(const Api::HeldGroupContribution& a_group) override { AddHeldGroup(lists, feature, a_group); }
			void RegisterNearTwin(const Api::NearTwinContribution& a_twin) override { SetNearTwin(lists, feature, a_twin); }

			void RegisterDeferredRange(const Api::DeferredRangeContribution& a_range) override
			{
				if (a_range.slot >= Api::DeferredRangeSlot::Count || a_range.name.empty() || a_range.family >= Api::ProgramFamily::Count || !a_range.draws ||
					!a_range.target)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete deferred range", feature));
				auto& entry = lists.deferredRanges[static_cast<std::size_t>(a_range.slot)];
				if (entry.slot != Api::DeferredRangeSlot::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes deferred range slot {}, which {} has", feature,
						static_cast<std::uint32_t>(a_range.slot), entry.name));
				entry = a_range;
			}

			void RegisterVertexStream(const Api::VertexStreamContribution& a_stream) override
			{
				// Its scene resources and the component following its entry class must be registered first.
				bool followed = false;
				for (const auto& component : lists.coordinatorComponents)
					followed = followed || (a_stream.entryClass != Api::EntryClass::None && component.entryClass.follows == a_stream.entryClass);
				if (a_stream.name.empty() || !a_stream.select || !a_stream.upload || a_stream.resources.index >= lists.sceneResources.size() || !followed)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete vertex stream", feature));
				if (lists.vertexStream.entryClass != Api::EntryClass::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes vertex stream {}, which {} has: Core's draws have one second stream", feature,
						a_stream.name, lists.vertexStream.name));
				lists.vertexStream = a_stream;
			}

			void RegisterPaletteRows(const Api::PaletteRowsContribution& a_rows) override
			{
				const auto& rule = a_rows.rule;
				const bool ruleValid = rule.rowsPerUnit && rule.minUnits && rule.minUnits <= rule.maxUnits &&
				                       (!rule.pow2 || (std::has_single_bit(rule.maxUnits) && std::has_single_bit(rule.octaveSteps)));
				if (a_rows.itemClass == Api::FrameValueItemClass::None || a_rows.itemClass >= Api::FrameValueItemClass::Count || a_rows.name.empty() || !ruleValid ||
					!a_rows.geometrySource || !a_rows.beginRun || !a_rows.sampleChunk || !a_rows.endRun)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete palette rows class", feature));
				auto& entry = lists.paletteRows[static_cast<std::size_t>(a_rows.itemClass) - 1];
				if (entry.itemClass != Api::FrameValueItemClass::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes palette rows class {}, which {} has", feature,
						static_cast<std::uint32_t>(a_rows.itemClass), entry.name));
				entry = a_rows;
			}

			std::uint8_t RegisterMaterialPort(const Api::MaterialPortContribution& a_port) override { return AddMaterialPort(lists, feature, a_port); }
			void RegisterProgramFamily(const Api::ProgramFamilyContribution& a_family) override { AddProgramFamily(lists, feature, a_family); }
			void RegisterFrameConstantRow(const Api::FrameConstantRow& a_row) override { AddFrameConstantRow(lists, feature, a_row); }
			void RegisterFrameTexture(const Api::FrameTexture& a_texture) override { AddFrameTexture(lists, feature, a_texture); }

			Api::ShadingFieldId RegisterShadingField(const Api::ShadingFieldContribution& a_field) override
			{
				if (a_field.field >= Api::ShadingRowField::Count || a_field.name.empty() || !a_field.capture ||
					(a_field.members != Api::EntryClass::None && !a_field.membership) || a_field.members >= Api::EntryClass::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete shading field", feature));
				if (std::exchange(lists.shadingFieldTaken[static_cast<std::size_t>(a_field.field)], true))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes shading field {} ({}), which another registration has", feature,
						a_field.name, static_cast<std::uint32_t>(a_field.field)));
				lists.shadingFields.push_back(a_field);
				return { static_cast<std::uint32_t>(lists.shadingFields.size() - 1) };
			}

			void RegisterMainCullInput(const Api::MainCullInputContribution& a_input) override
			{
				if (a_input.name.empty() || !a_input.unboundVisible || !a_input.structure || !a_input.programsVersion || !a_input.judge || !a_input.test)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete main cull input", feature));
				if (!lists.mainCullInput.name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes main cull input {}, which {} has", feature, a_input.name, lists.mainCullInput.name));
				lists.mainCullInput = a_input;
			}

			void RegisterCullBinding(const Api::CullBinding& a_binding) override
			{
				if (a_binding.slot >= Api::CullBindingSlot::Count || a_binding.name.empty() || !a_binding.select)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete cull binding", feature));
				auto& taken = lists.cullBindings[static_cast<std::size_t>(a_binding.slot)];
				if (taken.select)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s cull binding {} takes slot {}, which {} has", feature, a_binding.name,
						static_cast<std::uint32_t>(a_binding.slot), taken.name));
				taken = a_binding;
			}

			void RegisterDepthPyramid(const Api::DepthPyramidContribution& a_pyramid) override
			{
				if (a_pyramid.resource.empty() || a_pyramid.scratchResource.empty() || !a_pyramid.setup)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete depth pyramid", feature));
				if (lists.depthPyramid.setup)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes depth pyramid {}, which another feature has", feature, a_pyramid.resource));
				lists.depthPyramid = a_pyramid;
			}

			void RegisterPrimaryView(const Api::PrimaryViewContribution& a_view) override
			{
				if (!a_view.beforeOpaque || !a_view.afterOpaque)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete primary view", feature));
				if (lists.primaryView.beforeOpaque)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes a primary view, which {} has", feature, lists.primaryViewFeature));
				lists.primaryView = a_view;
				lists.primaryViewFeature = std::string(feature);
			}

			void RegisterShadowEpochs(const Api::ShadowEpochsContribution& a_epochs) override
			{
				if (!a_epochs.beforeShadowCasters || !a_epochs.beforeShadowMaps || !a_epochs.afterShadowMaps || !a_epochs.occlusionReady)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete shadow epochs", feature));
				if (lists.shadowEpochs.afterShadowMaps)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes the shadow epochs, which {} has", feature, lists.shadowEpochsFeature));
				lists.shadowEpochs = a_epochs;
				lists.shadowEpochsFeature = std::string(feature);
			}

			void RegisterRootStates(const Api::RootStatesContribution& a_states) override
			{
				// Its table family must be registered first, because its objects' references are the roots Core reads.
				if (a_states.name.empty() || !a_states.family || a_states.family.index >= lists.tableFamilies.size() || !a_states.view || !a_states.test)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete root states", feature));
				if (!lists.rootStates.name.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} contributes root states {}, which {} has", feature, a_states.name, lists.rootStates.name));
				lists.rootStates = a_states;
			}

			void RegisterMaterialContributor(const Api::MaterialContributor& a_contributor) override { AddMaterialContributor(lists, feature, a_contributor); }
			Api::ReferenceFactId RegisterReferenceFact(const Api::ReferenceFact& a_fact) override { return AddReferenceFact(lists, feature, a_fact); }

			void DeclareGeometryExtra(std::string_view a_extraName) override
			{
				if (a_extraName.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} declared a geometry extra without a name", feature));
				auto& declared = lists.lightingJoinRules.geometryExtra;
				if (!declared.empty())
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} declared geometry extra {}, but {} is declared", feature, a_extraName, declared));
				declared = std::string(a_extraName);
			}

			void RegisterPropertyBits(const Api::PropertyBits& a_bits) override { AddPropertyBits(lists, feature, a_bits); }
			void RegisterLightingObjectFlag(const Api::LightingObjectFlag& a_flag) override { AddLightingObjectFlag(lists, feature, a_flag); }

			void RegisterLightingKeyBits(const Api::LightingKeyBits& a_bits) override
			{
				if (!a_bits.keyBits || !a_bits.permutation)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered incomplete Lighting key bits", feature));
				auto& claimed = lists.lightingJoinRules.keyBits;
				if (claimed.keyBits)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered Lighting key bits, which another registration has", feature));
				claimed = a_bits;
			}

			void RegisterLightingPassRules(const Api::LightingPassRules& a_rules) override { AddLightingPassRules(lists, feature, a_rules); }
			void DeclarePerDrawBuffer(const Api::PerDrawBuffer& a_buffer) override { AddPerDrawBuffer(lists, feature, a_buffer); }

			std::string_view feature;
			const Api::FrameSchedule* schedule = nullptr;

		private:
			Lists& lists;
		};
	}

	void PreflightFeatures(std::span<const Api::Feature> a_features)
	{
		for (const auto& feature : a_features)
			if (feature.preflight)
				feature.preflight();
	}

	void InstallEarlyHooks(std::span<const Api::Feature> a_features)
	{
		static bool installed = false;
		if (std::exchange(installed, true) || Registered().installed)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the features' early hooks were installed twice, or after the features");
		for (const auto& feature : a_features)
			if (feature.earlyHooks)
				feature.earlyHooks();
	}

	void InstallFeatures(std::span<const Api::Feature> a_features, std::span<const Api::Feature> a_integrations, const Api::FrameSchedule& a_schedule)
	{
		auto& lists = Registered();
		if (std::exchange(lists.installed, true))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the features were installed twice");
		// Core's passes register first, so a feature reusing a Core key fails its install.
		Draws::RegisterCorePasses();
		// Lighting is kind 0. The features' material ports take kinds from 1 in registration order.
		lists.materialPorts[lists.materialPortCount++] = CoreLightingPort();
		Context context(lists);
		context.schedule = &a_schedule;
		for (const auto& feature : a_features) {
			context.feature = feature.name;
			if (feature.install)
				feature.install(context);
		}
		// Integrations install after every feature, because they may name what a feature registered. They have no early or source hooks.
		for (const auto& integration : a_integrations) {
			if (integration.earlyHooks || integration.sourceHooks)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("integration {} has early or source hooks", integration.name));
			context.feature = integration.name;
			if (integration.install)
				integration.install(context);
		}
		// Switch anchors may name a switch a later feature registered.
		CheckSwitchAnchors();
		// Classify inputs needed by name may come from a later feature or an integration.
		BindClassifyInputNeeds(lists.classifyInputNeeds);
		BindEngineChannelNeeds(lists.engineChannelNeeds);
		ResolveLightVolumes(lists.lightVolumes);
		ResolveSunVolume(lists.sunVolume);
		ResolveAncestorValue(lists.ancestorValue);
		ResolveLatePoses(lists.latePoses);
		ResolveViews(lists.views, lists.viewWork, lists.viewRoots, lists.cullHiddenSources);
		ResolveShadowViews(lists.occlusionMaps, lists.shadowSources, lists.shadowExclusion);
		ResolveViewBits();
		ResolveDrawFamilies(lists.drawFamilies);
		ResolveMainRanges(lists.mainRanges);
		ResolveVertexStream(lists.vertexStream);
		ResolveMainCullInput(lists.mainCullInput);
		ResolveCullBindings(lists.cullBindings);
		ResolveDepthPyramid(lists.depthPyramid);
		ResolvePrimaryView(lists.primaryView);
		ResolveShadowEpochs(lists.shadowEpochs);
		ResolveRootStates(lists.rootStates);
		ResolvePaletteRows(lists.paletteRows);
		ResolveShadingFields(lists.shadingFields);
		ResolveMaterialPorts(lists.materialPorts, lists.materialPortCount);
		ResolveMaterialContributors(lists.materialContributors);
		ResolveProgramFamilies(lists.programFamilies);
		ResolveDeferredRanges(lists.deferredRanges);
		std::ranges::sort(lists.frameConstantRows, {}, &FrameConstantRowEntry::row);
		// A schedule key that nobody registered fails the install.
		Draws::ResolveSchedule(a_schedule);
	}

	std::span<const FrameValuePoolEntry> RegisteredPools() { return Registered().pools; }
	std::span<const PayloadProductEntry> RegisteredProducts() { return Registered().products; }
	std::span<const SceneResourcesEntry> RegisteredSceneResources() { return Registered().sceneResources; }
	std::span<const CommitHookEntry> RegisteredCommitHooks() { return Registered().commitHooks; }
	std::span<const FrameBindingEntry> RegisteredFrameBindings() { return Registered().frameBindings; }
	std::span<const FrameConstantRowEntry> RegisteredFrameConstantRows() { return Registered().frameConstantRows; }
	std::span<const FrameTextureEntry> RegisteredFrameTextures() { return Registered().frameTextures; }
	std::span<const FrameGraphBuffersEntry> RegisteredFrameGraphBuffers() { return Registered().frameGraphBuffers; }
	std::span<const std::uint8_t> FrameTextureIndices() { return Registered().frameTextureIndices; }
	std::span<const ReportLinesEntry> RegisteredReportLines() { return Registered().reportLines; }
	std::span<const Api::StandInObserver> RegisteredStandInObservers() { return Registered().standInObservers; }
	std::span<const EngineChannelEntry> RegisteredEngineChannels() { return Registered().engineChannels; }
	const Api::ShowVeto& RegisteredShowVeto() { return Registered().showVeto; }
	bool ShowVetoRegistered() { return Registered().showVetoRegistered; }
	std::uint32_t ReservedHiddenSites() { return Registered().reservedHiddenSites; }
	std::span<const TableFamilyEntry> RegisteredTableFamilies() { return Registered().tableFamilies; }
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents() { return Registered().coordinatorComponents; }
	const TechniqueEntries& RegisteredTechniques() { return Registered().techniques; }
	const GeometryTypesPorted& RegisteredGeometryTypes() { return Registered().geometryTypes; }
	const GeometryTypeContributions& RegisteredGeometryTypeContributions() { return Registered().geometryTypeContributions; }
	const EntryClassRules& RegisteredEntryClassRules() { return Registered().entryClassRules; }
	const OwnerClasses& RegisteredOwnerClasses() { return Registered().ownerClasses; }
	std::string_view OwnedEntriesLabel(bool a_withoutRecord) { return a_withoutRecord ? Registered().ownedWithoutRecordLabel : Registered().ownedLabel; }
	std::span<const PropertyFlagRule> PropertyFlagRules() { return std::span(Registered().propertyFlagRules).first(Registered().propertyFlagRuleCount); }
	const CategoryRoots& RegisteredCategoryRoots() { return Registered().categoryRoots; }
	const CategoryRootGathers& RegisteredCategoryRootGathers() { return Registered().categoryRootGathers; }
	const Api::UnheldRootsContribution& RegisteredUnheldRoots() { return Registered().unheldRoots; }
	const ParentsTaken& RegisteredParents() { return Registered().parents; }
	const ObjectClassesTaken& RegisteredObjectClasses() { return Registered().objectClasses; }
	std::span<const ShadingContributionEntry> RegisteredShadingContributions() { return Registered().shadingContributions; }
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals() { return Registered().frameGlobals; }
	std::span<const ClassifyInputEntry> RegisteredClassifyInputs() { return Registered().classifyInputs; }
	std::span<const HeldGroupEntry> HeldGroups() { return Registered().heldGroups; }
	const NearTwinEntry& NearTwin() { return Registered().nearTwin; }
	const LightingJoinRules& RegisteredLightingJoinRules() { return Registered().lightingJoinRules; }

	std::uint32_t PerDrawMask(Api::ShaderStage a_stage)
	{
		const auto stage = static_cast<std::size_t>(a_stage);
		return stage < Registered().perDrawMasks.size() ? Registered().perDrawMasks[stage] : 0u;
	}

	bool EntryClassTaken(Api::EntryClass a_class)
	{
		return a_class < Api::EntryClass::Count && Registered().entryClassesTaken[static_cast<std::size_t>(a_class)];
	}
}
