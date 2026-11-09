// Host check for proteco_codec.c:  make -C tests  [FW=/path/to/firmware]  [RAW="a.sub b.sub"]
// Round trips keys through encoder -> decoder (also with timing jitter), then decodes any RAW
// .sub files given on the command line and prints the keys found.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../proteco_codec.h"

static int decode_upload(ProtecoEncoder* enc, int jitter, uint32_t* last_key) {
    ProtecoDecoder dec = {0};
    int frames = 0;
    uint32_t key;
    for(LevelDuration ld = proteco_encoder_yield(enc); !level_duration_is_reset(ld);
        ld = proteco_encoder_yield(enc)) {
        int32_t d = (int32_t)level_duration_get_duration(ld);
        if(jitter) d += (rand() % (2 * jitter + 1)) - jitter;
        if(proteco_decoder_feed(&dec, level_duration_get_level(ld), (uint32_t)d, &key)) {
            frames++;
            *last_key = key;
        }
    }
    // closing guard so the last frame is reported as well
    if(proteco_decoder_feed(&dec, false, 100000, &key)) {
        frames++;
        *last_key = key;
    }
    return frames;
}

static void decode_raw_file(const char* path) {
    FILE* f = fopen(path, "r");
    if(!f) {
        perror(path);
        exit(1);
    }
    ProtecoDecoder dec = {0};
    char line[8192];
    int frames = 0;
    uint32_t key;
    while(fgets(line, sizeof(line), f)) {
        if(strncmp(line, "RAW_Data:", 9) != 0) continue;
        for(char* tok = strtok(line + 9, " \r\n"); tok; tok = strtok(NULL, " \r\n")) {
            long v = strtol(tok, NULL, 10);
            if(proteco_decoder_feed(&dec, v > 0, (uint32_t)labs(v), &key)) {
                printf("  %s: frame %d key 0x%06X\n", path, ++frames, (unsigned)key);
            }
        }
    }
    fclose(f);
    printf("%s: %d frame(s)\n", path, frames);
}

int main(int argc, char** argv) {
    const uint32_t keys[] = {0x000000, 0xFFFFFF, 0x5AC3E1, 0x123456, 0xA5A5A5};
    for(size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        for(int jitter = 0; jitter <= 120; jitter += 120) {
            ProtecoEncoder enc;
            uint32_t got = ~0u;
            proteco_encoder_load(&enc, keys[i], 3);
            int frames = decode_upload(&enc, jitter, &got);
            assert(frames == 3);
            assert(got == keys[i]);
        }
    }
    // a 25th bit (longer, non Proteco frame) must not be reported
    ProtecoDecoder dec = {0};
    uint32_t key;
    ProtecoEncoder enc;
    proteco_encoder_load(&enc, 0x123456, 1);
    for(size_t i = 0; i < PROTECO_UPLOAD_SIZE; i++)
        assert(!proteco_decoder_feed(
            &dec,
            level_duration_get_level(enc.upload[i]),
            level_duration_get_duration(enc.upload[i]),
            &key));
    assert(!proteco_decoder_feed(&dec, false, 440, &key));
    assert(!proteco_decoder_feed(&dec, true, 880, &key));
    assert(!proteco_decoder_feed(&dec, false, 9240, &key));
    printf("codec round trip: ok\n");

    for(int i = 1; i < argc; i++)
        decode_raw_file(argv[i]);
    return 0;
}
