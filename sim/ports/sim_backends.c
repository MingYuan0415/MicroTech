/** @file Scriptable fake hardware backends for real middleware services.
 *
 * The services themselves (power_service, imu_service, ...) are the real
 * sources; only their ops tables are faked here, mirroring bsp ops shapes.
 * M6's Agent sim.set_* commands route into these setters.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "imu_service.h"
#include "power_service.h"

/* ------------------------------------------------------------ power (PMU) */

static atomic_uint s_power_voltage_mv = 3920U;
static atomic_int s_power_percent = 82;
static atomic_bool s_power_charging;
static atomic_bool s_power_vbus;

static bool _power_is_available(void)
{
    return true;
}

static esp_err_t _power_get_info(power_info_t *info)
{
    if (info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    info->battery_voltage_mv = (uint16_t)atomic_load(&s_power_voltage_mv);
    info->battery_percent = (int8_t)atomic_load(&s_power_percent);
    info->is_charging = atomic_load(&s_power_charging);
    info->is_vbus_connected = atomic_load(&s_power_vbus);
    return ESP_OK;
}

static esp_err_t _power_poll_irq(uint32_t *status)
{
    if (status != NULL)
    {
        *status = 0U;
    }
    return ESP_OK;
}

const power_service_power_ops_t sim_power_ops =
{
    .is_available = _power_is_available,
    .get_info = _power_get_info,
    .poll_irq = _power_poll_irq,
};

void sim_backends_set_power(uint16_t voltage_mv, int8_t percent,
                            bool charging, bool vbus)
{
    atomic_store(&s_power_voltage_mv, voltage_mv);
    atomic_store(&s_power_percent, percent);
    atomic_store(&s_power_charging, charging);
    atomic_store(&s_power_vbus, vbus);
}

/* --------------------------------------------------------------- IMU */

static atomic_int s_imu_pitch_cdeg = 200;   /* 2.00 deg */
static atomic_int s_imu_roll_cdeg = -150;
static atomic_uint s_imu_sequence;

static bool _imu_is_available(void)
{
    return true;
}

static esp_err_t _imu_configure(uint32_t sample_rate_hz)
{
    (void)sample_rate_hz;
    return ESP_OK;
}

static esp_err_t _imu_read(imu_service_sample_t *sample)
{
    float pitch;
    float roll;
    struct timespec now;

    if (sample == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    pitch = (float)atomic_load(&s_imu_pitch_cdeg) / 100.0f;
    roll = (float)atomic_load(&s_imu_roll_cdeg) / 100.0f;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    sample->acceleration_mps2.x = 9.81f * sinf(roll * (float)M_PI / 180.0f);
    sample->acceleration_mps2.y = -9.81f * sinf(pitch * (float)M_PI / 180.0f);
    sample->acceleration_mps2.z = 9.81f * cosf(pitch * (float)M_PI / 180.0f) *
                                  cosf(roll * (float)M_PI / 180.0f);
    sample->angular_velocity_dps.x = 0.0f;
    sample->angular_velocity_dps.y = 0.0f;
    sample->angular_velocity_dps.z = 0.0f;
    sample->temperature_c = 31.5f;
    sample->sensor_timestamp = (uint32_t)(now.tv_nsec / 1000U);
    sample->status_int = 0U;
    sample->status0 = 0U;
    sample->status1 = 0U;
    sample->data_ready = true;
    sample->interrupt_active = false;
    sample->interrupt_level_valid = true;
    sample->sampled_at_us = (int64_t)now.tv_sec * 1000000 +
                            now.tv_nsec / 1000;
    sample->sequence = atomic_fetch_add(&s_imu_sequence, 1U) + 1U;
    return ESP_OK;
}

static esp_err_t _imu_set_enabled(bool enabled)
{
    (void)enabled;
    return ESP_OK;
}

static esp_err_t _imu_poll_interrupt(bool *active)
{
    if (active != NULL)
    {
        *active = false;
    }
    return ESP_OK;
}

const imu_service_imu_ops_t sim_imu_ops =
{
    .is_available = _imu_is_available,
    .configure = _imu_configure,
    .read = _imu_read,
    .set_enabled = _imu_set_enabled,
    .poll_interrupt = _imu_poll_interrupt,
};

void sim_backends_set_imu(int pitch_cdeg, int roll_cdeg)
{
    atomic_store(&s_imu_pitch_cdeg, pitch_cdeg);
    atomic_store(&s_imu_roll_cdeg, roll_cdeg);
}

/* --------------------------------------------------------- misc hooks */

void sim_backends_log_restart_request(void)
{
    fprintf(stderr, "sim: restart requested (process will continue)\n");
}

void sim_backends_restart(void *context)
{
    (void)context;
    sim_backends_log_restart_request();
}
