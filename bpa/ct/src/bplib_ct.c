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

/*
** Include
*/

#include "bplib_ct.h"
#include "bplib_ct_db.h"
#include "bplib_ct_ccs.h"
#include "bplib_bblocks.h"
#include "bplib_eid.h"
#include "bplib_pdb.h"
#include "bplib_as.h"
#include "bplib_inst.h"
#include "bplib_nc.h"
#include "bplib_bi.h"
#include <errno.h>


/*
** Internal Function Definitions
*/

static BPLib_Status_t BPLib_CT_AssignSeqCounterUnlocked(BPLib_Instance_t *Inst, uint32_t ContactId);

uint8_t BPLib_CT_GetCtebIndex(BPLib_Bundle_t *Bundle)
{
    uint8_t ExtBlockIdx;
    
    if (Bundle == NULL)
    {
        return BPLIB_MAX_NUM_EXTENSION_BLOCKS;
    }

    for (ExtBlockIdx = 0; ExtBlockIdx < BPLIB_MAX_NUM_EXTENSION_BLOCKS; ExtBlockIdx++)
    {
        if (Bundle->blocks.ExtBlocks[ExtBlockIdx].Header.BlockType == BPLib_BlockType_CTEB)
        {
            return ExtBlockIdx;
        }
    }

    return BPLIB_MAX_NUM_EXTENSION_BLOCKS;
}


BPLib_Status_t BPLib_CT_SignalCustodyImpl(BPLib_Instance_t *Inst, BPLib_Bundle_t *Bundle,
                        BPLib_CT_DispositionCode_t DispCode, bool StoreBundle, uint8_t ExtBlockIdx)
{
    size_t OpenCcsIdx;
    BPLib_CustodyBlockData_t *CtebPtr;
    BPLib_Status_t Status = BPLIB_SUCCESS;
    BPLib_CT_DbEntry_t *DbEntry = NULL;

    CtebPtr = &(Bundle->blocks.ExtBlocks[ExtBlockIdx].BlockData.CustodyBlockData);

    /* Check to see if the bundle is in the CTDB */
    Status = BPLib_CT_GetEntryFromCtdbWithId(&(Inst->Ct), 
                            Bundle->blocks.PrimaryBlock.BundleId, &DbEntry);
    if (Status == BPLIB_SUCCESS && DbEntry != NULL)
    {
        if (DispCode == BPLib_CT_CustodyRefused)
        {
            /* Make sure any CTDB entry that might exist with this bundle are removed */
            (void) BPLib_CT_RemoveFromCtdb(Inst, DbEntry);
        }
    }
    else if (DispCode == BPLib_CT_CustodyAccepted)
    {
        return BPLIB_NOT_FOUND_ERR;
    }

    /* For foreign bundles, need to accept/reject custody with previous node via CCS */
    if (Bundle->Meta.LocalBundle == false && Bundle->Meta.IngressID < BPLIB_MAX_NUM_CONTACTS)
    {
        OpenCcsIdx = BPLib_CT_GetOpenCcsIdx(Inst, &(CtebPtr->BlockSrcAdminEID),
                                            CtebPtr->BundleSeqId);

        Status = BPLib_CT_AddToOpenCcs(Inst, OpenCcsIdx, Bundle->Meta.IngressID,
                                        CtebPtr, DispCode);
    }

    if (DispCode == BPLib_CT_CustodyAccepted)
    {
        if (StoreBundle)
        {
            Inst->Ct.BundleCountInCustody++;
            DbEntry->State = BPLib_CT_InCustody;
        }
        
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_ACCEPTED_CUSTODY, 1);
    }
    else
    {
        Status = BPLIB_CT_CUSTODY_REFUSED_ERR;
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_REJECTED_CUSTODY, 1);
    }

    return Status;
}

/*
** External Function Definitions
*/

