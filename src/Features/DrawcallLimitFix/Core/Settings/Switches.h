#pragma once

#include "Features/DrawcallLimitFix/Api/Settings.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace DCLF
{
	/**
	 * @brief Every CS_* switch Core reads, in the startup summary's order. Each is a row of the switch table
	 * (src/Testing/SwitchRegistry.cpp), which has its kind and description. Features register their own and read them by Api::SwitchId.
	 */
	enum class Switch : std::uint8_t
	{
		// Features: unset is DCLF's full featureset; the startup log names a run that narrows one.
		Dclf,
		AsyncCompute,
		SceneCopies,
		Shrink,
		Cull,
		CpuPipeline,
		CpuRecord,
		CpuBatch,
		PlainSnowSparkle,

		// Parity checks: a live path against a from-scratch reference, logging each mismatch.
		Parity,
		PlayerParity,
		CaptureParity,
		ParityBoth,

		// Diagnostics: stats, shader debugging, and probes. See dclf-validation.md, "Probes and diagnostics".
		Stats,
		PassStats,
		HeapFrameBlocks,
		Profile,
		ShaderDebug,
		ShaderSourceDir,
		StageDump,
		TableStart,
		ReleaseGuard,

		Count
	};

	/**
	 * @brief A switch's value from the environment, else from the switches file, else empty.
	 * The file is <Documents>\My Games\Skyrim Special Edition\SKSE\CommunityShaders-DCLF.ini, flat NAME=value lines.
	 * It exists because Mod Organizer 2 hands the game its own environment block, so a launcher's exported switches never arrive.
	 * The render graph's CS_ORG_* switches are read from the environment only, matching RenderGraphRuntime.
	 * Every switch is read once, on first use of any, so none can change under a running frame.
	 */
	const std::string& SwitchValue(Switch a_switch);

	/** @brief Whether a switch is set to "1". */
	bool SwitchEnabled(Switch a_switch);

	/** @brief Whether a default-on switch is on: anything but "0" (unset included). */
	inline bool SwitchOnUnlessZero(Switch a_switch) { return SwitchValue(a_switch) != "0"; }

	/**
	 * @brief Main thread, at install. Registers and reads a feature's switch.
	 * Fails the install for a taken name or both an after and a before. Anchors are checked once every feature registered.
	 */
	Api::SwitchId RegisterSwitch(const Api::FeatureSwitch& a_switch, std::string_view a_feature);
	/** @brief Main thread, at the end of install. Fails the install for an anchor that names no switch or loops back. */
	void CheckSwitchAnchors();
	/** @brief Whether any parity or diagnostic switch other than the shader build's is set to something but 0. Sets the report pace. */
	bool AnyObserverSwitch();
	/** @brief A switch's name, Core's or a feature's, or empty. */
	std::string_view SwitchName(Switch a_switch);
	std::string_view SwitchName(Api::SwitchId a_switch);
	/** @brief Any thread after registration. A feature switch's value, or empty. */
	const std::string& SwitchValue(Api::SwitchId a_switch);
	/** @brief Whether a feature's switch is set to "1". */
	bool SwitchEnabled(Api::SwitchId a_switch);
	/** @brief Any thread after registration. A feature switch's value by name, for readers outside the feature that have no id. */
	const std::string& RegisteredSwitchValue(std::string_view a_name);

	/**
	 * @brief Every switch the features registered, in registration order. Core's are rows of the switch table.
	 * a_row gets the name, the lower-case kind, whether it is on by default (a feature that only "0" turns off) and the description.
	 */
	void ForEachFeatureSwitch(const std::function<void(std::string_view, std::string_view, bool, std::string_view)>& a_row);

	/** @brief Whether the registry is complete. */
	bool FeatureSwitchesRegistered();

	/** @brief The switches file's directory, the SKSE log directory, outside MO2's virtual file system. */
	std::filesystem::path SwitchesDirectory();

	/**
	 * @brief Every set switch as NAME=value, for the startup log. An unknown CS_DCLF_* name is listed as unknown, so a typo never silently does nothing.
	 */
	std::string SwitchSummary();

	/**
	 * @brief The feature switches set to anything but the full featureset, as NAME=value, or empty.
	 * The startup log names a reduced run so a measurement cannot silently leave a feature out.
	 */
	std::string ReducedFeatures();
}
