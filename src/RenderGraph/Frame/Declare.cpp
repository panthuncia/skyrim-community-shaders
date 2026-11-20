#include "RenderGraph/Frame/Declare.h"

#include <deque>

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

#	include "RenderGraph/Frame/DeclareLane.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/VariablesLane.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <ORGModuleServices/Variables.h>

#	include <cstring>
#	include <mutex>

namespace RenderGraph::Frame
{
	namespace
	{
		[[noreturn]] void FailDeclaration(std::string_view a_why)
		{
			const auto message = fmt::format("Community Shaders: a frame declaration failed: {}", a_why);
			logger::critical("[ORG] frame declaration: {}", message);
			spdlog::default_logger()->flush();
			stl::report_and_fail(message);
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
					FailDeclaration(fmt::format("{} depends on {}, which is no frame variable", a_owner, a_dependency.frameVariable));
				return index;
			}
			if (!a_dependency.setting)
				FailDeclaration(fmt::format("{} depends on a setting that is not bound (CS::Setting::Bind)", a_owner));
			std::vector<std::byte> bytes(a_dependency.settingSize);
			if (!CS::SettingRegistry::RaisedBytes(a_dependency.setting, bytes.data(), bytes.size()))
				FailDeclaration(fmt::format("{} depends on the setting {}, whose binding is not {} bytes", a_owner, CS::SettingRegistry::Name(a_dependency.setting),
					a_dependency.settingSize));
			if (bytes.size() > Vars::Lane::SettingValue::kCapacity)
				FailDeclaration(fmt::format("{} depends on the setting {}, which is too big to travel with a revision", a_owner, CS::SettingRegistry::Name(a_dependency.setting)));
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
		};

