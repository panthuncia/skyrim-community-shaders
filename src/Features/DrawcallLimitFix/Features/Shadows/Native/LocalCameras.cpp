// Ports of the per-light pieces of CalculateActiveShadowCasterLights (AE 0x1414cc570, SE 0x1412e2f60): the frustum and paraboloid
// lights' UpdateCamera, the focus cameras, the lens flare queue, the scissor rectangle, the release of a light not kept, and the
// bookkeeping of Accumulate. Each keeps the engine's writes and its float operation order.
// Still called through the engine: memory allocation, the BSShaderAccumulator and NiCamera constructors (first use only),
// NiAVObject::Update on the light's own cameras, BSTArrayBase::AddUninitialized (the lens flare list's growth) and
// BSPortalGraphEntry::ClearVisibility (it takes the portal graph's lock and releases its rooms' visibility records).

#include "Features/DrawcallLimitFix/Features/Shadows/Native/LocalCameras.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>

namespace DCLF::Features::Shadows::Native
{
	namespace LocalCamerasImpl
	{
		namespace
		{
			using DCLF::Engine::At;
			using Descriptor = RE::BSShadowLight::ShadowmapDescriptor;

			/** @brief An engine global by its Address Library ids, SE then AE. The address is looked up once. */
			template <class T, std::uint64_t Se, std::uint64_t Ae>
			T& Global()
			{
				static const std::uintptr_t address = REL::RelocationID(Se, Ae).address();
				return *reinterpret_cast<T*>(address);
			}

			/** @brief An engine function by its Address Library ids, SE then AE. */
			template <class F, std::uint64_t Se, std::uint64_t Ae>
			F Function()
			{
				static const std::uintptr_t address = REL::RelocationID(Se, Ae).address();
				return reinterpret_cast<F>(address);
			}

			// Engine globals (AE address in the comment).
			float& LodFadeCutoff() { return Global<float, 528316, 415264>(); }                              // 0x1433dcfac: UpdateCamera's lodFade cutoff
			std::uint16_t& ShadowMapSize() { return Global<std::uint16_t, 513206, 390938>(); }              // 0x142032ff4: a spot light's shadow map size
			RE::ShadowSceneNode** SceneNodes() { return &Global<RE::ShadowSceneNode*, 513211, 390951>(); }  // 0x142033060: ShadowSceneNode* per scene graph index
			std::uint32_t& TlsIndex() { return Global<std::uint32_t, 528600, 415542>(); }                   // 0x1435f0878: the executable's TLS index
			std::uint32_t& FocusCount() { return Global<std::uint32_t, 527703, 414625>(); }                 // 0x14332a498: the focus targets' count
			std::byte*& FocusTargets() { return Global<std::byte*, 527701, 414623>(); }                     // 0x14332a488: the focus targets, 16 bytes each, the node at +8
			float& FocusDistance() { return Global<float, 513753, 391721>(); }                              // 0x1420357c0: the focus port's falloff distance (450)
			std::uint32_t& FocusMapSize() { return Global<std::uint32_t, 513751, 391718>(); }               // 0x1420357a8: the focus shadow map size (2048)
			std::uint8_t& LensFlaresOn() { return Global<std::uint8_t, 513349, 391128>(); }                 // 0x142033658: lens flares are on
			std::byte* LensFlareList() { return &Global<std::byte, 527921, 414868>(); }                     // 0x14338c250: BSTArray<NiPointer<BSLight>>
			std::uint32_t& ShadowFreeMask() { return Global<std::uint32_t, 513748, 391715>(); }             // 0x142035798: the free slices of kSHADOWMAPS
			std::uint32_t& CameraInsideMask() { return Global<std::uint32_t, 528093, 415038>(); }           // 0x14338c90c: the lights whose sphere holds the camera
			std::uint32_t& ShadowLightCount() { return Global<std::uint32_t, 528091, 415036>(); }           // 0x14338c904: gLastFrameEnabledShadowCasterLightsCount2
			std::uint32_t& ScreenWidth() { return Global<std::uint32_t, 524978, 411459>(); }                // 0x14328c078
			std::uint32_t& ScreenHeight() { return Global<std::uint32_t, 524979, 411460>(); }               // 0x14328c07c

			// Engine functions still called.
			using AccumulatorCtor = RE::BSShaderAccumulator* (*)(void*, std::uint32_t);
			using CameraCtor = RE::NiCamera* (*)(void*);
			using ClearVisibility = void (*)(void*);
			using AddUninitialized = std::uint32_t (*)(void*, void*, std::uint32_t, std::uint32_t);
			AccumulatorCtor MakeAccumulator() { return Function<AccumulatorCtor, 99920, 106564>(); }             // AE 0x1414b1cf0
			CameraCtor MakeCamera() { return Function<CameraCtor, 69258, 70624>(); }                             // AE 0x140d2afa0
			ClearVisibility ClearEntryVisibility() { return Function<ClearVisibility, 74395, 76119>(); }         // AE 0x140e14200
			AddUninitialized ArrayAddUninitialized() { return Function<AddUninitialized, 66908, 68163>(); }      // AE 0x140cc6890
			std::uintptr_t HeapAllocatorFunctorVtable() { return REL::RelocationID(228548, 186376).address(); }  // AE 0x141768440

