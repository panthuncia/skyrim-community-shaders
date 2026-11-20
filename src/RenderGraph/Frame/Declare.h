#pragma once

#include "RenderGraph/Frame/Variables.h"
#include "Utils/Setting.h"

#include <d3d11.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

namespace GraphResources
{
	class Resource;
	class StructuredBuffer;
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
* A feature declares its textures, buffers and clients as functions of variables: the frame-wide ones (Frame/Variables.h) and its
* own CS::Setting fields. It does that once, at setup, on the render thread. A change of a dependency is evaluated when the next
* frame revision starts (Frame/Revision.h), against the values that revision carries:
* - a texture whose description changes gets a new version, made off the render thread. A History texture's new version is
*   born cleared, and FirstFrame says so for its first frame;
* - a structured buffer whose element count changes gets a new version, made off the render thread, with undefined contents;
* - a client whose key changes is registered again, or removed when its key is empty. The revision then waits for the stage
*   builds that hold the change, so the values, the versions and the builds switch in at the same Begin;
* - a program whose key changes is built off the render thread first. The revision starts once it is built.
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
		struct BufferDeclaration;
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
	* @brief A count from the inputs. A new count makes a new version even at the same extent, as a reset does. A History texture's
	* new version is born cleared, and FirstFrame says so.
	*/
	using RenewFn = std::function<std::uint64_t(const Inputs&)>;

