// The scene's growable buffers: registration, ties to DCLF's state graph, and the producer's requirements and seals.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"

#include <ORGModuleServices/ElasticCapacity.h>

namespace DCLF::Draws
{
	namespace
	{
		std::uint64_t DeviceAddressOf(const org::Buffer& a_buffer) { return PooledBackings::DeviceAddressOf(a_buffer); }

		/** @brief The set's pool of DCLF's pooled backings, on DCLF's retirement clock with no caps. The growth producer is its one acquirer. */
		std::shared_ptr<org::services::BackingPool> MakeGrowthPool()
		{
			auto hooks = PooledBackings::Hooks();
			return org::services::BackingPool::Create(std::move(hooks), ::RenderGraph::Frame::Retirement::Clock(), { .name = "cs.dclf.scene-buffers", .maxPerClass = 0 });
		}
	}

	SceneBufferSet& SceneBufferSet::Get()
	{
		static SceneBufferSet set;
		return set;
	}

	SceneBufferSet::SceneBufferSet() :
		set([this] {
			org::services::GrowableBufferSetHooks hooks;
			// Growth runs as graph work on the preparation pool. When a batch finishes it wakes the builder, which seals again.
			hooks.post = [](org::async::VersionGrowthWork a_work) { return SceneScheduler::Graph().PostGrowth(std::move(a_work)); };
			hooks.wake = [] { WakeSnapshotBuilder(); };
			hooks.uploads = [this] { return uploads.lock(); };
			hooks.addressOf = [](const org::Buffer& a_buffer) { return DeviceAddressOf(a_buffer); };
			// Each version is a pooled backing, kept from reuse until the frames that read it retire.
			hooks.pool = MakeGrowthPool();
			hooks.grown = [](const org::services::GrowableRegistration& a_buffer, std::uint64_t a_from, std::uint64_t a_to, std::uint64_t a_bytes) {
				logger::info("[DCLF] {}: {} {} to {} ({} KB), as graph work", a_buffer.name, a_from, a_to < a_from ? "shrunk" : "grown", a_to, a_bytes / 1024);
			};
			return hooks;
		}())
	{}

	Versioned MakeVersioned(std::shared_ptr<org::Buffer> a_buffer, GrowableSpec a_spec)
	{
		return SceneBufferSet::Get().Make(std::move(a_buffer), std::move(a_spec));
	}

	Versioned SceneBufferSet::Make(std::shared_ptr<org::Buffer> a_first, GrowableSpec a_spec)
	{
		if (!a_first)
			return nullptr;
		org::services::GrowableRegistration registration;
		registration.name = a_first->GetName();
		auto versioned = org::VersionedBuffer::Create(std::move(a_first));
		registration.buffer = versioned;
		registration.extent = a_spec.extent;
		registration.policy = a_spec.policy;
		// Gives memory back once the scene asks for much less (CS_DCLF_SHRINK).
		registration.shrink = PooledBackings::Shrink();
		registration.contents = a_spec.contents;
		registration.capture = std::move(a_spec.capture);
		// Layouts and diagnostics read the device address. Shaders read the buffer view ref.
		registration.address = true;
		(void)set.Register(std::move(registration));
		return versioned;
	}

	SealedSet SceneBufferSet::Seal(GrowthBuild&& a_build)
	{
		auto sealed = set.Seal(std::move(a_build));
		// The next seal that needs a failed growth posts it again.
		if (!sealed.error.empty() && failuresLogged++ < 8)
			logger::error("[DCLF] a growth of the scene's buffers failed: {}", sealed.error);
		return sealed;
	}

	void SceneBufferSet::Require(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_units)
	{
		if (a_buffer && a_units)
			a_build.Require(set.HandleOf(*a_buffer), a_units);
	}

	void SceneBufferSet::RequireNewer(GrowthBuild& a_build, const Versioned& a_buffer, std::uint64_t a_serial)
	{
		if (a_buffer && a_serial)
			a_build.RequireNewer(set.HandleOf(*a_buffer), a_serial);
	}

	std::uint64_t SceneBufferSet::Units(const GrowthBuild& a_build, const Versioned& a_buffer)
	{
		return a_buffer ? a_build.Units(set.HandleOf(*a_buffer)) : 0;
	}

	std::uint64_t SceneBufferSet::Asked(const GrowthBuild& a_build, const Versioned& a_buffer)
	{
		if (!a_buffer)
			return 0;
		const auto handle = set.HandleOf(*a_buffer);
		return std::max(a_build.Asked(handle), a_build.Units(handle));
	}

	std::string SceneBufferSet::Report()
	{
		const auto stats = set.TakeStats();
		if (!stats.batches && !stats.grown && !stats.failed && !stats.latches && !stats.sealed && !stats.kept)
			return {};
		return fmt::format("{} growth batches posted, {} buffers grown ({} of them shrunk), {} failed; {} latch blocks made; {} sets sealed, {} seals kept the last set",
			stats.batches, stats.grown, stats.shrunk, stats.failed, stats.latches, stats.sealed, stats.kept);
	}
}
