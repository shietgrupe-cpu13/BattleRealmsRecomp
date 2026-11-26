#include "d3d7_scanline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {
struct Point { float x, y; };

float edge(Point a, Point b, float x, float y)
{
	return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

bool topLeft(Point a, Point b)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

uint32_t nextRandom(uint32_t& value)
{
	value = value * 1664525u + 1013904223u;
	return value;
}

bool verifyTriangle(Point a, Point b, Point c)
{
	constexpr int32_t width = 64;
	float area = edge(a, b, c.x, c.y);
	if (std::fabs(area) < 1.0e-8f) return true;
	if (area < 0.0f) std::swap(b, c);
	const float minX = std::min({a.x, b.x, c.x});
	const float maxX = std::max({a.x, b.x, c.x});
	const float minY = std::min({a.y, b.y, c.y});
	const float maxY = std::max({a.y, b.y, c.y});
	const int32_t x0 = std::max<int32_t>(0, static_cast<int32_t>(std::ceil(minX - 0.5f)));
	const int32_t x1 = std::min<int32_t>(width - 1, static_cast<int32_t>(std::floor(maxX - 0.5f)));
	const int32_t y0 = std::max<int32_t>(0, static_cast<int32_t>(std::ceil(minY - 0.5f)));
	const int32_t y1 = std::min<int32_t>(width - 1, static_cast<int32_t>(std::floor(maxY - 0.5f)));
	if (x1 < x0 || y1 < y0) return true;
	const bool tl0 = topLeft(b, c), tl1 = topLeft(c, a), tl2 = topLeft(a, b);
	const float step0 = b.y - c.y, step1 = c.y - a.y, step2 = a.y - b.y;
	const float stepY0 = c.x - b.x, stepY1 = a.x - c.x, stepY2 = b.x - a.x;
	float row0 = edge(b, c, float(x0) + 0.5f, float(y0) + 0.5f);
	float row1 = edge(c, a, float(x0) + 0.5f, float(y0) + 0.5f);
	float row2 = edge(a, b, float(x0) + 0.5f, float(y0) + 0.5f);
	for (int32_t y = y0; y <= y1; ++y) {
		const std::array<br::runtime::d3d7::ScanlineEdge, 3> edges{{
			{row0, step0, tl0}, {row1, step1, tl1}, {row2, step2, tl2}}};
		const auto span = br::runtime::d3d7::computeTriangleScanlineSpan(x0, x1, edges);
		for (int32_t x = x0; x <= x1; ++x) {
			const float offset = float(x - x0);
			const bool reference = br::runtime::d3d7::scanlineEdgePasses(row0 + step0 * offset, tl0) &&
				br::runtime::d3d7::scanlineEdgePasses(row1 + step1 * offset, tl1) &&
				br::runtime::d3d7::scanlineEdgePasses(row2 + step2 * offset, tl2);
			bool optimized = false;
			if (span.status == br::runtime::d3d7::ScanlineSpanStatus::Validated)
				optimized = x >= span.firstPixel && x <= span.lastPixel;
			else if (span.status == br::runtime::d3d7::ScanlineSpanStatus::Fallback)
				optimized = reference; // Runtime retains the original edge checks for this row.
			if (reference != optimized) return false;
		}
		row0 += stepY0; row1 += stepY1; row2 += stepY2;
	}
	return true;
}
} // namespace

int main()
{
	// Reproduce the raster loop's empty-first-row case. The next row must use
	// advanced edge values or the remaining triangle is lost as well.
	std::array<float, 3> advancingEdges{{-0.5f, -0.5f, -0.5f}};
	const std::array<float, 3> edgeStepsY{{1.0f, 1.0f, 1.0f}};
	const std::array<br::runtime::d3d7::ScanlineEdge, 3> firstEmptyEdges{{
		{advancingEdges[0], 0.0f, false}, {advancingEdges[1], 0.0f, false},
		{advancingEdges[2], 0.0f, false}}};
	if (br::runtime::d3d7::computeTriangleScanlineSpan(0, 8, firstEmptyEdges).status !=
		br::runtime::d3d7::ScanlineSpanStatus::Empty) return 3;
	br::runtime::d3d7::advanceTriangleScanlineEdges(advancingEdges[0], advancingEdges[1],
		advancingEdges[2], edgeStepsY[0], edgeStepsY[1], edgeStepsY[2]);
	const std::array<br::runtime::d3d7::ScanlineEdge, 3> secondRowEdges{{
		{advancingEdges[0], 0.0f, false}, {advancingEdges[1], 0.0f, false},
		{advancingEdges[2], 0.0f, false}}};
	if (br::runtime::d3d7::computeTriangleScanlineSpan(0, 8, secondRowEdges).status !=
		br::runtime::d3d7::ScanlineSpanStatus::Validated) {
		std::cerr << "FAIL an empty scanline did not advance edge state to the next covered row\n";
		return 4;
	}
	for (const auto& triangle : std::array<std::array<Point, 3>, 8>{{
		{{{0.0f, 0.0f}, {64.0f, 0.0f}, {0.0f, 64.0f}}},
		{{{0.5f, 0.5f}, {63.5f, 0.5f}, {63.5f, 63.5f}}},
		{{{-20.0f, 18.25f}, {77.0f, 39.5f}, {8.0f, 61.75f}}},
		{{{12.0f, 8.0f}, {12.0f, 56.0f}, {52.0f, 32.0f}}},
		{{{4.5f, 4.5f}, {20.5f, 20.5f}, {36.5f, 36.5f}}},
		{{{2.0f, 2.0f}, {62.0f, 2.0f}, {32.0f, 62.0f}}},
		{{{1.25f, 60.75f}, {61.75f, 1.5f}, {62.25f, 61.5f}}},
		{{{-8.0f, -8.0f}, {72.0f, -8.0f}, {32.0f, 72.0f}}},
	}}) {
		if (!verifyTriangle(triangle[0], triangle[1], triangle[2])) {
			std::cerr << "FAIL targeted scanline coverage differs from edge-test reference\n";
			return 1;
		}
	}
	uint32_t random = 0x7b31d94fu;
	for (uint32_t i = 0u; i < 20000u; ++i) {
		Point points[3]{};
		for (Point& point : points) {
			point.x = float(int32_t(nextRandom(random) % 401u) - 168) * 0.25f;
			point.y = float(int32_t(nextRandom(random) % 401u) - 168) * 0.25f;
		}
		if (!verifyTriangle(points[0], points[1], points[2])) {
			std::cerr << "FAIL randomized top-left coverage mismatch triangle=" << i << '\n';
			return 2;
		}
	}
	std::cout << "PASS 20,008 scanline spans exactly match top-left edge coverage\n";
	return 0;
}
