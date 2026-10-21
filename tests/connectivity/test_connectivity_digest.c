/** @file Regression tests for the production PSA-backed sync digest. */
#include "connectivity_manager_digest.h"
#include "host_psa.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* SHA-256("abc"), the canonical FIPS 180-4 vector. */
static const uint8_t s_abc_digest[32U] =
{
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
};

/* SHA-256("") — the empty message still produces a digest. */
static const uint8_t s_empty_digest[32U] =
{
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
    0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
    0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55,
};

static void _test_known_vectors(void)
{
    uint8_t digest[32U];

    connectivity_host_psa_reset();
    memset(digest, 0xA5, sizeof(digest));
    assert(connectivity_manager_digest_sha256((const uint8_t *)"abc", 3U,
            digest) == ESP_OK);
    assert(memcmp(digest, s_abc_digest, sizeof(digest)) == 0);
    /* The implementation initializes PSA before hashing. */
    assert(connectivity_host_psa_init_calls() == 1U);
    assert(connectivity_host_psa_hash_calls() == 1U);

    connectivity_host_psa_reset();
    memset(digest, 0xA5, sizeof(digest));
    assert(connectivity_manager_digest_sha256(NULL, 0U, digest) == ESP_OK);
    assert(memcmp(digest, s_empty_digest, sizeof(digest)) == 0);
}

static void _test_argument_validation(void)
{
    uint8_t digest[32U];

    connectivity_host_psa_reset();
    assert(connectivity_manager_digest_sha256(NULL, 1U, digest) ==
           ESP_ERR_INVALID_ARG);
    assert(connectivity_manager_digest_sha256((const uint8_t *)"x", 1U,
            NULL) == ESP_ERR_INVALID_ARG);
    /* Rejected calls must not reach the crypto backend. */
    assert(connectivity_host_psa_init_calls() == 0U);
    assert(connectivity_host_psa_hash_calls() == 0U);
}

static void _test_backend_failure_zeroes_digest(void)
{
    uint8_t digest[32U];

    connectivity_host_psa_reset();
    connectivity_host_psa_fail_init(PSA_ERROR_COMMUNICATION_FAILURE);
    memset(digest, 0xA5, sizeof(digest));
    assert(connectivity_manager_digest_sha256((const uint8_t *)"abc", 3U,
            digest) == ESP_ERR_INVALID_STATE);
    /* A failed digest must never be published as if it were a sync identity. */
    for (size_t index = 0U; index < sizeof(digest); ++index)
    {
        assert(digest[index] == 0U);
    }
    assert(connectivity_host_psa_hash_calls() == 0U);

    connectivity_host_psa_reset();
    connectivity_host_psa_fail_hash(PSA_ERROR_INSUFFICIENT_MEMORY);
    memset(digest, 0xA5, sizeof(digest));
    assert(connectivity_manager_digest_sha256((const uint8_t *)"abc", 3U,
            digest) == ESP_ERR_INVALID_STATE);
    for (size_t index = 0U; index < sizeof(digest); ++index)
    {
        assert(digest[index] == 0U);
    }

    /* A short hash is a failure, not a truncated digest. */
    connectivity_host_psa_reset();
    connectivity_host_psa_short_hash(true);
    memset(digest, 0xA5, sizeof(digest));
    assert(connectivity_manager_digest_sha256((const uint8_t *)"abc", 3U,
            digest) == ESP_ERR_INVALID_STATE);
    for (size_t index = 0U; index < sizeof(digest); ++index)
    {
        assert(digest[index] == 0U);
    }
}

int main(void)
{
    _test_known_vectors();
    _test_argument_validation();
    _test_backend_failure_zeroes_digest();
    puts("connectivity manager digest tests passed");
    return 0;
}
