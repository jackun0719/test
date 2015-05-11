/**
 * @file
 */
/******************************************************************************
* Copyright 2013-2014, Qualcomm Connected Experiences, Inc.
*
******************************************************************************/

#include "aj_nvram.h"
#include "qcom/qcom_dset.h"
#include "dsetid.h"
#define AJ_DATA_ID DSETID_ALLJOYN_START

#include "aj_target_nvram.h"

uint32_t AJ_EMULATED_NVRAM[AJ_NVRAM_SIZE/4];
uint8_t* AJ_NVRAM_BASE_ADDRESS;

static int WriteToFlash()
{
    int32_t status = A_ERROR;
    uint32_t handle = 0;

    // overwrite the dset if it already exists
    qcom_dset_delete(AJ_DATA_ID, DSET_MEDIA_NVRAM, NULL, NULL);

    // first create the dset (or overwrite if it already exists)
    status = qcom_dset_create(&handle, AJ_DATA_ID, AJ_NVRAM_SIZE, DSET_MEDIA_NVRAM, NULL, NULL);
    if (status != A_OK) {
        // failed to create; do nothing?
        return -1;
    }

    // now write the entire emulated NVRAM chunk to the dset
    status = qcom_dset_write(
        handle,
        (A_UINT8*)AJ_EMULATED_NVRAM, sizeof(AJ_EMULATED_NVRAM),
        0, DSET_MEDIA_NVRAM, NULL, NULL);

    if (status == A_OK) {
        // now commit
        qcom_dset_commit(handle, NULL, NULL);
    }

    // finally, close the handle
    qcom_dset_close(handle, NULL, NULL);
    return AJ_OK;
}

static int ReadFromFlash()
{
    int32_t status;
    uint32_t handle;

    // attempt to open the dset
    status = qcom_dset_open(&handle, AJ_DATA_ID, DSET_MEDIA_NVRAM, NULL, NULL);
    if (status != A_OK) {
        // there might not be a dset on the device at the moment,
        return AJ_OK;
    }

    // now read from the dset into RAM
    qcom_dset_read(handle, (A_UINT8*)AJ_EMULATED_NVRAM, sizeof(AJ_EMULATED_NVRAM), 0, NULL, NULL);

    // finally, close the handle
    qcom_dset_close(handle, NULL, NULL);
    return AJ_OK;
}

void AJ_NVRAM_Init()
{
    AJ_NVRAM_BASE_ADDRESS = (uint8_t* )AJ_EMULATED_NVRAM;
    static uint8_t inited = FALSE;
    if (!inited) {
        inited = TRUE;

        memset(AJ_NVRAM_BASE_ADDRESS, INVALID_DATA_BYTE, AJ_NVRAM_SIZE);

        if (ReadFromFlash() == AJ_OK) {
//        	AJ_NVRAM_Layout_Print();
        }

        if (*((uint32_t*) AJ_NVRAM_BASE_ADDRESS) != AJ_NV_SENTINEL) {
            AJ_WarnPrintf(("AJ_NVRAM_Init: No sentinel found\n"));
            _AJ_NVRAM_Clear();
        }
    }
}
static uint8_t g_flashFlag = TRUE;
void _AJ_NV_Write(void* dest, void* buf, uint16_t size)
{
    memcpy(dest, buf, size);
    if (g_flashFlag == TRUE) {
   		WriteToFlash();
    }
}

void _AJ_NV_Read(void* src, void* buf, uint16_t size)
{
   memcpy(buf, src, size);
}

void _AJ_NVRAM_Clear()
{
    memset(AJ_NVRAM_BASE_ADDRESS, INVALID_DATA_BYTE, AJ_NVRAM_SIZE);
    *((uint32_t*) AJ_NVRAM_BASE_ADDRESS) = AJ_NV_SENTINEL;
    WriteToFlash();
}

// Compact the storage by removing invalid entries
AJ_Status _AJ_CompactNVStorage()
{
    uint16_t* data = (uint16_t*)(AJ_NVRAM_BASE_ADDRESS + SENTINEL_OFFSET);
    uint8_t* writePtr = (uint8_t*) data;
    uint16_t entrySize = 0;
    uint16_t garbage = 0;
    while ((uint8_t*) data < (uint8_t*) AJ_NVRAM_END_ADDRESS && *data != INVALID_DATA) {
        NV_EntryHeader* header = (NV_EntryHeader*) data;
        entrySize = ENTRY_HEADER_SIZE + header->capacity;
        if (header->id != INVALID_ID) {
            // do a single commit at the end
            memcpy(writePtr, data, entrySize);
            writePtr += entrySize;
        } else {
            garbage += entrySize;
        }
        data += entrySize >> 1;
    }

    memset(writePtr, INVALID_DATA_BYTE, garbage);
    WriteToFlash();
    return AJ_OK;
}
