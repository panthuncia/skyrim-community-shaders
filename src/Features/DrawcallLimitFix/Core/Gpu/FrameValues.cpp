// The render thread's Kick and the frame value producers it dispatches onto the preparation pool, one at a time.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "RenderGraph/BufferRefs.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/Walkers.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PaletteRows.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadingFields.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/LatePoses/LatePoseOverrides.h"
#include <Render/PassBuilders.h>
#include <Resources/Buffers/VersionedBuffer.h>
#include <Tracy/TracyC.h>

#include "Features/DrawcallLimitFix/Core/Engine/EngineReleases.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"

#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "RenderGraph/Async/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Direct.h"
#include "Snapshot/FrameCamera.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Recorder.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PlacementRows/BillboardRow.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

#include <bit>
#include <deque>
#include <cmath>
#include <optional>
#include <cstring>
#include <limits>
#include <mutex>
#include <unordered_map>

#include "Features/DrawcallLimitFix/Core/Gpu/FrameValueMoves.h"
#include "Features/DrawcallLimitFix/Core/Gpu/PooledBackings.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SkinPartitionWatch.h"
#include "RenderGraph/Async/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/Upscaling.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <OpenRenderGraph/GraphHost.h>
#include <Render/Runtime/IUploadService.h>
#include <Resources/Buffers/Buffer.h>
#include <ORGModuleServices/BackingPool.h>
#include <ORGModuleServices/ElasticCapacity.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <rhi.h>
#include <rhi_interop.h>

#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <chrono>

namespace DCLF
{
	namespace
	{
		Api::PaletteRowsFirstPerson firstPersonPalette;

		// Row-major 3x4, as the shaders read a transform.
		void StoreTransform(const RE::NiTransform& a_transform, float (&a_out)[12])
		{
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				a_out[row * 4 + 0] = r[row][0] * s;
				a_out[row * 4 + 1] = r[row][1] * s;
				a_out[row * 4 + 2] = r[row][2] * s;
			}
			a_out[3] = a_transform.translate.x;
			a_out[7] = a_transform.translate.y;
			a_out[11] = a_transform.translate.z;
		}

		// A listed root's bound for the sun entry test, or an unbounded sphere.
		void StoreListedRoot(const RE::NiAVObject* a_node, float (&a_out)[4])
		{
			if (a_node && a_node->worldBound.radius >= 0.0f) {
				const auto& bound = a_node->worldBound;
				a_out[0] = bound.center.x;
				a_out[1] = bound.center.y;
				a_out[2] = bound.center.z;
				a_out[3] = bound.radius;
			} else {
				a_out[0] = a_out[1] = a_out[2] = 0.0f;
				a_out[3] = std::numeric_limits<float>::max();
			}
		}

		void StoreFadeNode(const RE::BSShaderProperty* a_property, float (&a_out)[4])
		{
			const auto node = LodFadeNodeOf(a_property);
			std::memcpy(a_out, node.data(), sizeof(a_out));
		}

		/** @brief a_geometry's depth-first index among a_node's geometries (null slots skipped); false if not found. */
		bool OrdinalUnder(const RE::NiNode& a_node, const RE::BSGeometry& a_geometry, std::uint32_t& a_index)
		{
			for (const auto& child : Engine::Children(a_node)) {
				auto* object = child.get();
				if (!object)
					continue;
				if (object == &a_geometry)
					return true;
				if (object->AsGeometry())
					++a_index;
				else if (const auto* node = object->AsNode(); node && OrdinalUnder(*node, a_geometry, a_index))
					return true;
			}
			return false;
		}

		// What an object's parent chain holds: its nearest BSOrderedNode and whether a given fade node is an ancestor.
		// A property's fade node is a plain pointer that may be gone, so only a node on the live chain is read. Call under an engine-read lease.
		struct Ancestors
		{
			const RE::NiNode* ordered = nullptr;
			const RE::NiNode* fadeNode = nullptr;
		};
		Ancestors AncestorsOf(const RE::NiAVObject& a_object, const void* a_fadeNode)
		{
			static const REL::Relocation<const RE::NiRTTI*> orderedRtti{ RE::BSOrderedNode::Ni_RTTI };
			Ancestors out;
			for (auto* node = a_object.parent; node; node = node->parent) {
				if (!out.ordered && node->GetRTTI() == orderedRtti.get())
					out.ordered = node;
				if (node == a_fadeNode)
					out.fadeNode = node;
			}
			return out;
		}

