#include <array>
#include <cstdlib>
#include <iostream>
#include <vortex2.h>
#include "common.h"

#define RT_CHECK(expr) do { \
  int ret = (expr); \
  if (ret != 0) { std::cerr << #expr << " failed: " << ret << std::endl; std::exit(1); } \
} while (0)

int main() {
  vx_device_h device = nullptr;
  vx_queue_h queue = nullptr;
  vx_buffer_h result_buffer = nullptr;
  vx_module_h module = nullptr;
  vx_kernel_h kernel = nullptr;
  vx_event_h launch = nullptr;
  vx_event_h read = nullptr;

  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t num_cores = 0;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  if (num_cores != 2) {
    std::cout << "SKIPPED: world_isolation requires exactly two cores, got " << num_cores << std::endl;
    vx_queue_release(queue);
    vx_device_release(device);
    return 0;
  }

  std::array<uint32_t, 2> results = {0xffffffff, 0xffffffff};
  RT_CHECK(vx_buffer_create(device, sizeof(results), VX_MEM_READ_WRITE, &result_buffer));
  kernel_arg_t args = {};
  RT_CHECK(vx_buffer_address(result_buffer, &args.result_addr));
  RT_CHECK(vx_enqueue_write(queue, result_buffer, 0, results.data(), sizeof(results), 0, nullptr, nullptr));

  RT_CHECK(vx_module_load_file(device, "kernel.vxbin", &module));
  RT_CHECK(vx_module_get_kernel(module, "main", &kernel));
  vx_launch_info_t li = {};
  li.struct_size = sizeof(li);
  li.kernel = kernel;
  li.args_host = &args;
  li.args_size = sizeof(args);
  li.ndim = 1;
  li.grid_dim[0] = 2;
  li.block_dim[0] = 1;
  RT_CHECK(vx_enqueue_launch(queue, &li, 0, nullptr, &launch));
  RT_CHECK(vx_enqueue_read(queue, results.data(), result_buffer, 0, sizeof(results), 1, &launch, &read));
  RT_CHECK(vx_event_wait_value(read, 1, VX_TIMEOUT_INFINITE));

  const std::array<uint32_t, 2> expected = {0x00000000, 0x00010001};
  if (results != expected) {
    std::cerr << "expected {0x0, 0x10001}, got {0x" << std::hex << results[0]
              << ", 0x" << results[1] << "}" << std::endl;
    return 1;
  }

  vx_event_release(read);
  vx_event_release(launch);
  vx_kernel_release(kernel);
  vx_module_release(module);
  vx_buffer_release(result_buffer);
  vx_queue_release(queue);
  vx_device_release(device);
  std::cout << "PASSED!" << std::endl;
  return 0;
}
