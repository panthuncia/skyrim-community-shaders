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
* @brief Graph-backed twins of Buffer.h's wrappers, for features whose GPU work moved onto the render graph.
*
* Each twin owns an ORG resource named "Feature::Name". Passes declare Graph(), or Versioned() for a versioned texture.
* While a D3D11 reader remains, the twin also exports D3D11 views of the same memory, and an exported resource is never aliased.
* A transient's first use each frame must be a write, and nothing may read it in a later frame.
* Teardown and Resize release the D3D11 side first, then free the graph resource after D3D11's last use.
* A same-shape Resize of a versioned texture is a new version. Any other Resize triggers a graph rebuild by itself.
* New versions are published only at a frame boundary, so during a frame use RequestSize. Everything runs on the render thread.
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
		// A transient's own alias pool, aliased only with resources naming the same pool. Unset means the shared pool. A diagnostic.
		std::optional<std::uint64_t> aliasPool;
		// Passes declare only the slot or published id, so a same-shape Resize is a new version, not a graph build. Not for transients.
		bool versioned = false;
		// A 2D texture that shaders read and write as an array, so it has array views even with one slice.
		bool array = false;
	};

	/** @brief The one way to ask for a structural graph build after a resource identity change no version covers. Render thread.
	*  An empty a_epochs means every epoch. */
	void RequestStructuralRebuild(std::string_view a_reason, std::span<const std::uint32_t> a_epochs = {});

	/** @brief How many structural rebuilds RequestStructuralRebuild asked for this session. */
	std::uint64_t StructuralRebuilds();

	/** @brief Whether a live aliasable twin has this graph resource name. Any thread. */
	bool IsTransientName(std::string_view a_name);

	/** @brief Notes a transient twin's name while it lives (Resource's constructor and destructor). */
	void NoteTransientName(const std::string& a_name, bool a_live);

	/** @brief What ExportTexture makes an image from. DXGI_FORMAT_UNKNOWN leaves a view out.
	*  A typeless texture takes its DSV's format, as the engine's R24G8_TYPELESS depth targets need. */
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
		bool array = false;  // array views even with one slice (Options::array)
	};

	/** @brief An exported image: the graph's texture and the D3D11 texture DXVK made of the same memory. Both null on failure. */
	struct ExportedTexture
	{
		std::shared_ptr<org::ExternalTextureResource> graph;
		winrt::com_ptr<ID3D11Resource> d3d11;
		std::uint64_t image = 0;  // the VkImage, for readers that hold DXVK's description of it
	};

	/** @brief Exports a graph texture for an owner that makes its own D3D11 views, such as engine targets. Render thread. Empty on failure.
	*  The caller keeps the image alive while D3D11 may use it, then hands it to ReleaseAfterD3D11. */
	ExportedTexture ExportTexture(const ExportDesc& a_desc, const std::string& a_name);

	/** @brief One subresource's bytes for an upload (Texture2D::Stage, Texture3D::Stage): rows of texels, or of 4x4 blocks. */
	struct SubresourceData
	{
		const void* data = nullptr;
		UINT rowPitch = 0;    // bytes from one row to the next in data
		UINT slicePitch = 0;  // rowPitch times the rows
	};

	/** @brief A twin texture's contents in a host-visible buffer, written once by the CPU. Immutable. */
	struct StagedUpload;

	/** @brief A copy pass for staged uploads. It copies on every run it is in, so give it as a one-shot pass. Null entries are skipped. */
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

		/** @brief The slot a versioned texture's passes declare, the same across same-shape Resizes. Null otherwise. */
		const std::shared_ptr<org::VersionedTexture>& Versioned() const { return slot; }

		/** @brief Registers the graph resource by name with a graph being built. A versioned texture registers its slot as the resolver. */
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

		/** @brief A token that expires when the twin is destroyed, for holders of a pointer to it. */
		std::weak_ptr<const void> Alive() const { return alive; }

		/** @brief Bumped by every Resize of any twin: what the anchors and PublishedResources compare to notice one. */
		static std::uint64_t ResizeGeneration();

		/** @brief What bumped the resize generation past a_since, for the log. Only the last few are kept. */
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

		/** @brief RequestSize's work. a_resize runs at the next frame boundary if the twin lives. a_current drops an earlier request. */
		void RequestExtent(const std::array<UINT, 3>& a_size, bool a_current, std::function<void(const std::array<UINT, 3>&)> a_resize);

		/** @brief After Create, sets up a versioned texture's slot. Returns true when the new texture became a_previous's next version.
		*  False means the caller must bump the resize generation. */
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
	* @brief A graph texture, the twin of Buffer.h's Texture2D. The D3D11 description defines it. Usage must be DEFAULT.
	* A depth texture uses a typeless format such as R24G8_TYPELESS. The image and DSV take the depth format, the SRV the read format.
	*/
	class Texture2D : public Resource
	{
	public:
		Texture2D(const D3D11_TEXTURE2D_DESC& a_desc, std::string a_name, Options a_options = {});

		const D3D11_TEXTURE2D_DESC& Desc() const { return desc; }

		/** @brief The exported D3D11 texture (for CopyResource and the like); null when not exported. */
		ID3D11Texture2D* D3D11() const { return static_cast<ID3D11Texture2D*>(d3d11.get()); }
		ID3D11RenderTargetView* RTV() const { return rtv.get(); }

		/** @brief Stages contents for MakeUploadPass, one entry per subresource in D3D11CalcSubresource order. Empty entries are skipped.
		*  a_width and a_height default to the texture's mip 0 size. Bytes are copied before return. Any thread. Null on failure. */
		std::shared_ptr<const StagedUpload> Stage(std::span<const SubresourceData> a_subresources, UINT a_width = 0, UINT a_height = 0) const;

		/** @brief Stage of zeros into every subresource: texels or blocks of zero bytes, as DXVK's new images hold. */
		std::shared_ptr<const StagedUpload> StageZeros() const;

		/** @brief Recreates the texture at a new size. Same-shape versioned textures become a new version, else call FrameAnchors::Refresh.
		*  Only at a frame boundary. Returns false when the size is unchanged. */
		bool Resize(UINT a_width, UINT a_height);

		/** @brief The size the texture takes at the next frame boundary. Render thread, at any point of a frame. */
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

	/** @brief A graph structured buffer, the twin of Buffer.h's StructuredBuffer. Exported views cover every element. */
	class StructuredBuffer : public Resource
	{
	public:
		StructuredBuffer(const D3D11_BUFFER_DESC& a_desc, UINT a_count, std::string a_name, Options a_options = {});

		UINT Count() const { return count; }
		ID3D11Buffer* D3D11() const { return static_cast<ID3D11Buffer*>(d3d11.get()); }

	private:
		UINT count = 0;
	};

	/** @brief A graph buffer, the twin of Buffer.h's raw or typed Buffer. An exported one takes D3D11 views through CreateSRV and CreateUAV. */
	class Buffer : public Resource
	{
	public:
		Buffer(const D3D11_BUFFER_DESC& a_desc, std::string a_name, Options a_options = {});

		ID3D11Buffer* D3D11() const { return static_cast<ID3D11Buffer*>(d3d11.get()); }

		/** @brief An exported buffer's D3D11 view; false when not exported or D3D11 refuses the view. */
		bool CreateSRV(const D3D11_SHADER_RESOURCE_VIEW_DESC& a_desc);
		bool CreateUAV(const D3D11_UNORDERED_ACCESS_VIEW_DESC& a_desc);
	};

	/** @brief The twins' self-test, CS_ORG_GRAPHRES_SELFTEST=1. Writes, reads back, resizes and checks an exported texture. */
	namespace SelfTest
	{
		/** @brief Whether CS_ORG_GRAPHRES_SELFTEST=1 is set. */
		bool Requested();

		/** @brief Before the render graph initializes: when requested, reserves DeferredPasses. */
		void Reserve();

		/** @brief Once at a frame's end, makes the test's program and texture and registers its client. */
		void Setup();
	}
}
