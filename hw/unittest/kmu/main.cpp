// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vl_simulator.h"
#include "VVX_kmu_top.h"
#include <iostream>
#include "VX_config.h"
#include "VX_types.h"
#include <cassert>

#define CHECK(expr) do { \
  if (!(expr)) { \
    std::cerr << "check failed: " << #expr << std::endl; \
    return 1; \
  } \
} while (0)

#ifndef TRACE_START_TIME
#define TRACE_START_TIME 0ull
#endif

#ifndef TRACE_STOP_TIME
#define TRACE_STOP_TIME -1ull
#endif

static uint64_t timestamp = 0;
static bool trace_enabled = false;
static uint64_t trace_start_time = TRACE_START_TIME;
static uint64_t trace_stop_time  = TRACE_STOP_TIME;

double sc_time_stamp() {
  return timestamp;
}

bool sim_trace_enabled() {
  if (timestamp >= trace_start_time
   && timestamp < trace_stop_time)
    return true;
  return trace_enabled;
}

void sim_trace_enable(bool enable) {
  trace_enabled = enable;
}

template <typename T>
int write_dcr(vl_simulator<T>& sim, const int addr, const int value, int tick) {
    sim->dcr_req_valid = 1;
    sim->dcr_req_rw    = 1;
    sim->dcr_req_addr  = addr;
    sim->dcr_req_data  = value;
    tick = sim.step(tick, 2);
    sim->dcr_req_valid = 0;
    sim->dcr_req_rw    = 0;
    return tick;
}

int main(int argc, char **argv) {
  Verilated::commandArgs(argc, argv);

  vl_simulator<VVX_kmu_top> sim;
  int tick = 0;

  int dummy_pc    = 0x12345678;
  int dummy_param = 0x87654321;
  int dummy_grid[3]  = {3, 3, 3};
  int dummy_block[3] = {5, 5, 6};

  // Reset first so all internal state initializes cleanly
  tick = sim.reset(tick);
  sim->start         = 0;
  sim->dcr_req_valid = 0;
  sim->dcr_req_rw    = 0;

  // Write kernel configuration DCRs after reset
  tick = write_dcr(sim, VX_DCR_KMU_STARTUP_ADDR0, dummy_pc,       tick);
  tick = write_dcr(sim, VX_DCR_KMU_STARTUP_ARG0,  dummy_param,    tick);
  tick = write_dcr(sim, VX_DCR_KMU_GRID_DIM_X,    dummy_grid[0],  tick);
  tick = write_dcr(sim, VX_DCR_KMU_GRID_DIM_Y,    dummy_grid[1],  tick);
  tick = write_dcr(sim, VX_DCR_KMU_GRID_DIM_Z,    dummy_grid[2],  tick);
  tick = write_dcr(sim, VX_DCR_KMU_BLOCK_DIM_X,   dummy_block[0], tick);
  tick = write_dcr(sim, VX_DCR_KMU_BLOCK_DIM_Y,   dummy_block[1], tick);
  tick = write_dcr(sim, VX_DCR_KMU_BLOCK_DIM_Z,   dummy_block[2], tick);
  tick = write_dcr(sim, VX_DCR_KMU_BLOCK_SIZE,     dummy_block[0] * dummy_block[1] * dummy_block[2], tick);
  tick = write_dcr(sim, VX_DCR_KMU_WARP_STEP_X,   VX_CFG_NUM_THREADS, tick);

  // Pulse start for one cycle to arm KMU dispatch
  sim->start = 1;
  tick = sim.step(tick, 2);
  sim->start = 0;

  bool seen_core0[3] = {};
  bool seen_core1[3] = {};
  for (uint32_t i = 0; i < 200; ++i) {
    tick = sim.step(tick, 2);
    if (sim->core0_valid) {
      CHECK((sim->core0_block_x & 1) == 0);
      CHECK(sim->core0_mask == 0x1);
      seen_core0[sim->core0_block_x] = true;
    }
    if (sim->core1_valid) {
      CHECK((sim->core1_block_x & 1) == 1);
      CHECK(sim->core1_mask == 0x2);
      seen_core1[sim->core1_block_x] = true;
    }
  }
  CHECK(seen_core0[0] && seen_core0[2]);
  CHECK(seen_core1[1]);

  return 0;
}
