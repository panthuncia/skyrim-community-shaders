#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <source_location>
#include <span>
#include <string>
#include <vector>

#include "RenderGraph/Async/EventQueue.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	/**
	 * @brief Shader-visible descriptors for the game's textures and samplers.
	 *
	 * Each D3D11 view is imported once into BasicRHI without ownership and written into ORG's shader-visible heap.
	 * Every index handed to a draw comes with ownership of its descriptor and view until the GPU is done with it.
	 *
	 * An import can take milliseconds while the game streams textures, because it waits on DXVK's CS thread and the driver.
	 * So Request never imports on the calling thread. The import thread answers it, and a draw still waiting stays the engine's.
	 * ResolveBinding imports inline for the frame's own textures, whose draws cannot be deferred.
	 *
	 * With pinless imports DXVK may move an image. Each import is registered with Relocation::DxvkSource. A move supersedes the old
	 * import, whose registration keeps the old storage alive until its last owner goes, and the next lookup imports the new storage.
	 */
	class GpuTextures
	{
	public:
		static constexpr std::uint32_t kInvalid = ~0u;

		enum class Reject : std::uint32_t
		{
			None,
			NotImage,
			Import,
			View,
			Count
		};

		struct Stats
		{
			std::uint32_t cached = 0;
			std::uint32_t registrySlots = 0;
			std::uint64_t cleanupPending = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Reject::Count)> rejected{};
			std::uint32_t samplers = 0;
			std::uint32_t unsupportedLayouts = 0;  // images in a layout BasicRHI cannot import
			std::uint64_t importedAsync = 0;   // imports the import thread made
			std::uint32_t requestsPending = 0;
			std::uint64_t requestsAnswered = 0;
			std::uint64_t superseded = 0;        // pinless: imports a relocation superseded
			std::uint32_t pinned = 0;            // pinless: imports DXVK pinned all the same (images the GPU writes)
		};
		/** @brief CS_DCLF_STATS: the import thread's times since the last take, in ms. */
		struct ImportTimes
		{
			float queuedMax = 0;  // a request's wait for the import thread
			float answerMax = 0;  // one answer's work
			std::vector<std::string> slowest;  // answers of 20 ms or more, with their steps
		};
		/** @brief Render thread, on report frames. */
		ImportTimes TakeImportTimes();
		struct Binding
		{
			std::uint32_t index = kInvalid;
			std::shared_ptr<const void> owner;
			bool pending = false;  // asked for and not answered yet
			explicit operator bool() const { return index != kInvalid; }
		};
		/** @brief Request's answer. Never pending. A rejected view gets kInvalid with no owner. */
		struct Reply
		{
			ID3D11ShaderResourceView* view = nullptr;  // an identity only, the reference is gone
			std::uint64_t cookie = 0;
			Binding binding;
		};
		using Replies = RenderGraph::Frame::EventQueue<Reply, 1024>;
		static constexpr std::uint32_t kSamplerCount = 4 * 5;  // the engine's (address mode, filter mode) table
		/** @brief Bindings that never change once made: the null descriptor and the engine's samplers by address * 5 + filter. */
		struct FixedBindings
		{
			Binding null;
			std::array<Binding, kSamplerCount> samplers{};
		};

		/** @brief Who first asked for a view. Diagnostic only. */
		enum class Source : std::uint32_t
		{
			Material = 0,  // + the slot
			Feature = 16,  // + its index
			Projected = 32,
			Mask = 48,
			FrameSlot = 64,  // + N
			FamilyHeader = 96,  // + the header's index
			FamilyNull = 120,  // + its ViewKind
			ShadowDiffuse = 192,
			ImGui = 256,
			Scaleform = 257,
			CloudMaps = 258,
			PhysicalSky = 259,
			VolumetricClouds = 260,
			DeferredComposite = 261,
		};

		static constexpr std::uint32_t Tag(Source a_source, std::uint32_t a_offset = 0) { return static_cast<std::uint32_t>(a_source) + a_offset; }

		/**
		 * @brief A shader resource's declared dimension. Empty slots need a null view of the right kind.
		 * A cube, 3D or array read through a 2D descriptor is undefined, and AMD loses the device on it.
		 */
		enum class ViewKind : std::uint8_t
		{
			Texture2D,
			Texture2DArray,
			Texture3D,
			Cube,
			Buffer,
		};

		/** @brief Why ResolveBinding gave no binding. */
		enum class ResolveFailure : std::uint8_t
		{
			None,
			Rejected,  // the status counts why
			Pending,  // a later call takes it
			StreamedWindow,  // full window not loaded yet, a later call takes it
			NoContext,
		};

		static GpuTextures& Get();

		/** @brief Who asked for the superseded imports still alive, each holding an old storage. */
		std::string DescribeSuperseded() const;

		/** @brief Resolve with ownership of this exact import and descriptor slot, importing inline if needed. a_sourceTag is diagnostic only. */
		Binding ResolveBinding(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag = ~0u, ResolveFailure* a_failure = nullptr,
			std::source_location a_where = std::source_location::current());

		/**
		 * @brief ResolveBinding for a holder that follows a streamed texture's window: it imports the window there is now, never
		 * waiting for the full one. The import is superseded when the window changes (RelocatedViews names it, Superseded says so),
		 * and the holder resolves again then.
		 */
		Binding ResolveFollowingWindow(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag = ~0u, ResolveFailure* a_failure = nullptr,
			std::source_location a_where = std::source_location::current());

		/** @brief Render thread. A zero-reading binding of a_kind for an empty slot, or kInvalid if it could not be made. */
		Binding NullOf(ViewKind a_kind);

		/** @brief Render thread. The 1x1 zero D3D11 view of a_kind that NullOf imports, or null for a buffer or when it could not be made. */
		static ID3D11ShaderResourceView* NullView(ViewKind a_kind);

		/** @brief The words DescribeFailure ends with for a texture still on its way (importing, or streaming its window in). */
		static constexpr std::string_view kLaterBatch = "a later batch takes it";

		/** @brief One log line describing a refusal. */
		static std::string DescribeFailure(ID3D11ShaderResourceView* a_view, std::uint32_t a_slot, ResolveFailure a_failure);

		/** @brief A source tag in words, for logs. */
		static std::string SourceName(std::uint32_t a_tag);

		/**
		 * @brief Any thread. Asks for a_view's binding, answered later as an event into a_replies.
		 * a_view must be alive at the call. The request holds a reference until it is answered.
		 */
		void Request(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, Replies& a_replies, std::uint64_t a_cookie);

		/** @brief Render thread, outside an epoch. Publishes what the import thread imports with, and the fixed bindings. */
		void PublishImportContext();

		/** @brief Any thread. The fixed bindings last published, or null before the first. */
		std::shared_ptr<const FixedBindings> Fixed() const;

		/** @brief Any thread. Combines a_owners into one owner whose last release runs on the published cleanup queue. */
		std::shared_ptr<const void> Seal(std::vector<std::shared_ptr<const void>> a_owners) const;

		/** @brief A null texture view's heap index, or kInvalid when unsupported. */
		std::uint32_t NullIndex();
		/**
		 * @brief A null buffer view's heap index, or kInvalid when unsupported. A buffer register needs one.
		 * A null texture descriptor read as a buffer is not zero on every GPU, RDNA for one.
		 */
		std::uint32_t NullBufferIndex(bool a_typed);
		/** @brief NullBufferIndex for a buffer view, otherwise NullIndex. */
		std::uint32_t NullIndexFor(ID3D11ShaderResourceView* a_view);
		/** @brief The null descriptor with its owner. */
		Binding NullBinding();

		/** @brief The sampler index for the engine's (address mode, filter mode), or kInvalid if unknown. */
		std::uint32_t Sampler(std::uint32_t a_addressMode, std::uint32_t a_filterMode);
		/** @brief The sampler index with ownership of its descriptor. */
		Binding SamplerBinding(std::uint32_t a_addressMode, std::uint32_t a_filterMode);

		/** @brief Changes on device reset. Per-binding identities, not this, tell a new import apart at a reused SRV address. */
		std::uint32_t Generation() const { return generation.load(std::memory_order_acquire); }  // any thread

		/**
		 * @brief Any thread. Views superseded since a_cursor, which it advances: those DXVK moved (ORG's commit, at Present) and those
		 * that name another image now (NoteViewChanged).
		 * False when the log no longer reaches a_cursor, so every binding should be resolved again.
		 */
		bool RelocatedViews(std::uint64_t& a_cursor, std::vector<ID3D11ShaderResourceView*>& a_out) const;

		/**
		 * @brief Any thread. The view names another image now, so supersede its import: a streamed texture got a new window, or the
		 * engine target set pointed an engine target's view at a new version (a resize). a_view is an identity, never dereferenced.
		 */
		void NoteViewChanged(const ID3D11ShaderResourceView* a_view);

		/** @brief Any thread. Whether a binding's owner was superseded. */
		static bool Superseded(const void* a_owner);

		/** @brief For reports. The window a binding's owner imported, as an identity, or null. */
		static const void* ImportedWindow(const void* a_owner);

		/** @brief For reports. The heap index a binding's owner was imported at, or kInvalid. */
		static std::uint32_t ImportedIndex(const void* a_owner);

		Stats GetStats() const;

		/**
		 * @brief For a report: the requests not answered yet, counted by where they wait (the base window's gate, the import queue, the
		 * import itself), and the oldest a_max with their texture and window state. Any thread.
		 */
		std::string DescribeWaiting(std::size_t a_max) const;

		void Clear();

		~GpuTextures();

	private:
		GpuTextures();
		Binding Resolve(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, ResolveFailure* a_failure, std::source_location a_where, bool a_pinFull);

		std::atomic<std::uint32_t> generation{ 0 };

		struct Impl;
		std::unique_ptr<Impl> impl;
		Stats stats;
	};
}
