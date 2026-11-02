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

#ifndef BPLIB_STOR_H
#define BPLIB_STOR_H

/* ======== */
/* Includes */
/* ======== */

#include "bplib_api_types.h"
#include "bplib_cfg.h"
#include "bplib_eid.h"
#include "bplib_mem.h"
#include "bplib_stor_loadbatch.h"
#include "bplib_stor_sql.h"
#include "bplib_ct.h"

/* ====== */
/* Macros */
/* ====== */

#ifndef BPLIB_STOR_INSERTBATCHSIZE
#define BPLIB_STOR_INSERTBATCHSIZE 100
#endif

#ifndef BPLIB_STOR_DISCARDBATCHSIZE
#define BPLIB_STOR_DISCARDBATCHSIZE 25000
#endif

/* We conditionally allow this to be defined by a compile time variable
** so that the unit tests can pass in :memory: here and avoid using the disk
*/
#ifndef BPLIB_STOR_DBNAME
#define BPLIB_STOR_DBNAME       "bplib-storage.db"
#endif

#define BPLIB_STOR_MAX_IDLE_TIME        (4000u)

/**
 * \brief Size of a full custodial batch operation
 */
#define BPLIB_STOR_CT_BATCH_SIZE        (100u)

/* ======== */
/* Typedefs */
/* ======== */

/**
 * \brief A batch of storage operations associated with a set of bundle IDs generated
 *        after receiving and processing a CCS
 */
typedef struct 
{
    uint32_t BundleIDs[BPLIB_STOR_CT_BATCH_SIZE];
    BPLib_CT_StorOp_t Ops[BPLIB_STOR_CT_BATCH_SIZE];
    size_t   Size;
} BPLib_STOR_CtUpdateBatch_t;

struct BPLib_BundleCache
{
    pthread_mutex_t        lock;
    sqlite3*               db;
    int64_t                BundleRowsToDelete[BPLIB_STOR_DISCARDBATCHSIZE];
    BPLib_Bundle_t*        InsertBatch[BPLIB_STOR_INSERTBATCHSIZE];
    size_t                 InsertBatchSize;
    BPLib_STOR_LoadBatch_t ChannelLoadBatches[BPLIB_MAX_NUM_CHANNELS];
    BPLib_STOR_LoadBatch_t ContactLoadBatches[BPLIB_MAX_NUM_CONTACTS];
    BPLib_STOR_CtUpdateBatch_t CustodyUpdateBatch;
    int64_t                LastActiveTime;
    size_t                 BundleCountNotEgressed;  /* Number of bundles in storage that have not been marked as egressed */

    /* Storage-related MIB reports */
    uint32_t BundleCountStored;
    size_t   BytesStorageInUse;
    size_t   StorageSize;
};

/**
 * \brief Storage housekeeping payload
 */

typedef struct BPLib_StorageHkTlm_Payload BPLib_StorageHkTlm_Payload_t;

struct BPLib_StorageHkTlm_Payload
{
    size_t  BytesMemInUse;     /** \brief Bytes in memory that are in use */
    size_t  BytesMemFree;      /** \brief Number of bytes free */
    size_t  BytesMemHighWater; /** \brief Memory high water mark in bytes */
    size_t  KbStorageInUse;    /** \brief Kilobytes of storage currently in use */
    size_t  KbBundlesInStor;   /** \brief Kilobytes of storage currently occupied by bundles */
    int64_t MonotonicTime;     /** \brief Monotonic Time Counter */
    int64_t CorrelationFactor; /** \brief Time Correlation Factor */
};

/*
** Storage Policy Table
*/
typedef struct
{
    BPLib_EID_Pattern_t SrcEIDs[BPLIB_MAX_NUM_STORE_EIDS];
    uint32_t            StorageSize;
} BPLib_STOR_StorageSet_t;

typedef struct
{
    BPLib_STOR_StorageSet_t StorageSet[BPLIB_MAX_NUM_STORE_SET];
} BPLib_STOR_StorageTable_t;

/*
** Externs
*/

extern BPLib_StorageHkTlm_Payload_t BPLib_STOR_StoragePayload;

