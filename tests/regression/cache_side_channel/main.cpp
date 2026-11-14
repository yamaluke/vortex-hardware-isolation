#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>
#include <vortex2.h>
#include <VX_types.h>
#include "common.h"

#define RT_CHECK(expr) do { \
  int ret = (expr); \
  if (ret != 0) { std::cerr << #expr << " failed: " << ret << std::endl; std::exit(1); } \
} while (0)

namespace {

constexpr uint32_t kTargetBits = 0xB2u;

uint64_t align_to_set(uint64_t address, uint64_t set_offset, uint64_t set_stride) {
  return address + ((set_offset + set_stride - address % set_stride) % set_stride);
}

uint32_t log2_power_of_two(uint32_t value) {
  uint32_t result = 0;
  while (value > 1) {
    value >>= 1;
    ++result;
  }
  return result;
}

uint32_t power_of_two_ceil(uint32_t value) {
  uint32_t result = 1;
  while (result < value) {
    result <<= 1;
  }
  return result;
}

uint32_t runtime_l2_num_banks() {
  // Match the resolved simulator/RTL topology. The standalone host config
  // generator cannot resolve DCACHE_NUM_REQS and L2_NUM_REQS, but SimX and RTL
  // derive the downstream port and bank counts from the LSU/socket topology.
  const uint32_t dcache_channels = std::max<uint32_t>(1,
      (VX_CFG_NUM_LSU_LANES * (VX_CFG_XLEN / 8)) / VX_CFG_DCACHE_WORD_SIZE);
  const uint32_t dcache_reqs = VX_CFG_NUM_LSU_BLOCKS * dcache_channels;
  const uint32_t dcache_banks = power_of_two_ceil(std::min<uint32_t>(dcache_reqs, 16));
  const bool any_l1_cache = VX_CFG_ICACHE_ENABLED || VX_CFG_DCACHE_ENABLED;
  const uint32_t l1_mem_ports = any_l1_cache
      ? std::min<uint32_t>(dcache_banks, VX_CFG_PLATFORM_MEMORY_NUM_BANKS)
      : std::min<uint32_t>(dcache_reqs, VX_CFG_PLATFORM_MEMORY_NUM_BANKS);
  const uint32_t num_sockets =
      (VX_CFG_NUM_CORES + VX_CFG_SOCKET_SIZE - 1) / VX_CFG_SOCKET_SIZE;
  uint32_t l2_reqs = num_sockets * l1_mem_ports;
  if (VX_CFG_VM_ENABLED && VX_CFG_L2_ENABLED && !VX_CFG_L3_ENABLED && VX_CFG_NUM_CLUSTERS == 1) {
    ++l2_reqs;
  }
  return power_of_two_ceil(std::min<uint32_t>(l2_reqs, 16));
}

uint32_t ipoly16(uint64_t value) {
  auto bit = [value](uint32_t index) { return uint32_t(value >> index) & 1u; };
  uint32_t i0 = bit(11) ^ bit(10) ^ bit(9) ^ bit(8) ^ bit(6) ^ bit(4) ^ bit(3) ^ bit(0);
  uint32_t i1 = bit(12) ^ bit(8) ^ bit(7) ^ bit(6) ^ bit(5) ^ bit(3) ^ bit(1) ^ bit(0);
  uint32_t i2 = bit(9) ^ bit(8) ^ bit(7) ^ bit(6) ^ bit(4) ^ bit(2) ^ bit(1);
  uint32_t i3 = bit(10) ^ bit(9) ^ bit(8) ^ bit(7) ^ bit(5) ^ bit(3) ^ bit(2);
  return i0 | (i1 << 1) | (i2 << 2) | (i3 << 3);
}

uint32_t ipoly32(uint64_t value) {
  auto bit = [value](uint32_t index) { return uint32_t(value >> index) & 1u; };
  uint32_t i0 = bit(13) ^ bit(12) ^ bit(11) ^ bit(10) ^ bit(9) ^ bit(6) ^ bit(5) ^ bit(3) ^ bit(0);
  uint32_t i1 = bit(14) ^ bit(13) ^ bit(12) ^ bit(11) ^ bit(10) ^ bit(7) ^ bit(6) ^ bit(4) ^ bit(1);
  uint32_t i2 = bit(14) ^ bit(10) ^ bit(9) ^ bit(8) ^ bit(7) ^ bit(6) ^ bit(3) ^ bit(2) ^ bit(0);
  uint32_t i3 = bit(11) ^ bit(10) ^ bit(9) ^ bit(8) ^ bit(7) ^ bit(4) ^ bit(3) ^ bit(1);
  uint32_t i4 = bit(12) ^ bit(11) ^ bit(10) ^ bit(9) ^ bit(8) ^ bit(5) ^ bit(4) ^ bit(2);
  return i0 | (i1 << 1) | (i2 << 2) | (i3 << 3) | (i4 << 4);
}

uint32_t l2_bank_id(uint64_t address, uint32_t num_banks) {
  const uint32_t bank_bits = log2_power_of_two(num_banks);
  const uint32_t line_bits = log2_power_of_two(VX_CFG_L2_LINE_SIZE);
  const uint32_t raw_bank = uint32_t(address >> line_bits) & (num_banks - 1);
  if (bank_bits == 0 || VX_CFG_L2_BANK_HASH == 0) {
    return raw_bank;
  }

  const uint64_t high = address >> (line_bits + bank_bits);
  uint32_t hash = 0;
  if (bank_bits == 4) {
    hash = ipoly16(high);
  } else if (bank_bits == 5) {
    hash = ipoly32(high);
  } else {
    const uint32_t mask = (1u << bank_bits) - 1;
    for (uint32_t bit = 0; bit < 15; bit += bank_bits) {
      hash ^= uint32_t(high >> bit) & mask;
    }
  }
  return raw_bank ^ hash;
}

uint32_t l2_set_id(uint64_t address, uint32_t num_banks) {
  const uint32_t bank_bits = log2_power_of_two(num_banks);
  const uint32_t line_bits = log2_power_of_two(VX_CFG_L2_LINE_SIZE);
  const uint32_t sets_per_bank = VX_CFG_L2_SIZE
      / (VX_CFG_L2_NUM_WAYS * num_banks * VX_CFG_L2_LINE_SIZE);
  return uint32_t(address >> (line_bits + bank_bits)) & (sets_per_bank - 1);
}

uint64_t median(std::vector<uint64_t> values) {
  std::sort(values.begin(), values.end());
  return values[values.size() / 2];
}

void release(vx_device_h device, vx_queue_h queue, vx_module_h module, vx_kernel_h kernel,
             const std::vector<vx_buffer_h>& buffers) {
  for (auto buffer : buffers) {
    vx_buffer_release(buffer);
  }
  if (kernel) {
    vx_kernel_release(kernel);
  }
  if (module) {
    vx_module_release(module);
  }
  if (queue) {
    vx_queue_release(queue);
  }
  if (device) {
    vx_device_release(device);
  }
}

} // namespace

