#ifndef __DISPLAY_BENCHMARK_HOST_PORT_H__
#define __DISPLAY_BENCHMARK_HOST_PORT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void display_benchmark_host_port_reset(void);
void display_benchmark_host_port_fail_next_create(void);
void display_benchmark_host_port_fail_create_on(size_t index);
void display_benchmark_host_port_hide_external_task(const char *name);
void display_benchmark_host_port_fail_next_pm_inhibit(void);
bool display_benchmark_host_port_pm_inhibited(void);
size_t display_benchmark_host_port_create_count(void);
size_t display_benchmark_host_port_delete_count(void);
uint32_t display_benchmark_host_port_stack_depth(size_t index);
unsigned display_benchmark_host_port_stack_caps(size_t index);
unsigned display_benchmark_host_port_priority(size_t index);
int display_benchmark_host_port_core_id(size_t index);
const char *display_benchmark_host_port_deleted_name(size_t index);

#endif /* __DISPLAY_BENCHMARK_HOST_PORT_H__ */
