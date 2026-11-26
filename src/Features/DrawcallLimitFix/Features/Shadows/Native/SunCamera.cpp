// Port of BSShadowDirectionalLight::UpdateCamera (AE 0x141512230 id 108496, SE 0x1413251c0 id 101499) and its helpers:
//   sun direction smoothing   AE 0x1415148c0 (108497)  SE 0x141327740 (101500); SE keeps the bDisableShadowJumps branch in
//                             0x1413279e0 (101501), AE inlines it
//   cull plane fit            AE 0x141514c10 (108499)  SE 0x141327aa0 (101502)
//   convex hull, split step   AE 0x140e19490 (76219), 0x140e1a030 (76226)  SE 0x140d42710 (74493), 0x140d43120
//   NiCamera::WorldPtToScreen AE 0x140d2c5b0 (70639)   SE 0x140c66530 (69269)
//   NiCamera::SetViewFrustum  AE 0x140d2b0e0 (70626)   SE inlined call of the same name
//   NiFrustum ctor            AE 0x140d33fa0           SE 0x140c6d900 (69453)
//   NiPlane from 3 points     AE 0x140d32ab0 (70804)   SE 0x140c6c3e0
//   NiPlane from normal+point AE 0x140d32a60 (70803)   SE 0x140c6c390 (69426)
//   NiPoint3::Normalize       AE 0x1402b08d0 (18829)   SE 0x1401799d0
// Float operations follow the AE disassembly's order. The SE build groups a few sums differently (frustum corners); that is an
// ulp-level difference only.

#include "Features/DrawcallLimitFix/Features/Shadows/Native/SunCamera.h"

#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <limits>
#include <xmmintrin.h>

namespace DCLF::Features::Shadows::Native::SunCameraImpl
{
	using Descriptor = RE::BSShadowLight::ShadowmapDescriptor;
	using SunData = RE::BSShadowDirectionalLight::RUNTIME_DATA;

	// Raw offsets with no CommonLib member. SE and AE use the same ones.
	constexpr std::size_t kNiAVObjectFlags = 0xF4;     // NiAVObject flags: bit 0 hides the light, and UpdateCamera then does nothing
	constexpr std::size_t kUnitsPerTexel = 0xCC;       // ShadowmapDescriptor::unitsPerTexel: CommonLib says uint32, the engine stores a float
	constexpr std::size_t kLightPending = 0xD0;        // BSLight::unk0D0, cleared at the end
	constexpr std::size_t kMemoryContextSlot = 0x768;  // in the thread's TLS block: the allocation context id
	constexpr std::uint32_t kMemoryContextId = 0x1D;   // the id the engine sets around its allocations here
	constexpr std::size_t kCameraSize = 0x188;         // sizeof(NiCamera), flat
	constexpr std::size_t kAccumulatorSize = 0x180;    // sizeof(BSShaderAccumulator), flat
	constexpr std::uint32_t kAccumulatorArg = 99;      // the accumulator constructor's argument
	constexpr std::uint32_t kMaxCascades = 3;          // the light's split arrays hold 3; the engine does not check

	// Plain .rdata constants (AE address, SE address).
	constexpr float kMovedDistance = 0.01f;   // 0x141ad2818, 0x14152fb58
	constexpr float kEpsilon = 1e-6f;         // 0x141ad27f8, 0x14152fa28
	constexpr float kBackOff = 15000.0f;      // 0x141ac54bc, 0x14186bd88: shadow cameras sit this far back along the sun; their far plane
	constexpr float kDepthScale = 14980.0f;   // 0x141ac54b8, 0x14186bd84
	constexpr float kDepthPad = 20.0f;        // 0x141ad28d4, 0x141536b9c
	constexpr float kOrthoNear = 10.0f;       // 0x141ad28c8, 0x1415483f8
	constexpr float kProjectEpsilon = 1e-5f;  // 0x1417afdc4, 0x14156b7b4
	constexpr float kMinFirstSlice = 500.0f;  // 0x141ad29e4, 0x14155cfe0
	constexpr float kHalf = 0.5f;             // 0x141ad285c, 0x141536b98
	constexpr float kTwo = 2.0f;              // 0x141ad288c, 0x141525c5c
	constexpr float kFloatMax = FLT_MAX;      // 0x1417743f8, 0x14152fa4c
	constexpr float kFloatLowest = -FLT_MAX;  // 0x14177ae08, 0x141536bb4
	// The sign mask at 0x141ad3130 (SE 0x14152d050) is written as unary minus, the abs mask at 0x141774410 as std::fabs, and
	// NiPoint3::ZERO (0x143272c98, SE 0x143012370) as zero.

	using CameraCtor = RE::NiCamera* (*)(void*);
	using AccumulatorCtor = RE::BSShaderAccumulator* (*)(void*, std::uint32_t);

	/** @brief The engine globals UpdateCamera reads, resolved once. Ids are (SE, AE). */
	struct EngineData
	{
		const float* viewShadowDistance;         // runtime clamp of the view's far distance (AE 0x1433dcfa8, SE 0x143283b7c)
		const std::uint8_t* disableShadowJumps;  // bDisableShadowJumps:Display
		const float* sunStaticTimeUpdateScale;   // fSunStaticTimeUpdateScale:Display
		const RE::NiPoint3* sunUp;               // (0, 1, 0), the up reference of the sun's basis (AE 0x142029cb8, SE 0x141e062d8)
		const std::uint32_t* resolution;         // the cascades' shadow map size (AE 0x14328cb54, SE 0x14302c7bc)
		const float* shadowDistance;             // fShadowDistance:Display
		const float* firstSliceDistance;         // fFirstSliceDistance:Display
		const float* splitOverlap;               // fSplitOverlap:Display
		const std::uint8_t* fullViewPort;        // bDirShadowMapFullViewPort:Display
		const float* jumpCos;                    // cosine of the jump angle, set by the light's constructor (AE 0x142035cc8, SE 0x141e10a48)
		const float* sunShadowUpdateTime;        // fSunShadowUpdateTime:Display
		const std::uint32_t* tlsIndex;           // the module's TLS index
		CameraCtor cameraCtor;                   // NiCamera::NiCamera
		AccumulatorCtor accumulatorCtor;         // BSShaderAccumulator::BSShaderAccumulator
	};

	template <class T>
	T Resolve(std::uint64_t a_se, std::uint64_t a_ae)
	{
		return reinterpret_cast<T>(REL::RelocationID(a_se, a_ae).address());
	}

