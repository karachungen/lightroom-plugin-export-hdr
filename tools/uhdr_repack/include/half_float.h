#pragma once

#include <cstddef>
#include <cmath>
#include <cstdint>

namespace uhdr_repack {

/** IEEE 754 binary16 storage (portable across macOS half and Windows). */
using fp16_t = uint16_t;

uint16_t float_to_half(float value);
void float_rgba_to_half_rgba(const float* src, fp16_t* dst, size_t num_floats);

void even_normalize(unsigned orig_w, unsigned orig_h, unsigned* w, unsigned* h);

}  // namespace uhdr_repack
