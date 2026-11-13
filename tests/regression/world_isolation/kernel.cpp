#include <vx_intrinsics.h>
#include <vx_spawn2.h>
#include "common.h"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  if (threadIdx.x == 0) {
    auto result = reinterpret_cast<uint32_t*>(arg->result_addr);
    result[blockIdx.x] = (blockIdx.x << 16) | uint32_t(vx_core_id());
  }
}
