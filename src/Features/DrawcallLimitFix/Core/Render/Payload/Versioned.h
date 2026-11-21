#pragma once

// Features include this header directly, so it must stay self-contained.

#include <ORGModuleServices/GrowableBufferSet.h>
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <ORGModuleServices/GrowthPolicy.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/Buffers/VersionedBuffer.h>

#include <cstdint>
#include <memory>

namespace DCLF::Draws
{
	/**
	 * @brief A buffer that grows by publishing new versions. Old versions stay unchanged for work recorded against them, so nothing waits.
	 * Builds ahead write to the version their sealed set names. Commits write to the adopted set's version.
	 */
	using Versioned = std::shared_ptr<org::VersionedBuffer>;
	/**
	 * @brief The only source of a versioned buffer's address, SRV index and capacity.
	 * By convention `sealed` is the producer's set and `adopted` is the set the render thread installed with its snapshot.
	 */
	using NamedVersions = org::services::NamedVersions;

	struct GrowableSpec
	{
		org::services::BufferExtent extent;
		org::services::GrowthPolicy policy = org::services::GrowthPolicy::Doubling();
		org::services::GrowableContents contents = org::services::GrowableContents::Fresh;
		org::services::GrowableCapture capture;  // Capture's
	};

	/** @brief Any thread. Creates the first version and registers it with the scene's set. */
	Versioned MakeVersioned(std::shared_ptr<org::Buffer> a_buffer, GrowableSpec a_spec);

	/** @brief Units in the version a_sealed names, or 0 when the set predates the buffer. */
	inline std::uint64_t NamedUnits(const NamedVersions& a_sealed, const Versioned& a_buffer) { return a_sealed.UnitsOf(a_buffer); }

	inline const std::shared_ptr<org::Buffer>& Target(const std::shared_ptr<org::Buffer>& a_buffer) { return a_buffer; }
	inline std::shared_ptr<org::Buffer> Target(const Versioned& a_buffer) { return a_buffer ? a_buffer->Get() : nullptr; }
}
