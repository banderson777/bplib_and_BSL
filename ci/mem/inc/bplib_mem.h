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

#ifndef BPLIB_MEM_H
#define BPLIB_MEM_H

#include "bplib_mem_impl.h"
#include "bplib_bblocks.h"
#include "bplib_api_types.h"
#include "bplib_ct.h"
#include "bplib_arp.h"

#include <pthread.h>


/*
** Macro Definitions
*/

/**
 ** \brief Defines the size of a memory block's metadata
*/
#define BPLIB_MEM_META_DATA_SIZE (sizeof(size_t) + sizeof(struct BPLib_MEM_Block *))

/**
 ** \brief Defines the total size of a big memory block
*/
#define BPLIB_MEM_BIG_BLK_SIZE (2048u)

/**
 ** \brief Defines the size of a big memory block's available user data
*/
#define BPLIB_MEM_BIG_BLK_DATA_SIZE (BPLIB_MEM_BIG_BLK_SIZE - BPLIB_MEM_META_DATA_SIZE)

/**
 ** \brief Defines the total size of a small memory block
*/
#define BPLIB_MEM_SMALL_BLK_SIZE (128u)

/**
 ** \brief Defines the size of a small memory block's available user data
*/
#define BPLIB_MEM_SMALL_BLK_DATA_SIZE (BPLIB_MEM_SMALL_BLK_SIZE - BPLIB_MEM_META_DATA_SIZE)

/**
 ** \brief Defines the total number of possible memory block types
*/
#define BPLIB_MEM_TOTAL_NUM_BLOCKS      2


/*
** Type Definitions
*/

/**
 * @defgroup Memory Block User Data Types
 * @{
 */
typedef uint8_t BPLib_MEM_SmallData_t[BPLIB_MEM_SMALL_BLK_DATA_SIZE];
typedef uint8_t BPLib_MEM_BigData_t[BPLIB_MEM_BIG_BLK_DATA_SIZE];
/** @} */

/**
 ** \brief Union of known possible user data types
*/
typedef union
{
    /* Structs associated with the big memory block */
    BPLib_MEM_BigData_t     BigData;
    BPLib_Bundle_t          Bundle;
    BPLib_ARP_AdminRecord_t AdminRecord;

    /* Structs associated with the small memory block */
    BPLib_MEM_SmallData_t SmallData;
    BPLib_CT_DbEntry_t    DbEntry;
} BPLib_MEM_UserData_t;

/**
 * @struct BPLib_MEM_Block_t
 * @brief Represents a block of memory in the pool.
 * 
 * This structure holds a chunk of usable memory (`user_data`). 
 * It also has a pointer (`next`) to link to other blocks in a linked list.
 */
struct BPLib_MEM_Block
{
    size_t used_len; /**< Byte-length of user data currently within the chunk. This is initialized to 0. */
    struct BPLib_MEM_Block* next; /**< Pointer to the next block in the list */
    BPLib_MEM_UserData_t user_data; /**< User data stored in the block */
};

/**
 * @struct BPLib_MEM_Pool_t
 * @brief Represents a memory pool that manages memory blocks.
 * 
 * This structure holds the implementation of the pool (`impl`), and a mutex lock (`lock`)
 * for thread safety when accessing the memory pool. There are three possible memory pool
 * implementations: 
 *    - STD, which just uses the standard library malloc/free functions and ignores the 
 *      memory buffer passed in. This is the default memory pool for bpcat.
 *    - BEN, which implements a custom fixed-size memory pool based on Ben Kenwright's
 *      paper. This one is not currently used by any default implementations and can 
 *      only handle blocks of one size, but it is kept for posterity and for future
 *      extensions
 *    - CFE, which is just a wrapper to the cFE Memory Pool functions. This is the default
 *      memory pool for the cFS BPNode app and it supports all defined memory blocks types.
 */
typedef struct BPLib_MEM_Pool
{
    BPLib_MEM_PoolImpl_t impl; /**< The pool implementation (details hidden) */
    pthread_mutex_t lock; /**< Mutex for synchronizing access to the pool */
    size_t         HighWaterMark;   /** \brief Highwater memory mark */
} BPLib_MEM_Pool_t;

/** Generic sequential access to a canonical block's BTSD. */
typedef struct
{
    BPLib_Bundle_t *bundle;
    uint64_t offset;
    size_t remaining;
} BPLib_BlockReader_t;

typedef struct
{
    BPLib_Bundle_t *bundle;
    uint64_t offset;
    size_t remaining;
    size_t total;
    uint64_t block_num;
    uint8_t *staging;
    size_t written;
} BPLib_BlockWriter_t;

