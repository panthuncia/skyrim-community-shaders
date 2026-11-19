#pragma once
#include "RenderGraph/BufferRef.h"
#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/BufferRefs.h"
#endif

// The main pass's capture and the layout of the per-frame constant blocks at their frame slots, with the push words that name them.

namespace DCLF::Draws
{
	// The main pass's bindings where its opaque batches start, or the depth pass's.
	struct Capture
	{
		std::array<ID3D11Buffer*, kConstantBufferRegisters> vsBuffers{};
		std::array<ID3D11Buffer*, kConstantBufferRegisters> psBuffers{};
		std::array<ID3D11ShaderResourceView*, kTextureRegisters> psViews{};
		winrt::com_ptr<ID3D11Texture2D> depth;
		std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> targets;
		std::uint32_t targetCount = 0;
		std::uint32_t viewportWidth = 0, viewportHeight = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
		// World transforms are stored relative to this camera. The engine has moved the camera on by the time the epoch packs the constants.
		RE::NiPoint3 eye, previousEye;

		void Release()
		{
			for (auto* buffer : vsBuffers)
				if (buffer)
					buffer->Release();
			for (auto* buffer : psBuffers)
				if (buffer)
					buffer->Release();
			for (auto* view : psViews)
				if (view)
					view->Release();
		}
	};

	Capture CaptureBindings();

