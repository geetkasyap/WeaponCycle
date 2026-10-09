#pragma once

namespace orbitdeck {

struct PixelPoint {
    int x;
    int y;
};

struct PixelRect {
    int left;
    int top;
    int right;
    int bottom;
};

struct SquarePlacement {
    int left;
    int top;
    int size;
};

[[nodiscard]] constexpr SquarePlacement CalculateSquarePlacement(const PixelPoint centre,
                                                                 const PixelRect bounds,
                                                                 const int requestedSize) noexcept {
    const long long width = static_cast<long long>(bounds.right) - bounds.left;
    const long long height = static_cast<long long>(bounds.bottom) - bounds.top;
    if (width <= 0 || height <= 0) {
        return {bounds.left, bounds.top, 0};
    }

    const long long available = width < height ? width : height;
    const long long requested = requestedSize > 0 ? requestedSize : 0;
    const long long size = requested < available ? requested : available;
    const long long maximumLeft = static_cast<long long>(bounds.right) - size;
    const long long maximumTop = static_cast<long long>(bounds.bottom) - size;
    const long long centredLeft = static_cast<long long>(centre.x) - size / 2;
    const long long centredTop = static_cast<long long>(centre.y) - size / 2;
    const long long left = centredLeft < bounds.left
                               ? bounds.left
                               : (centredLeft > maximumLeft ? maximumLeft : centredLeft);
    const long long top =
        centredTop < bounds.top ? bounds.top : (centredTop > maximumTop ? maximumTop : centredTop);

    return {static_cast<int>(left), static_cast<int>(top), static_cast<int>(size)};
}

}  // namespace orbitdeck
