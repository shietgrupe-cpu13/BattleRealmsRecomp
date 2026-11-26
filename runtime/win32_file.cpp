#include "win32_file.h"

#include "guest.h"
#include "ra3_guest_bridge.h"
#include "win32_base.h"
#include "android_diagnostics.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define BR_READ _read
#define BR_WRITE _write
#define BR_CLOSE _close
#define BR_SEEK _lseeki64
#define BR_FSTAT _fstat64
#define BR_SYNC _commit
#define BR_TRUNCATE _chsize_s
using br_stat = struct _stat64;
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define BR_READ ::read
#define BR_WRITE ::write
#define BR_CLOSE ::close
#define BR_SEEK ::lseek
#define BR_FSTAT ::fstat
#define BR_SYNC ::fsync
#define BR_TRUNCATE ::ftruncate
using br_stat = struct stat;
#endif

namespace br::runtime {
namespace {
constexpr uint32_t kGenericRead = 0x80000000u;
constexpr uint32_t kGenericWrite = 0x40000000u;
constexpr uint32_t kSynchronize = 0x00100000u;
constexpr uint32_t kCreateNew = 1u;
constexpr uint32_t kCreateAlways = 2u;
constexpr uint32_t kOpenExisting = 3u;
constexpr uint32_t kOpenAlways = 4u;
constexpr uint32_t kTruncateExisting = 5u;
constexpr uint32_t kFileFlagOverlapped = 0x40000000u;
constexpr uint32_t kFileTypeDisk = 1u;
constexpr uint32_t kInvalidFileAttributes = 0xffffffffu;
constexpr uint32_t kInvalidHandle = 6u;
constexpr uint32_t kFileNotFound = 2u;
constexpr uint32_t kAccessDenied = 5u;
constexpr uint32_t kPathNotFound = 3u;
constexpr uint32_t kAlreadyExists = 183u;
constexpr uint32_t kInvalidParameter = 87u;
constexpr uint32_t kInvalidFunction = 1u;
constexpr uint32_t kNegativeSeek = 131u;
constexpr uint32_t kHandleBase = 0x6f000000u;
constexpr uint32_t kHandleLimit = 0x6ffffffcu;
constexpr uint32_t kFindHandleBase = 0x6d000000u;
constexpr uint32_t kFindHandleLimit = 0x6dfffffcu;
constexpr uint32_t kMaxPath = 4096u;
constexpr uint32_t kFileNotFoundError = 2u;
constexpr uint32_t kNoMoreFilesError = 18u;

std::mutex fileMutex;
std::mutex currentDirectoryMutex;
std::string currentDirectory;
struct FileHandle {
	int fd = -1;
	bool readable = false;
	bool writable = false;
};
std::map<uint32_t, FileHandle> fileHandles;
uint32_t nextFileHandle = kHandleBase;
Win32FileOpener fileOpener = nullptr;
Win32FileAttributesQuery fileAttributesQuery = nullptr;
Win32FileDelete fileDelete = nullptr;
Win32FileDirectoryCreator fileDirectoryCreator = nullptr;
Win32FileFindQuery fileFindQuery = nullptr;
std::map<std::string, uint32_t> guestAttributeOverrides;
struct FindHandle {
	std::vector<Win32FileFindEntry> entries;
	size_t next = 0;
};
std::map<uint32_t, FindHandle> findHandles;
uint32_t nextFindHandle = kFindHandleBase;

[[maybe_unused]] uint32_t virtualLogicalDrives()
{
	Win32FileOpener opener = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		opener = fileOpener;
	}
	if (!opener) return 0u;
	const Win32FileOpenResult executable = opener("Battle_Realms_F.exe", kGenericRead, kOpenExisting);
	if (executable.fd < 0) return 0u;
	(void)BR_CLOSE(executable.fd);
	// The selected Android game tree is mounted as one virtual fixed volume.
	return 1u << 2;
}

uint32_t argument(CPU* cpu, uint32_t index)
{
	return guest::load<uint32_t>(cpu->r[ESP] + 4u + index * 4u);
}

void traceGuestFileCallStack(const CPU* cpu, uint32_t importReturn,
	const std::string& path)
{
	if (!cpu || !std::getenv("BR_TRACE_FILE_CALLSTACK")) return;
	std::string lowered = path;
	for (char& ch : lowered)
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	if (lowered.find("front_end") == std::string::npos &&
		lowered.find("main_menu") == std::string::npos &&
		lowered.find("liquid_splash") == std::string::npos &&
		lowered.find("br_logo") == std::string::npos) return;

	std::fprintf(stderr, "[br-file-calls] guest-path='%s' import-return=%08x guest-ebp=%08x\n",
		path.c_str(), importReturn, cpu->r[EBP]);
	uint32_t frame = cpu->r[EBP];
	for (uint32_t depth = 0; depth < 12u; ++depth) {
		if (!br::runtime::sharedGuestAddressSpace().host(frame, 8u)) break;
		const uint32_t previous = guest::load<uint32_t>(frame);
		const uint32_t returnAddress = guest::load<uint32_t>(frame + 4u);
		std::fprintf(stderr, "[br-file-calls]   frame[%u]=%08x return=%08x prev=%08x\n",
			depth, frame, returnAddress, previous);
		if (previous <= frame || previous - frame > 0x00100000u) break;
		frame = previous;
	}
}

bool normalizeGuestPath(const std::string& input, std::string& output);

bool readGuestString(uint32_t address, std::string& value)
{
	if (!address) return false;
	value.clear();
	value.reserve(128);
	for (uint32_t i = 0; i < kMaxPath; ++i) {
		const auto* byte = guest::host(address + i);
		if (!byte) return false;
		if (!*byte) return !value.empty();
		value.push_back(static_cast<char>(*byte));
	}
	return false;
}

bool readGuestStringAllowEmpty(uint32_t address, std::string& value)
{
	if (!address) return false;
	value.clear();
	for (uint32_t i = 0; i < kMaxPath; ++i) {
		const auto* byte = guest::host(address + i);
		if (!byte) return false;
		if (!*byte) return true;
		value.push_back(static_cast<char>(*byte));
	}
	return false;
}

bool wildcardMatch(const std::string& pattern, const std::string& name)
{
	size_t p = 0, n = 0, star = std::string::npos, retry = 0;
	auto fold = [](unsigned char ch) { return static_cast<unsigned char>(std::tolower(ch)); };
	while (n < name.size()) {
		if (p < pattern.size() && (pattern[p] == '?' || fold(static_cast<unsigned char>(pattern[p])) == fold(static_cast<unsigned char>(name[n])))) {
			++p; ++n;
		} else if (p < pattern.size() && pattern[p] == '*') {
			star = p++; retry = n;
		} else if (star != std::string::npos) {
			p = star + 1u; n = ++retry;
		} else return false;
	}
	while (p < pattern.size() && pattern[p] == '*') ++p;
	return p == pattern.size();
}

void writeFindData(uint32_t output, const Win32FileFindEntry& entry)
{
	constexpr uint32_t kFindDataSize = 320u;
	auto* destination = br::runtime::sharedGuestAddressSpace().host(output, kFindDataSize);
	if (!output || !destination) {
		setWin32LastError(998u); // ERROR_NOACCESS
		return;
	}
	std::memset(destination, 0, kFindDataSize);
	guest::store<uint32_t>(output, entry.attributes);
	const uint64_t fileTime = entry.modifiedUnixMillis > 11644473600000ull
		? (entry.modifiedUnixMillis - 11644473600000ull) * 10000ull : 0ull;
	guest::store<uint32_t>(output + 20u, static_cast<uint32_t>(fileTime));
	guest::store<uint32_t>(output + 24u, static_cast<uint32_t>(fileTime >> 32u));
	guest::store<uint32_t>(output + 28u, static_cast<uint32_t>(entry.size >> 32u));
	guest::store<uint32_t>(output + 32u, static_cast<uint32_t>(entry.size));
	const size_t count = std::min<size_t>(entry.name.size(), 259u);
	for (size_t i = 0; i < count; ++i)
		guest::store<uint8_t>(output + 44u + static_cast<uint32_t>(i), static_cast<uint8_t>(entry.name[i]));
}

void findFirstFileA(CPU* cpu, const ApiImportStub&)
{
	std::string rawPath, relativePath;
	const uint32_t output = argument(cpu, 1);
	if (!readGuestString(argument(cpu, 0), rawPath) || !normalizeGuestPath(rawPath, relativePath) ||
		!output || !br::runtime::sharedGuestAddressSpace().host(output, 320u)) {
		cpu->r[EAX] = 0xffffffffu;
		setWin32LastError(kInvalidParameter);
		return;
	}
	const size_t separator = relativePath.find_last_of('/');
	const std::string directory = separator == std::string::npos ? "." : relativePath.substr(0, separator);
	const std::string pattern = separator == std::string::npos ? relativePath : relativePath.substr(separator + 1u);
	Win32FileFindQuery query = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		query = fileFindQuery;
	}
	std::vector<Win32FileFindEntry> entries;
	if (!query || !query(directory.c_str(), pattern.c_str(), entries)) {
		cpu->r[EAX] = 0xffffffffu;
		setWin32LastError(kAccessDenied);
		return;
	}
	entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const auto& entry) {
		return !wildcardMatch(pattern, entry.name);
	}), entries.end());
	if (entries.empty()) {
		cpu->r[EAX] = 0xffffffffu;
		setWin32LastError(kFileNotFoundError);
		return;
	}
	const Win32FileFindEntry first = entries.front();
	uint32_t handle = 0;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		handle = nextFindHandle;
		nextFindHandle = nextFindHandle >= kFindHandleLimit ? kFindHandleBase : nextFindHandle + 4u;
		findHandles[handle] = FindHandle{std::move(entries), 1u};
	}
	writeFindData(output, first);
	cpu->r[EAX] = handle;
}