	// The CPU-written structured buffers the main pass binds at t16 and up, minus the frame graph buffers, which come from the graph.
	std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, std::uint32_t a_graphBuffers);

	inline ID3D11Buffer* BufferOf(ID3D11ShaderResourceView* a_view)
	{
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		return static_cast<ID3D11Buffer*>(resource.get());  // the view keeps it alive
	}

	// A growing byte arena of 256-byte aligned constant blocks.
	class ConstantArena
	{
	public:
		void Reset(std::uint64_t a_capacity)
		{
			bytes.clear();
			capacity = a_capacity;
		}

		// Offset of a zeroed block, or ~0 when the arena is full.
		std::uint64_t Allocate(std::size_t a_size)
		{
			const std::uint64_t offset = (bytes.size() + kConstantAlignment - 1) & ~(kConstantAlignment - 1);
			if (offset + a_size > capacity)
				return ~0ull;
			bytes.resize(offset + std::max<std::size_t>(a_size, 16));
			return offset;
		}

		std::span<std::byte> At(std::uint64_t a_offset, std::size_t a_size) { return { bytes.data() + a_offset, a_size }; }
		const std::vector<std::byte>& Bytes() const { return bytes; }

	private:
		std::vector<std::byte> bytes;
		std::uint64_t capacity = 0;
	};

	// The per-frame constant blocks live at fixed slots so a record can name a block before its contents exist.
	// A slot is D3D11's constant buffer maximum, so no block overflows.
	constexpr std::uint64_t kFrameSlotBytes = 65536;
	constexpr std::uint32_t kFrameSlotSharedLight = 2 * kConstantBufferRegisters;
	constexpr std::uint32_t kFrameSlotLighting = kFrameSlotSharedLight + 1;
	// The shared light slot holds two blocks. The first starts with the zero block the main shadow layout reads.
	// The second is every bindless draw's PS b3, either Light Limit Fix's zeroed StrictLightData or DCLF's own point light table.
	constexpr std::uint64_t kSharedLightBlockBytes = kFrameSlotBytes / 2;
	constexpr std::uint64_t kSharedLightBlockOffset = std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes + kSharedLightBlockBytes;
	// The frame record holds every register a draw's rows do not give. Each commit writes it.
	constexpr std::uint32_t kFrameSlotRecord = kFrameSlotLighting + 1;
	constexpr std::uint32_t kFrameSlotCount = kFrameSlotRecord + 1;
	constexpr std::uint64_t kFrameConstantBytes = std::uint64_t(kFrameSlotCount) * kFrameSlotBytes;

	constexpr std::uint64_t FrameSlotOffset(bool a_pixelStage, std::uint32_t a_register)
	{
		return (std::uint64_t(a_pixelStage ? kConstantBufferRegisters : 0u) + a_register) * kFrameSlotBytes;
	}
	// The lighting slot's rows (PS b13): frame lighting c0-c5, LOD fades c6-c13, foliage record c14-c15, extras c16.
	constexpr std::uint64_t kExtrasPixelFrameOffset = std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting) + sizeof(LodFadeFrame) + 32;
	static_assert(sizeof(FrameLighting) + sizeof(LodFadeFrame) + 32 == 16 * 16);
	// The record and every slot's block are read through 16-byte-aligned pointers on Vulkan's descriptor-buffer model.
	static_assert(kFrameSlotBytes % 256 == 0 && sizeof(FrameLighting) % 16 == 0);
	// Core's frame constant rows follow from c17, 16 bytes each. Uploads.cpp checks the first comes right after the extras' row.
	constexpr std::uint64_t FrameConstantRowOffset(std::uint32_t a_row) { return std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + std::uint64_t(a_row) * 16; }
	static_assert(sizeof(FrameFog) == 3 * 16);

	/** @brief The frame push word of a stage's constant buffer register, or ~0u when it is not pushed. */
	inline std::uint32_t FramePushWord(bool a_pixel, std::uint32_t a_register)
	{
		std::uint32_t word = kFramePushRegisters;
		for (const bool pixel : { false, true }) {
			const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
				if (!((mask >> r) & 1))
					continue;
				if (pixel == a_pixel && r == a_register)
					return word;
				word += 2;
			}
		}
		return ~0u;
	}

	/** @brief A pass-wide block's offset in the frame constants. */
	constexpr std::uint64_t FrameBlockOffset(bool a_pixel, std::uint32_t a_register)
	{
		if (a_pixel && a_register == 3)
			return kSharedLightBlockOffset;
		if (a_pixel && a_register == kFrameLightingRegister)
			return std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
		return FrameSlotOffset(a_pixel, a_register);
	}

	/** @brief Calls a_visit(pixel, register, block) for each pass-wide block, in push word order. */
	template <class F>
	void ForEachFrameBlock(F&& a_visit)
	{
		std::uint32_t block = 0;
		for (const bool pixel : { false, true }) {
			const std::uint32_t mask = pixel ? kFramePushPS : kFramePushVS;
			for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r)
				if ((mask >> r) & 1)
					a_visit(pixel, r, block++);
		}
	}

	inline std::array<std::uint32_t, kFramePushWords> FramePushWords(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef)
	{
		std::array<std::uint32_t, kFramePushWords> words{};
		ForEachFrameBlock([&](bool a_pixel, std::uint32_t a_register, std::uint32_t a_block) {
			const std::uint64_t address = a_frameConstants + FrameBlockOffset(a_pixel, a_register);
			words[kFramePushRegisters + 2 * a_block] = static_cast<std::uint32_t>(address);
			words[kFramePushRegisters + 2 * a_block + 1] = static_cast<std::uint32_t>(address >> 32);
		});
		const std::uint64_t record = a_frameConstants + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes;
		words[kFramePushRecord] = static_cast<std::uint32_t>(record);
		words[kFramePushRecord + 1] = static_cast<std::uint32_t>(record >> 32);
		const RenderGraph::BufferRef recordRef = a_frameConstantsRef + std::uint64_t(kFrameSlotRecord) * kFrameSlotBytes;
		words[kFramePushRecordRef] = recordRef.viewSlot;
		words[kFramePushRecordRef + 1] = recordRef.byteOffset;
#if defined(CS_HAS_RENDER_GRAPH)
		// CS_DCLF_REF_CHECK. The stages reach the record and every row through these two words.
		RenderGraph::CheckBufferRef(recordRef, record, kFrameSlotBytes, "frame record (frame push words 2-3)");
#endif
		return words;
	}

	/** @brief The uniform buffer views of one frame constants buffer's pass-wide blocks. The views retire with the buffer. */
	class FrameBlockViews
	{
	public:
		/** @brief Render thread. Returns null, and logs, when a slot or view cannot be made. */
		static std::shared_ptr<const FrameBlockViews> Create(rhi::Device a_device, const std::shared_ptr<org::Buffer>& a_frameConstants);
		/** @brief A block a view covers. The offset is 256-aligned and the size a multiple of 256. */
		struct Block
		{
			std::uint64_t offset = 0;
			std::uint32_t bytes = 0;
		};
		/** @brief Views of other blocks of a_buffer, such as the D3D12 shadow views' SharedData and FeatureData. Null when one cannot be made. */
		static std::shared_ptr<const FrameBlockViews> CreateFor(rhi::Device a_device, const std::shared_ptr<org::Buffer>& a_buffer, std::span<const Block> a_blocks,
			const char* a_what);
		FrameBlockViews() = default;
		FrameBlockViews(const FrameBlockViews&) = delete;
		FrameBlockViews& operator=(const FrameBlockViews&) = delete;
		~FrameBlockViews();

		std::array<std::uint32_t, kFrameBlocks> indices{};

	private:
		std::array<rhi::DescriptorSlot, kFrameBlocks> slots{};
		std::uint32_t made = 0;
		std::shared_ptr<org::runtime::IDescriptorService> service;
		std::shared_ptr<org::Buffer> buffer;
	};

	// The blocks the render thread packs into the frame slots for one epoch.
	struct FrameBlocks
	{
		std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
		std::uint32_t vsMask = 0, psMask = 0;  // the slots supplied
	};
}
