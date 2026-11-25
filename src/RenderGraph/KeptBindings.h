#pragma once

#include <cstdint>
#include <memory>
#include <source_location>
#include <vector>

#include <winrt/base.h>

struct ID3D11ShaderResourceView;

namespace RenderGraph
{
	/**
	* @brief D3D11 views' descriptor bindings kept across frames, for a client whose views repeat frame after frame (the HUD's, the
	* overlay's). A kept answer is reused while its import stands: same GpuTextures generation, not superseded. While the undeclared
	* access check is on, every call resolves again, so the check sees each read. Render thread.
	*/
	class KeptBindings
	{
	public:
		/** @brief A view's binding, with one owner for the import and one holding a reference to the view. */
		struct Kept
		{
			std::uint32_t index = 0;
			std::shared_ptr<const void> owner;
			std::shared_ptr<const void> view;
		};

		/**
		* @brief a_view's binding: the kept one, else GpuTextures::ResolveBinding's, kept when it is ready. Null when a_view has no
		* binding now. The pointer is valid until the next call.
		*/
		const Kept* Resolve(ID3D11ShaderResourceView* a_view, std::uint32_t a_sourceTag, std::source_location a_where = std::source_location::current());

		/** @brief Drops what is kept, such as at a device change. */
		void Clear() { entries.clear(); }

	private:
		struct Entry
		{
			winrt::com_ptr<ID3D11ShaderResourceView> key;  // holds the view, so its address names it while kept
			Kept kept;
			std::uint32_t generation = 0;
			std::uint64_t lastFrame = 0;
		};

		std::vector<Entry> entries;
		std::uint64_t prunedFrame = 0;
	};
}