void findNextFileA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	std::lock_guard<std::mutex> lock(fileMutex);
	auto found = findHandles.find(handle);
	if (found == findHandles.end() || !output || !br::runtime::sharedGuestAddressSpace().host(output, 320u) ||
		found->second.next >= found->second.entries.size()) {
		cpu->r[EAX] = 0;
		setWin32LastError(found == findHandles.end() ? kInvalidHandle : kNoMoreFilesError);
		return;
	}
	writeFindData(output, found->second.entries[found->second.next++]);
	cpu->r[EAX] = 1u;
}

void findClose(CPU* cpu, const ApiImportStub&)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	const size_t erased = findHandles.erase(argument(cpu, 0));
	cpu->r[EAX] = erased ? 1u : 0u;
	if (!erased) setWin32LastError(kInvalidHandle);
}

void getLogicalDrives(CPU* cpu, const ApiImportStub&)
{
#if defined(_WIN32) && !defined(BR_FORCE_VIRTUAL_DRIVE_ENUMERATION)
	const uint32_t drives = static_cast<uint32_t>(::GetLogicalDrives());
	if (!drives) setWin32LastError(static_cast<uint32_t>(::GetLastError()));
#else
	// Expose the virtual C: volume only when its selected SAF root can really
	// open the original game executable through the configured read bridge.
	const uint32_t drives = virtualLogicalDrives();
#endif
	cpu->r[EAX] = drives;
	if (std::getenv("BR_TRACE_FILE"))
		std::fprintf(stderr, "[br-file] GetLogicalDrives mask=%08x\n", drives);
}

