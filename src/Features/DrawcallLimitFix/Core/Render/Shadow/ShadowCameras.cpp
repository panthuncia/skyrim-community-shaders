// Ports of the engine's camera, plane and viewport math. Keep the engine's operation order so results match bit for bit.

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace DCLF::ShadowCameras
{
	void FrustumPlanes(const RE::NiFrustum& a_frustum, const RE::NiTransform& a_world, Planes& a_out)
	{
		// Port of FUN_140d32cc0 (AE). The view direction is rotation column 0, up is column 1 and right is column 2.
		const auto& r = a_world.rotate.entry;
		const float dir[3] = { r[0][0], r[1][0], r[2][0] };
		const float up[3] = { r[0][1], r[1][1], r[2][1] };
		const float right[3] = { r[0][2], r[1][2], r[2][2] };
		const float eye[3] = { a_world.translate.x, a_world.translate.y, a_world.translate.z };
		// Port of FUN_140d32a60. It sums in this order: d = (p.y * n.y + n.x * p.x) + p.z * n.z.
		auto plane = [&](std::uint32_t a_index, const float (&a_n)[3], const float (&a_p)[3]) {
			auto& p = a_out.plane[a_index];
			p[0] = a_n[0];
			p[1] = a_n[1];
			p[2] = a_n[2];
			const float yy = a_p[1] * a_n[1];
			const float xx = a_n[0] * a_p[0];
			const float zz = a_p[2] * a_n[2];
			p[3] = (yy + xx) + zz;
		};
		auto along = [&](const float (&a_axis)[3], float a_distance, float (&a_out3)[3]) {
			for (std::uint32_t i = 0; i < 3; ++i)
				a_out3[i] = a_axis[i] * a_distance + eye[i];
		};
		auto negate = [](const float (&a_v)[3], float (&a_out3)[3]) {
			for (std::uint32_t i = 0; i < 3; ++i)
				a_out3[i] = -a_v[i];
		};
		float point[3], normal[3];
		// The near plane faces along the view direction. The far plane faces against it.
		along(dir, a_frustum.fNear, point);
		plane(0, dir, point);
		along(dir, a_frustum.fFar, point);
		negate(dir, normal);
		plane(1, normal, point);
		if (!a_frustum.bOrtho) {
			// Side planes pass through the eye. Each normal is the side axis plus the view direction, scaled by 1 / sqrt(t*t + 1).
			auto side = [&](std::uint32_t a_index, const float (&a_axis)[3], float a_axisScale, float a_dirScale) {
				for (std::uint32_t i = 0; i < 3; ++i)
					normal[i] = a_axis[i] * a_axisScale + dir[i] * a_dirScale;
				plane(a_index, normal, eye);
			};
			float s = 1.0f / std::sqrt(a_frustum.fLeft * a_frustum.fLeft + 1.0f);
			side(2, right, s, -(a_frustum.fLeft * s));
			s = 1.0f / std::sqrt(a_frustum.fRight * a_frustum.fRight + 1.0f);
			side(3, right, -s, a_frustum.fRight * s);
			s = 1.0f / std::sqrt(a_frustum.fTop * a_frustum.fTop + 1.0f);
			side(4, up, -s, a_frustum.fTop * s);
			s = 1.0f / std::sqrt(a_frustum.fBottom * a_frustum.fBottom + 1.0f);
			side(5, up, s, -(a_frustum.fBottom * s));
		} else {
			// Orthographic side planes face inward, through the eye moved along their axis to the frustum edge.
			along(right, a_frustum.fLeft, point);
			plane(2, right, point);
			along(right, a_frustum.fRight, point);
			negate(right, normal);
			plane(3, normal, point);
			along(up, a_frustum.fTop, point);
			negate(up, normal);
			plane(4, normal, point);
			along(up, a_frustum.fBottom, point);
			plane(5, up, point);
		}
		a_out.mask = 0x3F;  // NiCullingProcess::SetFrustum
	}

	Corners FrustumCorners(const RE::NiCamera& a_camera)
	{
		const auto& r = a_camera.world.rotate.entry;
		const auto& t = a_camera.world.translate;
		const auto& f = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum;
		const float farDistance = f.fFar;
		const float eye[3] = { t.x, t.y, t.z };
		float dir[3], up[3], right[3];
		for (std::uint32_t i = 0; i < 3; ++i) {
			dir[i] = r[i][0] * farDistance;
			up[i] = r[i][1] * f.fTop * farDistance;
			right[i] = r[i][2] * f.fRight * farDistance;
		}
		Corners out;
		out[0] = { eye[0], eye[1], eye[2] };
		for (std::uint32_t i = 0; i < 3; ++i) {
			const float centreUp = eye[i] + dir[i] + up[i];
			const float centreDown = eye[i] + dir[i] - up[i];
			out[1][i] = centreUp + right[i];
			out[2][i] = centreUp - right[i];
			out[3][i] = centreDown + right[i];
			out[4][i] = centreDown - right[i];
		}
		return out;
	}

	bool CornersOrEdgesInside(const Corners& a_points, const Planes& a_planes)
	{
		const auto& planes = a_planes.plane;
		for (const auto& p : a_points) {
			std::uint32_t k = 0;
			while (0.0f < (p[0] * planes[k][0] + p[1] * planes[k][1] + p[2] * planes[k][2]) - planes[k][3])
				if (++k > 5)
					return true;
		}
		static constexpr std::uint8_t kEdges[8][2] = { { 0, 1 }, { 0, 2 }, { 0, 3 }, { 0, 4 }, { 1, 2 }, { 1, 3 }, { 2, 4 }, { 3, 4 } };
		const auto side = [](float a_value) -> std::uint8_t { return 0.0f <= a_value ? (0.0f < a_value ? 1 : 0) : 2; };
		for (const auto& edge : kEdges) {
			const auto& a = a_points[edge[0]];
			const auto& b = a_points[edge[1]];
			for (std::uint32_t j = 0; j < 6; ++j) {
				const auto& n = planes[j];
				const std::uint8_t sa = side((a[0] * n[0] + a[1] * n[1] + a[2] * n[2]) - n[3]);
				const std::uint8_t sb = side((b[0] * n[0] + b[1] * n[1] + b[2] * n[2]) - n[3]);
				if (sa == sb) {
					if (sa == 0)
						return true;
					continue;
				}
				const float dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
				const float t = ((n[0] * n[3] - a[0]) * n[0] + (n[3] * n[1] - a[1]) * n[1] + (n[3] * n[2] - a[2]) * n[2]) / (dx * n[0] + dy * n[1] + dz * n[2]);
				if (!(0.0f <= t) || !(t <= 1.0f))
					continue;
				std::uint32_t k = 0;
				while (k == j || 0.0f <= ((dy * t + a[1]) * planes[k][1] + (dx * t + a[0]) * planes[k][0] + (dz * t + a[2]) * planes[k][2]) - planes[k][3])
					if (++k > 5)
						return true;
			}
		}
		return false;
	}

	bool FrustaIntersect(const RE::NiCamera& a_view, const RE::NiCamera& a_light)
	{
		Planes viewPlanes, lightPlanes;
		FrustumPlanes(const_cast<RE::NiCamera&>(a_view).GetRuntimeData2().viewFrustum, a_view.world, viewPlanes);
		FrustumPlanes(const_cast<RE::NiCamera&>(a_light).GetRuntimeData2().viewFrustum, a_light.world, lightPlanes);
		return CornersOrEdgesInside(FrustumCorners(a_view), lightPlanes) || CornersOrEdgesInside(FrustumCorners(a_light), viewPlanes);
	}

	bool SphereInside(const float (&a_centre)[3], float a_radius, const Planes& a_planes)
	{
		for (std::uint32_t k = 0; k < 6; ++k) {
			if (!((a_planes.mask >> k) & 1))
				continue;
			const auto& n = a_planes.plane[k];
			const float value = ((a_centre[1] * n[1] + a_centre[0] * n[0]) + a_centre[2] * n[2]) - n[3];
			if (!(value > -a_radius))
				return false;
		}
		return true;
	}

	bool LodFaded(const RE::NiCamera& a_view, const RE::NiPoint3& a_position, float a_radius, bool a_frustumOrder)
	{
		const float dx = a_position.x - a_view.world.translate.x;
		const float dy = a_position.y - a_view.world.translate.y;
		const float dz = a_position.z - a_view.world.translate.z;
		// Each class sums in its own order. Keep it, so float results match the engine.
		const float squared = a_frustumOrder ? (dy * dy + dx * dx) + dz * dz : (dx * dx + dy * dy) + dz * dz;
		const float lodAdjust = const_cast<RE::NiCamera&>(a_view).GetRuntimeData2().lodAdjust;
		return (squared - a_radius * a_radius) * lodAdjust >= Engine::Global<float>(Engine::Address::kLodFadeCutoff);
	}

	void CullPlanes(const RE::NiCullingProcess* a_process, const RE::NiCamera& a_camera, Planes& a_out)
	{
		// As in Process2, custom cull planes replace all six planes and the active mask.
		if (a_process && a_process->doCustomCullPlanes) {
			const auto& planes = a_process->customCullPlanes;
			for (std::uint32_t p = 0; p < 6; ++p)
				a_out.plane[p] = { planes.cullingPlanes[p].normal.x, planes.cullingPlanes[p].normal.y, planes.cullingPlanes[p].normal.z, planes.cullingPlanes[p].constant };
			a_out.mask = planes.activePlanes.underlying() & 0x3Fu;
			return;
		}
		FrustumPlanes(const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum, a_camera.world, a_out);
	}

	void CameraMatrices(const RE::NiCamera& a_camera, Camera& a_out)
	{
		// NiRect<float> members are protected. The order is left, right, top, bottom.
		auto& data = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2();
		static_assert(sizeof(data.port) == sizeof(float) * 4);
		std::array<float, 4> port;
		std::memcpy(port.data(), &data.port, sizeof(float) * 4);
		CameraMatrices(a_camera.world, data.viewFrustum, port, a_out);
	}

	void CameraMatrices(const RE::NiTransform& a_world, const RE::NiFrustum& a_frustum, const std::array<float, 4>& a_port, Camera& a_out)
	{
		// Port of FUN_140e57ae0 (AE), with the arguments FUN_140e58b80 passes.
		const auto& r = a_world.rotate.entry;
		auto& v = a_out.view;
		v = { r[0][2], r[0][1], r[0][0], 0.0f, r[1][2], r[1][1], r[1][0], 0.0f, r[2][2], r[2][1], r[2][0], 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
		const auto& f = a_frustum;
		const float one = 1.0f;
		const float rl = one / (f.fRight - f.fLeft);
		const float fn = one / (f.fFar - f.fNear);
		const float tb = one / (f.fTop - f.fBottom);
		auto& p = a_out.proj;
		p = {};
		p[0] = rl + rl;
		p[5] = tb + tb;
		const float x = -((f.fLeft + f.fRight) * rl);
		const float y = -(tb * (f.fBottom + f.fTop));
		if (!f.bOrtho) {
			p[8] = x;
			p[9] = y;
			p[10] = fn * f.fFar;
			p[14] = -(fn * (f.fNear * f.fFar));
			p[11] = one;
			p[15] = 0.0f;
		} else {
			p[12] = x;
			p[13] = y;
			p[10] = fn;
			p[14] = -(fn * f.fNear);
			p[11] = 0.0f;
			p[15] = one;
		}
		a_out.viewProj = Multiply(v, p);
		a_out.eye = { a_world.translate.x, a_world.translate.y, a_world.translate.z };
		a_out.port = a_port;
	}

	std::array<float, 16> Multiply(const std::array<float, 16>& a_a, const std::array<float, 16>& a_b)
	{
		// Port of FUN_1404a6ed0. Each row is (x * B0 + z * B2) + (y * B1 + w * B3), in that order.
		std::array<float, 16> out;
		for (std::uint32_t row = 0; row < 4; ++row) {
			const float* a = a_a.data() + row * 4;
			for (std::uint32_t c = 0; c < 4; ++c) {
				const float zx = a[2] * a_b[8 + c] + a[0] * a_b[c];
				const float wy = a[3] * a_b[12 + c] + a[1] * a_b[4 + c];
				out[row * 4 + c] = zx + wy;
			}
		}
		return out;
	}

	RE::NiFrustum ShadowmapFrustum(const RE::NiCamera& a_camera, const std::array<std::int32_t, 4>& a_port, bool a_parabolic, std::uint32_t a_width,
		std::uint32_t a_height, bool& a_toTexture)
	{
		// Port of RenderShadowmap's tail (AE 0x1414f0e68 on). The port's differences are unsigned, as the engine converts them.
		RE::NiFrustum frustum = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().viewFrustum;
		a_toTexture = frustum.bOrtho && !a_parabolic;
		float top = frustum.fTop, bottom = frustum.fBottom;
		if (a_toTexture) {
			const float width = static_cast<float>(a_width), height = static_cast<float>(a_height);
			const float span = (height / static_cast<float>(static_cast<std::uint32_t>(a_port[2] - a_port[3]))) * (frustum.fTop - frustum.fBottom);
			bottom = frustum.fBottom - (static_cast<float>(static_cast<std::uint32_t>(a_port[3])) * span) / height;
			frustum.fRight = (width / static_cast<float>(static_cast<std::uint32_t>(a_port[1] - a_port[0]))) * (frustum.fRight - frustum.fLeft) + frustum.fLeft;
			top = bottom + span;
		}
		frustum.fTop = bottom;
		frustum.fBottom = top;
		return frustum;
	}

	std::array<float, 16> LightTransform(const RE::NiCamera& a_camera, const std::array<std::int32_t, 4>& a_port, bool a_parabolic, std::uint32_t a_width,
		std::uint32_t a_height)
	{
		bool toTexture = false;
		const RE::NiFrustum frustum = ShadowmapFrustum(a_camera, a_port, a_parabolic, a_width, a_height, toTexture);
		Camera camera;
		CameraMatrices(a_camera.world, frustum, { 0.0f, 1.0f, 1.0f, 0.0f }, camera);
		auto projection = camera.proj;
		if (toTexture) {
			static constexpr std::array<float, 16> kTexture{ 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 0.5f, 0.0f, 1.0f };
			projection = Multiply(projection, kTexture);
		}
		// The view's translation row: the eye through the rotation's columns, negated, each sum in the engine's order.
		auto view = camera.view;
		const auto& r = a_camera.world.rotate.entry;
		const auto& e = a_camera.world.translate;
		view[12] = -((e.x * r[0][2] + e.y * r[1][2]) + e.z * r[2][2]);
		view[13] = -((e.x * r[0][1] + e.y * r[1][1]) + e.z * r[2][1]);
		view[14] = -((e.y * r[1][0] + e.x * r[0][0]) + e.z * r[2][0]);
		return Multiply(view, projection);
	}

	void Block(const Camera& a_camera, std::byte* a_out)
	{
		std::memset(a_out, 0, kBlockBytes);
		auto* block = reinterpret_cast<float*>(a_out);
		auto transposed = [&](std::uint32_t a_register, const std::array<float, 16>& a_m) {
			for (std::uint32_t row = 0; row < 4; ++row)
				for (std::uint32_t c = 0; c < 4; ++c)
					block[a_register * 4 + row * 4 + c] = a_m[c * 4 + row];
		};
		transposed(0, a_camera.view);
		transposed(4, a_camera.proj);
		transposed(8, a_camera.viewProj);
		transposed(12, a_camera.viewProj);
		block[160] = a_camera.eye[0];
		block[161] = a_camera.eye[1];
		block[162] = a_camera.eye[2];
	}

	std::array<std::uint32_t, 4> Viewport(const std::array<float, 4>& a_port, std::uint32_t a_width, std::uint32_t a_height)
	{
		// As Renderer::UpdateViewPort: x = left * W, y = (1 - top) * H, width = (right - left) * W, height = (top - bottom) * H.
		// The capture truncates each value to an integer.
		const float w = static_cast<float>(a_width), h = static_cast<float>(a_height);
		const float x = a_port[0] * w;
		const float y = (1.0f - a_port[2]) * h;
		const float width = (a_port[1] - a_port[0]) * w;
		const float height = (a_port[2] - a_port[3]) * h;
		return { static_cast<std::uint32_t>(std::max(0.0f, x)), static_cast<std::uint32_t>(std::max(0.0f, y)), static_cast<std::uint32_t>(width),
			static_cast<std::uint32_t>(height) };
	}

	bool Close(float a_a, float a_b)
	{
		return std::bit_cast<std::uint32_t>(a_a) == std::bit_cast<std::uint32_t>(a_b) || std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a));
	}

	bool SamePlanes(const Planes& a_a, const Planes& a_b)
	{
		if (a_a.mask != a_b.mask)
			return false;
		for (std::uint32_t p = 0; p < 6; ++p) {
			if (!((a_a.mask >> p) & 1))
				continue;
			for (std::uint32_t i = 0; i < 4; ++i)
				if (!Close(a_a.plane[p][i], a_b.plane[p][i]))
					return false;
		}
		return true;
	}

	std::string DescribePlanes(const Planes& a_mine, const Planes& a_engine)
	{
		std::string match, sets;
		for (std::uint32_t e = 0; e < 6; ++e) {
			int found = -1;
			for (std::uint32_t m = 0; m < 6 && found < 0; ++m) {
				bool same = true;
				for (std::uint32_t i = 0; i < 4; ++i)
					same = same && Close(a_mine.plane[m][i], a_engine.plane[e][i]);
				if (same)
					found = static_cast<int>(m);
			}
			match += found < 0 ? std::string("-") : std::to_string(found);
		}
		for (std::uint32_t p = 0; p < 6; ++p)
			sets += fmt::format(" [{}: ({:.4f} {:.4f} {:.4f} {:.1f}) / ({:.4f} {:.4f} {:.4f} {:.1f})]", p, a_mine.plane[p][0], a_mine.plane[p][1], a_mine.plane[p][2], a_mine.plane[p][3],
				a_engine.plane[p][0], a_engine.plane[p][1], a_engine.plane[p][2], a_engine.plane[p][3]);
		return fmt::format("the engine's planes are DCLF's {} (masks {:#x} {:#x});{}", match, a_mine.mask, a_engine.mask, sets);
	}
}