/*
** Exported Functions
*/

/**
 * \brief Storage initialization
 *
 *  \par Description
 *       STOR initialization function
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance, which contains cache instance within
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_STOR_Init(BPLib_Instance_t* Inst);

/**
 * \brief Storage teardown
 *
 *  \par Description
 *       Tear down storage by destroying its mutex lock
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance, which contains cache instance within
 *
 *  \return void
 */
void BPLib_STOR_Destroy(BPLib_Instance_t* Inst);

/**
 * \brief Validate Storage Table configurations
 *
 *  \par Description
 *       Validate configuration table parameters
 *
 *  \par Assumptions, External Events, and Notes:
 *       - This function is called by whatever external task handles table management.
 *         Every time a new Storage table is loaded, this function should be called to
 *         validate its parameters.
 *
 *  \param[in] TblData Pointer to the config table
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Validation was successful
 */
BPLib_Status_t BPLib_STOR_StorageTblValidateFunc(void *TblData);

/**
 * \brief Store a bundle
 *
 *  \par Description
 *       If there is room in storage to accept this bundle, add it to the insertion batch.
 *       If the insertion batch reaches its size limit, trigger a batch bundle storage 
 *       operation with \ref BPLib_STOR_FlushPendingUnlocked. If a storage operation
 *       was done, finalize the custodial transfer by signaling custody for the stored
 *       bundles.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *  \param[in] Bundle Pointer to a bundle to store
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 *  \retval BPLIB_NO_STOR_ERR No room in storage remaining to store this bundle
 */
BPLib_Status_t BPLib_STOR_StoreBundle(BPLib_Instance_t* Inst, BPLib_Bundle_t* Bundle);

/**
 * \brief Flush pending bundles from memory into storage
 *
 *  \par Description
 *       Flush the insertion batch by moving whatever bundles are in the batch from 
 *       memory into storage. If any custodial bundles were stored, finalize the custodial
 *       transfer by signaling custody.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_STOR_FlushPending(BPLib_Instance_t* Inst);

/**
 * \brief Egress stored bundles corresponding to provided channel/contact
 *
 *  \par Description
 *       Given the provided egress ID and whether it corresponds to a channel or contact,
 *       perform one of the three egress operation steps:
 *       Step 1: If the given load batch is empty, search storage for bundle row IDs
 *               for bundles that can be egressed on this contact/channel and add them
 *               to the load batch.
 *       Step 2: If there are unconsumed bundle row IDs in the given load batch, load all
 *               the bundles in that batch into memory and put them on the relevant egress
 *               queue. End this operation if node memory runs out.
 *       Step 3: If the load batch is marked as consumed, mark all bundles in the batch
 *               as egressed in storage to mark them as deletable and reset the load batch.
 *
 *  \par Assumptions, External Events, and Notes:
 *       This operation is skipped if an active ingress operation is detected or if no 
 *       non-egressed bundles remain in storage.
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *  \param[in] EgressID The ID of the channel of contact requesting bundles
 *  \param[in] LocalDelivery Whether the egress ID corresponds to a channel (true) or contact (false)
 *  \param[in] NumEgressed The number of bundles loaded into memory by this operation
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 *  \retval BPLIB_STOR_NO_BUNDLE_FOUND_ERR A bundle disappeared between operations
 *  \retval BPLIB_STOR_NO_MEM_ERR Ran out of node memory when trying to load a bundle
 */
BPLib_Status_t BPLib_STOR_EgressForID(BPLib_Instance_t* Inst, uint32_t EgressID,
                                        bool LocalDelivery, size_t* NumEgressed);

/**
 * \brief Perform storage garbage collection operations
 *
 *  \par Description
 *       If no active ingress or egress operation is detected, search storage for bundles 
 *       to discard based on either expiration (bundles have exceeded their lifetime) or 
 *       based on egress (bundles have been successfully egressed from this node). If 
 *       the stored bundle count drops to 0 as a result of these operations, a storage
 *       cleanup operation is automatically triggered. The latest database size is also
 *       queried by this operation.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_STOR_GarbageCollect(BPLib_Instance_t* Inst);

/**
 * \brief Update values in the STOR housekeeping packet with values of the
 *        BPLib_Instance_t representing the current iteration of FSW
 * \param[in] Inst Instance variable that represents the current system
 * \return void
 */