void getLogicalDriveStringsA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t capacity = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (capacity && (!output || !sharedGuestAddressSpace().host(output, capacity))) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS.
		return;
	}
#if defined(_WIN32) && !defined(BR_FORCE_VIRTUAL_DRIVE_ENUMERATION)
	char* buffer = capacity ? reinterpret_cast<char*>(sharedGuestAddressSpace().host(output, capacity)) : nullptr;
	const DWORD result = ::GetLogicalDriveStringsA(capacity, buffer);
	if (!result) setWin32LastError(static_cast<uint32_t>(::GetLastError()));
	cpu->r[EAX] = result;
#else
	char drives[5]{};
	const uint32_t mask = virtualLogicalDrives();
	uint32_t requiredBuffer = 0u;
	if (mask & (1u << 2)) {
		drives[0] = 'C'; drives[1] = ':'; drives[2] = '\\';
		requiredBuffer = 5u; // C:\\0 plus the MULTI_SZ terminator.
	}
	if (!requiredBuffer) {
		cpu->r[EAX] = 0u;
		return;
	}
	if (!capacity) {
		cpu->r[EAX] = requiredBuffer;
		return;
	}
	if (capacity < requiredBuffer) {
		cpu->r[EAX] = requiredBuffer;
		return;
	}
	for (uint32_t i = 0; i < requiredBuffer; ++i)
		guest::store<uint8_t>(output + i, static_cast<uint8_t>(drives[i]));
	cpu->r[EAX] = requiredBuffer - 1u;
#endif
	if (std::getenv("BR_TRACE_FILE"))
		std::fprintf(stderr, "[br-file] GetLogicalDriveStringsA capacity=%u result=%u\n",
			capacity, cpu->r[EAX]);
}

void getDriveTypeA(CPU* cpu, const ApiImportStub&)
{
	std::string root;
	if (!readGuestString(argument(cpu, 0), root)) {
		cpu->r[EAX] = 1u; // DRIVE_NO_ROOT_DIR.
		return;
	}
#if defined(_WIN32) && !defined(BR_FORCE_VIRTUAL_DRIVE_ENUMERATION)
	cpu->r[EAX] = static_cast<uint32_t>(::GetDriveTypeA(root.c_str()));
#else
	const bool virtualC = root.size() >= 2u &&
		std::tolower(static_cast<unsigned char>(root[0])) == 'c' && root[1] == ':';
	cpu->r[EAX] = virtualC && (virtualLogicalDrives() & (1u << 2)) ? 3u : 1u;
	// DRIVE_FIXED when the SAF-backed C: volume is reachable, otherwise NO_ROOT_DIR.
#endif
	if (std::getenv("BR_TRACE_FILE"))
		std::fprintf(stderr, "[br-file] GetDriveTypeA root='%s' type=%u\n", root.c_str(), cpu->r[EAX]);
}

bool normalizeGuestPath(const std::string& input, std::string& output)
{
	std::string path = input;
	std::replace(path.begin(), path.end(), '\\', '/');
	bool absolute = !path.empty() && path[0] == '/';
	if (path.size() >= 2 && path[1] == ':') {
		path.erase(0, 2);
		absolute = true;
	}
	std::vector<std::string> components;
	for (size_t begin = 0; begin < path.size();) {
		while (begin < path.size() && path[begin] == '/') ++begin;
		const size_t end = path.find('/', begin);
		const size_t stop = end == std::string::npos ? path.size() : end;
		if (stop > begin) {
			const std::string component = path.substr(begin, stop - begin);
			if (component == "..") return false;
			if (component != ".") components.push_back(component);
		}
		if (end == std::string::npos) break;
		begin = end + 1;
	}
	// Windows paths embedded in old game configuration files can include the
	// installation directory. The selected SAF folder is already that root.
	for (size_t i = components.size(); i > 0; --i) {
		std::string lower = components[i - 1];
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if (lower == "battle realms complete") {
			components.erase(components.begin(), components.begin() + static_cast<std::ptrdiff_t>(i));
			break;
		}
	}
	if (components.empty()) {
		if (absolute) {
			output = ".";
			return true;
		}
		std::lock_guard<std::mutex> lock(currentDirectoryMutex);
		output = currentDirectory.empty() ? "." : currentDirectory;
		return true;
	}
	if (!absolute) {
		std::lock_guard<std::mutex> lock(currentDirectoryMutex);
		if (!currentDirectory.empty()) {
			std::vector<std::string> prefix;
			for (size_t begin = 0; begin < currentDirectory.size();) {
				const size_t end = currentDirectory.find('/', begin);
				const size_t stop = end == std::string::npos ? currentDirectory.size() : end;
				if (stop > begin) prefix.push_back(currentDirectory.substr(begin, stop - begin));
				if (end == std::string::npos) break;
				begin = end + 1;
			}
			prefix.insert(prefix.end(), components.begin(), components.end());
			components.swap(prefix);
		}
	}
	output.clear();
	for (const auto& component : components) {
		if (!output.empty()) output.push_back('/');
		output += component;
	}
	return true;
}