	const EngineData& Data()
	{
		static const EngineData data{
			Resolve<const float*>(528314, 415263),
			Resolve<const std::uint8_t*>(513807, 391866),
			Resolve<const float*>(513801, 391857),
			Resolve<const RE::NiPoint3*>(511963, 388555),
			Resolve<const std::uint32_t*>(524993, 411474),
			Resolve<const float*>(513793, 391845),
			Resolve<const float*>(513795, 391848),
			Resolve<const float*>(513805, 391863),
			Resolve<const std::uint8_t*>(513803, 391860),
			Resolve<const float*>(513810, 391871),
			Resolve<const float*>(513799, 391854),
			Resolve<const std::uint32_t*>(528600, 415542),
			Resolve<CameraCtor>(69258, 70624),
			Resolve<AccumulatorCtor>(99920, 106564),
		};
		return data;
	}

	/** @brief The frame's real delta time: BSShaderManager::State timerValues[3] (AE 0x14203308c, SE 0x141e0defc). */
	float RealDelta()
	{
		return RE::BSShaderManager::State::GetSingleton().timerValues[RE::BSShaderManager::kRealDelta];
	}

	template <class T>
	T& Field(void* a_base, std::size_t a_offset)
	{
		return *reinterpret_cast<T*>(static_cast<std::byte*>(a_base) + a_offset);
	}

	/** @brief cvttss2si: truncation, 0x80000000 when out of range. */
	std::int32_t Trunc(float a_value)
	{
		return _mm_cvtt_ss2si(_mm_set_ss(a_value));
	}

	/** @brief The engine's ceil: truncate, then step up when positive with a fraction. Out of range values pass through. */
	float Ceil(float a_value)
	{
		const std::int32_t i = Trunc(a_value);
		if (i == std::numeric_limits<std::int32_t>::min() || static_cast<float>(i) == a_value)
			return a_value;
		return static_cast<float>(i + (std::signbit(a_value) ? 0 : 1));
	}

	/** @brief The engine's floor: truncate, then step down when negative with a fraction. */
	float Floor(float a_value)
	{
		const std::int32_t i = Trunc(a_value);
		if (i == std::numeric_limits<std::int32_t>::min() || static_cast<float>(i) == a_value)
			return a_value;
		return static_cast<float>(i - (std::signbit(a_value) ? 1 : 0));
	}

	/** @brief A texel count as the engine reads it: 64 bit truncation, low 32 bits, back to float. */
	float WholeCount(float a_value)
	{
		return static_cast<float>(static_cast<std::uint32_t>(_mm_cvttss_si64(_mm_set_ss(a_value))));
	}

	/** @brief NiPoint3::Normalize (AE 0x1402b08d0). */
	void Normalize(RE::NiPoint3& a_point)
	{
		const float length = std::sqrt((a_point.x * a_point.x + a_point.y * a_point.y) + a_point.z * a_point.z);
		if (length > kEpsilon) {
			const float inverse = 1.0f / length;
			a_point.x *= inverse;
			a_point.y *= inverse;
			a_point.z *= inverse;
		} else {
			a_point = { 0.0f, 0.0f, 0.0f };
		}
	}

	/** @brief NiCamera::SetViewFrustum (AE 0x140d2b0e0): copies the frustum, keeping near above far / maxFarNearRatio and the minimum. */
	void SetViewFrustum(RE::NiCamera& a_camera, const RE::NiFrustum& a_frustum)
	{
		auto& data = a_camera.GetRuntimeData2();
		float nearPlane = a_frustum.fNear;
		data.viewFrustum.fNear = nearPlane;
		const float ratioNear = a_frustum.fFar / data.maxFarNearRatio;
		if (nearPlane < ratioNear) {
			data.viewFrustum.fNear = ratioNear;
			nearPlane = ratioNear;
		}
		if (nearPlane < data.minNearPlaneDist)
			data.viewFrustum.fNear = data.minNearPlaneDist;
		data.viewFrustum.fLeft = a_frustum.fLeft;
		data.viewFrustum.fRight = a_frustum.fRight;
		data.viewFrustum.fTop = a_frustum.fTop;
		data.viewFrustum.fBottom = a_frustum.fBottom;
		data.viewFrustum.fFar = a_frustum.fFar;
		data.viewFrustum.bOrtho = a_frustum.bOrtho;
	}

	RE::NiFrustum MakeFrustum(float a_left, float a_right, float a_top, float a_bottom, float a_near, float a_far)
	{
		RE::NiFrustum frustum{};
		frustum.fLeft = a_left;
		frustum.fRight = a_right;
		frustum.fTop = a_top;
		frustum.fBottom = a_bottom;
		frustum.fNear = a_near;
		frustum.fFar = a_far;
		frustum.bOrtho = true;
		return frustum;
	}

	/** @brief NiRect's members are protected in CommonLib. The engine's layout: left, right, top, bottom. */
	template <class T>
	struct RectFields
	{
		T left, right, top, bottom;
	};
	template <class T>
	RectFields<T>& Fields(RE::NiRect<T>& a_rect)
	{
		static_assert(sizeof(RectFields<T>) == sizeof(RE::NiRect<T>));
		return reinterpret_cast<RectFields<T>&>(a_rect);
	}
	template <class T>
	const RectFields<T>& Fields(const RE::NiRect<T>& a_rect)
	{
		return reinterpret_cast<const RectFields<T>&>(a_rect);
	}

	void SetPort(RE::NiCamera& a_camera, float a_left, float a_right, float a_top, float a_bottom)
	{
		auto& port = Fields(a_camera.GetRuntimeData2().port);
		port.left = a_left;
		port.right = a_right;
		port.top = a_top;
		port.bottom = a_bottom;
	}

	/** @brief NiCamera::WorldPtToScreen (AE 0x140d2c5b0). Leaves a_out untouched and returns false when w is within a_epsilon. */
	bool WorldToScreen(const RE::NiCamera& a_camera, const RE::NiPoint3& a_point, RE::NiPoint3& a_out, float a_epsilon)
	{
		const auto& m = a_camera.GetRuntimeData().worldToCam;
		const auto& port = Fields(a_camera.GetRuntimeData2().port);
		const float w = std::fabs(((a_point.x * m[3][0] + a_point.y * m[3][1]) + a_point.z * m[3][2]) + m[3][3]);
		if (!(w > a_epsilon))
			return false;
		const float inverse = 1.0f / w;
		float x = ((a_point.y * m[0][1] + a_point.x * m[0][0]) + a_point.z * m[0][2]) + m[0][3];
		float y = ((m[1][1] * a_point.y + m[1][0] * a_point.x) + m[1][2] * a_point.z) + m[1][3];
		x = inverse * x;
		y = inverse * y;
		x = ((port.right - port.left) * kHalf) * x;
		y = ((port.top - port.bottom) * kHalf) * y;
		a_out.x = (port.right + port.left) * kHalf + x;
		a_out.y = (port.top + port.bottom) * kHalf + y;
		a_out.z = (((m[2][1] * a_point.y + m[2][0] * a_point.x) + m[2][2] * a_point.z) + m[2][3]) * inverse;
		return true;
	}

