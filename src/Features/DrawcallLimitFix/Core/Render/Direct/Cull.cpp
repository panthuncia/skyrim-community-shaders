// The CPU-driven pipeline's cull. It runs BuildDrawsCS.hlsl's per-input decisions over a ViewBatch's lanes, publishes the
// frame's CPU cull state, and diffs the main camera's verdicts for the visibility decoder.
// The decisions must match the shader's. A change to one is a change to both.

#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/CullSimd.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <Tracy/Tracy.hpp>

#include <emmintrin.h>
#include <intrin.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <new>

namespace DCLF::Direct
{
	namespace
	{
		constexpr std::uint32_t kNoRoot = 0xFFFFFFFFu;
		constexpr std::uint32_t kNoProgram = 0xFFFFFFFFu;  // must match Portals' kPortalNoProgram
		constexpr std::uint32_t kObjectCrossfadeCopy = 1u << 30;
		constexpr std::uint32_t kDrawAll = 0xFFFFu;
		// The frustum stamp word holds the stamp in the low 28 bits and the fade test's drop in bit 31.
		constexpr std::uint32_t kStampMask = 0x0FFFFFFFu;
		constexpr std::uint32_t kStampFadeHidden = 0x80000000u;
		// Skylighting's occlusion map draws nothing with a bound radius of 32 or less.
		constexpr float kOcclusionMinRadius = 32.0f;

		bool MainCamera(ViewType a_type)
		{
			return a_type == ViewType::Main || a_type == ViewType::Decal || a_type == ViewType::Forward || a_type == ViewType::Refraction ||
			       a_type == ViewType::Water;
		}
		// Shadow epoch views drop casters under a fading root.
		bool ShadowFamily(ViewType a_type)
		{
			return a_type == ViewType::Sun || a_type == ViewType::Spot || a_type == ViewType::Point || a_type == ViewType::SkyOccluder ||
			       a_type == ViewType::PrecipOccluder;
		}

		// ---- Frustum stamps: per object slot, the frame it was last in the main camera's frustum. Chunks are made on first
		// write by compare-exchange and never moved or freed while DCLF runs. ----

		class StampTable
		{
		public:
			static constexpr std::uint32_t kChunkShift = 16;
			static constexpr std::uint32_t kChunk = 1u << kChunkShift;
			static constexpr std::uint32_t kChunks = (kObjectIndexMask + 1) >> kChunkShift;

			~StampTable()
			{
				for (auto& chunk : chunks)
					delete[] chunk.load(std::memory_order_acquire);
			}
			std::uint32_t Load(std::uint32_t a_object) const
			{
				const auto* chunk = chunks[(a_object & kObjectIndexMask) >> kChunkShift].load(std::memory_order_acquire);
				return chunk ? chunk[a_object & (kChunk - 1)].load(std::memory_order_relaxed) : 0u;
			}
			void Store(std::uint32_t a_object, std::uint32_t a_word)
			{
				Chunk(a_object)[a_object & (kChunk - 1)].store(a_word, std::memory_order_relaxed);
			}

		private:
			std::atomic<std::uint32_t>* Chunk(std::uint32_t a_object)
			{
				auto& slot = chunks[(a_object & kObjectIndexMask) >> kChunkShift];
				if (auto* chunk = slot.load(std::memory_order_acquire))
					return chunk;
				auto* made = new std::atomic<std::uint32_t>[kChunk]();
				std::atomic<std::uint32_t>* expected = nullptr;
				if (slot.compare_exchange_strong(expected, made, std::memory_order_acq_rel, std::memory_order_acquire))
					return made;
				delete[] made;
				return expected;
			}
			std::array<std::atomic<std::atomic<std::uint32_t>*>, kChunks> chunks{};
		};
		StampTable stamps;

		// ---- The frame state ----

		struct Arm
		{
			std::shared_ptr<const ViewBatchSet> set;
			std::uint32_t frame = 0, lastArmed = 0;
			bool armed = false, everArmed = false, observer = false;
		};
		Arm arm;  // render thread
		std::atomic<std::uint32_t> cpuStatesFrame{ ~0u };
		struct RowStash
		{
			std::uint64_t seq = 0;
			const BindlessPlacement* rows = nullptr;
			std::size_t count = 0;
		};
		RowStash rowStash;  // one producer at a time

		// Readers count themselves before loading, both sides seq_cst. A publish that sees no reader frees what it replaced,
		// otherwise the replaced frames wait for a publish that sees none.
		struct Held
		{
			std::shared_ptr<const CullFrame> frame;
		};
		std::atomic<const Held*> latest{ nullptr };
		std::atomic<std::uint32_t> latestReaders{ 0 };
		std::vector<const Held*> retired;  // publisher only

		struct Waiter
		{
			std::uint32_t frame = 0;
			std::function<void(const std::shared_ptr<const CullFrame>&)> then;
			Waiter* next = nullptr;
		};
		std::atomic<Waiter*> waiters{ nullptr };

		void PushWaiter(Waiter* a_waiter)
		{
			a_waiter->next = waiters.load(std::memory_order_relaxed);
			while (!waiters.compare_exchange_weak(a_waiter->next, a_waiter, std::memory_order_release, std::memory_order_relaxed)) {}
		}
		bool Reached(std::uint32_t a_published, std::uint32_t a_wanted) { return static_cast<std::int32_t>(a_published - a_wanted) >= 0; }

		/** @brief Runs every waiter the newest frame satisfies, each exactly once, and puts the rest back. */
		void RunReady()
		{
			for (;;) {
				const auto current = LatestCullFrame();
				if (!current)
					return;
				Waiter* later = nullptr;
				for (Waiter* w = waiters.exchange(nullptr, std::memory_order_acq_rel); w;) {
					Waiter* following = w->next;
					if (Reached(current->sceneFrame, w->frame)) {
						w->then(current);
						delete w;
					} else {
						w->next = later;
						later = w;
					}
					w = following;
				}
				if (!later)
					return;
				for (Waiter* w = later; w;) {
					Waiter* following = w->next;
					PushWaiter(w);
					w = following;
				}
				// A frame was published while the waiters were taken out, so look again.
				const auto now = LatestCullFrame();
				if (!now || now->sceneFrame == current->sceneFrame)
					return;
			}
		}

		// ---- The tests ----