FileHandle findFileHandle(uint32_t handle)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	const auto found = fileHandles.find(handle);
	return found == fileHandles.end() ? FileHandle{} : found->second;
}

void createFileA(CPU* cpu, const ApiImportStub&)
{
	std::string rawPath;
	std::string relativePath;
	if (!readGuestString(argument(cpu, 0), rawPath) || !normalizeGuestPath(rawPath, relativePath)) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	const uint32_t access = argument(cpu, 1);
	const uint32_t creation = argument(cpu, 4);
	const uint32_t flags = argument(cpu, 5);
	const uint32_t caller = guest::load<uint32_t>(cpu->r[ESP]);
	traceGuestFileCallStack(cpu, caller, relativePath);
	if (std::getenv("BR_TRACE_GUEST_FILES"))
		std::fprintf(stderr, "[br-file-api] CreateFileA caller=%08x path='%s' access=%08x disposition=%u\n",
			caller, rawPath.c_str(), access, creation);
	const uint32_t unsupportedAccess = access & ~(kGenericRead | kGenericWrite | kSynchronize);
	const bool readable = (access & kGenericRead) != 0;
	const bool writable = (access & kGenericWrite) != 0;
	const bool validDisposition = creation >= kCreateNew && creation <= kTruncateExisting;
	if (unsupportedAccess || (!readable && !writable) || !validDisposition ||
		((creation == kCreateNew || creation == kCreateAlways || creation == kTruncateExisting) && !writable) ||
		(flags & kFileFlagOverlapped)) {
		setWin32LastError(unsupportedAccess ? kAccessDenied : kInvalidParameter);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	Win32FileOpener opener = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		opener = fileOpener;
	}
	if (!opener) {
		setWin32LastError(kFileNotFound);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	const Win32FileOpenResult opened = opener(relativePath.c_str(), access, creation);
	if (opened.fd < 0) {
		if (std::getenv("BR_TRACE_GUEST_FILES"))
			std::fprintf(stderr, "[br-file-api] CreateFileA caller=%08x path='%s' failed error=%u\n",
				caller, rawPath.c_str(), opened.lastError ? opened.lastError : kFileNotFound);
		setWin32LastError(opened.lastError ? opened.lastError : kFileNotFound);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	std::lock_guard<std::mutex> lock(fileMutex);
	if (nextFileHandle > kHandleLimit) {
		(void)BR_CLOSE(opened.fd);
		setWin32LastError(8u);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	const uint32_t handle = nextFileHandle;
	nextFileHandle += 4u;
	fileHandles.emplace(handle, FileHandle{opened.fd, readable, writable});
	cpu->r[EAX] = handle;
	if (opened.lastError) setWin32LastError(opened.lastError);
}

void readFile(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	const uint32_t bufferAddress = argument(cpu, 1);
	const uint32_t requested = argument(cpu, 2);
	const uint32_t bytesReadAddress = argument(cpu, 3);
	if (argument(cpu, 4) != 0 || !bytesReadAddress) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	const FileHandle file = findFileHandle(handle);
	uint8_t* buffer = guest::host(bufferAddress);
	if (file.fd < 0) {
		setWin32LastError(kInvalidHandle);
		cpu->r[EAX] = 0;
		return;
	}
	if (!file.readable) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0;
		return;
	}
	if (!buffer && requested) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	const auto count = BR_READ(file.fd, buffer, requested);
	if (count < 0) {
		setWin32LastError(errno == EINTR ? 6u : kInvalidFunction);
		cpu->r[EAX] = 0;
		return;
	}
	guest::store<uint32_t>(bytesReadAddress, static_cast<uint32_t>(count));
	cpu->r[EAX] = 1;
}

void setFilePointer(CPU* cpu, const ApiImportStub&)
{
	const FileHandle file = findFileHandle(argument(cpu, 0));
	if (file.fd < 0) {
		setWin32LastError(kInvalidHandle);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	if (!file.readable && !file.writable) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	const uint32_t low = argument(cpu, 1);
	const uint32_t highAddress = argument(cpu, 2);
	const uint32_t method = argument(cpu, 3);
	int origin = 0;
	if (method == 1u) origin = 1;
	else if (method == 2u) origin = 2;
	else if (method != 0u) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	int64_t distance = static_cast<int32_t>(low);
	if (highAddress) {
		const uint64_t combined = (uint64_t(guest::load<uint32_t>(highAddress)) << 32) | low;
		distance = static_cast<int64_t>(combined);
	}
	const int64_t position = BR_SEEK(file.fd, distance, origin);
	if (position < 0) {
		setWin32LastError(errno == EINVAL ? kNegativeSeek : kInvalidFunction);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	if (highAddress) guest::store<uint32_t>(highAddress, static_cast<uint32_t>(uint64_t(position) >> 32));
	cpu->r[EAX] = static_cast<uint32_t>(position);
}

bool fileSize(int fd, uint64_t& size)
{
	br_stat status{};
	if (BR_FSTAT(fd, &status) != 0 || status.st_size < 0) return false;
	size = static_cast<uint64_t>(status.st_size);
	return true;
}

void getFileSize(CPU* cpu, const ApiImportStub&)
{
	const FileHandle file = findFileHandle(argument(cpu, 0));
	uint64_t size = 0;
	if (file.fd < 0 || !fileSize(file.fd, size)) {
		setWin32LastError(file.fd < 0 ? kInvalidHandle : kInvalidFunction);
		cpu->r[EAX] = 0xffffffffu;
		return;
	}
	const uint32_t highAddress = argument(cpu, 1);
	if (highAddress) guest::store<uint32_t>(highAddress, static_cast<uint32_t>(size >> 32));
	cpu->r[EAX] = static_cast<uint32_t>(size);
}

void getFileType(CPU* cpu, const ApiImportStub&)
{
	const FileHandle file = findFileHandle(argument(cpu, 0));
	if (file.fd < 0) {
		setWin32LastError(kInvalidHandle);
		cpu->r[EAX] = 0;
		return;
	}
	if (!file.readable && !file.writable) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0;
		return;
	}
	cpu->r[EAX] = kFileTypeDisk;
}

void writeFile(CPU* cpu, const ApiImportStub&)
{
	const FileHandle file = findFileHandle(argument(cpu, 0));
	const uint32_t bufferAddress = argument(cpu, 1);
	const uint32_t requested = argument(cpu, 2);
	const uint32_t bytesWrittenAddress = argument(cpu, 3);
	if (argument(cpu, 4) != 0 || !bytesWrittenAddress) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	if (file.fd < 0) {
		setWin32LastError(kInvalidHandle);
		cpu->r[EAX] = 0;
		return;
	}
	if (!file.writable) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0;
		return;
	}
	const uint8_t* buffer = guest::host(bufferAddress);
	if (!buffer && requested) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	const auto count = BR_WRITE(file.fd, buffer, requested);
	if (count < 0) {
		setWin32LastError(kInvalidFunction);
		cpu->r[EAX] = 0;
		return;
	}
	guest::store<uint32_t>(bytesWrittenAddress, static_cast<uint32_t>(count));
	cpu->r[EAX] = 1;
}

void flushFileBuffers(CPU* cpu, const ApiImportStub&)
{
	const FileHandle file = findFileHandle(argument(cpu, 0));
	if (file.fd < 0) {
		setWin32LastError(kInvalidHandle);
		cpu->r[EAX] = 0;
		return;
	}
	if (!file.writable || BR_SYNC(file.fd) != 0) {
		setWin32LastError(file.writable ? kInvalidFunction : kAccessDenied);
		cpu->r[EAX] = 0;
		return;
	}
	cpu->r[EAX] = 1;
}

void setEndOfFile(CPU* cpu, const ApiImportStub&)
{
	const FileHandle file = findFileHandle(argument(cpu, 0));
	if (file.fd < 0) {
		setWin32LastError(kInvalidHandle);
		cpu->r[EAX] = 0;
		return;
	}
	if (!file.writable) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0;
		return;
	}
	const int64_t position = BR_SEEK(file.fd, 0, 1);
	if (position < 0 || BR_TRUNCATE(file.fd, position) != 0) {
		setWin32LastError(kInvalidFunction);
		cpu->r[EAX] = 0;
		return;
	}
	cpu->r[EAX] = 1;
}

void getFileAttributesA(CPU* cpu, const ApiImportStub&)
{
	std::string rawPath;
	std::string relativePath;
	if (!readGuestString(argument(cpu, 0), rawPath) || !normalizeGuestPath(rawPath, relativePath)) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = kInvalidFileAttributes;
		return;
	}
	Win32FileAttributesQuery query = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		query = fileAttributesQuery;
	}
	if (!query) {
		setWin32LastError(kFileNotFound);
		cpu->r[EAX] = kInvalidFileAttributes;
		return;
	}
	uint32_t attributes = query(relativePath.c_str());
	if (attributes != kInvalidFileAttributes) {
		std::lock_guard<std::mutex> lock(fileMutex);
		const auto override = guestAttributeOverrides.find(relativePath);
		if (override != guestAttributeOverrides.end()) attributes = override->second;
	}
	if (attributes == kInvalidFileAttributes) setWin32LastError(kFileNotFound);
	cpu->r[EAX] = attributes;
}

