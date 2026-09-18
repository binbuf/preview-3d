#pragma once
#include "model_core/ControlProtocol.h"
#include <windows.h>
namespace import_worker {
bool HandleThreeMfImportFileRequest(HANDLE stdOut, const model_core::ParseThreeMfFileRequest& request);
int RunThreeMfImport();
}
