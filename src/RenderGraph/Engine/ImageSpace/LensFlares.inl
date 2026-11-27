// Native's lens flares (the engine's gather, SE 100274 / AE 106988, and its parts: AE 0x1414c2a90 the visible flares and their history
// slots, 0x1414c33b0 a flare's screen position and fade, 0x1414c3810 a sprite's constants), computed by CS and staged as the lens flare
// steps with no engine call. The game inputs come from the snapshot's ImageSpace block; the lights from the shadow light set the native
// caster selection made (Shadows/Native/LightSet.h): each kept light with lens flare data, as values. A flare's history (how many frames
// in a row it was seen, and its texel in kLENSFLAREVIS) is kept per light here, by render frame, where the engine kept it in the light
// and counted game frames.
// Included by ImageSpace.cpp.

namespace NativePost
{
	namespace LensFlareGame
	{
		template <class T>
		const T& At(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<const T*>(REL::RelocationID(a_se, a_ae).address());
		}
	}

	/** @brief The lens flares' game inputs. Game thread (the snapshot capture). */
	LensFlareInputs ReadLensFlares()
	{
		using LensFlareGame::At;
		LensFlareInputs in;
		in.on = At<bool>(513349, 391128);                               // ToggleLensFlare's flag
		in.sun = At<const RE::BSLensFlareRenderData*>(527915, 414867);  // the weather's sun lens flare
		const auto* direction = &At<float>(527924, 414871);             // Sun::Update's direction to the sun
		in.sunDirection = { direction[0], direction[1], direction[2] };
		in.sunVisibility = At<float>(527933, 414878);   // Sun::Update
		const auto* tint = &At<float>(527934, 414879);  // the sky's sun flare colour
		in.sunTint = { tint[0], tint[1], tint[2] };
		in.fadeDistance = At<float>(513351, 391131);
		in.intensity = At<float>(513353, 391134);  // SetLensFlareGlobalIntensity
		return in;
	}

	namespace LensFlareRun
	{
		constexpr std::uint32_t kSlots = 16;         // kLENSFLAREVIS's texels, one per flare seen
		constexpr std::uint32_t kNewFlares = 15;     // the gather stops taking flares that need a texel at this many
		constexpr std::uint32_t kMostFlares = 16;    // and stops after this many flares seen
		constexpr float kEpsilon = 1.0e-6f;          // DAT_141ad27f8, NiPoint3::Unitize's
		constexpr float kScreenTolerance = 1.0e-5f;  // DAT_1417afdc4, WorldPtToScreenPt3's
		constexpr float kAngularFadeBase = 255.0f;   // DAT_141ad29a8
		// The sprites' history weights (AE 391126, SE 513347; the same table on both): row (rotation + 4 * (frames seen - 1, at most 3)),
		// one weight per kLENSFLAREVIS channel, so a flare's visibility averages the frames it was seen in.
		constexpr std::array<std::array<float, 4>, 16> kHistoryWeights{ {
			{ 1.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.5f, 0.0f, 0.0f, 0.5f },
			{ 0.5f, 0.5f, 0.0f, 0.0f },
			{ 0.0f, 0.5f, 0.5f, 0.0f },
			{ 0.0f, 0.0f, 0.5f, 0.5f },
			{ 0.34f, 0.0f, 0.33f, 0.33f },
			{ 0.33f, 0.34f, 0.0f, 0.33f },
			{ 0.33f, 0.33f, 0.34f, 0.0f },
			{ 0.0f, 0.33f, 0.33f, 0.34f },
			{ 0.25f, 0.25f, 0.25f, 0.25f },
			{ 0.25f, 0.25f, 0.25f, 0.25f },
			{ 0.25f, 0.25f, 0.25f, 0.25f },
			{ 0.25f, 0.25f, 0.25f, 0.25f },
		} };

		using Float4 = std::array<float, 4>;

