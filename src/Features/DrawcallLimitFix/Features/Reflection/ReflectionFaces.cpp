// Water reflection update, add-root and face render hooks. Render thread.

// ReflectionView.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/EngineCasters.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <array>
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
			inline constexpr Engine::Offset kAccumulatorRenderCall{ { 107564, 0x2d8 }, { 100780, 0x2d6 } };  // the face render's FUN_1414a90f0(camera, accumulator, 8)
			inline constexpr Engine::Offset kAccumulatorRender{ { 106436 }, { 99789 } };
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
		AfterFaceDraws afterFaceDraws = nullptr;
		RootsOwned rootsOwned = nullptr;
		std::uint32_t skipRoots = 0;
		RootStats rootStats;

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

		/**
		 * @brief The add-root at a LOD call site, skipped when DCLF draws that root.
		 * Engine-only colour roots under it are still added, and the engine draws them in the faces.
		 */
		template <std::uint32_t Bit, std::uint32_t Site>
		struct AddLodRoot
		{
			static void thunk(void* a_camera, void* a_root)
			{
				if (!(skipRoots & Bit)) {
					func(a_camera, a_root);
					return;
				}
				const auto& subset = Scene::FrameView::Get().GetEngineCasters();
				if (!subset)
					return;
				const auto& colour = subset->Of(Scene::EngineOnlySubset::kColour);
				for (std::size_t r = 0; r < colour.roots.size(); ++r)
					if (colour.category[r] == a_root) {
						func(a_camera, const_cast<RE::NiAVObject*>(colour.roots[r]));
						++rootStats.engineOnlyRoots;
					}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The face's orientation (FUN_1414ed500), which sets the face's camera. */
		struct Orient
		{
			static void thunk(RE::NiCamera* a_camera, std::uint32_t a_face)
			{
				func(a_camera, a_face);
				if (faceOriented && a_camera)
					faceOriented(*a_camera, a_face);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The face's accumulator render (FUN_1414a90f0(camera, accumulator, 8)). */
		struct AccumulatorRender
		{
			static void thunk(RE::NiCamera* a_camera, void* a_accumulator, std::uint32_t a_flags)
			{
				func(a_camera, a_accumulator, a_flags);
				if (afterFaceDraws)
					afterFaceDraws();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSCubeMapCamera's face render (vfunc 0x35). a_faces' bits are the faces it renders. */
		struct FaceRender
		{
			static void thunk(RE::NiCamera* a_camera, std::uint32_t a_faces, bool a_silhouettes, bool a_clearRoots, bool a_noSky)
			{
				camera = a_camera;
				faceMask = a_faces;
				// Only mode 0's registrations reach PassCapture's hooks (FUN_1414b2330), so only then can a member's passes be withheld.
				plain = !a_silhouettes && !a_noSky;
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
			for (const auto& offset : { kReflectSky, kOrientCall, kOrient, kAccumulatorRenderCall, kAccumulatorRender, kUpdateCall, kUpdate, kAddRootCalls[0],
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
		HookSites::RequireCall("the water reflection's accumulator render call", base + kAccumulatorRenderCall);
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
		// Preflight checked every call. The orientation, accumulator render and update chain onto whatever they call.
		stl::write_vfunc<0x35, FaceRender>(RE::VTABLE_BSCubeMapCamera[0]);
		stl::write_thunk_call<Orient>(base + kOrientCall);
		stl::write_thunk_call<AccumulatorRender>(base + kAccumulatorRenderCall);
		stl::write_thunk_call<Update>(base + kUpdateCall);
		// LOD land 0x14315b898, LOD objects 0x14315b8a0, LOD trees 0x14315b880.
		stl::write_thunk_call<AddLodRoot<kLodRoots, 0>>(base + kAddRootCalls[0]);
		stl::write_thunk_call<AddLodRoot<kLodRoots, 1>>(base + kAddRootCalls[1]);
		stl::write_thunk_call<AddLodRoot<kTreeRoot, 2>>(base + kAddRootCalls[2]);
		logger::info("[DCLF] reflection face hooks installed: the update, its LOD add-roots, the face render (vfunc 0x35), its orientation and draws");
	}

	bool InFace() { return camera != nullptr; }
	std::uint32_t FaceMask() { return faceMask; }
	bool Plain() { return plain; }
	void SetFaceOriented(FaceOriented a_callback) { faceOriented = a_callback; }
	void SetAfterFaceDraws(AfterFaceDraws a_callback) { afterFaceDraws = a_callback; }
	void SetRootsOwned(RootsOwned a_callback) { rootsOwned = a_callback; }

	RootStats TakeRootStats()
	{
		const auto out = rootStats;
		rootStats = {};
		return out;
	}
}
