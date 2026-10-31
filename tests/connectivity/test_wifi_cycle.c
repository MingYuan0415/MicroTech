#include "connectivity_manager.h"
#include "event_bus.h"
#include "app_manager_mailbox.h"
#include "freertos/task.h"
#include "host_freertos.h"
#include "host_nv_storage.h"
#include "host_wifi_port.h"
#include "wifi_service.h"
#include "wifi_service_port.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TEST_TIMEOUT_MS 5000U

#define CHECK(expression)                                                    \
    do                                                                       \
    {                                                                        \
        if (!(expression))                                                   \
        {                                                                    \
            fprintf(stderr, "CHECK failed at %s:%d: %s\n",                 \
                    __FILE__, __LINE__, #expression);                        \
            return false;                                                    \
        }                                                                    \
    } while (0)

static const connectivity_manager_config_t s_config =
{
    .task_priority = 4U,
    .wifi_task_priority = 4U,
};

static void _sleep_one_ms(void)
{
    const struct timespec delay = { .tv_sec = 0, .tv_nsec = 1000000L };
    (void)nanosleep(&delay, NULL);
}

static bool _wait_manager_state(connectivity_manager_state_t state,
                                connectivity_manager_status_snapshot_t *out)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t status;
        if (connectivity_manager_get_status(&status) == ESP_OK &&
                status.state == state)
        {
            if (out != NULL)
            {
                *out = status;
            }
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_scan_running(void)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_scan_snapshot_t scan;
        if (connectivity_manager_get_scan_snapshot(&scan) == ESP_OK &&
                scan.running)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_terminal(connectivity_manager_operation_id_t operation,
                           esp_err_t result)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t status;
        if (connectivity_manager_get_status(&status) == ESP_OK &&
                status.operation_id == operation &&
                status.operation_complete && status.last_error == result)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _wait_profile_ssid(const char *ssid)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_profile_snapshot_t profiles;
        if (connectivity_manager_get_profiles(&profiles) == ESP_OK)
        {
            for (uint8_t index = 0U; index < profiles.count; ++index)
            {
                if (strcmp(profiles.profiles[index].ssid, ssid) == 0)
                {
                    return true;
                }
            }
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _save_profile(const char *ssid, const char *password,
                          connectivity_manager_operation_id_t *out)
{
    const connectivity_manager_credentials_t credentials =
    {
        .ssid = ssid,
        .ssid_length = strlen(ssid),
        .password = password,
        .password_length = strlen(password),
        .security = CONNECTIVITY_MANAGER_SECURITY_PERSONAL,
    };
    if (connectivity_manager_request_save_profile(&credentials, out) != ESP_OK)
    {
        return false;
    }
    return _wait_profile_ssid(ssid);
}

static void _set_scan_record(const char *ssid,
                             connectivity_manager_security_t security)
{
    wifi_service_port_scan_record_t records[2];
    memset(records, 0, sizeof(records));
    const size_t length = strlen(ssid);
    records[0].ssid_length = (uint8_t)length;
    memcpy(records[0].ssid, ssid, length);
    records[0].rssi = -40;
    records[0].channel = 1U;
    records[0].security = security == CONNECTIVITY_MANAGER_SECURITY_OPEN ?
                          WIFI_SERVICE_SECURITY_OPEN :
                          WIFI_SERVICE_SECURITY_PERSONAL;
    host_wifi_port_set_scan_records(records, 1U, false);
}

static bool _submit_scan_done(void)
{
    const wifi_service_port_event_t event =
    {
        .type = WIFI_SERVICE_PORT_EVENT_SCAN_DONE,
        .epoch = host_wifi_port_epoch(),
        .status = 0,
        .scan_id = host_wifi_port_scan_id(),
    };
    return wifi_service_port_submit_event(&event) == ESP_OK;
}

static bool _submit_disconnect(void)
{
    const wifi_service_port_event_t event =
    {
        .type = WIFI_SERVICE_PORT_EVENT_STA_DISCONNECTED,
        .epoch = host_wifi_port_epoch(),
        .status = ESP_FAIL,
        .failure = WIFI_SERVICE_FAILURE_AP_NOT_FOUND,
        .disconnect_reason = 201U,
    };
    return wifi_service_port_submit_event(&event) == ESP_OK;
}

static bool _submit_got_ip(uint32_t address)
{
    const wifi_service_port_event_t event =
    {
        .type = WIFI_SERVICE_PORT_EVENT_GOT_IP,
        .epoch = host_wifi_port_epoch(),
        .status = ESP_OK,
        .ipv4_address = address,
    };
    return wifi_service_port_submit_event(&event) == ESP_OK;
}

static bool _complete_connection(void)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        wifi_service_status_snapshot_t radio;
        if (wifi_service_get_status(&radio) == ESP_OK &&
                radio.state == WIFI_SERVICE_STATE_CONNECTING)
        {
            break;
        }
        _sleep_one_ms();
    }
    const wifi_service_port_event_t connected =
    {
        .type = WIFI_SERVICE_PORT_EVENT_STA_CONNECTED,
        .epoch = host_wifi_port_epoch(),
        .status = ESP_OK,
    };
    if (wifi_service_port_submit_event(&connected) != ESP_OK ||
            !_wait_manager_state(CONNECTIVITY_MANAGER_STATE_WAITING_IP, NULL) ||
            !_submit_got_ip(UINT32_C(0x0a000001)))
    {
        return false;
    }
    return _wait_manager_state(CONNECTIVITY_MANAGER_STATE_IP_READY, NULL);
}

static bool _wait_connect_calls(unsigned expected)
{
    return host_wifi_port_wait_calls(HOST_WIFI_PORT_CONNECT, expected,
                                     TEST_TIMEOUT_MS);
}

static bool _test_cycle_matches_only_present_profile(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Present", "present12", &save));
    CHECK(_save_profile("Absent", "absent123", &save));
    _set_scan_record("Present", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(1U));
    CHECK(_complete_connection());
    CHECK(_wait_terminal(enable, ESP_OK));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == 1U);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: only a present saved profile is attempted");
    return true;
}

