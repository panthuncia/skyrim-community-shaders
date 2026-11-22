// Helpers.h first: volk must precede every Vulkan header in this translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "Features/DrawcallLimitFix/Features/LocalLights/LightContextResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "RenderGraph/Async/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContextAtlas.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramsCpu.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/StrictGates.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <algorithm>
#include <bit>
#include <map>
#include <stdexcept>

namespace DCLF::Features::LocalLights
{
	namespace
	{
		Api::SceneResourcesId sceneId;
		constexpr const char* kObjectGatesName = "cs.dclf.object-gates";
		// Past this many gates the table is dropped and rebuilt, so chains of freed nodes are not kept forever.
		constexpr std::size_t kMaxGates = 4096;

		// Render thread. Gate 0 is the empty chain. Ids stay stable until the table is dropped.
		struct Gates
		{
			std::map<std::vector<std::uintptr_t>, std::uint32_t> interned;
			std::vector<Portals::GateChain> chains;
			std::uint64_t generation = 0;  // Bumped when the table is dropped.
			std::uint64_t rootsKey = 0;
			bool rootsBuilt = false;
			std::vector<std::uint32_t> rootGate;  // By root slot.
			std::uint64_t columnKey = 0;
			bool columnBuilt = false;
			std::vector<std::uint32_t> column;  // By object slot.
			// Immutable copy, because the CPU pipeline's culls read it off the render thread.
			std::shared_ptr<const std::vector<std::uint32_t>> published;
			std::uint64_t unmodelled = 0, rootless = 0;

			void Reset()
			{
				interned.clear();
				chains.assign(1, {});
				interned.emplace(std::vector<std::uintptr_t>{}, 0u);
				++generation;
				rootsBuilt = columnBuilt = false;
				unmodelled = 0;
			}

			std::uint32_t Intern(const Portals::GateChain& a_chain)
			{
				std::vector<std::uintptr_t> key;
				key.reserve(a_chain.size() * 4);
				for (const auto& link : a_chain)
					key.insert(key.end(), { std::uintptr_t(link.kind), reinterpret_cast<std::uintptr_t>(link.node), link.program, link.roomMode });
				const auto [it, added] = interned.try_emplace(std::move(key), static_cast<std::uint32_t>(chains.size()));
				if (added) {
					chains.push_back(a_chain);
					unmodelled += std::any_of(a_chain.begin(), a_chain.end(), [](const auto& a_link) { return a_link.kind == Portals::GateLink::kShape; });
				}
				return it->second;
			}
		};

		// Readers declare these late-bound buffers. Each publication's backing becomes a version of one, so a reader on
		// another queue waits for the upload that fills it.
		constexpr const char* kAtlasStandInName = "cs.dclf.light-contexts.atlas.stand-in";
		constexpr const char* kGatesStandInName = "cs.dclf.light-contexts.gates.stand-in";

		struct Scene
		{
			std::shared_ptr<org::services::BackingPool> pool;
			std::shared_ptr<org::VersionedBuffer> atlasVersions, gateVersions;
			std::shared_ptr<const LightContextPublication> current;
			Draws::Versioned objectGates;  // By object slot.
			org::services::FilledContents<> objectGatesFilled;  // Which version holds which column key.
			Gates gates;
		};

		// Each version holds the lease that keeps its words on the GPU.
		struct PublicationBackings
		{
			std::shared_ptr<const org::BufferVersion> atlas, gates;
		};

		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto scene = std::make_shared<Scene>();
			scene->pool = org::services::BackingPool::Create(PooledBackings::PerFrameHooks(), ::RenderGraph::Frame::Retirement::Clock(),
				{ .name = "cs.dclf.light-contexts", .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
			auto atlasStandIn = a_setup.StructuredBuffer(1, 4, kAtlasStandInName);
			auto gatesStandIn = a_setup.StructuredBuffer(1, 4, kGatesStandInName);
			if (!atlasStandIn || !gatesStandIn)
				return nullptr;
			scene->atlasVersions = org::VersionedBuffer::CreateLateBound(std::move(atlasStandIn));
			scene->gateVersions = org::VersionedBuffer::CreateLateBound(std::move(gatesStandIn));
			scene->objectGates = Draws::MakeVersioned(a_setup.StructuredBuffer(a_setup.smallStart ? 4u : 4096u, sizeof(std::uint32_t), kObjectGatesName), { .extent = { 0, 4, 4 } });
			scene->gates.Reset();
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& scene = a_graph.State<Scene>();
			Draws::Register(a_graph.graph, org::ResourceIdentifier(kObjectGatesName), scene.objectGates);
		}

		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			Draws::UnitGroup{}.Add(a_requirements.State<Scene>().objectGates).Require(a_requirements.build, a_requirements.objectRows);
		}

