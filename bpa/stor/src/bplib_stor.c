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

#include "bplib_stor.h"
#include "bplib_qm.h"
#include "bplib_em.h"
#include "bplib_eventids.h"
#include "bplib_fwp.h"
#include "bplib_nc.h"
#include "bplib_eid.h"
#include "bplib_as.h"
#include "bplib_stor_sql.h"
#include "bplib_stor_sql_store.h"
#include "bplib_stor_sql_load.h"
#include "bplib_stor_sql_cust.h"
#include "bplib_inst.h"

#include <stdio.h>

/* ======= */
/* Globals */
/* ======= */

BPLib_StorageHkTlm_Payload_t BPLib_STOR_StoragePayload;

/*
** Internal Functions
*/

void BPLib_STOR_FinalizeCustody(BPLib_Instance_t *Inst, BPLib_Bundle_t *CustodialBundles[], 
                        size_t NumCustodialBundles, BPLib_CT_DispositionCode_t DispCode)
{
    BPLib_CLA_ContactRunState_t ConState;
    size_t i;
    bool PushedBundle = true;
    bool BundleStored = true;

    if (DispCode == BPLib_CT_CustodyRefused)
    {
        BundleStored = false;
    }

    for (i = 0; i < NumCustodialBundles; i++)
    {
        /* Finalize custodial transfer for custodial bundles */
        (void) BPLib_CT_SignalCustody(Inst, CustodialBundles[i], DispCode, BundleStored);

        /* Custodial bundles with an egress path should get sent out instead of freed */
        (void) BPLib_CLA_GetContactRunState(CustodialBundles[i]->Meta.EgressID, &ConState);
        if (CustodialBundles[i]->Meta.EgressID < BPLIB_MAX_NUM_CONTACTS &&
            ConState == BPLIB_CLA_STARTED && DispCode == BPLib_CT_CustodyAccepted)
        {
            PushedBundle = BPLib_QM_WaitQueueTryPush(&(Inst->ContactEgressJobs[CustodialBundles[i]->Meta.EgressID]), 
                                        &CustodialBundles[i], QM_WAIT_FOREVER);
        }
        /* There's no egress path, free memory */
        else
        {
            PushedBundle = false;
        }

        if (PushedBundle == false)
        {
            BPLib_MEM_BundleFree(&Inst->pool, CustodialBundles[i]);
        }
    }

    return;
}


