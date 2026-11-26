#pragma once

#include "import_binder.h"

#include "cpu.h"

#include <cstdint>
#include <map>
#include <string>
#include <cstddef>

namespace br::runtime {

using ImportedCallHandler = void (*)(CPU*, const ApiImportStub&);
using GuestImportLookup = const ApiImportStub* (*)(const void* context, uint32_t guestAddress);

struct ImportedCall {
	ImportedCallHandler handler = nullptr;
	uint32_t stackPopBytes = 0;
};

class ImportDispatchRegistry {
public:
	bool registerName(const std::string& library, const std::string& name,
		ImportedCallHandler handler, uint32_t stackPopBytes);
	bool registerOrdinal(const std::string& library, uint16_t ordinal,
		ImportedCallHandler handler, uint32_t stackPopBytes);
	const ImportedCall* find(const ApiImportStub& import) const;
	size_t size() const { return handlers_.size(); }

private:
	std::map<std::string, ImportedCall> handlers_;
};

// Configure once after the PE imports and API handlers have been registered.
// The resolver should map a guest IAT stub address back to its PE import record.
void configureLiftDispatch(GuestImportLookup importLookup, const void* importContext,
	const ImportDispatchRegistry* handlers);
void configureLiftDispatch(const ImportBindingTable& imports,
	const ImportDispatchRegistry* handlers);
[[noreturn]] void dispatchFailure(CPU* cpu, uint32_t guestAddress, const char* reason);

} // namespace br::runtime

// ABI expected by generated functions and RA3's lifted CPU helper header.
void call_target(CPU* cpu, uint32_t guestAddress);
void call_target_ic(CPU* cpu, uint32_t guestAddress, std::atomic<uint64_t>* inlineCache);
// Call a lifted Win32 stdcall callback (e.g. WndProc) from a native shim.
bool callGuestStdcall(CPU* cpu, uint32_t guestAddress, const uint32_t* arguments,
	uint32_t argumentCount, uint32_t& result, std::string& error);
[[noreturn]] void unsupported(CPU* cpu, uint32_t guestAddress, const char* instruction);
void ra3_hook_after(CPU* cpu, uint32_t function, uint32_t a0, uint32_t a1,
	uint32_t a2, uint32_t ecx);
bool ra3_gate(CPU* cpu, uint32_t function);

extern uint32_t g_eflags_hi;
extern uint32_t ra3_stub_tolower;
