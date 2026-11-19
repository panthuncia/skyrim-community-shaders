#pragma once

// Core's side of Api/VertexStreams.h: the stream's entry, resolved once at install, and what its select and upload get.
// Without a stream, every draw's second stream repeats its first.

#include "Features/DrawcallLimitFix/Api/VertexStreams.h"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace org
{
	class VersionedBuffer;
	namespace services
	{
		class NamedVersions;
	}
}

namespace DCLF::Draws
{
	struct SceneBuffers;
	class CommitUploads;
}

namespace DCLF::Api
{
	/** @brief What the stream's select gets on the render thread. It answers with buffer. */
	class VertexStreamSelect
	{
	public:
		const std::shared_ptr<void>& state;
		const std::shared_ptr<org::VersionedBuffer>* buffer = nullptr;  // null means none

		template <class T>
		const T& State() const
		{
			return *static_cast<const T*>(state.get());
		}
	};

	/** @brief What the stream's upload gets in a commit: the adopted set, the payload's rows, the buffer, the state and the staged uploads. */
	class VertexStreamUpload
	{
	public:
		const org::services::NamedVersions& adopted;
		std::span<const VertexStreamRow> rows;
		const std::shared_ptr<org::VersionedBuffer>& buffer;
		const std::shared_ptr<void>& state;
		Draws::CommitUploads& uploads;

		template <class T>
		T& State() const
		{
			return *static_cast<T*>(state.get());
		}
	};
}

namespace DCLF::Extensions
{
	/** @brief The stream's entry, with its name owned. */
	struct VertexStreamEntry
	{
		bool registered = false;
		std::string name;
		Api::VertexStreamContribution contribution;
	};

	/** @brief Main thread, at the end of InstallFeatures. */
	void ResolveVertexStream(const Api::VertexStreamContribution& a_registered);
	/** @brief Any thread, after InstallFeatures. */
	const VertexStreamEntry& VertexStream();
	/** @brief Any thread, after InstallFeatures. Whether the stream's objects are DCLF's. Builds embed the buffer's address only then. */
	bool VertexStreamTaken();
	/** @brief The scene resources call order: the stream's resources first, then the rest in registration order. */
	std::span<const std::uint32_t> SceneResourcesOrder();
	/** @brief Whether a_resources belongs to the stream. */
	bool VertexStreamResources(std::uint32_t a_resources);

	/** @brief Render thread, after scene resources setup. The stream's buffer, or null. */
	std::shared_ptr<org::VersionedBuffer> SelectVertexStream(const std::vector<std::shared_ptr<void>>& a_states);
	/** @brief Render thread, first in a commit. Uploads a_rows into the adopted version. Returns the rows sent, or 0 without a stream. */
	std::uint32_t UploadVertexStream(const org::services::NamedVersions& a_adopted, std::span<const Api::VertexStreamRow> a_rows, const Draws::SceneBuffers& a_scene,
		Draws::CommitUploads& a_uploads);
}
