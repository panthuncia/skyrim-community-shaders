#include "RenderGraph/Frame/Declare.h"

#include <algorithm>
#include <deque>

#include "RenderGraph/Async/Lanes.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/DeclareLane.h"
#include "RenderGraph/Frame/DeclareRegistration.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/VariablesLane.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <ORGModuleServices/Async/SerializedTaskPump.h>
#include <ORGModuleServices/Variables.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>

namespace RenderGraph::Frame
{
	namespace
	{
		/**
		* @brief The one thread check of the declarations: they are made and evaluated on the thread that owns rendering
		* (FrameAnchors::IsRenderOwner), at setup and at the adoption point, which runs there (decision 70). The registry has no
		* lock. Fatal elsewhere.
		*/
		void RequireRenderOwner(std::string_view a_what)
		{
			if (!FrameAnchors::IsRenderOwner())
				RenderGraph::Fatal("frame declaration", fmt::format("{} ran on thread {}, which does not own rendering; declarations are made and evaluated there only",
					a_what, ::GetCurrentThreadId()));
		}

		const org::services::VariableSnapshot* DeclaredSnapshotOf(const void* a_snapshot)
		{
			return static_cast<const org::services::VariableSnapshot*>(a_snapshot);
		}

		/** @brief A dependency resolved to its variable's index in the frame variables. */
		std::uint32_t IndexOfDependency(const Dependency& a_dependency, std::string_view a_owner)
		{
			if (!a_dependency.frameVariable.empty()) {
				const auto index = Vars::Lane::IndexOf(a_dependency.frameVariable);
				if (index == UINT32_MAX)
					RenderGraph::Fatal("frame declaration", fmt::format("{} depends on {}, which is no frame variable", a_owner, a_dependency.frameVariable));
				return index;
			}
			if (!a_dependency.setting)
				RenderGraph::Fatal("frame declaration", fmt::format("{} depends on a setting that is not bound (CS::Setting::Bind)", a_owner));
			std::vector<std::byte> bytes(a_dependency.settingSize);
			if (!CS::SettingRegistry::RaisedBytes(a_dependency.setting, bytes.data(), bytes.size()))
				RenderGraph::Fatal("frame declaration", fmt::format("{} depends on the setting {}, whose binding is not {} bytes", a_owner, CS::SettingRegistry::Name(a_dependency.setting),
					a_dependency.settingSize));
			if (bytes.size() > Vars::Lane::SettingValue::kCapacity)
				RenderGraph::Fatal("frame declaration", fmt::format("{} depends on the setting {}, which is too big to travel with a revision", a_owner, CS::SettingRegistry::Name(a_dependency.setting)));
			return Vars::Lane::SettingIndex(a_dependency.setting, CS::SettingRegistry::Name(a_dependency.setting), bytes.data(), bytes.size());
		}

		/** @brief What every declaration keeps to tell whether its dependencies changed. */
		struct DependencyState
		{
			std::vector<std::uint32_t> indices;
			std::vector<std::uint64_t> serials;  // at the last evaluation, 0 before the first

			void Resolve(const Dependencies& a_dependencies, std::string_view a_owner)
			{
				for (const auto& dependency : a_dependencies)
					indices.push_back(IndexOfDependency(dependency, a_owner));
				serials.assign(indices.size(), 0);
			}

			/** @brief Whether a_values changed a dependency since the last call, and remembers a_values' serials. */
			bool Changed(const org::services::VariableSnapshot& a_values)
			{
				bool changed = false;
				for (std::size_t i = 0; i < indices.size(); ++i) {
					// A variable declared after the snapshot was taken is in the next one.
					if (indices[i] >= a_values.Size()) {
						changed |= serials[i] == 0;
						continue;
					}
					const auto serial = a_values.Serial(indices[i]);
					changed |= serial != serials[i];
					serials[i] = serial;
				}
				return changed;
			}

			/** @brief The dependencies' serials in a_values, without remembering them. Zero for a variable not in it yet. */
			std::vector<std::uint64_t> SerialsIn(const org::services::VariableSnapshot& a_values) const
			{
				std::vector<std::uint64_t> out(indices.size(), 0);
				for (std::size_t i = 0; i < indices.size(); ++i)
					if (indices[i] < a_values.Size())
						out[i] = a_values.Serial(indices[i]);
				return out;
			}
		};

		using DeclaredExtent = std::array<unsigned int, 3>;

		/**
		* @brief Fatal when a size depends on a constants-only variable (decision 6a). Render-size content lives in a texture sized
		* for the display, and the dynamic part reaches constants and viewports only.
		*/
		void RequireSizes(const Dependencies& a_dependencies, const DependencyState& a_state, std::string_view a_owner)
		{
			for (std::size_t i = 0; i < a_state.indices.size() && i < a_dependencies.size(); ++i)
				if (!a_dependencies[i].frameVariable.empty() && Vars::Lane::ConstantsOnly(a_state.indices[i]))
					RenderGraph::Fatal("frame declaration", fmt::format("{} depends on {}, which holds constants only and may not size a texture or a buffer; size it for the "
												"display and pass the dynamic part as constants and viewports",
						a_owner, a_dependencies[i].frameVariable));
		}
	}

	namespace Detail
	{
		struct TextureDeclaration
		{
			std::string name;
			DependencyState dependencies;
			Texture2DDescFn desc2D;
			Texture3DDescFn desc3D;
			RenewFn renew;
			std::shared_ptr<GraphResources::Resource> twin;
			DeclaredExtent asked{};       // the extent of the last version asked for
			std::uint64_t renewed = 0;  // the renew count of the last version asked for
			// The first version is made again on the lane at the first evaluation, so it is born as the declaration says.
			bool bornPending = false;
			// For Written. The bound version a submitted run wrote (NoteWritten), compared with the bound one.
			const void* writtenVersion = nullptr;
		};

