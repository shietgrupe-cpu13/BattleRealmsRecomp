#pragma once

#include "import_binder.h"
#include "lift_dispatch.h"
#include "cpu.h"

#include <string>
#include <vector>

namespace br::runtime {

// Owns the mapped main PE and its IAT/API-stub table for the lifetime of the
// process. This prepares the executable for lifted dispatch; it does not run
// its entrypoint or claim that Win32 startup is complete.
class MainExecutableRuntime {
public:
	bool initialize(const std::vector<uint8_t>& file, GuestAddressSpace& guest,
		const ImportDispatchRegistry& handlers, std::string& error);
	bool prepareInitialCpu(CPU& cpu, GuestAddressSpace& guest, std::string& error);

	bool initialized() const { return initialized_; }
	const pe32::Image& image() const { return image_; }
	const ImportBindingTable& imports() const { return imports_; }
	const ImportBindStats& stats() const { return stats_; }

private:
	pe32::Image image_;
	ImportBindingTable imports_;
	ImportBindStats stats_{};
	bool initialized_ = false;
	bool attempted_ = false;
};

} // namespace br::runtime
