#include "win32_startup.h"

#include "guest.h"
#include "ra3_guest_bridge.h"
#include "win32_base.h"
#include "android_diagnostics.h"
#include "d3d7_fixed_function.h"
#include "d3d7_gpu_backend.h"
#include "d3d7_scanline.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace br::runtime {
namespace {
#if defined(__ANDROID__)
std::atomic<uint64_t> gProfileD3dDrawCalls{0};
std::atomic<uint64_t> gProfileD3dRasterNs{0};
std::atomic<uint64_t> gProfileD3dSetupNs{0};
std::atomic<uint64_t> gProfileD3dGeometryNs{0};
std::atomic<uint64_t> gProfileD3dGpuSubmitNs{0};
std::atomic<uint64_t> gProfileD3dFallbackNs{0};
std::atomic<uint64_t> gProfileD3dSyncReadbackNs{0};
std::atomic<uint64_t> gProfileDdrawFlips{0};
std::atomic<uint64_t> gProfileGpuDrawCalls{0};
std::atomic<uint64_t> gProfileSoftwareFallbackDrawCalls{0};
std::atomic<uint64_t> gProfileGpuTriangles{0};
std::atomic<uint64_t> gProfileSoftwareFallbackTriangles{0};
std::atomic<bool> gLoggedD3d7MultiStageAccepted{false};
std::atomic<bool> gLoggedFvf2c2Completed{false};
std::atomic<uint32_t> gAudioAcmOpenLogCount{0};
std::atomic<uint32_t> gAudioAcmConvertLogCount{0};
std::atomic<uint32_t> gAudioDsoundCreateLogCount{0};
std::atomic<uint32_t> gAudioDsoundPlayLogCount{0};
std::atomic<uint32_t> gAudioDsoundLockLogCount{0};

bool allowAudioTraceLog(std::atomic<uint32_t>& count, uint32_t initialLimit, uint32_t periodicInterval = 0u)
{
	const uint32_t index = count.fetch_add(1u, std::memory_order_relaxed);
	return index < initialLimit || (periodicInterval && index % periodicInterval == 0u);
}

struct D3dDrawProfileScope {
	using Clock = std::chrono::steady_clock;
	Clock::time_point start = Clock::now();
	uint64_t setupNanoseconds = 0u;
	uint64_t geometryNanoseconds = 0u;
	uint64_t gpuSubmitNanoseconds = 0u;
	uint64_t fallbackNanoseconds = 0u;
	uint64_t syncReadbackNanoseconds = 0u;
	~D3dDrawProfileScope()
	{
		const auto elapsed = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
			Clock::now() - start).count());
		gProfileD3dDrawCalls.fetch_add(1u, std::memory_order_relaxed);
		gProfileD3dRasterNs.fetch_add(elapsed, std::memory_order_relaxed);
		gProfileD3dSetupNs.fetch_add(setupNanoseconds, std::memory_order_relaxed);
		gProfileD3dGeometryNs.fetch_add(geometryNanoseconds, std::memory_order_relaxed);
		gProfileD3dGpuSubmitNs.fetch_add(gpuSubmitNanoseconds, std::memory_order_relaxed);
		gProfileD3dFallbackNs.fetch_add(fallbackNanoseconds, std::memory_order_relaxed);
		gProfileD3dSyncReadbackNs.fetch_add(syncReadbackNanoseconds, std::memory_order_relaxed);
	}
};
#endif

struct StartupContext {
	uint32_t mainModuleBase = 0;
	uint32_t commandLineAddress = 0;
	ImportBindingTable* imports = nullptr;
	uint32_t screenWidth = 0;
	uint32_t screenHeight = 0;
	uint32_t userDefaultLcid = 0x0409u;
	uint32_t ansiCodePage = 1252u;
	std::string mainModuleName;
};

std::mutex gStartupContextMutex;
StartupContext gStartupContext;
std::mutex gRegisteredMessagesMutex;
std::map<std::string, uint32_t> gRegisteredMessagesByName;
std::map<uint32_t, std::string> gRegisteredMessageNamesById;
struct RegisteredWindowClass {
	uint16_t atom = 0;
	uint32_t windowProcedure = 0;
	uint32_t style = 0;
	uint32_t instance = 0;
	uint32_t icon = 0;
	uint32_t cursor = 0;
	uint32_t background = 0;
};
struct GuestDdrawSurface {
	uint32_t handle = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t pitch = 0;
	uint32_t bpp = 0;
	uint32_t caps = 0;
	uint32_t pixelFlags = 0;
	uint32_t redMask = 0;
	uint32_t greenMask = 0;
	uint32_t blueMask = 0;
	uint32_t alphaMask = 0;
	uint32_t pixels = 0;
	uint32_t bytes = 0;
	uint32_t references = 1;
	uint32_t backBuffer = 0;
	uint32_t zBuffer = 0;
	uint64_t cpuRevision = 1u;
};
struct GuestD3d7Light {
	std::array<uint32_t, 26> values{}; // D3DLIGHT7 (104 bytes).
	bool configured = false;
	bool enabled = false;
};
struct GuestD3d7Device {
	uint32_t handle = 0;
	uint32_t renderTarget = 0;
	uint32_t references = 1;
	bool inScene = false;
	std::array<uint32_t, 8> textures{};
	std::array<uint32_t, 6> viewport{};
	std::array<uint32_t, 256> renderState{};
	std::array<std::array<uint32_t, 32>, 8> textureStageState{};
	std::array<std::array<uint32_t, 16>, 24> transforms{};
	std::array<uint32_t, 21> material{}; // D3DMATERIAL7 (84 bytes).
	std::array<GuestD3d7Light, 8> lights{};
};
struct GuestD3d7VertexBuffer {
	uint32_t handle = 0;
	uint32_t references = 1;
	uint32_t data = 0;
	uint32_t size = 0;
	uint32_t fvf = 0;
	uint32_t caps = 0;
	uint32_t vertices = 0;
	uint32_t stride = 0;
	bool locked = false;
};
struct GuestDirectInput {
	uint32_t handle = 0;
	uint32_t references = 1;
};
struct GuestDirectInputDevice {
	uint32_t handle = 0;
	uint32_t references = 1;
	uint32_t kind = 0; // 1 = system mouse, 2 = system keyboard.
	uint32_t dataFormat = 0;
	uint32_t dataSize = 0;
	uint32_t cooperativeWindow = 0;
	uint32_t cooperativeFlags = 0;
	uint32_t bufferSize = 0;
	uint32_t notificationEvent = 0;
	bool absoluteMouse = false;
	bool acquired = false;
};
struct GuestDirectSound {
	uint32_t handle = 0;
	uint32_t references = 1;
	uint32_t cooperativeWindow = 0;
	uint32_t cooperativeLevel = 0;
	std::array<uint32_t, 16> listenerParameters{};
	bool initialized = true;
};
struct GuestDirectSoundBuffer {
	uint32_t handle = 0;
	uint32_t parent = 0;
	uint32_t listenerHandle = 0;
	uint32_t references = 1;
	uint32_t flags = 0;
	uint32_t bytes = 0;
	uint32_t sampleRate = 0;
	uint32_t averageBytesPerSecond = 0;
	uint16_t channels = 0;
	uint16_t blockAlign = 0;
	uint16_t bitsPerSample = 0;
	uint32_t data = 0;
	uint32_t playCursor = 0;
	uint32_t writeCursor = 0;
	uint32_t lockOffset = 0;
	uint32_t lockBytes = 0;
	uint32_t volume = 0;
	uint32_t pan = 0;
	uint32_t frequency = 0;
	uint32_t playFlags = 0;
	double playbackPhase = 0.0;
	bool primary = false;
	bool playing = false;
	bool locked = false;
};
struct GuestDirectSound3DListener {
	uint32_t handle = 0;
	uint32_t parentBuffer = 0;
};
struct GuestAcmStream {
	uint16_t sourceTag = 0;
	uint16_t destinationTag = 0;
	uint16_t channels = 0;
	uint16_t sourceBlockAlign = 0;
	uint16_t destinationBlockAlign = 0;
	uint16_t samplesPerBlock = 0;
	std::vector<std::pair<int16_t, int16_t>> coefficients;
};
std::mutex gAcmStreamsMutex;
std::map<uint32_t, GuestAcmStream> gAcmStreams;
uint32_t gNextAcmStreamHandle = 0x6e000000u;
std::mutex gWindowClassesMutex;
std::map<std::string, RegisteredWindowClass> gWindowClasses;
std::map<uint16_t, std::string> gWindowClassNamesByAtom;
uint16_t gNextWindowClassAtom = 1u;
struct GuestWindow {
	uint32_t handle = 0;
	std::string className;
	std::string title;
	uint32_t windowProcedure = 0;
	uint32_t style = 0;
	uint32_t extendedStyle = 0;
	uint32_t x = 0, y = 0, width = 0, height = 0;
	uint32_t parent = 0, menu = 0, instance = 0, createParameter = 0;
	std::map<int32_t, uint32_t> extraLongs;
	bool enabled = true;
	bool updateRegionInvalid = false;
};
std::map<uint32_t, GuestWindow> gGuestWindows;
struct GuestWindowFrameInsets { uint32_t left = 0, top = 0, right = 0, bottom = 0; };

GuestWindowFrameInsets guestWindowFrameInsets(uint32_t style, uint32_t extendedStyle,
	bool hasMenu)
{
	constexpr uint32_t kWsPopup = 0x80000000u;
	constexpr uint32_t kWsCaption = 0x00c00000u;
	constexpr uint32_t kWsThickFrame = 0x00040000u;
	constexpr uint32_t kWsBorder = 0x00800000u;
	constexpr uint32_t kWsDlgFrame = 0x00400000u;
	constexpr uint32_t kWsExClientEdge = 0x00000200u;
	constexpr uint32_t kWsExWindowEdge = 0x00000100u;
	GuestWindowFrameInsets result{};
	if (style & kWsPopup) return result;
	uint32_t frame = 0u;
	if (style & kWsThickFrame) frame = 8u;
	else if (style & (kWsBorder | kWsDlgFrame)) frame = 1u;
	if (extendedStyle & (kWsExClientEdge | kWsExWindowEdge)) frame += 2u;
	result.left = result.right = result.bottom = frame;
	result.top = frame + ((style & kWsCaption) ? 23u : 0u) + (hasMenu ? 19u : 0u);
	return result;
}
struct GuestMessage {
	uint32_t window = 0;
	uint32_t message = 0;
	uint32_t wParam = 0;
	uint32_t lParam = 0;
	uint32_t time = 0;
	int32_t x = 0;
	int32_t y = 0;
};
uint64_t gSynthesizedPaintMessageCount = 0;
uint32_t gTracedPaintCallbackCount = 0;
uint32_t gTracedGetMessageCallCount = 0;
uint32_t gTracedDefWindowProcCallCount = 0;
uint32_t gTracedSendMessageCallCount = 0;
std::mutex gGuestMessageMutex;
std::condition_variable gGuestMessageAvailable;
std::deque<GuestMessage> gGuestMessageQueue;
uint32_t guestMessageTime();

bool findInvalidPaintMessage(uint32_t filterWindow, uint32_t minMessage,
	uint32_t maxMessage, GuestMessage& result)
{
	if ((minMessage || maxMessage) &&
		(0x000fu < minMessage || 0x000fu > maxMessage)) return false;
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	for (const auto& item : gGuestWindows) {
		const GuestWindow& window = item.second;
		if (!window.updateRegionInvalid || !(window.style & 0x10000000u)) continue;
		if (filterWindow == 0xffffffffu) continue; // Thread messages only.
		if (filterWindow && window.handle != filterWindow) continue;
		result = {};
		result.window = window.handle;
		result.message = 0x000fu;
		result.time = guestMessageTime();
		++gSynthesizedPaintMessageCount;
		if (const char* value = std::getenv("BR_PAINT_DIAGNOSTIC_LIMIT")) {
			char* end = nullptr;
			const unsigned long limit = std::strtoul(value, &end, 10);
			if (end != value && *end == '\0' && limit &&
				gSynthesizedPaintMessageCount >= limit)
				throw std::runtime_error("WM_PAINT diagnostic message limit reached");
		}
		return true;
	}
	return false;
}
const auto gGuestMessageEpoch = std::chrono::steady_clock::now();
std::mutex gDdrawSurfacesMutex;
std::mutex gFramePresenterMutex;
Win32FramePresenter gFramePresenter = nullptr;
std::unordered_map<uint32_t, GuestDdrawSurface> gDdrawSurfaces;
using PixelDecodeLut = std::array<d3d7::BlendColor, 1u << 16u>;
std::mutex gPixelDecodeLutsMutex;
std::unordered_map<uint64_t, std::unique_ptr<PixelDecodeLut>> gPixelDecodeLuts;

const d3d7::BlendColor* pixelDecodeLut(const d3d7::PixelFormatLayout& format)
{
	if (format.bytesPerPixel != 2u) return nullptr;
	const uint64_t key = uint64_t(format.red.mask) | (uint64_t(format.green.mask) << 16u) |
		(uint64_t(format.blue.mask) << 32u) | (uint64_t(format.alpha.mask) << 48u);
	std::lock_guard<std::mutex> lock(gPixelDecodeLutsMutex);
	const auto found = gPixelDecodeLuts.find(key);
	if (found != gPixelDecodeLuts.end()) return found->second->data();
	auto table = std::make_unique<PixelDecodeLut>();
	for (uint32_t pixel = 0u; pixel < table->size(); ++pixel) {
		const auto decoded = d3d7::decodePixel(pixel, format);
		(*table)[pixel] = {decoded[0], decoded[1], decoded[2], decoded[3]};
	}
	const auto* result = table->data();
	gPixelDecodeLuts.emplace(key, std::move(table));
	return result;
}
uint32_t gGuestPrimarySurface = 0u;
uint64_t gGuestPresentationCount = 0u;
std::mutex gD3d7DevicesMutex;
std::unordered_map<uint32_t, GuestD3d7Device> gD3d7Devices;
std::mutex gD3d7VertexBuffersMutex;
std::unordered_map<uint32_t, GuestD3d7VertexBuffer> gD3d7VertexBuffers;
std::mutex gDirectInputMutex;
std::unordered_map<uint32_t, GuestDirectInput> gDirectInputs;
std::unordered_map<uint32_t, GuestDirectInputDevice> gDirectInputDevices;
std::mutex gDirectSoundMutex;
std::unordered_map<uint32_t, GuestDirectSound> gDirectSounds;
std::unordered_map<uint32_t, GuestDirectSoundBuffer> gDirectSoundBuffers;
std::unordered_map<uint32_t, GuestDirectSound3DListener> gDirectSound3DListeners;
std::array<uint8_t, 256> gDirectInputKeyboardState{};
std::array<uint8_t, 256> gAsyncKeyPressedSinceQuery{};
std::array<uint8_t, 3> gDirectInputMouseButtons{};
int32_t gDirectInputMouseX = 0;
int32_t gDirectInputMouseY = 0;
int32_t gDirectInputMouseWheel = 0;
int32_t gDirectInputPointerX = 0;
int32_t gDirectInputPointerY = 0;
int32_t gLastGuestMouseX = 0;
int32_t gLastGuestMouseY = 0;
bool gHaveGuestMousePosition = false;
uint32_t gNextDdrawSurface = 0x6c440000u;
uint32_t gNextDdrawPixels = 0x6c500000u;
uint32_t gNextD3d7VertexBuffer = 0x6c470000u;
uint32_t gNextD3d7VertexBufferData = 0x6d000000u;
std::mutex gLoadedModulesMutex;
uint32_t gDdrawLibraryReferences = 0u;
uint32_t gDinputLibraryReferences = 0u;
uint32_t gDsoundLibraryReferences = 0u;
bool loadGuestSurface(uint32_t handle, GuestDdrawSurface& surface);
d3d7::GpuSurface gpuSurfaceView(const GuestDdrawSurface& surface);
void synchronizeSurfaceForCpu(uint32_t handle);
void markSurfaceCpuWrite(uint32_t handle);
bool callWindowProcedure(CPU* cpu, uint32_t procedure, uint32_t window,
	uint32_t message, uint32_t wParam, uint32_t lParam, uint32_t& result);

void enqueueGuestMessage(const GuestMessage& message)
{
	{
		std::lock_guard<std::mutex> lock(gGuestMessageMutex);
		gGuestMessageQueue.push_back(message);
	}
	gGuestMessageAvailable.notify_one();
}

uint32_t guestMessageTime()
{
	const auto elapsed = std::chrono::steady_clock::now() - gGuestMessageEpoch;
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}
uint32_t gNextGuestWindow = 0x6c100000u;
uint32_t gForegroundGuestWindow = 0;
uint32_t gActiveGuestWindow = 0;
bool gGuestApplicationActive = false;
int32_t gCursorDisplayCount = 0;
constexpr uint32_t kCreateStructBase = 0x6c200000u;
constexpr uint32_t kWindowPosAddress = 0x6c210000u;
std::once_flag gCreateStructMemoryOnce;
bool gCreateStructMemoryReady = false;
constexpr uint32_t kEnvironmentAnsi = 0x6fff0000u;
constexpr uint32_t kEnvironmentWide = 0x6fff1000u;

StartupContext startupContext()
{
	std::lock_guard<std::mutex> lock(gStartupContextMutex);
	return gStartupContext;
}

uint32_t argument(CPU* cpu, uint32_t index)
{
	return guest::load<uint32_t>(cpu->r[ESP] + 4u + index * 4u);
}

bool ensureEnvironmentBlocks()
{
	static std::once_flag once;
	static bool ready = false;
	std::call_once(once, [] {
		ready = guest::commit(kEnvironmentAnsi, 0x2000u);
		if (ready) std::memset(guest::host(kEnvironmentAnsi), 0, 0x2000u);
	});
	return ready;
}

void getEnvironmentStringsA(CPU* cpu, const ApiImportStub&)
{
	if (!ensureEnvironmentBlocks()) {
		cpu->r[EAX] = 0;
		setWin32LastError(8u); // ERROR_NOT_ENOUGH_MEMORY
		return;
	}
	cpu->r[EAX] = kEnvironmentAnsi;
}

void getEnvironmentStringsW(CPU* cpu, const ApiImportStub&)
{
	if (!ensureEnvironmentBlocks()) {
		cpu->r[EAX] = 0;
		setWin32LastError(8u);
		return;
	}
	cpu->r[EAX] = kEnvironmentWide;
}

void freeEnvironmentStringsA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t block = argument(cpu, 0);
	if (block == kEnvironmentAnsi) {
		cpu->r[EAX] = 1u;
		return;
	}
	cpu->r[EAX] = 0;
	setWin32LastError(87u); // ERROR_INVALID_PARAMETER
}

void freeEnvironmentStringsW(CPU* cpu, const ApiImportStub&)
{
	const uint32_t block = argument(cpu, 0);
	if (block == kEnvironmentWide) {
		cpu->r[EAX] = 1u;
		return;
	}
	cpu->r[EAX] = 0;
	setWin32LastError(87u);
}

void getStdHandle(CPU* cpu, const ApiImportStub&)
{
	const uint32_t selector = argument(cpu, 0);
	if (selector >= 0xfffffff4u && selector <= 0xfffffff6u)
		cpu->r[EAX] = 0x10u + (0xfffffff6u - selector);
	else {
		cpu->r[EAX] = 0xffffffffu;
		setWin32LastError(87u);
	}
}

void setHandleCount(CPU* cpu, const ApiImportStub&)
{
	// The CRT requests its descriptor-table capacity during process startup.
	cpu->r[EAX] = argument(cpu, 0);
}

void getAcp(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = startupContext().ansiCodePage;
}

void isDbcsLeadByteEx(CPU* cpu, const ApiImportStub&)
{
	uint32_t codePage = argument(cpu, 0);
	const uint8_t byte = static_cast<uint8_t>(argument(cpu, 1));
	if (codePage == 0u || codePage == 3u) codePage = startupContext().ansiCodePage;
	else if (codePage == 1u) codePage = 437u; // CP_OEMCP in this Windows compatibility profile.
	else if (codePage == 2u) codePage = 10000u; // CP_MACCP (single-byte Mac Roman).

	bool leadByte = false;
	switch (codePage) {
	case 932u:
		leadByte = (byte >= 0x81u && byte <= 0x9fu) || (byte >= 0xe0u && byte <= 0xfcu);
		break;
	case 936u:
	case 949u:
	case 950u:
		leadByte = byte >= 0x81u && byte <= 0xfeu;
		break;
	case 1361u:
		leadByte = (byte >= 0x84u && byte <= 0xd3u) ||
			(byte >= 0xd8u && byte <= 0xdeu) || (byte >= 0xe0u && byte <= 0xf9u);
		break;
	default:
		// IsDBCSLeadByteEx validates lead-byte ranges only for the five
		// Windows DBCS code pages above; SBCS and UTF-8 return FALSE.
		break;
	}
	cpu->r[EAX] = leadByte ? 1u : 0u;
}

void getCPInfo(CPU* cpu, const ApiImportStub&)
{
	const uint32_t codePage = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output) {
		cpu->r[EAX] = 0;
		setWin32LastError(998u); // ERROR_NOACCESS
		return;
	}
	const uint32_t effectiveCodePage = codePage ? codePage : 1252u;
	uint32_t maxCharSize = 1u;
	if (effectiveCodePage == 65001u) maxCharSize = 4u;
	else if (effectiveCodePage != 1252u && effectiveCodePage != 437u) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u); // ERROR_INVALID_PARAMETER
		return;
	}
	guest::store<uint32_t>(output, maxCharSize);
	guest::store<uint8_t>(output + 4u, static_cast<uint8_t>('?'));
	guest::store<uint8_t>(output + 5u, 0u);
	for (uint32_t i = 0; i < 12u; ++i) guest::store<uint8_t>(output + 6u + i, 0u);
	cpu->r[EAX] = 1u;
}

uint16_t getCtype1(uint16_t ch)
{
	constexpr uint16_t upper = 0x0001u, lower = 0x0002u, digit = 0x0004u;
	constexpr uint16_t space = 0x0008u, punct = 0x0010u, control = 0x0020u;
	constexpr uint16_t blank = 0x0040u, xdigit = 0x0080u, alpha = 0x0100u;
	uint16_t type = 0;
	const bool isUpper = (ch >= 'A' && ch <= 'Z') ||
		(ch >= 0x00c0u && ch <= 0x00deu && ch != 0x00d7u);
	const bool isLower = (ch >= 'a' && ch <= 'z') || ch == 0x00dfu ||
		(ch >= 0x00e0u && ch <= 0x00ffu && ch != 0x00f7u);
	if (isUpper) type |= upper | alpha;
	if (isLower) type |= lower | alpha;
	if (ch >= '0' && ch <= '9') type |= digit;
	if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F') ||
		(ch >= 'a' && ch <= 'f')) type |= xdigit;
	if (ch == ' ' || (ch >= '\t' && ch <= '\r') || ch == 0x00a0u) type |= space;
	if (ch == ' ' || ch == '\t') type |= blank;
	if (ch < 0x20u || (ch >= 0x7fu && ch <= 0x9fu)) type |= control;
	const bool asciiPunct = (ch >= 0x21u && ch <= 0x2fu) || (ch >= 0x3au && ch <= 0x40u) ||
		(ch >= 0x5bu && ch <= 0x60u) || (ch >= 0x7bu && ch <= 0x7eu);
	if (asciiPunct || (ch >= 0x00a1u && ch <= 0x00bfu && ch != 0x00aau && ch != 0x00bau))
		type |= punct;
	return type;
}

void getStringTypeW(CPU* cpu, const ApiImportStub&)
{
	const uint32_t infoType = argument(cpu, 0);
	const uint32_t source = argument(cpu, 1);
	const int32_t requested = static_cast<int32_t>(argument(cpu, 2));
	const uint32_t output = argument(cpu, 3);
	if (infoType != 1u || !source || !output || requested < -1) {
		cpu->r[EAX] = 0;
		setWin32LastError(!source || !output ? 998u : 87u);
		return;
	}
	const uint32_t count = requested == -1 ? 0x10000u : static_cast<uint32_t>(requested);
	uint32_t processed = 0;
	for (; processed < count; ++processed) {
		const uint16_t ch = guest::load<uint16_t>(source + processed * 2u);
		guest::store<uint16_t>(output + processed * 2u, getCtype1(ch));
		if (!ch && requested == -1) { ++processed; break; }
	}
	if (requested == -1 && processed == count) {
		cpu->r[EAX] = 0;
		setWin32LastError(1113u); // ERROR_NO_UNICODE_TRANSLATION
		return;
	}
	cpu->r[EAX] = 1u;
}

void lcMapStringW(CPU* cpu, const ApiImportStub&)
{
	const uint32_t flags = argument(cpu, 1);
	const uint32_t source = argument(cpu, 2);
	const int32_t requested = static_cast<int32_t>(argument(cpu, 3));
	const uint32_t destination = argument(cpu, 4);
	const uint32_t destinationCapacity = argument(cpu, 5);
	const bool lowerCase = (flags & 0x100u) != 0;
	const bool upperCase = (flags & 0x200u) != 0;
	if (!source || requested < -1 || (!lowerCase && !upperCase) || (lowerCase && upperCase) ||
		(flags & ~(0x100u | 0x200u)) != 0u) {
		cpu->r[EAX] = 0;
		setWin32LastError(!source ? 998u : 87u);
		return;
	}
	const uint32_t count = requested == -1 ? 0x10000u : static_cast<uint32_t>(requested);
	std::vector<uint16_t> mapped;
	bool terminated = false;
	for (uint32_t i = 0; i < count; ++i) {
		uint16_t ch = guest::load<uint16_t>(source + i * 2u);
		if (!ch && requested == -1) { mapped.push_back(0u); terminated = true; break; }
		if (lowerCase) {
			if ((ch >= 'A' && ch <= 'Z') || (ch >= 0x00c0u && ch <= 0x00d6u) ||
				(ch >= 0x00d8u && ch <= 0x00deu)) ch = static_cast<uint16_t>(ch + 0x20u);
			else if (ch == 0x0178u) ch = 0x00ffu;
		} else {
			if ((ch >= 'a' && ch <= 'z') || (ch >= 0x00e0u && ch <= 0x00f6u) ||
				(ch >= 0x00f8u && ch <= 0x00feu)) ch = static_cast<uint16_t>(ch - 0x20u);
			else if (ch == 0x00ffu) ch = 0x0178u;
		}
		mapped.push_back(ch);
	}
	if (requested == -1 && !terminated) {
		cpu->r[EAX] = 0;
		setWin32LastError(1113u);
		return;
	}
	if (!destinationCapacity) {
		cpu->r[EAX] = static_cast<uint32_t>(mapped.size());
		return;
	}
	if (!destination) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u);
		return;
	}
	if (mapped.size() > destinationCapacity) {
		cpu->r[EAX] = 0;
		setWin32LastError(122u);
		return;
	}
	for (size_t i = 0; i < mapped.size(); ++i)
		guest::store<uint16_t>(destination + static_cast<uint32_t>(i * 2u), mapped[i]);
	cpu->r[EAX] = static_cast<uint32_t>(mapped.size());
}

bool encode1252(uint16_t ch, uint8_t& output)
{
	if (ch <= 0x7fu || (ch >= 0xa0u && ch <= 0xffu)) {
		output = static_cast<uint8_t>(ch);
		return true;
	}
	static constexpr uint16_t high[32] = {
		0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
		0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
		0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
		0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
	};
	for (uint32_t i = 0; i < 32u; ++i) {
		if (high[i] == ch) {
			output = static_cast<uint8_t>(0x80u + i);
			return true;
		}
	}
	return false;
}

uint16_t decode1252(uint8_t ch)
{
	if (ch < 0x80u || ch >= 0xa0u) return ch;
	static constexpr uint16_t high[32] = {
		0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021,
		0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
		0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014,
		0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178
	};
	return high[ch - 0x80u];
}

void multiByteToWideChar(CPU* cpu, const ApiImportStub&)
{
	const uint32_t codePage = argument(cpu, 0);
	const uint32_t flags = argument(cpu, 1);
	const uint32_t source = argument(cpu, 2);
	const int32_t sourceLength = static_cast<int32_t>(argument(cpu, 3));
	const uint32_t destination = argument(cpu, 4);
	const uint32_t destinationCapacity = argument(cpu, 5);
	const bool utf8 = codePage == 65001u;
	const uint32_t effectiveCodePage = codePage ? codePage : 1252u;
	if (!source || sourceLength < -1 ||
		(effectiveCodePage != 1252u && effectiveCodePage != 437u && !utf8) ||
		(utf8 ? (flags & ~8u) != 0u : (flags & ~9u) != 0u)) {
		cpu->r[EAX] = 0;
		setWin32LastError(!source ? 998u : 87u);
		return;
	}
	const uint32_t count = sourceLength == -1 ? 0x100000u : static_cast<uint32_t>(sourceLength);
	std::vector<uint16_t> converted;
	bool terminated = false;
	for (uint32_t i = 0; i < count;) {
		const uint8_t first = guest::load<uint8_t>(source + i);
		if (!first && sourceLength == -1) { converted.push_back(0u); terminated = true; break; }
		if (!utf8) {
			converted.push_back(effectiveCodePage == 1252u ? decode1252(first) : first);
			++i;
			continue;
		}
		uint32_t codepoint = 0;
		uint32_t trailing = 0;
		if (first < 0x80u) codepoint = first;
		else if (first >= 0xc2u && first <= 0xdfu) { codepoint = first & 0x1fu; trailing = 1; }
		else if (first >= 0xe0u && first <= 0xefu) { codepoint = first & 0x0fu; trailing = 2; }
		else if (first >= 0xf0u && first <= 0xf4u) { codepoint = first & 0x07u; trailing = 3; }
		else trailing = 4;
		bool valid = trailing <= 3u && i + trailing < count;
		for (uint32_t j = 1; valid && j <= trailing; ++j) {
			const uint8_t next = guest::load<uint8_t>(source + i + j);
			if ((next & 0xc0u) != 0x80u) { valid = false; break; }
			codepoint = (codepoint << 6) | (next & 0x3fu);
		}
		if (valid && ((trailing == 2u && codepoint < 0x800u) ||
			(trailing == 3u && (codepoint < 0x10000u || codepoint > 0x10ffffu)) ||
			(codepoint >= 0xd800u && codepoint <= 0xdfffu))) valid = false;
		if (!valid) {
			if (flags & 8u) {
				cpu->r[EAX] = 0;
				setWin32LastError(1113u); // ERROR_NO_UNICODE_TRANSLATION
				return;
			}
			converted.push_back(0xfffdu);
			++i;
			continue;
		}
		if (codepoint <= 0xffffu) converted.push_back(static_cast<uint16_t>(codepoint));
		else {
			codepoint -= 0x10000u;
			converted.push_back(static_cast<uint16_t>(0xd800u + (codepoint >> 10)));
			converted.push_back(static_cast<uint16_t>(0xdc00u + (codepoint & 0x3ffu)));
		}
		i += trailing + 1u;
	}
	if (sourceLength == -1 && !terminated) {
		cpu->r[EAX] = 0;
		setWin32LastError(1113u);
		return;
	}
	if (!destinationCapacity) {
		cpu->r[EAX] = static_cast<uint32_t>(converted.size());
		return;
	}
	if (!destination) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u);
		return;
	}
	if (converted.size() > destinationCapacity) {
		// Windows writes the portion that fits before reporting
		// ERROR_INSUFFICIENT_BUFFER. The game relies on this: it reserves the
		// terminator slot itself after converting a -1 length source, and thus
		// deliberately passes capacity one smaller than the required count.
		for (uint32_t i = 0; i < destinationCapacity; ++i)
			guest::store<uint16_t>(destination + i * 2u, converted[i]);
		cpu->r[EAX] = 0;
		setWin32LastError(122u);
		return;
	}
	for (size_t i = 0; i < converted.size(); ++i)
		guest::store<uint16_t>(destination + static_cast<uint32_t>(i * 2u), converted[i]);
	cpu->r[EAX] = static_cast<uint32_t>(converted.size());
}

void appendUtf8(std::vector<uint8_t>& output, uint32_t codepoint)
{
	if (codepoint <= 0x7fu) output.push_back(static_cast<uint8_t>(codepoint));
	else if (codepoint <= 0x7ffu) {
		output.push_back(static_cast<uint8_t>(0xc0u | (codepoint >> 6)));
		output.push_back(static_cast<uint8_t>(0x80u | (codepoint & 0x3fu)));
	} else if (codepoint <= 0xffffu) {
		output.push_back(static_cast<uint8_t>(0xe0u | (codepoint >> 12)));
		output.push_back(static_cast<uint8_t>(0x80u | ((codepoint >> 6) & 0x3fu)));
		output.push_back(static_cast<uint8_t>(0x80u | (codepoint & 0x3fu)));
	} else {
		output.push_back(static_cast<uint8_t>(0xf0u | (codepoint >> 18)));
		output.push_back(static_cast<uint8_t>(0x80u | ((codepoint >> 12) & 0x3fu)));
		output.push_back(static_cast<uint8_t>(0x80u | ((codepoint >> 6) & 0x3fu)));
		output.push_back(static_cast<uint8_t>(0x80u | (codepoint & 0x3fu)));
	}
}

void wideCharToMultiByte(CPU* cpu, const ApiImportStub&)
{
	const uint32_t codePage = argument(cpu, 0);
	const uint32_t source = argument(cpu, 2);
	const int32_t sourceLength = static_cast<int32_t>(argument(cpu, 3));
	const uint32_t destination = argument(cpu, 4);
	const uint32_t destinationCapacity = argument(cpu, 5);
	const uint32_t defaultCharAddress = argument(cpu, 6);
	const uint32_t usedDefaultAddress = argument(cpu, 7);
	const bool utf8 = codePage == 65001u;
	if (!source || sourceLength < -1 || (utf8 && (defaultCharAddress || usedDefaultAddress))) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u); // ERROR_INVALID_PARAMETER
		return;
	}
	const uint32_t effectiveCodePage = codePage ? codePage : 1252u;
	if (!utf8 && effectiveCodePage != 1252u) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u);
		return;
	}
	std::vector<uint8_t> converted;
	bool usedDefault = false;
	const uint32_t count = sourceLength == -1 ? 0x10000u : static_cast<uint32_t>(sourceLength);
	bool terminated = false;
	for (uint32_t i = 0; i < count; ++i) {
		const uint16_t ch = guest::load<uint16_t>(source + i * 2u);
		if (!ch && sourceLength == -1) {
			converted.push_back(0);
			terminated = true;
			break;
		}
		if (utf8) {
			uint32_t codepoint = ch;
			if (ch >= 0xd800u && ch <= 0xdbffu && i + 1u < count) {
				const uint16_t low = guest::load<uint16_t>(source + (i + 1u) * 2u);
				if (low >= 0xdc00u && low <= 0xdfffu) {
					codepoint = 0x10000u + ((uint32_t(ch - 0xd800u) << 10) | (low - 0xdc00u));
					++i;
				} else codepoint = 0xfffdu;
			} else if (ch >= 0xdc00u && ch <= 0xdfffu) codepoint = 0xfffdu;
			appendUtf8(converted, codepoint);
		} else {
			uint8_t byte = 0;
			if (encode1252(ch, byte)) {
				converted.push_back(byte);
			} else {
				usedDefault = true;
				converted.push_back(defaultCharAddress ? guest::load<uint8_t>(defaultCharAddress) : static_cast<uint8_t>('?'));
			}
		}
	}
	if (sourceLength == -1 && !terminated) {
		cpu->r[EAX] = 0;
		setWin32LastError(1113u); // ERROR_NO_UNICODE_TRANSLATION
		return;
	}
	if (usedDefaultAddress) guest::store<int32_t>(usedDefaultAddress, usedDefault ? 1 : 0);
	if (!destinationCapacity) {
		cpu->r[EAX] = static_cast<uint32_t>(converted.size());
		return;
	}
	if (!destination) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u);
		return;
	}
	if (converted.size() > destinationCapacity) {
		cpu->r[EAX] = 0;
		setWin32LastError(122u); // ERROR_INSUFFICIENT_BUFFER
		return;
	}
	for (size_t i = 0; i < converted.size(); ++i)
		guest::store<uint8_t>(destination + static_cast<uint32_t>(i), converted[i]);
	cpu->r[EAX] = static_cast<uint32_t>(converted.size());
}

void getCommandLineA(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = startupContext().commandLineAddress;
}

void getVersionExA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t info = argument(cpu, 0);
	if (!info) {
		cpu->r[EAX] = 0;
		setWin32LastError(998u); // ERROR_NOACCESS
		return;
	}
	const uint32_t size = guest::load<uint32_t>(info);
	if (size != 148u && size != 156u) {
		cpu->r[EAX] = 0;
		setWin32LastError(122u); // ERROR_INSUFFICIENT_BUFFER
		return;
	}
	guest::store<uint32_t>(info + 0u, size);
	guest::store<uint32_t>(info + 4u, 5u); // Windows XP
	guest::store<uint32_t>(info + 8u, 1u);
	guest::store<uint32_t>(info + 12u, 2600u);
	guest::store<uint32_t>(info + 16u, 2u); // VER_PLATFORM_WIN32_NT
	constexpr char servicePack[] = "Service Pack 3";
	for (uint32_t i = 0; i < 128u; ++i)
		guest::store<uint8_t>(info + 20u + i, i < sizeof(servicePack)
			? static_cast<uint8_t>(servicePack[i]) : 0u);
	if (size == 156u) {
		guest::store<uint16_t>(info + 144u, 3u); // wServicePackMajor
		guest::store<uint16_t>(info + 146u, 0u);
		guest::store<uint16_t>(info + 148u, 0u); // wSuiteMask
		guest::store<uint8_t>(info + 150u, 1u); // VER_NT_WORKSTATION
		guest::store<uint8_t>(info + 151u, 0u);
	}
	cpu->r[EAX] = 1u;
}

void acmGetVersion(CPU* cpu, const ApiImportStub&)
{
	// Match the NT-family ACM version reported by Wine for the Windows
	// compatibility profile used by this runtime (4.0.1381). Battle Realms
	// only needs the version query at this startup point; ACM codecs and
	// stream operations remain separate services and are not implied here.
	cpu->r[EAX] = 0x04000565u;
}

uint32_t readAcmWord(uint32_t address)
{
	return guest::load<uint16_t>(address);
}

void acmStreamOpen(CPU* cpu, const ApiImportStub&)
{
	const uint32_t output = argument(cpu, 0u);
	const uint32_t driver = argument(cpu, 1u);
	const uint32_t source = argument(cpu, 2u);
	const uint32_t destination = argument(cpu, 3u);
	const uint32_t filter = argument(cpu, 4u);
	const uint32_t callback = argument(cpu, 5u);
	const uint32_t openFlags = argument(cpu, 7u);
	if (!output || !source || !destination || driver || filter || callback || openFlags) {
		cpu->r[EAX] = 11u; // MMSYSERR_INVALPARAM.
		return;
	}
	GuestAcmStream stream;
	stream.sourceTag = static_cast<uint16_t>(readAcmWord(source));
	stream.destinationTag = static_cast<uint16_t>(readAcmWord(destination));
	const uint16_t sourceChannels = static_cast<uint16_t>(readAcmWord(source + 2u));
	const uint16_t destinationChannels = static_cast<uint16_t>(readAcmWord(destination + 2u));
	const uint32_t sourceRate = guest::load<uint32_t>(source + 4u);
	const uint32_t destinationRate = guest::load<uint32_t>(destination + 4u);
	const uint16_t sourceBits = static_cast<uint16_t>(readAcmWord(source + 14u));
	const uint16_t destinationBits = static_cast<uint16_t>(readAcmWord(destination + 14u));
	stream.sourceBlockAlign = static_cast<uint16_t>(readAcmWord(source + 12u));
	stream.destinationBlockAlign = static_cast<uint16_t>(readAcmWord(destination + 12u));
	stream.channels = sourceChannels;
	if (stream.sourceTag != 2u || stream.destinationTag != 1u ||
		sourceChannels < 1u || sourceChannels > 2u || sourceChannels != destinationChannels ||
		sourceRate != destinationRate || sourceBits != 4u || destinationBits != 16u ||
		!stream.sourceBlockAlign || stream.destinationBlockAlign != sourceChannels * 2u) {
		cpu->r[EAX] = 8u; // MMSYSERR_NOTSUPPORTED.
		return;
	}
	const uint16_t extraBytes = static_cast<uint16_t>(readAcmWord(source + 16u));
	if (extraBytes < 4u) { cpu->r[EAX] = 11u; return; }
	stream.samplesPerBlock = static_cast<uint16_t>(readAcmWord(source + 18u));
	const uint16_t coefficientCount = static_cast<uint16_t>(readAcmWord(source + 20u));
	if (!coefficientCount || coefficientCount > 32u ||
		extraBytes < 4u + coefficientCount * 4u ||
		stream.samplesPerBlock != (uint32_t(stream.sourceBlockAlign) * 2u / sourceChannels - 12u)) {
		cpu->r[EAX] = 11u;
		return;
	}
	stream.coefficients.reserve(coefficientCount);
	for (uint32_t index = 0; index < coefficientCount; ++index) {
		const uint32_t coefficient = source + 22u + index * 4u;
		stream.coefficients.emplace_back(
			guest::load<int16_t>(coefficient), guest::load<int16_t>(coefficient + 2u));
	}
	std::lock_guard<std::mutex> lock(gAcmStreamsMutex);
	uint32_t handle = gNextAcmStreamHandle;
	do {
		gNextAcmStreamHandle += 4u;
		if (!gNextAcmStreamHandle) gNextAcmStreamHandle = 0x6e000000u;
		if (!gAcmStreams.count(handle)) break;
		handle = gNextAcmStreamHandle;
	} while (true);
	gAcmStreams.emplace(handle, std::move(stream));
	guest::store<uint32_t>(output, handle);
	cpu->r[EAX] = 0u; // MMSYSERR_NOERROR.
#if defined(__ANDROID__)
	if (allowAudioTraceLog(gAudioAcmOpenLogCount, 12u))
		androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsAudio", "ACM_STREAM_OPEN",
			"handle=%08x src_tag=%u dst_tag=%u channels=%u rate=%u src_bits=%u dst_bits=%u src_block=%u dst_block=%u samples_per_block=%u coefficients=%u",
			handle, stream.sourceTag, stream.destinationTag, stream.channels, sourceRate,
			sourceBits, destinationBits, stream.sourceBlockAlign, stream.destinationBlockAlign,
			stream.samplesPerBlock, coefficientCount);
#endif
}

int16_t decodeMsAdpcmNibble(uint8_t nibble, int16_t& delta, int16_t& sample1,
	int16_t& sample2, const std::pair<int16_t, int16_t>& coefficient)
{
	static constexpr int16_t adaptation[16] = {
		230, 230, 230, 230, 307, 409, 512, 614,
		768, 614, 512, 409, 307, 230, 230, 230
	};
	const int32_t signedNibble = (nibble & 8u) ? int32_t(nibble) - 16 : nibble;
	const int32_t prediction = int32_t(sample1) * coefficient.first +
		int32_t(sample2) * coefficient.second;
	// MS-ADPCM uses integer division by 256 for the predictor. C/C++ signed
	// division truncates toward zero, matching the Windows/Wine decoder; an
	// arithmetic shift would round negative non-integral predictions down and
	// bias every such decoded sample by one.
	const int32_t linear = prediction / 256;
	const int32_t decoded = std::clamp(linear + signedNibble * int32_t(delta), -32768, 32767);
	delta = static_cast<int16_t>(std::max(16, (int32_t(adaptation[nibble]) * delta) >> 8));
	sample2 = sample1;
	sample1 = static_cast<int16_t>(decoded);
	return sample1;
}

bool decodeMsAdpcmBlock(const GuestAcmStream& stream, const uint8_t* source,
	uint32_t destination, uint32_t capacityBytes)
{
	const uint32_t channels = stream.channels;
	const uint32_t headerBytes = 7u * channels;
	const uint32_t expectedBytes = uint32_t(stream.samplesPerBlock) * channels * 2u;
	if (!channels || channels > 2u || stream.sourceBlockAlign < headerBytes ||
		capacityBytes < expectedBytes) return false;
	int16_t predictor[2]{};
	int16_t delta[2]{};
	int16_t sample1[2]{};
	int16_t sample2[2]{};
	for (uint32_t channel = 0; channel < channels; ++channel) {
		const uint8_t predictorIndex = source[channel];
		if (predictorIndex >= stream.coefficients.size()) return false;
		predictor[channel] = predictorIndex;
		delta[channel] = std::max<int16_t>(16, static_cast<int16_t>(
			uint16_t(source[channels + channel * 2u]) |
			(uint16_t(source[channels + channel * 2u + 1u]) << 8)));
		sample1[channel] = static_cast<int16_t>(uint16_t(source[channels * 3u + channel * 2u]) |
			(uint16_t(source[channels * 3u + channel * 2u + 1u]) << 8));
		sample2[channel] = static_cast<int16_t>(uint16_t(source[channels * 5u + channel * 2u]) |
			(uint16_t(source[channels * 5u + channel * 2u + 1u]) << 8));
	}
	uint32_t frame = 0u;
	for (uint32_t channel = 0; channel < channels; ++channel)
		guest::store<int16_t>(destination + (frame * channels + channel) * 2u, sample2[channel]);
	++frame;
	for (uint32_t channel = 0; channel < channels; ++channel)
		guest::store<int16_t>(destination + (frame * channels + channel) * 2u, sample1[channel]);
	++frame;
	const uint32_t codeCount = (uint32_t(stream.samplesPerBlock) - 2u) * channels;
	const uint32_t dataOffset = headerBytes;
	for (uint32_t index = 0; index < codeCount; index += 2u) {
		const uint8_t packed = source[dataOffset + index / 2u];
		if (channels == 1u) {
			for (uint32_t nibbleIndex = 0; nibbleIndex < 2u && index + nibbleIndex < codeCount; ++nibbleIndex) {
				const uint8_t nibble = nibbleIndex ? static_cast<uint8_t>(packed & 0x0fu) :
					static_cast<uint8_t>(packed >> 4);
				const int16_t sample = decodeMsAdpcmNibble(nibble, delta[0], sample1[0], sample2[0],
					stream.coefficients[static_cast<size_t>(predictor[0])]);
				guest::store<int16_t>(destination + frame * 2u, sample);
				++frame;
			}
		} else {
			for (uint32_t channel = 0; channel < 2u; ++channel) {
				const uint8_t nibble = channel ? static_cast<uint8_t>(packed & 0x0fu) :
					static_cast<uint8_t>(packed >> 4);
				const int16_t sample = decodeMsAdpcmNibble(nibble, delta[channel],
					sample1[channel], sample2[channel],
					stream.coefficients[static_cast<size_t>(predictor[channel])]);
				guest::store<int16_t>(destination + (frame * channels + channel) * 2u, sample);
			}
			++frame;
		}
	}
	return frame == stream.samplesPerBlock;
}

void acmStreamPrepareHeader(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0u);
	const uint32_t header = argument(cpu, 1u);
	GuestAcmStream stream;
	{
		std::lock_guard<std::mutex> lock(gAcmStreamsMutex);
		const auto found = gAcmStreams.find(handle);
		if (found == gAcmStreams.end()) { cpu->r[EAX] = 5u; return; }
		stream = found->second;
	}
	(void)stream;
	if (!header || guest::load<uint32_t>(header) < 84u) { cpu->r[EAX] = 11u; return; }
	const uint32_t status = guest::load<uint32_t>(header + 4u);
	guest::store<uint32_t>(header + 4u, (status | 2u) & ~0x10000u);
	guest::store<uint32_t>(header + 20u, 0u);
	guest::store<uint32_t>(header + 36u, 0u);
	cpu->r[EAX] = 0u;
}

void acmStreamConvert(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0u);
	const uint32_t header = argument(cpu, 1u);
	const uint32_t flags = argument(cpu, 2u);
	GuestAcmStream stream;
	{
		std::lock_guard<std::mutex> lock(gAcmStreamsMutex);
		const auto found = gAcmStreams.find(handle);
		if (found == gAcmStreams.end()) { cpu->r[EAX] = 5u; return; }
		stream = found->second;
	}
	if (!header || guest::load<uint32_t>(header) < 84u ||
		(flags & ~(4u | 0x10u | 0x20u)) != 0u) { cpu->r[EAX] = 11u; return; }
	const uint32_t status = guest::load<uint32_t>(header + 4u);
	if (!(status & 2u)) { cpu->r[EAX] = 11u; return; }
	const uint32_t source = guest::load<uint32_t>(header + 12u);
	const uint32_t sourceLength = guest::load<uint32_t>(header + 16u);
	const uint32_t destination = guest::load<uint32_t>(header + 28u);
	const uint32_t destinationLength = guest::load<uint32_t>(header + 32u);
	if ((!source && sourceLength) || (!destination && destinationLength)) { cpu->r[EAX] = 11u; return; }
	const uint32_t outputBytesPerBlock = uint32_t(stream.samplesPerBlock) * stream.channels * 2u;
	if (!stream.sourceBlockAlign || !outputBytesPerBlock) { cpu->r[EAX] = 8u; return; }
	const uint32_t sourceBlocks = sourceLength / stream.sourceBlockAlign;
	const uint32_t outputBlocks = destinationLength / outputBytesPerBlock;
	const uint32_t blockCount = std::min(sourceBlocks, outputBlocks);
	uint32_t completed = 0u;
	for (; completed < blockCount; ++completed) {
		const uint32_t sourceAddress = source + completed * stream.sourceBlockAlign;
		const uint32_t destinationAddress = destination + completed * outputBytesPerBlock;
		if (!decodeMsAdpcmBlock(stream, guest::host(sourceAddress), destinationAddress,
			outputBytesPerBlock)) break;
	}
	guest::store<uint32_t>(header + 20u, completed * stream.sourceBlockAlign);
	guest::store<uint32_t>(header + 36u, completed * outputBytesPerBlock);
	guest::store<uint32_t>(header + 4u, status | 0x10000u);
	cpu->r[EAX] = completed ? 0u : (sourceLength && destinationLength ? 8u : 0u);
#if defined(__ANDROID__)
	if (allowAudioTraceLog(gAudioAcmConvertLogCount, 8u, 128u))
		androidDiagnosticLogEvent(cpu->r[EAX] ? ANDROID_LOG_WARN : ANDROID_LOG_INFO,
			"BattleRealmsAudio", "ACM_STREAM_CONVERT",
			"handle=%08x flags=%08x src_bytes=%u dst_bytes=%u src_block=%u dst_block=%u requested_blocks=%u converted_blocks=%u src_used=%u dst_used=%u result=%08x",
			handle, flags, sourceLength, destinationLength, stream.sourceBlockAlign,
			outputBytesPerBlock, blockCount, completed,
			completed * stream.sourceBlockAlign, completed * outputBytesPerBlock, cpu->r[EAX]);
#endif
}

void acmStreamUnprepareHeader(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0u);
	const uint32_t header = argument(cpu, 1u);
	{
		std::lock_guard<std::mutex> lock(gAcmStreamsMutex);
		if (!gAcmStreams.count(handle)) { cpu->r[EAX] = 5u; return; }
	}
	if (!header || guest::load<uint32_t>(header) < 84u) { cpu->r[EAX] = 11u; return; }
	const uint32_t status = guest::load<uint32_t>(header + 4u);
	if (!(status & 2u)) { cpu->r[EAX] = 11u; return; }
	guest::store<uint32_t>(header + 4u, status & ~(2u | 0x10000u));
	cpu->r[EAX] = 0u;
}

void acmStreamClose(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0u);
	std::lock_guard<std::mutex> lock(gAcmStreamsMutex);
	cpu->r[EAX] = gAcmStreams.erase(handle) ? 0u : 5u;
}

void getStartupInfoA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t output = argument(cpu, 0);
	// STARTUPINFOA is 68 bytes in the 32-bit ABI. The native launcher has no
	// console/window handles yet, so all fields except cb are zero.
	for (uint32_t offset = 0; offset < 68u; offset += 4u)
		guest::store<uint32_t>(output + offset, 0u);
	guest::store<uint32_t>(output, 68u);
}

std::string readGuestString(uint32_t address)
{
	std::string value;
	value.reserve(64);
	for (uint32_t offset = 0; offset < 1024u; ++offset) {
		const char ch = static_cast<char>(guest::load<uint8_t>(address + offset));
		if (!ch) break;
		value.push_back(ch);
	}
	return value;
}

std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
	return value;
}

void getModuleHandleA(CPU* cpu, const ApiImportStub&)
{
	const StartupContext context = startupContext();
	const uint32_t nameAddress = argument(cpu, 0);
	if (!nameAddress) {
		cpu->r[EAX] = context.mainModuleBase;
		return;
	}
	std::string requested = lower(readGuestString(nameAddress));
	const size_t separator = requested.find_last_of("\\/");
	if (separator != std::string::npos) requested.erase(0, separator + 1u);
	std::string mainName = lower(context.mainModuleName);
	if (requested == mainName || (requested.find('.') == std::string::npos &&
		mainName.compare(0, requested.size(), requested) == 0 && mainName.size() == requested.size() + 4u)) {
		cpu->r[EAX] = context.mainModuleBase;
		return;
	}
	cpu->r[EAX] = 0;
	setWin32LastError(126u); // ERROR_MOD_NOT_FOUND
}

void loadLibraryA(CPU* cpu, const ApiImportStub&)
{
	std::string requested = lower(readGuestString(argument(cpu, 0)));
	const size_t separator = requested.find_last_of("\\/");
	if (separator != std::string::npos) requested.erase(0, separator + 1u);
// System DLLs are guest pseudo-modules backed by native compatibility
// handlers; the original 32-bit DLLs are never loaded into the ARM64 host.
	if (requested == "ddraw.dll") {
		{
			std::lock_guard<std::mutex> lock(gLoadedModulesMutex);
			++gDdrawLibraryReferences;
		}
		cpu->r[EAX] = 0x7e0a0000u;
		return;
	}
	if (requested == "dinput.dll") {
		{
			std::lock_guard<std::mutex> lock(gLoadedModulesMutex);
			++gDinputLibraryReferences;
		}
		cpu->r[EAX] = 0x7e0b0000u;
		if (std::getenv("BR_TRACE_DDRAW"))
			std::fprintf(stderr, "[br-loader] LoadLibraryA dinput.dll -> 7e0b0000\n");
		return;
	}
	if (requested == "dsound.dll") {
		{
			std::lock_guard<std::mutex> lock(gLoadedModulesMutex);
			++gDsoundLibraryReferences;
		}
		cpu->r[EAX] = 0x7e0c0000u;
		if (std::getenv("BR_TRACE_DDRAW"))
			std::fprintf(stderr, "[br-loader] LoadLibraryA dsound.dll -> 7e0c0000\n");
		return;
	}
	cpu->r[EAX] = 0;
	setWin32LastError(126u); // ERROR_MOD_NOT_FOUND
}

void getProcAddress(CPU* cpu, const ApiImportStub&)
{
	const StartupContext context = startupContext();
	const uint32_t module = argument(cpu, 0);
	const uint32_t procedure = argument(cpu, 1);
	bool ddrawLoaded = false;
	bool dinputLoaded = false;
	bool dsoundLoaded = false;
	{
		std::lock_guard<std::mutex> lock(gLoadedModulesMutex);
		ddrawLoaded = module == 0x7e0a0000u && gDdrawLibraryReferences != 0u;
		dinputLoaded = module == 0x7e0b0000u && gDinputLibraryReferences != 0u;
		dsoundLoaded = module == 0x7e0c0000u && gDsoundLibraryReferences != 0u;
	}
	if ((!ddrawLoaded && !dinputLoaded && !dsoundLoaded) || !procedure || procedure < 0x10000u) {
		cpu->r[EAX] = 0;
		setWin32LastError(procedure && procedure < 0x10000u ? 127u : 126u);
		return;
	}
	const std::string name = readGuestString(procedure);
	const bool ddrawExport = ddrawLoaded &&
		(name == "DirectDrawCreateEx" || name == "DirectDrawEnumerateExA");
	const bool dinputExport = dinputLoaded && name == "DirectInputCreateEx";
	const bool dsoundExport = dsoundLoaded && name == "DirectSoundCreate";
	if (!ddrawExport && !dinputExport && !dsoundExport) {
		cpu->r[EAX] = 0;
		setWin32LastError(127u); // ERROR_PROC_NOT_FOUND
		return;
	}
	if (!context.imports) {
		cpu->r[EAX] = 0;
		setWin32LastError(127u);
		return;
	}
	std::string error;
	cpu->r[EAX] = context.imports->getOrCreateDynamicStub(
		dinputExport ? "dinput.dll" : dsoundExport ? "dsound.dll" : "ddraw.dll", name, error);
	if (!cpu->r[EAX]) setWin32LastError(8u); // ERROR_NOT_ENOUGH_MEMORY
}

void freeLibrary(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDdrawModule = 0x7e0a0000u;
	constexpr uint32_t kDinputModule = 0x7e0b0000u;
	constexpr uint32_t kDsoundModule = 0x7e0c0000u;
	const uint32_t module = argument(cpu, 0);
	if (module != kDdrawModule && module != kDinputModule && module != kDsoundModule) {
		cpu->r[EAX] = 0u;
		setWin32LastError(6u); // ERROR_INVALID_HANDLE.
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gLoadedModulesMutex);
		uint32_t& references = module == kDdrawModule ? gDdrawLibraryReferences :
			module == kDinputModule ? gDinputLibraryReferences : gDsoundLibraryReferences;
		if (!references) {
			cpu->r[EAX] = 0u;
			setWin32LastError(6u); // ERROR_INVALID_HANDLE.
			return;
		}
		--references;
	}
	cpu->r[EAX] = 1u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		const char* library = module == kDdrawModule ? "ddraw.dll" :
			module == kDinputModule ? "dinput.dll" : "dsound.dll";
		const uint32_t references = module == kDdrawModule ? gDdrawLibraryReferences :
			module == kDinputModule ? gDinputLibraryReferences : gDsoundLibraryReferences;
		std::fprintf(stderr, "[br-loader] FreeLibrary %s refs=%u\n", library, references);
	}
}

void isDebuggerPresent(CPU* cpu, const ApiImportStub&)
{
	// The native Android/headless host is not attached as a debugger to the
	// lifted guest process, matching the ordinary non-debugged Windows path.
	cpu->r[EAX] = 0u;
}

void directDrawEnumerateExA(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDisplayInfo = 0x6c300000u;
	static std::once_flag once;
	static bool ready = false;
	std::call_once(once, [] {
		std::string error;
		ready = sharedGuestAddressSpace().commitRange(kDisplayInfo, 0x1000u, error);
		if (!ready) return;
		constexpr std::string_view description = "Primary Display Driver";
		constexpr std::string_view name = "display";
		for (uint32_t i = 0; i < description.size(); ++i)
			guest::store<uint8_t>(kDisplayInfo + i, static_cast<uint8_t>(description[i]));
		guest::store<uint8_t>(kDisplayInfo + static_cast<uint32_t>(description.size()), 0u);
		for (uint32_t i = 0; i < name.size(); ++i)
			guest::store<uint8_t>(kDisplayInfo + 0x100u + i, static_cast<uint8_t>(name[i]));
		guest::store<uint8_t>(kDisplayInfo + 0x100u + static_cast<uint32_t>(name.size()), 0u);
	});
	const uint32_t callback = argument(cpu, 0);
	const uint32_t context = argument(cpu, 1);
	if (!ready || !callback) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS
		return;
	}
	const CPU saved = *cpu;
	const uint32_t args[5] = { 0u, kDisplayInfo, kDisplayInfo + 0x100u, context, 0u };
	uint32_t callbackResult = 0;
	std::string error;
	if (!callGuestStdcall(cpu, callback, args, 5u, callbackResult, error)) {
		*cpu = saved;
		br::runtime::dispatchFailure(cpu, callback, error.c_str());
	}
	*cpu = saved;
	cpu->r[EAX] = 0u; // DD_OK; the single emulated primary display was enumerated.
}

constexpr uint32_t kDdrawObject = 0x6c400000u;
constexpr uint32_t kDdrawVtable = 0x6c401000u;
constexpr uint32_t kD3d7Object = 0x6c400100u;
constexpr uint32_t kD3d7Vtable = 0x6c402000u;
constexpr uint32_t kDdrawSurfaceVtable = 0x6c403000u;
constexpr uint32_t kDdrawSurfaceVtableArea = 0x6c403100u;
constexpr uint32_t kD3d7DeviceVtable = 0x6c404000u;
constexpr uint32_t kD3d7DeviceObject = 0x6c405000u;
constexpr uint32_t kD3d7VertexBufferVtable = 0x6c406000u;
constexpr uint32_t kD3d7DeviceDesc = 0x6c420000u;
constexpr uint32_t kD3d7DeviceDescription = 0x6c421000u;
constexpr uint32_t kD3d7DeviceName = 0x6c421100u;
constexpr uint32_t kD3d7ZBufferPixelFormat = 0x6c430000u;
constexpr uint32_t kD3d7TexturePixelFormat = 0x6c431000u;
constexpr uint32_t kDirectInputObjectBase = 0x6c480000u;
constexpr uint32_t kDirectInputVtable = 0x6c484000u;
constexpr uint32_t kDirectInputDeviceVtable = 0x6c485000u;
constexpr uint32_t kDirectSoundObjectBase = 0x6c490000u;
constexpr uint32_t kDirectSoundVtable = 0x6c490400u;
constexpr uint32_t kDirectSoundBufferVtable = 0x6c490500u;
constexpr uint32_t kDirectSound3DListenerVtable = 0x6c490600u;
uint32_t gNextDirectInputObject = kDirectInputObjectBase;
uint32_t gNextDirectInputDevice = 0x6c482000u;
uint32_t gNextDirectSoundObject = 0x6c491000u;
uint32_t gNextDirectSoundBuffer = 0x6c493000u;
uint32_t gNextDirectSound3DListener = 0x6c494000u;
uint32_t gNextDirectSoundData = 0x6e000000u;
constexpr std::array<std::array<uint32_t, 3>, 3> kGuestDisplayModes = {{
	std::array<uint32_t, 3>{{1024u, 768u, 16u}},
	std::array<uint32_t, 3>{{800u, 600u, 16u}},
	std::array<uint32_t, 3>{{640u, 480u, 16u}}
}};
std::mutex gDisplayModeMutex;
std::array<uint32_t, 4> gCurrentGuestDisplayMode{{0u, 0u, 0u, 0u}};
struct GuestDdrawCooperativeState {
	uint32_t window = 0u;
	uint32_t flags = 0u;
	bool lost = false;
};
std::mutex gDdrawCooperativeMutex;
GuestDdrawCooperativeState gDdrawCooperativeState{};
constexpr std::array<const char*, 30> kDdraw7Methods = {{
	"QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette",
	"CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface",
	"GetCaps", "GetDisplayMode", "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency",
	"GetScanLine", "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode", "SetCooperativeLevel",
	"SetDisplayMode", "WaitForVerticalBlank", "GetAvailableVidMem", "GetSurfaceFromDC",
	"RestoreAllSurfaces", "TestCooperativeLevel", "GetDeviceIdentifier", "StartModeTest", "EvaluateMode"
}};
	constexpr std::array<const char*, 8> kD3d7Methods = {{
	"QueryInterface", "AddRef", "Release", "EnumDevices", "CreateDevice",
	"CreateVertexBuffer", "EnumZBufferFormats", "EvictManagedTextures"
}};
constexpr std::array<const char*, 49> kDdrawSurfaceMethods = {{
	"QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect",
	"Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces",
	"EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps",
	"GetClipper", "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition",
	"GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
	"ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette",
	"Unlock", "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface",
	"PageLock", "PageUnlock", "SetSurfaceDesc", "SetPrivateData", "GetPrivateData",
	"FreePrivateData", "GetUniquenessValue", "ChangeUniquenessValue", "SetPriority",
	"GetPriority", "SetLOD", "GetLOD"
}};
constexpr std::array<const char*, 49> kD3d7DeviceMethods = {{
	"QueryInterface", "AddRef", "Release", "GetCaps", "EnumTextureFormats", "BeginScene",
	"EndScene", "GetDirect3D", "SetRenderTarget", "GetRenderTarget", "Clear", "SetTransform",
	"GetTransform", "SetViewport", "MultiplyTransform", "GetViewport", "SetMaterial", "GetMaterial",
	"SetLight", "GetLight", "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock",
	"PreLoad", "DrawPrimitive", "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus",
	"DrawPrimitiveStrided", "DrawIndexedPrimitiveStrided", "DrawPrimitiveVB", "DrawIndexedPrimitiveVB",
	"ComputeSphereVisibility", "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState",
	"ValidateDevice", "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock",
	"Load", "LightEnable", "GetLightEnable", "SetClipPlane", "GetClipPlane", "GetInfo"
}};
constexpr std::array<const char*, 8> kD3d7VertexBufferMethods = {{
	"QueryInterface", "AddRef", "Release", "Lock", "Unlock", "ProcessVertices",
	"GetVertexBufferDesc", "Optimize"
}};
constexpr std::array<const char*, 12> kDirectInput7Methods = {{
	"QueryInterface", "AddRef", "Release", "CreateDevice", "EnumDevices",
	"GetDeviceStatus", "RunControlPanel", "Initialize", "FindDevice", "CreateDeviceEx",
	"EnumDevicesBySemantics", "ConfigureDevices"
}};
constexpr std::array<const char*, 32> kDirectInputDevice7Methods = {{
	"QueryInterface", "AddRef", "Release", "GetCapabilities", "EnumObjects",
	"GetProperty", "SetProperty", "Acquire", "Unacquire", "GetDeviceState",
	"GetDeviceData", "SetDataFormat", "SetEventNotification", "SetCooperativeLevel",
	"GetObjectInfo", "GetDeviceInfo", "RunControlPanel", "Initialize", "CreateEffect",
	"EnumEffects", "GetEffectInfo", "GetForceFeedbackState", "SendForceFeedbackCommand",
	"EnumCreatedEffectObjects", "Escape", "Poll", "SendDeviceData", "EnumEffectsInFile",
	"WriteEffectToFile", "BuildActionMap", "SetActionMap", "GetImageInfo"
}};
constexpr std::array<const char*, 11> kDirectSoundMethods = {{
	"QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
	"DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig",
	"SetSpeakerConfig", "Initialize"
}};
constexpr std::array<const char*, 21> kDirectSoundBufferMethods = {{
	"QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition", "GetFormat",
	"GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play",
	"SetCurrentPosition", "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop",
	"Unlock", "Restore"
}};
constexpr std::array<const char*, 17> kDirectSound3DListenerMethods = {{
	"QueryInterface", "AddRef", "Release", "GetAllParameters", "GetDistanceFactor",
	"GetDopplerFactor", "GetOrientation", "GetPosition", "GetRolloffFactor",
	"GetVelocity", "SetAllParameters", "SetDistanceFactor", "SetDopplerFactor",
	"SetOrientation", "SetPosition", "SetRolloffFactor", "SetVelocity"
}};

bool isGuestGuid(uint32_t address, const std::array<uint8_t, 16>& expected)
{
	const auto* bytes = sharedGuestAddressSpace().host(address, 16u);
	return bytes && std::memcmp(bytes, expected.data(), expected.size()) == 0;
}

bool ensureDdraw7Object(ImportBindingTable* imports)
{
	if (!imports) return false;
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kDdrawObject, 0x7000u, error)) return false;
	guest::store<uint32_t>(kDdrawObject, kDdrawVtable);
	guest::store<uint32_t>(kDdrawObject + 4u, 1u);
	guest::store<uint32_t>(kDdrawObject + 8u, 0u);
	for (uint32_t i = 0; i < kDdraw7Methods.size(); ++i) {
		const std::string method = std::string("IDirectDraw7::") + kDdraw7Methods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("ddraw.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDdrawVtable + i * 4u, stub);
	}
	guest::store<uint32_t>(kD3d7Object, kD3d7Vtable);
	guest::store<uint32_t>(kD3d7Object + 4u, 1u);
	for (uint32_t i = 0; i < kD3d7Methods.size(); ++i) {
		const std::string method = std::string("IDirect3D7::") + kD3d7Methods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("ddraw.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kD3d7Vtable + i * 4u, stub);
	}
	for (uint32_t i = 0; i < kDdrawSurfaceMethods.size(); ++i) {
		const std::string method = std::string("IDirectDrawSurface7::") + kDdrawSurfaceMethods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("ddraw.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDdrawSurfaceVtable + i * 4u, stub);
	}
	for (uint32_t i = 0; i < kD3d7DeviceMethods.size(); ++i) {
		const std::string method = std::string("IDirect3DDevice7::") + kD3d7DeviceMethods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("ddraw.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kD3d7DeviceVtable + i * 4u, stub);
	}
	for (uint32_t i = 0; i < kD3d7VertexBufferMethods.size(); ++i) {
		const std::string method = std::string("IDirect3DVertexBuffer7::") + kD3d7VertexBufferMethods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("ddraw.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kD3d7VertexBufferVtable + i * 4u, stub);
	}
	return true;
}

constexpr std::array<uint8_t, 16> kIidUnknown = {{
	0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
}};
constexpr std::array<uint8_t, 16> kIidDirectInput7A = {{
	0x84,0xb6,0x4c,0x9a,0x6d,0x23,0xd3,0x11,0x8e,0x9d,0x00,0xc0,0x4f,0x68,0x44,0xae
}};
constexpr std::array<uint8_t, 16> kIidDirectInputDevice7A = {{
	0xbc,0xc6,0xd7,0x57,0x56,0x23,0xd3,0x11,0x8e,0x9d,0x00,0xc0,0x4f,0x68,0x44,0xae
}};
constexpr std::array<uint8_t, 16> kGuidSystemMouse = {{
	0x60,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00
}};
constexpr std::array<uint8_t, 16> kGuidSystemKeyboard = {{
	0x61,0x2b,0x1d,0x6f,0xa0,0xd5,0xcf,0x11,0xbf,0xc7,0x44,0x45,0x53,0x54,0x00,0x00
}};
constexpr uint32_t kDinputNoInterface = 0x80004002u;
constexpr uint32_t kDinputInvalidParam = 0x80070057u;
constexpr uint32_t kDinputUnsupported = 0x80004001u;
constexpr uint32_t kDinputDeviceNotRegistered = 0x80040154u;
constexpr uint32_t kDinputNotAcquired = 0x8007000cu;
constexpr uint32_t kDsoundNoAggregation = 0x8878000au;
constexpr uint32_t kDsoundControlUnavailable = 0x8878001eu;
constexpr uint32_t kDsoundInvalidParam = 0x80070057u;
constexpr uint32_t kDsoundNoDriver = 0x88780078u;
constexpr uint32_t kDsoundUninitialized = 0x887800aaU;
constexpr uint32_t kDsoundUnsupported = 0x80004001u;
constexpr std::array<uint8_t, 16> kIidDirectSound = {{
	0x83,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60
}};
constexpr std::array<uint8_t, 16> kIidDirectSoundBuffer = {{
	0x85,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60
}};
constexpr std::array<uint8_t, 16> kIidDirectSound3DListener = {{
	0x84,0xfa,0x9a,0x27,0x81,0x49,0xce,0x11,0xa5,0x21,0x00,0x20,0xaf,0x0b,0xe5,0x60
}};

bool ensureDirectInputVtables(ImportBindingTable* imports)
{
	if (!imports) return false;
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kDirectInputObjectBase, 0x7000u, error)) return false;
	for (uint32_t i = 0; i < kDirectInput7Methods.size(); ++i) {
		const std::string method = std::string("IDirectInput7A::") + kDirectInput7Methods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("dinput.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDirectInputVtable + i * 4u, stub);
	}
	for (uint32_t i = 0; i < kDirectInputDevice7Methods.size(); ++i) {
		const std::string method = std::string("IDirectInputDevice7A::") + kDirectInputDevice7Methods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("dinput.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDirectInputDeviceVtable + i * 4u, stub);
	}
	return true;
}

bool ensureDirectSoundVtables(ImportBindingTable* imports)
{
	if (!imports) return false;
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kDirectSoundObjectBase, 0x2000u, error)) return false;
	for (uint32_t i = 0; i < kDirectSoundMethods.size(); ++i) {
		const std::string method = std::string("IDirectSound::") + kDirectSoundMethods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("dsound.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDirectSoundVtable + i * 4u, stub);
	}
	for (uint32_t i = 0; i < kDirectSoundBufferMethods.size(); ++i) {
		const std::string method = std::string("IDirectSoundBuffer::") + kDirectSoundBufferMethods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("dsound.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDirectSoundBufferVtable + i * 4u, stub);
	}
	for (uint32_t i = 0; i < kDirectSound3DListenerMethods.size(); ++i) {
		const std::string method = std::string("IDirectSound3DListener::") + kDirectSound3DListenerMethods[i];
		const uint32_t stub = imports->getOrCreateDynamicStub("dsound.dll", method, error);
		if (!stub) return false;
		guest::store<uint32_t>(kDirectSound3DListenerVtable + i * 4u, stub);
	}
	return true;
}

void directInputCreateEx(CPU* cpu, const ApiImportStub&)
{
	const StartupContext context = startupContext();
	const uint32_t instance = argument(cpu, 0);
	const uint32_t version = argument(cpu, 1);
	const uint32_t iid = argument(cpu, 2);
	const uint32_t output = argument(cpu, 3);
	const uint32_t outer = argument(cpu, 4);
	if (instance != context.mainModuleBase || version != 0x700u || !output || outer ||
		!isGuestGuid(iid, kIidDirectInput7A)) {
		cpu->r[EAX] = kDinputInvalidParam;
		return;
	}
	if (!sharedGuestAddressSpace().host(output, 4u) || !ensureDirectInputVtables(context.imports)) {
		cpu->r[EAX] = 0x8007000eu; // E_OUTOFMEMORY.
		return;
	}
	uint32_t object = 0;
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		object = gNextDirectInputObject;
		gNextDirectInputObject += 0x100u;
		gDirectInputs.emplace(object, GuestDirectInput{object, 1u});
	}
	guest::store<uint32_t>(object, kDirectInputVtable);
	guest::store<uint32_t>(object + 4u, 1u);
	guest::store<uint32_t>(output, object);
	cpu->r[EAX] = 0u; // DI_OK for the requested version and IID.
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dinput] DirectInputCreateEx version=0x%04x iid=IDirectInput7A -> %08x\n",
			version, object);
}

void dinputInterfaceQueryInterface(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), iid = argument(cpu, 1), output = argument(cpu, 2);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDinputInvalidParam; return; }
	if (!isGuestGuid(iid, kIidUnknown) && !isGuestGuid(iid, kIidDirectInput7A)) {
		guest::store<uint32_t>(output, 0u);
		cpu->r[EAX] = kDinputNoInterface;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		auto found = gDirectInputs.find(self);
		if (found == gDirectInputs.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
		++found->second.references;
		guest::store<uint32_t>(self + 4u, found->second.references);
	}
	guest::store<uint32_t>(output, self);
	cpu->r[EAX] = 0u;
}

void dinputInterfaceAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputs.find(self);
	if (found == gDirectInputs.end()) { cpu->r[EAX] = 0u; return; }
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	cpu->r[EAX] = found->second.references;
}

void dinputInterfaceRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputs.find(self);
	if (found == gDirectInputs.end()) { cpu->r[EAX] = 0u; return; }
	if (found->second.references) --found->second.references;
	guest::store<uint32_t>(self + 4u, found->second.references);
	cpu->r[EAX] = found->second.references;
}

uint32_t createDirectInputDevice(uint32_t input, uint32_t guidAddress,
	uint32_t iidAddress, uint32_t output, uint32_t outer)
{
	if (!output || !sharedGuestAddressSpace().host(output, 4u) || outer)
		return kDinputInvalidParam;
	uint32_t kind = 0;
	if (isGuestGuid(guidAddress, kGuidSystemMouse)) kind = 1u;
	else if (isGuestGuid(guidAddress, kGuidSystemKeyboard)) kind = 2u;
	if (!kind) { guest::store<uint32_t>(output, 0u); return kDinputDeviceNotRegistered; }
	if (iidAddress && !isGuestGuid(iidAddress, kIidDirectInputDevice7A) &&
		!isGuestGuid(iidAddress, kIidUnknown)) {
		guest::store<uint32_t>(output, 0u);
		return kDinputNoInterface;
	}
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		if (gDirectInputs.find(input) == gDirectInputs.end()) {
			guest::store<uint32_t>(output, 0u);
			return kDinputInvalidParam;
		}
	}
	const uint32_t device = gNextDirectInputDevice;
	gNextDirectInputDevice += 0x100u;
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		gDirectInputDevices.emplace(device, GuestDirectInputDevice{device, 1u, kind});
	}
	guest::store<uint32_t>(device, kDirectInputDeviceVtable);
	guest::store<uint32_t>(device + 4u, 1u);
	guest::store<uint32_t>(output, device);
	return 0u;
}

void dinputCreateDevice(CPU* cpu, const ApiImportStub&)
{
	const uint32_t result = createDirectInputDevice(argument(cpu, 0), argument(cpu, 1),
		0u, argument(cpu, 2), argument(cpu, 3));
	cpu->r[EAX] = result;
}

void dinputCreateDeviceEx(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = createDirectInputDevice(argument(cpu, 0), argument(cpu, 1),
		argument(cpu, 2), argument(cpu, 3), argument(cpu, 4));
	if (std::getenv("BR_TRACE_DDRAW") && cpu->r[EAX] == 0u)
		std::fprintf(stderr, "[br-dinput] CreateDeviceEx system=%s IID=IDirectInputDevice7A -> %08x\n",
			isGuestGuid(argument(cpu, 1), kGuidSystemMouse) ? "mouse" : "keyboard",
			guest::load<uint32_t>(argument(cpu, 3)));
}

void dinputDeviceQueryInterface(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), iid = argument(cpu, 1), output = argument(cpu, 2);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDinputInvalidParam; return; }
	if (!isGuestGuid(iid, kIidUnknown) && !isGuestGuid(iid, kIidDirectInputDevice7A)) {
		guest::store<uint32_t>(output, 0u);
		cpu->r[EAX] = kDinputNoInterface;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	guest::store<uint32_t>(output, self);
	cpu->r[EAX] = 0u;
}

void dinputDeviceAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = 0u; return; }
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	cpu->r[EAX] = found->second.references;
}

void dinputDeviceRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = 0u; return; }
	if (found->second.references) --found->second.references;
	guest::store<uint32_t>(self + 4u, found->second.references);
	cpu->r[EAX] = found->second.references;
}

void dinputDeviceSetDataFormat(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), format = argument(cpu, 1);
	const auto* header = sharedGuestAddressSpace().host(format, 24u);
	if (!header) { cpu->r[EAX] = kDinputInvalidParam; return; }
	const uint32_t size = guest::load<uint32_t>(format);
	const uint32_t objectSize = guest::load<uint32_t>(format + 4u);
	const uint32_t dataSize = guest::load<uint32_t>(format + 12u);
	const uint32_t objectCount = guest::load<uint32_t>(format + 16u);
	const uint32_t objects = guest::load<uint32_t>(format + 20u);
	if (size < 24u || objectSize < 16u || !dataSize || dataSize > 0x10000u ||
		objectCount > 0x1000u || (objectCount && !sharedGuestAddressSpace().host(objects,
			static_cast<size_t>(objectCount) * objectSize))) {
		cpu->r[EAX] = kDinputInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	found->second.dataFormat = format;
	found->second.dataSize = dataSize;
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dinput] SetDataFormat device=%08x bytes=%u objects=%u flags=%08x\n",
			self, dataSize, objectCount, guest::load<uint32_t>(format + 8u));
}

void dinputDeviceSetCooperativeLevel(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), window = argument(cpu, 1), flags = argument(cpu, 2);
	const uint32_t access = flags & 3u, foreground = flags & 0x0cu;
	const bool validWindow = !window || gGuestWindows.find(window) != gGuestWindows.end();
	if ((access != 1u && access != 2u) || (foreground != 4u && foreground != 8u) ||
		(flags & ~0x1fu) || !validWindow || (!window && foreground == 4u)) {
		cpu->r[EAX] = kDinputInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	found->second.cooperativeWindow = window;
	found->second.cooperativeFlags = flags;
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dinput] SetCooperativeLevel device=%08x hwnd=%08x flags=%08x\n",
			self, window, flags);
}

void dinputDeviceAcquire(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end() || !found->second.dataFormat ||
		!found->second.cooperativeFlags) { cpu->r[EAX] = kDinputNotAcquired; return; }
	found->second.acquired = true;
	cpu->r[EAX] = 0u;
}

void dinputDeviceUnacquire(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	found->second.acquired = false;
	cpu->r[EAX] = 0u;
}

void dinputDeviceGetState(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), size = argument(cpu, 1), data = argument(cpu, 2);
	if (!data || !sharedGuestAddressSpace().host(data, size)) { cpu->r[EAX] = kDinputInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	if (!found->second.acquired) { cpu->r[EAX] = kDinputNotAcquired; return; }
	std::memset(sharedGuestAddressSpace().host(data, size), 0, size);
	if (found->second.kind == 2u) {
		const uint32_t bytes = std::min(size, static_cast<uint32_t>(gDirectInputKeyboardState.size()));
		for (uint32_t i = 0; i < bytes; ++i) guest::store<uint8_t>(data + i, gDirectInputKeyboardState[i]);
	} else {
		if (size >= 16u) {
			const int32_t x = found->second.absoluteMouse ? gDirectInputPointerX : gDirectInputMouseX;
			const int32_t y = found->second.absoluteMouse ? gDirectInputPointerY : gDirectInputMouseY;
			guest::store<uint32_t>(data, static_cast<uint32_t>(x));
			guest::store<uint32_t>(data + 4u, static_cast<uint32_t>(y));
			guest::store<uint32_t>(data + 8u, static_cast<uint32_t>(gDirectInputMouseWheel));
			for (uint32_t i = 0; i < gDirectInputMouseButtons.size(); ++i)
				guest::store<uint8_t>(data + 12u + i, gDirectInputMouseButtons[i]);
			if (!found->second.absoluteMouse) gDirectInputMouseX = gDirectInputMouseY = 0;
			gDirectInputMouseWheel = 0;
		}
	}
	cpu->r[EAX] = 0u;
}

void dinputDeviceGetData(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), elementSize = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2), countAddress = argument(cpu, 3);
	if (!countAddress || !sharedGuestAddressSpace().host(countAddress, 4u) ||
		(output && (!elementSize || elementSize < 16u))) {
		cpu->r[EAX] = kDinputInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	if (!found->second.acquired) { cpu->r[EAX] = kDinputNotAcquired; return; }
	if (output && guest::load<uint32_t>(countAddress) &&
		!sharedGuestAddressSpace().host(output, static_cast<size_t>(elementSize) * guest::load<uint32_t>(countAddress))) {
		cpu->r[EAX] = kDinputInvalidParam;
		return;
	}
	guest::store<uint32_t>(countAddress, 0u); // No queued host input events yet.
	cpu->r[EAX] = 0u;
}

void dinputDevicePoll(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	cpu->r[EAX] = found != gDirectInputDevices.end() && found->second.acquired
		? 0u : kDinputNotAcquired;
}

void dinputGetDeviceStatus(CPU* cpu, const ApiImportStub&)
{
	const uint32_t guid = argument(cpu, 1);
	cpu->r[EAX] = isGuestGuid(guid, kGuidSystemMouse) || isGuestGuid(guid, kGuidSystemKeyboard)
		? 0u : kDinputDeviceNotRegistered;
}

void dinputDeviceGetCapabilities(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 24u) || guest::load<uint32_t>(output) < 24u) {
		cpu->r[EAX] = kDinputInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	guest::store<uint32_t>(output + 4u, 1u); // DIDC_ATTACHED.
	guest::store<uint32_t>(output + 8u, found->second.kind == 1u ? 0x0102u : 0x0403u);
	guest::store<uint32_t>(output + 12u, found->second.kind == 1u ? 3u : 0u);
	guest::store<uint32_t>(output + 16u, found->second.kind == 1u ? 3u : 128u);
	guest::store<uint32_t>(output + 20u, 0u);
	cpu->r[EAX] = 0u;
}

void dinputDeviceGetProperty(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), property = argument(cpu, 1), header = argument(cpu, 2);
	if (!header || !sharedGuestAddressSpace().host(header, 20u)) { cpu->r[EAX] = kDinputInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	if (property == 1u) guest::store<uint32_t>(header + 16u, found->second.bufferSize);
	else if (property == 2u) guest::store<uint32_t>(header + 16u, found->second.absoluteMouse ? 1u : 0u);
	else { cpu->r[EAX] = kDinputUnsupported; return; }
	cpu->r[EAX] = 0u;
}

void dinputDeviceSetProperty(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), property = argument(cpu, 1), header = argument(cpu, 2);
	if (!header || !sharedGuestAddressSpace().host(header, 20u)) { cpu->r[EAX] = kDinputInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	if (property == 1u) found->second.bufferSize = std::min(guest::load<uint32_t>(header + 16u), 256u);
	else if (property == 2u) found->second.absoluteMouse = guest::load<uint32_t>(header + 16u) == 1u;
	else { cpu->r[EAX] = kDinputUnsupported; return; }
	cpu->r[EAX] = 0u;
}

void dinputDeviceSetEventNotification(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), event = argument(cpu, 1);
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	found->second.notificationEvent = event;
	cpu->r[EAX] = 0u;
}

void dinputDeviceGetInfo(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 40u)) { cpu->r[EAX] = kDinputInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectInputMutex);
	auto found = gDirectInputDevices.find(self);
	if (found == gDirectInputDevices.end()) { cpu->r[EAX] = kDinputInvalidParam; return; }
	const uint32_t size = guest::load<uint32_t>(output);
	if (size < 40u) { cpu->r[EAX] = kDinputInvalidParam; return; }
	const auto& guid = found->second.kind == 1u ? kGuidSystemMouse : kGuidSystemKeyboard;
	for (uint32_t i = 0; i < guid.size(); ++i) {
		guest::store<uint8_t>(output + 4u + i, guid[i]);
		guest::store<uint8_t>(output + 20u + i, guid[i]);
	}
	guest::store<uint32_t>(output + 36u, found->second.kind == 1u ? 0x0102u : 0x0403u);
	cpu->r[EAX] = 0u;
}

void dinputUnsupported(CPU* cpu, const ApiImportStub&) { cpu->r[EAX] = kDinputUnsupported; }

void directSoundUnsupported(CPU* cpu, const ApiImportStub& import)
{
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] unsupported observed method %s\n", import.name.c_str());
	cpu->r[EAX] = kDsoundUnsupported;
}

void directSoundCreate(CPU* cpu, const ApiImportStub&)
{
	const uint32_t guid = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	const uint32_t outer = argument(cpu, 2);
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] DirectSoundCreate entry guid=%08x out=%08x outer=%08x\n", guid, output, outer);
	if (!output || outer || guid) {
		// This title requests the default playback device (NULL GUID). Other
		// device GUIDs are not silently redirected to the default device.
		if (output && sharedGuestAddressSpace().host(output, 4u)) guest::store<uint32_t>(output, 0u);
		cpu->r[EAX] = outer ? kDsoundNoAggregation : guid ? kDsoundNoDriver : kDsoundInvalidParam;
		return;
	}
	const bool outputReady = sharedGuestAddressSpace().host(output, 4u) != nullptr;
	if (std::getenv("BR_TRACE_DDRAW")) std::fprintf(stderr, "[br-dsound] output memory valid=%u\n", outputReady ? 1u : 0u);
	const bool vtablesReady = outputReady && ensureDirectSoundVtables(startupContext().imports);
	if (std::getenv("BR_TRACE_DDRAW")) std::fprintf(stderr, "[br-dsound] vtables ready=%u\n", vtablesReady ? 1u : 0u);
	if (!vtablesReady) {
		cpu->r[EAX] = 0x8007000eu; // E_OUTOFMEMORY
		return;
	}
	uint32_t object = 0;
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		object = gNextDirectSoundObject;
		gNextDirectSoundObject += 0x20u;
		GuestDirectSound state{};
		state.handle = object;
		state.references = 1u;
		state.listenerParameters[0] = 64u;
		state.listenerParameters[9] = 0x3f800000u;  // Default front vector (0, 0, 1).
		state.listenerParameters[11] = 0x3f800000u; // Default top vector (0, 1, 0).
		state.listenerParameters[13] = 0x3f800000u; // DS3D_DEFAULTDISTANCEFACTOR.
		state.listenerParameters[14] = 0x3f800000u; // DS3D_DEFAULTROLLOFFFACTOR.
		state.listenerParameters[15] = 0x3f800000u; // DS3D_DEFAULTDOPPLERFACTOR.
		gDirectSounds.emplace(object, state);
	}
	if (std::getenv("BR_TRACE_DDRAW")) std::fprintf(stderr, "[br-dsound] object allocated=%08x\n", object);
	guest::store<uint32_t>(object, kDirectSoundVtable);
	guest::store<uint32_t>(object + 4u, 1u);
	guest::store<uint32_t>(output, object);
	cpu->r[EAX] = 0u; // DS_OK; object and vtable are present in guest memory.
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] DirectSoundCreate default device -> %08x\n", object);
}

void directSoundQueryInterface(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), iid = argument(cpu, 1), output = argument(cpu, 2);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (!isGuestGuid(iid, kIidUnknown) && !isGuestGuid(iid, kIidDirectSound)) {
		guest::store<uint32_t>(output, 0u);
		cpu->r[EAX] = 0x80004002u; // E_NOINTERFACE
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSounds.find(self);
	if (found == gDirectSounds.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	guest::store<uint32_t>(output, self);
	cpu->r[EAX] = 0u;
}

void directSoundAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSounds.find(self);
	if (found == gDirectSounds.end()) { cpu->r[EAX] = 0u; return; }
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	cpu->r[EAX] = found->second.references;
}

void directSoundRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSounds.find(self);
	if (found == gDirectSounds.end()) { cpu->r[EAX] = 0u; return; }
	if (found->second.references) --found->second.references;
	guest::store<uint32_t>(self + 4u, found->second.references);
	cpu->r[EAX] = found->second.references;
	if (!found->second.references) gDirectSounds.erase(found);
}

void directSoundSetCooperativeLevel(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), window = argument(cpu, 1), level = argument(cpu, 2);
	if (level < 1u || level > 4u) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	{
		std::lock_guard<std::mutex> windowLock(gWindowClassesMutex);
		if (!gGuestWindows.count(window)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSounds.find(self);
	if (found == gDirectSounds.end()) { cpu->r[EAX] = kDsoundUninitialized; return; }
	found->second.cooperativeWindow = window;
	found->second.cooperativeLevel = level;
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] SetCooperativeLevel hwnd=%08x level=%u\n", window, level);
}

void directSoundCreateBuffer(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), desc = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2), outer = argument(cpu, 3);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	guest::store<uint32_t>(output, 0u);
	if (!desc || !sharedGuestAddressSpace().host(desc, 36u) || outer) {
		cpu->r[EAX] = outer ? 0x8878000au : kDsoundInvalidParam;
		return;
	}
	const uint32_t descSize = guest::load<uint32_t>(desc);
	const uint32_t flags = guest::load<uint32_t>(desc + 4u);
	uint32_t bufferBytes = guest::load<uint32_t>(desc + 8u);
	const uint32_t format = guest::load<uint32_t>(desc + 16u);
	const bool primary = (flags & 0x00000001u) != 0u;
	if ((descSize != 20u && descSize != 36u) ||
		(primary ? (bufferBytes != 0u || format != 0u) : (!format || !bufferBytes ||
			!sharedGuestAddressSpace().host(format, 18u)))) {
		cpu->r[EAX] = kDsoundInvalidParam;
		return;
	}
	uint16_t channels = 2u, blockAlign = 4u, bits = 16u;
	uint32_t sampleRate = 44100u, averageRate = sampleRate * blockAlign;
	if (primary) {
		// DirectSound's primary-buffer descriptor carries no format or size;
		// the guest sets its format immediately after creation.
		bufferBytes = 24576u;
	} else {
		const uint16_t tag = guest::load<uint16_t>(format);
		channels = guest::load<uint16_t>(format + 2u);
		sampleRate = guest::load<uint32_t>(format + 4u);
		averageRate = guest::load<uint32_t>(format + 8u);
		blockAlign = guest::load<uint16_t>(format + 12u);
		bits = guest::load<uint16_t>(format + 14u);
		const uint16_t extraBytes = guest::load<uint16_t>(format + 16u);
		if (tag != 1u || !channels || channels > 2u || !sampleRate ||
			(bits != 8u && bits != 16u) || blockAlign != channels * (bits / 8u) ||
			averageRate != sampleRate * blockAlign || bufferBytes % blockAlign != 0u || extraBytes != 0u) {
			cpu->r[EAX] = 0x88780064u; // DSERR_BADFORMAT
			return;
		}
	}
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		auto device = gDirectSounds.find(self);
		if (device == gDirectSounds.end()) { cpu->r[EAX] = kDsoundUninitialized; return; }
		if (!device->second.cooperativeLevel) { cpu->r[EAX] = 0x887800aau; return; }
	}
	const uint32_t mappedBytes = (bufferBytes + 0xfffu) & ~0xfffu;
	uint32_t object = 0, data = 0;
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		object = gNextDirectSoundBuffer;
		gNextDirectSoundBuffer += 0x20u;
		data = gNextDirectSoundData;
		if (mappedBytes > 0x01000000u || data > 0x6f000000u - mappedBytes) {
			cpu->r[EAX] = 0x8007000eu;
			return;
		}
		gNextDirectSoundData += mappedBytes;
	}
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(object, 0x20u, error) ||
		!sharedGuestAddressSpace().commitRange(data, mappedBytes, error)) {
		cpu->r[EAX] = 0x8007000eu; // E_OUTOFMEMORY
		return;
	}
	if (auto* samples = sharedGuestAddressSpace().host(data, bufferBytes))
		std::memset(samples, 0, bufferBytes);
	else { cpu->r[EAX] = 0x8007000eu; return; }
	GuestDirectSoundBuffer state{};
	state.handle = object;
	state.parent = self;
	state.flags = flags;
	state.bytes = bufferBytes;
	state.sampleRate = sampleRate;
	state.averageBytesPerSecond = averageRate;
	state.channels = channels;
	state.blockAlign = blockAlign;
	state.bitsPerSample = bits;
	state.data = data;
	state.frequency = sampleRate;
	state.primary = primary;
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		auto device = gDirectSounds.find(self);
		if (device == gDirectSounds.end()) { cpu->r[EAX] = kDsoundUninitialized; return; }
		++device->second.references;
		guest::store<uint32_t>(self + 4u, device->second.references);
		gDirectSoundBuffers.emplace(object, state);
	}
	guest::store<uint32_t>(object, kDirectSoundBufferVtable);
	guest::store<uint32_t>(object + 4u, 1u);
	guest::store<uint32_t>(output, object);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] CreateSoundBuffer %s object=%08x bytes=%u fmt=%uHz/%uch/%ubit flags=%08x\n",
			primary ? "primary" : "secondary", object, bufferBytes, sampleRate, channels, bits, flags);
#if defined(__ANDROID__)
	if (allowAudioTraceLog(gAudioDsoundCreateLogCount, 16u))
		androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsAudio", "DSOUND_BUFFER_CREATED",
			"buffer=%08x primary=%u bytes=%u rate=%u channels=%u bits=%u align=%u flags=%08x",
			object, primary ? 1u : 0u, bufferBytes, sampleRate, channels, bits, blockAlign, flags);
#endif
}

void directSoundBufferQueryInterface(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), iid = argument(cpu, 1), output = argument(cpu, 2);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const bool bufferInterface = isGuestGuid(iid, kIidUnknown) || isGuestGuid(iid, kIidDirectSoundBuffer);
	const bool listenerInterface = isGuestGuid(iid, kIidDirectSound3DListener);
	std::unique_lock<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (!bufferInterface && (!listenerInterface || !found->second.primary || !(found->second.flags & 0x00000010u))) {
		guest::store<uint32_t>(output, 0u);
		cpu->r[EAX] = 0x80004002u; // E_NOINTERFACE.
		if (std::getenv("BR_TRACE_DDRAW"))
			std::fprintf(stderr, "[br-dsound] QueryInterface buffer=%08x iid=%08x -> E_NOINTERFACE flags=%08x primary=%u\n",
				self, iid, found->second.flags, found->second.primary ? 1u : 0u);
		return;
	}
	if (listenerInterface) {
		if (!found->second.listenerHandle) {
			const uint32_t object = gNextDirectSound3DListener;
			gNextDirectSound3DListener += 0x10u;
			std::string error;
			if (!sharedGuestAddressSpace().commitRange(object, 0x10u, error)) {
				guest::store<uint32_t>(output, 0u);
				cpu->r[EAX] = 0x8007000eu; // E_OUTOFMEMORY.
				return;
			}
			found->second.listenerHandle = object;
			gDirectSound3DListeners.emplace(object, GuestDirectSound3DListener{object, self});
			guest::store<uint32_t>(object, kDirectSound3DListenerVtable);
			guest::store<uint32_t>(object + 4u, 0u);
			guest::store<uint32_t>(object + 8u, self);
		}
		const uint32_t object = found->second.listenerHandle;
		guest::store<uint32_t>(object + 4u, guest::load<uint32_t>(object + 4u) + 1u);
		guest::store<uint32_t>(output, object);
		guest::store<uint32_t>(self + 4u, ++found->second.references);
		cpu->r[EAX] = 0u;
		if (std::getenv("BR_TRACE_DDRAW"))
			std::fprintf(stderr, "[br-dsound] QueryInterface buffer=%08x iid=IDirectSound3DListener -> %08x refs=%u\n",
				self, object, found->second.references);
		return;
	}
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	guest::store<uint32_t>(output, self);
	cpu->r[EAX] = 0u;
}

void directSound3DListenerAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto listener = gDirectSound3DListeners.find(self);
	if (listener == gDirectSound3DListeners.end()) { cpu->r[EAX] = 0u; return; }
	auto buffer = gDirectSoundBuffers.find(listener->second.parentBuffer);
	if (buffer == gDirectSoundBuffers.end()) { cpu->r[EAX] = 0u; return; }
	const uint32_t refs = guest::load<uint32_t>(self + 4u) + 1u;
	guest::store<uint32_t>(self + 4u, refs);
	guest::store<uint32_t>(buffer->first + 4u, ++buffer->second.references);
	cpu->r[EAX] = refs;
}

void directSound3DListenerRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	uint32_t parentDevice = 0;
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		auto listener = gDirectSound3DListeners.find(self);
		if (listener == gDirectSound3DListeners.end()) { cpu->r[EAX] = 0u; return; }
		auto buffer = gDirectSoundBuffers.find(listener->second.parentBuffer);
		if (buffer == gDirectSoundBuffers.end()) { gDirectSound3DListeners.erase(listener); cpu->r[EAX] = 0u; return; }
		uint32_t listenerRefs = guest::load<uint32_t>(self + 4u);
		if (listenerRefs) --listenerRefs;
		guest::store<uint32_t>(self + 4u, listenerRefs);
		if (buffer->second.references) --buffer->second.references;
		guest::store<uint32_t>(buffer->first + 4u, buffer->second.references);
		cpu->r[EAX] = listenerRefs;
		if (listenerRefs) return;
		buffer->second.listenerHandle = 0u;
		gDirectSound3DListeners.erase(listener);
		if (buffer->second.references) return;
		parentDevice = buffer->second.parent;
		gDirectSoundBuffers.erase(buffer);
		auto device = gDirectSounds.find(parentDevice);
		if (device != gDirectSounds.end() && device->second.references) {
			guest::store<uint32_t>(parentDevice + 4u, --device->second.references);
			if (!device->second.references) gDirectSounds.erase(device);
		}
	}
}

void directSound3DListenerUnsupported(CPU* cpu, const ApiImportStub& import)
{
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] unsupported observed method %s\n", import.name.c_str());
	cpu->r[EAX] = kDsoundUnsupported;
}

void directSoundBufferGetControl(CPU* cpu, bool volume)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) {
		cpu->r[EAX] = kDsoundInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto buffer = gDirectSoundBuffers.find(self);
	if (buffer == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const uint32_t requiredFlag = volume ? 0x00000080u : 0x00000040u;
	if (!(buffer->second.flags & requiredFlag)) {
		cpu->r[EAX] = kDsoundControlUnavailable;
		return;
	}
	const int32_t value = static_cast<int32_t>(volume ? buffer->second.volume : buffer->second.pan);
	guest::store<int32_t>(output, value);
	cpu->r[EAX] = 0u;
}

void directSoundBufferGetVolume(CPU* cpu, const ApiImportStub&) { directSoundBufferGetControl(cpu, true); }
void directSoundBufferGetPan(CPU* cpu, const ApiImportStub&) { directSoundBufferGetControl(cpu, false); }

void directSoundBufferSetControl(CPU* cpu, bool volume)
{
	const uint32_t self = argument(cpu, 0);
	const int32_t value = static_cast<int32_t>(argument(cpu, 1));
	const int32_t minimum = -10000;
	const int32_t maximum = volume ? 0 : 10000;
	if (value < minimum || value > maximum) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto buffer = gDirectSoundBuffers.find(self);
	if (buffer == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const uint32_t requiredFlag = volume ? 0x00000080u : 0x00000040u;
	if (!(buffer->second.flags & requiredFlag)) {
		cpu->r[EAX] = kDsoundControlUnavailable;
		return;
	}
	if (volume) buffer->second.volume = static_cast<uint32_t>(value);
	else buffer->second.pan = static_cast<uint32_t>(value);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDSOUND"))
		std::fprintf(stderr, "[br-dsound] %s object=%08x value=%d flags=%08x -> DS_OK\n",
			volume ? "SetVolume" : "SetPan", self, value, buffer->second.flags);
}

void directSoundBufferSetVolume(CPU* cpu, const ApiImportStub&) { directSoundBufferSetControl(cpu, true); }
void directSoundBufferSetPan(CPU* cpu, const ApiImportStub&) { directSoundBufferSetControl(cpu, false); }

void directSound3DListenerGetAllParameters(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 64u) || guest::load<uint32_t>(output) < 64u) {
		cpu->r[EAX] = kDsoundInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto listener = gDirectSound3DListeners.find(self);
	if (listener == gDirectSound3DListeners.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const auto buffer = gDirectSoundBuffers.find(listener->second.parentBuffer);
	if (buffer == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const auto device = gDirectSounds.find(buffer->second.parent);
	if (device == gDirectSounds.end()) { cpu->r[EAX] = kDsoundUninitialized; return; }
	for (uint32_t i = 0; i < device->second.listenerParameters.size(); ++i)
		guest::store<uint32_t>(output + i * 4u, device->second.listenerParameters[i]);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] IDirectSound3DListener::GetAllParameters object=%08x size=64 -> DS_OK\n", self);
}

void directSound3DListenerSetRolloffFactor(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	const uint32_t factorBits = argument(cpu, 1);
	float factor = 0.0f;
	std::memcpy(&factor, &factorBits, sizeof(factor));
	const uint32_t apply = argument(cpu, 2);
	if (!std::isfinite(factor) || factor < 0.0f || factor > 10.0f || apply > 1u) {
		cpu->r[EAX] = kDsoundInvalidParam;
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto listener = gDirectSound3DListeners.find(self);
	if (listener == gDirectSound3DListeners.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const auto buffer = gDirectSoundBuffers.find(listener->second.parentBuffer);
	if (buffer == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const auto device = gDirectSounds.find(buffer->second.parent);
	if (device == gDirectSounds.end()) { cpu->r[EAX] = kDsoundUninitialized; return; }
	device->second.listenerParameters[14] = argument(cpu, 1);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] IDirectSound3DListener::SetRolloffFactor %.3f apply=%u -> DS_OK\n",
			factor, apply);
}

void directSoundBufferAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = 0u; return; }
	guest::store<uint32_t>(self + 4u, ++found->second.references);
	cpu->r[EAX] = found->second.references;
}

void directSoundBufferRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	uint32_t parent = 0;
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		auto found = gDirectSoundBuffers.find(self);
		if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = 0u; return; }
		if (found->second.references) --found->second.references;
		guest::store<uint32_t>(self + 4u, found->second.references);
		cpu->r[EAX] = found->second.references;
		if (found->second.references) return;
		parent = found->second.parent;
		gDirectSoundBuffers.erase(found);
		auto device = gDirectSounds.find(parent);
		if (device != gDirectSounds.end() && device->second.references) {
			guest::store<uint32_t>(parent + 4u, --device->second.references);
			if (!device->second.references) gDirectSounds.erase(device);
		}
	}
}

void directSoundBufferGetCaps(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), caps = argument(cpu, 1);
	if (!caps || !sharedGuestAddressSpace().host(caps, 20u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (guest::load<uint32_t>(caps) < 20u) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	guest::store<uint32_t>(caps + 4u, found->second.flags | 0x00000008u); // DSBCAPS_LOCSOFTWARE
	guest::store<uint32_t>(caps + 8u, found->second.bytes);
	guest::store<uint32_t>(caps + 12u, 0u);
	guest::store<uint32_t>(caps + 16u, 0u);
	cpu->r[EAX] = 0u;
}

void directSoundBufferSetFormat(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), format = argument(cpu, 1);
	if (!format || !sharedGuestAddressSpace().host(format, 18u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const uint16_t tag = guest::load<uint16_t>(format);
	const uint16_t channels = guest::load<uint16_t>(format + 2u);
	const uint32_t sampleRate = guest::load<uint32_t>(format + 4u);
	const uint32_t averageRate = guest::load<uint32_t>(format + 8u);
	const uint16_t blockAlign = guest::load<uint16_t>(format + 12u);
	const uint16_t bits = guest::load<uint16_t>(format + 14u);
	const uint16_t extraBytes = guest::load<uint16_t>(format + 16u);
	if (tag != 1u || !channels || channels > 2u || !sampleRate ||
		(bits != 8u && bits != 16u) || blockAlign != channels * (bits / 8u) ||
		averageRate != sampleRate * blockAlign || extraBytes != 0u) {
		cpu->r[EAX] = 0x88780064u; // DSERR_BADFORMAT
		return;
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (!found->second.primary) { cpu->r[EAX] = kDsoundControlUnavailable; return; }
	const auto device = gDirectSounds.find(found->second.parent);
	if (device == gDirectSounds.end() || device->second.cooperativeLevel < 2u) {
		cpu->r[EAX] = kDsoundUninitialized;
		return;
	}
	found->second.sampleRate = sampleRate;
	found->second.averageBytesPerSecond = averageRate;
	found->second.channels = channels;
	found->second.blockAlign = blockAlign;
	found->second.bitsPerSample = bits;
	found->second.frequency = sampleRate;
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] primary SetFormat %uHz/%uch/%ubit\n", sampleRate, channels, bits);
}

void directSoundBufferPlay(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), reserved1 = argument(cpu, 1);
	const uint32_t reserved2 = argument(cpu, 2), flags = argument(cpu, 3);
	if (reserved1 || reserved2 || (flags & ~0x00000007u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	std::unique_lock<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (found->second.primary) {
		const auto device = gDirectSounds.find(found->second.parent);
		if (device == gDirectSounds.end() || device->second.cooperativeLevel < 2u) {
			cpu->r[EAX] = kDsoundUninitialized;
			return;
		}
	}
	found->second.playing = true;
	found->second.playFlags = flags;
	found->second.playbackPhase = 0.0;
	cpu->r[EAX] = 0u;
	const GuestDirectSoundBuffer diagnosticBuffer = found->second;
	lock.unlock();
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-dsound] Play buffer=%08x flags=%08x primary=%u\n",
			self, flags, diagnosticBuffer.primary ? 1u : 0u);
#if defined(__ANDROID__)
	if (allowAudioTraceLog(gAudioDsoundPlayLogCount, 20u)) {
		androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsAudio", "DSOUND_BUFFER_PLAY",
			"buffer=%08x primary=%u flags=%08x bytes=%u rate=%u frequency=%u channels=%u bits=%u cursor=%u volume=%d pan=%d",
			self, diagnosticBuffer.primary ? 1u : 0u, flags, diagnosticBuffer.bytes,
			diagnosticBuffer.sampleRate, diagnosticBuffer.frequency, diagnosticBuffer.channels,
			diagnosticBuffer.bitsPerSample, diagnosticBuffer.playCursor,
			static_cast<int32_t>(diagnosticBuffer.volume), static_cast<int32_t>(diagnosticBuffer.pan));
	}
#endif
}

void directSoundBufferGetCurrentPosition(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), playOutput = argument(cpu, 1), writeOutput = argument(cpu, 2);
	if ((playOutput && !sharedGuestAddressSpace().host(playOutput, 4u)) ||
		(writeOutput && !sharedGuestAddressSpace().host(writeOutput, 4u))) {
		cpu->r[EAX] = kDsoundInvalidParam; return;
	}
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (playOutput) guest::store<uint32_t>(playOutput, found->second.playCursor);
	if (writeOutput) guest::store<uint32_t>(writeOutput, found->second.writeCursor);
	cpu->r[EAX] = 0u;
}

void directSoundBufferSetCurrentPosition(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), position = argument(cpu, 1);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (position >= found->second.bytes || !found->second.blockAlign || position % found->second.blockAlign) {
		cpu->r[EAX] = kDsoundInvalidParam; return;
	}
	found->second.playCursor = position;
	found->second.playbackPhase = 0.0;
	cpu->r[EAX] = 0u;
}

void directSoundBufferGetFormat(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1), capacity = argument(cpu, 2), written = argument(cpu, 3);
	constexpr uint32_t kWaveFormatExSize = 18u;
	if ((output && capacity && !sharedGuestAddressSpace().host(output, std::min(capacity, kWaveFormatExSize))) ||
		(written && !sharedGuestAddressSpace().host(written, 4u))) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (written) guest::store<uint32_t>(written, kWaveFormatExSize);
	if (output && capacity >= kWaveFormatExSize) {
		guest::store<uint16_t>(output, 1u);
		guest::store<uint16_t>(output + 2u, found->second.channels);
		guest::store<uint32_t>(output + 4u, found->second.sampleRate);
		guest::store<uint32_t>(output + 8u, found->second.averageBytesPerSecond);
		guest::store<uint16_t>(output + 12u, found->second.blockAlign);
		guest::store<uint16_t>(output + 14u, found->second.bitsPerSample);
		guest::store<uint16_t>(output + 16u, 0u);
	}
	cpu->r[EAX] = 0u;
}

void directSoundBufferGetFrequency(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	guest::store<uint32_t>(output, found->second.frequency);
	cpu->r[EAX] = 0u;
}

void directSoundBufferGetStatus(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	const auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const uint32_t status = found->second.playing ? (1u | ((found->second.playFlags & 1u) ? 4u : 0u)) : 0u;
	guest::store<uint32_t>(output, status);
	cpu->r[EAX] = 0u;
}

void directSoundBufferLock(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), requestedOffset = argument(cpu, 1), requestedBytes = argument(cpu, 2);
	const uint32_t firstPtrOut = argument(cpu, 3), firstBytesOut = argument(cpu, 4);
	const uint32_t secondPtrOut = argument(cpu, 5), secondBytesOut = argument(cpu, 6), flags = argument(cpu, 7);
	if (!firstPtrOut || !firstBytesOut || !secondPtrOut || !secondBytesOut || (flags & ~3u) ||
		!sharedGuestAddressSpace().host(firstPtrOut, 4u) || !sharedGuestAddressSpace().host(firstBytesOut, 4u) ||
		!sharedGuestAddressSpace().host(secondPtrOut, 4u) || !sharedGuestAddressSpace().host(secondBytesOut, 4u)) {
		cpu->r[EAX] = kDsoundInvalidParam; return;
	}
	std::unique_lock<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	auto& buffer = found->second;
	if (buffer.locked) { cpu->r[EAX] = 0x88780032u; return; } // DSERR_INVALIDCALL.
	if ((flags & 2u) && (flags & 1u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const uint32_t offset = (flags & 1u) ? buffer.writeCursor : requestedOffset;
	const uint32_t bytes = (flags & 2u) ? buffer.bytes : requestedBytes;
	if (!bytes || offset >= buffer.bytes || bytes > buffer.bytes || offset % buffer.blockAlign || bytes % buffer.blockAlign) {
		cpu->r[EAX] = kDsoundInvalidParam; return;
	}
	const uint32_t firstBytes = std::min(bytes, buffer.bytes - offset);
	const uint32_t secondBytes = bytes - firstBytes;
	const uint32_t firstPtr = buffer.data + offset;
	const uint32_t secondPtr = secondBytes ? buffer.data : 0u;
	if (!sharedGuestAddressSpace().host(firstPtr, firstBytes) ||
		(secondBytes && !sharedGuestAddressSpace().host(secondPtr, secondBytes))) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	guest::store<uint32_t>(firstPtrOut, firstPtr);
	guest::store<uint32_t>(firstBytesOut, firstBytes);
	guest::store<uint32_t>(secondPtrOut, secondPtr);
	guest::store<uint32_t>(secondBytesOut, secondBytes);
	buffer.locked = true; buffer.lockOffset = offset; buffer.lockBytes = bytes;
	cpu->r[EAX] = 0u;
#if defined(__ANDROID__)
	const uint32_t bufferBytes = buffer.bytes, sampleRate = buffer.sampleRate;
	const uint32_t frequency = buffer.frequency, playCursor = buffer.playCursor;
	const uint32_t writeCursor = buffer.writeCursor;
	const uint16_t channels = buffer.channels, bits = buffer.bitsPerSample;
	const bool playing = buffer.playing;
#endif
	lock.unlock();
#if defined(__ANDROID__)
	if (allowAudioTraceLog(gAudioDsoundLockLogCount, 24u, 256u))
		androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsAudio", "DSOUND_BUFFER_LOCK",
			"buffer=%08x bytes=%u rate=%u frequency=%u channels=%u bits=%u playing=%u cursor=%u write_cursor=%u request_offset=%u request_bytes=%u actual_offset=%u actual_bytes=%u flags=%08x wrapped=%u",
			self, bufferBytes, sampleRate, frequency, channels, bits, playing ? 1u : 0u,
			playCursor, writeCursor, requestedOffset, requestedBytes, offset, bytes, flags,
			secondBytes ? 1u : 0u);
#endif
}

void directSoundBufferUnlock(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), firstPtr = argument(cpu, 1), firstBytes = argument(cpu, 2);
	const uint32_t secondPtr = argument(cpu, 3), secondBytes = argument(cpu, 4);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	auto& buffer = found->second;
	if (!buffer.locked) { cpu->r[EAX] = 0x88780032u; return; }
	const uint32_t expectedFirst = std::min(buffer.lockBytes, buffer.bytes - buffer.lockOffset);
	const uint32_t expectedSecond = buffer.lockBytes - expectedFirst;
	if (firstPtr != buffer.data + buffer.lockOffset || firstBytes != expectedFirst ||
		secondPtr != (expectedSecond ? buffer.data : 0u) || secondBytes != expectedSecond) {
		cpu->r[EAX] = kDsoundInvalidParam; return;
	}
	buffer.writeCursor = (buffer.lockOffset + buffer.lockBytes) % buffer.bytes;
	buffer.locked = false; buffer.lockOffset = buffer.lockBytes = 0u;
	cpu->r[EAX] = 0u;
}

void directSoundBufferSetFrequency(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0), requested = argument(cpu, 1);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	if (requested && (requested < 100u || requested > 200000u)) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	const uint32_t frequency = requested ? requested : found->second.sampleRate;
	found->second.frequency = frequency;
	cpu->r[EAX] = 0u;
}

void directSoundBufferStop(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDirectSoundMutex);
	auto found = gDirectSoundBuffers.find(self);
	if (found == gDirectSoundBuffers.end()) { cpu->r[EAX] = kDsoundInvalidParam; return; }
	found->second.playing = false; found->second.playFlags = 0u; found->second.playbackPhase = 0.0;
	cpu->r[EAX] = 0u;
}


void directDrawCreateEx(CPU* cpu, const ApiImportStub&)
{
	constexpr std::array<uint8_t, 16> iidDirectDraw7 = {{
		0xc0,0x5e,0xe6,0x15,0x9c,0x3b,0xd2,0x11,0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b
	}};
	const StartupContext context = startupContext();
	const uint32_t output = argument(cpu, 1);
	const uint32_t iid = argument(cpu, 2);
	const uint32_t outer = argument(cpu, 3);
	if (!output || outer || !isGuestGuid(iid, iidDirectDraw7) || !ensureDdraw7Object(context.imports)) {
		cpu->r[EAX] = 0x80004002u; // E_NOINTERFACE / invalid aggregation.
		return;
	}
	guest::store<uint32_t>(output, kDdrawObject);
	cpu->r[EAX] = 0u; // DD_OK
}

void ddrawQueryInterface(CPU* cpu, const ApiImportStub&)
{
	constexpr std::array<uint8_t, 16> iidDirectDraw7 = {{
		0xc0,0x5e,0xe6,0x15,0x9c,0x3b,0xd2,0x11,0xb9,0x2f,0x00,0x60,0x97,0x97,0xea,0x5b
	}};
	constexpr std::array<uint8_t, 16> iidUnknown = {{
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xc0,0x00,0x00,0x00,0x00,0x00,0x00,0x46
	}};
	constexpr std::array<uint8_t, 16> iidDirect3D7 = {{
		0x77,0x9e,0x04,0xf5,0x61,0x48,0xd2,0x11,0xa4,0x07,0x00,0xa0,0xc9,0x06,0x29,0xa8
	}};
	const uint32_t self = argument(cpu, 0);
	const uint32_t iid = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2);
	if (!output) { cpu->r[EAX] = 0x80004003u; return; } // E_POINTER
	const bool drawInterface = self == kDdrawObject &&
		(isGuestGuid(iid, iidDirectDraw7) || isGuestGuid(iid, iidUnknown));
	const bool d3dInterface = self == kDdrawObject && isGuestGuid(iid, iidDirect3D7);
	const bool d3dIdentity = self == kD3d7Object &&
		(isGuestGuid(iid, iidDirect3D7) || isGuestGuid(iid, iidUnknown));
	if (!drawInterface && !d3dInterface && !d3dIdentity) {
		guest::store<uint32_t>(output, 0u);
		cpu->r[EAX] = 0x80004002u; // E_NOINTERFACE
		return;
	}
	const uint32_t object = d3dInterface || d3dIdentity ? kD3d7Object : kDdrawObject;
	guest::store<uint32_t>(output, object);
	guest::store<uint32_t>(object + 4u, guest::load<uint32_t>(object + 4u) + 1u);
	cpu->r[EAX] = 0u;
}

void ddrawAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	const uint32_t object = self == kD3d7Object ? kD3d7Object : kDdrawObject;
	const uint32_t references = guest::load<uint32_t>(object + 4u) + 1u;
	guest::store<uint32_t>(object + 4u, references);
	cpu->r[EAX] = references;
}

void ddrawRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	const uint32_t object = self == kD3d7Object ? kD3d7Object : kDdrawObject;
	const uint32_t previous = guest::load<uint32_t>(object + 4u);
	const uint32_t references = previous ? previous - 1u : 0u;
	guest::store<uint32_t>(object + 4u, references);
	cpu->r[EAX] = references;
}

void ddrawSetDisplayMode(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	const uint32_t width = argument(cpu, 1);
	const uint32_t height = argument(cpu, 2);
	const uint32_t bpp = argument(cpu, 3);
	const uint32_t refresh = argument(cpu, 4);
	const uint32_t flags = argument(cpu, 5);
	if (self != kDdrawObject || flags != 0u) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS.
		return;
	}
	const bool listed = std::any_of(kGuestDisplayModes.begin(), kGuestDisplayModes.end(),
		[width, height, bpp](const auto& mode) {
			return mode[0] == width && mode[1] == height && mode[2] == bpp;
		});
	if (!listed || (refresh != 0u && refresh != 60u)) {
		cpu->r[EAX] = 0x88760078u; // DDERR_INVALIDMODE = MAKE_DDHRESULT(120).
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gDisplayModeMutex);
		gCurrentGuestDisplayMode = {{width, height, bpp, refresh}};
	}
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] SetDisplayMode accepted enumerated guest mode %ux%ux%u refresh=%u\n",
			width, height, bpp, refresh);
	}
	cpu->r[EAX] = 0u; // DD_OK; the selected mode is recorded for later GetDisplayMode/surface calls.
}

void ddrawGetDisplayMode(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDdsCapsPrimary = 0x00000200u;
	constexpr uint32_t kDdsdDisplayMode = 0x0000100fu; // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT.
	constexpr uint32_t kDdpfrgb = 0x00000040u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t desc = argument(cpu, 1);
	if (self != kDdrawObject || !desc || !sharedGuestAddressSpace().host(desc, 108u) ||
		guest::load<uint32_t>(desc) < 108u || guest::load<uint32_t>(desc) > 4096u) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS.
		return;
	}
	std::array<uint32_t, 4> mode{};
	{
		std::lock_guard<std::mutex> lock(gDisplayModeMutex);
		mode = gCurrentGuestDisplayMode;
	}
	if (!mode[0] || !mode[1] || !mode[2]) {
		cpu->r[EAX] = 0x887600d4u; // DDERR_NOCOOPERATIVELEVELSET.
		return;
	}
	const uint32_t size = guest::load<uint32_t>(desc);
	std::memset(sharedGuestAddressSpace().host(desc + 4u, size - 4u), 0, size - 4u);
	guest::store<uint32_t>(desc + 4u, kDdsdDisplayMode);
	guest::store<uint32_t>(desc + 8u, mode[1]);
	guest::store<uint32_t>(desc + 12u, mode[0]);
	guest::store<uint32_t>(desc + 16u, ((mode[0] * ((mode[2] + 7u) / 8u)) + 3u) & ~3u);
	const uint32_t pf = desc + 72u;
	guest::store<uint32_t>(pf, 32u);
	guest::store<uint32_t>(pf + 4u, kDdpfrgb);
	guest::store<uint32_t>(pf + 12u, mode[2]);
	if (mode[2] == 16u) {
		guest::store<uint32_t>(pf + 16u, 0x0000f800u);
		guest::store<uint32_t>(pf + 20u, 0x000007e0u);
		guest::store<uint32_t>(pf + 24u, 0x0000001fu);
	}
	guest::store<uint32_t>(desc + 104u, kDdsCapsPrimary);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] GetDisplayMode reports %ux%ux%u pitch=%u\n",
			mode[0], mode[1], mode[2], guest::load<uint32_t>(desc + 16u));
	}
}

void ddrawFlipToGdiSurface(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidObject = 0x88760064u;
	constexpr uint32_t kDderrNotFound = 0x887600ffu;
	const uint32_t self = argument(cpu, 0);
	if (self != kDdrawObject || guest::load<uint32_t>(self) != kDdrawVtable) {
		cpu->r[EAX] = kDderrInvalidObject;
		return;
	}

	uint32_t primary = 0u;
	GuestDdrawSurface surface{};
	{
		std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
		primary = gGuestPrimarySurface;
		const auto found = gDdrawSurfaces.find(primary);
		if (!primary || found == gDdrawSurfaces.end() || !(found->second.caps & 0x00000200u)) {
			cpu->r[EAX] = kDderrNotFound;
			return;
		}
		surface = found->second;
	}

	// This headless backend represents the GDI-visible surface with the current
	// primary front surface. As that object is already primary, the DirectDraw
	// operation requires no buffer rotation; report success only after verifying
	// that the guest owns a live primary surface.
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-ddraw] FlipToGDISurface ddraw=%08x gdi-primary=%08x caps=%08x action=already-front\n",
			self, primary, surface.caps);
	cpu->r[EAX] = 0u; // DD_OK: modeled GDI surface is already the primary front surface.
}

void ddrawGetDeviceIdentifier(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDeviceIdentifier2Size = 1068u;
	constexpr uint32_t kDriverNameOffset = 0u;
	constexpr uint32_t kDescriptionOffset = 512u;
	constexpr uint32_t kDriverVersionOffset = 1024u;
	constexpr uint32_t kVendorIdOffset = 1032u;
	constexpr uint32_t kDeviceGuidOffset = 1048u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t identifier = argument(cpu, 1);
	const uint32_t flags = argument(cpu, 2);
	if (self != kDdrawObject || !identifier || flags != 0u ||
		!sharedGuestAddressSpace().host(identifier, kDeviceIdentifier2Size)) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS.
		return;
	}
	std::memset(sharedGuestAddressSpace().host(identifier, kDeviceIdentifier2Size), 0, kDeviceIdentifier2Size);
	constexpr char driver[] = "BattleRealmsRecomp";
	constexpr char description[] = "DirectDraw 7 compatibility renderer";
	for (uint32_t i = 0; i < sizeof(driver); ++i) guest::store<uint8_t>(identifier + kDriverNameOffset + i, static_cast<uint8_t>(driver[i]));
	for (uint32_t i = 0; i < sizeof(description); ++i) guest::store<uint8_t>(identifier + kDescriptionOffset + i, static_cast<uint8_t>(description[i]));
	guest::store<uint64_t>(identifier + kDriverVersionOffset, 1u);
	// Vendor/device IDs and the device GUID remain zero: this is a software
	// compatibility backend, not the Windows machine's physical graphics card.
	(void)kDeviceGuidOffset;
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] GetDeviceIdentifier -> software compatibility device\n");
	}
}

void d3d7CreateDevice(CPU* cpu, const ApiImportStub&)
{
	constexpr std::array<uint8_t, 16> iidHal = {{
		0xe0,0x3d,0xe6,0x84,0xaa,0x46,0xcf,0x11,0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e
	}};
	const uint32_t self = argument(cpu, 0);
	const uint32_t deviceGuid = argument(cpu, 1);
	const uint32_t renderTarget = argument(cpu, 2);
	const uint32_t output = argument(cpu, 3);
	if (self != kD3d7Object || !isGuestGuid(deviceGuid, iidHal) || !renderTarget || !output ||
		!sharedGuestAddressSpace().host(output, 4u)) {
		cpu->r[EAX] = 0x8876086au; // D3DERR_INVALIDDEVICE.
		return;
	}
	{
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		auto target = gDdrawSurfaces.find(renderTarget);
		if (target == gDdrawSurfaces.end()) {
			cpu->r[EAX] = 0x8876086au; // D3DERR_INVALIDDEVICE.
			return;
		}
		++target->second.references;
		guest::store<uint32_t>(renderTarget + 4u, target->second.references);
	}
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kD3d7DeviceObject, 0x20u, error)) {
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		auto target = gDdrawSurfaces.find(renderTarget);
		if (target != gDdrawSurfaces.end() && target->second.references > 1u) {
			--target->second.references;
			guest::store<uint32_t>(renderTarget + 4u, target->second.references);
		}
		cpu->r[EAX] = 0x8007000eu; // E_OUTOFMEMORY.
		return;
	}
	guest::store<uint32_t>(kD3d7DeviceObject, kD3d7DeviceVtable);
	guest::store<uint32_t>(kD3d7DeviceObject + 4u, 1u);
	GuestD3d7Device device{};
	device.handle = kD3d7DeviceObject;
	device.renderTarget = renderTarget;
	for (uint32_t channel = 0u; channel < 4u; ++channel) {
		device.material[channel] = 0x3f800000u; // Default diffuse RGBA = 1.
		device.material[4u + channel] = 0x3f800000u; // Default ambient RGBA = 1.
		device.material[8u + channel] = channel == 3u ? 0x3f800000u : 0u;
		device.material[12u + channel] = channel == 3u ? 0x3f800000u : 0u;
	}
	for (uint32_t state : {1u, 2u, 3u, 16u, 17u, 18u, 19u, 20u, 21u, 22u, 23u})
		for (uint32_t i = 0; i < 4u; ++i) device.transforms[state][i * 5u] = 0x3f800000u;
	GuestDdrawSurface targetInfo{};
	if (loadGuestSurface(renderTarget, targetInfo)) {
		device.viewport = {{0u, 0u, targetInfo.width, targetInfo.height, 0u, 0x3f800000u}};
	}
	device.renderState[7] = 1u;  // D3DRENDERSTATE_ZENABLE default.
	device.renderState[14] = 1u; // D3DRENDERSTATE_ZWRITEENABLE default.
	device.renderState[19] = 2u; // D3DBLEND_ONE.
	device.renderState[20] = 1u; // D3DBLEND_ZERO.
	device.renderState[22] = 3u; // D3DCULL_CCW.
	device.renderState[23] = 4u; // D3DCMP_LESSEQUAL.
	device.renderState[25] = 8u; // D3DRENDERSTATE_ALPHAFUNC = D3DCMP_ALWAYS.
	device.renderState[137] = 1u; // D3DRENDERSTATE_LIGHTING.
	device.renderState[141] = 1u; // D3DRENDERSTATE_COLORVERTEX.
	device.renderState[145] = 1u; // D3DRENDERSTATE_DIFFUSEMATERIALSOURCE = COLOR1.
	// Model the Direct3D device defaults so untouched texture states are not
	// mistaken for invalid enum zeroes by GetTextureStageState or rasterization.
	d3d7::initializeDefaultTextureStageStates(device.textureStageState);
	{
		std::lock_guard<std::mutex> devicesLock(gD3d7DevicesMutex);
		gD3d7Devices[kD3d7DeviceObject] = device;
	}
	guest::store<uint32_t>(output, kD3d7DeviceObject);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		GuestDdrawSurface target{};
		loadGuestSurface(renderTarget, target);
		std::fprintf(stderr, "[br-d3d7] CreateDevice HAL target=%08x %ux%ux%u z=%08x -> %08x\n",
			renderTarget, target.width, target.height, target.bpp, target.zBuffer, kD3d7DeviceObject);
	}
}

void d3d7DeviceAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
	auto found = gD3d7Devices.find(self);
	if (found == gD3d7Devices.end()) { cpu->r[EAX] = 0u; return; }
	cpu->r[EAX] = ++found->second.references;
	guest::store<uint32_t>(self + 4u, found->second.references);
}

void d3d7DeviceRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	uint32_t renderTarget = 0u;
	std::array<uint32_t, 8> textures{};
	uint32_t count = 0u;
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto found = gD3d7Devices.find(self);
		if (found == gD3d7Devices.end()) { cpu->r[EAX] = 0u; return; }
		count = found->second.references ? --found->second.references : 0u;
		renderTarget = found->second.renderTarget;
		textures = found->second.textures;
		guest::store<uint32_t>(self + 4u, count);
		if (!count) gD3d7Devices.erase(found);
	}
	if (!count) {
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		auto target = gDdrawSurfaces.find(renderTarget);
		if (target != gDdrawSurfaces.end() && target->second.references > 0u) {
			--target->second.references;
			guest::store<uint32_t>(renderTarget + 4u, target->second.references);
		}
		for (const uint32_t texture : textures) {
			auto foundTexture = gDdrawSurfaces.find(texture);
			if (!texture || foundTexture == gDdrawSurfaces.end()) continue;
			if (foundTexture->second.references > 0u) --foundTexture->second.references;
			guest::store<uint32_t>(texture + 4u, foundTexture->second.references);
			if (!foundTexture->second.references) {
				if (gGuestPrimarySurface == texture) gGuestPrimarySurface = 0u;
				gDdrawSurfaces.erase(foundTexture);
			}
		}
	}
	cpu->r[EAX] = count;
}

void d3d7DeviceBeginScene(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	constexpr uint32_t kD3derrSceneInScene = 0x887602f8u; // MAKE_DDHRESULT(760).
	const uint32_t self = argument(cpu, 0);
	uint32_t renderTarget = 0u;
	{
		std::lock_guard<std::mutex> devicesLock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end() || !device->second.renderTarget) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		if (device->second.inScene) {
			cpu->r[EAX] = kD3derrSceneInScene;
			return;
		}
		renderTarget = device->second.renderTarget;
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		if (gDdrawSurfaces.find(renderTarget) == gDdrawSurfaces.end()) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		device->second.inScene = true;
	}
	cpu->r[EAX] = 0u; // D3D_OK; this call begins a scene, it does not draw or present.
	if (std::getenv("BR_TRACE_DDRAW") || std::getenv("BR_TRACE_D3D7"))
		std::fprintf(stderr, "[br-d3d7] BeginScene device=%08x target=%08x result=D3D_OK\n",
			self, renderTarget);
}

void d3d7DeviceEndScene(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	constexpr uint32_t kD3derrSceneNotInScene = 0x887602f9u; // MAKE_DDHRESULT(761).
	const uint32_t self = argument(cpu, 0);
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		if (!device->second.inScene) {
			cpu->r[EAX] = kD3derrSceneNotInScene;
			return;
		}
		device->second.inScene = false;
	}
	cpu->r[EAX] = 0u; // D3D_OK; presentation remains a separate operation.
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-d3d7] EndScene device=%08x result=D3D_OK\n", self);
}

void d3d7DeviceSetRenderTarget(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t targetHandle = argument(cpu, 1);
	const uint32_t flags = argument(cpu, 2);
	if (!targetHandle || flags != 0u) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	uint32_t previousTarget = 0u;
	{
		std::lock_guard<std::mutex> devicesLock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		previousTarget = device->second.renderTarget;
		if (previousTarget == targetHandle) { cpu->r[EAX] = 0u; return; }
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		auto target = gDdrawSurfaces.find(targetHandle);
		if (target == gDdrawSurfaces.end() || (target->second.caps & 0x00000200u)) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		++target->second.references;
		guest::store<uint32_t>(targetHandle + 4u, target->second.references);
		device->second.renderTarget = targetHandle;
		if (previousTarget) {
			auto previous = gDdrawSurfaces.find(previousTarget);
			if (previous != gDdrawSurfaces.end() && previous->second.references > 0u) {
				--previous->second.references;
				guest::store<uint32_t>(previousTarget + 4u, previous->second.references);
			}
		}
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] SetRenderTarget device=%08x target=%08x previous=%08x\n",
			self, targetHandle, previousTarget);
	}
}

void d3d7DeviceGetRenderTarget(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	uint32_t targetHandle = 0u;
	{
		std::lock_guard<std::mutex> devicesLock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end() || !device->second.renderTarget) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		targetHandle = device->second.renderTarget;
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		auto target = gDdrawSurfaces.find(targetHandle);
		if (target == gDdrawSurfaces.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		++target->second.references;
		guest::store<uint32_t>(targetHandle + 4u, target->second.references);
	}
	guest::store<uint32_t>(output, targetHandle);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-d3d7] GetRenderTarget device=%08x -> %08x (AddRef)\n", self, targetHandle);
}

void d3d7DeviceSetViewport(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t viewport = argument(cpu, 1);
	const auto* data = viewport ? sharedGuestAddressSpace().host(viewport, 24u) : nullptr;
	if (!data) { cpu->r[EAX] = kD3derrInvalidParams; return; }
	std::array<uint32_t, 6> values{};
	for (uint32_t i = 0; i < values.size(); ++i) values[i] = guest::load<uint32_t>(viewport + i * 4u);
	if (values[2] == 0u || values[3] == 0u) { cpu->r[EAX] = kD3derrInvalidParams; return; }
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
		device->second.viewport = values;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		float minZ = 0.0f, maxZ = 0.0f;
		std::memcpy(&minZ, &values[4], sizeof(minZ));
		std::memcpy(&maxZ, &values[5], sizeof(maxZ));
		std::fprintf(stderr, "[br-d3d7] SetViewport x=%u y=%u %ux%u z=%g..%g\n",
			values[0], values[1], values[2], values[3], minZ, maxZ);
	}
}

void d3d7DeviceGetViewport(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 24u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	std::array<uint32_t, 6> values{};
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		const auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		values = device->second.viewport;
	}
	for (uint32_t i = 0; i < values.size(); ++i)
		guest::store<uint32_t>(output + i * 4u, values[i]);
	cpu->r[EAX] = 0u; // D3D_OK; returns the device's current viewport state.
	if (std::getenv("BR_TRACE_DDRAW")) {
		float minZ = 0.0f, maxZ = 0.0f;
		std::memcpy(&minZ, &values[4], sizeof(minZ));
		std::memcpy(&maxZ, &values[5], sizeof(maxZ));
		std::fprintf(stderr, "[br-d3d7] GetViewport device=%08x x=%u y=%u %ux%u z=%g..%g\n",
			self, values[0], values[1], values[2], values[3], minZ, maxZ);
	}
}

void d3d7DeviceSetTransform(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t state = argument(cpu, 1);
	const uint32_t matrix = argument(cpu, 2);
	const auto* data = matrix ? sharedGuestAddressSpace().host(matrix, 64u) : nullptr;
	const bool validState = state == 1u || state == 2u || state == 3u || (state >= 16u && state <= 23u);
	if (!validState || !data) { cpu->r[EAX] = kD3derrInvalidParams; return; }
	std::array<uint32_t, 16> values{};
	for (uint32_t i = 0; i < values.size(); ++i) values[i] = guest::load<uint32_t>(matrix + i * 4u);
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
		device->second.transforms[state] = values;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-d3d7] SetTransform state=%u matrix=%08x -> D3D_OK\n", state, matrix);
}

void d3d7DeviceGetTransform(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t state = argument(cpu, 1);
	const uint32_t matrix = argument(cpu, 2);
	const auto* output = matrix ? sharedGuestAddressSpace().host(matrix, 64u) : nullptr;
	const bool validState = state == 1u || state == 2u || state == 3u || (state >= 16u && state <= 23u);
	if (!validState || !output) { cpu->r[EAX] = kD3derrInvalidParams; return; }
	std::array<uint32_t, 16> values{};
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
		values = device->second.transforms[state];
	}
	for (uint32_t i = 0; i < values.size(); ++i) guest::store<uint32_t>(matrix + i * 4u, values[i]);
	cpu->r[EAX] = 0u;
}

void d3d7DeviceSetRenderState(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t state = argument(cpu, 1);
	const uint32_t value = argument(cpu, 2);
	if (state >= 256u) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		device->second.renderState[state] = value;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] SetRenderState state=%u value=%08x\n", state, value);
	}
}

void d3d7DeviceSetMaterial(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t materialAddress = argument(cpu, 1);
	if (!materialAddress || !sharedGuestAddressSpace().host(materialAddress, 84u)) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
	auto device = gD3d7Devices.find(self);
	if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
	for (uint32_t i = 0; i < device->second.material.size(); ++i) {
		device->second.material[i] = guest::load<uint32_t>(materialAddress + i * 4u);
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] SetMaterial diffuse=%08x,%08x,%08x,%08x power=%08x\n",
			device->second.material[0], device->second.material[1], device->second.material[2],
			device->second.material[3], device->second.material[16]);
	}
}

void d3d7DeviceGetMaterial(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t materialAddress = argument(cpu, 1);
	if (!materialAddress || !sharedGuestAddressSpace().host(materialAddress, 84u)) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
	auto device = gD3d7Devices.find(self);
	if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
	for (uint32_t i = 0; i < device->second.material.size(); ++i) {
		guest::store<uint32_t>(materialAddress + i * 4u, device->second.material[i]);
	}
	cpu->r[EAX] = 0u;
}

uint32_t d3d7MaxActiveLights()
{
	constexpr uint32_t kMaxActiveLightsOffset = 148u;
	return std::min<uint32_t>(guest::load<uint32_t>(kD3d7DeviceDesc + kMaxActiveLightsOffset), 8u);
}

void d3d7DeviceSetLight(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	constexpr uint32_t kLightSize = 104u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t index = argument(cpu, 1);
	const uint32_t lightAddress = argument(cpu, 2);
	if (index >= d3d7MaxActiveLights() || !lightAddress ||
		!sharedGuestAddressSpace().host(lightAddress, kLightSize)) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	std::array<uint32_t, 26> values{};
	for (uint32_t i = 0; i < values.size(); ++i)
		values[i] = guest::load<uint32_t>(lightAddress + i * 4u);
	if (values[0] < 1u || values[0] > 3u) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
		device->second.lights[index].values = values;
		device->second.lights[index].configured = true;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] SetLight index=%u type=%u diffuse=%08x,%08x,%08x,%08x ambient=%08x,%08x,%08x,%08x pos=%08x,%08x,%08x dir=%08x,%08x,%08x\n",
			index, values[0], values[1], values[2], values[3], values[4],
			values[9], values[10], values[11], values[12], values[13], values[14], values[15],
			values[16], values[17], values[18]);
	}
}

void d3d7DeviceGetLight(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidParams = 0x8876086cu;
	constexpr uint32_t kLightSize = 104u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t index = argument(cpu, 1);
	const uint32_t lightAddress = argument(cpu, 2);
	if (index >= d3d7MaxActiveLights() || !lightAddress ||
		!sharedGuestAddressSpace().host(lightAddress, kLightSize)) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	std::array<uint32_t, 26> values{};
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end() || !device->second.lights[index].configured) {
			cpu->r[EAX] = kDderrInvalidParams;
			return;
		}
		values = device->second.lights[index].values;
	}
	for (uint32_t i = 0; i < values.size(); ++i)
		guest::store<uint32_t>(lightAddress + i * 4u, values[i]);
	cpu->r[EAX] = 0u;
}

void d3d7DeviceLightEnable(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t index = argument(cpu, 1);
	const uint32_t enable = argument(cpu, 2);
	if (index >= d3d7MaxActiveLights()) { cpu->r[EAX] = kDderrInvalidParams; return; }
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kDderrInvalidParams; return; }
		device->second.lights[index].enabled = enable != 0u;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-d3d7] LightEnable index=%u enable=%u\n", index, enable != 0u ? 1u : 0u);
}

void d3d7DeviceGetLightEnable(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t index = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2);
	if (index >= d3d7MaxActiveLights() || !output || !sharedGuestAddressSpace().host(output, 4u)) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	bool enabled = false;
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kDderrInvalidParams; return; }
		enabled = device->second.lights[index].enabled;
	}
	guest::store<uint32_t>(output, enabled ? 1u : 0u);
	cpu->r[EAX] = 0u;
}

void d3d7DeviceGetCaps(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3dDeviceDesc7Size = 236u;
	constexpr uint32_t kD3derrInvalidParams = 0x80070057u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, kD3dDeviceDesc7Size)) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		if (gD3d7Devices.find(self) == gD3d7Devices.end()) {
			cpu->r[EAX] = kD3derrInvalidParams;
			return;
		}
	}
	const auto* caps = sharedGuestAddressSpace().host(kD3d7DeviceDesc, kD3dDeviceDesc7Size);
	if (!caps) { cpu->r[EAX] = 0x80004005u; return; }
	std::memcpy(sharedGuestAddressSpace().host(output, kD3dDeviceDesc7Size), caps, kD3dDeviceDesc7Size);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] GetCaps copied %u-byte advertised HAL descriptor caps=%08x\n",
			kD3dDeviceDesc7Size, guest::load<uint32_t>(output));
	}
}

uint32_t d3d7FvfStride(uint32_t fvf)
{
	d3d7::FvfLayout layout{};
	return d3d7::decodeFvf(fvf, layout) ? layout.stride : 0u;
}

void d3d7DeviceDrawIndexedPrimitive(CPU* cpu, const ApiImportStub&)
{
#if defined(__ANDROID__)
	D3dDrawProfileScope profileScope;
#endif
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	constexpr uint32_t kD3derrSceneNotInScene = 0x887602f9u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t primitiveType = argument(cpu, 1);
	const uint32_t fvf = argument(cpu, 2);
	const uint32_t vertices = argument(cpu, 3);
	const uint32_t vertexCount = argument(cpu, 4);
	const uint32_t indices = argument(cpu, 5);
	const uint32_t indexCount = argument(cpu, 6);
	const uint32_t flags = argument(cpu, 7);
#if defined(__ANDROID__)
	const uint32_t guestCaller = guest::load<uint32_t>(cpu->r[ESP]);
#endif
	d3d7::FvfLayout fvfLayout{};
	const bool validFvf = d3d7::decodeFvf(fvf, fvfLayout);
	const uint32_t stride = validFvf ? fvfLayout.stride : 0u;
	const uint64_t vertexBytes = uint64_t(stride) * vertexCount;
	const uint64_t indexBytes = uint64_t(indexCount) * sizeof(uint16_t);
	const auto* vertexData = stride && vertexBytes <= 64ull * 1024ull * 1024ull
		? sharedGuestAddressSpace().host(vertices, static_cast<uint32_t>(vertexBytes)) : nullptr;
	const auto* indexData = indexCount && indexBytes <= 64ull * 1024ull * 1024ull
		? sharedGuestAddressSpace().host(indices, static_cast<uint32_t>(indexBytes)) : nullptr;
	if (!vertexData || !indexData || !vertexCount || !indexCount) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	if ((primitiveType == d3d7::kPrimitiveTriangleList && indexCount % 3u != 0u) ||
		((primitiveType == d3d7::kPrimitiveTriangleStrip || primitiveType == d3d7::kPrimitiveTriangleFan) && indexCount < 3u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	uint16_t maximumIndex = 0u;
	for (uint32_t i = 0; i < indexCount; ++i) {
		const uint16_t index = guest::load<uint16_t>(indices + i * 2u);
		maximumIndex = std::max(maximumIndex, index);
		if (index >= vertexCount) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	}
	thread_local std::vector<uint16_t> rasterIndicesScratch;
	std::vector<uint16_t>& rasterIndices = rasterIndicesScratch;
	rasterIndices.clear();
	if (primitiveType == d3d7::kPrimitiveTriangleList) {
		rasterIndices.resize(indexCount);
		for (uint32_t i = 0u; i < indexCount; ++i) rasterIndices[i] = guest::load<uint16_t>(indices + i * 2u);
	} else {
		if (indexCount > (1u << 20u)) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		rasterIndices.reserve(size_t(indexCount - 2u) * 3u);
		for (uint32_t i = 0u; i + 2u < indexCount; ++i) {
			const uint16_t a = guest::load<uint16_t>(indices + i * 2u);
			const uint16_t b = guest::load<uint16_t>(indices + (i + 1u) * 2u);
			const uint16_t c = guest::load<uint16_t>(indices + (i + 2u) * 2u);
			if (primitiveType == d3d7::kPrimitiveTriangleFan) {
				if (i == 0u) {
					rasterIndices.push_back(a); rasterIndices.push_back(b); rasterIndices.push_back(c);
				} else {
					rasterIndices.push_back(guest::load<uint16_t>(indices));
					rasterIndices.push_back(b); rasterIndices.push_back(c);
				}
			} else if (i & 1u) {
				rasterIndices.push_back(b); rasterIndices.push_back(a); rasterIndices.push_back(c);
			} else {
				rasterIndices.push_back(a); rasterIndices.push_back(b); rasterIndices.push_back(c);
			}
		}
	}
	const uint32_t rasterIndexCount = static_cast<uint32_t>(rasterIndices.size());
	GuestD3d7Device device{};
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		const auto found = gD3d7Devices.find(self);
		if (found == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		device = found->second;
	}
	if (!device.inScene) { cpu->r[EAX] = kD3derrSceneNotInScene; return; }
	const bool xyzNormalDiffuseSpecularTex1 = fvfLayout.position == d3d7::PositionKind::XYZ &&
		fvfLayout.normalOffset != UINT32_MAX && fvfLayout.diffuseOffset != UINT32_MAX &&
		fvfLayout.specularOffset != UINT32_MAX && fvfLayout.textureCount == 1u;
	const bool xyzDiffuseTex1 = fvfLayout.position == d3d7::PositionKind::XYZ &&
		fvfLayout.normalOffset == UINT32_MAX && fvfLayout.diffuseOffset != UINT32_MAX &&
		fvfLayout.textureCount == 1u;
	const bool xyzRhwDiffuseTex1 = fvfLayout.position == d3d7::PositionKind::XYZRHW &&
		fvfLayout.diffuseOffset != UINT32_MAX && fvfLayout.textureCount == 1u;
	const bool alphaTestSupported = d3d7::supportsAlphaTest(device.renderState[15u],
		device.renderState[24u], device.renderState[25u]);
	const bool transformedGeometry = fvfLayout.position == d3d7::PositionKind::XYZRHW;
	const uint32_t textureStageCount = d3d7::activeTextureStageCount(device.textureStageState);
	bool stageConfigurationSupported = textureStageCount <= device.textureStageState.size();
	std::array<bool, 8> textureStageUsesTexture{};
	for (uint32_t stage = 0u; stage < textureStageCount && stage < device.textureStageState.size(); ++stage) {
		const auto& ts = device.textureStageState[stage];
		const auto validOp = [](uint32_t op) { return op >= d3d7::kTextureOpSelectArg1 && op <= d3d7::kTextureOpModulate2x; };
		const auto validArg = [](uint32_t arg) { return arg == d3d7::kTextureArgDiffuse ||
			arg == d3d7::kTextureArgCurrent || arg == d3d7::kTextureArgTexture; };
		const auto validArguments = [&validOp, &validArg](uint32_t op, uint32_t arg1, uint32_t arg2, bool allowDisable) {
			if (allowDisable && op == d3d7::kTextureOpDisable) return true;
			if (!validOp(op)) return false;
			if (op == d3d7::kTextureOpSelectArg1) return validArg(arg1);
			if (op == d3d7::kTextureOpSelectArg2) return validArg(arg2);
			return validArg(arg1) && validArg(arg2);
		};
		if (!validArguments(ts[1u], ts[2u], ts[3u], false) ||
			!validArguments(ts[4u], ts[5u], ts[6u], true)) {
			stageConfigurationSupported = false;
			break;
		}
		const bool stageUsesTexture = d3d7::textureOpUses(ts[1u], ts[2u], ts[3u], d3d7::kTextureArgTexture) ||
			d3d7::textureOpUses(ts[4u], ts[5u], ts[6u], d3d7::kTextureArgTexture);
		textureStageUsesTexture[stage] = stageUsesTexture;
		const uint32_t coordIndex = ts[11u] & 0xffffu;
		if ((stageUsesTexture && ((ts[11u] & 0xffff0000u) != 0u ||
			coordIndex >= fvfLayout.textureCount || fvfLayout.textureDimensions[coordIndex] > 2u ||
			!((ts[13u] >= 1u && ts[13u] <= 3u) && (ts[14u] >= 1u && ts[14u] <= 3u)) ||
			!((ts[16u] == 1u || ts[16u] == 2u) && (ts[17u] == 1u || ts[17u] == 2u) &&
				(ts[18u] == 0u || ts[18u] == 1u)) ||
			!device.textures[stage]))) {
			stageConfigurationSupported = false;
			break;
		}
	}
	const uint32_t cullMode = device.renderState[22u];
	const bool renderStateSupported = alphaTestSupported &&
		(!device.renderState[27u] || d3d7::supportsBlendFactors(device.renderState[19u], device.renderState[20u])) &&
		(device.renderState[23u] >= 1u && device.renderState[23u] <= 8u) &&
		(cullMode >= 1u && cullMode <= 3u) &&
		d3d7::supportsFogConfiguration(device.renderState[28u], device.renderState[35u],
			device.renderState[140u], fvfLayout.specularOffset != UINT32_MAX) &&
		device.renderState[145u] <= 2u && device.renderState[146u] <= 2u &&
		device.renderState[147u] <= 2u && device.renderState[148u] <= 2u &&
		(!device.renderState[137u] || transformedGeometry || fvfLayout.normalOffset != UINT32_MAX);
	const uint32_t renderStateRejection =
		(!alphaTestSupported ? 1u : 0u) |
		(device.renderState[27u] && !d3d7::supportsBlendFactors(device.renderState[19u], device.renderState[20u]) ? 2u : 0u) |
		(!(device.renderState[23u] >= 1u && device.renderState[23u] <= 8u) ? 4u : 0u) |
		(!(cullMode >= 1u && cullMode <= 3u) ? 8u : 0u) |
		(!d3d7::supportsFogConfiguration(device.renderState[28u], device.renderState[35u],
			device.renderState[140u], fvfLayout.specularOffset != UINT32_MAX) ? 16u : 0u) |
		(device.renderState[145u] > 2u || device.renderState[146u] > 2u ||
			device.renderState[147u] > 2u || device.renderState[148u] > 2u ? 32u : 0u) |
		(device.renderState[137u] && !transformedGeometry && fvfLayout.normalOffset == UINT32_MAX ? 64u : 0u);
	const bool topologySupported = primitiveType == d3d7::kPrimitiveTriangleList ||
		primitiveType == d3d7::kPrimitiveTriangleStrip || primitiveType == d3d7::kPrimitiveTriangleFan;
	const bool vertexLayoutSupported = transformedGeometry || fvfLayout.position == d3d7::PositionKind::XYZ;
	if (!topologySupported || flags != 0u || vertexCount > 1u << 20u ||
		!vertexLayoutSupported || !stageConfigurationSupported || !renderStateSupported) {
		const uint32_t rejection = (!topologySupported ? 1u : 0u) |
			(flags != 0u ? 2u : 0u) | (!vertexLayoutSupported ? 4u : 0u) |
			(!stageConfigurationSupported ? 8u : 0u) | (!renderStateSupported ? 16u : 0u);
		// Android does not have the host runner's stderr trace file. Preserve the
		// same strict first-failure policy, but report the guest's actual draw
		// state to logcat so the next fixed-function path can be implemented from
		// observed values rather than accepting an unsupported draw.
#if defined(__ANDROID__)
		GuestDdrawSurface currentTarget{};
		const bool haveTarget = device.renderTarget && loadGuestSurface(device.renderTarget, currentTarget);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"D3D7 rejected indexed draw caller=%08x target=%08x (%ux%u %ubpp z=%08x) type=%u fvf=%08x vertices=%08x/%u indices=%08x/%u flags=%08x texture0=%08x",
			guest::load<uint32_t>(cpu->r[ESP]), device.renderTarget,
			haveTarget ? currentTarget.width : 0u, haveTarget ? currentTarget.height : 0u,
			haveTarget ? currentTarget.bpp : 0u, haveTarget ? currentTarget.zBuffer : 0u,
			primitiveType, fvf, vertices, vertexCount, indices, indexCount, flags, device.textures[0]);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"D3D7 rejection mask=%u (topology=1 flags=2 vertex-layout=4 stage-state=8 render-state=16) lighting=%u normal=%u RHW=%u",
			rejection, device.renderState[137u], fvfLayout.normalOffset != UINT32_MAX,
			transformedGeometry);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"D3D7 state rs[7,14,15,19,20,22,23,24,25,27,28,29,137,139]=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%08x stage0[1,2,3,4,5,6,16,17,18]=%u,%u,%u,%u,%u,%u,%u,%u,%u stage1[1]=%u",
			device.renderState[7], device.renderState[14], device.renderState[15],
			device.renderState[19], device.renderState[20], device.renderState[22],
			device.renderState[23], device.renderState[24], device.renderState[25],
			device.renderState[27], device.renderState[28], device.renderState[29],
			device.renderState[137], device.renderState[139],
			device.textureStageState[0][1], device.textureStageState[0][2],
			device.textureStageState[0][3], device.textureStageState[0][4],
			device.textureStageState[0][5], device.textureStageState[0][6],
			device.textureStageState[0][16], device.textureStageState[0][17],
			device.textureStageState[0][18], device.textureStageState[1][1]);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"D3D7 textures stage0=%08x stage1=%08x fvf-stride=%u",
			device.textures[0], device.textures[1], stride);
		GuestDdrawSurface rejectedTexture0{}, rejectedTexture1{};
	const bool haveTexture0 = device.textures[0] && loadGuestSurface(device.textures[0], rejectedTexture0);
	const bool haveTexture1 = device.textures[1] && loadGuestSurface(device.textures[1], rejectedTexture1);
		br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
			"D3D7 rejected draw fog(color,table,start,end,density,vertex)=%08x,%08x,%08x,%08x,%08x,%08x textures=%ux%u/%u,%ux%u/%u",
			device.renderState[34], device.renderState[35], device.renderState[36],
			device.renderState[37], device.renderState[38], device.renderState[140],
			haveTexture0 ? rejectedTexture0.width : 0u,
			haveTexture0 ? rejectedTexture0.height : 0u,
			haveTexture0 ? rejectedTexture0.bpp : 0u,
			haveTexture1 ? rejectedTexture1.width : 0u,
			haveTexture1 ? rejectedTexture1.height : 0u,
			haveTexture1 ? rejectedTexture1.bpp : 0u);
		for (uint32_t stage = 0u; stage < 2u; ++stage) {
			br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
				"D3D7 stage%u color(op,arg1,arg2)=%u,%u,%u alpha(op,arg1,arg2)=%u,%u,%u texcoord=%u address(U,V)=%u,%u filter(min,mag,mip)=%u,%u,%u",
				stage,
				device.textureStageState[stage][1], device.textureStageState[stage][2],
				device.textureStageState[stage][3], device.textureStageState[stage][4],
				device.textureStageState[stage][5], device.textureStageState[stage][6],
				device.textureStageState[stage][11], device.textureStageState[stage][13],
				device.textureStageState[stage][14], device.textureStageState[stage][17],
				device.textureStageState[stage][16], device.textureStageState[stage][18]);
		}
		if (fvf == 0x000002c2u) {
			for (uint32_t vertex = 0u; vertex < std::min(vertexCount, 3u); ++vertex) {
				const uint32_t base = vertices + vertex * stride;
				br::runtime::androidDiagnosticLogPrint(ANDROID_LOG_ERROR, "BattleRealmsNative",
					"D3D7 FVF2C2 vertex%u pos=%08x,%08x,%08x diffuse=%08x specular=%08x uv0=%08x,%08x uv1=%08x,%08x",
					vertex, guest::load<uint32_t>(base), guest::load<uint32_t>(base + 4u),
					guest::load<uint32_t>(base + 8u), guest::load<uint32_t>(base + 12u),
					guest::load<uint32_t>(base + 16u), guest::load<uint32_t>(base + 20u),
					guest::load<uint32_t>(base + 24u), guest::load<uint32_t>(base + 28u),
					guest::load<uint32_t>(base + 32u));
			}
		}
#endif
		if (std::getenv("BR_TRACE_DDRAW") || std::getenv("BR_TRACE_D3D7")) {
			std::fprintf(stderr, "[br-d3d7] rejection mask=%u topology=%u flags=%u vertex-layout=%u stage-state=%u render-state=%u render-state-mask=%u\n",
				rejection, topologySupported, flags != 0u, vertexLayoutSupported,
				stageConfigurationSupported, renderStateSupported, renderStateRejection);
			std::fprintf(stderr, "[br-d3d7] rejected state fog-enable=%u fog-table=%u fog-vertex=%u fog-color=%08x specular=%u material-sources(D,A,S,E)=%u,%u,%u,%u alpha=%u/%u/%u blend=%u/%u/%u z=%u/%u/%u cull=%u lighting=%u\n",
				device.renderState[28u], device.renderState[35u], device.renderState[140u],
				device.renderState[34u], device.renderState[29u], device.renderState[145u],
				device.renderState[146u], device.renderState[147u], device.renderState[148u],
				device.renderState[15u], device.renderState[24u], device.renderState[25u],
				device.renderState[27u], device.renderState[19u], device.renderState[20u],
				device.renderState[7u], device.renderState[23u], device.renderState[14u],
				cullMode, device.renderState[137u]);
			GuestDdrawSurface currentTarget{};
			const bool haveTarget = device.renderTarget && loadGuestSurface(device.renderTarget, currentTarget);
			std::fprintf(stderr, "[br-d3d7] unsupported indexed draw caller=%08x target=%08x (%ux%u %ubpp, pixels=%08x z=%08x) type=%u fvf=%08x stride=%u vertices=%08x/%u indices=%08x/%u flags=%08x\n",
				guest::load<uint32_t>(cpu->r[ESP]), device.renderTarget,
				haveTarget ? currentTarget.width : 0u, haveTarget ? currentTarget.height : 0u,
				haveTarget ? currentTarget.bpp : 0u, haveTarget ? currentTarget.pixels : 0u,
				haveTarget ? currentTarget.zBuffer : 0u, primitiveType, fvf, stride, vertices,
				vertexCount, indices, indexCount, flags);
			std::fprintf(stderr, "[br-d3d7] draw states lighting=%u alpha-test=%u alpha-ref=%u alpha-func=%u z-enable=%u z-func=%u z-write=%u blend=%u cull=%u stage0-colorop=%u alphaop=%u tex0=%08x viewport=%u,%u %ux%u z=%08x..%08x\n",
				device.renderState[137u], device.renderState[15u], device.renderState[24u],
				device.renderState[25u], device.renderState[7u], device.renderState[23u],
				device.renderState[14u], device.renderState[27u], device.renderState[22u],
				device.textureStageState[0][1], device.textureStageState[0][4], device.textures[0],
				device.viewport[0], device.viewport[1], device.viewport[2], device.viewport[3],
				device.viewport[4], device.viewport[5]);
			if (std::getenv("BR_TRACE_D3D7_DRAW_DETAIL")) {
				std::fprintf(stderr, "[br-d3d7] unsupported material ambient=%08x,%08x,%08x,%08x emissive=%08x,%08x,%08x,%08x\n",
					device.material[4], device.material[5], device.material[6], device.material[7],
					device.material[12], device.material[13], device.material[14], device.material[15]);
				const uint32_t diffuseOffset = xyzDiffuseTex1 ? 12u : (xyzRhwDiffuseTex1 ? 16u : 24u);
				const uint32_t specularOffset = xyzNormalDiffuseSpecularTex1 ? 28u : 0u;
				const uint32_t uvOffset = xyzDiffuseTex1 ? 16u : (xyzRhwDiffuseTex1 ? 20u : 32u);
				for (uint32_t i = 0; i < std::min(vertexCount, 4u); ++i) {
					const uint32_t address = vertices + i * stride;
					std::fprintf(stderr, "[br-d3d7] unsupported vertex[%u] pos=%08x,%08x,%08x diffuse=%08x specular=%08x uv=%08x,%08x\n",
						i, guest::load<uint32_t>(address), guest::load<uint32_t>(address + 4u),
						guest::load<uint32_t>(address + 8u), guest::load<uint32_t>(address + diffuseOffset),
						specularOffset ? guest::load<uint32_t>(address + specularOffset) : 0u,
						guest::load<uint32_t>(address + uvOffset), guest::load<uint32_t>(address + uvOffset + 4u));
				}
			}
		}
		throw std::runtime_error("D3D7 DrawIndexedPrimitive encountered a render state/format outside the captured fixed-function paths");
	}
	GuestDdrawSurface target{};
	GuestDdrawSurface depth{};
	std::array<GuestDdrawSurface, 8> textures{};
	if (!loadGuestSurface(device.renderTarget, target) || !target.width || !target.height ||
		(target.bpp != 16u && target.bpp != 32u) ||
		!loadGuestSurface(target.zBuffer, depth) || depth.width != target.width ||
		depth.height != target.height || (depth.bpp != 16u && depth.bpp != 32u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	for (uint32_t stage = 0u; stage < textureStageCount; ++stage) {
		if (!textureStageUsesTexture[stage]) continue;
		if (!loadGuestSurface(device.textures[stage], textures[stage]) || !textures[stage].width ||
			!textures[stage].height || (textures[stage].bpp != 16u && textures[stage].bpp != 32u)) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
	}
	const uint32_t pixelBytes = (target.bpp + 7u) / 8u;
	const auto* targetPixels = sharedGuestAddressSpace().host(target.pixels, target.bytes);
	auto* outputPixels = sharedGuestAddressSpace().host(target.pixels, target.bytes);
	auto* depthPixels = sharedGuestAddressSpace().host(depth.pixels, depth.bytes);
	std::array<const uint8_t*, 8> texturePixels{};
	if (!targetPixels || !outputPixels || !depthPixels || !pixelBytes) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	for (uint32_t stage = 0u; stage < textureStageCount; ++stage) {
		if (textureStageUsesTexture[stage]) {
			texturePixels[stage] = sharedGuestAddressSpace().host(textures[stage].pixels, textures[stage].bytes);
			if (!texturePixels[stage]) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		}
	}
#if defined(__ANDROID__)
	if (textureStageCount > 1u && !gLoggedD3d7MultiStageAccepted.exchange(true, std::memory_order_relaxed)) {
		br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
			"D3D7_TWO_STAGE_PATH_ACCEPTED", "caller=0x%08x target=%ux%u fvf=%08x texture0=%ux%u texture1=%ux%u fog_vertex=%u fog_color=%08x",
			guestCaller, target.width, target.height, fvf,
			textures[0].width, textures[0].height, textures[1].width,
			textures[1].height, device.renderState[140u], device.renderState[34u]);
	}
#endif

	struct Color { float r, g, b, a; };
	struct ClipVertex { float p[4]; std::array<std::array<float, 4>, 8> uv; Color color, specular; };
	struct ScreenVertex { float x, y, z, invW; std::array<std::array<float, 4>, 8> uv; Color color, specular; };
	d3d7::PixelFormatLayout targetPixelFormat{};
	std::array<d3d7::PixelFormatLayout, 8> texturePixelFormats{};
	if (!d3d7::makePixelFormatLayout(target.bpp, target.redMask, target.greenMask,
		target.blueMask, target.alphaMask, targetPixelFormat)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	for (uint32_t stage = 0u; stage < textureStageCount; ++stage) {
		if (textureStageUsesTexture[stage] && !d3d7::makePixelFormatLayout(textures[stage].bpp,
			textures[stage].redMask, textures[stage].greenMask, textures[stage].blueMask,
			textures[stage].alphaMask, texturePixelFormats[stage])) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
	}
	const d3d7::BlendColor* targetDecodeTable = pixelDecodeLut(targetPixelFormat);
	std::array<const d3d7::BlendColor*, 8> textureDecodeTables{};
	for (uint32_t stage = 0u; stage < textureStageCount; ++stage)
		if (textureStageUsesTexture[stage])
			textureDecodeTables[stage] = pixelDecodeLut(texturePixelFormats[stage]);
	const bool tracePixelProbe = std::getenv("BR_TRACE_D3D7_PIXEL_PROBE") != nullptr;
	static std::atomic<uint32_t> nextPixelProbeDraw{0u};
	const uint32_t pixelProbeDraw = tracePixelProbe
		? nextPixelProbeDraw.fetch_add(1u, std::memory_order_relaxed) : UINT32_MAX;
	const bool traceThisPixelProbeDraw = tracePixelProbe && pixelProbeDraw < 3u;
	if (traceThisPixelProbeDraw) {
		const auto logSurfacePixels = [pixelProbeDraw](const char* label,
			const GuestDdrawSurface& surface, const uint8_t* pixels) {
			uint32_t hash = 2166136261u;
			uint32_t nonzeroBytes = 0u;
			for (uint32_t i = 0u; i < surface.bytes; ++i) {
				hash = (hash ^ pixels[i]) * 16777619u;
				nonzeroBytes += pixels[i] != 0u ? 1u : 0u;
			}
			std::fprintf(stderr, "[br-d3d7-pixel] draw=%u surface=%s handle=%08x pixels=%08x size=%ux%u bpp=%u pitch=%u masks=%08x/%08x/%08x/%08x bytes=%u nonzero=%u fnv=%08x first=%02x%02x%02x%02x\n",
				pixelProbeDraw, label, surface.handle, surface.pixels, surface.width, surface.height,
				surface.bpp, surface.pitch, surface.redMask, surface.greenMask, surface.blueMask,
				surface.alphaMask, surface.bytes, nonzeroBytes, hash,
				surface.bytes > 0u ? pixels[0] : 0u, surface.bytes > 1u ? pixels[1] : 0u,
				surface.bytes > 2u ? pixels[2] : 0u, surface.bytes > 3u ? pixels[3] : 0u);
		};
		logSurfacePixels("target-before", target, targetPixels);
		for (uint32_t stage = 0u; stage < textureStageCount; ++stage) {
			if (textureStageUsesTexture[stage])
				logSurfacePixels(stage == 0u ? "texture0" : "textureN", textures[stage], texturePixels[stage]);
		}
		std::fprintf(stderr, "[br-d3d7-pixel] draw=%u fvf=%08x stride=%u stages=%u lighting=%u rs-alpha-test=%u rs-alpha-blend=%u rs-z=%u/%u stage0=%u,%u,%u,%u,%u,%u texture0=%08x target-format-bytes=%u masks=%08x/%08x/%08x/%08x\n",
			pixelProbeDraw, fvf, stride, textureStageCount, device.renderState[137u],
			device.renderState[15u], device.renderState[27u], device.renderState[7u],
			device.renderState[23u], device.textureStageState[0][1],
			device.textureStageState[0][2], device.textureStageState[0][3],
			device.textureStageState[0][4], device.textureStageState[0][5],
			device.textureStageState[0][6], device.textures[0], targetPixelFormat.bytesPerPixel,
			targetPixelFormat.red.mask, targetPixelFormat.green.mask,
			targetPixelFormat.blue.mask, targetPixelFormat.alpha.mask);
	}
	const auto floatFromBits = [](uint32_t bits) { float value; std::memcpy(&value, &bits, sizeof(value)); return value; };
	const auto clamp01 = [](float value) { return std::max(0.0f, std::min(1.0f, value)); };
	const auto matrixPoint = [&device, &floatFromBits](const float input[4], uint32_t state) {
		float output[4]{};
		for (uint32_t column = 0; column < 4u; ++column)
			for (uint32_t row = 0; row < 4u; ++row)
				output[column] += input[row] * floatFromBits(device.transforms[state][row * 4u + column]);
		return std::array<float, 4>{{output[0], output[1], output[2], output[3]}};
	};
	std::array<float, 9> worldViewLinear{};
	for (uint32_t row = 0u; row < 3u; ++row) {
		for (uint32_t column = 0u; column < 3u; ++column) {
			for (uint32_t inner = 0u; inner < 3u; ++inner)
				worldViewLinear[row * 3u + column] +=
					floatFromBits(device.transforms[1u][row * 4u + inner]) *
					floatFromBits(device.transforms[2u][inner * 4u + column]);
		}
	}
	const auto readColor = [](const d3d7::PixelFormatLayout& format, const uint8_t* base,
		uint32_t pitch, uint32_t x, uint32_t y, const d3d7::BlendColor* decodeTable) {
		const uint8_t* address = base + size_t(y) * pitch + size_t(x) * format.bytesPerPixel;
		const uint32_t pixel = d3d7::loadPixel(address, format);
		if (decodeTable) {
			const auto& rgba = decodeTable[pixel & 0xffffu];
			return Color{rgba.r, rgba.g, rgba.b, rgba.a};
		}
		const auto rgba = d3d7::decodePixel(pixel, format);
		return Color{rgba[0], rgba[1], rgba[2], rgba[3]};
	};
	const auto writeColor = [](const d3d7::PixelFormatLayout& format, uint8_t* base,
		uint32_t pitch, uint32_t x, uint32_t y, const Color& color) {
		uint8_t* address = base + size_t(y) * pitch + size_t(x) * format.bytesPerPixel;
		d3d7::storePixel(address, format, d3d7::encodePixel(
			{{color.r, color.g, color.b, color.a}}, format));
	};
	const auto unpackD3dColor = [](uint32_t packed) {
		return Color{float((packed >> 16u) & 0xffu) / 255.0f,
			float((packed >> 8u) & 0xffu) / 255.0f, float(packed & 0xffu) / 255.0f,
			float((packed >> 24u) & 0xffu) / 255.0f};
	};
	const Color globalAmbient = unpackD3dColor(device.renderState[139u]);
	const auto materialColor = [&floatFromBits, &device](uint32_t offset) {
		return d3d7::BlendColor{floatFromBits(device.material[offset]),
			floatFromBits(device.material[offset + 1u]), floatFromBits(device.material[offset + 2u]),
			floatFromBits(device.material[offset + 3u])};
	};
	const d3d7::FixedFunctionMaterial fixedMaterial{
		materialColor(0u), materialColor(4u), materialColor(8u), materialColor(12u),
		floatFromBits(device.material[16u])};
	std::array<d3d7::FixedFunctionLight, 8> fixedLights{};
	std::array<bool, 8> fixedLightsEnabled{};
	for (uint32_t index = 0u; index < device.lights.size(); ++index) {
		const GuestD3d7Light& guestLight = device.lights[index];
		if (!guestLight.configured) continue;
		const auto color = [&floatFromBits, &guestLight](uint32_t offset) {
			return d3d7::BlendColor{floatFromBits(guestLight.values[offset]),
				floatFromBits(guestLight.values[offset + 1u]), floatFromBits(guestLight.values[offset + 2u]),
				floatFromBits(guestLight.values[offset + 3u])};
		};
		d3d7::FixedFunctionLight& light = fixedLights[index];
		light.type = guestLight.values[0];
		light.diffuse = color(1u); light.specular = color(5u); light.ambient = color(9u);
		float lightPosition[4] = {floatFromBits(guestLight.values[13]),
			floatFromBits(guestLight.values[14]), floatFromBits(guestLight.values[15]), 1.0f};
		float lightDirection[4] = {floatFromBits(guestLight.values[16]),
			floatFromBits(guestLight.values[17]), floatFromBits(guestLight.values[18]), 0.0f};
		const auto positionView = matrixPoint(lightPosition, 2u);
		const auto directionView = matrixPoint(lightDirection, 2u);
		light.position = {{positionView[0], positionView[1], positionView[2]}};
		light.direction = {{directionView[0], directionView[1], directionView[2]}};
		light.range = floatFromBits(guestLight.values[19]);
		light.falloff = floatFromBits(guestLight.values[20]);
		light.attenuation0 = floatFromBits(guestLight.values[21]);
		light.attenuation1 = floatFromBits(guestLight.values[22]);
		light.attenuation2 = floatFromBits(guestLight.values[23]);
		light.theta = floatFromBits(guestLight.values[24]);
		light.phi = floatFromBits(guestLight.values[25]);
		fixedLightsEnabled[index] = guestLight.enabled;
	}
	uint32_t pixelProbeVertexCount = 0u;
	const auto processVertex = [&](uint32_t index) {
		d3d7::DecodedVertex decoded{};
		if (!d3d7::decodeVertex(vertexData + size_t(index) * stride, fvfLayout, decoded))
			throw std::runtime_error("D3D7 vertex did not match the validated FVF layout");
		const bool transformed = transformedGeometry;
		const Color vertexColor = unpackD3dColor(decoded.diffuse);
		Color specular = fvfLayout.specularOffset != UINT32_MAX
			? unpackD3dColor(decoded.specular) : Color{0.0f, 0.0f, 0.0f, 1.0f};
		std::array<std::array<float, 4>, 8> uv = decoded.texture;
		Color color = vertexColor;
		if (transformed) {
			return ClipVertex{{decoded.position[0], decoded.position[1], decoded.position[2], decoded.position[3]}, uv, color, specular};
		}
		float position[4] = {decoded.position[0], decoded.position[1], decoded.position[2], 1.0f};
		const auto world = matrixPoint(position, 1u);
		const auto view = matrixPoint(world.data(), 2u);
		const auto clip = matrixPoint(view.data(), 3u);
		if (device.renderState[137u] != 0u) {
			std::array<float, 3> viewNormal{};
			if (!d3d7::transformNormalInverseTranspose(worldViewLinear, decoded.normal, viewNormal))
				throw std::runtime_error("D3D7 lighting encountered a singular world-view normal matrix");
			const auto lit = d3d7::illuminateVertexD3d7(
				{{view[0], view[1], view[2]}}, viewNormal,
				fixedMaterial, {globalAmbient.r, globalAmbient.g, globalAmbient.b, globalAmbient.a},
				{vertexColor.r, vertexColor.g, vertexColor.b, vertexColor.a},
				{specular.r, specular.g, specular.b, specular.a}, fixedLights.data(),
				fixedLightsEnabled.data(), fixedLights.size(),
				(device.renderState[141u] != 0u), device.renderState);
			if (traceThisPixelProbeDraw && pixelProbeVertexCount < 4u) {
				uint32_t enabledLights = 0u;
				for (bool enabled : fixedLightsEnabled) enabledLights += enabled ? 1u : 0u;
				std::fprintf(stderr,
					"[br-d3d7-pixel] draw=%u vertex=%u fvf-index=%u raw-diffuse=%08x raw-specular=%08x normal=%.5g,%.5g,%.5g material-diffuse=%.4g,%.4g,%.4g,%.4g material-ambient=%.4g,%.4g,%.4g,%.4g material-emissive=%.4g,%.4g,%.4g global-ambient=%.4g,%.4g,%.4g sources=%u,%u,%u,%u colorvertex=%u lights=%u lit=%.5g,%.5g,%.5g,%.5g\n",
					pixelProbeDraw, pixelProbeVertexCount, index, decoded.diffuse, decoded.specular,
					decoded.normal[0], decoded.normal[1], decoded.normal[2],
					fixedMaterial.diffuse.r, fixedMaterial.diffuse.g, fixedMaterial.diffuse.b, fixedMaterial.diffuse.a,
					fixedMaterial.ambient.r, fixedMaterial.ambient.g, fixedMaterial.ambient.b, fixedMaterial.ambient.a,
					fixedMaterial.emissive.r, fixedMaterial.emissive.g, fixedMaterial.emissive.b,
					globalAmbient.r, globalAmbient.g, globalAmbient.b,
					device.renderState[145u], device.renderState[146u], device.renderState[147u], device.renderState[148u],
					device.renderState[141u], enabledLights,
					lit.diffuse.r, lit.diffuse.g, lit.diffuse.b, lit.diffuse.a);
				++pixelProbeVertexCount;
			}
			color = {lit.diffuse.r, lit.diffuse.g, lit.diffuse.b, lit.diffuse.a};
			specular = {lit.specular.r, lit.specular.g, lit.specular.b, lit.specular.a};
		}
		return ClipVertex{{clip[0], clip[1], clip[2], clip[3]}, uv, color, specular};
	};
	// Indexed meshes repeat vertices across triangles. Transforming and lighting
	// the same immutable guest vertex for every index reference is expensive on
	// Android's CPU raster path. Cache only moderate meshes with enough index
	// reuse to amortize the bounded per-draw storage; large or low-reuse draws
	// retain the allocation-free original path.
	constexpr uint32_t kMaxCachedVertices = 1024u;
	const bool cacheTransformedVertices = vertexCount <= kMaxCachedVertices &&
		rasterIndexCount >= vertexCount * 2u && rasterIndexCount >= 192u;
	thread_local std::vector<ClipVertex> transformedVertexCacheScratch;
	thread_local std::vector<uint8_t> transformedVertexCacheValidScratch;
	std::vector<ClipVertex>& transformedVertexCache = transformedVertexCacheScratch;
	std::vector<uint8_t>& transformedVertexCacheValid = transformedVertexCacheValidScratch;
	transformedVertexCache.clear();
	transformedVertexCacheValid.clear();
	if (cacheTransformedVertices) {
		transformedVertexCache.resize(vertexCount);
		transformedVertexCacheValid.assign(vertexCount, 0u);
	}
	uint64_t transformedVertexCacheHits = 0u;
	const auto getVertex = [&](uint32_t index) {
		if (!cacheTransformedVertices) return processVertex(index);
		if (transformedVertexCacheValid[index]) {
			++transformedVertexCacheHits;
			return transformedVertexCache[index];
		}
		ClipVertex value = processVertex(index);
		transformedVertexCache[index] = value;
		transformedVertexCacheValid[index] = 1u;
		return value;
	};
	const auto clipDistance = [](const ClipVertex& vertex, uint32_t plane) {
		switch (plane) {
		case 0: return vertex.p[0] + vertex.p[3];
		case 1: return vertex.p[3] - vertex.p[0];
		case 2: return vertex.p[1] + vertex.p[3];
		case 3: return vertex.p[3] - vertex.p[1];
		case 4: return vertex.p[2];
		default: return vertex.p[3] - vertex.p[2];
		}
	};
	const auto interpolate = [](const ClipVertex& a, const ClipVertex& b, float t) {
		ClipVertex result{};
		for (uint32_t i = 0; i < 4u; ++i) result.p[i] = a.p[i] + (b.p[i] - a.p[i]) * t;
		for (uint32_t set = 0u; set < 8u; ++set)
			for (uint32_t component = 0u; component < 4u; ++component)
				result.uv[set][component] = a.uv[set][component] + (b.uv[set][component] - a.uv[set][component]) * t;
		result.color = {a.color.r + (b.color.r - a.color.r) * t,
			a.color.g + (b.color.g - a.color.g) * t, a.color.b + (b.color.b - a.color.b) * t,
			a.color.a + (b.color.a - a.color.a) * t};
		result.specular = {a.specular.r + (b.specular.r - a.specular.r) * t,
			a.specular.g + (b.specular.g - a.specular.g) * t,
			a.specular.b + (b.specular.b - a.specular.b) * t,
			a.specular.a + (b.specular.a - a.specular.a) * t};
		return result;
	};
	thread_local std::vector<ClipVertex> clippedScratch;
	std::vector<ClipVertex>& clipped = clippedScratch;
	clipped.clear();
	// Reuse clipping scratch storage across input triangles. A temporary vector
	// allocated for each plane/triangle caused avoidable allocator churn in the
	// software renderer's hottest call path.
	thread_local std::vector<ClipVertex> clipScratchStorage;
	std::vector<ClipVertex>& clipScratch = clipScratchStorage;
	clipScratch.clear();
	clipScratch.reserve(12u);
	thread_local std::vector<d3d7::GpuVertex> gpuVerticesScratch;
	std::vector<d3d7::GpuVertex>& gpuVertices = gpuVerticesScratch;
	gpuVertices.clear();
#if defined(__ANDROID__) && defined(BR_ANDROID_D3D7_GLES)
	const bool tryGpuRenderer = true;
	gpuVertices.reserve(std::min<uint32_t>(rasterIndexCount * 2u, 1u << 20u));
#else
	const bool tryGpuRenderer = false;
#endif
	thread_local std::vector<ScreenVertex> screenScratch;
	std::vector<ScreenVertex>& screen = screenScratch;
	screen.clear();
	uint64_t testedFragments = 0u;
	uint64_t passedDepth = 0u;
	uint64_t changedPixels = 0u;
	uint64_t rasterBoundsCandidates = 0u;
	uint64_t scanlineSpanPixels = 0u;
	uint64_t skippedBoundsCandidates = 0u;
	uint64_t scanlineFallbackRows = 0u;
	uint32_t flatColorTriangles = 0u;
	uint32_t clipRejectedTriangles = 0u;
	uint32_t projectionRejectedPolygons = 0u;
	uint32_t rasterizedTriangles = 0u;
	uint32_t degenerateTriangles = 0u;
	uint32_t culledTriangles = 0u;
	uint32_t offscreenTriangles = 0u;
	uint32_t coveredTriangles = 0u;
	const uint32_t depthBytes = (depth.bpp + 7u) / 8u;
	const bool depthEnabled = device.renderState[7u] != 0u;
	const bool depthWriteEnabled = device.renderState[14u] != 0u;
	const bool alphaBlendEnabled = device.renderState[27u] != 0u;
	const auto alphaTestPasses = [&device](float alpha) {
		const float reference = float(device.renderState[24u] & 0xffu) / 255.0f;
		const uint32_t function = device.renderState[25u];
		return function == 1u ? false : function == 2u ? alpha < reference :
			function == 3u ? alpha == reference : function == 4u ? alpha <= reference :
			function == 5u ? alpha > reference : function == 6u ? alpha != reference :
			function == 7u ? alpha >= reference : function == 8u;
	};
	const uint32_t viewportX = device.viewport[0];
	const uint32_t viewportY = device.viewport[1];
	const uint32_t viewportWidth = device.viewport[2];
	const uint32_t viewportHeight = device.viewport[3];
	const float viewportMinZ = floatFromBits(device.viewport[4]);
	const float viewportMaxZ = floatFromBits(device.viewport[5]);
	if (!viewportWidth || !viewportHeight || !depthBytes) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	const auto edge = [](const ScreenVertex& a, const ScreenVertex& b, float x, float y) {
		return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
	};
	const auto topLeft = [](const ScreenVertex& a, const ScreenVertex& b) {
		const float dx = b.x - a.x, dy = b.y - a.y;
		return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
	};
	const bool interpolateSpecular = device.renderState[29u] != 0u ||
		(device.renderState[28u] != 0u && device.renderState[140u] != 0u);
	const bool traceRaster = std::getenv("BR_TRACE_DDRAW") || std::getenv("BR_TRACE_D3D7");
	// Keep an exact edge-test reference mode for host-side pixel comparisons.
	const bool useScanlineSpans = std::getenv("BR_D3D7_REFERENCE_SCAN") == nullptr;
	const bool traceRasterProgress = std::getenv("BR_TRACE_D3D7_RASTER_PROGRESS") != nullptr;
	uint32_t rasterTriangleOrdinal = 0u;
	uint32_t pixelProbeSampleCount = 0u;
	const auto addressCoord = [](float value, uint32_t addressMode) {
		if (addressMode == 1u) {
			if (value >= 0.0f && value < 1.0f) return value;
			value -= std::floor(value);
			return value;
		}
		if (addressMode == 2u) {
			if (value >= 0.0f && value <= 1.0f) return value;
			value = std::fmod(std::fabs(value), 2.0f);
			return value <= 1.0f ? value : 2.0f - value;
		}
		if (addressMode == 3u) {
			if (value >= 0.0f && value <= 1.0f) return value;
			return std::max(0.0f, std::min(1.0f, value));
		}
		return std::numeric_limits<float>::quiet_NaN();
	};
	const auto sampleTexture = [&](const GuestDdrawSurface& sampleSurface,
		const d3d7::PixelFormatLayout& sampleFormat,
		const uint8_t* samplePixels, const d3d7::BlendColor* decodeTable,
		float u, float v, uint32_t filter,
		uint32_t addressU, uint32_t addressV) {
		u = addressCoord(u, addressU); v = addressCoord(v, addressV);
		if (!std::isfinite(u) || !std::isfinite(v)) return Color{0.0f, 0.0f, 0.0f, 0.0f};
		if (filter == 1u) {
			const uint32_t x = std::min<uint32_t>(static_cast<uint32_t>(u * float(sampleSurface.width)), sampleSurface.width - 1u);
			const uint32_t y = std::min<uint32_t>(static_cast<uint32_t>(v * float(sampleSurface.height)), sampleSurface.height - 1u);
			return readColor(sampleFormat, samplePixels, sampleSurface.pitch, x, y, decodeTable);
		}
		const float fx = std::max(0.0f, std::min(float(sampleSurface.width - 1u), u * float(sampleSurface.width) - 0.5f));
		const float fy = std::max(0.0f, std::min(float(sampleSurface.height - 1u), v * float(sampleSurface.height) - 0.5f));
		const uint32_t x0 = static_cast<uint32_t>(std::floor(fx));
		const uint32_t y0 = static_cast<uint32_t>(std::floor(fy));
		const uint32_t x1 = std::min(x0 + 1u, sampleSurface.width - 1u);
		const uint32_t y1 = std::min(y0 + 1u, sampleSurface.height - 1u);
		const float tx = fx - float(x0), ty = fy - float(y0);
		const Color c00 = readColor(sampleFormat, samplePixels, sampleSurface.pitch, x0, y0, decodeTable);
		const Color c10 = readColor(sampleFormat, samplePixels, sampleSurface.pitch, x1, y0, decodeTable);
		const Color c01 = readColor(sampleFormat, samplePixels, sampleSurface.pitch, x0, y1, decodeTable);
		const Color c11 = readColor(sampleFormat, samplePixels, sampleSurface.pitch, x1, y1, decodeTable);
		const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
		return Color{lerp(lerp(c00.r, c10.r, tx), lerp(c01.r, c11.r, tx), ty),
			lerp(lerp(c00.g, c10.g, tx), lerp(c01.g, c11.g, tx), ty),
			lerp(lerp(c00.b, c10.b, tx), lerp(c01.b, c11.b, tx), ty),
			lerp(lerp(c00.a, c10.a, tx), lerp(c01.a, c11.a, tx), ty)};
	};
	const auto rasterTriangle = [&](ScreenVertex a, ScreenVertex b, ScreenVertex c) {
		++rasterizedTriangles;
		const uint64_t testedBeforeTriangle = testedFragments;
		float area = edge(a, b, c.x, c.y);
		if (std::fabs(area) < 1.0e-8f) { ++degenerateTriangles; return; }
		if ((cullMode == 2u && area > 0.0f) || (cullMode == 3u && area < 0.0f)) {
			++culledTriangles;
			return;
		}
		if (area < 0.0f) { std::swap(b, c); area = -area; }
		const float inverseArea = 1.0f / area;
		std::array<uint32_t, 8> textureFilters{};
		for (uint32_t stage = 0u; stage < textureStageCount; ++stage) {
			const auto& stageState = device.textureStageState[stage];
			textureFilters[stage] = stageState[17u]; // D3DTSS_MINFILTER.
			if (!textureStageUsesTexture[stage]) continue;
			const uint32_t coordinate = stageState[11u] & 0xffffu;
			const auto footprintVertex = [coordinate](const ScreenVertex& vertex) {
				return d3d7::TextureFootprintVertex{vertex.x, vertex.y, vertex.invW,
					vertex.uv[coordinate][0], vertex.uv[coordinate][1]};
			};
			const std::array<d3d7::TextureFootprintVertex, 3> footprint{{
				footprintVertex(a), footprintVertex(b), footprintVertex(c)}};
			if (!d3d7::textureFootprintIsMinified(footprint,
				textures[stage].width, textures[stage].height))
				textureFilters[stage] = stageState[16u]; // D3DTSS_MAGFILTER.
		}
		const bool edge0TopLeft = topLeft(b, c);
		const bool edge1TopLeft = topLeft(c, a);
		const bool edge2TopLeft = topLeft(a, b);
		const float minX = std::min({a.x, b.x, c.x});
		const float maxX = std::max({a.x, b.x, c.x});
		const float minY = std::min({a.y, b.y, c.y});
		const float maxY = std::max({a.y, b.y, c.y});
		const int32_t x0 = std::max<int32_t>(0, static_cast<int32_t>(std::ceil(minX - 0.5f)));
		const int32_t x1 = std::min<int32_t>(static_cast<int32_t>(target.width) - 1,
			static_cast<int32_t>(std::floor(maxX - 0.5f)));
		const int32_t y0 = std::max<int32_t>(0, static_cast<int32_t>(std::ceil(minY - 0.5f)));
		const int32_t y1 = std::min<int32_t>(static_cast<int32_t>(target.height) - 1,
			static_cast<int32_t>(std::floor(maxY - 0.5f)));
		if (x1 < x0 || y1 < y0) { ++offscreenTriangles; return; }
		const float startSx = float(x0) + 0.5f, startSy = float(y0) + 0.5f;
		float rowE0 = edge(b, c, startSx, startSy);
		float rowE1 = edge(c, a, startSx, startSy);
		float rowE2 = edge(a, b, startSx, startSy);
		const float edge0StepX = b.y - c.y, edge1StepX = c.y - a.y, edge2StepX = a.y - b.y;
		const float edge0StepY = c.x - b.x, edge1StepY = a.x - c.x, edge2StepY = b.x - a.x;
		// The barycentric weights are the edge functions scaled by inverseArea.
		// Their x increments are constant across the span, so calculate those
		// three products once per triangle instead of multiplying every pixel.
		const float weight0StepX = edge0StepX * inverseArea;
		const float weight1StepX = edge1StepX * inverseArea;
		const float weight2StepX = edge2StepX * inverseArea;
		const bool flatColor = textureStageCount == 0u && device.renderState[28u] == 0u &&
			device.renderState[29u] == 0u &&
			(a.color.r == 0.0f || a.color.r == 1.0f) &&
			(a.color.g == 0.0f || a.color.g == 1.0f) &&
			(a.color.b == 0.0f || a.color.b == 1.0f) &&
			(a.color.a == 0.0f || a.color.a == 1.0f) &&
			(a.color.r == b.color.r && b.color.r == c.color.r) &&
			(a.color.g == b.color.g && b.color.g == c.color.g) &&
			(a.color.b == b.color.b && b.color.b == c.color.b) &&
			(a.color.a == b.color.a && b.color.a == c.color.a);
		const uint32_t flatColorPixel = flatColor && !alphaBlendEnabled
			? d3d7::encodePixel({{a.color.r, a.color.g, a.color.b, a.color.a}}, targetPixelFormat) : 0u;
		if (flatColor) ++flatColorTriangles;
		++rasterTriangleOrdinal;
		if (traceRasterProgress &&
			(rasterTriangleOrdinal <= 4u || (rasterTriangleOrdinal & 31u) == 0u))
			std::fprintf(stderr, "[br-d3d7-progress] triangle=%u bounds=%d,%d..%d,%d candidates=%llu\n",
				rasterTriangleOrdinal, x0, y0, x1, y1,
				static_cast<unsigned long long>(x1 >= x0 && y1 >= y0
					? uint64_t(x1 - x0 + 1) * uint64_t(y1 - y0 + 1) : 0u));
		for (int32_t py = y0; py <= y1; ++py) {
			if (x1 >= x0)
				rasterBoundsCandidates += static_cast<uint64_t>(x1 - x0 + 1);
			if (traceRasterProgress && py > y0 && ((py - y0) & 127) == 0)
				std::fprintf(stderr, "[br-d3d7-progress] triangle=%u row=%d tested=%llu passed=%llu\n",
					rasterTriangleOrdinal, py,
					static_cast<unsigned long long>(testedFragments),
					static_cast<unsigned long long>(passedDepth));
			d3d7::ScanlineSpan span{};
			if (useScanlineSpans) {
				span = d3d7::computeTriangleScanlineSpan(x0, x1, {{
					{rowE0, edge0StepX, edge0TopLeft},
					{rowE1, edge1StepX, edge1TopLeft},
					{rowE2, edge2StepX, edge2TopLeft}}});
				if (span.status == d3d7::ScanlineSpanStatus::Empty) {
					if (x1 >= x0)
						skippedBoundsCandidates += static_cast<uint64_t>(x1 - x0 + 1);
					// Empty rows still advance the triangle's edge equations. Without
					// this, a triangle starting above its first covered row stays
					// permanently rejected and loses all of its later pixels.
					d3d7::advanceTriangleScanlineEdges(rowE0, rowE1, rowE2,
						edge0StepY, edge1StepY, edge2StepY);
					continue;
				}
				if (span.status == d3d7::ScanlineSpanStatus::Validated) {
					const uint64_t spanWidth = static_cast<uint64_t>(span.lastPixel - span.firstPixel + 1);
					scanlineSpanPixels += spanWidth;
					skippedBoundsCandidates += static_cast<uint64_t>(x1 - x0 + 1) - spanWidth;
				} else {
					++scanlineFallbackRows;
				}
			}
			const int32_t spanX0 = useScanlineSpans && span.status == d3d7::ScanlineSpanStatus::Validated
				? span.firstPixel : x0;
			const int32_t spanX1 = useScanlineSpans && span.status == d3d7::ScanlineSpanStatus::Validated
				? span.lastPixel : x1;
			const bool checkPixelEdges = !useScanlineSpans ||
				span.status == d3d7::ScanlineSpanStatus::Fallback;
			const float firstOffset = float(spanX0 - x0);
			float e0 = rowE0 + edge0StepX * firstOffset;
		float e1 = rowE1 + edge1StepX * firstOffset;
		float e2 = rowE2 + edge2StepX * firstOffset;
		float w0 = e0 * inverseArea, w1 = e1 * inverseArea, w2 = e2 * inverseArea;
			for (int32_t px = spanX0; px <= spanX1; ++px,
				e0 += edge0StepX, e1 += edge1StepX, e2 += edge2StepX,
				w0 += weight0StepX, w1 += weight1StepX, w2 += weight2StepX) {
			if (checkPixelEdges && (e0 < 0.0f || (e0 == 0.0f && !edge0TopLeft) ||
				e1 < 0.0f || (e1 == 0.0f && !edge1TopLeft) ||
				e2 < 0.0f || (e2 == 0.0f && !edge2TopLeft))) continue;
			++testedFragments;
			const float z = clamp01(w0 * a.z + w1 * b.z + w2 * c.z);
			const size_t depthOffset = size_t(py) * depth.pitch + size_t(px) * depthBytes;
			if (depthEnabled) {
				uint32_t oldDepth = 0u;
				for (uint32_t i = 0; i < depthBytes; ++i)
					oldDepth |= uint32_t(depthPixels[depthOffset + i]) << (i * 8u);
				const float previousDepth = depth.bpp == 16u ? float(oldDepth & 0xffffu) / 65535.0f
					: float(oldDepth) / 4294967295.0f;
				const uint32_t depthFunction = device.renderState[23u];
				const bool depthPass = depthFunction == 1u ? false : depthFunction == 2u ? z < previousDepth :
					depthFunction == 3u ? z == previousDepth : depthFunction == 4u ? z <= previousDepth :
					depthFunction == 5u ? z > previousDepth : depthFunction == 6u ? z != previousDepth :
					depthFunction == 7u ? z >= previousDepth : depthFunction == 8u;
				if (!depthPass) continue;
			}
			++passedDepth;
			if (flatColor) {
				if (device.renderState[15u] && !alphaTestPasses(a.color.a)) continue;
				if (alphaBlendEnabled) {
					const Color destination = readColor(targetPixelFormat, targetPixels, target.pitch,
						static_cast<uint32_t>(px), static_cast<uint32_t>(py), targetDecodeTable);
					const d3d7::BlendColor blended = d3d7::blend(device.renderState[19u],
						device.renderState[20u], {a.color.r, a.color.g, a.color.b, a.color.a},
						{destination.r, destination.g, destination.b, destination.a});
					writeColor(targetPixelFormat, outputPixels, target.pitch, static_cast<uint32_t>(px),
						static_cast<uint32_t>(py), {blended.r, blended.g, blended.b, blended.a});
				} else {
					uint8_t* address = outputPixels + size_t(py) * target.pitch +
						size_t(px) * targetPixelFormat.bytesPerPixel;
					d3d7::storePixel(address, targetPixelFormat, flatColorPixel);
				}
				const uint32_t depthValue = depth.bpp == 16u
					? static_cast<uint32_t>(z * 65535.0f + 0.5f)
					: static_cast<uint32_t>(z * 4294967295.0f);
				if (depthWriteEnabled)
					for (uint32_t i = 0; i < depthBytes; ++i)
						depthPixels[depthOffset + i] = static_cast<uint8_t>(depthValue >> (i * 8u));
				++changedPixels;
				continue;
			}
			const float reciprocalW = w0 * a.invW + w1 * b.invW + w2 * c.invW;
			if (!(reciprocalW > 0.0f)) continue;
			const float inverseReciprocalW = 1.0f / reciprocalW;
		std::array<d3d7::BlendColor, 8> stageTextureColors{};
		for (uint32_t stage = 0u; stage < textureStageCount; ++stage) {
			stageTextureColors[stage] = {1.0f, 1.0f, 1.0f, 1.0f};
			if (!textureStageUsesTexture[stage]) continue;
			const uint32_t coord = device.textureStageState[stage][11u] & 0xffffu;
			std::array<float, 4> uv{};
			for (uint32_t component = 0u; component < 2u; ++component)
				uv[component] = (w0 * a.uv[coord][component] * a.invW +
					w1 * b.uv[coord][component] * b.invW +
					w2 * c.uv[coord][component] * c.invW) * inverseReciprocalW;
			const Color sampled = sampleTexture(textures[stage], texturePixelFormats[stage], texturePixels[stage],
				textureDecodeTables[stage], uv[0], uv[1], textureFilters[stage],
				device.textureStageState[stage][13u], device.textureStageState[stage][14u]);
			stageTextureColors[stage] = {sampled.r, sampled.g, sampled.b, sampled.a};
		}
			const Color diffuse{
				(w0 * a.color.r * a.invW + w1 * b.color.r * b.invW + w2 * c.color.r * c.invW) * inverseReciprocalW,
				(w0 * a.color.g * a.invW + w1 * b.color.g * b.invW + w2 * c.color.g * c.invW) * inverseReciprocalW,
				(w0 * a.color.b * a.invW + w1 * b.color.b * b.invW + w2 * c.color.b * c.invW) * inverseReciprocalW,
				(w0 * a.color.a * a.invW + w1 * b.color.a * b.invW + w2 * c.color.a * c.invW) * inverseReciprocalW};
			Color specular{};
			if (interpolateSpecular) {
				specular = {
					(w0 * a.specular.r * a.invW + w1 * b.specular.r * b.invW + w2 * c.specular.r * c.invW) * inverseReciprocalW,
					(w0 * a.specular.g * a.invW + w1 * b.specular.g * b.invW + w2 * c.specular.g * c.invW) * inverseReciprocalW,
					(w0 * a.specular.b * a.invW + w1 * b.specular.b * b.invW + w2 * c.specular.b * c.invW) * inverseReciprocalW,
					(w0 * a.specular.a * a.invW + w1 * b.specular.a * b.invW + w2 * c.specular.a * c.invW) * inverseReciprocalW};
			}
			d3d7::BlendColor combined{diffuse.r, diffuse.g, diffuse.b, diffuse.a};
			if (textureStageCount && !d3d7::applyTextureStages(device.textureStageState,
				stageTextureColors, textureStageCount,
				{diffuse.r, diffuse.g, diffuse.b, diffuse.a}, combined)) {
					cpu->r[EAX] = kD3derrInvalidCall;
					return;
			}
			Color source{combined.r, combined.g, combined.b, combined.a};
			if (device.renderState[29u]) {
				source.r = clamp01(source.r + specular.r);
				source.g = clamp01(source.g + specular.g);
				source.b = clamp01(source.b + specular.b);
			}
			if (device.renderState[28u] && device.renderState[140u] != 0u) {
				const Color fog = unpackD3dColor(device.renderState[34u]);
				const d3d7::BlendColor fogged = d3d7::applyVertexFog(
					{source.r, source.g, source.b, source.a},
					{fog.r, fog.g, fog.b, fog.a}, specular.a);
				source = {fogged.r, fogged.g, fogged.b, fogged.a};
			}
			if (traceThisPixelProbeDraw && pixelProbeSampleCount < 3u) {
				++pixelProbeSampleCount;
				const auto texture = stageTextureColors[0];
				std::fprintf(stderr, "[br-d3d7-pixel] draw=%u sample=%u xy=%d,%d uv=%g,%g tex=%g,%g,%g,%g diffuse=%g,%g,%g,%g output=%g,%g,%g,%g packed=%08x\n",
					pixelProbeDraw, pixelProbeSampleCount, px, py,
					a.uv[0][0], a.uv[0][1], texture.r, texture.g, texture.b, texture.a,
					diffuse.r, diffuse.g, diffuse.b, diffuse.a,
					source.r, source.g, source.b, source.a,
					d3d7::encodePixel({{source.r, source.g, source.b, source.a}}, targetPixelFormat));
			}
		if (device.renderState[15u]) {
			if (!alphaTestPasses(source.a)) continue;
		}
			const Color destination = alphaBlendEnabled
				? readColor(targetPixelFormat, targetPixels, target.pitch, static_cast<uint32_t>(px), static_cast<uint32_t>(py), targetDecodeTable)
				: Color{};
			const d3d7::BlendColor sourceColor{source.r, source.g, source.b, source.a};
			const d3d7::BlendColor destinationColor{destination.r, destination.g, destination.b, destination.a};
			const d3d7::BlendColor blended = alphaBlendEnabled
				? d3d7::blend(device.renderState[19u], device.renderState[20u], sourceColor, destinationColor)
				: sourceColor;
			const Color output{blended.r, blended.g, blended.b, blended.a};
			writeColor(targetPixelFormat, outputPixels, target.pitch, static_cast<uint32_t>(px), static_cast<uint32_t>(py), output);
			const uint32_t depthValue = depth.bpp == 16u ? static_cast<uint32_t>(z * 65535.0f + 0.5f)
				: static_cast<uint32_t>(z * 4294967295.0f);
			if (depthWriteEnabled)
				for (uint32_t i = 0; i < depthBytes; ++i) depthPixels[depthOffset + i] = static_cast<uint8_t>(depthValue >> (i * 8u));
			++changedPixels;
		}
			d3d7::advanceTriangleScanlineEdges(rowE0, rowE1, rowE2,
				edge0StepY, edge1StepY, edge2StepY);
		}
		if (testedFragments != testedBeforeTriangle) ++coveredTriangles;
	};
#if defined(__ANDROID__)
	const auto d3dGeometryStart = std::chrono::steady_clock::now();
	profileScope.setupNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(d3dGeometryStart - profileScope.start).count());
#endif
	for (uint32_t first = 0u; first < rasterIndexCount; first += 3u) {
		const uint16_t ia = rasterIndices[first];
		const uint16_t ib = rasterIndices[first + 1u];
		const uint16_t ic = rasterIndices[first + 2u];
		clipped = {getVertex(ia), getVertex(ib), getVertex(ic)};
		if (!transformedGeometry) {
			// Most game geometry is already inside the homogeneous view volume.
			// Detect fully accepted/rejected triangles before running Sutherland-
			// Hodgman, which otherwise copies every large ClipVertex through six
			// temporary polygon passes even when no clipping is needed.
			uint32_t combinedOutcode = 0u;
			uint32_t commonOutcode = 0x3fu;
			for (const ClipVertex& vertex : clipped) {
				uint32_t outcode = 0u;
				for (uint32_t plane = 0u; plane < 6u; ++plane) {
					// Match the original inside test exactly, including NaN behavior.
					if (!(clipDistance(vertex, plane) >= 0.0f)) outcode |= 1u << plane;
				}
				combinedOutcode |= outcode;
				commonOutcode &= outcode;
			}
			if (commonOutcode != 0u) {
				clipped.clear();
			} else if (combinedOutcode != 0u) {
				for (uint32_t plane = 0u; plane < 6u && !clipped.empty(); ++plane) {
					clipScratch.clear();
					ClipVertex previous = clipped.back();
					float previousDistance = clipDistance(previous, plane);
					for (const ClipVertex& current : clipped) {
						const float currentDistance = clipDistance(current, plane);
						const bool previousInside = previousDistance >= 0.0f;
						const bool currentInside = currentDistance >= 0.0f;
						if (previousInside != currentInside) {
							const float t = previousDistance / (previousDistance - currentDistance);
							clipScratch.push_back(interpolate(previous, current, t));
						}
						if (currentInside) clipScratch.push_back(current);
						previous = current;
						previousDistance = currentDistance;
					}
					clipped.swap(clipScratch);
				}
			}
		}
		if (clipped.size() < 3u) { ++clipRejectedTriangles; continue; }
		screen.clear();
		for (const ClipVertex& v : clipped) {
			if (transformedGeometry) {
				if (!(v.p[3] > 0.0f)) continue;
				screen.push_back({v.p[0], v.p[1], clamp01(v.p[2]), v.p[3],
					v.uv, v.color, v.specular});
				continue;
			}
			if (!(v.p[3] > 1.0e-8f)) continue;
			const float invW = 1.0f / v.p[3];
			screen.push_back({float(viewportX) + (v.p[0] * invW + 1.0f) * float(viewportWidth) * 0.5f,
				float(viewportY) + (1.0f - v.p[1] * invW) * float(viewportHeight) * 0.5f,
				viewportMinZ + v.p[2] * invW * (viewportMaxZ - viewportMinZ), invW,
				v.uv, v.color, v.specular});
		}
		if (screen.size() < 3u) { ++projectionRejectedPolygons; continue; }
		for (size_t i = 1u; i + 1u < screen.size(); ++i) {
			if (!tryGpuRenderer) {
				rasterTriangle(screen[0], screen[i], screen[i + 1u]);
				continue;
			}
			const ScreenVertex triangle[3] = {screen[0], screen[i], screen[i + 1u]};
			for (const ScreenVertex& source : triangle) {
				d3d7::GpuVertex vertex{};
				vertex.x = source.x; vertex.y = source.y; vertex.z = source.z;
				vertex.reciprocalW = source.invW;
				vertex.diffuse = {{source.color.r, source.color.g, source.color.b, source.color.a}};
				vertex.specular = {{source.specular.r, source.specular.g, source.specular.b, source.specular.a}};
				for (uint32_t set = 0u; set < vertex.texcoord.size(); ++set)
					vertex.texcoord[set] = {{source.uv[set][0], source.uv[set][1]}};
				gpuVertices.push_back(vertex);
			}
		}
	}
#if defined(__ANDROID__)
	profileScope.geometryNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
		std::chrono::nanoseconds>(std::chrono::steady_clock::now() - d3dGeometryStart).count());
#endif
	bool gpuRendered = false;
	if (!gpuVertices.empty()) {
		d3d7::GpuDrawState gpuState{};
		gpuState.target = gpuSurfaceView(target);
		gpuState.depth = gpuSurfaceView(depth);
		gpuState.renderState = device.renderState;
		gpuState.textureStageState = device.textureStageState;
		gpuState.activeTextureStages = textureStageCount;
		gpuState.viewportX = viewportX; gpuState.viewportY = viewportY;
		gpuState.viewportWidth = viewportWidth; gpuState.viewportHeight = viewportHeight;
		for (uint32_t stage = 0u; stage < textureStageCount; ++stage)
			if (textureStageUsesTexture[stage]) gpuState.textures[stage] = gpuSurfaceView(textures[stage]);
	#if defined(__ANDROID__)
		auto gpuSubmitStart = std::chrono::steady_clock::now();
	#endif
		gpuRendered = d3d7::renderGpuTriangles(gpuState, gpuVertices.data(),
			static_cast<uint32_t>(gpuVertices.size()));
	#if defined(__ANDROID__)
		profileScope.gpuSubmitNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
			std::chrono::nanoseconds>(std::chrono::steady_clock::now() - gpuSubmitStart).count());
	#endif
		if (!gpuRendered) {
			// An unsupported GLES draw may follow earlier GLES draws to this same
			// target. Import those pixels before the CPU reference rasterizer
			// continues the command stream, or it would overwrite stale guest data.
		#if defined(__ANDROID__)
			auto syncStart = std::chrono::steady_clock::now();
		#endif
			synchronizeSurfaceForCpu(target.handle);
			if (depthWriteEnabled) synchronizeSurfaceForCpu(depth.handle);
		#if defined(__ANDROID__)
			profileScope.syncReadbackNanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<
				std::chrono::nanoseconds>(std::chrono::steady_clock::now() - syncStart).count());
		#endif
			// Replay the same clipped triangles through the proven software path.
		#if defined(__ANDROID__)
			auto fallbackStart = std::chrono::steady_clock::now();
		#endif
			for (size_t i = 0u; i + 2u < gpuVertices.size(); i += 3u) {
				ScreenVertex triangle[3]{};
				for (uint32_t corner = 0u; corner < 3u; ++corner) {
					const auto& source = gpuVertices[i + corner];
					auto& out = triangle[corner];
					out.x = source.x; out.y = source.y; out.z = source.z; out.invW = source.reciprocalW;
					out.color = {source.diffuse[0], source.diffuse[1], source.diffuse[2], source.diffuse[3]};
					out.specular = {source.specular[0], source.specular[1], source.specular[2], source.specular[3]};
					for (uint32_t set = 0u; set < out.uv.size(); ++set) {
						out.uv[set][0] = source.texcoord[set][0]; out.uv[set][1] = source.texcoord[set][1];
					}
				}
				rasterTriangle(triangle[0], triangle[1], triangle[2]);
			}
		#if defined(__ANDROID__)
			profileScope.fallbackNanoseconds = static_cast<uint64_t>(std::chrono::duration_cast<
				std::chrono::nanoseconds>(std::chrono::steady_clock::now() - fallbackStart).count());
		#endif
		}
	}
	if (!gpuRendered) {
		markSurfaceCpuWrite(target.handle);
		if (depthWriteEnabled) markSurfaceCpuWrite(depth.handle);
	}
#if defined(__ANDROID__)
	if (tryGpuRenderer && !gpuVertices.empty()) {
		if (gpuRendered) {
			gProfileGpuDrawCalls.fetch_add(1u, std::memory_order_relaxed);
			gProfileGpuTriangles.fetch_add(gpuVertices.size() / 3u, std::memory_order_relaxed);
		} else {
			gProfileSoftwareFallbackDrawCalls.fetch_add(1u, std::memory_order_relaxed);
			gProfileSoftwareFallbackTriangles.fetch_add(rasterIndexCount / 3u, std::memory_order_relaxed);
		}
	}
#endif
	bool computeFramebufferHash = traceRaster;
#if defined(__ANDROID__)
	computeFramebufferHash = computeFramebufferHash ||
		(fvf == 0x000002c2u && !gLoggedFvf2c2Completed.load(std::memory_order_relaxed));
#endif
	if (gpuRendered && computeFramebufferHash) {
		#if defined(__ANDROID__)
		auto syncStart = std::chrono::steady_clock::now();
		#endif
		synchronizeSurfaceForCpu(target.handle);
		if (depthWriteEnabled) synchronizeSurfaceForCpu(depth.handle);
		#if defined(__ANDROID__)
		profileScope.syncReadbackNanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<
			std::chrono::nanoseconds>(std::chrono::steady_clock::now() - syncStart).count());
		#endif
	}
	uint32_t hash = 0u;
	if (computeFramebufferHash) {
	#if defined(__ANDROID__)
		auto hashStart = std::chrono::steady_clock::now();
	#endif
		hash = 2166136261u;
		for (uint32_t i = 0u; i < target.bytes; ++i)
			hash = (hash ^ outputPixels[i]) * 16777619u;
	#if defined(__ANDROID__)
		profileScope.syncReadbackNanoseconds += static_cast<uint64_t>(std::chrono::duration_cast<
			std::chrono::nanoseconds>(std::chrono::steady_clock::now() - hashStart).count());
	#endif
	}
	cpu->r[EAX] = 0u;
#if defined(__ANDROID__)
	if (fvf == 0x000002c2u && !gLoggedFvf2c2Completed.exchange(true, std::memory_order_relaxed)) {
		if (gpuRendered) {
			br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
				"D3D7_GLES_DRAW_COMPLETED", "caller=0x%08x submitted_triangles=%u target=%08x framebuffer_hash=%08x",
				guestCaller, static_cast<uint32_t>(gpuVertices.size() / 3u), target.handle, hash);
		} else {
			br::runtime::androidDiagnosticLogEvent(ANDROID_LOG_INFO, "BattleRealmsNative",
				"D3D7_FVF2C2_DRAW_COMPLETED", "caller=0x%08x input_triangles=%u clip_rejected=%u projection_rejected=%u raster_triangles=%u degenerate=%u culled=%u offscreen=%u covered=%u tested_fragments=%llu depth_passed=%llu changed_pixels=%llu cull_mode=%u depth_enable=%u viewport=%u,%u,%u,%u framebuffer_hash=%08x",
				guestCaller, rasterIndexCount / 3u, clipRejectedTriangles, projectionRejectedPolygons, rasterizedTriangles,
				degenerateTriangles, culledTriangles, offscreenTriangles, coveredTriangles,
				static_cast<unsigned long long>(testedFragments), static_cast<unsigned long long>(passedDepth),
				static_cast<unsigned long long>(changedPixels), cullMode, depthEnabled ? 1u : 0u,
				viewportX, viewportY, viewportWidth, viewportHeight, hash);
		}
	}
#endif
	if (traceRaster)
		std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive rasterized target=%08x fvf=%08x topology=%u vertices=%u stages=%u lighting=%u triangles=%u flat-color=%u vertex-cache-hits=%llu bounds-candidates=%llu span-pixels=%llu skipped-bounds=%llu fallback-rows=%llu tested=%llu depth-passed=%llu fragments=%llu hash=%08x\n",
			target.handle, fvf, primitiveType, vertexCount, textureStageCount,
			device.renderState[137u], rasterIndexCount / 3u, flatColorTriangles,
			static_cast<unsigned long long>(transformedVertexCacheHits),
			static_cast<unsigned long long>(rasterBoundsCandidates),
			static_cast<unsigned long long>(scanlineSpanPixels),
			static_cast<unsigned long long>(skippedBoundsCandidates),
			static_cast<unsigned long long>(scanlineFallbackRows),
			static_cast<unsigned long long>(testedFragments),
			static_cast<unsigned long long>(passedDepth), static_cast<unsigned long long>(changedPixels), hash);
	return;

	/*
	const uint32_t self = argument(cpu, 0);
	const uint32_t primitiveType = argument(cpu, 1);
	const uint32_t fvf = argument(cpu, 2);
	const uint32_t vertices = argument(cpu, 3);
	const uint32_t vertexCount = argument(cpu, 4);
	const uint32_t indices = argument(cpu, 5);
	const uint32_t indexCount = argument(cpu, 6);
	const uint32_t flags = argument(cpu, 7);
	const uint32_t stride = d3d7FvfStride(fvf);
	const uint64_t vertexBytes = uint64_t(stride) * vertexCount;
	const uint64_t indexBytes = uint64_t(indexCount) * sizeof(uint16_t);
	const auto* vertexData = stride && vertexBytes <= 256ull * 1024ull * 1024ull
		? sharedGuestAddressSpace().host(vertices, static_cast<uint32_t>(vertexBytes)) : nullptr;
	const auto* indexData = indexCount && indexBytes <= 256ull * 1024ull * 1024ull
		? sharedGuestAddressSpace().host(indices, static_cast<uint32_t>(indexBytes)) : nullptr;
	if (!vertexData || !indexData || !vertexCount || !indexCount) {
		cpu->r[EAX] = 0x8876086cu; // D3DERR_INVALIDCALL for invalid guest buffers/format.
		return;
	}
	uint16_t minimumIndex = UINT16_MAX;
	uint16_t maximumIndex = 0u;
	for (uint32_t i = 0; i < indexCount; ++i) {
		const uint16_t index = guest::load<uint16_t>(indices + i * 2u);
		minimumIndex = std::min(minimumIndex, index);
		maximumIndex = std::max(maximumIndex, index);
		if (index >= vertexCount) {
			cpu->r[EAX] = 0x8876086cu;
			return;
		}
	}
	if (std::getenv("BR_TRACE_DDRAW") || std::getenv("BR_TRACE_D3D7")) {
		const uint32_t caller = guest::load<uint32_t>(cpu->r[ESP]);
		GuestD3d7Device device{};
		{
			std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
			const auto found = gD3d7Devices.find(self);
			if (found != gD3d7Devices.end()) device = found->second;
		}
		std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive input device=%08x caller=%08x type=%u fvf=%08x stride=%u vertices=%08x/%u indices=%08x/%u range=%u..%u flags=%08x\n",
			self, caller, primitiveType, fvf, stride, vertices, vertexCount, indices,
			indexCount, minimumIndex, maximumIndex, flags);
		for (uint32_t i = 0; i < std::min<uint32_t>(indexCount, 6u); ++i)
			std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive index[%u]=%u\n", i,
				guest::load<uint16_t>(indices + i * 2u));
		const uint32_t sampleCount = std::min<uint32_t>(maximumIndex + 1u, 3u);
		for (uint32_t i = 0; i < sampleCount; ++i) {
			float x = 0.0f, y = 0.0f, z = 0.0f;
			float nx = 0.0f, ny = 0.0f, nz = 0.0f;
			float u = 0.0f, v = 0.0f;
			std::memcpy(&x, vertexData + size_t(i) * stride, sizeof(x));
			std::memcpy(&y, vertexData + size_t(i) * stride + 4u, sizeof(y));
			std::memcpy(&z, vertexData + size_t(i) * stride + 8u, sizeof(z));
			std::memcpy(&nx, vertexData + size_t(i) * stride + 12u, sizeof(nx));
			std::memcpy(&ny, vertexData + size_t(i) * stride + 16u, sizeof(ny));
			std::memcpy(&nz, vertexData + size_t(i) * stride + 20u, sizeof(nz));
			std::memcpy(&u, vertexData + size_t(i) * stride + 32u, sizeof(u));
			std::memcpy(&v, vertexData + size_t(i) * stride + 36u, sizeof(v));
			std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive vertex[%u]=(%g,%g,%g) n=(%g,%g,%g) uv=(%g,%g) diffuse=%08x specular=%08x\n",
				i, x, y, z, nx, ny, nz, u, v, guest::load<uint32_t>(vertices + i * stride + 24u),
				guest::load<uint32_t>(vertices + i * stride + 28u));
		}
		for (const uint32_t state : {1u, 2u, 3u}) {
			std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive transform[%u]=", state);
			for (uint32_t i = 0; i < 16u; ++i)
				std::fprintf(stderr, "%s%08x", i ? "," : "", device.transforms[state][i]);
			std::fputc('\n', stderr);
		}
		std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive render-state lighting=%u ambient=%08x cull=%u zfunc=%u zwrite=%u alphablend=%u fog=%u fogcolor=%08x\n",
			device.renderState[137u], device.renderState[139u], device.renderState[22u],
			device.renderState[23u], device.renderState[14u], device.renderState[27u],
			device.renderState[28u], device.renderState[146u]);
		std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive stage0-state=");
		for (uint32_t i = 0; i < device.textureStageState[0].size(); ++i)
			std::fprintf(stderr, "%s%u:%08x", i ? "," : "", i, device.textureStageState[0][i]);
		std::fputc('\n', stderr);
		GuestDdrawSurface texture{};
		if (device.textures[0] && loadGuestSurface(device.textures[0], texture))
			std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitive texture0=%08x %ux%ux%u pitch=%u pixels=%08x stage0-colorop=%u\n",
				texture.handle, texture.width, texture.height, texture.bpp, texture.pitch,
				texture.pixels, device.textureStageState[0][1]);
	}
	// Keep the current first-failure policy: validate and report real guest draw
	// inputs, but do not claim success until rasterization has written pixels.
	throw std::runtime_error("IDirect3DDevice7::DrawIndexedPrimitive has valid input, but the D3D7 raster path is not implemented");
	*/
}

void d3d7DeviceDrawPrimitive(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	constexpr uint32_t kD3ddpWait = 0x00000001u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t primitiveType = argument(cpu, 1);
	const uint32_t fvf = argument(cpu, 2);
	const uint32_t vertices = argument(cpu, 3);
	const uint32_t vertexCount = argument(cpu, 4);
	const uint32_t flags = argument(cpu, 5);
	const uint32_t stride = d3d7FvfStride(fvf);
	const uint64_t vertexBytes = uint64_t(stride) * vertexCount;
	if (!stride || !vertexCount || vertexBytes > 64ull * 1024ull * 1024ull ||
		!sharedGuestAddressSpace().host(vertices, static_cast<uint32_t>(vertexBytes)) ||
		(flags & ~kD3ddpWait)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	std::vector<uint16_t> generatedIndices;
	if (!d3d7::expandTrianglePrimitive(primitiveType, vertexCount, generatedIndices)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	const uint64_t indexBytes64 = uint64_t(generatedIndices.size()) * sizeof(uint16_t);
	if (indexBytes64 > 64ull * 1024ull * 1024ull || indexBytes64 > 0xffffffffu) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	const uint32_t indexBytes = static_cast<uint32_t>(indexBytes64);
	const uint32_t originalEsp = cpu->r[ESP];
	const uint64_t scratchBytes64 = uint64_t(indexBytes) + 0x40u;
	if (scratchBytes64 > originalEsp) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	const uint32_t scratchEsp = originalEsp - static_cast<uint32_t>(scratchBytes64);
	const uint32_t indexAddress = scratchEsp;
	const uint32_t adaptedEsp = originalEsp - 0x40u;
	if (!sharedGuestAddressSpace().host(indexAddress, indexBytes) ||
		!sharedGuestAddressSpace().host(adaptedEsp, 36u) ||
		!sharedGuestAddressSpace().host(originalEsp, 4u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	for (uint32_t i = 0; i < generatedIndices.size(); ++i)
		guest::store<uint16_t>(indexAddress + i * 2u, generatedIndices[i]);
	guest::store<uint32_t>(adaptedEsp, guest::load<uint32_t>(originalEsp));
	const uint32_t adaptedArguments[8] = {self, 4u, fvf, vertices, vertexCount,
		indexAddress, static_cast<uint32_t>(generatedIndices.size()), 0u};
	for (uint32_t i = 0; i < 8u; ++i)
		guest::store<uint32_t>(adaptedEsp + 4u + i * 4u, adaptedArguments[i]);
	cpu->r[ESP] = adaptedEsp;
	try {
		const ApiImportStub unused{};
		d3d7DeviceDrawIndexedPrimitive(cpu, unused);
	} catch (...) {
		cpu->r[ESP] = originalEsp;
		throw;
	}
	cpu->r[ESP] = originalEsp;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-d3d7] DrawPrimitive caller=%08x type=%u fvf=%08x vertices=%08x/%u flags=%08x triangles=%u result=%08x\n",
			guest::load<uint32_t>(originalEsp), primitiveType, fvf, vertices, vertexCount,
			flags, static_cast<uint32_t>(generatedIndices.size() / 3u), cpu->r[EAX]);
}

void d3d7DeviceDrawPrimitiveVB(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t primitiveType = argument(cpu, 1);
	const uint32_t vertexBufferHandle = argument(cpu, 2);
	const uint32_t startVertex = argument(cpu, 3);
	const uint32_t vertexCount = argument(cpu, 4);
	const uint32_t flags = argument(cpu, 5);
	GuestD3d7VertexBuffer vertexBuffer{};
	{
		std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
		const auto found = gD3d7VertexBuffers.find(vertexBufferHandle);
		if (found == gD3d7VertexBuffers.end() || found->second.locked ||
			!found->second.stride || startVertex > found->second.vertices ||
			vertexCount > found->second.vertices - startVertex) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		vertexBuffer = found->second;
	}
	const uint64_t vertexAddress = uint64_t(vertexBuffer.data) + uint64_t(startVertex) * vertexBuffer.stride;
	const uint64_t vertexBytes = uint64_t(vertexCount) * vertexBuffer.stride;
	if (!vertexCount || vertexAddress > 0xffffffffu || vertexBytes > 64ull * 1024ull * 1024ull ||
		!sharedGuestAddressSpace().host(static_cast<uint32_t>(vertexAddress), static_cast<uint32_t>(vertexBytes))) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	const uint32_t originalEsp = cpu->r[ESP];
	const uint32_t adaptedEsp = originalEsp - 0x20u;
	if (!sharedGuestAddressSpace().host(adaptedEsp, 28u) ||
		!sharedGuestAddressSpace().host(originalEsp, 4u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	guest::store<uint32_t>(adaptedEsp, guest::load<uint32_t>(originalEsp));
	const uint32_t adaptedArguments[6] = {
		self, primitiveType, vertexBuffer.fvf, static_cast<uint32_t>(vertexAddress), vertexCount, flags};
	for (uint32_t i = 0; i < 6u; ++i)
		guest::store<uint32_t>(adaptedEsp + 4u + i * 4u, adaptedArguments[i]);
	cpu->r[ESP] = adaptedEsp;
	try {
		const ApiImportStub unused{};
		d3d7DeviceDrawPrimitive(cpu, unused);
	} catch (...) {
		cpu->r[ESP] = originalEsp;
		throw;
	}
	cpu->r[ESP] = originalEsp;
	if (std::getenv("BR_TRACE_D3D7"))
		std::fprintf(stderr, "[br-d3d7] DrawPrimitiveVB vb=%08x fvf=%08x type=%u start=%u vertices=%u flags=%08x result=%08x\n",
			vertexBufferHandle, vertexBuffer.fvf, primitiveType, startVertex, vertexCount, flags, cpu->r[EAX]);
}

void d3d7DeviceDrawIndexedPrimitiveVB(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t primitiveType = argument(cpu, 1);
	const uint32_t vertexBufferHandle = argument(cpu, 2);
	const uint32_t startVertex = argument(cpu, 3);
	const uint32_t vertexCount = argument(cpu, 4);
	const uint32_t indices = argument(cpu, 5);
	const uint32_t indexCount = argument(cpu, 6);
	const uint32_t flags = argument(cpu, 7);
	GuestD3d7VertexBuffer vertexBuffer{};
	{
		std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
		const auto found = gD3d7VertexBuffers.find(vertexBufferHandle);
		if (found == gD3d7VertexBuffers.end() || found->second.locked ||
			!found->second.stride || startVertex > found->second.vertices ||
			vertexCount > found->second.vertices - startVertex) {
			cpu->r[EAX] = kD3derrInvalidCall;
			return;
		}
		vertexBuffer = found->second;
	}
	const uint64_t vertexAddress = uint64_t(vertexBuffer.data) + uint64_t(startVertex) * vertexBuffer.stride;
	const uint64_t vertexBytes = uint64_t(vertexCount) * vertexBuffer.stride;
	const uint64_t indexBytes = uint64_t(indexCount) * sizeof(uint16_t);
	if (!vertexCount || !indexCount || vertexAddress > 0xffffffffu ||
		vertexBytes > 64ull * 1024ull * 1024ull || indexBytes > 64ull * 1024ull * 1024ull ||
		!sharedGuestAddressSpace().host(static_cast<uint32_t>(vertexAddress), static_cast<uint32_t>(vertexBytes)) ||
		!sharedGuestAddressSpace().host(indices, static_cast<uint32_t>(indexBytes))) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	const uint32_t originalEsp = cpu->r[ESP];
	const uint32_t scratchEsp = originalEsp - 0x40u;
	if (!sharedGuestAddressSpace().host(scratchEsp, 36u) ||
		!sharedGuestAddressSpace().host(originalEsp, 4u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	guest::store<uint32_t>(scratchEsp, guest::load<uint32_t>(originalEsp));
	const uint32_t adaptedArguments[8] = {
		self, primitiveType, vertexBuffer.fvf, static_cast<uint32_t>(vertexAddress),
		vertexCount, indices, indexCount, flags};
	for (uint32_t i = 0; i < 8u; ++i)
		guest::store<uint32_t>(scratchEsp + 4u + i * 4u, adaptedArguments[i]);
	cpu->r[ESP] = scratchEsp;
	try {
		const ApiImportStub unused{};
		d3d7DeviceDrawIndexedPrimitive(cpu, unused);
	} catch (...) {
		cpu->r[ESP] = originalEsp;
		throw;
	}
	cpu->r[ESP] = originalEsp;
	if (std::getenv("BR_TRACE_D3D7"))
		std::fprintf(stderr, "[br-d3d7] DrawIndexedPrimitiveVB vb=%08x fvf=%08x start=%u vertices=%u indices=%u flags=%08x result=%08x\n",
			vertexBufferHandle, vertexBuffer.fvf, startVertex, vertexCount, indexCount,
			flags, cpu->r[EAX]);
}

void d3d7CreateVertexBuffer(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDescSize = 16u;
	constexpr uint32_t kD3derrInvalidParams = 0x80070057u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t desc = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2);
	const uint32_t flags = argument(cpu, 3);
	if (self != kD3d7Object || !desc || !output || flags != 0u ||
		!sharedGuestAddressSpace().host(desc, kDescSize) || !sharedGuestAddressSpace().host(output, 4u) ||
		guest::load<uint32_t>(desc) != kDescSize) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	const uint32_t caps = guest::load<uint32_t>(desc + 4u);
	const uint32_t fvf = guest::load<uint32_t>(desc + 8u);
	const uint32_t vertices = guest::load<uint32_t>(desc + 12u);
	const uint32_t stride = d3d7FvfStride(fvf);
	const uint64_t byteCount = uint64_t(stride) * vertices;
	if (!stride || !vertices || vertices > (1u << 20u) || byteCount > 256ull * 1024ull * 1024ull) {
		cpu->r[EAX] = 0x8876086cu; // D3DERR_INVALIDCALL.
		return;
	}
	const uint32_t dataSize = static_cast<uint32_t>(byteCount);
	const uint32_t mappedSize = static_cast<uint32_t>((byteCount + 0xfffu) & ~uint64_t(0xfffu));
	std::string error;
	std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
	const uint32_t object = gNextD3d7VertexBuffer;
	const uint32_t data = gNextD3d7VertexBufferData;
	if (!sharedGuestAddressSpace().commitRange(object, 0x20u, error) ||
		!sharedGuestAddressSpace().commitRange(data, mappedSize, error)) {
		cpu->r[EAX] = 0x8007000eu; // E_OUTOFMEMORY.
		return;
	}
	std::memset(sharedGuestAddressSpace().host(object, 0x20u), 0, 0x20u);
	std::memset(sharedGuestAddressSpace().host(data, dataSize), 0, dataSize);
	guest::store<uint32_t>(object, kD3d7VertexBufferVtable);
	guest::store<uint32_t>(object + 4u, 1u);
	GuestD3d7VertexBuffer buffer{};
	buffer.handle = object;
	buffer.data = data;
	buffer.size = dataSize;
	buffer.fvf = fvf;
	buffer.caps = caps;
	buffer.vertices = vertices;
	buffer.stride = stride;
	gD3d7VertexBuffers.emplace(object, buffer);
	gNextD3d7VertexBuffer += 0x20u;
	gNextD3d7VertexBufferData += mappedSize;
	guest::store<uint32_t>(output, object);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] CreateVertexBuffer fvf=%08x stride=%u vertices=%u bytes=%u -> %08x\n",
			fvf, stride, vertices, dataSize, object);
	}
}

void d3d7VertexBufferAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
	auto found = gD3d7VertexBuffers.find(self);
	if (found == gD3d7VertexBuffers.end()) { cpu->r[EAX] = 0u; return; }
	cpu->r[EAX] = ++found->second.references;
	guest::store<uint32_t>(self + 4u, found->second.references);
}

void d3d7VertexBufferRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
	auto found = gD3d7VertexBuffers.find(self);
	if (found == gD3d7VertexBuffers.end()) { cpu->r[EAX] = 0u; return; }
	const uint32_t references = found->second.references ? --found->second.references : 0u;
	guest::store<uint32_t>(self + 4u, references);
	if (!references) gD3d7VertexBuffers.erase(found);
	cpu->r[EAX] = references;
}

void d3d7VertexBufferLock(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidParams = 0x80070057u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t flags = argument(cpu, 1);
	const uint32_t dataOut = argument(cpu, 2);
	const uint32_t sizeOut = argument(cpu, 3);
	if (!dataOut || !sharedGuestAddressSpace().host(dataOut, 4u) || (sizeOut && !sharedGuestAddressSpace().host(sizeOut, 4u))) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
	auto found = gD3d7VertexBuffers.find(self);
	if (found == gD3d7VertexBuffers.end() || found->second.locked) { cpu->r[EAX] = 0x88760868u; return; }
	if (flags & 0x00000002u) std::memset(sharedGuestAddressSpace().host(found->second.data, found->second.size), 0, found->second.size);
	found->second.locked = true;
	guest::store<uint32_t>(dataOut, found->second.data);
	if (sizeOut) guest::store<uint32_t>(sizeOut, found->second.size);
	cpu->r[EAX] = 0u;
}

void d3d7VertexBufferUnlock(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
	auto found = gD3d7VertexBuffers.find(self);
	if (found == gD3d7VertexBuffers.end() || !found->second.locked) { cpu->r[EAX] = 0x88760868u; return; }
	found->second.locked = false;
	cpu->r[EAX] = 0u;
}

void d3d7VertexBufferGetDesc(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDescSize = 16u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, kDescSize)) { cpu->r[EAX] = 0x80070057u; return; }
	std::lock_guard<std::mutex> lock(gD3d7VertexBuffersMutex);
	auto found = gD3d7VertexBuffers.find(self);
	if (found == gD3d7VertexBuffers.end()) { cpu->r[EAX] = 0x80070057u; return; }
	guest::store<uint32_t>(output, kDescSize);
	guest::store<uint32_t>(output + 4u, found->second.caps);
	guest::store<uint32_t>(output + 8u, found->second.fvf);
	guest::store<uint32_t>(output + 12u, found->second.vertices);
	cpu->r[EAX] = 0u;
}

void d3d7DeviceSetTexture(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	constexpr uint32_t kDdsCapsTexture = 0x00001000u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t stage = argument(cpu, 1);
	const uint32_t texture = argument(cpu, 2);
	if (stage >= 8u) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	{
		std::lock_guard<std::mutex> devicesLock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		const uint32_t previous = device->second.textures[stage];
		if (previous == texture) { cpu->r[EAX] = 0u; return; }
		std::lock_guard<std::mutex> surfacesLock(gDdrawSurfacesMutex);
		if (texture) {
			auto next = gDdrawSurfaces.find(texture);
			if (next == gDdrawSurfaces.end() || !(next->second.caps & kDdsCapsTexture)) {
				cpu->r[EAX] = kD3derrInvalidCall;
				return;
			}
			++next->second.references;
			guest::store<uint32_t>(texture + 4u, next->second.references);
		}
		if (previous) {
			auto old = gDdrawSurfaces.find(previous);
			if (old != gDdrawSurfaces.end()) {
				if (old->second.references > 0u) --old->second.references;
				guest::store<uint32_t>(previous + 4u, old->second.references);
				if (!old->second.references) {
					if (gGuestPrimarySurface == previous) gGuestPrimarySurface = 0u;
					gDdrawSurfaces.erase(old);
				}
			}
		}
		device->second.textures[stage] = texture;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-d3d7] SetTexture device=%08x stage=%u texture=%08x\n", self, stage, texture);
}

void d3d7DeviceSetTextureStageState(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t stage = argument(cpu, 1);
	const uint32_t state = argument(cpu, 2);
	const uint32_t value = argument(cpu, 3);
	if (stage >= 8u || state >= 32u) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto device = gD3d7Devices.find(self);
		if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
		device->second.textureStageState[stage][state] = value;
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] SetTextureStageState stage=%u state=%u value=%08x\n",
			stage, state, value);
	}
}

void d3d7DeviceGetTextureStageState(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3derrInvalidCall = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t stage = argument(cpu, 1);
	const uint32_t state = argument(cpu, 2);
	const uint32_t output = argument(cpu, 3);
	if (stage >= 8u || state >= 32u || !output || !sharedGuestAddressSpace().host(output, 4u)) {
		cpu->r[EAX] = kD3derrInvalidCall;
		return;
	}
	std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
	auto device = gD3d7Devices.find(self);
	if (device == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidCall; return; }
	guest::store<uint32_t>(output, device->second.textureStageState[stage][state]);
	cpu->r[EAX] = 0u;
}

void d3d7DeviceEnumTextureFormats(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	const uint32_t callback = argument(cpu, 1);
	const uint32_t context = argument(cpu, 2);
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		if (gD3d7Devices.find(self) == gD3d7Devices.end() || !callback) {
			cpu->r[EAX] = 0x80070057u; // D3DERR_INVALIDPARAMS.
			return;
		}
	}
	struct TextureFormat { uint32_t flags, bits, red, green, blue, alpha; };
	// These are the common Direct3D7 RGB formats exposed by the existing
	// PCRecomp D3D HLE. Battle Realms' callback decides which it can use.
	static constexpr std::array<TextureFormat, 4> formats = {{
		{0x40u, 16u, 0x0000f800u, 0x000007e0u, 0x0000001fu, 0u},
		{0x41u, 16u, 0x00007c00u, 0x000003e0u, 0x0000001fu, 0x00008000u},
		{0x41u, 16u, 0x00000f00u, 0x000000f0u, 0x0000000fu, 0x0000f000u},
		{0x41u, 32u, 0x00ff0000u, 0x0000ff00u, 0x000000ffu, 0xff000000u}
	}};
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kD3d7TexturePixelFormat, 0x1000u, error)) {
		cpu->r[EAX] = 0x8007000eu;
		return;
	}
	const CPU saved = *cpu;
	uint32_t enumerated = 0u;
	for (const TextureFormat& format : formats) {
		std::memset(sharedGuestAddressSpace().host(kD3d7TexturePixelFormat, 32u), 0, 32u);
		guest::store<uint32_t>(kD3d7TexturePixelFormat, 32u);
		guest::store<uint32_t>(kD3d7TexturePixelFormat + 4u, format.flags);
		guest::store<uint32_t>(kD3d7TexturePixelFormat + 12u, format.bits);
		guest::store<uint32_t>(kD3d7TexturePixelFormat + 16u, format.red);
		guest::store<uint32_t>(kD3d7TexturePixelFormat + 20u, format.green);
		guest::store<uint32_t>(kD3d7TexturePixelFormat + 24u, format.blue);
		guest::store<uint32_t>(kD3d7TexturePixelFormat + 28u, format.alpha);
		const uint32_t args[2] = {kD3d7TexturePixelFormat, context};
		uint32_t keepGoing = 0u;
		if (!callGuestStdcall(cpu, callback, args, 2u, keepGoing, error)) {
			*cpu = saved;
			br::runtime::dispatchFailure(cpu, callback, error.c_str());
		}
		*cpu = saved;
		++enumerated;
		if (keepGoing == 0u) break; // D3DENUMRET_CANCEL.
	}
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] EnumTextureFormats callback=%08x formats_seen=%u\n", callback, enumerated);
	}
}

void d3d7DeviceClear(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kD3dclearTarget = 0x00000001u;
	constexpr uint32_t kD3dclearZBuffer = 0x00000002u;
	constexpr uint32_t kD3derrInvalidParams = 0x8876086cu;
	const uint32_t self = argument(cpu, 0);
	const uint32_t rectCount = argument(cpu, 1);
	const uint32_t rectArray = argument(cpu, 2);
	const uint32_t flags = argument(cpu, 3);
	const uint32_t argb = argument(cpu, 4);
	const uint32_t zBits = argument(cpu, 5);
	if (rectCount > 4096u || (rectCount && (!rectArray ||
		!sharedGuestAddressSpace().host(rectArray, uint64_t(rectCount) * 16u)))) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	GuestD3d7Device device{};
	{
		std::lock_guard<std::mutex> lock(gD3d7DevicesMutex);
		auto found = gD3d7Devices.find(self);
		if (found == gD3d7Devices.end()) { cpu->r[EAX] = kD3derrInvalidParams; return; }
		device = found->second;
	}
	GuestDdrawSurface target{};
	GuestDdrawSurface depth{};
	if ((flags & kD3dclearTarget) && !loadGuestSurface(device.renderTarget, target)) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	if ((flags & kD3dclearZBuffer) && (!loadGuestSurface(device.renderTarget, target) ||
		!target.zBuffer || !loadGuestSurface(target.zBuffer, depth))) {
		cpu->r[EAX] = kD3derrInvalidParams;
		return;
	}
	if (!(flags & (kD3dclearTarget | kD3dclearZBuffer))) { cpu->r[EAX] = 0u; return; }
	if (flags & kD3dclearTarget) synchronizeSurfaceForCpu(target.handle);
	if (flags & kD3dclearZBuffer) synchronizeSurfaceForCpu(depth.handle);
	auto rectAt = [rectArray, rectCount](uint32_t index, int32_t& left, int32_t& top, int32_t& right, int32_t& bottom) {
		if (!rectCount) { left = 0; top = 0; right = -1; bottom = -1; return; }
		const uint32_t address = rectArray + index * 16u;
		left = static_cast<int32_t>(guest::load<uint32_t>(address));
		top = static_cast<int32_t>(guest::load<uint32_t>(address + 4u));
		right = static_cast<int32_t>(guest::load<uint32_t>(address + 8u));
		bottom = static_cast<int32_t>(guest::load<uint32_t>(address + 12u));
	};
	auto writePixels = [](const GuestDdrawSurface& surface, int32_t left, int32_t top, int32_t right,
		int32_t bottom, uint32_t pixel) {
		const int32_t x0 = std::max<int32_t>(left, 0);
		const int32_t y0 = std::max<int32_t>(top, 0);
		const int32_t x1 = std::min<int32_t>(right, static_cast<int32_t>(surface.width));
		const int32_t y1 = std::min<int32_t>(bottom, static_cast<int32_t>(surface.height));
		const uint32_t bytesPerPixel = (surface.bpp + 7u) / 8u;
		if (x1 <= x0 || y1 <= y0 || bytesPerPixel == 0u) return;
		auto* pixels = sharedGuestAddressSpace().host(surface.pixels, surface.bytes);
		if (!pixels) return;
		for (int32_t y = y0; y < y1; ++y) {
			auto* row = pixels + static_cast<size_t>(y) * surface.pitch;
			for (int32_t x = x0; x < x1; ++x) {
				uint8_t* out = row + static_cast<size_t>(x) * bytesPerPixel;
				for (uint32_t b = 0; b < bytesPerPixel; ++b) out[b] = static_cast<uint8_t>(pixel >> (b * 8u));
			}
		}
	};
	const uint32_t rectsToClear = rectCount ? rectCount : 1u;
	uint32_t colorPixel = argb;
	if (target.bpp == 16u) {
		colorPixel = (((argb >> 19u) & 0x1fu) << 11u) |
			(((argb >> 10u) & 0x3fu) << 5u) | ((argb >> 3u) & 0x1fu);
	} else if (target.bpp == 24u) {
		colorPixel = ((argb >> 16u) & 0xffu) | (argb & 0x0000ff00u) | ((argb & 0xffu) << 16u);
	}
	float z = 0.0f;
	std::memcpy(&z, &zBits, sizeof(z));
	const float clampedZ = std::max(0.0f, std::min(1.0f, z));
	const uint32_t zPixel = depth.bpp == 16u ? static_cast<uint32_t>(clampedZ * 65535.0f + 0.5f)
		: static_cast<uint32_t>(clampedZ * 4294967295.0f);
	for (uint32_t i = 0; i < rectsToClear; ++i) {
		int32_t left, top, right, bottom;
		rectAt(i, left, top, right, bottom);
		if (right < 0) right = static_cast<int32_t>(target.width);
		if (bottom < 0) bottom = static_cast<int32_t>(target.height);
		if (flags & kD3dclearTarget) writePixels(target, left, top, right, bottom, colorPixel);
		if (flags & kD3dclearZBuffer) writePixels(depth, left, top, right, bottom, zPixel);
	}
	if (flags & kD3dclearTarget) markSurfaceCpuWrite(target.handle);
	if (flags & kD3dclearZBuffer) markSurfaceCpuWrite(depth.handle);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-d3d7] Clear flags=%02x color=%08x rects=%u target=%08x z=%08x\n",
			flags, argb, rectCount, target.handle, depth.handle);
	}
}

bool createGuestSurface(uint32_t width, uint32_t height, uint32_t bpp, uint32_t caps,
	uint32_t pixelFlags, uint32_t redMask, uint32_t greenMask, uint32_t blueMask,
	uint32_t alphaMask, uint32_t& handle, std::string& error)
{
	if (!width || !height || width > 8192u || height > 8192u ||
		(bpp != 8u && bpp != 16u && bpp != 24u && bpp != 32u)) return false;
	const uint32_t bytesPerPixel = (bpp + 7u) / 8u;
	const uint64_t rawPitch = uint64_t(width) * bytesPerPixel;
	const uint64_t pitch = (rawPitch + 3u) & ~uint64_t(3u);
	const uint64_t byteCount = pitch * height;
	if (byteCount > 512ull * 1024ull * 1024ull || pitch > UINT32_MAX) return false;
	const uint32_t byteSize = static_cast<uint32_t>(byteCount);
	const uint32_t mappedSize = static_cast<uint32_t>((byteCount + 0xfffu) & ~uint64_t(0xfffu));
	const uint32_t object = gNextDdrawSurface;
	const uint32_t pixels = gNextDdrawPixels;
	if (!sharedGuestAddressSpace().commitRange(object, 0x20u, error) ||
		!sharedGuestAddressSpace().commitRange(pixels, mappedSize, error)) return false;
	std::memset(sharedGuestAddressSpace().host(object, 0x20u), 0, 0x20u);
	std::memset(sharedGuestAddressSpace().host(pixels, byteSize), 0, byteSize);
	guest::store<uint32_t>(object, kDdrawSurfaceVtable);
	guest::store<uint32_t>(object + 4u, 1u);
	GuestDdrawSurface surface{};
	surface.handle = object;
	surface.width = width;
	surface.height = height;
	surface.pitch = static_cast<uint32_t>(pitch);
	surface.bpp = bpp;
	surface.caps = caps;
	surface.pixelFlags = pixelFlags;
	surface.redMask = redMask;
	surface.greenMask = greenMask;
	surface.blueMask = blueMask;
	surface.alphaMask = alphaMask;
	surface.pixels = pixels;
	surface.bytes = byteSize;
	{
		std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
		gDdrawSurfaces.emplace(object, surface);
		if (caps & 0x00000200u) gGuestPrimarySurface = object; // DDSCAPS_PRIMARYSURFACE.
	}
	gNextDdrawSurface += 0x20u;
	gNextDdrawPixels += mappedSize;
	handle = object;
	return true;
}

bool loadGuestSurface(uint32_t handle, GuestDdrawSurface& surface)
{
	std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
	const auto found = gDdrawSurfaces.find(handle);
	if (found == gDdrawSurfaces.end()) return false;
	surface = found->second;
	return true;
}

d3d7::GpuSurface gpuSurfaceView(const GuestDdrawSurface& surface)
{
	d3d7::GpuSurface view{};
	view.handle = surface.handle;
	view.width = surface.width;
	view.height = surface.height;
	view.pitch = surface.pitch;
	view.bitsPerPixel = surface.bpp;
	view.redMask = surface.redMask;
	view.greenMask = surface.greenMask;
	view.blueMask = surface.blueMask;
	view.alphaMask = surface.alphaMask;
	view.cpuRevision = surface.cpuRevision;
	view.pixels = sharedGuestAddressSpace().host(surface.pixels, surface.bytes);
	view.byteCount = surface.bytes;
	return view;
}

void synchronizeSurfaceForCpu(uint32_t handle)
{
	GuestDdrawSurface surface{};
	if (!loadGuestSurface(handle, surface))
		throw std::runtime_error("D3D7 GPU synchronization referenced an invalid DirectDraw surface");
	const auto view = gpuSurfaceView(surface);
	if (!view.pixels || !d3d7::synchronizeGpuSurface(view))
		throw std::runtime_error("D3D7 GPU surface readback failed; refusing stale guest pixels");
}

void markSurfaceCpuWrite(uint32_t handle)
{
	{
		std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
		auto found = gDdrawSurfaces.find(handle);
		if (found == gDdrawSurfaces.end()) return;
		++found->second.cpuRevision;
		if (!found->second.cpuRevision) found->second.cpuRevision = 1u;
	}
	d3d7::markGpuSurfaceCpuWrite(handle);
}

void writeGuestPixelFormat(uint32_t address, const GuestDdrawSurface& surface)
{
	std::memset(sharedGuestAddressSpace().host(address, 32u), 0, 32u);
	guest::store<uint32_t>(address, 32u);
	guest::store<uint32_t>(address + 4u, surface.pixelFlags);
	guest::store<uint32_t>(address + 12u, surface.bpp);
	if (surface.pixelFlags & 0x00000400u) {
		guest::store<uint32_t>(address + 20u, surface.bpp == 16u ? 0x0000ffffu : 0xffffffffu);
	} else {
		guest::store<uint32_t>(address + 16u, surface.redMask);
		guest::store<uint32_t>(address + 20u, surface.greenMask);
		guest::store<uint32_t>(address + 24u, surface.blueMask);
		guest::store<uint32_t>(address + 28u, surface.alphaMask);
	}
}

void writeGuestSurfaceDesc(uint32_t address, const GuestDdrawSurface& surface)
{
	const uint32_t size = guest::load<uint32_t>(address);
	if (size < 108u || size > 4096u || !sharedGuestAddressSpace().host(address, size)) return;
	std::memset(sharedGuestAddressSpace().host(address + 4u, size - 4u), 0, size - 4u);
	guest::store<uint32_t>(address + 4u, 0x0000180fu); // CAPS | HEIGHT | WIDTH | PITCH | LPSURFACE | PIXELFORMAT.
	guest::store<uint32_t>(address + 8u, surface.height);
	guest::store<uint32_t>(address + 12u, surface.width);
	guest::store<uint32_t>(address + 16u, surface.pitch);
	guest::store<uint32_t>(address + 36u, surface.pixels);
	writeGuestPixelFormat(address + 72u, surface);
	guest::store<uint32_t>(address + 104u, surface.caps);
}

void ddrawCreateSurface(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDdsdCaps = 0x00000001u;
	constexpr uint32_t kDdsdHeight = 0x00000002u;
	constexpr uint32_t kDdsdWidth = 0x00000004u;
	constexpr uint32_t kDdsdPixelFormat = 0x00001000u;
	constexpr uint32_t kDdsdBackBufferCount = 0x00000020u;
	constexpr uint32_t kDdsCapsPrimary = 0x00000200u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t desc = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2);
	const uint32_t outer = argument(cpu, 3);
	if (self != kDdrawObject || !desc || !output || outer) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS.
		return;
	}
	const uint32_t size = guest::load<uint32_t>(desc);
	const uint32_t descFlags = guest::load<uint32_t>(desc + 4u);
	if (size < 108u || size > 4096u || !sharedGuestAddressSpace().host(desc, size) ||
		!(descFlags & kDdsdCaps)) {
		cpu->r[EAX] = 0x80070057u;
		return;
	}
	const uint32_t caps = guest::load<uint32_t>(desc + 104u);
	const bool primary = (caps & kDdsCapsPrimary) != 0u;
	uint32_t width = guest::load<uint32_t>(desc + 12u);
	uint32_t height = guest::load<uint32_t>(desc + 8u);
	uint32_t bpp = 0u;
	{
		std::lock_guard<std::mutex> lock(gDisplayModeMutex);
		if (primary) {
			width = gCurrentGuestDisplayMode[0];
			height = gCurrentGuestDisplayMode[1];
			bpp = gCurrentGuestDisplayMode[2];
		}
	}
	const uint32_t pf = desc + 72u;
	uint32_t pixelFlags = (descFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(pf + 4u) : 0u;
	if (!bpp) bpp = (descFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(pf + 12u) : 16u;
	if (!(descFlags & kDdsdWidth) && !primary) width = 0u;
	if (!(descFlags & kDdsdHeight) && !primary) height = 0u;
	uint32_t redMask = (descFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(pf + 16u) : 0u;
	uint32_t greenMask = (descFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(pf + 20u) : 0u;
	uint32_t blueMask = (descFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(pf + 24u) : 0u;
	uint32_t alphaMask = (descFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(pf + 28u) : 0u;
	if (primary && !(descFlags & kDdsdPixelFormat)) {
		pixelFlags = 0x00000040u; // DDPF_RGB.
		if (bpp == 16u) { redMask = 0x0000f800u; greenMask = 0x000007e0u; blueMask = 0x0000001fu; }
		else if (bpp == 32u) { redMask = 0x00ff0000u; greenMask = 0x0000ff00u; blueMask = 0x000000ffu; }
	}
	std::string error;
	uint32_t surface = 0u;
	if (!createGuestSurface(width, height, bpp, caps, pixelFlags, redMask, greenMask,
		blueMask, alphaMask, surface, error)) {
		cpu->r[EAX] = (width && height) ? 0x8007000eu : 0x80070057u; // DDERR_OUTOFVIDEOMEMORY / invalid desc.
		return;
	}
	if (primary && (descFlags & kDdsdBackBufferCount) && guest::load<uint32_t>(desc + 20u) > 0u) {
		uint32_t backBuffer = 0u;
		if (createGuestSurface(width, height, bpp, 0x00000004u | 0x00000800u,
			pixelFlags, redMask, greenMask, blueMask, alphaMask, backBuffer, error)) {
			std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
			gDdrawSurfaces[surface].backBuffer = backBuffer;
		}
	}
	guest::store<uint32_t>(output, surface);
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] CreateSurface desc_flags=%08x caps=%08x mode=%ux%ux%u primary=%u -> %08x\n",
			descFlags, caps, width, height, bpp, primary ? 1u : 0u, surface);
	}
	cpu->r[EAX] = 0u; // DD_OK.
}

void ddrawSurfaceAddRef(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
	auto found = gDdrawSurfaces.find(self);
	if (found == gDdrawSurfaces.end()) { cpu->r[EAX] = 0u; return; }
	cpu->r[EAX] = ++found->second.references;
	guest::store<uint32_t>(self + 4u, found->second.references);
}

void ddrawSurfaceRelease(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
	auto found = gDdrawSurfaces.find(self);
	if (found == gDdrawSurfaces.end()) { cpu->r[EAX] = 0u; return; }
	const uint32_t count = found->second.references ? --found->second.references : 0u;
	guest::store<uint32_t>(self + 4u, count);
	if (!count) {
		if (gGuestPrimarySurface == self) gGuestPrimarySurface = 0u;
		gDdrawSurfaces.erase(found);
	}
	cpu->r[EAX] = count;
}

void ddrawSurfaceFlip(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidParams = 0x80070057u;
	constexpr uint32_t kDderrInvalidObject = 0x88760064u;
	constexpr uint32_t kDderrNotFlippable = 0x88760076u;
	constexpr uint32_t kDderrSurfaceLost = 0x887601c2u;
	constexpr uint32_t kDdsCapsPrimary = 0x00000200u;
	constexpr uint32_t kDdsCapsFlip = 0x00000010u;
	constexpr uint32_t kDdsCapsBackBuffer = 0x00000004u;
	constexpr uint32_t kDdfFlipWait = 0x00000001u;
	constexpr uint32_t kDdfFlipNoVsync = 0x00000008u;
	constexpr uint32_t kDdfFlipDoNotWait = 0x00000020u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t overrideSurface = argument(cpu, 1);
	const uint32_t flags = argument(cpu, 2);
	if (flags & ~(kDdfFlipWait | kDdfFlipNoVsync | kDdfFlipDoNotWait) ||
		((flags & kDdfFlipWait) && (flags & kDdfFlipDoNotWait))) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	GuestDdrawSurface preFlipFront{};
	if (loadGuestSurface(self, preFlipFront)) {
		synchronizeSurfaceForCpu(self);
		if (preFlipFront.backBuffer) synchronizeSurfaceForCpu(preFlipFront.backBuffer);
	}
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] Surface::Flip self=%08x override=%08x flags=%08x caller=%08x\n",
			self, overrideSurface, flags, guest::load<uint32_t>(cpu->r[ESP]));
	}
	uint32_t frontPixels = 0u;
	uint32_t frontWidth = 0u;
	uint32_t frontHeight = 0u;
	uint32_t frontPitch = 0u;
	uint32_t frontBpp = 0u;
	uint32_t backHandle = 0u;
	uint32_t redMask = 0u, greenMask = 0u, blueMask = 0u;
	uint64_t presentation = 0u;
	{
		std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
		auto front = gDdrawSurfaces.find(self);
		if (front == gDdrawSurfaces.end()) { cpu->r[EAX] = kDderrInvalidObject; return; }
		if (self != gGuestPrimarySurface || !(front->second.caps & kDdsCapsPrimary) ||
			!(front->second.caps & kDdsCapsFlip) || !front->second.backBuffer) {
			cpu->r[EAX] = kDderrNotFlippable;
			return;
		}
		backHandle = front->second.backBuffer;
		if (overrideSurface && overrideSurface != backHandle) {
			cpu->r[EAX] = kDderrNotFlippable;
			return;
		}
		auto back = gDdrawSurfaces.find(backHandle);
		if (back == gDdrawSurfaces.end() || !(back->second.caps & kDdsCapsBackBuffer) ||
			front->second.width != back->second.width || front->second.height != back->second.height ||
			front->second.pitch != back->second.pitch || front->second.bpp != back->second.bpp ||
			front->second.bytes != back->second.bytes) {
			cpu->r[EAX] = kDderrNotFlippable;
			return;
		}
		{
			std::lock_guard<std::mutex> cooperativeLock(gDdrawCooperativeMutex);
			if (gDdrawCooperativeState.lost) { cpu->r[EAX] = kDderrSurfaceLost; return; }
		}
		// DirectDraw flips the backing memory between front and back surface
		// interfaces. Keep COM identities and the attachment edge stable so the
		// D3D render-target object continues to address the new back buffer.
		std::swap(front->second.pixels, back->second.pixels);
		frontPixels = front->second.pixels;
		frontWidth = front->second.width;
		frontHeight = front->second.height;
		frontPitch = front->second.pitch;
		frontBpp = front->second.bpp;
		redMask = front->second.redMask;
		greenMask = front->second.greenMask;
		blueMask = front->second.blueMask;
		presentation = ++gGuestPresentationCount;
	}
	markSurfaceCpuWrite(self);
	markSurfaceCpuWrite(backHandle);
	const bool traceFlip = std::getenv("BR_TRACE_DDRAW") || std::getenv("BR_TRACE_D3D7");
	uint32_t hash = 0u;
	const uint32_t rowBytes = frontPitch;
	const auto* pixels = sharedGuestAddressSpace().host(frontPixels, rowBytes * frontHeight);
	if (!pixels) { cpu->r[EAX] = kDderrInvalidObject; return; }
	Win32FramePresenter presenter = nullptr;
	{
		std::lock_guard<std::mutex> lock(gFramePresenterMutex);
		presenter = gFramePresenter;
	}
#if defined(__ANDROID__)
	gProfileDdrawFlips.fetch_add(1u, std::memory_order_relaxed);
#endif
	if (presenter)
		presenter(pixels, frontWidth, frontHeight, frontPitch, frontBpp,
			redMask, greenMask, blueMask);
	if (traceFlip) {
		hash = 2166136261u;
		for (uint32_t i = 0u; i < rowBytes * frontHeight; ++i)
			hash = (hash ^ pixels[i]) * 16777619u;
	}
	const uint32_t fragmentsBytes = (frontBpp + 7u) / 8u;
	if (const char* capturePath = std::getenv("BR_CAPTURE_FRAMEBUFFER")) {
		const std::string path(capturePath);
		const char* requestedPresentation = std::getenv("BR_CAPTURE_FRAMEBUFFER_PRESENTATION");
		const char* requestedPresentations = std::getenv("BR_CAPTURE_FRAMEBUFFER_PRESENTATIONS");
		uint64_t requestedIndex = presentation;
		bool captureThisPresentation = true;
		bool captureToBasePath = true;
		if (requestedPresentations && *requestedPresentations) {
			captureThisPresentation = false;
			uint64_t lastRequestedIndex = 0u;
			const char* cursor = requestedPresentations;
			while (*cursor) {
				char* end = nullptr;
				const unsigned long long parsed = std::strtoull(cursor, &end, 10);
				if (end == cursor || (*end && *end != ',')) break;
				lastRequestedIndex = static_cast<uint64_t>(parsed);
				if (lastRequestedIndex == presentation) captureThisPresentation = true;
				cursor = *end == ',' ? end + 1u : end;
			}
			captureToBasePath = presentation == lastRequestedIndex;
		} else if (requestedPresentation) {
			requestedIndex = std::strtoull(requestedPresentation, nullptr, 10);
			captureThisPresentation = requestedIndex == presentation;
		}
		const bool onE = path.size() >= 3u && (path[0] == 'E' || path[0] == 'e') &&
			path[1] == ':' && (path[2] == '\\' || path[2] == '/');
		if (onE && captureThisPresentation && fragmentsBytes && redMask && greenMask && blueMask) {
			std::string outputPath = path;
			if (requestedPresentations && *requestedPresentations && !captureToBasePath) {
				const size_t extension = outputPath.find_last_of('.');
				outputPath.insert(extension == std::string::npos ? outputPath.size() : extension,
					"_p" + std::to_string(presentation));
			}
			std::ofstream image(outputPath, std::ios::binary | std::ios::trunc);
			if (image) {
				image << "P6\n" << frontWidth << ' ' << frontHeight << "\n255\n";
				std::vector<char> row(size_t(frontWidth) * 3u);
				for (uint32_t y = 0; y < frontHeight; ++y) {
					for (uint32_t x = 0; x < frontWidth; ++x) {
						const size_t offset = size_t(y) * frontPitch + size_t(x) * fragmentsBytes;
						uint32_t packed = 0u;
						for (uint32_t b = 0; b < fragmentsBytes; ++b)
							packed |= uint32_t(pixels[offset + b]) << (8u * b);
						const auto channel = [packed](uint32_t mask) {
							uint32_t shift = 0u;
							while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
							const uint32_t maxValue = shift < 32u ? mask >> shift : 0u;
							const uint32_t value = maxValue ? (packed & mask) >> shift : 0u;
							return static_cast<uint8_t>(maxValue ? (value * 255u + maxValue / 2u) / maxValue : 0u);
						};
						row[size_t(x) * 3u + 0u] = static_cast<char>(channel(redMask));
						row[size_t(x) * 3u + 1u] = static_cast<char>(channel(greenMask));
						row[size_t(x) * 3u + 2u] = static_cast<char>(channel(blueMask));
					}
					image.write(row.data(), static_cast<std::streamsize>(row.size()));
				}
			}
		}
	}
	cpu->r[EAX] = 0u;
	if (traceFlip) {
		std::fprintf(stderr, "[br-ddraw] Surface::Flip presented=%llu front=%08x back=%08x dimensions=%ux%ux%u bytes=%u hash=%08x result=DD_OK\n",
			static_cast<unsigned long long>(presentation), self, backHandle,
			frontWidth, frontHeight, frontBpp, rowBytes * frontHeight, hash);
	}
}

void ddrawSurfaceGetDdInterface(CPU* cpu, const ApiImportStub&)
{
	const uint32_t self = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 4u)) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS.
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
		if (gDdrawSurfaces.find(self) == gDdrawSurfaces.end()) {
			cpu->r[EAX] = 0x80070057u;
			return;
		}
	}
	const uint32_t references = guest::load<uint32_t>(kDdrawObject + 4u) + 1u;
	guest::store<uint32_t>(kDdrawObject + 4u, references);
	guest::store<uint32_t>(output, kDdrawObject);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-ddraw] Surface::GetDDInterface surface=%08x -> %08x refs=%u\n",
			self, kDdrawObject, references);
}

void ddrawSurfaceAddAttached(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDdsCapsZBuffer = 0x00020000u;
	constexpr uint32_t kDderrInvalidParams = 0x80070057u;
	constexpr uint32_t kDderrCannotAttachSurface = 0x8876000au; // MAKE_DDHRESULT(10).
	constexpr uint32_t kDderrSurfaceAlreadyAttached = 0x8876019au; // MAKE_DDHRESULT(410).
	const uint32_t self = argument(cpu, 0);
	const uint32_t attachedHandle = argument(cpu, 1);
	std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
	auto parent = gDdrawSurfaces.find(self);
	auto attached = gDdrawSurfaces.find(attachedHandle);
	if (parent == gDdrawSurfaces.end() || attached == gDdrawSurfaces.end() || self == attachedHandle) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	if ((attached->second.caps & kDdsCapsZBuffer) == 0u) {
		cpu->r[EAX] = kDderrCannotAttachSurface;
		return;
	}
	if (parent->second.zBuffer == attachedHandle) {
		cpu->r[EAX] = kDderrSurfaceAlreadyAttached;
		return;
	}
	if (parent->second.zBuffer != 0u) {
		cpu->r[EAX] = kDderrCannotAttachSurface;
		return;
	}
	parent->second.zBuffer = attachedHandle;
	++attached->second.references;
	guest::store<uint32_t>(attachedHandle + 4u, attached->second.references);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] AddAttachedSurface zbuffer parent=%08x child=%08x refs=%u\n",
			self, attachedHandle, attached->second.references);
	}
}

void ddrawSurfaceDeleteAttached(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidParams = 0x80070057u;
	constexpr uint32_t kDderrSurfaceNotAttached = 0x887601ccu; // MAKE_DDHRESULT(460).
	const uint32_t self = argument(cpu, 0);
	const uint32_t flags = argument(cpu, 1);
	const uint32_t attachedHandle = argument(cpu, 2);
	std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
	auto parent = gDdrawSurfaces.find(self);
	if (parent == gDdrawSurfaces.end() || flags != 0u) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	if (parent->second.zBuffer != attachedHandle) {
		cpu->r[EAX] = kDderrSurfaceNotAttached;
		return;
	}
	parent->second.zBuffer = 0u;
	auto attached = gDdrawSurfaces.find(attachedHandle);
	if (attached != gDdrawSurfaces.end()) {
		if (attached->second.references != 0u) --attached->second.references;
		guest::store<uint32_t>(attachedHandle + 4u, attached->second.references);
	}
	cpu->r[EAX] = 0u;
}

void ddrawSurfaceGetCaps(CPU* cpu, const ApiImportStub&)
{
	GuestDdrawSurface surface{};
	const uint32_t output = argument(cpu, 1);
	if (!loadGuestSurface(argument(cpu, 0), surface) || !output ||
		!sharedGuestAddressSpace().host(output, 16u)) { cpu->r[EAX] = 0x80070057u; return; }
	std::memset(sharedGuestAddressSpace().host(output, 16u), 0, 16u);
	guest::store<uint32_t>(output, surface.caps);
	cpu->r[EAX] = 0u;
}

void ddrawSurfaceGetPixelFormat(CPU* cpu, const ApiImportStub&)
{
	GuestDdrawSurface surface{};
	const uint32_t output = argument(cpu, 1);
	if (!loadGuestSurface(argument(cpu, 0), surface) || !output ||
		!sharedGuestAddressSpace().host(output, 32u)) { cpu->r[EAX] = 0x80070057u; return; }
	writeGuestPixelFormat(output, surface);
	cpu->r[EAX] = 0u;
}

void ddrawSurfaceGetDesc(CPU* cpu, const ApiImportStub&)
{
	GuestDdrawSurface surface{};
	const uint32_t output = argument(cpu, 1);
	if (!loadGuestSurface(argument(cpu, 0), surface) || !output) { cpu->r[EAX] = 0x80070057u; return; }
	writeGuestSurfaceDesc(output, surface);
	cpu->r[EAX] = 0u;
}

void ddrawSurfaceLock(CPU* cpu, const ApiImportStub&)
{
	GuestDdrawSurface surface{};
	const uint32_t output = argument(cpu, 2);
	if (!loadGuestSurface(argument(cpu, 0), surface) || !output) { cpu->r[EAX] = 0x80070057u; return; }
	synchronizeSurfaceForCpu(surface.handle);
	writeGuestSurfaceDesc(output, surface);
	cpu->r[EAX] = 0u;
}

void ddrawSurfaceUnlock(CPU* cpu, const ApiImportStub&)
{
	GuestDdrawSurface surface{};
	if (!loadGuestSurface(argument(cpu, 0), surface)) { cpu->r[EAX] = 0x88760064u; return; }
	markSurfaceCpuWrite(surface.handle);
	cpu->r[EAX] = 0u;
}

void ddrawSurfaceBlt(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidRect = 0x88760096u; // MAKE_DDHRESULT(150).
	constexpr uint32_t kDderrUnsupported = 0x80004001u; // DDERR_UNSUPPORTED / E_NOTIMPL.
	constexpr uint32_t kDderrInvalidParams = 0x80070057u;
	constexpr uint32_t kDderrOutOfMemory = 0x8007000eu;
	constexpr uint32_t kDdbitWait = 0x01000000u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t destinationRectAddress = argument(cpu, 1);
	const uint32_t sourceHandle = argument(cpu, 2);
	const uint32_t sourceRectAddress = argument(cpu, 3);
	const uint32_t flags = argument(cpu, 4);
	const uint32_t effects = argument(cpu, 5);
	GuestDdrawSurface destination{}, source{};
	if (!loadGuestSurface(self, destination) || !loadGuestSurface(sourceHandle, source)) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	// DDBLT_WAIT is safe to honor because this guest implementation performs
	// the copy synchronously. Other effects/ROP/key flags require semantics we
	// have not observed or implemented, so fail explicitly.
	if ((flags & ~kDdbitWait) || effects) {
		cpu->r[EAX] = kDderrUnsupported;
		return;
	}
	synchronizeSurfaceForCpu(sourceHandle);
	if (self != sourceHandle) synchronizeSurfaceForCpu(self);
	if ((destination.bpp != 16u && destination.bpp != 32u) ||
		(source.bpp != 16u && source.bpp != 32u) || !destination.pitch || !source.pitch) {
		cpu->r[EAX] = kDderrUnsupported;
		return;
	}
	struct Rect { int32_t left, top, right, bottom; };
	auto readRect = [](uint32_t address, const GuestDdrawSurface& surface, Rect& rect) {
		if (!address) {
			rect = {0, 0, static_cast<int32_t>(surface.width), static_cast<int32_t>(surface.height)};
			return true;
		}
		if (!sharedGuestAddressSpace().host(address, 16u)) return false;
		rect = {guest::load<int32_t>(address), guest::load<int32_t>(address + 4u),
			guest::load<int32_t>(address + 8u), guest::load<int32_t>(address + 12u)};
		return true;
	};
	Rect destinationRect{}, sourceRect{};
	if (!readRect(destinationRectAddress, destination, destinationRect) ||
		!readRect(sourceRectAddress, source, sourceRect)) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	const auto validRect = [](const Rect& rect, const GuestDdrawSurface& surface) {
		return rect.left >= 0 && rect.top >= 0 && rect.right > rect.left && rect.bottom > rect.top &&
			uint32_t(rect.right) <= surface.width && uint32_t(rect.bottom) <= surface.height;
	};
	if (!validRect(destinationRect, destination) || !validRect(sourceRect, source)) {
		cpu->r[EAX] = kDderrInvalidRect;
		return;
	}
	const uint32_t destinationPixelBytes = destination.bpp / 8u;
	const uint32_t sourcePixelBytes = source.bpp / 8u;
	const uint32_t destinationWidth = uint32_t(destinationRect.right - destinationRect.left);
	const uint32_t destinationHeight = uint32_t(destinationRect.bottom - destinationRect.top);
	const uint32_t sourceWidth = uint32_t(sourceRect.right - sourceRect.left);
	const uint32_t sourceHeight = uint32_t(sourceRect.bottom - sourceRect.top);
	const uint64_t sourceSpan = uint64_t(sourceWidth) * sourceHeight * sourcePixelBytes;
	const uint64_t destinationSpan = uint64_t(destinationWidth) * destinationHeight * destinationPixelBytes;
	const auto* sourcePixels = sharedGuestAddressSpace().host(source.pixels, source.bytes);
	auto* destinationPixels = sharedGuestAddressSpace().host(destination.pixels, destination.bytes);
	if (!sourcePixels || !destinationPixels || sourceSpan > 64ull * 1024ull * 1024ull ||
		destinationSpan > 64ull * 1024ull * 1024ull) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	std::vector<uint8_t> overlapSnapshot;
	if (self == sourceHandle) {
		try { overlapSnapshot.resize(static_cast<size_t>(sourceSpan)); }
		catch (const std::bad_alloc&) { cpu->r[EAX] = kDderrOutOfMemory; return; }
		for (uint32_t y = 0; y < sourceHeight; ++y) {
			const auto* row = sourcePixels + size_t(sourceRect.top + int32_t(y)) * source.pitch +
				size_t(sourceRect.left) * sourcePixelBytes;
			std::memcpy(overlapSnapshot.data() + size_t(y) * sourceWidth * sourcePixelBytes,
				row, size_t(sourceWidth) * sourcePixelBytes);
		}
	}
	const auto readPixel = [&](uint32_t x, uint32_t y) {
		const uint8_t* pixel = self == sourceHandle
			? overlapSnapshot.data() + (size_t(y) * sourceWidth + x) * sourcePixelBytes
			: sourcePixels + size_t(sourceRect.top + int32_t(y)) * source.pitch +
				size_t(sourceRect.left + int32_t(x)) * sourcePixelBytes;
		uint32_t value = 0u;
		std::memcpy(&value, pixel, sourcePixelBytes);
		return value;
	};
	const auto channel8 = [](uint32_t value, uint32_t mask, uint32_t fallback) {
		if (!mask) return fallback;
		uint32_t shift = 0u;
		while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
		const uint32_t maximum = mask >> shift;
		return maximum ? ((((value & mask) >> shift) * 255u + maximum / 2u) / maximum) : fallback;
	};
	const auto packChannel = [](uint32_t channel, uint32_t mask) {
		if (!mask) return 0u;
		uint32_t shift = 0u;
		while (shift < 32u && ((mask >> shift) & 1u) == 0u) ++shift;
		const uint32_t maximum = mask >> shift;
		return maximum ? (((channel * maximum + 127u) / 255u) << shift) & mask : 0u;
	};
	const bool sameFormat = source.bpp == destination.bpp && source.redMask == destination.redMask &&
		source.greenMask == destination.greenMask && source.blueMask == destination.blueMask &&
		source.alphaMask == destination.alphaMask;
	for (uint32_t y = 0; y < destinationHeight; ++y) {
		const uint32_t sourceY = uint32_t(uint64_t(y) * sourceHeight / destinationHeight);
		auto* output = destinationPixels + size_t(destinationRect.top + int32_t(y)) * destination.pitch +
			size_t(destinationRect.left) * destinationPixelBytes;
		for (uint32_t x = 0; x < destinationWidth; ++x) {
			const uint32_t sourceX = uint32_t(uint64_t(x) * sourceWidth / destinationWidth);
			uint32_t pixel = readPixel(sourceX, sourceY);
			if (!sameFormat) {
				pixel = packChannel(channel8(pixel, source.redMask, 0u), destination.redMask) |
					packChannel(channel8(pixel, source.greenMask, 0u), destination.greenMask) |
					packChannel(channel8(pixel, source.blueMask, 0u), destination.blueMask) |
					packChannel(channel8(pixel, source.alphaMask, 255u), destination.alphaMask);
			}
			std::memcpy(output + size_t(x) * destinationPixelBytes, &pixel, destinationPixelBytes);
		}
	}
	markSurfaceCpuWrite(self);
	cpu->r[EAX] = 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-ddraw] Blt dst=%08x rect=%d,%d-%d,%d src=%08x rect=%d,%d-%d,%d flags=%08x formats=%u->%u result=DD_OK\n",
			self, destinationRect.left, destinationRect.top, destinationRect.right, destinationRect.bottom,
			sourceHandle, sourceRect.left, sourceRect.top, sourceRect.right, sourceRect.bottom,
			flags, source.bpp, destination.bpp);
}

void ddrawSurfaceGetAttachedSurface(CPU* cpu, const ApiImportStub&)
{
	GuestDdrawSurface surface{};
	constexpr uint32_t kDdsCapsZBuffer = 0x00020000u;
	constexpr uint32_t kDdsCapsBackBuffer = 0x00000004u;
	const uint32_t capsAddress = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2);
	const uint32_t requestedCaps = capsAddress && sharedGuestAddressSpace().host(capsAddress, 4u)
		? guest::load<uint32_t>(capsAddress) : 0u;
	if (!loadGuestSurface(argument(cpu, 0), surface) || !capsAddress || !output) {
		cpu->r[EAX] = 0x80070057u;
		return;
	}
	const uint32_t selected = (requestedCaps & kDdsCapsZBuffer) ? surface.zBuffer
		: (requestedCaps & kDdsCapsBackBuffer) ? surface.backBuffer : 0u;
	if (!selected) {
		cpu->r[EAX] = 0x88760082u; // DDERR_NOTFOUND / invalid attached surface request.
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
		auto found = gDdrawSurfaces.find(selected);
		if (found == gDdrawSurfaces.end()) { cpu->r[EAX] = 0x88760082u; return; }
		++found->second.references;
		guest::store<uint32_t>(selected + 4u, found->second.references);
	}
	guest::store<uint32_t>(output, selected);
	cpu->r[EAX] = 0u;
}

void ddrawSetCooperativeLevel(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDdsclFullscreen = 0x00000001u;
	constexpr uint32_t kDdsclNormal = 0x00000008u;
	constexpr uint32_t kDdsclExclusive = 0x00000010u;
	constexpr uint32_t kDderrInvalidObject = 0x88760064u;
	constexpr uint32_t kDderrInvalidParams = 0x80070057u;
	const uint32_t self = argument(cpu, 0);
	const uint32_t window = argument(cpu, 1);
	const uint32_t flags = argument(cpu, 2);
	if (std::getenv("BR_TRACE_DDRAW")) {
		const uint32_t returnAddress = guest::load<uint32_t>(cpu->r[ESP]);
		std::fprintf(stderr, "[br-ddraw] SetCooperativeLevel self=%08x hwnd=%08x flags=%08x caller=%08x\n",
			self, window, flags, returnAddress);
	}
	if (self != kDdrawObject || guest::load<uint32_t>(self) != kDdrawVtable) {
		cpu->r[EAX] = kDderrInvalidObject;
		return;
	}
	const bool exclusive = (flags & kDdsclExclusive) != 0u;
	if (!window || !(flags & (kDdsclNormal | kDdsclExclusive)) ||
		(exclusive && !(flags & kDdsclFullscreen)) ||
		((flags & kDdsclNormal) && exclusive)) {
		cpu->r[EAX] = kDderrInvalidParams;
		return;
	}
	bool applicationActive = false;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		if (!gGuestWindows.count(window)) {
			cpu->r[EAX] = kDderrInvalidParams;
			return;
		}
		applicationActive = gGuestApplicationActive;
	}
	{
		std::lock_guard<std::mutex> lock(gDdrawCooperativeMutex);
		gDdrawCooperativeState.window = window;
		gDdrawCooperativeState.flags = flags;
		gDdrawCooperativeState.lost = exclusive && !applicationActive;
	}
	cpu->r[EAX] = 0u;
}

void ddrawTestCooperativeLevel(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDderrInvalidObject = 0x88760064u;
	constexpr uint32_t kDderrNoExclusiveMode = 0x887600e1u;
	const uint32_t self = argument(cpu, 0);
	if (self != kDdrawObject || guest::load<uint32_t>(self) != kDdrawVtable) {
		cpu->r[EAX] = kDderrInvalidObject;
		return;
	}
	uint32_t window = 0u;
	uint32_t flags = 0u;
	bool lost = false;
	{
		std::lock_guard<std::mutex> lock(gDdrawCooperativeMutex);
		window = gDdrawCooperativeState.window;
		flags = gDdrawCooperativeState.flags;
		lost = gDdrawCooperativeState.lost;
	}
	// Wine models this query as DD_OK unless an exclusive device has actually
	// been lost. Track that state from delivered WM_ACTIVATEAPP events instead
	// of inferring it from surface creation or a successful SetDisplayMode.
	const uint32_t result = lost ? kDderrNoExclusiveMode : 0u;
	if (std::getenv("BR_TRACE_DDRAW"))
		std::fprintf(stderr, "[br-ddraw] TestCooperativeLevel self=%08x hwnd=%08x flags=%08x lost=%u result=%08x\n",
			self, window, flags, lost ? 1u : 0u, result);
	cpu->r[EAX] = result;
}

void ddrawGetCaps(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDriverCaps = 0x00000040u | 0x00000200u | 0x00400000u | 0x04000000u;
	// The 32-bit Windows DirectDraw probe on this host reports CANRENDERWINDOWED
	// in dwCaps2 (0x00080000). Battle Realms gates its windowed-mode candidate
	// on this exact bit before accepting the enumerated mode list.
	constexpr uint32_t kDriverCaps2 = 0x00080000u;
	constexpr uint32_t kSurfaceCaps = 0x00000200u | 0x00000040u | 0x00000010u |
		0x00000800u | 0x00004000u;
	for (uint32_t index = 1u; index <= 2u; ++index) {
		const uint32_t caps = argument(cpu, index);
		if (!caps) continue;
		const uint32_t size = guest::load<uint32_t>(caps);
		if (size < 108u || size > 4096u || !sharedGuestAddressSpace().host(caps, size)) {
			cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS
			return;
		}
		std::memset(sharedGuestAddressSpace().host(caps + 4u, size - 4u), 0, size - 4u);
		guest::store<uint32_t>(caps + 4u, kDriverCaps);
		guest::store<uint32_t>(caps + 8u, kDriverCaps2);
		guest::store<uint32_t>(caps + 12u, 0x00000200u); // source-blit color key
		guest::store<uint32_t>(caps + 24u, 0x00000044u); // 8-bit palette and primary surface
		guest::store<uint32_t>(caps + 60u, 256u << 20u);
		guest::store<uint32_t>(caps + 64u, 192u << 20u);
		guest::store<uint32_t>(caps + 104u, kSurfaceCaps);
	}
	cpu->r[EAX] = 0u;
}

void ddrawEnumDisplayModes(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kModeDesc = 0x6c410000u;
	constexpr uint32_t kDescSize = 124u;
	constexpr uint32_t kDdsdHeight = 0x00000002u;
	constexpr uint32_t kDdsdWidth = 0x00000004u;
	constexpr uint32_t kDdsdPixelFormat = 0x00001000u;
	constexpr uint32_t kDdsdRefreshRate = 0x00040000u;
	constexpr uint32_t kDdsdPitch = 0x00000008u;
	constexpr uint32_t kDdpfrgb = 0x00000040u;
	const uint32_t filter = argument(cpu, 2);
	const uint32_t context = argument(cpu, 3);
	const uint32_t callback = argument(cpu, 4);
	if (!callback) { cpu->r[EAX] = 0x80070057u; return; }
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kModeDesc, 0x1000u, error)) {
		cpu->r[EAX] = 0x80004005u; // DDERR_GENERIC
		return;
	}
	const uint32_t filterFlags = filter ? guest::load<uint32_t>(filter + 4u) : 0u;
	const uint32_t wantedWidth = filter && (filterFlags & kDdsdWidth) ? guest::load<uint32_t>(filter + 12u) : 0u;
	const uint32_t wantedHeight = filter && (filterFlags & kDdsdHeight) ? guest::load<uint32_t>(filter + 8u) : 0u;
	const uint32_t wantedBpp = filter && (filterFlags & kDdsdPixelFormat) ? guest::load<uint32_t>(filter + 84u) : 0u;
	const CPU saved = *cpu;
	for (const auto& mode : kGuestDisplayModes) {
		const uint32_t width = mode[0], height = mode[1], bpp = mode[2];
		if ((wantedWidth && wantedWidth != width) || (wantedHeight && wantedHeight != height) ||
			(wantedBpp && wantedBpp != bpp)) continue;
		std::memset(sharedGuestAddressSpace().host(kModeDesc, kDescSize), 0, kDescSize);
		guest::store<uint32_t>(kModeDesc, kDescSize);
		guest::store<uint32_t>(kModeDesc + 4u,
			kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat | kDdsdRefreshRate);
		guest::store<uint32_t>(kModeDesc + 8u, height);
		guest::store<uint32_t>(kModeDesc + 12u, width);
		guest::store<uint32_t>(kModeDesc + 16u, width * (bpp / 8u));
		guest::store<uint32_t>(kModeDesc + 24u, 60u);
		guest::store<uint32_t>(kModeDesc + 72u, 32u);
		guest::store<uint32_t>(kModeDesc + 76u, kDdpfrgb);
		guest::store<uint32_t>(kModeDesc + 84u, bpp);
		if (bpp == 16u) {
			guest::store<uint32_t>(kModeDesc + 88u, 0xf800u);
			guest::store<uint32_t>(kModeDesc + 92u, 0x07e0u);
			guest::store<uint32_t>(kModeDesc + 96u, 0x001fu);
		} else {
			guest::store<uint32_t>(kModeDesc + 88u, 0x00ff0000u);
			guest::store<uint32_t>(kModeDesc + 92u, 0x0000ff00u);
			guest::store<uint32_t>(kModeDesc + 96u, 0x000000ffu);
		}
		const uint32_t args[2] = { kModeDesc, context };
		uint32_t keepGoing = 0;
		if (!callGuestStdcall(cpu, callback, args, 2u, keepGoing, error)) {
			*cpu = saved;
			br::runtime::dispatchFailure(cpu, callback, error.c_str());
		}
		if (std::getenv("BR_TRACE_DDRAW")) {
			std::fprintf(stderr, "[br-ddraw] EnumDisplayModes %ux%ux%u callback=0x%08x continue=%u context=0x%08x mode_count=%u\n",
				width, height, bpp, callback, keepGoing, context,
				guest::load<uint32_t>(context + 0x3528u));
		}
		*cpu = saved;
		if (!keepGoing) break;
	}
	cpu->r[EAX] = 0u; // DD_OK
}

void d3d7EnumDevices(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kDescSize = 236u; // sizeof(D3DDEVICEDESC7), from d3dcaps.h.
	constexpr std::array<uint8_t, 16> iidHal = {{
		0xe0,0x3d,0xe6,0x84,0xaa,0x46,0xcf,0x11,0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e
	}};
	constexpr std::string_view description = "Direct3D HAL";
	const uint32_t callback = argument(cpu, 1);
	const uint32_t context = argument(cpu, 2);
	if (argument(cpu, 0) != kD3d7Object || !callback) {
		cpu->r[EAX] = 0x80070057u; // DDERR_INVALIDPARAMS
		return;
	}
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kD3d7DeviceDesc, 0x2000u, error)) {
		cpu->r[EAX] = 0x80004005u; // DDERR_GENERIC
		return;
	}
	std::memset(sharedGuestAddressSpace().host(kD3d7DeviceDesc, kDescSize), 0, kDescSize);
	// Advertise the legacy HAL capabilities the game callback explicitly tests.
	guest::store<uint32_t>(kD3d7DeviceDesc + 0u, 0x00092000u); // HW T&L, HW raster, DrawPrimitives2.
	for (const uint32_t prim : {4u, 60u}) {
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 0u, 56u);
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 4u, 0x0000001eu); // primitive raster caps
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 8u, 0x003f21f1u); // shade, blend, depth, fog caps
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 12u, 0xffu); // depth comparisons
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 16u, 0x1fffu); // source blend
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 20u, 0x1fffu); // destination blend
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 24u, 0xffu); // alpha comparisons
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 28u, 0x00084808u);
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 32u, 0x0000000fu);
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 36u, 0x0303033fu);
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 40u, 0xffu);
		guest::store<uint32_t>(kD3d7DeviceDesc + prim + 44u, 0x0fu);
	}
	guest::store<uint32_t>(kD3d7DeviceDesc + 116u, 0x00000400u | 0x00000100u); // 16/32-bit render targets
	guest::store<uint32_t>(kD3d7DeviceDesc + 120u, 0x00000400u); // 16-bit depth buffer
	guest::store<uint32_t>(kD3d7DeviceDesc + 124u, 1u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 128u, 1u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 132u, 2048u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 136u, 2048u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 140u, 2048u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 144u, 2048u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 148u, 1u);
	const float guardBand[5] = {-2048.f, -2048.f, 2048.f, 2048.f, 0.f};
	std::memcpy(sharedGuestAddressSpace().host(kD3d7DeviceDesc + 152u, sizeof(guardBand)), guardBand, sizeof(guardBand));
	guest::store<uint32_t>(kD3d7DeviceDesc + 172u, 0x00080001u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 176u, 0x0001ffffu);
	guest::store<uint16_t>(kD3d7DeviceDesc + 184u, 2u);
	guest::store<uint16_t>(kD3d7DeviceDesc + 186u, 1u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 188u, 8u);
	const float maxVertexW = 1.f;
	uint32_t maxVertexWBits = 0;
	std::memcpy(&maxVertexWBits, &maxVertexW, sizeof(maxVertexWBits));
	guest::store<uint32_t>(kD3d7DeviceDesc + 192u, maxVertexWBits);
	std::memcpy(sharedGuestAddressSpace().host(kD3d7DeviceDesc + 196u, iidHal.size()), iidHal.data(), iidHal.size());
	guest::store<uint16_t>(kD3d7DeviceDesc + 212u, 6u);
	guest::store<uint16_t>(kD3d7DeviceDesc + 214u, 4u);
	guest::store<uint32_t>(kD3d7DeviceDesc + 216u, 0x00000001u);
	for (uint32_t i = 0; i < description.size(); ++i) {
		guest::store<uint8_t>(kD3d7DeviceDescription + i, static_cast<uint8_t>(description[i]));
		guest::store<uint8_t>(kD3d7DeviceName + i, static_cast<uint8_t>(description[i]));
	}
	guest::store<uint8_t>(kD3d7DeviceDescription + static_cast<uint32_t>(description.size()), 0u);
	guest::store<uint8_t>(kD3d7DeviceName + static_cast<uint32_t>(description.size()), 0u);
	const CPU saved = *cpu;
	const uint32_t args[4] = {kD3d7DeviceDescription, kD3d7DeviceName, kD3d7DeviceDesc, context};
	uint32_t accepted = 0u;
	if (!callGuestStdcall(cpu, callback, args, 4u, accepted, error)) {
		*cpu = saved;
		br::runtime::dispatchFailure(cpu, callback, error.c_str());
	}
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] EnumDevices callback=0x%08x ctx=0x%08x dwDevCaps=0x%08x return=%u\n",
			callback, context, guest::load<uint32_t>(kD3d7DeviceDesc), accepted);
	}
	*cpu = saved;
	cpu->r[EAX] = 0u; // D3D_OK; one HAL device was enumerated.
}

void d3d7EnumZBufferFormats(CPU* cpu, const ApiImportStub&)
{
	constexpr std::array<uint8_t, 16> iidHal = {{
		0xe0,0x3d,0xe6,0x84,0xaa,0x46,0xcf,0x11,0x81,0x6f,0x00,0x00,0xc0,0x20,0x15,0x6e
	}};
	const uint32_t self = argument(cpu, 0);
	const uint32_t deviceGuid = argument(cpu, 1);
	const uint32_t callback = argument(cpu, 2);
	const uint32_t context = argument(cpu, 3);
	if (self != kD3d7Object || !callback || !isGuestGuid(deviceGuid, iidHal)) {
		cpu->r[EAX] = 0x80070057u; // D3DERR_INVALIDPARAMS for an unsupported device GUID.
		return;
	}
	std::string error;
	if (!sharedGuestAddressSpace().commitRange(kD3d7ZBufferPixelFormat, 0x1000u, error)) {
		cpu->r[EAX] = 0x80004005u; // D3DERR_OUTOFVIDEOMEMORY / generic host allocation failure.
		return;
	}
	// Windows x86 Direct3D 7 EnumZBufferFormats on the host reports a 16-bit
	// Z-only pixel format with these exact DDPIXELFORMAT fields. This is the
	// format the guest callback selects for its requested 16-bit depth.
	std::memset(sharedGuestAddressSpace().host(kD3d7ZBufferPixelFormat, 32u), 0, 32u);
	guest::store<uint32_t>(kD3d7ZBufferPixelFormat + 0u, 32u);
	guest::store<uint32_t>(kD3d7ZBufferPixelFormat + 4u, 0x00000400u); // DDPF_ZBUFFER
	guest::store<uint32_t>(kD3d7ZBufferPixelFormat + 12u, 16u);
	guest::store<uint32_t>(kD3d7ZBufferPixelFormat + 20u, 0x0000ffffu); // dwZBitMask
	const CPU saved = *cpu;
	const uint32_t args[2] = {kD3d7ZBufferPixelFormat, context};
	uint32_t keepGoing = 0u;
	if (!callGuestStdcall(cpu, callback, args, 2u, keepGoing, error)) {
		*cpu = saved;
		br::runtime::dispatchFailure(cpu, callback, error.c_str());
	}
	if (std::getenv("BR_TRACE_DDRAW")) {
		std::fprintf(stderr, "[br-ddraw] EnumZBufferFormats callback=0x%08x ctx=0x%08x format=Z16 return=%u\n",
			callback, context, keepGoing);
	}
	*cpu = saved;
	cpu->r[EAX] = 0u; // D3D_OK.
}

void getSystemMetrics(CPU* cpu, const ApiImportStub&)
{
	const StartupContext context = startupContext();
	switch (argument(cpu, 0)) {
	case 0u: cpu->r[EAX] = context.screenWidth; break; // SM_CXSCREEN
	case 1u: cpu->r[EAX] = context.screenHeight; break; // SM_CYSCREEN
	default: cpu->r[EAX] = 0u; break;
	}
}

void getUserDefaultLcid(CPU* cpu, const ApiImportStub&)
{
	cpu->r[EAX] = startupContext().userDefaultLcid;
}

void getLocaleInfoA(CPU* cpu, const ApiImportStub&)
{
	const StartupContext context = startupContext();
	const uint32_t lcid = argument(cpu, 0);
	const uint32_t type = argument(cpu, 1);
	const uint32_t output = argument(cpu, 2);
	const uint32_t capacity = argument(cpu, 3);
	if ((lcid != 0x0400u && lcid != 0x0800u && lcid != context.userDefaultLcid) || type != 0x1004u) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u); // ERROR_INVALID_PARAMETER / unsupported LCTYPE
		return;
	}
	const std::string codePage = std::to_string(context.ansiCodePage);
	const uint32_t required = static_cast<uint32_t>(codePage.size() + 1u);
	if (capacity == 0u) {
		cpu->r[EAX] = required;
		return;
	}
	if (!output || capacity < required || !sharedGuestAddressSpace().host(output, required)) {
		cpu->r[EAX] = 0;
		setWin32LastError(capacity < required ? 122u : 998u); // insufficient buffer / no access
		return;
	}
	for (uint32_t i = 0; i < codePage.size(); ++i)
		guest::store<uint8_t>(output + i, static_cast<uint8_t>(codePage[i]));
	guest::store<uint8_t>(output + static_cast<uint32_t>(codePage.size()), 0u);
	cpu->r[EAX] = required;
}

void destroyWindow(CPU* cpu, const ApiImportStub&);

void defWindowProcA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t windowHandle = argument(cpu, 0);
	const uint32_t message = argument(cpu, 1);
	if (std::getenv("BR_TRACE_WINDOW_MESSAGES") && gTracedDefWindowProcCallCount < 32u) {
		++gTracedDefWindowProcCallCount;
		std::fprintf(stderr, "[br-window] DefWindowProcA hwnd=0x%08x msg=0x%04x wp=0x%08x lp=0x%08x caller=0x%08x\n",
			windowHandle, message, argument(cpu, 2), argument(cpu, 3), guest::load<uint32_t>(cpu->r[ESP]));
	}
	switch (message) {
	case 0x0081u: // WM_NCCREATE: a window must accept nonclient creation to continue.
		cpu->r[EAX] = 1u;
		return;
	case 0x0001u: // WM_CREATE
	case 0x0003u: // WM_MOVE
	case 0x0005u: // WM_SIZE
	case 0x0006u: // WM_ACTIVATE
	case 0x0007u: // WM_SETFOCUS
	case 0x0008u: // WM_KILLFOCUS
	case 0x001cu: // WM_ACTIVATEAPP
	case 0x0100u: // WM_KEYDOWN
	case 0x0101u: // WM_KEYUP
	case 0x0102u: // WM_CHAR
	case 0x0104u: // WM_SYSKEYDOWN
	case 0x0105u: // WM_SYSKEYUP
	case 0x0106u: // WM_SYSCHAR
		cpu->r[EAX] = 0u;
		return;
	case 0x000fu: { // WM_PAINT: DefWindowProc validates the update region.
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(windowHandle);
		if (found != gGuestWindows.end()) {
			if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
				std::fprintf(stderr, "[br-window] DefWindowProcA WM_PAINT hwnd=0x%08x caller=0x%08x invalid=%u->0\n",
					windowHandle, guest::load<uint32_t>(cpu->r[ESP]),
					found->second.updateRegionInvalid ? 1u : 0u);
			found->second.updateRegionInvalid = false;
		}
		cpu->r[EAX] = 0u;
		return;
	}
	case 0x0082u: // WM_NCDESTROY
	case 0x0002u: // WM_DESTROY
	case 0x0018u: // WM_SHOWWINDOW
		cpu->r[EAX] = 0u;
		return;
	case 0x0010u: { // WM_CLOSE: DefWindowProc destroys the HWND synchronously.
		// Use the same lifecycle implementation as the exported DestroyWindow
		// API so the guest WndProc receives WM_DESTROY/WM_NCDESTROY and the
		// window/message state is removed consistently. DefWindowProc itself
		// returns zero for WM_CLOSE; DestroyWindow's BOOL is not its LRESULT.
		destroyWindow(cpu, ApiImportStub{});
		const uint32_t destroyed = cpu->r[EAX];
		if (!destroyed) return;
		cpu->r[EAX] = 0u;
		return;
	}
	case 0x0046u: // WM_WINDOWPOSCHANGING: the default procedure leaves the proposal intact.
		cpu->r[EAX] = 0u;
		return;
	case 0x0047u: { // WM_WINDOWPOSCHANGED: DefWindowProc derives WM_MOVE and WM_SIZE.
		const uint32_t position = argument(cpu, 3);
		if (!position || !sharedGuestAddressSpace().host(position, 28u)) {
			cpu->r[EAX] = 0u;
			setWin32LastError(998u); // ERROR_NOACCESS
			return;
		}
		const uint32_t x = guest::load<uint32_t>(position + 8u);
		const uint32_t y = guest::load<uint32_t>(position + 12u);
		const uint32_t width = guest::load<uint32_t>(position + 16u);
		const uint32_t height = guest::load<uint32_t>(position + 20u);
		const uint32_t flags = guest::load<uint32_t>(position + 24u);
		GuestWindow window;
		{
			std::lock_guard<std::mutex> lock(gWindowClassesMutex);
			const auto found = gGuestWindows.find(windowHandle);
			if (found == gGuestWindows.end()) {
				cpu->r[EAX] = 0u;
				setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE
				return;
			}
			window = found->second;
		}
		uint32_t ignored = 0u;
		if (!(flags & 0x0002u)) { // SWP_NOMOVE
			const uint32_t packedPosition = (x & 0xffffu) | ((y & 0xffffu) << 16u);
			if (!callWindowProcedure(cpu, window.windowProcedure, windowHandle,
				0x0003u, 0u, packedPosition, ignored)) return; // WM_MOVE
		}
		if (!(flags & 0x0001u)) { // SWP_NOSIZE
			const uint32_t packedSize = (width & 0xffffu) | ((height & 0xffffu) << 16u);
			if (!callWindowProcedure(cpu, window.windowProcedure, windowHandle,
				0x0005u, 0u, packedSize, ignored)) return; // WM_SIZE / SIZE_RESTORED
		}
		if (!(flags & 0x0001u) || !(flags & 0x0002u)) {
			std::lock_guard<std::mutex> lock(gWindowClassesMutex);
			const auto found = gGuestWindows.find(windowHandle);
			if (found != gGuestWindows.end()) found->second.updateRegionInvalid = true;
		}
		cpu->r[EAX] = 0u;
		return;
	}
	default: {
		const uint32_t stack = cpu->r[ESP];
		const uint32_t caller = guest::load<uint32_t>(stack);
		unsupported(cpu, caller, "DefWindowProcA message semantics");
	}
	}
}

void sendMessageA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t windowHandle = argument(cpu, 0);
	const uint32_t message = argument(cpu, 1);
	const uint32_t wParam = argument(cpu, 2);
	const uint32_t lParam = argument(cpu, 3);
	const bool trace = std::getenv("BR_TRACE_WINDOW_MESSAGES") &&
		gTracedSendMessageCallCount < 32u;
	if (trace) {
		++gTracedSendMessageCallCount;
		std::fprintf(stderr, "[br-window] SendMessageA hwnd=0x%08x msg=0x%04x wp=0x%08x lp=0x%08x caller=0x%08x\n",
			windowHandle, message, wParam, lParam, guest::load<uint32_t>(cpu->r[ESP]));
	}

	// SendMessage is synchronous: invoke the registered guest WndProc on the
	// current guest thread and return its LRESULT. Copy the procedure while the
	// window table is locked, then release the lock before running guest code so
	// reentrant User32 calls (including DestroyWindow) cannot deadlock.
	GuestWindow window;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(windowHandle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			return;
		}
		window = found->second;
	}

	uint32_t result = 0u;
	if (!callWindowProcedure(cpu, window.windowProcedure, windowHandle,
		message, wParam, lParam, result)) return;
	cpu->r[EAX] = result;
	if (trace)
		std::fprintf(stderr, "[br-window] SendMessageA result=0x%08x hwnd=0x%08x msg=0x%04x\n",
			result, windowHandle, message);
}

void peekMessageA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t output = argument(cpu, 0);
	const uint32_t filterWindow = argument(cpu, 1);
	const uint32_t minMessage = argument(cpu, 2);
	const uint32_t maxMessage = argument(cpu, 3);
	const uint32_t removeFlags = argument(cpu, 4);
	if (!output || !sharedGuestAddressSpace().host(output, 28u) || (removeFlags & 0x0000fffcu)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(!output || !sharedGuestAddressSpace().host(output, 28u) ? 998u : 87u);
		return;
	}
	{
		std::lock_guard<std::mutex> lock(gGuestMessageMutex);
		const bool filterRange = minMessage != 0u || maxMessage != 0u;
		for (auto message = gGuestMessageQueue.begin(); message != gGuestMessageQueue.end(); ++message) {
			if (message->message != 0x0012u && filterWindow && message->window != filterWindow) continue;
			if (message->message != 0x0012u && filterRange &&
				(message->message < minMessage || message->message > maxMessage)) continue;
			guest::store<uint32_t>(output, message->window);
			guest::store<uint32_t>(output + 4u, message->message);
			guest::store<uint32_t>(output + 8u, message->wParam);
			guest::store<uint32_t>(output + 12u, message->lParam);
			guest::store<uint32_t>(output + 16u, message->time);
			guest::store<int32_t>(output + 20u, message->x);
			guest::store<int32_t>(output + 24u, message->y);
			if (removeFlags & 1u) gGuestMessageQueue.erase(message);
			cpu->r[EAX] = 1u;
			return;
		}
	}
	GuestMessage paint;
	if (findInvalidPaintMessage(filterWindow, minMessage, maxMessage, paint)) {
		guest::store<uint32_t>(output, paint.window);
		guest::store<uint32_t>(output + 4u, paint.message);
		guest::store<uint32_t>(output + 8u, paint.wParam);
		guest::store<uint32_t>(output + 12u, paint.lParam);
		guest::store<uint32_t>(output + 16u, paint.time);
		guest::store<int32_t>(output + 20u, paint.x);
		guest::store<int32_t>(output + 24u, paint.y);
		cpu->r[EAX] = 1u;
		return;
	}
	cpu->r[EAX] = 0u;
}

void postQuitMessage(CPU* cpu, const ApiImportStub&)
{
	postGuestQuitMessage(argument(cpu, 0));
	cpu->r[EAX] = 0u;
}

void getMessageA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t output = argument(cpu, 0);
	const uint32_t filterWindow = argument(cpu, 1);
	const uint32_t minMessage = argument(cpu, 2);
	const uint32_t maxMessage = argument(cpu, 3);
	if (std::getenv("BR_TRACE_WINDOW_MESSAGES") && gTracedGetMessageCallCount < 8u) {
		++gTracedGetMessageCallCount;
		std::fprintf(stderr, "[br-window] GetMessageA request hwnd=0x%08x range=0x%04x..0x%04x\n",
			filterWindow, minMessage, maxMessage);
		std::lock_guard<std::mutex> windowLock(gWindowClassesMutex);
		for (const auto& item : gGuestWindows)
			std::fprintf(stderr, "[br-window] GetMessageA window hwnd=0x%08x style=0x%08x invalid=%u\n",
				item.first, item.second.style, item.second.updateRegionInvalid ? 1u : 0u);
	}
	if (!output || !sharedGuestAddressSpace().host(output, 28u)) {
		cpu->r[EAX] = 0xffffffffu;
		setWin32LastError(998u); // ERROR_NOACCESS
		return;
	}
	if (filterWindow) {
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		if (!gGuestWindows.count(filterWindow)) {
			cpu->r[EAX] = 0xffffffffu;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE
			return;
		}
	}
	const bool filterRange = minMessage != 0u || maxMessage != 0u;
	const auto matchesMessage = [&](const GuestMessage& item) {
		if (item.message == 0x0012u) return true;
		if (filterWindow && item.window != filterWindow) return false;
		return !filterRange || (item.message >= minMessage && item.message <= maxMessage);
	};
	const auto started = std::chrono::steady_clock::now();
	uint32_t diagnosticTimeoutMs = 0u;	// Test-only wall-clock bound while this GetMessage call is blocked.
	if (const char* value = std::getenv("BR_GETMESSAGE_TIMEOUT_MS")) {
		char* end = nullptr;
		const unsigned long parsed = std::strtoul(value, &end, 10);
		if (end != value && parsed <= 600000ul) diagnosticTimeoutMs = static_cast<uint32_t>(parsed);
	}
	std::unique_lock<std::mutex> lock(gGuestMessageMutex);
	for (;;) {
			auto message = std::find_if(gGuestMessageQueue.begin(), gGuestMessageQueue.end(), matchesMessage);
		if (message != gGuestMessageQueue.end()) {
			const GuestMessage value = *message;
			gGuestMessageQueue.erase(message);
			lock.unlock();
			guest::store<uint32_t>(output, value.window);
			guest::store<uint32_t>(output + 4u, value.message);
			guest::store<uint32_t>(output + 8u, value.wParam);
			guest::store<uint32_t>(output + 12u, value.lParam);
			guest::store<uint32_t>(output + 16u, value.time);
			guest::store<int32_t>(output + 20u, value.x);
			guest::store<int32_t>(output + 24u, value.y);
			if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
				std::fprintf(stderr, "[br-window] GetMessageA hwnd=0x%08x msg=0x%04x wp=0x%08x lp=0x%08x\n",
					value.window, value.message, value.wParam, value.lParam);
			cpu->r[EAX] = value.message == 0x0012u ? 0u : 1u;
			return;
		}
		lock.unlock();
		pumpMultimediaTimers(cpu);
		lock.lock();
		if (std::find_if(gGuestMessageQueue.begin(), gGuestMessageQueue.end(), matchesMessage) !=
			gGuestMessageQueue.end()) continue;
		lock.unlock();
		GuestMessage paint;
		const bool hasPaint = findInvalidPaintMessage(filterWindow, minMessage, maxMessage, paint);
		lock.lock();
		if (std::find_if(gGuestMessageQueue.begin(), gGuestMessageQueue.end(), matchesMessage) !=
			gGuestMessageQueue.end()) continue; // Posted messages take priority over synthesized paint.
		if (hasPaint) {
			lock.unlock();
			guest::store<uint32_t>(output, paint.window);
			guest::store<uint32_t>(output + 4u, paint.message);
			guest::store<uint32_t>(output + 8u, paint.wParam);
			guest::store<uint32_t>(output + 12u, paint.lParam);
			guest::store<uint32_t>(output + 16u, paint.time);
			guest::store<int32_t>(output + 20u, paint.x);
			guest::store<int32_t>(output + 24u, paint.y);
			if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
				std::fprintf(stderr, "[br-window] GetMessageA synthesized invalid-region WM_PAINT hwnd=0x%08x\n",
					paint.window);
			cpu->r[EAX] = 1u;
			return;
		}
		if (diagnosticTimeoutMs && std::chrono::steady_clock::now() - started >=
			std::chrono::milliseconds(diagnosticTimeoutMs)) {
			lock.unlock();
			throw std::runtime_error("GetMessageA diagnostic timeout: no posted message or invalid paint was available");
		}
		gGuestMessageAvailable.wait_for(lock, std::chrono::milliseconds(10));
		lock.unlock();
		pumpMultimediaTimers(cpu);
		lock.lock();
	}
}

void translateMessage(CPU* cpu, const ApiImportStub&)
{
	const uint32_t input = argument(cpu, 0);
	if (!input || !sharedGuestAddressSpace().host(input, 28u)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u);
		return;
	}
	const uint32_t message = guest::load<uint32_t>(input + 4u);
	const uint32_t key = guest::load<uint32_t>(input + 8u);
	if (message != 0x0100u && message != 0x0104u) {
		cpu->r[EAX] = 0u;
		return;
	}
	bool shift = false, control = false, capsLock = false;
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		shift = (gDirectInputKeyboardState[0x10u] & 0x80u) != 0u;
		control = (gDirectInputKeyboardState[0x11u] & 0x80u) != 0u;
		capsLock = (gDirectInputKeyboardState[0x14u] & 1u) != 0u;
	}
	uint32_t character = 0u;
	if (key >= 'A' && key <= 'Z') {
		character = (shift != capsLock) ? key : key + ('a' - 'A');
		if (control) character = key - 'A' + 1u;
	} else if (key >= '0' && key <= '9') {
		constexpr char shiftedDigits[] = ")!@#$%^&*(";
		character = shift ? static_cast<uint8_t>(shiftedDigits[key - '0']) : key;
	} else if (key == 0x20u || key == 0x09u || key == 0x0du || key == 0x08u) {
		character = key == 0x0du ? 0x0du : key;
	} else {
		struct KeyPair { uint32_t vk; char normal; char shifted; };
		static constexpr KeyPair punctuation[] = {
			{0xbau, ';', ':'}, {0xbbu, '=', '+'}, {0xbcu, ',', '<'}, {0xbdu, '-', '_'},
			{0xbeu, '.', '>'}, {0xbfu, '/', '?'}, {0xc0u, '`', '~'}, {0xdbu, '[', '{'},
			{0xdcu, '\\', '|'}, {0xddu, ']', '}'}, {0xdeu, '\'', '"'}
		};
		for (const KeyPair& pair : punctuation) {
			if (key == pair.vk) { character = static_cast<uint8_t>(shift ? pair.shifted : pair.normal); break; }
		}
	}
	if (!character) { cpu->r[EAX] = 0u; return; }
	GuestMessage translated;
	translated.window = guest::load<uint32_t>(input);
	translated.message = message == 0x0104u ? 0x0106u : 0x0102u; // WM_SYSCHAR / WM_CHAR
	translated.wParam = character;
	translated.lParam = guest::load<uint32_t>(input + 12u);
	translated.time = guest::load<uint32_t>(input + 16u);
	translated.x = guest::load<int32_t>(input + 20u);
	translated.y = guest::load<int32_t>(input + 24u);
	enqueueGuestMessage(translated);
	cpu->r[EAX] = 1u;
}

void dispatchMessageA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t input = argument(cpu, 0);
	if (!input || !sharedGuestAddressSpace().host(input, 28u)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u);
		return;
	}
	const uint32_t windowHandle = guest::load<uint32_t>(input);
	const uint32_t message = guest::load<uint32_t>(input + 4u);
	if (message == 0x0012u) { cpu->r[EAX] = 0u; return; }
	GuestWindow window;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(windowHandle);
		if (found == gGuestWindows.end()) { cpu->r[EAX] = 0u; return; }
		window = found->second;
	}
	const uint32_t lParam = guest::load<uint32_t>(input + 12u);
	if (message == 0x0113u && lParam) {
		uint32_t result = 0u;
		std::string error;
		if (!callGuestStdcall(cpu, lParam,
			(std::array<uint32_t, 4>{{windowHandle, message,
				guest::load<uint32_t>(input + 8u), guest::load<uint32_t>(input + 12u)}}).data(),
			4u, result, error)) {
			dispatchFailure(cpu, lParam, error.c_str());
		}
		cpu->r[EAX] = result;
		return;
	}
	uint32_t result = 0u;
	if (!callWindowProcedure(cpu, window.windowProcedure, windowHandle, message,
		guest::load<uint32_t>(input + 8u), lParam, result)) return;
	cpu->r[EAX] = result;
}

void findWindowA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t classAddress = argument(cpu, 0);
	const uint32_t titleAddress = argument(cpu, 1);
	const std::string className = classAddress ? lower(readGuestString(classAddress)) : std::string{};
	const std::string title = titleAddress ? lower(readGuestString(titleAddress)) : std::string{};
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	for (const auto& item : gGuestWindows) {
		if ((!className.empty() && lower(item.second.className) != className) ||
			(!title.empty() && lower(item.second.title) != title)) continue;
		cpu->r[EAX] = item.first;
		return;
	}
	cpu->r[EAX] = 0;
}

void getKeyboardLayout(CPU* cpu, const ApiImportStub&)
{
	const uint32_t threadId = argument(cpu, 0);
	if (threadId != 0u) {
		cpu->r[EAX] = 0u;
		setWin32LastError(87u); // The guest runtime currently models its calling thread only.
		return;
	}
	const uint32_t languageId = startupContext().userDefaultLcid & 0xffffu;
	// The compatibility profile's loaded default layout is US English (0409).
	// A Windows HKL carries the LANGID in its low word and the layout id in
	// its high word; mirror the active profile for this guest thread.
	const uint32_t activeLanguage = languageId ? languageId : 0x0409u;
	cpu->r[EAX] = (activeLanguage << 16u) | activeLanguage;
}

void registerWindowMessageA(CPU* cpu, const ApiImportStub&)
{
	const std::string message = readGuestString(argument(cpu, 0));
	if (message.empty()) {
		cpu->r[EAX] = 0;
		return;
	}
	std::lock_guard<std::mutex> lock(gRegisteredMessagesMutex);
	const auto existing = gRegisteredMessagesByName.find(message);
	if (existing != gRegisteredMessagesByName.end()) {
		cpu->r[EAX] = existing->second;
		return;
	}
	uint32_t hash = 2166136261u;
	for (unsigned char byte : message) hash = (hash ^ byte) * 16777619u;
	uint32_t id = 0xc000u + (hash & 0x3fffu);
	for (uint32_t attempt = 0; attempt < 0x4000u; ++attempt) {
		const uint32_t candidate = 0xc000u + ((id - 0xc000u + attempt) & 0x3fffu);
		if (!gRegisteredMessageNamesById.count(candidate)) {
			gRegisteredMessageNamesById.emplace(candidate, message);
			gRegisteredMessagesByName.emplace(message, candidate);
			cpu->r[EAX] = candidate;
			return;
		}
	}
	cpu->r[EAX] = 0;
}

bool peResourceIdExists(uint32_t moduleBase, uint32_t typeId, uint32_t resourceId)
{
	auto read16 = [](uint32_t address, uint16_t& value) {
		const auto* p = sharedGuestAddressSpace().host(address, 2u);
		if (!p) return false;
		value = uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8u);
		return true;
	};
	auto read32 = [](uint32_t address, uint32_t& value) {
		const auto* p = sharedGuestAddressSpace().host(address, 4u);
		if (!p) return false;
		value = uint32_t(p[0]) | (uint32_t(p[1]) << 8u) |
			(uint32_t(p[2]) << 16u) | (uint32_t(p[3]) << 24u);
		return true;
	};
	uint32_t peOffset = 0, resourceRva = 0, optionalHeader = 0;
	uint16_t magic = 0;
	if (!read32(moduleBase + 0x3cu, peOffset) || peOffset > 0x100000u) return false;
	optionalHeader = moduleBase + peOffset + 24u;
	if (!read16(optionalHeader, magic) || magic != 0x10bu ||
		!read32(optionalHeader + 96u + 2u * 8u, resourceRva) || !resourceRva) return false;
	const uint32_t root = moduleBase + resourceRva;
	auto findChildDirectory = [&](uint32_t directory, uint32_t id, uint32_t& child) {
		uint16_t named = 0, ids = 0;
		if (!read16(directory + 12u, named) || !read16(directory + 14u, ids)) return false;
		const uint32_t count = uint32_t(named) + ids;
		if (count > 4096u) return false;
		for (uint32_t i = 0; i < count; ++i) {
			uint32_t name = 0, offset = 0;
			const uint32_t entry = directory + 16u + i * 8u;
			if (!read32(entry, name) || !read32(entry + 4u, offset)) return false;
			if ((name & 0x80000000u) || (name & 0xffffu) != id || !(offset & 0x80000000u)) continue;
			child = root + (offset & 0x7fffffffu);
			return true;
		}
		return false;
	};
	uint32_t typeDirectory = 0, nameDirectory = 0;
	return findChildDirectory(root, typeId, typeDirectory) &&
		findChildDirectory(typeDirectory, resourceId, nameDirectory);
}

void loadIconA(CPU* cpu, const ApiImportStub&)
{
	const StartupContext context = startupContext();
	const uint32_t module = argument(cpu, 0);
	const uint32_t resource = argument(cpu, 1);
	if (!module || module != context.mainModuleBase || resource > 0xffffu ||
		!peResourceIdExists(module, 14u, resource)) {
		cpu->r[EAX] = 0;
		return;
	}
	// Keep a typed guest handle for the verified RT_GROUP_ICON resource. The
	// Android window/icon presentation layer is not connected at this stage.
	cpu->r[EAX] = 0x6c000000u | resource;
}

void loadCursorA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t module = argument(cpu, 0);
	const uint32_t resource = argument(cpu, 1);
	if ((!module && resource == 0x7f00u) || (module && resource <= 0xffffu &&
		peResourceIdExists(module, 12u, resource)))
		cpu->r[EAX] = 0x6b000000u | resource;
	else cpu->r[EAX] = 0;
}

void getStockObject(CPU* cpu, const ApiImportStub&)
{
	const uint32_t object = argument(cpu, 0);
	cpu->r[EAX] = object <= 17u ? 0x6a000000u | object : 0u;
}

void registerClassA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t structure = argument(cpu, 0);
	const auto* wndClass = sharedGuestAddressSpace().host(structure, 40u);
	if (!wndClass) {
		cpu->r[EAX] = 0;
		setWin32LastError(998u); // ERROR_NOACCESS
		return;
	}
	const uint32_t classNameAddress = guest::load<uint32_t>(structure + 36u);
	const std::string className = readGuestString(classNameAddress);
	const uint32_t windowProcedure = guest::load<uint32_t>(structure + 4u);
	if (className.empty() || !windowProcedure) {
		cpu->r[EAX] = 0;
		setWin32LastError(87u); // ERROR_INVALID_PARAMETER
		return;
	}
	RegisteredWindowClass registered;
	registered.windowProcedure = windowProcedure;
	registered.style = guest::load<uint32_t>(structure);
	registered.instance = guest::load<uint32_t>(structure + 16u);
	registered.icon = guest::load<uint32_t>(structure + 20u);
	registered.cursor = guest::load<uint32_t>(structure + 24u);
	registered.background = guest::load<uint32_t>(structure + 28u);
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	const std::string normalizedName = lower(className);
	const auto existing = gWindowClasses.find(normalizedName);
	if (existing != gWindowClasses.end()) {
		cpu->r[EAX] = 0;
		setWin32LastError(1410u); // ERROR_CLASS_ALREADY_EXISTS
		return;
	}
	const uint16_t atom = gNextWindowClassAtom++;
	registered.atom = atom;
	gWindowClasses.emplace(normalizedName, registered);
	gWindowClassNamesByAtom.emplace(atom, normalizedName);
	cpu->r[EAX] = atom;
}

bool callWindowProcedure(CPU* cpu, uint32_t procedure, uint32_t window,
	uint32_t message, uint32_t wParam, uint32_t lParam, uint32_t& result)
{
	if (message == 0x001cu) { // WM_ACTIVATEAPP updates the process activation state.
		const bool active = wParam != 0u;
		{
			std::lock_guard<std::mutex> lock(gWindowClassesMutex);
			gGuestApplicationActive = active;
		}
		{
			std::lock_guard<std::mutex> lock(gDdrawCooperativeMutex);
			if (gDdrawCooperativeState.flags & 0x00000010u)
				gDdrawCooperativeState.lost = !active;
		}
	}
	const uint32_t arguments[] = {window, message, wParam, lParam};
	const bool traceGameState = std::getenv("BR_TRACE_GAME_STATE") != nullptr;
	const uint32_t state50Before = traceGameState ? guest::load<uint32_t>(0x006de340u) : 0u;
	const uint32_t state60Before = traceGameState ? guest::load<uint32_t>(0x006de350u) : 0u;
	std::string error;
	if (!callGuestStdcall(cpu, procedure, arguments, 4u, result, error)) {
		dispatchFailure(cpu, procedure, error.c_str());
		return false;
	}
	if (traceGameState) {
		const uint32_t state50After = guest::load<uint32_t>(0x006de340u);
		const uint32_t state60After = guest::load<uint32_t>(0x006de350u);
		const bool tracePaintSample = message == 0x000fu && gTracedPaintCallbackCount < 16u;
		if (tracePaintSample) ++gTracedPaintCallbackCount;
		if (state50After != state50Before || state60After != state60Before || tracePaintSample) {
			const auto threadId = std::hash<std::thread::id>{}(std::this_thread::get_id());
			std::fprintf(stderr, "[br-state] WndProc msg=0x%04x wp=%08x lp=%08x host-thread=%zx guest-esp=%08x fsbase=%08x fields=%08x/%08x->%08x/%08x result=%08x\n",
				message, wParam, lParam, static_cast<size_t>(threadId), cpu->r[ESP], cpu->fsbase,
				state50Before, state60Before, state50After, state60After, result);
		}
	}
	if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
		std::fprintf(stderr, "[br-window] WndProc=0x%08x hwnd=0x%08x msg=0x%04x wp=0x%08x lp=0x%08x result=0x%08x\n",
			procedure, window, message, wParam, lParam, result);
	return true;
}

void createWindowExA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t extendedStyle = argument(cpu, 0);
	const uint32_t classAddress = argument(cpu, 1);
	const std::string requestedClass = classAddress <= 0xffffu
		? std::string{} : lower(readGuestString(classAddress));
	const uint16_t classAtom = static_cast<uint16_t>(classAddress);
	const std::string title = readGuestString(argument(cpu, 2));
	RegisteredWindowClass registered;
	std::string className = requestedClass;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		if (className.empty()) {
			const auto atom = gWindowClassNamesByAtom.find(classAtom);
			if (atom != gWindowClassNamesByAtom.end()) className = atom->second;
		}
		const auto found = gWindowClasses.find(className);
		if (found == gWindowClasses.end()) {
			cpu->r[EAX] = 0;
			setWin32LastError(1407u); // ERROR_CANNOT_FIND_WND_CLASS
			return;
		}
		registered = found->second;
	}
	std::call_once(gCreateStructMemoryOnce, [] {
		gCreateStructMemoryReady = guest::commit(kCreateStructBase, 0x10000u) &&
			guest::commit(kWindowPosAddress, 0x1000u);
	});
	if (!gCreateStructMemoryReady) {
		cpu->r[EAX] = 0;
		setWin32LastError(8u); // ERROR_NOT_ENOUGH_MEMORY
		return;
	}
	GuestWindow window;
	window.className = className;
	window.title = title;
	window.windowProcedure = registered.windowProcedure;
	window.extendedStyle = extendedStyle;
	window.style = argument(cpu, 3);
	window.updateRegionInvalid = (window.style & 0x10000000u) != 0u;
	window.x = argument(cpu, 4);
	window.y = argument(cpu, 5);
	window.width = argument(cpu, 6);
	window.height = argument(cpu, 7);
	window.parent = argument(cpu, 8);
	window.menu = argument(cpu, 9);
	window.instance = argument(cpu, 10);
	window.createParameter = argument(cpu, 11);
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		if (gNextGuestWindow < 0x6c1ffffcu) {
			window.handle = gNextGuestWindow;
			gNextGuestWindow += 4u;
			gGuestWindows.emplace(window.handle, window);
		}
	}
	if (!window.handle) {
		cpu->r[EAX] = 0;
		setWin32LastError(8u);
		return;
	}

	const uint32_t structure = kCreateStructBase + (window.handle - 0x6c100000u) * 12u;
	for (uint32_t offset = 0; offset < 48u; offset += 4u) guest::store<uint32_t>(structure + offset, 0u);
	guest::store<uint32_t>(structure, window.createParameter);
	guest::store<uint32_t>(structure + 4u, window.instance);
	guest::store<uint32_t>(structure + 8u, window.menu);
	guest::store<uint32_t>(structure + 12u, window.parent);
	guest::store<uint32_t>(structure + 16u, window.height);
	guest::store<uint32_t>(structure + 20u, window.width);
	guest::store<uint32_t>(structure + 24u, window.y);
	guest::store<uint32_t>(structure + 28u, window.x);
	guest::store<uint32_t>(structure + 32u, window.style);
	guest::store<uint32_t>(structure + 36u, argument(cpu, 2));
	guest::store<uint32_t>(structure + 40u, classAddress);
	guest::store<uint32_t>(structure + 44u, extendedStyle);

	uint32_t result = 0;
	if (!callWindowProcedure(cpu, window.windowProcedure, window.handle, 0x81u, 0u, structure, result) || !result) {
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		gGuestWindows.erase(window.handle);
		cpu->r[EAX] = 0;
		return;
	}
	if (!callWindowProcedure(cpu, window.windowProcedure, window.handle, 1u, 0u, structure, result) ||
		static_cast<int32_t>(result) == -1) {
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		gGuestWindows.erase(window.handle);
		cpu->r[EAX] = 0;
		return;
	}
	// Initial placement follows the same WM_WINDOWPOSCHANGING/CHANGED path as
	// SetWindowPos. DefWindowProc then emits the corresponding WM_MOVE/WM_SIZE
	// notifications, which the game's window procedure may consume.
	guest::store<uint32_t>(kWindowPosAddress + 0u, window.handle);
	guest::store<uint32_t>(kWindowPosAddress + 4u, 0u); // HWND_TOP
	guest::store<uint32_t>(kWindowPosAddress + 8u, window.x);
	guest::store<uint32_t>(kWindowPosAddress + 12u, window.y);
	guest::store<uint32_t>(kWindowPosAddress + 16u, window.width);
	guest::store<uint32_t>(kWindowPosAddress + 20u, window.height);
	guest::store<uint32_t>(kWindowPosAddress + 24u, 0u);
	if (!callWindowProcedure(cpu, window.windowProcedure, window.handle,
		0x0046u, 0u, kWindowPosAddress, result) ||
		!callWindowProcedure(cpu, window.windowProcedure, window.handle,
			0x0047u, 0u, kWindowPosAddress, result)) {
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		gGuestWindows.erase(window.handle);
		cpu->r[EAX] = 0u;
		return;
	}
	// WS_VISIBLE causes the window manager to notify the procedure synchronously.
	if (window.style & 0x10000000u) {
		if (!callWindowProcedure(cpu, window.windowProcedure, window.handle,
			0x0018u, 1u, 0u, result)) return; // WM_SHOWWINDOW(TRUE)
	}
	cpu->r[EAX] = window.handle;
}

void getWindowLongA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	const int32_t index = static_cast<int32_t>(argument(cpu, 1));
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	const auto found = gGuestWindows.find(handle);
	if (found == gGuestWindows.end()) {
		cpu->r[EAX] = 0u;
		setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
		return;
	}
	const GuestWindow& window = found->second;
	switch (index) {
	case -4: cpu->r[EAX] = window.windowProcedure; return; // GWL_WNDPROC.
	case -6: cpu->r[EAX] = window.instance; return; // GWL_HINSTANCE.
	case -8: cpu->r[EAX] = window.parent; return; // GWL_HWNDPARENT.
	case -12: cpu->r[EAX] = window.menu; return; // GWL_ID/menu handle.
	case -16: cpu->r[EAX] = window.style; return; // GWL_STYLE.
	case -20: cpu->r[EAX] = window.extendedStyle; return; // GWL_EXSTYLE.
	case -21: {
		const auto value = window.extraLongs.find(index);
		cpu->r[EAX] = value == window.extraLongs.end() ? 0u : value->second; // GWL_USERDATA.
		return;
	}
	default: {
		const auto value = window.extraLongs.find(index);
		if (value == window.extraLongs.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1413u); // ERROR_INVALID_INDEX.
			return;
		}
		cpu->r[EAX] = value->second;
		return;
	}
	}
}

void setWindowLongA(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	const int32_t index = static_cast<int32_t>(argument(cpu, 1));
	const uint32_t value = argument(cpu, 2);
	constexpr uint32_t kWsDisabled = 0x08000000u;
	GuestWindow snapshot;
	uint32_t previous = 0u;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		GuestWindow& window = found->second;
		switch (index) {
		case -4: previous = window.windowProcedure; window.windowProcedure = value; break;
		case -6: previous = window.instance; window.instance = value; break;
		case -8:
			if (value && !gGuestWindows.count(value)) {
				cpu->r[EAX] = 0u;
				setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
				return;
			}
			previous = window.parent; window.parent = value; break;
		case -12: previous = window.menu; window.menu = value; break;
		case -16: previous = window.style; window.style = value; break;
		case -20: previous = window.extendedStyle; window.extendedStyle = value; break;
		case -21:
			if (const auto old = window.extraLongs.find(index); old != window.extraLongs.end()) previous = old->second;
			window.extraLongs[index] = value;
			break;
		default: {
			auto old = window.extraLongs.find(index);
			if (old == window.extraLongs.end()) {
				cpu->r[EAX] = 0u;
				setWin32LastError(1413u); // ERROR_INVALID_INDEX.
				return;
			}
			previous = old->second;
			old->second = value;
			break;
		}
		}
		snapshot = window;
		if (index == -16) {
			const bool wasEnabled = (previous & kWsDisabled) == 0u;
			const bool isEnabled = (value & kWsDisabled) == 0u;
			if (wasEnabled != isEnabled) window.enabled = isEnabled;
		}
	}
	if (index == -16 && ((previous ^ value) & kWsDisabled)) {
		uint32_t ignored = 0u;
		if (!callWindowProcedure(cpu, snapshot.windowProcedure, handle,
			0x000au, (value & kWsDisabled) ? 0u : 1u, 0u, ignored)) return; // WM_ENABLE.
	}
	cpu->r[EAX] = previous;
}

void getClientRect(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	const uint32_t output = argument(cpu, 1);
	if (!output || !sharedGuestAddressSpace().host(output, 16u)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS.
		return;
	}
	GuestWindow window;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		window = found->second;
	}
	const GuestWindowFrameInsets insets = guestWindowFrameInsets(
		window.style, window.extendedStyle, window.menu != 0u);
	const uint32_t clientWidth = window.width > insets.left + insets.right
		? window.width - insets.left - insets.right : 0u;
	const uint32_t clientHeight = window.height > insets.top + insets.bottom
		? window.height - insets.top - insets.bottom : 0u;
	guest::store<int32_t>(output, 0);
	guest::store<int32_t>(output + 4u, 0);
	guest::store<int32_t>(output + 8u, static_cast<int32_t>(clientWidth));
	guest::store<int32_t>(output + 12u, static_cast<int32_t>(clientHeight));
	cpu->r[EAX] = 1u;
}

void adjustWindowRect(CPU* cpu, const ApiImportStub&)
{
	const uint32_t rect = argument(cpu, 0);
	const uint32_t style = argument(cpu, 1);
	const bool hasMenu = argument(cpu, 2) != 0u;
	const uint32_t extendedStyle = argument(cpu, 3);
	if (!rect || !sharedGuestAddressSpace().host(rect, 16u)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS.
		return;
	}
	const GuestWindowFrameInsets insets = guestWindowFrameInsets(style, extendedStyle, hasMenu);
	const int64_t left = static_cast<int64_t>(guest::load<int32_t>(rect)) - insets.left;
	const int64_t top = static_cast<int64_t>(guest::load<int32_t>(rect + 4u)) - insets.top;
	const int64_t right = static_cast<int64_t>(guest::load<int32_t>(rect + 8u)) + insets.right;
	const int64_t bottom = static_cast<int64_t>(guest::load<int32_t>(rect + 12u)) + insets.bottom;
	if (left < std::numeric_limits<int32_t>::min() || left > std::numeric_limits<int32_t>::max() ||
		top < std::numeric_limits<int32_t>::min() || top > std::numeric_limits<int32_t>::max() ||
		right < std::numeric_limits<int32_t>::min() || right > std::numeric_limits<int32_t>::max() ||
		bottom < std::numeric_limits<int32_t>::min() || bottom > std::numeric_limits<int32_t>::max()) {
		cpu->r[EAX] = 0u;
		setWin32LastError(534u); // ERROR_ARITHMETIC_OVERFLOW.
		return;
	}
	guest::store<int32_t>(rect, static_cast<int32_t>(left));
	guest::store<int32_t>(rect + 4u, static_cast<int32_t>(top));
	guest::store<int32_t>(rect + 8u, static_cast<int32_t>(right));
	guest::store<int32_t>(rect + 12u, static_cast<int32_t>(bottom));
	cpu->r[EAX] = 1u;
}

void setRect(CPU* cpu, const ApiImportStub&)
{
	const uint32_t rect = argument(cpu, 0);
	if (!rect || !sharedGuestAddressSpace().host(rect, 16u)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS.
		return;
	}
	for (uint32_t i = 0; i < 4u; ++i)
		guest::store<int32_t>(rect + i * 4u, static_cast<int32_t>(argument(cpu, i + 1u)));
	cpu->r[EAX] = 1u;
}

void setWindowPos(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kSwpNoSize = 0x0001u, kSwpNoMove = 0x0002u;
	constexpr uint32_t kSwpNoZOrder = 0x0004u, kSwpNoActivate = 0x0010u;
	constexpr uint32_t kSwpFrameChanged = 0x0020u, kSwpShowWindow = 0x0040u;
	constexpr uint32_t kSwpHideWindow = 0x0080u, kSwpNoSendChanging = 0x0400u;
	constexpr uint32_t kWsVisible = 0x10000000u;
	const uint32_t handle = argument(cpu, 0), insertAfter = argument(cpu, 1);
	const uint32_t flags = argument(cpu, 6);
	if ((flags & ~0x7fffu) || ((flags & kSwpShowWindow) && (flags & kSwpHideWindow))) {
		cpu->r[EAX] = 0u;
		setWin32LastError(87u); // ERROR_INVALID_PARAMETER.
		return;
	}
	GuestWindow window;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		if (!(flags & kSwpNoZOrder) && insertAfter && insertAfter != 1u && insertAfter != 0xffffu &&
			insertAfter != 0xfffffffeu && !gGuestWindows.count(insertAfter)) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // Invalid insert-after HWND.
			return;
		}
		window = found->second;
	}
	if (!(flags & kSwpNoMove)) { window.x = argument(cpu, 2); window.y = argument(cpu, 3); }
	if (!(flags & kSwpNoSize)) { window.width = argument(cpu, 4); window.height = argument(cpu, 5); }
	if (flags & kSwpShowWindow) window.style |= kWsVisible;
	if (flags & kSwpHideWindow) window.style &= ~kWsVisible;
	guest::store<uint32_t>(kWindowPosAddress + 0u, handle);
	guest::store<uint32_t>(kWindowPosAddress + 4u, insertAfter);
	guest::store<uint32_t>(kWindowPosAddress + 8u, window.x);
	guest::store<uint32_t>(kWindowPosAddress + 12u, window.y);
	guest::store<uint32_t>(kWindowPosAddress + 16u, window.width);
	guest::store<uint32_t>(kWindowPosAddress + 20u, window.height);
	guest::store<uint32_t>(kWindowPosAddress + 24u, flags);
	uint32_t ignored = 0u;
	if (!(flags & kSwpNoSendChanging) && !callWindowProcedure(cpu, window.windowProcedure,
	handle, 0x0046u, 0u, kWindowPosAddress, ignored)) return;
	window.x = guest::load<uint32_t>(kWindowPosAddress + 8u);
	window.y = guest::load<uint32_t>(kWindowPosAddress + 12u);
	window.width = guest::load<uint32_t>(kWindowPosAddress + 16u);
	window.height = guest::load<uint32_t>(kWindowPosAddress + 20u);
	const uint32_t finalFlags = guest::load<uint32_t>(kWindowPosAddress + 24u);
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u);
			return;
		}
		const bool visibleBefore = (found->second.style & kWsVisible) != 0u;
		found->second.x = window.x;
		found->second.y = window.y;
		found->second.width = window.width;
		found->second.height = window.height;
		found->second.style = window.style;
		if ((finalFlags & (kSwpShowWindow | kSwpHideWindow)) && visibleBefore != ((window.style & kWsVisible) != 0u)) {
			// WM_SHOWWINDOW is sent after the position notifications below.
		}
	}
	guest::store<uint32_t>(kWindowPosAddress + 24u, finalFlags);
	if (!callWindowProcedure(cpu, window.windowProcedure, handle,
		0x0047u, 0u, kWindowPosAddress, ignored)) return;
	if ((finalFlags & (kSwpShowWindow | kSwpHideWindow)) != 0u &&
		!callWindowProcedure(cpu, window.windowProcedure, handle,
			0x0018u, (window.style & kWsVisible) ? 1u : 0u, 0u, ignored)) return;
	if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
		std::fprintf(stderr, "[br-window] SetWindowPos hwnd=%08x xy=%u,%u size=%ux%u flags=%08x\n",
			handle, window.x, window.y, window.width, window.height, finalFlags);
	cpu->r[EAX] = 1u;
}

void showWindow(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kWsVisible = 0x10000000u;
	const uint32_t handle = argument(cpu, 0);
	const uint32_t command = argument(cpu, 1);
	GuestWindow window;
	bool wasVisible = false;
	bool isVisible = command != 0u;
	uint32_t previousProcedure = 0u;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		wasVisible = (found->second.style & kWsVisible) != 0u;
		previousProcedure = found->second.windowProcedure;
		if (isVisible) found->second.style |= kWsVisible;
		else found->second.style &= ~kWsVisible;
		if (wasVisible != isVisible) found->second.updateRegionInvalid = isVisible;
		window = found->second;
	}
	if (wasVisible != isVisible) {
		uint32_t ignored = 0u;
		if (!callWindowProcedure(cpu, previousProcedure, handle, 0x0018u,
			isVisible ? 1u : 0u, 0u, ignored)) return; // WM_SHOWWINDOW.
	}
	cpu->r[EAX] = wasVisible ? 1u : 0u;
}

void enableWindow(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kWsDisabled = 0x08000000u;
	const uint32_t handle = argument(cpu, 0);
	const bool enable = argument(cpu, 1) != 0u;
	uint32_t procedure = 0u;
	bool wasDisabled = false;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		wasDisabled = !found->second.enabled;
		procedure = found->second.windowProcedure;
		found->second.enabled = enable;
		if (enable) found->second.style &= ~kWsDisabled;
		else found->second.style |= kWsDisabled;
	}
	if (wasDisabled == enable) {
		uint32_t ignored = 0u;
		if (!callWindowProcedure(cpu, procedure, handle, 0x000au, enable ? 1u : 0u, 0u, ignored)) return;
	}
	cpu->r[EAX] = wasDisabled ? 1u : 0u;
}

void updateWindow(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	uint32_t procedure = 0u;
	bool invalid = false;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		procedure = found->second.windowProcedure;
		invalid = found->second.updateRegionInvalid;
	}
	if (invalid) {
		uint32_t ignored = 0u;
		if (!callWindowProcedure(cpu, procedure, handle, 0x000fu, 0u, 0u, ignored)) return; // WM_PAINT.
	}
	cpu->r[EAX] = 1u;
}

void destroyWindow(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0);
	GuestWindow window;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(handle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		window = found->second;
	}
	uint32_t ignored = 0u;
	if (!callWindowProcedure(cpu, window.windowProcedure, handle, 0x0002u, 0u, 0u, ignored) ||
		!callWindowProcedure(cpu, window.windowProcedure, handle, 0x0082u, 0u, 0u, ignored)) return;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		gGuestWindows.erase(handle);
		if (gActiveGuestWindow == handle) gActiveGuestWindow = 0u;
		if (gForegroundGuestWindow == handle) gForegroundGuestWindow = 0u;
	}
	{
		std::lock_guard<std::mutex> lock(gGuestMessageMutex);
		gGuestMessageQueue.erase(std::remove_if(gGuestMessageQueue.begin(), gGuestMessageQueue.end(),
			[handle](const GuestMessage& message) { return message.window == handle; }),
			gGuestMessageQueue.end());
	}
	cpu->r[EAX] = 1u;
}

void setForegroundWindow(CPU* cpu, const ApiImportStub&)
{
	const uint32_t window = argument(cpu, 0);
	GuestWindow guestWindow;
	bool activateApplication = false;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(window);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE
			return;
		}
		guestWindow = found->second;
		activateApplication = !gGuestApplicationActive;
		gGuestApplicationActive = true;
		gForegroundGuestWindow = window;
		gActiveGuestWindow = window;
	}
	uint32_t ignored = 0u;
	// The first transition of this guest process into the foreground sends
	// WM_ACTIVATEAPP before the per-window WM_ACTIVATE/WM_SETFOCUS messages.
	// Battle Realms' original WndProc uses this process-level notification to
	// switch its message pump from GetMessageA to PeekMessageA.
	if ((activateApplication && !callWindowProcedure(cpu, guestWindow.windowProcedure, window,
		0x001cu, 1u, 0u, ignored)) ||
		!callWindowProcedure(cpu, guestWindow.windowProcedure, window,
		0x0006u, 1u, 0u, ignored) || // WM_ACTIVATE / WA_ACTIVE
		!callWindowProcedure(cpu, guestWindow.windowProcedure, window,
			0x0007u, 0u, 0u, ignored)) return; // WM_SETFOCUS
	cpu->r[EAX] = 1u;
}

void drawMenuBar(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kSwpNoSize = 0x0001u;
	constexpr uint32_t kSwpNoMove = 0x0002u;
	constexpr uint32_t kSwpNoZOrder = 0x0004u;
	constexpr uint32_t kSwpNoActivate = 0x0010u;
	constexpr uint32_t kSwpFrameChanged = 0x0020u;
	const uint32_t windowHandle = argument(cpu, 0);
	GuestWindow window;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(windowHandle);
		if (found == gGuestWindows.end()) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
		window = found->second;
	}

	// DrawMenuBar forces a nonclient/frame recalculation even when this window
	// has no application menu. Match the underlying SetWindowPos notifications
	// without moving/resizing or changing activation/z-order.
	constexpr uint32_t kWindowPosFlags = kSwpNoSize | kSwpNoMove | kSwpNoZOrder |
		kSwpNoActivate | kSwpFrameChanged;
	guest::store<uint32_t>(kWindowPosAddress + 0u, windowHandle);
	guest::store<uint32_t>(kWindowPosAddress + 4u, 0u);
	guest::store<uint32_t>(kWindowPosAddress + 8u, window.x);
	guest::store<uint32_t>(kWindowPosAddress + 12u, window.y);
	guest::store<uint32_t>(kWindowPosAddress + 16u, window.width);
	guest::store<uint32_t>(kWindowPosAddress + 20u, window.height);
	guest::store<uint32_t>(kWindowPosAddress + 24u, kWindowPosFlags);
	uint32_t ignored = 0u;
	if (!callWindowProcedure(cpu, window.windowProcedure, windowHandle,
		0x0046u, 0u, kWindowPosAddress, ignored) ||
		!callWindowProcedure(cpu, window.windowProcedure, windowHandle,
			0x0047u, 0u, kWindowPosAddress, ignored)) return;
	if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
		std::fprintf(stderr, "[br-window] DrawMenuBar hwnd=%08x menu=%08x framechange-flags=%08x result=1\n",
			windowHandle, window.menu, kWindowPosFlags);
	cpu->r[EAX] = 1u;
}

void redrawWindow(CPU* cpu, const ApiImportStub&)
{
	const uint32_t windowHandle = argument(cpu, 0);
	const uint32_t rect = argument(cpu, 1);
	const uint32_t region = argument(cpu, 2);
	const uint32_t flags = argument(cpu, 3);
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		if (!gGuestWindows.count(windowHandle)) {
			cpu->r[EAX] = 0u;
			setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE.
			return;
		}
	}

	// The first guest call is RedrawWindow(hwnd, NULL, NULL, RDW_FRAME).
	// Win32 specifies that RDW_FRAME has no effect unless RDW_INVALIDATE is also
	// present, so this valid-window call succeeds without inventing a paint.
	if (flags == 0x0400u && !rect && !region) {
		if (std::getenv("BR_TRACE_WINDOW_MESSAGES"))
			std::fprintf(stderr, "[br-window] RedrawWindow hwnd=%08x flags=%08x result=1 effect=none-without-RDW_INVALIDATE\n",
				windowHandle, flags);
		cpu->r[EAX] = 1u;
		return;
	}
	const uint32_t stack = cpu->r[ESP];
	const uint32_t caller = guest::load<uint32_t>(stack);
	unsupported(cpu, caller, "RedrawWindow flags/region semantics");
}

void setActiveWindow(CPU* cpu, const ApiImportStub&)
{
	const uint32_t window = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	if (!gGuestWindows.count(window)) {
		cpu->r[EAX] = 0;
		setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE
		return;
	}
	cpu->r[EAX] = gActiveGuestWindow;
	gActiveGuestWindow = window;
}

void bringWindowToTop(CPU* cpu, const ApiImportStub&)
{
	const uint32_t window = argument(cpu, 0);
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	if (!gGuestWindows.count(window)) {
		cpu->r[EAX] = 0;
		setWin32LastError(1400u); // ERROR_INVALID_WINDOW_HANDLE
		return;
	}
	// The guest window model has no native z-order yet. Keep its activation
	// state consistent with the documented BringWindowToTop side effect.
	gForegroundGuestWindow = window;
	gActiveGuestWindow = window;
	cpu->r[EAX] = 1u;
}

void showCursor(CPU* cpu, const ApiImportStub&)
{
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	if (argument(cpu, 0)) ++gCursorDisplayCount;
	else --gCursorDisplayCount;
	cpu->r[EAX] = static_cast<uint32_t>(gCursorDisplayCount);
}

bool guestClientOriginOnScreen(uint32_t windowHandle, int64_t& originX,
	int64_t& originY, uint32_t& error);

void getCursorPos(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kPointSize = 8u;
	const uint32_t output = argument(cpu, 0);
	if (!output || !sharedGuestAddressSpace().host(output, kPointSize)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS for an invalid guest output pointer.
		return;
	}
	int32_t x = 0;
	int32_t y = 0;
	bool havePosition = false;
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		x = gDirectInputPointerX;
		y = gDirectInputPointerY;
		havePosition = gHaveGuestMousePosition;
	}
	int64_t originX = 0;
	int64_t originY = 0;
	uint32_t window = 0u;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		window = gActiveGuestWindow ? gActiveGuestWindow : gForegroundGuestWindow;
	}
	uint32_t originError = 0u;
	if (window) (void)guestClientOriginOnScreen(window, originX, originY, originError);
	// The host input bridge stores guest-client coordinates. GetCursorPos
	// exposes desktop coordinates, so translate through the active guest
	// window's modeled screen origin. With no host pointer event yet, the
	// virtual cursor begins at screen origin (0, 0).
	if (havePosition) {
		const int64_t screenX = static_cast<int64_t>(x) + originX;
		const int64_t screenY = static_cast<int64_t>(y) + originY;
		x = static_cast<int32_t>(static_cast<uint32_t>(screenX));
		y = static_cast<int32_t>(static_cast<uint32_t>(screenY));
	}
	guest::store<int32_t>(output, x);
	guest::store<int32_t>(output + 4u, y);
	cpu->r[EAX] = 1u;
	if (std::getenv("BR_TRACE_INPUT"))
		std::fprintf(stderr, "[br-input] GetCursorPos hwnd=%08x screen=%d,%d client-event=%u\n",
			window, x, y, havePosition ? 1u : 0u);
}

bool guestClientOriginOnScreen(uint32_t windowHandle, int64_t& originX,
	int64_t& originY, uint32_t& error)
{
	constexpr uint32_t kWsChild = 0x40000000u;
	constexpr uint32_t kInvalidWindowHandle = 1400u;

	originX = originY = 0;
	std::array<uint32_t, 64> visited{};
	uint32_t visitedCount = 0;
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	uint32_t current = windowHandle;
	while (current) {
		if (visitedCount == visited.size() ||
			std::find(visited.begin(), visited.begin() + visitedCount, current) !=
				visited.begin() + visitedCount) {
			error = kInvalidWindowHandle;
			return false;
		}
		visited[visitedCount++] = current;
		const auto found = gGuestWindows.find(current);
		if (found == gGuestWindows.end()) {
			error = kInvalidWindowHandle;
			return false;
		}
		const GuestWindow& window = found->second;
		originX += static_cast<int32_t>(window.x);
		originY += static_cast<int32_t>(window.y);
		const GuestWindowFrameInsets insets = guestWindowFrameInsets(
			window.style, window.extendedStyle, window.menu != 0u);
		originX += insets.left;
		originY += insets.top;
		if (!(window.style & kWsChild)) break;
		if (!window.parent) {
			error = kInvalidWindowHandle;
			return false;
		}
		current = window.parent;
	}
	return true;
}

void setCursorPos(CPU* cpu, const ApiImportStub&)
{
	const int32_t screenX = static_cast<int32_t>(argument(cpu, 0));
	const int32_t screenY = static_cast<int32_t>(argument(cpu, 1));
	const uint32_t window = activeGuestWindowHandle();
	int64_t originX = 0;
	int64_t originY = 0;
	uint32_t error = 0u;
	if (window && !guestClientOriginOnScreen(window, originX, originY, error)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(error);
		return;
	}
	const int64_t clientX64 = static_cast<int64_t>(screenX) - originX;
	const int64_t clientY64 = static_cast<int64_t>(screenY) - originY;
	if (clientX64 < std::numeric_limits<int32_t>::min() || clientX64 > std::numeric_limits<int32_t>::max() ||
		clientY64 < std::numeric_limits<int32_t>::min() || clientY64 > std::numeric_limits<int32_t>::max()) {
		cpu->r[EAX] = 0u;
		setWin32LastError(534u); // ERROR_ARITHMETIC_OVERFLOW.
		return;
	}
	const int32_t clientX = static_cast<int32_t>(clientX64);
	const int32_t clientY = static_cast<int32_t>(clientY64);
	if (window) {
		// Match the mouse-move side effect of repositioning the system cursor,
		// while storing coordinates in this guest window's client space.
		postGuestMouseMessage(0x0200u, clientX, clientY, 0u);
	} else {
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		const int32_t dx = gHaveGuestMousePosition ? clientX - gLastGuestMouseX : 0;
		const int32_t dy = gHaveGuestMousePosition ? clientY - gLastGuestMouseY : 0;
		gDirectInputMouseX += dx;
		gDirectInputMouseY += dy;
		gDirectInputPointerX = gLastGuestMouseX = clientX;
		gDirectInputPointerY = gLastGuestMouseY = clientY;
		gHaveGuestMousePosition = true;
	}
	cpu->r[EAX] = 1u;
	if (std::getenv("BR_TRACE_INPUT"))
		std::fprintf(stderr, "[br-input] SetCursorPos screen=%d,%d window=%08x client=%d,%d\n",
			screenX, screenY, window, clientX, clientY);
}

void clientToScreen(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kPointSize = 8u;
	const uint32_t windowHandle = argument(cpu, 0);
	const uint32_t pointAddress = argument(cpu, 1);
	if (!pointAddress || !sharedGuestAddressSpace().host(pointAddress, kPointSize)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS.
		return;
	}
	int64_t originX = 0;
	int64_t originY = 0;
	uint32_t error = 0u;
	if (!guestClientOriginOnScreen(windowHandle, originX, originY, error)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(error);
		return;
	}
	const int64_t screenX = static_cast<int64_t>(guest::load<int32_t>(pointAddress)) + originX;
	const int64_t screenY = static_cast<int64_t>(guest::load<int32_t>(pointAddress + 4u)) + originY;
	if (screenX < std::numeric_limits<int32_t>::min() || screenX > std::numeric_limits<int32_t>::max() ||
		screenY < std::numeric_limits<int32_t>::min() || screenY > std::numeric_limits<int32_t>::max()) {
		cpu->r[EAX] = 0u;
		setWin32LastError(534u); // ERROR_ARITHMETIC_OVERFLOW.
		return;
	}
	guest::store<int32_t>(pointAddress, static_cast<int32_t>(screenX));
	guest::store<int32_t>(pointAddress + 4u, static_cast<int32_t>(screenY));
	cpu->r[EAX] = 1u;
	if (std::getenv("BR_TRACE_INPUT"))
		std::fprintf(stderr, "[br-input] ClientToScreen hwnd=%08x origin=%lld,%lld client=%d,%d screen=%lld,%lld\n",
			windowHandle, static_cast<long long>(originX), static_cast<long long>(originY),
			static_cast<int32_t>(screenX - originX), static_cast<int32_t>(screenY - originY),
			static_cast<long long>(screenX), static_cast<long long>(screenY));
}

void screenToClient(CPU* cpu, const ApiImportStub&)
{
	constexpr uint32_t kPointSize = 8u;
	const uint32_t windowHandle = argument(cpu, 0);
	const uint32_t pointAddress = argument(cpu, 1);
	if (!pointAddress || !sharedGuestAddressSpace().host(pointAddress, kPointSize)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(998u); // ERROR_NOACCESS.
		return;
	}
	int64_t originX = 0;
	int64_t originY = 0;
	uint32_t error = 0u;
	if (!guestClientOriginOnScreen(windowHandle, originX, originY, error)) {
		cpu->r[EAX] = 0u;
		setWin32LastError(error);
		return;
	}
	const int32_t screenX = guest::load<int32_t>(pointAddress);
	const int32_t screenY = guest::load<int32_t>(pointAddress + 4u);
	const int64_t clientX = static_cast<int64_t>(screenX) - originX;
	const int64_t clientY = static_cast<int64_t>(screenY) - originY;
	if (clientX < std::numeric_limits<int32_t>::min() || clientX > std::numeric_limits<int32_t>::max() ||
		clientY < std::numeric_limits<int32_t>::min() || clientY > std::numeric_limits<int32_t>::max()) {
		cpu->r[EAX] = 0u;
		setWin32LastError(534u); // ERROR_ARITHMETIC_OVERFLOW.
		return;
	}
	guest::store<int32_t>(pointAddress, static_cast<int32_t>(clientX));
	guest::store<int32_t>(pointAddress + 4u, static_cast<int32_t>(clientY));
	cpu->r[EAX] = 1u;
	if (std::getenv("BR_TRACE_INPUT"))
		std::fprintf(stderr, "[br-input] ScreenToClient hwnd=%08x origin=%lld,%lld screen=%d,%d client=%lld,%lld\n",
			windowHandle, static_cast<long long>(originX), static_cast<long long>(originY),
			screenX, screenY, static_cast<long long>(clientX), static_cast<long long>(clientY));
}

void getAsyncKeyState(CPU* cpu, const ApiImportStub&)
{
	const uint32_t virtualKey = argument(cpu, 0) & 0xffffu;
	const uint32_t caller = sharedGuestAddressSpace().host(cpu->r[ESP], sizeof(uint32_t))
		? guest::load<uint32_t>(cpu->r[ESP]) : 0u;
	if (virtualKey >= gDirectInputKeyboardState.size()) {
		cpu->r[EAX] = 0u;
		return;
	}
	uint16_t result = 0u;
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		uint8_t keyState = gDirectInputKeyboardState[virtualKey];
		// GetAsyncKeyState shares one virtual-key space for keyboard and mouse.
		// The mouse buttons are also exposed through DirectInput, so use the same
		// state updated by the host mouse-message bridge instead of the unrelated
		// keyboard-state array.
		if (virtualKey == 0x01u) keyState = gDirectInputMouseButtons[0]; // VK_LBUTTON
		else if (virtualKey == 0x02u) keyState = gDirectInputMouseButtons[1]; // VK_RBUTTON
		else if (virtualKey == 0x04u) keyState = gDirectInputMouseButtons[2]; // VK_MBUTTON
		if (keyState & 0x80u) result |= 0x8000u;
		if (gAsyncKeyPressedSinceQuery[virtualKey]) result |= 0x0001u;
		gAsyncKeyPressedSinceQuery[virtualKey] = 0u;
	}
	cpu->r[EAX] = result;
	if (result && std::getenv("BR_TRACE_INPUT"))
		std::fprintf(stderr, "[br-input] GetAsyncKeyState vk=%04x result=%04x guest-caller=%08x\n",
			virtualKey, result, caller);
}

void messageBoxA(CPU* cpu, const ApiImportStub&)
{
	if (std::getenv("BR_TRACE_DDRAW")) {
		const std::string text = readGuestString(argument(cpu, 1));
		if (text.find("supported display mode") != std::string::npos) {
			std::fprintf(stderr, "[br-ddraw] game rejected display setup: devices_seen=%u devices_selected=%u enum_calls=%u device_index=%u gamma=%08x\n",
				guest::load<uint32_t>(0x0087a0c0u), guest::load<uint32_t>(0x0087a0d0u),
				guest::load<uint32_t>(0x0087a0bcu), guest::load<uint32_t>(0x0087a0c4u),
				guest::load<uint32_t>(0x0085be40u));
			const uint32_t record = 0x00865268u;
			const uint32_t modeCount = guest::load<uint32_t>(record + 0x3528u);
			std::fprintf(stderr, "[br-ddraw] selection globals: setting=%u setting_aux=%u setting_flags=%08x selected_flags=%08x\n",
				guest::load<uint32_t>(0x00879250u), guest::load<uint32_t>(0x00879254u),
				guest::load<uint32_t>(0x00879258u), guest::load<uint32_t>(0x0087a098u));
		std::fprintf(stderr, "[br-ddraw] selected record: enum_hwraster=%08x enum_hw_tnl=%08x d3d_caps=%08x render_depth=%08x z_depth=%08x mode_count=%u driver_mode=%08x chosen_mode=%u\n",
				guest::load<uint32_t>(record + 0x114u), guest::load<uint32_t>(record + 0x118u),
				guest::load<uint32_t>(record + 0x28u), guest::load<uint32_t>(record + 0x28u + 116u),
				guest::load<uint32_t>(record + 0x28u + 120u), modeCount,
				guest::load<uint32_t>(record + 0x3530u), guest::load<uint32_t>(0x0087a0c8u));
			const uint32_t modeBase = record + 0x4b8u;
			for (uint32_t i = 0; i < modeCount && i < 16u; ++i) {
				const uint32_t mode = modeBase + i * 0x7cu;
				std::fprintf(stderr, "[br-ddraw] selected mode[%u]: flags=%08x %ux%ux%u refresh=%u masks=%08x/%08x/%08x\n",
					i, guest::load<uint32_t>(mode + 4u), guest::load<uint32_t>(mode + 12u),
					guest::load<uint32_t>(mode + 8u), guest::load<uint32_t>(mode + 84u),
					guest::load<uint32_t>(mode + 24u), guest::load<uint32_t>(mode + 88u),
					guest::load<uint32_t>(mode + 92u), guest::load<uint32_t>(mode + 96u));
			}
		}
	}
	// Startup trace mode has no modal Android dialog bridge. MB_OK is the only
	// observed style; acknowledge it so guest startup can expose later blockers.
	if (argument(cpu, 3) == 0u) cpu->r[EAX] = 1u; // IDOK
	else cpu->r[EAX] = 0u;
}

void getWindow(CPU* cpu, const ApiImportStub&)
{
	const uint32_t handle = argument(cpu, 0), command = argument(cpu, 1);
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	const auto found = gGuestWindows.find(handle);
	if (found == gGuestWindows.end()) { cpu->r[EAX] = 0; return; }
	// The current guest window model has no sibling/owner graph. For the
	// observed GW_HWNDNEXT query, no peer has been created, so return NULL.
	if (command == 2u) { cpu->r[EAX] = 0; return; }
	cpu->r[EAX] = 0;
}

void systemParametersInfoA(CPU* cpu, const ApiImportStub&)
{
	// The observed guest call requests SPI_SETSCREENSAVEACTIVE. Android owns
	// display sleep policy, so acknowledge this legacy preference without
	// changing the host device's system settings.
	const uint32_t action = argument(cpu, 0);
	cpu->r[EAX] = action == 0x0011u ? 1u : 0u;
}
} // namespace

uint64_t guestPresentationCount()
{
	std::lock_guard<std::mutex> lock(gDdrawSurfacesMutex);
	return gGuestPresentationCount;
}

void renderWin32DirectSoundPcm(int16_t* output, uint32_t frames,
	uint32_t sampleRate, uint16_t channels, Win32AudioMixStats* stats)
{
	if (stats) *stats = {};
	if (!output || !sampleRate || (channels != 1u && channels != 2u)) return;
	const size_t sampleCount = static_cast<size_t>(frames) * channels;
	std::fill(output, output + sampleCount, int16_t{0});
	if (!frames) return;

	// Keep headroom for simultaneous effects/music, then clamp once after all
	// buffers have been accumulated. The callback reuses this per-thread scratch
	// storage, so steady-state audio rendering does not allocate.
	thread_local std::vector<int32_t> mixed;
	try {
		mixed.resize(sampleCount);
	} catch (...) {
		// Exceptions must never escape an Android real-time audio callback.
		return;
	}
	std::fill(mixed.begin(), mixed.end(), 0);
	{
		std::lock_guard<std::mutex> lock(gDirectSoundMutex);
		for (auto& entry : gDirectSoundBuffers) {
			auto& buffer = entry.second;
			if (!buffer.playing || buffer.locked || !buffer.data || !buffer.bytes ||
				!buffer.sampleRate || !buffer.frequency || !buffer.blockAlign ||
				(buffer.bitsPerSample != 8u && buffer.bitsPerSample != 16u) ||
				(buffer.channels != 1u && buffer.channels != 2u) ||
				buffer.blockAlign != buffer.channels * (buffer.bitsPerSample / 8u) ||
				buffer.bytes < buffer.blockAlign || buffer.bytes % buffer.blockAlign != 0u)
				continue;
			const uint32_t sourceFrames = buffer.bytes / buffer.blockAlign;
			const auto* pcm = static_cast<const uint8_t*>(sharedGuestAddressSpace().host(buffer.data, buffer.bytes));
			if (!pcm || !sourceFrames) continue;
			if (stats) ++stats->activeBuffers;

			const bool looping = (buffer.playFlags & 1u) != 0u;
			const double step = static_cast<double>(buffer.frequency) / sampleRate;
			const int32_t volumeDb = static_cast<int32_t>(buffer.volume);
			const double volume = volumeDb <= -10000 ? 0.0 : std::pow(10.0, static_cast<double>(volumeDb) / 2000.0);
			const double pan = static_cast<double>(static_cast<int32_t>(buffer.pan)) / 10000.0;
			const double leftGain = volume * (pan > 0.0 ? std::pow(10.0, -pan * 5.0) : 1.0);
			const double rightGain = volume * (pan < 0.0 ? std::pow(10.0, pan * 5.0) : 1.0);
			double phase = std::clamp(buffer.playbackPhase, 0.0, 0.999999999);
			uint32_t sourceFrame = std::min(buffer.playCursor / buffer.blockAlign, sourceFrames - 1u);
			bool stoppedAtEnd = false;

			const auto readSample = [&](uint32_t frame, uint16_t channel) -> int32_t {
				const uint16_t srcChannel = buffer.channels == 1u ? 0u : channel;
				const uint8_t* sample = pcm + static_cast<size_t>(frame) * buffer.blockAlign +
					static_cast<size_t>(srcChannel) * (buffer.bitsPerSample / 8u);
				if (buffer.bitsPerSample == 8u) return (static_cast<int32_t>(*sample) - 128) << 8;
				int16_t value = 0;
				std::memcpy(&value, sample, sizeof(value));
				return value;
			};
			for (uint32_t frame = 0; frame < frames; ++frame) {
				if (sourceFrame >= sourceFrames) {
					if (!looping) { stoppedAtEnd = true; break; }
					sourceFrame %= sourceFrames;
				}
				uint32_t nextFrame = sourceFrame + 1u;
				if (nextFrame >= sourceFrames) nextFrame = looping ? 0u : sourceFrames - 1u;
				const auto interpolate = [&](uint16_t sourceChannel) {
					const double a = readSample(sourceFrame, sourceChannel);
					const double b = readSample(nextFrame, sourceChannel);
					return static_cast<double>(a + (b - a) * phase);
				};
				const double left = interpolate(0u) * leftGain;
				const double right = (buffer.channels == 1u ? interpolate(0u) : interpolate(1u)) * rightGain;
				if (channels == 1u) {
					mixed[frame] += static_cast<int32_t>((left + right) * 0.5);
				} else {
					mixed[static_cast<size_t>(frame) * 2u] += static_cast<int32_t>(left);
					mixed[static_cast<size_t>(frame) * 2u + 1u] += static_cast<int32_t>(right);
				}
				phase += step;
				const uint32_t advance = static_cast<uint32_t>(phase);
				phase -= advance;
				sourceFrame += advance;
			}
			if (!looping && sourceFrame >= sourceFrames) {
				buffer.playing = false;
				buffer.playFlags = 0u;
				buffer.playCursor = buffer.bytes;
				buffer.playbackPhase = 0.0;
			} else {
				if (looping) sourceFrame %= sourceFrames;
				buffer.playCursor = sourceFrame * buffer.blockAlign;
				buffer.playbackPhase = phase;
			}
			if (stoppedAtEnd && !looping) {
				buffer.playing = false;
				buffer.playFlags = 0u;
				buffer.playCursor = buffer.bytes;
				buffer.playbackPhase = 0.0;
			}
			buffer.writeCursor = (buffer.playCursor + buffer.blockAlign) % buffer.bytes;
		}
	}
	for (size_t i = 0; i < sampleCount; ++i) {
		const int16_t outputSample = static_cast<int16_t>(std::clamp(mixed[i], -32768, 32767));
		if (stats) {
			if (mixed[i] < -32768 || mixed[i] > 32767) ++stats->clippedSamples;
			const uint32_t absolute = static_cast<uint32_t>(std::abs(static_cast<int32_t>(outputSample)));
			stats->peakAbsoluteSample = std::max(stats->peakAbsoluteSample, absolute);
			stats->silentSamples += outputSample == 0 ? 1u : 0u;
			const int64_t wideSample = outputSample;
			stats->sumSquaredSamples += static_cast<uint64_t>(wideSample * wideSample);
		}
		output[i] = outputSample;
	}
}

uint64_t synthesizedPaintMessageCount()
{
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	return gSynthesizedPaintMessageCount;
}

void configureWin32StartupContext(uint32_t mainModuleBase, uint32_t commandLineAddress,
	const std::string& mainModuleName, ImportBindingTable* imports)
{
	std::lock_guard<std::mutex> lock(gStartupContextMutex);
	gStartupContext.mainModuleBase = mainModuleBase;
	gStartupContext.commandLineAddress = commandLineAddress;
	gStartupContext.mainModuleName = mainModuleName;
	gStartupContext.imports = imports;
}

void configureWin32StartupDisplayMetrics(uint32_t width, uint32_t height)
{
	std::lock_guard<std::mutex> lock(gStartupContextMutex);
	gStartupContext.screenWidth = width;
	gStartupContext.screenHeight = height;
}

void configureWin32FramePresenter(Win32FramePresenter presenter)
{
	std::lock_guard<std::mutex> lock(gFramePresenterMutex);
	gFramePresenter = presenter;
}

Win32FrameRenderStats consumeWin32FrameRenderStats()
{
#if defined(__ANDROID__)
	return {gProfileD3dDrawCalls.exchange(0u, std::memory_order_relaxed),
		gProfileD3dRasterNs.exchange(0u, std::memory_order_relaxed),
		gProfileDdrawFlips.exchange(0u, std::memory_order_relaxed),
		gProfileGpuDrawCalls.exchange(0u, std::memory_order_relaxed),
		gProfileSoftwareFallbackDrawCalls.exchange(0u, std::memory_order_relaxed),
		gProfileGpuTriangles.exchange(0u, std::memory_order_relaxed),
		gProfileSoftwareFallbackTriangles.exchange(0u, std::memory_order_relaxed),
		d3d7::consumeGpuBackendBatchSubmissions(),
		gProfileD3dSetupNs.exchange(0u, std::memory_order_relaxed),
		gProfileD3dGeometryNs.exchange(0u, std::memory_order_relaxed),
		gProfileD3dGpuSubmitNs.exchange(0u, std::memory_order_relaxed),
		gProfileD3dFallbackNs.exchange(0u, std::memory_order_relaxed),
		gProfileD3dSyncReadbackNs.exchange(0u, std::memory_order_relaxed),
		d3d7::consumeGpuBackendSubmitNanoseconds()};
#else
	return {};
#endif
}

uint32_t activeGuestWindowHandle()
{
	std::lock_guard<std::mutex> lock(gWindowClassesMutex);
	if (gActiveGuestWindow && gGuestWindows.count(gActiveGuestWindow)) return gActiveGuestWindow;
	if (gForegroundGuestWindow && gGuestWindows.count(gForegroundGuestWindow)) return gForegroundGuestWindow;
	for (const auto& entry : gGuestWindows)
		if (entry.second.style & 0x10000000u) return entry.first;
	return 0u;
}

void activeGuestWindowSize(uint32_t& width, uint32_t& height)
{
	width = height = 0u;
	{
		std::lock_guard<std::mutex> lock(gWindowClassesMutex);
		const auto found = gGuestWindows.find(gActiveGuestWindow ? gActiveGuestWindow : gForegroundGuestWindow);
		if (found != gGuestWindows.end()) {
			width = found->second.width;
			height = found->second.height;
		}
	}
	if (!width || !height) {
		const StartupContext context = startupContext();
		if (!width) width = context.screenWidth;
		if (!height) height = context.screenHeight;
	}
}

void postGuestKeyMessage(uint32_t virtualKey, bool down)
{
	if (virtualKey >= gDirectInputKeyboardState.size()) return;
	bool wasDown = false;
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		wasDown = (gDirectInputKeyboardState[virtualKey] & 0x80u) != 0u;
		gDirectInputKeyboardState[virtualKey] = down ? 0x80u : 0u;
		if (down && !wasDown) gAsyncKeyPressedSinceQuery[virtualKey] = 1u;
	}
	const uint32_t window = activeGuestWindowHandle();
	if (!window) return;
	GuestMessage message;
	message.window = window;
	message.message = down ? 0x0100u : 0x0101u; // WM_KEYDOWN / WM_KEYUP.
	message.wParam = virtualKey;
	message.lParam = 1u | (wasDown ? (1u << 30) : 0u) |
		(down ? 0u : ((1u << 30) | (1u << 31)));
	message.time = guestMessageTime();
	enqueueGuestMessage(message);
}

void postGuestMouseMessage(uint32_t messageId, int32_t x, int32_t y, uint32_t wParam)
{
	const uint32_t window = activeGuestWindowHandle();
	{
		std::lock_guard<std::mutex> lock(gDirectInputMutex);
		const int32_t dx = gHaveGuestMousePosition ? x - gLastGuestMouseX : 0;
		const int32_t dy = gHaveGuestMousePosition ? y - gLastGuestMouseY : 0;
		gDirectInputMouseX += dx;
		gDirectInputMouseY += dy;
		gDirectInputPointerX = x;
		gDirectInputPointerY = y;
		gLastGuestMouseX = x;
		gLastGuestMouseY = y;
		gHaveGuestMousePosition = true;
		if (messageId == 0x0201u || messageId == 0x0202u) {
			const bool wasDown = (gDirectInputMouseButtons[0] & 0x80u) != 0u;
			const bool down = messageId == 0x0201u;
			gDirectInputMouseButtons[0] = down ? 0x80u : 0u;
			if (down && !wasDown) gAsyncKeyPressedSinceQuery[0x01u] = 1u; // VK_LBUTTON edge.
		} else if (messageId == 0x0204u || messageId == 0x0205u) {
			const bool wasDown = (gDirectInputMouseButtons[1] & 0x80u) != 0u;
			const bool down = messageId == 0x0204u;
			gDirectInputMouseButtons[1] = down ? 0x80u : 0u;
			if (down && !wasDown) gAsyncKeyPressedSinceQuery[0x02u] = 1u; // VK_RBUTTON edge.
		} else if (messageId == 0x0207u || messageId == 0x0208u) {
			const bool wasDown = (gDirectInputMouseButtons[2] & 0x80u) != 0u;
			const bool down = messageId == 0x0207u;
			gDirectInputMouseButtons[2] = down ? 0x80u : 0u;
			if (down && !wasDown) gAsyncKeyPressedSinceQuery[0x04u] = 1u; // VK_MBUTTON edge.
		}
		else if (messageId == 0x020au) {
			const int32_t delta = static_cast<int16_t>(wParam >> 16);
			gDirectInputMouseWheel += delta;
		}
	}
	// Async key state and DirectInput device state represent host input, even
	// when no guest HWND currently exists to receive a queued window message.
	// Match postGuestKeyMessage(): update the shared input state first, then
	// deliver the corresponding guest message only when a window is active.
	if (!window) return;
	GuestMessage event;
	event.window = window;
	event.message = messageId;
	event.wParam = wParam;
	event.lParam = (static_cast<uint32_t>(static_cast<uint16_t>(x))) |
		(static_cast<uint32_t>(static_cast<uint16_t>(y)) << 16);
	event.time = guestMessageTime();
	event.x = x;
	event.y = y;
	enqueueGuestMessage(event);
}

void postGuestQuitMessage(uint32_t exitCode)
{
	GuestMessage message;
	message.message = 0x0012u;
	message.wParam = exitCode;
	message.time = guestMessageTime();
	enqueueGuestMessage(message);
}

void configureWin32StartupLocale(uint32_t userDefaultLcid, uint32_t ansiCodePage)
{
	std::lock_guard<std::mutex> lock(gStartupContextMutex);
	gStartupContext.userDefaultLcid = userDefaultLcid ? userDefaultLcid : 0x0409u;
	gStartupContext.ansiCodePage = ansiCodePage ? ansiCodePage : 1252u;
}

bool registerWin32StartupHandlers(ImportDispatchRegistry& handlers)
{
	return handlers.registerName("kernel32.dll", "GetCommandLineA", getCommandLineA, 0) &&
		handlers.registerName("kernel32.dll", "GetVersionExA", getVersionExA, 4) &&
		handlers.registerName("msacm32.dll", "acmGetVersion", acmGetVersion, 0) &&
		handlers.registerName("msacm32.dll", "acmStreamOpen", acmStreamOpen, 32) &&
		handlers.registerName("msacm32.dll", "acmStreamPrepareHeader", acmStreamPrepareHeader, 12) &&
		handlers.registerName("msacm32.dll", "acmStreamConvert", acmStreamConvert, 12) &&
		handlers.registerName("msacm32.dll", "acmStreamUnprepareHeader", acmStreamUnprepareHeader, 12) &&
		handlers.registerName("msacm32.dll", "acmStreamClose", acmStreamClose, 8) &&
		handlers.registerName("kernel32.dll", "GetEnvironmentStrings", getEnvironmentStringsA, 0) &&
		handlers.registerName("kernel32.dll", "GetEnvironmentStringsW", getEnvironmentStringsW, 0) &&
		handlers.registerName("kernel32.dll", "FreeEnvironmentStringsA", freeEnvironmentStringsA, 4) &&
		handlers.registerName("kernel32.dll", "FreeEnvironmentStringsW", freeEnvironmentStringsW, 4) &&
		handlers.registerName("kernel32.dll", "GetStdHandle", getStdHandle, 4) &&
		handlers.registerName("kernel32.dll", "SetHandleCount", setHandleCount, 4) &&
		handlers.registerName("kernel32.dll", "GetACP", getAcp, 0) &&
		handlers.registerName("kernel32.dll", "IsDBCSLeadByteEx", isDbcsLeadByteEx, 8) &&
		handlers.registerName("kernel32.dll", "GetCPInfo", getCPInfo, 8) &&
		handlers.registerName("kernel32.dll", "GetStringTypeW", getStringTypeW, 16) &&
		handlers.registerName("kernel32.dll", "LCMapStringW", lcMapStringW, 24) &&
		handlers.registerName("kernel32.dll", "MultiByteToWideChar", multiByteToWideChar, 24) &&
		handlers.registerName("kernel32.dll", "WideCharToMultiByte", wideCharToMultiByte, 32) &&
		handlers.registerName("kernel32.dll", "GetStartupInfoA", getStartupInfoA, 4) &&
		handlers.registerName("kernel32.dll", "GetModuleHandleA", getModuleHandleA, 4) &&
		handlers.registerName("kernel32.dll", "LoadLibraryA", loadLibraryA, 4) &&
		handlers.registerName("kernel32.dll", "GetProcAddress", getProcAddress, 8) &&
		handlers.registerName("kernel32.dll", "FreeLibrary", freeLibrary, 4) &&
		handlers.registerName("kernel32.dll", "IsDebuggerPresent", isDebuggerPresent, 0) &&
		handlers.registerName("ddraw.dll", "DirectDrawEnumerateExA", directDrawEnumerateExA, 12) &&
		handlers.registerName("ddraw.dll", "DirectDrawCreateEx", directDrawCreateEx, 16) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::QueryInterface", ddrawQueryInterface, 12) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::AddRef", ddrawAddRef, 4) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::Release", ddrawRelease, 4) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::FlipToGDISurface", ddrawFlipToGdiSurface, 4) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::SetCooperativeLevel", ddrawSetCooperativeLevel, 12) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::TestCooperativeLevel", ddrawTestCooperativeLevel, 4) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::GetCaps", ddrawGetCaps, 12) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::EnumDisplayModes", ddrawEnumDisplayModes, 20) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::GetDisplayMode", ddrawGetDisplayMode, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::GetDeviceIdentifier", ddrawGetDeviceIdentifier, 12) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::SetDisplayMode", ddrawSetDisplayMode, 24) &&
		handlers.registerName("ddraw.dll", "IDirectDraw7::CreateSurface", ddrawCreateSurface, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::QueryInterface", ddrawQueryInterface, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::AddRef", ddrawAddRef, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::Release", ddrawRelease, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::EnumDevices", d3d7EnumDevices, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::CreateDevice", d3d7CreateDevice, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::CreateVertexBuffer", d3d7CreateVertexBuffer, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3D7::EnumZBufferFormats", d3d7EnumZBufferFormats, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::AddRef", d3d7DeviceAddRef, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::Release", d3d7DeviceRelease, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::BeginScene", d3d7DeviceBeginScene, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::EndScene", d3d7DeviceEndScene, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::DrawPrimitive", d3d7DeviceDrawPrimitive, 24) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::DrawPrimitiveVB", d3d7DeviceDrawPrimitiveVB, 24) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::DrawIndexedPrimitive", d3d7DeviceDrawIndexedPrimitive, 32) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::DrawIndexedPrimitiveVB", d3d7DeviceDrawIndexedPrimitiveVB, 32) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetRenderTarget", d3d7DeviceSetRenderTarget, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetRenderTarget", d3d7DeviceGetRenderTarget, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetTransform", d3d7DeviceSetTransform, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetTransform", d3d7DeviceGetTransform, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetViewport", d3d7DeviceSetViewport, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetViewport", d3d7DeviceGetViewport, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetRenderState", d3d7DeviceSetRenderState, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetMaterial", d3d7DeviceSetMaterial, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetMaterial", d3d7DeviceGetMaterial, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetLight", d3d7DeviceSetLight, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetLight", d3d7DeviceGetLight, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::LightEnable", d3d7DeviceLightEnable, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetLightEnable", d3d7DeviceGetLightEnable, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetCaps", d3d7DeviceGetCaps, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetTextureStageState", d3d7DeviceSetTextureStageState, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::SetTexture", d3d7DeviceSetTexture, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::GetTextureStageState", d3d7DeviceGetTextureStageState, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3DVertexBuffer7::AddRef", d3d7VertexBufferAddRef, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DVertexBuffer7::Release", d3d7VertexBufferRelease, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DVertexBuffer7::Lock", d3d7VertexBufferLock, 16) &&
		handlers.registerName("ddraw.dll", "IDirect3DVertexBuffer7::Unlock", d3d7VertexBufferUnlock, 4) &&
		handlers.registerName("ddraw.dll", "IDirect3DVertexBuffer7::GetVertexBufferDesc", d3d7VertexBufferGetDesc, 8) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::EnumTextureFormats", d3d7DeviceEnumTextureFormats, 12) &&
		handlers.registerName("ddraw.dll", "IDirect3DDevice7::Clear", d3d7DeviceClear, 24) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::AddRef", ddrawSurfaceAddRef, 4) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::Release", ddrawSurfaceRelease, 4) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::GetDDInterface", ddrawSurfaceGetDdInterface, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::AddAttachedSurface", ddrawSurfaceAddAttached, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::DeleteAttachedSurface", ddrawSurfaceDeleteAttached, 12) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::GetCaps", ddrawSurfaceGetCaps, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::GetPixelFormat", ddrawSurfaceGetPixelFormat, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::GetSurfaceDesc", ddrawSurfaceGetDesc, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::Blt", ddrawSurfaceBlt, 24) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::Lock", ddrawSurfaceLock, 20) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::Unlock", ddrawSurfaceUnlock, 8) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::GetAttachedSurface", ddrawSurfaceGetAttachedSurface, 12) &&
		handlers.registerName("ddraw.dll", "IDirectDrawSurface7::Flip", ddrawSurfaceFlip, 12) &&
		handlers.registerName("dinput.dll", "DirectInputCreateEx", directInputCreateEx, 20) &&
		handlers.registerName("dsound.dll", "DirectSoundCreate", directSoundCreate, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound::QueryInterface", directSoundQueryInterface, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound::AddRef", directSoundAddRef, 4) &&
		handlers.registerName("dsound.dll", "IDirectSound::Release", directSoundRelease, 4) &&
		handlers.registerName("dsound.dll", "IDirectSound::CreateSoundBuffer", directSoundCreateBuffer, 16) &&
		handlers.registerName("dsound.dll", "IDirectSound::GetCaps", directSoundUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound::DuplicateSoundBuffer", directSoundUnsupported, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound::SetCooperativeLevel", directSoundSetCooperativeLevel, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound::Compact", directSoundUnsupported, 4) &&
		handlers.registerName("dsound.dll", "IDirectSound::GetSpeakerConfig", directSoundUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound::SetSpeakerConfig", directSoundUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound::Initialize", directSoundUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::QueryInterface", directSoundBufferQueryInterface, 12) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::AddRef", directSoundBufferAddRef, 4) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Release", directSoundBufferRelease, 4) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetCaps", directSoundBufferGetCaps, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetCurrentPosition", directSoundBufferGetCurrentPosition, 12) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetFormat", directSoundBufferGetFormat, 16) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetVolume", directSoundBufferGetVolume, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetPan", directSoundBufferGetPan, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetFrequency", directSoundBufferGetFrequency, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::GetStatus", directSoundBufferGetStatus, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Initialize", directSoundUnsupported, 12) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Lock", directSoundBufferLock, 32) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Play", directSoundBufferPlay, 16) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::SetCurrentPosition", directSoundBufferSetCurrentPosition, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::SetFormat", directSoundBufferSetFormat, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::SetVolume", directSoundBufferSetVolume, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::SetPan", directSoundBufferSetPan, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::SetFrequency", directSoundBufferSetFrequency, 8) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Stop", directSoundBufferStop, 4) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Unlock", directSoundBufferUnlock, 20) &&
		handlers.registerName("dsound.dll", "IDirectSoundBuffer::Restore", directSoundUnsupported, 4) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::QueryInterface", directSoundBufferQueryInterface, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::AddRef", directSound3DListenerAddRef, 4) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::Release", directSound3DListenerRelease, 4) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetAllParameters", directSound3DListenerGetAllParameters, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetDistanceFactor", directSound3DListenerUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetDopplerFactor", directSound3DListenerUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetOrientation", directSound3DListenerUnsupported, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetPosition", directSound3DListenerUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetRolloffFactor", directSound3DListenerUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::GetVelocity", directSound3DListenerUnsupported, 8) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetAllParameters", directSound3DListenerUnsupported, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetDistanceFactor", directSound3DListenerUnsupported, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetDopplerFactor", directSound3DListenerUnsupported, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetOrientation", directSound3DListenerUnsupported, 32) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetPosition", directSound3DListenerUnsupported, 20) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetRolloffFactor", directSound3DListenerSetRolloffFactor, 12) &&
		handlers.registerName("dsound.dll", "IDirectSound3DListener::SetVelocity", directSound3DListenerUnsupported, 20) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::QueryInterface", dinputInterfaceQueryInterface, 12) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::AddRef", dinputInterfaceAddRef, 4) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::Release", dinputInterfaceRelease, 4) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::CreateDevice", dinputCreateDevice, 16) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::EnumDevices", dinputUnsupported, 20) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::GetDeviceStatus", dinputGetDeviceStatus, 8) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::RunControlPanel", dinputUnsupported, 12) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::Initialize", dinputUnsupported, 12) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::FindDevice", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::CreateDeviceEx", dinputCreateDeviceEx, 20) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::EnumDevicesBySemantics", dinputUnsupported, 24) &&
		handlers.registerName("dinput.dll", "IDirectInput7A::ConfigureDevices", dinputUnsupported, 20) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::QueryInterface", dinputDeviceQueryInterface, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::AddRef", dinputDeviceAddRef, 4) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::Release", dinputDeviceRelease, 4) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetCapabilities", dinputDeviceGetCapabilities, 8) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::EnumObjects", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetProperty", dinputDeviceGetProperty, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SetProperty", dinputDeviceSetProperty, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::Acquire", dinputDeviceAcquire, 4) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::Unacquire", dinputDeviceUnacquire, 4) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetDeviceState", dinputDeviceGetState, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetDeviceData", dinputDeviceGetData, 20) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SetDataFormat", dinputDeviceSetDataFormat, 8) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SetEventNotification", dinputDeviceSetEventNotification, 8) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SetCooperativeLevel", dinputDeviceSetCooperativeLevel, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetObjectInfo", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetDeviceInfo", dinputDeviceGetInfo, 8) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::RunControlPanel", dinputUnsupported, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::Initialize", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::CreateEffect", dinputUnsupported, 20) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::EnumEffects", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetEffectInfo", dinputUnsupported, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetForceFeedbackState", dinputUnsupported, 8) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SendForceFeedbackCommand", dinputUnsupported, 8) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::EnumCreatedEffectObjects", dinputUnsupported, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::Escape", dinputUnsupported, 12) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::Poll", dinputDevicePoll, 4) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SendDeviceData", dinputUnsupported, 20) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::EnumEffectsInFile", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::WriteEffectToFile", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::BuildActionMap", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::SetActionMap", dinputUnsupported, 16) &&
		handlers.registerName("dinput.dll", "IDirectInputDevice7A::GetImageInfo", dinputUnsupported, 8) &&
		handlers.registerName("user32.dll", "FindWindowA", findWindowA, 8) &&
		handlers.registerName("user32.dll", "PeekMessageA", peekMessageA, 20) &&
		handlers.registerName("user32.dll", "GetMessageA", getMessageA, 16) &&
		handlers.registerName("user32.dll", "SendMessageA", sendMessageA, 16) &&
		handlers.registerName("user32.dll", "TranslateMessage", translateMessage, 4) &&
		handlers.registerName("user32.dll", "DispatchMessageA", dispatchMessageA, 4) &&
		handlers.registerName("user32.dll", "PostQuitMessage", postQuitMessage, 4) &&
		handlers.registerName("user32.dll", "GetKeyboardLayout", getKeyboardLayout, 4) &&
		handlers.registerName("user32.dll", "RegisterWindowMessageA", registerWindowMessageA, 4) &&
		handlers.registerName("user32.dll", "LoadIconA", loadIconA, 8) &&
		handlers.registerName("user32.dll", "LoadCursorA", loadCursorA, 8) &&
		handlers.registerName("user32.dll", "RegisterClassA", registerClassA, 4) &&
		handlers.registerName("user32.dll", "CreateWindowExA", createWindowExA, 48) &&
		handlers.registerName("user32.dll", "SetForegroundWindow", setForegroundWindow, 4) &&
		handlers.registerName("user32.dll", "DrawMenuBar", drawMenuBar, 4) &&
		handlers.registerName("user32.dll", "RedrawWindow", redrawWindow, 16) &&
		handlers.registerName("user32.dll", "SetActiveWindow", setActiveWindow, 4) &&
		handlers.registerName("user32.dll", "BringWindowToTop", bringWindowToTop, 4) &&
		handlers.registerName("user32.dll", "GetClientRect", getClientRect, 8) &&
		handlers.registerName("user32.dll", "GetWindowLongA", getWindowLongA, 8) &&
		handlers.registerName("user32.dll", "SetWindowLongA", setWindowLongA, 12) &&
		handlers.registerName("user32.dll", "SetWindowPos", setWindowPos, 28) &&
		handlers.registerName("user32.dll", "AdjustWindowRect", adjustWindowRect, 16) &&
		handlers.registerName("user32.dll", "SetRect", setRect, 20) &&
		handlers.registerName("user32.dll", "ShowWindow", showWindow, 8) &&
		handlers.registerName("user32.dll", "UpdateWindow", updateWindow, 4) &&
		handlers.registerName("user32.dll", "EnableWindow", enableWindow, 8) &&
		handlers.registerName("user32.dll", "DestroyWindow", destroyWindow, 4) &&
		handlers.registerName("user32.dll", "ShowCursor", showCursor, 4) &&
		handlers.registerName("user32.dll", "GetCursorPos", getCursorPos, 4) &&
		handlers.registerName("user32.dll", "SetCursorPos", setCursorPos, 8) &&
		handlers.registerName("user32.dll", "ClientToScreen", clientToScreen, 8) &&
		handlers.registerName("user32.dll", "ScreenToClient", screenToClient, 8) &&
		handlers.registerName("user32.dll", "GetAsyncKeyState", getAsyncKeyState, 4) &&
		handlers.registerName("user32.dll", "MessageBoxA", messageBoxA, 16) &&
		handlers.registerName("user32.dll", "GetWindow", getWindow, 8) &&
		handlers.registerName("user32.dll", "SystemParametersInfoA", systemParametersInfoA, 16) &&
		handlers.registerName("user32.dll", "GetSystemMetrics", getSystemMetrics, 4) &&
		handlers.registerName("kernel32.dll", "GetUserDefaultLCID", getUserDefaultLcid, 0) &&
		handlers.registerName("kernel32.dll", "GetLocaleInfoA", getLocaleInfoA, 16) &&
		handlers.registerName("user32.dll", "DefWindowProcA", defWindowProcA, 16) &&
		handlers.registerName("gdi32.dll", "GetStockObject", getStockObject, 4);
}

} // namespace br::runtime
