#pragma once

#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>
#include <winrt/base.h>

namespace org
{
	class RenderGraph;
	class Resource;
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
 * - A Resize bumps the resize generation (ResizeGeneration). At their next run the frame anchors see it and build their graphs again,
 *   and PublishedResources publishes the new resource under the twin's id: the owner calls nothing. Passes read Graph() when they
 *   are made (ClientDesc's make), never keep one across builds.
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
	};

	constexpr Export operator|(Export a_left, Export a_right) { return static_cast<Export>(static_cast<std::uint8_t>(a_left) | static_cast<std::uint8_t>(a_right)); }

	/** @brief Whether a_set includes a_view. */
	constexpr bool Has(Export a_set, Export a_view) { return (static_cast<std::uint8_t>(a_set) & static_cast<std::uint8_t>(a_view)) != 0; }

	struct Options
	{
		Lifetime lifetime = Lifetime::Transient;
		Export d3d11 = Export::None;
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

		/** @brief Registers the graph resource by name with a graph being built (ClientDesc::registerResources). */
		void Register(org::RenderGraph& a_graph) const;

		Lifetime GetLifetime() const { return options.lifetime; }

		/** @brief Whether D3D11 views were asked for; an exported resource is never aliasable. */
		bool Exported() const { return options.d3d11 != Export::None; }

		/** @brief Whether the graph may alias the resource: transient and not exported. */
		bool Aliasable() const { return options.lifetime == Lifetime::Transient && !Exported(); }

		/** @brief The exported D3D11 views of the whole resource; null when not exported. */
		ID3D11ShaderResourceView* SRV() const { return srv.get(); }
		ID3D11UnorderedAccessView* UAV() const { return uav.get(); }

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

	protected:
		Resource(std::string a_name, Options a_options) :
			name(std::move(a_name)), options(a_options) {}
		~Resource() { Reset(); }

		/** @brief The views' names, as Buffer.h's detail::SetD3DName gives them. */
		void NameViews() const;

		/** @brief Called by a Resize that made a new resource. */
		static void BumpResizeGeneration();

		std::string name;
		Options options;
		std::shared_ptr<const void> alive = std::make_shared<const int>(0);
		std::shared_ptr<org::Resource> graph;
		// The exported texture or buffer, as DXVK returned it: one COM object, so the derived twins' D3D11() casts it down.
		winrt::com_ptr<ID3D11Resource> d3d11;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
	};

	/**
	 * @brief A graph texture, the twin of Buffer.h's Texture2D.
	 *
	 * The D3D11 description says what the graph texture is: its size, mips, array size, typed format, and views (SRV always, UAV, RTV,
	 * DSV from the bind flags; D3D11_RESOURCE_MISC_TEXTURECUBE for a cube). Usage must be DEFAULT, without CPU access. An exported
	 * texture's D3D11 views are the whole texture in its own format.
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
		 * @brief Recreates the texture at a new size (a resolution change), the export first released. Then call FrameAnchors::Refresh.
		 * @return Whether it was recreated; false, with nothing changed, when the size is the same.
		 */
		bool Resize(UINT a_width, UINT a_height);

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

		/** @brief Texture2D::Resize with a depth. */
		bool Resize(UINT a_width, UINT a_height, UINT a_depth);

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
	 * resized and checked again, which also releases the first one through ReleaseAfterD3D11. Render thread, from FrameAnchors.
	 */
	namespace SelfTest
	{
		/** @brief Whether CS_ORG_GRAPHRES_SELFTEST=1 is set (FrameAnchors reserves DeferredPasses for it). */
		bool Requested();

		/** @brief At an anchor's run with the runtime active: once, makes the test's program and texture and registers its client. */
		void Ensure();

		/** @brief After DeferredPasses' epoch was submitted: copies the texture through its export and checks the last copy. */
		void AfterRun();
	}
}