		/** @brief A view's sphere test planes. A bound is outside where dot(n, c) - d < -r. */
		struct PlaneSet
		{
			std::array<std::array<float, 4>, 12> planes{};
			std::uint32_t count = 0;
			void Add(const std::array<float, 4>& a_plane) { planes[count++] = a_plane; }
		};

		PlaneSet PlanesOf(const CullView& a_view)
		{
			PlaneSet set;
			for (std::uint32_t p = 0; p < 6; ++p)
				if (a_view.planeMask & (1u << p))
					set.Add(a_view.planes[p]);
			for (std::uint32_t p = 0; p < 6; ++p) {
				// A clamped shadow view pancakes casters in front of its near plane onto it.
				if (p == 4 && (a_view.cullFlags & Draws::kCullNoNearPlane))
					continue;
				if (a_view.clipPlanes[p][0] != 0.0f || a_view.clipPlanes[p][1] != 0.0f || a_view.clipPlanes[p][2] != 0.0f)
					set.Add(a_view.clipPlanes[p]);
			}
			return set;
		}

		struct Spheres
		{
			const float *x, *y, *z, *r;
			Simd::Spheres simd{ x, y, z, r };
		};

		/** @brief The widest frustum kernel the CPU has, null for the SSE pass. */
		org::cpu::FrustumFn WideFrustum()
		{
			return org::cpu::WideFrustum(Simd::Supported());
		}

		/** @brief The wide paraboloid kernel the CPU has, null for the SSE pre-pass and per-lane test. */
		Simd::ParabolicFn WideParabolic()
		{
			const Simd::Level level = Simd::Supported();
			return level == Simd::Level::Avx512 ? &Simd::ParabolicAvx512 : level == Simd::Level::Avx2 ? &Simd::ParabolicAvx2 : nullptr;
		}

		std::uint32_t ParabolicFlagsOf(const CullView& a_view)
		{
			return ((a_view.cullFlags & Draws::kCullParabolicBothSides) ? Simd::kParabolicBothSides : 0u) | ((a_view.cullFlags & Draws::kCullParabolicBack) ? Simd::kParabolicBack : 0u);
		}

		/** @brief ParabolicSphereMask's first test, four lanes at a time. a_pass[i] is 0 where the bound misses the view's sphere. */
		void ParabolicPass(const Spheres& a_spheres, std::uint32_t a_count, const std::array<float, 4>& a_sphere, std::uint8_t* a_pass)
		{
			const __m128 sx = _mm_set1_ps(a_sphere[0]), sy = _mm_set1_ps(a_sphere[1]), sz = _mm_set1_ps(a_sphere[2]), sr = _mm_set1_ps(a_sphere[3]);
			std::uint32_t i = 0;
			for (; i + 4 <= a_count; i += 4) {
				const __m128 r = _mm_loadu_ps(a_spheres.r + i);
				const __m128 dx = _mm_sub_ps(_mm_loadu_ps(a_spheres.x + i), sx), dy = _mm_sub_ps(_mm_loadu_ps(a_spheres.y + i), sy),
							 dz = _mm_sub_ps(_mm_loadu_ps(a_spheres.z + i), sz);
				const __m128 reach = _mm_sub_ps(_mm_sub_ps(_mm_sqrt_ps(_mm_add_ps(_mm_add_ps(_mm_mul_ps(dx, dx), _mm_mul_ps(dy, dy)), _mm_mul_ps(dz, dz))), r), sr);
				const int bits = _mm_movemask_ps(_mm_or_ps(_mm_cmplt_ps(reach, _mm_setzero_ps()), _mm_cmplt_ps(r, _mm_setzero_ps())));
				for (std::uint32_t k = 0; k < 4; ++k)
					a_pass[i + k] = static_cast<std::uint8_t>((bits >> k) & 1);
			}
			for (; i < a_count; ++i) {
				const float dx = a_spheres.x[i] - a_sphere[0], dy = a_spheres.y[i] - a_sphere[1], dz = a_spheres.z[i] - a_sphere[2];
				a_pass[i] = a_spheres.r[i] < 0.0f || (std::sqrt((dx * dx + dy * dy) + dz * dz) - a_spheres.r[i]) - a_sphere[3] < 0.0f ? 1u : 0u;
			}
		}

		/** @brief ParabolicPrograms.hlsli's ParabolicSphereMask (FUN_14151a1e0). Keep its sqrt, subtraction order and strict comparisons. */
		std::uint32_t ParabolicSphereMask(const float a_bound[4], const std::array<float, 4>& a_sphere, const std::array<float, 4>& a_plane, bool a_bothSides)
		{
			const float dx = a_bound[0] - a_sphere[0], dy = a_bound[1] - a_sphere[1], dz = a_bound[2] - a_sphere[2];
			if (!((std::sqrt((dx * dx + dy * dy) + dz * dz) - a_bound[3]) - a_sphere[3] < 0.0f))
				return 0;
			const float side = ((a_bound[1] * a_plane[1] + a_bound[0] * a_plane[0]) + a_bound[2] * a_plane[2]) - a_plane[3];
			if (side - a_bound[3] < 0.0f)
				return a_bothSides ? 3u : 1u;
			return side > 0.0f ? 1u : (a_bothSides ? 2u : 0u);
		}

		/** @brief The front hemisphere takes codes 1 and 3, the back one 2 and 3. */
		bool ParabolicRejected(const CullView& a_view, const float a_bound[4])
		{
			if (a_bound[3] < 0.0f)
				return false;
			const std::uint32_t code = ParabolicSphereMask(a_bound, a_view.planes[0], a_view.planes[1], (a_view.cullFlags & Draws::kCullParabolicBothSides) != 0);
			return (a_view.cullFlags & Draws::kCullParabolicBack) ? (code & 2u) == 0 : (code & 1u) == 0;
		}

