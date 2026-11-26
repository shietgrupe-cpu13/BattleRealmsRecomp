#include "lift_dispatch.h"
#include "win32_base.h"
#include "win32_file.h"
#include "win32_memory.h"
#include "win32_sync.h"
#include "guest.h"
#include "br_x87.h"
#include <limits>

#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define BR_TEST_OPEN _open
#define BR_TEST_RDONLY (_O_RDONLY | _O_BINARY)
#define BR_TEST_RDWR (_O_RDWR | _O_BINARY)
#define BR_TEST_WRONLY (_O_WRONLY | _O_BINARY)
#define BR_TEST_CREAT _O_CREAT
#define BR_TEST_EXCL _O_EXCL
#define BR_TEST_TRUNC _O_TRUNC
#else
#include <fcntl.h>
#include <unistd.h>
#define BR_TEST_OPEN ::open
#define BR_TEST_RDONLY O_RDONLY
#define BR_TEST_RDWR O_RDWR
#define BR_TEST_WRONLY O_WRONLY
#define BR_TEST_CREAT O_CREAT
#define BR_TEST_EXCL O_EXCL
#define BR_TEST_TRUNC O_TRUNC
#endif

namespace {
unsigned liftedCalls = 0;
unsigned importedCalls = 0;
std::filesystem::path fixtureRoot;
std::string observedFixturePath;
unsigned fixtureOpenCalls = 0;
const br::runtime::ApiImportStub fakeImport{
	"kernel32.dll", "FakeTwoArgTest", 0, 0x70000000u, 1, false};
const br::runtime::ApiImportStub setLastErrorImport{
	"kernel32.dll", "SetLastError", 0, 0x70000010u, 1, false};
const br::runtime::ApiImportStub getLastErrorImport{
	"kernel32.dll", "GetLastError", 0, 0x70000020u, 1, false};
const br::runtime::ApiImportStub qpcImport{
	"kernel32.dll", "QueryPerformanceCounter", 0, 0x70000030u, 1, false};
const br::runtime::ApiImportStub getLocalTimeImport{
	"kernel32.dll", "GetLocalTime", 0, 0x70000310u, 1, false};
const br::runtime::ApiImportStub systemTimeToFileTimeImport{
	"kernel32.dll", "SystemTimeToFileTime", 0, 0x70000320u, 1, false};
const br::runtime::ApiImportStub fileTimeToLocalFileTimeImport{
	"kernel32.dll", "FileTimeToLocalFileTime", 0, 0x70000330u, 1, false};
const br::runtime::ApiImportStub fileTimeToSystemTimeImport{
	"kernel32.dll", "FileTimeToSystemTime", 0, 0x70000340u, 1, false};
const br::runtime::ApiImportStub sleepExImport{
	"kernel32.dll", "SleepEx", 0, 0x70000350u, 1, false};
const br::runtime::ApiImportStub createEventImport{
	"kernel32.dll", "CreateEventA", 0, 0x70000040u, 1, false};
const br::runtime::ApiImportStub setEventImport{
	"kernel32.dll", "SetEvent", 0, 0x70000050u, 1, false};
const br::runtime::ApiImportStub resetEventImport{
	"kernel32.dll", "ResetEvent", 0, 0x70000060u, 1, false};
const br::runtime::ApiImportStub waitImport{
	"kernel32.dll", "WaitForSingleObject", 0, 0x70000070u, 1, false};
const br::runtime::ApiImportStub closeImport{
	"kernel32.dll", "CloseHandle", 0, 0x70000080u, 1, false};
const br::runtime::ApiImportStub initCsImport{
	"kernel32.dll", "InitializeCriticalSection", 0, 0x70000090u, 1, false};
const br::runtime::ApiImportStub enterCsImport{
	"kernel32.dll", "EnterCriticalSection", 0, 0x700000a0u, 1, false};
const br::runtime::ApiImportStub leaveCsImport{
	"kernel32.dll", "LeaveCriticalSection", 0, 0x700000b0u, 1, false};
const br::runtime::ApiImportStub tryEnterCsImport{
	"kernel32.dll", "TryEnterCriticalSection", 0, 0x700000c0u, 1, false};
const br::runtime::ApiImportStub interlockedIncrementImport{
	"kernel32.dll", "InterlockedIncrement", 0, 0x700000d0u, 1, false};
const br::runtime::ApiImportStub interlockedDecrementImport{
	"kernel32.dll", "InterlockedDecrement", 0, 0x700000e0u, 1, false};
const br::runtime::ApiImportStub interlockedExchangeImport{
	"kernel32.dll", "InterlockedExchange", 0, 0x700000f0u, 1, false};
const br::runtime::ApiImportStub getProcessHeapImport{
	"kernel32.dll", "GetProcessHeap", 0, 0x70000100u, 1, false};
const br::runtime::ApiImportStub heapCreateImport{
	"kernel32.dll", "HeapCreate", 0, 0x70000110u, 1, false};
const br::runtime::ApiImportStub heapDestroyImport{
	"kernel32.dll", "HeapDestroy", 0, 0x70000120u, 1, false};
const br::runtime::ApiImportStub heapAllocImport{
	"kernel32.dll", "HeapAlloc", 0, 0x70000130u, 1, false};
const br::runtime::ApiImportStub heapFreeImport{
	"kernel32.dll", "HeapFree", 0, 0x70000140u, 1, false};
const br::runtime::ApiImportStub heapReAllocImport{
	"kernel32.dll", "HeapReAlloc", 0, 0x70000150u, 1, false};
const br::runtime::ApiImportStub heapSizeImport{
	"kernel32.dll", "HeapSize", 0, 0x70000160u, 1, false};
const br::runtime::ApiImportStub globalAllocImport{
	"kernel32.dll", "GlobalAlloc", 0, 0x70000170u, 1, false};
const br::runtime::ApiImportStub globalFreeImport{
	"kernel32.dll", "GlobalFree", 0, 0x70000180u, 1, false};
const br::runtime::ApiImportStub globalLockImport{
	"kernel32.dll", "GlobalLock", 0, 0x70000190u, 1, false};
const br::runtime::ApiImportStub globalUnlockImport{
	"kernel32.dll", "GlobalUnlock", 0, 0x700001a0u, 1, false};
const br::runtime::ApiImportStub globalSizeImport{
	"kernel32.dll", "GlobalSize", 0, 0x700001b0u, 1, false};
const br::runtime::ApiImportStub virtualAllocImport{
	"kernel32.dll", "VirtualAlloc", 0, 0x700001c0u, 1, false};
const br::runtime::ApiImportStub virtualFreeImport{
	"kernel32.dll", "VirtualFree", 0, 0x700001d0u, 1, false};
const br::runtime::ApiImportStub tlsAllocImport{
	"kernel32.dll", "TlsAlloc", 0, 0x700001e0u, 1, false};
const br::runtime::ApiImportStub tlsFreeImport{
	"kernel32.dll", "TlsFree", 0, 0x700001f0u, 1, false};
const br::runtime::ApiImportStub tlsGetValueImport{
	"kernel32.dll", "TlsGetValue", 0, 0x70000200u, 1, false};
const br::runtime::ApiImportStub tlsSetValueImport{
	"kernel32.dll", "TlsSetValue", 0, 0x70000210u, 1, false};
const br::runtime::ApiImportStub createFileImport{
	"kernel32.dll", "CreateFileA", 0, 0x70000220u, 1, false};
const br::runtime::ApiImportStub readFileImport{
	"kernel32.dll", "ReadFile", 0, 0x70000230u, 1, false};
const br::runtime::ApiImportStub setFilePointerImport{
	"kernel32.dll", "SetFilePointer", 0, 0x70000240u, 1, false};
const br::runtime::ApiImportStub getFileSizeImport{
	"kernel32.dll", "GetFileSize", 0, 0x70000250u, 1, false};
const br::runtime::ApiImportStub getFileTypeImport{
	"kernel32.dll", "GetFileType", 0, 0x70000260u, 1, false};
const br::runtime::ApiImportStub writeFileImport{
	"kernel32.dll", "WriteFile", 0, 0x70000270u, 1, false};
const br::runtime::ApiImportStub flushFileBuffersImport{
	"kernel32.dll", "FlushFileBuffers", 0, 0x70000280u, 1, false};
const br::runtime::ApiImportStub setEndOfFileImport{
	"kernel32.dll", "SetEndOfFile", 0, 0x70000290u, 1, false};
const br::runtime::ApiImportStub getFileAttributesImport{
	"kernel32.dll", "GetFileAttributesA", 0, 0x700002a0u, 1, false};
const br::runtime::ApiImportStub getCurrentDirectoryImport{
	"kernel32.dll", "GetCurrentDirectoryA", 0, 0x700002b0u, 1, false};
const br::runtime::ApiImportStub setCurrentDirectoryImport{
	"kernel32.dll", "SetCurrentDirectoryA", 0, 0x700002c0u, 1, false};
const br::runtime::ApiImportStub getModuleFileNameImport{
	"kernel32.dll", "GetModuleFileNameA", 0, 0x700002d0u, 1, false};
const br::runtime::ApiImportStub createDirectoryExImport{
	"kernel32.dll", "CreateDirectoryExA", 0, 0x700002e8u, 1, false};
const br::runtime::ApiImportStub getVersionImport{
	"kernel32.dll", "GetVersion", 0, 0x700002e0u, 1, false};

void liftedOne(CPU* cpu)
{
	++liftedCalls;
	cpu->r[EAX] += 7;
}

void liftedTwo(CPU* cpu)
{
	++liftedCalls;
	cpu->r[EAX] += 11;
}

const br::runtime::ApiImportStub* resolveFakeImport(const void*, uint32_t address)
{
	switch (address) {
	case 0x70000000u: return &fakeImport;
	case 0x70000010u: return &setLastErrorImport;
	case 0x70000020u: return &getLastErrorImport;
	case 0x70000030u: return &qpcImport;
	case 0x70000310u: return &getLocalTimeImport;
	case 0x70000320u: return &systemTimeToFileTimeImport;
	case 0x70000330u: return &fileTimeToLocalFileTimeImport;
	case 0x70000340u: return &fileTimeToSystemTimeImport;
	case 0x70000350u: return &sleepExImport;
	case 0x70000040u: return &createEventImport;
	case 0x70000050u: return &setEventImport;
	case 0x70000060u: return &resetEventImport;
	case 0x70000070u: return &waitImport;
	case 0x70000080u: return &closeImport;
	case 0x70000090u: return &initCsImport;
	case 0x700000a0u: return &enterCsImport;
	case 0x700000b0u: return &leaveCsImport;
	case 0x700000c0u: return &tryEnterCsImport;
	case 0x700000d0u: return &interlockedIncrementImport;
	case 0x700000e0u: return &interlockedDecrementImport;
	case 0x700000f0u: return &interlockedExchangeImport;
	case 0x70000100u: return &getProcessHeapImport;
	case 0x70000110u: return &heapCreateImport;
	case 0x70000120u: return &heapDestroyImport;
	case 0x70000130u: return &heapAllocImport;
	case 0x70000140u: return &heapFreeImport;
	case 0x70000150u: return &heapReAllocImport;
	case 0x70000160u: return &heapSizeImport;
	case 0x70000170u: return &globalAllocImport;
	case 0x70000180u: return &globalFreeImport;
	case 0x70000190u: return &globalLockImport;
	case 0x700001a0u: return &globalUnlockImport;
	case 0x700001b0u: return &globalSizeImport;
	case 0x700001c0u: return &virtualAllocImport;
	case 0x700001d0u: return &virtualFreeImport;
	case 0x700001e0u: return &tlsAllocImport;
	case 0x700001f0u: return &tlsFreeImport;
	case 0x70000200u: return &tlsGetValueImport;
	case 0x70000210u: return &tlsSetValueImport;
	case 0x70000220u: return &createFileImport;
	case 0x70000230u: return &readFileImport;
	case 0x70000240u: return &setFilePointerImport;
	case 0x70000250u: return &getFileSizeImport;
	case 0x70000260u: return &getFileTypeImport;
	case 0x70000270u: return &writeFileImport;
	case 0x70000280u: return &flushFileBuffersImport;
	case 0x70000290u: return &setEndOfFileImport;
	case 0x700002a0u: return &getFileAttributesImport;
	case 0x700002b0u: return &getCurrentDirectoryImport;
	case 0x700002c0u: return &setCurrentDirectoryImport;
	case 0x700002d0u: return &getModuleFileNameImport;
	case 0x700002e8u: return &createDirectoryExImport;
	case 0x700002e0u: return &getVersionImport;
	default: return nullptr;
	}
}

br::runtime::Win32FileOpenResult openFixtureFile(
	const char* relativePath, uint32_t desiredAccess, uint32_t disposition)
{
	if (!relativePath) return {-1, 87u};
	const std::filesystem::path relative(relativePath);
	for (const auto& component : relative) {
		if (component == "..") return {-1, 87u};
	}
	observedFixturePath = relative.generic_string();
	++fixtureOpenCalls;
	const std::filesystem::path sourcePath = fixtureRoot / relative;
	const std::filesystem::path overlayPath = fixtureRoot / "app-private" / relative;
	std::error_code ec;
	const bool overlayExists = std::filesystem::is_regular_file(overlayPath, ec);
	const bool sourceExists = std::filesystem::is_regular_file(sourcePath, ec);
	const bool exists = overlayExists || sourceExists;
	if (disposition == 1u && exists) return {-1, 80u};
	if ((disposition == 3u || disposition == 5u) && !exists) return {-1, 2u};
	uint32_t lastError = 0u;
	if ((disposition == 2u || disposition == 4u) && exists) lastError = 183u;
	const bool writable = (desiredAccess & 0x40000000u) != 0;
	const bool readable = (desiredAccess & 0x80000000u) != 0;
	std::filesystem::path actualPath = overlayExists ? overlayPath : sourcePath;
	if (writable || disposition != 3u) {
		actualPath = overlayPath;
		if (!overlayExists && sourceExists && (disposition == 3u || disposition == 4u)) {
			std::filesystem::create_directories(overlayPath.parent_path());
			std::filesystem::copy_file(sourcePath, overlayPath,
				std::filesystem::copy_options::overwrite_existing, ec);
			if (ec) return {-1, 5u};
		}
	}
	if (actualPath == overlayPath) {
		std::filesystem::create_directories(overlayPath.parent_path(), ec);
		if (ec) return {-1, 5u};
	}
	int flags = readable && writable ? BR_TEST_RDWR :
		(writable ? BR_TEST_WRONLY : BR_TEST_RDONLY);
	if (disposition == 1u) flags |= BR_TEST_CREAT | BR_TEST_EXCL;
	else if (disposition == 2u) flags |= BR_TEST_CREAT | BR_TEST_TRUNC;
	else if (disposition == 4u) flags |= BR_TEST_CREAT;
	else if (disposition == 5u) flags |= BR_TEST_CREAT | BR_TEST_TRUNC;
	int fd = -1;
#if defined(_WIN32)
	fd = _open(actualPath.string().c_str(), flags, _S_IREAD | _S_IWRITE);
#else
	fd = ::open(actualPath.string().c_str(), flags, 0600);
#endif
	if (fd < 0) return {-1, errno == EEXIST ? 80u : (errno == ENOENT ? 2u : 5u)};
	return {fd, lastError};
}

uint32_t getFixtureFileAttributes(const char* relativePath)
{
	if (!relativePath) return 0xffffffffu;
	const std::filesystem::path relative(relativePath);
	const auto overlay = fixtureRoot / "app-private" / relative;
	const auto source = fixtureRoot / relative;
	std::error_code ec;
	if (std::filesystem::is_directory(overlay, ec)) return 0x10u;
	if (std::filesystem::is_regular_file(overlay, ec)) return 0x20u;
	if (std::filesystem::is_directory(source, ec)) return 0x10u;
	if (std::filesystem::is_regular_file(source, ec)) return 0x01u;
	return 0xffffffffu;
}

uint32_t createFixtureDirectory(const char* templatePath, const char* relativePath)
{
	if (!templatePath || !relativePath) return 87u;
	if ((getFixtureFileAttributes(templatePath) & 0x10u) == 0u)
		return getFixtureFileAttributes(templatePath) == 0xffffffffu ? 2u : 5u;
	const std::filesystem::path relative(relativePath);
	for (const auto& component : relative) if (component == "..") return 87u;
	const auto overlayRoot = fixtureRoot / "app-private";
	const auto overlayTarget = overlayRoot / relative;
	const auto sourceTarget = fixtureRoot / relative;
	std::error_code error;
	if (std::filesystem::exists(overlayTarget, error) || std::filesystem::exists(sourceTarget, error))
		return 183u;
	const auto parent = relative.parent_path();
	const uint32_t parentAttributes = getFixtureFileAttributes(
		parent.empty() ? "." : parent.generic_string().c_str());
	if (parentAttributes == 0xffffffffu || (parentAttributes & 0x10u) == 0u)
		return 3u;
	std::filesystem::create_directories(overlayTarget.parent_path(), error);
	if (error) return error == std::errc::no_such_file_or_directory ? 3u : 5u;
	if (!std::filesystem::create_directory(overlayTarget, error))
		return !error || error == std::errc::file_exists ? 183u : 5u;
	return 0u;
}

void writeGuestString(uint32_t address, const std::string& value)
{
	for (size_t index = 0; index < value.size(); ++index)
		guest::store<uint8_t>(address + static_cast<uint32_t>(index), static_cast<uint8_t>(value[index]));
	guest::store<uint8_t>(address + static_cast<uint32_t>(value.size()), 0u);
}

void fakeTwoArgImport(CPU* cpu, const br::runtime::ApiImportStub& import)
{
	if (import.name == "FakeTwoArgTest") {
		++importedCalls;
		cpu->r[EAX] = 0x12345678u;
	}
}
} // namespace

