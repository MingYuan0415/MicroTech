/** @file Fault controls for the host PSA hash provider. */
#ifndef __CONNECTIVITY_HOST_PSA_H__
#define __CONNECTIVITY_HOST_PSA_H__

#include <stdbool.h>

#include "psa/crypto.h"

/** @brief Reset the provider and clear every injected fault. */
void connectivity_host_psa_reset(void);
/** @brief Make psa_crypto_init() return @p result. */
void connectivity_host_psa_fail_init(psa_status_t result);
/** @brief Make psa_hash_compute() return @p result. */
void connectivity_host_psa_fail_hash(psa_status_t result);
/** @brief Report one byte less than the requested digest size. */
void connectivity_host_psa_short_hash(bool enabled);
/** @brief Count psa_crypto_init() calls. */
unsigned connectivity_host_psa_init_calls(void);
/** @brief Count psa_hash_compute() calls. */
unsigned connectivity_host_psa_hash_calls(void);

#endif /* __CONNECTIVITY_HOST_PSA_H__ */
