#ifndef __HOST_FACTORY_RESET_SERVICE_H__
#define __HOST_FACTORY_RESET_SERVICE_H__

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reset the process-lifetime factory-reset service fake. */
void host_factory_reset_service_reset(void);

/** @brief Return how often a factory reset was requested. */
unsigned host_factory_reset_service_request_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_FACTORY_RESET_SERVICE_H__ */