		void StoreForwardSort(const RE::BSGeometry& a_geometry, float (&a_out)[4])
		{
			a_out[0] = a_out[1] = a_out[2] = a_out[3] = 0.0f;
			const auto& data = a_geometry.GetGeometryRuntimeData();
			const auto* alpha = data.alphaProperty.get();
			bool blended = alpha && (alpha->alphaFlags & 1);
			if (!blended)
				if (const auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(data.shaderProperty.get()); lighting && lighting->material)
					blended = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material)->materialAlpha < 1.0f;
			// A fading part (hint 9) draws with its root's parts as one group at the fade node's centre, last child first.
			// This matches the engine (FUN_1414b2330). Sorting by each part's own bound drew a faded head as a dark mask over the hair.
			const auto* property = data.shaderProperty.get();
			const auto ancestors = AncestorsOf(a_geometry, property ? property->fadeNode : nullptr);
			const RE::NiNode* fading = nullptr;
			if (ancestors.fadeNode && const_cast<RE::BSFadeNode*>(property->fadeNode)->GetRuntimeData().currentFade < 1.0f)
				fading = ancestors.fadeNode;
			if (!blended && !fading)
				return;
			const RE::NiNode* ordered = ancestors.ordered;
			if (!ordered)
				ordered = fading;
			std::uint32_t index = 0;
			if (!ordered || !OrdinalUnder(*ordered, a_geometry, index))
				return;
			if (ordered == fading)
				index = 0xFFFFu - (std::min)(index, 0xFFFEu);
			const auto& bound = ordered->worldBound;
			a_out[0] = bound.center.x;
			a_out[1] = bound.center.y;
			a_out[2] = bound.center.z;
			a_out[3] = std::bit_cast<float>(index + 1);
		}
	}

	FrameValues::FirstPersonPaletteStats FrameValues::TakeFirstPersonPaletteStats()
	{
		FirstPersonPaletteStats out;
		out.writes = firstPersonPalette.writes.exchange(0, std::memory_order_relaxed);
		out.units = firstPersonPalette.units.exchange(0, std::memory_order_relaxed);
		out.farthest = std::bit_cast<float>(firstPersonPalette.farthest.exchange(0, std::memory_order_relaxed));
		return out;
	}

	void FrameValues::SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_listedRoot, const RE::BSShaderProperty* a_fadeProperty,
		BindlessPlacement& a_out)
	{
		StoreTransform(a_geometry.world, a_out.world);
		// Render flag 0x10 means the previous transform is the current one (SetupGeometry).
		if (GeometryPort::kMainPassRenderFlags & 0x10)
			std::memcpy(a_out.previousWorld, a_out.world, sizeof(a_out.world));
		else
			StoreTransform(DrawnPreviousWorld(a_geometry), a_out.previousWorld);
		a_out.bound[0] = a_geometry.worldBound.center.x;
		a_out.bound[1] = a_geometry.worldBound.center.y;
		a_out.bound[2] = a_geometry.worldBound.center.z;
		a_out.bound[3] = a_geometry.worldBound.radius;
		StoreListedRoot(a_listedRoot, a_out.sunEntry);
		StoreFadeNode(a_fadeProperty, a_out.lodFadeNode);
		StoreForwardSort(a_geometry, a_out.forwardSort);
		// Sampled regardless of the toggle so rows never depend on it.
		PlacementRows::SampleBillboardRow(a_geometry, a_out);
	}

	bool FrameValues::SampleSlot(const SceneTables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out)
	{
		const auto* geometry = a_slot < a_tables.objectGeometry.size() ? a_tables.objectGeometry[a_slot] : nullptr;
		if (!geometry)
			return false;
		const RE::BSShaderProperty* fadeProperty = a_tables.IsLayer(a_slot) ? LayerPropertyOf(*geometry) : geometry->GetGeometryRuntimeData().shaderProperty.get();
		SampleRow(*geometry, a_slot < a_tables.listedRoot.size() ? a_tables.listedRoot[a_slot] : nullptr, fadeProperty, a_out);
		return true;
	}

	namespace
	{
		using Plan = PlacementPlan;
		using PaletteRow = FrameValues::PaletteRow;

		using FrameLeases = std::array<org::services::BackingLease, FrameValues::kBuiltInPools>;

		// GPU points of the frames that bound backings, oldest first. Uploads into a reused copy wait for these.
		using BoundPoints = std::deque<std::pair<std::uint64_t, org::GraphHost::GpuPoint>>;

		// Bind a frame's backings to its last submission so none is reused before that frame completes on the GPU.
		/**
		 * @brief Whether a producer the frame's work waits for may wait on a_point on the GPU: each value is reached or within the last
		 * present's sealed point. Unlike Retirement::Sealed, a seal or point still queued on the submission thread is not safe.
		 */
		bool SafeProducerWait(const org::GraphHost::GpuPoint& a_point)
		{
			if (a_point.points.empty())
				return true;
			const auto sealed = ::RenderGraph::Frame::Retirement::SealedGpuPoint();
			if (!sealed.Passed() || !a_point.Passed())
				return false;
			for (const auto& [timeline, value] : a_point.Resolve()) {
				if (!timeline || !*timeline || (*timeline)->GetCompletedValue() >= value)
					continue;
				const auto known = std::ranges::find_if(sealed.points, [&](const auto& a_entry) { return a_entry.first.get() == timeline.get(); });
				if (known == sealed.points.end() || known->second < value)
					return false;
			}
			return true;
		}

		void BindFrameLeases(FrameLeases& a_leases, std::vector<org::services::BackingLease>& a_registered, BoundPoints& a_points)
		{
			const auto point = ::RenderGraph::Frame::Retirement::SubmittedPoint();
			bool bound = false;
			const auto bind = [&](org::services::BackingLease& a_lease) {
				if (a_lease) {
					a_lease.Bind(point);
					a_lease.Reset();
					bound = true;
				}
			};
			for (auto& lease : a_leases)
				bind(lease);
			for (auto& lease : a_registered)
				bind(lease);
			const auto* host = RenderGraphRuntime::Get().Host();
			if (bound && host && point && (a_points.empty() || a_points.back().first < point))
				a_points.emplace_back(point, ::RenderGraph::Frame::Retirement::SubmittedGpuPoint());
			const auto completed = ::RenderGraph::Frame::Retirement::Clock()->Completed();
			while (a_points.size() > 1 && a_points[1].first <= completed)
				a_points.pop_front();
		}

		void SampleItem(const Plan::Item& a_item, BindlessPlacement& a_row, BindlessPlacement& a_layer)
		{
			const auto& geometry = *a_item.geometry;
			FrameValues::SampleRow(geometry, a_item.listedRoot.get(), geometry.GetGeometryRuntimeData().shaderProperty.get(), a_row);
			if (a_item.layerSlot != kNoObjectSlot) {
				a_layer = a_row;
				StoreFadeNode(LayerPropertyOf(geometry), a_layer.lodFadeNode);
			}
		}

		/**
		 * @brief The move keys an object answers to: itself, every reference on its chain, or every node above it if there is none.
		 * A superset of the scene pass's keys, so the producer samples whatever the light path would. Call under an engine-read lease.
		 */
		void KeysOf(const RE::NiAVObject& a_object, std::vector<const void*>& a_out)
		{
			a_out.clear();
			a_out.push_back(&a_object);
			bool reference = false;
			for (const RE::NiAVObject* at = &a_object; at; at = at->parent)
				if (const void* owner = at->GetUserData()) {
					a_out.push_back(owner);
					reference = true;
				}
			if (!reference)
				for (const RE::NiAVObject* at = a_object.parent; at; at = at->parent)
					a_out.push_back(at);
			// Writers that move the ordered node's bound without moving the leaf key the node, so key it too.
			if (const auto* ordered = AncestorsOf(a_object, nullptr).ordered)
				a_out.push_back(ordered);
			std::sort(a_out.begin(), a_out.end());
			a_out.erase(std::unique(a_out.begin(), a_out.end()), a_out.end());
		}

		// Engine references go to the retire queue, let go on the main thread.
		template <class T>
		void ReleaseLater(RE::NiPointer<T>& a_reference)
		{
			if (a_reference)
				EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(a_reference)));
		}
	}

	struct FrameValues::Impl
	{
		enum Part : std::size_t
		{
			kPlacements,
			kPalettes,
			kShading
		};
		static constexpr std::array<const char*, kBuiltInPools> kPartNames{ "placements", "palettes", "shading" };
		static constexpr std::array<std::uint32_t, kBuiltInPools> kStrides{ sizeof(BindlessPlacement), sizeof(PaletteRow), sizeof(BindlessShading) };

		/** @brief One row set's pool. Kick is its only acquirer and released backings are destroyed on the graph host's cleanup lane. */
		struct Pool
		{
			std::shared_ptr<org::services::BackingPool> pool;
			std::uint64_t capacity = 0;  // render thread only
			org::services::ShrinkWindow window;
			std::atomic<std::uint64_t> newestContentEpoch{ 0 };
			std::atomic<std::uint64_t> floor{ 0 };  // diagnostics
			std::atomic<std::uint64_t> acquires{ 0 }, made{ 0 }, runs{ 0 }, bytes{ 0 };  // since the last take
			std::string backingName;
			// Fresh backings for a job that may not wait for the capped pool's bound one (LeaseBackings): uncapped, reusing only
			// retired backings. Made at the first such job.
			std::shared_ptr<org::services::BackingPool> overflow;
			std::atomic<std::uint64_t> overflowAcquires{ 0 };  // since the start
		};

		// Grow a quarter past the rows asked, not to a power of two. Every class is held once per frame in flight, and palettes reach tens of MB.
		static constexpr auto kPoolGrowth = org::services::GrowthPolicy::Headroom(25, 1024);

		static std::shared_ptr<org::services::BackingPool> MakePool(std::string_view a_name)
		{
			// Each frame writes its own copy, so the copies live in ORG's per-frame blocks.
			// Uncapped, and a backing is reused only once the GPU is done with it: a job that reused a bound copy waited for the bound
			// work on the copy queue, and that work could be waiting for the job's own signal, which deadlocked both queues.
			// A job never waits on the GPU now.
			return org::services::BackingPool::Create(PooledBackings::PerFrameHooks(), ::RenderGraph::Frame::Retirement::Clock(),
				{ .name = fmt::format("cs.dclf.frame-values.{}", a_name), .maxPerClass = 0, .spareFree = PooledBackings::SceneCopies(), .reuseBound = false });
		}

		Impl()
		{
			for (std::size_t part = 0; part < kBuiltInPools; ++part)
				pools[part].pool = MakePool(kPartNames[part]);
			MakeRegistered();
		}

		/** @brief A registered pool and its Pool, made with the built-in ones (the features' pools are all registered at install). */
		struct Registered
		{
			const Extensions::FrameValuePoolEntry* entry = nullptr;
			Pool pool;
			// The rows its backing must hold: a game-side pool's kicks raise it in the game half, the render reads it at its lease.
			std::atomic<std::uint64_t> needed{ 0 };
			std::optional<std::uint64_t> sampleFloor;  // chain: the floor a game-side sample opens its build at
		};

		// Render thread.
		std::array<Pool, kBuiltInPools> pools;
		FrameLeases frameLeases;  // bound at EndFrame
		// The late poses' second placement and palette backings, leased at Kick when the frame has a plan.
		struct LateStage
		{
			std::shared_ptr<const LatePoses::FramePlan> plan;
			FrameLeases frameLeases, jobLeases;
		} late;
		std::vector<org::services::BackingLease> noRegistered;
		BoundPoints boundPoints;
		using ResourceOwners = std::vector<std::shared_ptr<const void>>;
		std::shared_ptr<ResourceOwners> frameFieldOwners;
		std::deque<std::pair<std::uint64_t, std::shared_ptr<ResourceOwners>>> retiredFieldOwners;

		void RetireFieldOwners()
		{
			if (frameFieldOwners)
				retiredFieldOwners.emplace_back(::RenderGraph::Frame::Retirement::SubmittedPoint(), std::move(frameFieldOwners));
			const auto completed = ::RenderGraph::Frame::Retirement::Clock()->Completed();
			while (!retiredFieldOwners.empty() && retiredFieldOwners.front().first <= completed)
				retiredFieldOwners.pop_front();
		}

		// Producer-owned: references follow the CPU rows and each backing's last successful upload.
		// A failed upload can draw the old backing, so its descriptors must remain alive as well.
		std::unordered_map<std::uint64_t, std::shared_ptr<const void>> fieldOwners;
		std::vector<ShadingFieldValue> pendingFieldValues;
		// Past this many jobs without an upload a backing has left the rotation: more than the frames in flight and their copies.
		static constexpr std::uint64_t kIdleBackingJobs = 16;
		struct BackingOwners
		{
			std::weak_ptr<org::services::Backing> backing;
			std::shared_ptr<const ResourceOwners> resources;
			std::uint64_t seq = 0;
		};
		std::unordered_map<std::uint64_t, BackingOwners> backingFieldOwners;

		std::unique_ptr<Registered[]> registered;
		std::size_t registeredCount = 0;
		std::vector<org::services::BackingLease> registeredFrameLeases;
		std::vector<std::uint32_t> registeredIndex;
		// Made once, on whichever thread first needs them. A pass's declaration may run on the graph host's thread.
		std::vector<std::shared_ptr<org::VersionedBuffer>> lateBuffers;
		std::vector<std::shared_ptr<RenderGraph::RefLatch>> poolRefLatches;
		std::once_flag lateBuffersMade;

		const std::vector<std::shared_ptr<org::VersionedBuffer>>& LateBuffers()
		{
			std::call_once(lateBuffersMade, [this] {
				const auto count = kBuiltInPools + Extensions::RegisteredPools().size();
				for (std::size_t i = 0; i < count; ++i) {
					auto standIn = org::Buffer::CreateUnmaterializedStructuredBuffer(1, 4, false);
					standIn->SetName(fmt::format("cs.dclf.frame-values.{}.stand-in", i));
					standIn->Materialize();
					lateBuffers.push_back(org::VersionedBuffer::CreateLateBound(std::move(standIn), org::VersionedBuffer::LateReady::FrameWait));
				}
				for (std::size_t i = kBuiltInPools; i < count; ++i)
					poolRefLatches.push_back(std::make_shared<RenderGraph::RefLatch>());
			});
			return lateBuffers;
		}

		/** @brief Publish a lease's backing as a row set's version. The version holds no owner, or the backing could never return to its pool. */
		void PublishFrameVersion(std::size_t a_set, const org::services::BackingLease& a_lease)
		{
			const auto& buffers = LateBuffers();
			if (a_set < buffers.size() && a_lease && a_lease->buffer)
				buffers[a_set]->PublishLate(a_lease->buffer);
		}
		std::shared_ptr<const Plan> plan;
		std::uint64_t planSerial = 0;
		ankerl::unordered_dense::map<const void*, Plan::Item> rootViewMovers;
		std::vector<std::shared_ptr<const Plan>> unsampled;
		std::vector<ShadingItem> unsampledShading;
		/**
		 * @brief Shading items whose lease was refused, handed back to the next Kick. A slot is named only once, so a dropped item would
		 * leave its row zero for good and a blended decal would draw nothing.
		 */
		struct RefusedShading
		{
			std::vector<ShadingItem> items;
			RefusedShading* next = nullptr;
		};
		std::atomic<RefusedShading*> refusedShading{ nullptr };
		void PushRefusedShading(std::vector<ShadingItem> a_items)
		{
			auto* node = new RefusedShading{ std::move(a_items), refusedShading.load(std::memory_order_relaxed) };
			while (!refusedShading.compare_exchange_weak(node->next, node, std::memory_order_release, std::memory_order_relaxed)) {
			}
		}
		std::vector<ShadingItem> TakeRefusedShading()
		{
			std::vector<RefusedShading*> nodes;
			for (auto* node = refusedShading.exchange(nullptr, std::memory_order_acquire); node; node = node->next)
				nodes.push_back(node);
			std::vector<ShadingItem> items;
			for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
				items.insert(items.end(), std::make_move_iterator((*it)->items.begin()), std::make_move_iterator((*it)->items.end()));
				delete *it;
			}
			return items;
		}
		~Impl() { (void)TakeRefusedShading(); }
		std::vector<ShadingFieldValue> unsampledFieldValues;
		// By sampling job: the plans it applies, held until it is done. Game half.
		std::deque<std::pair<std::uint64_t, std::shared_ptr<const Plan>>> plans;
		// The render's upload job: its sequence is the GPU's frame wait value. Render thread.
		std::uint64_t seq = 0;
		bool kicked = false;
		// The sampling jobs, one per ticket. Game half.
		std::uint64_t sampleSeq = 0;
		std::atomic<std::uint64_t> sampleKicked{ 0 };
		// Sampling and upload jobs run one at a time in kick order on one chain, so the kept rows need no lock.
		std::atomic<std::uint64_t> chainKicked{ 0 }, chainDone{ 0 };
		// The rows the built-in backings must hold, raised by each sampling kick. The kept arrays never shrink, so neither do these.
		std::array<std::atomic<std::uint64_t>, kBuiltInPools> neededRows{};
		// Chain: the floors the last upload leased at, which the next sampling jobs open their builds at. Floors only rise, so an older one
		// trims less and stays safe.
		std::array<std::optional<std::uint64_t>, kBuiltInPools> sampleFloors;
		std::uint64_t pendingSampleUs = 0;  // chain: the sampling time since the last upload
		bool unavailableLogged = false;

		KeptWholeArray<BindlessPlacement> rows;
		KeptWholeArray<PaletteRow> palettes;
		KeptWholeArray<BindlessShading> shading;
		// The render's views of them, taken by its upload job before done. The render thread reads them once done reaches seq and lets
		// them go at EndFrame; the next upload job replaces them only after the next Kick.
		org::async::KeptElements<BindlessPlacement> renderRows;
		org::async::KeptElements<PaletteRow> renderPalettes;
		org::async::KeptElements<BindlessShading> renderShading;
		// A registered pool's producer reads its frame's publication only while its Kick is the newest.
		std::atomic<std::uint64_t> kickedSeq{ 0 };
		std::vector<std::uint8_t> listed;
		std::vector<std::uint8_t> blockListed;
		std::vector<std::uint8_t> shadingListed;
		// Written slots whose previous transform still differs from the current. Resampled until equal.
		std::vector<Plan::Item> settling;
		std::vector<std::uint32_t> listedSlots, listedBlocks;
		std::vector<const void*> rowOwner;
		std::vector<std::array<float, 4>> lastSphere;
		std::vector<std::uint64_t> sphereSeq;
		std::atomic<bool> spheresWanted{ false };
		::RenderGraph::Frame::LatestSlot<Direct::SnapshotSpheres> spheresSlot;
		std::atomic<bool> streamSpheresWanted{ false };
		::RenderGraph::Frame::LatestSlot<Direct::SnapshotSpheres> streamSpheresSlot;

		// The moved-only sampling. Plans are applied in order as deltas onto kept movers and roots, indexed by move key.
		// Each keeps engine references that go to the render thread when it leaves.
		struct KeptMover
		{
			Plan::Item item;
			std::vector<const void*> keys;
			bool live = false, keyed = false, everyFrame = false;
		};
		struct KeptRoot
		{
			RE::NiPointer<RE::NiAVObject> root;
			std::vector<Plan::Item> dependents;
			ankerl::unordered_dense::map<const void*, std::uint32_t> dependentAt;
			std::vector<const void*> keys;
			bool live = false, keyed = false;
		};
		std::vector<KeptMover> keptMovers;
		std::vector<KeptRoot> keptRoots;
		std::vector<std::uint32_t> freeMovers, freeRoots;
		ankerl::unordered_dense::map<const void*, std::uint32_t> moverAt, rootAt;
		std::size_t liveMovers = 0, liveRoots = 0;
		struct Keyed
		{
			std::vector<std::uint32_t> movers, roots;
		};
		ankerl::unordered_dense::map<const void*, Keyed> keyIndex;
		ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> slotRoots;
		// Movers and roots due every frame: those a refused lease left unkeyed, and palette rows class items.
		std::vector<std::uint32_t> unkeyedMovers, unkeyedRoots, everyFrameMovers;
		std::vector<std::uint32_t> newMovers, newRoots;  // due once
		std::atomic<std::uint64_t> plansApplied{ 0 }, planResets{ 0 }, planItems{ 0 };
		void ApplyPlan(const Plan& a_plan);
		void ClearKept();
		void KeyMover(std::uint32_t a_mover);
		void UnkeyMover(std::uint32_t a_mover);
		void KeyRoot(std::uint32_t a_root);
		void UnkeyRoot(std::uint32_t a_root);
		void UpsertMover(const Plan::Item& a_item);
		void RemoveMover(const void* a_geometry);
		void UpsertRoot(const Plan::Root& a_root);
		void RemoveRoot(const void* a_node);
		void RemoveDependent(const void* a_node, const void* a_geometry);
		void ListSlotRoot(const Plan::Item& a_item, std::uint32_t a_root);
		void UnlistSlotRoot(const Plan::Item& a_item, std::uint32_t a_root);
		std::vector<const void*> drainedKeys;
		std::vector<const void*> lastKeys;  // the gate is this frame or the last
		// Keys due at the next producer, because their sample changed a row and must settle, or their lease was refused.
		std::vector<const void*> owedKeys;
		std::vector<RE::NiPointer<RE::NiAVObject>> owedNodes;
		std::vector<std::uint8_t> moverDue, rootDue;
		std::vector<std::uint32_t> dueMovers, dueRoots;
		std::vector<const void*> watchedList;
		ankerl::unordered_dense::set<const void*> watchedPartitions;
		std::atomic<std::uint64_t> done{ 0 };
		// The newest producer's signal. The render thread reads it once done reaches the frame's sequence.
		std::shared_ptr<org::TrackedUploadTicket> signal;
		std::uint64_t presentWaitSeq = 0;
		std::atomic<std::uint64_t> sampledSeq{ 0 };
		void MarkSampled(std::uint64_t a_seq)
		{
			sampledSeq.store(a_seq, std::memory_order_release);
			sampledSeq.notify_all();
		}

		// Since the last report.
		std::atomic<std::uint64_t> frames{ 0 }, sampled{ 0 }, rowsChanged{ 0 }, skins{ 0 }, paletteRowsChanged{ 0 }, shadingSampled{ 0 }, shadingChanged{ 0 },
			wetnessChanged{ 0 }, defects{ 0 }, runs{ 0 }, bytes{ 0 }, refused{ 0 }, failures{ 0 }, settled{ 0 }, seeded{ 0 };
		std::atomic<std::uint64_t> movers{ 0 }, moversDue{ 0 }, roots{ 0 }, rootsDue{ 0 }, moveKeys{ 0 }, moveNodes{ 0 };
		std::atomic<std::uint64_t> sampleUs{ 0 }, uploadUs{ 0 }, maxTotalUs{ 0 };
		std::atomic<std::uint64_t> reusedInFlight{ 0 };

		/** @brief A first-person member's rows and palettes, sampled at Kick before Main::Draw moves the skeleton into its root's space. */
		struct RootViewSample
		{
			std::uint32_t slot = kNoObjectSlot, layerSlot = kNoObjectSlot, boneOffset = 0, boneRows = 0;
			BindlessPlacement row{}, layer{};
			std::vector<PaletteRow> current, previous;
		};

		struct Job
		{
			bool upload = false;      // the render's upload job, else a ticket's sampling job
			std::uint64_t chain = 0;  // its place on the chain
			std::uint64_t seq = 0;    // the upload's frame wait value, or the sampling job's own sequence
			const Plan* plan = nullptr;
			std::uint64_t planSerial = 0;
			std::vector<const Plan*> written;
			std::vector<ShadingItem> shadingItems;
			std::vector<ShadingFieldValue> fieldValues;
			std::shared_ptr<ResourceOwners> fieldResources;
			// Shading rows cover the plan's slots or the newest slot an item names. A slot can bind before a plan covers it, and a dropped
			// item is never named again, so its row would stay zero.
			std::size_t shadingSlots = 0;
			mutable std::vector<std::size_t> refusedShading;
			std::shared_ptr<const FrameGlobals> globals;
			// The trim floor is the oldest content a backing in circulation held at Kick. nullopt means the journal keeps nothing.
			FrameLeases leases;
			std::array<std::optional<std::uint64_t>, kBuiltInPools> oldestContentEpochs;
			// The GPU point uploads wait for when a leased backing is still bound by a frame in flight. Empty means none.
			org::GraphHost::GpuPoint writeAfter;
			struct RegisteredPart
			{
				std::shared_ptr<const void> input;
				org::services::BackingLease lease;
				std::optional<std::uint64_t> oldestContentEpoch;
			};
			std::vector<RegisteredPart> registered;
			std::shared_ptr<rhi::TimelinePtr> timeline;
			std::shared_ptr<org::runtime::IUploadService> uploads;
			std::vector<RootViewSample> rootViewSamples;
			// By class index - 1.
			std::array<Api::PaletteRowsInput, Api::kPaletteRowsClasses> paletteInputs{};
			std::shared_ptr<const Direct::ViewBatchSet> viewBatches;
			std::vector<Direct::ForwardSortRow>* forwardRows = nullptr;  // the frame slot's, which no one else touches until done
			std::shared_ptr<const LatePoses::FramePlan> latePlan;
			FrameLeases lateLeases;
		};
		// The late copies' rows and the ranges they overrode in the last send. The next build marks those ranges changed.
		LatePoses::Rows lateRows;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> lateDirtyPlacements, lateDirtyPalettes;
		std::vector<org::StreamingUploadRange> overrideRanges, splicedRanges;
		void BuildLateRows(const Job& a_job);
		void MarkLateDirty();
		struct Listed
		{
			const Plan::Item* item;
			bool palette;
			bool settles = false;
			bool mover = false;  // due again next frame when its sample changed something
			bool continued = false;  // the slot's row was last sampled for the same geometry
		};
		static constexpr std::size_t kSampleGrain = 64;
		// A particle system item also counts its rows, so one with 100 particles costs about 20 plain items.
		static constexpr std::size_t kSampleChunkCost = 64;
		static constexpr std::size_t kSampleRowsPerUnit = 24;
		// Per chunk, marked afterwards because the journals are single-threaded.
		struct Changed
		{
			std::vector<std::uint32_t> slots;
			std::vector<std::pair<std::uint32_t, std::uint32_t>> paletteRuns;  // (first row, rows)
			std::vector<const Plan::Item*> settling;
			std::vector<const void*> owed;
			std::uint64_t skins = 0, defects = 0;
			void Clear()
			{
				slots.clear();
				paletteRuns.clear();
				settling.clear();
				owed.clear();
				skins = defects = 0;
			}
		};
		// Producer scratch kept across frames so nothing allocates per frame once grown.
		std::vector<Listed> sampleItems;
		std::vector<Changed> sampleChanged;
		std::vector<std::size_t> sampleBounds;
		std::vector<const ShadingItem*> shadingItemsScratch;
		std::vector<org::StreamingUploadRange> uploadRanges;
		// Written from many threads after the one Mutable() call each.
		struct SampleOut
		{
			std::vector<BindlessPlacement>& out;
			std::vector<PaletteRow>& paletteOut;
		};
		struct PaletteRun
		{
			const Extensions::PaletteRowsEntry* entry = nullptr;
			std::vector<Api::PaletteRowsItem> items;
		};
		using PaletteRuns = std::array<PaletteRun, Api::kPaletteRowsClasses>;
		struct Deferred
		{
			const Listed* listed = nullptr;
			std::size_t slotsAt = 0, paletteRunsAt = 0, owedAt = 0;
			std::uint32_t paletteClass = 0, write = 0, paletteAt = 0;
			void* state = nullptr;
			bool layerChanged = false;
			BindlessPlacement row{};
		};
		struct DeferredChunk
		{
			std::array<Deferred, kSampleGrain> items;
			std::array<Api::PaletteRowsWrite, kSampleGrain> writes;
			std::uint32_t count = 0, rowCount = 0;
			std::vector<PaletteRow> rows;
		};
		struct ItemScratch
		{
			BindlessPlacement row, layer;
			DeferredChunk* deferred = nullptr;
			std::array<std::size_t, Api::kPaletteRowsClasses> cursors{};
		};
		void ListItem(const Plan::Item& a_item, bool a_settles, bool a_mover, std::size_t a_rows, std::size_t a_blocks, std::vector<Listed>& a_items);
		void MarkSampledRoots();
		void ApplyRootViewSamples(const Job& a_job, const SampleOut& a_out, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked);
		static void SampleBounds(const std::vector<Listed>& a_items, std::vector<std::size_t>& a_bounds);
		void SampleItems(const Job& a_job, const std::vector<Listed>& a_items, const SampleOut& a_out, std::vector<Changed>& a_changed,
			std::atomic<std::uint64_t>& a_refused);
		static void BeginPaletteRuns(const Job& a_job, const std::vector<Listed>& a_items, PaletteRuns& a_runs);
		static void SampleChunk(const Job& a_job, const std::vector<Listed>& a_items, const PaletteRuns& a_runs, std::size_t a_begin, std::size_t a_end,
			const SampleOut& a_out, Changed& a_mine, std::atomic<std::uint64_t>& a_refused);
		static void SampleListed(const Listed& a_listed, const PaletteRuns& a_runs, const SampleOut& a_out, Changed& a_mine, ItemScratch& a_scratch,
			bool a_leased, std::atomic<std::uint64_t>& a_refused);
		static Deferred& Defer(const Listed& a_listed, std::uint32_t a_class, const Api::PaletteRowsItem& a_classItem, ItemScratch& a_scratch);
		static void WriteDeferred(const Job& a_job, const PaletteRuns& a_runs, DeferredChunk& a_deferred);
		static void FinishDeferred(const SampleOut& a_out, Changed& a_mine, DeferredChunk& a_deferred);
		static bool SamplePalette(const Plan::Item& a_item, const SampleOut& a_out, Changed& a_mine);
		void SampleRoots(const SampleOut& a_out, std::vector<std::vector<std::uint32_t>>& a_rootChanged, std::vector<std::uint8_t>& a_rootRefused,
			std::atomic<std::uint64_t>& a_refused);
		void MarkSampled(const std::vector<Changed>& a_changed, const std::vector<std::vector<std::uint32_t>>& a_rootChanged,
			const std::vector<std::uint8_t>& a_rootRefused, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked);
		// Render thread.
		static void FrameStartRegistered(const std::shared_ptr<const FrameGlobals>& a_globals);
		void ApplyRootViewPlans(const std::vector<std::shared_ptr<const Plan>>& a_plans);
		void QueuePlans(std::vector<std::shared_ptr<const Plan>>& a_plans);
		void LeaseBackings(Job& a_job);
		void ReusedCopyPoint(std::uint64_t a_writeAfter, Job& a_job);
		/** @brief The GPU point a reused backing bound at clock point a_writeAfter waits for (empty when it is reusable now). */
		org::GraphHost::GpuPoint BoundGpuPoint(std::uint64_t a_writeAfter) const;
		void ReleaseLatePoses()
		{
			late.jobLeases = {};
			BindFrameLeases(late.frameLeases, noRegistered, boundPoints);
		}
		// Returns the GPU point the lease's copy is still bound to, or 0 if reusable now.
		std::uint64_t LeasePool(Pool& a_pool, std::string_view a_name, std::uint32_t a_stride, org::services::BackingFlags a_flags, std::uint64_t a_needed, org::services::BackingLease& a_frameLease,
			org::services::BackingLease& a_jobLease, std::optional<std::uint64_t>& a_oldestContentEpoch, bool a_fresh = false);
		void MakeRegistered();
		void KickRegistered(Job& a_job, const FrameGlobals* a_globals, bool a_perRender);
		void SampleRootViews(const std::vector<std::shared_ptr<const Plan>>& a_plans, const SetSnapshot& a_claims, std::vector<RootViewSample>& a_out);
		void WaitTurn(std::uint64_t a_chain);
		void EndTurn(std::uint64_t a_chain);
		void RunSample(Job& a_job);
		void RunUpload(Job& a_job);
		void Sample(const Job& a_job);
		void ApplyPlans(const Job& a_job);
		void TrackSpheres(std::uint64_t a_seq, const std::vector<BindlessPlacement>& a_out);
		void PostSnapshotSpheres(const Job& a_job, const std::vector<BindlessPlacement>& a_out);
		static void StoreLiveSpheres(const Job& a_job, const std::vector<BindlessPlacement>& a_out);
		void StoreForwardRows(const Job& a_job, const std::vector<BindlessPlacement>& a_out);
		// The forward rows the last producer filled, read by the render thread once done reaches forwardRowsSeq.
		const std::vector<Direct::ForwardSortRow>* forwardRowsTarget = nullptr;
		std::uint64_t forwardRowsSeq = 0;
		void MarkMover(std::uint32_t a_mover);
		void MarkRoot(std::uint32_t a_root);
		void MarkDue(const Job& a_job);
		void SampleShadingRows(const Job& a_job, std::size_t a_slots);
		enum class Pools
		{
			GameSide,
			RenderSide,
			All
		};
		template <class Call>
		void ForEachRegistered(const Job& a_job, Pools a_pools, Call&& a_call);
		void Upload(const Job& a_job);
		template <class T>
		void Send(const Job& a_job, Part a_part, const KeptWholeArray<T>& a_rows);
		// a_overrides are sorted, disjoint ranges sent instead of the rows there.
		void SendRows(const org::services::BackingLease& a_lease, Pool& a_pool, std::string_view a_name, const Job& a_job, std::size_t a_count,
			std::uint64_t a_contentEpoch, const void* a_view, Api::FrameValuePoolProducer::EmitRowsFn a_emit,
			std::span<const org::StreamingUploadRange> a_overrides = {});
		template <class T>
		void SendLate(const Job& a_job, Part a_part, const KeptWholeArray<T>& a_rows, std::span<const org::StreamingUploadRange> a_overrides);
		template <class T>
		static void BeginBuild(const Job& a_job, Part a_part, KeptWholeArray<T>& a_rows)
		{
			a_rows.BeginBuild(a_job.oldestContentEpochs[a_part] ? *a_job.oldestContentEpochs[a_part] : a_rows.ContentEpoch());
		}
	};

	void FrameValues::Impl::WaitTurn(std::uint64_t a_chain)
	{
		// One job at a time, in kick order. Each builds on the rows the previous one left, and upload signals rise monotonically.
		for (auto finished = chainDone.load(std::memory_order_acquire); finished + 1 < a_chain; finished = chainDone.load(std::memory_order_acquire))
			chainDone.wait(finished, std::memory_order_acquire);
	}

	void FrameValues::Impl::EndTurn(std::uint64_t a_chain)
	{
		chainDone.store(a_chain, std::memory_order_release);
		chainDone.notify_all();
	}

	namespace
	{
		/** @brief Logs a wait of 100 ms or more, at most once a second. The game's animation jobs wait for every sampling job. */
		void NoteLongWait(std::chrono::steady_clock::time_point a_start, const char* a_what, std::uint64_t a_seq)
		{
			const auto now = std::chrono::steady_clock::now();
			const auto ms = std::chrono::duration<double, std::milli>(now - a_start).count();
			if (ms < 100.0)
				return;
			static std::atomic<std::int64_t> last{ 0 };
			const auto tick = std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
			if (auto seen = last.load(std::memory_order_relaxed); tick != seen && last.compare_exchange_strong(seen, tick, std::memory_order_relaxed))
				logger::warn("[DCLF] frame values job {}: {} took {:.0f} ms (the sampling jobs behind it, and the engine's animation jobs that wait for them, wait too)",
					a_seq, a_what, ms);
		}
	}

	void FrameValues::Impl::RunSample(Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.SampleJob");
		const auto queued = std::chrono::steady_clock::now();
		WaitTurn(a_job.chain);
		NoteLongWait(queued, "the sampling job's wait for its chain turn", a_job.seq);
		pendingFieldValues.insert(pendingFieldValues.end(), a_job.fieldValues.begin(), a_job.fieldValues.end());
		// The builds open at the floors the last upload leased at.
		a_job.oldestContentEpochs = sampleFloors;
		for (std::size_t i = 0; i < a_job.registered.size(); ++i)
			a_job.registered[i].oldestContentEpoch = registered[i].sampleFloor;
		const auto start = std::chrono::steady_clock::now();
		try {
			Sample(a_job);
		} catch (const std::exception& e) {
			if (failures++ == 0)
				logger::error("[DCLF] frame values' sampling {} failed: {}; the next upload sends the rows as they are", a_job.seq, e.what());
		}
		MarkSampled(a_job.seq);
		const auto us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		sampleUs += us;
		pendingSampleUs += us;
		EndTurn(a_job.chain);
	}

	void FrameValues::Impl::RunUpload(Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues");
		WaitTurn(a_job.chain);
		// It reads only sampled rows and render-side state: a lease asked for here is counted as a defect.
		const EngineReadWindow::RenderSideJob renderSide;
		sampleFloors = a_job.oldestContentEpochs;
		for (std::size_t i = 0; i < a_job.registered.size(); ++i)
			registered[i].sampleFloor = a_job.registered[i].oldestContentEpoch;
		std::erase_if(backingFieldOwners, [](const auto& entry) { return entry.second.backing.expired(); });
		// A backing the pool stopped handing out is nobody's fallback, so release its descriptors' imports now.
		for (auto& [generation, entry] : backingFieldOwners)
			if (entry.seq + kIdleBackingJobs < a_job.seq)
				entry.resources.reset();
		if (const auto previous = backingFieldOwners.find(a_job.leases[kShading]->generation); previous != backingFieldOwners.end())
			a_job.fieldResources->push_back(previous->second.resources);
		const auto start = std::chrono::steady_clock::now();
		try {
			// The late copies' rows of the last upload go back to the plain rows in every backing that held them.
			MarkLateDirty();
			const auto& out = rows.Get();
			StoreLiveSpheres(a_job, out);
			StoreForwardRows(a_job, out);
			PostSnapshotSpheres(a_job, out);
			{
				DCLF_ZONE("CS.DCLF.FV.S.OnSampled");
				Direct::OnFrameValuesSampled(a_job.seq, std::span<const BindlessPlacement>(out));
			}
			{
				DCLF_ZONE("CS.DCLF.FV.S.RenderSidePools");
				ForEachRegistered(a_job, Pools::RenderSide, [](const Extensions::FrameValuePoolEntry& a_entry, Api::FrameValuePoolProducer& a_producer) { a_entry.sample(a_producer); });
			}
			if (a_job.latePlan)
				BuildLateRows(a_job);
			// Uploads into reused copies wait on the GPU, not here. Resolve waits for the submission thread to pass the point.
			const auto resolveStart = std::chrono::steady_clock::now();
			const auto writeAfter = a_job.writeAfter.Resolve();
			NoteLongWait(resolveStart, "the upload's wait in its chain turn for the submission thread to pass its write-after point", a_job.seq);
			if (!writeAfter.empty() && !a_job.uploads->QueueStreamingWait(writeAfter))
				throw std::runtime_error("the dedicated uploader cannot wait for the frame its copies were bound to");
			Upload(a_job);
			uploadUs += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		} catch (const std::exception& e) {
			if (failures++ == 0)
				logger::error("[DCLF] frame values {} failed: {}; the frame draws the rows its buffers held", a_job.seq, e.what());
		}
		// Always signal. Every batch of the frame waits for it.
		signal = a_job.uploads ? a_job.uploads->QueueStreamingSignal(a_job.timeline, a_job.seq) : nullptr;
		if (!signal)
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("frame values {} could not signal the frame's wait; the GPU would wait for it forever", a_job.seq));
		const auto total = std::exchange(pendingSampleUs, 0) +
		                   static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count());
		for (auto max = maxTotalUs.load(std::memory_order_relaxed); total > max && !maxTotalUs.compare_exchange_weak(max, total, std::memory_order_relaxed);) {}
		++frames;
		renderRows = rows.Shared();
		renderPalettes = palettes.Shared();
		renderShading = shading.Shared();
		done.store(a_job.seq, std::memory_order_release);
		done.notify_all();
		EndTurn(a_job.chain);
	}

	namespace
	{
		// A producer never drops an engine reference. They go to the render thread.
		void ReleaseItem(Plan::Item& a_item)
		{
			ReleaseLater(a_item.geometry);
			ReleaseLater(a_item.listedRoot);
		}

		template <class Index>
		void EraseIndex(std::vector<Index>& a_list, Index a_index)
		{
			if (const auto at = std::find(a_list.begin(), a_list.end(), a_index); at != a_list.end()) {
				*at = a_list.back();
				a_list.pop_back();
			}
		}
	}

	void FrameValues::Impl::ApplyPlans(const Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.ApplyPlans");
		for (const auto* written : a_job.written)
			ApplyPlan(*written);
	}

	void FrameValues::Impl::ApplyPlan(const Plan& a_plan)
	{
		++plansApplied;
		if (a_plan.reset) {
			++planResets;
			ClearKept();
		}
		planItems += a_plan.moversLeft.size() + a_plan.rootsLeft.size() + a_plan.dependentsLeft.size() + a_plan.movers.size() + a_plan.roots.size();
		for (const void* geometry : a_plan.moversLeft)
			RemoveMover(geometry);
		for (const void* node : a_plan.rootsLeft)
			RemoveRoot(node);
		for (const auto& [node, geometry] : a_plan.dependentsLeft)
			RemoveDependent(node, geometry);
		for (const auto& item : a_plan.movers)
			UpsertMover(item);
		for (const auto& root : a_plan.roots)
			UpsertRoot(root);
	}

	void FrameValues::Impl::ClearKept()
	{
		for (auto& mover : keptMovers)
			ReleaseItem(mover.item);
		for (auto& root : keptRoots) {
			ReleaseLater(root.root);
			for (auto& dependent : root.dependents)
				ReleaseItem(dependent);
		}
		keptMovers.clear();
		keptRoots.clear();
		freeMovers.clear();
		freeRoots.clear();
		moverAt.clear();
		rootAt.clear();
		liveMovers = liveRoots = 0;
		keyIndex.clear();
		slotRoots.clear();
		unkeyedMovers.clear();
		unkeyedRoots.clear();
		everyFrameMovers.clear();
		newMovers.clear();
		newRoots.clear();
	}

	void FrameValues::Impl::KeyMover(std::uint32_t a_mover)
	{
		auto& mover = keptMovers[a_mover];
		// Palette rows class items, such as camera-facing particles, are due every frame with no keys.
		if (mover.item.itemClass != Api::FrameValueItemClass::None) {
			mover.everyFrame = true;
			everyFrameMovers.push_back(a_mover);
			return;
		}
		if (mover.item.firstPerson)
			return;
		EngineReadWindow::Lease lease;
		if (!lease) {
			unkeyedMovers.push_back(a_mover);
			return;
		}
		KeysOf(*mover.item.geometry, mover.keys);
		for (const void* key : mover.keys)
			keyIndex[key].movers.push_back(a_mover);
		mover.keyed = true;
	}

	void FrameValues::Impl::UnkeyMover(std::uint32_t a_mover)
	{
		auto& mover = keptMovers[a_mover];
		if (std::exchange(mover.everyFrame, false))
			EraseIndex(everyFrameMovers, a_mover);
		if (std::exchange(mover.keyed, false))
			for (const void* key : mover.keys)
				if (const auto it = keyIndex.find(key); it != keyIndex.end()) {
					EraseIndex(it->second.movers, a_mover);
					if (it->second.movers.empty() && it->second.roots.empty())
						keyIndex.erase(it);
				}
		mover.keys.clear();
	}

	void FrameValues::Impl::KeyRoot(std::uint32_t a_root)
	{
		auto& root = keptRoots[a_root];
		EngineReadWindow::Lease lease;
		if (!lease) {
			unkeyedRoots.push_back(a_root);
			return;
		}
		KeysOf(*root.root, root.keys);
		for (const void* key : root.keys)
			keyIndex[key].roots.push_back(a_root);
		root.keyed = true;
	}

	void FrameValues::Impl::UnkeyRoot(std::uint32_t a_root)
	{
		auto& root = keptRoots[a_root];
		if (std::exchange(root.keyed, false))
			for (const void* key : root.keys)
				if (const auto it = keyIndex.find(key); it != keyIndex.end()) {
					EraseIndex(it->second.roots, a_root);
					if (it->second.movers.empty() && it->second.roots.empty())
						keyIndex.erase(it);
				}
		root.keys.clear();
	}

	void FrameValues::Impl::UpsertMover(const Plan::Item& a_item)
	{
		if (!a_item.geometry)
			return;
		std::uint32_t m;
		if (const auto it = moverAt.find(a_item.geometry.get()); it != moverAt.end()) {
			m = it->second;
			UnkeyMover(m);
			ReleaseItem(keptMovers[m].item);
		} else {
			if (!freeMovers.empty()) {
				m = freeMovers.back();
				freeMovers.pop_back();
			} else {
				m = static_cast<std::uint32_t>(keptMovers.size());
				keptMovers.emplace_back();
			}
			moverAt.emplace(a_item.geometry.get(), m);
			keptMovers[m].live = true;
			++liveMovers;
		}
		keptMovers[m].item = a_item;
		if (moverDue.size() < keptMovers.size())
			moverDue.resize(keptMovers.size(), 0);
		KeyMover(m);
		newMovers.push_back(m);
	}

	void FrameValues::Impl::RemoveMover(const void* a_geometry)
	{
		const auto it = moverAt.find(a_geometry);
		if (it == moverAt.end())
			return;
		const std::uint32_t m = it->second;
		moverAt.erase(it);
		UnkeyMover(m);
		auto& mover = keptMovers[m];
		ReleaseItem(mover.item);
		mover.item = {};
		mover.live = false;
		freeMovers.push_back(m);
		--liveMovers;
	}

	void FrameValues::Impl::ListSlotRoot(const Plan::Item& a_item, std::uint32_t a_root)
	{
		for (const std::uint32_t slot : { a_item.slot, a_item.layerSlot })
			if (slot != kNoObjectSlot) {
				auto& rootsOf = slotRoots[slot];
				if (std::find(rootsOf.begin(), rootsOf.end(), a_root) == rootsOf.end())
					rootsOf.push_back(a_root);
			}
	}

	void FrameValues::Impl::UnlistSlotRoot(const Plan::Item& a_item, std::uint32_t a_root)
	{
		for (const std::uint32_t slot : { a_item.slot, a_item.layerSlot })
			if (const auto it = slot != kNoObjectSlot ? slotRoots.find(slot) : slotRoots.end(); it != slotRoots.end()) {
				EraseIndex(it->second, a_root);
				if (it->second.empty())
					slotRoots.erase(it);
			}
	}

	void FrameValues::Impl::UpsertRoot(const Plan::Root& a_root)
	{
		std::uint32_t r;
		if (const auto it = rootAt.find(a_root.root.get()); it != rootAt.end()) {
			r = it->second;
		} else {
			// The coordinator counts this defect. Leave the root and its dependents out.
			if (!a_root.root)
				return;
			if (!freeRoots.empty()) {
				r = freeRoots.back();
				freeRoots.pop_back();
			} else {
				r = static_cast<std::uint32_t>(keptRoots.size());
				keptRoots.emplace_back();
			}
			rootAt.emplace(a_root.root.get(), r);
			auto& root = keptRoots[r];
			root.root = a_root.root;
			root.live = true;
			++liveRoots;
			if (rootDue.size() < keptRoots.size())
				rootDue.resize(keptRoots.size(), 0);
			KeyRoot(r);
		}
		auto& root = keptRoots[r];
		for (const auto& dependent : a_root.dependents) {
			if (!dependent.geometry)
				continue;
			if (const auto at = root.dependentAt.find(dependent.geometry.get()); at != root.dependentAt.end()) {
				auto& kept = root.dependents[at->second];
				UnlistSlotRoot(kept, r);
				ReleaseItem(kept);
				kept = dependent;
			} else {
				root.dependentAt.emplace(dependent.geometry.get(), static_cast<std::uint32_t>(root.dependents.size()));
				root.dependents.push_back(dependent);
			}
			ListSlotRoot(dependent, r);
		}
		newRoots.push_back(r);
	}

	void FrameValues::Impl::RemoveDependent(const void* a_node, const void* a_geometry)
	{
		const auto it = rootAt.find(a_node);
		if (it == rootAt.end())
			return;
		const std::uint32_t r = it->second;
		auto& root = keptRoots[r];
		const auto at = root.dependentAt.find(a_geometry);
		if (at == root.dependentAt.end())
			return;
		const std::uint32_t index = at->second;
		root.dependentAt.erase(at);
		UnlistSlotRoot(root.dependents[index], r);
		ReleaseItem(root.dependents[index]);
		if (index + 1 != root.dependents.size()) {
			root.dependents[index] = std::move(root.dependents.back());
			root.dependentAt.insert_or_assign(root.dependents[index].geometry.get(), index);
		}
		root.dependents.pop_back();
		newRoots.push_back(r);
	}

	void FrameValues::Impl::RemoveRoot(const void* a_node)
	{
		const auto it = rootAt.find(a_node);
		if (it == rootAt.end())
			return;
		const std::uint32_t r = it->second;
		rootAt.erase(it);
		UnkeyRoot(r);
		auto& root = keptRoots[r];
		for (auto& dependent : root.dependents) {
			UnlistSlotRoot(dependent, r);
			ReleaseItem(dependent);
		}
		ReleaseLater(root.root);
		root = {};
		freeRoots.push_back(r);
		--liveRoots;
	}

	void FrameValues::Impl::MarkMover(std::uint32_t a_mover)
	{
		if (a_mover >= keptMovers.size() || !keptMovers[a_mover].live)
			return;
		if (!std::exchange(moverDue[a_mover], 1))
			dueMovers.push_back(a_mover);
	}

	void FrameValues::Impl::MarkRoot(std::uint32_t a_root)
	{
		if (a_root >= keptRoots.size() || !keptRoots[a_root].live)
			return;
		if (!std::exchange(rootDue[a_root], 1))
			dueRoots.push_back(a_root);
	}

	void FrameValues::Impl::MarkDue(const Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.Due");
		for (const auto m : dueMovers)
			moverDue[m] = 0;
		for (const auto r : dueRoots)
			rootDue[r] = 0;
		dueMovers.clear();
		dueRoots.clear();
		ApplyPlans(a_job);
		const auto markKey = [&](const void* a_key) {
			const auto it = keyIndex.find(a_key);
			if (it == keyIndex.end())
				return;
			for (const auto m : it->second.movers)
				MarkMover(m);
			for (const auto r : it->second.roots)
				MarkRoot(r);
		};
		// This frame's keys. For each node Havok wrote, key both the reference above it and the node itself.
		drainedKeys.clear();
		const void* last = nullptr;
		moveKeys += FrameValueMoves::keys.Drain([&](const void* a_key) {
			if (a_key != std::exchange(last, a_key))
				drainedKeys.push_back(a_key);
		});
		moveNodes += FrameValueMoves::nodes.Drain([&](RE::NiPointer<RE::NiAVObject>&& a_node) { owedNodes.push_back(std::move(a_node)); });
		// One lease for all of them. If refused, they wait for the next producer.
		if (!owedNodes.empty()) {
			EngineReadWindow::Lease lease;
			if (lease) {
				for (auto& owed : owedNodes) {
					const RE::NiAVObject* node = owed.get();
					drainedKeys.push_back(node);
					for (const RE::NiAVObject* at = node; at; at = at->parent)
						if (const void* owner = at->GetUserData()) {
							drainedKeys.push_back(owner);
							break;
						}
					ReleaseLater(owed);
				}
				owedNodes.clear();
			}
		}
		for (const void* key : drainedKeys)
			markKey(key);
		for (const void* key : lastKeys)
			markKey(key);
		for (const void* key : owedKeys)
			markKey(key);
		owedKeys.clear();
		std::swap(lastKeys, drainedKeys);
		for (const auto m : newMovers)
			MarkMover(m);
		for (const auto r : newRoots)
			MarkRoot(r);
		newMovers.clear();
		newRoots.clear();
		std::erase_if(unkeyedMovers, [&](std::uint32_t a_mover) {
			auto& mover = keptMovers[a_mover];
			if (!mover.live || mover.keyed)
				return true;
			MarkMover(a_mover);
			EngineReadWindow::Lease lease;
			if (!lease)
				return false;
			KeysOf(*mover.item.geometry, mover.keys);
			for (const void* key : mover.keys)
				keyIndex[key].movers.push_back(a_mover);
			mover.keyed = true;
			return true;
		});
		std::erase_if(unkeyedRoots, [&](std::uint32_t a_root) {
			auto& root = keptRoots[a_root];
			if (!root.live || root.keyed)
				return true;
			MarkRoot(a_root);
			EngineReadWindow::Lease lease;
			if (!lease)
				return false;
			KeysOf(*root.root, root.keys);
			for (const void* key : root.keys)
				keyIndex[key].roots.push_back(a_root);
			root.keyed = true;
			return true;
		});
		// RaceMenu may be replacing a skin partition. Every item holding it stays due for a few seconds so the palette sample sees the new one.
		if (SkinPartitionWatch::Any()) {
			SkinPartitionWatch::Collect(watchedList);
			watchedPartitions.clear();
			watchedPartitions.insert(watchedList.begin(), watchedList.end());
			const auto watches = [&](const Plan::Item& a_item) { return a_item.skinPartition && watchedPartitions.contains(a_item.skinPartition); };
			for (std::uint32_t m = 0; m < keptMovers.size(); ++m)
				if (keptMovers[m].live && watches(keptMovers[m].item))
					MarkMover(m);
			for (std::uint32_t r = 0; r < keptRoots.size(); ++r)
				if (keptRoots[r].live && std::ranges::any_of(keptRoots[r].dependents, watches))
					MarkRoot(r);
		}
		for (const auto m : everyFrameMovers)
			MarkMover(m);
		// Without the move writers' hooks nothing says what moved, so everything is due.
		if (!Scene::MoveEventsLive()) {
			for (std::uint32_t m = 0; m < keptMovers.size(); ++m)
				MarkMover(m);
			for (std::uint32_t r = 0; r < keptRoots.size(); ++r)
				MarkRoot(r);
		}
	}

	void FrameValues::Impl::Sample(const Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.Sample");
		const auto& work = *a_job.plan;
		TracyCZoneN(sBegin, "CS.DCLF.FV.S.BeginBuild", true);
		BeginBuild(a_job, kPlacements, rows);
		BeginBuild(a_job, kPalettes, palettes);
		auto& out = rows.Mutable();
		if (out.size() < work.slots) {
			const std::size_t first = out.size();
			out.resize(work.slots, BindlessPlacement{});
			rows.MarkRange(first, work.slots - first);
		}
		// Two row sets per block, current and previous.
		auto& paletteOut = palettes.Mutable();
		if (const std::size_t paletteRows = 2ull * work.boneCapacity; paletteOut.size() < paletteRows) {
			const std::size_t first = paletteOut.size();
			paletteOut.resize(paletteRows, PaletteRow{});
			palettes.MarkRange(first, paletteRows - first);
		}
		TracyCZoneEnd(sBegin);
		MarkDue(a_job);
		TracyCZoneN(sList, "CS.DCLF.FV.S.ListItems", true);
		auto& items = sampleItems;
		items.clear();
		items.reserve(dueMovers.size() + (a_job.written.empty() ? 0 : a_job.written.back()->written.size()) + settling.size());
		const std::size_t blocks = paletteOut.size() / 2;
		if (listed.size() < out.size())
			listed.resize(out.size(), 0);
		if (blockListed.size() < blocks)
			blockListed.resize(blocks, 0);
		const std::size_t slots = out.size();
		for (const auto m : dueMovers)
			ListItem(keptMovers[m].item, false, true, slots, blocks, items);
		for (auto it = a_job.written.rbegin(); it != a_job.written.rend(); ++it)
			for (const auto& item : (*it)->written)
				ListItem(item, true, false, slots, blocks, items);
		// Carried settling items go last because a slot listed above is newer. They may hold the last engine references.
		auto carried = std::move(settling);
		settling.clear();
		struct HandOver
		{
			std::vector<Plan::Item>& items;
			~HandOver()
			{
				for (auto& item : items) {
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.geometry)));
					EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.listedRoot)));
				}
			}
		} handOver{ carried };
		for (const auto& item : carried)
			ListItem(item, true, false, slots, blocks, items);
		TracyCZoneValue(sList, items.size());
		TracyCZoneEnd(sList);
		TracyCZoneN(sRoots0, "CS.DCLF.FV.S.MarkRootsAndRootView", true);
		MarkSampledRoots();
		const SampleOut sampleOut{ out, paletteOut };
		std::uint64_t rootViewMarked = 0, rootViewPaletteMarked = 0;
		ApplyRootViewSamples(a_job, sampleOut, rootViewMarked, rootViewPaletteMarked);
		auto& changed = sampleChanged;
		for (auto& chunk : changed)
			chunk.Clear();
		SampleBounds(items, sampleBounds);
		changed.resize(sampleBounds.size() - 1);
		std::atomic<std::uint64_t> refusedHere{ 0 };
		TracyCZoneEnd(sRoots0);
		TracyCZoneN(sItems, "CS.DCLF.FV.S.SampleItems", true);
		SampleItems(a_job, items, sampleOut, changed, refusedHere);
		TracyCZoneEnd(sItems);
		TracyCZoneN(sRoots, "CS.DCLF.FV.S.SampleRoots", true);
		TracyCZoneValue(sRoots, dueRoots.size());
		// Roots write their dependents' sun entries and fade nodes after the items.
		std::vector<std::vector<std::uint32_t>> rootChanged(dueRoots.size());
		std::vector<std::uint8_t> rootRefused(dueRoots.size(), 0);
		SampleRoots(sampleOut, rootChanged, rootRefused, refusedHere);
		TracyCZoneEnd(sRoots);
		TracyCZoneN(sMarked, "CS.DCLF.FV.S.MarkSampled", true);
		std::uint64_t marked = 0, paletteMarked = 0;
		MarkSampled(changed, rootChanged, rootRefused, marked, paletteMarked);
		sampled += static_cast<std::uint64_t>(items.size() + a_job.rootViewSamples.size());
		rowsChanged += marked + rootViewMarked;
		paletteRowsChanged += paletteMarked + rootViewPaletteMarked;
		movers += liveMovers;
		moversDue += dueMovers.size();
		roots += liveRoots;
		rootsDue += dueRoots.size();
		if (const auto refusals = refusedHere.load())
			refused += refusals;
		// Sampled for the next upload: done holds the last upload's sequence.
		TrackSpheres(done.load(std::memory_order_acquire) + 1, out);
		for (const auto slot : listedSlots)
			listed[slot] = 0;
		for (const auto block : listedBlocks)
			blockListed[block] = 0;
		listedSlots.clear();
		listedBlocks.clear();
		TracyCZoneEnd(sMarked);
		SampleShadingRows(a_job, a_job.shadingSlots);
		DCLF_ZONE_NAMED(sRegistered, "CS.DCLF.FV.S.Registered", true);
		ForEachRegistered(a_job, Pools::GameSide, [](const Extensions::FrameValuePoolEntry& a_entry, Api::FrameValuePoolProducer& a_producer) { a_entry.sample(a_producer); });
	}

	void FrameValues::Impl::TrackSpheres(std::uint64_t a_seq, const std::vector<BindlessPlacement>& a_out)
	{
		// Only slots sampled this frame can have moved their bound.
		if (lastSphere.size() < a_out.size()) {
			lastSphere.resize(a_out.size(), std::array<float, 4>{});
			sphereSeq.resize(a_out.size(), 0);
		}
		for (const auto slot : listedSlots) {
			if (slot >= a_out.size())
				continue;
			const auto& bound = a_out[slot].bound;
			auto& last = lastSphere[slot];
			if (std::memcmp(last.data(), bound, sizeof(bound)) != 0) {
				std::memcpy(last.data(), bound, sizeof(bound));
				sphereSeq[slot] = a_seq;
			}
		}
	}

	void FrameValues::Impl::PostSnapshotSpheres(const Job& a_job, const std::vector<BindlessPlacement>& a_out)
	{
		if (streamSpheresWanted.exchange(false, std::memory_order_acq_rel)) {
			DCLF_ZONE("CS.DCLF.FrameValues.StreamingSpheres");
			auto table = std::make_unique<Direct::SnapshotSpheres>();
			table->seq = a_job.seq;
			table->sphere.resize(a_out.size());
			for (std::size_t i = 0; i < a_out.size(); ++i)
				std::memcpy(table->sphere[i].data(), a_out[i].bound, sizeof(a_out[i].bound));
			table->owner.assign(rowOwner.begin(), rowOwner.begin() + std::min(rowOwner.size(), a_out.size()));
			table->owner.resize(a_out.size(), nullptr);
			streamSpheresSlot.Post(std::move(table));
		}
		if (!spheresWanted.exchange(false, std::memory_order_acq_rel))
			return;
		DCLF_ZONE("CS.DCLF.FrameValues.SnapshotSpheres");
		auto table = std::make_unique<Direct::SnapshotSpheres>();
		const std::size_t slots = a_out.size();
		table->seq = a_job.seq;
		table->sphere.resize(slots);
		for (std::size_t i = 0; i < slots; ++i)
			std::memcpy(table->sphere[i].data(), a_out[i].bound, sizeof(a_out[i].bound));
		table->owner.assign(rowOwner.begin(), rowOwner.begin() + std::min(rowOwner.size(), slots));
		table->owner.resize(slots, nullptr);
		// Moving slots take the live column every frame.
		table->moving.assign(slots, 0);
		for (std::size_t i = 0; i < std::min(slots, sphereSeq.size()); ++i)
			table->moving[i] = sphereSeq[i] && sphereSeq[i] + 2 > a_job.seq ? 1 : 0;
		const auto mark = [&](std::uint32_t a_slot) {
			if (a_slot < slots)
				table->moving[a_slot] = 1;
		};
		for (const auto& mover : keptMovers)
			if (mover.live) {
				mark(mover.item.slot);
				mark(mover.item.layerSlot);
			}
		for (const auto& item : settling)
			mark(item.slot);
		spheresSlot.Post(std::move(table));
	}

	void FrameValues::Impl::StoreForwardRows(const Job& a_job, const std::vector<BindlessPlacement>& a_out)
	{
		const auto* set = a_job.viewBatches.get();
		auto* target = a_job.forwardRows;
		if (!set || !target)
			return;
		DCLF_ZONE("CS.DCLF.FrameValues.ForwardRows");
		if (target->size() < a_out.size())
			target->resize(a_out.size());
		const BindlessPlacement* source = a_out.data();
		const std::size_t sourceRows = a_out.size();
		Direct::ForwardSortRow* sortRows = target->data();
		for (const Direct::ViewType type : { Direct::ViewType::Forward, Direct::ViewType::Refraction })
			for (const auto& batch : set->List(type)) {
				const auto* objects = batch.object;
				for (std::uint32_t lane = 0; lane < batch.count; ++lane)
					if (const std::uint32_t object = objects[lane]; object < sourceRows)
						sortRows[object] = Direct::ForwardSortRowOf(source[object]);
			}
		forwardRowsTarget = target;
		forwardRowsSeq = a_job.seq;
	}

	void FrameValues::Impl::StoreLiveSpheres(const Job& a_job, const std::vector<BindlessPlacement>& a_out)
	{
		const auto* set = a_job.viewBatches.get();
		if (!set)
			return;
		DCLF_ZONE("CS.DCLF.FrameValues.LiveSpheres");
		const std::size_t parity = Direct::LiveParity(a_job.seq);
		// The set starts both live columns from the snapshot. A mover's sphere is unknown there, so it is written every frame.
		for (const auto& ref : set->moverLanes) {
			const auto& batch = set->lists[ref.view][ref.batch];
			const std::uint32_t object = batch.object[ref.lane];
			if (object >= a_out.size())
				continue;
			const auto& bound = a_out[object].bound;
			const auto& live = batch.liveSpheres[parity];
			live.cx[ref.lane] = bound[0];
			live.cy[ref.lane] = bound[1];
			live.cz[ref.lane] = bound[2];
			live.r[ref.lane] = bound[3];
		}
	}

	void FrameValues::RequestSnapshotSpheres()
	{
		impl->spheresWanted.store(true, std::memory_order_release);
	}

	std::unique_ptr<Direct::SnapshotSpheres> FrameValues::TakeSnapshotSpheres()
	{
		return impl->spheresSlot.Take();
	}

	void FrameValues::RequestStreamingSpheres()
	{
		impl->streamSpheresWanted.store(true, std::memory_order_release);
	}

	std::unique_ptr<Direct::SnapshotSpheres> FrameValues::TakeStreamingSpheres()
	{
		return impl->streamSpheresSlot.Take();
	}

	// First-person items are sampled by the render thread, except palette rows class items.
	void FrameValues::Impl::ListItem(const Plan::Item& a_item, bool a_settles, bool a_mover, std::size_t a_rows, std::size_t a_blocks, std::vector<Listed>& a_items)
	{
		if ((a_item.firstPerson && a_item.itemClass == Api::FrameValueItemClass::None) || a_item.slot >= a_rows || !a_item.geometry || listed[a_item.slot])
			return;
		// An old write or settling item can outlive its skin while the palette block is reused by another actor.
		// Validate against the applied plans first, since different block starts can still overlap a recycled allocation.
		if (a_settles && a_item.boneRows && a_item.itemClass == Api::FrameValueItemClass::None) {
			const auto owner = moverAt.find(a_item.geometry.get());
			if (owner == moverAt.end())
				return;
			const auto& current = keptMovers[owner->second].item;
			if (Plan::Key::Of(current) != Plan::Key::Of(a_item))
				return;
		}
		listed[a_item.slot] = 1;
		listedSlots.push_back(a_item.slot);
		if (rowOwner.size() <= a_item.slot)
			rowOwner.resize(std::size_t(a_item.slot) + 1, nullptr);
		const bool continued = rowOwner[a_item.slot] == a_item.geometry.get();
		rowOwner[a_item.slot] = a_item.geometry.get();
		const bool palette = a_item.boneRows && std::size_t(a_item.boneOffset) + a_item.boneRows <= a_blocks && !blockListed[a_item.boneOffset];
		if (palette) {
			blockListed[a_item.boneOffset] = 1;
			listedBlocks.push_back(a_item.boneOffset);
		}
		a_items.push_back({ &a_item, palette, a_settles, a_mover, continued });
	}

	// A root writes its dependents' sun entry and fade node after their rows, so resample it when any was sampled.
	void FrameValues::Impl::MarkSampledRoots()
	{
		for (const auto slot : listedSlots)
			if (const auto it = slotRoots.find(slot); it != slotRoots.end())
				for (const auto r : it->second)
					MarkRoot(r);
	}

	void FrameValues::Impl::ApplyRootViewSamples(const Job& a_job, const SampleOut& a_out, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked)
	{
		auto& out = a_out.out;
		auto& paletteOut = a_out.paletteOut;
		for (const auto& sample : a_job.rootViewSamples) {
			if (sample.slot >= out.size())
				continue;
			if (std::memcmp(&out[sample.slot], &sample.row, sizeof(sample.row)) != 0) {
				out[sample.slot] = sample.row;
				rows.Mark(sample.slot);
				++a_marked;
			}
			if (sample.layerSlot != kNoObjectSlot && sample.layerSlot < out.size() && std::memcmp(&out[sample.layerSlot], &sample.layer, sizeof(sample.layer)) != 0) {
				out[sample.layerSlot] = sample.layer;
				rows.Mark(sample.layerSlot);
				++a_marked;
			}
			if (!sample.boneRows || sample.current.size() != sample.boneRows || sample.previous.size() != sample.boneRows)
				continue;
			const auto at = PaletteRowsOf(sample.boneOffset, sample.boneRows);
			if (std::size_t(at.previous) + sample.boneRows > paletteOut.size() || std::size_t(at.current) + sample.boneRows > paletteOut.size())
				continue;
			const std::size_t paletteBytes = std::size_t(sample.boneRows) * sizeof(PaletteRow);
			if (std::memcmp(&paletteOut[at.current], sample.current.data(), paletteBytes) != 0 || std::memcmp(&paletteOut[at.previous], sample.previous.data(), paletteBytes) != 0) {
				std::memcpy(&paletteOut[at.current], sample.current.data(), paletteBytes);
				std::memcpy(&paletteOut[at.previous], sample.previous.data(), paletteBytes);
				palettes.MarkRange(at.current, 2 * sample.boneRows);
				a_paletteMarked += 2 * sample.boneRows;
			}
		}
	}

	void FrameValues::Impl::SampleItems(const Job& a_job, const std::vector<Listed>& a_items, const SampleOut& a_out, std::vector<Changed>& a_changed,
		std::atomic<std::uint64_t>& a_refused)
	{
		auto& executor = ::RenderGraph::Frame::SharedExecutor();
		// A class's beginRun belongs to the producer alone, so run it before the parallel loop.
		PaletteRuns paletteRuns;
		BeginPaletteRuns(a_job, a_items, paletteRuns);
		executor.ParallelFor("CS.DCLF.FrameValues.Items", a_changed.size(), 1, [&](std::size_t a_begin, std::size_t a_end) {
			for (std::size_t c = a_begin; c < a_end; ++c) {
				DCLF_ZONE("CS.DCLF.FrameValues.Items");
				SampleChunk(a_job, a_items, paletteRuns, sampleBounds[c], sampleBounds[c + 1], a_out, a_changed[c], a_refused);
			}
		});
		for (std::size_t c = 0; c < paletteRuns.size(); ++c)
			if (paletteRuns[c].entry)
				paletteRuns[c].entry->contribution.endRun({ a_job.paletteInputs[c], paletteRuns[c].items });
	}

	// Split by cost, not count, so the frame's particle systems spread over the pool instead of filling one or two chunks.
	void FrameValues::Impl::SampleBounds(const std::vector<Listed>& a_items, std::vector<std::size_t>& a_bounds)
	{
		a_bounds.clear();
		a_bounds.push_back(0);
		std::size_t cost = 0;
		for (std::size_t i = 0; i < a_items.size(); ++i) {
			const auto& listed = a_items[i];
			const bool classRows = listed.palette && listed.item->itemClass != Api::FrameValueItemClass::None;
			cost += 1 + (classRows ? listed.item->boneRows / kSampleRowsPerUnit : 0);
			if (cost >= kSampleChunkCost || i + 1 - a_bounds.back() == kSampleGrain) {
				a_bounds.push_back(i + 1);
				cost = 0;
			}
		}
		if (a_bounds.back() != a_items.size())
			a_bounds.push_back(a_items.size());
	}

	void FrameValues::Impl::BeginPaletteRuns(const Job& a_job, const std::vector<Listed>& a_items, PaletteRuns& a_runs)
	{
		std::array<std::size_t, Api::kPaletteRowsClasses> counts{};
		for (std::size_t c = 0; c < a_runs.size(); ++c)
			a_runs[c].entry = Extensions::PaletteRows(static_cast<Api::FrameValueItemClass>(c + 1));
		const auto classOf = [&](const Listed& a_listed) -> std::size_t {
			const auto c = static_cast<std::size_t>(a_listed.item->itemClass);
			return a_listed.palette && c != 0 && c <= a_runs.size() && a_runs[c - 1].entry ? c : 0;
		};
		for (const auto& listed : a_items)
			if (const auto c = classOf(listed))
				++counts[c - 1];
		for (std::size_t c = 0; c < a_runs.size(); ++c)
			a_runs[c].items.reserve(counts[c]);
		for (std::uint32_t i = 0; i < a_items.size(); ++i)
			if (const auto c = classOf(a_items[i]))
				a_runs[c - 1].items.push_back({ a_items[i].item->geometry.get(), nullptr, i });
		for (std::size_t c = 0; c < a_runs.size(); ++c)
			if (a_runs[c].entry)
				a_runs[c].entry->contribution.beginRun({ a_job.paletteInputs[c], a_runs[c].items });
	}

	// Class items are deferred and written per class once, then finished in place so the chunk's lists keep the run's order.
	void FrameValues::Impl::SampleChunk(const Job& a_job, const std::vector<Listed>& a_items, const PaletteRuns& a_runs, std::size_t a_begin, std::size_t a_end,
		const SampleOut& a_out, Changed& a_mine, std::atomic<std::uint64_t>& a_refused)
	{
		ItemScratch scratch;
		for (std::size_t c = 0; c < a_runs.size(); ++c) {
			const auto& items = a_runs[c].items;
			const auto first = std::lower_bound(items.begin(), items.end(), a_begin,
				[](const Api::PaletteRowsItem& a_item, std::size_t a_listed) { return a_item.listed < a_listed; });
			scratch.cursors[c] = static_cast<std::size_t>(first - items.begin());
		}
		{
			// One engine-read lease per chunk, taken lazily. If refused, every engine-reading item of the chunk is refused.
			std::optional<EngineReadWindow::Lease> lease;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				if (!lease && !a_items[i].item->firstPerson)
					lease.emplace();
				SampleListed(a_items[i], a_runs, a_out, a_mine, scratch, lease && *lease, a_refused);
			}
		}
		if (!scratch.deferred)
			return;
		WriteDeferred(a_job, a_runs, *scratch.deferred);
		FinishDeferred(a_out, a_mine, *scratch.deferred);
	}

	void FrameValues::Impl::SampleListed(const Listed& a_listed, const PaletteRuns& a_runs, const SampleOut& a_out, Changed& a_mine, ItemScratch& a_scratch,
		bool a_leased, std::atomic<std::uint64_t>& a_refused)
	{
		auto& out = a_out.out;
		auto& row = a_scratch.row;
		auto& layer = a_scratch.layer;
		const auto& [itemPointer, palette, settles, mover, continued] = a_listed;
		const auto& item = *itemPointer;
		const auto paletteClass = static_cast<std::uint32_t>(item.itemClass);
		const bool deferred = palette && paletteClass != 0 && paletteClass <= a_runs.size() && a_runs[paletteClass - 1].entry;
		const Api::PaletteRowsItem* classItem = deferred ? &a_runs[paletteClass - 1].items[a_scratch.cursors[paletteClass - 1]++] : nullptr;
		// A first-person class item reads no engine memory and never settles.
		const bool firstPerson = item.firstPerson;
		if (!firstPerson && !a_leased) {
			++a_refused;
			if (settles)
				a_mine.settling.push_back(&item);  // the next producer takes it
			if (mover)
				a_mine.owed.push_back(item.geometry.get());
			return;
		}
		bool changes = false;
		if (firstPerson)
			row = out[item.slot];
		else
			SampleItem(item, row, layer);
		// Native: the previous transform is the one this render side last drew for the object, not the engine's previous update, so a
		// repeated snapshot draws without motion and a dropped one moves from what was really drawn. LOD keeps its current transform.
		const bool drawnPrevious = !firstPerson && continued && Snapshot::FrameCamera::Owned() &&
		                           &DrawnPreviousWorld(*item.geometry) != &item.geometry->world && !(GeometryPort::kMainPassRenderFlags & 0x10);
		if (drawnPrevious) {
			std::memcpy(row.previousWorld, out[item.slot].world, sizeof(row.previousWorld));
			if (item.layerSlot != kNoObjectSlot)
				std::memcpy(layer.previousWorld, out[item.slot].world, sizeof(layer.previousWorld));
		}
		const bool moving = drawnPrevious ? std::memcmp(row.world, row.previousWorld, sizeof(row.world)) != 0 :
		                                    std::memcmp(&item.geometry->world, &DrawnPreviousWorld(*item.geometry), sizeof(RE::NiTransform)) != 0;
		if (settles && !firstPerson && moving)
			a_mine.settling.push_back(&item);
		// A particle snapshot's data is never read here. Its row waits for its class's write.
		Deferred* waiting = classItem ? &Defer(a_listed, paletteClass - 1, *classItem, a_scratch) : nullptr;
		if (waiting) {
			waiting->row = row;
			waiting->slotsAt = a_mine.slots.size();
		} else if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
			out[item.slot] = row;
			a_mine.slots.push_back(item.slot);
			changes = true;
		}
		if (!firstPerson && item.layerSlot != kNoObjectSlot && item.layerSlot < out.size() && std::memcmp(&out[item.layerSlot], &layer, sizeof(layer)) != 0) {
			out[item.layerSlot] = layer;
			a_mine.slots.push_back(item.layerSlot);
			changes = true;
		}
		if (waiting) {
			waiting->layerChanged = changes;
			waiting->paletteRunsAt = a_mine.paletteRuns.size();
			waiting->owedAt = a_mine.owed.size();
			return;
		}
		if (palette && SamplePalette(item, a_out, a_mine))
			changes = true;
		// Keep sampling a mover until a sample changes nothing, so its previous world and palette settle after motion stops.
		if (mover && changes)
			a_mine.owed.push_back(item.geometry.get());
	}

	FrameValues::Impl::Deferred& FrameValues::Impl::Defer(const Listed& a_listed, std::uint32_t a_class, const Api::PaletteRowsItem& a_classItem, ItemScratch& a_scratch)
	{
		if (!a_scratch.deferred) {
			// About 20 KB, reused per thread since a chunk runs on one thread. Every field is written before it is read.
			static thread_local std::unique_ptr<DeferredChunk> threadChunk;
			if (!threadChunk)
				threadChunk = std::make_unique<DeferredChunk>();
			threadChunk->count = threadChunk->rowCount = 0;
			a_scratch.deferred = threadChunk.get();
		}
		auto& chunk = *a_scratch.deferred;
		auto& deferred = chunk.items[chunk.count++];
		deferred.listed = &a_listed;
		deferred.paletteClass = a_class;
		deferred.state = a_classItem.state;
		deferred.paletteAt = chunk.rowCount;
		chunk.rowCount += a_listed.item->boneRows;
		return deferred;
	}

	void FrameValues::Impl::WriteDeferred(const Job& a_job, const PaletteRuns& a_runs, DeferredChunk& a_deferred)
	{
		auto& chunk = a_deferred;
		chunk.rows.assign(chunk.rowCount, PaletteRow{});
		std::uint32_t next = 0;
		for (std::uint32_t c = 0; c < a_runs.size(); ++c) {
			const std::uint32_t first = next;
			for (std::uint32_t k = 0; k < chunk.count; ++k) {
				auto& deferred = chunk.items[k];
				if (deferred.paletteClass != c)
					continue;
				const auto& item = *deferred.listed->item;
				deferred.write = next;
				chunk.writes[next++] = { deferred.state, item.firstPerson, item.boneRows, chunk.rows[deferred.paletteAt].v };
			}
			if (next != first)
				a_runs[c].entry->contribution.sampleChunk({ a_job.paletteInputs[c], std::span(chunk.writes).subspan(first, next - first), firstPersonPalette });
		}
	}

	// Reverse order, so inserting at a later place leaves earlier places unchanged.
	void FrameValues::Impl::FinishDeferred(const SampleOut& a_out, Changed& a_mine, DeferredChunk& a_deferred)
	{
		auto& out = a_out.out;
		auto& paletteOut = a_out.paletteOut;
		for (std::uint32_t k = a_deferred.count; k-- > 0;) {
			auto& deferred = a_deferred.items[k];
			const auto& write = a_deferred.writes[deferred.write];
			const auto& item = *deferred.listed->item;
			auto& row = deferred.row;
			if (write.placed) {
				std::memcpy(row.world, write.world, sizeof(row.world));
				std::memcpy(row.previousWorld, write.world, sizeof(row.previousWorld));
				std::memcpy(row.bound, write.bound, sizeof(row.bound));
			}
			bool changes = deferred.layerChanged;
			if (std::memcmp(&out[item.slot], &row, sizeof(row)) != 0) {
				out[item.slot] = row;
				a_mine.slots.insert(a_mine.slots.begin() + static_cast<std::ptrdiff_t>(deferred.slotsAt), item.slot);
				changes = true;
			}
			// A class's block has no previous rows.
			const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
			const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
			if (std::memcmp(&paletteOut[at.current], write.palette, bytes) != 0) {
				std::memcpy(&paletteOut[at.current], write.palette, bytes);
				a_mine.paletteRuns.emplace(a_mine.paletteRuns.begin() + static_cast<std::ptrdiff_t>(deferred.paletteRunsAt), at.current, item.boneRows);
				changes = true;
			}
			if (deferred.listed->mover && changes)
				a_mine.owed.insert(a_mine.owed.begin() + static_cast<std::ptrdiff_t>(deferred.owedAt), item.geometry.get());
		}
	}

	namespace
	{
		/**
		 * @brief Captures a skin's geometry again when its skin instance holds a different partition than the plan.
		 * RaceMenu body morphs swap NiSkinInstance::skinPartition directly with no engine writer, so no event names the change.
		 */
		void SkinPartitionMoved(const RE::BSGeometry& a_geometry, const void* a_planned)
		{
			auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
			// A racy read is fine here. It is an aligned pointer that is only compared.
			if (!skin || !a_planned || skin->skinPartition.get() == a_planned)
				return;
			static std::mutex lock;
			// Removed once the plan holds that partition, so this stays small.
			static ankerl::unordered_dense::map<const void*, const void*> captured;
			// Lock the skin's critical section as the writer does, so the old partition cannot be released mid-capture.
			// It lives at NiSkinInstance+0x60 on SE and AE. VR's layout is unknown.
			auto* skinLock = REL::Module::IsVR() ? nullptr : &Engine::At<REX::W32::CRITICAL_SECTION>(skin, 0x60);
			if (skinLock)
				REX::W32::EnterCriticalSection(skinLock);
			const void* now = skin->skinPartition.get();
			bool capture = false;
			{
				std::scoped_lock guard(lock);
				if (now == a_planned) {
					captured.erase(&a_geometry);
				} else {
					auto [it, inserted] = captured.try_emplace(&a_geometry, now);
					capture = inserted || it->second != now;
					it->second = now;
				}
			}
			if (capture) {
				using G = SceneCapture::GeometryRecord;
				Scene::PushGeometryUpdate(&a_geometry, G::kSkin | G::kRenderer);
				// A RaceMenu slider drag makes one per shape per step, so log only the first few.
				static std::atomic<std::uint32_t> logged{ 0 };
				if (logged.fetch_add(1, std::memory_order_relaxed) < 8)
					logger::info("[DCLF] skin partition of {} replaced outside the engine's writers ({:p} -> {:p}): captured again",
						a_geometry.name.c_str(), a_planned, now);
			}
			if (skinLock)
				REX::W32::LeaveCriticalSection(skinLock);
		}
	}

	bool FrameValues::Impl::SamplePalette(const Plan::Item& a_item, const SampleOut& a_out, Changed& a_mine)
	{
		const auto& item = a_item;
		SkinPartitionMoved(*item.geometry, item.skinPartition);
		auto& paletteOut = a_out.paletteOut;
		// The frame after motion stops, the update copies current to previous. That change keeps the skin due once more so motion vectors settle.
		auto& geometry = *item.geometry;
		const float* current = nullptr;
		const float* previous = nullptr;
		if (!DCLF::SkinPalettes::Sample(geometry, item.boneRows, current, previous)) {
			++a_mine.defects;
			return false;
		}
		++a_mine.skins;
		const auto at = PaletteRowsOf(item.boneOffset, item.boneRows);
		const std::size_t bytes = std::size_t(item.boneRows) * sizeof(PaletteRow);
		if (std::memcmp(&paletteOut[at.current], current, bytes) != 0 || std::memcmp(&paletteOut[at.previous], previous, bytes) != 0) {
			std::memcpy(&paletteOut[at.current], current, bytes);
			std::memcpy(&paletteOut[at.previous], previous, bytes);
			a_mine.paletteRuns.emplace_back(at.current, 2 * item.boneRows);
			return true;
		}
		return false;
	}
	void FrameValues::Impl::SampleRoots(const SampleOut& a_out, std::vector<std::vector<std::uint32_t>>& a_rootChanged, std::vector<std::uint8_t>& a_rootRefused,
		std::atomic<std::uint64_t>& a_refused)
	{
		auto& out = a_out.out;
		::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.FrameValues.Roots", dueRoots.size(), 16, [&](std::size_t a_begin, std::size_t a_end) {
			DCLF_ZONE("CS.DCLF.FrameValues.Roots");
			// One lease per chunk. If refused, each of its roots is.
			EngineReadWindow::Lease lease;
			for (std::size_t r = a_begin; r < a_end; ++r) {
				const auto& root = keptRoots[dueRoots[r]];
				if (!lease) {
					++a_refused;
					a_rootRefused[r] = 1;
					continue;
				}
				float entry[4];
				StoreListedRoot(root.root.get(), entry);
				for (const auto& dependent : root.dependents) {
					if (dependent.firstPerson || dependent.slot >= out.size() || !dependent.geometry)
						continue;
					const auto write = [&](std::uint32_t a_slot, const RE::BSShaderProperty* a_property) {
						auto& row = out[a_slot];
						float fade[4];
						StoreFadeNode(a_property, fade);
						if (std::memcmp(row.sunEntry, entry, sizeof(entry)) == 0 && std::memcmp(row.lodFadeNode, fade, sizeof(fade)) == 0)
							return;
						std::memcpy(row.sunEntry, entry, sizeof(entry));
						std::memcpy(row.lodFadeNode, fade, sizeof(fade));
						a_rootChanged[r].push_back(a_slot);
					};
					write(dependent.slot, dependent.geometry->GetGeometryRuntimeData().shaderProperty.get());
					if (dependent.layerSlot != kNoObjectSlot && dependent.layerSlot < out.size())
						write(dependent.layerSlot, LayerPropertyOf(*dependent.geometry));
				}
			}
		});
	}

	void FrameValues::Impl::MarkSampled(const std::vector<Changed>& a_changed, const std::vector<std::vector<std::uint32_t>>& a_rootChanged,
		const std::vector<std::uint8_t>& a_rootRefused, std::uint64_t& a_marked, std::uint64_t& a_paletteMarked)
	{
		for (const auto& chunk : a_changed) {
			for (const auto* item : chunk.settling)
				settling.push_back(*item);
			owedKeys.insert(owedKeys.end(), chunk.owed.begin(), chunk.owed.end());
		}
		settled += settling.size();
		for (const auto& chunk : a_changed) {
			for (const auto slot : chunk.slots)
				rows.Mark(slot);
			a_marked += chunk.slots.size();
			for (const auto& [first, count] : chunk.paletteRuns) {
				palettes.MarkRange(first, count);
				a_paletteMarked += count;
			}
			skins += chunk.skins;
			defects += chunk.defects;
		}
		for (std::size_t r = 0; r < dueRoots.size(); ++r) {
			for (const auto slot : a_rootChanged[r])
				rows.Mark(slot);
			a_marked += a_rootChanged[r].size();
			if (a_rootRefused[r] || !a_rootChanged[r].empty())
				owedKeys.push_back(keptRoots[dueRoots[r]].root.get());
		}
	}

	namespace
	{
		struct ShadingFieldRange
		{
			std::size_t offset = 0, bytes = 0;
		};
		constexpr std::array<ShadingFieldRange, Extensions::kShadingRowFields> kShadingFieldRanges{ {
			{ offsetof(BindlessShading, skinPerGeometry), sizeof(BindlessShading::skinPerGeometry) },
			{ offsetof(BindlessShading, landscapeSeams), sizeof(BindlessShading::landscapeSeams) },
			{ offsetof(BindlessShading, skinMaterial), sizeof(BindlessShading::skinMaterial) },
			{ offsetof(BindlessShading, waterMaterial), sizeof(BindlessShading::waterMaterial) },
		} };
		static_assert(sizeof(BindlessShading::skinPerGeometry) <= sizeof(ShadingFieldValue::value));
	}

	void FrameValues::Impl::SampleShadingRows(const Job& a_job, std::size_t a_slots)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.Shading");
		BeginBuild(a_job, kShading, shading);
		auto& out = shading.Mutable();
		if (out.size() < a_slots) {
			const std::size_t first = out.size();
			out.resize(a_slots, BindlessShading{});
			shading.MarkRange(first, a_slots - first);
		}
		// Each slot once, by its newest item, since the slot may have been reused.
		auto& items = shadingItemsScratch;
		items.clear();
		items.reserve(a_job.shadingItems.size());
		if (shadingListed.size() < out.size())
			shadingListed.resize(out.size(), 0);
		for (auto it = a_job.shadingItems.rbegin(); it != a_job.shadingItems.rend(); ++it)
			if (it->slot < out.size() && it->property && !std::exchange(shadingListed[it->slot], 1))
				items.push_back(&*it);
		for (const auto* item : items)
			shadingListed[item->slot] = 0;
		constexpr std::size_t kGrain = 128;
		std::vector<std::vector<std::uint32_t>> changed((items.size() + kGrain - 1) / kGrain);
		std::vector<std::vector<std::size_t>> refusedItems(changed.size());
		std::atomic<std::uint64_t> refusedHere{ 0 };
		::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.FrameValues.Shading", items.size(), kGrain, [&](std::size_t a_begin, std::size_t a_end) {
			DCLF_ZONE("CS.DCLF.FrameValues.Shading");
			FrameGlobals::Scope scope(a_job.globals);
			auto& mine = changed[a_begin / kGrain];
			// One lease per chunk. If refused, its items go back to the next frame's producer.
			EngineReadWindow::Lease lease;
			for (std::size_t i = a_begin; i < a_end; ++i) {
				const auto& item = *items[i];
				if (!lease) {
					++refusedHere;
					refusedItems[a_begin / kGrain].push_back(static_cast<std::size_t>(items[i] - a_job.shadingItems.data()));
					continue;
				}
				BindlessShading row = out[item.slot];
				SampleShading(*item.property.get(), item.pass, item.member, row);
				// The shading fields come only from the captures below, not from SampleShading.
				if (std::memcmp(&row, &out[item.slot], sizeof(row)) != 0) {
					out[item.slot] = row;
					mine.push_back(item.slot);
				}
			}
		});
		std::uint64_t marked = 0, wetMarked = 0;
		for (const auto& slots : changed) {
			for (const auto slot : slots)
				shading.Mark(slot);
			marked += slots.size();
		}
		for (const auto& [slot, field, value, resources] : pendingFieldValues) {
			if (slot >= out.size())
				continue;
			const auto ownerKey = std::uint64_t(slot) * Extensions::kShadingRowFields + static_cast<std::size_t>(field);
			if (resources)
				fieldOwners[ownerKey] = resources;
			else
				fieldOwners.erase(ownerKey);
			const auto& range = kShadingFieldRanges[static_cast<std::size_t>(field)];
			auto* fieldBytes = reinterpret_cast<std::byte*>(&out[slot]) + range.offset;
			if (std::memcmp(fieldBytes, value.data(), range.bytes) == 0)
				continue;
			std::memcpy(fieldBytes, value.data(), range.bytes);
			shading.Mark(slot);
			++wetMarked;
		}
		pendingFieldValues.clear();
		for (const auto& chunk : refusedItems)
			a_job.refusedShading.insert(a_job.refusedShading.end(), chunk.begin(), chunk.end());
		shadingSampled += items.size();
		shadingChanged += marked;
		wetnessChanged += wetMarked;
		if (const auto refusals = refusedHere.load())
			refused += refusals;
	}

	// Catch the leased backing up with the journal's runs since its content. No frame reads it before the one that leased it,
	// and that frame waits for this producer's signal.
	void FrameValues::Impl::SendRows(const org::services::BackingLease& a_lease, Pool& a_pool, std::string_view a_name, const Job& a_job, std::size_t a_count,
		std::uint64_t a_contentEpoch, const void* a_view, Api::FrameValuePoolProducer::EmitRowsFn a_emit, std::span<const org::StreamingUploadRange> a_overrides)
	{
		auto& backing = *a_lease;
		if (a_count > backing.capacityClass)
			throw std::runtime_error(fmt::format("its {} backing holds {} rows of {}", a_name, backing.capacityClass, a_count));
		// One batched upload per backing. Nothing writes the rows meanwhile, so the runs can point into them.
		auto& ranges = uploadRanges;
		ranges.clear();
		std::uint64_t sentBytes = 0;
		{
			DCLF_ZONE("CS.DCLF.FrameValues.Send.Emit");
			auto put = [&](const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
				ranges.push_back({ a_data, a_bytes, a_offset });
				sentBytes += a_bytes;
			};
			using Put = decltype(put);
			const Api::FrameValuePoolProducer::RowSink sink{ &put, [](void* a_owner, const void* a_data, std::size_t a_bytes, std::size_t a_offset) {
																(*static_cast<Put*>(a_owner))(a_data, a_bytes, a_offset);
															} };
			const Api::FrameValuePoolProducer::SendTarget target{ backing.address, backing.capacityClass, &a_lease };
			a_emit(a_view, backing.contentEpoch.load(std::memory_order_acquire), target, sink);
		}
		// The copies of one batch must not overlap, so overrides replace the rows' bytes in place.
		if (!a_overrides.empty()) {
			auto& spliced = splicedRanges;
			spliced.clear();
			for (const auto& range : ranges) {
				std::size_t at = range.dstOffset;
				const std::size_t end = range.dstOffset + range.size;
				const auto piece = [&](std::size_t a_from, std::size_t a_to) {
					if (a_from < a_to)
						spliced.push_back({ static_cast<const std::byte*>(range.data) + (a_from - range.dstOffset), a_to - a_from, a_from });
				};
				auto it = std::ranges::upper_bound(a_overrides, at, std::less{}, [](const org::StreamingUploadRange& a_range) { return a_range.dstOffset + a_range.size; });
				for (; it != a_overrides.end() && it->dstOffset < end; ++it) {
					piece(at, std::min(end, it->dstOffset));
					at = std::max(at, it->dstOffset + it->size);
				}
				piece(at, end);
			}
			spliced.insert(spliced.end(), a_overrides.begin(), a_overrides.end());
			ranges.swap(spliced);
			sentBytes = 0;
			for (const auto& range : ranges)
				sentBytes += range.size;
		}
		if (!ranges.empty()) {
			DCLF_ZONE("CS.DCLF.FrameValues.Send.Queue");
			auto ticket = a_job.uploads->QueueTrackedStreamingUploadRanges(ranges,
				org::WorkerOwnedDestination{ backing.buffer, org::WorkerOwnedDestination::Ownership::PooledBackingLease });
			if (!ticket || ticket->state.load(std::memory_order_acquire) == org::TrackedUploadTicketState::Cancelled)
				throw std::runtime_error("the dedicated uploader refused its rows");
		}
		const std::uint64_t sent = ranges.size();
		backing.contentEpoch.store(a_contentEpoch, std::memory_order_release);
		a_pool.newestContentEpoch.store(a_contentEpoch, std::memory_order_release);
		a_pool.runs += sent;
		a_pool.bytes += sentBytes;
		runs += sent;
		bytes += sentBytes;
	}

	// Emitted from the rows themselves. No producer writes them while it sends.
	template <class T>
	void FrameValues::Impl::Send(const Job& a_job, Part a_part, const KeptWholeArray<T>& a_rows)
	{
		SendRows(a_job.leases[a_part], pools[a_part], kPartNames[a_part], a_job, a_rows.Size(), a_rows.ContentEpoch(), &a_rows,
			[](const void* a_view, std::uint64_t a_contentEpoch, const Api::FrameValuePoolProducer::SendTarget&, const Api::FrameValuePoolProducer::RowSink& a_sink) {
				static_cast<const KeptWholeArray<T>*>(a_view)->Emit(a_contentEpoch, a_sink);
			});
	}

	template <class T>
	void FrameValues::Impl::SendLate(const Job& a_job, Part a_part, const KeptWholeArray<T>& a_rows, std::span<const org::StreamingUploadRange> a_overrides)
	{
		SendRows(a_job.lateLeases[a_part], pools[a_part], kPartNames[a_part], a_job, a_rows.Size(), a_rows.ContentEpoch(), &a_rows,
			[](const void* a_view, std::uint64_t a_contentEpoch, const Api::FrameValuePoolProducer::SendTarget&, const Api::FrameValuePoolProducer::RowSink& a_sink) {
				static_cast<const KeptWholeArray<T>*>(a_view)->Emit(a_contentEpoch, a_sink);
			},
			a_overrides);
	}

	void FrameValues::Impl::BuildLateRows(const Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.LatePoses");
		// Only plain kept movers, not first-person members or palette rows class items.
		const auto find = [&](const void* a_geometry, LatePoses::Location& a_at) {
			const auto it = moverAt.find(a_geometry);
			if (it == moverAt.end())
				return false;
			const auto& mover = keptMovers[it->second];
			if (!mover.live || mover.item.firstPerson || mover.item.itemClass != Api::FrameValueItemClass::None)
				return false;
			a_at = { mover.item.slot, mover.item.layerSlot, mover.item.boneOffset, mover.item.boneRows };
			return true;
		};
		LatePoses::BuildRows(*a_job.latePlan, find, rows.Get(), palettes.Get(), lateRows);
	}

	void FrameValues::Impl::MarkLateDirty()
	{
		for (const auto& [first, count] : lateDirtyPlacements)
			rows.MarkRange(first, count);
		for (const auto& [first, count] : lateDirtyPalettes)
			palettes.MarkRange(first, count);
		lateDirtyPlacements.clear();
		lateDirtyPalettes.clear();
	}

	template <class Call>
	void FrameValues::Impl::ForEachRegistered(const Job& a_job, Pools a_pools, Call&& a_call)
	{
		struct Owner
		{
			Impl& impl;
			const Job& job;
		} owner{ *this, a_job };
		const auto send = [](void* a_owner, std::size_t a_pool, std::size_t a_count, std::uint64_t a_contentEpoch, const void* a_view,
							  Api::FrameValuePoolProducer::EmitRowsFn a_emit) {
			auto& [impl, job] = *static_cast<Owner*>(a_owner);
			impl.SendRows(job.registered[a_pool].lease, impl.registered[a_pool].pool, impl.registered[a_pool].entry->name, job, a_count, a_contentEpoch, a_view, a_emit);
		};
		for (std::size_t i = 0; i < a_job.registered.size(); ++i) {
			const bool perRender = registered[i].entry->perRender;
			if ((a_pools == Pools::GameSide && perRender) || (a_pools == Pools::RenderSide && !perRender))
				continue;
			const auto& part = a_job.registered[i];
			Api::FrameValuePoolProducer producer;
			producer.seq = a_job.seq;
			producer.globals = a_job.globals.get();
			producer.input = &part.input;
			producer.oldestContentEpoch = part.oldestContentEpoch;
			producer.kickedSeq = a_job.upload ? &kickedSeq : &sampleKicked;
			producer.send = send;
			producer.owner = &owner;
			producer.pool = i;
			producer.seeded = &seeded;
			a_call(*registered[i].entry, producer);
		}
	}

	void FrameValues::Impl::Upload(const Job& a_job)
	{
		DCLF_ZONE("CS.DCLF.FrameValues.Upload");
		{
			DCLF_ZONE("CS.DCLF.FrameValues.Send.placements");
			Send(a_job, kPlacements, rows);
		}
		{
			DCLF_ZONE("CS.DCLF.FrameValues.Send.palettes");
			Send(a_job, kPalettes, palettes);
		}
		// The next build marks the late rows changed, so every backing holding them is sent the frame's rows again.
		if (a_job.lateLeases[kPlacements]) {
			DCLF_ZONE("CS.DCLF.FrameValues.Send.latePlacements");
			auto& overrides = overrideRanges;
			overrides.clear();
			for (std::size_t i = 0; i < lateRows.placements.size(); ++i) {
				const std::uint32_t slot = lateRows.placementSlots[i];
				overrides.push_back({ &lateRows.placements[i], sizeof(BindlessPlacement), std::size_t(slot) * sizeof(BindlessPlacement) });
				lateDirtyPlacements.emplace_back(slot, 1u);
			}
			std::ranges::sort(overrides, {}, &org::StreamingUploadRange::dstOffset);
			SendLate(a_job, kPlacements, rows, overrides);
		}
		if (a_job.lateLeases[kPalettes]) {
			DCLF_ZONE("CS.DCLF.FrameValues.Send.latePalettes");
			auto& overrides = overrideRanges;
			overrides.clear();
			std::size_t at = 0;
			for (const auto& [first, count] : lateRows.paletteRuns) {
				overrides.push_back({ &lateRows.palettes[at], std::size_t(count) * sizeof(PaletteRow), std::size_t(first) * sizeof(PaletteRow) });
				lateDirtyPalettes.emplace_back(first, count);
				at += count;
			}
			std::ranges::sort(overrides, {}, &org::StreamingUploadRange::dstOffset);
			SendLate(a_job, kPalettes, palettes, overrides);
		}
		{
			DCLF_ZONE("CS.DCLF.FrameValues.Send.shading");
			auto owners = std::make_shared<ResourceOwners>();
			owners->reserve(fieldOwners.size());
			for (const auto& [key, owner] : fieldOwners)
				owners->push_back(owner);
			a_job.fieldResources->push_back(owners);
			Send(a_job, kShading, shading);
			backingFieldOwners[a_job.leases[kShading]->generation] = { a_job.leases[kShading].Get(), std::move(owners), a_job.seq };
		}
		// Tracy zone names are static, so the pool's name goes in the zone text.
		ForEachRegistered(a_job, Pools::All, [](const Extensions::FrameValuePoolEntry& a_entry, Api::FrameValuePoolProducer& a_producer) {
			DCLF_ZONE("CS.DCLF.FrameValues.Send.Registered");
			ZoneText(a_entry.name.data(), a_entry.name.size());
			a_entry.send(a_producer);
		});
	}

	FrameValues::FrameValues() :
		impl(std::make_unique<Impl>())
	{}

	FrameValues::~FrameValues() = default;

	FrameValues& FrameValues::Get()
	{
		static FrameValues values;
		return values;
	}

	void FrameValues::Impl::FrameStartRegistered(const std::shared_ptr<const FrameGlobals>& a_globals)
	{
		const Api::FrameValuePoolFrameStart start{ a_globals };
		for (const auto& entry : Extensions::RegisteredPools())
			if (entry.frameStart)
				entry.frameStart(start);
	}

	void FrameValues::Impl::ApplyRootViewPlans(const std::vector<std::shared_ptr<const Plan>>& a_plans)
	{
		for (const auto& applied : a_plans) {
			if (!applied)
				continue;
			auto release = [](Plan::Item& a_item) {
				ReleaseLater(a_item.geometry);
				ReleaseLater(a_item.listedRoot);
			};
			if (applied->reset) {
				for (auto& [geometry, item] : rootViewMovers)
					release(item);
				rootViewMovers.clear();
			}
			for (const void* geometry : applied->moversLeft)
				if (const auto it = rootViewMovers.find(geometry); it != rootViewMovers.end()) {
					release(it->second);
					rootViewMovers.erase(it);
				}
			for (const auto& item : applied->movers) {
				if (!item.geometry)
					continue;
				const auto it = rootViewMovers.find(item.geometry.get());
				if (it != rootViewMovers.end()) {
					release(it->second);
					if (item.firstPerson)
						it->second = item;
					else
						rootViewMovers.erase(it);
				} else if (item.firstPerson) {
					rootViewMovers.emplace(item.geometry.get(), item);
				}
			}
		}
	}

	void FrameValues::Impl::QueuePlans(std::vector<std::shared_ptr<const Plan>>& a_plans)
	{
		for (auto& queued : a_plans)
			if (queued) {
				unsampled.push_back(queued);
				plan = std::move(queued);
				++planSerial;
			}
	}

	void FrameValues::Impl::LeaseBackings(Job& a_job)
	{
		const std::array<std::uint64_t, kBuiltInPools> needed{ neededRows[kPlacements].load(std::memory_order_acquire), neededRows[kPalettes].load(std::memory_order_acquire),
			neededRows[kShading].load(std::memory_order_acquire) };
		// Each lease, so the ones bound to work the job may not wait for can be leased again fresh.
		const auto leaseAll = [&](bool a_freshIfBound, std::uint64_t a_completed) {
			std::uint64_t writeAfter = 0;
			const auto lease = [&](Pool& a_pool, std::string_view a_name, std::uint32_t a_stride, org::services::BackingFlags a_flags, std::uint64_t a_needed,
								   org::services::BackingLease& a_frameLease, org::services::BackingLease& a_jobLease, std::optional<std::uint64_t>& a_floor) {
				// A second pass leases again only what the first left bound to unfinished work.
				if (a_freshIfBound && !(a_jobLease && a_jobLease->safeAt.load(std::memory_order_acquire) > a_completed))
					return;
				writeAfter = std::max(writeAfter, LeasePool(a_pool, a_name, a_stride, a_flags, a_needed, a_frameLease, a_jobLease, a_floor, a_freshIfBound));
			};
			// Lease the late poses' backings first, so the frame's floors cover their content.
			if (const auto& latePlan = late.plan) {
				const bool parts[kBuiltInPools]{ !latePlan->rigid.empty(), !latePlan->skins.empty(), false };
				for (const std::size_t part : { kPlacements, kPalettes })
					if (parts[part]) {
						std::optional<std::uint64_t> floor;
						lease(pools[part], kPartNames[part], kStrides[part], {}, needed[part], late.frameLeases[part], late.jobLeases[part], floor);
					}
			}
			for (std::size_t part = 0; part < kBuiltInPools; ++part)
				lease(pools[part], kPartNames[part], kStrides[part], {}, needed[part], frameLeases[part], a_job.leases[part], a_job.oldestContentEpochs[part]);
			for (std::size_t i = 0; i < a_job.registered.size(); ++i) {
				auto& entry = registered[i];
				auto& part = a_job.registered[i];
				lease(entry.pool, entry.entry->name, entry.entry->stride, { .unorderedAccess = entry.entry->unorderedAccess, .deviceAddress = entry.entry->deviceAddress },
					entry.needed.load(std::memory_order_acquire),
					registeredFrameLeases[i], part.lease, part.oldestContentEpoch);
			}
			return writeAfter;
		};
		const auto completed = ::RenderGraph::Frame::Retirement::Clock()->Completed();
		std::uint64_t writeAfter = leaseAll(false, completed);
		// The frame's own work waits for this job's signal, so the copy may wait only for work the last present sealed. A capped pool's
		// bound backing can be bound to a point past that: a bind at the frame's start takes everything submitted so far, the frame's
		// early epochs with it. Waiting for those deadlocks the graphics and copy queues, so such backings are leased again fresh.
		if (writeAfter > completed && !SafeProducerWait(BoundGpuPoint(writeAfter))) {
			if (std::uint64_t replaced = leaseAll(true, completed); replaced > completed)
				logger::error("[DCLF] frame values {}: a fresh backing is still bound to unfinished work (clock {})", a_job.seq, replaced);
			// Logged at the 1st, 10th, 100th... job: how often it happens is the overflow pools' memory cost.
			static std::uint64_t freshJobs = 0;
			if (const auto count = ++freshJobs; count == 1 || count == 10 || count == 100 || count == 1000 || count % 10000 == 0)
				logger::info("[DCLF] frame values {}: a reused copy was bound to work the last present did not seal; it took fresh backings instead "
							 "({} jobs so far)",
					a_job.seq, count);
			writeAfter = 0;
		}
		ReusedCopyPoint(writeAfter, a_job);
	}

	org::GraphHost::GpuPoint FrameValues::Impl::BoundGpuPoint(std::uint64_t a_writeAfter) const
	{
		if (a_writeAfter <= ::RenderGraph::Frame::Retirement::Clock()->Completed())
			return {};
		const auto at = std::ranges::find_if(boundPoints, [&](const auto& a_point) { return a_point.first >= a_writeAfter; });
		return at != boundPoints.end() ? at->second : ::RenderGraph::Frame::Retirement::SubmittedGpuPoint();
	}

	void FrameValues::Impl::ReusedCopyPoint(std::uint64_t a_writeAfter, Job& a_job)
	{
		const auto writeAfter = a_writeAfter;
		// Points rise per queue, so the latest one covers the others.
		if (writeAfter > ::RenderGraph::Frame::Retirement::Clock()->Completed()) {
			a_job.writeAfter = BoundGpuPoint(writeAfter);
			reusedInFlight.fetch_add(1, std::memory_order_relaxed);
			// Waiting for this frame's own work could hold the uploader, and the signal behind it, until the present.
			if (static bool logged = false; !logged && !::RenderGraph::Frame::Retirement::Sealed(a_job.writeAfter)) {
				logged = true;
				logger::error("[DCLF] frame values {}: a reused copy's uploads wait for work the last present did not seal; the uploader may hold "
							  "the frame's signal until the frame presents",
					a_job.seq);
			}
		}
	}

	std::uint64_t FrameValues::Impl::LeasePool(Pool& a_pool, std::string_view a_name, std::uint32_t a_stride, org::services::BackingFlags a_flags, std::uint64_t a_needed, org::services::BackingLease& a_frameLease,
		org::services::BackingLease& a_jobLease, std::optional<std::uint64_t>& a_oldestContentEpoch, bool a_fresh)
	{
		// Shrink only after the asks stay much smaller for a while. A fresh backing is written whole.
		org::services::FitCapacity(a_pool.capacity, a_pool.window, std::max<std::uint64_t>(a_needed, 1), kPoolGrowth, PooledBackings::Shrink());
		if (a_pool.backingName.empty())
			a_pool.backingName = fmt::format("cs.dclf.frame-values.{}", a_name);
		if (a_fresh && !a_pool.overflow)
			a_pool.overflow = org::services::BackingPool::Create(PooledBackings::PerFrameHooks(), ::RenderGraph::Frame::Retirement::Clock(),
				{ .name = a_pool.backingName + ".fresh", .maxPerClass = 0, .spareFree = 1, .reuseBound = false });
		auto& from = a_fresh ? *a_pool.overflow : *a_pool.pool;
		org::services::BackingPool::AcquireInfo info;
		auto lease = from.Acquire({ .capacityClass = a_pool.capacity, .stride = a_stride, .flags = a_flags, .name = a_pool.backingName },
			a_pool.newestContentEpoch.load(std::memory_order_acquire), &info);
		if (!lease)
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the frame values' {} pool returned no backing{}", a_name, info.error.empty() ? std::string() : ": " + info.error));
		++a_pool.acquires;
		if (a_fresh)
			a_pool.overflowAcquires.fetch_add(1, std::memory_order_relaxed);
		if (info.allocated)
			++a_pool.made;
		// The journal keeps changes back to the oldest content either pool holds.
		a_oldestContentEpoch = a_pool.pool->OldestContentEpoch();
		if (a_pool.overflow)
			if (const auto overflow = a_pool.overflow->OldestContentEpoch())
				a_oldestContentEpoch = a_oldestContentEpoch ? std::min(*a_oldestContentEpoch, *overflow) : *overflow;
		a_pool.floor.store(a_oldestContentEpoch.value_or(0), std::memory_order_relaxed);
		a_frameLease = lease;
		a_jobLease = std::move(lease);
		return info.writeAfter;
	}

	void FrameValues::Impl::MakeRegistered()
	{
		const auto entries = Extensions::RegisteredPools();
		registered = std::make_unique<Registered[]>(entries.size());
		for (std::size_t i = 0; i < entries.size(); ++i) {
			registered[i].entry = &entries[i];
			registered[i].pool.pool = MakePool(entries[i].name);
		}
		registeredFrameLeases.resize(entries.size());
		registeredIndex.assign(entries.size(), 0u);
		registeredCount = entries.size();
	}

	void FrameValues::Impl::KickRegistered(Job& a_job, const FrameGlobals* a_globals, bool a_perRender)
	{
		const std::size_t count = registeredCount;
		if (count != Extensions::RegisteredPools().size())
			::RenderGraph::Fatal("Drawcall Limit Fix", "the frame values were made before every feature registered its pools");
		a_job.registered.resize(count);
		const Api::FrameValuePoolKick kick{ a_job.seq, a_globals, a_job.shadingSlots };
		for (std::size_t i = 0; i < count; ++i) {
			if (registered[i].entry->perRender != a_perRender)
				continue;
			auto input = registered[i].entry->kick(kick);
			a_job.registered[i].input = std::move(input.input);
			// A game-side pool's sampling jobs may outnumber the uploads, so its backing holds the most any of them asked for.
			auto& needed = registered[i].needed;
			if (a_perRender)
				needed.store(input.rows, std::memory_order_release);
			else
				for (auto held = needed.load(std::memory_order_relaxed); input.rows > held && !needed.compare_exchange_weak(held, input.rows, std::memory_order_acq_rel);) {}
		}
	}

	void FrameValues::Impl::SampleRootViews(const std::vector<std::shared_ptr<const Plan>>& a_plans, const SetSnapshot& a_claims, std::vector<RootViewSample>& a_out)
	{
		std::vector<std::uint8_t> taken;
		const std::uint8_t rootPhases = Extensions::Views().rootPhases;
		auto take = [&](const Plan::Item& a_item) {
			if (!a_item.firstPerson || !a_item.geometry || a_item.slot == kNoObjectSlot || !(a_claims.PhasesOf(a_item.geometry.get()) & rootPhases))
				return;
			if (taken.size() <= a_item.slot)
				taken.resize(a_item.slot + 1, 0);
			if (std::exchange(taken[a_item.slot], 1))
				return;
			auto& sample = a_out.emplace_back();
			sample.slot = a_item.slot;
			sample.layerSlot = a_item.layerSlot;
			SampleItem(a_item, sample.row, sample.layer);
			if (!a_item.boneRows)
				return;
			auto& geometry = *a_item.geometry;
			SkinPartitionMoved(geometry, a_item.skinPartition);
			const float* current = nullptr;
			const float* previous = nullptr;
			if (!DCLF::SkinPalettes::Sample(geometry, a_item.boneRows, current, previous))
				return;
			sample.boneOffset = a_item.boneOffset;
			sample.boneRows = a_item.boneRows;
			const auto* currentRows = reinterpret_cast<const PaletteRow*>(current);
			const auto* previousRows = reinterpret_cast<const PaletteRow*>(previous);
			sample.current.assign(currentRows, currentRows + a_item.boneRows);
			sample.previous.assign(previousRows, previousRows + a_item.boneRows);
		};
		for (const auto& [geometry, item] : rootViewMovers)
			take(item);
		for (auto it = a_plans.rbegin(); it != a_plans.rend(); ++it)
			if (*it)
				for (const auto& item : (*it)->written)
					take(item);
	}

	struct FrameValues::RootViews
	{
		std::vector<Impl::RootViewSample> samples;
	};

	std::shared_ptr<FrameValues::RootViews> FrameValues::TakeRootViews(const std::vector<std::shared_ptr<const PlacementPlan>>& a_plans,
		const SetSnapshot* a_claims)
	{
		auto& s = *impl;
		s.ApplyRootViewPlans(a_plans);
		auto views = std::make_shared<RootViews>();
		if (a_claims)
			s.SampleRootViews(a_plans, *a_claims, views->samples);
		return views;
	}

	void FrameValues::KickSample(std::vector<std::shared_ptr<const PlacementPlan>> a_plans, std::vector<ShadingItem> a_shading,
		std::vector<ShadingFieldValue> a_fieldValues, std::shared_ptr<const FrameGlobals> a_globals, std::shared_ptr<RootViews> a_rootViews)
	{
		auto& s = *impl;
		TracyCZoneN(kStart, "CS.DCLF.FV.K.FrameStartRegistered", true);
		// Runs whether or not the ticket has a sampling job.
		Impl::FrameStartRegistered(a_globals);
		TracyCZoneEnd(kStart);
		TracyCZoneN(kPlans, "CS.DCLF.FV.K.Plans", true);
		// Refused items first since they are oldest, then untaken ones, then this ticket's.
		if (auto refused = s.TakeRefusedShading(); !refused.empty())
			s.unsampledShading.insert(s.unsampledShading.begin(), std::make_move_iterator(refused.begin()), std::make_move_iterator(refused.end()));
		s.unsampledShading.insert(s.unsampledShading.end(), std::make_move_iterator(a_shading.begin()), std::make_move_iterator(a_shading.end()));
		s.unsampledFieldValues.insert(s.unsampledFieldValues.end(), a_fieldValues.begin(), a_fieldValues.end());
		s.QueuePlans(a_plans);
		TracyCZoneEnd(kPlans);
		// Startup tickets come before the upload queue exists, so their inputs wait for the first sampling job.
		if (!Available())
			return;
		TracyCZoneN(kJob, "CS.DCLF.FV.K.SampleJob", true);
		// Plans hold engine references, so they are released here, in the game half, once their job is done.
		const auto sampled = s.sampledSeq.load(std::memory_order_acquire);
		while (!s.plans.empty() && s.plans.front().first <= sampled)
			s.plans.pop_front();
		static const Plan kEmpty{};
		const std::uint64_t seq = ++s.sampleSeq;
		s.sampleKicked.store(seq, std::memory_order_release);
		Impl::Job job;
		job.seq = seq;
		job.plan = s.plan ? s.plan.get() : &kEmpty;
		job.planSerial = s.plan ? s.planSerial : 0;
		// Held until the job is done.
		for (auto& unsampled : s.unsampled) {
			job.written.push_back(unsampled.get());
			s.plans.emplace_back(seq, std::move(unsampled));
		}
		s.unsampled.clear();
		job.shadingItems = std::exchange(s.unsampledShading, {});
		job.fieldValues = std::exchange(s.unsampledFieldValues, {});
		job.shadingSlots = job.plan->slots;
		for (const auto& item : job.shadingItems)
			if (item.slot != kNoObjectSlot)
				job.shadingSlots = std::max<std::size_t>(job.shadingSlots, std::size_t(item.slot) + 1);
		for (const auto& value : job.fieldValues)
			if (value.slot != kNoObjectSlot)
				job.shadingSlots = std::max<std::size_t>(job.shadingSlots, std::size_t(value.slot) + 1);
		job.globals = a_globals;
		// The rows the arrays will hold, before the job takes its place on the chain: an upload kicked after it leases for them.
		const std::array<std::uint64_t, kBuiltInPools> rowsAsked{ job.plan->slots, 2ull * job.plan->boneCapacity, job.shadingSlots };
		for (std::size_t part = 0; part < kBuiltInPools; ++part)
			for (auto held = s.neededRows[part].load(std::memory_order_relaxed);
				rowsAsked[part] > held && !s.neededRows[part].compare_exchange_weak(held, rowsAsked[part], std::memory_order_acq_rel);) {}
		TracyCZoneEnd(kJob);
		TracyCZoneN(kReg, "CS.DCLF.FV.K.KickRegistered", true);
		s.KickRegistered(job, a_globals.get(), false);
		TracyCZoneEnd(kReg);
		TracyCZoneN(kRoot, "CS.DCLF.FV.K.PaletteKicksAndRootViews", true);
		for (std::size_t c = 0; c < job.paletteInputs.size(); ++c)
			if (const auto* entry = Extensions::PaletteRows(static_cast<Api::FrameValueItemClass>(c + 1)); entry && entry->contribution.kick)
				entry->contribution.kick({ seq, job.paletteInputs[c] });
		// The first-person members the game half sampled before Main::Draw's first-person shift (FUN_1414cb020).
		if (a_rootViews)
			job.rootViewSamples = std::move(a_rootViews->samples);
		TracyCZoneEnd(kRoot);
		if (s.plan)
			s.plans.emplace_back(seq, s.plan);
		job.chain = s.chainKicked.fetch_add(1, std::memory_order_acq_rel) + 1;
		DCLF_ZONE("CS.DCLF.FV.K.DispatchSample");
		if (!::RenderGraph::Frame::SharedExecutor().DispatchFrameCritical(SceneScheduler::Scope(), "frame values sampling", [&s, job = std::move(job)](const auto&) mutable {
				s.RunSample(job);
				if (!job.refusedShading.empty()) {
					std::vector<ShadingItem> refused;
					refused.reserve(job.refusedShading.size());
					for (const auto i : job.refusedShading)
						refused.push_back(std::move(job.shadingItems[i]));
					s.PushRefusedShading(std::move(refused));
				}
				// Engine references go to the render thread, never dropped on this worker.
				for (auto& item : job.shadingItems)
					if (item.property)  // a refused item's went back with it
						EngineReleases::Push(RE::NiPointer<RE::NiRefObject>(std::move(item.property)));
			}))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the frame values' sampling was refused by DCLF's executor");
	}

	bool FrameValues::Kick(std::shared_ptr<const FrameGlobals> a_globals)
	{
		auto& s = *impl;
		s.kicked = false;
		TracyCZoneN(kBind, "CS.DCLF.FV.K.BindAndRetire", true);
		// EndFrame did not come, so bind the backings to what was submitted so far.
		BindFrameLeases(s.frameLeases, s.registeredFrameLeases, s.boundPoints);
		s.ReleaseLatePoses();
		s.RetireFieldOwners();
		TracyCZoneEnd(kBind);
		auto* host = RenderGraphRuntime::Get().Host();
		auto timeline = RenderGraphRuntime::Get().FrameWaitTimeline();
		auto uploads = host ? host->RetainUploads() : nullptr;
		if (!host || !timeline || !uploads || !uploads->HasDedicatedStreamingQueue()) {
			if (host)
				host->SetFrameWaitValue(0);
			// With no backing leased, no index can name a backing the pool may release.
			s.late.plan = nullptr;
			frameIndex = paletteIndex = shadingIndex = 0;
			std::fill(s.registeredIndex.begin(), s.registeredIndex.end(), 0u);
			if (!s.unavailableLogged && s.seq) {
				s.unavailableLogged = true;
				logger::error("[DCLF] frame values unavailable ({}): DCLF's draws have no placements", !host ? "no render graph" : "no dedicated upload queue");
			}
			return false;
		}
		TracyCZoneN(kJob, "CS.DCLF.FV.K.Job", true);
		const std::uint64_t seq = ++s.seq;
		s.kickedSeq.store(seq, std::memory_order_release);
		Impl::Job job;
		job.upload = true;
		job.seq = seq;
		// Its place on the chain before the rows it leases for: every sampling job before it has raised them.
		job.chain = s.chainKicked.fetch_add(1, std::memory_order_acq_rel) + 1;
		job.fieldResources = s.frameFieldOwners = std::make_shared<Impl::ResourceOwners>();
		job.shadingSlots = s.neededRows[Impl::kShading].load(std::memory_order_acquire);
		job.globals = a_globals;
		job.viewBatches = viewBatches;
		job.forwardRows = std::exchange(forwardRows, nullptr);
		TracyCZoneEnd(kJob);
		TracyCZoneN(kReg, "CS.DCLF.FV.K.KickRegistered", true);
		s.KickRegistered(job, a_globals.get(), true);
		TracyCZoneEnd(kReg);
		TracyCZoneN(kLease, "CS.DCLF.FV.K.LeaseBackings", true);
		// The floor, the oldest content in circulation, is what the journals keep.
		s.LeaseBackings(job);
		job.latePlan = std::move(s.late.plan);
		job.lateLeases = std::move(s.late.jobLeases);
		TracyCZoneEnd(kLease);
		job.timeline = std::move(timeline);
		job.uploads = std::move(uploads);
		DCLF_ZONE("CS.DCLF.FV.K.Dispatch");
		frameIndex = s.frameLeases[Impl::kPlacements]->srvIndex;
		paletteIndex = s.frameLeases[Impl::kPalettes]->srvIndex;
		shadingIndex = s.frameLeases[Impl::kShading]->srvIndex;
		for (std::size_t i = 0; i < s.registeredIndex.size(); ++i)
			s.registeredIndex[i] = s.registeredFrameLeases[i]->srvIndex;
		for (std::size_t i = 0; i < s.frameLeases.size(); ++i)
			s.PublishFrameVersion(i, s.frameLeases[i]);
		for (std::size_t i = 0; i < s.registeredFrameLeases.size(); ++i)
			s.PublishFrameVersion(s.frameLeases.size() + i, s.registeredFrameLeases[i]);
		// Batches submitted from here wait for the frame's values. The upload job always signals.
		host->SetFrameWaitValue(seq);
		s.kicked = true;
		if (!::RenderGraph::Frame::SharedExecutor().DispatchFrameCritical(SceneScheduler::Scope(), "frame values", [&s, job = std::move(job)](const auto&) mutable {
				s.RunUpload(job);
				job.leases = {};
				for (auto& part : job.registered)
					part.lease = org::services::BackingLease{};
			}))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the frame values' producer was refused by DCLF's executor");
		return true;
	}

	void FrameValues::SetLatePoses(std::shared_ptr<const LatePoses::FramePlan> a_plan)
	{
		impl->late.plan = std::move(a_plan);
	}

	void FrameValues::UseLatePoses()
	{
		auto& s = *impl;
		if (!s.kicked)
			return;
		if (const auto& lease = s.late.frameLeases[Impl::kPlacements]) {
			frameIndex = lease->srvIndex;
			s.PublishFrameVersion(Impl::kPlacements, lease);
		}
		if (const auto& lease = s.late.frameLeases[Impl::kPalettes]) {
			paletteIndex = lease->srvIndex;
			s.PublishFrameVersion(Impl::kPalettes, lease);
		}
	}

	void FrameValues::DeclareReads(org::RenderPassBuilder& a_builder, std::uint32_t a_sets)
	{
		DeclareReadsExcept(a_builder, a_sets, ~std::size_t(0));
	}

	void FrameValues::DeclarePoolWrite(org::RenderPassBuilder& a_builder, Api::FrameValuePoolId a_pool, std::uint32_t a_sets, RenderGraph::DeclaredRefs& a_out)
	{
		const auto& buffers = impl->LateBuffers();
		const std::size_t set = a_pool ? kBuiltInPools + a_pool.index : ~std::size_t(0);
		// A pass declares a buffer for one access, so the written pool is not also declared read.
		DeclareReadsExcept(a_builder, a_sets, set);
		if (set >= buffers.size())
			return;
		RenderGraph::DeclareLateRef(a_builder, buffers[set], RenderGraph::RefAccess::Write, fmt::format("frame value pool {}", a_pool.index),
			impl->poolRefLatches[a_pool.index], a_out);
	}

	void FrameValues::DeclareReadsExcept(org::RenderPassBuilder& a_builder, std::uint32_t a_sets, std::size_t a_skip)
	{
		const auto& buffers = impl->LateBuffers();
		const auto declare = [&](std::size_t a_set) {
			if (a_set < buffers.size() && a_set != a_skip)
				a_builder.ShaderResource(*buffers[a_set]);
		};
		if (a_sets & kReadPlacements)
			declare(Impl::kPlacements);
		if (a_sets & kReadPalettes)
			declare(Impl::kPalettes);
		if (a_sets & kReadShading)
			declare(Impl::kShading);
		if (a_sets & kReadPools)
			for (std::size_t i = kBuiltInPools; i < buffers.size(); ++i)
				declare(i);
	}

	rhi::TimelinePoint FrameValues::FrameWaitPoint() const
	{
		auto* host = RenderGraphRuntime::Get().Host();
		const auto timeline = RenderGraphRuntime::Get().FrameWaitTimeline();
		const std::uint64_t value = host && impl->kicked ? host->FrameWaitValue() : 0;
		if (!value || !timeline || !*timeline)
			return {};
		return { (*timeline)->GetHandle(), value };
	}

	namespace
	{
		/** @brief Blocks until a_ticket has left the upload queue. */
		void WaitTicketLeftQueue(org::TrackedUploadTicket& a_ticket)
		{
			const auto pending = [&] {
				const auto state = a_ticket.state.load(std::memory_order_acquire);
				return state == org::TrackedUploadTicketState::Queued || state == org::TrackedUploadTicketState::Claimed;
			};
			if (!pending())
				return;
			auto changes = std::make_shared<std::atomic<std::uint32_t>>(0u);
			a_ticket.SetChangeCallback([changes] {
				changes->fetch_add(1, std::memory_order_release);
				changes->notify_all();
			});
			for (auto seen = changes->load(std::memory_order_acquire); pending(); seen = changes->load(std::memory_order_acquire))
				changes->wait(seen, std::memory_order_acquire);
		}
	}

	void FrameValues::WaitSignalSubmitted()
	{
		auto& s = *impl;
		if (!s.kicked)
			return;
		// The copy queue signal must be submitted before the present. A present can block on the frame's GPU work while holding the
		// submission lock the upload queue needs, and that work waits for the signal.
		{
			DCLF_ZONE("CS.DCLF.FrameValues.SignalSubmitted");
			for (auto done = s.done.load(std::memory_order_acquire); done < s.seq; done = s.done.load(std::memory_order_acquire))
				s.done.wait(done, std::memory_order_acquire);
			const auto ticket = s.signal;
			if (!ticket || s.presentWaitSeq == s.seq)
				return;
			const auto state = ticket->state.load(std::memory_order_acquire);
			if (state != org::TrackedUploadTicketState::Queued && state != org::TrackedUploadTicketState::Claimed)
				return;
			// With a queued native present, the submission thread waits for the signal before presenting. The copy batch only waits for
			// sealed points, which are already queued, so it cannot need the present.
			const bool queuedPresent = RenderGraph::Engine::Native::Active() && RenderGraphRuntime::Get().QueuesPresent() &&
			                           !globals::features::upscaling.IsFrameGenerationActive() && !LoadHold::PresentsHeld();
			if (queuedPresent) {
				s.presentWaitSeq = s.seq;
				RenderGraphRuntime::Get().WaitBeforePresent([ticket] {
					DCLF_ZONE("CS.DCLF.FrameValues.PresentWaitsSignal");
					WaitTicketLeftQueue(*ticket);
				});
				return;
			}
			// The copy batch may wait for graph work still in DXVK's stream. Flush it first or the wait never ends.
			RenderGraphRuntime::Get().FlushPendingStreamWork();
			WaitTicketLeftQueue(*ticket);
		}
	}

	void FrameValues::EndFrame()
	{
		auto& s = *impl;
		if (!s.kicked)
			return;
		WaitSignalSubmitted();
		s.kicked = false;
		// The render is done reading its rows. A job still running keeps them; the next one replaces them after the next Kick.
		if (s.done.load(std::memory_order_acquire) >= s.seq) {
			s.renderRows.reset();
			s.renderPalettes.reset();
			s.renderShading.reset();
		}
		BindFrameLeases(s.frameLeases, s.registeredFrameLeases, s.boundPoints);
		s.ReleaseLatePoses();
		s.RetireFieldOwners();
	}

	bool FrameValues::ProducerDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return false;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		return s.done.load(std::memory_order_acquire) >= s.seq;
	}

	void FrameValues::WaitSampled() const
	{
		auto& s = *impl;
		const auto kicked = s.sampleKicked.load(std::memory_order_acquire);
		for (auto sampled = s.sampledSeq.load(std::memory_order_acquire); sampled < kicked; sampled = s.sampledSeq.load(std::memory_order_acquire))
			s.sampledSeq.wait(sampled, std::memory_order_acquire);
	}

	void FrameValues::NoteRefused()
	{
		++impl->refused;
	}

	const org::async::KeptChunks<BindlessPlacement>* FrameValues::RowsIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return s.renderRows.get();
	}

	bool FrameValues::ForwardRowsFilled(const std::vector<Direct::ForwardSortRow>* a_target) const
	{
		const auto& s = *impl;
		// The producer wrote them before it stored done.
		return a_target && s.seq && s.done.load(std::memory_order_acquire) >= s.seq && s.forwardRowsSeq == s.seq && s.forwardRowsTarget == a_target;
	}

	const org::async::KeptChunks<FrameValues::PaletteRow>* FrameValues::PalettesIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return s.renderPalettes.get();
	}

	const org::async::KeptChunks<BindlessShading>* FrameValues::ShadingIfDone(bool a_wait) const
	{
		auto& s = *impl;
		if (!s.seq)
			return nullptr;
		if (a_wait)
			for (auto finished = s.done.load(std::memory_order_acquire); finished < s.seq; finished = s.done.load(std::memory_order_acquire))
				s.done.wait(finished, std::memory_order_acquire);
		if (s.done.load(std::memory_order_acquire) < s.seq)
			return nullptr;
		return s.renderShading.get();
	}

	FrameValues::Counters FrameValues::TakeCounters()
	{
		auto& s = *impl;
		Counters c;
		c.frames = s.frames.exchange(0);
		if (!c.frames)
			return c;
		c.sampled = s.sampled.exchange(0);
		c.settled = s.settled.exchange(0);
		c.seeded = s.seeded.exchange(0);
		c.rowsChanged = s.rowsChanged.exchange(0);
		c.skins = s.skins.exchange(0);
		c.paletteRowsChanged = s.paletteRowsChanged.exchange(0);
		c.shadingSampled = s.shadingSampled.exchange(0);
		c.shadingChanged = s.shadingChanged.exchange(0);
		c.wetnessChanged = s.wetnessChanged.exchange(0);
		c.runs = s.runs.exchange(0);
		c.bytes = s.bytes.exchange(0);
		c.sampleUs = s.sampleUs.exchange(0);
		c.uploadUs = s.uploadUs.exchange(0);
		c.maxTotalUs = s.maxTotalUs.exchange(0);
		c.refused = s.refused.exchange(0);
		c.defects = s.defects.exchange(0);
		c.failures = s.failures.exchange(0);
		c.plans = s.plansApplied.exchange(0);
		c.planResets = s.planResets.exchange(0);
		c.planItems = s.planItems.exchange(0);
		c.movers = s.movers.exchange(0);
		c.moversDue = s.moversDue.exchange(0);
		c.roots = s.roots.exchange(0);
		c.rootsDue = s.rootsDue.exchange(0);
		c.moveKeys = s.moveKeys.exchange(0);
		c.moveNodes = s.moveNodes.exchange(0);
		c.reusedInFlight = s.reusedInFlight.exchange(0);
		return c;
	}

	std::uint32_t FrameValues::PoolIndex(Api::FrameValuePoolId a_pool) const
	{
		const auto& indices = impl->registeredIndex;
		return a_pool.index < indices.size() ? indices[a_pool.index] : 0u;
	}

	std::uint64_t FrameValues::PoolAddress(Api::FrameValuePoolId a_pool) const
	{
		const auto backing = PoolBacking(a_pool);
		return backing ? backing->address : 0;
	}

	RenderGraph::BufferRef FrameValues::PoolRef(Api::FrameValuePoolId a_pool) const
	{
		const auto backing = PoolBacking(a_pool);
		auto* host = RenderGraphRuntime::Get().Host();
		if (!backing || !backing->buffer || !host)
			return {};
		// Not a raw ref, so the graph tracks the pool's writers and readers.
		const RenderGraph::BufferRef ref = RenderGraph::MakeBufferRef(host->Device(), backing->buffer->GetAPIResource().GetHandle(), 0, std::source_location::current());
		(void)impl->LateBuffers();  // makes the latches
		if (a_pool.index < impl->poolRefLatches.size())
			impl->poolRefLatches[a_pool.index]->Set(ref);
		return ref;
	}

	std::uint64_t FrameValues::PoolBytes(Api::FrameValuePoolId a_pool) const
	{
		const auto backing = PoolBacking(a_pool);
		return backing ? backing->byteCapacity : 0;
	}

	std::shared_ptr<const org::services::Backing> FrameValues::PoolBacking(Api::FrameValuePoolId a_pool) const
	{
		const auto& s = *impl;
		if (!s.kicked || a_pool.index >= s.registeredFrameLeases.size())
			return nullptr;
		return s.registeredFrameLeases[a_pool.index].Get();
	}

	bool FrameValues::Available() const
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto uploads = host ? host->RetainUploads() : nullptr;
		return host && RenderGraphRuntime::Get().FrameWaitTimeline() && uploads && uploads->HasDedicatedStreamingQueue();
	}

	std::vector<FrameValues::PoolStats> FrameValues::TakePoolStats()
	{
		auto& s = *impl;
		const std::size_t registered = s.registeredCount;
		std::vector<PoolStats> out(kBuiltInPools + registered);
		for (std::size_t part = 0; part < out.size(); ++part) {
			auto& pool = part < kBuiltInPools ? s.pools[part] : s.registered[part - kBuiltInPools].pool;
			auto& stats = out[part];
			stats.name = part < kBuiltInPools ? Impl::kPartNames[part] : s.registered[part - kBuiltInPools].entry->name.c_str();
			const auto census = pool.pool->Count();
			stats.live = census.live;
			stats.leased = census.leased;
			stats.free = census.free;
			stats.liveBytes = census.bytes;
			stats.allocated = census.allocated;
			stats.reused = census.reused;
			stats.released = census.released;
			stats.acquires = pool.acquires.exchange(0, std::memory_order_relaxed);
			stats.made = pool.made.exchange(0, std::memory_order_relaxed);
			stats.runs = pool.runs.exchange(0, std::memory_order_relaxed);
			stats.bytes = pool.bytes.exchange(0, std::memory_order_relaxed);
			stats.floor = pool.floor.load(std::memory_order_relaxed);
		}
		return out;
	}
}