void createDirectoryExA(CPU* cpu, const ApiImportStub&)
{
	std::string rawTemplate, templatePath, rawTarget, targetPath;
	if (!readGuestString(argument(cpu, 0), rawTemplate) ||
		!normalizeGuestPath(rawTemplate, templatePath) ||
		!readGuestString(argument(cpu, 1), rawTarget) ||
		!normalizeGuestPath(rawTarget, targetPath)) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0u;
		return;
	}
#if defined(__ANDROID__)
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"CreateDirectoryExA template='%s' target='%s' security=%08x guest-caller=%08x",
		rawTemplate.c_str(), rawTarget.c_str(), argument(cpu, 2), guest::load<uint32_t>(cpu->r[ESP]));
#endif

	// SECURITY_ATTRIBUTES is the Win32 12-byte x86 layout. Validate the guest
	// pointer and nLength before accepting it; Android currently uses the
	// platform default ACL because its private app directory owns the overlay.
	const uint32_t securityAttributes = argument(cpu, 2);
	if (securityAttributes) {
		const auto* bytes = br::runtime::sharedGuestAddressSpace().host(securityAttributes, 12u);
		if (!bytes) {
			setWin32LastError(998u); // ERROR_NOACCESS
			cpu->r[EAX] = 0u;
			return;
		}
		if (guest::load<uint32_t>(securityAttributes) != 12u) {
			setWin32LastError(kInvalidParameter);
			cpu->r[EAX] = 0u;
			return;
		}
	}

	Win32FileAttributesQuery attributesQuery = nullptr;
	Win32FileDirectoryCreator creator = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		attributesQuery = fileAttributesQuery;
		creator = fileDirectoryCreator;
	}
	const uint32_t templateAttributes = attributesQuery
		? attributesQuery(templatePath.c_str()) : kInvalidFileAttributes;
	if (templateAttributes == kInvalidFileAttributes) {
		setWin32LastError(kFileNotFound);
		cpu->r[EAX] = 0u;
		return;
	}
	if (!(templateAttributes & 0x10u)) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0u;
		return;
	}
	if (!creator) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0u;
		return;
	}

	const uint32_t error = creator(templatePath.c_str(), targetPath.c_str());
	if (error) {
#if defined(__ANDROID__)
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_WARN, "BattleRealmsNative",
			"CreateDirectoryExA failed target='%s' error=%u", rawTarget.c_str(), error);