void BPLib_STOR_UpdateHkPkt(BPLib_Instance_t* Inst);

/**
 * \brief Perform storage cleanup operations
 *
 *  \par Description
 *       Runs the sqlite "VACUUM" query to defragment storage.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_STOR_Cleanup(BPLib_Instance_t* Inst);

/**
 * \brief Update custodial bundles in storage
 *
 *  \par Description
 *       Use the custody update batch to update the state of a batch of custodial bundles.
 *       Possible operations include:
 *          - Mark bundle as egressed (for successful custody transfers)
 *          - Stop bundle retransmission (for rejected custody transfers)
 *          - Trigger bundle retransmission (for dropped bundles)
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *
 *  \return void
 */
void BPLib_STOR_UpdateCustodialBundles(BPLib_Instance_t* Inst);

/**
 * \brief Add a bundle operation to the custodial update batch
 *
 *  \par Description
 *       Add the provided bundle ID and storage operation to the custodial bundle update
 *       update path. This compatibility entry point now commits each operation
 *       synchronously under the storage lock. Success means durable commit;
 *       failure means the caller must retain/retry the operation. No accepted
 *       operation is left only in the volatile batch.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *  \param[in] BundleID Unique bundle identifier
 *  \param[in] Op Enumeration of storage operation to run
 *
 *  \return BPLIB_SUCCESS after commit, otherwise a validation or SQL error
 */
BPLib_Status_t BPLib_STOR_AddToCustodialUpdateBatch(BPLib_Instance_t *Inst, uint32_t BundleId, 
                                                                    BPLib_CT_StorOp_t Op);

/**
 * \brief Set new retransmission trigger
 *
 *  \par Description
 *       For all bundles corresponding to the provided contact ID, update their
 *       retransmission triggers to this contact's retranmit timeout value.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *  \param[in] ContactId Contact identifier
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_STOR_SetNewRetransmitTrigger(BPLib_Instance_t *Inst, uint32_t ContactId);

/**
 * \brief Set last storage active time
 *
 *  \par Description
 *       Set the last active time to the current monotonic time
 *
 *  \par Assumptions, External Events, and Notes:
 *       This function is called whenever bundles are loaded into storage or loaded out
 *       of storage.
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *
 *  \return void
 */
void BPLib_STOR_SetLastActiveTime(BPLib_Instance_t* Inst);

/**
 * \brief Check if storage ingress or egress is active
 *
 *  \par Description
 *       If the last time storage was active has exceeded the maximum idle time allowed,
 *       return false. Otherwise return true.
 *
 *  \par Assumptions, External Events, and Notes:
 *
 *  \param[in] Inst Pointer to BPLib Instance
 *
 *  \return Whether ingress/egress is currently active
 *  \retval true:  Ingress/egress is active
 *  \retval false: Ingress/egress is not active
 */
bool BPLib_STOR_IsIngressEgressActive(BPLib_Instance_t* Inst);

/**
 * \brief Egress bundles from storage and into the relevant egress queue in node 
 *        memory. This is a multi-step operation that expects this function to be 
 *        called at a regular wakeup cadence, and different operations are performed
 *        each time. The operations are:
 *            - Load a batch of bundle IDs that match a viable contact/channel into memory
 *            - Load the bundle ID's full bundle into memory and place it on the relevant
 *              queue. 
 *            - Once a load batch has been fully loaded into memory, mark the bundles
 *              in storage as egressed for future deletion
 * \param[in] Inst Instance variable that represents the current system
 * \param[in] MaxBundles Maximum number of bundles that can be egressed per call
 *
 * \return Execution status
 * \retval BPLIB_SUCCESS Validation was successful
 */
BPLib_Status_t BPLib_STOR_Egress(BPLib_Instance_t *Instance, size_t MaxBundles);

#endif /* BPLIB_STOR_H */