			// Layouts the functions read and write without CommonLib members.
			constexpr std::size_t kRefCount = 0x8;           // NiRefObject's reference count
			constexpr std::size_t kObjectFlags = 0xF4;       // NiAVObject::flags; bit 0 hidden
			constexpr std::size_t kWorldBoundCentre = 0xE4;  // NiAVObject::worldBound centre, then radius at +0xF0
			constexpr std::size_t kWorldBoundRadius = 0xF0;
			constexpr std::size_t kCameraPort = 0x174;  // NiCamera::port, NiRect<float>: left, right, top, bottom
			constexpr std::size_t kCameraSize = 0x188;
			constexpr std::size_t kDescriptorPortLeft = 0xD0;  // ShadowmapDescriptor::port, NiRect<int32>
			constexpr std::size_t kDescriptorPortRight = 0xD4;
			constexpr std::size_t kDescriptorPortTop = 0xD8;
			constexpr std::size_t kDescriptorPortBottom = 0xDC;
			constexpr std::size_t kLightRectLeft = 0x544;  // BSShadowLight::projectedBoundingBox, NiRect<uint32>
			constexpr std::size_t kLightRectRight = 0x548;
			constexpr std::size_t kLightRectTop = 0x54C;
			constexpr std::size_t kLightRectBottom = 0x550;
			constexpr std::size_t kAccumulatorSize = 0x180;
			constexpr std::uint32_t kAccumulatorCtorArg = 99;      // what the shadow lights pass the constructor
			constexpr std::size_t kAccumulatorDrawDecals = 0x12C;  // bool, cleared on creation
			constexpr std::size_t kAccumulatorUnk12D = 0x12D;      // bool, set on creation
			constexpr std::size_t kAccumulatorSceneNode = 0x148;   // ShadowSceneNode* activeShadowSceneNode
			constexpr std::size_t kAccumulatorRenderMode = 0x150;
			constexpr std::uint32_t kSpotRenderMode = 0xD;            // a spot light's shadow map render mode
			constexpr std::size_t kAccumulatorLightIndex = 0x160;     // the shadow light count + 1
			constexpr std::size_t kAccumulatorCascade = 0x168;        // a sun cascade's index
			constexpr std::size_t kAccumulatorShadowFlag = 0x12E;     // set by the sun's Accumulate
			constexpr std::uint32_t kFocusRenderMode = 0xE;           // the focus accumulators' render mode (FUN_1414f0b90)
			constexpr std::size_t kAccumulatorLightBit = 0x164;       // the light's mask bit
			constexpr std::size_t kProcessPortalEntry = 0x30190;      // BSCullingProcess's BSPortalGraphEntry*
			constexpr std::size_t kProcessBackAccumulator = 0x30200;  // BSParabolicCullingProcess's second accumulator (NiPointer)
			constexpr std::size_t kEntryVisibility = 0x30;            // BSPortalGraphEntry fields ClearVisibility writes
			constexpr std::size_t kEntryCount = 0x44;
			constexpr std::size_t kEntrySlots = 0x60;
			constexpr std::size_t kEntrySlotSize = 0x18;
			constexpr std::size_t kMemoryContext = 0x768;  // the thread's allocation context, in its TLS block
			constexpr std::uint32_t kAccumulatorContext = 0x1D;
			constexpr std::uint32_t kNotCulled = 0;  // BSLight::frustrumCull: visible
			constexpr std::uint32_t kCulled = 0xFF;  // BSLight::frustrumCull: culled
			// ShadowmapDescriptor::unitsPerTexel holds a float's bits; ReturnShadowmaps resets it to 1.
			constexpr std::uint32_t kUnitsPerTexelOne = std::bit_cast<std::uint32_t>(1.0f);

			enum class Kind
			{
				Other,
				Directional,
				Frustum,
				Parabolic,
			};

