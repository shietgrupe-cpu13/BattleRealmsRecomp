#include "d3d7_batch_state.h"

int main()
{
	using namespace br::runtime::d3d7;
	GpuDrawState first{};
	first.activeTextureStages = 1u;
	first.renderState[7] = 1u;
	first.renderState[137] = 1u; // Lighting is already resolved into submitted vertices.
	first.textureStageState[0][18] = 3u; // Not consumed by this fixed-function backend.

	GpuDrawState next = first;
	next.renderState[137] = 0u;
	next.textureStageState[0][18] = 9u;
	if (!sameGpuBatchState(first, next)) return 1;

	next = first;
	next.renderState[7] = 0u;
	if (sameGpuBatchState(first, next)) return 2;
	next = first;
	next.textureStageState[0][2] = 2u;
	if (sameGpuBatchState(first, next)) return 3;
	next = first;
	next.activeTextureStages = 2u;
	if (sameGpuBatchState(first, next)) return 4;

	const GpuFragmentShaderKey shaderKey = makeGpuFragmentShaderKey(first);
	next = first;
	next.renderState[24] = 127u; // Alpha reference is a uniform, not shader source.
	next.renderState[34] = 0xff123456u; // Fog color is a uniform.
	next.textureStageState[0][17] = 2u; // Minification filter is sampler state.
	if (!(shaderKey == makeGpuFragmentShaderKey(next))) return 5;
	next = first;
	next.renderState[25] = 4u;
	if (shaderKey == makeGpuFragmentShaderKey(next)) return 6;
	next = first;
	next.textureStageState[0][11] = 1u;
	if (shaderKey == makeGpuFragmentShaderKey(next)) return 7;
	return 0;
}
