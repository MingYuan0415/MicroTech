/** @file Host PSA hash provider backed by the project test fixture. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "psa/crypto.h"

#include "tinycrypt/sha256.h"

/**
 * @brief Count the calls the digest implementation makes into PSA.
 *
 * The real implementation is compiled against these shims so its status and
 * length handling is exercised; the digest itself is computed by tinycrypt,
 * which is the only SHA-256 available to host tests without linking Mbed TLS.
 */
static unsigned s_crypto_init_calls;
static unsigned s_hash_calls;
static psa_status_t s_crypto_init_result = PSA_SUCCESS;
static psa_status_t s_hash_result = PSA_SUCCESS;
static bool s_short_hash;

void connectivity_host_psa_reset(void)
{
    s_crypto_init_calls = 0U;
    s_hash_calls = 0U;
    s_crypto_init_result = PSA_SUCCESS;
    s_hash_result = PSA_SUCCESS;
    s_short_hash = false;
}

void connectivity_host_psa_fail_init(psa_status_t result)
{
    s_crypto_init_result = result;
}

void connectivity_host_psa_fail_hash(psa_status_t result)
{
    s_hash_result = result;
}

void connectivity_host_psa_short_hash(bool enabled)
{
    s_short_hash = enabled;
}

unsigned connectivity_host_psa_init_calls(void)
{
    return s_crypto_init_calls;
}

unsigned connectivity_host_psa_hash_calls(void)
{
    return s_hash_calls;
}

psa_status_t psa_crypto_init(void)
{
    ++s_crypto_init_calls;
    return s_crypto_init_result;
}

psa_status_t psa_hash_compute(psa_algorithm_t alg,
                              const uint8_t *input,
                              size_t input_length,
                              uint8_t *hash,
                              size_t hash_size,
                              size_t *hash_length)
{
    ++s_hash_calls;
    if (alg != PSA_ALG_SHA_256 || hash == NULL || hash_length == NULL ||
            (input == NULL && input_length != 0U))
    {
        return PSA_ERROR_GENERIC_ERROR;
    }
    if (s_hash_result != PSA_SUCCESS)
    {
        return s_hash_result;
    }
    struct tc_sha256_state_struct state;

    /* tinycrypt rejects a NULL data pointer even for a zero-length message,
     * while PSA treats (NULL, 0) as the empty message. */
    memset(&state, 0, sizeof(state));
    if (tc_sha256_init(&state) != 1 ||
            (input_length != 0U &&
             tc_sha256_update(&state, input, input_length) != 1) ||
            tc_sha256_final(hash, &state) != 1)
    {
        return PSA_ERROR_GENERIC_ERROR;
    }
    memset(&state, 0, sizeof(state));
    *hash_length = s_short_hash ? hash_size - 1U : hash_size;
    return PSA_SUCCESS;
}
