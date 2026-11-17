/* Actual pre-fix dsound.c normalized UTF-8 text SHA256 4f411b812ff28c934af5be532da4198c207c66995fcb0126b117a1abd252f18c. */
static void mix_chunk(float *mix, uint32_t frames)
{
    memset(mix, 0, sizeof(float) * 2 * frames);
    for (dsbuf_t *b = buffers; b; b = b->next) {
        if (b->primary || !b->playing || !b->size || !b->block) continue;
        float vol = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
        float pan = (float)b->pan / 10000.0f;
        float gl = vol * (pan > 0 ? powf(10.0f, -pan * 5.0f) : 1.0f);
        float gr = vol * (pan < 0 ? powf(10.0f, pan * 5.0f) : 1.0f);
        if (b->flags & DSBCAPS_CTRL3D && b->mode3d != 2) {       /* not DS3DMODE_DISABLE */
            float d[3];
            float dist = 0.0f;
            for (int k = 0; k < 3; ++k) {
                d[k] = b->mode3d == 1 ? b->pos[k] : (b->pos[k] - listener_pos[k]);   /* HEADRELATIVE */
                dist += d[k] * d[k];
            }
            dist = sqrtf(dist) * distance_factor;
            float atten = 1.0f;
            if (dist > b->min_distance && b->min_distance > 0.0f) {
                float dd = dist > b->max_distance ? b->max_distance : dist;
                atten = b->min_distance / (b->min_distance + rolloff * (dd - b->min_distance));
            }
            /* Right vector = top x front; pan from the projection. */
            float right[3] = {
                listener_top[1] * listener_front[2] - listener_top[2] * listener_front[1],
                listener_top[2] * listener_front[0] - listener_top[0] * listener_front[2],
                listener_top[0] * listener_front[1] - listener_top[1] * listener_front[0] };
            float side = 0.0f;
            if (dist > 0.0001f) side = (d[0] * right[0] + d[1] * right[1] + d[2] * right[2]) / (dist / distance_factor);
            gl *= atten * (side > 0 ? 1.0f - 0.5f * side : 1.0f);
            gr *= atten * (side < 0 ? 1.0f + 0.5f * side : 1.0f);
        }
        uint32_t total = b->size / b->block;
        double step = (double)b->frequency / MIX_RATE;
        uint32_t start = b->play_pos;
        double frame = (double)(b->play_pos / b->block) + b->frac;
        int wrapped = 0, stopped = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            uint32_t f = (uint32_t)frame;
            if (f >= total) {
                if (!b->looping) { stopped = 1; break; }
                frame -= total; f = (uint32_t)frame; wrapped = 1;
            }
            float l = sample_at(b, f, 0), r = b->channels > 1 ? sample_at(b, f, 1) : l;
            mix[2 * i] += l * gl;
            mix[2 * i + 1] += r * gr;
            frame += step;
        }
        /* The chunk can end exactly on the last frame (a sound whose length is
         * a whole number of 10 ms ticks): a one-shot buffer stops there; the
         * modulo below would otherwise restart it from 0 for ever. */
        if (frame >= total) {
            if (b->looping) { frame = fmod(frame, (double)total); wrapped = 1; }
            else stopped = 1;
        }
        uint32_t f = (uint32_t)frame;
        if (f > total) f = total;
        b->frac = frame - (double)(uint32_t)frame;
        b->play_pos = (f * b->block) % (b->size ? b->size : 1);
        if (stopped) {
            DSTRACE("buffer %08X reached its end (%u bytes)", b->com.guest, b->size);
            b->playing = 0;
            b->play_pos = 0;
            b->frac = 0;
            for (uint32_t k = 0; k < b->notify_count; ++k) if (b->notify_offset[k] == 0xFFFFFFFFu) signal_event(b->notify_event[k]);
        }
        notify_passed(b, start, stopped ? b->size : b->play_pos, wrapped);
    }
}

COM_METHOD(dsb, Lock, 8)
{
    dsbuf_t *b = BUFF;
    uint32_t offset = ARG(1), bytes = ARG(2), flags = ARG(7);
    if (b->primary || !b->size) return DSERR_INVALIDCALL;
    if (flags & 1u) offset = b->play_pos;                 /* DSBLOCK_FROMWRITECURSOR */
    if (flags & 2u) bytes = b->size;                      /* DSBLOCK_ENTIREBUFFER */
    if (offset >= b->size || bytes > b->size) return DSERR_INVALIDPARAM;
    if (dstrace && (b->locks < 4 || !(b->locks & 63)))
        DSTRACE("buffer %08X lock #%u offset %u bytes %u flags %X (play %u)", b->com.guest, b->locks, offset, bytes, flags, b->play_pos);
    ++b->locks;
    uint32_t first = bytes <= b->size - offset ? bytes : b->size - offset;
    MEM32(ARG(3)) = b->data + offset;
    MEM32(ARG(4)) = first;
    if (ARG(5)) MEM32(ARG(5)) = first < bytes ? b->data : 0;
    if (ARG(6)) MEM32(ARG(6)) = bytes - first;
    return DS_OK;
}