		void Fit(Api::SceneResourcesFit& a_fit)
		{
			// A set sealed before any revision required the column names none of it. Then no object fits, and nothing throws.
			const auto units = Draws::NamedUnits(a_fit.sealed, a_fit.State<Scene>().objectGates);
			a_fit.fit.objects = std::min(a_fit.fit.objects, static_cast<std::uint32_t>(std::min<std::uint64_t>(units, UINT32_MAX)));
		}

		// An object takes its fade root's gate. An object without a root gets its own chain.
		void BuildColumn(Gates& a_gates, const SceneTables& a_tables)
		{
			if (a_gates.chains.size() > kMaxGates)
				a_gates.Reset();
			const auto roots = Extensions::RootListOf(a_tables);
			const std::uint64_t structure = PortalViews::Get().ProgramsVersion();
			const std::uint64_t rootsKey = roots.contentEpoch * 0x9E3779B97F4A7C15ull ^ structure;
			Portals::GateChain chain;
			if (!a_gates.rootsBuilt || a_gates.rootsKey != rootsKey || a_gates.rootGate.size() != roots.nodes.size()) {
				a_gates.rootsKey = rootsKey;
				a_gates.rootsBuilt = true;
				a_gates.rootGate.assign(roots.nodes.size(), 0u);
				for (std::size_t r = 0; r < roots.nodes.size(); ++r)
					if (const auto* root = static_cast<const RE::NiAVObject*>(roots.nodes[r])) {
						Portals::ChainOf(root, chain);
						a_gates.rootGate[r] = a_gates.Intern(chain);
					}
				a_gates.columnBuilt = false;
			}
			const std::uint64_t columnKey = (DCLF::Scene::FrameView::Get().GetTablesGeneration() * 0x9E3779B97F4A7C15ull ^ rootsKey) + a_gates.generation;
			if (a_gates.columnBuilt && a_gates.columnKey == columnKey && a_gates.column.size() == a_tables.objectGeometry.size())
				return;
			a_gates.columnKey = columnKey;
			a_gates.columnBuilt = true;
			a_gates.rootless = 0;
			a_gates.column.assign(a_tables.objectGeometry.size(), 0u);
			for (std::size_t o = 0; o < a_tables.objectGeometry.size(); ++o) {
				const auto root = roots.RootOf(static_cast<std::uint32_t>(o));
				if (root < a_gates.rootGate.size() && roots.NodeOf(root))
					a_gates.column[o] = a_gates.rootGate[root];
				else if (const auto* geometry = a_tables.objectGeometry[o]) {
					Portals::ChainOf(geometry, chain);
					a_gates.column[o] = a_gates.Intern(chain);
					++a_gates.rootless;
				}
			}
			a_gates.published = std::make_shared<const std::vector<std::uint32_t>>(a_gates.column);
		}