		using Light = DCLF::Features::Shadows::Native::ShadowLight;

		/** @brief A flare's history: the render frame it was last seen in, how many frames in a row, and its texel. */
		struct History
		{
			const RE::BSShadowLight* light = nullptr;  // a key, null for the sun
			std::uint64_t last = 0;
			std::uint32_t seen = 0;
			std::uint32_t slot = 0;
		};

		/** @brief Render thread. */
		struct State
		{
			std::vector<History> lights;  // the lights seen last frame
			History sun;
			std::array<Float4, kSlots> slots{};  // the visibility pass's texel positions; a texel not used this frame keeps its last
			std::uint32_t rotation = 0;          // the kLENSFLAREVIS channel this frame's visibility pass writes
			std::uint64_t frames = 0, drawn = 0, sprites = 0;
		};

		State& S()
		{
			static State state;
			return state;
		}

		/** @brief NiPoint3::Unitize (AE 0x1402b08d0). */
		RE::NiPoint3 Unitize(const RE::NiPoint3& a_v)
		{
			const float length = std::sqrt(a_v.x * a_v.x + a_v.y * a_v.y + a_v.z * a_v.z);
			if (kEpsilon < length) {
				const float inverse = 1.0f / length;
				return { a_v.x * inverse, a_v.y * inverse, a_v.z * inverse };
			}
			return {};
		}

		/** @brief NiCamera::WorldPtToScreenPt3 (AE 0x140d2c5b0) with the snapshot camera's matrix and port. */
		bool WorldToScreen(const Snapshot::CameraView& a_camera, const RE::NiPoint3& a_p, float& a_x, float& a_y, float& a_z)
		{
			const auto& m = a_camera.worldToCam;
			const float w = std::fabs(a_p.x * m[3][0] + a_p.y * m[3][1] + a_p.z * m[3][2] + m[3][3]);
			if (!(kScreenTolerance < w))
				return false;
			const float inverse = 1.0f / w;
			// NiRect's fields are protected: left, right, top, bottom.
			std::array<float, 4> port;
			std::memcpy(port.data(), &a_camera.port, sizeof(port));
			float x = a_p.y * m[0][1] + a_p.x * m[0][0] + a_p.z * m[0][2] + m[0][3];
			float y = m[1][1] * a_p.y + m[1][0] * a_p.x + m[1][2] * a_p.z + m[1][3];
			x = inverse * x;
			y = inverse * y;
			x = (port[1] - port[0]) * 0.5f * x;
			y = (port[2] - port[3]) * 0.5f * y;
			a_x = (port[1] + port[0]) * 0.5f + x;
			a_y = (port[2] + port[3]) * 0.5f + y;
			a_z = (m[2][1] * a_p.y + m[2][0] * a_p.x + m[2][2] * a_p.z + m[2][3]) * inverse;
			return true;
		}

		/** @brief The camera's view direction (its world rotation's first column), unitized as the engine does. */
		RE::NiPoint3 ViewDirection(const Snapshot::CameraView& a_camera)
		{
			const auto& r = a_camera.rotation.entry;
			return { r[0][0], r[1][0], r[2][0] };
		}

