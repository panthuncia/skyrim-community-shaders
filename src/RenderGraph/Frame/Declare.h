#pragma once

#include "RenderGraph/Frame/Variables.h"
#include "Utils/Setting.h"

#include <d3d11.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

namespace GraphResources
{
	class Resource;
	class Texture2D;
	class Texture3D;
	struct Options;
}

namespace FrameAnchors
{
	struct ClientDesc;
}

/**
* @brief The CS declaration API (frame revision plan, step 6).
*
* A feature declares its textures and its clients as functions of variables: the frame-wide ones (Frame/Variables.h) and its
* own CS::Setting fields. It does that once, at setup, on the render thread. A change of a dependency is evaluated when the next
* frame revision starts (Frame/Revision.h), against the values that revision carries:
* - a texture whose description changes gets a new version, made off the render thread. A History texture's new version is
*   born cleared, and FirstFrame says so for its first frame;
* - a client whose key changes is registered again, or removed when its key is empty. The revision then waits for the stage
*   builds that hold the change, so the values, the versions and the builds switch in at the same Begin.
* In the frame, a feature reads sizes and values from Revision::Adopted(), never from its live settings or a twin's Desc().
*
* This header includes nothing of ORG or OMS. Without the render graph the declarations are inert: a texture keeps the
* description it was declared with, and a client is never registered.
*/
namespace RenderGraph::Frame
{
	/** @brief One variable a declaration depends on: a frame-wide variable or a CS::Setting. */
	struct Dependency
	{
		std::string_view frameVariable;  // a Vars::Var name, or empty
		CS::SettingVariableId setting = 0;
		std::size_t settingSize = 0;
	};

	template <class T>
	Dependency DependencyOf(Vars::Var<T> a_variable)
	{
		return { a_variable.name, 0, 0 };
	}

	template <class T>
	Dependency DependencyOf(const CS::Setting<T>& a_setting)
	{
		return { {}, a_setting.Id(), sizeof(T) };
	}

	/** @brief The variables a declaration depends on. Its description or key may read only these. */
	using Dependencies = std::vector<Dependency>;

	/** @brief DependsOn(Vars::DisplaySize(), settings.gridSize, ...). A CS::Setting must be bound (CS::Setting::Bind). */
	template <class... D>
	Dependencies DependsOn(const D&... a_dependencies)
	{
		return { DependencyOf(a_dependencies)... };
	}

	/** @brief The values of one frame revision's variables, as a declaration's function or the frame reads them. */
	class Inputs
	{
	public:
		/** @brief A frame-wide variable's value. */
		template <class T>
		T Get(Vars::Var<T> a_variable) const;

		/** @brief A CS::Setting's value. The setting must fit Vars::Lane::SettingValue (32 bytes). */
		template <class T>
		T Get(const CS::Setting<T>& a_setting) const
		{
			T value{};
			ReadSetting(a_setting.Id(), &value, sizeof(T));
			return value;
		}

		/** @brief Null reads the newest written values (at declaration). Otherwise an org::services::VariableSnapshot. */
		explicit Inputs(const void* a_snapshot) :
			snapshot(a_snapshot) {}

	private:
		void ReadSetting(CS::SettingVariableId a_id, void* a_out, std::size_t a_size) const;
		const void* snapshot = nullptr;
	};

	namespace Detail
	{
		struct TextureDeclaration;
		struct ClientDeclaration;
	}

	/** @brief A declared texture. Keep it for as long as the feature uses the texture. Render thread. */
	template <class Twin>
	class DeclaredTexture
	{
	public:
		DeclaredTexture() = default;

		/** @brief The twin passes declare and register (GraphResources). Its extent changes only at adoption. */
		Twin& operator*() const { return *twin; }
		Twin* operator->() const { return twin; }
		Twin* Get() const { return twin; }
		explicit operator bool() const { return twin != nullptr; }

		const std::shared_ptr<Detail::TextureDeclaration>& Declaration() const { return declaration; }

		DeclaredTexture(std::shared_ptr<Detail::TextureDeclaration> a_declaration, Twin* a_twin) :
			declaration(std::move(a_declaration)), twin(a_twin) {}

	private:
		std::shared_ptr<Detail::TextureDeclaration> declaration;
		Twin* twin = nullptr;
	};

	using DeclaredTexture2D = DeclaredTexture<GraphResources::Texture2D>;
	using DeclaredTexture3D = DeclaredTexture<GraphResources::Texture3D>;

	/** @brief A description from the inputs. Only the extent may change between revisions (same format, mips, slices, flags). */
	using Texture2DDescFn = std::function<D3D11_TEXTURE2D_DESC(const Inputs&)>;
	using Texture3DDescFn = std::function<D3D11_TEXTURE3D_DESC(const Inputs&)>;

