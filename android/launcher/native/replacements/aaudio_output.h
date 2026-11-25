#ifndef PORT_AAUDIO_OUTPUT_H
#define PORT_AAUDIO_OUTPUT_H
#include <aaudio/AAudio.h>
#include <stdint.h>

/* The real-time mixer writes at most 'frames' interleaved stereo PCM16 frames and returns
 * how many it produced. A short read becomes silence. Keep the mixer and user data alive until close. */
typedef int32_t (*port_mix_stereo)(void *user, int16_t *pcm, int32_t frames);
typedef struct {
    AAudioStream *stream;
    port_mix_stereo mix;
    void *user;
} port_audio_output;

/* Initialize the struct to zero. One control thread owns open/close and polls stream state for
 * DISCONNECTED to close and reopen after route changes. The mixer must never block or allocate. */
aaudio_result_t port_audio_open(port_audio_output *output, int32_t mixer_rate, port_mix_stereo mix, void *user);
void port_audio_close(port_audio_output *output);
#endif
