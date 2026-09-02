/** @file Scripted HTTP responses for CI (bypasses live curl per match). */
#ifndef SIM_HTTP_H
#define SIM_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Register/replace a canned response for URLs containing the key. */
int sim_http_set_response(const char *url_substring, int status,
                          const char *content_type,
                          const uint8_t *body, size_t size);
/** @brief CI mode: unscripted URLs fail like an offline radio (no curl). */
int sim_http_set_script_only(bool enabled);

#endif /* SIM_HTTP_H */
