// The face positions vertex stream. A GPU buffer of face shape positions, uploaded by each commit.

#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "RenderGraph/BufferRefs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Extensions/VertexStreams.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBufferSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <cstdint>
#include <memory>

namespace DCLF::Features::Faces
{
	namespace
	{
		// In vertices.
		constexpr std::uint32_t kInitialFaceVertices = 1u << 18;

		/** @brief Maps region to the snapshot generation uploaded into this version. */
		struct FaceUploads
		{
			org::services::FilledVersion version;
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> regions;
		};

		/** @brief One float4 per vertex of each drawn face shape. Render thread, except for the handle. */
		struct FacePositions
		{
			Draws::Versioned positions;
			FaceUploads uploaded;
		};

		std::shared_ptr<org::Buffer> DeviceBuffer(std::uint64_t a_bytes, const char* a_name)
		{
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		}

		// Without a buffer view ref there is no state, and the scene's tables are not made.
		std::shared_ptr<void> Setup(const Api::SceneResourcesSetup& a_setup)
		{
			auto state = std::make_shared<FacePositions>();
			state->positions = Draws::MakeVersioned(DeviceBuffer(std::uint64_t(a_setup.smallStart ? 1024u : kInitialFaceVertices) * Draws::kStreamVertexBytes,
														"cs.dclf.face-positions"),
				{ .extent = { 0, Draws::kStreamVertexBytes, 0 } });
			// Only checks that a ref can be made. Readers use each sealed version's ref.
			if (!RenderGraph::DeclaredRefOf(a_setup.device, *state->positions->Get()))
				return nullptr;
			return state;
		}

		void RegisterGraph(const Api::SceneResourcesGraph& a_graph)
		{
			Draws::Register(a_graph.graph, org::ResourceIdentifier("cs.dclf.face-positions"), a_graph.State<FacePositions>().positions);
		}

		void Require(const Api::SceneResourcesRequirements& a_requirements)
		{
			std::uint64_t faceVertices = 0;
			for (const auto& stream : a_requirements.tables.streamRows)
				if (stream.object != Api::VertexStreamRow::kNoObject)
					faceVertices = std::max<std::uint64_t>(faceVertices, std::uint64_t(stream.first) + stream.count);
			Draws::SceneBufferSet::Get().Require(a_requirements.build, a_requirements.State<FacePositions>().positions, faceVertices);
		}

		void Fit(Api::SceneResourcesFit& a_fit)
		{
			a_fit.fit.streamVertices = static_cast<std::uint32_t>(std::min<std::uint64_t>(Draws::NamedUnits(a_fit.sealed, a_fit.State<FacePositions>().positions), UINT32_MAX));
		}

		void Select(Api::VertexStreamSelect& a_select)
		{
			a_select.buffer = &a_select.State<FacePositions>().positions;
		}

		// Uploads a region only when the version does not already hold the head's current snapshot.
		// A region past the version's vertices waits for the growth.
		std::uint32_t Upload(const Api::VertexStreamUpload& a_upload)
		{
			auto& uploaded = a_upload.State<FacePositions>().uploaded;
			const auto& version = a_upload.adopted.Of(a_upload.buffer);
			if (!uploaded.version.Holds(version)) {
				uploaded.regions.clear();
				uploaded.version.Filled(version);
			}
			std::uint32_t count = 0;
			for (const auto& stream : a_upload.rows) {
				if (stream.object == Api::VertexStreamRow::kNoObject || std::uint64_t(stream.first) + stream.count > version.units)
					continue;
				auto& region = uploaded.regions[stream.first];
				if (region == stream.generation)
					continue;
				a_upload.uploads(a_upload.buffer, stream.vertices, std::size_t(stream.count) * Draws::kStreamVertexBytes, std::uint64_t(stream.first) * Draws::kStreamVertexBytes);
				region = stream.generation;
				++count;
			}
			return count;
		}
	}

	void RegisterPositions(Api::InstallContext& a_context)
	{
		const auto resources = a_context.RegisterSceneResources({ .name = "face-positions", .setup = &Setup, .registerGraph = &RegisterGraph, .require = &Require, .fit = &Fit });
		a_context.RegisterVertexStream({ .name = "face positions", .entryClass = Api::EntryClass::FaceShape, .resources = resources, .select = &Select, .upload = &Upload });
	}
}
