/** @file Host-backed SD volume and audio availability controls for the sim. */
#ifndef __SIM_SD_HOST_H__
#define __SIM_SD_HOST_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Maximum basename bytes including NUL for simulator WAV files. */
#define SIM_SD_NAME_MAX 64

/** @brief Use a host directory; format is permitted only for a fresh owned sandbox. */
esp_err_t host_sd_boot(const char *dir, bool owned_sandbox);
/** @brief Rename the backing directory in/out to fake mount transitions. */
bool host_sd_set_mounted(bool mounted);
/** @brief Report whether the SD volume currently resolves. */
bool host_sd_is_mounted(void);
/** @brief Return the mounted volume path, or NULL while unmounted. */
const char *host_sd_directory(void);
/** @brief Override FAT capacity reporting with deterministic values (CI) so
 *  goldens do not depend on the host filesystem. Pass deterministic=false to
 *  restore real statvfs reporting. */
void host_sd_set_report_capacity(bool deterministic, uint64_t total_bytes,
                                 uint64_t free_bytes);
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

/** @brief Script the next mount attempt to fail without formatting. */
void host_sd_set_mount_failure(bool fail);
/** @brief Script the next format attempt to fail after erasing nothing. */
void host_sd_set_format_failure(bool fail);
/** @brief Inject a capacity-query failure without changing free space to zero. */
void host_sd_set_space_failure(bool fail);
/** @brief Count capacity-query failures the adapter reported. */
uint64_t host_sd_space_error_count(void);
/** @brief Return the host adapter registered with sd_storage_service. */
struct sd_storage_service_mount_ops;
const struct sd_storage_service_mount_ops *sim_sd_mount_ops(void);

#endif /* __SIM_SD_HOST_H__ */