		// Uploads only the changed words, or all of them for a version not yet filled, such as after a growth.
		void UploadColumn(Scene& a_scene, const Api::CommitContext& a_commit)
		{
			BuildColumn(a_scene.gates, a_commit.tables);
			if (!a_commit.adopted.Names(*a_scene.objectGates))
				return;
			const auto& version = a_commit.adopted.Of(*a_scene.objectGates);
			const auto& column = a_scene.gates.column;
			if (column.size() > version.units)
				return;  // Should not happen. The revision sized the column to its objects.
			const auto filled = a_scene.objectGatesFilled.Since(version.serial);
			if (filled && *filled == a_scene.gates.columnKey)
				return;
			static std::vector<std::uint32_t> sent;  // Render thread.
			std::vector<std::uint32_t> differing;
			if (!filled || sent.size() != column.size()) {
				if (!column.empty())
					a_commit.uploads(a_scene.objectGates, column.data(), column.size() * sizeof(std::uint32_t), 0);
			} else {
				for (std::uint32_t o = 0; o < column.size(); ++o)
					if (sent[o] != column[o])
						differing.push_back(o);
				Draws::SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					a_commit.uploads(a_scene.objectGates, column.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
			}
			sent = column;
			a_scene.objectGatesFilled.Filled(version.serial, a_scene.gates.columnKey);
		}

		// One word per interned gate per context. Empty when no context is strict.
		std::vector<std::uint32_t> EncodeVerdicts(const Gates& a_gates, const LightSelection::Frame& a_selection)
		{
			const auto& atlas = a_selection.portalContexts;
			const std::uint32_t contexts = atlas.empty() ? 0u : atlas[0];
			std::vector<const Portals::Context*> byEntry(contexts, nullptr);
			bool strict = false;
			for (const auto& entry : a_selection.frameLights)
				if (entry.portalContext && entry.portalContextIndex < contexts) {
					byEntry[entry.portalContextIndex] = entry.portalContext.get();
					strict = strict || entry.portalContext->strict;
				}
			if (!strict)
				return {};
			const auto gateCount = static_cast<std::uint32_t>(a_gates.chains.size());
			std::vector<std::uint32_t> words;
			words.reserve(Portals::kGateHeaderWords + std::size_t(gateCount) * contexts);
			words.insert(words.end(), { gateCount, contexts, atlas[2], atlas[3], 0u, 0u, 0u, 0u });
			for (const auto& chain : a_gates.chains)
				for (std::uint32_t c = 0; c < contexts; ++c)
					words.push_back(byEntry[c] ? Portals::GateWord(chain, *byEntry[c]) : kPortalNoProgram);
			return words;
		}

		// Shaders read this structured buffer as a ByteAddressBuffer.
		std::shared_ptr<const org::BufferVersion> Upload(Scene& a_scene, const Api::CommitContext& a_commit, const std::vector<std::uint32_t>& a_words, const char* a_name,
			org::VersionedBuffer& a_buffer, std::uint32_t& a_descriptor)
		{
			const auto capacity = std::bit_ceil(std::max<std::uint64_t>(64, a_words.size()));
			org::services::BackingPool::AcquireInfo acquired;
			auto lease = std::make_shared<org::services::BackingLease>(
				a_scene.pool->Acquire({ .capacityClass = capacity, .stride = 4, .name = a_name }, org::services::BackingPool::kAnyEpoch, &acquired));
			if (!*lease)
				throw std::runtime_error("allocation failed: " + acquired.error);
			if (!(*lease)->srvIndex)
				throw std::runtime_error("missing GPU descriptor");
			a_descriptor = (*lease)->srvIndex;
			a_commit.uploads((*lease)->buffer, a_words.data(), a_words.size() * sizeof(std::uint32_t), 0);
			// Copy the buffer first, because the lease moves into the version.
			auto buffer = (*lease)->buffer;
			return a_buffer.PublishLate(std::move(buffer), std::move(lease));
		}

		void Commit(const Api::CommitContext& a_commit)
		{
			auto* scene = Extensions::SceneResourcesOf<Scene>(a_commit.scene, sceneId);
			if (!scene)
				return;
			const auto* selection = LightSelection::Get().Current(a_commit.frame);
			if (!selection || selection->portalContexts.empty()) {
				scene->current.reset();
				return;
			}
			const auto& words = selection->portalContexts;
			// Commit hooks run unguarded on the render thread: a rejected or unallocated atlas publishes nothing this frame.
			if (!scene->current || scene->current->frame != a_commit.frame ||
				scene->current->generation != (words.size() >= 4 ? std::uint64_t(words[2]) | (std::uint64_t(words[3]) << 32) : 0)) {
				scene->current.reset();
				try {
					auto publication = std::make_shared<LightContextPublication>();
					publication->frame = a_commit.frame;
					publication->generation = DCLF::Portals::ValidateContexts(words, a_commit.frame);
					publication->words = words;
					auto backings = std::make_shared<PublicationBackings>();
					backings->atlas = Upload(*scene, a_commit, words, "cs.dclf.light-contexts.atlas", *scene->atlasVersions, publication->descriptor);
					// The column must come first, because it interns the chains the verdicts cover.
					if (std::any_of(selection->frameLights.begin(), selection->frameLights.end(), [](const auto& a_entry) { return a_entry.portalContext && a_entry.portalContext->strict; })) {
						UploadColumn(*scene, a_commit);
						publication->gateWords = EncodeVerdicts(scene->gates, *selection);
						publication->unmodelledGates = scene->gates.unmodelled;
						publication->rootlessObjects = scene->gates.rootless;
						if (!publication->gateWords.empty()) {
							backings->gates = Upload(*scene, a_commit, publication->gateWords, "cs.dclf.light-contexts.gates", *scene->gateVersions, publication->gatesDescriptor);
						}
					}
					publication->owner = std::move(backings);
					scene->current = std::move(publication);
				} catch (const std::exception& e) {
					static std::uint64_t rejected = 0;  // Render thread.
					if (std::has_single_bit(++rejected))
						logger::error("[DCLF][structural-gap] point-light context publication rejected at frame {}: {}. Occurrences: {}", a_commit.frame, e.what(), rejected);
					return;
				}
			} else if (scene->current->gatesDescriptor)
				UploadColumn(*scene, a_commit);  // The column is versioned, so a later commit's version must be filled too.
			// Each execution retains the lease until GPU retirement, including executions sharing this frame's atlas.
			a_commit.executionOwners.push_back(scene->current);
		}

		template <bool Gates>
		void SelectContexts(Api::CullBindingSelect& a_select)
		{
			if (auto* scene = Extensions::SceneResourcesOf<Scene>(a_select.scene, sceneId))
				a_select.Published(Gates ? scene->gateVersions : scene->atlasVersions);
			if (const auto publication = CurrentLightContexts(a_select.scene, a_select.frame))
				a_select.Index(Gates ? publication->gatesDescriptor : publication->descriptor);
		}

		void SelectObjectGates(Api::CullBindingSelect& a_select)
		{
			if (auto* scene = Extensions::SceneResourcesOf<Scene>(a_select.scene, sceneId))
				a_select.Versioned(scene->objectGates);
		}
	}

