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

`include "VX_define.vh"

module VX_kmu_top import VX_gpu_pkg::*;
(
    input wire                              clk,
    input wire                              reset,

    // DCR request interface
    input  wire                             dcr_req_valid,
    input  wire                             dcr_req_rw,
    input  wire [VX_DCR_ADDR_WIDTH-1:0]     dcr_req_addr,
    input  wire [VX_DCR_DATA_WIDTH-1:0]     dcr_req_data,

    // Start trigger: pulse high for one cycle to begin CTA dispatch
    input  wire                             start,

    output wire                             core0_valid,
    output wire                             core1_valid,
    output wire [31:0]                      core0_block_x,
    output wire [31:0]                      core1_block_x,
    output wire [KMU_CORE_MASK_W-1:0]       core0_mask,
    output wire [KMU_CORE_MASK_W-1:0]       core1_mask
);

    VX_kmu_bus_if kmu_bus_if[1]();
    VX_kmu_bus_if core_kmu_bus_if[2]();

    for (genvar i = 0; i < 2; ++i) begin
        assign core_kmu_bus_if[i].ready = 1'b1;
    end

    VX_kmu_bus_arb #(
        .NUM_INPUTS  (1),
        .NUM_OUTPUTS (2),
        .DEST_DIV    (1)
    ) router (
        .clk        (clk),
        .reset      (reset),
        .bus_in_if  (kmu_bus_if),
        .bus_out_if (core_kmu_bus_if)
    );

    kmu_req_t core_req[2];
    for (genvar i = 0; i < 2; ++i) begin
        assign core_req[i] = kmu_req_t'(core_kmu_bus_if[i].data);
    end
    assign core0_valid   = core_kmu_bus_if[0].valid;
    assign core1_valid   = core_kmu_bus_if[1].valid;
    assign core0_block_x = core_req[0].args.compute.block_idx[0];
    assign core1_block_x = core_req[1].args.compute.block_idx[0];
    assign core0_mask    = core_kmu_bus_if[0].core_mask;
    assign core1_mask    = core_kmu_bus_if[1].core_mask;

    wire kmu_busy;
    `UNUSED_VAR (kmu_busy)

    VX_kmu kmu (
        .clk           (clk),
        .reset         (reset),
        .dcr_req_valid (dcr_req_valid),
        .dcr_req_rw    (dcr_req_rw),
        .dcr_req_addr  (dcr_req_addr),
        .dcr_req_data  (dcr_req_data),
        .start         (start),
        .busy          (kmu_busy),
        .kmu_bus_if    (kmu_bus_if[0])
    );

endmodule

