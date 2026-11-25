#include <zlib.h>

/* One complete RFC 1950 stream with the exact size recorded by the game's archive. */
int port_inflate_zlib(const Bytef *src, uInt size, Bytef *dst, uInt expected)
{
    Bytef empty;
    z_stream stream = {0};
    stream.next_in = (Bytef *)src;
    stream.avail_in = size;
    stream.next_out = expected ? dst : &empty;
    stream.avail_out = expected ? expected : 1;
    int result = inflateInit(&stream);
    if (result != Z_OK) return result;
    result = inflate(&stream, Z_FINISH);
    int complete = result == Z_STREAM_END && !stream.avail_in && stream.total_out == expected;
    inflateEnd(&stream);
    return complete ? Z_OK : result == Z_MEM_ERROR ? Z_MEM_ERROR : Z_DATA_ERROR;
}
