#pragma once
// What the fix should output, computed on the CPU (same math as src/hdr_to_sdr.hlsl).
// Used by hdrtest and convtest.
#include <algorithm>
#include <cmath>

inline double SrgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
inline double LinearToSrgb(double l)
{
    l = std::clamp(l, 0.0, 1.0);
    return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}

struct FixParams {
    double sdrWhite = 80, peak = 1000, knee = 0.75;
};

// Expected output of the fix for linear input m (1.0 = SDR white), linear result.
inline double ExpectedLinear(double m, const FixParams& p)
{
    const double w = p.peak / p.sdrWhite;
    if (p.knee >= 0.999 || w <= 1.0001 || m <= p.knee)
        return std::min(m, 1.0);
    const double W = (w - p.knee) / (1 - p.knee);
    const double x = (m - p.knee) / (1 - p.knee);
    const double y = std::min(x * (1 + x / (W * W)) / (1 + x), 1.0);
    return p.knee + y * (1 - p.knee);
}
