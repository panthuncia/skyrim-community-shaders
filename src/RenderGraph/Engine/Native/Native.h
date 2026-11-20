#pragma once

#include <d3d11.h>
#include <dxgi.h>

#include <filesystem>
#include <cstdint>
#include <memory>
#include <string>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include <winrt/base.h>

struct DxvkOrgInteropDeviceInfo;
struct DxvkOrgInteropResourceInfo;
struct VkQueue_T;
struct DxvkOrgInteropTextureWrap;

namespace rhi
{
	class CommandList;
	class DevicePtr;
	class Resource;
	struct TimelinePoint;
	struct QueueProgress;
}

namespace FrameGen
{
	struct PresentationConfig;
}

namespace org
{
	class ExternalTextureResource;
	class GloballyIndexedResource;
}

/**
 * @brief Native mode (CS_ORG_NATIVE=1) renders without DXVK. CS makes a BasicRHI device itself, and the engine's device,
 * context and swap chain are tripwire handles over it. The render graph submits straight to the device's queues and ORG presents.
 * It is a development option. The DXVK build stays the default.
 *
 * Startup runs in numbered stages, and each logs one "[ORG native] stage" line so a failed start shows how far it got.
 * Anything that still needs DXVK fails at first use with a message naming it. Native mode never falls back to DXVK.
 */
namespace RenderGraph::Engine::Native
{
	/** @brief Whether CS_ORG_NATIVE=1 asks for native mode. Any thread. */
	bool Requested();

	/** @brief Whether native mode made its device. Any thread. */
	bool Active();

	/** @brief Logs one startup stage line. */
	void Stage(std::string_view a_stage, std::string_view a_detail = {});

	/** @brief Logs a startup error naming what still needs DXVK, then ends the process with a message box. */
	[[noreturn]] void Fail(std::string_view a_dependency, std::string_view a_why);

	/** @brief Stage 1. Fails on a switch that conflicts with native mode. Must run before any other switch is read. */
	void ApplySwitches();

	/** @brief Whether the frame runs on the render graph, in native or guest mode. Any thread. */
	bool GraphMode();

	/**
	 * @brief In native or guest mode, logs why Drawcall Limit Fix will not run, then ends the process with a message box. Outside
	 * them it returns. Both modes withhold the engine's draws (the flush latch) and DCLF draws the world, so without it the world
	 * would be empty.
	 */
	void RequireDclf(std::string_view a_cause);

	/** @brief On D3D12, the DCLF switches ApplySwitches turned off for lack of a D3D12 port, for DCLF's log line. */
	std::string D3D12DclfOff();

	/** @brief Stage 2. Fails on anything that still needs DXVK. Needs the features' boot state. */
	void CheckStartup();

	/** @brief CS's runtime DLL directory, CommunityShaders/bin. */
	std::filesystem::path RuntimeDirectory();

	/** @brief Stage 3, in place of D3D11CreateDeviceAndSwapChain. Returns tripwire handles over BasicRHI's device and ORG's swap chain. */
	HRESULT CreateDeviceAndSwapChain(IDXGIAdapter* a_adapter, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_swapChain, ID3D11Device** a_device,
		D3D_FEATURE_LEVEL* a_featureLevel, ID3D11DeviceContext** a_context);

	/** @brief Checks that the engine's fields hold the native handles after its own InitD3D. */
	void AfterInitD3D();

	/** @brief BasicRHI's device once stage 3 made it, else null. */
	std::shared_ptr<rhi::DevicePtr> Device();

	/** @brief Makes the back buffer a graph-owned slot with one version per present slot. Must run before any import. */
	void OnGraphReady();

	/** @brief Loads GenerateMips' compute programs. Must run after the shader compiler exists, or engine mip builds do nothing. */
	void OnShaderCompilerReady();

	/** @brief The native equivalent of dxvkGetInteropResourceInfo, for textures native mode made. */
	bool DescribeResource(const IUnknown* a_object, DxvkOrgInteropResourceInfo& a_info);

	/**
	 * @name Graph-owned textures. Shader-only textures made with contents are "exempt" and rest in the shader-read state untracked,
	 * until a context write promotes them. Every other texture is tracked as a persistent ORG texture once the graph is up.
	 */
	/** @{ */
	/** @brief Whether native mode is active and sorts its textures as above. Any thread. */
	bool OwnedResources();

	/** @brief The graph-owned ORG texture behind a texture, view, port handle or stream shell. Null otherwise. Any thread. */
	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOf(const IUnknown* a_object);

	/** @brief GraphTextureOf for a VkImage handle, for DCLF's imports. */
	std::shared_ptr<org::GloballyIndexedResource> GraphTextureOfImage(std::uint64_t a_image);

