#pragma once

#include <ultrahdr_api.h>

#include <cstdlib>

namespace uhdr_repack {

/** RAII holder for uhdr_raw_image_t heap planes */
class RawImageHolder {
 public:
  RawImageHolder() = default;
  ~RawImageHolder() { reset(); }

  RawImageHolder(const RawImageHolder&) = delete;
  RawImageHolder& operator=(const RawImageHolder&) = delete;

  RawImageHolder(RawImageHolder&& other) noexcept : img_(other.img_) {
    other.img_ = {};
  }

  uhdr_raw_image_t* get() { return &img_; }
  const uhdr_raw_image_t* get() const { return &img_; }
  uhdr_raw_image_t& ref() { return img_; }
  const uhdr_raw_image_t& ref() const { return img_; }

  void reset() {
    /* UHDR_PLANE_PACKED and UHDR_PLANE_Y are both index 0; free each slot once. */
    for (int i = 0; i < 3; ++i) {
      if (img_.planes[i]) {
        free(img_.planes[i]);
        img_.planes[i] = nullptr;
      }
    }
    img_ = {};
  }

 private:
  uhdr_raw_image_t img_{};
};

}  // namespace uhdr_repack
