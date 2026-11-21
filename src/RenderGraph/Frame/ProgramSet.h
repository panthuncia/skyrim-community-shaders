#pragma once

#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/ProgramService.h"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
* @brief Programs for a set of keys that the frame meets, such as a renderer's draw states or a target's formats.
*
* A key known at setup is queued with CS's setup batch (Queue). A key met later is wanted (Want). The wanted keys are Observed,
* so a new one is an event, and the next revision starts once its program is built. A client that reads the set depends on
* Var() and reads its programs with For(a_in). The frame reads Adopted(). A key with no program in that revision reads null:
* it is not drawn yet.
*
* The descriptions are made, and their defines resolved, on the render thread when a revision starts. The new keys of a revision
* build in one job on the programs lane. A failed build is fatal. Every program is kept for the session.
*
* Render thread only. The set stays at one address, and its name must be unique in the session.
*/
namespace RenderGraph::Frame
{
	template <class Key, class P>
	class ProgramSet
	{
	public:
		using Desc = typename P::Desc;
		using Describe = std::function<Desc(const Key&)>;
		using Map = std::map<Key, std::shared_ptr<const P>>;

		/** @brief Render owner, at setup. a_describe makes a key's description on the render thread. Empty without the graph. */
		ProgramSet(std::string_view a_name, Describe a_describe) :
			name(a_name), describe(std::move(a_describe)), wanted(a_name)
		{
			declared = DeclareProgram(
				name, DependsOn(wanted.Var()), [this](const Inputs& a_in) { return BatchFor(a_in); },
				[cache = cache](const Batch& a_batch) { return Build(cache, a_batch); });
		}

		ProgramSet(const ProgramSet&) = delete;
		ProgramSet& operator=(const ProgramSet&) = delete;

		/** @brief Render owner. Queues a_key's build. At setup it joins CS's setup batch; later it gates the next revision. */
		void Queue(const Key& a_key)
		{
			if (Wanted(a_key))
				return;
			if (auto fixed = Programs::Queue(name, describe(a_key)))
				queued.emplace(a_key, std::move(fixed));
		}

		/** @brief Render thread. Asks for a_key. A new key is an event, so the next revision builds it. True when it was new. */
		bool Want(const Key& a_key)
		{
			if (Wanted(a_key))
				return false;
			latest.insert(std::lower_bound(latest.begin(), latest.end(), a_key), a_key);
			wanted.Observe(latest);
			return true;
		}

		/** @brief Whether a_key was queued or wanted. */
		bool Wanted(const Key& a_key) const { return queued.contains(a_key) || std::binary_search(latest.begin(), latest.end(), a_key); }

		/** @brief What a client that reads the set depends on: DependsOn(set.Var(), ...). */
		const CS::Setting<std::uint64_t>& Var() const { return wanted.Var(); }

		/** @brief Render thread, in a client's key or make. a_key's program in a revision's values, or null when it has none. */
		std::shared_ptr<const P> For(const Inputs& a_in, const Key& a_key) const
		{
			if (const auto found = queued.find(a_key); found != queued.end())
				return found->second.Ready() ? found->second.Get() : nullptr;
			return Lookup(declared.For(a_in), a_key);
		}

		/** @brief Render thread, in the frame. a_key's program in the adopted revision, or null when it has none. */
		std::shared_ptr<const P> Adopted(const Key& a_key) const
		{
			if (const auto found = queued.find(a_key); found != queued.end())
				return found->second.Ready() ? found->second.Get() : nullptr;
			return Lookup(Revision::Adopted().Program(declared), a_key);
		}

	private:
		/** @brief Every program built so far. Builds run on the programs lane, several at once. */
		struct Cache
		{
			std::mutex mutex;
			Map built;
		};

		/** @brief A revision's keys, and the descriptions of those no build has yet. Equal when the keys are. */
		struct Batch
		{
			std::vector<Key> keys;  // sorted
			std::vector<std::pair<Key, Desc>> missing;
			bool operator==(const Batch& a_other) const { return keys == a_other.keys; }
		};

		/** @brief Render thread, when a revision starts. Describes the keys no build has yet. */
		Batch BatchFor(const Inputs& a_in) const
		{
			Batch batch;
			if (const auto* keys = wanted.Get(a_in))
				batch.keys = *keys;
			for (const auto& key : batch.keys) {
				bool built = false;
				{
					const std::lock_guard lock(cache->mutex);
					built = cache->built.contains(key);
				}
				if (built)
					continue;
				Desc desc = describe(key);
				Programs::ResolveDefines(desc);
				batch.missing.emplace_back(key, std::move(desc));
			}
			return batch;
		}

		/** @brief On the programs lane. Builds the missing keys and returns the batch's programs. Null when a build failed. */
		static std::shared_ptr<const Map> Build(const std::shared_ptr<Cache>& a_cache, const Batch& a_batch)
		{
			for (const auto& [key, desc] : a_batch.missing) {
				{
					const std::lock_guard lock(a_cache->mutex);
					if (a_cache->built.contains(key))
						continue;
				}
				auto program = Programs::Load(desc);
				if (!program)
					return nullptr;
				const std::lock_guard lock(a_cache->mutex);
				a_cache->built.try_emplace(key, std::move(program));
			}
			auto map = std::make_shared<Map>();
			const std::lock_guard lock(a_cache->mutex);
			for (const auto& key : a_batch.keys) {
				const auto found = a_cache->built.find(key);
				// A key was built before the batch was made, or is one of its missing keys.
				if (found == a_cache->built.end())
					return nullptr;
				map->emplace(key, found->second);
			}
			return map;
		}

		static std::shared_ptr<const P> Lookup(const std::shared_ptr<const Map>& a_map, const Key& a_key)
		{
			if (!a_map)
				return nullptr;
			const auto found = a_map->find(a_key);
			return found == a_map->end() ? nullptr : found->second;
		}

		std::string name;
		Describe describe;
		std::shared_ptr<Cache> cache = std::make_shared<Cache>();
		Observed<std::vector<Key>> wanted;
		std::vector<Key> latest;  // the newest wanted keys, sorted
		std::map<Key, Programs::Fixed<P>> queued;
		DeclaredProgram<Map> declared;
	};
}

