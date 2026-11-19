#pragma once

// The compatibility monitor. Parity checks a player can turn on while playing, to find engine state another plugin changed where
// DCLF has no hook, and name the references and plugins involved.
// Only checks that leave the main path alone run here, mainly the scene mirror's live probe. Checks that change what DCLF draws
// or when stay developer switches.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace DCLF::Diagnostics::PlayerParity
{
	/** @brief One offender: a field of the objects under one reference that changed with no event, counted over the session. */
	struct Offender
	{
		std::uint32_t form = 0;    // the first owning reference's form ID, 0 above any reference's root
		std::uint32_t references = 1;  // a property or alpha is shared by every instance of its mesh
		std::uint8_t type = 0;     // SceneMirror::kTypeNames, or kLeak, kEvicted
		std::uint32_t fields = 0;  // the differing fields' bits
		std::uint64_t count = 0;   // the probes that found them missed
		std::uint32_t firstFrame = 0, lastFrame = 0;
		std::string example;       // the first difference, as the mirror parity logs it
	};

	/** @brief Offender kinds besides the mirror's record types. A geometry the engine draws too, and a subtree gone with no detach. */
	inline constexpr std::uint8_t kLeak = 4, kEvicted = 5;

	/** @brief Offenders since the monitor last started or was cleared, oldest first. Any thread. */
	struct Offenders
	{
		std::vector<Offender> entries;
		std::uint64_t probes = 0;  // checks since the session started
		std::uint64_t overflow = 0;  // new offenders past the table's limit, counted only
		std::uint32_t sessionFrame = 0;
	};

	/** @brief Installs the frame-start hook and seeds the request from CS_DCLF_PLAYER_PARITY. PostPostLoad. */
	void Install();

	/** @brief Turns the monitor on or off from the next frame. */
	void Request(bool a_on);
	bool Requested();

	/** @brief Whether the monitor is on this frame. */
	bool Active();

	/** @brief True once after the monitor started, so its observers drop what they kept while off. Coordinator thread. */
	bool TakeReset();

	/** @brief A missed difference on an object of reference a_form. Coordinator thread. */
	void NoteMissed(const void* a_object, std::uint32_t a_form, std::uint8_t a_type, std::uint32_t a_fields, std::string_view a_example);

	/** @brief A native pass of a geometry DCLF draws, which is a leak. Render thread, while Active. */
	void NoteLeak(std::uint32_t a_form, std::string a_example);

	/** @brief Publishes the offenders when they changed, applying a pending clear. Coordinator thread, after each mirror check. */
	void NoteCheck();

	/** @brief The current offenders snapshot. */
	std::shared_ptr<const Offenders> Current();

	/** @brief Clears the offenders at the coordinator's next check. */
	void Clear();

	/** @brief Writes the report to a_path, or beside the log when empty. Returns its path, or why it was not written. Main or render thread. */
	std::string WriteReport(const std::filesystem::path& a_path = {});

	/** @brief The menu section. */
	void DrawPanel();
}
