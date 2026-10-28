#include "overlay/SelectionEngine.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string_view>

namespace {
std::size_t allocations{};
bool measuring{};
}  // namespace

void* operator new(const std::size_t size) {
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        if (measuring)
            ++allocations;
        return memory;
    }
    throw std::bad_alloc{};
}
void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

int main(int argc, char** argv) {
    const bool baseline = argc == 2 && std::string_view(argv[1]) == "--baseline";
    std::vector<orbitdeck::WheelItem> items(12);
    items[0].children.resize(8);
    items[0].children[0].children.resize(8);
    items[0].children[0].children[0].children.resize(8);
    const auto layout = orbitdeck::BuildWheelLayout(items, {0, 0, 0}, 66.0F, 156.0F);
    orbitdeck::SelectionEngine selection;
    selection.Configure(layout, 50.0F, 82.0F);
    struct Point {
        float x;
        float y;
    };
    std::array<Point, 1024> points{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float angle = static_cast<float>(i) * 0.31F;
        const float radius =
            i % 11 == 0 ? 20.0F : 100.0F + static_cast<float>((i / 32) % 4) * 70.0F;
        points[i] = {std::cos(angle) * radius, std::sin(angle) * radius};
    }
    std::array<double, 5> timings{};
    std::size_t checksum{};
    std::size_t totalAllocations{};
    constexpr std::size_t samples{20000};
    for (double& time : timings) {
        allocations = 0;
        selection.BeginGesture(0.0F, 0.0F);
        const auto start = std::chrono::steady_clock::now();
        measuring = true;
        for (std::size_t i = 0; i < samples; ++i) {
            const Point point = points[i % points.size()];
            const auto selected = selection.Update(point.x, point.y);
            checksum += selected.value_or(0);
        }
        measuring = false;
        time = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start)
                   .count() /
               samples;
        totalAllocations += allocations;
    }
    std::sort(timings.begin(), timings.end());
    std::printf(
        "{\"selectionUpdates\":%zu,\"allocations\":%zu,"
        "\"medianMicroseconds\":%.4f,\"worstRunMicroseconds\":%.4f,"
        "\"checksum\":%zu}\n",
        samples * timings.size(), totalAllocations, timings[2], timings.back(), checksum);
    return !baseline && totalAllocations != 0 ? 1 : 0;
}