		/** @brief The local shadow lights whose sphere the bound meets and whose planes it is inside. */
		std::uint32_t LocalShadowMask(const CullView& a_view, const float a_bound[4], bool a_landscape, std::uint32_t a_object)
		{
			std::uint32_t mask = 0;
			const float radius = a_bound[3] + a_view.planeMargin;
			for (std::uint32_t v = 0; v < a_view.shadowVolumeCount; ++v) {
				const auto& volume = a_view.shadowVolumes[v];
				if ((mask & volume.maskBit) || (a_landscape && !volume.affectsLand))
					continue;
				const float dx = a_bound[0] - volume.sphere[0], dy = a_bound[1] - volume.sphere[1], dz = a_bound[2] - volume.sphere[2];
				const float reach = volume.sphere[3] + radius;
				if (dx * dx + dy * dy + dz * dz > reach * reach)
					continue;
				bool inside = true;
				for (std::uint32_t p = 0; p < 12 && inside; ++p) {
					if (!((p < 6 ? volume.masks[0] : volume.masks[1]) & (1u << (p % 6))))
						continue;
					const float* plane = volume.planes[p];
					inside = plane[0] * a_bound[0] + plane[1] * a_bound[1] + plane[2] * a_bound[2] - plane[3] >= -radius;
				}
				if (inside && volume.portalContext && a_view.strict && a_view.strict->rejects &&
					a_view.strict->rejects(*a_view.strict, volume.portalContext - 1, a_object, a_bound, radius))
					inside = false;
				if (inside)
					mask |= volume.maskBit;
			}
			return mask;
		}

		bool InSunCascades(const CullView& a_view, const float a_bound[4])
		{
			for (std::uint32_t c = 0; c < a_view.cascadeCount; ++c)
				if (Draws::InSunCascade(a_view.cascades[c], a_bound, a_bound[3] + a_view.planeMargin))
					return true;
			return false;
		}

		/** @brief The draw's object word for a view: the index, the sun miss and the local shadow mask. */
		std::uint32_t ObjectWordOf(const CullView& a_view, std::uint32_t a_object, std::uint32_t a_flags, const float a_bound[4])
		{
			std::uint32_t word = a_object & kObjectIndexMask;
			if (a_view.sunTest && (a_flags & kLaneSunTest) && !InSunCascades(a_view, a_bound))
				word |= kObjectSunMiss;
			if (a_view.shadowVolumeCount)
				word |= (LocalShadowMask(a_view, a_bound, (a_flags & kLaneLandscapeLights) != 0, a_object) & 0xFu) << kObjectLocalShadowShift;
			return word;
		}

		std::uint32_t RootWordOf(const CullFrame& a_frame, std::uint32_t a_root)
		{
			return a_root < a_frame.rootWords.size() ? a_frame.rootWords[a_root] : 0u;
		}

		/** @brief Only a positive radius grows by the margin. The test culls a zero radius, and a negative one is unknown. */
		float PortalRadius(float a_radius, float a_margin)
		{
			return a_radius > 0.0f ? a_radius + a_margin : a_radius;
		}

		bool PortalRejected(const CullFrame& a_frame, std::uint32_t a_root, const float a_bound[4], float a_margin)
		{
			if (a_root == kNoRoot || a_root >= a_frame.rootPrograms.size() || a_frame.programs.empty() || !a_frame.portalTest)
				return false;
			const std::uint32_t program = a_frame.rootPrograms[a_root];
			if (program == kNoProgram)
				return false;
			return a_frame.portalTest(a_frame.programs, program, a_bound, PortalRadius(a_bound[3], a_margin), 0u) != 1;
		}

		/**
		 * @brief Stamps a main-camera lane that is in the frustum and returns the fade drop. Uses the root's OnVisible verdict
		 * this frame if it ran, else the fade-out distance test against the previous stamp.
		 */
		bool StampFrustum(const CullFrame& a_frame, std::uint32_t a_object, std::uint32_t a_root, std::uint32_t a_flags, float a_fadeDistance)
		{
			bool fadeHidden = false;
			const std::uint32_t rootWord = a_root != kNoRoot ? RootWordOf(a_frame, a_root) : 0u;
			const bool serviced = (rootWord & kRootServiced) != 0;
			if (serviced)
				fadeHidden = (rootWord & kRootDrawn) == 0;
			const auto& eye = a_frame.fadeEye;
			if (!serviced && (a_flags & kLaneFadeTest) && a_fadeDistance != 0.0f && eye[3] != 0.0f && a_object < a_frame.fadeNode.size()) {
				const std::uint32_t previous = stamps.Load(a_object);
				const bool wasInView = (previous & kStampMask) == ((a_frame.stamp - 1) & kStampMask);
				const bool wasHidden = wasInView && (previous & kStampFadeHidden);
				const auto& node = a_frame.fadeNode[a_object];
				const float dx = node[0] - eye[0], dy = node[1] - eye[1], dz = node[2] - eye[2];
				const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) * (a_fadeDistance > 0.0f ? eye[3] : 1.0f);
				fadeHidden = distance > std::abs(a_fadeDistance) && (!wasInView || wasHidden);
			}
			stamps.Store(a_object, a_frame.stamp | (fadeHidden ? kStampFadeHidden : 0u));
			return fadeHidden;
		}

		/** @brief BSTreeNode::OnVisible's height test. A tree whose root is above the frame's limit draws nothing. */
		bool AboveTreeHeight(const CullFrame& a_frame, std::uint32_t a_object)
		{
			return a_object < a_frame.fadeNode.size() && a_frame.fadeNode[a_object][2] - a_frame.treeHeight[0] > a_frame.treeHeight[1];
		}

		/** @brief Maps a lane's partition bits to its sequence bits. The k-th set bit is sequence k. */
		std::uint32_t SequenceBits(std::uint32_t a_partitions, std::uint32_t a_union)
		{
			std::uint32_t bits = 0;
			for (std::uint32_t rest = a_partitions & a_union; rest; rest &= rest - 1) {
				const std::uint32_t bit = static_cast<std::uint32_t>(std::countr_zero(rest));
				bits |= 1u << std::popcount(a_union & ((1u << bit) - 1u));
			}
			return bits;
		}

