/** @file Agent-side query helpers for the widget tree (LVGL lock held). */
#ifndef SIM_AGENT_QUERY_H
#define SIM_AGENT_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Find nodes matching a flat query; returns a JSON string or NULL. */
char *sim_agent_query_find_active_screen(const char *query_json,
        bool include_layers,
        uint32_t limit,
        bool visible_only,
        const char **error);

/** Serialize one path under a named layer; returns a JSON string or NULL. */
char *sim_agent_query_inspect_active_screen(const char *layer,
        const int32_t *path,
        size_t path_len,
        uint32_t depth,
        uint32_t max_nodes,
        bool *truncated,
        const char **error);

/** Hit-test a point and return the topmost object. */
char *sim_agent_query_hit_test(int32_t x, int32_t y, const char **error);

/** Return geometry for a point or a layer/path object. */
char *sim_agent_query_geometry(int32_t x, int32_t y, bool use_point,
                               const char *layer, const int32_t *path,
                               size_t path_len, const char **error);

#endif /* SIM_AGENT_QUERY_H */
