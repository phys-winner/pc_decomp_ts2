/* Original pre-UpdateSurface-fix host C, pinned for the no-op regression. */
static void surface_upload_rect(d3d_surface *s, uint32_t mem, const uint32_t *rect)
{
    uint32_t x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
    if (s->fmt->block_bytes) {
        x0 &= ~3u; y0 &= ~3u;
        x1 = (x1 + 3) & ~3u; y1 = (y1 + 3) & ~3u;
    }
    if (x1 > s->width) x1 = s->width;
    if (y1 > s->height) y1 = s->height;
    if (x1 <= x0 || y1 <= y0) return;
    uint32_t offset = s->fmt->block_bytes ? (y0 / 4) * s->pitch + (x0 / 4) * s->fmt->block_bytes
                                          : y0 * s->pitch + x0 * (s->fmt->bits / 8);
    const uint8_t *data = (const uint8_t *)GPTR(mem + offset);
    if (s->container) {
        d3d_texture_upload_rect(s->container, s->face, s->level, x0, y0, x1 - x0, y1 - y0, data, s->pitch);
    } else if (s->fmt->internal && !s->fmt->depth && s->pool != D3DPOOL_SYSTEMMEM && s->pool != D3DPOOL_SCRATCH) {
        /* Standalone GPU surfaces are stored like render targets (d3d_surface_new). */
        uint32_t row = d3d_format_pitch(s->fmt, x1 - x0), rows = d3d_format_rows(s->fmt, y1 - y0);
        surface_upload_args *a = (surface_upload_args *)d3d_rt_begin(run_surface_upload, (uint32_t)sizeof *a + row * rows);
        a->s = s; a->x0 = x0; a->y0 = y0; a->x1 = x1; a->y1 = y1; a->pitch = row;
        for (uint32_t y = 0; y < rows; ++y) memcpy((uint8_t *)(a + 1) + (size_t)y * row, data + (size_t)y * s->pitch, row);
        d3d_rt_commit();
    }
}

void d3d_surface_upload(d3d_surface *s)
{
    if (!s->lock_mem || !s->fmt) return;
    uint32_t whole[4] = { 0, 0, s->width, s->height };
    surface_upload_rect(s, s->lock_mem, s->lock_whole ? whole : s->lock_rect);
}

void d3d_update_surface(d3d_surface *src, uint32_t src_rect, d3d_surface *dst, uint32_t dst_point)
{
    if (!src->sysmem) { void_log("[d3d9] UpdateSurface from a non-system surface"); return; }
    if (src_rect || dst_point) void_log("[d3d9] UpdateSurface with rectangles: whole surface copied");
    dst->lock_mem = src->sysmem;
    d3d_surface_upload(dst);
    dst->lock_mem = 0;
}
