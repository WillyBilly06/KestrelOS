/* audio - play a tone, a scale or a file through /dev/audio.
 *
 *   audio                  what the sound hardware is
 *   audio tone [hz] [ms]   a sine wave
 *   audio scale            an octave, to hear that timing and pitch are right
 *   audio sweep            20 Hz to 20 kHz, which shows what the output can do
 *   audio play <file>      raw signed 16-bit stereo at 48 kHz
 *   audio test             check the hardware is really reading the buffer
 *   audio stop             stop and discard whatever is queued
 *
 * Samples are signed 16-bit, two channels interleaved, 48 kHz - the format the
 * driver configures the codec for.  A sine is generated rather than read from a
 * table so the pitch is exact at any frequency.
 */
#include "kestrel.h"
#include "math.h"

#define RATE     48000
#define CHANNELS 2
#define CHUNK    2048          /* frames written per pass */

/* Wait for everything written to reach the speaker.  Without this a program
 * exits while its sound is still queued, and a script cannot put one sound
 * after another. */
static void drain(int fd) {
    for (int i = 0; i < 400; i++) {
        uint32_t queued = 0;
        if (ioctl(fd, 4, &queued) < 0) return;
        if (queued < 4096) break;
        sleep_ms(20);
    }
    sleep_ms(60);
}

/* Which device to play through.  Set by `-u`, because a machine with a codec
 * of its own and a headset plugged in has two, and the built-in one is the
 * reasonable default. */
static const char *audio_device = "/dev/audio";

static int open_audio(void) {
    int fd = open(audio_device, O_WRONLY);
    if (fd < 0) {
        printf("audio: cannot open %s (%s)\n", audio_device, strerror(-fd));
        if (audio_device[5] == 'u')
            printf("       No USB audio device is connected, or it offers "
                   "nothing this can play through.\n");
        else
            printf("       The System app's Devices tab lists what hardware "
                   "was found.\n");
    }
    return fd;
}

/* A short fade at each end: a tone that starts and stops at full amplitude
 * clicks, because the speaker cone is asked to move instantly. */
static float envelope(int frame, int total) {
    const int fade = RATE / 100;             /* 10 ms */
    if (total < 2 * fade) return 1.0f;
    if (frame < fade) return (float)frame / (float)fade;
    if (frame > total - fade) return (float)(total - frame) / (float)fade;
    return 1.0f;
}

static int play_tone(int fd, float hz, int ms, float amplitude) {
    int frames = RATE * ms / 1000;
    static int16_t buffer[CHUNK * CHANNELS];

    float phase = 0.0f;
    float step = (float)(M_TAU) * hz / (float)RATE;

    for (int done = 0; done < frames; ) {
        int n = frames - done;
        if (n > CHUNK) n = CHUNK;

        for (int i = 0; i < n; i++) {
            float v = sinf(phase) * amplitude * envelope(done + i, frames);
            phase += step;
            if (phase > (float)M_TAU) phase -= (float)M_TAU;

            int16_t sample = (int16_t)(v * 30000.0f);
            buffer[i * CHANNELS + 0] = sample;
            buffer[i * CHANNELS + 1] = sample;
        }

        size_t bytes = (size_t)n * CHANNELS * sizeof(int16_t);
        if (write(fd, buffer, bytes) != (ssize_t)bytes) {
            printf("audio: the device stopped accepting samples\n");
            return 1;
        }
        done += n;
    }
    drain(fd);
    return 0;
}