	/**
	* @brief Render thread, at setup. Makes a versioned twin from a_desc of the newest values and follows a_dependencies from then
	* on. a_options.versioned is forced on. A description that changes more than the extent is fatal. Null without the graph or
	* when the twin cannot be made (logged).
	*/
	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options);
	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options);

	/**
	* @brief A declared client. Keep it as long as the client follows its dependencies. Dropping it stops that and leaves the
	* registration as it is, since declarations live until process exit.
	*/
	class DeclaredClient
	{
	public:
		DeclaredClient() = default;
		explicit DeclaredClient(std::shared_ptr<Detail::ClientDeclaration> a_declaration) :
			declaration(std::move(a_declaration)) {}

		/**
		* @brief Any point of a frame, render thread. The key reads something that is not a variable yet (another feature's
		* published inputs), and that changed. The key is evaluated again when the next revision starts. An interim for state
		* that becomes a variable when its feature migrates.
		*/
		void Invalidate() const;

		/** @brief Whether the client is registered now. */
		bool Registered() const;

		explicit operator bool() const { return declaration != nullptr; }

	private:
		std::shared_ptr<Detail::ClientDeclaration> declaration;
	};

	namespace Detail
	{
		using ErasedKey = std::shared_ptr<const void>;
		struct ClientFunctions
		{
			std::function<ErasedKey(const Inputs&)> key;                       // null: no client
			std::function<bool(const void*, const void*)> equal;               // two non-null keys
			std::function<bool(const void*, FrameAnchors::ClientDesc&)> make;  // the registration for a key
		};
		DeclaredClient DeclareClient(std::string a_id, Dependencies a_dependencies, ClientFunctions a_functions);
	}

	/**
	* @brief Render thread, at setup. The client a_id is a function of a_dependencies: a_key gives what its registration depends
	* on, or nothing for no client, and a_make fills the registration for a key (false is fatal). A new key registers the client
	* again when a revision starts, and the revision waits for the stage builds that hold it. An equal key does nothing, so a
	* key may quantise a float (density > 0). The first evaluation is at the first revision after this call.
	*/
	template <class Key>
	DeclaredClient DeclareClient(std::string a_id, Dependencies a_dependencies, std::function<std::optional<Key>(const Inputs&)> a_key,
		std::function<bool(const Key&, FrameAnchors::ClientDesc&)> a_make)
	{
		Detail::ClientFunctions functions;
		functions.key = [key = std::move(a_key)](const Inputs& a_inputs) -> Detail::ErasedKey {
			auto value = key(a_inputs);
			if (!value)
				return nullptr;
			return std::make_shared<const Key>(std::move(*value));
		};
		functions.equal = [](const void* a_left, const void* a_right) { return *static_cast<const Key*>(a_left) == *static_cast<const Key*>(a_right); };
		functions.make = [make = std::move(a_make)](const void* a_keyValue, FrameAnchors::ClientDesc& a_out) { return make(*static_cast<const Key*>(a_keyValue), a_out); };
		return Detail::DeclareClient(std::move(a_id), std::move(a_dependencies), std::move(functions));
	}

	/** @brief What the frame reads: the variables of the revision adopted last, and the declared textures as they are bound. */
	class AdoptedRevision
	{
	public:
		template <class T>
		T Get(Vars::Var<T> a_variable) const
		{
			return Values().Get(a_variable);
		}

		template <class T>
		T Get(const CS::Setting<T>& a_setting) const
		{
			return Values().Get(a_setting);
		}

		/** @brief The extent of the bound version (width, height, depth). */
		template <class Twin>
		std::array<std::uint32_t, 3> Extent(const DeclaredTexture<Twin>& a_texture) const
		{
			return ExtentOf(a_texture.Declaration().get());
		}

		/** @brief True in the first frame that binds a new version of the texture. A History texture is born cleared then. */
		template <class Twin>
		bool FirstFrame(const DeclaredTexture<Twin>& a_texture) const
		{
			return FirstFrameOf(a_texture.Declaration().get());
		}

		/** @brief Whether the texture is declared and live. */
		template <class Twin>
		bool Has(const DeclaredTexture<Twin>& a_texture) const
		{
			return static_cast<bool>(a_texture);
		}

	private:
		Inputs Values() const;
		std::array<std::uint32_t, 3> ExtentOf(const Detail::TextureDeclaration* a_texture) const;
		bool FirstFrameOf(const Detail::TextureDeclaration* a_texture) const;
	};
}

namespace RenderGraph::Frame::Revision
{
	/** @brief Render thread. The revision the frame runs. */
	const Frame::AdoptedRevision& Adopted();
}