	/** @brief NiPlane from a normal and a point on it (AE 0x140d32a60). */
	RE::NiPlane PlaneAt(const RE::NiPoint3& a_normal, const RE::NiPoint3& a_point)
	{
		RE::NiPlane plane{};
		plane.normal = a_normal;
		plane.constant = (a_point.y * a_normal.y + a_normal.x * a_point.x) + a_point.z * a_normal.z;
		return plane;
	}

	/** @brief NiPlane through three points (AE 0x140d32ab0): normal (p2 - p1) x (p3 - p2) normalized, through p1. */
	RE::NiPlane PlaneThrough(const RE::NiPoint3& a_p1, const RE::NiPoint3& a_p2, const RE::NiPoint3& a_p3)
	{
		const float e1x = a_p2.x - a_p1.x;
		const float e1y = a_p2.y - a_p1.y;
		const float e1z = a_p2.z - a_p1.z;
		const float e2x = a_p3.x - a_p2.x;
		const float e2y = a_p3.y - a_p2.y;
		const float e2z = a_p3.z - a_p2.z;
		float nz = e2y * e1x - e2x * e1y;
		float ny = e2x * e1z - e2z * e1x;
		float nx = e2z * e1y - e2y * e1z;
		const float length = std::sqrt((ny * ny + nx * nx) + nz * nz);
		if (length > kEpsilon) {
			const float inverse = 1.0f / length;
			nx *= inverse;
			ny *= inverse;
			nz *= inverse;
		} else {
			nx = 0.0f;
			ny = 0.0f;
			nz = 0.0f;
		}
		RE::NiPlane plane{};
		plane.normal = { nx, ny, nz };
		plane.constant = (ny * a_p1.y + nx * a_p1.x) + nz * a_p1.z;
		return plane;
	}

	// The 2D convex hull of the projected corners (AE 0x140e19490, 0x140e1a030): a quickhull on integer points.
	constexpr std::uint32_t kHullPoints = 8;

	struct HullPoint
	{
		std::int32_t x;
		std::int32_t y;
	};

	struct HullNode
	{
		std::uint32_t point;
		std::uint32_t next;
	};

	struct HullLine
	{
		float nx;
		float ny;
		float d;
	};

	struct HullState
	{
		const HullPoint* points;
		std::array<HullNode, kHullPoints> nodes;
		std::uint32_t nodeCount;
	};

	HullLine LineThrough(const HullPoint& a_from, const HullPoint& a_to)
	{
		HullLine line{};
		line.nx = static_cast<float>(a_from.y) - static_cast<float>(a_to.y);
		line.ny = static_cast<float>(a_to.x) - static_cast<float>(a_from.x);
		line.d = static_cast<float>(a_from.x) * line.nx + line.ny * static_cast<float>(a_from.y);
		return line;
	}

	float LineSide(const HullLine& a_line, const HullPoint& a_point)
	{
		return (static_cast<float>(a_point.x) * a_line.nx + static_cast<float>(a_point.y) * a_line.ny) - a_line.d;
	}

	/** @brief AE 0x140e1a030: adds the candidate farthest from a_line between nodes a_from and a_to, then splits the rest. */
	void HullSplit(HullState& a_state, std::uint32_t a_from, std::uint32_t a_to, const HullLine& a_line, std::uint32_t a_count, const std::uint32_t* a_candidates)
	{
		const HullPoint* points = a_state.points;
		std::uint32_t best = 0;
		if (a_count != 0) {
			float bestDistance = std::fabs((static_cast<float>(points[a_candidates[0]].y) * a_line.ny + static_cast<float>(points[a_candidates[0]].x) * a_line.nx) - a_line.d);
			for (std::uint32_t k = 1; k < a_count; ++k) {
				const HullPoint& p = points[a_candidates[k]];
				const float distance = std::fabs((static_cast<float>(p.y) * a_line.ny + static_cast<float>(p.x) * a_line.nx) - a_line.d);
				if (distance > bestDistance) {
					best = k;
					bestDistance = distance;
				}
			}
		}
		const std::uint32_t added = a_state.nodeCount++;
		a_state.nodes[added].point = a_candidates[best];
		a_state.nodes[added].next = a_to;
		a_state.nodes[a_from].next = added;
		if (a_count <= 1)
			return;

		const HullPoint& peak = points[a_candidates[best]];
		const HullLine first = LineThrough(points[a_state.nodes[a_from].point], peak);
		const HullLine second = LineThrough(peak, points[a_state.nodes[a_to].point]);
		std::array<std::uint32_t, kHullPoints> firstSet{};
		std::array<std::uint32_t, kHullPoints> secondSet{};
		std::uint32_t firstCount = 0;
		std::uint32_t secondCount = 0;
		for (std::uint32_t k = 0; k < a_count; ++k) {
			if (k == best)
				continue;
			const std::uint32_t index = a_candidates[k];
			if (LineSide(first, points[index]) <= 0.0f) {
				if (0.0f < LineSide(second, points[index]))
					secondSet[secondCount++] = index;
			} else {
				firstSet[firstCount++] = index;
			}
		}
		if (firstCount != 0)
			HullSplit(a_state, a_from, added, first, firstCount, firstSet.data());
		if (secondCount != 0)
			HullSplit(a_state, added, a_to, second, secondCount, secondSet.data());
	}

	/** @brief AE 0x140e19490 for the 8 corners: writes the hull's point indices in order and returns their count. */
	std::uint32_t ConvexHull(const std::array<HullPoint, kHullPoints>& a_points, std::array<std::uint32_t, kHullPoints>& a_out)
	{
		HullState state{};
		state.points = a_points.data();
		state.nodes[0].point = 0;
		state.nodes[1].point = 0;
		for (std::uint32_t i = 1; i < kHullPoints; ++i) {
			const std::int32_t x = a_points[i].x;
			if (x < a_points[state.nodes[0].point].x)
				state.nodes[0].point = i;
			if (a_points[state.nodes[1].point].x < x)
				state.nodes[1].point = i;
		}
		state.nodes[0].next = 1;
		state.nodes[1].next = 0;
		state.nodeCount = 2;

		const HullLine line = LineThrough(a_points[state.nodes[0].point], a_points[state.nodes[1].point]);
		std::array<std::uint32_t, kHullPoints> upper{};
		std::array<std::uint32_t, kHullPoints> lower{};
		std::uint32_t upperCount = 0;
		std::uint32_t lowerCount = 0;
		for (std::uint32_t i = 0; i < kHullPoints; ++i) {
			if (i == state.nodes[0].point || i == state.nodes[1].point)
				continue;
			const float side = LineSide(line, a_points[i]);
			if (side <= 0.0f) {
				if (side < 0.0f)
					lower[lowerCount++] = i;
			} else {
				upper[upperCount++] = i;
			}
		}
		if (upperCount != 0)
			HullSplit(state, 0, 1, line, upperCount, upper.data());
		if (lowerCount != 0)
			HullSplit(state, 1, 0, line, lowerCount, lower.data());

		std::uint32_t count = 0;
		a_out[count++] = state.nodes[0].point;
		for (std::uint32_t node = state.nodes[0].next; node != 0 && count < kHullPoints; node = state.nodes[node].next)
			a_out[count++] = state.nodes[node].point;
		return count;
	}

