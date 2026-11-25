#include "aaudio_output.h"
#include <string.h>

static aaudio_data_callback_result_t render(AAudioStream *stream, void *user, void *data, int32_t frames)
{
    (void)stream;
    port_audio_output *output = user;
    int32_t written = output->mix(output->user, data, frames);
    if (written < 0 || written > frames) written = 0;
    if (written < frames)
        memset((int16_t *)data + 2 * written, 0, (size_t)(frames - written) * 2 * sizeof(int16_t));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void port_audio_close(port_audio_output *output)
{
    if (output && output->stream) {
        AAudioStream_close(output->stream);
        output->stream = NULL;
    }
}

aaudio_result_t port_audio_open(port_audio_output *output, int32_t rate, port_mix_stereo mix, void *user)
{
    if (!output || output->stream || !mix || rate <= 0) return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
    AAudioStreamBuilder *builder = NULL;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) return result;
    output->mix = mix;
    output->user = user;
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, rate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(builder, render, output);
    result = AAudioStreamBuilder_openStream(builder, &output->stream);
    AAudioStreamBuilder_delete(builder);
    if (result == AAUDIO_OK) {
        result = AAudioStream_requestStart(output->stream);
        if (result != AAUDIO_OK) port_audio_close(output);
    }
    return result;
}