/** Borrow a decoded bundle and open sequential access to canonical block BTSD.
 * Keep bundle/blob/offset metadata alive and unchanged throughout access.
 * Missing block, empty BTSD, or invalid pointers return NULL_PTR_ERROR.
 * Reader allocates nothing and needs no close; caller synchronizes mutations.
 */
BPLib_Status_t BPLib_BlockReaderOpen(BPLib_Bundle_t *bundle, uint64_t block_num,
                                     BPLib_BlockReader_t *reader);
/** Read min(buf_size, remaining) bytes from an opened reader. EOF or zero
 * capacity succeeds with *bytes_read == 0. Copy failures set *bytes_read to zero
 * and leave position unchanged. Invalid pointers return NULL_PTR_ERROR.
 */
BPLib_Status_t BPLib_BlockReaderRead(BPLib_BlockReader_t *reader, void *buf,
                                     size_t buf_size, size_t *bytes_read);

/** Allocate staging for exactly the decoded BTSD size; resizing is unsupported.
 * Borrow a valid decoded bundle until Close; do not concurrently mutate it.
 * Size/block/blob/output errors return BUF_LEN_ERROR, allocation failure ERROR.
 * On success call Close exactly once; writes do not modify the blob until then.
 */
BPLib_Status_t BPLib_BlockWriterOpen(BPLib_Bundle_t *bundle, uint64_t block_num,
                                     size_t btsd_size, BPLib_BlockWriter_t *writer);
/** Append borrowed bytes to staging; excess size returns BUF_LEN_ERROR.
 * Requires a successfully opened writer; invalid pointers return NULL_PTR_ERROR.
 */
BPLib_Status_t BPLib_BlockWriterWrite(BPLib_BlockWriter_t *writer, const void *buf,
                                      size_t size);
/** Free staging exactly once. success=false discards it; success=true requires
 * all bytes, copies into the existing blob, and marks the block for re-encoding.
 * Incomplete commit frees staging and returns BUF_LEN_ERROR; do not close again
 * even on that error. Offsets/blob must still match the bundle used at Open.
 */
BPLib_Status_t BPLib_BlockWriterClose(BPLib_BlockWriter_t *writer, bool success);


/*
** Function Definitions
*/

/**
 * @brief Initializes a memory pool.
 * 
 * This function initializes a memory pool with the provided initial memory and size.
 * 
 * @param[out] pool Pointer to the memory pool to initialize.
 * @param[in] init_mem Pointer to the initial memory for the pool.
 * @param[in] init_size Size of the initial memory.
 * 
 * @return Status of the operation.
 */
BPLib_Status_t BPLib_MEM_PoolInit(BPLib_MEM_Pool_t* pool, void* init_mem, size_t init_size);

/**
 * @brief Destroys a memory pool.
 * 
 * This function destroys the memory pool and frees any allocated resources.
 * 
 * @param[in] pool Pointer to the memory pool to destroy.
 */
void BPLib_MEM_PoolDestroy(BPLib_MEM_Pool_t* pool);

/**
 * @brief Allocates a new memory block from the pool.
 * 
 * This function allocates a single memory block from the memory pool.
 * 
 * @param[in] pool Pointer to the memory pool from which to allocate a block.
 * 
 * @return Pointer to the allocated memory block.
 */
BPLib_MEM_Block_t* BPLib_MEM_BlockAlloc(BPLib_MEM_Pool_t* pool, size_t Size);

/**
 * @brief Frees a memory block back to the pool.
 * 
 * This function frees a memory block that was previously allocated from the pool.
 * 
 * @param[in] pool Pointer to the memory pool to which to return the block.
 * @param[in] block Pointer to the memory block to free.
 */
void BPLib_MEM_BlockFree(BPLib_MEM_Pool_t* pool, BPLib_MEM_Block_t* block);

/**
 * @brief Allocates a list of memory blocks from the pool.
 * 
 * This function allocates a list of memory blocks that together have the requested byte length.
 * 
 * @param[in] pool Pointer to the memory pool from which to allocate blocks.
 * @param[in] byte_len The total byte length of the blocks to allocate.
 * @param[in] BlockSize Size of blocks to allocate. Note that blocklists containing multiple
 *                      block types are currently not supported.
 * 
 * @return Pointer to the head of the allocated block list.
 */
BPLib_MEM_Block_t* BPLib_MEM_BlockListAlloc(BPLib_MEM_Pool_t* pool, size_t byte_len, size_t BlockSize);

/**
 * @brief Frees a list of memory blocks back to the pool.
 * 
 * This function frees a list of memory blocks that were previously allocated from the pool.
 * 
 * @param[in] pool Pointer to the memory pool to which to return the blocks.
 * @param[in] head Pointer to the head of the block list to free.
 */
