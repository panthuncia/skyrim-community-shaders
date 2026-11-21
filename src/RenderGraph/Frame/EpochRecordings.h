#pragma once

#include "RenderGraph/Fatal.h"

#include <OpenRenderGraph/EpochRecordings.h>

#include <cstddef>
#include <string_view>
#include <variant>

/**
* @brief CS's use of ORG's epoch recording requests and cache (OpenRenderGraph/EpochRecordings.h).
* A failed recording stops the game, as before: the cache's failure hook is RenderGraph::Fatal.
*/
namespace RenderGraph::Frame::EpochRecordings
{
	using org::epoch_recordings::AfterDrop;
	using org::epoch_recordings::Builds;
	using org::epoch_recordings::Outcome;
	using org::epoch_recordings::Posting;
	using org::epoch_recordings::Read;
	using org::epoch_recordings::Result;
	using org::epoch_recordings::Stats;

	/** @brief ORG's recording cache, with a failed recording fatal. */
	template <class Key, std::size_t Keep, AfterDrop Rule, class Hash, class Payload = std::monostate>
	class Cache : public org::epoch_recordings::Cache<Key, Keep, Rule, Hash, Payload>
	{
	public:
		Cache() :
			org::epoch_recordings::Cache<Key, Keep, Rule, Hash, Payload>([](std::string_view a_what, std::string_view a_error) { RenderGraph::Fatal(a_what, a_error); })
		{}
	};
}
