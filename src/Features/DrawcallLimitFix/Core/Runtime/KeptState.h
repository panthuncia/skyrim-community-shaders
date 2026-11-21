#pragma once

#include <ORGModuleServices/Async/KeptState.h>

// DCLF's persistent draw state is built from OMS's kept structures (org::async, KeptState.h), described in
// dclf-architecture.md, "Persistent draw state". Storage reuse is org::async::keptStorageOptions.reuseStorage.
namespace DCLF
{
	using org::async::ChangeJournal;
	using org::async::EventLog;
	using org::async::KeptArray;
	using org::async::KeptChunkElements;
	using org::async::KeptChunks;
	using org::async::KeptChunkStore;
	using org::async::KeptCopyStats;
	using org::async::KeptElements;
	using org::async::KeptSharing;
	using org::async::KeptView;
	using org::async::KeptWholeArray;
	using org::async::KeptWholeStore;
	using org::async::kKeptChunkBySize;
	using org::async::kKeptChunkBytes;
	using org::async::kKeptMergeGapBytes;
	using org::async::kKeptWhole;
	using org::async::LogCursor;
	using org::async::MarkedList;
	using org::async::SharedEventLog;
}