void BPLib_MEM_BlockListFree(BPLib_MEM_Pool_t* pool, BPLib_MEM_Block_t* head);

/**
 * @brief Allocates a bundle from the memory pool.
 * 
 * This function allocates a new `BPLib_Bundle_t` from the memory pool, 
 * including a blob of data specified by the `blob_data` and `data_len` parameters.
 * 
 * @param[in] pool Pointer to the memory pool from which to allocate the bundle.
 * @param[in] blob_data A pointer to the data to store in the bundle.
 * @param[in] data_len The length of the data to store in the bundle.
 * 
 * @return A pointer to the allocated bundle.
 */
BPLib_Bundle_t* BPLib_MEM_BundleAlloc(BPLib_MEM_Pool_t* pool, const void* blob_data, size_t data_len);

/**
 * @brief Frees a bundle from the memory pool.
 * 
 * This function frees the memory associated with a `BPLib_Bundle_t`.
 * The bundle pointer provided must have been allocated using BPLib_MEM_BundleAlloc.
 * 
 * @param[in] pool Pointer to the memory pool to which the bundle will be freed.
 * @param[in] bundle Pointer to the bundle to free.
 */
void BPLib_MEM_BundleFree(BPLib_MEM_Pool_t* pool, BPLib_Bundle_t* bundle);

/**
 * @brief Copies the blob data out of a bundle.
 * 
 * This function copies the blob data from the specified bundle into the provided buffer.
 * If the bundle has more blob data than max_len, this function returns BPLIB_BUF_LEN_ERROR
 * 
 * @param[in] bundle Pointer to the bundle from which to copy the data.
 * @param[out] out_buffer A buffer to store the copied data.
 * @param[in] max_len The maximum number of bytes to copy.
 * @param[out] out_size The actual number of bytes copied.
 * 
 * @return Status of the operation.
 */
BPLib_Status_t BPLib_MEM_BlobCopyOut(BPLib_Bundle_t* bundle, void* out_buffer, size_t max_len, size_t* out_size);

/**
 * @brief Copies part of the blob data out of a bundle.
 * 
 * This function copies populated blob bytes, starting at the supplied offset.
 * The complete range is checked against block used lengths before any copy;
 * an invalid range leaves OutputBuffer unchanged. A zero-byte read at the
 * exact end of the blob succeeds. NumBytesToCopy must fit OutputBufferSize.
 * 
 * @param[in] Bundle (BPLib_Bundle_t*) Pointer to the bundle from which to copy the data.
 * @param[in] Offset (uint64_t) Offset into the user data
 * @param[in] NumBytesToCopy (uint64_t) Number of bytes to copy
 * @param[out] OutputBuffer (void*) A buffer to store the copied data.
 * @param[in] OutputBufferSize (size_t) The size of the output buffer (also, maximum number of bytes to copy).
 * 
 * @return Status of the operation.
 */
BPLib_Status_t BPLib_MEM_CopyOutFromOffset(BPLib_Bundle_t* Bundle, uint64_t Offset,
                    uint64_t NumBytesToCopy, void* OutputBuffer, size_t OutputBufferSize);

/**
 * @brief Get bytes in use
 * 
 * This function gets the current number of bytes in use by the memory pool.
 * 
 * @param[in] Pool Pointer to the memory pool
 * 
 * @return Number of bytes in use
 */
size_t BPLib_MEM_GetBytesInUse(BPLib_MEM_Pool_t *Pool);

/**
 * @brief Get bytes free
 * 
 * This function gets the current number of bytes not in use by the memory pool.
 * 
 * @param[in] Pool Pointer to the memory pool
 * 
 * @return Number of bytes not in use
 */
size_t BPLib_MEM_GetBytesFree(BPLib_MEM_Pool_t *Pool);

/**
 * @brief Get memory block from user data
 * 
 * This function takes a pointer to a user_data struct, finds the memory block containing
 * this struct, and returns a pointer to the top of the memory block. Note that this
 * function assumes that the pointer passed in is a valid user_data pointer, any other
 * parameters could result in undefined behavior.
 * 
 * @param[in] UserData Pointer to a user data structure
 * 
 * @return Pointer to a memory block
 */
BPLib_MEM_Block_t *BPLib_MEM_GetBlockFromUserData(void *UserData);

/**
 * @brief Get memory highwater mark
 * 
 * This function returns the highest point of memory usage so far. It is only reset
 * on a node restart
 * 
 * @param[in] Pool Pointer to memory pool
 * 
 * @return Highwater mark size in bytes
 */
size_t BPLib_MEM_GetHighwaterMark(BPLib_MEM_Pool_t *Pool);

#endif /* BPLIB_MEM_H */
