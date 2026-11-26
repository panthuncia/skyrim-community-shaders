#include "Snapshot/FrameCamera.h"

#include "Snapshot/CoreFrame.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"

#include <cstring>

namespace Snapshot::FrameCamera
{
	namespace
	{
		/** @brief The render side's frame camera. Render owner. */
		struct RenderCamera
		{
			globals::FrameBufferCache main{};  // jittered: the world passes
			globals::FrameBufferCache post{};  // unjittered: the post chain and what draws after it
			std::array<float, 2> jitter{};
			Ratios ratios{};              // BeginRender's: the world passes' dynamic resolution
			bool fullResolution = false;  // OnFullResolution ran this render frame
			std::uint64_t builtFrame = 0;
			bool built = false;
			bool wrote = false;  // the blocks hold a camera this render frame
			// What the last render frame drew: the previous camera of the next one.
			Previous last{};
			bool haveLast = false;
		};

		RenderCamera& Render()
		{
			static RenderCamera camera;
			return camera;
		}
	}

	void Capture(const RE::NiCamera& a_camera, CameraView& a_view)
	{
		auto& camera = const_cast<RE::NiCamera&>(a_camera);
		a_view.valid = true;
		std::memcpy(a_view.worldToCam, camera.GetRuntimeData().worldToCam, sizeof(a_view.worldToCam));
		a_view.position = camera.world.translate;
		a_view.rotation = camera.world.rotate;
		a_view.frustum = camera.GetRuntimeData2().viewFrustum;
		a_view.port = camera.GetRuntimeData2().port;
		DCLF::ShadowCameras::Camera matrices;
		DCLF::ShadowCameras::CameraMatrices(a_camera, matrices);
		a_view.view = matrices.view;
		a_view.proj = matrices.proj;
		a_view.viewProj = matrices.viewProj;
	}

	std::array<float, 16> Inverse(const std::array<float, 16>& a_m)
	{
		std::array<double, 16> m{}, inv{};
		for (std::uint32_t i = 0; i < 16; ++i)
			m[i] = a_m[i];
		inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
		inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
		inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
		inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
		inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
		inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
		inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
		inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
		inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
		inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
		inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
		inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
		inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
		inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
		inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
		inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
		const double det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
		std::array<float, 16> out{};
		if (det == 0.0)
			return out;
		for (std::uint32_t i = 0; i < 16; ++i)
			out[i] = static_cast<float>(inv[i] / det);
		return out;
	}

	void WriteCamera(const std::array<float, 16>& a_view, const std::array<float, 16>& a_proj, const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye,
		std::array<float, 2> a_jitter, const Previous* a_previous, float* a_out)
	{
		auto transposed = [&](std::uint32_t a_register, const std::array<float, 16>& a_m) {
			for (std::uint32_t row = 0; row < 4; ++row)
				for (std::uint32_t c = 0; c < 4; ++c)
					a_out[a_register * 4 + row * 4 + c] = a_m[c * 4 + row];
		};
		auto proj = a_proj;
		auto viewProj = a_viewProj;
		if (a_jitter[0] != 0.0f || a_jitter[1] != 0.0f) {
			proj[8] += a_jitter[0];
			proj[9] += a_jitter[1];
			viewProj = DCLF::ShadowCameras::Multiply(a_view, proj);
		}
		transposed(0, a_view);
		transposed(4, proj);
		transposed(8, viewProj);
		transposed(12, a_viewProj);  // CameraViewProjUnjittered
		if (a_previous)
			transposed(16, a_previous->viewProjUnjittered);
		else
			std::memset(a_out + 16 * 4, 0, 4 * 4 * sizeof(float));
		// c20 is the unjittered projection's inverse and c24 the unjittered projection: the reverse of FrameBuffer.hlsli's names, as
		// the engine writes them. c36 is the jittered projection's inverse.
		transposed(20, Inverse(a_proj));
		transposed(24, a_proj);
		transposed(28, Inverse(a_view));
		transposed(32, Inverse(viewProj));
		transposed(36, Inverse(proj));
		a_out[160] = a_eye.x;
		a_out[161] = a_eye.y;
		a_out[162] = a_eye.z;
		a_out[163] = 0.0f;
		if (a_previous) {
			a_out[164] = a_previous->eye.x;
			a_out[165] = a_previous->eye.y;
			a_out[166] = a_previous->eye.z;
		} else {
			a_out[164] = a_out[165] = a_out[166] = 0.0f;
		}
		a_out[167] = 0.0f;
	}

