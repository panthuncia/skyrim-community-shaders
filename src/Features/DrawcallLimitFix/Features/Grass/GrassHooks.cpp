// The grass sink on GrassShared's capture, which pushes events for the render thread's mirror. Called from any thread.
// The capture hooks are shared with Grass Optimizations and install once.

#include "Features/DrawcallLimitFix/Features/Grass/Grass.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Features/Grass/Diagnostics/GrassEvents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include "GrassShared/GrassCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "GrassShared/GrassMeshLibrary.h"

#include <cstring>
#include <utility>

namespace DCLF::Features::Grass
{
	namespace
	{
		BucketSource SourceOf(const GrassShared::AddEvent& a_add)
		{
			BucketSource source;
			source.diffuse.reset(a_add.diffuse);
			const auto& bound = a_add.modelBound;
			source.boundCenter[0] = bound.center.x;
			source.boundCenter[1] = bound.center.y;
			source.boundCenter[2] = bound.center.z;
			source.modelRadius = bound.radius;
			source.wavePeriod = a_add.wavePeriod;
			source.triangleCount = a_add.triangleCount;
			if (a_add.pbr) {
				const auto& pbr = a_add.pbrData;
				source.normal.reset(pbr.normal);
				source.rmaos.reset(pbr.rmaos);
				source.features0.reset(pbr.features0);
				source.features1.reset(pbr.features1);
				source.emissive.reset(pbr.emissive);
				source.pbrFlags = pbr.flags;
				std::memcpy(source.pbrParams1, pbr.params1, sizeof(source.pbrParams1));
				std::memcpy(source.pbrParams2, pbr.params2, sizeof(source.pbrParams2));
				std::memcpy(source.pbrParams3, pbr.params3, sizeof(source.pbrParams3));
				std::memcpy(source.pbrParams4, pbr.params4, sizeof(source.pbrParams4));
			}
			return source;
		}

		// Copies everything, since the event's fields live only during the call. Mesh buffers are held until the render thread leases them.
		void OnAdd(const GrassShared::AddEvent& a_add)
		{
			Event event{ .kind = Event::Kind::Add, .seq = a_add.seq, .shape = a_add.shape, .group = a_add.group, .instances = std::vector<GrassInstances::Instance>(a_add.count) };
			if (a_add.count)
				std::memcpy(event.instances.data(), a_add.records, a_add.count * sizeof(GrassInstances::Instance));
			event.origin = { a_add.origin.x, a_add.origin.y, a_add.origin.z };
			event.key = { .mesh = a_add.meshKey, .material = a_add.material, .diffuse = a_add.diffuse, .vertexDesc = a_add.vertexDesc, .clampMode = a_add.clampMode,
				.alphaRef = a_add.alphaRef, .windDirection = { a_add.windDirection[0], a_add.windDirection[1] }, .pbr = a_add.pbr, .sphereNormal = a_add.sphereNormal, .uniformScale = a_add.uniformScale };
			event.source = SourceOf(a_add);
			// Read now while the shape is alive. It may be dead by the drain.
			event.lodMesh = GrassShared::MeshOf(a_add.shape);
			event.mesh.key = a_add.meshKey;
			event.mesh.vertexBuffer.copy_from(a_add.vertexBuffer);
			event.mesh.indexBuffer.copy_from(a_add.indexBuffer);
			event.mesh.vertexDesc = a_add.vertexDesc;
			event.mesh.indexCount = a_add.triangleCount * 3;
			Diagnostics::Notify(GrassCaptured{ a_add.shape, a_add.group, a_add.count, a_add.seq, GrassCaptured::Kind::Add });
			Push(std::move(event));
		}

		// A cell unload. The drain ignores groups it does not hold.
		void OnRemoveGroup(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint32_t a_group, std::uint64_t a_seq)
		{
			Diagnostics::Notify(GrassCaptured{ a_shape, a_group, 0, a_seq, GrassCaptured::Kind::RemoveGroup });
			Push(Event{ .kind = Event::Kind::RemoveGroup, .seq = a_seq, .shape = a_shape, .group = a_group });
		}

		// Fires for every BSMultiStreamInstanceTriShape, grass or not. The drain ignores shapes it does not hold.
		void OnRemove(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint64_t a_seq)
		{
			Diagnostics::Notify(GrassCaptured{ a_shape, 0, 0, a_seq, GrassCaptured::Kind::Remove });
			Push(Event{ .kind = Event::Kind::Remove, .seq = a_seq, .shape = a_shape });
		}

		// The engine still draws a rejected group, so the family must not withhold grass while it lives.
		void OnRejected(RE::BSMultiStreamInstanceTriShape* a_shape, std::uint32_t a_group, std::uint64_t a_seq)
		{
			Diagnostics::Notify(GrassCaptured{ a_shape, a_group, 0, a_seq, GrassCaptured::Kind::Rejected });
			Push(Event{ .kind = Event::Kind::Rejected, .seq = a_seq, .shape = a_shape, .group = a_group });
		}
	}

	void Push(Event&& a_event)
	{
		events.Push(std::move(a_event));
	}

	void InstallHooks()
	{
		// GrassShared holds this sink and Grass Optimizations'. Installing its hooks twice does nothing.
		GrassShared::AddSink({ .add = &OnAdd, .removeGroup = &OnRemoveGroup, .remove = &OnRemove, .rejected = &OnRejected });
		GrassShared::InstallCapture();
		HookSites::NoteDetour("the grass capture's attach queue detour", GrassShared::CaptureDetourAttached() ? 1 : 0);
		installed = true;
	}

	void PreflightHooks()
	{
		for (const auto& site : GrassShared::CaptureCallSites())
			HookSites::RequireCall(site.name, site.address);
	}
}