extern const LiftedEntry lifted_table[] = {
	{0x00401000u, liftedOne},
	{0x20002000u, liftedTwo},
};
extern const unsigned lifted_count = 2;

int main()
{
	CPU fxamCpu{};
	fxamCpu.top = 5u;
	fxamCpu.st[fxamCpu.top] = -1.0;
	br_fxam(&fxamCpu);
	if (fxamCpu.fcc != 0x4100u || fstatus(&fxamCpu) != 0x6900u || fxamCpu.top != 5u) {
		std::fprintf(stderr, "FXAM empty stack/status mismatch: cc=%04x status=%04x top=%u\n",
			fxamCpu.fcc, fstatus(&fxamCpu), fxamCpu.top);
		return 86;
	}
	fxamCpu.top = 0u;
	const auto checkFxam = [&fxamCpu](double value, uint16_t expected, const char* name) {
		br_fpush(&fxamCpu, value);
		br_fxam(&fxamCpu);
		const uint16_t expectedStatus = static_cast<uint16_t>(expected | ((fxamCpu.top & 7u) << 11));
		if (fxamCpu.fcc != expected || fstatus(&fxamCpu) != expectedStatus || fxamCpu.top != 7u) {
			std::fprintf(stderr, "FXAM %s classification/status mismatch: cc=%04x/%04x status=%04x/%04x top=%u\n",
				name, fxamCpu.fcc, expected, fstatus(&fxamCpu), expectedStatus, fxamCpu.top);
			br_fpop(&fxamCpu);
			return false;
		}
		br_fpop(&fxamCpu);
		br_fxam(&fxamCpu);
		if (fxamCpu.fcc != 0x4100u) {
			std::fprintf(stderr, "FXAM %s pop did not restore empty tag: cc=%04x\n", name, fxamCpu.fcc);
			return false;
		}
		return true;
	};
	if (!checkFxam(1.5, 0x0400u, "positive normal") ||
		!checkFxam(-1.5, 0x0600u, "negative normal") ||
		!checkFxam(0.0, 0x4000u, "positive zero") ||
		!checkFxam(-0.0, 0x4200u, "negative zero") ||
		!checkFxam(std::numeric_limits<double>::infinity(), 0x0500u, "positive infinity") ||
		!checkFxam(-std::numeric_limits<double>::infinity(), 0x0700u, "negative infinity") ||
		!checkFxam(std::numeric_limits<double>::quiet_NaN(), 0x0100u, "positive NaN") ||
		!checkFxam(-std::numeric_limits<double>::quiet_NaN(), 0x0300u, "negative NaN") ||
		!checkFxam(std::numeric_limits<double>::denorm_min(), 0x4400u, "positive denormal") ||
		!checkFxam(-std::numeric_limits<double>::denorm_min(), 0x4600u, "negative denormal")) return 86;
	br_fpush(&fxamCpu, -2.0);
	br_fxch(&fxamCpu, 1u);
	br_fxam(&fxamCpu);
	if (fxamCpu.fcc != 0x4100u) {
		std::fprintf(stderr, "FXAM FXCH failed to expose empty target: cc=%04x\n", fxamCpu.fcc);
		return 86;
	}
	br_fxch(&fxamCpu, 1u);
	br_fxam(&fxamCpu);
	if (fxamCpu.fcc != 0x0600u) {
		std::fprintf(stderr, "FXAM FXCH failed to restore occupied target: cc=%04x\n", fxamCpu.fcc);
		return 86;
	}
	br_fpop(&fxamCpu);
	CPU secondFxamCpu{};
	secondFxamCpu.top = 3u;
	br_fpush(&fxamCpu, 4.0);
	br_fxam(&secondFxamCpu);
	if (secondFxamCpu.fcc != 0x4100u) {
		std::fprintf(stderr, "FXAM tag state leaked between guest CPU contexts: cc=%04x\n",
			secondFxamCpu.fcc);
		return 86;
	}
	br_fxam(&fxamCpu);
	if (fxamCpu.fcc != 0x0400u) {
		std::fprintf(stderr, "FXAM occupied context lost its tag: cc=%04x\n", fxamCpu.fcc);
		return 86;
	}
	br_x87_reset(&fxamCpu);
	br_fxam(&fxamCpu);
	if (fxamCpu.fcc != 0x4100u) {
		std::fprintf(stderr, "FXAM CPU reset did not clear tags: cc=%04x\n", fxamCpu.fcc);
		return 86;
	}
	br::runtime::ImportDispatchRegistry handlers;
	if (!handlers.registerName("KERNEL32.DLL", "FakeTwoArgTest", fakeTwoArgImport, 8) ||
		handlers.registerName("kernel32.dll", "FakeTwoArgTest", fakeTwoArgImport, 8) ||
		!br::runtime::registerWin32BaseHandlers(handlers) ||
		!br::runtime::registerWin32SyncHandlers(handlers) ||
		!br::runtime::registerWin32MemoryHandlers(handlers) ||
		!br::runtime::registerWin32FileHandlers(handlers)) {
		std::fprintf(stderr, "import handler registration check failed\n");
		return 1;
	}
	if (!guest::init() || !guest::commit(0x00100000u, 0x10000u)) {
		std::fprintf(stderr, "guest stack memory setup failed\n");
		return 1;
	}
	const auto checkExt80Store = [&fxamCpu](double value, uint64_t expectedSignificand,
		uint16_t expectedSignExponent, const char* name) {
		br_fpush(&fxamCpu, value);
		br_fstp_ext80(&fxamCpu, 0x00100100u);
		br_fxam(&fxamCpu);
		const uint64_t significand = guest::load<uint64_t>(0x00100100u);
		const uint16_t signExponent = guest::load<uint16_t>(0x00100108u);
		if (significand != expectedSignificand || signExponent != expectedSignExponent ||
			fxamCpu.fcc != 0x4100u) {
			std::fprintf(stderr, "x87 FSTP EXT80 %s mismatch: significand=%016llx/%016llx signexp=%04x/%04x\n",
				name, static_cast<unsigned long long>(significand),
				static_cast<unsigned long long>(expectedSignificand), signExponent, expectedSignExponent);
			return false;
		}
		return true;
	};
	if (!checkExt80Store(1.5, 0xc000000000000000ull, 0x3fffu, "positive normal") ||
		!checkExt80Store(-2.0, 0x8000000000000000ull, 0xc000u, "negative normal") ||
		!checkExt80Store(0.0, 0u, 0u, "positive zero") ||
		!checkExt80Store(-0.0, 0u, 0x8000u, "negative zero") ||
		!checkExt80Store(std::numeric_limits<double>::infinity(), 0x8000000000000000ull,
			0x7fffu, "positive infinity") ||
		!checkExt80Store(std::numeric_limits<double>::quiet_NaN(), 0xc000000000000000ull,
			0x7fffu, "quiet NaN")) return 87;
	br::runtime::configureLiftDispatch(resolveFakeImport, nullptr, &handlers);

	CPU cpu{};
	cpu.r[ESP] = 0x001000f0u;
	guest::store<uint32_t>(cpu.r[ESP], 0x00abcdefu);
	call_target(&cpu, getVersionImport.guestAddress);
	if (cpu.r[EAX] != 0x0a280105u || cpu.r[ESP] != 0x001000f4u) {
		std::fprintf(stderr, "GetVersion compatibility result or stack cleanup mismatch\n");
		return 37;
	}
	cpu.r[EAX] = 5;
	call_target(&cpu, 0x00401000u);
	if (cpu.r[EAX] != 12 || liftedCalls != 1) {
		std::fprintf(stderr, "direct lifted dispatch mismatch\n");
		return 2;
	}

	std::atomic<uint64_t> inlineCache{0};
	call_target_ic(&cpu, 0x20002000u, &inlineCache);
	const uint64_t cached = inlineCache.load(std::memory_order_relaxed);
	if (cpu.r[EAX] != 23 || liftedCalls != 2 ||
		static_cast<uint32_t>(cached) != 0x20002000u || (cached >> 32) != 1) {
		std::fprintf(stderr, "indirect lifted dispatch/cache mismatch\n");
		return 3;
	}

	cpu.r[ESP] = 0x00100000u;
	call_target(&cpu, fakeImport.guestAddress);
	if (cpu.r[EAX] != 0x12345678u || cpu.r[ESP] != 0x0010000cu || importedCalls != 1) {
		std::fprintf(stderr, "import dispatch/stack cleanup mismatch\n");
		return 4;
	}

	// Exercise real base handlers through the same import dispatcher and guest stack ABI.
	cpu.r[ESP] = 0x00100100u;
	guest::store<uint32_t>(cpu.r[ESP], 0x00abcdefu); // caller return address
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x1234u);
	call_target(&cpu, setLastErrorImport.guestAddress);
	if (cpu.r[ESP] != 0x00100108u) {
		std::fprintf(stderr, "SetLastError stack cleanup mismatch\n");
		return 5;
	}
	guest::store<uint32_t>(cpu.r[ESP], 0x00abcdefu);
	call_target(&cpu, getLastErrorImport.guestAddress);
	if (cpu.r[EAX] != 0x1234u || cpu.r[ESP] != 0x0010010cu) {
		std::fprintf(stderr, "GetLastError behavior mismatch\n");
		return 6;
	}
	guest::store<uint32_t>(cpu.r[ESP], 0x00abcdefu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x00100200u);
	call_target(&cpu, qpcImport.guestAddress);
	const int64_t qpc = guest::load<int64_t>(0x00100200u);
	if (cpu.r[EAX] != 1u || qpc <= 0 || cpu.r[ESP] != 0x00100114u) {
		std::fprintf(stderr, "QueryPerformanceCounter behavior mismatch\n");
		return 7;
	}
	constexpr uint32_t systemTimeBuffer = 0x00100220u;
	guest::store<uint16_t>(systemTimeBuffer + 16u, 0xa55au);
	guest::store<uint32_t>(0x00100120u, 0x00abcdefu);
	guest::store<uint32_t>(0x00100124u, systemTimeBuffer);
	cpu.r[ESP] = 0x00100120u;
	call_target(&cpu, getLocalTimeImport.guestAddress);
	uint16_t systemTime[8]{};
	for (uint32_t i = 0; i < 8u; ++i)
		systemTime[i] = guest::load<uint16_t>(systemTimeBuffer + i * 2u);
	if (systemTime[0] < 2020u || systemTime[0] > 2100u || systemTime[1] < 1u || systemTime[1] > 12u ||
		systemTime[2] > 6u || systemTime[3] < 1u || systemTime[3] > 31u || systemTime[4] > 23u ||
		systemTime[5] > 59u || systemTime[6] > 60u || systemTime[7] > 999u ||
		guest::load<uint16_t>(systemTimeBuffer + 16u) != 0xa55au || cpu.r[ESP] != 0x00100128u) {
		std::fprintf(stderr, "GetLocalTime SYSTEMTIME layout, ranges, guard, or stack cleanup mismatch\n");
		return 8;
	}
	std::tm localDay{};
	localDay.tm_year = static_cast<int>(systemTime[0]) - 1900;
	localDay.tm_mon = static_cast<int>(systemTime[1]) - 1;
	localDay.tm_mday = systemTime[3];
	const std::time_t localDayTime = std::mktime(&localDay);
	if (localDayTime == static_cast<std::time_t>(-1) || localDay.tm_wday != systemTime[2]) {
		std::fprintf(stderr, "GetLocalTime day-of-week does not match local calendar date\n");
		return 8;
	}
	const auto invokeSystemTimeToFileTime = [&cpu](const uint16_t fields[8], uint64_t initialOutput) {
		constexpr uint32_t input = 0x00100260u;
		constexpr uint32_t output = 0x00100280u;
		for (uint32_t index = 0; index < 8u; ++index)
			guest::store<uint16_t>(input + index * 2u, fields[index]);
		guest::store<uint64_t>(output, initialOutput);
		guest::store<uint32_t>(output + 8u, 0xa55aa55au);
		cpu.r[ESP] = 0x00100120u;
		guest::store<uint32_t>(cpu.r[ESP], 0x00abcdefu);
		guest::store<uint32_t>(cpu.r[ESP] + 4u, input);
		guest::store<uint32_t>(cpu.r[ESP] + 8u, output);
		call_target(&cpu, systemTimeToFileTimeImport.guestAddress);
		return std::pair<uint32_t, uint64_t>{cpu.r[EAX], guest::load<uint64_t>(output)};
	};
	const uint16_t unixEpochSystemTime[8] = {1970u, 1u, 0u, 1u, 0u, 0u, 0u, 0u};
	const auto unixEpochFileTime = invokeSystemTimeToFileTime(unixEpochSystemTime, 0u);
	if (unixEpochFileTime.first != 1u || unixEpochFileTime.second != 116444736000000000ull ||
		cpu.r[ESP] != 0x0010012cu || guest::load<uint32_t>(0x00100288u) != 0xa55aa55au) {
		std::fprintf(stderr, "SystemTimeToFileTime Unix epoch, UTC conversion, guard, or stdcall ABI mismatch\n");
		return 88;
	}
	const uint16_t leapDateSystemTime[8] = {2000u, 2u, 0u, 29u, 12u, 34u, 56u, 789u};
	const auto leapDateFileTime = invokeSystemTimeToFileTime(leapDateSystemTime, 0u);
	if (leapDateFileTime.first != 1u || leapDateFileTime.second != 125963012967890000ull ||
		cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "SystemTimeToFileTime leap-day/millisecond conversion mismatch\n");
		return 89;
	}
	const uint16_t invalidLeapDate[8] = {2023u, 2u, 0u, 29u, 12u, 34u, 56u, 789u};
	const auto invalidDateFileTime = invokeSystemTimeToFileTime(invalidLeapDate, 0x1122334455667788ull);
	if (invalidDateFileTime.first != 0u || invalidDateFileTime.second != 0x1122334455667788ull ||
		br::runtime::currentWin32LastError() != 87u || cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "SystemTimeToFileTime accepted an invalid calendar date or changed output/ABI\n");
		return 90;
	}
	const auto invokeFileTimeToSystemTime = [&cpu](uint64_t inputValue, uint32_t inputAddress,
		uint32_t outputAddress, const uint16_t initialOutput[8]) {
		constexpr uint32_t stack = 0x00100120u;
		guest::store<uint64_t>(inputAddress, inputValue);
		for (uint32_t index = 0; index < 8u; ++index)
			guest::store<uint16_t>(outputAddress + index * 2u, initialOutput[index]);
		guest::store<uint16_t>(outputAddress + 16u, 0xa55au);
		guest::store<uint32_t>(stack, 0x00abcdefu);
		guest::store<uint32_t>(stack + 4u, inputAddress);
		guest::store<uint32_t>(stack + 8u, outputAddress);
		cpu.r[ESP] = stack;
		call_target(&cpu, fileTimeToSystemTimeImport.guestAddress);
		std::array<uint16_t, 8> fields{};
		for (uint32_t index = 0; index < 8u; ++index)
			fields[index] = guest::load<uint16_t>(outputAddress + index * 2u);
		return std::pair<uint32_t, std::array<uint16_t, 8>>{cpu.r[EAX], fields};
	};
	const uint16_t emptySystemTime[8]{};
	const auto windowsEpochSystemTime = invokeFileTimeToSystemTime(0u, 0x00100380u,
		0x001003a0u, emptySystemTime);
	const std::array<uint16_t, 8> expectedWindowsEpoch{1601u, 1u, 1u, 1u, 0u, 0u, 0u, 0u};
	if (windowsEpochSystemTime.first != 1u || windowsEpochSystemTime.second != expectedWindowsEpoch ||
		guest::load<uint16_t>(0x001003b0u) != 0xa55au || cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToSystemTime Windows epoch, weekday, output guard, or stdcall ABI mismatch\n");
		return 95;
	}
	const uint16_t unixSystemTime[8] = {1970u, 1u, 0u, 1u, 0u, 0u, 0u, 0u};
	const uint64_t unixEpochFileTimeValue = 116444736000000000ull;
	const auto unixSystemTimeResult = invokeFileTimeToSystemTime(unixEpochFileTimeValue,
		0x00100380u, 0x001003a0u, emptySystemTime);
	const std::array<uint16_t, 8> expectedUnixSystemTime{unixSystemTime[0], unixSystemTime[1], 4u,
		unixSystemTime[3], unixSystemTime[4], unixSystemTime[5], unixSystemTime[6], unixSystemTime[7]};
	if (unixSystemTimeResult.first != 1u || unixSystemTimeResult.second != expectedUnixSystemTime ||
		cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToSystemTime Unix epoch or day-of-week mismatch\n");
		return 96;
	}
	const uint64_t leapFileTimeValue = 125963012967890123ull;
	const auto leapSystemTimeResult = invokeFileTimeToSystemTime(leapFileTimeValue,
		0x00100380u, 0x001003a0u, emptySystemTime);
	const std::array<uint16_t, 8> expectedLeapSystemTime{2000u, 2u, 2u, 29u, 12u, 34u, 56u, 789u};
	if (leapSystemTimeResult.first != 1u || leapSystemTimeResult.second != expectedLeapSystemTime ||
		cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToSystemTime leap-day, milliseconds, or sub-millisecond truncation mismatch\n");
		return 97;
	}
	const uint16_t sentinelSystemTime[8] = {0x1111u, 0x2222u, 0x3333u, 0x4444u,
		0x5555u, 0x6666u, 0x7777u, 0x8888u};
	const auto highBitFileTime = invokeFileTimeToSystemTime(0x8000000000000000ull,
		0x00100380u, 0x001003a0u, sentinelSystemTime);
	const std::array<uint16_t, 8> expectedSentinelSystemTime{0x1111u, 0x2222u, 0x3333u,
		0x4444u, 0x5555u, 0x6666u, 0x7777u, 0x8888u};
	if (highBitFileTime.first != 0u || highBitFileTime.second != expectedSentinelSystemTime ||
		br::runtime::currentWin32LastError() != 87u || cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToSystemTime accepted high-bit FILETIME or changed output/ABI\n");
		return 98;
	}
	guest::store<uint16_t>(0x001003a0u, 0xaaaa);
	guest::store<uint32_t>(0x00100124u, 0u);
	guest::store<uint32_t>(0x00100128u, 0x001003a0u);
	cpu.r[ESP] = 0x00100120u;
	call_target(&cpu, fileTimeToSystemTimeImport.guestAddress);
	if (cpu.r[EAX] != 0u || guest::load<uint16_t>(0x001003a0u) != 0xaaaau ||
		br::runtime::currentWin32LastError() != 87u || cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToSystemTime accepted null FILETIME or changed output/ABI\n");
		return 99;
	}
	const auto setTestTimezone = [](const char* value) {
#if defined(_WIN32)
	_putenv_s("TZ", value ? value : "");
		_tzset();
#else
		if (value) setenv("TZ", value, 1);
		else unsetenv("TZ");
		tzset();
#endif
	};
	const char* previousTimezoneValue = std::getenv("TZ");
	const bool hadPreviousTimezone = previousTimezoneValue != nullptr;
	const std::string previousTimezone = previousTimezoneValue ? previousTimezoneValue : "";
	const auto invokeFileTimeToLocal = [&cpu](uint32_t inputAddress, uint32_t outputAddress,
		uint64_t inputValue, uint64_t initialOutput) {
		constexpr uint32_t stack = 0x00100120u;
		constexpr uint32_t returnAddress = 0x00abcdefu;
		guest::store<uint64_t>(inputAddress, inputValue);
		if (outputAddress != inputAddress) guest::store<uint64_t>(outputAddress, initialOutput);
		guest::store<uint32_t>(stack, returnAddress);
		guest::store<uint32_t>(stack + 4u, inputAddress);
		guest::store<uint32_t>(stack + 8u, outputAddress);
		cpu.r[ESP] = stack;
		call_target(&cpu, fileTimeToLocalFileTimeImport.guestAddress);
		return std::pair<uint32_t, uint64_t>{cpu.r[EAX], guest::load<uint64_t>(outputAddress)};
	};
	setTestTimezone("UTC0");
	constexpr uint64_t localConversionInput = 125963012967890123ull;
	const auto utcLocalConversion = invokeFileTimeToLocal(0x001002a0u, 0x001002b0u,
		localConversionInput, 0x1122334455667788ull);
	if (utcLocalConversion.first != 1u || utcLocalConversion.second != localConversionInput ||
		cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToLocalFileTime UTC conversion, 100-ns fraction, or stdcall ABI mismatch\n");
		return 91;
	}
	setTestTimezone("CST-8");
	const auto eastEightConversion = invokeFileTimeToLocal(0x001002a0u, 0x001002b0u,
		localConversionInput, 0x1122334455667788ull);
	if (eastEightConversion.first != 1u || eastEightConversion.second != localConversionInput + 288000000000ull ||
		cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToLocalFileTime local UTC+8 bias or 100-ns preservation mismatch\n");
		return 92;
	}
	const auto aliasedFileTime = invokeFileTimeToLocal(0x001002a0u, 0x001002a0u,
		localConversionInput, 0x1122334455667788ull);
	if (aliasedFileTime.first != 0u || aliasedFileTime.second != localConversionInput ||
		br::runtime::currentWin32LastError() != 87u || cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToLocalFileTime accepted aliased FILETIME pointers or changed ABI\n");
		return 93;
	}
	guest::store<uint32_t>(0x00100124u, 0u);
	guest::store<uint32_t>(0x00100128u, 0x001002b0u);
	guest::store<uint64_t>(0x001002b0u, 0x1122334455667788ull);
	cpu.r[ESP] = 0x00100120u;
	call_target(&cpu, fileTimeToLocalFileTimeImport.guestAddress);
	if (cpu.r[EAX] != 0u || guest::load<uint64_t>(0x001002b0u) != 0x1122334455667788ull ||
		br::runtime::currentWin32LastError() != 87u || cpu.r[ESP] != 0x0010012cu) {
		std::fprintf(stderr, "FileTimeToLocalFileTime accepted a null pointer or modified output/ABI\n");
		return 94;
	}
	setTestTimezone(hadPreviousTimezone ? previousTimezone.c_str() : nullptr);

	// Verify Win32 manual-reset event state, timeout status, invalid handles,
	// stack cleanup and recursive critical-section calls through call_target.
	const auto invoke = [&cpu](uint32_t target, std::initializer_list<uint32_t> args) {
		constexpr uint32_t stack = 0x00100300u;
		cpu.r[ESP] = stack;
		cpu.r[EAX] = 0;
		guest::store<uint32_t>(stack, 0x00abcdefu);
		uint32_t index = 0;
		for (uint32_t value : args) guest::store<uint32_t>(stack + 4u + 4u * index++, value);
		call_target(&cpu, target);
		return cpu.r[EAX];
	};
	// The game calls SleepEx(0, TRUE) from its active loop. It must yield, return
	// WAIT_TIMEOUT (zero) when no guest APC is pending, and clean its stdcall args.
	if (invoke(sleepExImport.guestAddress, {0u, 1u}) != 0u || cpu.r[ESP] != 0x0010030cu) {
		std::fprintf(stderr, "SleepEx(0, TRUE) return value or stdcall cleanup mismatch\n");
		return 95;
	}

	// Guest writes are redirected into a separate E:-workspace overlay.
	const char* fixtureEnv = std::getenv("BR_SMOKE_GAME_ROOT");
	fixtureRoot = fixtureEnv ? std::filesystem::path(fixtureEnv) :
		std::filesystem::path("E:/BattleRealmsRecomp/tmp/win32_file_fixture");
	std::filesystem::create_directories(fixtureRoot / "Levels");
	{
		std::ofstream fixture(fixtureRoot / "Levels" / "fixture.dat", std::ios::binary | std::ios::trunc);
		fixture << "first-second";
		if (!fixture) {
			std::fprintf(stderr, "could not create read-only file fixture on E:\\n");
			return 31;
		}
	}
	br::runtime::configureWin32FileOpener(openFixtureFile);
	br::runtime::configureWin32FileAttributesQuery(getFixtureFileAttributes);
	br::runtime::configureWin32FileDirectoryCreator(createFixtureDirectory);
	constexpr uint32_t pathAddress = 0x00102100u;
	constexpr uint32_t readBufferAddress = 0x00102200u;
	constexpr uint32_t bytesReadAddress = 0x00102240u;
	constexpr uint32_t highSizeAddress = 0x00102244u;
	constexpr uint32_t currentDirectoryBuffer = 0x00102300u;
	const std::string gameRoot = "D:\\Games\\Battle Realms Complete";
	if (invoke(getCurrentDirectoryImport.guestAddress, {4u, currentDirectoryBuffer}) !=
		gameRoot.size() + 1u ||
		invoke(getCurrentDirectoryImport.guestAddress, {128u, currentDirectoryBuffer}) != gameRoot.size() ||
		std::string(reinterpret_cast<const char*>(guest::host(currentDirectoryBuffer)), gameRoot.size()) != gameRoot) {
		std::fprintf(stderr, "GetCurrentDirectoryA root/buffer sizing mismatch\n");
		return 32;
	}
	const std::string levelsPath = "D:\\Games\\Battle Realms Complete\\Levels";
	for (size_t i = 0; i < levelsPath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(levelsPath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(levelsPath.size()), 0u);
	if (invoke(setCurrentDirectoryImport.guestAddress, {pathAddress}) != 1u ||
		invoke(getCurrentDirectoryImport.guestAddress, {128u, currentDirectoryBuffer}) != levelsPath.size() ||
		std::string(reinterpret_cast<const char*>(guest::host(currentDirectoryBuffer)), levelsPath.size()) != levelsPath) {
		std::fprintf(stderr, "Set/GetCurrentDirectoryA path update mismatch\n");
		return 33;
	}
	const std::string newDirectoryPath = gameRoot + "\\Levels\\created-by-createdirectoryexa";
	std::error_code directoryTestCleanup;
	std::filesystem::remove_all(fixtureRoot / "app-private" / "Levels" / "created-by-createdirectoryexa", directoryTestCleanup);
	std::filesystem::remove_all(fixtureRoot / "app-private" / "Levels" / "missing-parent-for-createdirectoryexa", directoryTestCleanup);
	std::filesystem::remove_all(fixtureRoot / "Levels" / "missing-parent-for-createdirectoryexa", directoryTestCleanup);
	writeGuestString(pathAddress - 64u, levelsPath);
	writeGuestString(pathAddress, newDirectoryPath);
	if (invoke(createDirectoryExImport.guestAddress, {pathAddress - 64u, pathAddress, 0u}) != 1u ||
		invoke(getFileAttributesImport.guestAddress, {pathAddress}) != 0x10u) {
		std::fprintf(stderr, "CreateDirectoryExA did not create the destination directory in the writable overlay\n");
		return 80;
	}
	if (invoke(createDirectoryExImport.guestAddress, {pathAddress - 64u, pathAddress, 0u}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 183u) {
		std::fprintf(stderr, "CreateDirectoryExA existing-destination error mismatch\n");
		return 81;
	}
	writeGuestString(pathAddress - 64u, levelsPath);
	writeGuestString(pathAddress, gameRoot + "\\Levels\\missing-parent-for-createdirectoryexa\\child");
	const uint32_t missingParentResult = invoke(createDirectoryExImport.guestAddress,
		{pathAddress - 64u, pathAddress, 0u});
	const uint32_t missingParentError = invoke(getLastErrorImport.guestAddress, {});
	if (missingParentResult != 0u || missingParentError != 3u) {
		std::fprintf(stderr, "CreateDirectoryExA missing-parent error mismatch: result=%u last-error=%u\n",
			missingParentResult, missingParentError);
		return 82;
	}
	writeGuestString(pathAddress - 64u, "fixture.dat");
	writeGuestString(pathAddress, newDirectoryPath + "-invalid-template");
	if (invoke(createDirectoryExImport.guestAddress, {pathAddress - 64u, pathAddress, 0u}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 5u) {
		std::fprintf(stderr, "CreateDirectoryExA rejected neither a non-directory template nor its error code\n");
		return 83;
	}
	writeGuestString(pathAddress - 64u, levelsPath);
	writeGuestString(pathAddress, gameRoot + "\\Levels\\created-with-security-attributes");
	guest::store<uint32_t>(0x00102400u, 12u);
	guest::store<uint32_t>(0x00102404u, 0u);
	guest::store<uint32_t>(0x00102408u, 0u);
	if (invoke(createDirectoryExImport.guestAddress,
		{pathAddress - 64u, pathAddress, 0x00102400u}) != 1u) {
		std::fprintf(stderr, "CreateDirectoryExA rejected a valid guest SECURITY_ATTRIBUTES structure\n");
		return 84;
	}
	guest::store<uint32_t>(0x00102400u, 8u);
	writeGuestString(pathAddress, gameRoot + "\\Levels\\invalid-security-attributes");
	if (invoke(createDirectoryExImport.guestAddress,
		{pathAddress - 64u, pathAddress, 0x00102400u}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 87u) {
		std::fprintf(stderr, "CreateDirectoryExA accepted an invalid SECURITY_ATTRIBUTES length\n");
		return 85;
	}
	const std::string relativeFilePath = "fixture.dat";
	for (size_t i = 0; i < relativeFilePath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(relativeFilePath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(relativeFilePath.size()), 0u);
	if (invoke(getFileAttributesImport.guestAddress, {pathAddress}) != 1u) {
		std::fprintf(stderr, "relative path did not use current directory\n");
		return 34;
	}
	for (size_t i = 0; i < gameRoot.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(gameRoot[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(gameRoot.size()), 0u);
	const std::string modulePath = gameRoot + "\\Battle_Realms_F.exe";
	if (invoke(getModuleFileNameImport.guestAddress,
		{0u, currentDirectoryBuffer, 4u}) != 4u ||
		invoke(getLastErrorImport.guestAddress, {}) != 122u) {
		std::fprintf(stderr, "GetModuleFileNameA truncation behavior mismatch\n");
		return 35;
	}
	if (invoke(getModuleFileNameImport.guestAddress,
		{0u, currentDirectoryBuffer, 128u}) != modulePath.size() ||
		std::string(reinterpret_cast<const char*>(guest::host(currentDirectoryBuffer)), modulePath.size()) != modulePath) {
		std::fprintf(stderr, "GetModuleFileNameA main-module path mismatch\n");
		return 36;
	}
	const std::string gamePath = "D:\\Games\\Battle Realms Complete\\Levels\\fixture.dat";
	for (size_t i = 0; i < gamePath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(gamePath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(gamePath.size()), 0u);
	if (invoke(getFileAttributesImport.guestAddress, {pathAddress}) != 1u) {
		std::fprintf(stderr, "source GetFileAttributesA mismatch\n");
		return 32;
	}
	for (size_t i = 0; i < levelsPath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(levelsPath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(levelsPath.size()), 0u);
	if (invoke(getFileAttributesImport.guestAddress, {pathAddress}) != 0x10u) {
		std::fprintf(stderr, "directory GetFileAttributesA mismatch\n");
		return 33;
	}
	const std::string missingPath = "D:\\Games\\Battle Realms Complete\\Levels\\missing.dat";
	for (size_t i = 0; i < missingPath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(missingPath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(missingPath.size()), 0u);
	if (invoke(getFileAttributesImport.guestAddress, {pathAddress}) != 0xffffffffu ||
		invoke(getLastErrorImport.guestAddress, {}) != 2u) {
		std::fprintf(stderr, "missing-file GetFileAttributesA mismatch\n");
		return 34;
	}
	for (size_t i = 0; i < gamePath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(gamePath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(gamePath.size()), 0u);
	const uint32_t fileHandle = invoke(createFileImport.guestAddress,
		{pathAddress, 0x80000000u, 7u, 0u, 3u, 0u, 0u});
	if (fileHandle == 0xffffffffu || fileHandle < 0x6f000000u ||
		observedFixturePath != "Levels/fixture.dat" || fixtureOpenCalls != 1u ||
		invoke(getFileTypeImport.guestAddress, {fileHandle}) != 1u ||
		invoke(getFileSizeImport.guestAddress, {fileHandle, highSizeAddress}) != 12u ||
		guest::load<uint32_t>(highSizeAddress) != 0u) {
		std::fprintf(stderr, "CreateFileA path mapping or file metadata mismatch\n");
		return 32;
	}
	guest::store<uint32_t>(bytesReadAddress, 0xffffffffu);
	if (invoke(readFileImport.guestAddress,
		{fileHandle, readBufferAddress, 5u, bytesReadAddress, 0u}) != 1u ||
		guest::load<uint32_t>(bytesReadAddress) != 5u ||
		std::string(reinterpret_cast<const char*>(guest::host(readBufferAddress)), 5u) != "first") {
		std::fprintf(stderr, "ReadFile content/byte count mismatch\n");
		return 33;
	}
	if (invoke(setFilePointerImport.guestAddress, {fileHandle, 6u, 0u, 0u}) != 6u ||
		invoke(readFileImport.guestAddress,
			{fileHandle, readBufferAddress, 6u, bytesReadAddress, 0u}) != 1u ||
		guest::load<uint32_t>(bytesReadAddress) != 6u ||
		std::string(reinterpret_cast<const char*>(guest::host(readBufferAddress)), 6u) != "second") {
		std::fprintf(stderr, "SetFilePointer/read-after-seek mismatch\n");
		return 34;
	}
	if (invoke(readFileImport.guestAddress,
		{fileHandle, readBufferAddress, 4u, bytesReadAddress, 0u}) != 1u ||
		guest::load<uint32_t>(bytesReadAddress) != 0u) {
		std::fprintf(stderr, "ReadFile EOF behavior mismatch\n");
		return 35;
	}
	const uint32_t sourceWriteHandle = invoke(createFileImport.guestAddress,
		{pathAddress, 0xc0000000u, 7u, 0u, 3u, 0u, 0u});
	const char patchBytes[] = "PATCH";
	for (uint32_t i = 0; i < 5u; ++i)
		guest::store<uint8_t>(readBufferAddress + i, static_cast<uint8_t>(patchBytes[i]));
	if (sourceWriteHandle == 0xffffffffu ||
		invoke(setFilePointerImport.guestAddress, {sourceWriteHandle, 0u, 0u, 0u}) != 0u ||
		invoke(writeFileImport.guestAddress,
			{sourceWriteHandle, readBufferAddress, 5u, bytesReadAddress, 0u}) != 1u ||
		guest::load<uint32_t>(bytesReadAddress) != 5u ||
		invoke(flushFileBuffersImport.guestAddress, {sourceWriteHandle}) != 1u ||
		invoke(closeImport.guestAddress, {sourceWriteHandle}) != 1u) {
		std::fprintf(stderr, "copy-on-write source edit failed\n");
		return 36;
	}
	{
		std::ifstream original(fixtureRoot / "Levels" / "fixture.dat", std::ios::binary);
		std::string originalText((std::istreambuf_iterator<char>(original)), {});
		std::ifstream overlay(fixtureRoot / "app-private" / "Levels" / "fixture.dat", std::ios::binary);
		std::string overlayText((std::istreambuf_iterator<char>(overlay)), {});
		if (originalText != "first-second" || overlayText != "PATCH-second") {
			std::fprintf(stderr, "copy-on-write changed source or missed overlay write\n");
			return 37;
		}
	}
	if (invoke(getFileAttributesImport.guestAddress, {pathAddress}) != 0x20u) {
		std::fprintf(stderr, "overlay GetFileAttributesA precedence mismatch\n");
		return 38;
	}
	const uint32_t overlayReadHandle = invoke(createFileImport.guestAddress,
		{pathAddress, 0x80000000u, 7u, 0u, 3u, 0u, 0u});
	if (overlayReadHandle == 0xffffffffu ||
		invoke(readFileImport.guestAddress,
			{overlayReadHandle, readBufferAddress, 12u, bytesReadAddress, 0u}) != 1u ||
		std::string(reinterpret_cast<const char*>(guest::host(readBufferAddress)), 12u) != "PATCH-second" ||
		invoke(closeImport.guestAddress, {overlayReadHandle}) != 1u) {
		std::fprintf(stderr, "read did not prefer the app-private overlay\n");
		return 38;
	}
	const uint32_t savePathAddress = pathAddress;
	const std::string savePath = "D:\\Games\\Battle Realms Complete\\userdata\\save.dat";
	for (size_t i = 0; i < savePath.size(); ++i)
		guest::store<uint8_t>(savePathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(savePath[i]));
	guest::store<uint8_t>(savePathAddress + static_cast<uint32_t>(savePath.size()), 0u);
	const uint32_t saveHandle = invoke(createFileImport.guestAddress,
		{savePathAddress, 0x40000000u, 7u, 0u, 2u, 0u, 0u});
	const char saveBytes[] = "save-data";
	for (uint32_t i = 0; i < 9u; ++i)
		guest::store<uint8_t>(readBufferAddress + i, static_cast<uint8_t>(saveBytes[i]));
	if (saveHandle == 0xffffffffu ||
		invoke(writeFileImport.guestAddress,
			{saveHandle, readBufferAddress, 9u, bytesReadAddress, 0u}) != 1u ||
		guest::load<uint32_t>(bytesReadAddress) != 9u ||
		invoke(setFilePointerImport.guestAddress, {saveHandle, 4u, 0u, 0u}) != 4u ||
		invoke(setEndOfFileImport.guestAddress, {saveHandle}) != 1u ||
		invoke(getFileSizeImport.guestAddress, {saveHandle, highSizeAddress}) != 4u ||
		invoke(flushFileBuffersImport.guestAddress, {saveHandle}) != 1u ||
		invoke(closeImport.guestAddress, {saveHandle}) != 1u ||
		std::filesystem::exists(fixtureRoot / "userdata" / "save.dat")) {
		std::fprintf(stderr, "private save creation/truncation/flush mismatch\n");
		return 39;
	}
	{
		std::ifstream save(fixtureRoot / "app-private" / "userdata" / "save.dat", std::ios::binary);
		std::string saveText((std::istreambuf_iterator<char>(save)), {});
		if (saveText != "save") {
			std::fprintf(stderr, "private save bytes mismatch\n");
			return 40;
		}
	}
	if (invoke(createFileImport.guestAddress,
		{pathAddress, 0x40000000u, 7u, 0u, 1u, 0u, 0u}) != 0xffffffffu ||
		invoke(getLastErrorImport.guestAddress, {}) != 80u) {
		std::fprintf(stderr, "CREATE_NEW collision behavior mismatch\n");
		return 41;
	}
	const std::string traversalPath = "D:\\Games\\Battle Realms Complete\\..\\outside.dat";
	for (size_t i = 0; i < traversalPath.size(); ++i)
		guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(i), static_cast<uint8_t>(traversalPath[i]));
	guest::store<uint8_t>(pathAddress + static_cast<uint32_t>(traversalPath.size()), 0u);
	if (invoke(createFileImport.guestAddress,
		{pathAddress, 0x80000000u, 7u, 0u, 3u, 0u, 0u}) != 0xffffffffu ||
		invoke(getLastErrorImport.guestAddress, {}) != 87u ||
		invoke(closeImport.guestAddress, {fileHandle}) != 1u ||
		invoke(closeImport.guestAddress, {fileHandle}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 6u) {
		std::fprintf(stderr, "file path traversal rejection or CloseHandle mismatch\n");
		return 37;
	}
	std::filesystem::remove_all(fixtureRoot);
	const uint32_t eventHandle = invoke(createEventImport.guestAddress, {0u, 1u, 0u, 0u});
	if (!eventHandle || cpu.r[ESP] != 0x00100314u ||
		invoke(waitImport.guestAddress, {eventHandle, 0u}) != 0x102u || cpu.r[ESP] != 0x0010030cu ||
		invoke(setEventImport.guestAddress, {eventHandle}) != 1u || cpu.r[ESP] != 0x00100308u ||
		invoke(waitImport.guestAddress, {eventHandle, 0u}) != 0u ||
		invoke(waitImport.guestAddress, {eventHandle, 0u}) != 0u ||
		invoke(resetEventImport.guestAddress, {eventHandle}) != 1u ||
		invoke(waitImport.guestAddress, {eventHandle, 0u}) != 0x102u ||
		invoke(closeImport.guestAddress, {eventHandle}) != 1u ||
		invoke(waitImport.guestAddress, {eventHandle, 0u}) != 0xffffffffu ||
		invoke(getLastErrorImport.guestAddress, {}) != 6u) {
		std::fprintf(stderr, "event synchronization behavior mismatch\n");
		return 8;
	}
	const uint32_t autoResetEvent = invoke(createEventImport.guestAddress, {0u, 0u, 0u, 0u});
	std::atomic<uint32_t> workerResult{0xffffffffu};
	std::atomic<uint32_t> workerStackAfter{0};
	std::thread waiter([&] {
		CPU worker{};
		worker.r[ESP] = 0x00100500u;
		guest::store<uint32_t>(worker.r[ESP], 0x00abcdefu);
		guest::store<uint32_t>(worker.r[ESP] + 4u, autoResetEvent);
		guest::store<uint32_t>(worker.r[ESP] + 8u, 2000u);
		call_target(&worker, waitImport.guestAddress);
		workerResult.store(worker.r[EAX], std::memory_order_release);
		workerStackAfter.store(worker.r[ESP], std::memory_order_release);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const uint32_t signalResult = invoke(setEventImport.guestAddress, {autoResetEvent});
	waiter.join();
	if (!autoResetEvent || signalResult != 1u ||
		workerResult.load(std::memory_order_acquire) != 0u ||
		workerStackAfter.load(std::memory_order_acquire) != 0x0010050cu ||
		invoke(waitImport.guestAddress, {autoResetEvent, 0u}) != 0x102u ||
		invoke(closeImport.guestAddress, {autoResetEvent}) != 1u) {
		std::fprintf(stderr, "cross-thread auto-reset event behavior mismatch\n");
		return 10;
	}
	constexpr uint32_t criticalSection = 0x00100400u;
	if (invoke(initCsImport.guestAddress, {criticalSection}) != 0u ||
		invoke(enterCsImport.guestAddress, {criticalSection}) != 0u ||
		invoke(enterCsImport.guestAddress, {criticalSection}) != 0u ||
		invoke(tryEnterCsImport.guestAddress, {criticalSection}) != 1u ||
		invoke(leaveCsImport.guestAddress, {criticalSection}) != 0u ||
		invoke(leaveCsImport.guestAddress, {criticalSection}) != 0u ||
		invoke(leaveCsImport.guestAddress, {criticalSection}) != 0u) {
		std::fprintf(stderr, "critical-section behavior mismatch\n");
		return 9;
	}
	constexpr uint32_t contendedCriticalSection = 0x00100404u;
	(void)invoke(initCsImport.guestAddress, {contendedCriticalSection});
	(void)invoke(enterCsImport.guestAddress, {contendedCriticalSection});
	std::atomic<bool> criticalWorkerStarted{false};
	std::atomic<bool> criticalWorkerFinished{false};
	std::thread criticalWorker([&] {
		CPU worker{};
		worker.r[ESP] = 0x00100600u;
		guest::store<uint32_t>(worker.r[ESP], 0x00abcdefu);
		guest::store<uint32_t>(worker.r[ESP] + 4u, contendedCriticalSection);
		criticalWorkerStarted.store(true, std::memory_order_release);
		call_target(&worker, enterCsImport.guestAddress);
		worker.r[ESP] = 0x00100620u;
		guest::store<uint32_t>(worker.r[ESP], 0x00abcdefu);
		guest::store<uint32_t>(worker.r[ESP] + 4u, contendedCriticalSection);
		call_target(&worker, leaveCsImport.guestAddress);
		criticalWorkerFinished.store(true, std::memory_order_release);
	});
	while (!criticalWorkerStarted.load(std::memory_order_acquire))
		std::this_thread::yield();
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	const bool remainedBlocked = !criticalWorkerFinished.load(std::memory_order_acquire);
	(void)invoke(leaveCsImport.guestAddress, {contendedCriticalSection});
	criticalWorker.join();
	if (!remainedBlocked || !criticalWorkerFinished.load(std::memory_order_acquire)) {
		std::fprintf(stderr, "cross-thread critical-section behavior mismatch\n");
		return 11;
	}
	constexpr uint32_t interlockedValue = 0x00100700u;
	guest::store<int32_t>(interlockedValue, 10);
	if (invoke(interlockedIncrementImport.guestAddress, {interlockedValue}) != 11u ||
		cpu.r[ESP] != 0x00100308u ||
		invoke(interlockedDecrementImport.guestAddress, {interlockedValue}) != 10u ||
		invoke(interlockedExchangeImport.guestAddress, {interlockedValue, 55u}) != 10u ||
		guest::load<int32_t>(interlockedValue) != 55) {
		std::fprintf(stderr, "interlocked operation behavior mismatch\n");
		return 12;
	}
	constexpr uint32_t workerCount = 4;
	constexpr uint32_t incrementsPerWorker = 1000;
	std::vector<std::thread> atomicWorkers;
	for (uint32_t i = 0; i < workerCount; ++i) {
		atomicWorkers.emplace_back([i] {
			CPU worker{};
			const uint32_t stack = 0x00101000u + i * 0x100u;
			guest::store<uint32_t>(stack, 0x00abcdefu);
			guest::store<uint32_t>(stack + 4u, interlockedValue);
			for (uint32_t n = 0; n < incrementsPerWorker; ++n) {
				worker.r[ESP] = stack;
				call_target(&worker, interlockedIncrementImport.guestAddress);
			}
		});
	}
	for (auto& worker : atomicWorkers) worker.join();
	if (guest::load<int32_t>(interlockedValue) !=
		55 + static_cast<int32_t>(workerCount * incrementsPerWorker)) {
		std::fprintf(stderr, "cross-thread interlocked increment mismatch\n");
		return 13;
	}
	const uint32_t processHeap = invoke(getProcessHeapImport.guestAddress, {});
	const uint32_t createdHeap = invoke(heapCreateImport.guestAddress, {0u, 0u, 0u});
	const uint32_t heapBlock = invoke(heapAllocImport.guestAddress, {processHeap, 8u, 13u});
	bool heapInitiallyZero = heapBlock >= 0x30000000u && heapBlock < 0x60000000u;
	for (uint32_t i = 0; i < 13u && heapInitiallyZero; ++i)
		heapInitiallyZero = guest::load<uint8_t>(heapBlock + i) == 0;
	if (processHeap != 0x00050000u || createdHeap != processHeap || !heapInitiallyZero ||
		cpu.r[ESP] != 0x00100310u ||
		invoke(heapSizeImport.guestAddress, {processHeap, 0u, heapBlock}) < 13u) {
		std::fprintf(stderr, "guest heap allocation/zero/size mismatch: heap=0x%08x created=0x%08x block=0x%08x zero=%u ESP=0x%08x size=0x%08x\n",
			processHeap, createdHeap, heapBlock, heapInitiallyZero ? 1u : 0u, cpu.r[ESP],
			invoke(heapSizeImport.guestAddress, {processHeap, 0u, heapBlock}));
		return 14;
	}
	guest::store<uint32_t>(heapBlock, 0x11223344u);
	const uint32_t grownBlock = invoke(heapReAllocImport.guestAddress,
		{processHeap, 8u, heapBlock, 40u});
	bool grownTailZero = grownBlock != 0 && guest::load<uint32_t>(grownBlock) == 0x11223344u;
	for (uint32_t i = 13; i < 40u && grownTailZero; ++i)
		grownTailZero = guest::load<uint8_t>(grownBlock + i) == 0;
	if (!grownTailZero || invoke(heapSizeImport.guestAddress, {processHeap, 0u, heapBlock}) != 0xffffffffu ||
		invoke(heapFreeImport.guestAddress, {processHeap, 0u, grownBlock}) != 1u ||
		invoke(heapFreeImport.guestAddress, {processHeap, 0u, grownBlock}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 87u ||
		invoke(heapDestroyImport.guestAddress, {createdHeap}) != 1u) {
		std::fprintf(stderr, "guest heap reallocation/free behavior mismatch\n");
		return 15;
	}
	const uint32_t globalBlock = invoke(globalAllocImport.guestAddress, {0x40u, 24u});
	bool globalInitiallyZero = globalBlock != 0 &&
		invoke(globalLockImport.guestAddress, {globalBlock}) == globalBlock &&
		invoke(globalLockImport.guestAddress, {globalBlock}) == globalBlock &&
		invoke(globalSizeImport.guestAddress, {globalBlock}) >= 24u;
	for (uint32_t i = 0; i < 24u && globalInitiallyZero; ++i)
		globalInitiallyZero = guest::load<uint8_t>(globalBlock + i) == 0;
	if (!globalInitiallyZero || invoke(globalUnlockImport.guestAddress, {globalBlock}) != 1u ||
		invoke(globalUnlockImport.guestAddress, {globalBlock}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 0u ||
		invoke(globalFreeImport.guestAddress, {globalBlock}) != 0u ||
		invoke(globalSizeImport.guestAddress, {globalBlock}) != 0xffffffffu) {
		std::fprintf(stderr, "global-memory API behavior mismatch\n");
		return 16;
	}
	constexpr uint32_t heapWorkers = 4;
	constexpr uint32_t heapOpsPerWorker = 500;
	std::atomic<bool> heapWorkersOk{true};
	std::vector<std::thread> concurrentHeapWorkers;
	for (uint32_t i = 0; i < heapWorkers; ++i) {
		concurrentHeapWorkers.emplace_back([i, processHeap, &heapWorkersOk] {
			CPU worker{};
			const uint32_t stack = 0x00104000u + i * 0x100u;
			auto call = [&](uint32_t target, std::initializer_list<uint32_t> args) {
				worker.r[ESP] = stack;
				guest::store<uint32_t>(stack, 0x00abcdefu);
				uint32_t index = 0;
				for (uint32_t value : args) guest::store<uint32_t>(stack + 4u + 4u * index++, value);
				call_target(&worker, target);
				return worker.r[EAX];
			};
			for (uint32_t n = 0; n < heapOpsPerWorker; ++n) {
				const uint32_t block = call(heapAllocImport.guestAddress, {processHeap, 0u, 32u});
				if (!block || call(heapFreeImport.guestAddress, {processHeap, 0u, block}) != 1u) {
					heapWorkersOk.store(false, std::memory_order_release);
					return;
				}
			}
		});
	}
	for (auto& worker : concurrentHeapWorkers) worker.join();
	if (!heapWorkersOk.load(std::memory_order_acquire)) {
		std::fprintf(stderr, "concurrent guest heap operations failed\n");
		return 17;
	}
	const uint32_t virtualBlock = invoke(virtualAllocImport.guestAddress,
		{0u, 0x5000u, 0x3000u, 0x04u}); // MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE
	if (virtualBlock < 0x60000000u || virtualBlock >= 0x6f000000u ||
		(virtualBlock & 0xffffu) != 0 || cpu.r[ESP] != 0x00100314u) {
		std::fprintf(stderr, "VirtualAlloc reserve/commit mismatch: base=0x%08x ESP=0x%08x\n",
			virtualBlock, cpu.r[ESP]);
		return 18;
	}
	guest::store<uint8_t>(virtualBlock + 0x1000u, 0xa5u);
	if (invoke(virtualFreeImport.guestAddress, {virtualBlock + 0x1000u, 0x1000u, 0x4000u}) != 1u ||
		cpu.r[ESP] != 0x00100310u) { // MEM_DECOMMIT
		std::fprintf(stderr, "VirtualFree decommit mismatch\n");
		return 19;
	}
	const uint32_t recommitted = invoke(virtualAllocImport.guestAddress,
		{virtualBlock + 0x1000u, 0x1000u, 0x1000u, 0x04u}); // MEM_COMMIT
	if (recommitted < virtualBlock || recommitted > virtualBlock + 0x1000u ||
		guest::load<uint8_t>(virtualBlock + 0x1000u) != 0u) {
		std::fprintf(stderr, "VirtualAlloc recommit/zero-fill mismatch: result=0x%08x\n", recommitted);
		return 20;
	}
	guest::store<uint8_t>(virtualBlock + 0x2000u, 0x5au);
	if (invoke(virtualFreeImport.guestAddress, {virtualBlock, 0u, 0x8000u}) != 1u ||
		invoke(virtualAllocImport.guestAddress, {0u, 0x5000u, 0x3000u, 0x04u}) != virtualBlock ||
		guest::load<uint8_t>(virtualBlock + 0x2000u) != 0u ||
		invoke(virtualFreeImport.guestAddress, {virtualBlock, 0u, 0x8000u}) != 1u) {
		std::fprintf(stderr, "VirtualFree release/reuse mismatch\n");
		return 21;
	}
	const uint32_t reservedOnly = invoke(virtualAllocImport.guestAddress,
		{0u, 0x1000u, 0x2000u, 0x04u}); // MEM_RESERVE
	const uint32_t committedOnly = invoke(virtualAllocImport.guestAddress,
		{reservedOnly + 0x1000u, 0x1000u, 0x1000u, 0x04u}); // MEM_COMMIT in that reserve
	if (!reservedOnly || committedOnly < reservedOnly || committedOnly > reservedOnly + 0x1000u ||
		invoke(virtualFreeImport.guestAddress, {reservedOnly, 0u, 0x8000u}) != 1u ||
		invoke(virtualAllocImport.guestAddress, {0u, 0x1000u, 0x1000u, 0x04u}) != 0u) {
		std::fprintf(stderr, "VirtualAlloc reserve-only/invalid-commit mismatch\n");
		return 22;
	}
	const uint32_t tlsIndex = invoke(tlsAllocImport.guestAddress, {});
	(void)invoke(setLastErrorImport.guestAddress, {0x4567u});
	const uint32_t tlsSetResult = invoke(tlsSetValueImport.guestAddress, {tlsIndex, 0x11223344u});
	const uint32_t lastErrorAfterTlsSet = invoke(getLastErrorImport.guestAddress, {});
	const uint32_t tlsMainValue = invoke(tlsGetValueImport.guestAddress, {tlsIndex});
	if (tlsIndex >= 64u || cpu.r[ESP] != 0x00100308u || tlsSetResult != 1u ||
		lastErrorAfterTlsSet != 0x4567u || tlsMainValue != 0x11223344u) {
		std::fprintf(stderr, "TLS allocation/current-thread value mismatch: index=%u ESP=0x%08x set=%u error=0x%08x value=0x%08x\n",
			tlsIndex, cpu.r[ESP], tlsSetResult, lastErrorAfterTlsSet, tlsMainValue);
		return 23;
	}
	(void)invoke(setLastErrorImport.guestAddress, {0x1234u});
	if (invoke(tlsGetValueImport.guestAddress, {tlsIndex}) != 0x11223344u ||
		invoke(getLastErrorImport.guestAddress, {}) != 0u) {
		std::fprintf(stderr, "TlsGetValue last-error clearing mismatch\n");
		return 24;
	}
	std::atomic<uint32_t> workerTlsValue{0xffffffffu};
	std::thread tlsWorker([&] {
		CPU worker{};
		const uint32_t stack = 0x00101800u;
		auto call = [&](uint32_t target, std::initializer_list<uint32_t> args) {
			worker.r[ESP] = stack;
			guest::store<uint32_t>(stack, 0x00abcdefu);
			uint32_t index = 0;
			for (uint32_t value : args) guest::store<uint32_t>(stack + 4u + 4u * index++, value);
			call_target(&worker, target);
			return worker.r[EAX];
		};
		const uint32_t initial = call(tlsGetValueImport.guestAddress, {tlsIndex});
		const uint32_t setResult = call(tlsSetValueImport.guestAddress, {tlsIndex, 0x55667788u});
		const uint32_t value = call(tlsGetValueImport.guestAddress, {tlsIndex});
		workerTlsValue.store(initial == 0u && setResult == 1u ? value : 0xffffffffu,
			std::memory_order_release);
	});
	tlsWorker.join();
	if (workerTlsValue.load(std::memory_order_acquire) != 0x55667788u ||
		invoke(tlsGetValueImport.guestAddress, {tlsIndex}) != 0x11223344u) {
		std::fprintf(stderr, "TLS per-thread isolation mismatch\n");
		return 25;
	}
	(void)invoke(setLastErrorImport.guestAddress, {0x4321u});
	if (invoke(tlsFreeImport.guestAddress, {tlsIndex}) != 1u ||
		invoke(getLastErrorImport.guestAddress, {}) != 0x4321u ||
		invoke(tlsFreeImport.guestAddress, {tlsIndex}) != 0u ||
		invoke(getLastErrorImport.guestAddress, {}) != 87u) {
		std::fprintf(stderr, "TLS free/last-error behavior mismatch\n");
		return 25;
	}
	const uint32_t reusedTlsIndex = invoke(tlsAllocImport.guestAddress, {});
	if (reusedTlsIndex != tlsIndex || invoke(tlsGetValueImport.guestAddress, {reusedTlsIndex}) != 0u) {
		std::fprintf(stderr, "TLS slot reuse retained stale thread value\n");
		return 26;
	}
	uint32_t remainingTlsSlots[63]{};
	for (uint32_t i = 0; i < 63u; ++i) {
		remainingTlsSlots[i] = invoke(tlsAllocImport.guestAddress, {});
		if (remainingTlsSlots[i] >= 64u) {
			std::fprintf(stderr, "TLS slot exhaustion occurred early\n");
			return 27;
		}
	}
	if (invoke(tlsAllocImport.guestAddress, {}) != 0xffffffffu ||
		invoke(getLastErrorImport.guestAddress, {}) != 8u) {
		std::fprintf(stderr, "TLS slot exhaustion behavior mismatch\n");
		return 28;
	}
	if (invoke(tlsFreeImport.guestAddress, {reusedTlsIndex}) != 1u) {
		std::fprintf(stderr, "TLS reused slot release failed\n");
		return 29;
	}
	for (const uint32_t slot : remainingTlsSlots) {
		if (invoke(tlsFreeImport.guestAddress, {slot}) != 1u) {
			std::fprintf(stderr, "TLS slot release failed\n");
			return 30;
		}
	}

	std::printf("lift dispatch ok: direct=%u indirect=%u fake_import=%u base_win32=%u sync_win32=%u memory_win32=%u file_api_handlers=%u atomic_workers=%u heap_workers=%u virtual_memory=%u tls_win32=%u ESP=0x%08x\n",
		1u, 1u, importedCalls, 9u, 14u, 21u, 12u, workerCount, heapWorkers, 7u, 4u, cpu.r[ESP]);
	return 0;
}