		struct BufferDeclaration
		{
			std::string name;
			DependencyState dependencies;
			BufferDescFn desc;
			D3D11_BUFFER_DESC shape{};  // the first description: only ByteWidth may change
			std::shared_ptr<GraphResources::StructuredBuffer> twin;
			std::uint32_t asked = 0;  // the element count of the last version asked for
			bool zero = false;          // born Zero: every version is uploaded as zeros on the lane
			bool bornPending = false;   // the first version is made again on the lane at the first evaluation
			const void* writtenVersion = nullptr;  // for Written, as a texture's
		};

		struct ClientDeclaration
		{
			std::string id;
			DependencyState dependencies;
			ClientFunctions functions;
			ErasedKey key;  // the registered client's, null while none is registered
			bool registered = false;
			bool invalid = true;        // evaluated at the next revision: a new declaration
			bool adoptPending = false;  // the registration's adopted call has not run yet
			// A client with a state (makeState). Its registrations' states by generation, oldest first, until the frames move past
			// them, and the adopted registration's.
			std::uint64_t generation = 0;
			std::deque<std::pair<std::uint64_t, ErasedState>> states;
			ErasedState adoptedState;
		};

		/** @brief One build of a program for a key: a slot on the programs queue (ProgramService.h). */
		struct ProgramBuild
		{
			ErasedKey key;
			std::shared_ptr<Programs::Detail::Slot> slot;
		};

		/** @brief A key the key function gave, and the dependencies' serials it was given for. */
		struct ProgramKeyAt
		{
			std::vector<std::uint64_t> serials;
			ErasedKey key;
		};

		struct ProgramDeclaration
		{
			std::string name;
			DependencyState dependencies;
			ProgramFunctions functions;
			// Render thread only. Every key built, kept for the session.
			std::vector<std::pair<ErasedKey, ErasedProgram>> built;
			std::vector<ProgramBuild> building;
			ErasedKey wanted;     // the key of the newest values checked
			bool invalid = true;  // evaluate at the next revision start: a new declaration
			// The newest keys the key function gave, newest last. For() and the adoption look keys up here.
			std::deque<ProgramKeyAt> keys;
			ErasedProgram adopted;
			std::shared_ptr<const void> adoptedValues;  // the snapshot adopted was looked up for
		};
	}

	namespace
	{
		struct DeclarationRegistry
		{
			// Render thread only.
			std::vector<std::weak_ptr<Detail::TextureDeclaration>> textures;
			std::vector<std::weak_ptr<Detail::BufferDeclaration>> buffers;
			std::vector<std::weak_ptr<Detail::ClientDeclaration>> clients;
			std::vector<std::weak_ptr<Detail::ProgramDeclaration>> programs;
			bool dirty = false;
		};

		/** @brief How many keys a program declaration remembers. Revisions in flight need only the last few. */
		constexpr std::size_t kRememberedKeys = 8;

		/** @brief Remembers the key a_values gave. Render thread. */
		void RememberKey(Detail::ProgramDeclaration& a_program, const org::services::VariableSnapshot& a_values, Detail::ErasedKey a_key)
		{
			a_program.keys.push_back({ a_program.dependencies.SerialsIn(a_values), std::move(a_key) });
			while (a_program.keys.size() > kRememberedKeys)
				a_program.keys.pop_front();
		}

		/**
		* @brief The key for a_values: the newest one the key function gave for the same dependency serials. Calls the key function
		* only for values no revision start evaluated, such as the newest written ones. Render thread.
		*/
		Detail::ErasedKey KeyAt(const Detail::ProgramDeclaration& a_program, const Inputs& a_values)
		{
			if (const auto* values = DeclaredSnapshotOf(a_values.Snapshot())) {
				const auto serials = a_program.dependencies.SerialsIn(*values);
				for (auto it = a_program.keys.rbegin(); it != a_program.keys.rend(); ++it)
					if (it->serials == serials)
						return it->key;
			}
			return a_program.functions.key(a_values);
		}

		/** @brief The build of a_key, or null when it is not built. Render thread. */
		const Detail::ErasedProgram* FindProgram(const Detail::ProgramDeclaration& a_program, const Detail::ErasedKey& a_key)
		{
			for (const auto& [key, program] : a_program.built)
				if (a_program.functions.equal(key.get(), a_key.get()))
					return &program;
			return nullptr;
		}

		/** @brief Moves the builds Programs::Collect collected into the built set. It is fatal there when one failed. Render thread. */
		void CollectProgramBuilds(Detail::ProgramDeclaration& a_program)
		{
			std::erase_if(a_program.building, [&](const Detail::ProgramBuild& a_build) {
				if (!a_build.slot->collected)
					return false;
				a_program.built.emplace_back(a_build.key, a_build.slot->program);
				logger::info("[ORG] frame declaration: program {} built on the programs queue ({} kept)", a_program.name, a_program.built.size());
				return true;
			});
		}

		/** @brief Queues a build of a_key unless one is queued. It gates the next revision start. Render thread. */
		void StartProgramBuild(Detail::ProgramDeclaration& a_program, const Detail::ErasedKey& a_key)
		{
			for (const auto& running : a_program.building)
				if (a_program.functions.equal(running.key.get(), a_key.get()))
					return;
			logger::info("[ORG] frame declaration: program {} is built for new values; the next revision starts once it is", a_program.name);
			auto slot = Programs::Detail::Queue(a_program.name, [key = a_key, build = a_program.functions.build]() -> std::shared_ptr<const void> {
				ZoneScopedN("CS.Frame.Program.Build");
				return build(key.get());
			}, true);
			a_program.building.push_back({ a_key, std::move(slot) });
		}

		DeclarationRegistry& DeclarationsOf()
		{
			static DeclarationRegistry s_registry;
			return s_registry;
		}

		template <class Desc>
		DeclaredExtent ExtentOfDesc(const Desc& a_desc)
		{
			if constexpr (std::is_same_v<Desc, D3D11_TEXTURE3D_DESC>)
				return { a_desc.Width, a_desc.Height, a_desc.Depth };
			else
				return { a_desc.Width, a_desc.Height, a_desc.ArraySize };
		}

