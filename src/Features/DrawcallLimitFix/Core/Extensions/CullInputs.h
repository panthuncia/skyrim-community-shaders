#pragma once

// Core's side of Api/CullInputs.h: the main cull input's entry and the batch its judge answers, and the cull bindings by slot.
// The primary cull's install requires the main walk's hook, which a feature's early hooks report.

#include "Features/DrawcallLimitFix/Api/CullInputs.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org
{
	class Buffer;
	class RenderPassBuilder;
	class VersionedBuffer;
	namespace services
	{
		class NamedVersions;
	}
}

namespace DCLF::Draws
{
	struct SceneBuffers;
}

namespace DCLF::Api
{
	/**
	 * @brief The fade roots a depth commit asks the judge about. Answers are Put back by the same index.
	 * Roots without a node get noProgram unasked. Core reuses the storage, so nothing allocates per commit once grown.
	 */
	class RootProgramsJudge
	{
	public:
		std::size_t Count() const { return asked.size(); }
		const void* Root(std::size_t a_index) const { return nodes[roots[asked[a_index]]]; }
		void Put(std::size_t a_index, std::uint32_t a_program) const { programs[asked[a_index]] = a_program; }

		std::span<const void* const> nodes;     // by root slot
		std::span<const std::uint32_t> roots;   // in Core's order
		std::span<const std::uint32_t> asked;   // per question, its place in roots
		std::span<std::uint32_t> programs;      // per place in roots, the answers
	};

	/** @brief What a cull binding's select gets on the render thread. It answers with one of Versioned, Buffer or Index, or leaves it unbound. */
	class CullBindingSelect
	{
	public:
		const Draws::SceneBuffers& scene;
		std::uint32_t frame = 0;                                     // a binding may alternate by frame
		const org::services::NamedVersions* adopted = nullptr;       // null at a declaration

		/** @brief Answers with a versioned buffer from the feature's scene resources. */
		void Versioned(const std::shared_ptr<org::VersionedBuffer>& a_buffer) { versioned = &a_buffer; }
		/** @brief Answers with a plain buffer made at scene setup. */
		void Buffer(const std::shared_ptr<org::Buffer>& a_buffer) { buffer = &a_buffer; }
		/** @brief Answers with a pooled buffer's SRV index for this frame. No pass declares it. */
		void Index(std::uint32_t a_srv) { srv = a_srv; }
		/**
		 * @brief With Index, for a publication an upload fills each frame: the late-bound versioned buffer readers declare instead of the backing.
		 * That lets a reader on another queue wait for the upload. Answer it at declarations too.
		 */
		void Published(const std::shared_ptr<org::VersionedBuffer>& a_buffer) { published = &a_buffer; }
		/** @brief With Versioned or Buffer: how many units readers may read now. 0 binds none. Unanswered means all of it. */
		void Rows(std::uint32_t a_rows) { rows = a_rows; }

		// The answer.
		const std::shared_ptr<org::VersionedBuffer>* versioned = nullptr;
		const std::shared_ptr<org::Buffer>* buffer = nullptr;
		const std::shared_ptr<org::VersionedBuffer>* published = nullptr;
		std::uint32_t srv = 0;
		std::uint32_t rows = ~0u;
	};
}

namespace DCLF::Extensions
{
	/** @brief The input's entry, with its name owned. */
	struct MainCullInputEntry
	{
		std::string name;
		Api::MainCullInputContribution contribution;
	};

	/** @brief Main thread, at the end of InstallFeatures. */
	void ResolveMainCullInput(const Api::MainCullInputContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without an input. */
	const MainCullInputEntry* MainCullInput();

	/** @brief Main thread, from a feature's early hooks. Reports that the main walk's hook is in. */
	void NoteMainWalkInstalled();
	/** @brief Main thread. Whether the main walk's hook is in. The primary cull requires it. */
	bool MainWalkInstalled();

	inline constexpr std::size_t kCullBindingSlots = static_cast<std::size_t>(Api::CullBindingSlot::Count);
	/** @brief A slot's binding, with its name owned. */
	struct CullBindingEntry
	{
		std::string name;
		Api::CullBinding binding;
	};

	/** @brief Main thread, at the end of InstallFeatures. */
	void ResolveCullBindings(const std::array<Api::CullBinding, kCullBindingSlots>& a_registered);
	/** @brief Any thread, after InstallFeatures. Null without a binding. */
	const CullBindingEntry* CullBindingOf(Api::CullBindingSlot a_slot);
	/** @brief Render thread, per pass or commit, never per object. Runs a_slot's select. a_adopted is null at a declaration. */
	Api::CullBindingSelect SelectCullBinding(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame = 0,
		const org::services::NamedVersions* a_adopted = nullptr);
	/** @brief The versioned answer for a pass's declaration and preparation, or a null handle. Stable while a_scene lives. */
	const std::shared_ptr<org::VersionedBuffer>& CullBindingBuffer(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame = 0);
	/** @brief The late-bound answer a reader declares, or a null handle. Stable while a_scene lives. */
	const std::shared_ptr<org::VersionedBuffer>& CullBindingPublished(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene);
	/** @brief Declares a read of every cull binding publication in a_slots. Slots without one are skipped. */
	void DeclareCullBindingPublications(org::RenderPassBuilder& a_builder, const Draws::SceneBuffers& a_scene, std::initializer_list<Api::CullBindingSlot> a_slots);
}