	namespace
	{
		// Gives the CPU pipeline the same gates the GPU path binds.
		Direct::StrictGates FrameStrictGatesOf(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
		{
			const auto publication = CurrentLightContexts(a_scene, a_frame);
			const auto* scene = Extensions::SceneResourcesOf<Scene>(a_scene, sceneId);
			if (!publication || !publication->gatesDescriptor || !scene || !scene->gates.published)
				return {};
			using Hold = std::pair<std::shared_ptr<const LightContextPublication>, std::shared_ptr<const std::vector<std::uint32_t>>>;
			auto hold = std::make_shared<const Hold>(publication, scene->gates.published);
			return DCLF::Portals::StrictGatesOf(hold->first->gateWords, hold->first->words, *hold->second, hold);
		}
	}

	void RegisterLightContexts(Api::InstallContext& a_context)
	{
		Direct::SetStrictGatesSource(&FrameStrictGatesOf);
		sceneId = a_context.RegisterSceneResources({ .name = "point-light contexts", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit });
		a_context.RegisterCommitHook({ .name = "point-light contexts", .commit = &Commit });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::LightContexts, .name = "cs.dclf.light-contexts", .select = &SelectContexts<false> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::StrictGates, .name = "cs.dclf.strict-gates", .select = &SelectContexts<true> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::ObjectGates, .name = kObjectGatesName, .select = &SelectObjectGates });
	}

	std::shared_ptr<const LightContextPublication> CurrentLightContexts(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto* scene = Extensions::SceneResourcesOf<Scene>(a_scene, sceneId);
		const auto* selection = LightSelection::Get().Current(a_frame);
		if (!scene || !scene->current || scene->current->frame != a_frame || !selection || selection->portalContexts.size() < 4)
			return nullptr;
		const auto& words = selection->portalContexts;
		const auto generation = std::uint64_t(words[2]) | (std::uint64_t(words[3]) << 32);
		return scene->current->generation == generation ? scene->current : nullptr;
	}
}
