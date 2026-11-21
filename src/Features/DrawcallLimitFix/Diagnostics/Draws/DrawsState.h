#pragma once

// DrawsState: the draw observers' state between hook points. Render thread unless noted.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "DrawsView.h"

namespace DCLF::Diagnostics
{
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
		WrappedVersion setParityVisibility, setParityFrustum;
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

		// Resident slots whose shading row no producer sampled (written 0) at two checks running.
		struct FrameValuesParity
		{
			std::uint64_t shadingChecks = 0, shadingUnsampled = 0;
			std::string shadingFirst;
		} frameValues;
		// The resident slots unsampled at the last check (kept across the report's reset of frameValues).
		std::vector<std::uint8_t> shadingUnsampledBefore;

		// Kept store checks, each run on its store's thread.
		struct Kept
		{
			ParityCounter objects, geometry, drawBound, shadow;
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
		std::uint32_t fadeInputsVaried = 0;
		bool cullingSetupLogged = false;
		std::array<std::uint32_t, 2> epochLines{};  // colour, Z-prepass
	};
}
