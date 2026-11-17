/* Pinned C939 readback pipeline for differential tests, not production. */
int d3d_gl_read_rgba(d3d_surface *s, const uint32_t *r, float *out)
{
    if (!s->fmt || s->fmt->depth || !s->fmt->internal) return 0;
    if (!read_fbo) { glGenFramebuffers(1, &read_fbo); glGenFramebuffers(1, &draw_fbo); }
    uint32_t origin[4] = { 0, 0, r[2] - r[0], r[3] - r[1] };
    if (d3d_scale_of(s) != 1.0f) {
        read_scaled(s, r);
        r = origin;
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo);
        attach(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s);
    }
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    GLsizei w = (GLsizei)(r[2] - r[0]), h = (GLsizei)(r[3] - r[1]);
    int ok = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        while (glGetError()) {}
        if (s->fmt->is_float) {
            glReadPixels((GLint)r[0], (GLint)r[1], w, h, GL_RGBA, GL_FLOAT, out);
        } else {
            uint8_t *tmp = (uint8_t *)malloc((size_t)w * (size_t)h * 4);
            glReadPixels((GLint)r[0], (GLint)r[1], w, h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
            d3d_gl_layout_to_rgba(GL_RGBA, GL_UNSIGNED_BYTE, tmp, (uint32_t)(w * h), out);
            free(tmp);
        }
        ok = glGetError() == GL_NO_ERROR;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    d3d_rt_device()->fbo_dirty = 1;
    if (!ok) return 0;
    /* Standalone GPU surfaces are stored like render targets (d3d_surface_new). */
    uint32_t usage = s->container ? s->container->usage : (s->usage | D3DUSAGE_RENDERTARGET);
    GLint sw[4];
    d3d_storage_swizzle(s->fmt, usage, sw);
    d3d_swizzle_rgba(sw, out, (uint32_t)(w * h));
    return 1;
}

void d3d_gl_read_surface(d3d_surface *src, uint32_t dst_mem, uint32_t dst_pitch)
{
    uint32_t w = src->width, h = src->height, rect[4] = { 0, 0, w, h };
    float *px = (float *)malloc((size_t)w * h * 16);
    int read_ok = d3d_gl_read_rgba(src, rect, px);
    if (!read_ok) {
        void_log("[gl] surface %ux%u format %u cannot be read back", w, h, src->format);
        for (uint32_t y = 0; y < h; ++y) memset(GPTR(dst_mem + y * dst_pitch), 0, dst_pitch);
    } else {
        for (uint32_t y = 0; y < h; ++y)
            if (!d3d_format_encode(src->format, px + (size_t)y * w * 4, w, (uint8_t *)GPTR(dst_mem + y * dst_pitch))) {
                void_log("[gl] read back of format %u unsupported", src->format);
                break;
            }
    }
    {
        /* Picture-sized reads (thumbnails, snapshots): the mean colour shows
         * whether all channels came back. */
        static uint32_t pictures;
        if (read_ok && w >= 128 && h >= 128 && pictures < 64) {
            ++pictures;
            double sum[4] = { 0, 0, 0, 0 };
            for (size_t i = 0; i < (size_t)w * h; ++i) for (int k = 0; k < 4; ++k) sum[k] += px[4 * i + k];
            double n = (double)w * h;
            void_log("[gl] read back %ux%u format %u (usage %X%s, scale %.3f): mean %.3f %.3f %.3f %.3f", w, h,
                src->format, src->container ? src->container->usage : src->usage, src->container ? ", texture" : "",
                (double)d3d_scale_of(src), sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
        }
    }
    if (void_verbose) {
        static uint32_t reads;
        if (reads++ % 64 == 0 || w * h <= 64) {
            const float *c = px + ((size_t)(h / 2) * w + w / 2) * 4;
            VLOG("[gl] read back %ux%u format %u (read %u): centre %.3f %.3f %.3f %.3f, first word %08X", w, h,
                src->format, reads, (double)c[0], (double)c[1], (double)c[2], (double)c[3], MEM32(dst_mem));
        }
    }
    free(px);
}