#endif
		setWin32LastError(error);
		cpu->r[EAX] = 0u;
		return;
	}
	cpu->r[EAX] = 1u;
#if defined(__ANDROID__)
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"CreateDirectoryExA created overlay directory target='%s'", rawTarget.c_str());
#endif
}

void setFileAttributesA(CPU* cpu, const ApiImportStub&)
{
	std::string rawPath, relativePath;
	if (!readGuestString(argument(cpu, 0), rawPath) || !normalizeGuestPath(rawPath, relativePath)) {
		setWin32LastError(kInvalidParameter); cpu->r[EAX] = 0; return;
	}
	Win32FileAttributesQuery query = nullptr;
	{ std::lock_guard<std::mutex> lock(fileMutex); query = fileAttributesQuery; }
	if (!query || query(relativePath.c_str()) == kInvalidFileAttributes) {
		setWin32LastError(kFileNotFound); cpu->r[EAX] = 0; return;
	}
	{ std::lock_guard<std::mutex> lock(fileMutex); guestAttributeOverrides[relativePath] = argument(cpu, 1); }
	cpu->r[EAX] = 1;
}

void deleteFileA(CPU* cpu, const ApiImportStub&)
{
	std::string rawPath, relativePath;
	if (!readGuestString(argument(cpu, 0), rawPath) || !normalizeGuestPath(rawPath, relativePath)) {
		setWin32LastError(kInvalidParameter); cpu->r[EAX] = 0; return;
	}
	Win32FileDelete deleter;
	{ std::lock_guard<std::mutex> lock(fileMutex); deleter = fileDelete; }
	const uint32_t error = deleter ? deleter(relativePath.c_str()) : kAccessDenied;
	if (error) { setWin32LastError(error); cpu->r[EAX] = 0; }
	else cpu->r[EAX] = 1;
}

void setCurrentDirectoryA(CPU* cpu, const ApiImportStub&)
{
	std::string rawPath;
	std::string relativePath;
	if (!readGuestString(argument(cpu, 0), rawPath) || !normalizeGuestPath(rawPath, relativePath)) {
		setWin32LastError(kInvalidParameter);
		cpu->r[EAX] = 0;
		return;
	}
	Win32FileAttributesQuery query = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		query = fileAttributesQuery;
	}
	const uint32_t attributes = query ? query(relativePath.c_str()) : kInvalidFileAttributes;
	if (attributes == kInvalidFileAttributes) {
		setWin32LastError(kFileNotFound);
		cpu->r[EAX] = 0;
		return;
	}
	if (!(attributes & 0x10u)) {
		setWin32LastError(kAccessDenied);
		cpu->r[EAX] = 0;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(currentDirectoryMutex);
		currentDirectory = relativePath == "." ? std::string{} : relativePath;
	}
	cpu->r[EAX] = 1;
}

void getCurrentDirectoryA(CPU* cpu, const ApiImportStub&)
{
	constexpr char kGameRoot[] = "D:\\Games\\Battle Realms Complete";
	std::string path = kGameRoot;
	std::string directory;
	{
		std::lock_guard<std::mutex> lock(currentDirectoryMutex);
		directory = currentDirectory;
	}
	if (!directory.empty()) {
		for (char& character : directory) {
			if (character == '/') character = '\\';
		}
		path.push_back('\\');
		path += directory;
	}
	const uint32_t capacity = argument(cpu, 0);
	const uint32_t bufferAddress = argument(cpu, 1);
	const uint32_t required = static_cast<uint32_t>(path.size() + 1u);
	if (capacity < required || !bufferAddress) {
		cpu->r[EAX] = required;
		return;
	}
	for (size_t i = 0; i < path.size(); ++i)
		guest::store<uint8_t>(bufferAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(path[i]));
	guest::store<uint8_t>(bufferAddress + static_cast<uint32_t>(path.size()), 0u);
	cpu->r[EAX] = static_cast<uint32_t>(path.size());
}

