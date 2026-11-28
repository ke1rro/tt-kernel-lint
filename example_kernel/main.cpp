#include <cmath>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <utility>
#include <tt-metalium/core_coord.hpp>
#include <tt-metalium/host_api.hpp>
#include <tt-metalium/device.hpp>
#include <tt-metalium/experimental/mock_device/mock_device.hpp>
#include <tt-metalium/experimental/program_preparation.hpp>
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

std::vector<bfloat16> generate_data(const float a, const float b) {
    std::mt19937 rng{0};
    std::uniform_real_distribution<float> dist(a, b);
    std::vector<bfloat16> data(Params::elems_per_tile);
    std::generate(data.begin(), data.end(), [&] { return bfloat16(dist(rng)); });
    return data;
}

void print_preview(std::string_view label, const std::vector<bfloat16>& data) {
    constexpr std::size_t preview_size = 32;
    constexpr std::size_t columns = 8;
    const auto flags = std::cout.flags();
    const auto precision = std::cout.precision();
    std::cout << label << " (first 32 elements)\n" << std::fixed << std::setprecision(4);
    for (std::size_t i = 0; i < std::min(preview_size, data.size()); ++i) {
        std::cout << std::setw(9) << static_cast<float>(data[i]);
        if ((i + 1) % columns == 0) {
            std::cout << '\n';
        }
    }
    std::cout.flags(flags);
    std::cout.precision(precision);
}

// Add two tiled matrices.
// Matrix A: random values
// Matrix B: constant values to add to A

// Two input tiles: one 32x32 tile per matrix; one output tile.
// Data Type: bfloat16
int main(int argc, char** argv) {
    bool compile_only = false;
    bool mock = false;
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "Usage: example_kernel [--jit-without-device | --jit-with-device]\n"
                     "No option runs and checks the kernel on device 0.\n"
                     "--jit-without-device uses TT-Metal's predefined Wormhole configuration.\n"
                     "--jit-with-device opens device 0 and uses its initialized configuration.\n"
                     "Both JIT modes build device binaries without dispatching the example kernel.\n";
        return 0;
    }
    if (argc == 2) {
        const std::string_view mode(argv[1]);
        mock = mode == "--jit-without-device" || mode == "--mock-compile-only";
        compile_only = mock || mode == "--jit-with-device" || mode == "--compile-only";
    }
    if (argc > 1 && !compile_only) {
        std::cerr << "Unknown arguments. Use --help.\n";
        return 2;
    }
    if (mock) {
        experimental::configure_mock_mode(tt::ARCH::WORMHOLE_B0, 1);
    }
    constexpr float value_to_add_to_matrix_A{1.0F};
    constexpr CoreCoord core = {0, 0};
    constexpr int device_id{0};
    constexpr uint32_t tiles_per_cb{1};

    std::shared_ptr<distributed::MeshDevice> mesh_device{distributed::MeshDevice::create_unit_mesh(device_id)};

    distributed::MeshWorkload workload;

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
    if (compile_only) {
        experimental::program_preparation::prepare(*mesh_device, workload);
        std::cout << (mock ? "JIT without device completed" : "JIT with device completed") << '\n';
        if (!mesh_device->close()) {
            std::cerr << "Device close failed.\n";
            return 1;
        }
        return 0;
    }

    // These samples stay in tile-buffer order.
    print_preview("A", a_data);
    print_preview("B", b_data);
    std::cout << std::flush;

    auto& command_queue = mesh_device->mesh_command_queue();
    distributed::EnqueueWriteMeshBuffer(command_queue, src0_dram_buffer, a_data, false);
    distributed::EnqueueWriteMeshBuffer(command_queue, src1_dram_buffer, b_data, false);
    distributed::EnqueueMeshWorkload(command_queue, workload, false);
    distributed::Finish(command_queue);

    std::vector<bfloat16> result_vec;
    distributed::EnqueueReadMeshBuffer(command_queue, result_vec, dst_dram_buffer, true);

    if (result_vec.size() != a_data.size()) {
        std::cerr << "Readback size error: expected " << a_data.size() << " elements, got " << result_vec.size()
                  << ".\n";
        if (!mesh_device->close()) {
            std::cerr << "Device close failed.\n";
        }
        return 1;
    }
    print_preview("A + B", result_vec);
    constexpr float eps = 1e-2f;
    constexpr std::size_t mismatch_detail_limit = 4;
    std::size_t mismatch_count = 0;

    for (size_t i{}; i < result_vec.size(); ++i) {
        const float expected = static_cast<float>(a_data[i]) + static_cast<float>(b_data[i]);
        const float actual = static_cast<float>(result_vec[i]);

        if (!std::isfinite(expected) || !std::isfinite(actual) || std::abs(expected - actual) > eps) {
            ++mismatch_count;
            if (mismatch_count <= mismatch_detail_limit) {
                std::cout << "Result mismatch at index: " << i << " expected: " << expected << " got: " << actual
                          << '\n';
            }
        }
    }

    const bool closed = mesh_device->close();
    if (!closed) {
        std::cerr << "Device close failed.\n";
    }
    if (mismatch_count == 0 && closed) {
        std::cout << "Checked " << result_vec.size() << " elements: Passed!\n";
    } else if (mismatch_count == 0) {
        std::cout << "Checked " << result_vec.size() << " elements: Failed (device close failed).\n";
    } else {
        std::cout << "Checked " << result_vec.size() << " elements: Failed (" << mismatch_count
                  << " mismatches; showing first " << std::min(mismatch_count, mismatch_detail_limit) << ").\n";
    }

    return mismatch_count == 0 && closed ? 0 : 1;
}