		/**
		 * @brief A LOD skin's partitions at its root's level as a draw mask. In a main view, a root crossing levels also draws
		 * the cross-fade copy. False when the level draws none of them.
		 */
		bool LodMask(const CullFrame& a_frame, std::uint32_t a_lod, std::uint32_t a_partitions, std::uint32_t a_root, bool a_mainFamily, std::uint32_t& a_mask)
		{
			a_mask = kDrawAll;
			if (a_lod == 0)
				return true;
			const std::uint32_t all = (a_lod | (a_lod >> 8) | (a_lod >> 16) | (a_lod >> 24)) & 0xFFu;
			const std::uint32_t rootWord = a_root != kNoRoot ? RootWordOf(a_frame, a_root) : 0u;
			if (!(rootWord & kRootListed)) {
				// No LOD state, so the partitions are used as is.
				if (a_partitions != 0)
					a_mask = SequenceBits(a_partitions, all);
				return a_mask != 0;
			}
			const std::uint32_t level = (rootWord >> kRootLodShift) & 3u;
			const std::uint32_t mask = (a_lod >> (8u * level)) & 0xFFu;
			const std::uint32_t copy = a_mainFamily && (rootWord & kRootLodCopy) && level < 3 ? ((a_lod >> (8u * (level + 1))) & 0xFFu) & ~mask : 0u;
			if (mask == 0 && copy == 0)
				return false;
			a_mask = SequenceBits(mask | copy, all) | (SequenceBits(copy, all) << 16);
			return true;
		}

		struct Lane
		{
			std::uint32_t object = 0, flags = 0, root = kNoRoot;
			float fadeDistance = 0.0f;
			float bound[4]{};
		};

		/** @brief A shadow view's drops before culling: caster class, sun entry, size tests and fading roots. */
		bool ShadowDropped(const CullView& a_view, const CullFrame& a_frame, const Lane& a_lane)
		{
			const bool volumetric = (a_lane.flags & kLaneVolumetricOnly) != 0;
			if (((a_view.cullFlags & Draws::kCullCastersOnly) && volumetric) || ((a_view.cullFlags & Draws::kCullVolumetricOnly) && !volumetric))
				return true;
			if ((a_view.cullFlags & Draws::kCullSunEntry) && a_view.sunEntryCount && a_lane.object < a_frame.sunEntry.size()) {
				const auto& entry = a_frame.sunEntry[a_lane.object];
				const float grown[4]{ entry[0], entry[1], entry[2], entry[3] + a_view.planeMargin };
				if (Draws::OutsideSunEntryProcesses(std::span(a_view.sunEntry, a_view.sunEntryCount), grown))
					return true;
			}
			const bool radiusKnown = a_lane.bound[3] >= 0.0f;
			if ((a_view.cullFlags & Draws::kCullMinRadius) && radiusKnown && a_lane.bound[3] <= kOcclusionMinRadius)
				return true;
			if ((a_view.cullFlags & Draws::kCullMinTexels) && radiusKnown && a_lane.bound[3] < a_view.minCasterRadius)
				return true;
			if (a_lane.root == kNoRoot)
				return false;
			const std::uint32_t rootWord = RootWordOf(a_frame, a_lane.root);
			return (a_view.cullFlags & Draws::kCullFadeOnVisible) ? (rootWord & kRootFadedOutOfOcclusion) != 0 : (rootWord & kRootFading) != 0;
		}

		/**
		 * @brief A main-camera lane. Writes the stamp for kAccumulated, then decides the draw. Main uses the stamp and the
		 * height test, the other lists the frustum alone, and refraction also drops a root whose OnVisible stopped.
		 */
		bool MainCameraKept(const CullView& a_view, const CullFrame& a_frame, const Lane& a_lane, bool a_inFrustum, const std::uint8_t* a_portalRejected)
		{
			const bool portalPassed = a_inFrustum && !(a_portalRejected ? *a_portalRejected != 0 : PortalRejected(a_frame, a_lane.root, a_lane.bound, a_view.planeMargin));
			bool fadeHidden = false;
			if (portalPassed && a_frame.reportsVisibility && !a_view.noStamp)
				fadeHidden = StampFrustum(a_frame, a_lane.object, a_lane.root, a_lane.flags, a_lane.fadeDistance);
			if (a_view.stampOnly || (a_lane.flags & kLaneCullOnly))
				return false;
			if (a_view.type == ViewType::Main) {
				if (portalPassed && (a_lane.flags & kLaneHeightTest))
					fadeHidden = fadeHidden || AboveTreeHeight(a_frame, a_lane.object);
				return portalPassed && !fadeHidden;
			}
			if (!a_inFrustum)
				return false;
			if (a_view.type == ViewType::Refraction && a_lane.root != kNoRoot) {
				const std::uint32_t rootWord = RootWordOf(a_frame, a_lane.root);
				if ((rootWord & kRootServiced) && !(rootWord & kRootDrawn))
					return false;
			}
			return true;
		}

		/** @brief Any other view's lane. Its drops, then its frustum or paraboloid, then its strict gates. */
		bool ViewKept(const CullView& a_view, const CullFrame& a_frame, const Lane& a_lane, bool a_inFrustum, bool a_parabolicDecided)
		{
			if (a_lane.flags & kLaneCullOnly)
				return false;
			if (ShadowFamily(a_view.type) && ShadowDropped(a_view, a_frame, a_lane))
				return false;
			if (!a_view.frustumCull)
				return true;
			// The wide paraboloid pass may already have decided it, otherwise run the per-lane test.
			const bool rejected = (a_view.cullFlags & Draws::kCullParabolic) && !a_parabolicDecided ? ParabolicRejected(a_view, a_lane.bound) : !a_inFrustum;
			if (rejected)
				return false;
			if (a_view.strictContext && a_view.strict && a_view.strict->rejects && a_lane.bound[3] >= 0.0f &&
				a_view.strict->rejects(*a_view.strict, a_view.strictContext - 1, a_lane.object, a_lane.bound, a_lane.bound[3] + a_view.planeMargin))
				return false;
			return true;
		}