	/** @brief An exempt texture as a read-only graph resource, made on first call. Null when the texture is not exempt. Any thread. */
	std::shared_ptr<org::ExternalTextureResource> ImmutableTextureOf(const IUnknown* a_texture, DXGI_FORMAT a_srvFormat);

	/** @brief The graph resource of an exempt texture the context wrote after creation. Null for any other texture. Any thread. */
	std::shared_ptr<org::ExternalTextureResource> PromotedTextureOf(const IUnknown* a_object);

	/** @brief Whether a native texture must be tracked when graphs read it. False for exempt textures, promoted or not. Any thread. */
	bool IsTrackedTexture(const IUnknown* a_object);
	/** @} */

	struct ResourceFacts
	{
		std::uint64_t serial = 0;     // unique for the session
		std::uint32_t bindFlags = 0;  // D3D11_BIND_*
		std::uint32_t usage = 0;      // D3D11_USAGE
		std::uint32_t dimension = 0;  // D3D11_RESOURCE_DIMENSION
		bool initialData = false;     // made with initial data
		bool written = false;         // only marked while a census or the validator is on
	};

	/** @brief The facts of the native resource behind a_object. False when native mode did not make it. Any thread. */
	bool DescribeForCensus(const void* a_object, ResourceFacts& a_out);

	/** @brief The engine's Present. Copies the back buffer into the swap chain image after the frame's graph work. */
	HRESULT Present(UINT a_syncInterval, UINT a_flags);

	/** @brief The swap chain's presentation. */
	struct PresentationState
	{
		std::uint64_t serial = 0;     // the request the swap chain was last made for. 0 is the swap chain made at startup
		std::uint64_t requested = 0;  // the latest request
		std::uint32_t format = 0;     // the swap chain images' rhi::Format
		bool hdr10 = false;
		bool presentationLayer = false;
		bool vsync = false;  // with a presentation layer, the vsync the swap chain was made for. Its presents keep it
	};

	/**
	 * @brief Replaces the swap chain with one for a_config, on the submission thread after the presents queued so far. The entry waits
	 * for the presents, destroys the swap chain, runs a_tornDown, then creates the new one (a presentation layer may wrap it when
	 * a_config names a frame generation method). The latest request wins: one superseded before it runs does nothing. A failure is
	 * fatal. Any thread.
	 * @return The request's serial. Presentation().serial reaches it once the swap chain is replaced.
	 */
	std::uint64_t RequestPresentation(const FrameGen::PresentationConfig& a_config, std::function<bool()> a_tornDown);

	/**
	 * @brief The torn-down step a replacement runs when its request names none (HDR's colour space change, say), so a feature load
	 * change waiting for a replacement goes through whichever request runs. Any thread.
	 */
	void SetPresentationTornDown(std::function<bool()> a_tornDown);

	/**
	 * @brief The step a swap chain change runs on the thread that asks for it, before anything changes: every RequestPresentation
	 * and the engine's ResizeBuffers. DLSS-G turns its mode off there. Any thread.
	 */
	void SetPresentationLeaving(std::function<void()> a_leaving);

	/** @brief The presentation the swap chain has now. Any thread. */
	PresentationState Presentation();

	/** @brief The presentation of the adopted frame revision, stable through the frame. Render owner. */
	PresentationState FramePresentation();

	/**
	 * @brief Takes the swap chain's presentation as the frame's. Inside the adoption point of a frame revision that waited for a
	 * replacement (FrameGen::PresentationRevision), so frames see a new presentation only with the revision made for it. Render owner.
	 */
	void AdoptPresentation();

	/**
	 * @brief Takes the latest request (RequestPresentation) as the frame's presentation before the swap chain has it, with
	 * a_hdr10 as its colour space when the output takes HDR10. For a colour space change asked for inside the adoption point:
	 * the submission thread replaces the swap chain after the presents queued so far, so earlier frames present into the old
	 * swap chain and the frames of the adopted revision into the new one. Render owner.
	 */
	void AdoptRequestedPresentation(bool a_hdr10);

	/** @brief Queued-present waits and their total time in microseconds since the last call. Any thread. */
	void TakePresentPace(std::uint64_t& a_waits, std::uint64_t& a_us);

	/**
	 * @brief Ends a frame whose present is held. Waits for the GPU as a present would, so the CPU cannot run ahead and
	 * free per-frame resources such as the upscaler's views while the GPU still reads them. Render thread.
	 */
	void PaceHeldPresent();

	/** @brief The native graphics queue's progress. Empty off native mode. */
	rhi::QueueProgress GraphicsProgress();

	/** @name What DXVK's interop device exports give the profilers in guest mode. */
	/** @{ */
	/** @brief The native Vulkan device as DXVK describes its own, without extension or feature lists. False on D3D12. */
	bool DescribeVulkanDevice(DxvkOrgInteropDeviceInfo& a_info);

