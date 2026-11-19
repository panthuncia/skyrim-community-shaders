#pragma once

// Scene graph walkers over a live subtree and a live parent chain. Any thread where the walked nodes are alive.

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace DCLF::Engine
{
	/** @brief The deepest parent chain a walk follows. Anything longer is not the scene graph. */
	inline constexpr std::uint32_t kMaxParentDepth = 64;

	/**
	 * @brief A node's child slots below its free index, null slots included. CommonLib's begin/end span the capacity, whose tail the
	 * engine never promises to clear. Always walk children through this, never a range-for over GetChildren().
	 */
	inline std::span<const RE::NiPointer<RE::NiAVObject>> Children(const RE::NiNode& a_node)
	{
		const auto& children = a_node.GetChildren();
		return { children.begin(), children.free_idx() };
	}

	/** @brief What a visit makes of its object: descend, skip its children, or stop. */
	enum class Walk : std::uint8_t
	{
		Descend,
		Skip,
		Stop
	};

	namespace Detail
	{
		/** @brief A visit's answer as a Walk. A bool true is Descend and false is Stop. void is Descend. */
		template <class Visit, class... Args>
		Walk Answer(Visit& a_visit, Args&&... a_args)
		{
			using R = std::invoke_result_t<Visit&, Args...>;
			if constexpr (std::is_void_v<R>) {
				a_visit(std::forward<Args>(a_args)...);
				return Walk::Descend;
			} else if constexpr (std::is_same_v<R, bool>) {
				return a_visit(std::forward<Args>(a_args)...) ? Walk::Descend : Walk::Stop;
			} else {
				return a_visit(std::forward<Args>(a_args)...);
			}
		}
	}

	/**
	 * @brief Walks the subtree under a_root depth first, at most a_limit objects. False when a visit stopped the walk.
	 */
	template <class Visit>
	bool ForEachLive(const RE::NiAVObject* a_root, Visit&& a_visit, std::size_t a_limit = ~std::size_t{ 0 })
	{
		if (!a_root)
			return true;
		std::vector<std::pair<RE::NiAVObject*, std::uint32_t>> stack{ { const_cast<RE::NiAVObject*>(a_root), 0u } };
		for (std::size_t visited = 0; !stack.empty() && visited < a_limit; ++visited) {
			const auto [object, depth] = stack.back();
			stack.pop_back();
			const Walk walk = Detail::Answer(a_visit, *object, depth);
			if (walk == Walk::Stop)
				return false;
			if (walk == Walk::Skip)
				continue;
			if (auto* node = object->AsNode())
				for (auto& child : Children(*node))
					if (child)
						stack.emplace_back(child.get(), depth + 1);
		}
		return true;
	}

	/** @brief Walks the parent chain from a_from up until a_visit returns false. Returns where it stopped, or null at the top. */
	template <class Visit>
	const RE::NiAVObject* ForEachParent(const RE::NiAVObject* a_from, Visit&& a_visit)
	{
		for (const RE::NiAVObject* object = a_from; object; object = object->parent)
			if (!a_visit(*object))
				return object;
		return nullptr;
	}
}
