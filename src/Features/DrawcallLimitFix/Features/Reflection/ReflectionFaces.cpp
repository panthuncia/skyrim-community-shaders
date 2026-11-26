// Water reflection update, add-root and face render hooks. Render thread.

// ReflectionView.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Snapshot/EngineReads.h"

#include "Deferred.h"
#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

#include <array>
#include <cstring>
#include <set>
#include <utility>

namespace DCLF::Features::Reflection::ReflectionFaces
{
	static_assert(kLodRoots == Api::ViewRootBit(Api::ViewRoot::LodLandAndObjects) && kTreeRoot == Api::ViewRootBit(Api::ViewRoot::LodTrees),
		"ReflectionFaces' root bits are Api::ViewRootBit's");

	namespace
	{
		namespace Address
		{
			inline constexpr Engine::Offset kReflectSky{ { 380026 }, { 508381 } };  // bool: the water reflection draws the sky
			inline constexpr Engine::Offset kUpdateCall{ { 32174, 0xc1c }, { 31383, 0x8a0 } };  // TESWaterReflections::Update's call
			inline constexpr Engine::Offset kUpdate{ { 32160 }, { 31373 } };
			inline constexpr Engine::Offset kOrientCall{ { 107564, 0xe0 }, { 100780, 0xe5 } };  // the face render's FUN_1414ed500(camera, face)
			inline constexpr Engine::Offset kOrient{ { 107563 }, { 100779 } };
			// The update's LOD add-roots. AE calls FUN_1405275e0(camera, root). SE calls the shared array append FUN_1401d05c0 directly,
			// so AddLodRoot's first argument is the array there. It only forwards it.
			inline constexpr std::array<Engine::Offset, 3> kAddRootCalls{ { { { 32160, 0x29d }, { 31373, 0x28e } }, { { 32160, 0x2b6 }, { 31373, 0x2ae } }, { { 32160, 0x2cf }, { 31373, 0x2ce } } } };
			inline constexpr Engine::Offset kAddRoot{ { 32258 }, { 15623 } };
		}

		// Render thread only. The update, the face render and every caller run there.
		const RE::NiCamera* camera = nullptr;
		std::uint32_t faceMask = 0;
		bool plain = false;
		FaceOriented faceOriented = nullptr;
		RootsOwned rootsOwned = nullptr;
		std::uint32_t skipRoots = 0;
		RootStats rootStats;

		/** @brief Since the last report. */
		struct FaceStats
		{
			std::uint32_t native = 0, engine = 0, submitFailed = 0;
			std::string engineWhy;  // the last reason a face was the engine's
		};
		FaceStats faceStats;

		/**
		 * @brief BSCubeMapCamera::SetFace's orientation (AE 0x1414ed500, SE 0x1412fe0e0): the world rotation's rows, its columns the
		 * view direction, up and right in D3D's cube map conventions (+X, -X, +Y, -Y, +Z, -Z).
		 */
		constexpr std::array<std::array<float, 9>, 6> kFaceRotations{ {
			{ 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f },
			{ -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 0.0f },
		} };

		std::array<float, 4> PortOf(const RE::NiCamera& a_camera)
		{
			// NiRect<float> members are protected. The order is left, right, top, bottom.
			std::array<float, 4> port;
			std::memcpy(port.data(), &const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2().port, sizeof(port));
			return port;
		}

		/** @brief The reflection cube's face size, read again only when the engine's cube texture changes. */
		std::array<std::uint32_t, 2> CubeSize()
		{
			static ID3D11Texture2D* texture = nullptr;
			static std::array<std::uint32_t, 2> size{};
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
			auto* next = renderer ? ::RenderGraph::Engine::Tripwire::Unwrap(renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS].texture) : nullptr;
			if (next != texture) {
				texture = next;
				size = {};
				if (texture) {
					D3D11_TEXTURE2D_DESC desc{};
					texture->GetDesc(&desc);
					size = { desc.Width, desc.Height };
				}
			}
			return size;
		}

		/**
		 * @brief A face's camera without SetFace: the position TESWaterReflections::Update gave the cube camera (its local translate;
		 * the world transform follows only at SetFace's update), the face's rotation, and the frustum and port the update set.
		 */
		Sky::FaceView NativeFace(const RE::NiCamera& a_cube, std::uint32_t a_face, const std::array<std::uint32_t, 2>& a_size, ShadowCameras::Camera& a_camera)
		{
			Sky::FaceView view;
			view.face = a_face;
			const auto& rotation = kFaceRotations[a_face];
			for (std::uint32_t r = 0; r < 3; ++r)
				for (std::uint32_t c = 0; c < 3; ++c)
					view.world.rotate.entry[r][c] = rotation[r * 3 + c];
			view.world.translate = a_cube.local.translate;
			view.world.scale = a_cube.local.scale;
			view.frustum = const_cast<RE::NiCamera&>(a_cube).GetRuntimeData2().viewFrustum;
			const auto port = PortOf(a_cube);
			ShadowCameras::CameraMatrices(view.world, view.frustum, port, a_camera);
			view.viewProj = a_camera.viewProj;
			view.eye = view.world.translate;
			view.rect = ShadowCameras::Viewport(port, a_size[0], a_size[1]);
			return view;
		}

