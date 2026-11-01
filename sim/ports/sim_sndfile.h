/** @file Minimal libsndfile ABI used by the host music stream adapter. */
#ifndef __SIM_SNDFILE_H__
#define __SIM_SNDFILE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int64_t sf_count_t;
typedef struct SNDFILE_tag SNDFILE;

typedef struct
{
    sf_count_t frames;
    int samplerate;
    int channels;
    int format;
    int sections;
    int seekable;
} SF_INFO;

#define SFM_READ 0x10

SNDFILE *sf_open(const char *path, int mode, SF_INFO *sfinfo);
sf_count_t sf_readf_short(SNDFILE *sndfile, int16_t *ptr, sf_count_t frames);
int sf_close(SNDFILE *sndfile);
const char *sf_strerror(SNDFILE *sndfile);

#ifdef __cplusplus
}
#endif

#endif /* __SIM_SNDFILE_H__ */