		/** @brief Portal tests for the main camera's lanes in the frustum, grouped by their root's program with a counting sort. */
		void PortalPass(const CullFrame& a_frame, const ViewBatch& a_batch, const Spheres& a_spheres, const std::uint8_t* a_pass, std::uint32_t a_count, float a_margin,
			std::uint8_t* a_rejected)
		{
			std::memset(a_rejected, 0, a_count);
			if (a_frame.programs.empty() || !a_batch.fadeRoot)
				return;
			const std::uint32_t programs = a_frame.programs.size() > 32 ? a_frame.programs[28] : 0u;  // the header's program count, word 112 / 4
			if (!programs)
				return;
			thread_local std::vector<std::uint32_t> firsts;
			thread_local std::vector<std::uint16_t> lanes;
			thread_local std::vector<std::uint32_t> programOf;
			firsts.assign(std::size_t(programs) + 2, 0u);
			programOf.resize(a_count);
			// Pass 1 counts each lane's program.
			for (std::uint32_t i = 0; i < a_count; ++i) {
				std::uint32_t program = kNoProgram;
				if (a_pass[i]) {
					const std::uint32_t root = a_batch.fadeRoot[i] ? a_batch.fadeRoot[i] - 1 : kNoRoot;
					if (root != kNoRoot && root < a_frame.rootPrograms.size())
						program = a_frame.rootPrograms[root];
				}
				programOf[i] = program;
				if (program == kNoProgram)
					continue;
				if (program >= programs) {
					a_rejected[i] = 1;
					continue;
				}
				++firsts[std::size_t(program) + 2];
			}
			for (std::size_t p = 2; p < firsts.size(); ++p)
				firsts[p] += firsts[p - 1];
			lanes.resize(firsts.back());
			for (std::uint32_t i = 0; i < a_count; ++i)
				if (programOf[i] < programs)
					lanes[firsts[std::size_t(programOf[i]) + 1]++] = static_cast<std::uint16_t>(i);
			thread_local std::vector<float> radii;
			radii.resize(a_count);
			for (std::uint32_t i = 0; i < a_count; ++i)
				radii[i] = PortalRadius(a_spheres.r[i], a_margin);
			// firsts[p] is now program p's first lane and firsts[p + 1] its end.
			for (std::uint32_t p = 0; p < programs; ++p)
				if (const std::uint32_t first = firsts[p], end = firsts[std::size_t(p) + 1]; end > first)
					a_frame.portalTestBatch(a_frame.programs, p, a_spheres.x, a_spheres.y, a_spheres.z, radii.data(), lanes.data() + first, end - first, a_rejected);
		}

		Spheres SpheresOf(const ViewBatch& a_batch, std::uint32_t a_parity)
		{
			const auto& live = a_batch.liveSpheres[a_parity & 1u];
			return { live.cx, live.cy, live.cz, live.r };
		}

		Lane LaneOf(const ViewBatch& a_batch, const Spheres& a_spheres, std::uint32_t a_i)
		{
			Lane lane;
			lane.object = a_batch.object[a_i] & kObjectIndexMask;
			lane.flags = a_batch.flags ? a_batch.flags[a_i] : 0u;
			lane.root = a_batch.fadeRoot && a_batch.fadeRoot[a_i] ? a_batch.fadeRoot[a_i] - 1 : kNoRoot;
			lane.fadeDistance = a_batch.fadeDistance ? a_batch.fadeDistance[a_i] : 0.0f;
			lane.bound[0] = a_spheres.x[a_i];
			lane.bound[1] = a_spheres.y[a_i];
			lane.bound[2] = a_spheres.z[a_i];
			lane.bound[3] = a_spheres.r[a_i];
			return lane;
		}

