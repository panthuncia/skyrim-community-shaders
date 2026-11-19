#pragma once

#include <array>
#include <cstdint>

namespace DCLF
{
	/**
	 * @brief What woke the scene pump. A source wakes a pass only when its effect is wanted before the frame's own pass.
	 *
	 * The hidden, leaf, value and property sources wake only outside the frame's render (Main::Draw to Present). Inside it the
	 * engine sets and undoes them for its own views, and the frame's next pass takes them. Per-frame queues never wake.
	 */
	enum class SceneWake : std::uint8_t
	{
		FrameInput,      // the frame inputs and the ahead context (once a frame)
		Attach,          // the tracker: an attach in the world
		Detach,          // the tracker: a detach
		Hidden,          // the tracker: a hidden bit store
		Leaf,            // the tracker: a geometry's property or alpha swapped
		PropertyUpdate,  // the tracker: a property's values
		AlphaUpdate,     // the tracker: an alpha property's values
		PropertyEvent,   // a property's flags, material or controller (SceneEvents)
		SwitchEvent,     // a switch's selection (the render thread's)
		MaterialAnswer,  // a requested material captured and answered (ServeMaterialRequests)
		TextureReply,    // a binding's texture answered (GpuTextures)
		Catalog,         // the pipeline lane's new catalog
		LoadDrain,       // a Present or frame start under a load screen (the queues drained once a call)
		Capture,         // the render thread's captures for the pass (the category capture, the mirror probe)
		Channel,         // an engine channel's wake (Api::WakeSource::Pass: Extensions::WakeChannel, its SceneStack events; the LOD gates')
		Count
	};
	inline constexpr std::array<const char*, static_cast<std::size_t>(SceneWake::Count)> kSceneWakeNames{ "frame input", "attach", "detach",
		"hidden", "leaf swap", "property values", "alpha values", "property event", "switch", "material answer", "texture reply",
		"catalog", "load drain", "captures", "LOD gate" };

	/**
	 * @brief Any thread, after the push it announces. Cheap when the source is already pending. A wake while a pass runs queues one more.
	 * Does nothing before StartScenePump, while passes run inline, or for engine events during a load screen.
	 */
	void WakeScenePass(SceneWake a_source);
	inline void WakeScenePropertyEvent() { WakeScenePass(SceneWake::PropertyEvent); }
	inline void WakeSceneSwitchEvent() { WakeScenePass(SceneWake::SwitchEvent); }
	inline void WakeSceneMaterialAnswer() { WakeScenePass(SceneWake::MaterialAnswer); }
	inline void WakeSceneTextureReply() { WakeScenePass(SceneWake::TextureReply); }
	/** @brief Render thread. Engine event wakes are held while a load screen is up. */
	void SetScenePassLoading(bool a_loading);
	/** @brief Pushes made inside the frame's render, which did not wake. */
	std::array<std::uint64_t, static_cast<std::size_t>(SceneWake::Count)> TakeScenePassDeferred();

	/** @brief Folds this thread's wakes into one, made when the outermost batch ends. */
	class ScenePassWakeBatch
	{
	public:
		ScenePassWakeBatch();
		~ScenePassWakeBatch();
		ScenePassWakeBatch(const ScenePassWakeBatch&) = delete;
		ScenePassWakeBatch& operator=(const ScenePassWakeBatch&) = delete;
	};
}
