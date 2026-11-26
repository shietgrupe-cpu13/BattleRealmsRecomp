#include "import_binder.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace br::runtime {
namespace {
std::string lower(std::string value)
{
	std::transform(value.begin(), value.end(), value.begin(),
		[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

std::string apiKey(const pe32::Import& import)
{
	return lower(import.library) + "!" + (import.byOrdinal
		? "O:" + std::to_string(import.ordinal)
		: "N:" + import.name);
}

const pe32::Export* findExport(const pe32::Image& image, const pe32::Import& import)
{
	for (const auto& candidate : image.exports) {
		if (import.byOrdinal) {
			if (candidate.ordinal == import.ordinal) return &candidate;
		} else if (std::find(candidate.names.begin(), candidate.names.end(), import.name) != candidate.names.end()) {
			return &candidate;
		}
	}
	return nullptr;
}

bool writeGuest32(GuestAddressSpace& guest, uint32_t address, uint32_t value)
{
	auto* slot = guest.host(address, 4);
	if (!slot) return false;
	for (unsigned i = 0; i < 4; ++i) slot[i] = static_cast<uint8_t>(value >> (i * 8));
	return true;
}
}

uint32_t ImportBindingTable::getOrCreateStub(const pe32::Import& import, std::string& error)
{
	const std::string key = apiKey(import);
	if (auto found = apiStubByKey_.find(key); found != apiStubByKey_.end()) {
		auto& stub = apiStubs_[stubIndexByAddress_.at(found->second)];
		++stub.referenceCount;
		return found->second;
	}
	const uint64_t address = uint64_t(StubBase) + uint64_t(apiStubs_.size()) * StubStride;
	if (address > StubLimit || address > std::numeric_limits<uint32_t>::max()) {
		error = "guest API import stub range exhausted";
		return 0;
	}
	ApiImportStub stub;
	stub.library = import.library;
	stub.name = import.name;
	stub.ordinal = import.ordinal;
	stub.byOrdinal = import.byOrdinal;
	stub.guestAddress = static_cast<uint32_t>(address);
	stub.referenceCount = 1;
	const size_t index = apiStubs_.size();
	apiStubs_.push_back(std::move(stub));
	apiStubByKey_.emplace(key, static_cast<uint32_t>(address));
	stubIndexByAddress_.emplace(static_cast<uint32_t>(address), index);
	return static_cast<uint32_t>(address);
}

uint32_t ImportBindingTable::getOrCreateDynamicStub(const std::string& library,
	const std::string& name, std::string& error)
{
	pe32::Import import;
	import.library = library;
	import.name = name;
	return getOrCreateStub(import, error);
}

bool ImportBindingTable::bindImageImports(pe32::Image& target, const pe32::Image& executable,
	const std::string& executableModule, GuestAddressSpace& guest,
	ImportBindStats& stats, std::string& error)
{
	stats = {};
	if (!guest.containsMapped(target.loadBase, target.sizeOfImage)) {
		error = "target image is not mapped in guest address space";
		return false;
	}
	const std::string exeName = lower(executableModule);
	for (const auto& import : target.imports) {
		const uint64_t slot64 = uint64_t(target.loadBase) + import.iatRva;
		if (slot64 > std::numeric_limits<uint32_t>::max() ||
			!guest.containsMapped(static_cast<uint32_t>(slot64), 4)) {
			error = "import address table slot is outside mapped image";
			return false;
		}
		uint32_t address = 0;
		if (lower(import.library) == exeName) {
			const pe32::Export* symbol = findExport(executable, import);
			if (!symbol || symbol->forwarder.size()) {
				error = "unresolved or forwarded Battle Realms executable import: " + import.library + "!" +
					(import.byOrdinal ? "#" + std::to_string(import.ordinal) : import.name);
				return false;
			}
			const uint64_t targetAddress = uint64_t(executable.loadBase) + symbol->rva;
			if (targetAddress > std::numeric_limits<uint32_t>::max()) {
				error = "resolved executable export address overflows guest address space";
				return false;
			}
			address = static_cast<uint32_t>(targetAddress);
			++stats.executableExportsResolved;
		} else {
			address = getOrCreateStub(import, error);
			if (!address) return false;
			++stats.apiImportReferences;
		}
		if (!writeGuest32(guest, static_cast<uint32_t>(slot64), address)) {
			error = "cannot write resolved import address to guest IAT";
			return false;
		}
		for (unsigned i = 0; i < 4; ++i)
			target.bytes[import.iatRva + i] = static_cast<uint8_t>(address >> (i * 8));
	}
	stats.uniqueApiStubs = static_cast<uint32_t>(apiStubs_.size());
	return true;
}

const ApiImportStub* ImportBindingTable::lookup(uint32_t guestAddress) const
{
	auto found = stubIndexByAddress_.find(guestAddress);
	return found == stubIndexByAddress_.end() ? nullptr : &apiStubs_[found->second];
}

} // namespace br::runtime