	/**
	* @brief Render thread, at setup. Makes a versioned twin from a_desc of the newest values and follows a_dependencies from then
	* on. a_options.versioned is forced on. A description that changes more than the extent is fatal. a_renew, when given, reads
	* only a_dependencies too. Null without the graph or when the twin cannot be made (logged).
	*/
	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options,
		RenewFn a_renew = {});
	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options,
		RenewFn a_renew = {});

	/** @brief A declared structured buffer. Keep it for as long as the feature uses the buffer. Render thread. */
	class DeclaredBuffer
	{
	public:
		DeclaredBuffer() = default;

		/**
		* @brief The twin. Passes declare its slot (BufferVersions()), never Graph(), so a new element count needs no build. Its
		* count, Graph() and D3D11 export change only at adoption.
		*/
		GraphResources::StructuredBuffer& operator*() const { return *twin; }
		GraphResources::StructuredBuffer* operator->() const { return twin; }
		GraphResources::StructuredBuffer* Get() const { return twin; }
		explicit operator bool() const { return twin != nullptr; }

		const std::shared_ptr<Detail::BufferDeclaration>& Declaration() const { return declaration; }

		DeclaredBuffer(std::shared_ptr<Detail::BufferDeclaration> a_declaration, GraphResources::StructuredBuffer* a_twin) :
			declaration(std::move(a_declaration)), twin(a_twin) {}

	private:
		std::shared_ptr<Detail::BufferDeclaration> declaration;
		GraphResources::StructuredBuffer* twin = nullptr;
	};

	/**
	* @brief A structured buffer's description from the inputs. Only ByteWidth may change between revisions (same stride, bind
	* and misc flags), and it must be a whole number of elements. One element is the existence rule's size while off.
	*/
	using BufferDescFn = std::function<D3D11_BUFFER_DESC(const Inputs&)>;

	/**
	* @brief Render thread, at setup. Makes a versioned structured buffer twin from a_desc of the newest values and follows
	* a_dependencies from then on. A new element count is a new version of the same slot, made on the revision lane and adopted
	* with the revision that carries the values, so the frame never sees a count its passes were not recorded for. A version's
	* contents are undefined until a pass writes them; an exported one gets its D3D11 export at adoption. a_options.versioned is
	* forced on, and a transient is fatal. A description that changes more than ByteWidth is fatal. Null without the graph or when
	* the twin cannot be made (logged).
	*/
	DeclaredBuffer DeclareBuffer(std::string a_name, Dependencies a_dependencies, BufferDescFn a_desc, const GraphResources::Options& a_options);

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
			std::function<void(const void*)> adopted;                          // may be empty
		};
		DeclaredClient DeclareClient(std::string a_id, Dependencies a_dependencies, ClientFunctions a_functions);
	}

	/**
	* @brief Render thread, at setup. The client a_id is a function of a_dependencies: a_key gives what its registration depends
	* on, or nothing for no client, and a_make fills the registration for a key (false is fatal). A new key registers the client
	* again when a revision starts, and the revision waits for the stage builds that hold it. An equal key does nothing, so a
	* key may quantise a float (density > 0). The first evaluation is at the first revision after this call.
	*
	* a_adopted, if given, is called inside the adoption point of the first Begin whose adopted values give the registered key,
	* once per registration. The frame runs that registration from then on. Use it to switch in what the frame binds outside the
	* graph, such as a new resource published under an old id. It needs a key of variables only: a key that reads other state may
	* never match the adopted values.
	*/
	template <class Key>
	DeclaredClient DeclareClient(std::string a_id, Dependencies a_dependencies, std::function<std::optional<Key>(const Inputs&)> a_key,
		std::function<bool(const Key&, FrameAnchors::ClientDesc&)> a_make, std::function<void(const Key&)> a_adopted = {})
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
		if (a_adopted)
			functions.adopted = [adopted = std::move(a_adopted)](const void* a_keyValue) { adopted(*static_cast<const Key*>(a_keyValue)); };
		return Detail::DeclareClient(std::move(a_id), std::move(a_dependencies), std::move(functions));
	}

	namespace Detail
	{
		struct ProgramDeclaration;
		using ErasedProgram = std::shared_ptr<const void>;
		struct ProgramFunctions
		{
			std::function<ErasedKey(const Inputs&)> key;          // never null
			std::function<bool(const void*, const void*)> equal;  // two keys
			std::function<ErasedProgram(const void*)> build;      // any thread. Null is a failed build
		};
		std::shared_ptr<ProgramDeclaration> DeclareProgram(std::string a_name, Dependencies a_dependencies, ProgramFunctions a_functions);
		ErasedProgram ProgramFor(const ProgramDeclaration* a_program, const Inputs& a_values);
		ErasedProgram AdoptedProgram(const ProgramDeclaration* a_program);
		void InvalidateProgram(ProgramDeclaration* a_program);
	}

	/** @brief A declared program. Keep it as long as the feature uses the program. */
	template <class P>
	class DeclaredProgram
	{
	public:
		DeclaredProgram() = default;
		explicit DeclaredProgram(std::shared_ptr<Detail::ProgramDeclaration> a_declaration) :
			declaration(std::move(a_declaration)) {}

		/**
		* @brief Render thread. The program for a revision's values, from a client's key or make function or a texture's
		* description. A revision starts only once its programs are built, so it is there. Fatal when it is missing.
		*/
		std::shared_ptr<const P> For(const Inputs& a_values) const
		{
			return std::static_pointer_cast<const P>(Detail::ProgramFor(declaration.get(), a_values));
		}

		/**
		* @brief Render thread. The key reads state that is not a variable yet, and it changed. The key is evaluated again
		* before the next revision starts. An interim, as DeclaredClient::Invalidate.
		*/
		void Invalidate() const { Detail::InvalidateProgram(declaration.get()); }

		const std::shared_ptr<Detail::ProgramDeclaration>& Declaration() const { return declaration; }

		explicit operator bool() const { return declaration != nullptr; }

	private:
		std::shared_ptr<Detail::ProgramDeclaration> declaration;
	};

	/**
	* @brief Render thread, at setup. A program, such as a shader permutation, as a function of a_dependencies. a_key gives what
	* the program is built from (its defines, a sample count), and a_build builds it for a key. Returns a DeclaredProgram of the
	* type a_build returns a shared_ptr to.
	* - The first key is built here, at once, since setup may compile.
	* - Later keys are built off the render thread, on the programs lane, so a_build must be safe there.
	* - A revision whose values need a key not built yet does not start until the build is done. Frames keep the adopted
	*   revision meanwhile, so a compile never blocks a Begin. A client's key or make reads the new program with For().
	* - Every key built is kept for the session, so going back to one costs nothing. Keys compare with operator==.
	* - A failed build after the first is fatal.
	* In the frame, read the program with Revision::Adopted().Program(). Empty without the graph, or when the first build failed
	* (logged).
	*/
	template <class KeyFn, class BuildFn>
	auto DeclareProgram(std::string a_name, Dependencies a_dependencies, KeyFn a_key, BuildFn a_build)
	{
		using Key = std::decay_t<std::invoke_result_t<KeyFn&, const Inputs&>>;
		using Built = std::decay_t<std::invoke_result_t<BuildFn&, const Key&>>;
		using Program = std::remove_const_t<typename Built::element_type>;
		Detail::ProgramFunctions functions;
		functions.key = [key = std::move(a_key)](const Inputs& a_inputs) mutable -> Detail::ErasedKey { return std::make_shared<const Key>(key(a_inputs)); };
		functions.equal = [](const void* a_left, const void* a_right) { return *static_cast<const Key*>(a_left) == *static_cast<const Key*>(a_right); };
		functions.build = [build = std::move(a_build)](const void* a_keyValue) mutable -> Detail::ErasedProgram {
			std::shared_ptr<const Program> program = build(*static_cast<const Key*>(a_keyValue));
			return program;
		};
		return DeclaredProgram<Program>(Detail::DeclareProgram(std::move(a_name), std::move(a_dependencies), std::move(functions)));
	}

	namespace Detail
	{
		Vars::Var<std::uint32_t> DeriveFromSetting(std::string_view a_name, const Dependency& a_setting, std::function<std::uint32_t(const void*)> a_fn);
	}

	/**
	* @brief Render thread, at setup. A frame variable "Derived.<a_name>" computed from one bound setting when a revision takes
	* the values. Its event fires only when the result changes, so a declaration on it ignores edits that keep the result (Post
	* Processing's COD Bloom: the used mips of its blend factors). Declaring a name again returns its variable and keeps the
	* first function. A declaration depends on it and reads it as on any frame variable. Without the graph it reads zero.
	*/
	template <class T>
	Vars::Var<std::uint32_t> DeriveFromSetting(std::string_view a_name, const CS::Setting<T>& a_setting, std::function<std::uint32_t(const T&)> a_fn)
	{
		return Detail::DeriveFromSetting(a_name, DependencyOf(a_setting), [fn = std::move(a_fn)](const void* a_bytes) {
			T value{};
			std::memcpy(&value, a_bytes, sizeof(T));
			return fn(value);
		});
	}

	/** @brief What the frame reads: the variables of the revision adopted last, and the declared textures as they are bound. */
	class AdoptedRevision
	{
	public:
		/** @brief The program for the adopted revision's values. Before the first adoption, the one built at declaration. */
		template <class P>
		std::shared_ptr<const P> Program(const DeclaredProgram<P>& a_program) const
		{
			return std::static_pointer_cast<const P>(Detail::AdoptedProgram(a_program.Declaration().get()));
		}

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

		/** @brief The element count of the buffer's bound version. Zero when it is not declared. */
		std::uint32_t Elements(const DeclaredBuffer& a_buffer) const;

		/** @brief True in the first frame that binds a new version of the buffer, and in the first frame after its declaration. */
		bool FirstFrame(const DeclaredBuffer& a_buffer) const;

		/** @brief Whether the buffer is declared and live. */
		bool Has(const DeclaredBuffer& a_buffer) const { return static_cast<bool>(a_buffer); }

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