		/** @brief TESWaterReflections::Update (0x140520570). Decides which roots DCLF draws before its add-roots. */
		struct Update
		{
			static void thunk(void* a_reflection)
			{
				// Flags at +0x10: bit 12 drops the LOD roots, bit 4 adds silhouettes (mode 0x1B). Without bit 12 and bReflectSky (0x1420104e8)
				// the sky is suppressed (0x19). Only mode 0 is plain.
				const auto flags = *reinterpret_cast<const std::uint16_t*>(static_cast<const std::byte*>(a_reflection) + 0x10);
				static const REL::Relocation<const bool*> reflectSky{ REL::Offset(Address::kReflectSky) };
				const bool silhouettes = (flags >> 4) & 1;
				const bool noSky = !((flags >> 12) & 1) && !*reflectSky.get();
				skipRoots = rootsOwned ? rootsOwned(!silhouettes && !noSky) : 0u;
				++rootStats.updates;
				rootStats.lodSkipped += (skipRoots & kLodRoots) ? 1u : 0u;
				rootStats.treeSkipped += (skipRoots & kTreeRoot) ? 1u : 0u;
				func(a_reflection);
				skipRoots = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The add-root at a LOD call site, skipped when DCLF draws that root. */
		template <std::uint32_t Bit, std::uint32_t Site>
		struct AddLodRoot
		{
			static void thunk(void* a_camera, void* a_root)
			{
				if (!(skipRoots & Bit))
					func(a_camera, a_root);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The face's orientation (FUN_1414ed500), which sets the face's camera. */
		struct Orient
		{
			static void thunk(RE::NiCamera* a_camera, std::uint32_t a_face)
			{
				func(a_camera, a_face);
				if (!a_camera)
					return;
				ShadowCameras::Camera engine;
				ShadowCameras::CameraMatrices(*a_camera, engine);
				if (faceOriented)
					faceOriented(engine, a_face);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The cube camera's LOD roots (BSCubeMapCamera +0x188), released after an update's last face as the face render does. */
		void ReleaseRoots(RE::NiCamera* a_camera)
		{
			auto& roots = *reinterpret_cast<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>*>(reinterpret_cast<std::byte*>(a_camera) + 0x188);
			roots.clear();
		}

		/**
		 * @brief Draws a_faces without the engine's face render: per face, its sky computed from the Sky object list and DCLF's LOD camera,
		 * after CS's reflection prepasses (Deferred, as its face render hook runs them). The prepasses and the inputs they bind are the
		 * same for every face, so they run once a frame. Returns the faces left to the engine.
		 */
		std::uint32_t DrawNative(RE::NiCamera* a_camera, std::uint32_t a_faces)
		{
			DCLF_ZONE("CS.DCLF.Reflection.NativeFaces");
			struct Prepared
			{
				std::uint32_t face = 0;
				ShadowCameras::Camera camera;
				std::shared_ptr<Sky::Run> run;
			};
			std::array<Prepared, 6> prepared;
			std::uint32_t count = 0;
			std::uint32_t left = 0;
			const auto size = CubeSize();
			// Each face's run first. A face the sky family cannot take is the engine's, whose face render runs the prepasses itself.
			for (std::uint32_t f = 0; f < 6; ++f) {
				const std::uint32_t bit = 1u << f;
				if (!(a_faces & bit))
					continue;
				auto& face = prepared[count];
				std::string why;
				face.run = Sky::PrepareFace(NativeFace(*a_camera, f, size, face.camera), why);
				if (!face.run) {
					left |= bit;
					++faceStats.engine;
					static std::set<std::string> logged;
					if (logged.size() < 32 && logged.insert(why).second)
						logger::info("[DCLF] reflection native: the engine renders a face ({}); logged once per reason", why);
					faceStats.engineWhy = std::move(why);
					continue;
				}
				face.face = f;
				++count;
			}
			if (!count)
				return left;
			camera = a_camera;
			faceMask = 1u << prepared[0].face;
			plain = true;
			// The update renders one face a call, and a frame may hold several updates. The prepasses bind the same views every time and
			// their graph clients run only at the frame's first, so they run and are read once a frame.
			static std::uint32_t preparedFrame = UINT32_MAX;
			static Sky::FaceInputs inputs;
			const auto frame = Family::GameFrame();
			const bool first = std::exchange(preparedFrame, frame) != frame;
			globals::deferred->BeginReflectionFace(first);
			if (first)
				inputs = Sky::ReadFaceInputs();
			for (std::uint32_t i = 0; i < count; ++i) {
				auto& face = prepared[i];
				faceMask = 1u << face.face;
				if (!Sky::SubmitFace(std::move(face.run), inputs))
					++faceStats.submitFailed;
				if (faceOriented)
					faceOriented(face.camera, face.face);
			}
			globals::deferred->EndReflectionFace();
			camera = nullptr;
			faceMask = 0;
			if (faceStats.native == 0) {
				static bool logFirst = true;
				if (std::exchange(logFirst, false))
					logger::info("[DCLF] reflection native: the first face drawn without the engine's face render (face {})", prepared[0].face);
			}
			faceStats.native += count;
			return left;
		}

		/** @brief BSCubeMapCamera's face render (vfunc 0x35). a_faces' bits are the faces it renders. */
		struct FaceRender
		{
			static void thunk(RE::NiCamera* a_camera, std::uint32_t a_faces, bool a_silhouettes, bool a_clearRoots, bool a_noSky)
			{
				// Only mode 0's registrations reach PassCapture's hooks (FUN_1414b2330), so only then can a member's passes be withheld.
				const bool plainMode = !a_silhouettes && !a_noSky;
				// A plain face whose LOD roots are all DCLF's holds only the sky, which the sky family computes, so the engine's render
				// is skipped. An update that adds roots DCLF does not draw stays the engine's.
				if (a_camera && plainMode && skipRoots == (kLodRoots | kTreeRoot) && Sky::Enabled()) {
					a_faces = DrawNative(a_camera, a_faces);
					if (!a_faces) {
						if (a_clearRoots)
							ReleaseRoots(a_camera);
						return;
					}
				}
				camera = a_camera;
				faceMask = a_faces;
				plain = plainMode;
				auto& capture = PassCapture::Get();
				// BSCubeMapCamera's two accumulators are at +0x1A0 and +0x1A8. The faces cull into the first.
				if (a_camera) {
					const auto* bytes = reinterpret_cast<const std::byte*>(a_camera);
					Api::ViewAccumulators accumulators;
					accumulators.accumulators[0] = *reinterpret_cast<void* const*>(bytes + 0x1A0);
					accumulators.accumulators[1] = *reinterpret_cast<void* const*>(bytes + 0x1A8);
					accumulators.count = 2;
					capture.SetViewRenderers(kView, accumulators);
				}
				capture.SetViewWithholdActive(kView, plain);
				capture.SetViewRendering(kView, true);
				func(a_camera, a_faces, a_silhouettes, a_clearRoots, a_noSky);
				capture.SetViewRendering(kView, false);
				capture.SetViewWithholdActive(kView, false);
				camera = nullptr;
				faceMask = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		bool Known()
		{
			using namespace Address;
			for (const auto& offset : { kReflectSky, kOrientCall, kOrient, kUpdateCall, kUpdate, kAddRootCalls[0],
					 kAddRootCalls[1], kAddRootCalls[2], kAddRoot })
				if (!offset.Known())
					return false;
			return true;
		}
	}

	void Preflight()
	{
		using namespace Address;
		if (!Known())
			return;
		const auto base = REL::Module::get().base();
		HookSites::RequireCall("the water reflection's face orientation call", base + kOrientCall);
		HookSites::RequireCall("the water reflection's update call", base + kUpdateCall);
		for (const auto& site : kAddRootCalls)
			HookSites::RequireCallTo("the water reflection's LOD add-root calls", base + site, base + kAddRoot);
	}

	void Install()
	{
		using namespace Address;
		const auto base = REL::Module::get().base();
		if (!Known()) {
			logger::warn("[DCLF] reflection face hooks not installed: this runtime lacks their addresses");
			return;
		}
		// Preflight checked every call. The orientation and update chain onto whatever they call.
		stl::write_vfunc<0x35, FaceRender>(RE::VTABLE_BSCubeMapCamera[0]);
		stl::write_thunk_call<Orient>(base + kOrientCall);
		stl::write_thunk_call<Update>(base + kUpdateCall);
		// LOD land 0x14315b898, LOD objects 0x14315b8a0, LOD trees 0x14315b880.
		stl::write_thunk_call<AddLodRoot<kLodRoots, 0>>(base + kAddRootCalls[0]);
		stl::write_thunk_call<AddLodRoot<kLodRoots, 1>>(base + kAddRootCalls[1]);
		stl::write_thunk_call<AddLodRoot<kTreeRoot, 2>>(base + kAddRootCalls[2]);
		logger::info("[DCLF] reflection face hooks installed: the update, its LOD add-roots, the face render (vfunc 0x35) and its orientation");
	}

	bool InFace() { return camera != nullptr; }
	std::uint32_t FaceMask() { return faceMask; }
	bool Plain() { return plain; }
	void SetFaceOriented(FaceOriented a_callback) { faceOriented = a_callback; }
	void SetRootsOwned(RootsOwned a_callback) { rootsOwned = a_callback; }

	std::string TakeReport()
	{
		const auto s = std::exchange(faceStats, {});
		return fmt::format("[DCLF] reflection faces without the engine: {} drawn natively, {} sky submits failed{}, {} the engine's{}\n", s.native, s.submitFailed,
			s.submitFailed ? " <- FAILED" : "", s.engine, s.engineWhy.empty() ? std::string() : fmt::format(" (last: {})", s.engineWhy));
	}

	RootStats TakeRootStats()
	{
		const auto out = rootStats;
		rootStats = {};
		return out;
	}
}
