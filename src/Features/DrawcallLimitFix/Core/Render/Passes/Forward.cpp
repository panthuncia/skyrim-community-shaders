// The forward sort for the colour epoch and the views, and the frame slot addresses that draw passes outside IndirectDraws push.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/PassesInternal.h"

namespace DCLF::Draws
{
	/*
	 * The colour BuildDraws appends visible forward draws with their view depth to their class's staging half. ForwardSortPass
	 * sorts each class back to front into its sorted half, which Features/Forward draws after the deferred composite.
	 */
	namespace
	{
		struct ForwardSortBindings
		{
			org::DeclaredViewToken sequences, count;
		};

		// A dispatch per state class with a forward sub-range.
		struct ForwardSortFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			std::vector<ForwardSortConstants> constants;
			std::vector<std::uint32_t> groups;
		};

		class ForwardSortPass final : public org::TypedRenderGraphPass<ForwardSortPass, ForwardSortFrame, ForwardSortBindings>
		{
		public:
			ForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source) :
				sequences(std::move(a_sequences)), counts(std::move(a_counts)), source(std::move(a_source)) {}

			ForwardSortBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.UnorderedAccess(*sequences).View(),
					counts.versioned ? a_builder.ShaderResource(*counts.versioned).View() : a_builder.ShaderResource(counts.buffer).View() };
			}

			void Revision(std::vector<std::uint64_t>&) const {}

			ForwardSortFrame Prepare(const ForwardSortBindings&, const org::PassPrepareContext& a_preparation) const
			{
				ForwardSortFrame prepared{};
				const auto sorted = source(a_preparation);
				if (!sorted.program || sorted.ranges.empty())
					return prepared;
				prepared.program = sorted.program;
				for (const auto& range : sorted.ranges) {
					if (!range.capacity)
						continue;
					ForwardSortConstants constants{};
					constants.stagingBase = range.stagingBase;
					constants.capacity = range.capacity;
					constants.sequenceStride = static_cast<std::uint32_t>(sizeof(DrawSequence));
					constants.countOffset = range.countOffset;
					constants.options = ForwardSortConstants::kCountOffset | range.options;
					prepared.constants.push_back(constants);
					// Full capacity. Threads past the count return.
					prepared.groups.push_back((range.capacity + kForwardSortGroup - 1) / kForwardSortGroup);
				}
				return prepared;
			}

			static void Record(const ForwardSortBindings& a_bindings, const ForwardSortFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || a_frame.constants.empty())
					return;
				const std::uint32_t sequencesIndex = a_recording.Index(a_bindings.sequences);
				const std::uint32_t countIndex = a_recording.Index(a_bindings.count);
				for (std::size_t r = 0; r < a_frame.constants.size(); ++r) {
					auto constants = a_frame.constants[r];
					constants.sequencesIndex = sequencesIndex;
					constants.countIndex = countIndex;
					RecordDispatch(a_recording.Commands(), *a_frame.program, constants, a_frame.groups[r]);
				}
			}

		private:
			Versioned sequences;
			LatchedTarget counts;
			std::function<ForwardSortSource(const org::PassPrepareContext&)> source;
		};
	}

	std::shared_ptr<org::RenderPass> MakeForwardSortPass(Versioned a_sequences, LatchedTarget a_counts, std::function<ForwardSortSource(const org::PassPrepareContext&)> a_source)
	{
		return std::make_shared<ForwardSortPass>(std::move(a_sequences), std::move(a_counts), std::move(a_source));
	}

	std::array<std::uint32_t, kFramePushWords> MainFramePushWords(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef)
	{
		return FramePushWords(a_frameConstants, a_frameConstantsRef);
	}

	void MainShadowLayoutPush(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef, std::uint32_t (&a_out)[kShadowPushWords])
	{
		auto split = [](std::uint32_t* a_words, std::uint64_t a_value) {
			a_words[0] = static_cast<std::uint32_t>(a_value);
			a_words[1] = static_cast<std::uint32_t>(a_value >> 32);
		};
		const std::uint64_t constants = a_frameConstants;
		split(a_out + kShadowPushFrameRecord, constants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes);
		const RenderGraph::BufferRef recordRef = a_frameConstantsRef + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes;
		a_out[kShadowPushFrameRecordRef] = recordRef.viewSlot;
		a_out[kShadowPushFrameRecordRef + 1] = recordRef.byteOffset;
		RenderGraph::CheckBufferRef(recordRef, constants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes, kFrameSlotBytes, "main shadow layout frame record");
		split(a_out + kShadowPushViewBlock, constants + FrameSlotOffset(false, 0));
		split(a_out + kShadowPushPerFrame, constants + FrameSlotOffset(false, kPerFrameVertexRegister));
		split(a_out + kShadowPushZeros, constants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		split(a_out + kShadowPushSharedData, constants + FrameSlotOffset(false, kSharedDataRegister));
		split(a_out + kShadowPushFeatureData, constants + FrameSlotOffset(false, kFeatureDataRegister));
	}

	void MainShadowLayoutViews(RenderGraph::BufferRef a_frameConstantsRef, const FrameBlockViews& a_blocks, std::uint32_t (&a_out)[kShadowPushWords])
	{
		auto ref = [&](std::uint32_t a_word, RenderGraph::BufferRef a_ref) {
			a_out[a_word] = a_ref.viewSlot;
			a_out[a_word + 1] = a_ref.byteOffset;
		};
		ref(kShadowPushViewBlock, a_frameConstantsRef + FrameSlotOffset(false, 0));
		ref(kShadowPushPerFrame, a_frameConstantsRef + FrameSlotOffset(false, kPerFrameVertexRegister));
		ref(kShadowPushZeros, a_frameConstantsRef + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		// A block's view is its ordinal in the frame push words' order.
		auto view = [&](std::uint32_t a_word, std::uint32_t a_register) {
			a_out[a_word] = a_blocks.indices[(FramePushWord(false, a_register) - kFramePushRegisters) / 2];
			a_out[a_word + 1] = 0;
		};
		view(kShadowPushSharedData, kSharedDataRegister);
		view(kShadowPushFeatureData, kFeatureDataRegister);
	}

	std::uint64_t LodFadeFrameAddress(std::uint64_t a_frameConstants)
	{
		return a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting);
	}
}