	/** @brief The first 0x68 bytes of NiFrustumPlanes: what the engine builds and copies (unk68 and unk6C come from its stack). */
	struct PlaneSet
	{
		std::array<RE::NiPlane, 6> planes;
		std::uint32_t active;
		std::uint32_t base;
	};
	static_assert(sizeof(PlaneSet) == 0x68);
	static_assert(offsetof(RE::NiFrustumPlanes, activePlanes) == offsetof(PlaneSet, active));
	static_assert(offsetof(RE::NiFrustumPlanes, basePlaneStates) == offsetof(PlaneSet, base));

	PlaneSet ClearedPlanes()
	{
		PlaneSet set{};
		set.active = 0x3F;
		set.base = 0x3F;
		return set;
	}

	void WriteCustomPlanes(RE::NiCullingProcess& a_process, const PlaneSet& a_planes)
	{
		std::memcpy(&a_process.customCullPlanes, &a_planes, sizeof(PlaneSet));
		a_process.doCustomCullPlanes = true;
	}

	/**
	 * @brief AE 0x141514c10: side planes along the hull's 5 longest edges, each parallel to the sun, then a plane facing the sun
	 * through the farthest corner. a_corners are offsets from a_origin.
	 */
	void FitCullPlanes(PlaneSet& a_planes, const std::array<RE::NiPoint3, kHullPoints>& a_corners, const std::array<std::uint32_t, kHullPoints>& a_hull, std::uint32_t a_count,
		const RE::NiPoint3& a_sun, const RE::NiPoint3& a_origin, std::uint32_t a_farthest)
	{
		a_planes.active = 0;
		std::array<float, 5> lengths{};
		std::uint32_t shortest = 0;
		std::uint32_t i = 0;
		for (; i < a_count; ++i) {
			const RE::NiPoint3& a = a_corners[a_hull[i]];
			const RE::NiPoint3& b = a_corners[a_hull[(i + 1) % a_count]];
			const float dx = a.x - b.x;
			const float dy = a.y - b.y;
			const float dz = a.z - b.z;
			const float length = std::sqrt((dy * dy + dx * dx) + dz * dz);
			std::uint32_t slot = i;
			if (i > 4 && length > lengths[shortest]) {
				slot = shortest;
				lengths[shortest] = length;
				shortest = 0;
				for (std::uint32_t j = 1; j < i && j < 5; ++j) {
					if (lengths[j] < lengths[shortest])
						shortest = j;
				}
			}
			if (slot >= 5)
				continue;
			lengths[slot] = length;
			if (length < lengths[shortest])
				shortest = slot;
			const RE::NiPoint3 p1{ a_origin.x + a.x, a_origin.y + a.y, a_origin.z + a.z };
			const RE::NiPoint3 p2{ a_origin.x + b.x, a_origin.y + b.y, a_origin.z + b.z };
			const RE::NiPoint3 p3{ (a_origin.x + a.x) - a_sun.x, (a_origin.y + a.y) - a_sun.y, (a_origin.z + a.z) - a_sun.z };
			a_planes.planes[slot] = PlaneThrough(p1, p2, p3);
			a_planes.active |= 1u << slot;
		}
		const std::uint32_t last = i < 5 ? i : 5;
		const RE::NiPoint3& farCorner = a_corners[a_farthest];
		const RE::NiPoint3 point{ farCorner.x + a_origin.x, farCorner.y + a_origin.y, farCorner.z + a_origin.z };
		a_planes.planes[last] = PlaneAt({ -a_sun.x, -a_sun.y, -a_sun.z }, point);
		a_planes.active |= 1u << last;
	}

	/** @brief AE 0x1415148c0: eases the sun direction toward the light's over fSunShadowUpdateTime and snaps on big jumps. */
	void SmoothSunDirection(SunData& a_sun, RE::NiPoint3& a_direction, std::uint32_t a_oldMode, const EngineData& a_data)
	{
		if (!(1.0f > a_sun.sunUpdate)) {
			if (a_sun.sunUpdateMode == 1) {
				a_sun.lastSunVector = a_sun.sunVector;
				a_sun.sunVector = a_direction;
				if (a_oldMode == 0)
					a_sun.sunUpdate = 0.0f;
			} else {
				bool settled = false;
				if (*a_data.disableShadowJumps == 0) {
					const RE::NiPoint3& current = a_sun.sunVector;
					const float dot = (current.y * a_direction.y + current.x * a_direction.x) + current.z * a_direction.z;
					const float threshold = *a_data.jumpCos;
					if (dot < threshold) {
						const float lower = 1.0f - ((1.0f - threshold) + (1.0f - threshold));
						if (dot < lower) {
							a_sun.sunVector = a_direction;
							a_sun.lastSunVector = a_direction;
							a_sun.sunUpdate = 1.0f;
						} else {
							a_sun.lastSunVector = a_sun.sunVector;
							a_sun.sunVector = a_direction;
							a_sun.sunUpdate = 0.0f;
						}
						settled = true;
					}
				}
				if (!settled)
					a_direction = a_sun.sunVector;
			}
		}

		if (a_sun.sunUpdate < 1.0f) {
			const float time = *a_data.sunShadowUpdateTime;
			float t = RealDelta() / (time > kEpsilon ? time : kEpsilon) + a_sun.sunUpdate;
			t = t < 1.0f ? t : 1.0f;
			a_sun.sunUpdate = t;
			const RE::NiPoint3 source = a_sun.sunUpdateMode == 1 ? a_direction : a_sun.sunVector;
			const RE::NiPoint3 last = a_sun.lastSunVector;
			a_direction.x = (source.x - last.x) * t + last.x;
			a_direction.y = (source.y - last.y) * t + last.y;
			a_direction.z = (source.z - last.z) * t + last.z;
			Normalize(a_direction);
			if (!(1.0f > a_sun.sunUpdate))
				a_sun.lastSunVector = a_sun.sunVector;
		}
	}

