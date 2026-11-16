// Desktop stand-in for the few NEON intrinsics LUFS Master uses, with the same lane semantics
// as ARM's definitions. Lets the NEON code path run (and be compared bit for bit with the
// plain C++ path) on a PC. Test use only: compile with -D__ARM_NEON -Itest/neon_shim.
#pragma once
#include <cmath>

struct float32x4_t { float v[4]; };
struct float32x2_t { float v[2]; };

inline float32x4_t vdupq_n_f32(float x) { return {{x, x, x, x}}; }
inline float32x4_t vld1q_f32(const float* p) { return {{p[0], p[1], p[2], p[3]}}; }
inline float32x4_t vmlaq_n_f32(float32x4_t a, float32x4_t b, float c)
{ float32x4_t r; for (int i = 0; i < 4; ++i) { const float m = b.v[i] * c; r.v[i] = a.v[i] + m; } return r; }
inline float32x4_t vabsq_f32(float32x4_t a) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = std::fabs(a.v[i]); return r; }
inline float32x4_t vmaxq_f32(float32x4_t a, float32x4_t b) { float32x4_t r; for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i]; return r; }
inline float32x2_t vget_low_f32(float32x4_t a) { return {{a.v[0], a.v[1]}}; }
inline float32x2_t vget_high_f32(float32x4_t a) { return {{a.v[2], a.v[3]}}; }
inline float32x2_t vpmax_f32(float32x2_t a, float32x2_t b)
{ return {{a.v[0] > a.v[1] ? a.v[0] : a.v[1], b.v[0] > b.v[1] ? b.v[0] : b.v[1]}}; }
inline float vget_lane_f32(float32x2_t a, int lane) { return a.v[lane]; }
