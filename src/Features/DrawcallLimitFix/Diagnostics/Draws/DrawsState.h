#pragma once

// DrawsState: the draw observers' state between hook points. Render thread unless noted.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "DrawsView.h"

namespace DCLF::Diagnostics
{
	/**
	 * @brief Flags a backing pool whose leased count stays above frames in flight plus two for kWindows windows.
	 * The live count is no measure, since a pool keeps free backings warm.
	 */
	struct PoolLeakWatch
	{
		static constexpr std::uint64_t kFramesInFlight = RenderGraphRuntime::kGameFramesInFlight;
		static constexpr std::uint64_t kSlack = 2;
		static constexpr std::uint32_t kWindows = 3;
		std::uint32_t over = 0;  // consecutive windows past the bound

		/** @brief Records this window's leased count. Returns a flag text, or empty while within the bound. */
		const char* Window(std::uint64_t a_leased)
		{
			over = a_leased > kFramesInFlight + kSlack ? over + 1 : 0;
			return over >= kWindows ? " <- POOL LEAK" : "";
		}
	};

	/**
	 * @brief A D3D11 view of a versioned buffer's version as 32-bit words, for immediate-context readbacks.
	 * It holds the version, so the view never outlives its backing.
	 */
	struct WrappedVersion
	{
		std::shared_ptr<const org::BufferVersion> wrapped;
		winrt::com_ptr<ID3D11Buffer> view;