	/** @brief Sets the thread's allocation context id as the engine does around its allocations, and restores it. */
	class ScopedMemoryContext
	{
	public:
		explicit ScopedMemoryContext(const EngineData& a_data)
		{
			const auto* blocks = reinterpret_cast<const std::uintptr_t*>(__readgsqword(0x58));
			slot = reinterpret_cast<std::uint32_t*>(blocks[*a_data.tlsIndex] + kMemoryContextSlot);
			saved = *slot;
			*slot = kMemoryContextId;
		}
		~ScopedMemoryContext() { *slot = saved; }
		ScopedMemoryContext(const ScopedMemoryContext&) = delete;
		ScopedMemoryContext& operator=(const ScopedMemoryContext&) = delete;

	private:
		std::uint32_t* slot;
		std::uint32_t saved;
	};

	RE::NiCamera* CreateCamera(const EngineData& a_data)
	{
		ScopedMemoryContext context(a_data);
		void* memory = RE::MemoryManager::GetSingleton()->Allocate(kCameraSize, 0, false);
		return memory ? a_data.cameraCtor(memory) : nullptr;
	}

	RE::BSShaderAccumulator* CreateAccumulator(const EngineData& a_data)
	{
		ScopedMemoryContext context(a_data);
		void* memory = RE::MemoryManager::GetSingleton()->Allocate(kAccumulatorSize, 0, false);
		return memory ? a_data.accumulatorCtor(memory, kAccumulatorArg) : nullptr;
	}

	/** @brief The view frustum's 4 corners at a_distance, as offsets from the camera: (+right +up), (-right +up), (+right -up), (-right -up). */
	void FrustumCorners(RE::NiPoint3* a_out, const RE::NiPoint3& a_dir, const RE::NiPoint3& a_up, const RE::NiPoint3& a_right, float a_distance)
	{
		const RE::NiPoint3 d{ a_dir.x * a_distance, a_dir.y * a_distance, a_dir.z * a_distance };
		const RE::NiPoint3 u{ a_up.x * a_distance, a_up.y * a_distance, a_up.z * a_distance };
		const RE::NiPoint3 r{ a_right.x * a_distance, a_right.y * a_distance, a_right.z * a_distance };
		const RE::NiPoint3 l{ -a_right.x * a_distance, -a_right.y * a_distance, -a_right.z * a_distance };
		a_out[0] = { (r.x + u.x) + d.x, (r.y + u.y) + d.y, (r.z + u.z) + d.z };
		a_out[1] = { (l.x + u.x) + d.x, (l.y + u.y) + d.y, (l.z + u.z) + d.z };
		a_out[2] = { (r.x - u.x) + d.x, (r.y - u.y) + d.y, (r.z - u.z) + d.z };
		a_out[3] = { (l.x - u.x) + d.x, (l.y - u.y) + d.y, (l.z - u.z) + d.z };
	}

	/** @brief The same corners at a cascade's end, in the cascade loop's own operation order. */
	void CascadeCorners(RE::NiPoint3* a_out, const RE::NiPoint3& a_dir, const RE::NiPoint3& a_up, const RE::NiPoint3& a_right, float a_end)
	{
		const RE::NiPoint3 d{ a_end * a_dir.x, a_end * a_dir.y, a_end * a_dir.z };
		a_out[0] = { (a_right.x + a_up.x) * a_end + d.x, (a_up.y + a_right.y) * a_end + d.y, (a_right.z + a_up.z) * a_end + d.z };
		a_out[1] = { (-a_right.x + a_up.x) * a_end + d.x, (-a_right.y + a_up.y) * a_end + d.y, (-a_right.z + a_up.z) * a_end + d.z };
		a_out[2] = { (a_end * a_right.x - a_end * a_up.x) + d.x, (a_end * a_right.y - a_end * a_up.y) + d.y, (a_end * a_right.z - a_end * a_up.z) + d.z };
		a_out[3] = { (-a_right.x * a_end - a_end * a_up.x) + d.x, (-a_right.y * a_end - a_end * a_up.y) + d.y, (-a_right.z * a_end - a_end * a_up.z) + d.z };
	}

	/** @brief Projects origin + each corner through a_camera; a failed projection keeps that slot's last value, as the engine's does. */
	void ProjectCorners(const RE::NiCamera& a_camera, const RE::NiPoint3& a_origin, const std::array<RE::NiPoint3, kHullPoints>& a_corners,
		std::array<RE::NiPoint3, kHullPoints>& a_screen, std::array<HullPoint, kHullPoints>& a_points)
	{
		for (std::uint32_t k = 0; k < kHullPoints; ++k) {
			const RE::NiPoint3 point{ a_origin.x + a_corners[k].x, a_origin.y + a_corners[k].y, a_origin.z + a_corners[k].z };
			WorldToScreen(a_camera, point, a_screen[k], kProjectEpsilon);
			a_points[k] = { Trunc(Ceil(a_screen[k].x)), Trunc(Ceil(a_screen[k].y)) };
		}
	}

	struct Bounds
	{
		float minX = kFloatMax;
		float minY = kFloatMax;
		float minZ = kFloatMax;
		float maxX = kFloatLowest;
		float maxY = kFloatLowest;
		float maxZ = kFloatLowest;
	};

	float MinOf(float a_value, float a_current)
	{
		const float t = a_value < a_current ? a_value : a_current;
		return t < a_value ? t : a_value;
	}

	float MaxOf(float a_value, float a_current)
	{
		const float t = a_value > a_current ? a_value : a_current;
		return t > a_value ? t : a_value;
	}