		/** @brief Whether two descriptions differ in more than their extent. A 2D texture's slice count is part of its extent. */
		template <class Desc>
		bool ShapeDiffers(Desc a_left, Desc a_right)
		{
			a_left.Width = a_right.Width;
			a_left.Height = a_right.Height;
			if constexpr (std::is_same_v<Desc, D3D11_TEXTURE3D_DESC>)
				a_left.Depth = a_right.Depth;
			else
				a_left.ArraySize = a_right.ArraySize;
			return std::memcmp(&a_left, &a_right, sizeof(Desc)) != 0;
		}

		template <class Twin, class DescFn>
		DeclaredTexture<Twin> DeclareTexture(std::string a_name, Dependencies a_dependencies, DescFn a_desc, GraphResources::Options a_options, RenewFn a_renew)
		{
			RequireRenderOwner(fmt::format("the declaration of {}", a_name));
			if (!a_desc)
				RenderGraph::Fatal("frame declaration", fmt::format("{} has no description", a_name));
			auto declaration = std::make_shared<Detail::TextureDeclaration>();
			declaration->name = a_name;
			declaration->dependencies.Resolve(a_dependencies, a_name);
			RequireSizes(a_dependencies, declaration->dependencies, a_name);
			// Versions are what a revision changes.
			a_options.versioned = true;
			// A transient aliases memory and cannot take a version, so a declared texture is Persistent or History.
			if (a_options.lifetime == GraphResources::Lifetime::Transient)
				RenderGraph::Fatal("frame declaration", fmt::format("{} is declared Transient; a declared texture keeps its own memory, so declare it Persistent or History", a_name));
			// A frame transient's bytes are shared within the frame, so its first writer in each frame writes it, never its birth.
			if (a_options.frameTransient && (a_options.lifetime == GraphResources::Lifetime::History || a_options.birth.Writes()))
				RenderGraph::Fatal("frame declaration", fmt::format("{} is a frame transient with a History lifetime or a birth that writes it; its "
																	"contents live within one frame",
															a_name));
			const auto desc = a_desc(Inputs(nullptr));
			auto twin = std::make_shared<Twin>(desc, a_name, a_options);
			if (!*twin)
				RenderGraph::Fatal("frame declaration", fmt::format("{} could not be made", a_name));
			// Every version is born on the lane, so the first one is made there too when the birth writes it.
			if (const auto why = twin->WhyUnborn())
				RenderGraph::Fatal("frame declaration", fmt::format("{} cannot be born as declared: {}", a_name, *why));
			declaration->bornPending = twin->BirthOf().Writes();
			auto* typed = twin.get();
			declaration->asked = ExtentOfDesc(desc);
			if (a_renew)
				declaration->renewed = a_renew(Inputs(nullptr));
			declaration->renew = std::move(a_renew);
			declaration->twin = std::move(twin);
			if constexpr (std::is_same_v<Twin, GraphResources::Texture3D>)
				declaration->desc3D = std::move(a_desc);
			else
				declaration->desc2D = std::move(a_desc);
			auto& registry = DeclarationsOf();
			registry.textures.push_back(declaration);
			registry.dirty = true;
			return { std::move(declaration), typed };
		}

		/** @brief A texture's description at a_values. Fatal on a change other than the extent. */
		DeclaredExtent EvaluateTexture(Detail::TextureDeclaration& a_texture, const Inputs& a_values)
		{
			if (a_texture.desc3D) {
				auto& twin = static_cast<GraphResources::Texture3D&>(*a_texture.twin);
				const auto desc = a_texture.desc3D(a_values);
				if (ShapeDiffers(desc, twin.Desc()))
					RenderGraph::Fatal("frame declaration", fmt::format("{}'s description changed more than its extent", a_texture.name));
				return ExtentOfDesc(desc);
			}
			auto& twin = static_cast<GraphResources::Texture2D&>(*a_texture.twin);
			const auto desc = a_texture.desc2D(a_values);
			if (ShapeDiffers(desc, twin.Desc()))
				RenderGraph::Fatal("frame declaration", fmt::format("{}'s description changed more than its extent", a_texture.name));
			return ExtentOfDesc(desc);
		}

		/** @brief A buffer description's element count. Fatal on a change other than ByteWidth, or a part of an element. */
		std::uint32_t BufferElements(const Detail::BufferDeclaration& a_buffer, const D3D11_BUFFER_DESC& a_desc)
		{
			auto shape = a_desc;
			shape.ByteWidth = a_buffer.shape.ByteWidth;
			if (std::memcmp(&shape, &a_buffer.shape, sizeof(D3D11_BUFFER_DESC)) != 0)
				RenderGraph::Fatal("frame declaration", fmt::format("{}'s description changed more than its size", a_buffer.name));
			if (!a_desc.ByteWidth || a_desc.ByteWidth % a_desc.StructureByteStride != 0)
				RenderGraph::Fatal("frame declaration", fmt::format("{}'s size, {} bytes, is no whole number of {}-byte elements", a_buffer.name, a_desc.ByteWidth, a_desc.StructureByteStride));
			return a_desc.ByteWidth / a_desc.StructureByteStride;
		}

		/** @brief The smallest extent a texture of a_mips fixed mips has: one texel at its last mip. */
		std::uint32_t DeclaredSmallestExtent(std::uint32_t a_mips)
		{
			return 1u << ((std::max)(a_mips, 1u) - 1u);
		}

		/** @brief a_desc, or its smallest extent while a_exists says the texture does not exist. */
		template <class Desc, class DescFn>
		std::function<Desc(const Inputs&)> ExistingDesc(std::string_view a_name, DescFn a_desc, Existence a_exists)
		{
			if (!a_exists.on)
				return a_desc;
			return [name = std::string(a_name), desc = std::move(a_desc), on = std::move(a_exists.on)](const Inputs& a_inputs) {
				auto out = desc(a_inputs);
				if (!out.MipLevels)
					RenderGraph::Fatal("frame declaration", fmt::format("{} has an existence rule and the whole mip chain; give it a fixed mip count", name));
				if (on(a_inputs))
					return out;
				const auto smallest = DeclaredSmallestExtent(out.MipLevels);
				out.Width = smallest;
				out.Height = smallest;
				if constexpr (std::is_same_v<Desc, D3D11_TEXTURE3D_DESC>)
					out.Depth = smallest;
				return out;
			};
		}