BPLib_Status_t BPLib_STOR_FlushPendingUnlocked(BPLib_Instance_t* Inst, 
                        BPLib_Bundle_t *CustodialBundles[], size_t *CustodialBundlesStored)
{
    BPLib_Status_t       Status;
    BPLib_BundleCache_t* CacheInst;
    size_t               i;
    size_t               TotalBytesStored;
    size_t               DuplicateBundlesIgnored;
    size_t               CustodialIdx;
    size_t               CustodialBundleCount;

    CacheInst               = &Inst->BundleStorage;
    TotalBytesStored        = 0;
    DuplicateBundlesIgnored = 0;
    *CustodialBundlesStored = 0;
    CustodialIdx            = 0;
    CustodialBundleCount    = 0;

    Status = BPLib_SQL_Store(Inst, &TotalBytesStored, &DuplicateBundlesIgnored, CustodialBundlesStored);

    if (Status == BPLIB_SUCCESS)
    {
        CacheInst->BytesStorageInUse      += TotalBytesStored;
        CacheInst->BundleCountStored      += CacheInst->InsertBatchSize - DuplicateBundlesIgnored;
        CacheInst->BundleCountNotEgressed += CacheInst->InsertBatchSize - DuplicateBundlesIgnored;

        if (DuplicateBundlesIgnored > 0)
        {
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED, DuplicateBundlesIgnored);
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DISCARDED, DuplicateBundlesIgnored);
            BPLib_EM_SendEvent(BPLIB_STOR_DUPL_DBG_EID,
                                BPLib_EM_EventType_DEBUG,
                                "Ignored %ld duplicate bundles in store batch.",
                                DuplicateBundlesIgnored);
        }
    }
    else if (Status == BPLIB_STOR_DB_FULL_ERR)
    {
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED, CacheInst->InsertBatchSize);
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DISCARDED, CacheInst->InsertBatchSize);
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED_NO_STORAGE, CacheInst->InsertBatchSize);

        BPLib_EM_SendEvent(BPLIB_STOR_DB_FULL_INF_EID,
                            BPLib_EM_EventType_ERROR,
                            "SQLite database is full, dropping %d bundles",
                            CacheInst->InsertBatchSize);
    }
    else
    {
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED, CacheInst->InsertBatchSize);
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DISCARDED, CacheInst->InsertBatchSize);

        BPLib_EM_SendEvent(BPLIB_STOR_SQL_STORE_ERR_EID,
                            BPLib_EM_EventType_ERROR,
                            "BPLib_SQL_Store failed to store bundle. RC=%d",
                            Status);
    }

    /* 
    ** Free the bundles, as they're now persistent
    ** Note: even if the storage fails, we free everything to avoid a leak.
    */
    for (i = 0; i < CacheInst->InsertBatchSize; i++)
    {        
        if (CacheInst->InsertBatch[i]->Meta.IsCustodial)
        {
            CustodialBundles[CustodialIdx++] = CacheInst->InsertBatch[i];
            CustodialBundleCount++;
        }
        /* Noncustodial bundles are all freed */
        else
        {
            BPLib_MEM_BundleFree(&Inst->pool, CacheInst->InsertBatch[i]);
        }
    }

    /* Unlikely error, but report it just in case */
    if (CustodialBundleCount != *CustodialBundlesStored && Status == BPLIB_SUCCESS)
    {
        BPLib_EM_SendEvent(BPLIB_STOR_UNKNOWN_CUST_ERR_EID, BPLib_EM_EventType_ERROR,
                "Storage reported successfully storing %ld custodial bundles but %ld were in the batch.",
                *CustodialBundlesStored, CustodialBundleCount);
    }

    *CustodialBundlesStored = CustodialBundleCount;
    
    CacheInst->InsertBatchSize = 0;

    return Status;
}


BPLib_Status_t BPLib_STOR_UpdateCustodialBundlesUnlocked(BPLib_Instance_t* Inst, BPLib_STOR_CtUpdateBatch_t *CustodyBatch)
{
    BPLib_Status_t Status;

    Status = BPLib_SQL_UpdateCustodialBundles(Inst, CustodyBatch);

    if (Status != BPLIB_SUCCESS)
    {
        BPLib_EM_SendEvent(BPLIB_STOR_CCS_ERR_EID, BPLib_EM_EventType_ERROR,
                "Error performing CCS storage operations, Status = %d.", Status);
    }

    if (Status == BPLIB_SUCCESS) CustodyBatch->Size = 0;
    return Status;
}

/*
** External Functions
*/

