#pragma once

// A commit's uploads on the render thread, either staged or latched into the epoch's latch block.
// Every write states which descriptors its bytes name, so they stay alive while the bytes are current.
// The upload classes are OMS's (ORGModuleServices/CommitUploads.h). DCLF's give them its fatal handler and its byte counts
// (FrameData), and keep the constructors and names its commits use.
// Features include this header directly, so it must stay self-contained.

#include "Features/DrawcallLimitFix/Core/Gpu/FrameData.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Gpu/OwnedRows.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/ContentOwners.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "RenderGraph/Fatal.h"

#include <ORGModuleServices/CommitUploads.h>
#include <Render/LatchBlock.h>
#include <Render/Runtime/IUploadService.h>
#include <Render/Runtime/StagedUploadBatch.h>
#include <Render/Runtime/UploadServiceAccess.h>
#include <Resources/Buffers/Buffer.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace DCLF::Draws
{
	/** @brief A readable name for the part of the frame-constants buffer at a_offset. */
	std::string FrameConstantsPart(std::uint64_t a_offset);

	/** @brief DCLF's hooks for the upload classes: RenderGraph::Fatal for their errors, FrameData for their bytes. */
	const org::services::CommitUploadHooks& CommitHooks();
	/** @brief Reuses a released batch from a_pool, or adds a new one, under DCLF's zone. */
	std::shared_ptr<org::runtime::StagedUploadBatch> AcquireCommitBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);

	// A commit's own uploads, staged directly and submitted when the commit ends. Each is one memcpy and one copy.
	// A versioned buffer is written in the version the adopted set names, the one the epoch's recordings resolve.
	class CommitUploads : public org::services::CommitUploads
	{
	public:
		CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool, const NamedVersions& a_adopted, ContentOwners* a_content = nullptr) :
			org::services::CommitUploads(AcquireCommitBatch(a_pool), a_adopted, CommitHooks(), a_content)
		{}
	};

	/**
	 * @brief A commit's per-frame values, written into its shape's latched copies and copied to their targets by the epoch's first pass.
	 * A value with no copy is staged by the fallback instead.
	 */
	class LatchedUploads : public org::services::LatchedUploads
	{
	public:
		using org::services::LatchedUploads::LatchedUploads;
	};
}
