#pragma once

#include "RenderGraph/Switches.h"

#include <cstdint>
#include <memory>
#include <source_location>
#include <string_view>

struct ID3D11Buffer;
struct ID3D11ShaderResourceView;

namespace org
{
	class Resource;
}

/**
* @brief The undeclared-access validator, CS_ORG_VALIDATE_UNDECLARED=1. Logs every raw bindless index or buffer ref CS hands a shader
* for a resource the graph should track. It only logs, never refuses.
*
* A raw index bypasses the declarations, so the graph neither transitions the resource nor orders it after its writers.
* Immutable resources, host feedback buffers and IMMUTABLE D3D11 resources are exempt. Lookups inside a DeclaredIndexScope or
* DeclaredViewScope count apart as declared interim indices.
* Each access is attributed to the epoch running on the thread, else to capture, else to another thread.
* Logs the first access per site and resource, then a summary every CS_ORG_CENSUS_FRAMES frames.
*/
namespace RenderGraph::UndeclaredAccess
{
	/** @brief Whether CS_ORG_VALIDATE_UNDECLARED=1. Any thread. */
	inline bool On()
	{
		static const bool on = Switches::On("CS_ORG_VALIDATE_UNDECLARED");
		return on;
	}

	/** @brief A raw index of a_view's resource, made by a_what at a_where. Any thread. */
	void NoteView(ID3D11ShaderResourceView* a_view, std::string_view a_what, std::source_location a_where);

	/** @brief A raw ref of a D3D11 buffer, made at a_where. Any thread. */
	void NoteBuffer(ID3D11Buffer* a_buffer, std::string_view a_what, std::source_location a_where);

	/** @brief A raw index or ref of an ORG resource, made at a_where. ExternalImmutableShaderResource is exempt. Any thread. */
	void NoteResource(const org::Resource& a_resource, std::string_view a_what, std::source_location a_where);

	/** @brief A raw index of the published twin a_id, at a_where. Any thread. */
	void NotePublished(std::string_view a_id, std::source_location a_where);

	/** @brief A raw ref of a BasicRHI buffer by handle, at a_where. Any thread. */
	void NoteRhiBuffer(std::uint32_t a_index, std::uint32_t a_generation, std::source_location a_where);

	/** @brief Notes a_resource as immutable after creation, made with its contents and only read. Its creator calls it while On(). */
	void NoteImmutable(const std::shared_ptr<org::Resource>& a_resource);

	/** @brief Notes a_resource as a host feedback buffer, written by shaders unordered and read only by the CPU, so nothing needs ordering.
	*  Only for buffers made for that use. Its creator calls it while On(). */
	void NoteHostFeedback(const std::shared_ptr<org::Resource>& a_resource);

	/** @brief For the scope, ShaderResourceIndex lookups on this thread are reads the reader declared. One thread-local flag. */
	class DeclaredIndexScope
	{
	public:
		DeclaredIndexScope();
		~DeclaredIndexScope();
		DeclaredIndexScope(const DeclaredIndexScope&) = delete;
		DeclaredIndexScope& operator=(const DeclaredIndexScope&) = delete;

	private:
		bool previous = false;
	};

	/** @brief For the scope, raw views noted on this thread are fallbacks for reads the caller's pass declares.
	*  Only for callers that declared the read themselves. */
	class DeclaredViewScope
	{
	public:
		DeclaredViewScope();
		~DeclaredViewScope();
		DeclaredViewScope(const DeclaredViewScope&) = delete;
		DeclaredViewScope& operator=(const DeclaredViewScope&) = delete;

	private:
		bool previous = false;
	};

	/** @brief Attributes accesses on this thread to epoch a_label for the scope. */
	class EpochScope
	{
	public:
		explicit EpochScope(const char* a_label);
		~EpochScope();
		EpochScope(const EpochScope&) = delete;
		EpochScope& operator=(const EpochScope&) = delete;

	private:
		const char* previous = nullptr;
	};

	/** @brief The frame's end. Reports a window when due. */
	void EndFrame();
}