BPLib_Status_t BPLib_CT_Init(BPLib_Instance_t *Inst)
{
    if (Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    memset(&(Inst->Ct), 0, sizeof(BPLib_CT_Context_t));

    pthread_mutex_init(&Inst->Ct.Lock, NULL);

    BPLib_RBT_InitRoot(&(Inst->Ct.SeqTreeRoot));
    BPLib_RBT_InitRoot(&(Inst->Ct.IdTreeRoot));

    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_CT_SetBundleId(BPLib_Bundle_t *Bundle)
{
    uint64_t UniqueIdArr[BPLIB_CT_BUNDLE_IDENTIFIER_ARRAY_LEN];

    if (Bundle == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    UniqueIdArr[0] = Bundle->blocks.PrimaryBlock.Timestamp.SequenceNumber;
    UniqueIdArr[1] = Bundle->blocks.PrimaryBlock.Timestamp.CreateTime;
    UniqueIdArr[2] = Bundle->blocks.PrimaryBlock.SrcEID.Scheme;
    UniqueIdArr[3] = Bundle->blocks.PrimaryBlock.SrcEID.IpnSspFormat;
    UniqueIdArr[4] = Bundle->blocks.PrimaryBlock.SrcEID.Allocator;
    UniqueIdArr[5] = Bundle->blocks.PrimaryBlock.SrcEID.Node;
    UniqueIdArr[6] = Bundle->blocks.PrimaryBlock.SrcEID.Service;

    /*
    ** Add any additional unique identifiers for a bundle here
    ** (and increase the array length accordingly)
    */

    /* Use a CRC-32C as a quick hash function to get a unique ID for this bundle */
    Bundle->blocks.PrimaryBlock.BundleId = BPLib_CRC_Calculate((void *) ((uintptr_t) UniqueIdArr),
                                BPLIB_CT_BUNDLE_IDENTIFIER_ARRAY_LEN * sizeof(uint64_t),
                                BPLib_CRC_Type_CRC32C);

    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_CT_ProcessNewBundle(BPLib_Instance_t* Inst, BPLib_Bundle_t *Bundle)
{
    BPLib_Status_t Status = BPLIB_SUCCESS;
    uint8_t ExtBlockIdx;
    BPLib_CT_DbEntry_t *DbEntry = NULL;
    BPLib_CT_DispositionCode_t DispCode;
    char BundleInfo[BPLIB_MAX_BUNDLE_INFO_STR_LENGTH];
    bool StoreBundle = true;

    if (Bundle == NULL || Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Set bundle ID for both custodial and non-custodial bundles */
    (void) BPLib_CT_SetBundleId(Bundle);

    BPLib_BI_GetBundleInfo(Bundle, BundleInfo, BPLIB_MAX_BUNDLE_INFO_STR_LENGTH);

    /* No CTEB was found, skip custody operations for this bundle */
    ExtBlockIdx = BPLib_CT_GetCtebIndex(Bundle);
    if (ExtBlockIdx >= BPLIB_MAX_NUM_EXTENSION_BLOCKS)
    {
        return BPLIB_SUCCESS;
    }

    BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_CUSTODY_REQUEST, 1);
    Bundle->Meta.IsCustodial = true;

    /* Default to refused custody */
    DispCode = BPLib_CT_CustodyRefused;

    pthread_mutex_lock(&Inst->Ct.Lock);

    /* Resolve existing custody before resource refusal: refusing a duplicate
     * must never remove the already-owned original entry. */
    BPLib_Status_t LookupStatus = BPLib_CT_GetEntryFromCtdbWithId(&Inst->Ct,
                                                Bundle->blocks.PrimaryBlock.BundleId, &DbEntry);
    if (LookupStatus == BPLIB_SUCCESS)
    {
        if (DbEntry == NULL)
        {
            pthread_mutex_unlock(&Inst->Ct.Lock);
            return BPLIB_NOT_FOUND_ERR;
        }
        BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_REDUNDANT, 1);
        DispCode = BPLib_CT_CustodyAccepted;
        Status = BPLIB_CT_DUPLICATE_ERR;
        StoreBundle = false;
        if (DbEntry->State == BPLib_CT_Initialized)
        {
            /* The original still awaits a durable commit. Keep its entry and
             * withhold an acknowledgement regardless of resource pressure. */
            pthread_mutex_unlock(&Inst->Ct.Lock);
            return Status;
        }
    }
    else if (LookupStatus != BPLIB_NOT_FOUND_ERR)
    {
        pthread_mutex_unlock(&Inst->Ct.Lock);
        return LookupStatus;
    }
    else
    {
        /* Advisory snapshot only. Never block under Ct.Lock on storage, whose
         * cleanup may need Ct.Lock. StoreBundle rechecks under its own lock. */
        size_t StorageUsed = 0;
        int LockStatus = pthread_mutex_trylock(&Inst->BundleStorage.lock);
        if (LockStatus == 0)
        {
            StorageUsed = Inst->BundleStorage.BytesStorageInUse;
            pthread_mutex_unlock(&Inst->BundleStorage.lock);
        }
        else if (LockStatus != EBUSY)
        {
            pthread_mutex_unlock(&Inst->Ct.Lock);
            return BPLIB_ERROR;
        }
        /* Subtract only after checking usage, preserving the existing exact-limit
         * refusal policy without allowing a size_t addition to wrap. */
        if (StorageUsed >= BPLIB_MAX_STORED_BUNDLE_BYTES ||
                 Bundle->Meta.TotalBytes >= BPLIB_MAX_STORED_BUNDLE_BYTES - StorageUsed)
        {
            size_t Remaining = StorageUsed < BPLIB_MAX_STORED_BUNDLE_BYTES
                ? BPLIB_MAX_STORED_BUNDLE_BYTES - StorageUsed : 0;
            BPLib_EM_SendEvent(BPLIB_CT_NO_STOR_ERR_EID, BPLib_EM_EventType_ERROR,
                                "Cannot process and accept bundle, not enough storage (%zu bytes remaining). %s.",
                                Remaining, BundleInfo);
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DEPLETED, 1);
            Status = BPLIB_CT_CUSTODY_REFUSED_ERR;
        }
        else if (Inst->Ct.CurrDbSize >= BPLIB_CT_DB_MAX_ENTRIES)
        {
            BPLib_EM_SendEvent(BPLIB_CT_NO_MEM_ERR_EID, BPLib_EM_EventType_ERROR,
                                "Cannot accept bundle, CTDB is full. %s", BundleInfo);
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_DEPLETED, 1);
            Status = BPLIB_CT_CUSTODY_REFUSED_ERR;
        }
        /* Custody accepted! */
        else if (BPLib_PDB_AcceptCustody(Bundle) == BPLIB_SUCCESS)
        {
            DispCode = BPLib_CT_CustodyAccepted;
        }
        else
        {
            Status = BPLIB_CT_CUSTODY_REFUSED_ERR;
        }
    }

    if (Status == BPLIB_SUCCESS)
    {
        /* Add bundle to CTDB but leave sequence ID/number undefined until egress */
        Status = BPLib_CT_InitEntry(Inst, Bundle->blocks.PrimaryBlock.BundleId);

        if (Status != BPLIB_SUCCESS)
        {
            DispCode = BPLib_CT_CustodyRefused;
        }
    }

    if (Status != BPLIB_SUCCESS)
    {
        /*
        ** Signal custody for rejected bundles or duplicate bundles, otherwise wait
        ** until a bundle is successfully stored to formally accept it
        */
        (void) BPLib_CT_SignalCustodyImpl(Inst, Bundle, DispCode, StoreBundle, ExtBlockIdx);
    }    
    
    pthread_mutex_unlock(&Inst->Ct.Lock);

    return Status;
}

BPLib_Status_t BPLib_CT_SignalCustody(BPLib_Instance_t *Inst, BPLib_Bundle_t *Bundle,
                            BPLib_CT_DispositionCode_t DispCode, bool StoreBundle)
{
    uint8_t ExtBlockIdx;
    BPLib_Status_t Status;

    if (Bundle == NULL || Inst == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Bundle is not custodial, do nothing */
    ExtBlockIdx = BPLib_CT_GetCtebIndex(Bundle);
    if (ExtBlockIdx >= BPLIB_MAX_NUM_EXTENSION_BLOCKS)
    {
        return BPLIB_SUCCESS;
    }

    pthread_mutex_lock(&Inst->Ct.Lock);
    Status = BPLib_CT_SignalCustodyImpl(Inst, Bundle, DispCode, StoreBundle, ExtBlockIdx);

    pthread_mutex_unlock(&Inst->Ct.Lock);

    return Status;
}

BPLib_Status_t BPLib_CT_UpdateBundle(BPLib_Instance_t* Inst, BPLib_Bundle_t *Bundle)
{
    BPLib_Status_t            Status = BPLIB_SUCCESS;
    BPLib_CustodyBlockData_t *CtebPtr;
    uint8_t                   ExtBlockIdx;
    BPLib_CT_DbEntry_t       *DbEntry = NULL;

    if (Inst == NULL || Bundle == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Do nothing for non-custodial bundles */
    ExtBlockIdx = BPLib_CT_GetCtebIndex(Bundle);
    if (ExtBlockIdx >= BPLIB_MAX_NUM_EXTENSION_BLOCKS)
    {
        return BPLIB_SUCCESS;
    }

    if (Bundle->Meta.EgressID >= BPLIB_MAX_NUM_CONTACTS)
    {
        BPLib_EM_SendEvent(BPLIB_CT_INV_EGRESS_ID_ERR_EID, BPLib_EM_EventType_ERROR,
                "Bundle has an invalid egress ID %d, check for memory corruption.", Bundle->Meta.EgressID);

        return BPLIB_INVALID_CONT_ID_ERR;
    }

    CtebPtr = &(Bundle->blocks.ExtBlocks[ExtBlockIdx].BlockData.CustodyBlockData);

    pthread_mutex_lock(&Inst->Ct.Lock);
    
    Status = BPLib_CT_GetEntryFromCtdbWithId(&(Inst->Ct),
                        Bundle->blocks.PrimaryBlock.BundleId, &DbEntry);
    if (Status == BPLIB_SUCCESS && DbEntry == NULL) Status = BPLIB_NOT_FOUND_ERR;
    if (Status != BPLIB_SUCCESS)
    {
        /* 
        ** Possible causes of this:
        **  - The CTDB has been corrupted -> unlikely and if this is the case,
        **    nothing we can really do
        **  - The node restarted without a mission-specific rebuild of pending
        **    custody entries from durable storage
        **  - Some mismatch in the threads where this bundle was loaded into
        **    memory in a retransmission right before a CCS confirms its
        **    retransmission and deletes the bundle from the CTDB -> possible, 
        **    so just throw the bundle out and fail silently
        */
    }
    else
    {
        /* Sanity check the DbEntry's state */
        if (DbEntry->State == BPLib_CT_Initialized)
        {
            BPLib_EM_SendEvent(BPLIB_CT_NONCUSTODIAL_ERR_EID, BPLib_EM_EventType_ERROR,
                    "Error, cannot update a custodial bundle whose custody has not been finalized; state = %d, seq_num = %llu",
                    (int)DbEntry->State, (unsigned long long)DbEntry->SeqNum);
            Status = BPLIB_CT_CUSTODY_REFUSED_ERR;
        }
        else if (DbEntry->State == BPLib_CT_Transferred)
        {
            /* 
            ** Silently trigger bundle deletion, storage probably just triggered a
            ** retransmission right before its custody was transferred successfully
            */
            Status = BPLIB_CT_CUSTODY_REFUSED_ERR;
        }

        /* Bundle CTEB fields have not been updated yet, assign new values */
        else if (DbEntry->State == BPLib_CT_InCustody)
        {
            /* Reserve a fresh stream before the old counter could wrap to zero.
             * MAX is left unused locally; inbound MAX remains supported. */
            if (Inst->Ct.SeqCounters[Bundle->Meta.EgressID].Counter == UINT64_MAX)
                Status = BPLib_CT_AssignSeqCounterUnlocked(Inst, Bundle->Meta.EgressID);
            if (Status == BPLIB_SUCCESS)
            {
                uint64_t SeqId = BPLib_CT_GetSequenceId(&Inst->Ct, Bundle->Meta.EgressID);
                uint64_t SeqNum = BPLib_CT_GetNextSequenceNum(&Inst->Ct, Bundle->Meta.EgressID);
                Status = BPLib_CT_UpdateEntry(Inst, DbEntry, SeqId, SeqNum);
                if (Status == BPLIB_SUCCESS)
                {
                    CtebPtr->BundleSeqId = SeqId;
                    CtebPtr->BundleSeqNum = SeqNum;
                    DbEntry->State = BPLib_CT_Transmitted;
                }
            }
        }
        
        /* Bundle CTEB fields have been set, this is a retransmission */
        else 
        {
            CtebPtr->BundleSeqId = DbEntry->SeqId;
            CtebPtr->BundleSeqNum = DbEntry->SeqNum;

            DbEntry->State = BPLib_CT_Retransmitted;

            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_CUSTODY_RE_FORWARDED, 1);
        }

        if (Status == BPLIB_SUCCESS)
        {
            BPLib_EID_CopyEids(&(CtebPtr->BlockSrcAdminEID), BPLIB_EID_INSTANCE);
            Bundle->blocks.ExtBlocks[ExtBlockIdx].Header.RequiresEncode = true;
        }
    }

    pthread_mutex_unlock(&Inst->Ct.Lock);

    return Status;
}

BPLib_Status_t BPLib_CT_ProcessCcs(BPLib_Instance_t *Inst, BPLib_CT_DeserializedCcs_t *Ccs)
{
    size_t SeqCollectIdx;
    BPLib_Status_t Status = BPLIB_SUCCESS;

    if (Inst == NULL || Ccs == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    if (Ccs->NumBundleSeqCollections > BPLIB_CT_MAX_RECVD_SEQ_COLLECTIONS)
    {
        return BPLIB_BUF_LEN_ERROR;
    }

    for (SeqCollectIdx = 0; SeqCollectIdx < Ccs->NumBundleSeqCollections; SeqCollectIdx++)
    {
        if (Ccs->BundleSeqCollections[SeqCollectIdx].SeqRangeLen > BPLIB_CT_MAX_SEQ_RANGE_LEN ||
            Ccs->BundleSeqCollections[SeqCollectIdx].SeqRangeLen % 2 == 0)
        {
            Status = BPLIB_BUF_LEN_ERROR;
            break;
        }

        Status = BPLib_CT_ProcessBundleSeqCollection(Inst, &(Ccs->BundleSeqCollections[SeqCollectIdx]));

        if (Status != BPLIB_SUCCESS)
        {
            BPLib_EM_SendEvent(BPLIB_QM_CT_IN_ERR_EID, BPLib_EM_EventType_ERROR,
                    "BSC processing encountered an error, disp_code=%ld, first_seq_num=%ld. Status = %d", 
                    Ccs->BundleSeqCollections[SeqCollectIdx].DispositionCode,
                    Ccs->BundleSeqCollections[SeqCollectIdx].FirstSeqNum, Status);
        }
    }

    return Status;
}

/* Caller holds Ct.Lock; callback failure leaves the current stream untouched. */
static BPLib_Status_t BPLib_CT_AssignSeqCounterUnlocked(BPLib_Instance_t *Inst, uint32_t ContactId)
{
    uint64_t Id = 0;
    if (Inst->Ct.AllocateSequenceId != NULL)
    {
        BPLib_Status_t Status = Inst->Ct.AllocateSequenceId(&Id);
        if (Status != BPLIB_SUCCESS) return Status;
        if (Id == 0 || Id == BPLIB_CT_SEQ_ID_ROLLOVER_VALUE ||
            Id == Inst->Ct.SeqCounters[ContactId].Id) return BPLIB_INVALID_CONFIG_ERR;
    }
    else
    {
        if (Inst->Ct.LastSeqCounterId == UINT64_MAX) return BPLIB_INVALID_CONFIG_ERR;
        Id = Inst->Ct.LastSeqCounterId + 1;
        if (Id == BPLIB_CT_SEQ_ID_ROLLOVER_VALUE) ++Id;
        Inst->Ct.LastSeqCounterId = Id;
    }
    Inst->Ct.SeqCounters[ContactId].Id = Id;
    Inst->Ct.SeqCounters[ContactId].Counter = 0;
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_CT_AssignSeqCounter(BPLib_Instance_t *Inst, uint32_t ContactId)
{
    if (Inst == NULL) return BPLIB_NULL_PTR_ERROR;
    if (ContactId >= BPLIB_MAX_NUM_CONTACTS) return BPLIB_INVALID_CONT_ID_ERR;
    pthread_mutex_lock(&Inst->Ct.Lock);
    BPLib_Status_t Status = BPLib_CT_AssignSeqCounterUnlocked(Inst, ContactId);
    pthread_mutex_unlock(&Inst->Ct.Lock);
    return Status;
}

void BPLib_CT_DeleteBundleFromCtdb(BPLib_Instance_t *Inst, uint32_t BundleId)
{
    BPLib_CT_DbEntry_t *DbEntry = NULL;
    BPLib_Status_t Status = BPLIB_SUCCESS;

    if (Inst == NULL)
    {
        return;
    }

    pthread_mutex_lock(&Inst->Ct.Lock);

    Status = BPLib_CT_GetEntryFromCtdbWithId(&(Inst->Ct), BundleId, &DbEntry);
    if (Status == BPLIB_SUCCESS)
    {
        Status = BPLib_CT_RemoveFromCtdb(Inst, DbEntry);
    }

    if (Status != BPLIB_SUCCESS)
    {
        BPLib_EM_SendEvent(BPLIB_CT_DB_DELETE_ERR_EID, BPLib_EM_EventType_ERROR,
                    "Error, could not delete bundle ID 0x%x from CTDB. Status = %d.",
                    BundleId, Status);
    }

    pthread_mutex_unlock(&Inst->Ct.Lock);
}

void BPLib_CT_BuildAndSendOpenCcs(BPLib_Instance_t* Instance, BPLib_CT_OpenCcs_t* OpenCcs)
{
    pthread_mutex_lock(&Instance->Ct.Lock);
    BPLib_CT_BuildAndSendOpenCcs_Impl(Instance, OpenCcs);
    pthread_mutex_unlock(&Instance->Ct.Lock);
}

void BPLib_CT_CheckCcsTimeout(BPLib_Instance_t* Instance)
{
    BPLib_CT_Context_t* Context;
    size_t              OpenCcsIdx;
    int64_t             TimeOpen;
    BPLib_CT_OpenCcs_t* OpenCcs;

    pthread_mutex_lock(&Instance->Ct.Lock);

    Context = &(Instance->Ct);
    for (OpenCcsIdx = 0; OpenCcsIdx < BPLIB_CT_MAX_OPEN_CCS; OpenCcsIdx++)
    {
        OpenCcs = &(Context->OpenCcss[OpenCcsIdx]);

        /* Check whether in progress CCSs exceed the time trigger */
        if (OpenCcs->InProgress == true && OpenCcs->CollectionStartTime != 0)
        {
            TimeOpen = BPLib_TIME_GetMonotonicTime() - OpenCcs->CollectionStartTime;

            /* Check if the open CCS is due to be sent */
            if (TimeOpen > OpenCcs->MaxTime)
            {
                BPLib_CT_BuildAndSendOpenCcs_Impl(Instance, OpenCcs);
            }
        }
    }

    pthread_mutex_unlock(&Instance->Ct.Lock);
}

BPLib_Status_t BPLib_CT_CompleteDelivery(BPLib_Instance_t *Inst, BPLib_Bundle_t *Bundle)
{
    BPLib_CT_DbEntry_t *DbEntry = NULL;
    BPLib_Status_t Status = BPLIB_SUCCESS;
    uint8_t ExtBlockIdx;

    if (Inst == NULL || Bundle == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Do nothing for non-custodial bundles */
    ExtBlockIdx = BPLib_CT_GetCtebIndex(Bundle);
    if (ExtBlockIdx >= BPLIB_MAX_NUM_EXTENSION_BLOCKS)
    {
        return BPLIB_SUCCESS;
    }

    pthread_mutex_lock(&Inst->Ct.Lock);

    /* Mark bundle as transferred */
    Status = BPLib_CT_GetEntryFromCtdbWithId(&(Inst->Ct), 
                            Bundle->blocks.PrimaryBlock.BundleId, &DbEntry);
    if (Status == BPLIB_SUCCESS && DbEntry != NULL)
    {
        /* Bundle was never stored, finalize custody */
        if (DbEntry->State == BPLib_CT_Initialized)
        {
            Status = BPLib_CT_RemoveFromCtdb(Inst, DbEntry);
        }
        else
        {
            /* Let storage delete CTDB entry */
            DbEntry->State = BPLib_CT_Delivered;
            Inst->Ct.BundleCountInCustody--;
        }
    }
    else
    {
        /* Storage probably already garbage collected this entry, ignore this */
        Status = BPLIB_SUCCESS;
    }

    pthread_mutex_unlock(&Inst->Ct.Lock);

    return Status;
}

bool BPLib_CT_TriggerCustodialGarbageCollection(BPLib_Instance_t *Inst)
{
    float    DecimalVal;
    bool     TriggerGC = false;
    
    if (Inst != NULL)
    {
        pthread_mutex_lock(&Inst->Ct.Lock);

        if (Inst->Ct.CurrDbSize == 0)
        {
            TriggerGC = false;
        }
        else
        {
            DecimalVal = ((float) (Inst->Ct.CurrDbSize - Inst->Ct.BundleCountInCustody) / 
                                    (float) Inst->Ct.CurrDbSize);
            TriggerGC = (BPLIB_CT_DB_MAX_PERCENT_NONCUSTODIAL_ENTRIES <= (uint32_t)(DecimalVal * 100));
        }

        pthread_mutex_unlock(&Inst->Ct.Lock);
    }

    return TriggerGC;
}
