#pragma once

// Draw families: a feature's instanced draws inside Core's main passes and a trees view's faces, at a fixed Core slot.
// Program families: a material port's keys drawn by the main pipeline set, also at a fixed Core slot.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/SceneResources.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace DCLF::Api
{
	// Defined in Core/Extensions/DrawFamilies.h.
	class DrawFamilyFrame;
	class DrawFamilyDeclare;
	class DrawFamilyDirect;
	class DrawFamilyRevision;
	class DrawFamilyRecord;
	class DrawFamilyFaceRow;
	class DrawFamilyLists;
	class DrawFamilyFaceWrite;

	/** @brief Core's fixed slot per draw family. Core owns the slot's pipelines, signature and pass points, whichever feature takes it. */
	enum class DrawFamily : std::uint8_t
	{
		// Tree LOD instances, as one instanced draw of a culled list.
		TreeInstances,
		// Grass instance shapes, as arrays of instanced draws with a GPU count, one per bucket and LOD tier. No faces.
		Grass,
		Count,
	};

	/** @brief How a family's main pipelines are built from its program. */
	enum class DrawFamilyBuild : std::uint8_t
	{
		// A depth pipeline with LESS and depth write, then colour pipelines with EQUAL and no depth write. Culling is off.
		// The colour pipelines blend as the program's engine write mode says.
		DepthThenEqualColour,
	};
	/** @brief The draw signature a family's draws execute with. */
	enum class DrawFamilySignature : std::uint8_t
	{
		// Plain non-indexed draw arguments, one draw or an array with a GPU count.
		// SV_InstanceID counts from the first instance only on Vulkan, so stages that read it must not use a first instance.
		DrawInstanced,
		// The same arguments after a word holding the first instance, 20 bytes per draw.
		// D3D12's SV_InstanceID ignores the first instance, so there the word goes to kDrawPushInstance for the stage to add.
		InstanceWordDrawInstanced,
	};

	/**
	 * @brief What Core builds a draw family's pipelines from.
	 *
	 * - shaderType: the engine shader whose defines the programs use. It must not be Lighting or another family's.
	 * - name, label, keyName, nativeNote: names for logs, reports, recipes and the "stays native" message.
	 * - descriptors.forward is 0 without faces. colour2 is used only when colours is 2.
	 * - writeMode: the engine alphaBlendWriteMode (0-12) whose blend state the colour pipelines use.
	 */
	struct DrawFamilyProgram
	{
		std::uint8_t shaderType = 0xFF;
		std::string_view name, label, keyName, sourcePath, nativeNote;
		struct Descriptors
		{
			std::uint32_t colour = 0, depth = 0, forward = 0, colour2 = 0;
		} descriptors;
		std::uint8_t colours = 1;
		std::uint8_t writeMode = 1;
		DrawFamilyBuild build = DrawFamilyBuild::DepthThenEqualColour;
		DrawFamilySignature signature = DrawFamilySignature::DrawInstanced;
	};

	/** @brief The engine shapes a family replaces. Their passes are withheld while the family is DCLF's. */
	struct DrawFamilyEngineShapes
	{
		const void* (*propertyVtable)() = nullptr;
	};

	/**
	 * @brief A draw family's per-frame decisions and its part of each pass. A family no feature takes adds no passes.
	 *
	 * - frame.decide: render thread, once per frame before the main cull. Skipped while the family is off or not set up.
	 *   Returns whether DCLF draws the family this frame.
	 * - frame.owned: called every frame right after the decision, even when decide was skipped.
	 *   Anything that needs the frame's ownership should listen here, not in decide.
	 * - main.record: only called once the slot's pipelines are published.
	 * - faces: the callbacks and data come all together or not at all. The faces' cull is the feature's own pass.
	 */
	struct DrawFamilyContribution
	{
		DrawFamily family = DrawFamily::Count;
		ObjectClass objectClass = ObjectClass::Count;
		SceneResourcesId resources;
		std::string_view colourRange;
		struct Frame
		{
			void (*serve)(const DrawFamilyFrame& a_frame) = nullptr;
			bool (*decide)(const DrawFamilyFrame& a_frame) = nullptr;
			void (*owned)(bool a_owned) = nullptr;
			const void* (*programSource)() = nullptr;
		} frame;
		struct Main
		{
			void (*declare)(DrawFamilyDeclare& a_declare) = nullptr;
			void (*revision)(const DrawFamilyRevision& a_revision, std::vector<std::uint64_t>& a_out) = nullptr;
			void (*record)(const DrawFamilyRecord& a_record) = nullptr;
			// Optional main draw for the CPU-driven pipeline. Without it the engine draws the family in CPU frames.
			bool (*direct)(DrawFamilyDirect& a_direct) = nullptr;
		} main;
		struct Faces
		{
			void (*declare)(DrawFamilyDeclare& a_declare) = nullptr;
			void (*record)(const DrawFamilyRecord& a_record) = nullptr;
			bool (*row)(DrawFamilyFaceRow& a_row) = nullptr;
			void (*lists)(DrawFamilyLists& a_lists) = nullptr;
			std::string_view rowName, listName;
			std::uint32_t rowBytes = 0, headerBytes = 0, unitWords = 0, cullGroup = 0;
			ViewRoot root = ViewRoot::Count;
			void (*face)(DrawFamilyFaceWrite& a_face) = nullptr;
		} faces;
		DrawFamilyProgram program;
		DrawFamilyEngineShapes engineShapes;
	};

	/** @brief Core's fixed slot per program family, with its own key bits beside Lighting's keys. */
	enum class ProgramFamily : std::uint8_t
	{
		// The Effect port's keys: key bit 17, spare bits 21-23, stage-key bit 37, program-id bit 63.
		Effect,
		Count,
	};

	/** @brief The stages a family's programs are built of. */
	enum class ProgramStages : std::uint8_t
	{
		VertexPixel,  // vertex and pixel stages only
	};

	/** @brief How a family's pipelines are built from its program. */
	enum class ProgramBuild : std::uint8_t
	{
		// Forward keys and decal groups. Decals use the DEFERRED pixel stage. Never part of the Z-prepass, and the Z key is never shared.
		ForwardOrDecal,
	};

	/**
	 * @brief Draws a material port's keys with the main pipeline set.
	 *
	 * - rowBlocks: false leaves the technique and geometry blocks zero, because the variants read the frame and object instead.
	 * - rootViewsForwardOnly: a category root's view, such as first person, draws these keys only when forward.
	 * - turnedKeyBits: a billboard-turned leaf stores its billboard mode plus 1 in the spare key bits.
	 * - records.alphaRefPassBits: pass bits whose keys keep the alpha threshold as reference whatever the alpha test.
	 */
	struct ProgramFamilyContribution
	{
		ProgramFamily family = ProgramFamily::Count;
		std::uint8_t shaderType = 0xFF;
		std::string_view name, keyName, sourcePath, nativeNote;
		ProgramStages stages = ProgramStages::VertexPixel;
		ProgramBuild build = ProgramBuild::ForwardOrDecal;
		bool rowBlocks = true;
		bool rootViewsForwardOnly = false;
		bool turnedKeyBits = false;
		struct Records
		{
			std::uint32_t suppressEmittance = 0, turned = 0, turnedModeShift = 0, alphaRefPassBits = 0;
		} records;
		const void* (*programSource)() = nullptr;
	};
}
