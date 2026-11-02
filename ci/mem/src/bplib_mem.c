/* Local integration modifications (banderson777, 2026); see LOCAL_CHANGES.md. */
/*
 * NASA Docket No. GSC-19,559-1, and identified as "Delay/Disruption Tolerant Networking 
 * (DTN) Bundle Protocol (BP) v7 Core Flight System (cFS) Application Build 7.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this 
 * file except in compliance with the License. You may obtain a copy of the License at 
 *
 * http://www.apache.org/licenses/LICENSE-2.0 
 *
 * Unless required by applicable law or agreed to in writing, software distributed under 
 * the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF 
 * ANY KIND, either express or implied. See the License for the specific language 
 * governing permissions and limitations under the License. The copyright notice to be 
 * included in the software is as follows: 
 *
 * Copyright 2025 United States Government as represented by the Administrator of the 
 * National Aeronautics and Space Administration. All Rights Reserved.
 *
 */

/* ======== */
/* Includes */
/* ======== */

#include "bplib_mem.h"
#include "bplib_bblocks.h"
#include <stdlib.h>
#include "bplib_ct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*******************************************************************************
* Exported Functions
*/
BPLib_Status_t BPLib_MEM_PoolInit(BPLib_MEM_Pool_t* pool, void* init_mem, size_t init_size)
{
    if (pool == NULL || init_mem == NULL || init_size == 0)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Sanity check that the struct sizes have not changed and exceeded the block data sizes */
    if (sizeof(BPLib_Bundle_t) > BPLIB_MEM_BIG_BLK_DATA_SIZE)
    {
        printf("MEM ERROR: Bundle size (%ld) is bigger than big block size (%ld)\n",
                    sizeof(BPLib_Bundle_t), BPLIB_MEM_BIG_BLK_DATA_SIZE);
        return BPLIB_ERROR;
    }
    if (sizeof(BPLib_CT_DbEntry_t) > BPLIB_MEM_SMALL_BLK_DATA_SIZE)
    {
        printf("MEM ERROR: CTDB entry (%ld) is bigger than small block size (%ld)\n",
                    sizeof(BPLib_CT_DbEntry_t), BPLIB_MEM_SMALL_BLK_DATA_SIZE);
        return BPLIB_ERROR;        
    }

    memset(pool, 0, sizeof(BPLib_MEM_Pool_t));
    memset(init_mem, 0, init_size);

    pthread_mutex_init(&pool->lock, NULL);
    
    return BPLib_MEM_PoolImplInit(&pool->impl, init_mem, init_size,
        sizeof(BPLib_MEM_Block_t));
}

void BPLib_MEM_PoolDestroy(BPLib_MEM_Pool_t* pool)
{
    if (pool == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&pool->lock);
    BPLib_MEM_PoolImplDestroy(&pool->impl);
    memset(pool, 0, sizeof(BPLib_MEM_Pool_t));
}

BPLib_MEM_Block_t* BPLib_MEM_BlockAlloc(BPLib_MEM_Pool_t* pool, size_t Size)
{
    BPLib_MEM_Block_t* block;

    if (pool == NULL)
    {
        return NULL;
    }

    pthread_mutex_lock(&pool->lock);
    
    block = (BPLib_MEM_Block_t*)(BPLib_MEM_PoolImplAlloc(&pool->impl, Size));

    if (block != NULL)
    {
        block->used_len = 0;
        block->next = NULL;

        if (BPLib_MEM_GetBytesInUse(pool) > pool->HighWaterMark)
        {
            pool->HighWaterMark = BPLib_MEM_GetBytesInUse(pool);
        }
    }

    pthread_mutex_unlock(&pool->lock);

    return block;
}

void BPLib_MEM_BlockFree(BPLib_MEM_Pool_t* pool, BPLib_MEM_Block_t* block)
{
    if (pool == NULL || block == NULL)
    {
        return;
    }

    pthread_mutex_lock(&pool->lock);
    BPLib_MEM_PoolImplFree(&pool->impl, (void*)block);
    pthread_mutex_unlock(&pool->lock);
}

