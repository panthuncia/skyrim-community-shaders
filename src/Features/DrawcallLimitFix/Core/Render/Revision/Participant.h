#pragma once

#include "Utils/Hash.h"

// The scene revision participants, one per ShapeKind. Each owns its kind's part of making a revision, posting inputs and building
// payloads ahead. They are always called in ShapeKind order, once per make, post or build, never per object.

namespace DCLF
{
	/** @brief One revision make. Holds the publication and what the main participant's requirements pass on to the others. */
	struct RevisionMake
	{
		ImplProducer& producer;
		const ScenePublication& publication;
		const SceneTables& tables;
		const Lookups& lookups;
		const PipelineCatalog* catalog = nullptr;
		IndirectState indirect;
		std::array<FamilyPipelines, Extensions::kDrawFamilies> familyPipelines;
		std::array<bool, Extensions::kDrawFamilies> familyOn{};
		std::array<bool, kViews> viewOn{};
		std::array<ZBucketPlan, 2> zPlans;
		std::shared_ptr<const SetClassPlan> classPlan;
		std::array<std::vector<LatchedCopy>, 2> mainLatched;
		// One hash over the pipeline slot lookups that the requirements and shapes read.
		std::uint64_t lookupsWitness = 0;
	};

	namespace Draws
	{
		/** @brief Require a latched-copies block for a_copies. Nothing is required when a_copies is empty. */
		void RequireLatchedBlock(GrowthBuild& a_build, LatchHandle a_latch, const std::vector<LatchedCopy>& a_copies);

		/** @brief Mixes values into the current group's word of the shapes key. */
		struct ShapesKeyMixer
		{
			ImplProducer::ShapesKey& key;
			std::size_t group = 0;
			void Mix(std::uint64_t a_value)
			{
				auto& h = key[group];
				h = (h ^ a_value) * Util::Fnv1a::kPrime;
				h ^= h >> 29;
			}
			void Ptr(const void* a_pointer) { Mix(reinterpret_cast<std::uintptr_t>(a_pointer)); }
			template <class T>
			void Raw(const T& a_value)
			{
				static_assert(std::is_trivially_copyable_v<T>);
				std::array<std::byte, (sizeof(T) + 7) / 8 * 8> bytes{};
				std::memcpy(bytes.data(), &a_value, sizeof(T));
				for (std::size_t b = 0; b < bytes.size(); b += 8) {
					std::uint64_t word;
					std::memcpy(&word, bytes.data() + b, 8);
					Mix(word);
				}
			}
		};
	}

	/** @brief What a participant builds its payload from in a build ahead, and the publication it goes into. */
	struct AheadBuild
	{
		ImplSnapshotBuilder::DrawPublication& out;
		const ImplSnapshotBuilder::AheadContext& context;
		const SceneTables& tables;
		const Lookups& lookups;
		std::uint32_t generation = 0, frame = 0;
		const std::shared_ptr<const EntrySpace>& sunCandidates;
		const std::shared_ptr<const EntrySpace>& lightCandidates;
		const TablesContentEpochs& oldestTables;
		const PayloadFloors& payloads;  // oldest journal position each payload pool's backing still holds
		ResidentRegion& mainRegion;
		MainRows::PairStats& stats;
	};

	class RevisionParticipant
	{
	public:
		/** @brief Require the buffers and latch layout this kind's shapes need. Views run last because they read the main layout. */
		virtual void RequireSequences(RevisionMake& a_make, GrowthBuild& a_build) = 0;
		/** @brief Require the latched-copies blocks. Runs after every participant's RequireSequences. */
		virtual void RequireLatched(RevisionMake&, GrowthBuild&) {}
		/** @brief Called after the set is sealed and before the shapes key, for example to note a bound the shapes outgrew. */
		virtual void NoteSealed(RevisionMake&) {}
		/** @brief Mix this kind's parts into the shapes key. */
		virtual void MixShapesKey(const RevisionMake& a_make, Draws::ShapesKeyMixer& a_mixer) const = 0;
		/** @brief Make this kind's shapes from the sealed set and the main layout. */
		virtual void MakeShapes(RevisionMake& a_make, const NamedVersions& a_sealed, const MainLayout& a_layout) = 0;
		/** @brief Render thread. Post this kind's part of the revision inputs. */
		virtual void PostInputs(RevisionInputs&) {}
		/** @brief Render thread, at frame start. Fill this kind's part of the context for the next builds ahead. */
		virtual void PrepareAhead(ImplSnapshotBuilder::AheadContext&) {}
		/**
		 * @brief The part of the payload that needs no main build. It runs beside the main build, so it must not touch mainRegion or
		 * write into a_build.out. BuildPayload of the same build always follows.
		 */
		virtual void BuildPayloadShared(const AheadBuild&) {}
		/** @brief Build this kind's payload into a_build.out, after the main build. */
		virtual void BuildPayload(const AheadBuild&) {}

	protected:
		~RevisionParticipant() = default;
	};
}
