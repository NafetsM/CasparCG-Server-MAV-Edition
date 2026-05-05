#pragma once

#ifdef ENABLE_NVOF

#include <core/frame/frame.h>
#include <memory>

namespace caspar { namespace replay {

// GPU-based frame interpolator using NVIDIA Optical Flow SDK.
// Generates intermediate frames between two source frames at arbitrary alpha.
// Requires ENABLE_NVOF to be defined (CMake option -DENABLE_NVOF=ON).
class nvof_interpolator
{
  public:
    // ctx: existing CUDA context (nullptr → create one internally)
    nvof_interpolator(int width, int height, void* cuda_ctx = nullptr);
    ~nvof_interpolator();

    nvof_interpolator(const nvof_interpolator&)            = delete;
    nvof_interpolator& operator=(const nvof_interpolator&) = delete;

    // Produce an interpolated frame at position alpha ∈ (0, 1) between a and b.
    // alpha = 0.5 → midpoint; alpha = 0.25 → quarter of the way from a to b.
    core::mutable_frame interpolate(const core::const_frame& frame_a,
                                    const core::const_frame& frame_b,
                                    float                    alpha);

    bool is_available() const;

  private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// Returns nullptr if NVOF is unavailable on this system.
std::unique_ptr<nvof_interpolator> try_create_nvof_interpolator(int width, int height);

}} // namespace caspar::replay

#endif // ENABLE_NVOF
