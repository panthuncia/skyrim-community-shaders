// The draw families' entries and their frame calls.

// Internal.h first, because volk must precede every Vulkan header in the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"

namespace DCLF::Extensions
{
	namespace
	{
		// Written only during InstallFeatures, before DCLF's threads exist. Read-only after.
		DrawFamilySet& Entries()
		{
			static DrawFamilySet entries;
			return entries;
		}

		// Per engine shader type, the family whose program it is, or Count.
		std::array<Api::DrawFamily, 256>& ShaderFamilies()
		{
			static std::array<Api::DrawFamily, 256> families = [] {
				std::array<Api::DrawFamily, 256> none;
				none.fill(Api::DrawFamily::Count);
				return none;
			}();
			return families;
		}

		std::uint32_t& FaceBytes()
		{
			static std::uint32_t bytes = (Draws::kViewBlockBytes + 255u) & ~255u;
			return bytes;
		}

		/** @brief Moves the strings into the entry and points the contribution's views at them. */
		void OwnStrings(DrawFamilyEntry& a_entry)
		{
			auto& program = a_entry.contribution.program;
			a_entry.name = program.name;
			a_entry.label = program.label;
			a_entry.keyName = program.keyName;
			a_entry.sourcePath = program.sourcePath;
			a_entry.nativeNote = program.nativeNote;
			program.name = a_entry.name;
			program.label = a_entry.label;
			program.keyName = a_entry.keyName;
			program.sourcePath = a_entry.sourcePath;
			program.nativeNote = a_entry.nativeNote;
			auto& faces = a_entry.contribution.faces;
			a_entry.rowName = faces.rowName;
			a_entry.listName = faces.listName;
			faces.rowName = a_entry.rowName;
			faces.listName = a_entry.listName;
		}
	}

	void ResolveDrawFamilies(const std::array<Api::DrawFamilyContribution, kDrawFamilies>& a_registered)
	{
		auto& entries = Entries();
		std::uint32_t faceValues = Draws::kViewBlockBytes;
		for (std::size_t f = 0; f < kDrawFamilies; ++f) {
			const auto& registered = a_registered[f];
			auto& entry = entries[f];
			if (registered.family == Api::DrawFamily::Count)
				continue;
			entry.taken = true;
			entry.contribution = registered;
			entry.colourRange = registered.colourRange;
			entry.contribution.colourRange = entry.colourRange;
			OwnStrings(entry);
			entry.engineShapeVtable = reinterpret_cast<std::uintptr_t>(registered.engineShapes.propertyVtable());
			ShaderFamilies()[registered.program.shaderType] = registered.family;
			// A family with faces places its row and list header after the face block and earlier families.
			if (registered.faces.face) {
				entry.faceLayout = { faceValues, faceValues + registered.faces.rowBytes };
				faceValues += registered.faces.rowBytes + registered.faces.headerBytes;
			}
		}
		FaceBytes() = (faceValues + 255u) & ~255u;
	}

	const DrawFamilySet& DrawFamilies() { return Entries(); }

	Api::DrawFamily DrawFamilyOfShader(std::uint8_t a_shaderType) { return ShaderFamilies()[a_shaderType]; }

	std::uint32_t DrawFamilyFaceBytes() { return FaceBytes(); }

	bool DrawFamilyOn(Api::DrawFamily a_family)
	{
		const auto& entry = DrawFamilyOf(a_family);
		return entry.taken && ObjectClassOn(entry.contribution.objectClass);
	}

	void ServeDrawFamilies(std::uint32_t a_frame)
	{
		const Api::DrawFamilyFrame frame{ a_frame };
		for (const auto& entry : Entries())
			if (entry.taken && entry.contribution.frame.serve)
				entry.contribution.frame.serve(frame);
	}
}
