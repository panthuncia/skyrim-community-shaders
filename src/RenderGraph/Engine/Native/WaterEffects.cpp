// Native's SetupSunAndSky and RenderWaterEffects: the game halves on the game thread, the water draws at CS's point. See WaterEffects.h.

#include "RenderGraph/Engine/Native/WaterEffects.h"

#include "EngineFixes/WaterReflectionCubemapFix.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/Publication.h"
#include "Snapshot/Retire.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace RenderGraph::Engine::Native::WaterEffects
{
	static_assert(RE::RENDER_TARGET::kWATER_DISPLACEMENT == 8 && RE::RENDER_TARGET::kWATER_DISPLACEMENT_SWAP == 9,
		"RenderDisplacement resets the displacement pair to targets 8 and 9");

	namespace
	{
		template <class T>
		T& Global(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
		}

		template <class T>
		T& Field(void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(static_cast<std::byte*>(a_base) + a_offset);
		}

		/** @brief Fatal unless a_site holds a_bytes: the patches below assume the engine's own code. */
		void RequireBytes(const char* a_what, std::uintptr_t a_site, std::initializer_list<std::uint8_t> a_bytes)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::size_t i = 0;
			for (const auto expected : a_bytes)
				if (bytes[i++] != expected)
					RenderGraph::Fatal("native water effects", fmt::format("{} at {:#x} is not the engine's code; another plugin may have patched it", a_what, a_site));
		}

		/** @brief Fatal unless a_site is a CALL rel32 to a_target. */
		void RequireCall(const char* a_what, std::uintptr_t a_site, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::int32_t relative = 0;
			std::memcpy(&relative, bytes + 1, sizeof(relative));
			if (bytes[0] != 0xE8 || a_site + 5 + relative != a_target)
				RenderGraph::Fatal("native water effects", fmt::format("{} at {:#x} is not the engine's call; another plugin may have patched it", a_what, a_site));
		}

		// Engine functions and data (AE 1.6.1170, SE 1.5.97).
		REL::RelocationID SetupSunAndSkyId() { return REL::RelocationID(35562, 36561); }
		REL::RelocationID WaterUpdateId() { return REL::RelocationID(31383, 32174); }
		REL::RelocationID ReflectionUpdateId() { return REL::RelocationID(31373, 32160); }

		bool installed = false;
		std::uint64_t sunAndSky = 0;

		// Game thread: the update being captured, and the frustum its water test uses.
		Work* building = nullptr;
		DCLF::ShadowCameras::Planes planes;
		bool havePlanes = false;

		struct Queue
		{
			std::mutex mutex;
			std::deque<Work> items;
		};

		Queue& Pending()
		{
			static Queue queue;
			return queue;
		}

		/** @brief The most updates the queue holds: past it, the oldest folds into the next, keeping its flow copies and clears. */
		constexpr std::size_t kMaxPending = 64;

		void Push(Work&& a_work)
		{
			std::scoped_lock lock(Pending().mutex);
			auto& items = Pending().items;
			items.push_back(std::move(a_work));
			if (items.size() <= kMaxPending)
				return;
			auto oldest = std::move(items.front());
			items.pop_front();
			auto& next = items.front();
			next.copies.insert(next.copies.begin(), std::make_move_iterator(oldest.copies.begin()), std::make_move_iterator(oldest.copies.end()));
			next.clearCube |= oldest.clearCube;
			next.displacement.clear |= oldest.displacement.clear;
		}

		struct Stats
		{
			std::atomic<std::uint64_t> updates = 0, early = 0, faces = 0, steps = 0, ripples = 0, copies = 0, drawnFaces = 0, rendered = 0;
		};

		Stats& Counts()
		{
			static Stats stats;
			return stats;
		}

		/**
		* @brief The water test inside TESWaterSystem::Update (SE 31398, AE 32189), without the engine's cull: a water object is visible
		* when its shape is not hidden and its bound is inside the snapshot camera's frustum. In an exterior the engine only takes shapes
		* under a BSMultiBoundNode and tests that node; so does this. The portal, multibound-shape and occluder tests are not repeated
		* (decision 37's trade): a water object the engine's cull hid now counts as visible, never the reverse.
		*/
		struct WaterVisible
		{
			static bool thunk([[maybe_unused]] RE::TESWaterSystem* a_system, void* a_object, [[maybe_unused]] void* a_eye, [[maybe_unused]] void* a_culler)
			{
				auto* shape = Field<RE::NiAVObject*>(a_object, 0x20);
				if (!shape || shape->GetFlags().any(RE::NiAVObject::Flag::kHidden) || !havePlanes)
					return false;
				auto* tes = RE::TES::GetSingleton();
				const RE::NiAVObject* tested = shape;
				if (tes && !tes->interiorCell) {
					auto* parent = shape->parent;
					if (!parent || !netimmerse_cast<RE::BSMultiBoundNode*>(parent))
						return false;
					tested = parent;
				}
				const auto& bound = tested->worldBound;
				for (std::uint32_t p = 0; p < 6; ++p) {
					if (!((planes.mask >> p) & 1))
						continue;
					const auto& plane = planes.plane[p];
					if (plane[0] * bound.center.x + plane[1] * bound.center.y + plane[2] * bound.center.z - plane[3] < -bound.radius)
						return false;
				}
				return true;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The face render TESWaterReflections::Update calls (BSCubeMapCamera vfunc 0x35): recorded, not drawn. */
		void RecordFace(RE::NiCamera* a_camera, std::uint32_t a_faces, bool a_silhouettes, bool a_clearRoots, bool a_noSky)
		{
			if (!building)
				RenderGraph::Fatal("native water effects", "TESWaterReflections::Update ran outside the capture's water update");
			if (a_camera) {
				auto& data = a_camera->GetRuntimeData2();
				for (std::uint32_t f = 0; f < 6; ++f) {
					if (!((a_faces >> f) & 1))
						continue;
					Face face{ .face = f, .translate = a_camera->local.translate, .scale = a_camera->local.scale, .frustum = data.viewFrustum, .plain = !a_silhouettes && !a_noSky };
					std::memcpy(face.port.data(), &data.port, sizeof(face.port));
					building->faces.push_back(face);
					Counts().faces.fetch_add(1, std::memory_order_relaxed);
				}
				// The face render releases the cube camera's roots after an update's last face.
				if (a_clearRoots)
					DCLF::Features::Reflection::ReflectionFaces::ReleaseRoots(a_camera);
			}
		}

		// TESWaterSystem::RenderDisplacement and ImageSpaceEffectWaterDisplacement's Render (SE 0x141303da0, AE 0x1414ef370).

		/** @brief The ripple queue arrays: current and previous positions (TESWaterSystem::AddRipple appends; the effect drains). */
		using RippleArray = RE::BSTArray<std::array<float, 2>>;

		/** @brief The effect's quad for the ripples (+0x90), made as the effect makes it on its first ripple: 2 triangles, 4 vertices. */
		const void* RippleGeometry(void* a_effect)
		{
			auto& geometry = Field<RE::NiPointer<RE::BSTriShape>>(a_effect, 0x90);
			if (geometry)
				return geometry.get();
			using Create = RE::BSTriShape* (*)(std::uint32_t, const std::uint16_t*, std::uint32_t, const float*, const float*, void*, void*, void*, void*, void*, void*,
				void*, void*, void*, void*);
			static REL::Relocation<Create> create{ REL::RelocationID(75659, 77465) };
			constexpr std::uint16_t kIndices[6]{ 0, 1, 2, 0, 2, 3 };
			// Four positions (three floats each) and four texture coordinates, from the engine's constants.
			float positions[12]{}, coordinates[8]{};
			std::memcpy(positions, reinterpret_cast<const void*>(REL::RelocationID(305504, 255239).address()), 16);
			std::memcpy(positions + 4, reinterpret_cast<const void*>(REL::RelocationID(232867, 196212).address()), 16);
			std::memcpy(positions + 8, reinterpret_cast<const void*>(REL::RelocationID(305505, 255240).address()), 16);
			std::memcpy(coordinates, reinterpret_cast<const void*>(REL::RelocationID(229662, 186918).address()), 16);
			std::memcpy(coordinates + 4, reinterpret_cast<const void*>(REL::RelocationID(234794, 190027).address()), 16);
			auto* made = create(2, kIndices, 4, positions, coordinates, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
			if (!made)
				RenderGraph::Fatal("native water effects", "the displacement effect's ripple quad could not be made");
			geometry.reset(made);
			made->name = "Partial Screen Trishape";
			return made;
		}

		/** @brief The sub-effect a_index's param pixel constants (+0x40), a_count float4s, as the engine left them. */
		void ReadPixelIds(void* a_effect, std::uint32_t a_index, DisplacementStep& a_step)
		{
			auto* params = Field<void**>(a_effect, 0x30);
			auto* param = params ? static_cast<RE::ImageSpaceShaderParam*>(params[a_index]) : nullptr;
			if (!param || !param->pixelConstantGroup)
				RenderGraph::Fatal("native water effects", fmt::format("the displacement effect's sub-effect {} has no pixel constants", a_index));
			const auto count = param->pixelConstantGroupSize;
			if (count > a_step.ids.size())
				RenderGraph::Fatal("native water effects", fmt::format("the displacement effect's sub-effect {} has {} pixel constants, more than a step holds", a_index, count));
			a_step.count = count;
			for (std::uint32_t id = 0; id < count; ++id)
				std::memcpy(a_step.ids[id].data(), param->pixelConstantGroup + id * 4u, 16);
		}

		/** @brief Sets a_id to (x, y, z, w), as the param setter (SE 0x1412d66f0, AE 0x1414bed50) does. */
		void SetId(DisplacementStep& a_step, std::uint32_t a_id, float a_x, float a_y = 0.0f, float a_z = 0.0f, float a_w = 0.0f)
		{
			if (a_id >= a_step.count)
				RenderGraph::Fatal("native water effects", fmt::format("a displacement step sets pixel constant {} of {}", a_id, a_step.count));
			a_step.ids[a_id] = { a_x, a_y, a_z, a_w };
		}

		/** @brief The texture offset and heightmap steps' ids: the clock, the blend, the mesh offset, the rain and wading values. */
		DisplacementStep QuadStep(void* a_effect, DisplacementStep::Kind a_kind, std::uint32_t a_index, float a_clock)
		{
			DisplacementStep step{ .kind = a_kind };
			ReadPixelIds(a_effect, a_index, step);
			const auto& blend = Global<float>(528281, 415231);
			const auto* offset = &Global<float>(528288, 415238);
			const auto* rain = &Global<float>(528304, 415254);
			const auto* wading = &Global<float>(528308, 415258);
			SetId(step, 0, a_clock);
			SetId(step, 1, blend);
			SetId(step, 2, offset[0], offset[1]);
			SetId(step, 4, rain[0], rain[1], rain[2], rain[3]);
			SetId(step, 5, wading[0], wading[1], wading[2], wading[3]);
			return step;
		}

		/**
		* @brief The effect's Render without its draws: the texture offset step, a ripple per queued ripple (the queue is drained), then a
		* heightmap step per 0.025 s of its clock. The displacement pair swaps after the ripples and after each heightmap step.
		*/
		void PlanEffect(Displacement& a_out)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			auto* effect = manager && manager->effects.size() > 0x1f ? static_cast<void*>(manager->effects[0x1f]) : nullptr;
			if (!effect)
				RenderGraph::Fatal("native water effects", "the ImageSpace manager has no water displacement effect");
			auto& current = Global<std::int32_t>(528285, 415235);
			auto& other = Global<std::int32_t>(528286, 415236);
			auto& rendering = Global<std::uint8_t>(528282, 415232);
			auto& stepped = Global<std::uint8_t>(528283, 415233);
			auto& rippleCount = Global<std::int32_t>(528284, 415234);
			const float delta = Global<float>(513214, 390954);
			auto& clock = Field<float>(effect, 0x98);
			auto& done = Field<float>(effect, 0x9c);
			auto& sum = Field<float>(effect, 0xa4);
			auto& step = Field<float>(effect, 0xa8);
			step = 0.025f;
			clock += delta;
			sum += clock;
			// The effect's two textures: the first is its output, the second its input.
			std::int32_t first = current, second = other;
			a_out.target = second;
			if (rendering) {
				a_out.steps.push_back(QuadStep(effect, DisplacementStep::Kind::TexOffset, 1, clock));
				rippleCount = 0;
				auto& positions = Global<RippleArray>(528290, 415240);
				auto& previous = Global<RippleArray>(528293, 415243);
				if (!positions.empty()) {
					a_out.rippleGeometry = RippleGeometry(effect);
					const float epsilon = Global<float>(229955, 187051);
					const float c64 = Global<float>(528299, 415249), c74 = Global<float>(528303, 415253);
					for (std::uint32_t i = 0; i < positions.size(); ++i) {
						const auto& a = positions[i];
						const auto& b = previous[i];
						const float dx = a[0] - b[0], dy = a[1] - b[1];
						float nx = b[1] - a[1], ny = 0.0f;
						const float length = std::sqrt(nx * nx + dx * dx);
						if (length <= epsilon) {
							nx = 0.0f;
						} else {
							nx /= length;
							ny = dx / length;
						}
						DisplacementStep ripple{ .kind = DisplacementStep::Kind::Ripple, .count = 2 };
						ripple.ids[0] = { dx, nx, b[0], c64 };
						ripple.ids[1] = { dy, ny, b[1], c74 };
						a_out.steps.push_back(ripple);
						++rippleCount;
						Counts().ripples.fetch_add(1, std::memory_order_relaxed);
					}
					positions.clear();
					previous.clear();
				}
			}
			stepped = 0;
			std::swap(first, second);
			while (step < clock - done) {
				done += step;
				if (rendering) {
					a_out.steps.push_back(QuadStep(effect, DisplacementStep::Kind::Heightmap, 4, clock));
					stepped = 1;
					std::swap(first, second);
				}
			}
			current = first;
			other = second;
		}

		/** @brief TESWaterSystem::RenderDisplacement (SE 31385, AE 32176) without its draws, in place of its call in the update. */
		struct RenderDisplacement
		{
			static void thunk(RE::TESWaterSystem* a_system)
			{
				if (!building)
					RenderGraph::Fatal("native water effects", "TESWaterSystem::RenderDisplacement ran outside the capture's water update");
				auto& idle = a_system->timeSinceLastRipplePlaced;
				auto& restart = a_system->unk0C8;
				auto& stepped = Global<std::uint8_t>(528283, 415233);
				if (Global<float>(232794, 470027) <= idle) {
					stepped = 0;
					restart = 1;
					return;
				}
				auto& out = building->displacement;
				if (restart) {
					// The engine clears to its constant in every channel; CS draws ClearSimulation, whose shader writes 0.5.
					if (Global<float>(230690, 187405) != 0.5f)
						RenderGraph::Fatal("native water effects", "the displacement clear colour is not ClearSimulation's 0.5");
					out.clear = true;
					Global<std::int32_t>(528285, 415235) = RE::RENDER_TARGET::kWATER_DISPLACEMENT;
					Global<std::int32_t>(528286, 415236) = RE::RENDER_TARGET::kWATER_DISPLACEMENT_SWAP;
					restart = 0;
				}
				auto& rendering = Global<std::uint8_t>(528282, 415232);
				rendering = 1;
				std::memcpy(&Global<float>(528308, 415258), reinterpret_cast<const void*>(REL::RelocationID(255829, 204903).address()), 16);
				// The wading actors' ripples join the queue: each one's current and previous position.
				auto& positions = Global<RippleArray>(528290, 415240);
				auto& previous = Global<RippleArray>(528293, 415243);
				for (const auto& item : a_system->wadingWaterData) {
					const auto* data = reinterpret_cast<const std::array<float, 2>*>(item.second);
					if (!data)
						continue;
					positions.push_back(data[0]);
					previous.push_back(data[1]);
				}
				if (positions.empty())
					idle += Global<float>(513214, 390954);
				else
					idle = 0.0f;
				PlanEffect(out);
				Counts().steps.fetch_add(out.steps.size(), std::memory_order_relaxed);
				rendering = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Main::RenderWaterEffects' flow-map ring (S16): BGSAutoWater's pending ring (+0x1008) and free pool (+0x1818).

		/** @brief The ring's pop, as RenderWaterEffects inlines it: slots at +0, read index +0x800, tail +0x804, write index +0x808. */
		void* PopRing(std::byte* a_ring)
		{
			auto slot = [&](std::uint32_t a_index) -> void*& { return *reinterpret_cast<void**>(a_ring + (a_index & 0xff) * 8); };
			std::atomic_ref<std::uint32_t> read(*reinterpret_cast<std::uint32_t*>(a_ring + 0x800));
			std::atomic_ref<std::uint32_t> tail(*reinterpret_cast<std::uint32_t*>(a_ring + 0x804));
			std::atomic_ref<std::uint32_t> write(*reinterpret_cast<std::uint32_t*>(a_ring + 0x808));
			for (auto r = read.load(); r != write.load(); r = read.load()) {
				auto expected = r;
				if (!read.compare_exchange_strong(expected, r + 1))
					continue;
				void* entry = std::exchange(slot(r), nullptr);
				// The tail follows over emptied slots, as far as the read index.
				for (auto t = tail.load(); slot(t) == nullptr && t != read.load();) {
					auto seen = t;
					if (!tail.compare_exchange_strong(seen, t + 1))
						break;
					t = tail.load();
				}
				return entry;
			}
			return nullptr;
		}

		/** @brief A pending entry's loaded flow texture holder (+0): loaded when its state (+0xC) is 3 or 4. */
		bool EntryLoaded(void* a_entry)
		{
			auto* holder = Field<void*>(a_entry, 0);
			if (!holder)
				return false;
			const auto state = Field<std::uint32_t>(holder, 0xC);
			return (((state & 0x70000000u) + 0xd0000000u) & 0xefffffffu) == 0;
		}

		/** @brief Whether the engine's flow copies are still in RenderWaterEffects (Unified Water removes them). */
		bool EngineFlowCopies()
		{
			const auto site = REL::RelocationID(35561, 36560).address() + REL::Relocate(0x202, 0x242);
			return *reinterpret_cast<const std::uint8_t*>(site) == 0xE8;
		}

		/** @brief Drains the pending ring: a loaded entry's copy is queued and the entry returns to the pool, an unloaded one waits. */
		void DrainFlowRing(Work& a_work)
		{
			auto* water = *reinterpret_cast<std::byte**>(REL::RelocationID(514289, 400449).address());
			if (!water)
				return;
			using Push = std::uint32_t (*)(void*, void*);
			static REL::Relocation<Push> push{ REL::RelocationID(31245, 32047) };
			static const bool copies = EngineFlowCopies();
			auto* ring = water + 0x1008;
			const auto readIndex = *reinterpret_cast<const std::uint32_t*>(ring + 0x800) & 0xff;
			const auto writeIndex = *reinterpret_cast<const std::uint32_t*>(ring + 0x808) & 0xff;
			const auto count = writeIndex >= readIndex ? writeIndex - readIndex : writeIndex - readIndex + 0x100;
			auto* flowMap = *reinterpret_cast<RE::NiSourceTexture**>(REL::RelocationID(527694, 414616).address());
			for (std::uint32_t i = 0; i < count; ++i) {
				void* entry = PopRing(ring);
				if (!entry)
					break;
				if (!EntryLoaded(entry)) {
					push(ring, entry);
					continue;
				}
				auto* texture = Field<RE::NiSourceTexture*>(Field<void*>(entry, 0), 0x28);
				if (copies && texture && flowMap) {
					auto* renderer = reinterpret_cast<std::byte*>(texture->rendererTexture);
					auto* view = renderer ? *reinterpret_cast<ID3D11ShaderResourceView**>(renderer + 0x10) : nullptr;
					if (view) {
						FlowCopy copy;
						copy.source.copy_from(view);
						copy.region = { Field<std::uint32_t>(entry, 0x14), Field<std::uint32_t>(entry, 0x8), Field<std::uint32_t>(entry, 0xc), Field<std::uint32_t>(entry, 0x10),
							Field<std::uint32_t>(entry, 0x18), Field<std::uint32_t>(entry, 0x1c), 0, 0 };
						a_work.copies.push_back(std::move(copy));
						Counts().copies.fetch_add(1, std::memory_order_relaxed);
					}
				}
				push(water + 0x1818, entry);
			}
		}

		/** @brief Main::Draw's call of Main::RenderWaterEffects (SE 35560, AE 36559 +0x172) and its argument (+0x16F) are jumped over. */
		void SkipEngineCall()
		{
			const auto at = REL::RelocationID(35560, 36559).address() + 0x16F;
			RequireBytes("Main::Draw's call of Main::RenderWaterEffects", at, { 0x49, 0x8B, 0xCC, 0xE8 });
			RequireCall("Main::Draw's call of Main::RenderWaterEffects", at + 3, REL::RelocationID(35561, 36560).address());
			constexpr std::uint8_t kJump[2]{ 0xEB, 0x06 };
			REL::safe_write(at, kJump, sizeof(kJump));
		}

		/** @brief TESWaterReflections::Update (SE 31373, AE 32160) records its faces and leaves the renderer's clear colour alone. */
		void PatchReflectionUpdate()
		{
			const auto update = ReflectionUpdateId().address();
			const auto face = update + REL::Relocate(0x484, 0x459);
			RequireBytes("TESWaterReflections::Update's face render call", face, { 0xFF, 0x90, 0xA8, 0x01, 0x00, 0x00 });
			SKSE::GetTrampoline().write_call<6>(face, reinterpret_cast<std::uintptr_t>(&RecordFace));
			for (const auto& [offset, id] : { std::pair{ REL::Relocate(0x339, 0x32D), REL::RelocationID(75463, 77249) }, std::pair{ REL::Relocate(0x4D5, 0x493), REL::RelocationID(75464, 77250) } }) {
				RequireCall("TESWaterReflections::Update's clear colour call", update + offset, id.address());
				REL::safe_fill(update + offset, 0x90, 5);
			}
		}
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR() || !Native::Requested())
			return;
		SkipEngineCall();
		const auto update = WaterUpdateId().address();
		const auto test = update + REL::Relocate(0x592, 0x8D3);
		RequireCall("TESWaterSystem::Update's water test", test, REL::RelocationID(31398, 32189).address());
		stl::write_thunk_call<WaterVisible>(test);
		const auto displacement = update + REL::Relocate(0xA2D, 0xE04);
		RequireCall("TESWaterSystem::Update's RenderDisplacement call", displacement, REL::RelocationID(31385, 32176).address());
		stl::write_thunk_call<RenderDisplacement>(displacement);
		PatchReflectionUpdate();
		installed = true;
		logger::info(
			"[ORG native] water effects: Main::Draw does not call RenderWaterEffects or SetupSunAndSky's engine function; the capture runs their game "
			"halves (the water update with its faces recorded, RenderDisplacement ported, a frustum water test) and CS draws the water work");
	}

	bool Owns()
	{
		return installed;
	}

	void SetupSunAndSky()
	{
		if (!installed)
			return;
		using Fn = std::uint64_t (*)(RE::Main*);
		static REL::Relocation<Fn> setup{ SetupSunAndSkyId() };
		sunAndSky = setup(RE::Main::GetSingleton());
	}

	std::uint64_t SunAndSkyResult()
	{
		return sunAndSky;
	}

	void Capture(const Snapshot::FrameSnapshot& a_snapshot)
	{
		if (!installed || a_snapshot.kind != Snapshot::FrameKind::World)
			return;
		auto* system = RE::TESWaterSystem::GetSingleton();
		auto* culler = *reinterpret_cast<std::byte**>(REL::RelocationID(528002, 414947).address());
		if (!system || !culler || !Field<void*>(culler, 0x18))
			return;
		Work work;
		work.snapshot = a_snapshot.id;
		work.clearCube = WaterReflectionCubemapFix::SceneChanged();
		Counts().updates.fetch_add(1, std::memory_order_relaxed);
		// The early-out: the map menu is closed and the water is off (flag bit 11) or the interior has no water.
		if (!Global<bool>(508354, 379982)) {
			Push(std::move(work));
			return;
		}
		auto* ui = RE::UI::GetSingleton();
		auto* strings = RE::InterfaceStrings::GetSingleton();
		auto* tes = RE::TES::GetSingleton();
		const bool map = ui && strings && ui->IsMenuOpen(strings->mapMenu);
		const bool waterOff = (Global<std::uint32_t>(514793, 400942) >> 11) & 1;
		const bool dry = tes && tes->interiorCell && !tes->interiorCell->cellFlags.all(RE::TESObjectCELL::Flag::kHasWater);
		if (!map && (waterOff || dry)) {
			// S20: the engine's resets, the water shapes hidden and the underwater effects off (SE 31412 and 31408, AE 32219 and 32215).
			Global<std::int32_t>(513267, 391008) = 0;
			system->unk010 = 0;
			system->playerUnderwater = false;
			static REL::Relocation<void (*)(RE::TESWaterSystem*)> hideShapes{ REL::RelocationID(31412, 32219) };
			static REL::Relocation<void (*)(RE::TESWaterSystem*)> underwaterOff{ REL::RelocationID(31408, 32215) };
			hideShapes(system);
			underwaterOff(system);
			Counts().early.fetch_add(1, std::memory_order_relaxed);
		} else {
			// The snapshot camera's frustum for the water test.
			const auto& view = a_snapshot.cameras.Current(Snapshot::ViewId::Main);
			havePlanes = view.valid;
			if (havePlanes) {
				RE::NiTransform world;
				world.rotate = view.rotation;
				world.translate = view.position;
				world.scale = 1.0f;
				DCLF::ShadowCameras::FrustumPlanes(view.frustum, world, planes);
			}
			auto* state = reinterpret_cast<std::uint8_t*>(&RE::BSShaderManager::State::GetSingleton());
			const auto previous = std::exchange(state[0x219], std::uint8_t{ 1 });
			building = &work;
			static REL::Relocation<void (*)(RE::TESWaterSystem*, void*)> update{ WaterUpdateId() };
			update(system, culler);
			building = nullptr;
			state[0x219] = previous;
			DrainFlowRing(work);
		}
		Push(std::move(work));
	}

	void Render()
	{
		if (!installed)
			return;
		const auto& snapshot = Snapshot::Current();
		if (!snapshot)
			return;
		auto ready = std::make_shared<std::vector<Work>>();
		{
			std::scoped_lock lock(Pending().mutex);
			auto& items = Pending().items;
			while (!items.empty() && items.front().snapshot <= snapshot->id) {
				ready->push_back(std::move(items.front()));
				items.pop_front();
			}
		}
		if (ready->empty())
			return;
		Counts().rendered.fetch_add(1, std::memory_order_relaxed);
		// Several updates since the last render point (replaced snapshots): each face as the newest update placed it, the newest update's
		// displacement steps (the engine stepped none on updates it did not render either) after any restart's clear, every flow copy.
		auto merged = std::make_shared<Work>();
		std::array<const Face*, 6> newest{};
		bool clear = false;
		for (auto& work : *ready) {
			merged->clearCube |= work.clearCube;
			clear |= work.displacement.clear;
			for (const auto& face : work.faces)
				newest[face.face] = &face;
			if (!work.displacement.steps.empty())
				merged->displacement = std::move(work.displacement);
			std::move(work.copies.begin(), work.copies.end(), std::back_inserter(merged->copies));
		}
		merged->displacement.clear = clear;
		if (merged->clearCube)
			WaterReflectionCubemapFix::ClearCubemap();
		for (const auto* face : newest)
			if (face)
				merged->faces.push_back(*face);
		if (!merged->faces.empty())
			Counts().drawnFaces.fetch_add(DCLF::Features::Reflection::ReflectionFaces::DrawRecorded(merged->faces), std::memory_order_relaxed);
		ImageSpace::StageNativeWater(*merged);
		// The flow copies' sources stay alive until the GPU finished this frame.
		Snapshot::Retire::AfterThisFrame(Snapshot::Retire::Lane::Render, std::move(merged));
	}

	std::string TakeStatsLine()
	{
		if (!installed)
			return {};
		auto& c = Counts();
		return fmt::format(
			"[ORG native] water effects: {} water updates on the game thread ({} early-outs), {} faces recorded, {} displacement steps ({} ripples), "
			"{} flow copies; {} render points, {} faces drawn",
			c.updates.exchange(0), c.early.exchange(0), c.faces.exchange(0), c.steps.exchange(0), c.ripples.exchange(0), c.copies.exchange(0), c.rendered.exchange(0),
			c.drawnFaces.exchange(0));
	}
}