		ID3D11Buffer* Of(const Draws::Versioned& a_buffer) { return Of(a_buffer ? a_buffer->Current() : nullptr); }
		/** @brief A view of the given version, which need not be the current one. */
		ID3D11Buffer* Of(std::shared_ptr<const org::BufferVersion> a_version)
		{
			auto current = std::move(a_version);
			if (!current || !current->buffer)
				return nullptr;
			if (!view || wrapped != current) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = static_cast<UINT>(current->buffer->GetSize());
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				view = RenderGraphRuntime::Get().WrapBuffer(*current->buffer, desc);
				wrapped = std::move(current);
			}
			return view.get();
		}
	};

	/** @brief Draw diagnostics state. The main path never reads it. */
	struct DrawsState
	{
		static DrawsState& Get()
		{
			static DrawsState state;
			return state;
		}


		// Build parity. GPU output is copied to staging after an epoch and compared a few frames later.
		struct ParityReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count, classCounts;
			// Their pipeline index is the set's index within its state class, as BuildDrawsCS stores it.
			std::vector<DrawSequence> expected;
			// Per decal group by slot. A culled slot reads back with index count zero.
			std::array<std::vector<DrawSequence>, kDecalGroups> expectedDecals;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;  // the buffer's ranges when copied
			std::vector<SetClassRanges> classes;
			// With plain draws on, the plan's buckets take the colour and terrain draws instead of the class sub-ranges.
			std::shared_ptr<const SetClassPlan> classPlan;
			std::uint32_t framesLeft = 0;
			// The bases the CPU's table-relative row offsets were placed at.
			std::uint64_t pipelineRowsBase = 0, materialRowsBase = 0;
			// The CPU's local shadow light selection per object. None on the Z-prepass.
			ankerl::unordered_dense::map<std::uint32_t, std::uint32_t> expectedLocalShadows;
			// Portal-strict lights' mask bits. BuildDraws gates them by the receiver's root, which this reference does not model.
			std::uint32_t gatedLocalShadows = 0;
			ankerl::unordered_dense::map<std::uint32_t, std::array<float, 4>> armedBounds;
			std::array<const void*, 4> armedChannels{};  // ChannelLights at arming
			// The sequence version the adopted set names, and the buffer's current one when copied.
			std::uint64_t sequencesNamed = 0, sequencesCurrent = 0;
			std::uint32_t armedEpoch = 0;  // parityEpochs when copied
		};
		std::optional<ParityReadback> parity;
		std::uint32_t parityEpochs = 0;
		WrappedVersion paritySequences, parityClassCounts;
		// Main epoch payloads, newest last. They show where a row address outside its epoch's tables came from.
		struct ParityPayloadSeen
		{
			std::uint64_t serial = 0;
			std::uint32_t epoch = 0;  // parityEpochs when first seen
			std::uint64_t materialBase = 0, materialBytes = 0, pipelineBase = 0, pipelineBytes = 0;
		};
		std::array<ParityPayloadSeen, 32> parityPayloads{};
		std::uint32_t parityPayloadsSeen = 0;

		// Set parity. Copied after a colour epoch and read a few epochs later without waiting.
		struct SetClassReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count, classCounts;
			std::vector<SetClassRanges> classes;
			std::shared_ptr<const SetClassPlan> plan;
			std::vector<std::uint32_t> pipelines, classOf;
			// Per object, the set indices of the payload inputs naming it. Each of its draws' class and index must come from these.
			ankerl::unordered_dense::map<std::uint32_t, std::vector<std::uint32_t>> setsOf;
			std::uint32_t sequenceDraws = 0, sequenceDecals = 0;
			bool sorted = false;  // each class's main sub-range is in pipeline order
			std::uint32_t framesLeft = 0, tries = 0;
			std::uint32_t armedEpoch = 0;  // setClassEpochs when copied
		};
		std::optional<SetClassReadback> setClass;
		std::uint32_t setClassEpochs = 0;
		WrappedVersion setClassSequences, setClassCounts;

		// The colour commit's last culling latch (sun test), and the depth commit's last terrain near test.
		BuildDrawsLatch sunUpload{};
		std::array<float, 4> terrainNearUpload{};
		// A fade-rejected root seen in view. The fade log's other windows focus on it.
		std::atomic<std::uint32_t> fadeLogFocus{ ~0u };

		// Set parity. One snapshot per frame in flight, read back a few frames later.
		struct SetParityFrame
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::uint32_t frame = 0;
			std::uint32_t framesLeft = 0;
			std::vector<std::uint8_t> depthState, colourState;  // MainBody::objectState of the two builds
			// Per object. Bit 0 in the main set, bit 1 bound, bit 2 alpha tested, bit 3 kObjectMember disagrees with the set.
			std::vector<std::uint8_t> flags;
			std::vector<const RE::BSGeometry*> geometry;
			// Keeps every named geometry alive until the readback.
			std::shared_ptr<const SceneTables> tables;
			// TEMP. The tables the colour payload was built from, to tell whether they are the snapshot's.
			std::shared_ptr<const SceneTables> payloadTables;
			// TEMP. Per object, bits 0 and 1 mark the colour payload's region and list naming it, bits 2 and 3 the depth payload's.
			// The input flags are the first list's.
			std::vector<std::uint8_t> listedBy;
			std::vector<std::uint32_t> listedFlags;
			// TEMP. The installed inputs backing after the colour epoch, and the inputs the epochs dispatched.
			winrt::com_ptr<ID3D11Buffer> inputsStaging;
			std::uint32_t inputCount = 0, depthInputCount = 0;
			std::uint64_t inputsGeneration = 0;
			// TEMP. The writer records after the colour epoch, and the installed scene list's descriptor.
			winrt::com_ptr<ID3D11Buffer> writersStaging;
			std::uint32_t inputsSrv = 0;
			// Frustum stamps, and per member whether its live bound is inside the depth frustum. Culling must not reject one inside.
			winrt::com_ptr<ID3D11Buffer> frustumStaging;
			std::vector<std::uint8_t> inView;
			std::vector<std::array<float, 4>> bound;
			// The walk's portal program words. A rejection in view is judged again on the CPU against them.
			std::vector<std::uint32_t> programWords;
			// Per object, its phases in the registration hooks' claims. The main claim must equal the set, or an object is drawn twice or not at all.
			std::vector<std::uint8_t> claims;
		};
		std::deque<SetParityFrame> setParityFrames;
		std::vector<winrt::com_ptr<ID3D11Buffer>> setParityStaging;  // released stagings, reused
		WrappedVersion setParityVisibility, setParityFrustum, setParityWriters;
		struct SetParityCounts
		{
			std::uint32_t frames = 0, framesWithDamage = 0, skipped = 0;
			std::uint32_t depthOnly = 0, colourOnly = 0, colourUnpublished = 0, withheldUndrawn = 0, withheldCulled = 0;
			std::uint32_t colourDrawnTotal = 0, depthDrawnTotal = 0;
			std::uint32_t alphaDepthOnly = 0, alphaColourOnly = 0, alphaWithheldUndrawn = 0;
			// Double draws outside the set, and records whose kObjectMember disagrees with the set.
			std::uint32_t outsideDrawn = 0, recordDisagrees = 0;
			std::uint32_t samples = 0;
			// Resident objects culled for 1-8 frames and then drawn again. Counted, not flagged, since occlusion uses this frame's depth.
			std::uint32_t gaps = 0, gapsRetest = 0, gapsRejected = 0, gapsTree = 0, gapSamples = 0;
			std::array<std::uint32_t, 3> gapsByLength{};
			std::array<std::uint32_t, 7> gapsByClass{};
			std::uint32_t gapsInView = 0, gapsNew = 0, gapsAfterNative = 0, gapsBeforeNative = 0;
			// Members rejected with their live bound in view, by frustum and by fade, and how many were skinned.
			std::uint32_t rejectedInView = 0, fadeHiddenInView = 0, skinnedInView = 0, inViewSamples = 0, fadeSamples = 0;
			// The frustum's in-view rejections judged again on the CPU with this frame's program words.
			// Each is culled there too, in view there and flagged, in view by the newer preprocessed flags, or unjudged.
			std::uint32_t programCulled = 0, programInView = 0, programPreprocessed = 0, programUnjudged = 0;
			// Fade-rejected in view while the engine (still culling the root) has its node fully faded in.
			std::uint32_t fadeContradicted = 0, fadeContradictedLast = 0;
			std::string fadeContradictedFirst;
			// Main claims outside the set are drawn by nobody. Unclaimed members are drawn twice.
			std::uint32_t claimedOutside = 0, unclaimedMembers = 0, claimSamples = 0;
		} setParity;
		// Gap detector per geometry. State 0 is not resident, 1 drawn, 2 withheld and GPU-culled.
		struct GapHistory
		{
			std::uint32_t frame = 0, first = 0;
			std::uint8_t last = 0;
			// The current withheld run. drawnBy is 1 for outside the set, 2 for DCLF.
			std::uint8_t run = 0, runClass = 0, runVerdict = 0, runInView = 0, drawnBy = 0, runDrawnBy = 0;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, GapHistory> gapHistory;

		// Frame value rows and palettes against the engine's.
		struct FrameValuesParity
		{
			std::uint64_t checks = 0, differs = 0, fresh = 0, paletteChecks = 0, paletteDiffers = 0;
			// Resident slots whose shading row no producer sampled (written 0) at two checks running.
			std::uint64_t shadingChecks = 0, shadingUnsampled = 0;
			std::string first, paletteFirst, shadingFirst;
			// Differing rows by field and owner, one example each, to find the writer.
			ankerl::unordered_dense::map<std::string, std::pair<std::uint64_t, std::string>> causes;
		} frameValues;
		// The resident slots unsampled at the last check (kept across the report's reset of frameValues).
		std::vector<std::uint8_t> shadingUnsampledBefore;

		// Kept store checks, each run on its store's thread.
		struct Kept
		{
			ParityCounter objects, geometry, drawBound, sunExclusion, parabolicExclusion, shadow;
			// Inputs the kept shadow build lacks, by reason, with the first of each.
			std::map<std::string, std::uint64_t> shadowMissingBy;
			std::map<std::string, std::string> shadowMissingFirst;
			// Extras rows as built, for the newest version and its predecessor.
			ankerl::unordered_dense::map<std::uint64_t, std::vector<float>> extrasMirrors;
			ParityCounter extras;
			// Differing rows in blocks no live object owns. They are never read, so not compared.
			std::uint64_t extrasFreedRows = 0;
		} kept;
		// Main commit log state, kept by CommitLog.cpp.
		std::array<std::uint32_t, kTextureRegisters> unfilledFrameBuffers{};
		std::uint32_t fadeInputsVaried = 0;
		bool cullingSetupLogged = false;
		std::array<std::uint32_t, 2> epochLines{};  // colour, Z-prepass

		// Leak watch for FrameValues' pools, sized by the render thread's FrameValuesLine.
		std::vector<PoolLeakWatch> frameValuesPools;

		// View mask checks run in the builds. The render thread's report takes them.
		struct ViewMasks
		{
			std::atomic<std::uint64_t> checks{ 0 }, inputs{ 0 }, differ{ 0 };
			// First difference as (list << 56) | (object << 24) | expected mask, and its mask.
			std::atomic<std::uint64_t> first{ ~0ull }, firstMask{ 0 };
		} viewMasks;


		// Shadow epoch checks, kept by ShadowChecks.cpp.
		std::uint32_t indexPoolChecks = 0;
		struct ShadowTargets
		{
			std::uint64_t checked = 0, differ = 0;
			std::string first;
		} shadowTargets;
		struct OcclusionView
		{
			std::uint32_t maps = 0, differ = 0;
			float largest = 0.0f;
			std::string first;
		};
		std::array<OcclusionView, kOcclusionViews> occlusionViews;

		// The catalog's constant tables against Community Shaders'.
		struct ConstantTables
		{
			std::uint32_t checks = 0;       // parity frames
			std::uint32_t entries = 0;      // resolved pipeline entries compared
			std::uint32_t uncached = 0;     // of them, not yet in ShaderCache and not compared
			std::uint32_t variables = 0;    // variables in both tables, compared
			std::uint32_t differ = 0;       // of them, at another offset
			std::uint32_t catalogOnly = 0;  // in DCLF's table only, the game's stage does not declare it
			std::uint32_t cacheOnly = 0;    // in ShaderCache's only, DCLF_BINDLESS blocks or never read
			std::string first, catalogOnlyFirst;
		} constantTables;

		// Foliage parity report. It runs on ORG's host thread, so the mutex orders it with the next frame's.
		struct Foliage
		{
			std::uint64_t frames = 0, flagged = 0, inFrameFlagged = 0, whiteFlagged = 0;
			double whiteAverage = -1.0;  // running average of near-white foliage pixels
			std::uint32_t whiteLogged = 0, whiteMax = 0;
			double changeAverage = 0.0;  // running average of reprojected changes
			std::uint32_t changeMax = 0;
			std::array<std::uint64_t, kFoliageCounters> totals{};
			std::uint32_t logged = 0, inFrameLogged = 0;
			std::uint32_t landFrames = 0, landMax = 0, landLogged = 0;
			std::mutex mutex;
		} foliage;

		// Capture point parity per report window. Differing frames and per-binding counts.
		struct CapturePoint
		{
			Reports::Window window;
			std::uint32_t frames = 0;
			ankerl::unordered_dense::map<std::string, std::uint32_t> differ;
		} capturePoint;
	};
}
