#include "d3d7_vertex_batch.h"

#include <cstddef>
#include <vector>

int main()
{
	using namespace br::runtime::d3d7;
	std::vector<GpuVertex> batch;
	GpuVertex triangle[3]{};
	triangle[0].x = 10.0f;
	triangle[1].y = 20.0f;
	triangle[2].z = 30.0f;
	if (!appendGpuVertices(batch, triangle, 3u) || batch.size() != 3u) return 1;
	const size_t firstCapacity = batch.capacity();
	if (firstCapacity < 3u) return 2;

	size_t capacityChanges = 0u;
	for (size_t i = 1u; i < 1000u; ++i) {
		const size_t oldCapacity = batch.capacity();
		if (!appendGpuVertices(batch, triangle, 3u)) return 3;
		if (batch.capacity() != oldCapacity) ++capacityChanges;
	}
	if (batch.size() != 3000u || capacityChanges > 8u) return 4;
	if (batch.front().x != 10.0f || batch[1].y != 20.0f || batch.back().z != 30.0f) return 5;
	if (!appendGpuVertices(batch, nullptr, 0u) || appendGpuVertices(batch, nullptr, 1u)) return 6;
	return 0;
}
