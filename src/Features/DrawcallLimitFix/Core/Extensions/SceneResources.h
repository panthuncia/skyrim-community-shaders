#pragma once

// Core's side of Api/SceneResources.h: what each scene resources callback gets, the calls Core makes, and the lookup of a feature's state.

#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"

#include <ORGModuleServices/GrowableBufferSet.h>
#include <rhi.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace org
{
	class Buffer;
	class RenderGraph;
	class VersionedBuffer;
}

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
	}
}

namespace DCLF::Draws
{
	/** @brief Every table family's rows unbounded, until a fit bounds them. */
	inline constexpr std::array<std::uint32_t, Extensions::kTableFamilySlots> kUnboundedFamilyRows = [] {
		std::array<std::uint32_t, Extensions::kTableFamilySlots> rows{};
		rows.fill(~0u);
		return rows;
	}();

	/**
	 * @brief What the scene's buffers hold. An object past them waits for the requirement that grows them, drawing and culling nothing.
	 * The payload is not covered, because it holds every row of its publication.
	 */
	struct SceneFit
	{
		std::uint32_t objects = ~0u;
		// The vertex stream's vertices. Unbounded without a stream.
		std::uint32_t streamVertices = ~0u;
		// Per table family, the rows the features' scene resources hold. Unbounded without a fit.
		std::array<std::uint32_t, Extensions::kTableFamilySlots> familyRows = kUnboundedFamilyRows;
		bool operator==(const SceneFit&) const = default;
	};
}

namespace DCLF::Api
{
	/** @brief What a scene resources setup gets on the render thread. Make versioned buffers with Draws::MakeVersioned. */
	class SceneResourcesSetup
	{
	public:
		rhi::Device device;
		bool smallStart = false;  // CS_DCLF_TABLE_START=small starts growable tables tiny, so growth runs early and often

		/** @brief A named, materialized structured buffer. Shaders read it through its SRV. */
		std::shared_ptr<org::Buffer> StructuredBuffer(std::uint32_t a_elements, std::uint32_t a_stride, const char* a_name, bool a_unorderedAccess = false) const;
	};

	/** @brief What graph registration gets: the graph and the setup's state. */
	class SceneResourcesGraph
	{
	public:
		org::RenderGraph& graph;
		const std::shared_ptr<void>& state;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/** @brief What a requirement gets on the snapshot builder: the revision's build, the publication's tables, and the posted input words. */
	class SceneResourcesRequirements
	{
	public:
		org::services::GrowableBufferSet::Build& build;
		const SceneTables& tables;
		const std::shared_ptr<void>& state;
		std::span<const std::uint32_t> inputs;
		std::uint32_t objectRows = 0;  // stable object-slot high-water mark

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/** @brief What a fit gets: the versions a set names, and the fit to fill. */
	class SceneResourcesFit
	{
	public:
		const org::services::NamedVersions& sealed;
		const std::shared_ptr<void>& state;
		Draws::SceneFit& fit;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/** @brief Where the depth commit latches each buffer, from offset 0. A versioned buffer is copied into the adopted version. */
	class SceneResourcesLatch
	{
	public:
		const std::shared_ptr<void>& state;
		// Core's sink for Latch.
		void (*sink)(void* a_context, const std::shared_ptr<org::Buffer>& a_buffer, const std::shared_ptr<org::VersionedBuffer>& a_versioned, std::size_t a_bytes);
		void* context;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
		void Latch(const std::shared_ptr<org::Buffer>& a_buffer, std::size_t a_bytes) const { sink(context, a_buffer, nullptr, a_bytes); }
		void Latch(const std::shared_ptr<org::VersionedBuffer>& a_buffer, std::size_t a_bytes) const { sink(context, nullptr, a_buffer, a_bytes); }
	};

	/** @brief What sealed gets: the versions the sealed set names, and the state. */
	class SceneResourcesSealed
	{
	public:
		const org::services::NamedVersions& sealed;
		const std::shared_ptr<void>& state;

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief The scene resources' states by registration. A slot may be null. */
	using SceneResourceStates = std::vector<std::shared_ptr<void>>;
	using SceneResourceInputs = std::array<std::uint32_t, kSceneResourceInputWords>;

	// Every call below except PostSceneResourceInputs runs the vertex stream's resources first, then the rest in registration order.

	/** @brief Render thread. Every setup. If the vertex stream's gives no state, the rest are skipped. */
	SceneResourceStates SetUpSceneResources(rhi::Device a_device, bool a_smallStart);
	/** @brief Render thread. Registers every state's buffers with a_graph. */
	void RegisterSceneResources(org::RenderGraph& a_graph, const SceneResourceStates& a_states);
	/** @brief Snapshot builder. Every state's requirements. */
	void RequireSceneResources(org::services::GrowableBufferSet::Build& a_build, const SceneTables& a_tables, const SceneResourceStates& a_states,
		const SceneResourceInputs& a_inputs);
	/** @brief Render thread. Every postInputs, each into its zeroed words of a_out. */
	void PostSceneResourceInputs(SceneResourceInputs& a_out);
	/** @brief a_id's posted words, or empty. */
	std::span<const std::uint32_t> SceneResourceInputsOf(const SceneResourceInputs& a_inputs, Api::SceneResourcesId a_id);
	/** @brief Render thread. Every state's latched buffers. */
	void LatchSceneResources(const SceneResourceStates& a_states,
		void (*a_sink)(void*, const std::shared_ptr<org::Buffer>&, const std::shared_ptr<org::VersionedBuffer>&, std::size_t), void* a_context);
	/** @brief What every state's buffers hold, as a_sealed names them. */
	void FitSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states, Draws::SceneFit& a_fit);
	/** @brief Snapshot builder, once a revision's set seals and before any of its shapes. Every state's sealed. */
	void SealSceneResources(const org::services::NamedVersions& a_sealed, const SceneResourceStates& a_states);

	/** @brief a_id's state in a_scene's tables, or empty. */
	const std::shared_ptr<void>& SceneResourcesStateOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id);
	/** @brief The state as the feature's type. Thread rules are the feature's. */
	template <class T>
	T* SceneResourcesOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id)
	{
		return static_cast<T*>(SceneResourcesStateOf(a_scene, a_id).get());
	}
	/** @brief Same, sharing ownership. */
	template <class T>
	std::shared_ptr<T> SharedSceneResourcesOf(const Draws::SceneBuffers& a_scene, Api::SceneResourcesId a_id)
	{
		return std::static_pointer_cast<T>(SceneResourcesStateOf(a_scene, a_id));
	}
}
