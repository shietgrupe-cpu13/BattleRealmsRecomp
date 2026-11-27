#pragma once

#include "d3d7_gpu_backend.h"

#include <cstddef>
#include <limits>
#include <vector>

namespace br::runtime::d3d7 {

// Keep compatible draw-call appends amortized O(1): reserving exactly the next
// size can reallocate and copy the accumulated batch for every appended draw.
inline bool appendGpuVertices(std::vector<GpuVertex>& batch,
	const GpuVertex* vertices, size_t count)
{
	if (count == 0u) return true;
	if (!vertices || count > batch.max_size() - batch.size()) return false;
	const size_t required = batch.size() + count;
	if (required > batch.capacity()) {
		size_t capacity = batch.capacity();
		if (capacity == 0u) capacity = 64u;
		while (capacity < required) {
			if (capacity > batch.max_size() / 2u) {
				capacity = required;
				break;
			}
			capacity *= 2u;
		}
		batch.reserve(capacity);
	}
	batch.insert(batch.end(), vertices, vertices + count);
	return true;
}

} // namespace br::runtime::d3d7
