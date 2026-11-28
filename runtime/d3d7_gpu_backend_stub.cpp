#include "d3d7_gpu_backend.h"

namespace br::runtime::d3d7 {

bool renderGpuTriangles(const GpuDrawState&, const GpuVertex*, uint32_t) noexcept
{
	return false;
}

uint64_t consumeGpuBackendBatchSubmissions() noexcept
{
	return 0u;
}

uint64_t consumeGpuBackendSubmitNanoseconds() noexcept
{
	return 0u;
}

uint64_t consumeGpuBackendSkippedStateCalls() noexcept
{
	return 0u;
}

bool synchronizeGpuSurface(const GpuSurface&) noexcept
{
	return true;
}

void markGpuSurfaceCpuWrite(uint32_t) noexcept {}
void invalidateGpuSurface(uint32_t) noexcept {}
void resetGpuRenderer() noexcept {}

} // namespace br::runtime::d3d7
