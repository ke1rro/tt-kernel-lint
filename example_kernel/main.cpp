#include <cmath>
#include <iostream>
#include <utility>
#include <tt-metalium/core_coord.hpp>
#include <tt-metalium/host_api.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/bfloat16.hpp>
#include <tt-metalium/tensor_accessor_args.hpp>
#include <tt-metalium/distributed.hpp>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <memory>
#include <random>
#include <vector>
#include "tt-metalium/base_types.hpp"

using namespace tt::tt_metal;

struct Params {
    static constexpr std::uint32_t elems_per_tile = tt::constants::TILE_WIDTH * tt::constants::TILE_HEIGHT;
    static constexpr std::uint32_t size_of_tile = sizeof(bfloat16) * elems_per_tile;
};

std::vector<bfloat16> generate_data(float a, float b) {
    std::mt19937 rng{0};
    std::uniform_real_distribution<float> dist(a, b);
    std::vector<bfloat16> data(Params::elems_per_tile);
    std::generate(data.begin(), data.end(), [&] { return bfloat16(dist(rng)); });
    return data;
}

// Add two tiled matrices.
// Matrix A: random values
// Matrix B: constant values to add to A

// Two input tiles: one 32x32 tile per matrix; one output tile.
// Data Type: bfloat16
int main() {
    bool pass{true};
    constexpr float value_to_add_to_matrix_A{1.0F};
    constexpr CoreCoord core = {0, 0};
    constexpr int device_id{0};
    constexpr uint32_t tiles_per_cb{1};

    std::shared_ptr<distributed::MeshDevice> mesh_device{distributed::MeshDevice::create_unit_mesh(device_id)};

    distributed::MeshWorkload workload;

    distributed::MeshCommandQueue& command_queue = mesh_device->mesh_command_queue();
    const auto device_range = distributed::MeshCoordinateRange(mesh_device->shape());
    Program program{CreateProgram()};

    distributed::DeviceLocalBufferConfig dram_config{
        .page_size = Params::size_of_tile, .buffer_type = BufferType::DRAM};

    distributed::ReplicatedBufferConfig buffer_config{.size = Params::size_of_tile};

    auto src0_dram_buffer = distributed::MeshBuffer::create(buffer_config, dram_config, mesh_device.get());
    auto src1_dram_buffer = distributed::MeshBuffer::create(buffer_config, dram_config, mesh_device.get());
    auto dst_dram_buffer = distributed::MeshBuffer::create(buffer_config, dram_config, mesh_device.get());

    // Matrix A
    auto a_data{generate_data(0.0F, 1.0F)};
    // Matrix B
    auto b_data{std::vector<bfloat16>(Params::elems_per_tile, bfloat16(value_to_add_to_matrix_A))};

    distributed::EnqueueWriteMeshBuffer(command_queue, src0_dram_buffer, a_data, false);
    distributed::EnqueueWriteMeshBuffer(command_queue, src1_dram_buffer, b_data, false);

    // CBs
    tt::CBIndex src0_cb{tt::CBIndex::c_0};
    tt::CBIndex src1_cb{tt::CBIndex::c_1};
    tt::CBIndex dst_cb{tt::CBIndex::c_16};

    CreateCircularBuffer(
        program,
        core,
        CircularBufferConfig(tiles_per_cb * Params::size_of_tile, {{src0_cb, tt::DataFormat::Float16_b}})
            .set_page_size(src0_cb, Params::size_of_tile));

    CreateCircularBuffer(
        program,
        core,
        CircularBufferConfig(tiles_per_cb * Params::size_of_tile, {{src1_cb, tt::DataFormat::Float16_b}})
            .set_page_size(src1_cb, Params::size_of_tile));

    CreateCircularBuffer(
        program,
        core,
        CircularBufferConfig(tiles_per_cb * Params::size_of_tile, {{dst_cb, tt::DataFormat::Float16_b}})
            .set_page_size(dst_cb, Params::size_of_tile));

    std::vector<uint32_t> reader_compile_time_args;
    TensorAccessorArgs(*src0_dram_buffer).append_to(reader_compile_time_args);
    TensorAccessorArgs(*src1_dram_buffer).append_to(reader_compile_time_args);

    // Kernels creation

    // Reader kernel
    auto reader = CreateKernel(
        program,
        EXAMPLE_KERNEL_SOURCE_DIR "/device/dataflow/reader.cpp",
        core,
        DataMovementConfig{
            .processor = DataMovementProcessor::RISCV_0,
            .noc = NOC::RISCV_0_default,
            .compile_args = reader_compile_time_args});

    // Writer Kernel
    std::vector<uint32_t> writer_compile_time_args;
    TensorAccessorArgs(*dst_dram_buffer).append_to(writer_compile_time_args);
    auto writer = CreateKernel(
        program,
        EXAMPLE_KERNEL_SOURCE_DIR "/device/dataflow/writer.cpp",
        core,
        DataMovementConfig{
            .processor = DataMovementProcessor::RISCV_1,
            .noc = NOC::RISCV_1_default,
            .compile_args = writer_compile_time_args});

    // Compute Kernel
    CreateKernel(
        program,
        EXAMPLE_KERNEL_SOURCE_DIR "/device/compute/add_compute.cpp",
        core,
        ComputeConfig{.math_fidelity = tt::tt_metal::MathFidelity::HiFi4});

    SetRuntimeArgs(
        program,
        reader,
        core,
        {static_cast<uint32_t>(src0_dram_buffer->address()), static_cast<uint32_t>(src1_dram_buffer->address())});
    SetRuntimeArgs(program, writer, core, {static_cast<uint32_t>(dst_dram_buffer->address())});

    workload.add_program(device_range, std::move(program));
    distributed::EnqueueMeshWorkload(command_queue, workload, false);
    distributed::Finish(command_queue);

    std::vector<bfloat16> result_vec;
    distributed::EnqueueReadMeshBuffer(command_queue, result_vec, dst_dram_buffer, true);

    if (result_vec.size() != a_data.size()) {
        mesh_device->close();
        return 1;
    }
    constexpr float eps = 1e-2f;

    for (size_t i{}; i < result_vec.size(); ++i) {
        const float expected = static_cast<float>(a_data[i]) + value_to_add_to_matrix_A;
        const float actual = static_cast<float>(result_vec[i]);

        if (!std::isfinite(actual) || std::abs(expected - actual) > eps) {
            pass = false;
            std::cout << "Result mismatch at index: " << i << " expected: " << expected << " got: " << actual << '\n';
        }
    }

    pass = mesh_device->close() && pass;

    if (pass) {
        std::cout << "Passed!\n";
    } else {
        std::cout << "Failed!\n";
    }

    return pass ? 0 : 1;
}
