#pragma once

// What an epoch's execution holds until the GPU retires it, made the same way for every submission. It holds what the
// execution reads, then the scene's content owners sealed at the end of its record step, which own the descriptors its rows name.

#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct MainPayload;
	struct SceneBuffers;

	class ExecutionOwners
	{
	public:
		/** @brief Render thread: an empty list on the graph's cleanup queue. False without one, and then nothing can be submitted. */
		static ExecutionOwners Make();

		explicit operator bool() const { return list != nullptr; }
		void Add(std::shared_ptr<const void> a_owner)
		{
			if (a_owner)
				list->push_back(std::move(a_owner));
		}
		/** @brief Adds a_payload's bindings. */
		void AddPayload(const MainPayload& a_payload);
		/** @brief The list itself, for a commit that adds what only this execution reads. */
		std::vector<std::shared_ptr<const void>>& List() { return *list; }
		/** @brief At the end of the record step: the content owners of a_scene as they now stand. */
		void Seal(SceneBuffers* a_scene);
		/** @brief The resource owner the execution is submitted with. */
		std::shared_ptr<const void> Handle() const { return list; }

	private:
		std::shared_ptr<std::vector<std::shared_ptr<const void>>> list;
	};
}