			Kind KindOf(const RE::BSShadowLight& a_light)
			{
				static const std::uintptr_t directional = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSShadowDirectionalLight[0] }.address();
				static const std::uintptr_t frustum = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSShadowFrustumLight[0] }.address();
				static const std::uintptr_t parabolic = REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSShadowParabolicLight[0] }.address();
				const auto vtable = *reinterpret_cast<const std::uintptr_t*>(&a_light);
				return vtable == frustum ? Kind::Frustum : vtable == parabolic ? Kind::Parabolic :
				                                       vtable == directional   ? Kind::Directional :
				                                                                 Kind::Other;
			}

			RE::NiCamera::RUNTIME_DATA2& CameraData(const RE::NiCamera& a_camera) { return const_cast<RE::NiCamera&>(a_camera).GetRuntimeData2(); }
			float LightRadius(const RE::NiLight& a_light) { return const_cast<RE::NiLight&>(a_light).GetLightRuntimeData().radius.x; }

			/** @brief Sets the thread's allocation context for one allocation, as the engine does around a spot light's accumulator. */
			class MemoryContext
			{
			public:
				explicit MemoryContext(std::uint32_t a_context)
				{
					auto** blocks = reinterpret_cast<std::byte**>(__readgsqword(0x58));
					slot = reinterpret_cast<std::uint32_t*>(blocks[TlsIndex()] + kMemoryContext);
					saved = *slot;
					*slot = a_context;
				}
				~MemoryContext() { *slot = saved; }
				MemoryContext(const MemoryContext&) = delete;
				MemoryContext& operator=(const MemoryContext&) = delete;

			private:
				std::uint32_t* slot = nullptr;
				std::uint32_t saved = 0;
			};

			/** @brief The accumulator a spot light's UpdateCamera makes on first use: render mode 0xD, no decals. */
			void MakeSpotAccumulator(Descriptor& a_descriptor)
			{
				const MemoryContext context(kAccumulatorContext);
				void* memory = RE::MemoryManager::GetSingleton()->Allocate(kAccumulatorSize, 0, false);
				RE::BSShaderAccumulator* accumulator = memory ? MakeAccumulator()(memory, kAccumulatorCtorArg) : nullptr;
				a_descriptor.shaderAccumulator.reset(accumulator);
				auto* made = a_descriptor.shaderAccumulator.get();
				At<std::uint32_t>(made, kAccumulatorRenderMode) = kSpotRenderMode;
				At<bool>(made, kAccumulatorDrawDecals) = false;
				At<bool>(made, kAccumulatorUnk12D) = true;
			}

			/** @brief Port of NiCamera::SetViewFrustum (AE FUN_140d2b0e0, SE FUN_140c651d0). The near plane is kept within the camera's limits. */
			void SetViewFrustum(RE::NiCamera& a_camera, const RE::NiFrustum& a_frustum)
			{
				auto& data = CameraData(a_camera);
				float nearPlane = a_frustum.fNear;
				data.viewFrustum.fNear = nearPlane;
				const float farLimit = a_frustum.fFar / data.maxFarNearRatio;
				if (!(nearPlane >= farLimit)) {
					data.viewFrustum.fNear = farLimit;
					nearPlane = farLimit;
				}
				if (!(nearPlane >= data.minNearPlaneDist))
					data.viewFrustum.fNear = data.minNearPlaneDist;
				data.viewFrustum.fLeft = a_frustum.fLeft;
				data.viewFrustum.fRight = a_frustum.fRight;
				data.viewFrustum.fTop = a_frustum.fTop;
				data.viewFrustum.fBottom = a_frustum.fBottom;
				data.viewFrustum.fFar = a_frustum.fFar;
				data.viewFrustum.bOrtho = a_frustum.bOrtho;
			}

			/** @brief NiAVObject::Update with zeroed update data, as the shadow lights call it on their cameras. */
			void UpdateNode(RE::NiAVObject& a_node)
			{
				RE::NiUpdateData data{};
				a_node.Update(data);
			}

			/** @brief Puts a camera at the light: its world rotation and position become the camera's local ones. */
			void PlaceAtLight(RE::NiCamera& a_camera, const RE::NiLight& a_light)
			{
				a_camera.local.translate = a_light.world.translate;
				a_camera.local.rotate = a_light.world.rotate;
			}

			// FUN_14150aba0 (AE), FUN_14131e000 (SE), as LightSelection.cpp ports it: the eye, then the far rectangle's corners in its order.
			using Corners = std::array<std::array<float, 3>, 5>;

			Corners FrustumCorners(const RE::NiCamera& a_camera)
			{
				const auto& r = a_camera.world.rotate.entry;
				const auto& t = a_camera.world.translate;
				const auto& f = CameraData(a_camera).viewFrustum;
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

			// Tests all six planes, whatever the mask.
			bool CornersOrEdgesInside(const Corners& a_points, const DCLF::ShadowCameras::Planes& a_planes)
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
				DCLF::ShadowCameras::Planes viewPlanes, lightPlanes;
				DCLF::ShadowCameras::FrustumPlanes(CameraData(a_view).viewFrustum, a_view.world, viewPlanes);
				DCLF::ShadowCameras::FrustumPlanes(CameraData(a_light).viewFrustum, a_light.world, lightPlanes);
				return CornersOrEdgesInside(FrustumCorners(a_view), lightPlanes) || CornersOrEdgesInside(FrustumCorners(a_light), viewPlanes);
			}

			/** @brief Port of BSMultiBoundSphere's plane test (AE 0x140e20250, SE FUN_140d48cc0) against the active planes. NaN counts as outside. */
			bool SphereInside(const float (&a_centre)[3], float a_radius, const DCLF::ShadowCameras::Planes& a_planes)
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

			/**
			 * @brief The lodDimmer both UpdateCameras leave: unchanged for a culled light, else 1, or 0 with the light culled when its lodFade
			 * distance is past the cutoff. Each class sums the squared distance in its own order.
			 */
			void ApplyLodFade(RE::BSShadowLight& a_light, const RE::NiCamera& a_view, const RE::NiLight& a_niLight, bool a_frustumOrder)
			{
				if (a_light.frustrumCull == kCulled)
					return;
				const auto& position = a_niLight.world.translate;
				const float dx = position.x - a_view.world.translate.x;
				const float dy = position.y - a_view.world.translate.y;
				const float dz = position.z - a_view.world.translate.z;
				float dimmer = 1.0f;
				if (a_light.lodFade) {
					const float radius = LightRadius(a_niLight);
					const float squared = a_frustumOrder ? (dy * dy + dx * dx) + dz * dz : (dx * dx + dy * dy) + dz * dz;
					if ((squared - radius * radius) * CameraData(a_view).lodAdjust >= LodFadeCutoff()) {
						a_light.frustrumCull = kCulled;
						dimmer = 0.0f;
					}
				}
				a_light.lodDimmer = dimmer;
			}

			/** @brief The clamp each projected axis gets: a minimum below a_low snaps the pair to the range, as the engine branches. */
			void ClampAxis(float& a_min, float& a_max, float a_low)
			{
				if (!(a_low > a_min)) {
					if (!(1.0f >= a_max)) {
						a_max = 1.0f;
						if (!(1.0f >= a_min))
							a_min = 1.0f;
					}
				} else {
					a_min = a_low;
					if (!(a_max >= a_low))
						a_max = a_low;
					else if (a_max > 1.0f)
						a_max = 1.0f;
				}
			}

			/** @brief The descriptors a light owns, plus its four focus descriptors, for the write list. */
			template <class Visit>
			void ForEachDescriptor(RE::BSShadowLight& a_light, Visit&& a_visit)
			{
				auto& data = a_light.GetRuntimeData();
				for (auto& descriptor : data.shadowmapDescriptors)
					a_visit(descriptor);
				for (auto& descriptor : data.focusShadowmapDescriptors)
					a_visit(descriptor);
			}

			bool UpdateFrustumCameraImpl(RE::BSShadowFrustumLight& a_light, const RE::NiCamera& a_viewCamera)
			{
				a_light.frustrumCull = kCulled;
				const auto* niLight = a_light.light.get();
				auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
				// The engine reads both without a check. A missing one would crash it; here the light stays culled.
				if (!niLight || descriptors.empty() || (At<std::uint8_t>(niLight, kObjectFlags) & 1))
					return false;
				auto& descriptor = descriptors[0];
				if (a_light.dynamic && descriptor.camera) {
					auto& camera = *descriptor.camera;
					PlaceAtLight(camera, *niLight);
					const auto& spot = a_light.GetShadowFrustumLightRuntimeData();
					RE::NiFrustum frustum = CameraData(camera).viewFrustum;
					frustum.fNear = spot.nearDistance;
					frustum.fFar = spot.farDistance > 0.0f ? spot.farDistance : LightRadius(*niLight);
					SetViewFrustum(camera, frustum);
					UpdateNode(camera);
				}
				if (!descriptor.shaderAccumulator)
					MakeSpotAccumulator(descriptor);
				if (auto* accumulator = descriptor.shaderAccumulator.get())
					At<RE::ShadowSceneNode*>(accumulator, kAccumulatorSceneNode) = SceneNodes()[0];
				const std::uint32_t size = ShadowMapSize();
				At<std::uint32_t>(&descriptor, kDescriptorPortRight) = size;
				At<std::uint32_t>(&descriptor, kDescriptorPortTop) = size;
				if (!descriptor.camera)
					return false;
				a_light.frustrumCull = FrustaIntersect(a_viewCamera, *descriptor.camera) ? kNotCulled : kCulled;
				ApplyLodFade(a_light, a_viewCamera, *niLight, true);
				return a_light.frustrumCull != kCulled;
			}

			bool UpdateParabolicCameraImpl(RE::BSShadowParabolicLight& a_light, const RE::NiCamera& a_viewCamera)
			{
				a_light.frustrumCull = kCulled;
				const auto* niLight = a_light.light.get();
				auto& data = a_light.GetRuntimeData();
				auto& descriptors = data.shadowmapDescriptors;
				if (!niLight || descriptors.empty() || (At<std::uint8_t>(niLight, kObjectFlags) & 1))
					return false;
				const bool twoSided = a_light.shadowMapCount == 2 && descriptors.size() >= 2;
				if (a_light.dynamic) {
					if (auto* front = descriptors[0].camera.get()) {
						PlaceAtLight(*front, *niLight);
						UpdateNode(*front);
					}
					if (twoSided && descriptors[1].camera) {
						// The back hemisphere looks the same way with its up axis (rotation column 1) flipped.
						auto& back = *descriptors[1].camera;
						back.local.translate = niLight->world.translate;
						const auto& m = niLight->world.rotate.entry;
						auto& r = back.local.rotate.entry;
						for (std::uint32_t row = 0; row < 3; ++row) {
							r[row][0] = m[row][0];
							r[row][1] = -m[row][1];
							r[row][2] = m[row][2];
						}
						UpdateNode(back);
					}
				}
				auto* sceneNode = SceneNodes()[data.sceneGraphIndex];
				if (auto* accumulator = descriptors[0].shaderAccumulator.get())
					At<RE::ShadowSceneNode*>(accumulator, kAccumulatorSceneNode) = sceneNode;
				if (twoSided)
					if (auto* accumulator = descriptors[1].shaderAccumulator.get())
						At<RE::ShadowSceneNode*>(accumulator, kAccumulatorSceneNode) = sceneNode;
				// A BSMultiBoundSphere at the light, tested against the planes NiCullingProcess::SetFrustum builds from the view camera.
				DCLF::ShadowCameras::Planes planes;
				DCLF::ShadowCameras::FrustumPlanes(CameraData(a_viewCamera).viewFrustum, a_viewCamera.world, planes);
				const auto& position = niLight->world.translate;
				const float centre[3] = { position.x, position.y, position.z };
				a_light.frustrumCull = SphereInside(centre, LightRadius(*niLight), planes) ? kNotCulled : kCulled;
				ApplyLodFade(a_light, a_viewCamera, *niLight, false);
				return a_light.frustrumCull != kCulled;
			}

			void UpdateFocusCamerasImpl(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera)
			{
				auto& data = a_light.GetRuntimeData();
				const std::uint32_t count = FocusCount();
				if (!count || data.shadowmapDescriptors.empty() || !data.shadowmapDescriptors[0].camera)
					return;
				// The engine does not bound the count by the four descriptors.
				const std::uint32_t focusCount = count < std::size(data.focusShadowmapDescriptors) ? count : static_cast<std::uint32_t>(std::size(data.focusShadowmapDescriptors));
				for (std::uint32_t i = 0; i < focusCount; ++i) {
					// The light's first camera's local axes, read again for each target: view direction (column 0) and up (column 1).
					const auto& m = data.shadowmapDescriptors[0].camera->local.rotate.entry;
					const float m00 = m[0][0], m10 = m[1][0], m20 = m[2][0];
					const float m01 = m[0][1], m11 = m[1][1], m21 = m[2][1];
					auto& descriptor = data.focusShadowmapDescriptors[i];
					if (!descriptor.camera) {
						void* memory = RE::MemoryManager::GetSingleton()->Allocate(kCameraSize, 0, false);
						descriptor.camera.reset(memory ? MakeCamera()(memory) : nullptr);
					}
					auto* camera = descriptor.camera.get();
					const auto* target = *reinterpret_cast<const RE::NiAVObject* const*>(FocusTargets() + static_cast<std::size_t>(i) * 16 + 8);
					if (!camera || !target)
						continue;
					const float radius = At<float>(target, kWorldBoundRadius);
					const float* boundCentre = &At<float>(target, kWorldBoundCentre);
					camera->local.translate.x = boundCentre[0] - m00 * radius * 1.1f;
					camera->local.translate.y = boundCentre[1] - m10 * radius * 1.1f;
					camera->local.translate.z = boundCentre[2] - m20 * radius * 1.1f;
					// The third axis is the cross product of the first two.
					const float c0 = m21 * m10 - m11 * m20;
					const float c1 = m20 * m01 - m00 * m21;
					const float c2 = m11 * m00 - m01 * m10;
					auto& r = camera->local.rotate.entry;
					r[0][0] = m00;
					r[0][1] = m01;
					r[0][2] = c0;
					r[1][0] = m10;
					r[1][1] = m11;
					r[1][2] = c1;
					r[2][0] = m20;
					r[2][1] = m21;
					r[2][2] = c2;
					// An orthographic box around the target's bound.
					RE::NiFrustum frustum = CameraData(*camera).viewFrustum;
					frustum.bOrtho = true;
					frustum.fNear = radius * 0.1f;
					frustum.fFar = radius * 2.2f;
					frustum.fLeft = -radius;
					frustum.fRight = radius;
					frustum.fBottom = -radius;
					frustum.fTop = radius;
					SetViewFrustum(*camera, frustum);
					// The port shrinks with the target's distance from the world camera: 50 * 2^t texels, t falling from 4 to 0 over the falloff distance.
					const auto& targetPosition = target->world.translate;
					const float dx = targetPosition.x - a_worldCamera.world.translate.x;
					const float dy = targetPosition.y - a_worldCamera.world.translate.y;
					const float dz = targetPosition.z - a_worldCamera.world.translate.z;
					const float falloff = FocusDistance();
					float t = (falloff * 4.0f - std::sqrt((dy * dy + dx * dx) + dz * dz)) / falloff;
					if (!(t >= 0.0f))
						t = 0.0f;
					const float texels = std::pow(2.0f, t) * 50.0f;
					const std::uint32_t mapSize = FocusMapSize();
					std::uint32_t size = static_cast<std::uint32_t>(static_cast<std::int64_t>(texels));
					if (mapSize < size)
						size = mapSize;
					At<std::uint32_t>(&descriptor, kDescriptorPortLeft) = 0;
					At<std::uint32_t>(&descriptor, kDescriptorPortRight) = size;
					At<std::uint32_t>(&descriptor, kDescriptorPortBottom) = 0;
					At<std::uint32_t>(&descriptor, kDescriptorPortTop) = size;
					float fraction = texels / static_cast<float>(mapSize);
					if (fraction > 1.0f)
						fraction = 1.0f;
					const float port[4] = { 0.0f, fraction, fraction, 0.0f };  // left, right, top, bottom
					std::memcpy(&At<float>(camera, kCameraPort), port, sizeof(port));
					UpdateNode(*camera);
				}
			}

			void QueueLensFlareImpl(RE::BSShadowLight& a_light)
			{
				if (!LensFlaresOn())
					return;
				// BSTArray<NiPointer<BSLight>>: the heap allocator (data, capacity) then the size. AddUninitialized grows it through a
				// BSTArrayAllocatorFunctor<BSTArrayHeapAllocator> on the stack.
				std::byte* list = LensFlareList();
				struct Functor
				{
					std::uintptr_t vtable;
					void* allocator;
				} functor{ HeapAllocatorFunctorVtable(), list };
				const std::uint32_t index = ArrayAddUninitialized()(list + 0x10, &functor, At<std::uint32_t>(list, 0x8), 8);
				if (index == ~0u)
					return;
				At<RE::BSShadowLight**>(list, 0)[index] = &a_light;
				a_light.IncRefCount();
			}

			void ProjectSphereImpl(const RE::NiCamera& a_camera, const float (&a_sphere)[4], float (&a_min)[3], float (&a_max)[3], float a_epsilon)
			{
				const auto& data = CameraData(a_camera);
				const auto& f = data.viewFrustum;
				const auto& r = a_camera.world.rotate.entry;
				const auto& eye = a_camera.world.translate;
				const auto& m = const_cast<RE::NiCamera&>(a_camera).GetRuntimeData().worldToCam;
				const float px = a_sphere[0], py = a_sphere[1], pz = a_sphere[2], radius = a_sphere[3];
				// Depth: the distance along the view direction past the near plane, over the depth range.
				const float inverseRange = 1.0f / (f.fFar - f.fNear);
				const float depth = (((py - eye.y) * r[1][0] + (px - eye.x) * r[0][0]) + (pz - eye.z) * r[2][0]) - f.fNear;
				if (!f.bOrtho) {
					a_min[2] = (depth - radius) * inverseRange;
					a_max[2] = (depth + radius) * inverseRange;
				} else {
					a_min[2] = 0.0f;
					a_max[2] = 0.0f;
				}
				// x from the points a radius along the right axis (column 2), y along the up axis (column 1). A point behind the epsilon gives
				// the whole range.
				const auto w = [&](float a_x, float a_y, float a_z) { return ((a_y * m[3][1] + a_x * m[3][0]) + a_z * m[3][2]) + m[3][3]; };
				const auto row = [&](std::uint32_t a_row, float a_x, float a_y, float a_z) {
					return ((a_y * m[a_row][1] + a_x * m[a_row][0]) + a_z * m[a_row][2]) + m[a_row][3];
				};
				bool projected = false;
				do {
					const float rx = radius * r[0][2], ry = radius * r[1][2], rz = radius * r[2][2];
					float ax = px - rx, ay = py - ry, az = pz - rz;
					float wa = w(ax, ay, az);
					if (wa <= a_epsilon)
						break;
					a_min[0] = row(0, ax, ay, az) / wa;
					ax = px + rx;
					ay = py + ry;
					az = pz + rz;
					wa = w(ax, ay, az);
					if (wa <= a_epsilon)
						break;
					a_max[0] = row(0, ax, ay, az) / wa;
					const float ux = radius * r[0][1], uy = radius * r[1][1], uz = radius * r[2][1];
					ax = px - ux;
					ay = py - uy;
					az = pz - uz;
					wa = w(ax, ay, az);
					if (wa <= a_epsilon)
						break;
					a_min[1] = row(1, ax, ay, az) / wa;
					ax = px + ux;
					ay = py + uy;
					az = pz + uz;
					wa = w(ax, ay, az);
					if (wa <= a_epsilon)
						break;
					a_max[1] = row(1, ax, ay, az) / wa;
					projected = true;
				} while (false);
				if (projected) {
					ClampAxis(a_min[0], a_max[0], -1.0f);
					ClampAxis(a_min[1], a_max[1], -1.0f);
				} else {
					a_min[0] = -1.0f;
					a_min[1] = -1.0f;
					a_max[0] = 1.0f;
					a_max[1] = 1.0f;
				}
				ClampAxis(a_min[2], a_max[2], 0.0f);
			}

			void ProjectLightRectImpl(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_shadowLightCount)
			{
				const auto* niLight = a_light.light.get();
				if (!niLight)
					return;
				const auto& position = niLight->world.translate;
				const float radius = LightRadius(*niLight);
				const float dx = position.x - a_worldCamera.world.translate.x;
				const float dy = position.y - a_worldCamera.world.translate.y;
				const float dz = position.z - a_worldCamera.world.translate.z;
				const float distance = std::sqrt((dx * dx + dy * dy) + dz * dz);
				// z of the bounds is never read here, as in the engine.
				float boundsMin[3]{}, boundsMax[3]{};
				if (distance >= radius + CameraData(a_worldCamera).viewFrustum.fNear) {
					// The sphere moved toward the camera by its radius, so its silhouette projects whole.
					const float inverse = 1.0f / distance;
					const float sphere[4] = { position.x - dx * radius * inverse, position.y - dy * radius * inverse, position.z - dz * radius * inverse, radius };
					ProjectSphereImpl(a_worldCamera, sphere, boundsMin, boundsMax, 1e-5f);
				} else {
					// The camera is inside the sphere: the whole screen.
					CameraInsideMask() |= 1u << (a_shadowLightCount & 0x1F);
					boundsMin[0] = boundsMin[1] = -1.0f;
					boundsMax[0] = boundsMax[1] = 1.0f;
				}
				const float height = static_cast<float>(ScreenHeight());
				const float width = static_cast<float>(ScreenWidth());
				// y is flipped into screen rows. SE and AE write the same rows with different arithmetic.
				std::int32_t bottom, top;
				if (REL::Module::IsAE()) {
					bottom = static_cast<std::int32_t>((0.5f - boundsMax[1] * 0.5f) * height);
					top = static_cast<std::int32_t>((0.5f - boundsMin[1] * 0.5f) * height);
				} else {
					bottom = static_cast<std::int32_t>((1.0f - (boundsMax[1] + 1.0f) * 0.5f) * height);
					top = static_cast<std::int32_t>((1.0f - (boundsMin[1] + 1.0f) * 0.5f) * height);
				}
				At<std::int32_t>(&a_light, kLightRectBottom) = bottom;
				At<std::int32_t>(&a_light, kLightRectTop) = top;
				At<std::int32_t>(&a_light, kLightRectLeft) = static_cast<std::int32_t>((boundsMin[0] + 1.0f) * 0.5f * width);
				At<std::int32_t>(&a_light, kLightRectRight) = static_cast<std::int32_t>((boundsMax[0] + 1.0f) * 0.5f * width);
			}

			bool IsFrustumOrDirectionalImpl(const RE::BSShadowLight& a_light)
			{
				const auto kind = KindOf(a_light);
				return kind == Kind::Frustum || kind == Kind::Directional;
			}

			void ReturnShadowmapsImpl(RE::BSShadowLight& a_light)
			{
				auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
				const std::uint32_t available = static_cast<std::uint32_t>(descriptors.size());
				const std::uint32_t count = a_light.shadowMapCount < available ? a_light.shadowMapCount : available;
				const auto kind = KindOf(a_light);
				// The paraboloid's back hemisphere shares the front's slice, and the cascades after the first own no slice from the free mask:
				// both only drop their target before BSShadowLight's.
				if (kind == Kind::Parabolic && a_light.shadowMapCount == 2 && descriptors.size() >= 2)
					descriptors[1].renderTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::kNONE;
				if (kind == Kind::Directional)
					for (std::uint32_t i = 1; i < count; ++i) {
						descriptors[i].renderTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::kNONE;
						descriptors[i].unitsPerTexel = kUnitsPerTexelOne;
					}
				for (std::uint32_t i = 0; i < count; ++i) {
					auto& descriptor = descriptors[i];
					if (descriptor.renderTarget == RE::RENDER_TARGETS_DEPTHSTENCIL::kNONE)
						continue;
					ShadowFreeMask() |= 1u << (descriptor.shadowmapIndex & 0x1F);
					descriptor.renderTarget = RE::RENDER_TARGETS_DEPTHSTENCIL::kNONE;
					descriptor.unitsPerTexel = kUnitsPerTexelOne;
				}
			}

			void ReleaseLightImpl(RE::BSShadowLight& a_light)
			{
				// The engine reads the process and its entry without a check.
				if (auto* process = a_light.cullingProcess)
					if (void* entry = At<void*>(process, kProcessPortalEntry))
						ClearEntryVisibility()(entry);
				ReturnShadowmapsImpl(a_light);
			}

			/** @brief BSShadowDirectionalLight::Accumulate (AE 0x141511c80) without its cascade culls: each cascade's index and bit, one count each. */
			void SunAccumulateBookkeeping(RE::BSShadowLight& a_light, std::uint32_t& a_globalCount)
			{
				auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
				const std::uint32_t maps = std::min<std::uint32_t>(a_light.shadowMapCount, descriptors.size());
				for (std::uint32_t i = 0; i < maps; ++i) {
					const std::uint32_t count = a_globalCount;
					if (auto* accumulator = descriptors[i].shaderAccumulator.get()) {
						At<std::uint32_t>(accumulator, kAccumulatorLightIndex) = count + 1;
						At<std::uint32_t>(accumulator, kAccumulatorLightBit) = count < 0x1F ? 1u << count : 0u;
						At<std::uint32_t>(accumulator, kAccumulatorCascade) = i;
						At<bool>(accumulator, kAccumulatorShadowFlag) = true;
					}
					a_globalCount = count + 1;
				}
			}

			void PrepareFocusAccumulatorsImpl(RE::BSShadowLight& a_light)
			{
				auto& focus = a_light.GetRuntimeData().focusShadowmapDescriptors;
				const std::uint32_t targets = std::min<std::uint32_t>(FocusCount(), static_cast<std::uint32_t>(std::size(focus)));
				for (std::uint32_t i = 0; i < targets; ++i) {
					auto& descriptor = focus[i];
					descriptor.isEnabled = true;
					if (!descriptor.shaderAccumulator) {
						const MemoryContext context(kAccumulatorContext);
						void* memory = RE::MemoryManager::GetSingleton()->Allocate(kAccumulatorSize, 0, false);
						descriptor.shaderAccumulator.reset(memory ? MakeAccumulator()(memory, kAccumulatorCtorArg) : nullptr);
						if (auto* made = descriptor.shaderAccumulator.get()) {
							At<std::uint32_t>(made, kAccumulatorRenderMode) = kFocusRenderMode;
							At<bool>(made, kAccumulatorDrawDecals) = false;
							At<bool>(made, kAccumulatorUnk12D) = true;
						}
					}
					if (auto* accumulator = descriptor.shaderAccumulator.get())
						At<RE::ShadowSceneNode*>(accumulator, kAccumulatorSceneNode) = SceneNodes()[0];
				}
			}

			void AccumulateBookkeepingImpl(RE::BSShadowLight& a_light, std::uint32_t& a_globalCount, [[maybe_unused]] std::uint32_t a_maskChannel)
			{
				const auto kind = KindOf(a_light);
				if (kind == Kind::Directional) {
					SunAccumulateBookkeeping(a_light, a_globalCount);
					return;
				}
				if (kind != Kind::Frustum && kind != Kind::Parabolic)
					return;
				auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
				const std::uint32_t count = a_globalCount;
				if (auto* accumulator = descriptors.empty() ? nullptr : descriptors[0].shaderAccumulator.get()) {
					At<std::uint32_t>(accumulator, kAccumulatorLightIndex) = count + 1;
					At<std::uint32_t>(accumulator, kAccumulatorLightBit) = count < 0x1F ? 1u << count : 0u;
				}
				if (kind == Kind::Frustum) {
					a_globalCount = count + 1;
					return;
				}
				// A two-sided paraboloid's process culls the back hemisphere into the second accumulator (AE FUN_141519e10, SE FUN_14132c650).
				const bool twoSided = a_light.shadowMapCount == 2;
				if (twoSided && descriptors.size() >= 2)
					if (auto* process = a_light.cullingProcess)
						At<RE::NiPointer<RE::BSShaderAccumulator>>(process, kProcessBackAccumulator) = descriptors[1].shaderAccumulator;
				a_globalCount = count + (twoSided ? 2 : 1);
			}
		}
	}

	bool UpdateFrustumCamera(RE::BSShadowFrustumLight& a_light, const RE::NiCamera& a_viewCamera)
	{
		return LocalCamerasImpl::UpdateFrustumCameraImpl(a_light, a_viewCamera);
	}

	bool UpdateParabolicCamera(RE::BSShadowParabolicLight& a_light, const RE::NiCamera& a_viewCamera)
	{
		return LocalCamerasImpl::UpdateParabolicCameraImpl(a_light, a_viewCamera);
	}

	void UpdateFocusCameras(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera)
	{
		LocalCamerasImpl::UpdateFocusCamerasImpl(a_light, a_worldCamera);
	}

	void QueueLensFlare(RE::BSShadowLight& a_light)
	{
		LocalCamerasImpl::QueueLensFlareImpl(a_light);
	}

	void ProjectSphere(const RE::NiCamera& a_camera, const float (&a_sphere)[4], float (&a_min)[3], float (&a_max)[3], float a_epsilon)
	{
		LocalCamerasImpl::ProjectSphereImpl(a_camera, a_sphere, a_min, a_max, a_epsilon);
	}

	void ProjectLightRect(RE::BSShadowLight& a_light, const RE::NiCamera& a_worldCamera, std::uint32_t a_shadowLightCount)
	{
		LocalCamerasImpl::ProjectLightRectImpl(a_light, a_worldCamera, a_shadowLightCount);
	}

	bool IsFrustumOrDirectional(const RE::BSShadowLight& a_light)
	{
		return LocalCamerasImpl::IsFrustumOrDirectionalImpl(a_light);
	}

	void ReturnShadowmaps(RE::BSShadowLight& a_light)
	{
		LocalCamerasImpl::ReturnShadowmapsImpl(a_light);
	}

	void ReleaseLight(RE::BSShadowLight& a_light)
	{
		LocalCamerasImpl::ReleaseLightImpl(a_light);
	}

	void AccumulateBookkeeping(RE::BSShadowLight& a_light, std::uint32_t& a_globalCount, std::uint32_t a_maskChannel)
	{
		LocalCamerasImpl::AccumulateBookkeepingImpl(a_light, a_globalCount, a_maskChannel);
	}

	void PrepareFocusAccumulators(RE::BSShadowLight& a_light)
	{
		LocalCamerasImpl::PrepareFocusAccumulatorsImpl(a_light);
	}
}
