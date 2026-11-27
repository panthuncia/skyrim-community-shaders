// Portal programs: each depth commit publishes the frame's encoding into an immutable pooled backing sized to it. Every execution
// that reads it retains it until GPU retirement. Also holds the member batch's scratch and PortalTest on the CPU.

// Helpers.h first, in its own include block, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "RenderGraph/Async/Retirement.h"

#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/GrowableBufferTypes.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Features::Portals
{
	namespace
	{
		constexpr const char* kProgramsName = "cs.dclf.portal-programs";
		constexpr const char* kStandInName = "cs.dclf.portal-programs.stand-in";

		/** @brief An immutable published encoding, its buffer version and the descriptor the latch names. */
		struct Publication
		{
			std::vector<std::uint32_t> words;
			std::shared_ptr<const org::BufferVersion> version;
			std::uint32_t descriptor = 0;
		};

		/**
		 * @brief The member batch, the publications' pool, the programs' buffer and the current publication. Render thread.
		 * The buffer is late-bound, so a reader on the compute queue waits for the upload that fills each version.
		 */
		struct ProgramsScene
		{
			std::shared_ptr<Batch> members;
			std::shared_ptr<org::services::BackingPool> pool;
			std::shared_ptr<org::VersionedBuffer> programs;
			std::shared_ptr<const Publication> current;
			std::uint32_t frame = ~0u;  // the scene frame current was published for
		};

		Api::SceneResourcesId sceneId;  // install

		ProgramsScene* SceneOf(const Draws::SceneBuffers& a_scene)
		{
			return Extensions::SceneResourcesOf<ProgramsScene>(a_scene, sceneId);
		}

		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto scene = std::make_shared<ProgramsScene>();
			scene->members = CreateBatch(a_setup, "cs.dclf.member-portal");
			if (!scene->members)
				return nullptr;
			scene->pool = org::services::BackingPool::Create(PooledBackings::PerFrameHooks(), ::RenderGraph::Frame::Retirement::Clock(),
				{ .name = kProgramsName, .maxPerClass = 0, .spareFree = PooledBackings::PerFrameSpares() });
			auto standIn = a_setup.StructuredBuffer(1, 4, kStandInName);
			if (!standIn)
				return nullptr;
			scene->programs = org::VersionedBuffer::CreateLateBound(std::move(standIn));
			return scene;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			const auto& scene = a_graph.State<ProgramsScene>();
			RegisterBatch(a_graph.graph, *scene.members);
		}

		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			const auto& scene = a_requirements.State<ProgramsScene>();
			const auto& batch = *scene.members;
			Draws::UnitGroup{}.Add(batch.queries).Add(batch.results).Add(batch.claims).Add(batch.traces).Require(a_requirements.build, a_requirements.objectRows);
		}

		void Fit(Api::SceneResourcesFit& a_fit)
		{
			// A set sealed before the revision required the batch names none of it. No object fits it, and that must not throw.
			const auto& b = *a_fit.State<ProgramsScene>().members;
			std::uint64_t units = ~0ull;
			for (const auto* buffer : { &b.queries, &b.results, &b.claims, &b.traces })
				units = std::min(units, Draws::NamedUnits(a_fit.sealed, *buffer));
			a_fit.fit.objects = std::min(a_fit.fit.objects, static_cast<std::uint32_t>(std::min<std::uint64_t>(units, UINT32_MAX)));
		}

		template <Api::CullBindingSlot Slot>
		void SelectMembers(Api::CullBindingSelect& a_select)
		{
			if (const auto* scene = SceneOf(a_select.scene)) {
				const auto& b = *scene->members;
				if constexpr (Slot == Api::CullBindingSlot::MemberQueries)
					a_select.Versioned(b.queries);
				else if constexpr (Slot == Api::CullBindingSlot::MemberResults)
					a_select.Versioned(b.results);
				else
					a_select.Versioned(b.claims);
			}
		}

		std::shared_ptr<const void> BatchSetup(const Api::PassSetup& a_setup)
		{
			return Extensions::SceneResourcesStateOf(a_setup.scene, sceneId);
		}
		std::shared_ptr<org::RenderPass> MemberBatch(const Api::PassContext& a_context)
		{
			const auto resources = a_context.Main();
			const auto* scene = SceneOf(*resources->scene);
			return scene ? MakeBatchPass(resources, scene->members, a_context.segment) : nullptr;
		}

		/** @brief Publishes the frame's encoding into a backing sized to it. An unchanged encoding keeps the last one. Render thread. */
		std::shared_ptr<const Publication> Publish(ProgramsScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			// Native's render side: the programs its light set carries; the game thread's walk rewrites PortalViews' own.
			const auto* boards = Features::Shadows::Native::AdoptedBoards();
			const auto& encoded = boards && boards->portals ? boards->portals->programs : PortalViews::Get().Encoded();
			if (encoded.size() < kPortalHeaderBytes / 4)
				return nullptr;
			if (a_scene.current && a_scene.current->words == encoded)
				return a_scene.current;
			auto publication = std::make_shared<Publication>();
			publication->words = encoded;
			org::services::BackingPool::AcquireInfo acquired;
			auto lease = std::make_shared<org::services::BackingLease>(a_scene.pool->Acquire(
				{ .capacityClass = std::bit_ceil(std::max<std::uint64_t>(64, encoded.size())), .stride = 4, .name = "cs.dclf.portal-programs.encoding" },
				org::services::BackingPool::kAnyEpoch, &acquired));
			if (!*lease)
				throw std::runtime_error("allocation failed: " + acquired.error);
			publication->descriptor = (*lease)->srvIndex;
			if (!publication->descriptor)
				throw std::runtime_error("missing GPU descriptor");
			a_uploads((*lease)->buffer, encoded.data(), encoded.size() * sizeof(std::uint32_t), 0);
			// Move the lease into the version, which keeps the backing current while this publication lives.
			auto buffer = (*lease)->buffer;
			publication->version = a_scene.programs->PublishLate(std::move(buffer), std::move(lease));
			return publication;
		}

		void Commit(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* scene = SceneOf(a_commit.scene);
			if (!scene)
				return;
			if (scene->frame != a_commit.frame) {
				scene->frame = a_commit.frame;
				// Commit hooks run unguarded on the render thread. Without a publication every reader takes the frustum alone.
				try {
					scene->current = Publish(*scene, a_commit.uploads);
				} catch (const std::exception& e) {
					scene->current.reset();
					static std::uint64_t failed = 0;  // render thread
					if (std::has_single_bit(++failed))
						logger::error("[DCLF][structural-gap] portal programs not published at frame {}: {}. Occurrences: {}", a_commit.frame, e.what(), failed);
				}
			}
			if (scene->current)
				a_commit.executionOwners.push_back(scene->current);
		}

		void SelectPrograms(Api::CullBindingSelect& a_select)
		{
			const auto* scene = SceneOf(a_select.scene);
			if (!scene)
				return;
			a_select.Published(scene->programs);
			if (scene->current && scene->frame == a_select.frame)
				a_select.Index(scene->current->descriptor);
		}
	}

	int PortalTest(std::span<const std::uint32_t> a_programs, std::uint32_t a_program, const float a_centre[3], float a_radius,
		std::uint32_t a_nodeFlags, std::string& a_why)
	{
		return PortalViews::Visible(a_programs, a_program, a_centre, a_radius, a_nodeFlags, a_why);
	}

	void RegisterPrograms(Api::InstallContext& a_context)
	{
		sceneId = a_context.RegisterSceneResources({ .name = "portal-programs", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::MemberQueries, .name = "member-portal-queries", .select = &SelectMembers<Api::CullBindingSlot::MemberQueries> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::MemberResults, .name = "member-portal-results", .select = &SelectMembers<Api::CullBindingSlot::MemberResults> });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::MemberClaims, .name = "member-portal-claims", .select = &SelectMembers<Api::CullBindingSlot::MemberClaims> });
		a_context.RegisterPass({ .key = "portals.member-evaluate", .passName = "cs.dclf.z.member-portal-evaluate", .make = &MemberBatch, .setup = &BatchSetup });

		a_context.RegisterCommitHook({ .name = "portal-programs", .commit = &Commit });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::Programs, .name = kProgramsName, .select = &SelectPrograms });
	}
}
