#pragma once

// Core's side of Api/Passes.h: what a pass's setup gets, what a scheduled pass is made from, and where an anchor adds passes.
// Also authoring helpers for passes and scene resources. A pass takes its epoch from its context, never a literal, because the schedule decides.

#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/PreparedPass.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/Buffers/VersionedBuffer.h>
#include <Resources/VersionedTexture.h>
#include <rhi.h>

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace org::services
{
	class NamedVersions;
}

namespace DCLF::Draws
{
	struct BuildState;
	struct GraphConditions;
	struct Resources;
	struct RevisionShapes;
	struct SceneBuffers;
	struct ShadowResources;
	struct ViewResources;
}

namespace DCLF::Api
{
	/** @brief What a pass's setup gets on the render thread: the device and the scene's tables as far as they are made. */
	class PassSetup
	{
	public:
		rhi::Device device;
		const Draws::SceneBuffers& scene;
	};

	/** @brief What a pass is made from at graph build: its epoch and its extension's resources. */
	class PassContext
	{
	public:
		std::string_view epochId;
		RenderGraphRuntime::Segment segment{};
		std::uint32_t epoch = 0;                                          // RenderGraphRuntime::EpochOf(segment)
		const std::shared_ptr<Draws::Resources>* main = nullptr;          // Main shape, or a View shape's main resources
		const std::shared_ptr<Draws::ShadowResources>* shadow = nullptr;  // Shadow shape
		const std::shared_ptr<Draws::ViewResources>* view = nullptr;      // View shape, or a hosted epoch's view
		// For a Main epoch that hosts view epochs, the resources of each hosted view, in epoch row order. Empty otherwise.
		std::span<const std::shared_ptr<Draws::ViewResources>> hosted;
		std::shared_ptr<const void> state;                                // null when setup made none
		// The build state. A make reads views and shadow slots from here, never from live fields the render thread changes.
		const Draws::BuildState* build = nullptr;

		const std::shared_ptr<Draws::Resources>& Main() const
		{
			assert(main);
			return *main;
		}
		const std::shared_ptr<Draws::ShadowResources>& Shadow() const
		{
			assert(shadow);
			return *shadow;
		}
		const std::shared_ptr<Draws::ViewResources>& View() const
		{
			assert(view);
			return *view;
		}
		const Draws::BuildState& Build() const
		{
			assert(build);
			return *build;
		}

		/**
		 * @brief The conditions of the build that makes the pass. A pass that needs one at preparation or recording captures it
		 * here, at its make, so it never reads the attached graphs' conditions.
		 */
		const Draws::GraphConditions& Conditions() const;

		template <class T>
		std::shared_ptr<const T> State() const
		{
			return std::static_pointer_cast<const T>(state);
		}

		/** @brief Whether the depth pyramid exists and the cull mode tests occlusion. The phase 2 block is added only then. */
		bool PhaseTwo() const;

		/**
		 * @brief Whether the graph is built for an interior cell, where exterior-only passes like grass and tree LOD are left out.
		 * Their families decide nothing to draw there anyway.
		 */
		bool Interior() const;

		/** @brief The segment whose shape the pass records against, following sharesWith. It searches, so call it from a make, not per frame. */
		RenderGraphRuntime::Segment ShapeSegment() const
		{
			const std::uint32_t row = Draws::EpochById(epochId);
			const std::uint32_t shared = row == Draws::kNoEpoch ? Draws::kNoEpoch : Draws::EpochTraitsOf(row).sharesWith;
			return shared == Draws::kNoEpoch ? segment : Draws::EpochTraitsOf(shared).segment;
		}
	};

	/** @brief What an anchor adds to its epoch, in order. */
	class PassOut
	{
	public:
		PassOut(std::vector<org::RenderGraph::ExternalPassDesc>& a_out, std::uint32_t a_epoch) :
			out(a_out), epoch(a_epoch) {}

		/** @brief Adds a_pass to the anchor's epoch. A null pass is added as given. */
		void Add(std::string a_name, PassKind a_kind, std::shared_ptr<org::RenderPass> a_pass, PassQueue a_queue = PassQueue::Default);
		// Diagnostics observers append finished descs here, and the executor keeps their order.
		std::vector<org::RenderGraph::ExternalPassDesc>& Descs() { return out; }

	private:
		std::vector<org::RenderGraph::ExternalPassDesc>& out;
		std::uint32_t epoch;
	};
}

namespace DCLF::Draws
{
	/**
	 * @brief Registers a graph resource under a_id. The graph then holds it strongly, tracks it and tags its memory with the id.
	 * Versioned buffers are not registered, because that would pin the registered version for the graph's life. Their passes declare them.
	 */
	template <class T>
	void Register(org::RenderGraph& a_graph, org::ResourceIdentifier a_id, const std::shared_ptr<T>& a_resource)
	{
		if constexpr (!std::is_same_v<T, org::VersionedBuffer>)
			if (a_resource)
				a_graph.RegisterResource(std::move(a_id), a_resource);
	}

	/** @brief Register for an import handle. An engine target's slot resolves the id, so the id follows the slot's versions. */
	inline void Register(org::RenderGraph& a_graph, org::ResourceIdentifier a_id, const FrameAnchors::Texture& a_texture)
	{
		if (const auto& slot = a_texture.Slot()) {
			if (!a_graph._resolverMap.contains(a_id))
				a_graph.RegisterResolver(std::move(a_id), slot);
		} else if (a_texture.Plain()) {
			a_graph.RegisterResource(std::move(a_id), a_texture.Plain());
		}
	}

	/** @brief One compute dispatch of a_program with its push constants. */
	template <class Constants>
	void RecordDispatch(org::RecordingContext& a_recording, const ComputeProgram& a_program, const Constants& a_constants, std::uint32_t a_groups)
	{
		auto& commands = a_recording.Commands();
		commands.BindLayout(a_program.layout->GetHandle());
		commands.BindPipeline(a_program.Pipeline(a_recording));
		commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, static_cast<std::uint32_t>(sizeof(Constants) / 4), reinterpret_cast<const std::uint32_t*>(&a_constants));
		commands.Dispatch(a_groups, 1, 1);
	}
}
