#pragma once

#include <cstdint>

#include <winrt/base.h>

namespace DCLF
{
	/**
	 * @brief The game's D3D11 buffers as the render graph sees them (Phase 2).
	 *
	 * Resolves a buffer once through RenderGraphRuntime::DescribeResource, which marks it stable in DXVK
	 * (never relocated or renamed from then on), and keeps a reference on it while it is cached, so the
	 * Vulkan handle and device address stay valid. Entries no table used for kEvictFrames frames are
	 * released. Buffers the game can map cannot be made stable; they are remembered as rejected.
	 *
	 * Render thread only.
	 */
	class GpuResources
	{
	public:
		struct Buffer
		{
			std::uint64_t vkBuffer = 0;  // VkBuffer
			std::uint64_t offset = 0;    // of the D3D11 buffer within vkBuffer
			std::uint64_t size = 0;
			std::uint64_t address = 0;   // device address of the first byte
		};

		struct Stats
		{
			std::uint32_t cached = 0;
			std::uint32_t rejected = 0;
			std::uint32_t resolvedThisFrame = 0;
			double resolveMs = 0.0;  // this frame
			std::uint64_t resolvedTotal = 0;
			double resolveMsTotal = 0.0;
			double resolveMsMax = 0.0;  // slowest single resolution
		};

		static GpuResources& Get();

		/** @brief Whether resolution is possible (the render graph runs on DXVK's device). */
		bool Enabled() const;

		/** @brief The buffer's Vulkan view, resolving it on first use; null if it cannot be made stable. */
		const Buffer* Resolve(ID3D11Buffer* a_buffer);

		/** @brief Once per frame, before the tables are built: releases entries unused for kEvictFrames. */
		void BeginFrame(std::uint32_t a_frame);

		const Stats& GetStats() const { return stats; }

		/** @brief Releases everything (feature off, device teardown). */
		void Clear();

	private:
		static constexpr std::uint32_t kEvictFrames = 600;

		struct Entry
		{
			winrt::com_ptr<ID3D11Buffer> reference;
			Buffer buffer;
			std::uint32_t lastUsed = 0;
			bool stable = false;
		};

		ankerl::unordered_dense::map<ID3D11Buffer*, Entry> entries;
		std::uint32_t frame = 0;
		Stats stats;
	};
}
