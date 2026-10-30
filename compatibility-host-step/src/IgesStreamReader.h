#pragma once

// T08 in-memory IGES model loader.
//
// OCCT's IGES work library only implements ReadFile: `IFSelect_WorkLibrary::
// ReadStream` has no IGES override, so `IGESControl_Reader::ReadStream` always
// fails even for valid bytes. The pinned C IGES record parser is stream-agnostic
// once handed lines, so this adapter feeds it the already-admitted 80-column
// records from the host's read-only mapping and builds the same IGES model that
// `IGESFile_Read` would. No path, temporary file, or second parser is added.

#include <IGESData_IGESModel.hxx>
#include <IGESData_Protocol.hxx>

#include <istream>

namespace step_host {

// Builds an IGES model from a complete 80-column IGES record stream. Returns a
// null handle on a malformed record stream or parser failure; the caller maps
// that to the product's typed MalformedData result.
Handle(IGESData_IGESModel) ReadIgesModelFromStream(
    std::istream& stream, const Handle(IGESData_Protocol)& protocol);

} // namespace step_host
