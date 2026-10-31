#include <cstdint>

#include "api/dataflow/noc.h"
#include "api/dataflow/circular_buffer.h"
#include "api/tensor/noc_traits.h"

void kernel_main() {
    uint32_t c_addr = get_arg_val<uint32_t>(0);

    // The circular buffer that we are going to read from and write to DRAM
    constexpr uint32_t cb_out0 = tt::CBIndex::c_16;
    const uint32_t tile_size_bytes = get_tile_size(cb_out0);

    // Address of the output buffer
    constexpr auto out0_args = TensorAccessorArgs<0>();
    const auto out0 = TensorAccessor(out0_args, c_addr);

    // Create Device 2.0 Noc and CircularBuffer objects
    Noc noc;
    CircularBuffer cb_out(cb_out0);

    // Make sure there is a tile in the circular buffer
    cb_out.wait_front(1);
    // write the tile to DRAM
    noc.async_write(cb_out, out0, tile_size_bytes, {}, {.page_id = 0});
    // This will wait until the write is done. As an alternative,
    // noc_async_write_flushed() can be faster because it waits
    // until the write request is sent. In that case, you have to
    // use noc_async_write_barrier() at least once at the end of
    // data movement kernel to make sure all writes are done.
    noc.async_write_barrier();
    // Mark the tile as consumed
    cb_out.pop_front(1);
}