static bool _test_disable_stops_radio_and_reenable_cycles(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Toggle", "toggle12", &save));
    _set_scan_record("Toggle", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(1U));

    connectivity_manager_operation_id_t disable = 0U;
    CHECK(connectivity_manager_request_wifi_disable(&disable) == ESP_OK);
    connectivity_manager_status_snapshot_t status;
    bool off = false;
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (connectivity_manager_get_status(&status) == ESP_OK &&
                status.manual_hold && !status.wifi_enabled &&
                host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED)
        {
            off = true;
            break;
        }
        _sleep_one_ms();
    }
    CHECK(off);
    CHECK(host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED);
    const unsigned stopped_calls = host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT);
    for (unsigned attempt = 0U; attempt < 300U; ++attempt)
    {
        _sleep_one_ms();
    }
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == stopped_calls);

    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(stopped_calls + 1U));
    CHECK(_complete_connection());
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IP_READY, NULL));
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: disable stops radio, re-enable starts a fresh cycle");
    return true;
}

static bool _test_cycle_fails_over_to_next_profile(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("CycleA", "cyclea12", &save));
    CHECK(_save_profile("CycleB", "cycleb12", &save));
    wifi_service_port_scan_record_t records[2];
    memset(records, 0, sizeof(records));
    records[0].ssid_length = strlen("CycleA");
    memcpy(records[0].ssid, "CycleA", records[0].ssid_length);
    records[0].rssi = -40;
    records[0].channel = 1U;
    records[0].security = WIFI_SERVICE_SECURITY_PERSONAL;
    records[1].ssid_length = strlen("CycleB");
    memcpy(records[1].ssid, "CycleB", records[1].ssid_length);
    records[1].rssi = -50;
    records[1].channel = 6U;
    records[1].security = WIFI_SERVICE_SECURITY_PERSONAL;
    host_wifi_port_set_scan_records(records, 2U, false);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    /* CycleB is selected (last saved), so it is attempted first. */
    for (unsigned attempt = 0U; attempt < CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES;
            ++attempt)
    {
        CHECK(_wait_connect_calls(attempt + 1U));
        CHECK(_submit_disconnect());
    }
    CHECK(_wait_connect_calls(4U));
    CHECK(_complete_connection());
    CHECK(_wait_terminal(enable, ESP_OK));
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT) == 4U);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: per-profile attempts then next saved profile succeeds");
    return true;
}

static bool _test_cycle_fails_over_and_turns_off(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Bad", "badpass12", &save));
    _set_scan_record("Bad", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    connectivity_manager_status_snapshot_t status;
    for (unsigned attempt = 0U; attempt < CONNECTIVITY_MANAGER_MAX_CONNECT_FAILURES;
            ++attempt)
    {
        if (!_wait_connect_calls(attempt + 1U))
        {
            (void)connectivity_manager_get_status(&status);
            printf("cycle fail wait attempt=%u count=%u state=%d port=%d\n",
                   attempt, host_wifi_port_call_count(HOST_WIFI_PORT_CONNECT),
                   (int)status.state, (int)host_wifi_port_state());
            return false;
        }
        CHECK(_submit_disconnect());
    }
    bool off = false;
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (connectivity_manager_get_status(&status) == ESP_OK &&
                status.manual_hold && !status.wifi_enabled &&
                host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED)
        {
            off = true;
            break;
        }
        _sleep_one_ms();
    }
    CHECK(off);
    CHECK(host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: bounded attempts then radio off");
    return true;
}

static bool _wait_profiles_empty(void)
{
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_profile_snapshot_t profiles;
        if (connectivity_manager_get_profiles(&profiles) == ESP_OK &&
                profiles.count == 0U)
        {
            return true;
        }
        _sleep_one_ms();
    }
    return false;
}