BPLib_MEM_Block_t* BPLib_MEM_BlockListAlloc(BPLib_MEM_Pool_t* pool, size_t byte_len, size_t BlockSize)
{
    size_t bytes_alloc, blocks_alloc;
    BPLib_MEM_Block_t* head;
    BPLib_MEM_Block_t* curr_tail;
    BPLib_MEM_Block_t* new_block;
    size_t BlockDataSize;

    if (pool == NULL || byte_len == 0)
    {
        return NULL;
    }

    if (BlockSize != BPLIB_MEM_BIG_BLK_SIZE && BlockSize != BPLIB_MEM_SMALL_BLK_SIZE)
    {
        return NULL;
    }

    BlockDataSize = BlockSize - BPLIB_MEM_META_DATA_SIZE;

    head = NULL;
    bytes_alloc = 0;
    blocks_alloc = 0;
    do
    {
        new_block = BPLib_MEM_BlockAlloc(pool, BlockSize);
        if (new_block == NULL)
        {
            BPLib_MEM_BlockListFree(pool, head);
            return NULL;
        }
        blocks_alloc++;
        bytes_alloc += BlockDataSize;

        if (head == NULL)
        {
            head = new_block;
            curr_tail = head;
        }
        else
        {
            curr_tail->next = new_block;
            curr_tail = new_block;
        }
    } while (bytes_alloc < byte_len);

    return head;
}

void BPLib_MEM_BlockListFree(BPLib_MEM_Pool_t* pool, BPLib_MEM_Block_t* head)
{
    BPLib_MEM_Block_t* to_free;
    BPLib_MEM_Block_t* curr;

    if (pool == NULL || head == NULL)
    {
        return;
    }

    curr = head;
    while (curr != NULL)
    {
        to_free = curr;
        curr = curr->next;
        BPLib_MEM_BlockFree(pool, to_free);
    }
}

BPLib_Bundle_t* BPLib_MEM_BundleAlloc(BPLib_MEM_Pool_t* pool, const void* blob_data, size_t data_len)
{
    BPLib_Bundle_t    *bundle;
    BPLib_MEM_Block_t *curr_block;
    size_t            copy_len;
    size_t            bytes_copied;
    size_t            bytes_remaining;

    /* NULL Checks */
    if ((pool == NULL) || (blob_data == NULL))
    {
        return NULL;
    }

    /* Allocate a MEM_Block_t for the Bundle Metadata (bblocks) */
    curr_block = BPLib_MEM_BlockAlloc(pool, BPLIB_MEM_BIG_BLK_SIZE);
    if (curr_block == NULL)
    {
        return NULL;
    }

    memset(&curr_block->user_data.Bundle, 0, sizeof(BPLib_Bundle_t));
    curr_block->used_len = sizeof(BPLib_Bundle_t);
    bundle               = (BPLib_Bundle_t *)(&curr_block->user_data.Bundle);

    /* Allocate a blob */
    curr_block = BPLib_MEM_BlockListAlloc(pool, data_len, BPLIB_MEM_BIG_BLK_SIZE);
    if (curr_block == NULL)
    {
        BPLib_MEM_BlockFree(pool, BPLib_MEM_GetBlockFromUserData(bundle));
        return NULL;
    }

    bundle->blob = curr_block;

    /* Copy the blob_data in to the bundle's blob MEM_Block_t list */
    bytes_copied = 0;
    while (curr_block != NULL)
    {
        bytes_remaining = data_len - bytes_copied;
        copy_len        = (bytes_remaining < BPLIB_MEM_BIG_BLK_DATA_SIZE) ? bytes_remaining : BPLIB_MEM_BIG_BLK_DATA_SIZE;

        memcpy(curr_block->user_data.BigData,
                (void*)((uintptr_t)(blob_data) + bytes_copied),
                copy_len);

        curr_block->used_len = copy_len;

        /* Go to the next block */
        bytes_copied += copy_len;
        curr_block = curr_block->next;
    }

    /* Save the total size of the bundle (decoded + encoded) in bytes */
    bundle->Meta.TotalBytes = bytes_copied + sizeof(BPLib_BBlocks_t);

    /* Initialize admin record payload to null (most bundles will not use this) */
    bundle->blocks.AdminRecordPayload = NULL;

    return bundle;
}