void getModuleFileNameA(CPU* cpu, const ApiImportStub&)
{
	constexpr char kMainModule[] = "D:\\Games\\Battle Realms Complete\\Battle_Realms_F.exe";
	const uint32_t bufferAddress = argument(cpu, 1);
	const uint32_t capacity = argument(cpu, 2);
	const uint32_t copied = std::min<uint32_t>(
		capacity, static_cast<uint32_t>(sizeof(kMainModule) - 1u));
	if (!bufferAddress || !capacity) {
		cpu->r[EAX] = 0;
		setWin32LastError(kInvalidParameter);
		return;
	}
	for (uint32_t i = 0; i < copied; ++i)
		guest::store<uint8_t>(bufferAddress + i, static_cast<uint8_t>(kMainModule[i]));
	if (copied < capacity)
		guest::store<uint8_t>(bufferAddress + copied, 0u);
	cpu->r[EAX] = copied;
	if (copied < sizeof(kMainModule) - 1u) setWin32LastError(122u);
}

std::string trimIniText(std::string value)
{
	const auto whitespace = [](unsigned char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; };
	while (!value.empty() && whitespace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
	while (!value.empty() && whitespace(static_cast<unsigned char>(value.back()))) value.pop_back();
	return value;
}

bool equalIniName(const std::string& left, const std::string& right)
{
	if (left.size() != right.size()) return false;
	for (size_t i = 0; i < left.size(); ++i) {
		if (std::tolower(static_cast<unsigned char>(left[i])) !=
			std::tolower(static_cast<unsigned char>(right[i]))) return false;
	}
	return true;
}

bool findIniInteger(const std::string& contents, const std::string& section,
	const std::string& key, uint32_t& result)
{
	bool inSection = false;
	for (size_t begin = 0; begin <= contents.size();) {
		const size_t end = contents.find('\n', begin);
		std::string line = trimIniText(contents.substr(begin,
			end == std::string::npos ? std::string::npos : end - begin));
		if (!line.empty() && line[0] != ';' && line[0] != '#') {
			if (line.front() == '[' && line.back() == ']') {
				inSection = equalIniName(trimIniText(line.substr(1, line.size() - 2u)), section);
			} else if (inSection) {
				const size_t equals = line.find('=');
				if (equals != std::string::npos && equalIniName(trimIniText(line.substr(0, equals)), key)) {
					const std::string value = trimIniText(line.substr(equals + 1u));
					char* parsedEnd = nullptr;
					const long parsed = std::strtol(value.c_str(), &parsedEnd, 10);
					result = (parsedEnd == value.c_str()) ? 0u : static_cast<uint32_t>(parsed);
					return true;
				}
			}
		}
		if (end == std::string::npos) break;
		begin = end + 1u;
	}
	return false;
}

std::string findIniString(const std::string& contents, const std::string& section,
	const std::string& key, const std::string& fallback)
{
	bool inSection = false;
	for (size_t begin = 0; begin <= contents.size();) {
		const size_t end = contents.find('\n', begin);
		std::string line = trimIniText(contents.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
		if (!line.empty() && line[0] != ';' && line[0] != '#') {
			if (line.front() == '[' && line.back() == ']') inSection = equalIniName(trimIniText(line.substr(1, line.size()-2u)), section);
			else if (inSection) { const size_t eq = line.find('='); if (eq != std::string::npos && equalIniName(trimIniText(line.substr(0,eq)), key)) return trimIniText(line.substr(eq+1u)); }
		}
		if (end == std::string::npos) break; begin = end + 1u;
	}
	return fallback;
}

void getPrivateProfileStringA(CPU* cpu, const ApiImportStub&)
{
	std::string section, key, fallback, rawPath, relativePath;
	const uint32_t buffer = argument(cpu, 3), capacity = argument(cpu, 4);
	if (!readGuestString(argument(cpu,0), section) || !readGuestString(argument(cpu,1), key) ||
		!readGuestStringAllowEmpty(argument(cpu,2), fallback) || !readGuestString(argument(cpu,5), rawPath) ||
		!normalizeGuestPath(rawPath, relativePath) || !buffer || !capacity) { cpu->r[EAX]=0; return; }
	Win32FileOpener opener; { std::lock_guard<std::mutex> lock(fileMutex); opener=fileOpener; }
	std::string value=fallback;
	if (opener) { auto opened=opener(relativePath.c_str(),kGenericRead,kOpenExisting); if (opened.fd>=0) {
		br_stat st{}; if (BR_FSTAT(opened.fd,&st)==0 && st.st_size>=0 && st.st_size<=16*1024*1024 && BR_SEEK(opened.fd,0,0)==0) {
			std::string data((size_t)st.st_size,'\0'); size_t n=0; while(n<data.size()){auto r=BR_READ(opened.fd,data.data()+n,(unsigned)(data.size()-n));if(r<=0)break;n+=(size_t)r;} if(n==data.size()) value=findIniString(data,section,key,fallback);
		} (void)BR_CLOSE(opened.fd); } }
	const uint32_t count=std::min<uint32_t>((uint32_t)value.size(),capacity-1u); for(uint32_t i=0;i<count;++i) guest::store<uint8_t>(buffer+i,(uint8_t)value[i]); guest::store<uint8_t>(buffer+count,0); cpu->r[EAX]=count;
}

void getPrivateProfileIntA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t fallback = argument(cpu, 2);
	std::string section, key, rawPath, relativePath;
	if (!readGuestString(argument(cpu, 0), section) || !readGuestString(argument(cpu, 1), key) ||
		!readGuestString(argument(cpu, 3), rawPath) || !normalizeGuestPath(rawPath, relativePath)) {
		cpu->r[EAX] = fallback;
		return;
	}

	Win32FileOpener opener = nullptr;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		opener = fileOpener;
	}
	uint32_t result = fallback;
	if (opener) {
		const Win32FileOpenResult opened = opener(relativePath.c_str(), kGenericRead, kOpenExisting);
		if (opened.fd >= 0) {
			br_stat metadata{};
			constexpr int64_t kMaximumIniBytes = 16 * 1024 * 1024;
			if (BR_FSTAT(opened.fd, &metadata) == 0 && metadata.st_size >= 0 && metadata.st_size <= kMaximumIniBytes &&
				BR_SEEK(opened.fd, 0, 0) == 0) {
				std::string contents(static_cast<size_t>(metadata.st_size), '\0');
				size_t offset = 0;
				while (offset < contents.size()) {
					const auto amount = BR_READ(opened.fd, contents.data() + offset,
						static_cast<unsigned>(contents.size() - offset));
					if (amount <= 0) break;
					offset += static_cast<size_t>(amount);
				}
				if (offset == contents.size()) (void)findIniInteger(contents, section, key, result);
			}
			(void)BR_CLOSE(opened.fd);
		}
	}
#if defined(__ANDROID__)
	br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_INFO, "BattleRealmsNative",
		"GetPrivateProfileIntA section='%s' key='%s' file='%s' result=%u",
		section.c_str(), key.c_str(), rawPath.c_str(), result);
