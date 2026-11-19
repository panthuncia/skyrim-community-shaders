#pragma once

// MaterialSources: rules that keep a material record's frame-sourced parts current without re-evaluating it. Pure functions.

#include <array>
#include <cstdint>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Core/Shading/MaterialWrites.h"

struct ID3D11ShaderResourceView;

namespace DCLF
{
	struct MaterialRecord;
	namespace MaterialPort
	{
		struct MaterialFrame;
	}

	/**
	 * @brief Where each value of a material record comes from, and the rule that keeps it current.
	 * The material's own fields change only when something writes the material, and each write recaptures it.
	 * TexcoordOffset reads the buffer the engine flips every frame. Shader-object and global values are shared per signature.
	 * The character light's t11 view is the frame's, so records keep only its modes.
	 */
	namespace MaterialSources
	{
		/** @brief Which frame-sourced groups a pass's SetupMaterial writes. Records with the same signature share those values. */
		std::uint32_t Signature(std::uint32_t a_passDescriptor);

		/** @brief The frame-sourced PS float positions. */
		const std::vector<std::uint32_t>& FramePSFloats();
		/** @brief TexcoordOffset's VS float positions. */
		const std::vector<std::uint32_t>& FrameVSFloats();

		/**
		 * @brief Copies the frame-sourced components of a live evaluation into a record of the same signature, including t11's modes.
		 * Returns true when t11 changed, because the record's textures are part of its version.
		 */
		bool ApplyFrameComponents(const MaterialRecord& a_live, MaterialRecord& a_record, std::uint32_t a_passDescriptor, bool* a_floatsChanged = nullptr);

		/** @brief A record as stored: no view at a character-light pass's t11, but its modes stay. */
		void StripFrameViews(MaterialRecord& a_record, std::uint32_t a_passDescriptor);
		/** @brief Whether a pass descriptor's t11 is the character light's. */
		bool FrameCharacterLight(std::uint32_t a_passDescriptor);
		/** @brief The character light view a live evaluation bound at t11, or null. */
		ID3D11ShaderResourceView* CharacterLightView(const MaterialRecord& a_live, std::uint32_t a_passDescriptor);

		/** @brief Pure. The character light view the frame gives: vanilla's render target, else TruePBR's. Null while neither has one, as while a cell loads. */
		ID3D11ShaderResourceView* FrameCharacterLightView(const MaterialPort::MaterialFrame& a_frame);

		/** @brief Copies every frame-sourced component of a_from over a_to, for comparisons. */
		void CopyFrameComponents(const MaterialRecord& a_from, MaterialRecord& a_to, std::uint32_t a_passDescriptor);
		/** @brief Copies the floats a record keeps from its first evaluation, IBLParams, which drift but no Lighting stage reads. */
		void KeepUnreadFloats(const MaterialRecord& a_from, MaterialRecord& a_to);
	}
}
