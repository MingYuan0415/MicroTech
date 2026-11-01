/** @file Base64 decoder for the host music-service backend. */
#include "mbedtls/base64.h"

#include <stdbool.h>
#include <stdint.h>

static int _value(unsigned char ch)
{
    if (ch >= 'A' && ch <= 'Z')
    {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z')
    {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0' + 52;
    }
    if (ch == '+')
    {
        return 62;
    }
    if (ch == '/')
    {
        return 63;
    }
    return -1;
}

int mbedtls_base64_decode(unsigned char *dst, size_t dlen, size_t *olen,
                          const unsigned char *src, size_t slen)
{
    if (dst == NULL || src == NULL)
    {
        return -1;
    }
    size_t written = 0U;
    uint32_t accumulator = 0U;
    unsigned bits = 0U;
    bool padding = false;

    for (size_t index = 0U; index < slen; ++index)
    {
        const unsigned char ch = src[index];
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t')
        {
            continue;
        }
        if (ch == '=')
        {
            padding = true;
            continue;
        }
        if (padding)
        {
            return -1;
        }
        const int value = _value(ch);
        if (value < 0)
        {
            return -1;
        }
        accumulator = (accumulator << 6U) | (uint32_t)value;
        bits += 6U;
        if (bits >= 8U)
        {
            bits -= 8U;
            if (written >= dlen)
            {
                return -2;
            }
            dst[written++] = (unsigned char)(accumulator >> bits);
            accumulator &= (UINT32_C(1) << bits) - 1U;
        }
    }
    if (bits >= 6U)
    {
        return -1;
    }
    if (olen != NULL)
    {
        *olen = written;
    }
    return 0;
}