	/**
	 * @brief The cascade's texel fit when bDirShadowMapFullViewPort is off: picks units per texel for the projected extent, snaps the
	 * extent to them and shrinks the viewport to it.
	 */
	void FitViewport(Descriptor& a_desc, Bounds& a_bounds, float a_resolution, float a_inverseResolution)
	{
		float& unitsPerTexel = Field<float>(&a_desc, kUnitsPerTexel);
		bool fits = false;
		const auto width = static_cast<std::uint16_t>(Trunc(Floor(a_bounds.maxX - a_bounds.minX)));
		const auto height = static_cast<std::uint16_t>(Trunc(Floor(a_bounds.maxY - a_bounds.minY)));
		const std::uint16_t size = width > height ? width : height;
		const float sizeF = static_cast<float>(size);
		if (sizeF > a_resolution * unitsPerTexel) {
			const float t = sizeF * a_inverseResolution;
			unitsPerTexel = Ceil(t);
			if (1.0f == unitsPerTexel && t < kHalf)
				unitsPerTexel = 1.0f / Floor(a_resolution / sizeF);
		} else if (sizeF * a_inverseResolution < 1.0f) {
			unitsPerTexel = Floor(a_resolution / sizeF);
			fits = true;
		}

		const float q = WholeCount(unitsPerTexel);
		const float inverse = 1.0f / q;
		if (fits) {
			a_bounds.maxX = Ceil(q * a_bounds.maxX) * inverse;
			a_bounds.minX = Floor(q * a_bounds.minX) * inverse;
			a_bounds.maxY = Ceil(q * a_bounds.maxY) * inverse;
			a_bounds.minY = Floor(q * a_bounds.minY) * inverse;
		} else {
			a_bounds.maxX = Ceil(a_bounds.maxX * inverse) * q;
			a_bounds.minX = Floor(inverse * a_bounds.minX) * q;
			a_bounds.maxY = Ceil(a_bounds.maxY * inverse) * q;
			a_bounds.minY = Floor(inverse * a_bounds.minY) * q;
		}
		const auto spanX = static_cast<std::int32_t>(static_cast<std::uint32_t>(Trunc(Floor(a_bounds.maxX - a_bounds.minX))) & 0xFFFF);
		const auto spanY = static_cast<std::int32_t>(static_cast<std::uint32_t>(Trunc(Floor(a_bounds.maxY - a_bounds.minY))) & 0xFFFF);
		const float scale = fits ? q : inverse;
		Fields(a_desc.port).right = static_cast<std::int32_t>(static_cast<std::uint32_t>(Trunc(Ceil(static_cast<float>(spanX) * scale))) & 0xFFFF);
		const std::uint32_t top = static_cast<std::uint32_t>(Trunc(Ceil(static_cast<float>(spanY) * scale))) & 0xFFFF;
		Fields(a_desc.port).top = static_cast<std::int32_t>(top + static_cast<std::uint32_t>(Fields(a_desc.port).bottom));
	}
}

