#ifndef __CONNECTIVITY_HOST_PSA_CRYPTO_H__
#define __CONNECTIVITY_HOST_PSA_CRYPTO_H__

#include <stddef.h>
#include <stdint.h>

/** @brief PSA status code, modeled on the Mbed TLS type. */
typedef int32_t psa_status_t;
/** @brief PSA algorithm identifier, modeled on the Mbed TLS type. */
typedef uint32_t psa_algorithm_t;

#define PSA_SUCCESS ((psa_status_t)0)
#define PSA_ERROR_GENERIC_ERROR ((psa_status_t)-132)
#define PSA_ERROR_NOT_PERMITTED ((psa_status_t)-133)
#define PSA_ERROR_BUFFER_TOO_SMALL ((psa_status_t)-138)
#define PSA_ERROR_COMMUNICATION_FAILURE ((psa_status_t)-145)
#define PSA_ERROR_INSUFFICIENT_MEMORY ((psa_status_t)-141)

#define PSA_ALG_SHA_256 ((psa_algorithm_t)0x02000009)

psa_status_t psa_crypto_init(void);
psa_status_t psa_hash_compute(psa_algorithm_t alg,
                              const uint8_t *input,
                              size_t input_length,
                              uint8_t *hash,
                              size_t hash_size,
                              size_t *hash_length);

#endif /* __CONNECTIVITY_HOST_PSA_CRYPTO_H__ */