		void DiffVisibility(const CullFrame& a_frame);
	}

	// ---- The frame state ----

	void KickCpuFrameState(std::shared_ptr<const ViewBatchSet> a_set, bool a_observer)
	{
		arm.set = std::move(a_set);
		arm.frame = Scene::FrameView::Get().SceneStep();
		arm.armed = arm.set != nullptr;
		arm.observer = a_observer;
	}

	CpuFrameArm TakeCpuFrameArm()
	{
		const std::uint32_t frame = Scene::FrameView::Get().SceneStep();
		if (!arm.armed || arm.frame != frame) {
			arm.armed = false;
			arm.set.reset();
			return {};
		}
		arm.armed = false;
		// An observer's frame stays a GPU frame. The GPU's states stay bound, and the next CPU frame restarts visibility.
		if (arm.observer)
			return { std::move(arm.set), frame, false, true };
		CpuFrameArm taken{ std::move(arm.set), frame, !arm.everArmed || arm.lastArmed + 1 != frame };
		arm.everArmed = true;
		arm.lastArmed = frame;
		cpuStatesFrame.store(frame, std::memory_order_release);
		return taken;
	}

	namespace
	{
		std::atomic<StrictGatesSource> strictGatesSource{ nullptr };
	}

	void SetStrictGatesSource(StrictGatesSource a_source)
	{
		strictGatesSource.store(a_source, std::memory_order_release);
	}

	std::shared_ptr<const StrictGates> FrameStrictGates(const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto source = strictGatesSource.load(std::memory_order_acquire);
		if (!source)
			return nullptr;
		auto gates = std::make_shared<const StrictGates>(source(a_scene, a_frame));
		return gates->rejects ? gates : nullptr;
	}

	bool CpuRootStatesFor(std::uint32_t a_sceneFrame)
	{
		return cpuStatesFrame.load(std::memory_order_acquire) == a_sceneFrame;
	}

	void OnFrameValuesSampled(std::uint64_t a_seq, std::span<const BindlessPlacement> a_rows)
	{
		rowStash = { a_seq, a_rows.data(), a_rows.size() };
	}

	std::span<const BindlessPlacement> SampledRows(std::uint64_t a_seq)
	{
		if (rowStash.seq != a_seq || !rowStash.rows)
			return {};
		return { rowStash.rows, rowStash.count };
	}

	void PublishCullFrame(std::shared_ptr<CullFrame> a_frame)
	{
		// The main camera's lists still to cull, plus one for this publication, dropped after the waiters run.
		std::uint32_t batches = 1;
		if (a_frame->set)
			for (const ViewType type : { ViewType::Main, ViewType::Decal, ViewType::Forward, ViewType::Refraction })
				batches += static_cast<std::uint32_t>(a_frame->set->List(type).size());
		// The water list is culled by the mask's site and the surface's. Only the mask's stamps.
		if (a_frame->set && a_frame->waterSites)
			batches += static_cast<std::uint32_t>(a_frame->set->List(ViewType::Water).size());
		a_frame->pendingStamps.store(batches, std::memory_order_relaxed);
		auto* held = new Held{ a_frame };
		const Held* replaced = latest.exchange(held, std::memory_order_seq_cst);
		if (replaced)
			retired.push_back(replaced);
		if (latestReaders.load(std::memory_order_seq_cst) == 0) {
			for (const Held* old : retired)
				delete old;
			retired.clear();
		}
		RunReady();
		if (a_frame->pendingStamps.fetch_sub(1, std::memory_order_acq_rel) == 1 && a_frame->reportsVisibility)
			DiffVisibility(*a_frame);
	}

	void DropCullFrame()
	{
		const Held* replaced = latest.exchange(nullptr, std::memory_order_seq_cst);
		if (replaced)
			retired.push_back(replaced);
		if (!retired.empty() && latestReaders.load(std::memory_order_seq_cst) == 0) {
			for (const Held* old : retired)
				delete old;
			retired.clear();
		}
	}

	void AfterCullFrame(std::uint32_t a_sceneFrame, std::function<void(const std::shared_ptr<const CullFrame>&)> a_then)
	{
		PushWaiter(new Waiter{ a_sceneFrame, std::move(a_then) });
		RunReady();
	}

	std::shared_ptr<const CullFrame> LatestCullFrame()
	{
		latestReaders.fetch_add(1, std::memory_order_seq_cst);
		const Held* held = latest.load(std::memory_order_seq_cst);
		std::shared_ptr<const CullFrame> frame = held ? held->frame : nullptr;
		latestReaders.fetch_sub(1, std::memory_order_release);
		return frame;
	}

	// ---- Wide kernel support. The level comes from ORGModuleServices' sphere cull. ----

	namespace Simd
	{
		Level Supported()
		{
			static const Level level = [] {
				const Level detected = org::cpu::Supported();
				logger::info("[DCLF] CPU pipeline: the cull's widest kernels: {}", NameOf(detected));
				return detected;
			}();
			return level;
		}
	}

	// ---- The cull ----

	namespace
	{
		/** @brief Runs the frustum or paraboloid over the lanes into a_pass. True when a_pass holds a paraboloid view's whole verdict. */
		bool PassOf(const ViewBatch& a_batch, const Spheres& a_spheres, std::uint32_t a_count, const CullView& a_view, std::uint8_t* a_pass)
		{
			// A view without a frustum test keeps every lane past its other tests.
			bool deadFolded = false, parabolicDecided = false;
			if (a_view.frustumCull && !(a_view.cullFlags & Draws::kCullParabolic)) {
				const PlaneSet planes = PlanesOf(a_view);
				if (const auto wide = WideFrustum()) {
					wide(a_spheres.simd, a_count, reinterpret_cast<const float (*)[4]>(planes.planes.data()), planes.count, a_batch.dead, a_pass);
					deadFolded = true;
				} else {
					org::cpu::FrustumSse(a_spheres.simd, a_count, reinterpret_cast<const float (*)[4]>(planes.planes.data()), planes.count, a_pass);
				}
			} else if (a_view.frustumCull) {
				if (const auto wide = WideParabolic()) {
					wide(a_spheres.simd, a_count, a_view.planes[0].data(), a_view.planes[1].data(), ParabolicFlagsOf(a_view), a_batch.dead, a_pass);
					deadFolded = parabolicDecided = true;
				} else {
					ParabolicPass(a_spheres, a_count, a_view.planes[0], a_pass);
				}
			}
			// A lane gone from this set passes nothing.
			if (a_batch.dead && !deadFolded)
				for (std::uint32_t i = 0; i < a_count; ++i)
					if ((a_batch.dead[i >> 6] >> (i & 63)) & 1u)
						a_pass[i] = 0;
			return parabolicDecided;
		}

		/** @brief Calls a_test(i) for each lane a_pass keeps, scanning sixteen pass bytes at a time. */
		template <class Test>
		void ForEachPassed(const std::uint8_t* a_pass, std::uint32_t a_count, Test&& a_test)
		{
			std::uint32_t base = 0;
			for (; base + 16 <= a_count; base += 16) {
				const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i*>(a_pass + base));
				for (auto bits = static_cast<std::uint32_t>(_mm_movemask_epi8(_mm_cmpgt_epi8(bytes, _mm_setzero_si128()))); bits; bits &= bits - 1)
					a_test(base + static_cast<std::uint32_t>(std::countr_zero(bits)));
			}
			for (; base < a_count; ++base)
				if (a_pass[base])
					a_test(base);
		}
	}

	const CulledBatch& CullBatch(const ViewBatch& a_batch, std::uint32_t a_parity, const CullView& a_view, const CullFrame& a_frame, CullArena& a_arena)
	{
		auto& out = *new (a_arena.Make<CulledBatch>(1)) CulledBatch{};
		out.batch = &a_batch;
		out.view = a_view.viewIndex;
		const std::uint32_t count = a_batch.count;
		auto* lanes = a_arena.Make<std::uint16_t>(std::max(count, 1u));
		auto* words = a_arena.Make<std::uint32_t>(std::max(count, 1u));
		auto* masks = a_arena.Make<std::uint32_t>(std::max(count, 1u));
		thread_local std::vector<std::uint8_t> pass;
		pass.assign(count, 1u);
		const Spheres spheres = SpheresOf(a_batch, a_parity);
		const bool parabolicDecided = PassOf(a_batch, spheres, count, a_view, pass.data());
		const bool mainCamera = MainCamera(a_view.type);
		const bool mainFamily = mainCamera || a_view.type == ViewType::Reflection || a_view.type == ViewType::FirstPerson;
		thread_local std::vector<std::uint8_t> portalRejected;
		const bool portalBatch = mainCamera && a_frame.portalTestBatch;
		if (portalBatch) {
			portalRejected.resize(count);
			PortalPass(a_frame, a_batch, spheres, pass.data(), count, a_view.planeMargin, portalRejected.data());
		}
		std::uint32_t kept = 0;
		DCLF_ZONE("CS.DCLF.Cpu.CullLanes");
		const auto test = [&](std::uint32_t i) {
			const Lane lane = LaneOf(a_batch, spheres, i);
			// A focus view only draws its target's members.
			if (a_view.focusRoot != 0 && lane.root != a_view.focusRoot - 1)
				return;
			// A focus-only caster, such as a hidden actor part, is drawn by focus views alone.
			if (a_view.focusRoot == 0 && (lane.flags & kLaneFocusOnly))
				return;
			const bool keep = mainCamera ? MainCameraKept(a_view, a_frame, lane, pass[i] != 0, portalBatch ? &portalRejected[i] : nullptr) :
			                               ViewKept(a_view, a_frame, lane, pass[i] != 0, parabolicDecided);
			if (!keep)
				return;
			std::uint32_t mask = kDrawAll;
			if (a_batch.lodPartitions && !LodMask(a_frame, a_batch.lodPartitions[i], a_batch.partitions ? a_batch.partitions[i] : 0u, lane.root, mainFamily, mask))
				return;
			lanes[kept] = static_cast<std::uint16_t>(i);
			words[kept] = ObjectWordOf(a_view, lane.object, lane.flags, lane.bound);
			masks[kept] = mask;
			++kept;
		};
		// A lane outside the frustum is kept by no view, and only a lane inside stamps. Shadow views pass only a few lanes in a hundred.
		ForEachPassed(pass.data(), count, test);
		out.count = kept;
		out.lanes = lanes;
		out.objectWords = words;
		out.drawMasks = masks;
		// The frame's last main-camera list diffs the verdicts for kAccumulated.
		if (mainCamera && !a_view.noStamp && a_frame.reportsVisibility && a_frame.pendingStamps.fetch_sub(1, std::memory_order_acq_rel) == 1)
			DiffVisibility(a_frame);
		return out;
	}

	const CulledBatch& ColourObjectWords(const CulledBatch& a_depth, const CullView& a_colour, const CullFrame& a_frame, CullArena& a_arena)
	{
		auto& out = *new (a_arena.Make<CulledBatch>(1)) CulledBatch(a_depth);
		auto* words = a_arena.Make<std::uint32_t>(std::max(a_depth.count, 1u));
		const auto& batch = *a_depth.batch;
		const Spheres spheres = SpheresOf(batch, static_cast<std::uint32_t>(LiveParity(a_frame.frameValuesSeq)));
		for (std::uint32_t k = 0; k < a_depth.count; ++k) {
			const Lane lane = LaneOf(batch, spheres, a_depth.lanes[k]);
			words[k] = ObjectWordOf(a_colour, lane.object, lane.flags, lane.bound);
		}
		out.objectWords = words;
		return out;
	}

	// ---- kAccumulated: VisibilityEventsCS on the CPU ----

	namespace
	{
		/** @brief The diff's state. One diff runs at a time, and a frame that finds one running is skipped. The next reports its changes. */
		struct VisibilityDiff
		{
			std::atomic<bool> running{ false };
			std::vector<std::uint32_t> objectReported;  // kReportedValid | the bits last reported
			std::vector<std::pair<std::uint32_t, std::uint32_t>> roots;  // the generation and the bits last reported
			std::atomic<bool> pendingReset{ false };  // a skipped frame was a reset, so the next diff resets
			std::atomic<std::uint64_t> diffs{ 0 }, skipped{ 0 };
		};
		VisibilityDiff visibilityDiff;
		constexpr std::uint32_t kReportedValid = 1u << 31;

		void DiffRoots(const CullFrame& a_frame, VisibilityDiff& a_diff, std::vector<Draws::VisibilityEvent>& a_events)
		{
			const std::size_t roots = std::min(a_frame.rootWords.size(), a_frame.rootGenerations.size());
			if (a_diff.roots.size() < roots)
				a_diff.roots.resize(roots, { 0u, 0u });
			for (std::uint32_t r = 0; r < roots; ++r) {
				const std::uint32_t generation = a_frame.rootGenerations[r];
				const std::uint32_t word = a_frame.rootWords[r];
				if (!(word & kRootListed) || generation == 0)
					continue;
				const std::uint32_t bits = (word & kRootUpdated) ? (Draws::kVisibilityBitVerdict | ((word & kRootInView) ? Draws::kVisibilityBitVisible : 0u)) : 0u;
				auto& last = a_diff.roots[r];
				const std::uint32_t lastBits = last.first == generation ? last.second : 0u;
				if (lastBits != bits)
					a_events.push_back({ r, generation, Draws::kVisibilityKindRoot, bits });
				last = { generation, bits };
			}
		}

		void DiffObjects(const CullFrame& a_frame, std::size_t a_objects, VisibilityDiff& a_diff, std::vector<Draws::VisibilityEvent>& a_events)
		{
			if (a_diff.objectReported.size() < a_objects)
				a_diff.objectReported.resize(a_objects, 0u);
			const std::uint32_t stamp = a_frame.stamp & kStampMask;
			for (std::uint32_t o = 0; o < a_objects; ++o) {
				const std::uint32_t word = stamps.Load(o);
				const bool stamped = (word & kStampMask) == stamp;
				const std::uint32_t bits = stamped ? ((word & kStampFadeHidden) ? Draws::kVisibilityBitFadeHidden : Draws::kVisibilityBitVisible) : 0u;
				auto& last = a_diff.objectReported[o];
				const std::uint32_t lastBits = (last & kReportedValid) ? (last & ~kReportedValid) : 0u;
				if (lastBits != bits)
					a_events.push_back({ o, 0u, Draws::kVisibilityKindObject, bits });
				last = kReportedValid | bits;
			}
		}

		void DiffVisibility(const CullFrame& a_frame)
		{
			DCLF_ZONE("CS.DCLF.Cpu.DiffVisibility");
			auto& diff = visibilityDiff;
			if (bool idle = false; !diff.running.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) {
				if (a_frame.visibilityReset)
					diff.pendingReset.store(true, std::memory_order_release);
				diff.skipped.fetch_add(1, std::memory_order_relaxed);
				return;
			}
			auto* batch = new Draws::VisibilityReadback::Batch();
			batch->frame = a_frame.sceneFrame;
			batch->reset = a_frame.visibilityReset || diff.pendingReset.exchange(false, std::memory_order_acq_rel);
			batch->tables = a_frame.tables;
			batch->publication = a_frame.publication;
			batch->retirement = a_frame.retirement;
			batch->candidates = a_frame.candidates;
			// A reset reports every verdict again, as a new backing's zeroed words do on the GPU.
			if (batch->reset) {
				diff.objectReported.clear();
				diff.roots.clear();
			}
			DiffRoots(a_frame, diff, batch->events);
			DiffObjects(a_frame, a_frame.tables ? a_frame.tables->objects.size() : 0u, diff, batch->events);
			diff.diffs.fetch_add(1, std::memory_order_relaxed);
			diff.running.store(false, std::memory_order_release);
			Draws::PostCpuVisibility(batch);
		}
	}

	// ---- The views ----

	namespace
	{
		/**
		 * @brief The view-projection's normalised clip planes in world space: -x, +x, -y, +y, near, far. Computed in double so
		 * each plane is rounded to float only once.
		 */
		std::array<std::array<float, 4>, 6> ClipPlanesOf(const double a_viewProj[16])
		{
			const auto row = [&](std::uint32_t a_r) { return std::array<double, 4>{ a_viewProj[a_r * 4], a_viewProj[a_r * 4 + 1], a_viewProj[a_r * 4 + 2], a_viewProj[a_r * 4 + 3] }; };
			const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
			const auto add = [](const std::array<double, 4>& a, const std::array<double, 4>& b, double s) {
				return std::array<double, 4>{ a[0] + s * b[0], a[1] + s * b[1], a[2] + s * b[2], a[3] + s * b[3] };
			};
			const std::array<std::array<double, 4>, 6> raw{ add(r3, r0, 1.0), add(r3, r0, -1.0), add(r3, r1, 1.0), add(r3, r1, -1.0), r2, add(r3, r2, -1.0) };
			std::array<std::array<float, 4>, 6> planes{};
			for (std::size_t p = 0; p < 6; ++p) {
				const double length = std::sqrt(raw[p][0] * raw[p][0] + raw[p][1] * raw[p][1] + raw[p][2] * raw[p][2]);
				if (!(length > 0.0))
					continue;  // a degenerate plane tests nothing
				planes[p] = { static_cast<float>(raw[p][0] / length), static_cast<float>(raw[p][1] / length), static_cast<float>(raw[p][2] / length),
					static_cast<float>(-raw[p][3] / length) };
			}
			return planes;
		}

		/**
		 * @brief Moves a view's planes out by its margin, which is the same as adding it to every bound's radius. A paraboloid's plane
		 * moves only for the back hemisphere. Moving it for the front one would reject a zero radius on the plane.
		 */
		void ApplyPlaneMargin(CullView& a_view)
		{
			if (a_view.cullFlags & Draws::kCullParabolic) {
				a_view.planes[0][3] += a_view.planeMargin;
				if (a_view.cullFlags & Draws::kCullParabolicBack)
					a_view.planes[1][3] += a_view.planeMargin;
				return;
			}
			for (std::size_t p = 0; p < 6; ++p) {
				a_view.planes[p][3] -= a_view.planeMargin;
				a_view.clipPlanes[p][3] -= a_view.planeMargin;
			}
		}
	}

	CullView CullViewOfLatch(ViewType a_type, std::uint32_t a_viewIndex, const Draws::BuildDrawsLatch& a_latch)
	{
		CullView view;
		view.type = a_type;
		view.viewIndex = a_viewIndex;
		double folded[16];
		Draws::FoldCullEye(a_latch.viewProj, a_latch.cullEye, folded);
		for (std::size_t i = 0; i < 16; ++i)
			view.viewProj[i] = static_cast<float>(folded[i]);
		for (std::size_t p = 0; p < 6; ++p)
			std::copy(std::begin(a_latch.cullPlanes[p]), std::end(a_latch.cullPlanes[p]), view.planes[p].begin());
		view.planeMask = a_latch.cullPlaneMask;
		view.cullFlags = a_latch.cullFlags;
		view.eye = { a_latch.fadeEye[0], a_latch.fadeEye[1], a_latch.fadeEye[2] };
		view.fadeEyeDistanceScale = a_latch.fadeEye[3];
		view.treeHeightBase = a_latch.treeHeight[0];
		view.treeHeight = a_latch.treeHeight[1];
		view.focusRoot = a_latch.focusRoot;
		view.frame = a_latch.visibilityStamp;
		// Cull mode 0 means no view-projection this frame, so nothing is frustum-tested.
		view.frustumCull = (a_latch.cullFlags & 0xFu) != 0;
		view.clipPlanes = ClipPlanesOf(folded);
		view.planeMargin = a_latch.cullEye[3];
		ApplyPlaneMargin(view);
		view.minCasterRadius = a_latch.minCasterRadius;
		view.strictContext = a_latch.lightContextsIndex && a_latch.strictGatesIndex ? a_latch.strictContext : 0u;
		view.sunTest = (a_latch.sunState & Draws::kSunTestOn) != 0;
		return view;
	}

	CullView MainCullView(const Draws::BuildDrawsLatch& a_depthLatch)
	{
		return CullViewOfLatch(ViewType::Main, 0, a_depthLatch);
	}

	CullView ColourCullView(ViewType a_type, const Draws::BuildDrawsLatch& a_colourLatch, std::span<const Draws::GpuCascade> a_cascades,
		std::span<const Draws::GpuShadowVolume> a_volumes)
	{
		CullView view = CullViewOfLatch(a_type, 0, a_colourLatch);
		view.cascades = a_cascades.data();
		view.cascadeCount = static_cast<std::uint32_t>(a_cascades.size());
		// A localShadowOffset of 0 means no local shadow region.
		if (a_colourLatch.localShadowOffset) {
			view.shadowVolumes = a_volumes.data();
			view.shadowVolumeCount = static_cast<std::uint32_t>(a_volumes.size());
		}
		return view;
	}

	CullView ShadowCullView(ViewType a_type, std::uint32_t a_slot, const Draws::BuildDrawsLatch& a_latch, std::span<const Draws::SunEntryProcess> a_sunEntry)
	{
		CullView view = CullViewOfLatch(a_type, a_slot, a_latch);
		if (a_latch.cullFlags & Draws::kCullSunEntry) {
			view.sunEntry = a_sunEntry.data();
			view.sunEntryCount = static_cast<std::uint32_t>(a_sunEntry.size());
		}
		return view;
	}

	CullView OcclusionCullView(ViewType a_type, std::uint32_t a_view, const Draws::BuildDrawsLatch& a_latch)
	{
		return CullViewOfLatch(a_type, a_view, a_latch);
	}

	CullView ReflectionCullView(std::uint32_t a_face, const Draws::BuildDrawsLatch& a_latch)
	{
		return CullViewOfLatch(ViewType::Reflection, a_face, a_latch);
	}

	CullView FirstPersonCullView(const Draws::BuildDrawsLatch& a_latch)
	{
		return CullViewOfLatch(ViewType::FirstPerson, 0, a_latch);
	}
}
