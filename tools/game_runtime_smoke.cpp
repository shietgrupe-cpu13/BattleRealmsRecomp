#include "game_runtime.h"

#include "cpu.h"
#include "guest.h"
#include "ra3_guest_bridge.h"
#include "win32_base.h"
#include "win32_file.h"
#include "win32_memory.h"
#include "win32_sync.h"
#include "win32_startup.h"

#include <fstream>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <io.h>
#define BR_TEST_OPEN _open
#define BR_TEST_FILENO _fileno
#else
#include <unistd.h>
#define BR_TEST_OPEN ::open
#define BR_TEST_FILENO fileno
#endif

namespace {
void entrypointMarker(CPU*) {}
uint32_t callbackArguments[4]{};
std::vector<uint32_t> windowCallbackMessages;
uint32_t windowCallbackArguments[4]{};
uint32_t windowCreateArguments[4]{};
uint32_t windowDefProcStub = 0u;
struct WindowMessageRecord { uint32_t hwnd, message, wParam, lParam; };
std::vector<WindowMessageRecord> windowMessageRecords;
const char* testExecutablePath = nullptr;
bool testVirtualExecutableAvailable = true;
void callbackProbe(CPU* cpu)
{
	for (uint32_t i = 0; i < 4u; ++i)
		callbackArguments[i] = guest::load<uint32_t>(cpu->r[ESP] + 4u + i * 4u);
	cpu->r[EAX] = 0x12345678u;
	cpu->r[ESP] += 20u; // WndProc stdcall: four arguments and the return slot
}

void windowCreateProbe(CPU* cpu)
{
	for (uint32_t i = 0; i < 4u; ++i)
		windowCallbackArguments[i] = guest::load<uint32_t>(cpu->r[ESP] + 4u + i * 4u);
	windowCallbackMessages.push_back(windowCallbackArguments[1]);
	windowMessageRecords.push_back({windowCallbackArguments[0], windowCallbackArguments[1],
		windowCallbackArguments[2], windowCallbackArguments[3]});
	if (windowCallbackArguments[1] == 1u)
		std::memcpy(windowCreateArguments, windowCallbackArguments, sizeof(windowCreateArguments));
	cpu->r[EAX] = windowCallbackArguments[1] == 0x81u ? 1u : 0u;
	if (windowCallbackArguments[1] == 0x0047u && windowDefProcStub) {
		// Model an ordinary guest WndProc forwarding WM_WINDOWPOSCHANGED to
		// DefWindowProcA. Windows derives WM_MOVE/WM_SIZE from this message;
		// those are delivered synchronously back through the guest procedure.
		const uint32_t savedStack = cpu->r[ESP];
		constexpr uint32_t nestedStack = 0x7ff0e700u;
		cpu->r[ESP] = nestedStack;
		guest::store<uint32_t>(nestedStack, 0xffffffffu);
		for (uint32_t i = 0; i < 4u; ++i)
			guest::store<uint32_t>(nestedStack + 4u + i * 4u, windowCallbackArguments[i]);
		call_target(cpu, windowDefProcStub);
		cpu->r[ESP] = savedStack;
	}
	cpu->r[ESP] += 20u;
}

bool testFindQuery(const char* directory, const char* wildcard,
	std::vector<br::runtime::Win32FileFindEntry>& entries)
{
	if (std::string(directory) != "." || std::string(wildcard) != "protlog*.txt") return false;
	entries.push_back({"protlog1.txt", 1u, 37u, 1700000000000ull});
	entries.push_back({"other.txt", 1u, 9u, 1700000000000ull});
	entries.push_back({"protlog2.txt", 1u, 41u, 1700000000000ull});
	return true;
}

br::runtime::Win32FileOpenResult testFileOpener(
	const char* relativePath, uint32_t access, uint32_t disposition)
{
	if (!relativePath || access != 0x80000000u || disposition != 3u) return {-1, 2u};
#if defined(_WIN32)
	if (std::string(relativePath) == "Battle_Realms_F.exe") {
		if (!testVirtualExecutableAvailable || !testExecutablePath) return {-1, 2u};
		const int fd = BR_TEST_OPEN(testExecutablePath, _O_RDONLY | _O_BINARY);
		return {fd, fd < 0 ? 2u : 0u};
	}
	if (std::string(relativePath) != "startup_profile_test.ini") return {-1, 2u};
	const int fd = BR_TEST_OPEN("E:\\BattleRealmsRecomp\\analysis\\startup_profile_test.ini", _O_RDONLY | _O_BINARY);
#else
	if (std::string(relativePath) != "startup_profile_test.ini") return {-1, 2u};
	const int fd = BR_TEST_OPEN("analysis/startup_profile_test.ini", O_RDONLY);
#endif
	return {fd, fd < 0 ? 2u : 0u};
}

void writeGuestString(uint32_t address, const char* value)
{
	for (uint32_t i = 0;; ++i) {
		const uint8_t byte = static_cast<uint8_t>(value[i]);
		guest::store<uint8_t>(address + i, byte);
		if (!byte) break;
	}
}
}

// Host-only stand-in for the production table. The smoke verifies that the
// exact source PE entrypoint is present; Android links the full generated table.
extern const LiftedEntry lifted_table[] = {
	{0x00100000u, callbackProbe},
	{0x00100004u, windowCreateProbe},
	{0x006a2441u, entrypointMarker}
};
extern const unsigned lifted_count = 3;

