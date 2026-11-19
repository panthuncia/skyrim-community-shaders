#pragma once

// Particle snapshots for Features/Particles. Included by the feature's sources only.

#include "Features/DrawcallLimitFix/Api/FrameValues.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

struct ID3D11Buffer;

namespace RE
{
	class BSGeometry;
	class NiParticleSystem;
}

namespace DCLF::Api
{
	class InstallContext;
}

namespace DCLF::Features::Particles
{
	/**
	 * @brief The particle systems DCLF draws (CS_DCLF_PARTICLES), so DCLF never reads a system's particle data after the update.
	 *
	 * The writer: a thunk after each update slot copies the state DCLF needs into the system's record, a triple buffer. This runs
	 * on whatever thread updates the system. The reader: FrameValues' producer takes the newest snapshot and expands it as
	 * FUN_140e51320 does, into five palette rows per particle that Effect.hlsl pulls by vertex index.
	 * The followed-record table is immutable and republished on change. Writers count themselves in (writersInside), so a replaced
	 * table is freed only after the count is seen at zero. Neither side waits, and no engine reference is held.
	 */
	class ParticleSnapshots
	{
	public:
		static ParticleSnapshots& Get();

		/** @brief Installs the writer's thunks in NiParticleSystem's vtable after checking them. Render thread, at install. */
		void Install(Api::InstallContext& a_context);
		bool Installed() const { return installed; }

		// Four corner rows per particle, relative to the model bound's centre with the colour in w, then the subtexture rect.
		static constexpr std::uint32_t kRowsPerParticle = 5;
		// The engine draws at most 0x800 particles a system (FUN_1414f2ad0) with a quad index list of as many quads (FUN_140e4d280).
		static constexpr std::uint32_t kMaxQuads = 0x800;
		static constexpr std::uint32_t kMinQuads = 8;
		// Round capacity up to a quarter octave, not a power of two. With powers of two, large lists held three times their capacity.
		static constexpr std::uint32_t kOctaveSteps = 4;
		static constexpr Api::PaletteRowsContribution::BlockRule kBlockRule{ .rowsPerUnit = kRowsPerParticle, .minUnits = kMinQuads, .maxUnits = kMaxQuads,
			.pow2 = true, .octaveSteps = kOctaveSteps };
		/**
		 * @brief The particle quads' vertex description: only the static texcoord stream at stream 0.
		 * NiParticles' own description puts position and colour on stream 1, which DCLF reserves for face streams.
		 */
		static constexpr std::uint64_t kSlotVertexDesc = 0x0000200000000001ull;
		/** @brief The quads a system of a_capacity particles is drawn with. */
		static std::uint32_t QuadsOf(std::uint32_t a_capacity);
		/** @brief The geometry slot key for a_quads quads: an address of DCLF's own, never an engine object's. */
		static const void* QuadSlotKey(std::uint32_t a_quads);

		/** @brief The renderer's quad buffers (FUN_140e4d280). Null before BeginFrame. */
		struct QuadBuffers
		{
			ID3D11Buffer* vertices = nullptr;
			ID3D11Buffer* indices = nullptr;
		};
		QuadBuffers EngineQuads() const;

		/** @brief The main camera a frame's quads face. */
		struct Camera
		{
			float up[3]{}, right[3]{}, forward[3]{}, eye[3]{};
			bool valid = false;
		};
		/** @brief Reads the frame's main camera, and the renderer's quads once. Render thread, at FrameDriver::BeginFrame. */
		void BeginFrame();
		/** @brief BeginFrame's camera. Render thread. */
		Camera FrameCamera() const { return frameCamera; }

		class Record;
		/** @brief a_geometry's record, followed from now on. Producer only. */
		Record* Follow(const void* a_geometry);
		/** @brief What Expand made of a record's newest snapshot. */
		struct Expanded
		{
			float world[12]{};
			float bound[4]{};
			std::uint32_t particles = 0;
			bool snapshot = false;  // false: no snapshot yet (the block is all degenerate quads, the placement the engine's)
		};
		/** @brief Expands a record's newest snapshot faced to a_camera into a_rows, back to front. One thread per record. */
		void Expand(Record& a_record, const Camera& a_camera, std::uint32_t a_quads, float* a_rows, Expanded& a_out);
		/** @brief Prunes records, publishes a changed table and frees what no writer can hold. Producer, after its items. */
		void EndRun();

		/** @brief Whether the producer expanded a snapshot of a_geometry in its last kCoveredRuns runs. Any thread. */
		bool Covered(const RE::BSGeometry* a_geometry, bool& a_followed) const;
		static constexpr std::uint32_t kCoveredRuns = 3;
		/** @brief A record whose snapshot has not changed for this many runs counts as frozen. */
		static constexpr std::uint32_t kFrozenRuns = 30;

		/** @brief The counters since the last Take. */
		struct Counters
		{
			std::uint64_t captures = 0, contended = 0, aligned = 0, expansions = 0, expandedParticles = 0, withoutSnapshot = 0;
			std::uint64_t published = 0, pruned = 0, unchanged = 0, frozen = 0;
			std::uint32_t followed = 0;
		};
		Counters Take();

		~ParticleSnapshots();

	private:
		ParticleSnapshots();
		struct Table;
		struct UpdateHook;
		struct SelectedHook;

		/** @brief Captures a_system after the engine updated it. Any thread. */
		void Capture(const RE::NiParticleSystem& a_system);
		const Table* PinTable() const;

		bool installed = false;
		std::atomic<const Table*> current{ nullptr };
		mutable std::atomic<std::uint32_t> writersInside{ 0 };
		std::atomic<ID3D11Buffer*> quadVertices{ nullptr }, quadIndices{ nullptr };
		Camera frameCamera;  // render thread

		std::unordered_map<const void*, std::unique_ptr<Record>> records;
		std::vector<std::unique_ptr<const Table>> tables;  // the published one last; the older ones until no writer can hold them
		std::vector<std::unique_ptr<Record>> retired;
		bool dirty = false;
		std::atomic<std::uint32_t> runs{ 0 };

		std::atomic<std::uint64_t> captures{ 0 }, contended{ 0 }, aligned{ 0 }, expansions{ 0 }, expandedParticles{ 0 }, withoutSnapshot{ 0 };
		std::atomic<std::uint64_t> published{ 0 }, pruned{ 0 }, unchanged{ 0 }, frozen{ 0 };
		std::atomic<std::uint32_t> followed{ 0 };  // records.size() at the last run's end (the report's)
	};
}
