/**
 * @file
 */
/******************************************************************************
* Copyright 2012-2014, Qualcomm Connected Experiences, Inc.
*
******************************************************************************/
#define AJ_MODULE TARGET_CRYPTO

#include "aj_target.h"
#include "aj_util.h"
#include "aj_crypto.h"
#include <qcom/qcom_security.h>
#include "aj_debug.h"

/**
 * Turn on per-module debug printing by setting this variable to non-zero value
 * (usually in debugger).
 */
#ifndef NDEBUG
uint8_t dbgTARGET_CRYPTO = 0;
#endif


static uint8_t seed[16];
static uint8_t key[16];

static int gatherBits(uint8_t* buffer, uint32_t len)
{
    /*
     * TODO - this is a very crappy entropy gathering function
     */
    while (len--) {
        *buffer++ ^= (uint8_t) AJ_GetElapsedTime(NULL, 0);
    }
}

static uint8_t entropyBits[32];
static uint32_t entropyIndex;
static uint8_t entropyInitialized = FALSE;

void AJ_EntropyBits(uint8_t bits)
{
    AJ_InfoPrintf(("entropyBits(): %02x\n", bits));
    entropyBits[entropyIndex++ & 0x1f] ^= bits;
}

static void AJ_EntropyUpdate()
{
    AJ_DumpBytes("AJ_Entropy", entropyBits, 32);
    memcpy(seed, entropyBits, sizeof(seed));
    memcpy(key, &entropyBits[16], sizeof(key));
}

void AJ_RandBytes(uint8_t* rand, uint32_t len)
{
    /*
     * On the first call we need to accumulate entropy
     * for the seed and the key.
     */
    if (!entropyInitialized) {
        entropyInitialized = TRUE;
        gatherBits(entropyBits, sizeof(entropyBits));
    }
    AJ_EntropyUpdate();
    AJ_AES_Enable(key);
    /*
     * This follows the NIST guidelines for using AES as a PRF
     */
    while (len) {
        uint32_t tmp[4];
        uint32_t sz = min(16, len);
        tmp[0] = (uint32_t) AJ_GetElapsedTime(NULL, 0);
        tmp[1] += 1;
        AJ_AES_ECB_128_ENCRYPT(key, (uint8_t*)tmp, (uint8_t*)tmp);
        AJ_AES_CBC_128_ENCRYPT(key, seed, seed, 16, (uint8_t*)tmp);
        memcpy(rand, seed, sz);
        AJ_AES_CBC_128_ENCRYPT(key, seed, seed, 16, (uint8_t*)tmp);
        len -= sz;
        rand += sz;
    }
    AJ_AES_Disable();
}

static void* keyState;

void AJ_AES_Enable(const uint8_t* key)
{
    keyState = qcom_aes_encrypt_init(key, 16);
}

void AJ_AES_Disable(void)
{
    qcom_aes_encrypt_deinit(keyState);
}

void AJ_AES_CTR_128(const uint8_t* key, const uint8_t* in, uint8_t* out, uint32_t len, uint8_t* ctr)
{
    while (len) {
        size_t n = min(len, 16);
        uint8_t enc[16];
        uint8_t* p = enc;
        uint16_t counter = (ctr[14] << 8) | ctr[15];
        len -= n;
        qcom_aes_encrypt(keyState, ctr, enc);
        while (n--) {
            *out++ = *p++ ^ *in++;
        }
        ++counter;
        ctr[15] = counter;
        ctr[14] = counter >> 8;
    }
}

void AJ_AES_CBC_128_ENCRYPT(const uint8_t* key, const uint8_t* in, uint8_t* out, uint32_t len, uint8_t* iv)
{
    uint8_t xorbuf[16];

    const uint8_t* ivec = iv;
    memcpy(xorbuf, iv, 16);
    while (len) {
        int i;
        for (i = 0; i < 16; ++i) {
            xorbuf[i] ^= *in++;
        }
        qcom_aes_encrypt(keyState, xorbuf, out);
        memcpy(xorbuf, out, 16);
        memcpy((uint8_t*) ivec, out, 16);
        out += 16;
        len -= 16;
    }
}

void AJ_AES_ECB_128_ENCRYPT(const uint8_t* key, const uint8_t* in, uint8_t* out)
{
    qcom_aes_encrypt(keyState, in, out);
}