BPLib_Status_t BPLib_STOR_Init(BPLib_Instance_t* Inst)
{
    BPLib_Status_t Status;
    size_t         i;

    if (Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Zero-out the storage housekeeping payload */
    memset((void*) &BPLib_STOR_StoragePayload, 0, sizeof(BPLib_StorageHkTlm_Payload_t));

    /* Zero-out the bundle storage */
    memset(&Inst->BundleStorage, 0, sizeof(BPLib_BundleCache_t));

    pthread_mutex_init(&Inst->BundleStorage.lock, NULL);

    for (i = 0; i < BPLIB_MAX_NUM_CHANNELS; i++)
    {
        Status = BPLib_STOR_LoadBatch_Init(&Inst->BundleStorage.ChannelLoadBatches[i]);
        if (Status != BPLIB_SUCCESS)
        {
            return Status;
        }
    }

    for (i = 0; i < BPLIB_MAX_NUM_CONTACTS; i++)
    {
        Status = BPLib_STOR_LoadBatch_Init(&Inst->BundleStorage.ContactLoadBatches[i]);
        if (Status != BPLIB_SUCCESS)
        {
            return Status;
        }
    }

    Status = BPLib_SQL_Init(Inst, (const char*) BPLIB_STOR_DBNAME);

    return Status;
}

void BPLib_STOR_Destroy(BPLib_Instance_t* Inst)
{
    if (Inst == NULL)
    {
        return;
    }

    pthread_mutex_destroy(&Inst->BundleStorage.lock);
}

/* Validate Storage table data */
BPLib_Status_t BPLib_STOR_StorageTblValidateFunc(void *TblData)
{
    BPLib_Status_t ReturnCode = BPLIB_SUCCESS;

    return ReturnCode;
}

BPLib_Status_t BPLib_STOR_StoreBundle(BPLib_Instance_t* Inst, BPLib_Bundle_t* Bundle)
{
    BPLib_Status_t       Status;
    BPLib_BundleCache_t* CacheInst;
    BPLib_Bundle_t *CustodialBundles[BPLIB_STOR_INSERTBATCHSIZE];
    size_t          CustodialBundlesStored;
    bool            RejectedCapacity = false;

    Status = BPLIB_SUCCESS;
    CustodialBundlesStored = 0;

    if ((Inst == NULL) || (Bundle == NULL) || (Bundle->blob == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    CacheInst = &Inst->BundleStorage;

    pthread_mutex_lock(&CacheInst->lock);

    size_t Remaining = CacheInst->BytesStorageInUse < BPLIB_MAX_STORED_BUNDLE_BYTES
        ? BPLIB_MAX_STORED_BUNDLE_BYTES - CacheInst->BytesStorageInUse : 0;
    /* Queued bundles have not yet contributed to committed usage. Reserve their
     * declared sizes before admitting another member of this batch. */
    for (size_t Index = 0; Index < CacheInst->InsertBatchSize; ++Index)
    {
        size_t Pending = CacheInst->InsertBatch[Index]->Meta.TotalBytes;
        if (Pending >= Remaining) { Remaining = 0; break; }
        Remaining -= Pending;
    }
    if (Bundle->Meta.TotalBytes >= Remaining)
    {
        BPLib_EM_SendEvent(BPLIB_CT_NO_STOR_ERR_EID, BPLib_EM_EventType_ERROR,
                            "Cannot store and accept bundle, not enough storage (%zu bytes remaining).", Remaining);
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED_NO_STORAGE, 1);
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED, 1);
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DISCARDED, 1);

        RejectedCapacity = true;
        Status = BPLIB_NO_STOR_ERR;
    }
    else
    {
        BPLib_STOR_SetLastActiveTime(Inst);

        /* Add to the next batch */
        CacheInst->InsertBatch[CacheInst->InsertBatchSize++] = Bundle;
        if (CacheInst->InsertBatchSize == BPLIB_STOR_INSERTBATCHSIZE)
        {
            Status = BPLib_STOR_FlushPendingUnlocked(Inst, CustodialBundles, &CustodialBundlesStored);
        }
    }

    pthread_mutex_unlock(&CacheInst->lock);

    if (RejectedCapacity)
    {
        /* Admission may already have created an uncommitted CTDB entry. Release
         * it so retransmission can retry, without nesting CT under the SQL lock. */
        if (Bundle->Meta.IsCustodial)
            (void) BPLib_CT_SignalCustody(Inst, Bundle, BPLib_CT_CustodyRefused, false);
        BPLib_MEM_BundleFree(&Inst->pool, Bundle);
    }

    /* Finalize custodial signaling for stored bundles */
    if (CustodialBundlesStored > 0)
    {
        if (Status == BPLIB_SUCCESS)
        {
            BPLib_STOR_FinalizeCustody(Inst, CustodialBundles, 
                                    CustodialBundlesStored, BPLib_CT_CustodyAccepted);
        }
        else
        {
            BPLib_STOR_FinalizeCustody(Inst, CustodialBundles, 
                                    CustodialBundlesStored, BPLib_CT_CustodyRefused);
        }
    }

    return Status;
}

