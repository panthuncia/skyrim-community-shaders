// The Faces feature's entry. Installs the morph hooks and registers the face component and vertex stream.

#include "Features/DrawcallLimitFix/Features/Faces/Faces.h"
#include "Features/DrawcallLimitFix/Features/Faces/FaceSnapshots.h"

namespace DCLF::Features::Faces
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			// Face shapes become DCLF's only once the hooks are in. The component tracks them either way.
			FaceSnapshots::Get().Install();
			// Registered whether or not the hooks are in. Classified every frame because the record follows the head's snapshot.
			a_context.RegisterGeometryType({ .geometryType = static_cast<std::uint8_t>(RE::BSGeometry::Type::kDynamicTriShape),
				.entry = { .entryClass = Api::EntryClass::FaceShape, .parentKind = Api::kMirroredKindFaceGen, .perFrame = true, .holdsParent = true } });
			RegisterShapes(a_context, FaceSnapshots::Enabled());
			// Set up first among the scene resources, whatever the list order.
			RegisterPositions(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "faces", .install = &Install, .preflight = &FaceSnapshots::Preflight };
	}
}
