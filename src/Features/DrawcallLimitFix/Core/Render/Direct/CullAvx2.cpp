// The CPU cull's paraboloid and portal AVX2 kernels, eight lanes at a time. Compiled with /arch:AVX2 without the game's PCH, and used only where the CPU supports AVX2.

#include "Features/DrawcallLimitFix/Core/Render/Direct/CullSimd.h"

#include <array>
#include <cstring>
#include <immintrin.h>

namespace DCLF::Direct::Simd
{
	namespace
	{
		// An 8-lane verdict mask as eight 0/1 bytes.
		constexpr std::array<std::uint64_t, 256> MakeBytes()
		{
			std::array<std::uint64_t, 256> bytes{};
			for (std::uint32_t m = 0; m < 256; ++m)
				for (std::uint32_t k = 0; k < 8; ++k)
					bytes[m] |= std::uint64_t((m >> k) & 1u) << (8 * k);
			return bytes;
		}
		constexpr auto kBytes = MakeBytes();
	}

	void ParabolicAvx2(const Spheres& a_spheres, std::uint32_t a_count, const float* a_sphere, const float* a_plane, std::uint32_t a_flags, const std::uint64_t* a_dead,
		std::uint8_t* a_pass)
	{
		const __m256 sx = _mm256_set1_ps(a_sphere[0]), sy = _mm256_set1_ps(a_sphere[1]), sz = _mm256_set1_ps(a_sphere[2]), sr = _mm256_set1_ps(a_sphere[3]);
		const __m256 px = _mm256_set1_ps(a_plane[0]), py = _mm256_set1_ps(a_plane[1]), pz = _mm256_set1_ps(a_plane[2]), pw = _mm256_set1_ps(a_plane[3]);
		const __m256 zero = _mm256_setzero_ps();
		const bool back = (a_flags & kParabolicBack) != 0, both = (a_flags & kParabolicBothSides) != 0;
		std::uint32_t i = 0;
		for (; i + 8 <= a_count; i += 8) {
			const __m256 x = _mm256_loadu_ps(a_spheres.x + i), y = _mm256_loadu_ps(a_spheres.y + i), z = _mm256_loadu_ps(a_spheres.z + i);
			const __m256 r = _mm256_loadu_ps(a_spheres.r + i);
			const __m256 dx = _mm256_sub_ps(x, sx), dy = _mm256_sub_ps(y, sy), dz = _mm256_sub_ps(z, sz);
			const __m256 reach = _mm256_sub_ps(_mm256_sub_ps(_mm256_sqrt_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(dx, dx), _mm256_mul_ps(dy, dy)), _mm256_mul_ps(dz, dz))), r), sr);
			const int meets = _mm256_movemask_ps(_mm256_cmp_ps(reach, zero, _CMP_LT_OQ));
			const __m256 side = _mm256_sub_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(y, py), _mm256_mul_ps(x, px)), _mm256_mul_ps(z, pz)), pw);
			const int straddles = _mm256_movemask_ps(_mm256_cmp_ps(_mm256_sub_ps(side, r), zero, _CMP_LT_OQ));
			const int front = _mm256_movemask_ps(_mm256_cmp_ps(side, zero, _CMP_GT_OQ));
			int keep = meets & (back ? (both ? (straddles | ~front) : 0) : (straddles | front));
			keep |= _mm256_movemask_ps(_mm256_cmp_ps(r, zero, _CMP_LT_OQ));
			if (a_dead)
				keep &= ~static_cast<int>((a_dead[i >> 6] >> (i & 63)) & 0xFFu);
			std::memcpy(a_pass + i, &kBytes[static_cast<std::uint32_t>(keep) & 0xFFu], 8);
		}
		for (; i < a_count; ++i) {
			const bool dead = a_dead && ((a_dead[i >> 6] >> (i & 63)) & 1u);
			a_pass[i] = !dead && ParabolicKept(a_spheres.x[i], a_spheres.y[i], a_spheres.z[i], a_spheres.r[i], a_sphere, a_plane, a_flags) ? 1u : 0u;
		}
	}

	void PlaneChunksAvx2(const std::uint32_t* a_set, std::uint32_t a_kind, const float* a_x, const float* a_y, const float* a_z, const float* a_r,
		const std::uint16_t* a_lanes, std::uint16_t* a_pass, std::uint32_t a_chunks)
	{
		std::uint32_t mask;
		std::memcpy(&mask, a_set + 24, 4);
		__m256 nx[6], ny[6], nz[6], d0[6];
		std::uint32_t planes = 0;
		for (std::uint32_t rest = mask & 0x3Fu; rest; rest &= rest - 1, ++planes) {
			float n[4];
			std::memcpy(n, a_set + std::size_t(_tzcnt_u32(rest)) * 4, 16);
			nx[planes] = _mm256_set1_ps(n[0]), ny[planes] = _mm256_set1_ps(n[1]), nz[planes] = _mm256_set1_ps(n[2]), d0[planes] = _mm256_set1_ps(n[3]);
		}
		for (std::uint32_t c = 0; c < a_chunks; ++c) {
			const std::uint32_t lanes = a_lanes[c];
			if (!lanes) {
				a_pass[c] = 0;
				continue;
			}
			const float* x = a_x + 16 * c;
			const float* y = a_y + 16 * c;
			const float* z = a_z + 16 * c;
			const float* r = a_r + 16 * c;
			const __m256 x0 = _mm256_loadu_ps(x), y0 = _mm256_loadu_ps(y), z0 = _mm256_loadu_ps(z), r0 = _mm256_loadu_ps(r);
			const __m256 x1 = _mm256_loadu_ps(x + 8), y1 = _mm256_loadu_ps(y + 8), z1 = _mm256_loadu_ps(z + 8), r1 = _mm256_loadu_ps(r + 8);
			const __m256 m0 = _mm256_sub_ps(_mm256_setzero_ps(), r0), m1 = _mm256_sub_ps(_mm256_setzero_ps(), r1);
			std::uint32_t outside = 0, notContained = 0;
			for (std::uint32_t p = 0; p < planes; ++p) {
				const __m256 d = _mm256_sub_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ny[p], y0), _mm256_mul_ps(nx[p], x0)), _mm256_mul_ps(z0, nz[p])), d0[p]);
				const __m256 e = _mm256_sub_ps(_mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(ny[p], y1), _mm256_mul_ps(nx[p], x1)), _mm256_mul_ps(z1, nz[p])), d0[p]);
				const std::uint32_t behind = static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(d, m0, _CMP_LE_OQ))) |
				                             (static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(e, m1, _CMP_LE_OQ))) << 8);
				const std::uint32_t near = static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(d, r0, _CMP_LT_OQ))) |
				                           (static_cast<std::uint32_t>(_mm256_movemask_ps(_mm256_cmp_ps(e, r1, _CMP_LT_OQ))) << 8);
				outside |= behind;
				notContained |= behind | near;
			}
			a_pass[c] = static_cast<std::uint16_t>((a_kind == 7u ? ~outside : notContained) & lanes);
		}
	}
}
