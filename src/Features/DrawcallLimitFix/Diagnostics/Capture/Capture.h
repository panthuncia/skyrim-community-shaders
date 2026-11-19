#pragma once

// The diagnostic capture. A player presses one menu button while a problem is visible. It records DCLF's state at the start
// and end of a short window and its behaviour in between, and writes one zip to send back.
// The zip holds every log line of the window, including a report window flushed to cover exactly the capture, the fade and
// growth censuses, a per-frame sample ring, the environment, settings, load order, DLLs, logs and a screenshot.
// The render thread drives it from Present. The scene lane writes its sections at a pass's end. A worker writes and zips the rest.

#include <cstdint>
#include <filesystem>
#include <string>

namespace DCLF::Capture
{
	enum class Phase : std::uint8_t
	{
		Idle,       // nothing running
		Armed,      // waiting for the menu to close
		Recording,  // the window
		Finishing,  // waiting for the reports and the scene lane's sections
		Packaging,  // the worker writes the bundle and zips it
	};

	/** @brief What the menu shows. */
	struct Status
	{
		Phase phase = Phase::Idle;
		float progress = 0.0f;           // the window's elapsed fraction while recording
		std::filesystem::path lastResult;  // the last zip, or the folder when zipping failed
		std::string lastError;           // why the last capture failed, or what it could not include
	};

	/**
	 * @brief Starts a capture of a_seconds. From the menu it starts two seconds after the menu closes, so the menu is in neither
	 * the screenshot nor the timings. Does nothing while a capture runs. Render thread.
	 */
	void Request(float a_seconds, bool a_immediate);

	/** @brief Stops an armed or recording capture and deletes its folder. Render thread. */
	void Cancel();

	/** @brief The capture's state, for the menu. Any thread. */
	Status GetStatus();

	/** @brief True while a capture records its window. Any thread. */
	bool Recording();

	/** @brief Advances the capture. Render thread, every Present, installed or not. */
	void OnPresent();

	/** @brief Writes the scene sections a capture asked for. Scene lane, at a pass's end. */
	void RunSceneLaneWork();

	/** @brief The capture's menu controls. Render thread. */
	void DrawControls();
}