int main(int argc, char** argv)
{
	if (argc != 2) {
		std::fprintf(stderr, "usage: game_runtime_smoke <read-only Battle_Realms_F.exe>\n");
		return 1;
	}
	std::ifstream stream(argv[1], std::ios::binary);
	if (!stream) {
		std::fprintf(stderr, "cannot open source executable\n");
		return 2;
	}
	std::vector<uint8_t> file{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
	testExecutablePath = argv[1];
	br::runtime::ImportDispatchRegistry handlers;
	if (!br::runtime::registerWin32BaseHandlers(handlers) ||
		!br::runtime::registerWin32StartupHandlers(handlers) ||
		!br::runtime::registerWin32SyncHandlers(handlers) ||
		!br::runtime::registerWin32MemoryHandlers(handlers) ||
		!br::runtime::registerWin32FileHandlers(handlers)) {
		std::fprintf(stderr, "cannot register current Win32 handler subset\n");
		return 3;
	}

	auto& addressSpace = br::runtime::sharedGuestAddressSpace();
	br::runtime::MainExecutableRuntime runtime;
	std::string error;
	if (!runtime.initialize(file, addressSpace, handlers, error)) {
		std::fprintf(stderr, "main PE runtime initialization failed: %s\n", error.c_str());
		return 4;
	}
	{
		std::ofstream profile("analysis/startup_profile_test.ini", std::ios::binary | std::ios::trunc);
		profile << "; focused GetPrivateProfileIntA test\r\n[Graphics]\r\nWidth=1280px\r\nWindowed = 1\r\n";
	}
	br::runtime::configureWin32FileOpener(testFileOpener);

	const auto iatStubFor = [&](const char* library, const char* name) {
		for (const auto& import : runtime.image().imports) {
			if (import.library != library || import.name != name) continue;
			const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
			if (!slot) return 0u;
			return uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
				(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
		}
		return 0u;
	};
	const auto iatStub = [&](const char* name) { return iatStubFor("KERNEL32.dll", name); };
	unsigned ordinalImportReferences = 0;
	unsigned boundOrdinalImportReferences = 0;
	for (const auto& import : runtime.image().imports) {
		if (!import.byOrdinal) continue;
		++ordinalImportReferences;
		const uint32_t slotAddress = runtime.image().loadBase + import.iatRva;
		const uint32_t stubAddress = guest::load<uint32_t>(slotAddress);
		const auto* stub = runtime.imports().lookup(stubAddress);
		if (stub && stub->byOrdinal && stub->ordinal == import.ordinal &&
			stub->library == import.library) ++boundOrdinalImportReferences;
	}
	if (!ordinalImportReferences || boundOrdinalImportReferences != ordinalImportReferences) {
		std::fprintf(stderr, "ordinal IAT binding mismatch: imports=%u bound=%u\n",
			ordinalImportReferences, boundOrdinalImportReferences);
		return 12;
	}
	uint32_t getVersionStub = 0;
	for (const auto& import : runtime.image().imports) {
		if (import.library == "KERNEL32.dll" && import.name == "GetVersion") {
			const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
			if (!slot) break;
			getVersionStub = uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
				(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
			break;
		}
	}
	const auto* api = runtime.imports().lookup(getVersionStub);
	if (!api || !handlers.find(*api)) {
		std::fprintf(stderr, "mapped executable GetVersion IAT does not resolve to a handler\n");
		return 5;
	}
	CPU cpu{};
	if (!runtime.prepareInitialCpu(cpu, addressSpace, error)) {
		std::fprintf(stderr, "cannot initialize guest startup context: %s\n", error.c_str());
		return 6;
	}
	const uint32_t logicalDrivesStub = iatStub("GetLogicalDrives");
	const uint32_t logicalDriveStringsStub = iatStub("GetLogicalDriveStringsA");
	const uint32_t driveTypeStub = iatStub("GetDriveTypeA");
	if (!logicalDrivesStub || !logicalDriveStringsStub || !driveTypeStub) {
		std::fprintf(stderr, "source PE is missing a logical-drive enumeration import\n");
		return 70;
	}
	#if defined(BR_TEST_VIRTUAL_DRIVE_ENUMERATION)
	testVirtualExecutableAvailable = false;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, logicalDrivesStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fed4u) {
		std::fprintf(stderr, "virtual drive discovery exposed C: without an accessible game executable\n");
		return 79;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	call_target(&cpu, logicalDriveStringsStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "virtual drive strings exposed an inaccessible C: root\n");
		return 80;
	}
	writeGuestString(0x7ff0fc00u, "C:\\");
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fc00u);
	call_target(&cpu, driveTypeStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "GetDriveTypeA claimed an inaccessible virtual C: root exists\n");
		return 81;
	}
	testVirtualExecutableAvailable = true;
	#endif
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, logicalDrivesStub);
	const uint32_t logicalDriveMask = cpu.r[EAX];
	if (!logicalDriveMask || cpu.r[ESP] != 0x7ff0fed4u) {
		std::fprintf(stderr, "GetLogicalDrives returned an empty mask or broke stdcall cleanup\n");
		return 71;
	}
	constexpr uint32_t driveStringsBuffer = 0x7ff0d000u;
	constexpr uint32_t driveStringsCapacity = 1024u;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	call_target(&cpu, logicalDriveStringsStub);
	const uint32_t requiredDriveStringsCapacity = cpu.r[EAX];
	if (!requiredDriveStringsCapacity || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "GetLogicalDriveStringsA size query failed: required=%u ESP=%08x\n",
			requiredDriveStringsCapacity, cpu.r[ESP]);
		return 77;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, driveStringsCapacity);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, driveStringsBuffer);
	call_target(&cpu, logicalDriveStringsStub);
	const uint32_t driveStringsLength = cpu.r[EAX];
	if (!driveStringsLength || driveStringsLength >= driveStringsCapacity || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "GetLogicalDriveStringsA returned an invalid length or broke stdcall cleanup\n");
		return 72;
	}
	if (requiredDriveStringsCapacity != driveStringsLength + 1u) {
		std::fprintf(stderr, "GetLogicalDriveStringsA size query/copy lengths disagree: needed=%u copied=%u\n",
			requiredDriveStringsCapacity, driveStringsLength);
		return 78;
	}
	uint32_t driveMaskFromStrings = 0u;
	uint32_t cursor = 0u;
	while (cursor < driveStringsCapacity && guest::load<uint8_t>(driveStringsBuffer + cursor)) {
		std::string root;
		while (cursor < driveStringsCapacity) {
			const char byte = static_cast<char>(guest::load<uint8_t>(driveStringsBuffer + cursor++));
			if (!byte) break;
			root.push_back(byte);
		}
		if (root.size() < 2u || root[1] != ':') {
			std::fprintf(stderr, "GetLogicalDriveStringsA returned a malformed root\n");
			return 73;
		}
		const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(root[0])));
		if (letter < 'A' || letter > 'Z') return 74;
		driveMaskFromStrings |= 1u << (letter - 'A');
		writeGuestString(0x7ff0fc00u, root.c_str());
		cpu.r[ESP] = 0x7ff0fed0u;
		guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
		guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fc00u);
		call_target(&cpu, driveTypeStub);
		if (cpu.r[EAX] == 1u || cpu.r[ESP] != 0x7ff0fed8u) {
			std::fprintf(stderr, "GetDriveTypeA rejected an enumerated root or broke stdcall cleanup: %s\n",
				root.c_str());
			return 75;
		}
	}
	if (driveMaskFromStrings != logicalDriveMask) {
		std::fprintf(stderr, "drive enumeration mismatch: mask=%08x strings=%08x\n",
			logicalDriveMask, driveMaskFromStrings);
		return 76;
	}
	#if defined(BR_TEST_VIRTUAL_DRIVE_ENUMERATION)
	if (logicalDriveMask != (1u << 2) || driveMaskFromStrings != (1u << 2)) {
		std::fprintf(stderr, "virtual SAF-backed drive should expose only accessible C: (mask=%08x)\n",
			logicalDriveMask);
		return 82;
	}
	std::fprintf(stderr, "virtual drive regression passed: absent game executable hides C:, readable game executable exposes C:\\ consistently\n");
	#endif
	std::fprintf(stderr, "logical-drive regression passed: mask=%08x roots consistent with GetDriveTypeA\n",
		logicalDriveMask);
	const uint32_t profileStub = iatStub("GetPrivateProfileIntA");
	if (!profileStub) {
		std::fprintf(stderr, "source PE has no GetPrivateProfileIntA IAT slot\n");
		return 43;
	}
	writeGuestString(0x7ff0fc00u, "gRaPhIcS");
	writeGuestString(0x7ff0fc40u, "wIdTh");
	writeGuestString(0x7ff0fc80u, "startup_profile_test.ini");
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fc00u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fc40u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x7ff0fc80u);
	call_target(&cpu, profileStub);
	if (cpu.r[EAX] != 1280u || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "GetPrivateProfileIntA INI lookup or stack cleanup mismatch: EAX=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 44;
	}
	writeGuestString(0x7ff0fc40u, "missing");
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fc00u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fc40u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0xffffff9du);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x7ff0fc80u);
	call_target(&cpu, profileStub);
	if (cpu.r[EAX] != 0xffffff9du || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "GetPrivateProfileIntA default or stack cleanup mismatch: EAX=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 45;
	}
	std::remove("analysis/startup_profile_test.ini");
	const uint32_t getUserLcidStub = iatStub("GetUserDefaultLCID");
	const uint32_t getLocaleInfoStub = iatStub("GetLocaleInfoA");
	if (!getUserLcidStub || !getLocaleInfoStub) {
		std::fprintf(stderr, "source PE is missing the locale imports reached at startup\n");
		return 50;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, getUserLcidStub);
	if (cpu.r[EAX] != 0x0409u || cpu.r[ESP] != 0x7ff0fed4u) {
		std::fprintf(stderr, "GetUserDefaultLCID value or stdcall cleanup mismatch\n");
		return 51;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x0409u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x1004u); // LOCALE_IDEFAULTANSICODEPAGE
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0x7ff0fb80u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 8u);
	call_target(&cpu, getLocaleInfoStub);
	const char expectedCodePage[] = "1252";
	if (cpu.r[EAX] != sizeof(expectedCodePage) || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "GetLocaleInfoA code-page query return or stack mismatch\n");
		return 52;
	}
	for (uint32_t i = 0; i < sizeof(expectedCodePage); ++i) {
		if (guest::load<uint8_t>(0x7ff0fb80u + i) != static_cast<uint8_t>(expectedCodePage[i])) {
			std::fprintf(stderr, "GetLocaleInfoA ANSI code-page value mismatch\n");
			return 53;
		}
	}
	const uint32_t defWindowProcStub = iatStubFor("USER32.dll", "DefWindowProcA");
	windowDefProcStub = defWindowProcStub;
	const uint32_t peekMessageStub = iatStubFor("USER32.dll", "PeekMessageA");
	if (!defWindowProcStub || !peekMessageStub) {
		std::fprintf(stderr, "source PE is missing DefWindowProcA or PeekMessageA imported by its WndProc\n");
		return 54;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x6c100000u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x81u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x6c200000u);
	call_target(&cpu, defWindowProcStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "DefWindowProcA WM_NCCREATE return or stack cleanup mismatch\n");
		return 55;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x6c100000u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 1u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x6c200000u);
	call_target(&cpu, defWindowProcStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "DefWindowProcA WM_CREATE return or stack cleanup mismatch\n");
		return 56;
	}
	writeGuestString(0x7ff0fb00u, "SmokeWindow");
	writeGuestString(0x7ff0fb40u, "Smoke title");
	for (uint32_t offset = 0; offset < 40u; offset += 4u)
		guest::store<uint32_t>(0x7ff0fa00u + offset, 0u);
	guest::store<uint32_t>(0x7ff0fa04u, 0x00100004u); // guest WndProc
	guest::store<uint32_t>(0x7ff0fa10u, 0x00400000u); // hInstance
	guest::store<uint32_t>(0x7ff0fa24u, 0x7ff0fb00u); // lpszClassName
	const uint32_t smokeRegisterClassStub = iatStubFor("USER32.dll", "RegisterClassA");
	const uint32_t createWindowStub = iatStubFor("USER32.dll", "CreateWindowExA");
	const uint32_t clientToScreenStub = iatStubFor("USER32.dll", "ClientToScreen");
	const uint32_t screenToClientStub = iatStubFor("USER32.dll", "ScreenToClient");
	const uint32_t getCursorPosStub = iatStubFor("USER32.dll", "GetCursorPos");
	const uint32_t setCursorPosStub = iatStubFor("USER32.dll", "SetCursorPos");
	const uint32_t smokeFindWindowStub = iatStubFor("USER32.dll", "FindWindowA");
	const uint32_t sendMessageStub = iatStubFor("USER32.dll", "SendMessageA");
	const uint32_t setForegroundStub = iatStubFor("USER32.dll", "SetForegroundWindow");
	const uint32_t drawMenuBarStub = iatStubFor("USER32.dll", "DrawMenuBar");
	const uint32_t redrawWindowStub = iatStubFor("USER32.dll", "RedrawWindow");
	const uint32_t setActiveStub = iatStubFor("USER32.dll", "SetActiveWindow");
	const uint32_t bringToTopStub = iatStubFor("USER32.dll", "BringWindowToTop");
	const uint32_t showCursorStub = iatStubFor("USER32.dll", "ShowCursor");
	const uint32_t getClientRectStub = iatStubFor("USER32.dll", "GetClientRect");
	const uint32_t getWindowLongStub = iatStubFor("USER32.dll", "GetWindowLongA");
	const uint32_t setWindowLongStub = iatStubFor("USER32.dll", "SetWindowLongA");
	const uint32_t setWindowPosStub = iatStubFor("USER32.dll", "SetWindowPos");
	const uint32_t adjustWindowRectStub = iatStubFor("USER32.dll", "AdjustWindowRect");
	const uint32_t setRectStub = iatStubFor("USER32.dll", "SetRect");
	const uint32_t showWindowStub = iatStubFor("USER32.dll", "ShowWindow");
	const uint32_t updateWindowStub = iatStubFor("USER32.dll", "UpdateWindow");
	const uint32_t enableWindowStub = iatStubFor("USER32.dll", "EnableWindow");
	const uint32_t destroyWindowStub = iatStubFor("USER32.dll", "DestroyWindow");
	if (!smokeRegisterClassStub || !createWindowStub || !clientToScreenStub || !screenToClientStub ||
		!getCursorPosStub || !setCursorPosStub ||
		!smokeFindWindowStub || !sendMessageStub || !setForegroundStub ||
		!drawMenuBarStub || !redrawWindowStub || !setActiveStub || !bringToTopStub || !showCursorStub ||
		!getClientRectStub || !getWindowLongStub || !setWindowLongStub || !setWindowPosStub ||
		!adjustWindowRectStub || !setRectStub || !showWindowStub || !updateWindowStub ||
		!enableWindowStub || !destroyWindowStub) {
		std::fprintf(stderr, "source PE is missing expected USER32 window imports\n");
		return 46;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fa00u);
	call_target(&cpu, smokeRegisterClassStub);
	if (cpu.r[EAX] == 0u || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "RegisterClassA did not retain the guest WndProc\n");
		return 47;
	}
	cpu.r[ESP] = 0x7ff0fe90u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u); // dwExStyle
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fb00u); // class
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0x7ff0fb40u); // title
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x10cf0000u); // style
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 10u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 20u);
	guest::store<uint32_t>(cpu.r[ESP] + 28u, 640u);
	guest::store<uint32_t>(cpu.r[ESP] + 32u, 480u);
	guest::store<uint32_t>(cpu.r[ESP] + 36u, 0u); // parent
	guest::store<uint32_t>(cpu.r[ESP] + 40u, 0u); // menu
	guest::store<uint32_t>(cpu.r[ESP] + 44u, 0x00400000u); // instance
	guest::store<uint32_t>(cpu.r[ESP] + 48u, 0x7ff0fa80u); // create parameter
	windowCallbackMessages.clear();
	std::memset(windowCreateArguments, 0, sizeof(windowCreateArguments));
	call_target(&cpu, createWindowStub);
	const uint32_t createdWindow = cpu.r[EAX];
	const uint32_t expectedWindowMessages[] = {0x81u, 1u, 0x46u, 0x47u, 0x0003u, 0x0005u, 0x18u};
	if (!createdWindow || cpu.r[ESP] != 0x7ff0fec4u ||
		windowCallbackMessages.size() != sizeof(expectedWindowMessages) / sizeof(expectedWindowMessages[0]) ||
		!std::equal(windowCallbackMessages.begin(), windowCallbackMessages.end(), expectedWindowMessages) ||
		windowCreateArguments[0] != createdWindow || windowCreateArguments[1] != 1u ||
		guest::load<uint32_t>(windowCreateArguments[3] + 16u) != 480u ||
		guest::load<uint32_t>(windowCreateArguments[3] + 20u) != 640u ||
		guest::load<uint32_t>(windowCreateArguments[3]) != 0x7ff0fa80u) {
		std::fprintf(stderr, "CreateWindowExA lifecycle callbacks or CREATESTRUCTA layout mismatch: hwnd=%08x ESP=%08x count=%zu msgs=",
			createdWindow, cpu.r[ESP], windowCallbackMessages.size());
		for (const uint32_t message : windowCallbackMessages) std::fprintf(stderr, "%04x ", message);
		std::fprintf(stderr, "create_args=%08x/%08x/%08x/%08x\n", windowCreateArguments[0],
			windowCreateArguments[1], windowCreateArguments[2], windowCreateArguments[3]);
		return 48;
	}
	// The same frame model must drive AdjustWindowRect, GetClientRect and the
	// coordinate helpers: a 640x480 thick-frame/caption window has a 624x441
	// client area, and AdjustWindowRect must round-trip those client dimensions.
	constexpr uint32_t guestRectAddress = 0x7ff0f780u;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, guestRectAddress);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 624u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 441u);
	call_target(&cpu, setRectStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fee8u) {
		std::fprintf(stderr, "SetRect returned failure or unbalanced stdcall stack\n");
		return 85;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, guestRectAddress);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x10cf0000u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	call_target(&cpu, adjustWindowRectStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fee4u ||
		guest::load<int32_t>(guestRectAddress) != -8 || guest::load<int32_t>(guestRectAddress + 4u) != -31 ||
		guest::load<int32_t>(guestRectAddress + 8u) != 632 || guest::load<int32_t>(guestRectAddress + 12u) != 449) {
		std::fprintf(stderr, "AdjustWindowRect geometry/ABI mismatch: %d,%d,%d,%d\n",
			guest::load<int32_t>(guestRectAddress), guest::load<int32_t>(guestRectAddress + 4u),
			guest::load<int32_t>(guestRectAddress + 8u), guest::load<int32_t>(guestRectAddress + 12u));
		return 86;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, guestRectAddress);
	call_target(&cpu, getClientRectStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fedcu ||
		guest::load<int32_t>(guestRectAddress) != 0 || guest::load<int32_t>(guestRectAddress + 4u) != 0 ||
		guest::load<int32_t>(guestRectAddress + 8u) != 624 || guest::load<int32_t>(guestRectAddress + 12u) != 441) {
		std::fprintf(stderr, "GetClientRect geometry/ABI mismatch\n");
		return 87;
	}
	// A nonclient window at (10,20) with an 8-pixel thick frame and 23-pixel
	// caption has client origin (18,51). ClientToScreen and ScreenToClient must
	// be inverse operations on the same valid guest HWND.
	constexpr uint32_t pointAddress = 0x7ff0f800u;
	guest::store<int32_t>(pointAddress, 512);
	guest::store<int32_t>(pointAddress + 4u, 384);
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, pointAddress);
	call_target(&cpu, clientToScreenStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fedcu ||
		guest::load<int32_t>(pointAddress) != 530 || guest::load<int32_t>(pointAddress + 4u) != 435) {
		std::fprintf(stderr, "ClientToScreen guest window origin or stdcall behavior mismatch: %d,%d\n",
			guest::load<int32_t>(pointAddress), guest::load<int32_t>(pointAddress + 4u));
		return 70;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, pointAddress);
	call_target(&cpu, screenToClientStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fedcu ||
		guest::load<int32_t>(pointAddress) != 512 || guest::load<int32_t>(pointAddress + 4u) != 384) {
		std::fprintf(stderr, "ScreenToClient failed to invert ClientToScreen\n");
		return 71;
	}
	guest::store<int32_t>(pointAddress, 512);
	guest::store<int32_t>(pointAddress + 4u, 384);
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0xdeadbeefu);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, pointAddress);
	call_target(&cpu, clientToScreenStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fedcu ||
		br::runtime::currentWin32LastError() != 1400u ||
		guest::load<int32_t>(pointAddress) != 512 || guest::load<int32_t>(pointAddress + 4u) != 384) {
		std::fprintf(stderr, "ClientToScreen invalid HWND failure/point preservation mismatch\n");
		return 72;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	call_target(&cpu, clientToScreenStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fedcu ||
		br::runtime::currentWin32LastError() != 998u) {
		std::fprintf(stderr, "ClientToScreen invalid POINT pointer failure/ABI mismatch\n");
		return 73;
	}
	// A visible window's invalid region produces WM_PAINT until its default
	// procedure validates it. Verify one generated paint, then no repeat.
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0f900u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 1u); // PM_REMOVE
	call_target(&cpu, peekMessageStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fee8u ||
		guest::load<uint32_t>(0x7ff0f904u) != 0x000fu ||
		guest::load<uint32_t>(0x7ff0f900u) != createdWindow) {
		std::fprintf(stderr, "PeekMessageA did not produce WM_PAINT for a visible invalid window\n");
		return 62;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x000fu);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	call_target(&cpu, defWindowProcStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "DefWindowProcA WM_PAINT return or stack cleanup mismatch\n");
		return 63;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0f900u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 1u);
	call_target(&cpu, peekMessageStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee8u) {
		std::fprintf(stderr, "DefWindowProcA did not validate the WM_PAINT update region\n");
		return 64;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fb00u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fb40u);
	call_target(&cpu, smokeFindWindowStub);
	if (cpu.r[EAX] != createdWindow || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "FindWindowA did not find the created guest window\n");
		return 49;
	}
	// SendMessageA must synchronously dispatch the original WM_CLOSE through
	// the WndProc registered for this live guest HWND and return its LRESULT.
	windowMessageRecords.clear();
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x0010u); // WM_CLOSE
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	call_target(&cpu, sendMessageStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u || windowMessageRecords.size() != 1u ||
		windowMessageRecords[0].hwnd != createdWindow || windowMessageRecords[0].message != 0x0010u ||
		windowMessageRecords[0].wParam != 0u || windowMessageRecords[0].lParam != 0u) {
		std::fprintf(stderr, "SendMessageA did not synchronously dispatch WM_CLOSE with correct LRESULT/ABI\n");
		return 70;
	}
	windowMessageRecords.clear();
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0xdeadbeefu);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x0010u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	call_target(&cpu, sendMessageStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u || !windowMessageRecords.empty()) {
		std::fprintf(stderr, "SendMessageA dispatched an invalid HWND or violated stdcall cleanup\n");
		return 71;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	windowMessageRecords.clear();
	call_target(&cpu, setForegroundStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "SetForegroundWindow rejected a live guest HWND or ABI mismatch\n");
		return 57;
	}
	const WindowMessageRecord expectedActivation[] = {
		{createdWindow, 0x001cu, 1u, 0u}, // WM_ACTIVATEAPP(TRUE)
		{createdWindow, 0x0006u, 1u, 0u}, // WM_ACTIVATE / WA_ACTIVE
		{createdWindow, 0x0007u, 0u, 0u}  // WM_SETFOCUS
	};
	if (windowMessageRecords.size() != sizeof(expectedActivation) / sizeof(expectedActivation[0]) ||
		!std::equal(windowMessageRecords.begin(), windowMessageRecords.end(),
			expectedActivation, [](const WindowMessageRecord& actual, const WindowMessageRecord& expected) {
				return actual.hwnd == expected.hwnd && actual.message == expected.message &&
					actual.wParam == expected.wParam && actual.lParam == expected.lParam;
			})) {
		std::fprintf(stderr, "SetForegroundWindow activation notification order/arguments mismatch\n");
		return 65;
	}
	windowCallbackMessages.clear();
	windowMessageRecords.clear();
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	call_target(&cpu, drawMenuBarStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u ||
		windowCallbackMessages.size() != 2u || windowCallbackMessages[0] != 0x0046u ||
		windowCallbackMessages[1] != 0x0047u || windowMessageRecords.size() != 2u ||
		windowMessageRecords[0].hwnd != createdWindow || windowMessageRecords[0].wParam != 0u ||
		!windowMessageRecords[0].lParam || windowMessageRecords[0].lParam != windowMessageRecords[1].lParam ||
		guest::load<uint32_t>(windowMessageRecords[0].lParam + 24u) != 0x0037u) {
		std::fprintf(stderr, "DrawMenuBar failed to validate hwnd or issue frame-change WINDOWPOS callbacks\n");
		return 67;
	}
	windowCallbackMessages.clear();
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x0400u); // RDW_FRAME without RDW_INVALIDATE has no effect.
	call_target(&cpu, redrawWindowStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fee4u || !windowCallbackMessages.empty()) {
		std::fprintf(stderr, "RedrawWindow valid HWND/RDW_FRAME no-invalidate behavior mismatch\n");
		return 68;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0xdeadbeefu);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x0400u);
	call_target(&cpu, redrawWindowStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "RedrawWindow invalid HWND did not fail or clean its stack\n");
		return 69;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x001cu); // WM_ACTIVATEAPP
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 1u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	call_target(&cpu, defWindowProcStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u) {
		std::fprintf(stderr, "DefWindowProcA WM_ACTIVATEAPP default result/stack cleanup mismatch\n");
		return 66;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	call_target(&cpu, setActiveStub);
	if (cpu.r[EAX] != createdWindow || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "SetActiveWindow previous-HWND result or stdcall cleanup mismatch\n");
		return 58;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	call_target(&cpu, bringToTopStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "BringWindowToTop rejected live guest HWND or stdcall cleanup mismatch\n");
		return 59;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	call_target(&cpu, showCursorStub);
	if (static_cast<int32_t>(cpu.r[EAX]) != -1 || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "ShowCursor hide count or stdcall cleanup mismatch\n");
		return 60;
	}
	const uint32_t wndProcArguments[] = {0x6f100001u, 0x00000081u, 0x1234u, 0x5678u};
	cpu.r[ESP] = 0x7ff0ff00u;
	uint32_t wndProcResult = 0;
	if (!callGuestStdcall(&cpu, 0x00100000u, wndProcArguments, 4u,
		wndProcResult, error) || wndProcResult != 0x12345678u ||
		cpu.r[ESP] != 0x7ff0ff00u || callbackArguments[0] != wndProcArguments[0] ||
		callbackArguments[1] != wndProcArguments[1] ||
		callbackArguments[2] != wndProcArguments[2] ||
		callbackArguments[3] != wndProcArguments[3]) {
		std::fprintf(stderr, "guest WndProc stdcall bridge failed: %s\n", error.c_str());
		return 42;
	}
	cpu.r[ESP] = 0x7ff0fffcu;
	if (cpu.r[ESP] != 0x7ff0fffcu || cpu.fsbase != 0x7fff0000u ||
		guest::load<uint32_t>(cpu.fsbase) != 0xffffffffu ||
		guest::load<uint32_t>(cpu.fsbase + 4u) != 0x7ff10000u ||
		guest::load<uint32_t>(cpu.fsbase + 8u) != 0x7fe10000u ||
		guest::load<uint32_t>(cpu.fsbase + 0x18u) != cpu.fsbase) {
		std::fprintf(stderr, "minimal x86 stack/TEB state mismatch ESP=%08x FSBASE=%08x TEB={%08x,%08x,%08x,%08x}\n",
			cpu.r[ESP], cpu.fsbase, guest::load<uint32_t>(cpu.fsbase),
			guest::load<uint32_t>(cpu.fsbase + 4u), guest::load<uint32_t>(cpu.fsbase + 8u),
			guest::load<uint32_t>(cpu.fsbase + 0x18u));
		return 7;
	}
	const uint32_t getCommandLineStub = iatStub("GetCommandLineA");
	cpu.r[ESP] = 0x7ff0fffcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, getCommandLineStub);
	const uint32_t commandLine = cpu.r[EAX];
	const char expectedCommandLine[] = "Battle_Realms_F.exe";
	for (uint32_t i = 0; i < sizeof(expectedCommandLine); ++i) {
		if (guest::load<uint8_t>(commandLine + i) != static_cast<uint8_t>(expectedCommandLine[i])) {
			std::fprintf(stderr, "GetCommandLineA startup value mismatch\n");
			return 8;
		}
	}
	const uint32_t getStartupInfoStub = iatStub("GetStartupInfoA");
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fe00u);
	call_target(&cpu, getStartupInfoStub);
	if (guest::load<uint32_t>(0x7ff0fe00u) != 68u ||
		guest::load<uint32_t>(0x7ff0fe04u) != 0u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "GetStartupInfoA structure or stack cleanup mismatch\n");
		return 9;
	}
	const uint32_t getModuleHandleStub = iatStub("GetModuleHandleA");
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	call_target(&cpu, getModuleHandleStub);
	if (cpu.r[EAX] != 0x00400000u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "GetModuleHandleA main-image result or stack cleanup mismatch\n");
		return 10;
	}
	const uint32_t getVersionExStub = iatStub("GetVersionExA");
	guest::store<uint32_t>(0x7ff0fe00u, 148u);
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x7ff0fe00u);
	call_target(&cpu, getVersionExStub);
	constexpr char expectedServicePack[] = "Service Pack 3";
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<uint32_t>(0x7ff0fe00u) != 148u ||
		guest::load<uint32_t>(0x7ff0fe04u) != 5u ||
		guest::load<uint32_t>(0x7ff0fe08u) != 1u ||
		guest::load<uint32_t>(0x7ff0fe0cu) != 2600u ||
		guest::load<uint32_t>(0x7ff0fe10u) != 2u) {
		std::fprintf(stderr, "GetVersionExA XP fields or stack cleanup mismatch\n");
		return 13;
	}
	for (uint32_t i = 0; i < sizeof(expectedServicePack); ++i) {
		if (guest::load<uint8_t>(0x7ff0fe14u + i) != static_cast<uint8_t>(expectedServicePack[i])) {
			std::fprintf(stderr, "GetVersionExA service pack string mismatch\n");
			return 14;
		}
	}
	cpu.r[ESP] = 0x7ff0fffcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, getVersionStub);
	if (cpu.r[EAX] != 0x0a280105u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "mapped PE GetVersion import dispatch failed\n");
		return 11;
	}
	const uint32_t getVersionResult = cpu.r[EAX];
	const uint32_t getEnvWStub = iatStub("GetEnvironmentStringsW");
	cpu.r[ESP] = 0x7ff0fffcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, getEnvWStub);
	const uint32_t environmentW = cpu.r[EAX];
	if (!environmentW || guest::load<uint16_t>(environmentW) != 0u ||
		guest::load<uint16_t>(environmentW + 2u) != 0u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "GetEnvironmentStringsW empty block or stack cleanup mismatch\n");
		return 15;
	}
	const uint32_t freeEnvWStub = iatStub("FreeEnvironmentStringsW");
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, environmentW);
	call_target(&cpu, freeEnvWStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "FreeEnvironmentStringsW result or stack cleanup mismatch\n");
		return 16;
	}
	const uint32_t getEnvAStub = iatStub("GetEnvironmentStrings");
	cpu.r[ESP] = 0x7ff0fffcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, getEnvAStub);
	const uint32_t environmentA = cpu.r[EAX];
	if (!environmentA || guest::load<uint8_t>(environmentA) != 0u ||
		guest::load<uint8_t>(environmentA + 1u) != 0u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "GetEnvironmentStringsA empty block or stack cleanup mismatch\n");
		return 17;
	}
	const uint32_t freeEnvAStub = iatStub("FreeEnvironmentStringsA");
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, environmentA);
	call_target(&cpu, freeEnvAStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "FreeEnvironmentStringsA result or stack cleanup mismatch\n");
		return 23;
	}
	const uint32_t getStdHandleStub = iatStub("GetStdHandle");
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0xfffffff5u);
	call_target(&cpu, getStdHandleStub);
	if (cpu.r[EAX] != 0x11u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "GetStdHandle output handle or stack cleanup mismatch\n");
		return 18;
	}
	const uint32_t getAcpStub = iatStub("GetACP");
	cpu.r[ESP] = 0x7ff0fffcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	call_target(&cpu, getAcpStub);
	if (cpu.r[EAX] != 1252u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "GetACP result or stack cleanup mismatch\n");
		return 19;
	}
	const uint32_t getCPInfoStub = iatStub("GetCPInfo");
	cpu.r[ESP] = 0x7ff0fff4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1252u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fdc0u);
	call_target(&cpu, getCPInfoStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<uint32_t>(0x7ff0fdc0u) != 1u ||
		guest::load<uint8_t>(0x7ff0fdc4u) != '?' ||
		guest::load<uint8_t>(0x7ff0fdc5u) != 0u) {
		std::fprintf(stderr, "GetCPInfo CP-1252 structure or stack cleanup mismatch\n");
		return 24;
	}
	for (uint32_t i = 0; i < 12u; ++i) {
		if (guest::load<uint8_t>(0x7ff0fdc6u + i) != 0u) {
			std::fprintf(stderr, "GetCPInfo CP-1252 lead-byte table is not empty\n");
			return 25;
		}
	}
	const uint32_t getStringTypeWStub = iatStub("GetStringTypeW");
	guest::store<uint16_t>(0x7ff0fe80u, 'A');
	guest::store<uint16_t>(0x7ff0fe82u, '9');
	guest::store<uint16_t>(0x7ff0fe84u, '!');
	cpu.r[ESP] = 0x7ff0ffecu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fe80u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 3u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x7ff0fe90u);
	call_target(&cpu, getStringTypeWStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<uint16_t>(0x7ff0fe90u) != 0x0181u ||
		guest::load<uint16_t>(0x7ff0fe92u) != 0x0084u ||
		guest::load<uint16_t>(0x7ff0fe94u) != 0x0010u) {
		std::fprintf(stderr, "GetStringTypeW CTYPE1 mismatch: EAX=%08x ESP=%08x types=%04x,%04x,%04x\n",
			cpu.r[EAX], cpu.r[ESP], guest::load<uint16_t>(0x7ff0fe90u),
			guest::load<uint16_t>(0x7ff0fe92u), guest::load<uint16_t>(0x7ff0fe94u));
		return 26;
	}
	const uint32_t setHandleCountStub = iatStub("SetHandleCount");
	cpu.r[ESP] = 0x7ff0fff8u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0x100u);
	call_target(&cpu, setHandleCountStub);
	if (cpu.r[EAX] != 0x100u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "SetHandleCount result or stack cleanup mismatch\n");
		return 22;
	}
	const uint32_t wideToMultiStub = iatStub("WideCharToMultiByte");
	constexpr uint32_t wideSource = 0x7ff0fe20u;
	constexpr uint32_t narrowOutput = 0x7ff0fe40u;
	const uint16_t wideText[] = {'H', 'i', ' ', 0x20acu, 0};
	for (uint32_t i = 0; i < sizeof(wideText) / sizeof(wideText[0]); ++i)
		guest::store<uint16_t>(wideSource + i * 2u, wideText[i]);
	cpu.r[ESP] = 0x7ff0ffdcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1252u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, wideSource);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 28u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 32u, 0x7ff0fe60u);
	call_target(&cpu, wideToMultiStub);
	if (cpu.r[EAX] != 5u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<int32_t>(0x7ff0fe60u) != 0) {
		std::fprintf(stderr, "WideCharToMultiByte required-size query or ABI mismatch\n");
		return 20;
	}
	cpu.r[ESP] = 0x7ff0ffdcu;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1252u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, wideSource);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, narrowOutput);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 5u);
	guest::store<uint32_t>(cpu.r[ESP] + 28u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 32u, 0x7ff0fe60u);
	call_target(&cpu, wideToMultiStub);
	if (cpu.r[EAX] != 5u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<uint8_t>(narrowOutput) != 'H' ||
		guest::load<uint8_t>(narrowOutput + 1u) != 'i' ||
		guest::load<uint8_t>(narrowOutput + 2u) != ' ' ||
		guest::load<uint8_t>(narrowOutput + 3u) != 0x80u ||
		guest::load<uint8_t>(narrowOutput + 4u) != 0u) {
		std::fprintf(stderr, "WideCharToMultiByte CP-1252 conversion or stack cleanup mismatch\n");
		return 21;
	}
	const uint32_t multiToWideStub = iatStub("MultiByteToWideChar");
	guest::store<uint8_t>(narrowOutput, 'H');
	guest::store<uint8_t>(narrowOutput + 1u, 'i');
	guest::store<uint8_t>(narrowOutput + 2u, ' ');
	guest::store<uint8_t>(narrowOutput + 3u, 0x80u);
	guest::store<uint8_t>(narrowOutput + 4u, 0u);
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1252u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 1u); // MB_PRECOMPOSED
	guest::store<uint32_t>(cpu.r[ESP] + 12u, narrowOutput);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 5u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 0u);
	call_target(&cpu, multiToWideStub);
	if (cpu.r[EAX] != 5u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "MultiByteToWideChar required-size query or ABI mismatch\n");
		return 27;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1252u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 1u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, narrowOutput);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 5u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 0x7ff0fe80u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 5u);
	call_target(&cpu, multiToWideStub);
	if (cpu.r[EAX] != 5u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<uint16_t>(0x7ff0fe80u) != 'H' ||
		guest::load<uint16_t>(0x7ff0fe82u) != 'i' ||
		guest::load<uint16_t>(0x7ff0fe84u) != ' ' ||
		guest::load<uint16_t>(0x7ff0fe86u) != 0x20acu ||
		guest::load<uint16_t>(0x7ff0fe88u) != 0u) {
		std::fprintf(stderr, "MultiByteToWideChar CP-1252 conversion or stack cleanup mismatch\n");
		return 28;
	}
	const uint32_t lcMapStringWStub = iatStub("LCMapStringW");
	guest::store<uint16_t>(0x7ff0fe70u, 'A');
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u); // LOCALE_USER_DEFAULT
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x100u); // LCMAP_LOWERCASE
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0x7ff0fe70u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 1u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 0u);
	call_target(&cpu, lcMapStringWStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u) {
		std::fprintf(stderr, "LCMapStringW lowercase size query or ABI mismatch\n");
		return 29;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x100u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0x7ff0fe70u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 1u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 0x7ff0fe90u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 1u);
	call_target(&cpu, lcMapStringWStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff10000u ||
		guest::load<uint16_t>(0x7ff0fe90u) != 'a') {
		std::fprintf(stderr, "LCMapStringW lowercase result or stack cleanup mismatch\n");
		return 30;
	}
	br::runtime::configureWin32FileFindQuery(testFindQuery);
	const uint32_t findFirstStub = iatStub("FindFirstFileA");
	const uint32_t findNextStub = iatStub("FindNextFileA");
	const uint32_t findCloseStub = iatStub("FindClose");
	constexpr uint32_t patternAddress = 0x7ff0fe00u;
	constexpr uint32_t findDataAddress = 0x7ff0fc00u;
	const char findPattern[] = "protlog*.txt";
	for (uint32_t i = 0; i < sizeof(findPattern); ++i)
		guest::store<uint8_t>(patternAddress + i, static_cast<uint8_t>(findPattern[i]));
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, patternAddress);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, findDataAddress);
	call_target(&cpu, findFirstStub);
	const uint32_t findHandle = cpu.r[EAX];
	if (findHandle == 0xffffffffu || cpu.r[ESP] != 0x7ff0fff0u ||
		guest::load<uint32_t>(findDataAddress) != 1u ||
		guest::load<uint32_t>(findDataAddress + 32u) != 37u ||
		guest::load<uint8_t>(findDataAddress + 44u) != 'p') {
		std::fprintf(stderr, "FindFirstFileA result, WIN32_FIND_DATAA layout, or ABI mismatch\n");
		return 31;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, findHandle);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, findDataAddress);
	call_target(&cpu, findNextStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fff0u ||
		guest::load<uint32_t>(findDataAddress + 32u) != 41u) {
		std::fprintf(stderr, "FindNextFileA enumeration or ABI mismatch\n");
		return 32;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, findHandle);
	call_target(&cpu, findCloseStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "FindClose handle release or ABI mismatch\n");
		return 33;
	}
	constexpr uint32_t mutexNameAddress = 0x7ff0fd00u;
	const char mutexName[] = "Battle Realms Mutex";
	for (uint32_t i = 0; i < sizeof(mutexName); ++i)
		guest::store<uint8_t>(mutexNameAddress + i, static_cast<uint8_t>(mutexName[i]));
	const uint32_t createMutexStub = iatStub("CreateMutexA");
	const uint32_t waitStub = iatStub("WaitForSingleObject");
	const uint32_t releaseMutexStub = iatStub("ReleaseMutex");
	const uint32_t closeHandleStub = iatStub("CloseHandle");
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, mutexNameAddress);
	call_target(&cpu, createMutexStub);
	const uint32_t mutexHandle = cpu.r[EAX];
	if (!mutexHandle || cpu.r[ESP] != 0x7ff0fff4u) {
		std::fprintf(stderr, "CreateMutexA handle or ABI mismatch\n");
		return 34;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, mutexHandle);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	call_target(&cpu, waitStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fff0u) {
		std::fprintf(stderr, "WaitForSingleObject mutex acquisition or ABI mismatch\n");
		return 35;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, mutexHandle);
	call_target(&cpu, releaseMutexStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "ReleaseMutex ownership or ABI mismatch\n");
		return 36;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, mutexHandle);
	call_target(&cpu, closeHandleStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "CloseHandle mutex release or ABI mismatch\n");
		return 37;
	}
	const uint32_t findWindowStub = [&]() {
		for (const auto& import : runtime.image().imports) {
			if (import.library != "USER32.dll" || import.name != "FindWindowA") continue;
			const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
			if (!slot) return 0u;
			return uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
				(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
		}
		return 0u;
	}();
	constexpr uint32_t windowNameAddress = 0x7ff0fd80u;
	const char windowName[] = "Battle Realms";
	for (uint32_t i = 0; i < sizeof(windowName); ++i)
		guest::store<uint8_t>(windowNameAddress + i, static_cast<uint8_t>(windowName[i]));
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, windowNameAddress);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, windowNameAddress);
	call_target(&cpu, findWindowStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fff0u) {
		std::fprintf(stderr, "FindWindowA pre-window result or ABI mismatch\n");
		return 38;
	}
	uint32_t registerMessageStub = 0;
	for (const auto& import : runtime.image().imports) {
		if (import.library != "USER32.dll" || import.name != "RegisterWindowMessageA") continue;
		const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
		if (slot) registerMessageStub = uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
			(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
		break;
	}
	constexpr uint32_t messageNameAddress = 0x7ff0fdc0u;
	const char registeredMessage[] = "Close Previous Battle Realms Autoplay instance";
	for (uint32_t i = 0; i < sizeof(registeredMessage); ++i)
		guest::store<uint8_t>(messageNameAddress + i, static_cast<uint8_t>(registeredMessage[i]));
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, messageNameAddress);
	call_target(&cpu, registerMessageStub);
	const uint32_t registeredMessageId = cpu.r[EAX];
	if (registeredMessageId < 0xc000u || registeredMessageId > 0xffffu || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "RegisterWindowMessageA range or ABI mismatch\n");
		return 39;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, messageNameAddress);
	call_target(&cpu, registerMessageStub);
	if (cpu.r[EAX] != registeredMessageId || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "RegisterWindowMessageA repeat stability or ABI mismatch\n");
		return 40;
	}
	uint32_t loadIconStub = 0;
	for (const auto& import : runtime.image().imports) {
		if (import.library != "USER32.dll" || import.name != "LoadIconA") continue;
		const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
		if (slot) loadIconStub = uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
			(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
		break;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, runtime.image().loadBase);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 101u);
	call_target(&cpu, loadIconStub);
	if (cpu.r[EAX] != 0x6c000065u || cpu.r[ESP] != 0x7ff0fff0u) {
		std::fprintf(stderr, "LoadIconA PE resource validation or ABI mismatch\n");
		return 41;
	}
	uint32_t loadCursorStub = 0, registerClassStub = 0, getStockObjectStub = 0;
	for (const auto& import : runtime.image().imports) {
		const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
		if (!slot) continue;
		const uint32_t value = uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
			(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
		if (import.library == "USER32.dll" && import.name == "LoadCursorA") loadCursorStub = value;
		if (import.library == "USER32.dll" && import.name == "RegisterClassA") registerClassStub = value;
		if (import.library == "GDI32.dll" && import.name == "GetStockObject") getStockObjectStub = value;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7f00u);
	call_target(&cpu, loadCursorStub);
	if (cpu.r[EAX] != 0x6b007f00u || cpu.r[ESP] != 0x7ff0fff0u) {
		std::fprintf(stderr, "LoadCursorA IDC_ARROW or ABI mismatch\n");
		return 42;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 4u);
	call_target(&cpu, getStockObjectStub);
	if (cpu.r[EAX] != 0x6a000004u || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "GetStockObject identity or ABI mismatch\n");
		return 43;
	}
	constexpr uint32_t wndClassAddress = 0x7ff0fc80u;
	for (uint32_t offset = 0; offset < 40u; offset += 4u)
		guest::store<uint32_t>(wndClassAddress + offset, 0u);
	guest::store<uint32_t>(wndClassAddress + 4u, 0x00450a02u);
	guest::store<uint32_t>(wndClassAddress + 16u, runtime.image().loadBase);
	guest::store<uint32_t>(wndClassAddress + 20u, 0x6c000065u);
	guest::store<uint32_t>(wndClassAddress + 24u, 0x6b007f00u);
	guest::store<uint32_t>(wndClassAddress + 28u, 0x6a000004u);
	guest::store<uint32_t>(wndClassAddress + 36u, windowNameAddress);
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, wndClassAddress);
	call_target(&cpu, registerClassStub);
	if (!cpu.r[EAX] || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "RegisterClassA stored class or ABI mismatch\n");
		return 44;
	}
	br::runtime::configureWin32StartupDisplayMetrics(2400u, 1080u);
	uint32_t getSystemMetricsStub = 0;
	for (const auto& import : runtime.image().imports) {
		if (import.library != "USER32.dll" || import.name != "GetSystemMetrics") continue;
		const uint8_t* slot = addressSpace.host(runtime.image().loadBase + import.iatRva, 4);
		if (slot) getSystemMetricsStub = uint32_t(slot[0]) | (uint32_t(slot[1]) << 8) |
			(uint32_t(slot[2]) << 16) | (uint32_t(slot[3]) << 24);
		break;
	}
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	call_target(&cpu, getSystemMetricsStub);
	if (cpu.r[EAX] != 2400u || cpu.r[ESP] != 0x7ff0ffecu) {
		std::fprintf(stderr, "GetSystemMetrics SM_CXSCREEN or ABI mismatch\n");
		return 45;
	}
	// SetCursorPos takes screen coordinates, while the guest input bridge stores
	// its pointer in client coordinates. GetCursorPos must round-trip the warp.
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 1356u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 610u);
	call_target(&cpu, setCursorPosStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "SetCursorPos did not accept the active guest window point\n");
		return 74;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, pointAddress);
	call_target(&cpu, getCursorPosStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u ||
		guest::load<int32_t>(pointAddress) != 1356 || guest::load<int32_t>(pointAddress + 4u) != 610) {
		std::fprintf(stderr, "SetCursorPos/GetCursorPos screen-coordinate round-trip mismatch: %d,%d\n",
			guest::load<int32_t>(pointAddress), guest::load<int32_t>(pointAddress + 4u));
		return 75;
	}

	// Batch A window-state regression: long values, visibility/focus state,
	// synchronous paint delivery, enable state, geometry notifications, and
	// destruction must stay in one guest-coordinate model.
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0xfffffff0u); // GWL_STYLE.
	call_target(&cpu, getWindowLongStub);
	if (cpu.r[EAX] != 0x10cf0000u || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "GetWindowLongA GWL_STYLE or stdcall cleanup mismatch\n");
		return 88;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0xffffffebu); // GWL_USERDATA.
	call_target(&cpu, getWindowLongStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fedcu) return 89;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0xffffffebu);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0x13579bdfu);
	call_target(&cpu, setWindowLongStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee0u) {
		std::fprintf(stderr, "SetWindowLongA GWL_USERDATA previous value/ABI mismatch\n");
		return 90;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0xffffffebu);
	call_target(&cpu, getWindowLongStub);
	if (cpu.r[EAX] != 0x13579bdfu || cpu.r[ESP] != 0x7ff0fedcu) return 91;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u); // SW_HIDE.
	call_target(&cpu, showWindowStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fedcu ||
		windowCallbackMessages.empty() || windowCallbackMessages.back() != 0x0018u) {
		std::fprintf(stderr, "ShowWindow(SW_HIDE) prior visibility or WM_SHOWWINDOW mismatch\n");
		return 92;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 5u); // SW_SHOW.
	call_target(&cpu, showWindowStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fedcu ||
		windowCallbackMessages.back() != 0x0018u) return 93;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	call_target(&cpu, updateWindowStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u ||
		windowCallbackMessages.back() != 0x000fu) {
		std::fprintf(stderr, "UpdateWindow did not synchronously dispatch WM_PAINT\n");
		return 94;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u); // disable.
	call_target(&cpu, enableWindowStub);
	if (cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fedcu ||
		windowCallbackMessages.back() != 0x000au) return 95;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 1u); // enable.
	call_target(&cpu, enableWindowStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fedcu) return 96;
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u); // HWND_TOP.
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 30u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 40u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 800u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 600u);
	guest::store<uint32_t>(cpu.r[ESP] + 28u, 0u);
	call_target(&cpu, setWindowPosStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fef0u ||
		windowCallbackMessages.size() < 4u ||
		windowCallbackMessages[windowCallbackMessages.size() - 4u] != 0x0046u ||
		windowCallbackMessages[windowCallbackMessages.size() - 3u] != 0x0047u ||
		windowCallbackMessages[windowCallbackMessages.size() - 2u] != 0x0003u ||
		windowCallbackMessages.back() != 0x0005u) {
		std::fprintf(stderr, "SetWindowPos failed to preserve the WINDOWPOS/WM_MOVE/WM_SIZE sequence\n");
		return 97;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, guestRectAddress);
	call_target(&cpu, getClientRectStub);
	if (cpu.r[EAX] != 1u || guest::load<int32_t>(guestRectAddress + 8u) != 784 ||
		guest::load<int32_t>(guestRectAddress + 12u) != 561) {
		std::fprintf(stderr, "GetClientRect did not follow SetWindowPos dimensions\n");
		return 98;
	}
	guest::store<int32_t>(pointAddress, 0);
	guest::store<int32_t>(pointAddress + 4u, 0);
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, pointAddress);
	call_target(&cpu, clientToScreenStub);
	if (cpu.r[EAX] != 1u || guest::load<int32_t>(pointAddress) != 38 ||
		guest::load<int32_t>(pointAddress + 4u) != 71) {
		std::fprintf(stderr, "SetWindowPos and ClientToScreen coordinate origins diverged\n");
		return 99;
	}
	// Exercise DirectSound's observable per-buffer volume/pan controls while
	// this smoke's guest HWND is still alive. Calls go through the generated
	// COM stubs, so HRESULTs and x86 stdcall cleanup are checked together.
	const auto soundDynamicStub = [&](const char* name) {
		std::string stubError;
		auto& imports = const_cast<br::runtime::ImportBindingTable&>(runtime.imports());
		const uint32_t address = imports.getOrCreateDynamicStub("dsound.dll", name, stubError);
		const auto* apiImport = imports.lookup(address);
		return address && apiImport && handlers.find(*apiImport) ? address : 0u;
	};
	const auto soundDispatch = [&](uint32_t stub, std::initializer_list<uint32_t> args) {
		constexpr uint32_t stack = 0x7ff0e700u;
		cpu.r[ESP] = stack;
		guest::store<uint32_t>(stack, 0xffffffffu);
		uint32_t offset = 4u;
		for (const uint32_t value : args) {
			guest::store<uint32_t>(stack + offset, value);
			offset += 4u;
		}
		call_target(&cpu, stub);
		return cpu.r[ESP] == stack + offset;
	};
	constexpr uint32_t dsOutput = 0x7ff0e900u, dsFormat = 0x7ff0e940u;
	constexpr uint32_t dsDesc = 0x7ff0e980u, dsBufferOutput = 0x7ff0e9c0u;
	constexpr uint32_t dsNoPanDesc = 0x7ff0ea00u, dsNoPanOutput = 0x7ff0ea40u;
	const uint32_t dsCreate = soundDynamicStub("DirectSoundCreate");
	const uint32_t dsSetCooperative = soundDynamicStub("IDirectSound::SetCooperativeLevel");
	const uint32_t dsCreateBuffer = soundDynamicStub("IDirectSound::CreateSoundBuffer");
	const uint32_t dsSetVolume = soundDynamicStub("IDirectSoundBuffer::SetVolume");
	const uint32_t dsGetVolume = soundDynamicStub("IDirectSoundBuffer::GetVolume");
	const uint32_t dsSetPan = soundDynamicStub("IDirectSoundBuffer::SetPan");
	const uint32_t dsGetPan = soundDynamicStub("IDirectSoundBuffer::GetPan");
	if (!dsCreate || !dsSetCooperative || !dsCreateBuffer || !dsSetVolume ||
		!dsGetVolume || !dsSetPan || !dsGetPan ||
		!soundDispatch(dsCreate, {0u, dsOutput, 0u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound volume/pan test setup could not create its device\n");
		return 109;
	}
	const uint32_t dsDevice = guest::load<uint32_t>(dsOutput);
	if (!soundDispatch(dsSetCooperative, {dsDevice, createdWindow, 2u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound volume/pan test could not set priority cooperative level: %08x\n", cpu.r[EAX]);
		return 110;
	}
	guest::store<uint16_t>(dsFormat, 1u);      // WAVE_FORMAT_PCM.
	guest::store<uint16_t>(dsFormat + 2u, 2u); // stereo.
	guest::store<uint32_t>(dsFormat + 4u, 44100u);
	guest::store<uint32_t>(dsFormat + 8u, 176400u);
	guest::store<uint16_t>(dsFormat + 12u, 4u);
	guest::store<uint16_t>(dsFormat + 14u, 16u);
	guest::store<uint16_t>(dsFormat + 16u, 0u);
	for (const uint32_t desc : {dsDesc, dsNoPanDesc}) {
		for (uint32_t offset = 0; offset < 20u; offset += 4u) guest::store<uint32_t>(desc + offset, 0u);
		guest::store<uint32_t>(desc, 20u);
		guest::store<uint32_t>(desc + 8u, 4096u);
		guest::store<uint32_t>(desc + 16u, dsFormat);
	}
	guest::store<uint32_t>(dsDesc + 4u, 0x000000c0u); // CTRLVOLUME | CTRLPAN.
	guest::store<uint32_t>(dsNoPanDesc + 4u, 0x00000080u); // CTRLVOLUME only.
	if (!soundDispatch(dsCreateBuffer, {dsDevice, dsDesc, dsBufferOutput, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsCreateBuffer, {dsDevice, dsNoPanDesc, dsNoPanOutput, 0u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound volume/pan test could not create buffers: %08x\n", cpu.r[EAX]);
		return 111;
	}
	const uint32_t dsBuffer = guest::load<uint32_t>(dsBufferOutput);
	const uint32_t dsNoPanBuffer = guest::load<uint32_t>(dsNoPanOutput);
	constexpr uint32_t dsValueOutput = 0x7ff0ea80u;
	if (!soundDispatch(dsSetVolume, {dsBuffer, static_cast<uint32_t>(-2500)}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsGetVolume, {dsBuffer, dsValueOutput}) || cpu.r[EAX] != 0u ||
		guest::load<int32_t>(dsValueOutput) != -2500) {
		std::fprintf(stderr, "DirectSound SetVolume/GetVolume did not round-trip a valid attenuation\n");
		return 112;
	}
	if (!soundDispatch(dsSetVolume, {dsBuffer, 1u}) || cpu.r[EAX] != 0x80070057u ||
		!soundDispatch(dsGetVolume, {dsBuffer, dsValueOutput}) || cpu.r[EAX] != 0u ||
		guest::load<int32_t>(dsValueOutput) != -2500) {
		std::fprintf(stderr, "DirectSound rejected-volume path changed state or returned wrong HRESULT\n");
		return 113;
	}
	if (!soundDispatch(dsSetPan, {dsBuffer, 9000u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsGetPan, {dsBuffer, dsValueOutput}) || cpu.r[EAX] != 0u ||
		guest::load<int32_t>(dsValueOutput) != 9000 ||
		!soundDispatch(dsSetPan, {dsBuffer, static_cast<uint32_t>(-10001)}) || cpu.r[EAX] != 0x80070057u ||
		!soundDispatch(dsGetPan, {dsBuffer, dsValueOutput}) || cpu.r[EAX] != 0u ||
		guest::load<int32_t>(dsValueOutput) != 9000) {
		std::fprintf(stderr, "DirectSound SetPan range or state-preservation test failed\n");
		return 114;
	}
	if (!soundDispatch(dsSetPan, {dsNoPanBuffer, 0u}) || cpu.r[EAX] != 0x8878001eu ||
		!soundDispatch(dsGetPan, {dsNoPanBuffer, dsValueOutput}) || cpu.r[EAX] != 0x8878001eu ||
		!soundDispatch(dsSetVolume, {dsNoPanBuffer, static_cast<uint32_t>(-10000)}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound control capability flags were not enforced: %08x\n", cpu.r[EAX]);
		return 115;
	}
	// Exercise the complete guest PCM write -> playback -> host mix path. The
	// samples are intentionally tiny and deterministic so cursor advancement,
	// channel order, and one-shot/loop state are observable without a device.
	const uint32_t dsLock = soundDynamicStub("IDirectSoundBuffer::Lock");
	const uint32_t dsUnlock = soundDynamicStub("IDirectSoundBuffer::Unlock");
	const uint32_t dsPlay = soundDynamicStub("IDirectSoundBuffer::Play");
	const uint32_t dsStop = soundDynamicStub("IDirectSoundBuffer::Stop");
	const uint32_t dsGetStatus = soundDynamicStub("IDirectSoundBuffer::GetStatus");
	const uint32_t dsGetPosition = soundDynamicStub("IDirectSoundBuffer::GetCurrentPosition");
	const uint32_t dsSetPosition = soundDynamicStub("IDirectSoundBuffer::SetCurrentPosition");
	const uint32_t dsSetFrequency = soundDynamicStub("IDirectSoundBuffer::SetFrequency");
	constexpr uint32_t dsLockFirstPtr = 0x7ff0eac0u, dsLockFirstBytes = 0x7ff0eac4u;
	constexpr uint32_t dsLockSecondPtr = 0x7ff0eac8u, dsLockSecondBytes = 0x7ff0eaccu;
	constexpr uint32_t dsCursorOut = 0x7ff0ead0u, dsWriteCursorOut = 0x7ff0ead4u;
	constexpr uint32_t dsStatusOut = 0x7ff0ead8u;
	const int16_t pcmFrames[][2] = {{1000, -1000}, {2000, -2000}, {3000, -3000}, {4000, -4000}};
	if (!dsLock || !dsUnlock || !dsPlay || !dsStop || !dsGetStatus || !dsGetPosition ||
		!dsSetPosition || !dsSetFrequency ||
		!soundDispatch(dsSetVolume, {dsBuffer, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsSetPan, {dsBuffer, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsLock, {dsBuffer, 0u, static_cast<uint32_t>(sizeof(pcmFrames)), dsLockFirstPtr, dsLockFirstBytes,
			dsLockSecondPtr, dsLockSecondBytes, 0u}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(dsLockFirstBytes) != sizeof(pcmFrames) ||
		guest::load<uint32_t>(dsLockSecondBytes) != 0u ||
		guest::load<uint32_t>(dsLockSecondPtr) != 0u) {
		std::fprintf(stderr, "DirectSound PCM Lock/setup failed: hr=%08x\n", cpu.r[EAX]);
		return 116;
	}
	const uint32_t pcmAddress = guest::load<uint32_t>(dsLockFirstPtr);
	for (uint32_t frame = 0; frame < 4u; ++frame) {
		guest::store<uint16_t>(pcmAddress + frame * 4u, static_cast<uint16_t>(pcmFrames[frame][0]));
		guest::store<uint16_t>(pcmAddress + frame * 4u + 2u, static_cast<uint16_t>(pcmFrames[frame][1]));
	}
	if (!soundDispatch(dsUnlock, {dsBuffer, pcmAddress, static_cast<uint32_t>(sizeof(pcmFrames)), 0u, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsPlay, {dsBuffer, 0u, 0u, 1u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound PCM Unlock/Play failed: hr=%08x\n", cpu.r[EAX]);
		return 117;
	}
	int16_t mixedPcm[8]{};
	br::runtime::Win32AudioMixStats mixQuality{};
	br::runtime::renderWin32DirectSoundPcm(mixedPcm, 4u, 44100u, 2u, &mixQuality);
	for (uint32_t i = 0; i < 4u; ++i) {
		if (mixedPcm[i * 2u] != pcmFrames[i][0] || mixedPcm[i * 2u + 1u] != pcmFrames[i][1]) {
			std::fprintf(stderr, "DirectSound PCM mixer mismatch at frame %u: got %d/%d expected %d/%d\n",
				i, mixedPcm[i * 2u], mixedPcm[i * 2u + 1u], pcmFrames[i][0], pcmFrames[i][1]);
			return 118;
		}
	}
	if (mixQuality.activeBuffers == 0u || mixQuality.clippedSamples != 0u ||
		mixQuality.silentSamples != 0u || mixQuality.peakAbsoluteSample < 4000u ||
		mixQuality.sumSquaredSamples == 0u) {
		std::fprintf(stderr, "DirectSound audio signal metrics failed: buffers=%u peak=%u clipped=%llu silent=%llu sum_sq=%llu\n",
			mixQuality.activeBuffers, mixQuality.peakAbsoluteSample,
			static_cast<unsigned long long>(mixQuality.clippedSamples),
			static_cast<unsigned long long>(mixQuality.silentSamples),
			static_cast<unsigned long long>(mixQuality.sumSquaredSamples));
		return 128;
	}
	if (!soundDispatch(dsGetPosition, {dsBuffer, dsCursorOut, dsWriteCursorOut}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(dsCursorOut) != 16u || guest::load<uint32_t>(dsWriteCursorOut) != 20u ||
		!soundDispatch(dsGetStatus, {dsBuffer, dsStatusOut}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(dsStatusOut) != 5u ||
		!soundDispatch(dsSetPosition, {dsBuffer, 4u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsSetFrequency, {dsBuffer, 22050u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound cursor/status/frequency state failed: hr=%08x pos=%u status=%u\n",
			cpu.r[EAX], guest::load<uint32_t>(dsCursorOut), guest::load<uint32_t>(dsStatusOut));
		return 119;
	}
	int16_t resampled[4]{};
	br::runtime::renderWin32DirectSoundPcm(resampled, 2u, 44100u, 2u);
	if (resampled[0] != 2000 || resampled[1] != -2000 || resampled[2] != 2500 || resampled[3] != -2500) {
		std::fprintf(stderr, "DirectSound frequency resampling failed: %d/%d, %d/%d\n",
			resampled[0], resampled[1], resampled[2], resampled[3]);
		return 120;
	}
	if (!soundDispatch(dsStop, {dsBuffer}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsGetStatus, {dsBuffer, dsStatusOut}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(dsStatusOut) != 0u) {
		std::fprintf(stderr, "DirectSound Stop/GetStatus did not stop playback: hr=%08x status=%u\n",
			cpu.r[EAX], guest::load<uint32_t>(dsStatusOut));
		return 121;
	}
	// The game also creates mono PCM effect buffers. Check unsigned 8-bit
	// centering and host-rate interpolation independently from its 16-bit path.
	const uint32_t dsCreateMono = soundDynamicStub("IDirectSound::CreateSoundBuffer");
	constexpr uint32_t dsMonoBufferOutput = 0x7ff0eae0u;
	guest::store<uint16_t>(dsFormat, 1u);
	guest::store<uint16_t>(dsFormat + 2u, 1u);
	guest::store<uint32_t>(dsFormat + 4u, 22050u);
	guest::store<uint32_t>(dsFormat + 8u, 22050u);
	guest::store<uint16_t>(dsFormat + 12u, 1u);
	guest::store<uint16_t>(dsFormat + 14u, 8u);
	guest::store<uint16_t>(dsFormat + 16u, 0u);
	guest::store<uint32_t>(dsNoPanDesc + 8u, 4u);
	guest::store<uint32_t>(dsNoPanDesc + 16u, dsFormat);
	if (!soundDispatch(dsCreateMono, {dsDevice, dsNoPanDesc, dsMonoBufferOutput, 0u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound mono 8-bit buffer creation failed: hr=%08x\n", cpu.r[EAX]);
		return 122;
	}
	const uint32_t monoBuffer = guest::load<uint32_t>(dsMonoBufferOutput);
	if (!soundDispatch(dsLock, {monoBuffer, 0u, 4u, dsLockFirstPtr, dsLockFirstBytes,
		dsLockSecondPtr, dsLockSecondBytes, 0u}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(dsLockFirstBytes) != 4u) {
		std::fprintf(stderr, "DirectSound mono 8-bit Lock/Unlock failed: hr=%08x\n", cpu.r[EAX]);
		return 123;
	}
	const uint32_t monoPcm = guest::load<uint32_t>(dsLockFirstPtr);
	guest::store<uint8_t>(monoPcm, 128u);
	guest::store<uint8_t>(monoPcm + 1u, 192u);
	guest::store<uint8_t>(monoPcm + 2u, 64u);
	guest::store<uint8_t>(monoPcm + 3u, 128u);
	if (!soundDispatch(dsUnlock, {monoBuffer, monoPcm, 4u, 0u, 0u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DirectSound mono 8-bit Unlock failed: hr=%08x\n", cpu.r[EAX]);
		return 123;
	}
	if (!soundDispatch(dsPlay, {monoBuffer, 0u, 0u, 1u}) || cpu.r[EAX] != 0u) return 124;
	int16_t monoResampled[8]{};
	br::runtime::renderWin32DirectSoundPcm(monoResampled, 4u, 44100u, 2u);
	const int16_t expectedMono[] = {0, 0, 8192, 8192, 16384, 16384, 0, 0};
	if (!std::equal(std::begin(monoResampled), std::end(monoResampled), std::begin(expectedMono))) {
		std::fprintf(stderr, "DirectSound mono 8-bit interpolation failed: %d/%d %d/%d %d/%d %d/%d\n",
			monoResampled[0], monoResampled[1], monoResampled[2], monoResampled[3],
			monoResampled[4], monoResampled[5], monoResampled[6], monoResampled[7]);
		return 125;
	}
	if (!soundDispatch(dsStop, {monoBuffer}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsSetPosition, {monoBuffer, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(dsPlay, {monoBuffer, 0u, 0u, 0u}) || cpu.r[EAX] != 0u) return 126;
	int16_t monoOneShot[8]{};
	br::runtime::renderWin32DirectSoundPcm(monoOneShot, 4u, 22050u, 2u);
	if (monoOneShot[0] != 0 || monoOneShot[2] != 16384 || monoOneShot[4] != -16384 ||
		monoOneShot[6] != 0 || !soundDispatch(dsGetPosition, {monoBuffer, dsCursorOut, dsWriteCursorOut}) ||
		cpu.r[EAX] != 0u || guest::load<uint32_t>(dsCursorOut) != 4u ||
		!soundDispatch(dsGetStatus, {monoBuffer, dsStatusOut}) ||
		cpu.r[EAX] != 0u || guest::load<uint32_t>(dsStatusOut) != 0u ||
		guest::load<uint32_t>(dsCursorOut) != 4u) {
		std::fprintf(stderr, "DirectSound one-shot mono playback/cursor failed: hr=%08x status=%u cursor=%u\n",
			cpu.r[EAX], guest::load<uint32_t>(dsStatusOut), guest::load<uint32_t>(dsCursorOut));
		return 127;
	}
	std::fprintf(stderr, "DirectSound regression passed: controls, circular PCM Lock/Unlock, PCM8/PCM16 mono/stereo mixing, resampling, looping/one-shot state, cursor/status, and stdcall cleanup\n");
	// MS-ADPCM's predictor uses signed integer division by 256, which truncates
	// toward zero. This block makes the first prediction -64/256: the expected
	// PCM value is 0. Rounding it down to -1 creates a persistent DC error.
	const auto acmDynamicStub = [&](const char* name) {
		std::string stubError;
		auto& imports = const_cast<br::runtime::ImportBindingTable&>(runtime.imports());
		const uint32_t address = imports.getOrCreateDynamicStub("msacm32.dll", name, stubError);
		const auto* apiImport = imports.lookup(address);
		return address && apiImport && handlers.find(*apiImport) ? address : 0u;
	};
	const uint32_t acmOpen = acmDynamicStub("acmStreamOpen");
	const uint32_t acmPrepare = acmDynamicStub("acmStreamPrepareHeader");
	const uint32_t acmConvert = acmDynamicStub("acmStreamConvert");
	const uint32_t acmUnprepare = acmDynamicStub("acmStreamUnprepareHeader");
	const uint32_t acmClose = acmDynamicStub("acmStreamClose");
	constexpr uint32_t acmHandleOut = 0x7ff0eb00u, adpcmFormat = 0x7ff0eb20u;
	constexpr uint32_t pcmFormat = 0x7ff0eb80u, streamHeader = 0x7ff0eba0u;
	constexpr uint32_t adpcmBlock = 0x7ff0ec00u, decodedBlock = 0x7ff0ec20u;
	constexpr int16_t standardAdpcmCoefficients[7][2] = {
		{256, 0}, {512, -256}, {0, 0}, {192, 64}, {240, 0}, {460, -208}, {392, -232}
	};
	guest::store<uint16_t>(adpcmFormat, 2u); // WAVE_FORMAT_ADPCM.
	guest::store<uint16_t>(adpcmFormat + 2u, 1u);
	guest::store<uint32_t>(adpcmFormat + 4u, 22050u);
	guest::store<uint32_t>(adpcmFormat + 8u, 11025u);
	guest::store<uint16_t>(adpcmFormat + 12u, 8u);
	guest::store<uint16_t>(adpcmFormat + 14u, 4u);
	guest::store<uint16_t>(adpcmFormat + 16u, 32u);
	guest::store<uint16_t>(adpcmFormat + 18u, 4u); // samples per block.
	guest::store<uint16_t>(adpcmFormat + 20u, 7u);
	for (uint32_t i = 0; i < 7u; ++i) {
		guest::store<int16_t>(adpcmFormat + 22u + i * 4u, standardAdpcmCoefficients[i][0]);
		guest::store<int16_t>(adpcmFormat + 24u + i * 4u, standardAdpcmCoefficients[i][1]);
	}
	guest::store<uint16_t>(pcmFormat, 1u); // WAVE_FORMAT_PCM.
	guest::store<uint16_t>(pcmFormat + 2u, 1u);
	guest::store<uint32_t>(pcmFormat + 4u, 22050u);
	guest::store<uint32_t>(pcmFormat + 8u, 44100u);
	guest::store<uint16_t>(pcmFormat + 12u, 2u);
	guest::store<uint16_t>(pcmFormat + 14u, 16u);
	guest::store<uint16_t>(pcmFormat + 16u, 0u);
	guest::store<uint8_t>(adpcmBlock, 3u); // predictor (192, 64).
	guest::store<uint16_t>(adpcmBlock + 1u, 16u); // initial delta.
	guest::store<int16_t>(adpcmBlock + 3u, 0);
	guest::store<int16_t>(adpcmBlock + 5u, -1); // sample 2.
	guest::store<uint8_t>(adpcmBlock + 7u, 0u); // two zero nibbles.
	guest::store<uint32_t>(streamHeader, 84u);
	guest::store<uint32_t>(streamHeader + 12u, adpcmBlock);
	guest::store<uint32_t>(streamHeader + 16u, 8u);
	guest::store<uint32_t>(streamHeader + 28u, decodedBlock);
	guest::store<uint32_t>(streamHeader + 32u, 8u);
	const bool adpcmOpened = acmOpen && acmPrepare && acmConvert && acmUnprepare && acmClose &&
		soundDispatch(acmOpen, {acmHandleOut, 0u, adpcmFormat, pcmFormat, 0u, 0u, 0u, 0u}) && cpu.r[EAX] == 0u;
	const uint32_t acmHandle = adpcmOpened ? guest::load<uint32_t>(acmHandleOut) : 0u;
	if (!adpcmOpened || !soundDispatch(acmPrepare, {acmHandle, streamHeader, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(acmConvert, {acmHandle, streamHeader, 0x30u}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(streamHeader + 20u) != 8u ||
		guest::load<uint32_t>(streamHeader + 36u) != 8u ||
		guest::load<int16_t>(decodedBlock) != -1 || guest::load<int16_t>(decodedBlock + 2u) != 0 ||
		guest::load<int16_t>(decodedBlock + 4u) != 0 || guest::load<int16_t>(decodedBlock + 6u) != 0 ||
		!soundDispatch(acmUnprepare, {acmHandle, streamHeader, 0u}) || cpu.r[EAX] != 0u ||
		!soundDispatch(acmClose, {acmHandle, 0u}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "MS-ADPCM predictor regression failed: samples=%d,%d,%d,%d srcUsed=%u dstUsed=%u\n",
			guest::load<int16_t>(decodedBlock), guest::load<int16_t>(decodedBlock + 2u),
			guest::load<int16_t>(decodedBlock + 4u), guest::load<int16_t>(decodedBlock + 6u),
			guest::load<uint32_t>(streamHeader + 20u), guest::load<uint32_t>(streamHeader + 36u));
		return 133;
	}
	std::fprintf(stderr, "MS-ADPCM regression passed: negative predictor division truncates toward zero and full mono block converts\n");
	// Battle Realms also requests stereo MS-ADPCM streams. Verify that each
	// packed byte is decoded as one high-nibble left sample and one low-nibble
	// right sample, while the two predictor histories stay independent.
	constexpr uint32_t stereoAdpcmFormat = 0x7ff0ed00u;
	constexpr uint32_t stereoPcmFormat = 0x7ff0ed80u;
	constexpr uint32_t stereoHeader = 0x7ff0eda0u;
	constexpr uint32_t stereoBlock = 0x7ff0ee00u;
	constexpr uint32_t stereoDecoded = 0x7ff0ee20u;
	guest::store<uint16_t>(stereoAdpcmFormat, 2u);
	guest::store<uint16_t>(stereoAdpcmFormat + 2u, 2u);
	guest::store<uint32_t>(stereoAdpcmFormat + 4u, 22050u);
	guest::store<uint32_t>(stereoAdpcmFormat + 8u, 88200u);
	guest::store<uint16_t>(stereoAdpcmFormat + 12u, 16u);
	guest::store<uint16_t>(stereoAdpcmFormat + 14u, 4u);
	guest::store<uint16_t>(stereoAdpcmFormat + 16u, 32u);
	guest::store<uint16_t>(stereoAdpcmFormat + 18u, 4u);
	guest::store<uint16_t>(stereoAdpcmFormat + 20u, 7u);
	for (uint32_t i = 0; i < 7u; ++i) {
		guest::store<int16_t>(stereoAdpcmFormat + 22u + i * 4u, standardAdpcmCoefficients[i][0]);
		guest::store<int16_t>(stereoAdpcmFormat + 24u + i * 4u, standardAdpcmCoefficients[i][1]);
	}
	guest::store<uint16_t>(stereoPcmFormat, 1u);
	guest::store<uint16_t>(stereoPcmFormat + 2u, 2u);
	guest::store<uint32_t>(stereoPcmFormat + 4u, 22050u);
	guest::store<uint32_t>(stereoPcmFormat + 8u, 88200u);
	guest::store<uint16_t>(stereoPcmFormat + 12u, 4u);
	guest::store<uint16_t>(stereoPcmFormat + 14u, 16u);
	guest::store<uint16_t>(stereoPcmFormat + 16u, 0u);
	guest::store<uint8_t>(stereoBlock + 0u, 0u); // left predictor.
	guest::store<uint8_t>(stereoBlock + 1u, 0u); // right predictor.
	guest::store<uint16_t>(stereoBlock + 2u, 16u); // left delta.
	guest::store<uint16_t>(stereoBlock + 4u, 16u); // right delta.
	guest::store<int16_t>(stereoBlock + 6u, 2000); // left sample 1.
	guest::store<int16_t>(stereoBlock + 8u, -2000); // right sample 1.
	guest::store<int16_t>(stereoBlock + 10u, 1000); // left sample 2.
	guest::store<int16_t>(stereoBlock + 12u, -1000); // right sample 2.
	guest::store<uint8_t>(stereoBlock + 14u, 0x11u); // +1 left, +1 right.
	guest::store<uint8_t>(stereoBlock + 15u, 0xf0u); // -1 left, 0 right.
	guest::store<uint32_t>(stereoHeader, 84u);
	guest::store<uint32_t>(stereoHeader + 12u, stereoBlock);
	guest::store<uint32_t>(stereoHeader + 16u, 16u);
	guest::store<uint32_t>(stereoHeader + 28u, stereoDecoded);
	guest::store<uint32_t>(stereoHeader + 32u, 16u);
	const bool stereoOpened = soundDispatch(acmOpen,
		{acmHandleOut, 0u, stereoAdpcmFormat, stereoPcmFormat, 0u, 0u, 0u, 0u}) && cpu.r[EAX] == 0u;
	const uint32_t stereoHandle = stereoOpened ? guest::load<uint32_t>(acmHandleOut) : 0u;
	const int16_t stereoExpected[8] = {1000, -1000, 2000, -2000, 2016, -1984, 2000, -1984};
	const bool stereoConverted = stereoOpened &&
		soundDispatch(acmPrepare, {stereoHandle, stereoHeader, 0u}) && cpu.r[EAX] == 0u &&
		soundDispatch(acmConvert, {stereoHandle, stereoHeader, 0x30u}) && cpu.r[EAX] == 0u;
	bool stereoSamplesMatch = true;
	for (size_t index = 0; index < std::size(stereoExpected); ++index)
		stereoSamplesMatch = stereoSamplesMatch &&
			guest::load<int16_t>(stereoDecoded + static_cast<uint32_t>(index * 2u)) == stereoExpected[index];
	const bool stereoHeadersMatch = guest::load<uint32_t>(stereoHeader + 20u) == 16u &&
		guest::load<uint32_t>(stereoHeader + 36u) == 16u;
	const bool stereoClosed = stereoConverted &&
		soundDispatch(acmUnprepare, {stereoHandle, stereoHeader, 0u}) && cpu.r[EAX] == 0u &&
		soundDispatch(acmClose, {stereoHandle, 0u}) && cpu.r[EAX] == 0u;
	if (!stereoConverted || !stereoHeadersMatch || !stereoSamplesMatch || !stereoClosed) {
		std::fprintf(stderr, "Stereo MS-ADPCM regression failed: samples=%d,%d,%d,%d,%d,%d,%d,%d srcUsed=%u dstUsed=%u\n",
			guest::load<int16_t>(stereoDecoded), guest::load<int16_t>(stereoDecoded + 2u),
			guest::load<int16_t>(stereoDecoded + 4u), guest::load<int16_t>(stereoDecoded + 6u),
			guest::load<int16_t>(stereoDecoded + 8u), guest::load<int16_t>(stereoDecoded + 10u),
			guest::load<int16_t>(stereoDecoded + 12u), guest::load<int16_t>(stereoDecoded + 14u),
			guest::load<uint32_t>(stereoHeader + 20u), guest::load<uint32_t>(stereoHeader + 36u));
		return 134;
	}
	std::fprintf(stderr, "MS-ADPCM regression passed: mono predictor rounding and stereo channel interleave\n");
	// GetAsyncKeyState exposes mouse buttons in the same virtual-key space as
	// Windows. Verify the high bit while held and the low edge bit exactly once,
	// using the same mouse-message bridge used by host and Android input.
	std::string asyncKeyStubError;
	const uint32_t getAsyncKeyStateStub = const_cast<br::runtime::ImportBindingTable&>(runtime.imports())
		.getOrCreateDynamicStub("user32.dll", "GetAsyncKeyState", asyncKeyStubError);
	const auto* asyncKeyImport = runtime.imports().lookup(getAsyncKeyStateStub);
	if (!getAsyncKeyStateStub || !asyncKeyImport || !handlers.find(*asyncKeyImport)) {
		std::fprintf(stderr, "GetAsyncKeyState dynamic import has no handler: %s\n", asyncKeyStubError.c_str());
		return 128;
	}
	auto queryAsyncKeyState = [&](uint32_t virtualKey) {
		cpu.r[ESP] = 0x7ff0fed0u;
		guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
		guest::store<uint32_t>(cpu.r[ESP] + 4u, virtualKey);
		call_target(&cpu, getAsyncKeyStateStub);
		return cpu.r[EAX];
	};
	const struct MouseKeyCase { uint32_t virtualKey, downMessage, upMessage, buttonMask; const char* name; } mouseKeys[] = {
		{0x01u, 0x0201u, 0x0202u, 1u, "left"},
		{0x02u, 0x0204u, 0x0205u, 2u, "right"},
		{0x04u, 0x0207u, 0x0208u, 4u, "middle"}
	};
	for (const auto& mouseKey : mouseKeys) {
		if (queryAsyncKeyState(mouseKey.virtualKey) != 0u) {
			std::fprintf(stderr, "GetAsyncKeyState(%s) did not start released\n", mouseKey.name);
			return 129;
		}
		br::runtime::postGuestMouseMessage(mouseKey.downMessage, 24, 36, mouseKey.buttonMask);
		if (queryAsyncKeyState(mouseKey.virtualKey) != 0x8001u ||
			queryAsyncKeyState(mouseKey.virtualKey) != 0x8000u) {
			std::fprintf(stderr, "GetAsyncKeyState(%s) held/pressed edge mismatch\n", mouseKey.name);
			return 130;
		}
		br::runtime::postGuestMouseMessage(mouseKey.upMessage, 24, 36, 0u);
		if (queryAsyncKeyState(mouseKey.virtualKey) != 0x0000u ||
			queryAsyncKeyState(mouseKey.virtualKey) != 0x0000u) {
			std::fprintf(stderr, "GetAsyncKeyState(%s) release/edge-clear mismatch\n", mouseKey.name);
			return 131;
		}
		br::runtime::postGuestMouseMessage(mouseKey.downMessage, 24, 36, mouseKey.buttonMask);
		br::runtime::postGuestMouseMessage(mouseKey.upMessage, 24, 36, 0u);
		if (queryAsyncKeyState(mouseKey.virtualKey) != 0x0001u ||
			queryAsyncKeyState(mouseKey.virtualKey) != 0x0000u) {
			std::fprintf(stderr, "GetAsyncKeyState(%s) missed press/release edge between queries\n", mouseKey.name);
			return 132;
		}
	}
	std::fprintf(stderr, "USER32 input regression passed: GetAsyncKeyState left/right/middle held and edge states match mouse messages\n");
	// DefWindowProcA(WM_CLOSE) must run the same synchronous guest window
	// destruction callbacks as DestroyWindow, then return its own zero LRESULT.
	cpu.r[ESP] = 0x7ff0fe90u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x7ff0fb00u);
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0x7ff0fb40u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0x10cf0000u);
	guest::store<uint32_t>(cpu.r[ESP] + 20u, 10u);
	guest::store<uint32_t>(cpu.r[ESP] + 24u, 20u);
	guest::store<uint32_t>(cpu.r[ESP] + 28u, 640u);
	guest::store<uint32_t>(cpu.r[ESP] + 32u, 480u);
	guest::store<uint32_t>(cpu.r[ESP] + 36u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 40u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 44u, 0x00400000u);
	guest::store<uint32_t>(cpu.r[ESP] + 48u, 0x7ff0fa80u);
	call_target(&cpu, createWindowStub);
	const uint32_t closeWindow = cpu.r[EAX];
	const size_t closeMessageStart = windowCallbackMessages.size();
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, closeWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0x0010u); // WM_CLOSE.
	guest::store<uint32_t>(cpu.r[ESP] + 12u, 0u);
	guest::store<uint32_t>(cpu.r[ESP] + 16u, 0u);
	call_target(&cpu, defWindowProcStub);
	if (!closeWindow || cpu.r[EAX] != 0u || cpu.r[ESP] != 0x7ff0fee4u ||
		windowCallbackMessages.size() != closeMessageStart + 2u ||
		windowCallbackMessages[closeMessageStart] != 0x0002u ||
		windowCallbackMessages[closeMessageStart + 1u] != 0x0082u) {
		std::fprintf(stderr, "DefWindowProcA WM_CLOSE did not dispatch WM_DESTROY/WM_NCDESTROY or preserve its LRESULT\n");
		return 128;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, createdWindow);
	call_target(&cpu, destroyWindowStub);
	if (cpu.r[EAX] != 1u || cpu.r[ESP] != 0x7ff0fed8u) {
		std::fprintf(stderr, "DestroyWindow failed lifecycle callbacks or stack cleanup\n");
		return 100;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, closeWindow);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, guestRectAddress);
	call_target(&cpu, getClientRectStub);
	if (cpu.r[EAX] != 0u || br::runtime::currentWin32LastError() != 1400u) {
		std::fprintf(stderr, "DefWindowProcA WM_CLOSE did not invalidate the HWND\n");
		return 101;
	}

	// GetViewport is a dynamically-created Direct3D 7 COM method, so it is not
	// present in the PE's static import table. Verify its invalid-device and
	// invalid-output contracts without treating a generated stub as support.
	std::string getViewportStubError;
	const uint32_t getViewportStub = const_cast<br::runtime::ImportBindingTable&>(runtime.imports()).getOrCreateDynamicStub(
		"ddraw.dll", "IDirect3DDevice7::GetViewport", getViewportStubError);
	const auto* getViewportApi = runtime.imports().lookup(getViewportStub);
	if (!getViewportStub || !getViewportApi || !handlers.find(*getViewportApi)) {
		std::fprintf(stderr, "GetViewport dynamic COM method has no registered handler: %s\n",
			getViewportStubError.c_str());
		return 82;
	}
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	constexpr uint32_t viewportOutput = 0x7ff0d800u;
	for (uint32_t i = 0; i < 6u; ++i) guest::store<uint32_t>(viewportOutput + i * 4u, 0xccccccccu);
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0xdeadbeefu);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, viewportOutput);
	call_target(&cpu, getViewportStub);
	if (cpu.r[EAX] != kD3derrInvalidCall || cpu.r[ESP] != 0x7ff0fedcu ||
		guest::load<uint32_t>(viewportOutput) != 0xccccccccu) {
		std::fprintf(stderr, "GetViewport invalid-device contract or stdcall cleanup failed: hr=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 83;
	}
	cpu.r[ESP] = 0x7ff0fed0u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0xdeadbeefu);
	guest::store<uint32_t>(cpu.r[ESP] + 8u, 0u);
	call_target(&cpu, getViewportStub);
	if (cpu.r[EAX] != kD3derrInvalidCall || cpu.r[ESP] != 0x7ff0fedcu) {
		std::fprintf(stderr, "GetViewport invalid-output contract or stdcall cleanup failed: hr=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 84;
	}
	// Exercise the success path with the same COM object graph used by the
	// guest: DirectDraw7 -> IDirect3D7 -> render-target surface -> D3D device.
	// The previous tests covered only invalid arguments and could not prove
	// that the Android crash site's real device handle returned its viewport.
	const auto dynamicStub = [&](const char* library, const char* name) {
		std::string stubError;
		const uint32_t stub = const_cast<br::runtime::ImportBindingTable&>(runtime.imports())
			.getOrCreateDynamicStub(library, name, stubError);
		const auto* imported = runtime.imports().lookup(stub);
		return stub && imported && handlers.find(*imported) ? stub : 0u;
	};
	const auto dispatchStdcall = [&](uint32_t stub, std::initializer_list<uint32_t> args) {
		constexpr uint32_t stack = 0x7ff0d700u;
		cpu.r[ESP] = stack;
		guest::store<uint32_t>(stack, 0xffffffffu);
		uint32_t offset = 4u;
		for (uint32_t value : args) {
			guest::store<uint32_t>(stack + offset, value);
			offset += 4u;
		}
		call_target(&cpu, stub);
		return cpu.r[ESP] == stack + offset;
	};
	constexpr uint32_t iidDdAddress = 0x7ff0db00u;
	constexpr uint32_t iidD3dAddress = 0x7ff0db10u;
	constexpr uint32_t iidHalAddress = 0x7ff0db20u;
	auto writeGuid = [&](uint32_t address, const std::array<uint8_t, 16>& bytes) {
		for (uint32_t i = 0; i < bytes.size(); ++i) guest::store<uint8_t>(address + i, bytes[i]);
	};
	writeGuid(iidDdAddress, {{0xc0,0x5e,0xe6,0x15,0x9c,0x3b,0xd2,0x11,0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b}});
	writeGuid(iidD3dAddress, {{0x77,0x9e,0x04,0xf5,0x61,0x48,0xd2,0x11,0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8}});
	writeGuid(iidHalAddress, {{0xe0,0x3d,0xe6,0x84,0xaa,0x46,0xcf,0x11,0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e}});
	constexpr uint32_t ddOutput = 0x7ff0db40u, d3dOutput = 0x7ff0db44u;
	constexpr uint32_t surfaceDesc = 0x7ff0dc00u, surfaceOutput = 0x7ff0db48u;
	constexpr uint32_t deviceOutput = 0x7ff0db4cu, viewportRoundTrip = 0x7ff0dd00u;
	const uint32_t createDd = dynamicStub("ddraw.dll", "DirectDrawCreateEx");
	const uint32_t queryInterface = dynamicStub("ddraw.dll", "IDirectDraw7::QueryInterface");
	const uint32_t setDisplayMode = dynamicStub("ddraw.dll", "IDirectDraw7::SetDisplayMode");
	const uint32_t createSurface = dynamicStub("ddraw.dll", "IDirectDraw7::CreateSurface");
	const uint32_t createDevice = dynamicStub("ddraw.dll", "IDirect3D7::CreateDevice");
	const uint32_t createVertexBuffer = dynamicStub("ddraw.dll", "IDirect3D7::CreateVertexBuffer");
	const uint32_t lockVertexBuffer = dynamicStub("ddraw.dll", "IDirect3DVertexBuffer7::Lock");
	const uint32_t unlockVertexBuffer = dynamicStub("ddraw.dll", "IDirect3DVertexBuffer7::Unlock");
	const uint32_t setViewport = dynamicStub("ddraw.dll", "IDirect3DDevice7::SetViewport");
	const uint32_t drawPrimitive = dynamicStub("ddraw.dll", "IDirect3DDevice7::DrawPrimitive");
	const uint32_t drawPrimitiveVB = dynamicStub("ddraw.dll", "IDirect3DDevice7::DrawPrimitiveVB");
	const uint32_t getSurfaceDesc = dynamicStub("ddraw.dll", "IDirectDrawSurface7::GetSurfaceDesc");
	const uint32_t surfaceBlt = dynamicStub("ddraw.dll", "IDirectDrawSurface7::Blt");
	if (!createDd || !queryInterface || !setDisplayMode || !createSurface || !createDevice ||
		!createVertexBuffer || !lockVertexBuffer || !unlockVertexBuffer || !setViewport || !drawPrimitive || !drawPrimitiveVB ||
		!getSurfaceDesc || !surfaceBlt ||
		!dispatchStdcall(createDd, {0u, ddOutput, iidDdAddress, 0u}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(ddOutput) != 0x6c400000u) {
		std::fprintf(stderr, "DirectDraw7 setup for GetViewport success test failed\n");
		return 102;
	}
	if (!dispatchStdcall(queryInterface, {guest::load<uint32_t>(ddOutput), iidD3dAddress, d3dOutput}) ||
		cpu.r[EAX] != 0u || guest::load<uint32_t>(d3dOutput) != 0x6c400100u ||
		!dispatchStdcall(setDisplayMode, {guest::load<uint32_t>(ddOutput), 640u, 480u, 16u, 60u, 0u}) ||
		cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "Direct3D7 interface/display mode setup failed\n");
		return 103;
	}
	for (uint32_t i = 0; i < 124u; i += 4u) guest::store<uint32_t>(surfaceDesc + i, 0u);
	guest::store<uint32_t>(surfaceDesc, 124u); // DDSURFACEDESC2.
	guest::store<uint32_t>(surfaceDesc + 4u, 0x7u); // CAPS | HEIGHT | WIDTH.
	guest::store<uint32_t>(surfaceDesc + 8u, 480u);
	guest::store<uint32_t>(surfaceDesc + 12u, 640u);
	guest::store<uint32_t>(surfaceDesc + 104u, 0x200u); // DDSCAPS_PRIMARYSURFACE.
	if (!dispatchStdcall(createSurface,
		{guest::load<uint32_t>(ddOutput), surfaceDesc, surfaceOutput, 0u}) || cpu.r[EAX] != 0u ||
		!guest::load<uint32_t>(surfaceOutput)) {
		std::fprintf(stderr, "Primary DirectDraw surface setup for GetViewport failed: hr=%08x\n", cpu.r[EAX]);
		return 104;
	}
	if (!dispatchStdcall(createDevice,
		{guest::load<uint32_t>(d3dOutput), iidHalAddress, guest::load<uint32_t>(surfaceOutput), deviceOutput}) ||
		cpu.r[EAX] != 0u || guest::load<uint32_t>(deviceOutput) != 0x6c405000u) {
		std::fprintf(stderr, "Direct3D7 device setup for GetViewport failed: hr=%08x\n", cpu.r[EAX]);
		return 105;
	}
	const uint32_t d3dDevice = guest::load<uint32_t>(deviceOutput);
	// The Android startup trace failed at DrawPrimitive with a four-vertex
	// triangle strip (FVF XYZRHW|DIFFUSE|TEX1). Verify its COM dispatch, guest
	// vertex reads, topology conversion, D3D scene-state HRESULT, and stdcall
	// cleanup without pretending a draw outside BeginScene succeeded.
	constexpr uint32_t drawVertices = 0x7ff0de00u;
	for (uint32_t i = 0; i < 4u; ++i) {
		const uint32_t vertex = drawVertices + i * 28u;
		guest::store<float>(vertex, static_cast<float>(i & 1u));
		guest::store<float>(vertex + 4u, static_cast<float>((i >> 1u) & 1u));
		guest::store<float>(vertex + 8u, 0.5f);
		guest::store<float>(vertex + 12u, 1.0f);
		guest::store<uint32_t>(vertex + 16u, 0xffffffffu);
		guest::store<float>(vertex + 20u, static_cast<float>(i & 1u));
		guest::store<float>(vertex + 24u, static_cast<float>((i >> 1u) & 1u));
	}
	if (!dispatchStdcall(drawPrimitive, {d3dDevice, 5u, 0x144u, drawVertices, 4u, 0u}) ||
		cpu.r[EAX] != 0x887602f9u) {
		std::fprintf(stderr, "DrawPrimitive triangle-strip dispatch/topology or not-in-scene HRESULT mismatch: hr=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 111;
	}
	if (!dispatchStdcall(drawPrimitive, {d3dDevice, 5u, 0x144u, drawVertices, 4u, 2u}) ||
		cpu.r[EAX] != 0x8876086cu) {
		std::fprintf(stderr, "DrawPrimitive unsupported flags were not rejected: hr=%08x\n", cpu.r[EAX]);
		return 112;
	}
	// The game currently stops at this dynamically generated COM import. Test
	// the vertex-buffer form through the same non-indexed topology path, with
	// the real FVF/stride and x86 stdcall cleanup, and verify an invalid range.
	constexpr uint32_t vbDesc = 0x7ff0e200u;
	constexpr uint32_t vbOutput = 0x7ff0e220u;
	constexpr uint32_t vbDataOutput = 0x7ff0e224u;
	constexpr uint32_t vbSizeOutput = 0x7ff0e228u;
	guest::store<uint32_t>(vbDesc, 16u);
	guest::store<uint32_t>(vbDesc + 4u, 0u);
	guest::store<uint32_t>(vbDesc + 8u, 0x144u);
	guest::store<uint32_t>(vbDesc + 12u, 4u);
	if (!dispatchStdcall(createVertexBuffer, {guest::load<uint32_t>(d3dOutput), vbDesc, vbOutput, 0u}) ||
		cpu.r[EAX] != 0u || !guest::load<uint32_t>(vbOutput)) {
		std::fprintf(stderr, "DrawPrimitiveVB vertex-buffer creation failed: hr=%08x\n", cpu.r[EAX]);
		return 114;
	}
	const uint32_t vb = guest::load<uint32_t>(vbOutput);
	if (!dispatchStdcall(lockVertexBuffer, {vb, 0u, vbDataOutput, vbSizeOutput}) ||
		cpu.r[EAX] != 0u || guest::load<uint32_t>(vbSizeOutput) != 112u) {
		std::fprintf(stderr, "DrawPrimitiveVB vertex-buffer lock failed: hr=%08x\n", cpu.r[EAX]);
		return 115;
	}
	const uint32_t vbVertices = guest::load<uint32_t>(vbDataOutput);
	for (uint32_t i = 0; i < 4u; ++i) {
		const uint32_t vertex = vbVertices + i * 28u;
		guest::store<float>(vertex, static_cast<float>(i & 1u));
		guest::store<float>(vertex + 4u, static_cast<float>((i >> 1u) & 1u));
		guest::store<float>(vertex + 8u, 0.5f);
		guest::store<float>(vertex + 12u, 1.0f);
		guest::store<uint32_t>(vertex + 16u, 0xffffffffu);
		guest::store<float>(vertex + 20u, static_cast<float>(i & 1u));
		guest::store<float>(vertex + 24u, static_cast<float>((i >> 1u) & 1u));
	}
	if (!dispatchStdcall(unlockVertexBuffer, {vb}) || cpu.r[EAX] != 0u ||
		!dispatchStdcall(drawPrimitiveVB, {d3dDevice, 5u, vb, 0u, 4u, 0u}) ||
		cpu.r[EAX] != 0x887602f9u) {
		std::fprintf(stderr, "DrawPrimitiveVB dispatch/scene HRESULT/stdcall cleanup mismatch: hr=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 116;
	}
	if (!dispatchStdcall(drawPrimitiveVB, {d3dDevice, 5u, vb, 3u, 4u, 0u}) ||
		cpu.r[EAX] != 0x8876086cu) {
		std::fprintf(stderr, "DrawPrimitiveVB invalid vertex range was not rejected: hr=%08x\n", cpu.r[EAX]);
		return 117;
	}
	// Exercise DDBLT_WAIT with nonzero source/destination RECT origins and a
	// 2x stretch. The smoke verifies the output pixels, invalid-rect HRESULT,
	// dynamic COM binding, and x86 stdcall cleanup.
	constexpr uint32_t bltDestinationDesc = 0x7ff0df00u;
	constexpr uint32_t bltDestinationOutput = 0x7ff0db50u;
	constexpr uint32_t bltSourceSurfaceDesc = 0x7ff0df80u;
	constexpr uint32_t bltDestinationRect = 0x7ff0de80u;
	constexpr uint32_t bltSourceRect = 0x7ff0dea0u;
	guest::store<uint32_t>(bltSourceSurfaceDesc, 124u);
	if (!dispatchStdcall(getSurfaceDesc,
		{guest::load<uint32_t>(surfaceOutput), bltSourceSurfaceDesc}) || cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "GetSurfaceDesc for DirectDraw Blt source failed\n");
		return 113;
	}
	const uint32_t bltSourcePixelBase = guest::load<uint32_t>(bltSourceSurfaceDesc + 36u);
	const uint32_t bltSourcePitch = guest::load<uint32_t>(bltSourceSurfaceDesc + 16u);
	const uint16_t sourcePattern[4] = {0xf800u, 0x07e0u, 0x001fu, 0xffffu};
	for (uint32_t y = 0; y < 2u; ++y)
		for (uint32_t x = 0; x < 2u; ++x)
			guest::store<uint16_t>(bltSourcePixelBase + (9u + y) * bltSourcePitch + (8u + x) * 2u,
				sourcePattern[y * 2u + x]);
	for (uint32_t i = 0; i < 124u; i += 4u) guest::store<uint32_t>(bltDestinationDesc + i, 0u);
	guest::store<uint32_t>(bltDestinationDesc, 124u);
	guest::store<uint32_t>(bltDestinationDesc + 4u, 0x1007u); // CAPS | HEIGHT | WIDTH | PIXELFORMAT.
	guest::store<uint32_t>(bltDestinationDesc + 8u, 6u);
	guest::store<uint32_t>(bltDestinationDesc + 12u, 6u);
	guest::store<uint32_t>(bltDestinationDesc + 76u, 0x40u); // DDPF_RGB.
	guest::store<uint32_t>(bltDestinationDesc + 84u, 16u);
	guest::store<uint32_t>(bltDestinationDesc + 88u, 0xf800u);
	guest::store<uint32_t>(bltDestinationDesc + 92u, 0x07e0u);
	guest::store<uint32_t>(bltDestinationDesc + 96u, 0x001fu);
	guest::store<uint32_t>(bltDestinationDesc + 104u, 0x40u); // DDSCAPS_OFFSCREENPLAIN.
	if (!dispatchStdcall(createSurface,
		{guest::load<uint32_t>(ddOutput), bltDestinationDesc, bltDestinationOutput, 0u}) ||
		cpu.r[EAX] != 0u || !guest::load<uint32_t>(bltDestinationOutput)) {
		std::fprintf(stderr, "Offscreen DirectDraw surface setup for Blt failed: hr=%08x\n", cpu.r[EAX]);
		return 114;
	}
	guest::store<int32_t>(bltDestinationRect, 1);
	guest::store<int32_t>(bltDestinationRect + 4u, 1);
	guest::store<int32_t>(bltDestinationRect + 8u, 5);
	guest::store<int32_t>(bltDestinationRect + 12u, 5);
	guest::store<int32_t>(bltSourceRect, 8);
	guest::store<int32_t>(bltSourceRect + 4u, 9);
	guest::store<int32_t>(bltSourceRect + 8u, 10);
	guest::store<int32_t>(bltSourceRect + 12u, 11);
	if (!dispatchStdcall(surfaceBlt, {guest::load<uint32_t>(bltDestinationOutput),
		bltDestinationRect, guest::load<uint32_t>(surfaceOutput), bltSourceRect, 0x01000000u, 0u}) ||
		cpu.r[EAX] != 0u) {
		std::fprintf(stderr, "DDBLT_WAIT source copy or x86 stdcall cleanup failed: hr=%08x ESP=%08x\n",
			cpu.r[EAX], cpu.r[ESP]);
		return 115;
	}
	guest::store<uint32_t>(bltDestinationDesc, 124u);
	if (!dispatchStdcall(getSurfaceDesc,
		{guest::load<uint32_t>(bltDestinationOutput), bltDestinationDesc}) || cpu.r[EAX] != 0u) return 116;
	const uint32_t bltDestinationPixelBase = guest::load<uint32_t>(bltDestinationDesc + 36u);
	const uint32_t bltDestinationPitch = guest::load<uint32_t>(bltDestinationDesc + 16u);
	for (uint32_t y = 0; y < 4u; ++y) {
		for (uint32_t x = 0; x < 4u; ++x) {
			const uint16_t expected = sourcePattern[(y / 2u) * 2u + x / 2u];
			const uint16_t observed = guest::load<uint16_t>(bltDestinationPixelBase +
				(1u + y) * bltDestinationPitch + (1u + x) * 2u);
			if (observed != expected) {
				std::fprintf(stderr, "DDBLT_WAIT scaling/pixel copy mismatch at %u,%u: expected=%04x got=%04x\n",
					x, y, expected, observed);
				return 117;
			}
		}
	}
	guest::store<int32_t>(bltSourceRect + 8u, 641);
	if (!dispatchStdcall(surfaceBlt, {guest::load<uint32_t>(bltDestinationOutput),
		bltDestinationRect, guest::load<uint32_t>(surfaceOutput), bltSourceRect, 0u, 0u}) ||
		cpu.r[EAX] != 0x88760096u) {
		std::fprintf(stderr, "DirectDraw Blt invalid source rectangle did not return DDERR_INVALIDRECT: %08x\n", cpu.r[EAX]);
		return 118;
	}
	std::puts("DirectDraw Blt regression passed: DDBLT_WAIT, scaled 16-bit copy, RECT offsets, invalid-rect HRESULT, and x86 stack cleanup");
	if (!dispatchStdcall(getViewportStub, {d3dDevice, viewportRoundTrip}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(viewportRoundTrip) != 0u ||
		guest::load<uint32_t>(viewportRoundTrip + 4u) != 0u ||
		guest::load<uint32_t>(viewportRoundTrip + 8u) != 640u ||
		guest::load<uint32_t>(viewportRoundTrip + 12u) != 480u ||
		guest::load<uint32_t>(viewportRoundTrip + 16u) != 0u ||
		guest::load<uint32_t>(viewportRoundTrip + 20u) != 0x3f800000u) {
		std::fprintf(stderr, "GetViewport failed to return the created device's default viewport\n");
		return 106;
	}
	guest::store<uint32_t>(viewportRoundTrip, 4u);
	guest::store<uint32_t>(viewportRoundTrip + 4u, 5u);
	guest::store<uint32_t>(viewportRoundTrip + 8u, 320u);
	guest::store<uint32_t>(viewportRoundTrip + 12u, 240u);
	guest::store<uint32_t>(viewportRoundTrip + 16u, 0x3e4ccccdu); // 0.2f.
	guest::store<uint32_t>(viewportRoundTrip + 20u, 0x3f4ccccdu); // 0.8f.
	if (!dispatchStdcall(setViewport, {d3dDevice, viewportRoundTrip}) || cpu.r[EAX] != 0u) return 107;
	for (uint32_t i = 0; i < 6u; ++i) guest::store<uint32_t>(viewportRoundTrip + i * 4u, 0u);
	if (!dispatchStdcall(getViewportStub, {d3dDevice, viewportRoundTrip}) || cpu.r[EAX] != 0u ||
		guest::load<uint32_t>(viewportRoundTrip) != 4u ||
		guest::load<uint32_t>(viewportRoundTrip + 4u) != 5u ||
		guest::load<uint32_t>(viewportRoundTrip + 8u) != 320u ||
		guest::load<uint32_t>(viewportRoundTrip + 12u) != 240u ||
		guest::load<uint32_t>(viewportRoundTrip + 16u) != 0x3e4ccccdu ||
		guest::load<uint32_t>(viewportRoundTrip + 20u) != 0x3f4ccccdu) {
		std::fprintf(stderr, "GetViewport did not preserve SetViewport state\n");
		return 108;
	}

	const uint32_t exitProcessStub = iatStub("ExitProcess");
	cpu.r[ESP] = 0x7ff0ffe4u;
	guest::store<uint32_t>(cpu.r[ESP], 0xffffffffu);
	guest::store<uint32_t>(cpu.r[ESP] + 4u, 0u);
	bool guestExitCaught = false;
	try {
		call_target(&cpu, exitProcessStub);
	} catch (const br::runtime::GuestProcessExit& exit) {
		guestExitCaught = exit.code == 0u;
	}
	if (!guestExitCaught) {
		std::fprintf(stderr, "ExitProcess did not signal a guest exit with code 0\n");
		return 46;
	}

	unsigned handledStubs = 0;
	for (const auto& stub : runtime.imports().apiStubs())
		if (handlers.find(stub)) ++handledStubs;
	std::printf("main PE prepare ok: image=0x%08x entry=0x%08x imports=%zu ordinal_imports=%u ordinal_IAT_bound=%u api_stubs=%u handled_stubs=%u startup_APIs=GetVersion,GetVersionExA,GetCommandLineA,GetStartupInfoA,GetModuleHandleA,GetEnvironmentStringsA/W,GetStdHandle,GetACP,GetCPInfo,GetStringTypeW,LCMapStringW,MultiByteToWideChar,WideCharToMultiByte,FindFirstFileA,FindNextFileA,FindClose,CreateMutexA,WaitForSingleObject,ReleaseMutex,FindWindowA,RegisterWindowMessageA,LoadIconA,RegisterClassA,CreateWindowExA,ExitProcess guest_exit_test=passed GetVersion=0x%08x result=0x%08x ESP=0x%08x FSBASE=0x%08x minimal_TEB=initialized entrypoint_execution=not_run\n",
		runtime.image().loadBase, runtime.image().entryAddress(), runtime.image().imports.size(),
		ordinalImportReferences, boundOrdinalImportReferences,
		runtime.stats().uniqueApiStubs, handledStubs, getVersionStub, getVersionResult, cpu.r[ESP], cpu.fsbase);
	return 0;
}
