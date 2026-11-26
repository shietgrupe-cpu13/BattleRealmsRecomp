#pragma once

#include "lift_dispatch.h"

#include <string>
#include <vector>

namespace br::runtime {

struct Win32FileOpenResult {
	int fd = -1;
	uint32_t lastError = 0;
};

// Source game documents are read-only; guest writes go to an isolated overlay.
using Win32FileOpener = Win32FileOpenResult (*)(
	const char* gameRelativePath, uint32_t desiredAccess, uint32_t creationDisposition);
using Win32FileAttributesQuery = uint32_t (*)(const char* gameRelativePath);
using Win32FileDelete = uint32_t (*)(const char* gameRelativePath);
// Creates only in the writable guest overlay and returns a Win32 error code.
// A zero result means the directory was newly created.
using Win32FileDirectoryCreator = uint32_t (*)(
	const char* templateRelativePath, const char* newRelativePath);
struct Win32FileFindEntry {
	std::string name;
	uint32_t attributes = 0;
	uint64_t size = 0;
	uint64_t modifiedUnixMillis = 0;
};
using Win32FileFindQuery = bool (*)(const char* gameRelativeDirectory,
	const char* wildcard, std::vector<Win32FileFindEntry>& entries);
void configureWin32FileOpener(Win32FileOpener opener);
void configureWin32FileAttributesQuery(Win32FileAttributesQuery query);
void configureWin32FileDelete(Win32FileDelete deleter);
void configureWin32FileDirectoryCreator(Win32FileDirectoryCreator creator);
void configureWin32FileFindQuery(Win32FileFindQuery query);
bool closeWin32FileHandle(uint32_t handle);
bool registerWin32FileHandlers(ImportDispatchRegistry& handlers);

} // namespace br::runtime
