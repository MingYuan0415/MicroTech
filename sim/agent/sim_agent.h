/** @file Agent bridge: TCP line-delimited JSON-RPC control channel.
 *
 * Listens on 127.0.0.1:<port> (never 0.0.0.0; never 5001 - reserved for the
 * on-device display benchmark). One connection is served per thread;
 * commands are serialized by a process-wide RPC mutex. Replies are written
 * with write(2) on the socket fd (not FILE*) because POSIX forbids switching
 * from fgets to fputs on an r+ stream without an intervening seek.
 *
 * Lock discipline (design doc "线程模型"):
 *  - read-only LVGL inspection (tree, screenshot copy): hold
 *    esp_lv_adapter_lock, never wait for the worker while holding it;
 *  - step / wait_idle / navigate / set_*: never take the LVGL lock except
 *    through the primitives that do so internally;
 *  - touch/key: atomic state only.
 */
#ifndef SIM_AGENT_H
#define SIM_AGENT_H

#include <stdbool.h>

/** @brief Start the agent server thread (no-op on failure returns false). */
bool sim_agent_start(int port, const char *out_dir);
/** @brief Signal the accept loop to stop. */
void sim_agent_stop(void);

#endif /* SIM_AGENT_H */
