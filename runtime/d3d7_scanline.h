#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace br::runtime::d3d7 {

struct ScanlineEdge {
	float valueAtFirstPixel = 0.0f;
	float stepPerPixel = 0.0f;
	bool topLeft = false;
};

enum class ScanlineSpanStatus : uint8_t { Empty, Validated, Fallback };

struct ScanlineSpan {
	int32_t firstPixel = 0;
	int32_t lastPixel = -1;
	ScanlineSpanStatus status = ScanlineSpanStatus::Empty;
};

inline bool scanlineEdgePasses(float value, bool topLeft)
{
	return value >= 0.0f && (value != 0.0f || topLeft);
}

inline void advanceTriangleScanlineEdges(float& edge0, float& edge1, float& edge2,
	float stepY0, float stepY1, float stepY2)
{
	edge0 += stepY0;
	edge1 += stepY1;
	edge2 += stepY2;
}

// Intersect three linear edge tests with the integer pixel interval. The
// returned interval is accepted only after checking both boundaries against
// the original floating-point top-left predicate. Ill-conditioned boundaries
// request a reference edge-test fallback for that row.
inline ScanlineSpan computeTriangleScanlineSpan(int32_t firstPixel, int32_t lastPixel,
	const std::array<ScanlineEdge, 3>& edges)
{
	if (lastPixel < firstPixel) return {};
	int32_t firstOffset = 0;
	int32_t lastOffset = lastPixel - firstPixel;
	for (const ScanlineEdge& edge : edges) {
		if (edge.stepPerPixel > 0.0f) {
			const float crossing = -edge.valueAtFirstPixel / edge.stepPerPixel;
			if (crossing > float(lastOffset) + 1.0f) firstOffset = lastOffset + 1;
			else if (crossing > 0.0f)
				firstOffset = std::max(firstOffset, static_cast<int32_t>(std::ceil(crossing)));
		} else if (edge.stepPerPixel < 0.0f) {
			const float crossing = -edge.valueAtFirstPixel / edge.stepPerPixel;
			if (crossing < -1.0f) lastOffset = -1;
			else if (crossing < float(lastOffset))
				lastOffset = std::min(lastOffset, static_cast<int32_t>(std::floor(crossing)));
		} else if (!scanlineEdgePasses(edge.valueAtFirstPixel, edge.topLeft)) {
			firstOffset = lastOffset + 1;
		}
	}
	if (firstOffset > lastOffset) return {};

	ScanlineSpan span{firstPixel + firstOffset, firstPixel + lastOffset,
		ScanlineSpanStatus::Fallback};
	const auto covered = [&](int32_t pixel) {
		const float offset = float(pixel - firstPixel);
		for (const ScanlineEdge& edge : edges)
			if (!scanlineEdgePasses(edge.valueAtFirstPixel + edge.stepPerPixel * offset, edge.topLeft))
				return false;
		return true;
	};
	for (uint32_t correction = 0u; span.firstPixel <= span.lastPixel &&
		correction < 2u && !covered(span.firstPixel); ++correction) ++span.firstPixel;
	for (uint32_t correction = 0u; span.firstPixel <= span.lastPixel &&
		correction < 2u && !covered(span.lastPixel); ++correction) --span.lastPixel;
	if (span.firstPixel <= span.lastPixel && covered(span.firstPixel) && covered(span.lastPixel))
		span.status = ScanlineSpanStatus::Validated;
	else {
		span.firstPixel = firstPixel;
		span.lastPixel = lastPixel;
	}
	return span;
}

} // namespace br::runtime::d3d7
