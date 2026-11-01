/** @file Minimal mbedtls base64 compatibility for the host music backend. */
#ifndef __SIM_MBEDTLS_BASE64_H__
#define __SIM_MBEDTLS_BASE64_H__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decode one Base64 string.
 *
 * @return 0 on success, -1 for malformed input, -2 when the output is short.
 */
int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen);

#ifdef __cplusplus
}
#endif

#endif /* __SIM_MBEDTLS_BASE64_H__ */