	/** @brief The adapter LUID as one 64-bit pattern, or 0. */
	std::uint64_t AdapterLuid();

	/** @brief What vendor SDK commands such as Streamline's evaluate need for one image. On Vulkan, view is destroyed after the commands. */
	struct ExternalImageInfo
	{
		void* native = nullptr;
		std::uint64_t view = 0;
		std::uint64_t state = 0;
		std::uint32_t format = 0;
		std::uint32_t width = 0, height = 0, mips = 1, layers = 1;
		std::uint32_t usage = 0;  // Vulkan: VkImageUsageFlags
	};

	/**
	 * @brief The native command list and per-image info for vendor SDK commands in a pass's Record. Images are in the common state.
	 * The pass must retain a_views until the GPU is past them.
	 */
	bool DescribeForCommands(rhi::CommandList& a_list, std::span<const rhi::Resource> a_images, void*& a_nativeList, std::vector<ExternalImageInfo>& a_infos,
		std::shared_ptr<const void>& a_views, std::string& a_why);

	/** @brief Runs a_callback now on the graphics queue under the queue lock. Matches dxvkEnqueueQueueCallback. */
	HRESULT __stdcall RunOnGraphicsQueue(ID3D11Device* a_device, void (*a_callback)(void*, VkQueue_T*), void* a_user);
	/** @} */

	/** @brief A native texture handle over a port's BasicRHI image, so D3D11 users and graph imports reach the same image. */
	winrt::com_ptr<ID3D11Resource> WrapTexture(const rhi::Resource& a_image, const DxvkOrgInteropTextureWrap& a_wrap, DxvkOrgInteropResourceInfo& a_info,
		const char* a_name, const D3D11_SUBRESOURCE_DATA* a_data);

	/** @brief A native buffer handle over a port's BasicRHI buffer. Null on failure. */
	winrt::com_ptr<ID3D11Buffer> WrapBuffer(const rhi::Resource& a_buffer, const D3D11_BUFFER_DESC& a_desc, const char* a_name);

	/**
	 * @name Unbacked engine targets. A texture handle with no memory, for an engine target native mode never uses.
	 * GPU work that reaches one is a recipe mismatch. It is fatal in development builds and skipped in release.
	 */
	/** @{ */
	winrt::com_ptr<ID3D11Texture2D> MakeUnbackedTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, bool a_dropClears);
	winrt::com_ptr<ID3D11Texture3D> MakeUnbackedTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name);
	/** @brief Whether a_object is an unbacked engine target. Any thread. */
	bool IsUnbacked(const void* a_object);
	/** @} */

	/**
	 * @brief While alive, tracked textures made on this thread become frame transients placed in the arena when the scope ends.
	 * Until then they have no image, so GPU work that reaches them is a recipe mismatch.
	 */
	class FrameTransientScope
	{
	public:
		explicit FrameTransientScope(const char* a_label);
		~FrameTransientScope();
		FrameTransientScope(const FrameTransientScope&) = delete;
		FrameTransientScope& operator=(const FrameTransientScope&) = delete;

	private:
		void* m_group = nullptr;
	};

	/** @brief Records and submits a_record now in its own command list on the graphics queue. a_record runs exactly once on success. */
	bool EmitCommands(void (*a_record)(void*, rhi::CommandList&), void* a_user, const rhi::TimelinePoint* a_waits, std::uint32_t a_waitCount,
		const rhi::TimelinePoint* a_signals, std::uint32_t a_signalCount, const char* a_label);

	/**
	 * @name Texture streaming shells, as DXVK's dxvkStream* exports. A shell is a storage-less texture whose window the streamer sets.
	 * Engine draws do not read shells in native mode, so the streamer decides every window.
	 */
	/** @{ */
	bool CreateStreamShell(const D3D11_TEXTURE2D_DESC& a_desc, ID3D11Texture2D** a_texture, ID3D11ShaderResourceView** a_view);
	HRESULT __stdcall StreamEnableView(ID3D11ShaderResourceView* a_shell, void(__stdcall* a_missing)(std::uint64_t), void(__stdcall* a_gone)(std::uint64_t),
		const D3D11_SUBRESOURCE_DATA*(__stdcall* a_fill)(std::uint64_t, BOOL), std::uint64_t a_user);
	void __stdcall StreamSetWindow(ID3D11ShaderResourceView* a_shell, ID3D11ShaderResourceView* a_window);
	ID3D11ShaderResourceView* __stdcall StreamGetWindow(ID3D11ShaderResourceView* a_shell);
	std::uint64_t __stdcall StreamGetBinds(ID3D11ShaderResourceView* a_shell);
	/** @} */

	/** @brief Waits for the last present and destroys the swap chain and back buffer. Runs at window close. */
	void Shutdown();
}
