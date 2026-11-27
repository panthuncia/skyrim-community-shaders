#pragma once

// Raw engine access shared by native's passes: engine globals by Address Library id, fields by offset, call-site checks, and the
// log for a clear or copy the skeleton did not take.

#include "RenderGraph/Fatal.h"
#include "Utils/Logging.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace RenderGraph::Engine::Native::Access
{
	/** @brief The engine global at Address Library id a_se (SE) or a_ae (AE). */
	template <class T>
	T& Global(std::uint64_t a_se, std::uint64_t a_ae)
	{
		return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
	}

	/** @brief The field of type T at a_offset bytes into a_object. */
	template <class T>
	T& Field(void* a_object, std::size_t a_offset)
	{
		return *reinterpret_cast<T*>(static_cast<std::byte*>(a_object) + a_offset);
	}

	/** @brief Fatal under a_area unless a_site is a CALL rel32 to a_target. a_what names the call in the message. */
	inline void RequireCall(const char* a_what, std::uintptr_t a_site, std::uintptr_t a_target, std::string_view a_area)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
		std::int32_t relative = 0;
		std::memcpy(&relative, bytes + 1, sizeof(relative));
		if (bytes[0] != 0xE8 || a_site + 5 + relative != a_target)
			RenderGraph::Fatal(a_area, fmt::format("{} at {:#x} is not the engine's call; another plugin may have patched it", a_what, a_site));
	}

	/**
	* @brief The clears and copies a pass hands the skeleton. One the skeleton did not take is not made, since nothing else makes it in
	* native: it is logged once per what, under the pass's own line.
	*/
	class UntakenLog
	{
	public:
		explicit UntakenLog(const char* a_line) :
			log(a_line) {}

		/** @brief Logs a_what the first time a_taken is false for it. */
		void operator()(bool a_taken, const char* a_what)
		{
			if (!a_taken)
				log.Report(a_what);
		}

	private:
		Util::ReasonLog log;
	};
}