		/**
		* @brief One frame's flares as the gather (AE 0x1414c2a90) finds them: a flare in view and close enough continues its texel when it
		* was seen the frame before, else waits for a free one. a_lights are the set's lights in the selection's order; those with lens
		* flare data are the engine's queue. Returns how many flares were seen. a_s.lights becomes this frame's lights.
		*/
		std::uint32_t Gather(State& a_s, const LensFlareInputs& a_in, const Snapshot::CameraView& a_camera, std::uint64_t a_frame, std::span<const Light> a_lights)
		{
			std::uint32_t seen = 0;
			std::array<bool, kSlots> used{};
			// The flares that wait for a texel: their position, and their history (0 the sun's, i + 1 that of lights[i]).
			struct Waiting
			{
				Float4 position{};
				std::size_t history = 0;
			};
			std::array<Waiting, kNewFlares + 1> waiting{};
			std::uint32_t waitingCount = 0;
			std::vector<History> lights;
			lights.reserve(a_lights.size());
			const auto historyOf = [&](std::size_t a_index) -> History& { return a_index == 0 ? a_s.sun : lights[a_index - 1]; };
			const auto& eye = a_camera.position;
			const auto view = Unitize(ViewDirection(a_camera));
			if (a_in.sun) {
				const RE::NiPoint3 point{ a_in.sunDirection[0] + eye.x, a_in.sunDirection[1] + eye.y, a_in.sunDirection[2] + eye.z };
				Float4 position{ 0.0f, 0.0f, 0.0f, 1.0f };
				if (0.0f < a_in.sunVisibility && WorldToScreen(a_camera, point, position[0], position[1], position[2])) {
					const auto toSun = Unitize({ a_in.sunDirection[0], a_in.sunDirection[1], a_in.sunDirection[2] });
					if (0.0f < toSun.y * view.y + toSun.x * view.x + toSun.z * view.z) {
						position[1] = 1.0f - position[1];
						position[2] = 1.0f;
						if (a_frame != a_s.sun.last + 1) {
							a_s.sun.seen = 1;
							waiting[waitingCount++] = { position, 0 };
						} else {
							++a_s.sun.seen;
							used[a_s.sun.slot] = true;
							a_s.slots[a_s.sun.slot] = position;
						}
						a_s.sun.last = a_frame;
						++seen;
					}
				}
			}
			for (const auto& light : a_lights) {
				if (!light.lensFlare)
					continue;
				if (waitingCount >= kNewFlares)
					break;
				const auto* data = light.lensFlare;
				if (!light.hasNiLight)
					continue;
				const RE::NiPoint3 point{ light.position[0], light.position[1], light.position[2] };
				Float4 position{ 0.0f, 0.0f, 0.0f, 1.0f };
				if (!WorldToScreen(a_camera, point, position[0], position[1], position[2]))
					continue;
				const RE::NiPoint3 offset{ point.x - eye.x, point.y - eye.y, point.z - eye.z };
				const float distance = std::sqrt(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
				const float past = (std::max)(distance - light.radius * data->fadeDistRadiusScale, 0.0f);
				const float fade = (std::min)(past / a_in.fadeDistance, 1.0f);
				const auto toLight = Unitize(offset);
				if (!(0.0f < view.x * toLight.x + view.y * toLight.y + view.z * toLight.z) || !(fade < 1.0f))
					continue;
				position[1] = 1.0f - position[1];
				// The light's history from last frame, if it was seen then.
				const auto previous = std::ranges::find_if(a_s.lights, [&](const History& a_h) { return a_h.light == light.light; });
				lights.push_back(previous != a_s.lights.end() ? *previous : History{ .light = light.light });
				auto& now = lights.back();
				if (a_frame == now.last + 1) {
					++now.seen;
					used[now.slot] = true;
					a_s.slots[now.slot] = position;
				} else {
					now.seen = 1;
					waiting[waitingCount++] = { position, lights.size() };
				}
				now.last = a_frame;
				if (++seen > kMostFlares)
					break;
			}
			// The waiting flares take the free texels in order. The last texel is never handed out; a flare with none starts again next frame.
			std::uint32_t cursor = 0;
			for (std::uint32_t i = 0; i < waitingCount; ++i) {
				while (cursor < kSlots && used[cursor])
					++cursor;
				auto& history = historyOf(waiting[i].history);
				if (cursor >= kSlots - 1) {
					history.seen = 1;
					history.last = a_frame - 10;
				} else {
					history.slot = cursor;
					a_s.slots[cursor] = waiting[i].position;
				}
				++cursor;
			}
			a_s.lights = std::move(lights);
			return seen;
		}

		/** @brief A flare's place this frame (AE 0x1414c33b0): its sprites' screen position and rotation, colour, fade and view factor. */
		struct Placement
		{
			Float4 position{};       // the sprites' vertex row: screen x, y and z (1 for the sun)
			float angle = 0.0f;      // the sprites' rotation, from the screen's centre
			float dot = 0.0f;        // the view direction against the direction to the flare
			float intensity = 0.0f;  // the sun's visibility, or the light's distance fade
			Float4 colour{};         // the light's diffuse, toward white by the flare's colour influence
		};

		bool Place(const LensFlareInputs& a_in, const WorldSettings& a_world, const Snapshot::CameraView& a_camera, const Light* a_light,
			const RE::BSLensFlareRenderData& a_data, float a_width, float a_height, Placement& a_out)
		{
			const auto& eye = a_camera.position;
			RE::NiPoint3 point;
			std::array<float, 3> diffuse{};
			if (a_light) {
				if (!a_light->hasNiLight)
					return false;
				point = { a_light->position[0], a_light->position[1], a_light->position[2] };
				a_out.intensity = 1.0f;
				diffuse = a_light->diffuse;
			} else {
				point = { a_in.sunDirection[0] + eye.x, a_in.sunDirection[1] + eye.y, a_in.sunDirection[2] + eye.z };
				a_out.intensity = a_in.sunVisibility;
				diffuse = a_world.sunColour;
			}
			const float influence = a_data.colorInfluence;
			a_out.colour = { (diffuse[0] - 1.0f) * influence + 1.0f, (diffuse[1] - 1.0f) * influence + 1.0f, (diffuse[2] - 1.0f) * influence + 1.0f, 0.0f };
			float x = 0.0f, y = 0.0f, z = 0.0f;
			if (!WorldToScreen(a_camera, point, x, y, z))
				return false;
			if (!a_light)
				z = 1.0f;
			const auto view = Unitize(ViewDirection(a_camera));
			RE::NiPoint3 toFlare{ a_in.sunDirection[0], a_in.sunDirection[1], a_in.sunDirection[2] };
			if (a_light) {
				toFlare = { point.x - eye.x, point.y - eye.y, point.z - eye.z };
				const float distance = std::sqrt(toFlare.x * toFlare.x + toFlare.y * toFlare.y + toFlare.z * toFlare.z);
				const float past = (std::max)(distance - a_light->radius * a_data.fadeDistRadiusScale, 0.0f);
				a_out.intensity = 1.0f - (std::min)(past / a_in.fadeDistance, 1.0f);
			}
			const auto direction = Unitize(toFlare);
			a_out.dot = direction.y * view.y + direction.x * view.x + direction.z * view.z;
			if (!(0.0f < a_out.dot) || !(0.0f < a_out.intensity))
				return false;
			a_out.position = { x, y, z, 0.0f };
			a_out.angle = std::atan2(((y - 0.5f) * a_height) / a_width, x - 0.5f);
			return true;
		}

		/** @brief BSLensFlareSpriteRenderData +0x10: tint, width, height, position, angular fade, opacity, flags. */
		struct SpriteData
		{
			float tint[3];
			float width, height, position, angularFade, opacity;
			std::uint32_t flags;  // 1: turns with its angle from the centre; 2: pixel row 0's w is 1
		};

		/** @brief A sprite's constants (AE 0x1414c3810) as the param's groups hold them: vertex constants 0 and 1, pixel constants 0 to 3. */
		struct SpriteConstants
		{
			std::array<float, 2 * 4> vertex{};
			std::array<float, 4 * 4> pixel{};
		};

		SpriteConstants SpriteRows(const LensFlareInputs& a_in, const Placement& a_place, const SpriteData& a_sprite, bool a_sun, std::uint32_t a_seen,
			std::uint32_t a_slot, std::uint32_t a_rotation, float a_width, float a_height)
		{
			SpriteConstants c;
			std::copy_n(a_place.position.data(), 4, c.vertex.data() + 4);
			c.vertex[7] = 1.0f - (a_sprite.position + a_sprite.position);
			const float angle = (a_sprite.flags & 1) ? a_place.angle : 0.0f;
			const float sine = std::sin(angle), cosine = std::cos(angle);
			const float aspect = 1.0f / (a_width / a_height);
			c.vertex[0] = a_sprite.width * cosine * aspect;
			c.vertex[1] = a_sprite.width * sine;
			c.vertex[2] = -(a_sprite.height * sine * aspect);
			c.vertex[3] = a_sprite.height * cosine;
			const float exponent = std::pow(kAngularFadeBase, a_sprite.angularFade);
			const float fade = std::pow(a_place.dot, exponent - 1.0f);
			const std::array<float, 3> tint = a_sun ? a_in.sunTint : std::array<float, 3>{ 1.0f, 1.0f, 1.0f };
			for (std::uint32_t i = 0; i < 3; ++i)
				c.pixel[i] = a_sprite.opacity * a_sprite.tint[i] * fade * a_place.intensity * tint[i] * a_place.colour[i];
			c.pixel[3] = (a_sprite.flags & 2) ? 1.0f : 0.0f;
			const std::uint32_t history = a_seen < 4 ? (a_seen > 0 ? a_seen - 1 : 0) : 3;
			std::copy_n(kHistoryWeights[a_rotation + history * 4].data(), 4, c.pixel.data() + 4);
			c.pixel[8] = static_cast<float>(a_slot);
			c.pixel[12] = a_in.intensity;
			return c;
		}

		/** @brief The ImageSpace manager's shader for a_effect, by the manager's own lookup. */
		const RE::BSImagespaceShader* ShaderOfEffect(Effects::ImageSpaceEffectEnum a_effect)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			const std::uint32_t index = RE::ImageSpaceManager::GetCurrentIndex(a_effect);
			if (!manager || index >= manager->effects.free_idx() || index >= manager->effects.capacity())
				return nullptr;
			return static_cast<const RE::BSImagespaceShader*>(manager->effects[static_cast<decltype(manager->effects)::size_type>(index)]);
		}