namespace DCLF::Features::Shadows::Native
{
	bool UpdateSunCamera(RE::BSShadowDirectionalLight& a_light, const RE::NiCamera& a_viewCamera)
	{
		using namespace SunCameraImpl;

		RE::NiLight* niLight = a_light.light.get();
		if (Field<std::uint8_t>(niLight, kNiAVObjectFlags) & 1)
			return true;

		const EngineData& data = Data();
		SunData& sun = a_light.GetShadowDirectionalLightRuntimeData();
		const RE::NiFrustum& view = a_viewCamera.GetRuntimeData2().viewFrustum;
		const auto& viewRotate = a_viewCamera.world.rotate.entry;
		const RE::NiPoint3 viewPos = a_viewCamera.world.translate;

		// The view frustum's axes. Perspective scales up and right to the frustum's edges at distance 1.
		const float viewClamp = *data.viewShadowDistance;
		const float distance = viewClamp < view.fFar ? viewClamp : view.fFar;
		const float nearDistance = view.fNear;
		const RE::NiPoint3 dir{ viewRotate[0][0], viewRotate[1][0], viewRotate[2][0] };
		RE::NiPoint3 up{ viewRotate[0][1], viewRotate[1][1], viewRotate[2][1] };
		RE::NiPoint3 right{ viewRotate[0][2], viewRotate[1][2], viewRotate[2][2] };
		if (!view.bOrtho) {
			up = { up.x * view.fTop, up.y * view.fTop, up.z * view.fTop };
			right = { right.x * view.fRight, right.y * view.fRight, right.z * view.fRight };
		}

		// The sun direction: the light's world X axis, smoothed or snapped against the last one.
		const auto& lightRotate = niLight->world.rotate.entry;
		RE::NiPoint3 sunDir{ lightRotate[0][0], lightRotate[1][0], lightRotate[2][0] };
		Normalize(sun.sunVector);
		const std::uint32_t oldMode = sun.sunUpdateMode;
		const RE::NiPoint3& lastPos = a_viewCamera.previousWorld.translate;
		const float mx = viewPos.x - lastPos.x;
		const float my = viewPos.y - lastPos.y;
		const float mz = viewPos.z - lastPos.z;
		const bool moved = kMovedDistance < std::sqrt((mx * mx + my * my) + mz * mz);
		sun.sunUpdateMode = moved ? 1 : 0;
		if (*data.disableShadowJumps == 0) {
			SmoothSunDirection(sun, sunDir, oldMode, data);
		} else if (sun.sunUpdate > 1.0f || moved) {
			const float length = std::sqrt((sunDir.y * sunDir.y + sunDir.x * sunDir.x) + sunDir.z * sunDir.z);
			if (length > kEpsilon) {
				const float inverse = 1.0f / length;
				sunDir = { sunDir.x * inverse, sunDir.y * inverse, sunDir.z * inverse };
			} else {
				sunDir = { 0.0f, 0.0f, 0.0f };
			}
			sun.sunVector = sunDir;
			sun.sunUpdate = 0.0f;
		} else {
			const float scale = *data.sunStaticTimeUpdateScale;
			sun.sunUpdate = RealDelta() / (scale > kEpsilon ? scale : kEpsilon) + sun.sunUpdate;
			sunDir = sun.sunVector;
		}

		// The shadow cameras' basis: columns sun, lift and side.
		const RE::NiPoint3& sunUp = *data.sunUp;
		RE::NiPoint3 side;
		{
			const float cz = sunUp.x * sunDir.y - sunUp.y * sunDir.x;
			const float cy = sunUp.z * sunDir.x - sunUp.x * sunDir.z;
			const float cx = sunUp.y * sunDir.z - sunUp.z * sunDir.y;
			const float length = std::sqrt((cx * cx + cy * cy) + cz * cz);
			if (length > kEpsilon) {
				const float inverse = 1.0f / length;
				side = { -(inverse * cx), -(inverse * cy), -(inverse * cz) };
			} else {
				side = { -0.0f, -0.0f, -0.0f };
			}
		}
		RE::NiPoint3 lift;
		{
			const float bz = sunDir.x * side.y - side.x * sunDir.y;
			const float by = side.x * sunDir.z - sunDir.x * side.z;
			const float bx = sunDir.y * side.z - sunDir.z * side.y;
			const float length = std::sqrt((bx * bx + by * by) + bz * bz);
			if (length > kEpsilon) {
				const float inverse = 1.0f / length;
				lift = { -(bx * inverse), -(by * inverse), -(bz * inverse) };
			} else {
				lift = { -0.0f, -0.0f, -0.0f };
			}
		}
		RE::NiMatrix3 basis;
		basis.entry[0][0] = sunDir.x;
		basis.entry[0][1] = lift.x;
		basis.entry[0][2] = side.x;
		basis.entry[1][0] = sunDir.y;
		basis.entry[1][1] = lift.y;
		basis.entry[1][2] = side.y;
		basis.entry[2][0] = sunDir.z;
		basis.entry[2][1] = lift.z;
		basis.entry[2][2] = side.z;

		// Corners 0-3 at the shadow distance, 4-7 at the near plane.
		std::array<RE::NiPoint3, kHullPoints> corners{};
		FrustumCorners(corners.data(), dir, up, right, distance);
		FrustumCorners(corners.data() + 4, dir, up, right, nearDistance);

		// The full-frustum camera and the culling planes of its processes.
		RE::NiUpdateData update{};
		RE::NiCamera& full = *sun.fullFrustumCamera;
		full.local.translate = { viewPos.x - sunDir.x * kBackOff, viewPos.y - sunDir.y * kBackOff, viewPos.z - sunDir.z * kBackOff };
		full.local.rotate = basis;
		SetViewFrustum(full, MakeFrustum(0.0f, 1.0f, 1.0f, 0.0f, kOrthoNear, kBackOff));
		SetPort(full, 0.0f, 1.0f, 1.0f, 0.0f);
		full.Update(update);

		std::array<HullPoint, kHullPoints> points{};
		std::array<std::uint32_t, kHullPoints> hull{};
		std::uint32_t farthest = 0;
		{
			std::array<RE::NiPoint3, kHullPoints> screen{};
			ProjectCorners(full, viewPos, corners, screen, points);
			float maxZ = kFloatLowest;
			for (std::uint32_t k = 0; k < kHullPoints; ++k) {
				if (screen[k].z > maxZ) {
					maxZ = screen[k].z;
					farthest = k;
				}
			}
			const std::uint32_t hullCount = ConvexHull(points, hull);
			PlaneSet planes = ClearedPlanes();
			FitCullPlanes(planes, corners, hull, hullCount, sunDir, viewPos, farthest);
			auto& processes = sun.fullFrustumCullingProcessArray;
			for (std::uint32_t k = 0; k < processes.size(); ++k)
				WriteCustomPlanes(*processes[k], planes);
		}

		// The cascade splits: an even step between the first slice and the end, then each split averages it with a log split.
		const std::uint32_t count = a_light.shadowMapCount;
		const float shadowDistance = *data.shadowDistance;
		const float firstSliceSetting = *data.firstSliceDistance;
		const float firstSlice = firstSliceSetting > kMinFirstSlice ? firstSliceSetting : kMinFirstSlice;
		const float evenFirst = shadowDistance / static_cast<float>(count);
		std::array<float, kMaxCascades> splits{};
		splits[0] = (evenFirst < firstSlice ? evenFirst : firstSlice) / shadowDistance;
		const std::uint32_t cascades = count < kMaxCascades ? count : kMaxCascades;
		if (count > 1) {
			const float step = 1.0f / static_cast<float>(count - 1);
			for (std::uint32_t i = 1; i < cascades; ++i)
				splits[i] = ((1.0f - splits[0]) * static_cast<float>(i)) * step + splits[0];
		}

		if (cascades != 0) {
			const std::uint32_t resolution = *data.resolution;
			const float ratio = distance / nearDistance;
			const float range = distance - nearDistance;
			const float resolutionF = static_cast<float>(resolution);
			const float inverseResolution = 1.0f / resolutionF;
			const float overlap = *data.splitOverlap;
			const bool fullViewPort = *data.fullViewPort != 0;
			Descriptor* descriptors = a_light.GetRuntimeData().shadowmapDescriptors.data();
			std::array<RE::NiPoint3, kHullPoints> screen{};  // kept across cascades: a failed projection keeps the slot's last value
			float start = nearDistance;

			for (std::uint32_t i = 0; i < cascades; ++i) {
				Descriptor& desc = descriptors[i];
				const float split = splits[i];
				const float power = std::pow(ratio, split);
				sun.startSplitDistances[i] = start;
				const float end = (((range * split) + nearDistance) + power * nearDistance) * kHalf + overlap;
				sun.endSplitDistances[i] = end;

				if (!desc.camera)
					desc.camera = RE::NiPointer<RE::NiCamera>(CreateCamera(data));
				RE::NiCamera& cam = *desc.camera;
				cam.local.rotate = basis;
				cam.UpdateWorldData(&update);
				const RE::NiMatrix3 w = cam.world.rotate;
				const RE::NiPoint3 axis{ w.entry[0][0], w.entry[1][0], w.entry[2][0] };

				// Clip planes at the split's start (facing away from the camera) and end (facing back).
				const RE::NiPoint3 nearPoint{ start * dir.x + viewPos.x, start * dir.y + viewPos.y, start * dir.z + viewPos.z };
				desc.clipPlanes.cullingPlanes[0] = PlaneAt(dir, nearPoint);
				const RE::NiPoint3 farPoint{ end * dir.x + viewPos.x, end * dir.y + viewPos.y, end * dir.z + viewPos.z };
				desc.clipPlanes.cullingPlanes[1] = PlaneAt({ -dir.x, -dir.y, -dir.z }, farPoint);

				// The slice's corners: the previous end (the near plane for the first), then this end.
				for (std::uint32_t k = 0; k < 4; ++k)
					corners[k] = corners[k + 4];
				CascadeCorners(corners.data() + 4, dir, up, right, end);

				// The camera's reference point: the view position snapped to whole texels in the camera's axes.
				const float unitsPerTexel = Field<float>(&desc, kUnitsPerTexel);
				const float dotSide = (viewPos.y * w.entry[1][2] + viewPos.x * w.entry[0][2]) + viewPos.z * w.entry[2][2];
				const float dotLift = (viewPos.y * w.entry[1][1] + viewPos.x * w.entry[0][1]) + viewPos.z * w.entry[2][1];
				const float dotSun = (viewPos.y * w.entry[1][0] + viewPos.x * w.entry[0][0]) + viewPos.z * w.entry[2][0];
				float alongSide = 0.0f;
				float alongLift = 0.0f;
				float alongSun = 0.0f;
				if (unitsPerTexel >= 1.0f) {
					const float q = WholeCount(unitsPerTexel);
					const float inverse = 1.0f / q;
					alongSide = Floor(dotSide * inverse) * q;
					alongLift = Floor(dotLift * inverse) * q;
					alongSun = Floor(dotSun * inverse) * q;
				} else {
					const float q = WholeCount(1.0f / unitsPerTexel);
					const float inverse = 1.0f / q;
					alongSide = inverse * Floor(dotSide * q);
					alongLift = inverse * Floor(dotLift * q);
					alongSun = inverse * Floor(dotSun * q);
				}
				const RE::NiPoint3 snapped{ (w.entry[0][1] * alongLift + w.entry[0][2] * alongSide) + w.entry[0][0] * alongSun,
					(w.entry[1][2] * alongSide + w.entry[1][1] * alongLift) + w.entry[1][0] * alongSun,
					(w.entry[2][2] * alongSide + w.entry[2][1] * alongLift) + w.entry[2][0] * alongSun };
				cam.local.translate = { snapped.x - w.entry[0][0] * kBackOff, snapped.y - w.entry[1][0] * kBackOff, snapped.z - w.entry[2][0] * kBackOff };
				cam.local.rotate = basis;
				SetViewFrustum(cam, MakeFrustum(0.0f, 1.0f, 1.0f, 0.0f, kOrthoNear, kBackOff));
				SetPort(cam, 0.0f, 1.0f, 1.0f, 0.0f);
				cam.Update(update);

				// The slice's projected extent, its hull and the culling process's planes.
				ProjectCorners(cam, snapped, corners, screen, points);
				Bounds bounds;
				for (std::uint32_t k = 0; k < kHullPoints; ++k) {
					const RE::NiPoint3& s = screen[k];
					if (s.z > bounds.maxZ)
						farthest = k;
					bounds.minX = MinOf(s.x, bounds.minX);
					bounds.minY = MinOf(s.y, bounds.minY);
					bounds.minZ = MinOf(s.z, bounds.minZ);
					bounds.maxX = MaxOf(s.x, bounds.maxX);
					bounds.maxY = MaxOf(s.y, bounds.maxY);
					bounds.maxZ = MaxOf(s.z, bounds.maxZ);
				}
				const std::uint32_t hullCount = ConvexHull(points, hull);
				PlaneSet planes = ClearedPlanes();
				FitCullPlanes(planes, corners, hull, hullCount, axis, snapped, farthest);
				WriteCustomPlanes(*desc.cullingProcess, planes);
				const float depthFar = bounds.maxZ * kDepthScale + kDepthPad;
				const float depthNear = bounds.minZ * kDepthScale - kDepthPad;

				// The viewport: the whole map, or fitted to the extent.
				Fields(desc.port).left = 0;
				Fields(desc.port).right = static_cast<std::int32_t>(resolution);
				Fields(desc.port).bottom = 0;
				Fields(desc.port).top = static_cast<std::int32_t>(resolution);
				if (!fullViewPort)
					FitViewport(desc, bounds, resolutionF, inverseResolution);
				SetViewFrustum(cam, MakeFrustum(bounds.minX, bounds.maxX, bounds.maxY, bounds.minY, depthNear, depthFar));
				const float portRight = static_cast<float>(static_cast<std::uint32_t>(Fields(desc.port).right)) * inverseResolution;
				const float portBottom = static_cast<float>(static_cast<std::uint32_t>(Fields(desc.port).bottom)) * inverseResolution;
				const float portTop = static_cast<float>(static_cast<std::uint32_t>(Fields(desc.port).top)) * inverseResolution;
				SetPort(cam, 0.0f, portRight > 1.0f ? 1.0f : portRight, portTop <= 1.0f ? portTop : 1.0f, portBottom);
				cam.Update(update);

				// The accumulator: made on first use, then pointed at the main shadow scene node each call.
				if (!desc.shaderAccumulator) {
					desc.shaderAccumulator = RE::NiPointer<RE::BSShaderAccumulator>(CreateAccumulator(data));
					auto* accumulator = desc.shaderAccumulator->GetRuntimeData();
					accumulator->renderMode = RE::BSShaderAccumulator::RENDER_MODE::kShadowMapClamped;
					accumulator->drawDecals = false;
					accumulator->unk12D = true;
				}
				desc.shaderAccumulator->GetRuntimeData()->activeShadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];

				start = sun.endSplitDistances[i] - overlap * kTwo;
			}
		}