#endif
	cpu->r[EAX] = result;
}
} // namespace

void configureWin32FileOpener(Win32FileOpener opener)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	fileOpener = opener;
}

void configureWin32FileAttributesQuery(Win32FileAttributesQuery query)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	fileAttributesQuery = query;
}

void configureWin32FileDelete(Win32FileDelete deleter)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	fileDelete = deleter;
}

void configureWin32FileDirectoryCreator(Win32FileDirectoryCreator creator)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	fileDirectoryCreator = creator;
}

void configureWin32FileFindQuery(Win32FileFindQuery query)
{
	std::lock_guard<std::mutex> lock(fileMutex);
	fileFindQuery = query;
}

bool closeWin32FileHandle(uint32_t handle)
{
	int fd = -1;
	{
		std::lock_guard<std::mutex> lock(fileMutex);
		const auto found = fileHandles.find(handle);
		if (found == fileHandles.end()) return false;
		fd = found->second.fd;
		fileHandles.erase(found);
	}
	return BR_CLOSE(fd) == 0;
}

bool registerWin32FileHandlers(ImportDispatchRegistry& handlers)
{
	return handlers.registerName("kernel32.dll", "GetLogicalDrives", getLogicalDrives, 0) &&
		handlers.registerName("kernel32.dll", "GetLogicalDriveStringsA", getLogicalDriveStringsA, 8) &&
		handlers.registerName("kernel32.dll", "GetDriveTypeA", getDriveTypeA, 4) &&
		handlers.registerName("kernel32.dll", "CreateFileA", createFileA, 28) &&
		handlers.registerName("kernel32.dll", "GetPrivateProfileStringA", getPrivateProfileStringA, 24) &&
		handlers.registerName("kernel32.dll", "GetPrivateProfileIntA", getPrivateProfileIntA, 16) &&
		handlers.registerName("kernel32.dll", "FindFirstFileA", findFirstFileA, 8) &&
		handlers.registerName("kernel32.dll", "FindNextFileA", findNextFileA, 8) &&
		handlers.registerName("kernel32.dll", "FindClose", findClose, 4) &&
		handlers.registerName("kernel32.dll", "ReadFile", readFile, 20) &&
		handlers.registerName("kernel32.dll", "WriteFile", writeFile, 20) &&
		handlers.registerName("kernel32.dll", "FlushFileBuffers", flushFileBuffers, 4) &&
		handlers.registerName("kernel32.dll", "SetEndOfFile", setEndOfFile, 4) &&
		handlers.registerName("kernel32.dll", "SetFilePointer", setFilePointer, 16) &&
		handlers.registerName("kernel32.dll", "GetFileSize", getFileSize, 8) &&
		handlers.registerName("kernel32.dll", "GetFileType", getFileType, 4) &&
		handlers.registerName("kernel32.dll", "GetFileAttributesA", getFileAttributesA, 4) &&
		handlers.registerName("kernel32.dll", "CreateDirectoryExA", createDirectoryExA, 12) &&
		handlers.registerName("kernel32.dll", "SetFileAttributesA", setFileAttributesA, 8) &&
		handlers.registerName("kernel32.dll", "DeleteFileA", deleteFileA, 4) &&
		handlers.registerName("kernel32.dll", "GetCurrentDirectoryA", getCurrentDirectoryA, 8) &&
		handlers.registerName("kernel32.dll", "SetCurrentDirectoryA", setCurrentDirectoryA, 4) &&
		handlers.registerName("kernel32.dll", "GetModuleFileNameA", getModuleFileNameA, 12);
}

} // namespace br::runtime
