#pragma once

#include "guest_address_space.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace br::runtime {

struct ApiImportStub {
	std::string library;
	std::string name;
	uint16_t ordinal = 0;
	uint32_t guestAddress = 0;
	uint32_t referenceCount = 0;
	bool byOrdinal = false;
};

struct ImportBindStats {
	uint32_t executableExportsResolved = 0;
	uint32_t apiImportReferences = 0;
	uint32_t uniqueApiStubs = 0;
};

// Patches a mapped image's IAT with direct game-EXE export addresses or
// fail-fast guest API stub addresses. It does not implement Win32 functions.
class ImportBindingTable {
public:
	static constexpr uint32_t StubBase = 0x70000000;
	static constexpr uint32_t StubStride = 16;
	static constexpr uint32_t StubLimit = 0x700ffff0;

	bool bindImageImports(pe32::Image& target, const pe32::Image& executable,
		const std::string& executableModule, GuestAddressSpace& guest,
		ImportBindStats& stats, std::string& error);
	uint32_t getOrCreateDynamicStub(const std::string& library, const std::string& name,
		std::string& error);
	const ApiImportStub* lookup(uint32_t guestAddress) const;
	const std::vector<ApiImportStub>& apiStubs() const { return apiStubs_; }

private:
	uint32_t getOrCreateStub(const pe32::Import& import, std::string& error);
	std::map<std::string, uint32_t> apiStubByKey_;
	std::map<uint32_t, size_t> stubIndexByAddress_;
	std::vector<ApiImportStub> apiStubs_;
};

} // namespace br::runtime
