#include "tools/MiniAppWindow.h"

#include <cmath>

int main() {
    double result = 0.0;
    if (!orbitdeck::EvaluateExpression(L"2 + 3 * 4", result) || std::fabs(result - 14.0) > 1.0e-9) {
        return 1;
    }
    if (!orbitdeck::EvaluateExpression(L"-(8 - 3) / 2", result) ||
        std::fabs(result + 2.5) > 1.0e-9) {
        return 2;
    }
    if (orbitdeck::EvaluateExpression(L"1 / 0", result) ||
        orbitdeck::EvaluateExpression(L"2 +", result) ||
        orbitdeck::EvaluateExpression(L"(3 * 4", result)) {
        return 3;
    }
    return 0;
}
