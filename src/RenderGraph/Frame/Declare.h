#pragma once

#include "RenderGraph/Frame/Birth.h"
#include "RenderGraph/Frame/Placement.h"
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

namespace org
{
	class RenderGraph;
}

namespace FrameAnchors
{
	struct ClientDesc;
	enum class Anchor : std::uint32_t;
}

/**
* @brief The CS declaration API (frame revision plan, step 6).
*
* A feature declares its textures, buffers and clients as functions of variables: the frame-wide ones (Frame/Variables.h) and its
* own CS::Setting fields. It does that once, at setup, on the thread that owns rendering. A change of a dependency is evaluated when the next
* frame revision starts (Frame/Revision.h), against the values that revision carries:
* - a texture whose description changes gets a new version, made off the render thread. Every version, the first one too, is
*   born as its declaration's Birth says (GraphResources::Options::birth, Frame/Birth.h), on the lane. Written says whether a
*   run wrote the version the frame binds;
* - a structured buffer whose element count changes gets a new version, made off the render thread, born zero or undefined;
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

		/** @brief A CS::Setting's value by its id, for a function that must not hold the setting (its owner may be replaced). */
		template <class T>
		T SettingById(CS::SettingVariableId a_id) const
		{
			T value{};
			ReadSetting(a_id, &value, sizeof(T));
			return value;
		}

		/** @brief Null reads the newest written values (at declaration). Otherwise an org::services::VariableSnapshot. */
		explicit Inputs(const void* a_snapshot) :
			snapshot(a_snapshot) {}

		/** @brief The org::services::VariableSnapshot read, or null for the newest written values. */
		const void* Snapshot() const { return snapshot; }

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

	/**
	* @brief A description from the inputs. Only the extent may change between revisions (same format, mips, flags). A 2D texture's
	* ArraySize may change too (ORG array-size versions): its revision switches in with graphs built for the new count, so it takes
	* a few frames longer than a new extent. A cube's faces and whether its views are arrays stay.
	*/
	using Texture2DDescFn = std::function<D3D11_TEXTURE2D_DESC(const Inputs&)>;
	using Texture3DDescFn = std::function<D3D11_TEXTURE3D_DESC(const Inputs&)>;

	/**
	* @brief A count from the inputs. A new count makes a new version even at the same extent, as a reset does. A History texture's
	* new version is born cleared, and FirstFrame says so.
	*/
	using RenewFn = std::function<std::uint64_t(const Inputs&)>;

	/**
	* @brief Render owner (fatal elsewhere), at setup. Makes a versioned twin from a_desc of the newest values and follows a_dependencies from then
	* on. a_options.versioned is forced on. A Transient lifetime is fatal: declare Persistent or History. A description that changes
	* more than the extent is fatal. a_renew, when given, reads
	* only a_dependencies too. Never null: a twin that cannot be made is fatal.
	*
	* a_options.birth is applied by the revision lane to every version. A birth that writes (Zero, Value, Fill) makes the first
	* version on the lane too: the first revision after this call carries it, so no frame binds a version that was not born. A
	* birth the lane cannot make (GraphResources::Resource::WhyUnborn) is fatal.
	*/
	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options,
		RenewFn a_renew = {});
	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options,
		RenewFn a_renew = {});

	/**
	* @brief Whether a declared texture exists for a revision's values (the existence rule, cleanup plan B6). A texture that does not
	* exist keeps its slot and its published id at its smallest extent: one texel, or 2^(mips-1) for a fixed mip count. Empty: it
	* always exists.
	*/
	struct Existence
	{
		Dependencies dependencies;
		std::function<bool(const Inputs&)> on;
	};

	/**
	* @brief Exists while every one of a_settings is true, as the revision carries them. Only their ids are kept, so a feature
	* object that is replaced later (Post Processing's sub-features) leaves nothing dangling.
	*/
	template <class... S>
	Existence ExistsWhen(const CS::Setting<bool>& a_setting, const S&... a_settings)
	{
		Existence existence;
		existence.dependencies = DependsOn(a_setting, a_settings...);
		existence.on = [ids = std::array{ a_setting.Id(), a_settings.Id()... }](const Inputs& a_inputs) {
			for (const auto id : ids)
				if (!a_inputs.SettingById<bool>(id))
					return false;
			return true;
		};
		return existence;
	}

	/** @brief Exists while a frame variable is true. */
	inline Existence ExistsWhen(Vars::Var<bool> a_variable)
	{
		return { DependsOn(a_variable), [a_variable](const Inputs& a_inputs) { return a_inputs.Get(a_variable); } };
	}

	/** @brief Exists while a_on is true. a_on reads only a_dependencies. */
	inline Existence ExistsWhen(Dependencies a_dependencies, std::function<bool(const Inputs&)> a_on)
	{
		return { std::move(a_dependencies), std::move(a_on) };
	}

	/** @brief A description with the dependencies it reads, such as DisplaySized gives. */
	struct SizedDesc2D
	{
		Dependencies dependencies;
		Texture2DDescFn desc;
	};

	/**
	* @brief a_desc at the display size divided by a_divisor (at least one texel), following DisplaySize. The engine target set
	* writes DisplaySize before the first declaration, so a zero display size is fatal.
	*/
	SizedDesc2D DisplaySized(D3D11_TEXTURE2D_DESC a_desc, std::uint32_t a_divisor = 1);

	/**
	* @brief DeclareTexture2D of a sized description that exists only while a_exists says so: one texel (or the smallest extent of
	* its mip count) otherwise, so its slot and published id never go away. A mip count of 0 (the whole chain) is fatal with an
	* existence rule, since the chain would change with the extent.
	*/
	DeclaredTexture2D DeclareTexture2D(std::string a_name, SizedDesc2D a_desc, const GraphResources::Options& a_options, Existence a_exists = {});

	/** @brief DeclareTexture2D/3D with the existence rule: a_desc while a_exists says so, the smallest extent otherwise. */
	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options,
		Existence a_exists);
	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options,
		Existence a_exists);

	namespace Detail
	{
		struct TransientDeclaration;
	}

	/**
	* @brief A declared transient: a texture whose description follows variables but which takes no versions. Each registration of
	* the client that uses it makes its own transient (Make), so the builds of that registration alias it in their pool.
	*
	* The client depends on Depends() and keys on ExtentAt(), and its make function calls Make with the key's extent. A change
	* then registers the client again, and the revision waits for the builds of its stages, so the new size switches in with
	* them at one Begin. Its passes write it before they read it, in one run of one stage. No later run, epoch or frame reads it,
	* and it is never published. Render thread.
	*/
	template <class Twin>
	class DeclaredTransient
	{
	public:
		/** @brief Width, height, and depth (3D) or slice count (2D). */
		using Extent = std::array<std::uint32_t, 3>;

		DeclaredTransient() = default;
		explicit DeclaredTransient(std::shared_ptr<Detail::TransientDeclaration> a_declaration) :
			declaration(std::move(a_declaration)) {}

		explicit operator bool() const { return declaration != nullptr; }

		/** @brief The name every transient it makes takes. */
		const std::string& Name() const;

		/** @brief The variables its description reads. The client that uses it depends on them too. */
		const Dependencies& Depends() const;

		/** @brief The extent for a revision's values, for the client's key. Fatal on a change other than the extent. */
		Extent ExtentAt(const Inputs& a_values) const;

		/** @brief The extent for the adopted revision's values: the one the frame's builds were made with. */
		Extent AdoptedExtent() const;

		/** @brief A new transient at a_extent, for one registration. Never null: a transient that cannot be made is fatal. */
		std::shared_ptr<Twin> Make(const Extent& a_extent) const;

	private:
		std::shared_ptr<Detail::TransientDeclaration> declaration;
	};

	using DeclaredTransient2D = DeclaredTransient<GraphResources::Texture2D>;
	using DeclaredTransient3D = DeclaredTransient<GraphResources::Texture3D>;

	/**
	* @brief Render owner (fatal elsewhere), at setup. A transient described by a_desc of a_dependencies. a_options.lifetime is
	* Transient, or History to keep it out of the pool (a diagnostic); other lifetimes, versions and exports are fatal. Only the
	* extent may change between revisions, as for DeclareTexture2D.
	*/
	DeclaredTransient2D DeclareTransient2D(std::string a_name, SizedDesc2D a_desc, const GraphResources::Options& a_options);
	DeclaredTransient3D DeclareTransient3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options);

	/**
	* @brief Registers one registration's declared transients with a graph being built, from the client's registerResources.
	* The stage logs them once per build. Null entries are skipped.
	*/
	void RegisterTransients(org::RenderGraph& a_graph, const std::vector<const GraphResources::Resource*>& a_transients);

	namespace Detail
	{
		/** @brief Logs the declared transients RegisterTransients registered on this thread since the last call, for a_build. */
		void LogBuildTransients(std::string_view a_build);
	}

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
	* @brief Render owner (fatal elsewhere), at setup. Makes a versioned structured buffer twin from a_desc of the newest values and follows
	* a_dependencies from then on. A new element count is a new version of the same slot, made on the revision lane and adopted
	* with the revision that carries the values, so the frame never sees a count its passes were not recorded for. A version is
	* born as a_options.birth says: Zero uploads zeros on the lane (the first version too), Unset or Undefined leaves it undefined
	* until a pass writes it; Value and Fill are fatal for a buffer. An exported one gets its D3D11 export at adoption.
	* a_options.versioned is forced on, and a transient is fatal. A description that changes more than ByteWidth is fatal. Never
	* null: a twin that cannot be made is fatal.
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

		/** @brief Whether the client is registered now. */
		bool Registered() const;

		explicit operator bool() const { return declaration != nullptr; }

		const std::shared_ptr<Detail::ClientDeclaration>& Declaration() const { return declaration; }

	private:
		std::shared_ptr<Detail::ClientDeclaration> declaration;
	};

	/**
	* @brief The registration a run uses now: its placement (FrameAnchors::Placement) and, for a client with a state, that
	* registration's state. Keep it for one frame at most.
	* - placement.draws: the frame's schedule holds a registration and has it on. A state client's state is that registration's.
	* - reason Unregistered: the client has no registration (its key is empty).
	* - !placement.InStage(): no stage of this frame takes the anchor's work for the client (the reason says why).
	* - otherwise a stage takes the work, but its schedule does not hold the client on yet: nothing is drawn.
	*/
	template <class State>
	class Held
	{
	public:
		Held() = default;
		Held(std::shared_ptr<State> a_state, FrameAnchors::Placement a_placement) :
			state(std::move(a_state)), placement(a_placement) {}

		State* operator->() const { return state.get(); }
		template <class S = State>
			requires(!std::is_void_v<S>)
		S& operator*() const
		{
			return *state;
		}
		State* Get() const { return state.get(); }
		const std::shared_ptr<State>& Shared() const { return state; }
		/** @brief A state client: whether a state is held. A client without a state: whether its registration draws. */
		explicit operator bool() const
		{
			if constexpr (std::is_void_v<State>)
				return placement.draws;
			else
				return state != nullptr;
		}

		const FrameAnchors::Placement& Placement() const { return placement; }
		/** @brief The registration draws now. */
		bool Draws() const { return placement.draws; }
		/** @brief A stage takes the work, but its schedule does not hold the client on yet. */
		bool Lagging() const { return placement.InStage() && !placement.draws; }
		FrameAnchors::Reason Reason() const { return placement.reason; }
		/** @brief The registration's generation (FrameAnchors::ClientDesc::generation), 0 when none is held. */
		std::uint64_t Generation() const { return placement.draws ? placement.generation : 0; }

	private:
		std::shared_ptr<State> state;
		FrameAnchors::Placement placement;
	};

	namespace Detail
	{
		using ErasedKey = std::shared_ptr<const void>;
		using ErasedState = std::shared_ptr<const void>;
		struct ClientFunctions
		{
			std::function<ErasedKey(const Inputs&)> key;                       // null: no client
			std::function<bool(const void*, const void*)> equal;               // two non-null keys
			std::function<bool(const void*, FrameAnchors::ClientDesc&)> make;  // the registration for a key, or
			// the registration and its state, for a client with a state. Null is fatal. Declare sets the generation.
			std::function<ErasedState(const void*, FrameAnchors::ClientDesc&)> makeState;
			std::function<void(const void*, const ErasedState&)> adopted;  // may be empty. The state is null without makeState
		};
		DeclaredClient DeclareClient(std::string a_id, Dependencies a_dependencies, ClientFunctions a_functions);

		struct ErasedHeld
		{
			ErasedState state;
			FrameAnchors::Placement placement;
		};
		ErasedHeld HeldOf(ClientDeclaration* a_client, FrameAnchors::Anchor a_anchor);
		ErasedState AdoptedOf(const ClientDeclaration* a_client);

		template <class State>
		std::shared_ptr<State> StateOf(const ErasedState& a_state)
		{
			return std::static_pointer_cast<State>(std::const_pointer_cast<void>(a_state));
		}
	}

	/**
	* @brief A declared client with a state per registration (DeclareClient<Key, State>). Declare keeps each registration's state
	* until the frames move past it.
	*/
	template <class State>
	class DeclaredClientOf : public DeclaredClient
	{
	public:
		DeclaredClientOf() = default;
		explicit DeclaredClientOf(DeclaredClient a_client) :
			DeclaredClient(std::move(a_client)) {}

		/**
		* @brief Render owner. The state of the registration a run of a_anchor uses now (FrameAnchors::Place). A schedule holding a
		* registration Declare does not know is fatal. With a_anchor Count, any open stage that holds the client.
		*/
		::RenderGraph::Frame::Held<State> Held(FrameAnchors::Anchor a_anchor) const
		{
			auto held = Detail::HeldOf(Declaration().get(), a_anchor);
			return ::RenderGraph::Frame::Held<State>(Detail::StateOf<State>(held.state), held.placement);
		}

		/**
		* @brief Render thread. The state of the registration the adopted revision holds: from the first Begin whose adopted values
		* give its key. Null before that, and once a revision start drops the client.
		*/
		std::shared_ptr<State> Adopted() const { return Detail::StateOf<State>(Detail::AdoptedOf(Declaration().get())); }
	};

	namespace Detail
	{
		/** @brief A client's make and adopted calls: without a state (State void), or with a state per registration. */
		template <class Key, class State>
		struct ClientCalls
		{
			using Make = std::function<std::shared_ptr<State>(const Key&, FrameAnchors::ClientDesc&)>;
			using Adopted = std::function<void(const Key&, State&)>;
		};

		template <class Key>
		struct ClientCalls<Key, void>
		{
			using Make = std::function<bool(const Key&, FrameAnchors::ClientDesc&)>;
			using Adopted = std::function<void(const Key&)>;
		};
	}

	/**
	* @brief Render owner (fatal elsewhere), at setup. The one way to register a client. The client a_id is a function of
	* a_dependencies: a_key gives what its registration depends on, or nothing for no client, and a_make fills the registration
	* for a key. A new key registers the client again when a revision starts, and the revision waits for the stage builds that
	* hold it, so the registration and its builds switch in at one Begin. An equal key does nothing, so a key may quantise a
	* float (density > 0). The first evaluation is at the first revision after this call.
	*
	* - State void: a_make returns false for a failed make, which is fatal.
	* - Otherwise a_make also returns the registration's state (null is fatal), such as its passes' shapes or its build. Declare
	*   keeps the states and sets the generation, so the frame finds the registration its schedule holds with Held(anchor) and
	*   the adopted one with Adopted(). The newest registration's commit runs for an older one the schedule still holds, so the
	*   commit reads Held's state, never the newest.
	*
	* a_adopted, if given, is called inside the adoption point of the first Begin whose adopted values give the registered key,
	* once per registration, with the key (and the state). The frame runs that registration from then on. Use it to switch in
	* what the frame binds outside the graph, such as a new resource published under an old id. It needs a key of variables
	* only: a key that reads other state may never match the adopted values.
	*/
	template <class Key, class State = void>
	DeclaredClientOf<State> DeclareClient(std::string a_id, Dependencies a_dependencies, std::function<std::optional<Key>(const Inputs&)> a_key,
		typename Detail::ClientCalls<Key, State>::Make a_make, typename Detail::ClientCalls<Key, State>::Adopted a_adopted = {})
	{
		Detail::ClientFunctions functions;
		functions.key = [key = std::move(a_key)](const Inputs& a_inputs) -> Detail::ErasedKey {
			auto value = key(a_inputs);
			if (!value)
				return nullptr;
			return std::make_shared<const Key>(std::move(*value));
		};
		functions.equal = [](const void* a_left, const void* a_right) { return *static_cast<const Key*>(a_left) == *static_cast<const Key*>(a_right); };
		if constexpr (std::is_void_v<State>) {
			functions.make = [make = std::move(a_make)](const void* a_keyValue, FrameAnchors::ClientDesc& a_out) { return make(*static_cast<const Key*>(a_keyValue), a_out); };
			if (a_adopted)
				functions.adopted = [adopted = std::move(a_adopted)](const void* a_keyValue, const Detail::ErasedState&) { adopted(*static_cast<const Key*>(a_keyValue)); };
		} else {
			functions.makeState = [make = std::move(a_make)](const void* a_keyValue, FrameAnchors::ClientDesc& a_out) -> Detail::ErasedState {
				return make(*static_cast<const Key*>(a_keyValue), a_out);
			};
			if (a_adopted)
				functions.adopted = [adopted = std::move(a_adopted)](const void* a_keyValue, const Detail::ErasedState& a_state) {
					adopted(*static_cast<const Key*>(a_keyValue), *Detail::StateOf<State>(a_state));
				};
		}
		return DeclaredClientOf<State>(Detail::DeclareClient(std::move(a_id), std::move(a_dependencies), std::move(functions)));
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

		const std::shared_ptr<Detail::ProgramDeclaration>& Declaration() const { return declaration; }

		explicit operator bool() const { return declaration != nullptr; }

	private:
		std::shared_ptr<Detail::ProgramDeclaration> declaration;
	};

	/**
	* @brief Render owner (fatal elsewhere), at setup. A program, such as a shader permutation, as a function of a_dependencies. a_key gives what
	* the program is built from (its defines, a sample count), and a_build builds it for a key. Returns a DeclaredProgram of the
	* type a_build returns a shared_ptr to.
	* - Every key is built off the render thread, on the programs lane (ProgramService.h), so a_build must be safe there. The
	*   first one too: nothing is built here.
	* - A revision whose values need a key not built yet does not start until the build is done. Frames keep the adopted
	*   revision meanwhile, so a compile never blocks a Begin. A client's key or make reads the new program with For().
	* - a_key runs when a revision starts and a dependency changed, never more often. A key that reads state outside CS reads
	*   it through an Observed (Frame/Observed.h) it depends on. For()
	*   and the adoption reuse the key it gave for those values. Keep the expensive part (planning) behind a key change.
	* - Every key built is kept for the session, so going back to one costs nothing. Keys compare with operator==.
	* - A failed build is fatal.
	* In the frame, read the program with Revision::Adopted().Program(), which is null until the first revision with the
	* program is adopted. Empty without the graph.
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
		/** @brief The program for the adopted revision's values. Null before the first revision that built it is adopted. */
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

		/** @brief The extent of the bound version: width, height, and depth (3D) or slice count (2D). */
		template <class Twin>
		std::array<std::uint32_t, 3> Extent(const DeclaredTexture<Twin>& a_texture) const
		{
			return ExtentOf(a_texture.Declaration().get());
		}

		/**
		* @brief The version the frame binds now, for a commit to hand to NoteWritten from its executed callback. Null when the
		* texture is not declared.
		*/
		template <class Twin>
		const void* Version(const DeclaredTexture<Twin>& a_texture) const
		{
			return VersionOf(a_texture.Declaration().get());
		}

		/**
		* @brief Whether a submitted run wrote the version the frame binds (NoteWritten). False from the adoption of a new version
		* until the executed callback of the first run that wrote it. A history whose version is not written holds its birth only.
		*/
		template <class Twin>
		bool Written(const DeclaredTexture<Twin>& a_texture) const
		{
			return WrittenOf(a_texture.Declaration().get());
		}

		/** @brief !Written: true from a new version's adoption until the first run that writes it was submitted. */
		template <class Twin>
		bool FirstFrame(const DeclaredTexture<Twin>& a_texture) const
		{
			return !Written(a_texture);
		}

		/** @brief Whether the texture is declared and live. */
		template <class Twin>
		bool Has(const DeclaredTexture<Twin>& a_texture) const
		{
			return static_cast<bool>(a_texture);
		}

		/** @brief The element count of the buffer's bound version. Zero when it is not declared. */
		std::uint32_t Elements(const DeclaredBuffer& a_buffer) const;

		/** @brief The buffer's bound version, for NoteWritten. Null when it is not declared. */
		const void* Version(const DeclaredBuffer& a_buffer) const;

		/** @brief Whether a submitted run wrote the buffer's bound version (NoteWritten). */
		bool Written(const DeclaredBuffer& a_buffer) const;

		/** @brief !Written: true from a new version's adoption until the first run that writes it was submitted. */
		bool FirstFrame(const DeclaredBuffer& a_buffer) const { return !Written(a_buffer); }

		/** @brief Whether the buffer is declared and live. */
		bool Has(const DeclaredBuffer& a_buffer) const { return static_cast<bool>(a_buffer); }

	private:
		Inputs Values() const;
		std::array<std::uint32_t, 3> ExtentOf(const Detail::TextureDeclaration* a_texture) const;
		const void* VersionOf(const Detail::TextureDeclaration* a_texture) const;
		bool WrittenOf(const Detail::TextureDeclaration* a_texture) const;
	};

	namespace Detail
	{
		void NoteWritten(TextureDeclaration* a_texture, const void* a_version);
	}

	/**
	* @brief Render thread, in a client's executed callback: the run that was just submitted wrote a_texture's version a_version,
	* which its commit read from Adopted().Version(). Written is true from then on while that version is bound. A version that is no
	* longer bound is ignored, so a run that wrote an older version marks nothing.
	*/
	template <class Twin>
	void NoteWritten(const DeclaredTexture<Twin>& a_texture, const void* a_version)
	{
		Detail::NoteWritten(a_texture.Declaration().get(), a_version);
	}

	/** @brief NoteWritten for a declared buffer. */
	void NoteWritten(const DeclaredBuffer& a_buffer, const void* a_version);
}

namespace RenderGraph::Frame::Revision
{
	/** @brief Render thread. The revision the frame runs. */
	const Frame::AdoptedRevision& Adopted();
}
