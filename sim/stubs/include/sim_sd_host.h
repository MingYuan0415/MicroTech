/** @file Host-backed SD volume and audio availability controls for the sim. */
#ifndef __SIM_SD_HOST_H__
#define __SIM_SD_HOST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Maximum stored recording name length, matching the service. */
#define SIM_SD_NAME_MAX 64

/** @brief Create the absolute backing directory and mount it as the volume. */
esp_err_t host_sd_boot(const char *dir);
/** @brief Rename the backing directory in/out to fake mount transitions. */
bool host_sd_set_mounted(bool mounted);
/** @brief Report whether the SD volume currently resolves. */
bool host_sd_is_mounted(void);
/** @brief Return the mounted volume path, or NULL while unmounted. */
const char *host_sd_directory(void);
/** @brief Return the recordings directory, or NULL while unmounted. */
const char *host_sd_recordings_dir(void);
/** @brief Synthesize a silent 16 kHz / 16-bit / stereo WAV recording. */
esp_err_t host_sd_write_wav(const char *name, uint32_t seconds);
/** @brief Delete all .wav/.part files; returns the removed count. */
size_t host_sd_clear_recordings(void);
/** @brief List .wav file names in the recordings directory. */
size_t host_sd_list_recordings(char names[][SIM_SD_NAME_MAX], size_t capacity);
/** @brief Toggle the audio service availability reported to the UI. */
void host_audio_set_available(bool available);

#endif /* __SIM_SD_HOST_H__ */