BPLib_Status_t BPLib_STOR_FlushPending(BPLib_Instance_t* Inst)
{
    BPLib_Status_t  Status = BPLIB_SUCCESS;
    BPLib_Bundle_t *CustodialBundles[BPLIB_STOR_INSERTBATCHSIZE];
    size_t          CustodialBundlesStored = 0;

    if (Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    pthread_mutex_lock(&Inst->BundleStorage.lock);

    if (Inst->BundleStorage.InsertBatchSize > 0)
    {
        Status = BPLib_STOR_FlushPendingUnlocked(Inst, CustodialBundles, &CustodialBundlesStored);
    }

    pthread_mutex_unlock(&Inst->BundleStorage.lock);

    if (CustodialBundlesStored > 0)
    {
        /* Finalize custodial signaling for stored bundles */
        if (Status == BPLIB_SUCCESS)
        {
            BPLib_STOR_FinalizeCustody(Inst, CustodialBundles, 
                                    CustodialBundlesStored, BPLib_CT_CustodyAccepted);
        }
        else
        {
            BPLib_STOR_FinalizeCustody(Inst, CustodialBundles, 
                                    CustodialBundlesStored, BPLib_CT_CustodyRefused);
        }
    }

    return Status;
}

BPLib_Status_t BPLib_STOR_EgressForID(BPLib_Instance_t* Inst, uint32_t EgressID,
                                        bool LocalDelivery, size_t* NumEgressed)
{
    BPLib_Status_t          Status;
    BPLib_BundleCache_t*    CacheInst;
    BPLib_STOR_LoadBatch_t* LoadBatch;
    BPLib_Bundle_t*         CurrBundle;
    BPLib_EID_Pattern_t     LocalEID;
    BPLib_EID_Pattern_t*    DestEIDs;
    BPLib_QM_WaitQueue_t*   EgressQueue;
    size_t                  EgressCnt;
    int64_t                 CurrBundleID;
    size_t                  NumEIDs;

    Status     = BPLIB_SUCCESS;
    CurrBundle = NULL;
    EgressCnt  = 0;

    if ((Inst == NULL) || (NumEgressed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    if (LocalDelivery && EgressID >= BPLIB_MAX_NUM_CHANNELS)
    {
        return BPLIB_STOR_PARAM_ERR;
    }

    if (!LocalDelivery && EgressID >= BPLIB_MAX_NUM_CONTACTS)
    {
        return BPLIB_STOR_PARAM_ERR;
    }

    if (BPLib_QM_IsIngressIdle(Inst) == false || Inst->BundleStorage.BundleCountNotEgressed == 0)
    {
        /* 
        ** Avoid searching the DB if the unsorted jobs queue (which is the ingress queue) isn't empty.
        ** Note: this is a pretty critical performance optimization that allows bplib
        ** to use all of its CPU resources for ingress.
        ** We also skip searching the DB if we already know that there are no bundles
        ** remaining in storage that have not been egressed
        */

        *NumEgressed = 0;

        return BPLIB_SUCCESS;
    }

    /* Determine which channel or contact's batch we're examining */
    CacheInst = &Inst->BundleStorage;

    if (LocalDelivery)
    {
        LoadBatch           = &(CacheInst->ChannelLoadBatches[EgressID]);
        LocalEID.MaxNode    = BPLIB_EID_INSTANCE.Node;
        LocalEID.MinNode    = BPLIB_EID_INSTANCE.Node;
        LocalEID.MaxService = Inst->ChanCtxt[EgressID].Config.LocalServiceNumber;
        LocalEID.MinService = Inst->ChanCtxt[EgressID].Config.LocalServiceNumber;
        DestEIDs            = &LocalEID;
        NumEIDs             = 1;
        EgressQueue         = &(Inst->ChannelEgressJobs[EgressID]);
    }
    else
    {
        LoadBatch   = &(CacheInst->ContactLoadBatches[EgressID]);
        DestEIDs    = Inst->ContCtxt[EgressID].Config.DestEIDs;
        NumEIDs     = BPLIB_MAX_CONTACT_DEST_EIDS;
        EgressQueue = &(Inst->ContactEgressJobs[EgressID]);
    }

    pthread_mutex_lock(&CacheInst->lock);

    /* If the load batch is empty, try to read more from storage */
    if (BPLib_STOR_LoadBatch_IsEmpty(LoadBatch))
    {
        /* Ask SQL to load egressable bundles from the specified Destination EID */
        Status = BPLib_SQL_FindForEIDs(Inst, LoadBatch, DestEIDs, NumEIDs, LocalDelivery);
        if (Status != BPLIB_SUCCESS)
        {
            BPLib_EM_SendEvent(BPLIB_STOR_SQL_LOAD_ERR_EID,
                                BPLib_EM_EventType_ERROR,
                                "BPLib_SQL_FindForEIDs failed to load bundle. RC=%d",
                                Status);
        }
    }
    else if (BPLib_STOR_LoadBatch_IsConsumed(LoadBatch))
    { /* All of the bundles for this batch have been egressed */
        /* Mark the batch as egressed */
        Status = BPLib_SQL_MarkBatchEgressed(Inst, LoadBatch, LocalDelivery);

        /* Retain the consumed batch on failure so the next poll retries the
         * database marking operation without reloading delivered bundles. */
        if (Status == BPLIB_SUCCESS)
        {
            (void) BPLib_STOR_LoadBatch_Reset(LoadBatch);
        }
    }
    else
    { /* There are bundles in the current batch that need to be egressed */
        while (BPLib_STOR_LoadBatch_PeekNextID(LoadBatch, &CurrBundleID) == BPLIB_SUCCESS &&
                !BPLib_QM_WaitQueueIsFull(EgressQueue))
        {
            /* Set the metadata EID */
            Status = BPLib_SQL_LoadBundle(Inst, CurrBundleID, &CurrBundle);
            if (Status == BPLIB_SUCCESS)
            {
                CurrBundle->Meta.EgressID = EgressID;                
                if (BPLib_QM_WaitQueueTryPush(EgressQueue, &CurrBundle, QM_NO_WAIT) == false)
                {
                    /* If QM couldn't accept the bundle, free it. It will be reloaded
                    ** next time.
                    */
                    BPLib_MEM_BundleFree(&Inst->pool, CurrBundle);

                    break;
                }

                /* After the bundle made it into the destination queue, mark it consumed */
                (void) BPLib_STOR_LoadBatch_AdvanceReader(LoadBatch);
                EgressCnt++;
            }
            else if (Status == BPLIB_STOR_NO_BUNDLE_FOUND_ERR)
            {
                /* Bundle ID belongs to a bundle that is now invalid, discard it but keep going */
                (void) BPLib_STOR_LoadBatch_AdvanceReader(LoadBatch);
            }
            else if (Status == BPLIB_STOR_NO_MEM_ERR)
            {
                BPLib_EM_SendEvent(BPLIB_STOR_NO_MEM_ERR_EID, BPLib_EM_EventType_DEBUG,
                            "Ran out of memory when trying to load a bundle from storage, try again next time. Bytes in use = %ld\n",
                            BPLib_MEM_GetBytesInUse(&Inst->pool));
                Status = BPLIB_SUCCESS;

                break;
            }
            else
            {
                /* If LoadBundle Failed, don't keep trying. */
                break;
            }
        }
    }

    if (EgressCnt != 0)
    {
        BPLib_STOR_SetLastActiveTime(Inst);
    }

    pthread_mutex_unlock(&CacheInst->lock);

    *NumEgressed = EgressCnt;
    return Status;
}

void BPLib_STOR_SetLastActiveTime(BPLib_Instance_t* Inst)
{
    if (Inst == NULL)
    {
        return;
    }

    Inst->BundleStorage.LastActiveTime = BPLib_TIME_GetMonotonicTime();
}

bool BPLib_STOR_IsIngressEgressActive(BPLib_Instance_t* Inst)
{
    bool IsActive = false;

    if (Inst == NULL)
    {
        return false;
    }

    if ((Inst->BundleStorage.LastActiveTime + BPLIB_STOR_MAX_IDLE_TIME) > BPLib_TIME_GetMonotonicTime())
    {
        IsActive = true;
    }

    return IsActive;
}

BPLib_Status_t BPLib_STOR_GarbageCollect(BPLib_Instance_t* Inst)
{
    BPLib_Status_t       Status = BPLIB_SUCCESS;
    BPLib_Status_t       FirstError = BPLIB_SUCCESS;
    BPLib_BundleCache_t* CacheInst;
    size_t               NumDiscarded;
    size_t               NumExpired;
    size_t               DbSize;
    bool                 TriggerCustodialGc;

    NumDiscarded = 0;
    NumExpired   = 0;

    if (Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    TriggerCustodialGc = BPLib_CT_TriggerCustodialGarbageCollection(Inst);

    CacheInst = &Inst->BundleStorage;

    pthread_mutex_lock(&CacheInst->lock);    

    /* Retry retained custody work even when traffic suppresses garbage collection. */
    if (CacheInst->CustodyUpdateBatch.Size > 0)
    {
        Status = BPLib_STOR_UpdateCustodialBundlesUnlocked(Inst, &CacheInst->CustodyUpdateBatch);
        if (Status != BPLIB_SUCCESS) FirstError = Status;
    }


    if (TriggerCustodialGc || 
       (BPLib_STOR_IsIngressEgressActive(Inst) == false && CacheInst->BundleCountStored != 0))
    {
        /* 
        ** Avoid searching the DB if any of the ingress/egress queues are not empty or
        ** storage operations are ongoing.
        ** Note: this is a pretty critical performance optimization that allows bplib
        ** to use all of its CPU resources for ingress and egress.
        */

        Status = BPLib_SQL_DiscardExpired(Inst, &NumExpired);
        if (Status != BPLIB_SUCCESS)
        {
            if (FirstError == BPLIB_SUCCESS) FirstError = Status;
            BPLib_EM_SendEvent(BPLIB_STOR_SQL_GC_ERR_EID,
                                BPLib_EM_EventType_ERROR,
                                "BPLib_SQL_DiscardExpired failed. RC=%d",
                                Status);
        }
        else if (NumExpired > 0)
        {
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED_EXPIRED, NumExpired);
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED, NumExpired);
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DISCARDED, NumExpired);

            CacheInst->BundleCountStored -= NumExpired;
            CacheInst->BundleCountNotEgressed -= NumExpired;

            BPLib_EM_SendEvent(BPLIB_STOR_EXPIRE_DBG_EID,
                                BPLib_EM_EventType_DEBUG,
                                "Discarded %d expired bundles from storage",
                                NumExpired);
        }

        Status = BPLib_SQL_DiscardEgressed(Inst, &NumDiscarded);
        if (Status != BPLIB_SUCCESS)
        {
            if (FirstError == BPLIB_SUCCESS) FirstError = Status;
            BPLib_EM_SendEvent(BPLIB_STOR_SQL_GC_ERR_EID,
                                BPLib_EM_EventType_ERROR,
                                "BPLib_SQL_DiscardEgressed failed. RC=%d",
                                Status);
        }
        else if (NumDiscarded > 0)
        {
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DISCARDED, NumDiscarded);
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DELETED, NumDiscarded);

            CacheInst->BundleCountStored -= NumDiscarded;

            BPLib_EM_SendEvent(BPLIB_STOR_DELETE_DBG_EID,
                                BPLib_EM_EventType_DEBUG,
                                "Discarded %d egressed bundles from storage",
                                NumDiscarded);
        }
    }

    /* Storage just dropped to 0 bundles stored - trigger automatic cleanup */
    if ((NumDiscarded > 0 || NumExpired > 0) && CacheInst->BundleCountStored == 0)
    {
        BPLib_EM_SendEvent(BPLIB_STOR_AUTO_CLEAN_START_INF_EID, BPLib_EM_EventType_INFORMATION,
                            "Automatic storage cleanup triggered, this could take a minute...");

        Status = BPLib_SQL_Cleanup(Inst);
        if (Status == BPLIB_SUCCESS)
        {
            BPLib_EM_SendEvent(BPLIB_STOR_AUTO_CLEAN_END_INF_EID, BPLib_EM_EventType_INFORMATION,
                    "Automatic storage cleanup completed.");            
        }
        else
        {
            if (FirstError == BPLIB_SUCCESS) FirstError = Status;
            BPLib_EM_SendEvent(BPLIB_STOR_AUTO_CLEAN_ERR_EID, BPLib_EM_EventType_ERROR,
                    "Automatic storage cleanup failed, Error = %d.", Status);              
        }
    }

    /* While we're at it, update our records with the latest storage size */
    Status = BPLib_SQL_GetDbSize(Inst, &DbSize);
    if (Status == BPLIB_SUCCESS)
    {
        Inst->BundleStorage.StorageSize = DbSize;
    }
    else
    {
        if (FirstError == BPLIB_SUCCESS) FirstError = Status;
        BPLib_EM_SendEvent(BPLIB_STOR_DB_GET_SIZE_ERR_EID,
                            BPLib_EM_EventType_ERROR,
                            "Error getting database size, RC = %d.",
                            Status);
    }

    pthread_mutex_unlock(&CacheInst->lock);

    return FirstError;
}

