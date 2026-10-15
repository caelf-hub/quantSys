#pragma once

#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace research {

// Solves A x = b for a dense k-by-k row-major A by Gauss-Jordan elimination
// with partial pivoting. Returns false when a pivot is below 1e-12.
inline bool solve_linear(std::vector<double> a, std::vector<double> b, size_t k,
                         std::vector<double>& x) {
    if (a.size() != k * k || b.size() != k) return false;
    for (size_t col = 0; col < k; ++col) {
        size_t pivot = col;
        for (size_t r = col + 1; r < k; ++r) {
            if (std::fabs(a[r * k + col]) > std::fabs(a[pivot * k + col])) pivot = r;
        }
        if (std::fabs(a[pivot * k + col]) < 1e-12) return false;
        if (pivot != col) {
            for (size_t j = 0; j < k; ++j) std::swap(a[col * k + j], a[pivot * k + j]);
            std::swap(b[col], b[pivot]);
        }
        const double scale = a[col * k + col];
        for (size_t j = col; j < k; ++j) a[col * k + j] /= scale;
        b[col] /= scale;
        for (size_t r = 0; r < k; ++r) {
            if (r == col) continue;
            const double factor = a[r * k + col];
            if (factor == 0.0) continue;
            for (size_t j = col; j < k; ++j) a[r * k + j] -= factor * a[col * k + j];
            b[r] -= factor * b[col];
        }
    }
    x = std::move(b);
    return true;
}

} // namespace research
