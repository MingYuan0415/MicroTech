/** @file Agent widget-tree dump (must be called with the LVGL lock held). */
#ifndef SIM_AGENT_TREE_H
#define SIM_AGENT_TREE_H

/** @brief Serialize the active screen tree; caller frees with free().
 *  @param include_layers when true, also attach the display top/sys layers
 *  under an "overlays" array (task switcher and other sys-layer UI live
 *  there and are otherwise invisible to the tree). */
char *sim_agent_tree_dump_active_screen(bool include_layers);

#endif /* SIM_AGENT_TREE_H */
