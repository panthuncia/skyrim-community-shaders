#include "RenderGraph/Frame/Declare.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

#	include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#	include "RenderGraph/Frame/DeclareLane.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/VariablesLane.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <ORGModuleServices/Async/SerializedTaskPump.h>
#	include <ORGModuleServices/Variables.h>

#	include <atomic>
#	include <chrono>
#	include <cstring>
#	include <deque>
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
			RenewFn renew;
			std::shared_ptr<GraphResources::Resource> twin;
			DeclaredExtent wished{};
			std::uint64_t renewed = 0;  // the renew count of the last version asked for
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
			bool invalid = true;         // evaluated at the next revision. A new declaration is.
			bool adoptPending = false;  // the registration's adopted call has not run yet
		};

		/** @brief One build of a program for a key, on the programs lane. The lane writes the result, then done. */
		struct ProgramBuild
		{
			std::string name;
			ErasedKey key;
			std::function<ErasedProgram(const void*)> build;
			ErasedProgram program;
			std::string error;
			double milliseconds = 0.0;
			std::atomic<bool> done{ false };
		};

		struct ProgramDeclaration
		{
			std::string name;
			DependencyState dependencies;
			ProgramFunctions functions;
			// Render thread only. Every key built, kept for the session.
			std::vector<std::pair<ErasedKey, ErasedProgram>> built;
			std::vector<std::shared_ptr<ProgramBuild>> building;
			ErasedKey wanted;  // the key of the newest values checked
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
			std::vector<std::weak_ptr<Detail::ClientDeclaration>> clients;
			std::vector<std::weak_ptr<Detail::ProgramDeclaration>> programs;
			bool dirty = false;
		};

		/** @brief The programs lane's queue. The render thread posts builds, the lane runs them one at a time. */
		struct ProgramLaneState
		{
			std::mutex mutex;
			std::deque<std::shared_ptr<Detail::ProgramBuild>> queue;
			org::async::SerializedTaskPump* pump = nullptr;
		};

		ProgramLaneState& ProgramLaneOf()
		{
			// Never destroyed: a build may still finish during process teardown.
			static auto* s_lane = new ProgramLaneState;
			return *s_lane;
		}

		/** @brief The lane's work: every queued build, one at a time. Never on the render thread. */
		void DrainProgramLane()
		{
			auto& lane = ProgramLaneOf();
			for (;;) {
				std::shared_ptr<Detail::ProgramBuild> job;
				{
					const std::lock_guard lock(lane.mutex);
					if (lane.queue.empty())
						return;
					job = std::move(lane.queue.front());
					lane.queue.pop_front();
				}
				ZoneScopedN("CS.Frame.Program.Build");
				const auto start = std::chrono::steady_clock::now();
				try {
					job->program = job->build(job->key.get());
					if (!job->program)
						job->error = "its build returned nothing";
				} catch (const std::exception& e) {
					job->error = e.what();
				}
				job->milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				job->build = {};
				job->done.store(true, std::memory_order_release);
			}
		}

		/**
		* @brief The programs lane. Builds compile synchronously, so it runs on the executor's streaming class, whose threads take
		* blocking work, not on the preparation pool the compiles themselves may run on.
		*/
		org::async::SerializedTaskPump& ProgramLane()
		{
			auto& lane = ProgramLaneOf();
			if (!lane.pump) {
				static const org::async::Scope scope = DCLF::SceneScheduler::Executor().CreateScope("CS frame programs");
				lane.pump = DCLF::SceneScheduler::MakeLanePump(DCLF::SceneScheduler::Executor(), scope, DCLF::SceneExecutor::Streaming,
					org::async::TaskDispatch::Cpu, "CS frame programs", [] { DrainProgramLane(); },
					[] { logger::critical("[ORG] frame declaration: the programs lane stopped (its task was refused)"); });
			}
			return *lane.pump;
		}

		/** @brief The build of a_key, or null when it is not built. Render thread. */
		const Detail::ErasedProgram* FindProgram(const Detail::ProgramDeclaration& a_program, const Detail::ErasedKey& a_key)
		{
			for (const auto& [key, program] : a_program.built)
				if (a_program.functions.equal(key.get(), a_key.get()))
					return &program;
			return nullptr;
		}

		/** @brief Moves finished builds into the built set. Fatal when one failed. Render thread. */
		void CollectProgramBuilds(Detail::ProgramDeclaration& a_program)
		{
			std::erase_if(a_program.building, [&](const std::shared_ptr<Detail::ProgramBuild>& a_build) {
				if (!a_build->done.load(std::memory_order_acquire))
					return false;
				if (!a_build->error.empty())
					FailDeclaration(fmt::format("the program {} could not be built: {}", a_program.name, a_build->error));
				a_program.built.emplace_back(a_build->key, a_build->program);
				logger::info("[ORG] frame declaration: program {} built on the programs lane in {:.1f} ms ({} kept)", a_program.name, a_build->milliseconds,
					a_program.built.size());
				return true;
			});
		}

		/** @brief Posts a build of a_key unless one is running. Render thread. */
		void StartProgramBuild(Detail::ProgramDeclaration& a_program, const Detail::ErasedKey& a_key)
		{
			for (const auto& running : a_program.building)
				if (a_program.functions.equal(running->key.get(), a_key.get()))
					return;
			auto job = std::make_shared<Detail::ProgramBuild>();
			job->name = a_program.name;
			job->key = a_key;
			job->build = a_program.functions.build;
			a_program.building.push_back(job);
			{
				auto& lane = ProgramLaneOf();
				const std::lock_guard lock(lane.mutex);
				lane.queue.push_back(std::move(job));
			}
			logger::info("[ORG] frame declaration: program {} is built for new values; the next revision starts once it is", a_program.name);
			if (!ProgramLane().Notify())
				FailDeclaration(fmt::format("the programs lane refused the build of {}", a_program.name));
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
		DeclaredTexture<Twin> DeclareTexture(std::string a_name, Dependencies a_dependencies, DescFn a_desc, GraphResources::Options a_options, RenewFn a_renew)
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
				a_client.adoptPending = false;
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
			a_client.adoptPending = static_cast<bool>(a_client.functions.adopted);
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
	template Vars::ImageSpaceShape Inputs::Get(Vars::Var<Vars::ImageSpaceShape>) const;

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

	std::shared_ptr<Detail::ProgramDeclaration> Detail::DeclareProgram(std::string a_name, Dependencies a_dependencies, ProgramFunctions a_functions)
	{
		if (!a_functions.key || !a_functions.equal || !a_functions.build)
			FailDeclaration(fmt::format("program {} has no key or build function", a_name));
		auto declaration = std::make_shared<ProgramDeclaration>();
		declaration->name = std::move(a_name);
		declaration->dependencies.Resolve(a_dependencies, declaration->name);
		declaration->functions = std::move(a_functions);
		// The first key is built here, at setup, so the frame has a program before the first revision.
		declaration->wanted = declaration->functions.key(Inputs(nullptr));
		auto program = declaration->functions.build(declaration->wanted.get());
		if (!program) {
			logger::error("[ORG] frame declaration: program {} could not be built", declaration->name);
			return nullptr;
		}
		declaration->built.emplace_back(declaration->wanted, program);
		declaration->adopted = std::move(program);
		DeclarationsOf().programs.push_back(declaration);
		return declaration;
	}

	Detail::ErasedProgram Detail::ProgramFor(const ProgramDeclaration* a_program, const Inputs& a_values)
	{
		if (!a_program)
			return nullptr;
		const auto key = a_program->functions.key(a_values);
		if (const auto* found = FindProgram(*a_program, key))
			return *found;
		FailDeclaration(fmt::format("program {} has no build for this revision's values", a_program->name));
	}

	Detail::ErasedProgram Detail::AdoptedProgram(const ProgramDeclaration* a_program)
	{
		return a_program ? a_program->adopted : nullptr;
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
				// A new renew count asks for a new version at the same extent.
				const auto renewed = texture->renew ? texture->renew(inputs) : texture->renewed;
				const bool renew = renewed != texture->renewed;
				if (extent == texture->wished && !renew)
					continue;
				texture->wished = extent;
				texture->renewed = renewed;
				a_wish(*texture->twin, extent, renew);
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
			const Inputs inputs(&a_values);
			bool ready = true;
			for (const auto& weak : registry.programs) {
				const auto program = weak.lock();
				CollectProgramBuilds(*program);
				if (program->dependencies.Changed(a_values))
					program->wanted = program->functions.key(inputs);
				if (FindProgram(*program, program->wanted))
					continue;
				// Latest wins: a build for older values keeps running and is kept, and this one is queued behind it.
				StartProgramBuild(*program, program->wanted);
				ready = false;
			}
			return ready;
		}

		void NoteBegin(std::uint64_t a_frame)
		{
			auto& registry = DeclarationsOf();
			for (const auto& weak : registry.textures)
				if (const auto texture = weak.lock()) {
					const void* version = texture->twin->Graph().get();
					if (version != texture->lastVersion) {
						texture->lastVersion = version;
						texture->firstFrame = a_frame;
					}
				}
			// The programs follow the adopted values. Their builds were done before the revision that carries them started.
			const auto& adopted = Vars::Lane::Adopted();
			if (!adopted)
				return;
			const Inputs inputs(adopted.get());
			for (const auto& weak : registry.programs)
				if (const auto program = weak.lock(); program && program->adoptedValues.get() != adopted.get()) {
					program->adoptedValues = adopted;
					// A program declared after the adopted revision started may have no build of its values. It keeps its first.
					if (const auto* found = FindProgram(*program, program->functions.key(inputs)))
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
				client->functions.adopted(client->key.get());
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
		struct ProgramDeclaration
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
	template Vars::ImageSpaceShape Inputs::Get(Vars::Var<Vars::ImageSpaceShape>) const;

	void Inputs::ReadSetting(CS::SettingVariableId a_id, void* a_out, std::size_t a_size) const { CS::SettingRegistry::RaisedBytes(a_id, a_out, a_size); }
	DeclaredTexture2D DeclareTexture2D(std::string, Dependencies, Texture2DDescFn, const GraphResources::Options&, RenewFn) { return {}; }
	DeclaredTexture3D DeclareTexture3D(std::string, Dependencies, Texture3DDescFn, const GraphResources::Options&, RenewFn) { return {}; }
	void DeclaredClient::Invalidate() const {}
	bool DeclaredClient::Registered() const { return false; }
	DeclaredClient Detail::DeclareClient(std::string, Dependencies, ClientFunctions) { return {}; }
	std::shared_ptr<Detail::ProgramDeclaration> Detail::DeclareProgram(std::string, Dependencies, ProgramFunctions) { return nullptr; }
	Detail::ErasedProgram Detail::ProgramFor(const ProgramDeclaration*, const Inputs&) { return nullptr; }
	Detail::ErasedProgram Detail::AdoptedProgram(const ProgramDeclaration*) { return nullptr; }
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
