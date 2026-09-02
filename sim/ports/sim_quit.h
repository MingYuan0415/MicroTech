/** @file Cross-thread quit request (main loop vs signal/Agent). */
#ifndef SIM_QUIT_H
#define SIM_QUIT_H

#include <stdatomic.h>

/** @brief Nonzero once SIGINT/SIGTERM or sim.exit requests shutdown. */
extern _Atomic int sim_quit_flag;

#endif /* SIM_QUIT_H */
