#pragma once

// Scene diagnostics state kept between hook points and reports. Each struct names its owner thread.
// Anything crossing between the coordinator and the render thread goes under a lock or by exchange.

#include "Census.h"
#include "SceneView.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DCLF::Diagnostics
{
	/** @brief Walk parity. The slot tables against a dense rebuild. Coordinator thread. */
	struct WalkParityState
	{
		std::uint32_t checks = 0;
		std::uint32_t objects = 0;
		std::uint32_t differ = 0;
		std::uint32_t missing = 0;
		std::uint32_t extra = 0;
		std::uint32_t staleVerdicts = 0;  // entries whose kept classification differs from the reference's
		std::uint32_t staleTraits = 0;    // entries the delta walk keeps that a fresh classification would evaluate every frame
		std::string first;
		std::string firstStale;
		std::string firstStaleTraits;
		std::map<std::string, std::uint32_t> byWhat;  // differences by what differs
	};

	/** @brief Change log parity. The change log against a diff of the tables. Coordinator thread. */
	struct ChangeLogParityState
	{
		std::vector<SceneTables::Columns> snapshot;
		LogCursor cursor;  // set while a snapshot waits to be checked
		std::uint64_t checks = 0, slots = 0, changed = 0, missing = 0, skipped = 0;
		std::string first;
	};


	/** @brief The frame's registrations, posted by the render thread for the next scene pass. Latest wins. */
	struct RegistrationDrain
	{
		struct Registration
		{
			const RE::BSGeometry* geometry = nullptr;
			std::uint32_t hint = 0;
		};
		std::vector<Registration> registrations;
		bool observed = false;  // only then does registrations hold them
	};

	/** @brief Registration parity counts, reported with the scene membership line. */
	struct RegistrationParityState
	{
		std::uint64_t registeredUnbound = 0;      // main-pass registrations of eligible objects DCLF has not bound
		std::uint64_t lightingShaderDiffers = 0;  // a Lighting pass whose shader is not the engine instance DCLF uses
		std::string registeredUnboundFirst;
		// 0 frames means not observed.
		std::uint64_t registrationFrames = 0, registrationsChecked = 0;
	};

	/** @brief Technique rows against the engine's current values. Render thread at Prepass. */
	struct TechniqueParityState
	{
		std::uint64_t checks = 0, techniquesChecked = 0, techniquesDiffer = 0;
		// Rows that differ only because the sample moved since the row's write. Counted apart, as publication lag.
		std::uint64_t techniquesLate = 0;
		std::string techniqueFirst;
	};

	/** @brief Completed extras rows against the engine's own routines. Render thread at Prepass. */
	struct ExtrasParityState
	{
		std::uint64_t objects = 0, differ = 0, staleStatic = 0;
		float maxDifference = 0.0f;
		std::string first;
	};

	/**
	 * @brief Each drawn slot's shading as the engine has it now, against the frame's rows. Render thread at Prepass.
	 * A difference is named next frame, already queued, or missed.
	 */
	struct ShadingParityState
	{
		std::uint64_t checks = 0, slots = 0, missing = 0, named = 0, late = 0;
		std::string first;
	};

	/** @brief Resident parity. Residents' passes built again and checked against their records. Coordinator thread. */
	struct ResidentParityState
	{
		std::uint64_t parityChecks = 0, parityChecked = 0, parityPass = 0, parityRecord = 0;
		std::uint64_t parityPending = 0;  // fading residents, which the feedback's next decode settles
	};

	/** @brief Candidate tables against their sets. */
	struct CandidateParityState
	{
		std::uint64_t checks = 0, mismatches = 0;
	};

	/** @brief Every 60th replayed publication against the tables. */
	struct ReplayParityState
	{
		std::uint64_t parityChecks = 0, parityMaterials = 0, parityDiffer = 0;
		std::uint64_t parityObjects = 0, parityObjectsDiffer = 0, parityLogsDiffer = 0;
		std::uint64_t parityGeometries = 0, parityGeometriesDiffer = 0, parityFamiliesDiffer = 0;
	};

	/** @brief A mirror probe slice, sent to the render thread. The render thread drops the held geometries. */
	struct MirrorProbeRequest
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};

	/** @brief A slice captured live, with each record's owning reference form ID, 0 above any reference's root. */
	struct MirrorProbe
	{
		SceneCapture::Records records;
		ankerl::unordered_dense::map<const void*, std::uint32_t> owners;
		// The engine hides and shows cull-hidden nodes around views within a frame, so a slice can catch one in between.
		// Their hidden bit is not compared. Sorted.
		std::vector<const void*> frameLocalHidden;
	};

	/** @brief Mirror record parity, also used by the compatibility monitor. */
	struct MirrorParityState
	{
		// Coordinator thread. The next slice's position, the frame it was asked in, and what the applied batch's events named.
		std::size_t probeCursor = 0;
		std::uint32_t probeFrame = ~0u;
		std::unique_ptr<MirrorProbe> batchProbe, probe;
		SceneMirror::KeySet eventKeys;
		SceneMirror::FieldMap eventFields;
		// Coordinator to render thread, one slice at a time, and the records back.
		EventQueue<std::shared_ptr<MirrorProbeRequest>, 16> probeRequests;
		LatestSlot<MirrorProbe> probePosted;
		std::atomic<bool> probeAsked{ false };
		// What a check found stale, for CS_DCLF_MIRROR_WATCH=parity.
		std::atomic<const void*> watchProperty{ nullptr }, watchAlpha{ nullptr }, watchNode{ nullptr };
		const void* missedProperty = nullptr;
		const void* missedAlpha = nullptr;
		const void* missedNode = nullptr;  // a fade node stale in currentFade, for CS_DCLF_MIRROR_WATCH=parity:current
		// For CS_DCLF_MIRROR_WATCH=missed:<field>. The first object missed in a watched field, its type and fields.
		const void* missedWatched = nullptr;
		std::uint8_t missedWatchedType = 0;
		std::uint32_t missedWatchedFields = 0;
		std::atomic<const void*> watchMissed{ nullptr };
		std::atomic<std::uint32_t> watchMissedTypeFields{ 0 };  // the type in the top byte, the fields below
		// The check by record type and field.
		struct Pending
		{
			std::uint8_t type = 0;
			std::uint32_t fields = 0;
			std::string what;
			std::uint32_t flagBits = 0;  // for a node, the flag bits that differ
			std::uint32_t owner = 0;     // MirrorProbe::owners
		};
		// A geometry's node and geometry records share its key.
		ankerl::unordered_dense::map<std::uintptr_t, Pending> pending;
		// Objects an event named since their last capture. A field differing on one of them has a hook.
		SceneMirror::KeySet named;
		struct Tally
		{
			std::uint64_t probes = 0, checked = 0, absent = 0;
			std::array<std::array<std::uint64_t, SceneMirror::kMaxFields>, SceneMirror::kTypes> evented{}, late{}, missed{};
			std::string firstAbsent, firstMissed, firstEvented;
			// Per type and field, the first missed object. Per node flag bit, how often it differed.
			std::array<std::array<std::string, SceneMirror::kMaxFields>, SceneMirror::kTypes> firstMissedBy;
			std::array<std::uint64_t, 32> flagBitsMissed{}, flagBitsEvented{};
		} tally;
	};

	/** @brief Set parity. The set's commit decided again. Slots it leaves out of the root view are FirstPerson's. */
	struct CommitParityState
	{
		std::uint64_t commitParityChecks = 0, commitParityDiffer = 0;  // full evaluations, slots that differed
	};

	/** @brief Scene diagnostics state. */
	struct SceneState
	{
		CommitParityState commit;
		MirrorParityState mirror;
		ReplayParityState replay;
		CandidateParityState candidates;
		ResidentParityState residents;
		// Render thread, Prepass.
		TechniqueParityState techniques;
		ExtrasParityState extras;
		ShadingParityState shading;
		WalkParityState walk;
		ChangeLogParityState changeLog;
		// The render thread posts a drain and the pass takes the newest. Nothing new posted leaves none, so no drain is checked twice.
		std::atomic<std::shared_ptr<const RegistrationDrain>> drainPosted;
		std::shared_ptr<const RegistrationDrain> drain;
		// Render thread. Registration is captured from every batch renderer, shadow cameras included, so it is filtered to these.
		ankerl::unordered_dense::set<const RE::BSBatchRenderer*> mainBatchRenderers;
		RegistrationParityState registrations;
		// Null while none waits.
		std::atomic<std::shared_ptr<CensusRequest>> censusRequest;
		std::atomic<std::shared_ptr<CensusReply>> censusReply;
	};

	SceneState& SceneDiagnostics();
}