static bool _test_forget_clears_profiles_and_nvs(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("ForgetMe", "forget12", &save));

    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget(&forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_OK));
    CHECK(_wait_profiles_empty());
    CHECK(!host_nv_storage_copy(NULL, 0U, NULL));
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.saved_profile);
    CHECK(host_nv_storage_erase_count() == 1U);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: forget clears profiles and persisted record");
    return true;
}

static bool _test_scan_timeout_turns_wifi_off(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Timeout", "timeout12", &save));
    _set_scan_record("Timeout", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());

    bool off = false;
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        connectivity_manager_status_snapshot_t status;
        if (connectivity_manager_get_status(&status) == ESP_OK &&
                status.manual_hold && !status.wifi_enabled &&
                host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED)
        {
            off = true;
            break;
        }
        _sleep_one_ms();
    }
    CHECK(off);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: scan timeout closes Wi-Fi");
    return true;
}

static bool _test_forget_active_link(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("ActiveForget", "active12", &save));
    _set_scan_record("ActiveForget",
                     CONNECTIVITY_MANAGER_SECURITY_PERSONAL);
    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(1U));
    CHECK(_complete_connection());
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IP_READY, NULL));

    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget(&forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_OK));
    CHECK(_wait_profiles_empty());
    CHECK(host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED);
    CHECK(host_nv_storage_erase_count() == 1U);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: forget clears an active link");
    return true;
}


static bool _test_cycle_start_storage_failure_terminal(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("StoreFail", "storefail1", &save));
    host_nv_storage_fail_next_set(ESP_FAIL);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_terminal(enable, ESP_FAIL));
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.wifi_enabled && status.manual_hold);
    CHECK(host_wifi_port_call_count(HOST_WIFI_PORT_START) == 0U);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: storage failure during enable has one terminal");
    return true;
}

static bool _test_disable_storage_failure_stops_radio(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("DisableFail", "disable12", &save));
    _set_scan_record("DisableFail", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);
    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(1U));
    CHECK(_complete_connection());

    host_nv_storage_fail_next_set(ESP_FAIL);
    connectivity_manager_operation_id_t disable = 0U;
    CHECK(connectivity_manager_request_wifi_disable(&disable) == ESP_OK);
    CHECK(_wait_terminal(disable, ESP_FAIL));
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.wifi_enabled && status.manual_hold);
    CHECK(host_wifi_port_state() == WIFI_SERVICE_PORT_STATE_STOPPED);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: storage failure still stops radio");
    return true;
}

static bool _test_multi_profile_failure_keeps_selected_ssid(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Selected", "selected1", &save));
    CHECK(_save_profile("Other", "other123", &save));
    connectivity_manager_status_snapshot_t before;
    CHECK(connectivity_manager_get_status(&before) == ESP_OK);
    char selected_ssid[CONNECTIVITY_MANAGER_SSID_MAX_BYTES + 1U] = { 0 };
    memcpy(selected_ssid, before.ssid, sizeof(before.ssid));
    CHECK(selected_ssid[0] != '\0');

    wifi_service_port_scan_record_t records[2];
    memset(records, 0, sizeof(records));
    records[0].ssid_length = strlen("Selected");
    memcpy(records[0].ssid, "Selected", records[0].ssid_length);
    records[0].rssi = -40;
    records[0].channel = 1U;
    records[0].security = WIFI_SERVICE_SECURITY_PERSONAL;
    records[1].ssid_length = strlen("Other");
    memcpy(records[1].ssid, "Other", records[1].ssid_length);
    records[1].rssi = -50;
    records[1].channel = 6U;
    records[1].security = WIFI_SERVICE_SECURITY_PERSONAL;
    host_wifi_port_set_scan_records(records, 2U, false);

    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    for (unsigned attempt = 0U; attempt < 6U; ++attempt)
    {
        CHECK(_wait_connect_calls(attempt + 1U));
        CHECK(_submit_disconnect());
    }
    bool off = false;
    connectivity_manager_status_snapshot_t status;
    for (unsigned attempt = 0U; attempt < TEST_TIMEOUT_MS; ++attempt)
    {
        if (connectivity_manager_get_status(&status) == ESP_OK &&
                status.manual_hold && !status.wifi_enabled)
        {
            off = true;
            break;
        }
        _sleep_one_ms();
    }
    CHECK(off);
    CHECK(strcmp(status.ssid, selected_ssid) == 0);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: failed sweep keeps selected profile SSID");
    return true;
}

