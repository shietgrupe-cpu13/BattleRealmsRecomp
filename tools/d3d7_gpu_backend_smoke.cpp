#include "d3d7_gpu_backend.h"

int main()
{
	using namespace br::runtime::d3d7;
	GpuDrawState state{};
	GpuVertex triangle[3]{};
	triangle[0].x = 0.0f; triangle[0].y = 0.0f;
	triangle[1].x = 1.0f; triangle[1].y = 0.0f;
	triangle[2].x = 0.0f; triangle[2].y = 1.0f;
	if (renderGpuTriangles(state, triangle, 3u)) return 1;
	if (!synchronizeGpuSurface(state.target)) return 2;
	markGpuSurfaceCpuWrite(1u);
	invalidateGpuSurface(1u);
	resetGpuRenderer();
	return 0;
}
