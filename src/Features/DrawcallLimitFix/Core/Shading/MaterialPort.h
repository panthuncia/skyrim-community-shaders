#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <memory>

#include <d3d11.h>
#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortFrame.h"

struct ID3D11ShaderResourceView;

namespace RE
{
	class BSShaderMaterial;
}

namespace DCLF
{
	/**
	 * @brief BSLightingShader::SetupMaterial ported, with Community Shaders' hooks on it as the material contributors.
	 * A pure function of two inputs, so the scene work can make a material record without the engine:
	 * MaterialSnapshot, the material's own fields and texture views captured where it is written or first seen,
	 * and MaterialFrame, the per-frame sources the render thread samples at the frame's start.
	 */
	namespace MaterialPort
	{
		/** @brief Bytes a snapshot keeps: at least the largest class the port reads. */
		inline constexpr std::uint32_t kMaxMaterialBytes = 0x400;
		/** @brief Texture fields a snapshot resolves: every NiSourceTexture field of the largest class. */
		inline constexpr std::uint32_t kMaxTextureFields = 32;

		/** @brief One texture field's byte offset and the view SetupMaterial would bind for it now. */
		struct TextureField
		{
			std::uint16_t offset = 0;
			ID3D11ShaderResourceView* view = nullptr;
		};

		/** @brief A material's fields and texture views at one point. Pure data, so no engine pointer is followed after Capture. */
		struct MaterialSnapshot
		{
			const void* key = nullptr;     // the material
			const void* vtable = nullptr;  // its class
			std::uint32_t feature = 0;     // BSShaderMaterial::GetFeature()
			std::uint8_t materialClass = Extensions::kNoMaterialClass;  // a material contributor's class (Extensions::MaterialClassOf)
			std::uint8_t port = 0;         // its material port's kind, 0 for Lighting
			std::uint32_t size = 0;        // bytes taken
			std::array<std::byte, kMaxMaterialBytes> bytes{};
			std::uint32_t textureCount = 0;
			std::array<TextureField, kMaxTextureFields> textures{};

			template <class T>
			T At(std::size_t a_offset) const
			{
				T value{};
				if (a_offset + sizeof(T) <= size)
					std::memcpy(&value, bytes.data() + a_offset, sizeof(T));
				return value;
			}
			/** @brief The view resolved for the texture field at a_offset, or null. */
			ID3D11ShaderResourceView* View(std::uint16_t a_offset) const
			{
				for (std::uint32_t i = 0; i < textureCount; ++i)
					if (textures[i].offset == a_offset)
						return textures[i].view;
				return nullptr;
			}
			/** @brief Whether the field at a_offset held a texture when captured. */
			bool HasTexture(std::uint16_t a_offset) const { return At<const void*>(a_offset) != nullptr; }
		};

		/** @brief Any thread that may read the material. */
		bool Capture(const RE::BSShaderMaterial& a_material, MaterialSnapshot& a_out);

		/**
		 * @brief A snapshot on its way to the scene work, holding a reference on the material taken at capture.
		 * The scene work passes the reference on to a material slot or to the render thread's releases. It is never just dropped.
		 */
		struct HeldSnapshot
		{
			MaterialSnapshot snapshot;
			std::uint64_t sequence = 0;                 // taken before the read, so the newer of two wins
			RE::BSShaderMaterial* reference = nullptr;  // one count, owned by the holder
			// A reference on each view, taken while the material and its textures are alive.
			// A writer may swap a texture out and free it before the scene work asks for its binding, so the snapshot holds them.
			std::array<winrt::com_ptr<ID3D11ShaderResourceView>, kMaxTextureFields> views;
		};
		/** @brief The captures for the scene work, its only consumer. */
		// No wake of its own. Requests and attaches wake the pass themselves, and writers' captures wait for the frame's pass.
		inline EventQueue<std::unique_ptr<HeldSnapshot>> captures;
		/** @brief Any thread where a_material is alive and readable. Pushes its snapshot with a reference onto captures. */
		void PushCapture(const RE::BSShaderMaterial* a_material);

		/** @brief What separates two captures of one material. */
		enum class SnapshotChange : std::uint8_t
		{
			None,        // the same fields and views (the reference count aside)
			Transforms,  // only the bytes a frame patch owns (Extensions::DifferInFrameSpans: the two texture-transform buffers, BSShaderMaterial
			             // +0xc..+0x2c, and the port's declared frame variables' spans)
			Fields,      // anything else
		};
		/**
		 * @brief Pure. How a_new differs from a_held. The reference count at +0x8 is ignored, since every capture moves it.
		 * Transform buffer and frame variable bytes only feed their frame variables, which a feature's frame patch may own.
		 */
		SnapshotChange SnapshotChangeOf(const MaterialSnapshot& a_held, const MaterialSnapshot& a_new);

		/** @brief Render thread, at the frame's start. The frame's sources. */
		MaterialFrame SampleFrame();

		/**
		 * @brief Pure. What SetupMaterial, with CS's hooks, writes for a_snapshot under a_passDescriptor.
		 * Returns false for a material the port does not cover.
		 */
		bool Evaluate(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const MaterialFrame& a_frame, MaterialRecord& a_out);

		/** @brief Vanilla BSLightingShader::SetupMaterial (1414dc310) for the vanilla material classes. */
		bool EvaluateVanilla(const MaterialSnapshot& a_snapshot, std::uint32_t a_passDescriptor, const VanillaFrame& a_frame, MaterialRecord& a_out);
		/** @brief The vanilla classes' texture field offsets. Sets a_count rather than adding to it. */
		void VanillaTextureFields(const RE::BSShaderMaterial& a_material, std::uint16_t* a_out, std::uint32_t& a_count);
		/** @brief A vanilla class's size from its Create's allocation, or 0. */
		std::uint32_t VanillaClassSize(const RE::BSShaderMaterial& a_material);
		/** @brief The view SetupMaterial binds for a texture object, or null. */
		ID3D11ShaderResourceView* ViewOf(const void* a_texture);
	}
}