		using DeclaredExtent = std::array<unsigned int, 3>;
	}

	namespace Detail
	{
		struct TextureDeclaration
		{
			std::string name;
			DependencyState dependencies;
			Texture2DDescFn desc2D;
			Texture3DDescFn desc3D;
			std::shared_ptr<GraphResources::Resource> twin;
			DeclaredExtent wished{};
			// For FirstFrame. The version the last Begin found, and the frame it was new in.
			const void* lastVersion = nullptr;
			std::uint64_t firstFrame = UINT64_MAX;
		};

		struct ClientDeclaration
		{
			std::string id;
			DependencyState dependencies;
			ClientFunctions functions;
			ErasedKey key;  // the registered client's, null while none is registered
			bool registered = false;
			bool invalid = true;  // evaluated at the next revision. A new declaration is.
		};
	}

	namespace
	{
		struct DeclarationRegistry
		{
			// Render thread only.
			std::vector<std::weak_ptr<Detail::TextureDeclaration>> textures;
			std::vector<std::weak_ptr<Detail::ClientDeclaration>> clients;
			bool dirty = false;
		};

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
				return { a_desc.Width, a_desc.Height, 1u };
		}

		/** @brief Whether two descriptions differ in more than their extent. */
		template <class Desc>
		bool ShapeDiffers(Desc a_left, Desc a_right)
		{
			a_left.Width = a_right.Width;
			a_left.Height = a_right.Height;
			if constexpr (std::is_same_v<Desc, D3D11_TEXTURE3D_DESC>)
				a_left.Depth = a_right.Depth;
			return std::memcmp(&a_left, &a_right, sizeof(Desc)) != 0;
		}

		template <class Twin, class DescFn>
		DeclaredTexture<Twin> DeclareTexture(std::string a_name, Dependencies a_dependencies, DescFn a_desc, GraphResources::Options a_options)
		{
			if (!a_desc)
				FailDeclaration(fmt::format("{} has no description", a_name));
			auto declaration = std::make_shared<Detail::TextureDeclaration>();
			declaration->name = a_name;
			declaration->dependencies.Resolve(a_dependencies, a_name);
			// Versions are what a revision changes.
			a_options.versioned = true;
			const auto desc = a_desc(Inputs(nullptr));
			auto twin = std::make_shared<Twin>(desc, a_name, a_options);
			if (!*twin) {
				logger::error("[ORG] frame declaration: {} could not be made", a_name);
				return {};
			}
			auto* typed = twin.get();
			declaration->wished = ExtentOfDesc(desc);
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
					FailDeclaration(fmt::format("{}'s description changed more than its extent", a_texture.name));
				return ExtentOfDesc(desc);
			}
			auto& twin = static_cast<GraphResources::Texture2D&>(*a_texture.twin);
			const auto desc = a_texture.desc2D(a_values);
			if (ShapeDiffers(desc, twin.Desc()))
				FailDeclaration(fmt::format("{}'s description changed more than its extent", a_texture.name));
			return ExtentOfDesc(desc);
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
				a_client.key.reset();
				logger::info("[ORG] frame declaration: client {} removed for the next revision", a_client.id);
				return FrameAnchors::StageBuildNeed{ before, FrameAnchors::ClientsSerial(), fmt::format("the removal of client '{}'", a_client.id) };
			}
			FrameAnchors::ClientDesc desc{};
			if (!a_client.functions.make(key.get(), desc))
				FailDeclaration(fmt::format("client {} could not be made for its new key", a_client.id));
			desc.id = a_client.id;
			if (!FrameAnchors::Register(std::move(desc)))
				FailDeclaration(fmt::format("client {} was refused by the frame anchors", a_client.id));
			a_client.registered = true;
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
			FailDeclaration(fmt::format("{} is no frame variable", a_variable.name));
		const org::services::Variable<T> variable{ index };
		if (const auto* values = DeclaredSnapshotOf(snapshot); values && index < values->Size())
			return values->Get(variable);
		return Vars::Lane::Set().Get(variable);
	}

	template Vars::Extent2D Inputs::Get(Vars::Var<Vars::Extent2D>) const;
	template Vars::Ratios Inputs::Get(Vars::Var<Vars::Ratios>) const;
	template Vars::Upscaler Inputs::Get(Vars::Var<Vars::Upscaler>) const;
	template bool Inputs::Get(Vars::Var<bool>) const;
	template Vars::TargetIdentity Inputs::Get(Vars::Var<Vars::TargetIdentity>) const;
	template Vars::ShadowMapShape Inputs::Get(Vars::Var<Vars::ShadowMapShape>) const;
	template Vars::CubemapShape Inputs::Get(Vars::Var<Vars::CubemapShape>) const;
	template Vars::HeightMapIdentity Inputs::Get(Vars::Var<Vars::HeightMapIdentity>) const;
	template std::uint32_t Inputs::Get(Vars::Var<std::uint32_t>) const;

	void Inputs::ReadSetting(CS::SettingVariableId a_id, void* a_out, std::size_t a_size) const
	{
		if (!a_id)
			FailDeclaration("a read of a setting that is not bound (CS::Setting::Bind)");
		const auto index = Vars::Lane::SettingIndex(a_id, CS::SettingRegistry::Name(a_id), nullptr, 0);
		if (const auto* values = DeclaredSnapshotOf(snapshot); values && index != UINT32_MAX && index < values->Size()) {
			const auto& value = values->Get(org::services::Variable<Vars::Lane::SettingValue>{ index });
			if (value.size != a_size)
				FailDeclaration(fmt::format("the setting {} holds {} bytes in its revision, not {}", CS::SettingRegistry::Name(a_id), value.size, a_size));
			std::memcpy(a_out, value.bytes.data(), a_size);
			return;
		}
		// Not in a revision yet: the value it was last raised with.
		if (!CS::SettingRegistry::RaisedBytes(a_id, a_out, a_size))
			FailDeclaration(fmt::format("the setting {} is not bound at {} bytes", CS::SettingRegistry::Name(a_id), a_size));
	}

	DeclaredTexture2D DeclareTexture2D(std::string a_name, Dependencies a_dependencies, Texture2DDescFn a_desc, const GraphResources::Options& a_options)
	{
		return DeclareTexture<GraphResources::Texture2D>(std::move(a_name), std::move(a_dependencies), std::move(a_desc), a_options);
	}

	DeclaredTexture3D DeclareTexture3D(std::string a_name, Dependencies a_dependencies, Texture3DDescFn a_desc, const GraphResources::Options& a_options)
	{
		return DeclareTexture<GraphResources::Texture3D>(std::move(a_name), std::move(a_dependencies), std::move(a_desc), a_options);
	}

	void DeclaredClient::Invalidate() const
	{
		if (!declaration || declaration->invalid)
			return;
		declaration->invalid = true;
		DeclarationsOf().dirty = true;
	}

	bool DeclaredClient::Registered() const
	{
		return declaration && declaration->registered;
	}

	DeclaredClient Detail::DeclareClient(std::string a_id, Dependencies a_dependencies, ClientFunctions a_functions)
	{
		if (!a_functions.key || !a_functions.equal || !a_functions.make)
			FailDeclaration(fmt::format("client {} has no key or make function", a_id));
		auto declaration = std::make_shared<ClientDeclaration>();
		declaration->id = std::move(a_id);
		declaration->dependencies.Resolve(a_dependencies, declaration->id);
		declaration->functions = std::move(a_functions);
		auto& registry = DeclarationsOf();
		registry.clients.push_back(declaration);
		registry.dirty = true;
		return DeclaredClient(std::move(declaration));
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
			FailDeclaration(fmt::format("{} has no function", name));
		const auto setting = IndexOfDependency(a_setting, name);
		const auto size = a_setting.settingSize;
		const auto& stored = s_names.emplace_back(name);
		// Runs where the lane takes its values. It reads the setting's bytes from the snapshot being built.
		Vars::Lane::DeriveUint(stored, setting, [fn = std::move(a_fn), setting, size, label = std::string_view(stored)](const org::services::VariableSnapshot& a_values) {
			const auto& value = a_values.Get(org::services::Variable<Vars::Lane::SettingValue>{ setting });
			if (value.size != size)
				FailDeclaration(fmt::format("{} reads a setting of {} bytes, which holds {}", label, size, value.size));
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

	bool AdoptedRevision::FirstFrameOf(const Detail::TextureDeclaration* a_texture) const
	{
		return a_texture && a_texture->firstFrame == Driver::Current().frame;
	}

	namespace Declarations
	{
		bool Dirty()
		{
			return DeclarationsOf().dirty;
		}

		std::vector<FrameAnchors::StageBuildNeed> Evaluate(const org::services::VariableSnapshot& a_values, const Wish& a_wish)
		{
			auto& registry = DeclarationsOf();
			registry.dirty = false;
			const Inputs inputs(&a_values);
			std::erase_if(registry.textures, [](const auto& a_weak) { return a_weak.expired(); });
			for (const auto& weak : registry.textures) {
				const auto texture = weak.lock();
				if (!texture->dependencies.Changed(a_values))
					continue;
				const auto extent = EvaluateTexture(*texture, inputs);
				if (extent == texture->wished)
					continue;
				texture->wished = extent;
				a_wish(*texture->twin, extent);
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

		void NoteBegin(std::uint64_t a_frame)
		{
			for (const auto& weak : DeclarationsOf().textures)
				if (const auto texture = weak.lock()) {
					const void* version = texture->twin->Graph().get();
					if (version != texture->lastVersion) {
						texture->lastVersion = version;
						texture->firstFrame = a_frame;
					}
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

#else

namespace RenderGraph::Frame
{
	namespace Detail
	{
		struct TextureDeclaration
		{};
		struct ClientDeclaration
		{};
	}

	template <class T>
	T Inputs::Get(Vars::Var<T>) const
	{
		return T{};
	}

	template Vars::Extent2D Inputs::Get(Vars::Var<Vars::Extent2D>) const;
	template Vars::Ratios Inputs::Get(Vars::Var<Vars::Ratios>) const;
	template Vars::Upscaler Inputs::Get(Vars::Var<Vars::Upscaler>) const;
	template bool Inputs::Get(Vars::Var<bool>) const;
	template Vars::TargetIdentity Inputs::Get(Vars::Var<Vars::TargetIdentity>) const;
	template Vars::ShadowMapShape Inputs::Get(Vars::Var<Vars::ShadowMapShape>) const;
	template Vars::CubemapShape Inputs::Get(Vars::Var<Vars::CubemapShape>) const;
	template Vars::HeightMapIdentity Inputs::Get(Vars::Var<Vars::HeightMapIdentity>) const;
	template std::uint32_t Inputs::Get(Vars::Var<std::uint32_t>) const;

	void Inputs::ReadSetting(CS::SettingVariableId a_id, void* a_out, std::size_t a_size) const { CS::SettingRegistry::RaisedBytes(a_id, a_out, a_size); }
	DeclaredTexture2D DeclareTexture2D(std::string, Dependencies, Texture2DDescFn, const GraphResources::Options&) { return {}; }
	DeclaredTexture3D DeclareTexture3D(std::string, Dependencies, Texture3DDescFn, const GraphResources::Options&) { return {}; }
	void DeclaredClient::Invalidate() const {}
	bool DeclaredClient::Registered() const { return false; }
	DeclaredClient Detail::DeclareClient(std::string, Dependencies, ClientFunctions) { return {}; }
	Vars::Var<std::uint32_t> Detail::DeriveFromSetting(std::string_view a_name, const Dependency&, std::function<std::uint32_t(const void*)>)
	{
		static std::deque<std::string> s_names;
		return { s_names.emplace_back(a_name) };
	}
	Inputs AdoptedRevision::Values() const { return Inputs(nullptr); }
	std::array<std::uint32_t, 3> AdoptedRevision::ExtentOf(const Detail::TextureDeclaration*) const { return {}; }
	bool AdoptedRevision::FirstFrameOf(const Detail::TextureDeclaration*) const { return false; }

	namespace Revision
	{
		const Frame::AdoptedRevision& Adopted()
		{
			static const Frame::AdoptedRevision s_adopted;
			return s_adopted;
		}
	}
}

#endif
