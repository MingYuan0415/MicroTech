/** @file Deterministic fake music-service fixtures for the host simulator. */
#ifndef __SIM_FAKES_MUSIC_H__
#define __SIM_FAKES_MUSIC_H__

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Current fake fixture after the last successful application. */
typedef struct sim_music_fixture_status
{
    const char *fixture;
    uint64_t generation;
    uint32_t state;
    uint32_t failure;
    uint32_t player_state;
    size_t home_count;
    size_t playlist_count;
    size_t lyric_lines;
    bool qr_available;
} sim_music_fixture_status_t;

/**
 * @brief Replace the complete fake music state.
 *
 * The operation is atomic with respect to service readers and is safe to
 * repeat. Every fixture starts from an empty state before applying its data.
 *
 * @param name fixture identifier.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unknown fixture, or
 *         ESP_ERR_NO_MEM while allocating snapshot data.
 */
esp_err_t host_music_service_set_fixture(const char *name);

/** @brief Copy the current fixture name and compact state summary. */
esp_err_t host_music_service_get_fixture_status(
    sim_music_fixture_status_t *status);

/** @brief Move a waiting fake QR session to the scanned state. */
esp_err_t host_music_service_login_scan(void);

/** @brief Complete the fake QR session and publish a logged-in catalog. */
esp_err_t host_music_service_login_confirm(void);

#endif /* __SIM_FAKES_MUSIC_H__ */
