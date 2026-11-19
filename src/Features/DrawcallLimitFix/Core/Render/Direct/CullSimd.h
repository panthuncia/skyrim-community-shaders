#pragma once

// The CPU cull's wide kernels, in CullAvx2.cpp and CullAvx512.cpp. Each file is compiled for its own instruction set
// without the game's PCH, and the kernel is chosen at run time by what the CPU and OS support. So this header uses plain types only.
// Each kernel must match the SSE pass bit for bit, so keep its operation order and use no FMA (kExpCullWideCheck checks it).

#include <cmath>
#include <cstdint>

namespace DCLF::Direct::Simd
{
	/** @brief A batch's sphere columns. */
	struct Spheres
	{
		const float *x, *y, *z, *r;
	};

	/**
	 * @brief The frustum over a_count lanes. a_pass[i] is 1 inside every plane or with a negative radius, else 0, and 0 for a
	 * lane set in a_dead. A lane group stops testing once every lane in it is outside a plane.
	 */
	using FrustumFn = void (*)(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);

	void FrustumAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);
	void FrustumAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float (*a_planes)[4], std::uint32_t a_planeCount, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);

	/** @brief ParabolicFn's view flags: draws both hemispheres, and is the back hemisphere's view. */
	inline constexpr std::uint32_t kParabolicBothSides = 1u, kParabolicBack = 2u;

	/**
	 * @brief BuildDrawsCS's ParabolicRejected over a_count lanes, in ParabolicSphereMask's exact operation order. a_pass[i] is 1
	 * where the lane is kept. The front hemisphere keeps codes 1 and 3, the back one 2 and 3.
	 */
	using ParabolicFn = void (*)(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags,
		const std::uint64_t* a_dead, std::uint8_t* a_pass);

	void ParabolicAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);
	void ParabolicAvx512(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags, const std::uint64_t* a_dead,
		std::uint8_t* a_pass);

	/** @brief One lane of ParabolicFn, scalar, in the same order. */
	inline bool ParabolicKept(float a_x, float a_y, float a_z, float a_r, const float* a_sphere, const float* a_plane, std::uint32_t a_flags)
	{
		if (a_r < 0.0f)
			return true;
		const float dx = a_x - a_sphere[0], dy = a_y - a_sphere[1], dz = a_z - a_sphere[2];
		const float squared = (dx * dx + dy * dy) + dz * dz;
		// The vector sqrt is correctly rounded, like the scalar one.
		if (!((std::sqrt(squared) - a_r) - a_sphere[3] < 0.0f))
			return false;
		const float side = ((a_y * a_plane[1] + a_x * a_plane[0]) + a_z * a_plane[2]) - a_plane[3];
		const bool straddles = side - a_r < 0.0f, front = side > 0.0f;
		const bool both = (a_flags & kParabolicBothSides) != 0;
		return (a_flags & kParabolicBack) ? both && (straddles || !front) : (straddles || front);
	}

	/**
	 * @brief PortalPrograms' plane set test over up to 32 bounds, as a mask of a_lanes. Stops once every bound is decided.
	 * Keep PortalDistance's order: ((n.y * c.y + n.x * c.x) + c.z * n.z) - d.
	 */
	using PlaneGroupFn = std::uint32_t (*)(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes);

	std::uint32_t PlaneGroupAvx2(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes);
	std::uint32_t PlaneGroupAvx512(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		std::uint32_t a_lanes);

	/** @brief PlaneGroupFn over chunks of 16 bounds. Evaluates every active plane with no early stop, so the loop stays predictable. */
	using PlaneChunksFn = void (*)(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks);

	void PlaneChunksAvx2(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks);
	void PlaneChunksAvx512(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks);

	/** @brief The widest kernels the CPU and OS support. AVX2 needs the YMM state, AVX-512 needs F, BW and the ZMM state. */
	enum class Level : std::uint8_t
	{
		Sse,
		Avx2,
		Avx512,
	};
	Level Supported();
	const char* NameOf(Level a_level);
	/** @brief The kernels the cull uses now, as far as kExpCullWide and kExpCullAvx512 allow. */
	Level Chosen();
}
