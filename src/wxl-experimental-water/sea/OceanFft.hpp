// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <algorithm>
#include <cmath>

namespace wxl::water::detail
{
    struct FftComplex { float re = 0.0f, im = 0.0f; };
    inline FftComplex Multiply(FftComplex a, FftComplex b)
    { return {a.re * b.re - a.im * b.im, a.re * b.im + a.im * b.re}; }

    // The water always uses the same inverse transform size. Its permutation and rotation
    // factors do not depend on weather or time; build them once, not for every row and column.
    template<int N> class InverseFftPlan
    {
        static_assert(N >= 2 && (N & (N - 1)) == 0);
        std::array<int, N> reverse_{};
        std::array<FftComplex, N - 1> rotations_{};
    public:
        explicit InverseFftPlan(float twoPi)
        {
            for (int i = 1, j = 0; i < N; ++i)
            {
                int bit = N >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                reverse_[i] = j;
            }
            for (int len = 2; len <= N; len <<= 1)
            {
                const float angle = twoPi / static_cast<float>(len);
                const FftComplex step{std::cos(angle), std::sin(angle)};
                FftComplex w{1.0f, 0.0f};
                for (int k = 0; k < len / 2; ++k)
                {
                    // Preserve the old recurrence and rounding, rather than recomputing cos(k*a).
                    rotations_[len / 2 - 1 + k] = w;
                    w = Multiply(w, step);
                }
            }
        }
        void Transform(FftComplex* a) const
        {
            for (int i = 1; i < N; ++i)
                if (i < reverse_[i]) std::swap(a[i], a[reverse_[i]]);
            for (int len = 2; len <= N; len <<= 1)
            {
                const int half = len >> 1;
                for (int i = 0; i < N; i += len)
                    for (int k = 0; k < half; ++k)
                    {
                        const FftComplex u = a[i + k];
                        const FftComplex v = Multiply(a[i + k + half], rotations_[half - 1 + k]);
                        a[i + k] = {u.re + v.re, u.im + v.im};
                        a[i + k + half] = {u.re - v.re, u.im - v.im};
                    }
            }
        }
    };
}