		/** @brief a_dependencies and the existence rule's, in one list. */
		Dependencies WithExistence(Dependencies a_dependencies, const Existence& a_exists)
		{
			for (const auto& dependency : a_exists.dependencies)
				a_dependencies.push_back(dependency);
			return a_dependencies;
		}

		/** @brief Registers, registers again or removes a client for a new key. The need is empty when nothing changed. */
		std::optional<FrameAnchors::StageBuildNeed> EvaluateClient(Detail::ClientDeclaration& a_client, const Inputs& a_values)
		{
			auto key = a_client.functions.key(a_values);
			if (!key && !a_client.registered)
				return std::nullopt;
			if (key && a_client.registered && a_client.functions.equal(key.get(), a_client.key.get()))
				return std::nullopt;
			const auto before = a_client.registered ? FrameAnchors::ClientSegments(a_client.id) : 0;
			if (!key) {
				FrameAnchors::Unregister(a_client.id);
				a_client.registered = false;
				a_client.adoptPending = false;
				a_client.key.reset();
				a_client.adoptedState.reset();
				logger::info("[ORG] frame declaration: client {} removed for the next revision", a_client.id);
				return FrameAnchors::StageBuildNeed{ before, FrameAnchors::ClientsSerial(), fmt::format("the removal of client '{}'", a_client.id) };
			}
			FrameAnchors::ClientDesc desc{};
			Detail::ErasedState state;
			const bool made = a_client.functions.makeState ? static_cast<bool>(state = a_client.functions.makeState(key.get(), desc)) :
			                                                 a_client.functions.make(key.get(), desc);
			if (!made)
				RenderGraph::Fatal("frame declaration", fmt::format("client {} could not be made for its new key", a_client.id));
			desc.id = a_client.id;
			// The generation names the registration, so Held finds the state of the one a schedule holds.
			desc.generation = 0;
			if (state) {
				desc.generation = ++a_client.generation;
				a_client.states.emplace_back(desc.generation, std::move(state));
			}
			if (!FrameAnchors::Register(std::move(desc)))
				RenderGraph::Fatal("frame declaration", fmt::format("client {} was refused by the frame anchors", a_client.id));
			a_client.registered = true;
			a_client.adoptPending = a_client.functions.adopted || a_client.functions.makeState;
			a_client.key = std::move(key);
			logger::info("[ORG] frame declaration: client {} registered for the next revision", a_client.id);
			return FrameAnchors::StageBuildNeed{ before | FrameAnchors::ClientSegments(a_client.id), FrameAnchors::ClientsSerial(),
				fmt::format("client '{}'", a_client.id) };
		}
	}

	template <class T>
	T Inputs::Get(Vars::Var<T> a_variable) const
	{
		const auto index = Vars::Lane::IndexOf(a_variable.name);
		if (index == UINT32_MAX)
			RenderGraph::Fatal("frame declaration", fmt::format("{} is no frame variable", a_variable.name));
		const org::services::Variable<T> variable{ index };
		if (const auto* values = DeclaredSnapshotOf(snapshot); values && index < values->Size())
			return values->Get(variable);
		return Vars::Lane::Set().Get(variable);
	}

	template Vars::Extent2D Inputs::Get(Vars::Var<Vars::Extent2D>) const;
	template Vars::Extent3D Inputs::Get(Vars::Var<Vars::Extent3D>) const;
	template Vars::Ratios Inputs::Get(Vars::Var<Vars::Ratios>) const;
	template Vars::Upscaler Inputs::Get(Vars::Var<Vars::Upscaler>) const;
	template bool Inputs::Get(Vars::Var<bool>) const;
	template Vars::TargetIdentity Inputs::Get(Vars::Var<Vars::TargetIdentity>) const;
	template Vars::ShadowMapShape Inputs::Get(Vars::Var<Vars::ShadowMapShape>) const;
	template Vars::CubemapShape Inputs::Get(Vars::Var<Vars::CubemapShape>) const;
	template Vars::HeightMapIdentity Inputs::Get(Vars::Var<Vars::HeightMapIdentity>) const;
	template Vars::ImageSpaceShape Inputs::Get(Vars::Var<Vars::ImageSpaceShape>) const;
	template Vars::SurfaceFormat Inputs::Get(Vars::Var<Vars::SurfaceFormat>) const;
	template std::uint32_t Inputs::Get(Vars::Var<std::uint32_t>) const;
	template ::RenderGraph::Engine::Targets::ShapeTable Inputs::Get(Vars::Var<::RenderGraph::Engine::Targets::ShapeTable>) const;

