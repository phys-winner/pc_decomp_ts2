/* DXT blocks transcoded to ATC for GLES drivers without S3TC (d3d9_atc.c). */
#ifndef VOID_D3D9_ATC_H
#define VOID_D3D9_ATC_H

#ifndef GL_ATC_RGB_AMD
#define GL_ATC_RGB_AMD 0x8C92
#endif
#ifndef GL_ATC_RGBA_EXPLICIT_ALPHA_AMD
#define GL_ATC_RGBA_EXPLICIT_ALPHA_AMD 0x8C93
#endif
#ifndef GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD
#define GL_ATC_RGBA_INTERPOLATED_ALPHA_AMD 0x87EE
#endif

/* The ATC storage for a texture of DXT conversion `conv` (CONV_DXT1/3/5) whose
 * first upload is this w x h block rect: DXT1 with transparent texels needs
 * alpha. 0 for other formats. */
GLenum d3d_atc_storage(int conv, const uint8_t *data, uint32_t width, uint32_t height, uint32_t pitch);
uint32_t d3d_atc_block_bytes(GLenum storage);
/* Transcode a w x h rect of DXT blocks (rows `pitch` apart) into tightly packed ATC blocks; returns the bytes. */
size_t d3d_atc_transcode(int conv, GLenum storage, const uint8_t *src, uint32_t width, uint32_t height, uint32_t pitch,
    uint8_t *dst);
/* The GPU's decode of one ATC block, for tests. */
void d3d_atc_decode_block(GLenum storage, const uint8_t *block, uint8_t rgba[16][4]);

#endif