		Field<std::uint8_t>(&a_light, kLightPending) = 0;
		return true;
	}

	void SunCameraWrites(RE::BSShadowDirectionalLight& a_light, std::vector<WrittenRange>& a_out)
	{
		using namespace SunCameraImpl;

		auto& sun = a_light.GetShadowDirectionalLightRuntimeData();
		a_out.push_back({ &sun.sunVector, sizeof(RE::NiPoint3) * 2, "light sunVector, lastSunVector" });
		a_out.push_back({ &sun.startSplitDistances[0], sizeof(float) * 6, "light start/end split distances" });
		a_out.push_back({ &sun.sunUpdate, sizeof(float) + sizeof(std::uint32_t), "light sunUpdate, sunUpdateMode" });
		a_out.push_back({ &Field<std::uint8_t>(&a_light, kLightPending), 1, "light BSLight 0xD0" });

		if (sun.fullFrustumCamera)
			a_out.push_back({ sun.fullFrustumCamera.get(), kCameraSize, "full-frustum camera" });
		for (auto& process : sun.fullFrustumCullingProcessArray) {
			if (!process)
				continue;
			// The engine also copies 8 bytes of its stack into customCullPlanes unk68/unk6C; the port leaves them, so they are not listed.
			a_out.push_back({ &process->customCullPlanes, sizeof(PlaneSet), "full-frustum process customCullPlanes" });
			a_out.push_back({ &process->doCustomCullPlanes, 1, "full-frustum process doCustomCullPlanes" });
		}

		const std::uint32_t count = a_light.shadowMapCount < kMaxCascades ? a_light.shadowMapCount : kMaxCascades;
		Descriptor* descriptors = a_light.GetRuntimeData().shadowmapDescriptors.data();
		for (std::uint32_t i = 0; i < count; ++i) {
			Descriptor& desc = descriptors[i];
			a_out.push_back({ &desc.camera, sizeof(void*) * 2, "cascade camera and accumulator pointers" });
			a_out.push_back({ &desc.clipPlanes.cullingPlanes[0], sizeof(RE::NiPlane) * 2, "cascade clip planes near, far" });
			a_out.push_back({ &Field<float>(&desc, kUnitsPerTexel), sizeof(float), "cascade unitsPerTexel" });
			a_out.push_back({ &desc.port, sizeof(desc.port), "cascade port" });
			if (desc.camera)
				a_out.push_back({ desc.camera.get(), kCameraSize, "cascade camera" });
			if (desc.cullingProcess) {
				a_out.push_back({ &desc.cullingProcess->customCullPlanes, sizeof(PlaneSet), "cascade process customCullPlanes" });
				a_out.push_back({ &desc.cullingProcess->doCustomCullPlanes, 1, "cascade process doCustomCullPlanes" });
			}
			if (desc.shaderAccumulator) {
				auto* accumulator = desc.shaderAccumulator->GetRuntimeData();
				a_out.push_back({ &accumulator->activeShadowSceneNode, sizeof(void*), "cascade accumulator activeShadowSceneNode" });
				a_out.push_back({ &accumulator->drawDecals, 2, "cascade accumulator drawDecals, unk12D (first use)" });
				a_out.push_back({ &accumulator->renderMode, sizeof(std::uint32_t), "cascade accumulator renderMode (first use)" });
			}
		}
	}
}