	void WriteRatios(const Ratios& a_ratios, float a_clampOffset, float* a_out)
	{
		const float w = a_ratios.width, h = a_ratios.height, pw = a_ratios.previousWidth, ph = a_ratios.previousHeight;
		const float c43[4] = { w, h, pw, ph };
		const float c44[4] = { 1.0f / w, 1.0f / h, w - a_clampOffset, pw - a_clampOffset };
		std::memcpy(a_out + 43 * 4, c43, sizeof(c43));
		std::memcpy(a_out + 44 * 4, c44, sizeof(c44));
	}

	bool Owned()
	{
		return RenderGraph::Engine::Native::Requested();
	}

	namespace
	{
		/** @brief Writes a_out from the adopted snapshot's main camera. False when it has none. */
		bool WriteMain(std::array<float, 2> a_jitter, const Ratios& a_ratios, const Previous& a_last, bool a_haveLast, globals::FrameBufferCache& a_out)
		{
			const auto& snapshot = Current();
			if (!snapshot)
				return false;
			const auto& view = snapshot->cameras.Current(ViewId::Main);
			if (!view.valid)
				return false;
			auto* out = reinterpret_cast<float*>(&a_out.data);
			// The first render has no previous camera: its own, so it starts without motion.
			const Previous self{ view.viewProj, view.position };
			WriteCamera(view.view, view.proj, view.viewProj, view.position, a_jitter, a_haveLast ? &a_last : &self, out);
			WriteRatios(a_ratios, Core().drClampOffset, out);
			// FrameParams (c42): the inverse of fGamma in x, 1 in y for the world view (0 in first person), z and w zero.
			a_out.data.FrameParams = { Core().inverseGamma, 1.0f, 0.0f, 0.0f };
			return true;
		}

		/** @brief Builds both blocks for the render frame. A new render frame first keeps what the previous one drew as its previous camera. */
		void Build(std::array<float, 2> a_jitter, const Ratios& a_ratios)
		{
			auto& render = Render();
			const auto frame = Snapshot::Render().frame;
			if (render.built && render.builtFrame != frame && render.wrote) {
				const auto& drawn = render.main.data;
				render.last.viewProjUnjittered = RowMajor(&drawn.CameraViewProjUnjittered);
				render.last.eye = { drawn.CameraPosAdjust.x, drawn.CameraPosAdjust.y, drawn.CameraPosAdjust.z };
				render.haveLast = true;
			}
			render.builtFrame = frame;
			render.built = true;
			render.jitter = a_jitter;
			render.ratios = a_ratios;
			render.fullResolution = false;
			render.wrote = WriteMain(a_jitter, a_ratios, render.last, render.haveLast, render.main);
			if (render.wrote)
				WriteMain({}, a_ratios, render.last, render.haveLast, render.post);
		}

		/**
		* @brief Builds the frame's blocks if nothing did this render frame: a frame without Main::UpdateJitter (a menu over the world,
		* the main menu) draws at full resolution without jitter, as the engine's state stands after the last PostDisplay.
		*/
		RenderCamera& Built()
		{
			auto& render = Render();
			if (!render.built || render.builtFrame != Snapshot::Render().frame)
				Build({}, {});
			return render;
		}
	}

	void BeginRender(std::array<float, 2> a_jitter, const Ratios& a_ratios)
	{
		Build(a_jitter, a_ratios);
	}

	void OnFullResolution()
	{
		if (!Owned())
			return;
		// The upscaler or Upscaling::PostDisplay set the engine back to full resolution: what draws after it reads ratios of 1.
		auto& render = Built();
		render.fullResolution = true;
		WriteRatios({}, Core().drClampOffset, reinterpret_cast<float*>(&render.post.data));
	}

	std::array<float, 2> ViewportRatios()
	{
		const auto& render = Built();
		if (render.fullResolution)
			return { 1.0f, 1.0f };
		return { render.ratios.width, render.ratios.height };
	}

	const globals::FrameBufferCache& Main()
	{
		if (Owned())
			return Built().main;
		return CS_ENGINE_READ(BoundState, BoundState, globals::game::frameBufferCached);
	}

	const globals::FrameBufferCache& Post()
	{
		if (Owned())
			return Built().post;
		return CS_ENGINE_READ(BoundState, BoundState, globals::game::frameBufferCached);
	}

	std::array<float, 16> RowMajor(const void* a_blockMatrix)
	{
		const auto* m = static_cast<const float*>(a_blockMatrix);
		std::array<float, 16> out;
		for (std::uint32_t r = 0; r < 4; ++r)
			for (std::uint32_t c = 0; c < 4; ++c)
				out[r * 4 + c] = m[c * 4 + r];
		return out;
	}

	std::array<float, 2> Jitter()
	{
		return Render().jitter;
	}
}
