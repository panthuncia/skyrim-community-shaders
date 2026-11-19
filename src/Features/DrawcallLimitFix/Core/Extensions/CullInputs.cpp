// The main cull input's entry, the main walk's install note and the cull bindings' entries.

// Helpers.h first in its own block, because volk must precede every Vulkan header in the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"

#include <Render/PassBuilders.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/Buffers/VersionedBuffer.h>

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		struct Resolved
		{
			MainCullInputEntry entry;
			bool registered = false;
			std::array<CullBindingEntry, kCullBindingSlots> bindings;
			std::array<bool, kCullBindingSlots> bound{};
		};

		Resolved& State()
		{
			static Resolved resolved;
			return resolved;
		}

		// Written once on the main thread by a feature's early hooks, before PrimaryCull::Install reads it.
		bool mainWalkInstalled = false;

		const std::shared_ptr<org::VersionedBuffer> kNoCullBuffer;
		const std::shared_ptr<org::VersionedBuffer> kNoCullPublished;
	}

	void ResolveMainCullInput(const Api::MainCullInputContribution& a_registered)
	{
		auto& resolved = State();
		resolved.registered = !a_registered.name.empty();
		if (!resolved.registered)
			return;
		auto& entry = resolved.entry;
		entry.name = std::string(a_registered.name);
		entry.contribution = a_registered;
		entry.contribution.name = entry.name;
	}

	const MainCullInputEntry* MainCullInput()
	{
		const auto& resolved = State();
		return resolved.registered ? &resolved.entry : nullptr;
	}

	void NoteMainWalkInstalled() { mainWalkInstalled = true; }
	bool MainWalkInstalled() { return mainWalkInstalled; }

	void ResolveCullBindings(const std::array<Api::CullBinding, kCullBindingSlots>& a_registered)
	{
		auto& resolved = State();
		for (std::size_t i = 0; i < kCullBindingSlots; ++i) {
			resolved.bound[i] = a_registered[i].select != nullptr;
			if (!resolved.bound[i])
				continue;
			auto& entry = resolved.bindings[i];
			entry.name = std::string(a_registered[i].name);
			entry.binding = a_registered[i];
			entry.binding.name = entry.name;
		}
	}

	const CullBindingEntry* CullBindingOf(Api::CullBindingSlot a_slot)
	{
		const auto& resolved = State();
		const auto i = static_cast<std::size_t>(a_slot);
		return i < kCullBindingSlots && resolved.bound[i] ? &resolved.bindings[i] : nullptr;
	}

	Api::CullBindingSelect SelectCullBinding(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame,
		const org::services::NamedVersions* a_adopted)
	{
		Api::CullBindingSelect select{ a_scene, a_frame, a_adopted };
		if (const auto* entry = CullBindingOf(a_slot))
			entry->binding.select(select);
		return select;
	}

	const std::shared_ptr<org::VersionedBuffer>& CullBindingBuffer(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto select = SelectCullBinding(a_slot, a_scene, a_frame);
		return select.versioned ? *select.versioned : kNoCullBuffer;
	}

	const std::shared_ptr<org::VersionedBuffer>& CullBindingPublished(Api::CullBindingSlot a_slot, const Draws::SceneBuffers& a_scene)
	{
		const auto select = SelectCullBinding(a_slot, a_scene);
		return select.published ? *select.published : kNoCullPublished;
	}

	void DeclareCullBindingPublications(org::RenderPassBuilder& a_builder, const Draws::SceneBuffers& a_scene, std::initializer_list<Api::CullBindingSlot> a_slots)
	{
		for (const auto slot : a_slots)
			if (const auto& published = CullBindingPublished(slot, a_scene))
				a_builder.ShaderResource(*published);
	}
}
