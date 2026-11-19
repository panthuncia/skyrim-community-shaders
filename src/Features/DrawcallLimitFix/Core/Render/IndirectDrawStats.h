#pragma once

// The counters IndirectDraws keeps for the reports.

#include <array>
#include <cstdint>
#include <string>

#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	/** @brief Why an epoch did not draw an object candidate. */
	enum class DrawSkip : std::uint32_t
	{
		Pipeline,  // not in the pipeline set yet
		Geometry,  // no stable vertex / index buffer
		Texture,   // a texture the shaders read is not resolved
		Sampler,
		Constants,  // a constant buffer the shaders read is not available
		Capacity,
		NotInSet,  // bound but not in this frame's DCLF set, so the engine draws it
		CandidateOnly,       // kept for the GPU culling to reject, but not drawable, so it has no bindings
		Count
	};

	/** @brief The main epochs' counters, per report interval unless noted. */
	struct IndirectDrawStats
	{
		std::uint32_t epochs = 0;
		std::uint32_t drawn = 0;  // last epoch
		std::array<std::uint32_t, static_cast<std::size_t>(DrawSkip::Count)> skipped{};
		std::array<std::uint32_t, 4> missingTextures{};  // registers of the last unresolved textures (diagnostics)
		std::uint32_t missingVertexConstants = 0;       // constant buffer registers without an address (bits)
		std::uint32_t missingPixelConstants = 0;
		std::uint32_t records = 0;                       // binding records built, last epoch
		std::uint64_t uploadBytes = 0;                   // last epoch
		double cpuMs = 0.0;                              // last epoch, assembly and epoch
		// Parts 0-2 are per binding record, not per draw.
		std::array<double, 8> partMs{};
		// The render thread's part of the main epochs, summed since the last report, by stage.
		std::array<double, 7> commitUs{};
		std::uint32_t commitEpochs = 0;
		std::uint32_t notReady = 0;            // epochs skipped (mirrors, targets or pipelines not ready)
		std::uint32_t shortBuffers = 0;        // draws whose vertex or index slice does not cover them
		// Draws skipped because a material's textures were not in the lookups yet. They land next frame. Steady state is 0.
		std::uint32_t deferredTextures = 0;
		// Frame textures (t16 up) a record reads that the pass had not bound at the commit, last epoch.
		std::uint32_t frameTexturesMissing = 0;
		std::array<std::uint64_t, 2> frameTexturesMissingRegisters{};  // which registers, as a bit set
		std::uint32_t cullDrawn = 0;     // sequences BuildDraws wrote, last sampled epoch
		std::uint32_t cullRejected = 0;  // draws its culling rejected (CS_DCLF_CULL)
		std::uint32_t cullTested = 0;    // draws it tested at all: 0 means the culling did not run
		std::uint32_t cullOccluded = 0;         // rejected by the HZB rather than by the frustum
		// Phase two re-tests what phase one rejected against the rebuilt HZB.
		std::uint32_t cullDrawnPhaseTwo = 0;      // depth draws it added
		std::uint32_t cullRescuedByPhaseTwo = 0;  // objects it brought back
		std::uint32_t cullRetests = 0;  // the inputs phase 1 left OccludedRetest
		// Decals (CS_DCLF_DECALS) submitted to the second colour pass, and what its single-phase culling did with them.
		std::uint32_t decalsDrawn = 0;
		std::uint32_t decalsCulled = 0;
		std::uint32_t decalsTested = 0;
		// The GPU sun test's tested and missed counts, against the same test run on the CPU.
		std::uint32_t sunTested = 0, sunMissed = 0, sunCpuTested = 0, sunCpuMissed = 0;
		// Terrain inputs within Terrain Blending's near distance, against the CPU's test (CS_DCLF_SET_PARITY).
		std::uint32_t terrainNearTested = 0, terrainNear = 0, terrainCpuTested = 0, terrainCpuNear = 0;
		bool terrainCpu = false;
		// Forward inputs through the culling. forwardEngine counts blended passes the engine handed in under CS_DCLF_SET_PARITY.
		std::uint32_t forwardTested = 0, forwardInView = 0, forwardOccluded = 0, forwardDrawn = 0, forwardEngine = 0;
		bool forwardObserved = false;
		// Refraction inputs through the culling. refractionEngine counts hint 4 passes the engine handed in under CS_DCLF_SET_PARITY.
		std::uint32_t refractionTested = 0, refractionInView = 0, refractionOccluded = 0, refractionDrawn = 0, refractionEngine = 0;
		bool refractionObserved = false;
		// Colour draws dropped for lack of a state class sub-range, either unplanned or past its capacity.
		std::uint32_t classUnplanned = 0, classOverflow = 0;
		// Inputs found under a fade root, and those dropped past their fade-out distance.
		std::uint32_t fadeTested = 0, fadeHidden = 0;
		std::uint32_t fadeRoots = 0;  // the fade roots listed
		// Persistent resident draws. Last epoch, except the resync counts, which run since the start.
		std::uint32_t residentInputs = 0, residentDraws = 0, residentPairs = 0, residentUndrawable = 0;
		std::uint64_t residentEpochs = 0, residentResyncs = 0;
		std::array<std::uint64_t, 5> residentResyncBy{};  // since the start, by reason (MainBuild::UpdateRegionEntries)
		std::uint64_t residentDecalRetakes = 0;           // since the start: decal group counts moved (decals re-taken, no resync)
		std::array<std::uint64_t, 2> residentLastEpoch{};
		std::uint32_t extraRows = 0;  // extras rows uploaded by the last epoch
		// All near or all far means the HZB build is wrong.
		std::uint32_t hzbNear = 0, hzbFar = 0, hzbSampled = 0;
		// One rejection in full, so an implausible count can be read instead of guessed at.
		struct HzbSample
		{
			bool valid = false;
			float farthest = 0.0f, nearestZ = 0.0f;
			float uvMin[2]{}, uvMax[2]{};
			std::uint32_t mip = 0;
		} hzbSample;
		// Pairs whose pipeline's constant tables differ from the ones their material row was packed with. They stay native.
		std::uint32_t rowTableConflicts = 0;
		// Scene lane builds per job kind, per report interval.
		struct Async
		{
			// Per epoch, commits of a payload built ahead.
			std::uint32_t used = 0;
		};
		std::array<Async, 3> async{};
	};

	/** @brief Why a shadow view offered to the epoch was not drawn. */
	enum class ShadowViewNotReady : std::uint32_t
	{
		Setup,      // no render graph, no Utility shader, or the shadow resources could not be created
		Pipelines,  // no shadow pipeline in the set yet
		Tables,     // the scene phase has not built this frame's tables, or the view is unknown
		Depth,      // the engine's shadow map could not be imported, or the view draws into an unknown target
		Epoch,      // the epoch itself failed
		Placement,  // the view drew where no placement is (target, slice, viewport), under another state, or twice: a defect
		RasterState,  // the view's rasterizer state is one no pipeline expresses (wireframe, or no depth clip without depth clamp)
		Count
	};

	/** @brief Counters for the shadow views and occlusion maps. */
	struct ShadowDrawStats
	{
		std::uint32_t views = 0;           // views offered (captured by the hook), per report interval
		std::uint64_t sunEntryChecks = 0, sunEntryMismatches = 0;  // CS_DCLF_PERSISTENT_PARITY, GPU sun-entry test vs CPU verdict
		std::uint32_t viewsDrawn = 0;      // views drawn by an epoch
		std::uint32_t narrowedEpochs = 0;  // epochs that drew a variant recording only the placements in use (ShadowFrame::activeViews)
		std::uint32_t retainedViews = 0;   // view slots kept in an epoch's shape with no work (a view that comes and goes)
		std::uint32_t epochs = 0;          // shadow epochs run (one per frame with views)
		std::uint32_t notReady = 0;        // views skipped: resources, pipelines or the depth import not ready
		std::uint32_t notReadyWithheld = 0;  // of those, views whose casters the engine withheld this frame: holes
		std::array<std::uint32_t, static_cast<std::size_t>(ShadowViewNotReady::Count)> notReadyReasons{};
		std::uint32_t focusSkipped = 0;    // focus views, left native
		std::uint32_t uncovered = 0;  // past DCLF's view slots or of a mode unknown at Rebuild, left native
		std::uint32_t faceUploads = 0;        // face position regions uploaded (a head's snapshot changed), per report interval
		// GPU culling counters of one sampled view, read back a few frames after its epoch.
		std::uint32_t cullDrawn = 0, cullRejected = 0, cullTested = 0;
		// The sampled view's caster inputs and the ones dropped before culling, by reason.
		std::uint32_t cullClass = 0, cullSunEntryOut = 0, cullMinRadius = 0, cullRootFading = 0, cullBelowTexels = 0, cullRowsLost = 0;
		std::uint32_t cullSampledView = ~0u, cullSampledMode = 0;
		// Casters and triangles drawn in the sampled view, by bound radius class.
		std::array<std::uint32_t, 4> probeCasters{}, probeTriangles{};
		// Casters per render mode at the last epoch, and set members a mode could not draw. The latter is a defect.
		std::array<std::uint32_t, 3> casters{};
		std::uint64_t setWaiting = 0;
		std::string setWaitingFirst;
		std::uint32_t inputs = 0;          // casters submitted over the modes the last shadow commit used
		std::uint32_t skippedPipeline = 0; // casters without a ready shadow pipeline, last view
		std::uint32_t skippedTexture = 0;  // alpha-tested casters whose diffuse could not be resolved, last view
		std::uint32_t deferredTextures = 0;   // of those, ones whose diffuse the lookups had not resolved yet (next frame)
		std::uint32_t deferredPipelines = 0;  // casters whose shadow pipeline the lookups had no entry for yet
		std::uint32_t records = 0;         // material rows, last frame
		double cpuMs = 0.0;                // per report interval, all views
		double captureMs = 0.0;            // per report interval, the hooks' captures
		double prepareMs = 0.0;            // of which the once-per-frame preparation
		double inputsMs = 0.0;             // of which building (per mode) and uploading the inputs
		double blocksMs = 0.0;             // of which the view's constant blocks and their uploads
		double executeMs = 0.0;            // of which the graph's own execution (compile, prepare, record)
		// Occlusion maps per occlusion view and report interval.
		std::array<std::uint32_t, kOcclusionViews> occlusionDrawn{};     // maps DCLF drew
		std::array<std::uint32_t, kOcclusionViews> occlusionNotReady{};  // maps taken at the shadow maps that the epoch could not draw
		std::array<std::uint32_t, kOcclusionViews> occlusionInputs{};    // occluders, last map
		double occlusionMs = 0.0;                                          // render thread, their latches in the shadow commits
		std::uint32_t occlusionEpochs = 0;                                 // shadow epochs that drew a map
	};

	inline constexpr std::array<const char*, 8> kEpochPartNames{
		"textures/samplers", "constant groups", "record push", "per-draw tail", "per-draw prologue",
		"uploads", "epoch prologue", "graph execute"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(ShadowViewNotReady::Count)> kShadowNotReadyNames{
		"setup", "pipelines", "tables", "depth", "epoch", "placement", "raster-state"
	};

	inline constexpr std::array<const char*, static_cast<std::size_t>(DrawSkip::Count)> kSkipNames{ "pipeline", "geometry", "texture", "sampler",
		"constants", "capacity", "not-in-set", "candidate-only" };
}