static bool _test_scan_preempts_connect_after_terminal(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Preempt", "preempt1", &save));
    _set_scan_record("Preempt", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);
    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(1U));

    connectivity_manager_operation_id_t scan = 0U;
    CHECK(connectivity_manager_request_scan(&scan) == ESP_OK);
    connectivity_manager_scan_snapshot_t snapshot;
    CHECK(connectivity_manager_get_scan_snapshot(&snapshot) == ESP_OK);
    CHECK(!snapshot.running);
    CHECK(_submit_disconnect());
    CHECK(_wait_scan_running());

    CHECK(_submit_scan_done());
    CHECK(_wait_scan_running());
    /* Deinit owns the active scan here; the point of this test is that the
     * foreground scan started after the connect terminal, not the following
     * resumed-cycle timing. */
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: scan preempts connect after its terminal");
    return true;
}

static bool _test_forget_erase_failure_clears_state(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("EraseFail", "erasefail1", &save));
    _set_scan_record("EraseFail", CONNECTIVITY_MANAGER_SECURITY_PERSONAL);
    connectivity_manager_operation_id_t enable = 0U;
    CHECK(connectivity_manager_request_wifi_enable(&enable) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_SCANNING, NULL));
    CHECK(_wait_scan_running());
    CHECK(_submit_scan_done());
    CHECK(_wait_connect_calls(1U));
    CHECK(_complete_connection());

    host_nv_storage_fail_next_erase(ESP_FAIL);
    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget(&forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_FAIL));
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(status.manual_hold);
    CHECK(status.state == CONNECTIVITY_MANAGER_STATE_IDLE);
    CHECK(status.ipv4_address == 0U);
    CHECK(status.saved_profile);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: forget erase failure clears stale IP state");
    return true;
}

static bool _test_last_profile_removal_sets_manual_hold(void)
{
    host_nv_storage_reset();
    host_wifi_port_reset();
    CHECK(app_manager_mailbox_init() == ESP_OK);
    CHECK(event_bus_init() == ESP_OK);
    CHECK(connectivity_manager_init(&s_config) == ESP_OK);
    CHECK(_wait_manager_state(CONNECTIVITY_MANAGER_STATE_IDLE, NULL));

    connectivity_manager_operation_id_t save = 0U;
    CHECK(_save_profile("Only", "onlypass1", &save));
    connectivity_manager_operation_id_t forget = 0U;
    CHECK(connectivity_manager_request_forget_network("Only", strlen("Only"),
            &forget) == ESP_OK);
    CHECK(_wait_terminal(forget, ESP_OK));
    CHECK(_wait_profiles_empty());
    connectivity_manager_status_snapshot_t status;
    CHECK(connectivity_manager_get_status(&status) == ESP_OK);
    CHECK(!status.wifi_enabled);
    CHECK(status.manual_hold);
    CHECK(connectivity_manager_deinit(CONNECTIVITY_MANAGER_WAIT_FOREVER) == ESP_OK);
    event_bus_deinit();
    CHECK(app_manager_mailbox_deinit() == ESP_OK);
    puts("wifi cycle: last profile removal sets manual hold");
    return true;
}

int main(void)
{
    if (!_test_cycle_matches_only_present_profile())
    {
        return 1;
    }
    if (!_test_cycle_fails_over_to_next_profile())
    {
        return 1;
    }
    if (!_test_disable_stops_radio_and_reenable_cycles())
    {
        return 1;
    }
    if (!_test_cycle_fails_over_and_turns_off())
    {
        return 1;
    }
    if (!_test_forget_clears_profiles_and_nvs())
    {
        return 1;
    }
    if (!_test_scan_timeout_turns_wifi_off())
    {
        return 1;
    }
    if (!_test_forget_active_link())
    {
        return 1;
    }
    if (!_test_cycle_start_storage_failure_terminal())
    {
        return 1;
    }
    if (!_test_disable_storage_failure_stops_radio())
    {
        return 1;
    }
    if (!_test_multi_profile_failure_keeps_selected_ssid())
    {
        return 1;
    }
    if (!_test_scan_preempts_connect_after_terminal())
    {
        return 1;
    }
    if (!_test_forget_erase_failure_clears_state())
    {
        return 1;
    }
    if (!_test_last_profile_removal_sets_manual_hold())
    {
        return 1;
    }
    puts("wifi cycle tests passed");
    return 0;
}
