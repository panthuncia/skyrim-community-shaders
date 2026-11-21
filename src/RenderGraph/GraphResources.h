#pragma once

#include <d3d11.h>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <source_location>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <winrt/base.h>

#include "RenderGraph/Frame/Birth.h"

namespace org
{
	class ExternalTextureResource;
	class GloballyIndexedResource;
	class RenderGraph;
	class RenderPass;
	class Resource;
	class VersionedBuffer;
	class VersionedTexture;
	struct BufferVersion;
}

namespace RenderGraph
{
	class RunGate;
}

namespace RenderGraph::Engine::Native
{
	enum class WrapFlags : std::uint32_t;
}

/**
* @brief Graph-backed twins of Buffer.h's wrappers, for features whose GPU work moved onto the render graph.
*
* Each twin owns an ORG resource named "Feature::Name". Passes declare Graph(), or Versioned() for a versioned texture.
* While a D3D11 reader remains, the twin also exports D3D11 views of the same memory, and an exported resource is never aliased.
* A transient's first use each frame must be a write, and nothing may read it in a later frame.
* Teardown releases the D3D11 side first, then frees the graph resource after D3D11's last use.
*
* Revisions (docs/development/plans/frame-revision-plan.md):
* - A twin keeps its size for life unless it is declared (Frame/Declare.h). There is no resize call.
* - A declared texture's new extent is a new version of the same slot, in the same shape class (format, mips, slices). The
*   revision lane makes it off the render thread (PlanPrepare, then Prepare from the plan), and the revision's adoption at a
*   frame's Begin makes it current (the slot's Adopt, then AdoptPrepared). An extent no version can serve is fatal.
* - Passes that follow a declared texture declare its slot, never Graph(), so a new version needs no build.
* - Nothing here publishes a version inside a frame.
* Everything runs on the render thread, except Prepare and WhyNotPreparable (any thread). Prepare from a plan never touches the twin.
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
		DSV = 1 << 3,  // 2D depth textures and depth arrays only (see Texture2D)
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
		// Passes declare only the slot or published id, so a new extent is a new version, not a graph build. Not for transients.
		bool versioned = false;
		// A 2D texture that shaders read and write as an array, so it has array views even with one slice.
		bool array = false;
		// What each of a declared texture's versions holds before its first writer (Frame::DeclareTexture2D). Unset: a History
		// texture is born zero, others undefined. Only versioned textures are born on the lane.
		RenderGraph::Frame::Birth birth;
		// A versioned 2D texture's first version joins this slot instead of a new one, as kFRAMEBUFFER joins the slot native made
		// with the swap chain. Its shape class must be the slot's, or it is fatal. The constructor takes it, so the twin keeps none.
		std::shared_ptr<org::VersionedTexture> joinSlot;
	};

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
		std::uint64_t image = 0;   // the VkImage, for readers that hold DXVK's description of it
		std::uint32_t layout = 0;  // the VkImageLayout D3D11 rests it in, which every image of its shape class rests in too
	};

	/** @brief Exports a graph texture for an owner that makes its own D3D11 views. Render thread. Empty on failure. Registers it in
	*  ImageRegistry as made. The caller keeps the image alive while D3D11 may use it, then retires it in ImageRegistry and hands it
	*  to ReleaseAfterD3D11. Kept for its last callers until they move to ExportForEngine. */
	ExportedTexture ExportTexture(const ExportDesc& a_desc, const std::string& a_name);

	/** @brief An ORG image with its D3D11 export, made by ExportForEngine and retired by Retire. */
	struct EngineExport
	{
		std::shared_ptr<org::ExternalTextureResource> graph;
		winrt::com_ptr<ID3D11Resource> d3d11;  // named a_name for RenderDoc
		std::uint64_t image = 0;               // the VkImage (ID3D12Resource on D3D12), its key in ImageRegistry
	};

	/**
	* @brief An ORG image with its D3D11 export, registered in ImageRegistry as made, with no slot (ImageRegistry::SetSlot adds one).
	* Any thread (the revision lane). Guest: the wrap holds the graph texture (Tripwire::AttachGraphResource), so DXVK's zero fill
	* never outlives the image. Native with WrapFlags::DeferInitialize: the graph texture starts Undefined, so its first writer
	* discards. Throws std::runtime_error naming a_name on any failure; never empty.
	*/
	EngineExport ExportForEngine(const ExportDesc& a_desc, std::string a_name, RenderGraph::Engine::Native::WrapFlags a_flags);

	/** @brief The one way to retire an EngineExport: forgets it in ImageRegistry, releases the D3D11 side, then the image after D3D11's last use. Render thread. */
	void Retire(EngineExport&& a_export);

	/** @brief One subresource's bytes for an upload (Texture2D::Stage, Texture3D::Stage): rows of texels, or of 4x4 blocks. */
	struct SubresourceData
	{
		const void* data = nullptr;
		UINT rowPitch = 0;    // bytes from one row to the next in data
		UINT slicePitch = 0;  // rowPitch times the rows
	};

	/** @brief A twin texture's contents in a host-visible buffer, written once by the CPU. Immutable. */
	struct StagedUpload;

	/** @brief A gate entry an upload waits for. */
	struct UploadGate
	{
		std::shared_ptr<const ::RenderGraph::RunGate> gate;
		std::uint32_t entry = 0;
	};

	/**
	* @brief A copy pass for staged uploads. Null entries are skipped. Ungated it copies on every run it is in.
	* With a_gate it copies only on runs whose commit opened the entry (a first-run gate, RunGate.h), so give it as atExecution.
	*/
	std::shared_ptr<org::RenderPass> MakeUploadPass(std::vector<std::shared_ptr<const StagedUpload>> a_uploads, UploadGate a_gate = {});

	/** @brief A texture's width, height and depth (1 for a 2D texture). */
	using Extent = std::array<UINT, 3>;

	/** @brief What a new version holds before its first write. */
	enum class Contents : std::uint8_t
	{
		Undefined,  // whatever the memory held: written before any read, or zeroed by its D3D11 export
		Zero,       // zero-filled before the version is ready (a history is born cleared)
		Fill,       // written by PreparedVersion::fill before the version is ready (Birth::Value, Birth::Fill)
	};

	/** @brief Writes one subresource of a Contents::Fill version. The same signature as org::TextureFillWriter. Any thread. */
	using TexelWriter = std::function<bool(std::uint32_t a_mip, std::uint32_t a_slice, std::uint32_t a_width, std::uint32_t a_rows, std::uint32_t a_depth,
		std::uint32_t a_rowPitch, std::byte* a_destination)>;

	/** @brief What adoption needs of an exported twin's prepared version. Opaque outside GraphResources.cpp. */
	struct PreparedExport;

	/** @brief What Prepare reads of a twin, copied by Resource::PlanPrepare. Opaque outside GraphResources.cpp. */
	struct PreparePlan;

	/**
	* @brief A versioned twin's next version, made by Resource::Prepare and not yet adopted.
	* Make the slot's pending version from resource (VersionedTexture::MakeImage), fill it as contents says, adopt it at a
	* frame boundary, then call Resource::AdoptPrepared with this.
	*/
	struct PreparedVersion
	{
		std::shared_ptr<org::VersionedTexture> slot;             // the slot it becomes a version of
		std::shared_ptr<org::GloballyIndexedResource> resource;  // the version's graph resource, its memory made
		Contents contents = Contents::Undefined;
		TexelWriter fill;  // Contents::Fill only
		Extent extent{};
		const void* twin = nullptr;                      // the Resource it was prepared for
		std::shared_ptr<const PreparedExport> exported;  // null for a graph-only twin
		explicit operator bool() const { return resource != nullptr; }
	};

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

		/** @brief The slot a versioned texture's passes declare, the same across its versions. Null otherwise. */
		const std::shared_ptr<org::VersionedTexture>& Versioned() const { return slot; }

		/** @brief The slot a versioned structured buffer's passes declare, the same across element counts. Null otherwise. */
		const std::shared_ptr<org::VersionedBuffer>& BufferVersions() const { return bufferSlot; }

		/** @brief Registers the graph resource by name with a graph being built. A versioned texture or buffer registers its slot as the resolver. */
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

		/** @brief The texture's current extent; zeros for a buffer. Render thread. */
		virtual Extent CurrentExtent() const { return {}; }

		/** @brief Why Prepare makes no version at a_next, or nothing when it makes one. Prepare needs a versioned texture, live,
		*  whose shape class holds there (format, mip count, slice count). Any thread. */
		std::optional<std::string> WhyNotPreparable(const Extent& a_next) const;

		/**
		* @brief Why the lane cannot make Options::birth, or nothing when it can. A birth that writes needs a versioned, single-sampled
		* colour texture; Value and Fill need a format with a texel encoding (Frame::EncodeTexel), and a guest export needs DXVK's
		* wrap with data. Render thread.
		*/
		std::optional<std::string> WhyUnborn() const;

		/** @brief The birth each version gets: Options::birth, or the lifetime's when it is unset. Render thread. */
		RenderGraph::Frame::Birth BirthOf() const;

		/**
		* @brief Makes the next version at a_next without changing the twin: its graph resource with memory, and what adoption needs.
		* Any thread. An exported twin's D3D11 wrap and views are made here too (native: WrapFlags::DeferInitialize, no GPU work), so
		* adoption only moves pointers. Empty when WhyNotPreparable names a cause. Throws, naming the twin, when memory or a resource
		* cannot be made. First contents follow the birth (BirthOf): graph-only versions and native colour exports are filled by the
		* uploader (Contents::Zero or Fill; a native colour export is zeroed even when Undefined), guest exports by DXVK's wrap (zeros,
		* or the birth's data), and depth or multisampled versions are born Undefined.
		*/
		PreparedVersion Prepare(const Extent& a_next) const;

		/**
		* @brief Render thread: what Prepare needs to make the version at a_next, copied from the twin. Null when WhyNotPreparable names a cause.
		* Prepare(plan) then never touches the twin, so a twin destroyed meanwhile is no hazard.
		*/
		std::shared_ptr<const PreparePlan> PlanPrepare(const Extent& a_next) const;

		/** @brief Prepare from a plan, on any thread, without the twin. Throws as Prepare does. */
		static PreparedVersion Prepare(const PreparePlan& a_plan);

		/**
		* @brief Render thread, at a frame boundary, after the slot adopted a_prepared's version. Takes the D3D11 export and views Prepare
		* made (exported twins), updates Desc(), Graph() and ImageRegistry, and releases the old D3D11 side after D3D11's last use.
		* Adopts no version itself. A version prepared for another twin or another slot is fatal.
		*/
		void AdoptPrepared(const PreparedVersion& a_prepared);

	protected:
		Resource(std::string a_name, Options a_options) :
			name(std::move(a_name)), options(a_options)
		{
			if (Aliasable())
				NoteTransientName(name, true);
		}
		virtual ~Resource()
		{
			Reset();
			if (Aliasable())
				NoteTransientName(name, false);
		}

		/** @brief The texture's D3D11 shape at a_extent, view formats left unknown. False for a buffer, or when a_extent changes
		*  the mip count. Called with the register lock held. */
		virtual bool ShapeAt(const Extent& a_extent, ExportDesc& a_out) const;

		/** @brief Sets the description's size to an adopted version's. */
		virtual void SetExtent(const Extent&) {}

		/** @brief WhyNotPreparable's cause, or null. Fills a_shape when it returns null. Called with the register lock held. */
		const char* NotPreparableCause(const Extent& a_next, ExportDesc& a_shape) const;

		/** @brief The views' names, as Buffer.h's detail::SetD3DName gives them. */
		void NameViews() const;

		/** @brief After Create, sets up a versioned texture's slot. Returns true when the new texture became a_previous's next version.
		*  False means a new slot, which readers declare only after a build. */
		bool AdoptSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape);

		/** @brief AdoptSlot's work, without telling the image registry (RenderGraph::ImageRegistry::SetSlot) about the slot. */
		bool JoinSlot(std::shared_ptr<org::VersionedTexture> a_previous, bool a_sameShape);

		std::string name;
		Options options;
		std::shared_ptr<const void> alive = std::make_shared<const int>(0);
		std::shared_ptr<org::Resource> graph;
		std::shared_ptr<org::VersionedTexture> slot;  // a versioned texture's (Options::versioned)
		std::shared_ptr<org::VersionedBuffer> bufferSlot;  // a versioned structured buffer's (Options::versioned)
		std::uint32_t exportLayout = 0;               // an exported texture's resting VkImageLayout, the same for every version
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
	* A depth texture may have up to eight slices; with more than one its SRV and DSV are array views of every slice.
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

		Extent CurrentExtent() const override;

	protected:
		bool ShapeAt(const Extent& a_extent, ExportDesc& a_out) const override;
		void SetExtent(const Extent& a_extent) override;

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

		Extent CurrentExtent() const override;

	protected:
		bool ShapeAt(const Extent& a_extent, ExportDesc& a_out) const override;
		void SetExtent(const Extent& a_extent) override;

	private:
		void Create();

		D3D11_TEXTURE3D_DESC desc;
	};

	/**
	* @brief A graph structured buffer, the twin of Buffer.h's StructuredBuffer. Exported views cover every element.
	* A versioned one (Options::versioned, not a transient) has a slot, BufferVersions(), that passes declare. A new element
	* count is then a new version of the slot (Frame::DeclareBuffer), made off the render thread and adopted at a frame boundary.
	*/
	class StructuredBuffer : public Resource
	{
	public:
		StructuredBuffer(const D3D11_BUFFER_DESC& a_desc, UINT a_count, std::string a_name, Options a_options = {});

		/** @brief The element count of the current version. Render thread. */
		UINT Count() const { return count; }
		UINT Stride() const { return stride; }
		ID3D11Buffer* D3D11() const { return static_cast<ID3D11Buffer*>(d3d11.get()); }

		/**
		* @brief Render thread, at a frame boundary, after BufferVersions() adopted a_version, a version of a_count elements. Makes
		* the D3D11 export and views (exported buffers), updates Graph() and Count(), and releases the old D3D11 side after
		* D3D11's last use. Fatal when a_version is not the slot's current version, or D3D11 refuses it.
		*/
		void AdoptVersion(const std::shared_ptr<const org::BufferVersion>& a_version, UINT a_count);

	private:
		/** @brief The D3D11 buffer and views of a_buffer, a_count elements. False when D3D11 refuses one. */
		bool Export(org::Resource& a_buffer, UINT a_count, winrt::com_ptr<ID3D11Resource>& a_d3d11, winrt::com_ptr<ID3D11ShaderResourceView>& a_srv,
			winrt::com_ptr<ID3D11UnorderedAccessView>& a_uav) const;

		UINT count = 0;
		UINT stride = 0;
		UINT bindFlags = 0;
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
}
