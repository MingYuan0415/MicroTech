/** @file Agent widget-tree dump (must be called with the LVGL lock held). */
#ifndef SIM_AGENT_TREE_H
#define SIM_AGENT_TREE_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/** @brief Serialize the active screen tree; caller frees with free().
 *  @param include_layers when true, also attach the display top/sys layers
 *  under an "overlays" array (task switcher and other sys-layer UI live
 *  there and are otherwise invisible to the tree). */
char *sim_agent_tree_dump_active_screen(bool include_layers);

/** @brief Serialize one object subtree; caller frees with free().
 *  @param depth 0 excludes children, -1 is unlimited.
 *  @param max_nodes bounds the number of serialized nodes.
 *  @param truncated receives true when max_nodes stopped the walk. */
char *sim_agent_tree_dump_object(lv_obj_t *obj, int depth,
                                 uint32_t max_nodes, bool *truncated);

#endif /* SIM_AGENT_TREE_H */
