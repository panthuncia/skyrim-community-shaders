#pragma once


namespace DCLF::Draws
{
	/**
	 * @brief Fixed-stride constant-buffer rows in one device buffer of the scene's set that grows instead of capping.
	 * A row past the sealed capacity waits for the growth requirement. Nothing is dropped and nothing falls back.
	 */
	struct GrowableRows : org::services::GrowableRows
	{
		bool Create(std::uint32_t a_stride, std::uint32_t a_rows, const char* a_name, org::services::GrowthPolicy a_policy = org::services::GrowthPolicy::Doubling(),
			org::services::GrowableContents a_contents = org::services::GrowableContents::Fresh, org::services::GrowableCapture a_capture = {});
	};
}