void BPLib_MEM_BundleFree(BPLib_MEM_Pool_t* pool, BPLib_Bundle_t* bundle)
{
    /* NULL Checks */
    if ((pool == NULL) || (bundle == NULL))
    {
        return;
    }

    /* Note: The cast here from bundle back to MEM_Block_t is relying
    ** on the user being absolutely sure they obtained this bundle from
    ** the BundleAlloc function. Passing in a bundle_t from somewhere else
    ** is undefined behavior, and is noted in the docstring.
    */
    BPLib_MEM_BlockListFree(pool, bundle->blob);

    if (bundle->blocks.AdminRecordPayload != NULL)
    {
        BPLib_MEM_BlockFree(pool, BPLib_MEM_GetBlockFromUserData(bundle->blocks.AdminRecordPayload));
    }

    BPLib_MEM_BlockFree(pool, BPLib_MEM_GetBlockFromUserData(bundle));
}

BPLib_Status_t BPLib_MEM_BlobCopyOut(BPLib_Bundle_t* bundle, void* out_buffer, size_t max_len, size_t* out_size)
{
    BPLib_MEM_Block_t* curr_block;
    size_t bytes_copied;

    if ((bundle == NULL) || (out_buffer == NULL) || (out_size == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    curr_block = bundle->blob;
    bytes_copied = 0;
    while (curr_block != NULL)
    {
        if ((bytes_copied + curr_block->used_len) > max_len)
        {
            *out_size = 0;
            return BPLIB_BUF_LEN_ERROR;
        }

        memcpy((void*)((uintptr_t)out_buffer + bytes_copied),
            (void*)curr_block->user_data.BigData,
            curr_block->used_len);

        bytes_copied += curr_block->used_len;
        curr_block = curr_block->next;
    }

    *out_size = bytes_copied;
    return BPLIB_SUCCESS;
}


BPLib_Status_t BPLib_MEM_CopyOutFromOffset(BPLib_Bundle_t* Bundle, uint64_t Offset,
                    uint64_t NumBytesToCopy, void* OutputBuffer, size_t OutputBufferSize)
{
    if (Bundle == NULL || OutputBuffer == NULL || Bundle->blob == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }
    if (NumBytesToCopy > OutputBufferSize)
    {
        return BPLIB_BUF_LEN_ERROR;
    }

    BPLib_MEM_Block_t *block = Bundle->blob;
    uint64_t skip = Offset;
    while (block && skip >= block->used_len)
    {
        skip -= block->used_len;
        block = block->next;
    }
    if (!block)
    {
        return skip == 0 && NumBytesToCopy == 0 ? BPLIB_SUCCESS : BPLIB_BUF_LEN_ERROR;
    }

    /* Validate the entire range before exposing any bytes to the caller.
     * Allocation capacity includes unused bytes and is not the blob length. */
    uint64_t remaining = NumBytesToCopy;
    uint64_t offset = skip;
    BPLib_MEM_Block_t *probe = block;
    while (probe && remaining)
    {
        uint64_t available = probe->used_len - offset;
        if (available > remaining) available = remaining;
        remaining -= available;
        probe = probe->next;
        offset = 0;
    }
    if (remaining) return BPLIB_MEM_CPY_FRM_OFFSET_NE_ERR;

    uint8_t *output = OutputBuffer;
    remaining = NumBytesToCopy;
    while (remaining)
    {
        size_t count = block->used_len - (size_t)skip;
        if (count > remaining) count = (size_t)remaining;
        memcpy(output, block->user_data.BigData + skip, count);
        output += count;
        remaining -= count;
        block = block->next;
        skip = 0;
    }
    return BPLIB_SUCCESS;
}

size_t BPLib_MEM_GetBytesInUse(BPLib_MEM_Pool_t *Pool)
{
    return BPLib_MEM_GetBytesInUseImpl(&Pool->impl);
}

size_t BPLib_MEM_GetBytesFree(BPLib_MEM_Pool_t *Pool)
{
    return BPLib_MEM_GetBytesFreeImpl(&Pool->impl);
}

size_t BPLib_MEM_GetHighwaterMark(BPLib_MEM_Pool_t *Pool)
{
    return Pool->HighWaterMark;
}

BPLib_MEM_Block_t *BPLib_MEM_GetBlockFromUserData(void *UserData)
{
    /* Return pointer to the top of the mem block if given a pointer to a block's user_data */
    return (BPLib_MEM_Block_t*)((uintptr_t)UserData - offsetof(BPLib_MEM_Block_t, user_data));
}

static BPLib_CanBlockHeader_t *BPLib_BlockHeaderFind(BPLib_Bundle_t *bundle, uint64_t num)
{
    if (!bundle) return NULL;
    if (bundle->blocks.PayloadHeader.BlockNum == num) return &bundle->blocks.PayloadHeader;
    for (size_t i = 0; i < BPLIB_MAX_NUM_EXTENSION_BLOCKS; ++i)
    {
        if (bundle->blocks.ExtBlocks[i].Header.BlockNum == num) return &bundle->blocks.ExtBlocks[i].Header;
    }
    return NULL;
}

BPLib_Status_t BPLib_BlockReaderOpen(BPLib_Bundle_t *bundle, uint64_t num, BPLib_BlockReader_t *reader)
{
    BPLib_CanBlockHeader_t *h = BPLib_BlockHeaderFind(bundle, num);
    if (!h || !reader || !bundle->blob || h->DataSize == 0) return BPLIB_NULL_PTR_ERROR;
    reader->bundle = bundle; reader->offset = h->DataOffsetStart; reader->remaining = h->DataSize;
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_BlockReaderRead(BPLib_BlockReader_t *reader, void *buf, size_t cap, size_t *got)
{
    if (!reader || !buf || !got) return BPLIB_NULL_PTR_ERROR;
    size_t n = (reader->remaining < cap) ? reader->remaining : cap;
    if (!n) { *got = 0; return BPLIB_SUCCESS; }
    BPLib_Status_t st = BPLib_MEM_CopyOutFromOffset(reader->bundle, reader->offset, n, buf, cap);
    if (st == BPLIB_SUCCESS) { reader->offset += n; reader->remaining -= n; *got = n; }
    else *got = 0;
    return st;
}

BPLib_Status_t BPLib_BlockWriterOpen(BPLib_Bundle_t *bundle, uint64_t num, size_t len, BPLib_BlockWriter_t *writer)
{
    BPLib_CanBlockHeader_t *h = BPLib_BlockHeaderFind(bundle, num);
    if (!h || !writer || !bundle->blob || len != h->DataSize) return BPLIB_BUF_LEN_ERROR;
    writer->staging = malloc(len);
    if (!writer->staging) return BPLIB_ERROR;
    writer->bundle = bundle; writer->offset = h->DataOffsetStart; writer->remaining = len; writer->total = len; writer->block_num = num; writer->written = 0;
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_BlockWriterWrite(BPLib_BlockWriter_t *writer, const void *buf, size_t len)
{
    if (!writer || !buf) return BPLIB_NULL_PTR_ERROR;
    if (len > writer->remaining) return BPLIB_BUF_LEN_ERROR;
    memcpy(writer->staging + writer->written, buf, len);
    writer->written += len; writer->remaining -= len;
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_BlockWriterClose(BPLib_BlockWriter_t *writer, bool success)
{
    if (!writer) return BPLIB_NULL_PTR_ERROR;
    if (success && writer->remaining != 0) { free(writer->staging); return BPLIB_BUF_LEN_ERROR; }
    if (success)
    {
        BPLib_CanBlockHeader_t *h = BPLib_BlockHeaderFind(writer->bundle, writer->block_num);
        if (h) h->RequiresEncode = true;
        BPLib_MEM_Block_t *blk = writer->bundle->blob;
        uint64_t skip = h ? h->DataOffsetStart : 0;
        while (blk && skip >= blk->used_len) { skip -= blk->used_len; blk = blk->next; }
        size_t left = writer->total; const uint8_t *src = writer->staging;
        while (blk && left) { size_t n = blk->used_len - (size_t)skip; if (n > left) n = left; memcpy(blk->user_data.BigData + skip, src, n); src += n; left -= n; blk = blk->next; skip = 0; }
    }
    free(writer->staging); writer->staging = NULL;
    return BPLIB_SUCCESS;
}
