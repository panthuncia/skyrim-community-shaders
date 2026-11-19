#pragma once

// Ports of the engine's camera, plane and viewport arithmetic, so DCLF can derive its own cameras.
// The ports follow the engine operation for operation so results match it to the bit. Do not "simplify" the math.
// Self-contained.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace RE
{
	class BSShadowLight;
	class NiAVObject;
	class NiCamera;
	class NiFrustum;
	class NiTransform;
	class NiCullingProcess;
}

namespace DCLF::ShadowCameras
{
	/** @brief Six planes as normal and constant, plus the active mask. A sphere is outside when n.c - d < -r. */
	struct Planes
	{
		std::array<std::array<float, 4>, 6> plane{};
		std::uint32_t mask = 0;
	};

	/** @brief A camera with the matrices BSGraphics::State::SetCameraData (0x140e56d70) builds, plus its eye, port and cull planes. */
	struct Camera
	{
		// Row-major like the engine's camera data. VS_PerFrame holds the transposes.
		std::array<float, 16> view{}, proj{}, viewProj{};
		std::array<float, 3> eye{};   // posAdjust
		std::array<float, 4> port{};  // NiRect<float>: left, right, top, bottom, y up
		Planes frustum;
		Planes caster;                // UpdateCamera's caster volume. Mask 0 means none.
		Planes cull;                  // the caster volume if there is one, else the frustum
		// For a portal-strict light, its light context index + 1. Its casters must pass the strict gates. 0 means not gated.
		std::uint32_t portalContext = 0;
		// Point lights cull by sphere and hemisphere plane, not a frustum (BSParabolicCullingProcess, FUN_141519d20).
		// The back map skips casters wholly in front of the plane, per FUN_14151a1e0's codes 1 and 3.
		bool parabolic = false, parabolicBack = false, parabolicBothSides = false;
		std::array<float, 4> parabolicSphere{}, parabolicPlane{};  // centre and radius; normal and constant
		bool valid = false;
	};

	/** @brief NiCullingProcess::SetFrustum's planes, ported from FUN_140d32cc0. */
	void FrustumPlanes(const RE::NiFrustum& a_frustum, const RE::NiTransform& a_world, Planes& a_out);
	/** @brief What BSCullingProcess::Process2 (0x140e28690) tests. The custom planes replace the frustum whole when present. */
	void CullPlanes(const RE::NiCullingProcess* a_process, const RE::NiCamera& a_camera, Planes& a_out);
	/** @brief SetCameraData's matrices and eye (FUN_140e57ae0), and the port. */
	void CameraMatrices(const RE::NiCamera& a_camera, Camera& a_out);
	/**
	 * @brief Writes VS_PerFrame (b12) with the camera matrices and CameraPosAdjust. Everything else is zero,
	 * since Utility.hlsl's shadow modes read only the view-projection and the eye.
	 */
	inline constexpr std::uint32_t kBlockBytes = 45 * 16;
	void Block(const Camera& a_camera, std::byte* a_out);
	/** @brief Renderer::UpdateViewPort's (0x140e441c0) viewport as x, y, width, height. */
	std::array<std::uint32_t, 4> Viewport(const std::array<float, 4>& a_port, std::uint32_t a_width, std::uint32_t a_height);

	/** @brief Equal bits, or within 1e-5 relative. Plane normalisation divides, so exact match is too strict. */
	bool Close(float a_a, float a_b);
	bool SamePlanes(const Planes& a_a, const Planes& a_b);
	/** @brief Describes the first difference between two plane sets, for logging. */
	std::string DescribePlanes(const Planes& a_mine, const Planes& a_engine);
}