static int play_sweep(int fd, int ms) {
    int frames = RATE * ms / 1000;
    static int16_t buffer[CHUNK * CHANNELS];

    float phase = 0.0f;

    for (int done = 0; done < frames; ) {
        int n = frames - done;
        if (n > CHUNK) n = CHUNK;

        for (int i = 0; i < n; i++) {
            /* Exponential in frequency, so the sweep sounds even across the
             * range rather than racing through the top three octaves. */
            float t = (float)(done + i) / (float)frames;
            float hz = 20.0f * powf(1000.0f, t);
            phase += (float)(M_TAU) * hz / (float)RATE;
            if (phase > (float)M_TAU) phase -= (float)M_TAU;

            float v = sinf(phase) * 0.6f * envelope(done + i, frames);
            int16_t sample = (int16_t)(v * 30000.0f);
            buffer[i * CHANNELS + 0] = sample;
            buffer[i * CHANNELS + 1] = sample;
        }

        size_t bytes = (size_t)n * CHANNELS * sizeof(int16_t);
        if (write(fd, buffer, bytes) != (ssize_t)bytes) return 1;
        done += n;
    }
    drain(fd);
    return 0;
}

static int play_file(int fd, const char *path) {
    int in = open(path, O_RDONLY);
    if (in < 0) {
        printf("audio: cannot open %s (%s)\n", path, strerror(-in));
        return 1;
    }

    static uint8_t buffer[8192];
    uint64_t total = 0;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n <= 0) break;
        if (write(fd, buffer, (size_t)n) != n) {
            printf("audio: the device stopped accepting samples\n");
            close(in);
            return 1;
        }
        total += (uint64_t)n;
    }
    close(in);
    drain(fd);

    uint64_t ms = total * 1000 / (RATE * CHANNELS * 2);
    printf("audio: played %llu bytes (%llu.%llu seconds)\n",
           (unsigned long long)total,
           (unsigned long long)(ms / 1000), (unsigned long long)((ms % 1000) / 100));
    return 0;
}

/* Play a short tone and watch the position register.  A driver that has
 * configured everything correctly but never actually started the stream looks
 * identical from the outside until this is checked - the hardware reports where
 * it has read to, and that number moving is the only proof it is running. */
static int self_test(int fd) {
    printf("audio: checking the hardware is reading the buffer\n");

    uint32_t before = 0, after = 0;
    ioctl(fd, 5, &before);

    if (play_tone(fd, 440.0f, 300, 0.5f) != 0) return 1;

    ioctl(fd, 5, &after);

    /* The buffer is a ring, so the position wraps; either it moved forwards or
     * it went round, and both mean it is running. */
    if (before == after) {
        printf("  position stayed at %u - the stream is not running\n", before);
        printf("\naudio: FAILED\n");
        return 1;
    }

    printf("  position moved from %u to %u\n", before, after);

    uint32_t queued = 0;
    ioctl(fd, 4, &queued);
    printf("  %u bytes still queued after draining\n", queued);
    printf("\naudio: the output stream is running\n");
    return 0;
}

