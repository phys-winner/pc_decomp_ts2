#include <lzma.h>

/* A legacy .lzma stream (13-byte LZMA-Alone header), with a caller-supplied dictionary memory limit. */
lzma_ret port_lzma_alone(const uint8_t *src, size_t size, uint8_t *dst, size_t expected, uint64_t memory_limit)
{
    uint8_t empty;
    lzma_stream stream = LZMA_STREAM_INIT;
    lzma_ret result = lzma_alone_decoder(&stream, memory_limit);
    if (result != LZMA_OK) return result;
    stream.next_in = src;
    stream.avail_in = size;
    stream.next_out = expected ? dst : &empty;
    stream.avail_out = expected ? expected : 1;
    result = lzma_code(&stream, LZMA_FINISH);
    if (result == LZMA_STREAM_END)
        result = !stream.avail_in && stream.total_out == expected ? LZMA_OK : LZMA_DATA_ERROR;
    else if (result == LZMA_OK) result = LZMA_DATA_ERROR;
    lzma_end(&stream);
    return result;
}