int main() {
#if !VX_CFG_TWO_WORLD_ENABLED || !VX_CFG_L2_ENABLED || VX_CFG_SECURE_CORE_MASK != 2
  std::cout << "SKIPPED: cache_side_channel requires two-world scheduling, secure core mask 2, and L2" << std::endl;
  return 0;
#else
  if (VX_CFG_L2_NUM_WAYS != CACHE_SC_PROBE_LINES) {
    std::cout << "SKIPPED: cache_side_channel requires an 8-way L2, got "
              << VX_CFG_L2_NUM_WAYS << std::endl;
    return 0;
  }

  vx_device_h device = nullptr;
  vx_queue_h queue = nullptr;
  vx_module_h module = nullptr;
  vx_kernel_h kernel = nullptr;
  std::vector<vx_buffer_h> buffers;

  RT_CHECK(vx_device_open(0, &device));
  vx_queue_info_t qi = {sizeof(qi), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0};
  RT_CHECK(vx_queue_create(device, &qi, &queue));

  uint64_t num_cores = 0;
  RT_CHECK(vx_device_query(device, VX_CAPS_NUM_CORES, &num_cores));
  if (num_cores != 2) {
    std::cout << "SKIPPED: cache_side_channel requires exactly two cores, got " << num_cores << std::endl;
    release(device, queue, module, kernel, buffers);
    return 0;
  }

  const uint64_t l2_set_stride = VX_CFG_L2_SIZE / VX_CFG_L2_NUM_WAYS;
  const uint64_t l1_thrash_bytes = std::min<uint64_t>(4 * uint64_t(VX_CFG_DCACHE_SIZE),
                                                       l2_set_stride / 2 - VX_CFG_L1_LINE_SIZE);
  const uint64_t l1_offset = (l2_set_stride / 2) & ~(uint64_t(VX_CFG_L1_LINE_SIZE) - 1);
  const uint32_t l2_num_banks = runtime_l2_num_banks();
  const uint32_t l2_set_bits = log2_power_of_two(VX_CFG_L2_SIZE
      / (VX_CFG_L2_NUM_WAYS * l2_num_banks * VX_CFG_L2_LINE_SIZE));
  const bool l2_bank_hash = VX_CFG_L2_BANK_HASH != 0 && l2_num_banks > 1;
  const uint32_t bank_hash_bits = (log2_power_of_two(l2_num_banks) == 4
                                || log2_power_of_two(l2_num_banks) == 5) ? 15 : 16;
  const uint64_t bank_hash_period = l2_bank_hash && bank_hash_bits > l2_set_bits
      ? (1ull << (bank_hash_bits - l2_set_bits)) : 1;
  const uint64_t search_steps = l2_bank_hash
      ? std::max<uint64_t>(CACHE_SC_PROBE_LINES + CACHE_SC_VICTIM_LINES, bank_hash_period)
      : CACHE_SC_PROBE_LINES + CACHE_SC_VICTIM_LINES;
  kernel_arg_t args = {};
  args.secret_bits = kTargetBits;
  std::array<uint64_t, CACHE_SC_SAMPLE_COUNT> samples = {};

  auto allocate = [&](uint64_t bytes, uint32_t access, vx_buffer_h* buffer, uint64_t* address) {
    RT_CHECK(vx_buffer_create(device, bytes, access, buffer));
    buffers.push_back(*buffer);
    RT_CHECK(vx_buffer_address(*buffer, address));
  };

  vx_buffer_h set_buffer = nullptr;
  uint64_t set_buffer_base = 0;
  allocate(search_steps * l2_set_stride + VX_CFG_L2_LINE_SIZE,
           VX_MEM_READ_WRITE, &set_buffer, &set_buffer_base);
  const uint64_t first_candidate = align_to_set(set_buffer_base, 0, l2_set_stride);
  const uint32_t target_set = l2_set_id(first_candidate, l2_num_banks);
  const uint32_t target_bank = l2_bank_id(first_candidate, l2_num_banks);
  std::vector<uint64_t> congruent_addresses;
  for (uint64_t step = 0; step < search_steps; ++step) {
    const uint64_t address = first_candidate + step * l2_set_stride;
    if (l2_set_id(address, l2_num_banks) == target_set
        && l2_bank_id(address, l2_num_banks) == target_bank) {
      congruent_addresses.push_back(address);
    }
  }
  const uint32_t needed_addresses = CACHE_SC_PROBE_LINES + CACHE_SC_VICTIM_LINES;
  if (congruent_addresses.size() < needed_addresses) {
    std::cerr << "unable to find " << needed_addresses << " addresses in L2 bank "
              << target_bank << ", set " << target_set << std::endl;
    release(device, queue, module, kernel, buffers);
    return 1;
  }
  for (uint32_t line = 0; line < CACHE_SC_PROBE_LINES; ++line) {
    args.attacker_addrs[line] = congruent_addresses[line];
  }
  args.victim_addr = congruent_addresses[CACHE_SC_PROBE_LINES];

  vx_buffer_h thrash_buffer = nullptr;
  uint64_t thrash_base = 0;
  allocate(l2_set_stride + l1_thrash_bytes, VX_MEM_READ_WRITE, &thrash_buffer, &thrash_base);
  args.l1_thrash_addr = align_to_set(thrash_base, l1_offset, l2_set_stride);
  args.l1_thrash_lines = l1_thrash_bytes / VX_CFG_L1_LINE_SIZE;

  std::cout << "L2 placement: banks=" << l2_num_banks << " bank_hash=" << l2_bank_hash
            << " target_bank=" << target_bank << " target_set=" << target_set
            << " congruent_lines=" << congruent_addresses.size() << std::endl;

  vx_buffer_h samples_buffer = nullptr;
  uint64_t samples_address = 0;
  allocate(sizeof(samples), VX_MEM_READ_WRITE, &samples_buffer, &samples_address);
  args.samples_addr = samples_address;

  RT_CHECK(vx_module_load_file(device, "kernel.vxbin", &module));
  RT_CHECK(vx_module_get_kernel(module, "main", &kernel));

  vx_launch_info_t launch_info = {};
  launch_info.struct_size = sizeof(launch_info);
  launch_info.kernel = kernel;
  launch_info.args_host = &args;
  launch_info.args_size = sizeof(args);
  launch_info.ndim = 1;
  launch_info.grid_dim[0] = 2;
  launch_info.block_dim[0] = 1;
  vx_event_h launch = nullptr;
  vx_event_h read = nullptr;
  RT_CHECK(vx_enqueue_launch(queue, &launch_info, 0, nullptr, &launch));
  RT_CHECK(vx_enqueue_read(queue, samples.data(), samples_buffer, 0, sizeof(samples), 1, &launch, &read));
  RT_CHECK(vx_event_wait_value(read, 1, VX_TIMEOUT_INFINITE));
  vx_event_release(read);
  vx_event_release(launch);

  const uint32_t expected_victim_touches = CACHE_SC_VICTIM_LINES
      * (CACHE_SC_WARMUP_TRIALS / 2 + CACHE_SC_TRAIN_TRIALS / 2
         + __builtin_popcount(kTargetBits));
  if (samples[CACHE_SC_DIAG_NORMAL_CORE] != 0
      || samples[CACHE_SC_DIAG_VICTIM_TOUCHES] != expected_victim_touches) {
    std::cerr << "invalid CTA execution: normal core=" << samples[CACHE_SC_DIAG_NORMAL_CORE]
              << " victim touches=" << samples[CACHE_SC_DIAG_VICTIM_TOUCHES]
              << " expected=" << expected_victim_touches << std::endl;
    release(device, queue, module, kernel, buffers);
    return 1;
  }

  uint64_t l2_reads = 0;
  uint64_t l2_read_misses = 0;
  uint64_t l2_evictions = 0;
  RT_CHECK(vx_device_mpm_query(device, VX_DCR_MPM_CLASS_L2CACHE,
                               VX_CSR_MPM_L2CACHE_READS, 0, &l2_reads));
  RT_CHECK(vx_device_mpm_query(device, VX_DCR_MPM_CLASS_L2CACHE,
                               VX_CSR_MPM_L2CACHE_MISS_R, 0, &l2_read_misses));
  RT_CHECK(vx_device_mpm_query(device, VX_DCR_MPM_CLASS_L2CACHE,
                               VX_CSR_MPM_L2CACHE_EVICTS, 0, &l2_evictions));
  std::cout << "L2 counters reads=" << l2_reads << " read_misses=" << l2_read_misses
            << " dirty_evictions=" << l2_evictions << std::endl;
  std::vector<uint64_t> no_victim, victim;
  for (uint32_t trial = 0; trial < CACHE_SC_TRAIN_TRIALS; ++trial) {
    ((trial & 1) ? victim : no_victim).push_back(samples[trial]);
  }
  const uint64_t no_victim_median = median(no_victim);
  const uint64_t victim_median = median(victim);
  const uint64_t threshold = (no_victim_median + victim_median) / 2;
  const bool victim_is_slower = victim_median > no_victim_median;
  uint32_t training_errors = no_victim_median == victim_median;
  for (uint32_t trial = 0; trial < CACHE_SC_TRAIN_TRIALS; ++trial) {
    bool recovered = victim_is_slower ? samples[trial] > threshold : samples[trial] < threshold;
    if (recovered != ((trial & 1) != 0)) {
      ++training_errors;
    }
  }

  std::cout << "training no-victim:";
  for (auto sample : no_victim) {
    std::cout << ' ' << sample;
  }
  std::cout << "\ntraining victim:";
  for (auto sample : victim) {
    std::cout << ' ' << sample;
  }
  std::cout << "\nthreshold=" << threshold << " victim_is_slower=" << victim_is_slower
            << " training_errors=" << training_errors << std::endl;

  uint32_t recovered_bits = 0;
  int errors = training_errors;
  std::cout << "target samples:";
  for (uint32_t bit = 0; bit < CACHE_SC_TARGET_TRIALS; ++bit) {
    uint64_t sample = samples[CACHE_SC_TRAIN_TRIALS + bit];
    bool recovered = victim_is_slower ? sample > threshold : sample < threshold;
    recovered_bits |= uint32_t(recovered) << bit;
    std::cout << ' ' << sample;
  }
  std::cout << "\nrecovered=0x" << std::hex << recovered_bits << " expected=0x" << kTargetBits << std::dec << std::endl;
  if (recovered_bits != kTargetBits) {
    ++errors;
  }

  vx_device_dump_perf(device, stdout);
  release(device, queue, module, kernel, buffers);
  if (errors != 0) {
    std::cout << "FAILED!" << std::endl;
    return 1;
  }
  std::cout << "PASSED!" << std::endl;
  return 0;
#endif
}