	void Inputs::ReadSetting(CS::SettingVariableId a_id, void* a_out, std::size_t a_size) const
	{
		if (!a_id)
			RenderGraph::Fatal("frame declaration", "a read of a setting that is not bound (CS::Setting::Bind)");
		const auto index = Vars::Lane::SettingIndex(a_id, CS::SettingRegistry::Name(a_id), nullptr, 0);
		if (const auto* values = DeclaredSnapshotOf(snapshot); values && index != UINT32_MAX && index < values->Size()) {
			const auto& value = values->Get(org::services::Variable<Vars::Lane::SettingValue>{ index });
			if (value.size != a_size)
				RenderGraph::Fatal("frame declaration", fmt::format("the setting {} holds {} bytes in its revision, not {}", CS::SettingRegistry::Name(a_id), value.size, a_size));
			std::memcpy(a_out, value.bytes.data(), a_size);
			return;
		}
		// Not in a revision yet: the value it was last raised with.
		if (!CS::SettingRegistry::RaisedBytes(a_id, a_out, a_size))
			RenderGraph::Fatal("frame declaration", fmt::format("the setting {} is not bound at {} bytes", CS::SettingRegistry::Name(a_id), a_size));
	}

	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options,
		RenewFn a_renew)
	{
		return DeclareTexture<GraphResources::Texture2D>(std::move(a_name), std::move(a_dependencies), std::move(a_desc), a_options, std::move(a_renew));
	}

	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options,
		RenewFn a_renew)
	{
		return DeclareTexture<GraphResources::Texture3D>(std::move(a_name), std::move(a_dependencies), std::move(a_desc), a_options, std::move(a_renew));
	}

	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options,
		Existence a_exists)
	{
		auto dependencies = WithExistence(std::move(a_dependencies), a_exists);
		auto desc = ExistingDesc<D3D11_TEXTURE2D_DESC>(a_name, std::move(a_desc), std::move(a_exists));
		return DeclareTexture<GraphResources::Texture2D>(std::move(a_name), std::move(dependencies), std::move(desc), a_options, {});
	}

	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options,
		Existence a_exists)
	{
		auto dependencies = WithExistence(std::move(a_dependencies), a_exists);
		auto desc = ExistingDesc<D3D11_TEXTURE3D_DESC>(a_name, std::move(a_desc), std::move(a_exists));
		return DeclareTexture<GraphResources::Texture3D>(std::move(a_name), std::move(dependencies), std::move(desc), a_options, {});
	}

	DeclaredTexture2D DeclareTexture2D(std::string a_name, SizedDesc2D a_desc, const GraphResources::Options& a_options, Existence a_exists)
	{
		return DeclareTexture2D(std::move(a_name), std::move(a_desc.dependencies), std::move(a_desc.desc), a_options, std::move(a_exists));
	}

	SizedDesc2D DisplaySized(D3D11_TEXTURE2D_DESC a_desc, std::uint32_t a_divisor)
	{
		const auto divisor = (std::max)(a_divisor, 1u);
		return { DependsOn(Vars::DisplaySize()), [a_desc, divisor](const Inputs& a_inputs) {
					const auto display = a_inputs.Get(Vars::DisplaySize());
					if (!display.width || !display.height)
						RenderGraph::Fatal("frame declaration", "DisplaySize is zero; the engine target set writes it before the first declaration");
					auto desc = a_desc;
					desc.Width = (std::max)(display.width / divisor, 1u);
					desc.Height = (std::max)(display.height / divisor, 1u);
					return desc;
				} };
	}

	namespace Detail
	{
		struct TransientDeclaration
		{
			std::string name;
			Dependencies dependencies;
			Texture2DDescFn desc2D;
			Texture3DDescFn desc3D;
			// The description at declaration. Make changes only its extent.
			D3D11_TEXTURE2D_DESC base2D{};
			D3D11_TEXTURE3D_DESC base3D{};
			GraphResources::Options options;
		};
	}

	namespace
	{
		/** @brief Fatal unless a_options make an aliasable transient, or a History one kept apart. */
		void RequireTransientOptions(std::string_view a_name, const GraphResources::Options& a_options)
		{
			if (a_options.lifetime == GraphResources::Lifetime::Persistent || a_options.versioned || a_options.d3d11 != GraphResources::Export::None ||
				a_options.joinSlot)
				RenderGraph::Fatal("frame declaration", fmt::format("{} is a declared transient with a lifetime, versions or an export it cannot have; it is "
															"Transient (or History to keep it apart), graph-only and unversioned",
															a_name));
		}

		std::shared_ptr<Detail::TransientDeclaration> MakeTransientDeclaration(std::string a_name, Dependencies a_dependencies,
			const GraphResources::Options& a_options)
		{
			RequireRenderOwner(fmt::format("the declaration of {}", a_name));
			RequireTransientOptions(a_name, a_options);
			// Resolved once, so a dependency that is no variable is fatal here.
			DependencyState state;
			state.Resolve(a_dependencies, a_name);
			RequireSizes(a_dependencies, state, a_name);
			auto declaration = std::make_shared<Detail::TransientDeclaration>();
			declaration->name = std::move(a_name);
			declaration->dependencies = std::move(a_dependencies);
			declaration->options = a_options;
			return declaration;
		}

		/** @brief a_desc with a_extent's size. */
		template <class Desc>
		Desc WithExtent(Desc a_desc, const DeclaredExtent& a_extent)
		{
			a_desc.Width = a_extent[0];
			a_desc.Height = a_extent[1];
			if constexpr (std::is_same_v<Desc, D3D11_TEXTURE3D_DESC>)
				a_desc.Depth = a_extent[2];
			else
				a_desc.ArraySize = a_extent[2];
			return a_desc;
		}

		/** @brief The declared transients this thread registered for the graph being built, with their extents. */
		thread_local std::vector<std::string> t_buildTransients;
	}

	DeclaredTransient2D DeclareTransient2D(std::string a_name, SizedDesc2D a_desc, const GraphResources::Options& a_options)
	{
		if (!a_desc.desc)
			RenderGraph::Fatal("frame declaration", fmt::format("{} has no description", a_name));
		auto declaration = MakeTransientDeclaration(std::move(a_name), std::move(a_desc.dependencies), a_options);
		declaration->base2D = a_desc.desc(Inputs(nullptr));
		declaration->desc2D = std::move(a_desc.desc);
		return DeclaredTransient2D(std::move(declaration));
	}

	DeclaredTransient3D DeclareTransient3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options)
	{
		if (!a_desc)
			RenderGraph::Fatal("frame declaration", fmt::format("{} has no description", a_name));
		auto declaration = MakeTransientDeclaration(std::move(a_name), std::move(a_dependencies), a_options);
		declaration->base3D = a_desc(Inputs(nullptr));
		declaration->desc3D = std::move(a_desc);
		return DeclaredTransient3D(std::move(declaration));
	}

	template <class Twin>
	const std::string& DeclaredTransient<Twin>::Name() const
	{
		return declaration->name;
	}

	template <class Twin>
	const Dependencies& DeclaredTransient<Twin>::Depends() const
	{
		return declaration->dependencies;
	}

	template <class Twin>
	typename DeclaredTransient<Twin>::Extent DeclaredTransient<Twin>::ExtentAt(const Inputs& a_values) const
	{
		if constexpr (std::is_same_v<Twin, GraphResources::Texture3D>) {
			const auto desc = declaration->desc3D(a_values);
			if (ShapeDiffers(desc, declaration->base3D))
				RenderGraph::Fatal("frame declaration", fmt::format("{}'s description changed more than its extent", declaration->name));
			return ExtentOfDesc(desc);
		} else {
			const auto desc = declaration->desc2D(a_values);
			if (ShapeDiffers(desc, declaration->base2D))
				RenderGraph::Fatal("frame declaration", fmt::format("{}'s description changed more than its extent", declaration->name));
			return ExtentOfDesc(desc);
		}
	}

	template <class Twin>
	typename DeclaredTransient<Twin>::Extent DeclaredTransient<Twin>::AdoptedExtent() const
	{
		return ExtentAt(Inputs(Vars::Lane::Adopted().get()));
	}

	template <class Twin>
	std::shared_ptr<Twin> DeclaredTransient<Twin>::Make(const Extent& a_extent) const
	{
		std::shared_ptr<Twin> twin;
		if constexpr (std::is_same_v<Twin, GraphResources::Texture3D>)
			twin = std::make_shared<Twin>(WithExtent(declaration->base3D, a_extent), declaration->name, declaration->options);
		else
			twin = std::make_shared<Twin>(WithExtent(declaration->base2D, a_extent), declaration->name, declaration->options);
		if (!*twin)
			RenderGraph::Fatal("frame declaration",
				fmt::format("the declared transient {} could not be made at {}x{}x{}", declaration->name, a_extent[0], a_extent[1], a_extent[2]));
		return twin;
	}

	template class DeclaredTransient<GraphResources::Texture2D>;
	template class DeclaredTransient<GraphResources::Texture3D>;

	void RegisterTransients(org::RenderGraph& a_graph, const std::vector<const GraphResources::Resource*>& a_transients)
	{
		for (const auto* twin : a_transients) {
			if (!twin)
				continue;
			twin->Register(a_graph);
			const auto extent = twin->CurrentExtent();
			t_buildTransients.push_back(fmt::format("{} {}x{}x{}{}", twin->Name(), extent[0], extent[1], extent[2], twin->Aliasable() ? "" : " (kept apart)"));
		}
	}

	void Detail::LogBuildTransients(std::string_view a_build)
	{
		if (t_buildTransients.empty())
			return;
		std::string list;
		for (const auto& entry : t_buildTransients)
			list += fmt::format("{}{}", list.empty() ? "" : ", ", entry);
		t_buildTransients.clear();
		logger::info("[ORG] frame declaration: the {} build places declared transients in its alias pool: {}", a_build, list);
	}

	DeclaredBuffer DeclareBuffer(std::string a_name, Dependencies a_dependencies, BufferDescFn a_desc, const GraphResources::Options& a_options)
	{
		RequireRenderOwner(fmt::format("the declaration of {}", a_name));
		if (!a_desc)
			RenderGraph::Fatal("frame declaration", fmt::format("{} has no description", a_name));
		auto declaration = std::make_shared<Detail::BufferDeclaration>();
		declaration->name = a_name;
		declaration->dependencies.Resolve(a_dependencies, a_name);
		RequireSizes(a_dependencies, declaration->dependencies, a_name);
		const auto desc = a_desc(Inputs(nullptr));
		if (!(desc.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || !desc.StructureByteStride)
			RenderGraph::Fatal("frame declaration", fmt::format("{} is no structured buffer", a_name));
		declaration->shape = desc;
		const auto elements = BufferElements(*declaration, desc);
		// Versions are what a revision changes.
		auto options = a_options;
		options.versioned = true;
		auto twin = std::make_shared<GraphResources::StructuredBuffer>(desc, elements, a_name, options);
		if (!*twin)
			RenderGraph::Fatal("frame declaration", fmt::format("{} could not be made", a_name));
		if (!twin->BufferVersions())
			RenderGraph::Fatal("frame declaration", fmt::format("{} has no versions: a declared buffer is not a transient", a_name));
		using BirthKind = Birth::Kind;
		const auto birth = a_options.birth.GetKind();
		if (birth == BirthKind::Value || birth == BirthKind::Fill)
			RenderGraph::Fatal("frame declaration", fmt::format("{} is a buffer born with a value or contents; a buffer is born Zero or Undefined", a_name));
		declaration->zero = birth == BirthKind::Zero;
		declaration->bornPending = declaration->zero;
		auto* typed = twin.get();
		declaration->asked = elements;
		declaration->desc = std::move(a_desc);
		declaration->twin = std::move(twin);
		auto& registry = DeclarationsOf();
		registry.buffers.push_back(declaration);
		registry.dirty = true;
		return { std::move(declaration), typed };
	}

	bool DeclaredClient::Registered() const
	{
		return declaration && declaration->registered;
	}

	Detail::ErasedHeld Detail::HeldOf(ClientDeclaration* a_client, FrameAnchors::Anchor a_anchor)
	{
		ErasedHeld out;
		if (!a_client || !a_client->registered) {
			out.placement.reason = FrameAnchors::Reason::Unregistered;
			return out;
		}
		out.placement = FrameAnchors::Place(a_client->id, a_anchor);
		// A client without a state has only its placement.
		if (!out.placement.draws || !a_client->functions.makeState)
			return out;
		auto& states = a_client->states;
		const auto generation = out.placement.generation;
		const auto found = std::ranges::find_if(states, [&](const auto& a_state) { return a_state.first == generation; });
		if (found == states.end())
			RenderGraph::Fatal("frame declaration", fmt::format("the frame's schedule holds registration {} of client {}, which it never made or the frames moved past", generation,
				a_client->id));
		// Schedules only move forward, so the older registrations are done.
		states.erase(states.begin(), found);
		out.state = states.front().second;
		return out;
	}

	Detail::ErasedState Detail::AdoptedOf(const ClientDeclaration* a_client)
	{
		return a_client ? a_client->adoptedState : nullptr;
	}

	DeclaredClient Detail::DeclareClient(std::string a_id, Dependencies a_dependencies, ClientFunctions a_functions)
	{
		RequireRenderOwner(fmt::format("the declaration of client {}", a_id));
		if (!a_functions.key || !a_functions.equal || (!a_functions.make && !a_functions.makeState))
			RenderGraph::Fatal("frame declaration", fmt::format("client {} has no key or make function", a_id));
		auto declaration = std::make_shared<ClientDeclaration>();
		declaration->id = std::move(a_id);
		declaration->dependencies.Resolve(a_dependencies, declaration->id);
		declaration->functions = std::move(a_functions);
		auto& registry = DeclarationsOf();
		registry.clients.push_back(declaration);
		registry.dirty = true;
		return DeclaredClient(std::move(declaration));
	}

	std::shared_ptr<Detail::ProgramDeclaration> Detail::DeclareProgram(std::string a_name, Dependencies a_dependencies, ProgramFunctions a_functions)
	{
		RequireRenderOwner(fmt::format("the declaration of program {}", a_name));
		if (!a_functions.key || !a_functions.equal || !a_functions.build)
			RenderGraph::Fatal("frame declaration", fmt::format("program {} has no key or build function", a_name));
		auto declaration = std::make_shared<ProgramDeclaration>();
		declaration->name = std::move(a_name);
		declaration->dependencies.Resolve(a_dependencies, declaration->name);
		declaration->functions = std::move(a_functions);
		// Nothing is built here. The next revision start evaluates the key and builds it on the programs lane, and that
		// revision starts once it is built.
		auto& registry = DeclarationsOf();
		registry.programs.push_back(declaration);
		registry.dirty = true;
		return declaration;
	}

	Detail::ErasedProgram Detail::ProgramFor(const ProgramDeclaration* a_program, const Inputs& a_values)
	{
		if (!a_program)
			return nullptr;
		const auto key = KeyAt(*a_program, a_values);
		if (const auto* found = FindProgram(*a_program, key))
			return *found;
		RenderGraph::Fatal("frame declaration", fmt::format("program {} has no build for this revision's values", a_program->name));
	}

	Detail::ErasedProgram Detail::AdoptedProgram(const ProgramDeclaration* a_program)
	{
		return a_program ? a_program->adopted : nullptr;
	}

	Vars::Var<std::uint32_t> Detail::DeriveFromSetting(std::string_view a_name, const Dependency& a_setting, std::function<std::uint32_t(const void*)> a_fn)
	{
		// A Var holds a view of its name, so the names live for the process. Render thread, at setup.
		static std::deque<std::string> s_names;
		const auto name = fmt::format("Derived.{}", a_name);
		for (const auto& known : s_names)
			if (known == name)
				return { known };
		if (!a_fn)
			RenderGraph::Fatal("frame declaration", fmt::format("{} has no function", name));
		const auto setting = IndexOfDependency(a_setting, name);
		const auto size = a_setting.settingSize;
		const auto& stored = s_names.emplace_back(name);
		// Runs where the lane takes its values. It reads the setting's bytes from the snapshot being built.
		Vars::Lane::DeriveUint(stored, setting, [fn = std::move(a_fn), setting, size, label = std::string_view(stored)](const org::services::VariableSnapshot& a_values) {
			const auto& value = a_values.Get(org::services::Variable<Vars::Lane::SettingValue>{ setting });
			if (value.size != size)
				RenderGraph::Fatal("frame declaration", fmt::format("{} reads a setting of {} bytes, which holds {}", label, size, value.size));
			return fn(value.bytes.data());
		});
		return { stored };
	}

	Inputs AdoptedRevision::Values() const
	{
		return Inputs(Vars::Lane::Adopted().get());
	}

	std::array<std::uint32_t, 3> AdoptedRevision::ExtentOf(const Detail::TextureDeclaration* a_texture) const
	{
		if (!a_texture || !a_texture->twin)
			return {};
		const auto extent = a_texture->twin->CurrentExtent();
		return { extent[0], extent[1], extent[2] };
	}

	const void* AdoptedRevision::VersionOf(const Detail::TextureDeclaration* a_texture) const
	{
		return a_texture && a_texture->twin ? a_texture->twin->Graph().get() : nullptr;
	}

	bool AdoptedRevision::WrittenOf(const Detail::TextureDeclaration* a_texture) const
	{
		const void* version = VersionOf(a_texture);
		return version && a_texture->writtenVersion == version;
	}

	std::uint32_t AdoptedRevision::Elements(const DeclaredBuffer& a_buffer) const
	{
		return a_buffer ? a_buffer->Count() : 0;
	}

	const void* AdoptedRevision::Version(const DeclaredBuffer& a_buffer) const
	{
		return a_buffer ? a_buffer->Graph().get() : nullptr;
	}

	bool AdoptedRevision::Written(const DeclaredBuffer& a_buffer) const
	{
		const void* version = Version(a_buffer);
		return version && a_buffer.Declaration()->writtenVersion == version;
	}

	void Detail::NoteWritten(TextureDeclaration* a_texture, const void* a_version)
	{
		if (a_texture && a_version && a_texture->twin && a_texture->twin->Graph().get() == a_version)
			a_texture->writtenVersion = a_version;
	}

	void NoteWritten(const DeclaredBuffer& a_buffer, const void* a_version)
	{
		if (a_buffer && a_version && a_buffer->Graph().get() == a_version)
			a_buffer.Declaration()->writtenVersion = a_version;
	}

	namespace Declarations
	{
		bool Dirty()
		{
			return DeclarationsOf().dirty || Programs::Pending();
		}

		std::vector<FrameAnchors::StageBuildNeed> Evaluate(const org::services::VariableSnapshot& a_values, const TextureExtentFn& a_textures,
			const BufferElementsFn& a_buffers)
		{
			RequireRenderOwner("the evaluation of the declarations");
			auto& registry = DeclarationsOf();
			registry.dirty = false;
			const Inputs inputs(&a_values);
			std::erase_if(registry.textures, [](const auto& a_weak) { return a_weak.expired(); });
			for (const auto& weak : registry.textures) {
				const auto texture = weak.lock();
				const bool changed = texture->dependencies.Changed(a_values);
				// The first version is made again on the lane, so it is born as declared.
				const bool born = std::exchange(texture->bornPending, false);
				if (!changed && !born)
					continue;
				const auto extent = EvaluateTexture(*texture, inputs);
				// A new renew count asks for a new version at the same extent.
				const auto renewed = texture->renew ? texture->renew(inputs) : texture->renewed;
				const bool renew = renewed != texture->renewed || born;
				if (extent == texture->asked && !renew)
					continue;
				texture->asked = extent;
				texture->renewed = renewed;
				a_textures(*texture->twin, extent, renew);
			}
			std::erase_if(registry.buffers, [](const auto& a_weak) { return a_weak.expired(); });
			for (const auto& weak : registry.buffers) {
				const auto buffer = weak.lock();
				const bool changed = buffer->dependencies.Changed(a_values);
				const bool born = std::exchange(buffer->bornPending, false);
				if (!changed && !born)
					continue;
				const auto elements = BufferElements(*buffer, buffer->desc(inputs));
				if (elements == buffer->asked && !born)
					continue;
				buffer->asked = elements;
				a_buffers(*buffer->twin, elements, buffer->zero);
			}
			std::vector<FrameAnchors::StageBuildNeed> needs;
			std::erase_if(registry.clients, [](const auto& a_weak) { return a_weak.expired(); });
			for (const auto& weak : registry.clients) {
				const auto client = weak.lock();
				const bool changed = client->dependencies.Changed(a_values);
				if (!changed && !client->invalid)
					continue;
				client->invalid = false;
				if (auto need = EvaluateClient(*client, inputs))
					needs.push_back(std::move(*need));
			}
			return needs;
		}

		bool ProgramsReady(const org::services::VariableSnapshot& a_values)
		{
			auto& registry = DeclarationsOf();
			std::erase_if(registry.programs, [](const auto& a_weak) { return a_weak.expired(); });
			// The service's gating builds too: fixed programs queued after setup. DCLF's engine draw programs gate nothing.
			bool ready = Programs::Collect();
			const Inputs inputs(&a_values);
			for (const auto& weak : registry.programs) {
				const auto program = weak.lock();
				CollectProgramBuilds(*program);
				const bool changed = program->dependencies.Changed(a_values);
				// The key function runs only here, and only for a change. For() and the adoption reuse the key.
				if (changed || std::exchange(program->invalid, false)) {
					program->wanted = program->functions.key(inputs);
					RememberKey(*program, a_values, program->wanted);
				}
				if (FindProgram(*program, program->wanted))
					continue;
				// Latest wins: a build for older values keeps running and is kept, and this one is queued behind it.
				StartProgramBuild(*program, program->wanted);
				ready = false;
			}
			return ready;
		}

		void JoinProgramsAtSetup(const org::services::VariableSnapshot& a_values)
		{
			RequireRenderOwner("the setup build of the declared programs");
			if (ProgramsReady(a_values))
				return;
			// A setup wait, before any frame. The declared builds are slots of the programs queue, so its setup join waits for them.
			Programs::JoinSetup("the setup build of the declared programs");
			if (!ProgramsReady(a_values))
				RenderGraph::Fatal("frame declaration", "a declared program has no build for the setup values after the setup wait");
		}

		std::size_t AdoptAtSetup()
		{
			RequireRenderOwner("the setup adoption of the declared clients");
			std::size_t adopted = 0;
			// Copied, since an adopted call may declare.
			const auto clients = DeclarationsOf().clients;
			for (const auto& weak : clients) {
				const auto client = weak.lock();
				if (!client || !client->adoptPending || !client->registered)
					continue;
				client->adoptPending = false;
				if (!client->states.empty())
					client->adoptedState = client->states.back().second;
				if (client->functions.adopted)
					client->functions.adopted(client->key.get(), client->adoptedState);
				++adopted;
			}
			return adopted;
		}

		void NoteBegin(std::uint64_t)
		{
			auto& registry = DeclarationsOf();
			// Written compares the bound version with the one a run wrote, so a new version needs nothing here.
			// The programs follow the adopted values. Their builds were done before the revision that carries them started.
			const auto& adopted = Vars::Lane::Adopted();
			if (!adopted)
				return;
			const Inputs inputs(adopted.get());
			for (const auto& weak : registry.programs)
				if (const auto program = weak.lock(); program && program->adoptedValues.get() != adopted.get()) {
					program->adoptedValues = adopted;
					// A program declared after the adopted revision started may have no build of its values yet. It keeps the last.
					if (const auto* found = FindProgram(*program, KeyAt(*program, inputs)))
						program->adopted = *found;
				}
			// A registration is the frame's once the adopted values give its key: the revision that carried them waited for its builds.
			const Inputs values(adopted.get());
			// Copied, since an adopted call may declare.
			const auto clients = registry.clients;
			for (const auto& weak : clients) {
				const auto client = weak.lock();
				if (!client || !client->adoptPending || !client->registered)
					continue;
				const auto key = client->functions.key(values);
				if (!key || !client->functions.equal(key.get(), client->key.get()))
					continue;
				client->adoptPending = false;
				// The newest registration is the registered key's.
				if (!client->states.empty())
					client->adoptedState = client->states.back().second;
				if (client->functions.adopted)
					client->functions.adopted(client->key.get(), client->adoptedState);
			}
		}
	}

	namespace Revision
	{
		const Frame::AdoptedRevision& Adopted()
		{
			static const Frame::AdoptedRevision s_adopted;
			return s_adopted;
		}
	}
}

