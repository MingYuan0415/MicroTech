/** @file esp_mmap_assets host port extras. */
#ifndef SIM_MMAP_ASSETS_H
#define SIM_MMAP_ASSETS_H

/** @brief Point the fake mmap assets backend at a staged directory. */
void sim_mmap_assets_set_dir(const char *dir);

/** @brief Look up a loaded asset by staged file name (debug/agent aid). */
const uint8_t *sim_mmap_assets_blob(const char *name, size_t *out_size);

#endif /* SIM_MMAP_ASSETS_H */
