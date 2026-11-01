// Writes a 440 Hz sine as raw s16le stereo 48 kHz PCM to stdout for N seconds
// (default 2). Phase 4 audio test: `tone 3 | pacat --raw ...`. Built for arm64 and x86_64.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 2.0;
    const int rate = 48000;
    long n = (long)(secs * rate);
    int16_t frame[2];
    for (long i = 0; i < n; i++) {
        double env = i < 2400 ? i / 2400.0 : (n - i < 2400 ? (n - i) / 2400.0 : 1.0);
        int16_t v = (int16_t)(sin(2 * M_PI * 440.0 * i / rate) * 12000 * env);
        frame[0] = frame[1] = v;
        if (fwrite(frame, sizeof frame, 1, stdout) != 1) return 1;
    }
    return 0;
}
