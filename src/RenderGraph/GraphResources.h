#pragma once

#include <d3d11.h>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

namespace org
{
	class ExternalTextureResource;
	class RenderGraph;
	class RenderPass;
	class Resource;
	class VersionedTexture;
}

/**
 * @brief Graph-backed twins of Buffer.h's wrappers, for features whose GPU work moves onto the render graph (docs/development/render-graph.md,
 * "Graph-backed feature resources").
 *
 * Each twin owns an ORG resource named as the D3D11 wrapper would name its resource ("Feature::Name"). A feature's anchor client registers
 * it (Register, from ClientDesc::registerResources) and its passes declare Graph() as they declare any graph resource. While a D3D11 reader
 * remains (an engine draw binding it as a PS SRV, a feature not ported yet), the twin also exports D3D11 views of the same memory, named
 * "Feature::Name SRV" / "UAV" / "RTV" as Buffer.h names them.
 *
 * Rules:
 * - A transient resource is aliasable: the graph may place it in memory it shares with others whose lifetimes do not overlap. Its first use
 *   in each frame must be a write, and nothing may read it in a later frame.
 * - An exported resource is never aliasable, whatever its lifetime: D3D11's use of it lies outside the graph, which would otherwise reuse
 *   its memory across that use. A resource becomes aliasable again when its last D3D11 reader is ported and the export is dropped.
 * - An exported texture is a host-owned graph texture whose home state is the layout DXVK rests it in: every epoch gives it back in
 *   that layout, as for DXVK's own images (DxvkImageImport).
 * - Teardown and Resize release the D3D11 views and texture first, then hand the graph resource to RenderGraphRuntime::ReleaseAfterD3D11,
 *   which frees it once the GPU is past every D3D11 command issued until then. Graph builds still holding it keep it alive themselves.
 * - A versioned texture (Options::versioned) keeps one org::VersionedTexture, its slot (Versioned()), for its whole life. Its passes
 *   declare the slot, never Graph(). A Resize of the same shape class (format, mip count, slice count) publishes the new texture as a
 *   new version of the slot: nothing is built again, and the passes keep declaring the same slot. The D3D11 export is made again
 *   with the texture, so a D3D11 reader takes SRV(), UAV() or D3D11() again after a Resize.
 * - Any other Resize (a twin that is not versioned, an aliasable one, or another mip count) bumps the resize generation
 *   (ResizeGeneration). At their next run the frame anchors see it and build their graphs again (RequestStructuralRebuild), and
 *   PublishedResources publishes the new resource under the twin's id: the owner calls nothing. Passes read Graph() when they
 *   are made (ClientDesc's make), never keep one across builds.
 * - A new version may be published only at a frame boundary, because a stage's recording binds the versions current at the
 *   frame's Begin. During a frame a feature calls RequestSize, which Frame::Driver::AtNextBegin applies as a Resize.
 *   Until then the twin keeps its size and Desc(), and that frame takes its sizes from Desc(). A failed resize there stops the game.
 *
 * Everything here runs on the render thread. Constructors never throw: a twin that could not be created is false (logged), and the feature
 * keeps its D3D11 path.
 *
 * Constant buffers have no twin here: a pass's constants are a LatchBlock (org::LatchBlock) or push constants, as in ORGLightCulling.cpp.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
namespace GraphResources
{
	/** @brief How long a resource's contents live (docs/development/org-aliasing-port-priorities.md, "Lifetime classes"). */
	enum class Lifetime : std::uint8_t
	{
		Transient,   // written before any read, every frame, and read only within that frame: aliasable unless exported
		History,     // read in a later frame (ping-pong, temporal accumulation): never aliased
		Persistent,  // written once or on a settings change: never aliased
	};

	/** @brief The D3D11 views a resource exports for readers not ported yet, as bit flags. */
	enum class Export : std::uint8_t
	{
		None = 0,
		SRV = 1 << 0,
		UAV = 1 << 1,
		RTV = 1 << 2,  // textures only
		DSV = 1 << 3,  // 2D depth textures only (see Texture2D)
	};

	constexpr Export operator|(Export a_left, Export a_right) { return static_cast<Export>(static_cast<std::uint8_t>(a_left) | static_cast<std::uint8_t>(a_right)); }

	/** @brief Whether a_set includes a_view. */
	constexpr bool Has(Export a_set, Export a_view) { return (static_cast<std::uint8_t>(a_set) & static_cast<std::uint8_t>(a_view)) != 0; }

	struct Options
	{
		Lifetime lifetime = Lifetime::Transient;
		Export d3d11 = Export::None;
		// A transient's own alias pool (ORG's manual pool id): aliased only with the resources that name the same pool, never with
		// another graph's. Unset: the shared automatic pool. A diagnostic (CS_ORG_SSGI_DEBUG=17).
		std::optional<std::uint64_t> aliasPool;
		// A texture whose passes declare only its slot (Resource::Versioned) or its published id, never Graph(). A Resize of the same
		// shape class is then a new version, not a graph build. Not for a transient: the graph places those at its builds.
		bool versioned = false;
	};

	/**
	 * @brief The one way a resource identity change asks for a structural graph build: a new resource, or a resource of another shape
	 * class (format, mip count, slice count), that no version can stand for. Logged as "[ORG] structural rebuild". a_epochs empty:
	 * every epoch. Render thread. Phase 4 moves these builds off the render thread; this is where that switch goes.
	 */
	void RequestStructuralRebuild(std::string_view a_reason, std::span<const std::uint32_t> a_epochs = {});

	/** @brief How many structural rebuilds RequestStructuralRebuild asked for this session. */
	std::uint64_t StructuralRebuilds();

	/**
	 * @brief Whether a live aliasable twin (Lifetime::Transient, not exported) has this graph resource name: written before any read in the
	 * frame, its memory shared with others otherwise. The recipe's
	 * check finds a present pass that reads one no present pass of its stage writes. Any thread.
	 */
	bool IsTransientName(std::string_view a_name);

	/** @brief Notes a transient twin's name while it lives (Resource's constructor and destructor). */
	void NoteTransientName(const std::string& a_name, bool a_live);

	/**
	 * @brief What ExportTexture makes an image from: the D3D11 texture's shape and format, and the format of each view the graph
	 * makes of it. DXGI_FORMAT_UNKNOWN leaves that view out.
	 *
	 * The texture's format may be typeless when a view gives the typed one, as the engine's depth targets do (R24G8_TYPELESS with a
	 * D24_UNORM_S8_UINT DSV). The image is made in the DSV's format when there is one, else in the texture's own.
	 */
	struct ExportDesc
	{
		D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
		UINT width = 0;
		UINT height = 0;
		UINT depthOrArraySize = 1;  // the depth of a 3D texture, the array size of a 2D one (six per cube)
		UINT mips = 1;              // 0: the whole chain, as in D3D11
		UINT samples = 1;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT bindFlags = 0;  // D3D11_BIND_*
		UINT miscFlags = 0;  // only D3D11_RESOURCE_MISC_TEXTURECUBE
		DXGI_FORMAT srv = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT rtv = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT uav = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT dsv = DXGI_FORMAT_UNKNOWN;
	};

	/** @brief An exported image: the graph's texture and the D3D11 texture DXVK made of the same memory. Both null on failure. */
	struct ExportedTexture
	{
		std::shared_ptr<org::ExternalTextureResource> graph;
		winrt::com_ptr<ID3D11Resource> d3d11;
		std::uint64_t image = 0;  // the VkImage, for readers that hold DXVK's description of it
	};

	/**
	 * @brief The exported-texture path of the twins, for an owner that makes its own D3D11 views (the engine's render targets,
	 * RenderGraph/Engine/Targets). A BasicRHI image the graph sees as a host-owned texture whose home is the layout DXVK rests it in,
	 * wrapped for D3D11 by the DXVK fork (dxvkCreateTextureFromVkImage).
	 *
	 * The caller keeps the image alive while D3D11 may use it: it hands the graph resource to RenderGraphRuntime::ReleaseAfterD3D11
	 * once the D3D11 texture and its views are gone. Render thread, with the runtime active. Logged and empty on any failure.
	 */
	ExportedTexture ExportTexture(const ExportDesc& a_desc, const std::string& a_name);

	/** @brief One subresource's bytes for an upload (Texture2D::Stage, Texture3D::Stage): rows of texels, or of 4x4 blocks. */
	struct SubresourceData
	{
		const void* data = nullptr;
		UINT rowPitch = 0;    // bytes from one row to the next in data
		UINT slicePitch = 0;  // rowPitch times the rows
	};

	/**
	 * @brief A twin texture's contents staged for a copy: a host-visible buffer the CPU filled once and never writes again, and the
	 * subresource copies out of it. Immutable. MakeUploadPass makes the pass that copies it.
	 */
	struct StagedUpload;

	/**
	 * @brief A copy pass for a client's make: each staged upload's buffer declared as a copy source and its texture as a copy
	 * destination, so the graph orders and tracks the write, in guest and native mode alike. It copies on every run it is in, so a
	 * client gives it as a one-shot pass (FrameAnchors::PassDesc::once), or keeps it only until a run with it was submitted. Null
	 * entries are left out.
	 */
	std::shared_ptr<org::RenderPass> MakeUploadPass(std::vector<std::shared_ptr<const StagedUpload>> a_uploads);

	/** @brief What every twin shares: the graph resource, its name and its exported views. */
	class Resource
	{
	public:
		Resource(const Resource&) = delete;
		Resource& operator=(const Resource&) = delete;

		/** @brief The ORG resource's name, "Feature::Name". */
		const std::string& Name() const { return name; }

		/** @brief The graph resource passes declare; null when the twin could not be created. */
		const std::shared_ptr<org::Resource>& Graph() const { return graph; }

		/**
		 * @brief The slot a versioned texture's passes declare (Options::versioned); null for any other twin, and when the twin could
		 * not be created. The same object across every Resize of the same shape class.
		 */
		const std::shared_ptr<org::VersionedTexture>& Versioned() const { return slot; }

		/**
		 * @brief Registers the graph resource by name with a graph being built (ClientDesc::registerResources). A versioned texture
		 * registers its slot as the name's resolver, so a declaration by name follows its versions.
		 */
		void Register(org::RenderGraph& a_graph) const;

		Lifetime GetLifetime() const { return options.lifetime; }

		/** @brief Whether D3D11 views were asked for; an exported resource is never aliasable. */
		bool Exported() const { return options.d3d11 != Export::None; }

		/** @brief Whether the graph may alias the resource: transient and not exported. */
		bool Aliasable() const { return options.lifetime == Lifetime::Transient && !Exported(); }

		/** @brief The exported D3D11 views of the whole resource; null when not exported. */
		ID3D11ShaderResourceView* SRV() const { return srv.get(); }
		ID3D11UnorderedAccessView* UAV() const { return uav.get(); }
		ID3D11DepthStencilView* DSV() const { return dsv.get(); }

		explicit operator bool() const { return graph != nullptr; }

		/** @brief Releases the D3D11 export, then the graph resource once D3D11's last use is done (ReleaseAfterD3D11). */
		void Reset();

		/**
		 * @brief A token that expires when the twin is destroyed: a holder of a pointer to the twin (PublishedResources) checks it
		 * before reading the twin again.
		 */
		std::weak_ptr<const void> Alive() const { return alive; }

		/** @brief Bumped by every Resize of any twin: what the anchors and PublishedResources compare to notice one. */
		static std::uint64_t ResizeGeneration();

		/**
		 * @brief The names of what bumped the resize generation past a_since, joined with commas, for the anchors' log line.
		 * Only the last few bumps are kept. "unknown" when none of them is newer than a_since.
		 */
		static std::string ResizeCauses(std::uint64_t a_since);

	protected:
		Resource(std::string a_name, Options a_options) :
			name(std::move(a_name)), options(a_options)
		{
			if (Aliasable())
				NoteTransientName(name, true);
		}
		~Resource()
		{
			Reset();
			if (Aliasable())
				NoteTransientName(name, false);
		}

		/** @brief The views' names, as Buffer.h's detail::SetD3DName gives them. */
		void NameViews() const;

		/** @brief Called by a Resize that made a new resource. a_who names it in the anchors' log line (ResizeCauses). */
		static void BumpResizeGeneration(std::string_view a_who);

		/**
		 * @brief RequestSize's work. a_size becomes the twin's next size and a_resize runs at the next frame boundary if the twin lives.
		 * With a_current set a_size is the size the twin has, which drops an earlier request.
		 */
		void RequestExtent(const std::array<UINT, 3>& a_size, bool a_current, std::function<void(const std::array<UINT, 3>&)> a_resize);

		/**
		 * @brief After Create: the slot of a versioned texture. With a_previous (the slot before a Resize) and a_sameShape, the new
		 * texture is published as its next version and true is returned: nothing needs a build. Otherwise a new slot is made (or none,
		 * for a twin that is not versioned) and false is returned: the caller bumps the resize generation.
		 */
		bool AdoptSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape);

		/** @brief AdoptSlot's work, without telling the made-image registry (RenderGraph::SetMadeImageSlot) about the slot. */
		bool JoinSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape);

		std::string name;
		Options options;
		std::shared_ptr<const void> alive = std::make_shared<const int>(0);
		std::shared_ptr<org::Resource> graph;
		std::shared_ptr<org::VersionedTexture> slot;  // a versioned texture's (Options::versioned)
		std::optional<std::array<UINT, 3>> requestedSize;  // RequestSize's, until the next frame boundary
		// The exported texture or buffer, as DXVK returned it: one COM object, so the derived twins' D3D11() casts it down.
		winrt::com_ptr<ID3D11Resource> d3d11;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11DepthStencilView> dsv;
	};

	/**
	 * @brief A graph texture, the twin of Buffer.h's Texture2D.
	 *
	 * The D3D11 description says what the graph texture is: its size, mips, array size, typed format, and views (SRV always, UAV, RTV,
	 * DSV from the bind flags; D3D11_RESOURCE_MISC_TEXTURECUBE for a cube). Usage must be DEFAULT, without CPU access. An exported
	 * texture's D3D11 views are the whole texture in its own format.
	 *
	 * A depth texture (D3D11_BIND_DEPTH_STENCIL) is a single 2D texture with one sample, in a typeless format as the engine's depth
	 * targets are: R16, R24G8, R32 or R32G8X24_TYPELESS. The graph image and the DSV take the depth format (D16_UNORM, D24_UNORM_S8_UINT,
	 * D32_FLOAT, D32_FLOAT_S8X24_UINT), the SRV the format that reads the depth (R16_UNORM, R24_UNORM_X8_TYPELESS, R32_FLOAT,
	 * R32_FLOAT_X8X24_TYPELESS). It has no RTV or UAV. Export::DSV exports its depth-stencil view, for D3D11 draws into it.
	 */
	class Texture2D : public Resource
	{
	public:
		Texture2D(const D3D11_TEXTURE2D_DESC& a_desc, std::string a_name, Options a_options = {});

		const D3D11_TEXTURE2D_DESC& Desc() const { return desc; }

		/** @brief The exported D3D11 texture (for CopyResource and the like); null when not exported. */
		ID3D11Texture2D* D3D11() const { return static_cast<ID3D11Texture2D*>(d3d11.get()); }
		ID3D11RenderTargetView* RTV() const { return rtv.get(); }

		/**
		 * @brief Stages the texture's contents for a copy pass (MakeUploadPass), in place of a D3D11 UpdateSubresource or
		 * CopyResource beside the graph.
		 *
		 * One entry per subresource, slice-major (D3D11CalcSubresource's order: slice * mips + mip); an entry without data is left
		 * as it is. a_width and a_height are mip 0's extent the entries hold, written at the top-left corner (each mip's halves);
		 * 0 takes the texture's. The bytes are copied before this returns. Any thread. Null, logged, without a graph or when the
		 * texture or the staging buffer could not be made.
		 */
		std::shared_ptr<const StagedUpload> Stage(std::span<const SubresourceData> a_subresources, UINT a_width = 0, UINT a_height = 0) const;

		/** @brief Stage of zeros into every subresource: texels or blocks of zero bytes, as DXVK's new images hold. */
		std::shared_ptr<const StagedUpload> StageZeros() const;

		/**
		 * @brief Recreates the texture at a new size (a resolution change), the export first released. A versioned texture of the same
		 * mip count becomes the slot's next version: nothing else to call. Otherwise call FrameAnchors::Refresh. Only at a frame
		 * boundary. During a frame use RequestSize.
		 * @return Whether it was recreated; false, with nothing changed, when the size is the same.
		 */
		bool Resize(UINT a_width, UINT a_height);

		/**
		 * @brief The size the texture takes at the next frame boundary through Resize. Until then it keeps its size and Desc().
		 * Its own size drops an earlier request. Render thread, at any point of a frame.
		 */
		void RequestSize(UINT a_width, UINT a_height);

	private:
		void Create();

		D3D11_TEXTURE2D_DESC desc;
	};

	/** @brief A graph 3D texture, the twin of Buffer.h's Texture3D. As Texture2D, without RTVs (the image is not 2D-array compatible). */
	class Texture3D : public Resource
	{
	public:
		Texture3D(const D3D11_TEXTURE3D_DESC& a_desc, std::string a_name, Options a_options = {});

		const D3D11_TEXTURE3D_DESC& Desc() const { return desc; }
		ID3D11Texture3D* D3D11() const { return static_cast<ID3D11Texture3D*>(d3d11.get()); }
		/** @brief Texture2D::Stage for mip 0 only: one entry per depth slice, in order. Mips past the first are left as they are. */
		std::shared_ptr<const StagedUpload> Stage(std::span<const SubresourceData> a_depthSlices) const;

		/** @brief Texture2D::Resize with a depth. */
		bool Resize(UINT a_width, UINT a_height, UINT a_depth);

		/** @brief Texture2D::RequestSize with a depth. */
		void RequestSize(UINT a_width, UINT a_height, UINT a_depth);

	private:
		void Create();

		D3D11_TEXTURE3D_DESC desc;
	};

	/**
	 * @brief A graph structured buffer, the twin of Buffer.h's StructuredBuffer: a_count elements of a_desc.StructureByteStride bytes,
	 * with a UAV when a_desc's bind flags have one. Its exported views are structured views of every element.
	 */
	class StructuredBuffer : public Resource
	{
	public:
		StructuredBuffer(const D3D11_BUFFER_DESC& a_desc, UINT a_count, std::string a_name, Options a_options = {});

		UINT Count() const { return count; }
		ID3D11Buffer* D3D11() const { return static_cast<ID3D11Buffer*>(d3d11.get()); }

	private:
		UINT count = 0;
	};

	/**
	 * @brief A graph buffer of a_desc.ByteWidth bytes, the twin of Buffer.h's Buffer (raw or typed). The graph sees a plain buffer; an
	 * exported one takes D3D11 views as Buffer.h's do (CreateSRV, CreateUAV).
	 */
	class Buffer : public Resource
	{
	public:
		Buffer(const D3D11_BUFFER_DESC& a_desc, std::string a_name, Options a_options = {});

		ID3D11Buffer* D3D11() const { return static_cast<ID3D11Buffer*>(d3d11.get()); }

		/** @brief An exported buffer's D3D11 view; false when not exported or D3D11 refuses the view. */
		bool CreateSRV(const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc);
		bool CreateUAV(const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc);
	};

	/**
	 * @brief The twins' self-test (CS_ORG_GRAPHRES_SELFTEST=1): a compute pass at the DeferredPasses anchor writes an exported graph
	 * texture, D3D11 copies it into a staging texture right after the epoch, and the value read back is logged once. Then the texture is
	 * resized and checked again, which also releases the first one through ReleaseAfterD3D11. Render thread.
	 */
	namespace SelfTest
	{
		/** @brief Whether CS_ORG_GRAPHRES_SELFTEST=1 is set. */
		bool Requested();

		/** @brief Before the render graph initializes: when requested, reserves DeferredPasses. */
		void Reserve();

		/**
		 * @brief At a frame's end with the runtime active (RenderGraphRuntime::SignalFramePace), once: when requested, makes the test's program and texture and registers its client, an
		 * ordinary one. Its executed callback copies the texture through its export after each run and checks the last copy.
		 */
		void Setup();
	}
}