static void show_hardware(void) {
    int fd = open("/dev/audio", O_WRONLY);
    if (fd < 0) {
        printf("No audio output.\n\n");
        printf("The kernel logs what it found at start-up; `events -a | grep hda`\n");
        printf("shows whether a controller was present and what the codec said.\n");
        return;
    }

    uint32_t format[3] = { 0, 0, 0 };
    uint32_t space = 0;
    ioctl(fd, 3, format);
    ioctl(fd, 1, &space);
    close(fd);

    printf("Audio output ready.\n");
    printf("  Format   %u Hz, %u channel%s, %u-bit signed\n",
           format[0], format[1], format[1] == 1 ? "" : "s", format[2]);
    printf("  Buffer   %u bytes free\n", space);
    printf("\nTry `audio tone`, `audio scale` or `audio sweep`.\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { show_hardware(); return 0; }

    /* -u sends it to a plugged-in device instead.  Taken before the command
     * so that `audio -u tone` reads the way it is meant. */
    if (argc > 1 && (!strcmp(argv[1], "-u") || !strcmp(argv[1], "--usb"))) {
        audio_device = "/dev/usbaudio";
        argv++;
        argc--;
        if (argc < 2) {
            printf("usage: audio -u <tone | scale | sweep | test | play <file>>\n");
            return 1;
        }
    }

    /* Recording: read from the device and write to a file, which is the
     * mirror of `play` and the only thing that proves the input side works
     * end to end rather than only being described. */
    if (!strcmp(argv[1], "record")) {
        if (argc < 3) {
            printf("usage: audio record <file> [seconds]\n");
            return 1;
        }
        int seconds = argc > 3 ? atoi(argv[3]) : 3;
        if (seconds < 1) seconds = 1;
        if (seconds > 60) seconds = 60;

        int fd = open(audio_device, O_RDONLY);
        if (fd < 0) {
            printf("audio: cannot open %s for recording (%s)\n",
                   audio_device, strerror(-fd));
            return 1;
        }

        int out = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC);
        if (out < 0) {
            printf("audio: cannot write %s (%s)\n", argv[2], strerror(-out));
            close(fd);
            return 1;
        }

        printf("recording %d second(s) to %s...\n", seconds, argv[2]);
        flush_output();

        /* Bytes rather than time: a sample count is what the file holds, and
         * counting them is exact where waiting on a clock is not. */
        unsigned rate = 48000, channels = 2, bits = 16;
        unsigned want = rate * channels * (bits / 8) * (unsigned)seconds;
        unsigned got = 0;

        static unsigned char chunk[8192];
        while (got < want) {
            int n = read(fd, chunk, sizeof chunk);
            if (n <= 0) break;
            if ((unsigned)n > want - got) n = (int)(want - got);
            if (write(out, chunk, (size_t)n) != n) break;
            got += (unsigned)n;
        }

        close(out);
        close(fd);

        if (!got) {
            printf(A_YELLOW "nothing arrived." A_RESET
                   "  The input converter may have no source selected.\n");
            return 1;
        }
        printf("wrote %u bytes (%u.%01u seconds of %u Hz, %u channel, %u bit).\n",
               got, got / (rate * channels * (bits / 8)),
               (got * 10 / (rate * channels * (bits / 8))) % 10,
               rate, channels, bits);
        return 0;
    }

    if (!strcmp(argv[1], "stop")) {
        int fd = open_audio();
        if (fd < 0) return 1;
        ioctl(fd, 2, NULL);
        close(fd);
        printf("audio: stopped\n");
        return 0;
    }

    int fd = open_audio();
    if (fd < 0) return 1;

    int result = 0;

    if (!strcmp(argv[1], "tone")) {
        float hz = (argc > 2) ? (float)atoi(argv[2]) : 440.0f;
        int   ms = (argc > 3) ? atoi(argv[3]) : 1000;
        if (hz < 20.0f) hz = 20.0f;
        if (hz > 20000.0f) hz = 20000.0f;
        if (ms < 10) ms = 10;
        if (ms > 30000) ms = 30000;
        printf("audio: %d Hz for %d ms\n", (int)hz, ms);
        result = play_tone(fd, hz, ms, 0.7f);

    } else if (!strcmp(argv[1], "scale")) {
        /* An equal-tempered octave from A above middle C. */
        static const char *names[] = { "A", "A#", "B", "C", "C#", "D",
                                       "D#", "E", "F", "F#", "G", "G#", "A" };
        printf("audio: an octave from 440 Hz\n");
        for (int i = 0; i <= 12 && !result; i++) {
            float hz = 440.0f * powf(2.0f, (float)i / 12.0f);
            printf("  %-3s %4d Hz\n", names[i], (int)hz);
            result = play_tone(fd, hz, 260, 0.6f);
        }

    } else if (!strcmp(argv[1], "sweep")) {
        printf("audio: 20 Hz to 20 kHz over four seconds\n");
        result = play_sweep(fd, 4000);

    } else if (!strcmp(argv[1], "test")) {
        result = self_test(fd);

    } else if (!strcmp(argv[1], "play")) {
        if (argc < 3) {
            printf("usage: audio play <file>\n");
            result = 1;
        } else {
            result = play_file(fd, argv[2]);
        }

    } else {
        printf("usage: audio [tone [hz] [ms] | scale | sweep | play <file>"
               " | test | stop]\n");
        result = 1;
    }

    close(fd);
    return result;
}
