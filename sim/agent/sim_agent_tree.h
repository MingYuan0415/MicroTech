/** @file Agent widget-tree dump (must be called with the LVGL lock held). */
#ifndef SIM_AGENT_TREE_H
#define SIM_AGENT_TREE_H

/** @brief Serialize the active screen tree; caller frees with free(). */
char *sim_agent_tree_dump_active_screen(void);

#endif /* SIM_AGENT_TREE_H */