void BPLib_STOR_UpdateHkPkt(BPLib_Instance_t* Inst)
{
    /* Update total storage size */
    BPLib_STOR_StoragePayload.KbStorageInUse = Inst->BundleStorage.StorageSize / 1000;

    /* Update the memory in use*/
    BPLib_STOR_StoragePayload.BytesMemInUse = BPLib_MEM_GetBytesInUse(&Inst->pool);

    /* Update the highwater mark */
    BPLib_STOR_StoragePayload.BytesMemHighWater = BPLib_MEM_GetHighwaterMark(&Inst->pool);

    /* Update the free memory */
    BPLib_STOR_StoragePayload.BytesMemFree = BPLib_MEM_GetBytesFree(&Inst->pool);

    /* Update kilobytes of data in use */
    BPLib_STOR_StoragePayload.KbBundlesInStor = (Inst->BundleStorage.BytesStorageInUse / 1000);

    return;
}


BPLib_Status_t BPLib_STOR_Cleanup(BPLib_Instance_t* Inst)
{
    BPLib_Status_t Status;

    if (Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    BPLib_EM_SendEvent(BPLIB_STOR_CLEANUP_INF_EID, BPLib_EM_EventType_INFORMATION,
            "Beginning storage cleanup. This may take a while and may interrupt any pending storage operations.");

    pthread_mutex_lock(&(Inst->BundleStorage.lock));

    Status = BPLib_SQL_Cleanup(Inst);

    pthread_mutex_unlock(&(Inst->BundleStorage.lock));
    
    return Status;
}

BPLib_Status_t BPLib_STOR_AddToCustodialUpdateBatch(BPLib_Instance_t *Inst, uint32_t BundleId,
                                                   BPLib_CT_StorOp_t Op)
{
    if (Inst == NULL) return BPLIB_NULL_PTR_ERROR;
    if (Op != BPLIB_CT_MARK_DELETE && Op != BPLIB_CT_STOP_RETRANSMIT &&
        Op != BPLIB_CT_START_RETRANSMIT) return BPLIB_STOR_PARAM_ERR;

    /* A CCS may release upstream custody as soon as this returns success.
     * Commit synchronously: no accepted operation exists only in RAM. */
    BPLib_STOR_CtUpdateBatch_t Batch = {.Size = 1, .BundleIDs = {BundleId}, .Ops = {Op}};
    pthread_mutex_lock(&Inst->BundleStorage.lock);
    BPLib_Status_t Status = BPLib_SQL_UpdateCustodialBundles(Inst, &Batch);
    pthread_mutex_unlock(&Inst->BundleStorage.lock);
    return Status;
}

void BPLib_STOR_UpdateCustodialBundles(BPLib_Instance_t* Inst)
{
    BPLib_STOR_CtUpdateBatch_t *CustodyBatch;

    if (Inst == NULL)
    {
        return;
    }

    pthread_mutex_lock(&(Inst->BundleStorage.lock));

    CustodyBatch = &(Inst->BundleStorage.CustodyUpdateBatch);

    if (CustodyBatch->Size <= BPLIB_STOR_CT_BATCH_SIZE && CustodyBatch->Size > 0)
    {
        BPLib_STOR_UpdateCustodialBundlesUnlocked(Inst, CustodyBatch);
    }

    pthread_mutex_unlock(&(Inst->BundleStorage.lock));
    
    return;
}

BPLib_Status_t BPLib_STOR_SetNewRetransmitTrigger(BPLib_Instance_t *Inst, uint32_t ContactId)
{
    BPLib_Status_t Status;
    size_t NumUpdated;

    if (Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    if (ContactId >= BPLIB_MAX_NUM_CONTACTS)
    {
        return BPLIB_INVALID_CONT_ID_ERR;
    }

    pthread_mutex_lock(&(Inst->BundleStorage.lock));

    Status = BPLib_SQL_SetNewRetransmitTrigger(Inst, ContactId,
                Inst->ContCtxt[ContactId].Config.DestEIDs, BPLIB_MAX_CONTACT_DEST_EIDS, 
                Inst->ContCtxt[ContactId].Config.RetransmitTimeout, &NumUpdated);

    pthread_mutex_unlock(&(Inst->BundleStorage.lock));

    if (Status == BPLIB_SUCCESS)
    {
        BPLib_EM_SendEvent(BPLIB_STOR_RETRANSMIT_UPDATE_DBG_EID, BPLib_EM_EventType_DEBUG,
                        "Updated retransmit triggers of %ld bundles for contact %d.",
                        NumUpdated, ContactId);
    }

    /* Error event handled upstream */
    
    return Status;    
}

BPLib_Status_t BPLib_STOR_Egress(BPLib_Instance_t *Instance, size_t MaxBundles)
{
    BPLib_Status_t Status;
    uint32_t ChanId, ContId;
    size_t NumLoaded;
    BPLib_CLA_ContactRunState_t ConState;

    for (ChanId = 0; ChanId < BPLIB_MAX_NUM_CHANNELS; ChanId++)
    {
        if (BPLib_NC_GetAppState(ChanId) == BPLIB_NC_APP_STATE_STARTED &&
            BPLib_PI_GetRegistrationState(Instance, ChanId) == BPLIB_PI_ACTIVE)
        {
            Status = BPLib_STOR_EgressForID(Instance, ChanId, true, &NumLoaded);
            if (Status != BPLIB_SUCCESS || NumLoaded >= MaxBundles)
            {
                return Status;
            }
        }
    }

    for (ContId = 0; ContId < BPLIB_MAX_NUM_CONTACTS; ContId++)
    {
        (void) BPLib_CLA_GetContactRunState(ContId, &ConState);
        if (ConState == BPLIB_CLA_STARTED)
        {
            Status = BPLib_STOR_EgressForID(Instance, ContId, false, &NumLoaded);
            if (Status != BPLIB_SUCCESS || NumLoaded >= MaxBundles)
            {
                return Status;
            }
        }
    }

    return BPLIB_SUCCESS;
}
