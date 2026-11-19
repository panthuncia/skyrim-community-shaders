#pragma once

// The extension registry: what features registered at install, read-only afterwards.
// Switches and toggles register with Core/Settings, which owns them.

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Feature.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Extensions
{
	/**
	 * @brief Main thread, once, before DCLF's threads start. Runs every feature's install, then every integration's, then the resolutions.
	 * The frame schedule is checked last, against Core's passes and the registered ones.
	 */
	void InstallFeatures(std::span<const Api::Feature> a_features, std::span<const Api::Feature> a_integrations, const Api::FrameSchedule& a_schedule);
	/** @brief Main thread, once, before DCLF's own engine hooks and observers install. Every feature's early hooks. */
	void InstallEarlyHooks(std::span<const Api::Feature> a_features);
	/** @brief Main thread, before any DCLF hook is written. Every feature's preflight. Conflicts land in HookSites::Conflicts(). */
	void PreflightFeatures(std::span<const Api::Feature> a_features);
	/** @brief Main thread, once, before InstallFeatures. Every feature's source hooks, source by source in Api::ShadowSource order. */
	void InstallSourceHooks(std::span<const Api::Feature> a_features);
	/** @brief The frame's epoch orders as RenderGraphRuntime::Segment values: the frame order, the async order and its revision-driven subset. */
	struct FrameOrders
	{
		std::vector<std::uint32_t> epochOrder, asyncOrder, revisionEpochs;
	};
	/** @brief Main thread, after InstallFeatures. Passed to RenderGraphRuntime::ConfigureFrame before the runtime initializes. */
	const FrameOrders& ScheduledFrameOrders();

	/** @brief A registered frame-value pool, with its name owned. */
	struct FrameValuePoolEntry
	{
		std::string name;
		std::uint32_t stride = 0;
		bool deviceAddress = false;
		bool unorderedAccess = false;
		decltype(Api::FrameValuePool::frameStart) frameStart = nullptr;
		decltype(Api::FrameValuePool::kick) kick = nullptr;
		decltype(Api::FrameValuePool::sample) sample = nullptr;
		decltype(Api::FrameValuePool::send) send = nullptr;
	};
	/** @brief A registered payload product. */
	struct PayloadProductEntry
	{
		std::string name;
		decltype(Api::PayloadProduct::build) build = nullptr;
		decltype(Api::PayloadProduct::carried) carried = nullptr;
		decltype(Api::PayloadProduct::install) install = nullptr;
	};
	/** @brief Total words all scene resources may post with the revision inputs. Grow it before registering more. */
	inline constexpr std::uint32_t kSceneResourceInputWords = 12;
	/** @brief Registered scene resources. Its posted inputs sit at [firstInput, firstInput + inputWords). */
	struct SceneResourcesEntry
	{
		std::string name;
		decltype(Api::SceneResources::setup) setup = nullptr;
		decltype(Api::SceneResources::registerGraph) registerGraph = nullptr;
		decltype(Api::SceneResources::require) require = nullptr;
		decltype(Api::SceneResources::fit) fit = nullptr;
		decltype(Api::SceneResources::postInputs) postInputs = nullptr;
		std::uint32_t firstInput = 0, inputWords = 0;
		decltype(Api::SceneResources::latched) latched = nullptr;
		decltype(Api::SceneResources::sealed) sealed = nullptr;
	};
	/** @brief A registered commit hook. */
	struct CommitHookEntry
	{
		std::string name;
		decltype(Api::CommitHook::commit) commit = nullptr;
	};
	/** @brief A registered frame binding. */
	struct FrameBindingEntry
	{
		std::string name;
		std::uint32_t textureRegister = ~0u;
		decltype(Api::FrameBinding::select) select = nullptr;
	};
	/** @brief A registered frame constant row. */
	struct FrameConstantRowEntry
	{
		std::string name;
		std::uint32_t row = ~0u;
		decltype(Api::FrameConstantRow::sample) sample = nullptr;
	};
	/** @brief A registered frame texture. */
	struct FrameTextureEntry
	{
		std::string name;
		std::uint32_t textureRegister = ~0u;
		decltype(Api::FrameTexture::view) view = nullptr;  // null means the captured view, graph only
		bool capturedWhenNull = false;
		decltype(Api::FrameTexture::graphResources) graphResources{};  // static strings. Empty ones are unused.
	};
	/** @brief A feature's registered report lines. */
	struct ReportLinesEntry
	{
		std::string name;
		Api::ReportPoint point = Api::ReportPoint::PrimaryCull;
		decltype(Api::ReportLines::lines) lines = nullptr;
		decltype(Api::ReportLines::text) text = nullptr;
		Api::CoordinatorComponentId component;
		decltype(Api::ReportLines::componentText) componentText = nullptr;
	};
	/** @brief A registered engine channel, with its name owned. */
	struct EngineChannelEntry
	{
		std::string name;
		Api::ChannelKind kind = Api::ChannelKind::Keys;
		Api::ChannelApply apply = Api::ChannelApply::AfterSwitches;
		Api::WakeSource wake = Api::WakeSource::None;
		bool namesKeys = false;
		decltype(Api::EngineChannel::drain) drain = nullptr;
		decltype(Api::EngineChannel::push) push = nullptr;
	};

	/** @brief A registered table family. */
	struct TableFamilyEntry
	{
		std::string name;
		decltype(Api::TableFamily::make) make = nullptr;
		decltype(Api::TableFamily::assign) assign = nullptr;
		decltype(Api::TableFamily::assignRow) assignRow = nullptr;
		decltype(Api::TableFamily::same) same = nullptr;
		decltype(Api::TableFamily::clear) clear = nullptr;
		decltype(Api::TableFamily::recycle) recycle = nullptr;
		std::uint32_t rowsEnd = ~0u, recordWord = ~0u;
		std::uint32_t recordShift = 0, recordBias = 0;
		bool recordTag = true, journaled = false;
		decltype(Api::TableFamily::rows) rows = nullptr;
	};
	/** @brief A registered coordinator component. */
	struct CoordinatorComponentEntry
	{
		std::string name;
		decltype(Api::CoordinatorComponent::make) make = nullptr;
		Api::CoordinatorComponent::Slots slots;
		Api::CoordinatorComponent::Residency residency;
		Api::CoordinatorComponent::Accumulate accumulate;
		Api::CoordinatorComponent::Walk walk;
		Api::CoordinatorComponent::DenseWalk dense;
		Api::CoordinatorComponent::Retirement retirement;
		Api::CoordinatorComponent::EntryClassHooks entryClass;
		Api::CoordinatorComponent::Events events;
		Api::CoordinatorComponent::Commit commit;
		Api::CoordinatorComponent::Adoption adoption;
	};
	/** @brief A registered shading contribution. */
	struct ShadingContributionEntry
	{
		std::string name;
		std::uint64_t propertyFlags = 0;
		decltype(Api::ShadingContribution::derive) derive = nullptr;
		std::uint32_t objectFlags = 0, blockWords = 0;
		std::span<const Api::ShadingVariable> vsVariables;
	};
	/** @brief A registered frame-globals contribution. Its floats are FrameGlobals::features[first, first + floats). */
	struct FrameGlobalsEntry
	{
		std::string name;
		std::uint32_t first = 0, floats = 0;
		decltype(Api::FrameGlobalsContribution::capture) capture = nullptr;
	};

	// Any thread, after InstallFeatures. Registrations in order, where an index is the extension's id.
	std::span<const FrameValuePoolEntry> RegisteredPools();
	std::span<const PayloadProductEntry> RegisteredProducts();
	std::span<const SceneResourcesEntry> RegisteredSceneResources();
	std::span<const CommitHookEntry> RegisteredCommitHooks();
	std::span<const FrameBindingEntry> RegisteredFrameBindings();
	// Frame constant rows in ascending row order. Frame textures in registration order.
	std::span<const FrameConstantRowEntry> RegisteredFrameConstantRows();
	std::span<const FrameTextureEntry> RegisteredFrameTextures();
	std::span<const ReportLinesEntry> RegisteredReportLines();
	std::span<const Api::StandInObserver> RegisteredStandInObservers();
	std::span<const EngineChannelEntry> RegisteredEngineChannels();
	/** @brief The show veto cells at a fixed address the stubs load. Defaults never open and never veto when none is registered. */
	const Api::ShowVeto& RegisteredShowVeto();
	bool ShowVetoRegistered();
	/** @brief How many hidden sites were reserved after HiddenSourceSites(). */
	std::uint32_t ReservedHiddenSites();
	std::span<const TableFamilyEntry> RegisteredTableFamilies();
	std::span<const CoordinatorComponentEntry> RegisteredCoordinatorComponents();
	std::span<const ShadingContributionEntry> RegisteredShadingContributions();
	std::span<const FrameGlobalsEntry> RegisteredFrameGlobals();
}