		/** @brief Stages one lens flare step from NativeSteps' rows (and sprite), as a native draw of a_shader's effect would. */
		void Stage(std::size_t a_effect, const RE::BSImagespaceShader* a_shader)
		{
			auto& state = IS();
			EnterScope(ScopeKind::LensFlare);
			if (state.scopeDepth == 0)
				return;
			const auto previous = state.current;
			state.current = EffectScope{ .shader = a_shader, .effect = a_effect, .covered = true, .eligible = true };
			auto& native = NativeSteps::Get();
			native.effect = a_effect;
			native.shader = a_shader;
			native.rowsAt = {};
			native.armed = true;
			TakeDraw();
			native.armed = false;
			state.current = previous;
			LeaveScope();
		}
	}

	/**
	* @brief Native: the lens flare gather (SE 100274, AE 106988) from the snapshot: the visibility pass over the frame's flare texels,
	* then each visible flare's sprites (the sun's first, then the light set's lights with lens flare data), staged as the lens flare
	* steps. Render owner, inside the world post chain.
	*/
	void RunLensFlares()
	{
		using namespace LensFlareRun;
		auto& s = S();
		const auto* set = DCLF::Features::Shadows::Native::RenderSet();
		const std::span<const Light> lights = set ? std::span<const Light>(set->lights) : std::span<const Light>{};
		const auto& snapshot = Snapshot::Current();
		const auto* block = snapshot ? snapshot->Block<Block>() : nullptr;
		const auto* camera = snapshot ? &snapshot->cameras.Current(Snapshot::ViewId::Main) : nullptr;
		if (block && block->lensFlares.on && camera && camera->valid) {
			const auto& in = block->lensFlares;
			++s.frames;
			if (Gather(s, in, *camera, Snapshot::Render().frame, lights) != 0) {
				const auto shapes = RenderGraph::Frame::Vars::Adopted(RenderGraph::Frame::Vars::EngineTargetShapes());
				const auto& main = shapes.colours[RE::RENDER_TARGET::kMAIN];
				const float width = static_cast<float>(main.width), height = static_cast<float>(main.height);
				const auto* visibility = ShaderOfEffect(Effects::ISLensFlareVisibility);
				const auto* sprites = ShaderOfEffect(Effects::ISLensFlare);
				const auto visibilityEffect = EffectIndexOf(Effects::ISLensFlareVisibility);
				const auto spriteEffect = EffectIndexOf(Effects::ISLensFlare);
				// The sprites' quad (AE 414874, SE 527927), made with the lens flares. The sprites draw into kMAIN (the gather sets it as the
				// sprite effect's output).
				const auto* quad = *REL::Relocation<const RE::BSGeometry**>{ REL::RelocationID(527927, 414874) }.get();
				if (!visibility || !sprites || visibilityEffect < 0 || spriteEffect < 0 || !quad)
					RenderGraph::Fatal("lens flares", "the ImageSpace manager has no lens flare effects or sprite quad");
				auto& native = NativeSteps::Get();
				native.sprite = {};
				if (const char* why = NativeSteps::BuildRowsFrom(*visibility, s.slots[0].data(), kSlots, native.rows))
					RenderGraph::Fatal("lens flares", std::format("the visibility pass's rows: {}", why));
				Chain().lensVisibilityChannel = static_cast<std::uint16_t>(s.rotation);
				Stage(static_cast<std::size_t>(visibilityEffect), visibility);
				Chain().lensVisibilityChannel = 0;
				const auto flare = [&](const Light* a_light) {
					const auto* data = a_light ? a_light->lensFlare : in.sun;
					Placement place;
					if (!data || !Place(in, block->world, *camera, a_light, *data, width, height, place))
						return;
					// A light the gather did not reach this frame has no history: one frame seen, texel 0.
					static const History kNone{ .seen = 1 };
					const History* history = &s.sun;
					if (a_light) {
						const auto found = std::ranges::find_if(s.lights, [&](const History& a_h) { return a_h.light == a_light->light; });
						history = found != s.lights.end() ? &*found : &kNone;
					}
					for (const auto& sprite : data->sprites) {
						const auto* bytes = reinterpret_cast<const std::byte*>(sprite.get());
						const auto* texture = bytes ? *reinterpret_cast<const RE::NiTexture* const*>(bytes + 0x40) : nullptr;
						if (!texture)
							continue;
						SpriteData values;
						std::memcpy(&values, bytes + 0x10, sizeof(values));
						const auto c = SpriteRows(in, place, values, data == in.sun, history->seen, history->slot, s.rotation, width, height);
						if (const char* why = NativeSteps::BuildRowsFrom(*sprites, c.pixel.data(), static_cast<std::uint32_t>(c.pixel.size() / 4), native.rows))
							RenderGraph::Fatal("lens flares", std::format("a sprite's pixel rows: {}", why));
						if (const char* why = NativeSteps::BuildSpriteFrom(*sprites, RE::RENDER_TARGET::kMAIN, c.vertex.data(), static_cast<std::uint32_t>(c.vertex.size() / 4), quad, -1, texture,
								native.sprite))
							RenderGraph::Fatal("lens flares", std::format("a sprite: {}", why));
						Stage(static_cast<std::size_t>(spriteEffect), sprites);
						++s.sprites;
					}
				};
				flare(nullptr);
				for (const auto& light : lights)
					if (light.lensFlare)
						flare(&light);
				native.sprite = {};
				s.rotation = (s.rotation + 1) & 3;
				++s.drawn;
			}
		}
	}
}
