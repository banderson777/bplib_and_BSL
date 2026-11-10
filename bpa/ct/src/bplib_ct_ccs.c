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
#include "bplib_ct_ccs.h"
#include "bplib_ct_db.h"
#include "bplib_bblocks.h"
#include "bplib_eid.h"
#include "bplib_inst.h"
#include "bplib_pdb.h"
#include "bplib_as.h"
#include "bplib_em.h"
#include "bplib_arp.h"
#include "bplib_stor.h"
#include "bplib_nc.h"
#include "bplib_inst.h"

/*
** Function Definitions
*/

void BPLib_CT_ResetOpenCcs(BPLib_CT_OpenCcs_t *OpenCcs)
{
    memset(OpenCcs, 0, sizeof(BPLib_CT_OpenCcs_t));

    return;
}

/* Normalize at most six included intervals plus the new singleton. All
 * arithmetic is bounded by the same span policy used by the receiver. */
static BPLib_Status_t BPLib_CT_InsertSequence(BPLib_CT_BundleSeqCollection_t *Collection,
                                            uint64_t Number, size_t *CcsSize)
{
    typedef struct { uint64_t First, Last; } Interval;
    Interval Runs[(BPLIB_CT_MAX_SEQ_RANGE_LEN + 1) / 2 + 1];
    size_t Count = 0, Merged = 0;
    uint64_t Span = 0, Position = Collection->FirstSeqNum;
    if (Collection->SeqRangeLen > BPLIB_CT_MAX_SEQ_RANGE_LEN ||
        (Collection->SeqRangeLen != 0 && Collection->SeqRangeLen % 2 == 0))
        return BPLIB_CT_CUSTODY_REFUSED_ERR;
    for (size_t i = 0; i < Collection->SeqRangeLen; ++i)
    {
        uint64_t Length = Collection->SeqRange[i];
        if (Length == 0 || Length > BPLIB_CT_MAX_CCS_SEQUENCE_SPAN - Span)
            return BPLIB_CT_CUSTODY_REFUSED_ERR;
        Span += Length;
    }
    if (Span != 0 && Span - 1 > UINT64_MAX - Position)
        return BPLIB_CT_CUSTODY_REFUSED_ERR;
    for (size_t i = 0; i < Collection->SeqRangeLen; ++i)
    {
        uint64_t Length = Collection->SeqRange[i];
        if (i % 2 == 0) Runs[Count++] = (Interval){Position, Position + Length - 1};
        Position += Length; /* Wrap is harmless only after the validated final run. */
    }
    Runs[Count++] = (Interval){Number, Number};
    for (size_t i = 1; i < Count; ++i)
    {
        Interval Value = Runs[i]; size_t j = i;
        while (j > 0 && Runs[j - 1].First > Value.First) { Runs[j] = Runs[j - 1]; --j; }
        Runs[j] = Value;
    }
    for (size_t i = 0; i < Count; ++i)
    {
        if (Merged > 0 && (Runs[i].First <= Runs[Merged - 1].Last ||
            Runs[i].First - Runs[Merged - 1].Last == 1))
        {
            if (Runs[i].Last > Runs[Merged - 1].Last) Runs[Merged - 1].Last = Runs[i].Last;
        }
        else Runs[Merged++] = Runs[i];
    }
    if (Runs[Merged - 1].Last - Runs[0].First >= BPLIB_CT_MAX_CCS_SEQUENCE_SPAN ||
        Merged * 2 - 1 > BPLIB_CT_MAX_SEQ_RANGE_LEN) return BPLIB_BUF_LEN_ERROR;
    size_t NewLength = Merged * 2 - 1, OldLength = Collection->SeqRangeLen;
    if ((NewLength >= OldLength && *CcsSize > SIZE_MAX - (NewLength - OldLength)) ||
        (NewLength < OldLength && *CcsSize < OldLength - NewLength)) return BPLIB_BUF_LEN_ERROR;
    BPLib_CT_BundleSeqCollection_t Updated = *Collection;
    memset(Updated.SeqRange, 0, sizeof(Updated.SeqRange));
    Updated.FirstSeqNum = Runs[0].First;
    Updated.LastSeqNumAdded = Runs[Merged - 1].Last;
    Updated.SeqRangeLen = NewLength;
    for (size_t i = 0; i < Merged; ++i)
    {
        Updated.SeqRange[i * 2] = Runs[i].Last - Runs[i].First + 1;
        if (i + 1 < Merged) Updated.SeqRange[i * 2 + 1] = Runs[i + 1].First - Runs[i].Last - 1;
    }
    *CcsSize = NewLength >= OldLength ? *CcsSize + NewLength - OldLength : *CcsSize - (OldLength - NewLength);
    *Collection = Updated;
    return BPLIB_SUCCESS;
}

/** Insert an earlier sequence into the selected disposition collection using the same bounded
 * merge as in-order sequences. */
BPLib_Status_t BPLib_CT_InsertOldSeqNumToOpenCcs(BPLib_CT_BundleSeqCollection_t *Collection,
                                            uint64_t BundleSeqNum, size_t *CcsSizePtr)
{
    if (Collection == NULL || CcsSizePtr == NULL) return BPLIB_NULL_PTR_ERROR;
    return BPLib_CT_InsertSequence(Collection, BundleSeqNum, CcsSizePtr);
}

/** Add one custody disposition to its open signal, splitting or flushing the signal when the
 * configured size or range limit is reached. */
BPLib_Status_t BPLib_CT_AddToOpenCcs(BPLib_Instance_t *Instance, size_t OpenCcsIdx,
                                    uint32_t ContactId, BPLib_CustodyBlockData_t *CtebPtr,
                                    BPLib_CT_DispositionCode_t DispositionCode)
{
    if (Instance == NULL || CtebPtr == NULL) return BPLIB_NULL_PTR_ERROR;
    if (OpenCcsIdx >= BPLIB_CT_MAX_OPEN_CCS) return BPLIB_BUF_LEN_ERROR;
    if (ContactId >= BPLIB_MAX_NUM_CONTACTS) return BPLIB_INVALID_CONT_ID_ERR;
    if (DispositionCode != BPLib_CT_CustodyAccepted && DispositionCode != BPLib_CT_CustodyRefused)
        return BPLIB_INVALID_CONFIG_ERR;
    size_t Index = DispositionCode == BPLib_CT_CustodyAccepted ? 0 : 1;
    BPLib_CT_OpenCcs_t *Open = &Instance->Ct.OpenCcss[OpenCcsIdx];
    bool Occupied = false;
    for (size_t i = 0; i < BPLIB_CT_MAX_SEQ_COLLECTIONS; ++i)
    {
        if (Open->BundleSeqCollections[i].SeqRangeLen != 0) Occupied = true;
    }
    if (Occupied && (Open->BundleSeqCollections[0].SeqId != CtebPtr->BundleSeqId ||
        Open->ContactId != ContactId || !BPLib_EID_IsMatch(&Open->SourceAdminEid, &CtebPtr->BlockSrcAdminEID)))
    {
        BPLib_CT_BuildAndSendOpenCcs_Impl(Instance, Open);
        Occupied = false;
    }
    if (!Occupied)
    {
        BPLib_CT_ResetOpenCcs(Open);
        Open->InProgress = true;
        Open->Size = BPLIB_MINIMUM_ENCODED_CCS_LEN;
        Open->SourceAdminEid = CtebPtr->BlockSrcAdminEID;
        Open->BundleSeqCollections[0].SeqId = CtebPtr->BundleSeqId;
        Open->CollectionStartTime = BPLib_TIME_GetMonotonicTime();
        Open->ContactId = ContactId;
        Open->MaxSize = Instance->ContCtxt[ContactId].Config.CSSizeTrigger;
        Open->MaxTime = Instance->ContCtxt[ContactId].Config.CSTimeTrigger;
    }
    BPLib_CT_BundleSeqCollection_t *Collection = &Open->BundleSeqCollections[Index];
    BPLib_Status_t Status = BPLib_CT_InsertSequence(Collection, CtebPtr->BundleSeqNum, &Open->Size);
    if (Status == BPLIB_BUF_LEN_ERROR)
    {
        /* Preserve routing/sequence identity across the reset and accept the
         * triggering acknowledgement into the next signal instead of dropping it. */
        BPLib_CT_BuildAndSendOpenCcs_Impl(Instance, Open);
        return BPLib_CT_AddToOpenCcs(Instance, OpenCcsIdx, ContactId, CtebPtr, DispositionCode);
    }
    if (Status != BPLIB_SUCCESS) return Status;
    Collection->SeqId = CtebPtr->BundleSeqId;
    Collection->DispositionCode = DispositionCode;
    Open->BundlesInCcs++;
    if (Open->Size >= Open->MaxSize || Collection->SeqRangeLen >= BPLIB_CT_MAX_SEQ_RANGE_LEN)
        BPLib_CT_BuildAndSendOpenCcs_Impl(Instance, Open);
    return BPLIB_SUCCESS;
}

/** Find an open signal for this remote administrative EID and sequence ID.
 * Reuse a free slot if there is no match; if all slots are occupied, send
 * the largest signal before reusing its slot. */
size_t BPLib_CT_GetOpenCcsIdx(BPLib_Instance_t* Instance, BPLib_EID_t *SourceAdminEID,
                                uint64_t SequenceId)
{
    size_t              OpenCcsIdx;
    size_t              FirstUnusedCcs = BPLIB_CT_MAX_OPEN_CCS;
    size_t              MaxCcsSize     = 0;
    size_t              LargestCcsIdx  = 0;
    size_t              RetCcsIdx;
    BPLib_CT_Context_t* Context;

    if (Instance == NULL || SourceAdminEID == NULL) return BPLIB_CT_MAX_OPEN_CCS;
    Context = &(Instance->Ct);

    for (OpenCcsIdx = 0; OpenCcsIdx < BPLIB_CT_MAX_OPEN_CCS; OpenCcsIdx++)
    {
        /* See if there's already an in progress CCS with the right EID */
        if (Context->OpenCcss[OpenCcsIdx].InProgress == true &&
            Context->OpenCcss[OpenCcsIdx].BundleSeqCollections[0].SeqId == SequenceId &&
            BPLib_EID_IsMatch(&(Context->OpenCcss[OpenCcsIdx].SourceAdminEid), SourceAdminEID))
        {
            break;
        }
        /* Find the first unused CCS */
        else if (FirstUnusedCcs == BPLIB_CT_MAX_OPEN_CCS &&
                 Context->OpenCcss[OpenCcsIdx].InProgress == false)
        {
            FirstUnusedCcs = OpenCcsIdx;
        }
        /* Find the largest CCS */
        else if (MaxCcsSize < Context->OpenCcss[OpenCcsIdx].Size)
        {
            MaxCcsSize = Context->OpenCcss[OpenCcsIdx].Size;
            LargestCcsIdx = OpenCcsIdx;
        }
    }

    /* Found an in progress raw CCS with a matching EID */
    if (OpenCcsIdx < BPLIB_CT_MAX_OPEN_CCS)
    {
        RetCcsIdx = OpenCcsIdx;
    }
    /* Found an unused CCS */
    else if (FirstUnusedCcs != BPLIB_CT_MAX_OPEN_CCS)
    {
        Context->OpenCcss[FirstUnusedCcs].InProgress = true;
        Context->OpenCcss[FirstUnusedCcs].Size = BPLIB_MINIMUM_ENCODED_CCS_LEN;
        Context->OpenCcss[FirstUnusedCcs].BundleSeqCollections[0].SeqId = SequenceId;
        BPLib_EID_CopyEids(&(Context->OpenCcss[FirstUnusedCcs].SourceAdminEid), *SourceAdminEID);

        RetCcsIdx = FirstUnusedCcs;
    }
    /* No CCSs were available, send the largest one and wipe it to use */
    else
    {
        BPLib_CT_BuildAndSendOpenCcs_Impl(Instance, &(Context->OpenCcss[LargestCcsIdx]));
        RetCcsIdx = LargestCcsIdx;

        Context->OpenCcss[RetCcsIdx].InProgress = true;
        Context->OpenCcss[RetCcsIdx].Size = BPLIB_MINIMUM_ENCODED_CCS_LEN;
        Context->OpenCcss[RetCcsIdx].BundleSeqCollections[0].SeqId = SequenceId;
        BPLib_EID_CopyEids(&(Context->OpenCcss[RetCcsIdx].SourceAdminEid), *SourceAdminEID);
    }

    return RetCcsIdx;
}

void BPLib_CT_BuildAndSendOpenCcs_Impl(BPLib_Instance_t* Instance, BPLib_CT_OpenCcs_t* OpenCcs)
{
    /* Have ARP build CCS and send the open CCS */
    BPLib_ARP_ProcessInProgressCcs(Instance, OpenCcs);
    BPLib_CT_ResetOpenCcs(OpenCcs);

    return;
}

/** Walk the bounded included and excluded ranges and queue storage actions. Drop the custody
 * mutex around storage calls, then look up an accepted entry again before changing its state. */
BPLib_Status_t BPLib_CT_ProcessBundleSeqCollection(BPLib_Instance_t *Inst,
                                                    BPLib_CT_BundleSeqCollection_t *SeqCollection)
{
    size_t SeqRangeIdx;
    uint64_t CurrSeqNum;
    uint64_t NextSeqNum;
    BPLib_Status_t TempStatus = BPLIB_SUCCESS;
    BPLib_Status_t Status = BPLIB_SUCCESS;
    BPLib_CT_DbEntry_t *DbEntry = NULL;
    bool NotFoundErr = false;
    uint32_t BundleId;

    if (Inst == NULL || SeqCollection == NULL) return BPLIB_NULL_PTR_ERROR;
    if (SeqCollection->SeqRangeLen > BPLIB_CT_MAX_SEQ_RANGE_LEN ||
        SeqCollection->DispositionCode == 0) return BPLIB_STOR_PARAM_ERR;
    uint64_t Work = 0;
    for (size_t i = 0; i < SeqCollection->SeqRangeLen; ++i)
    {
        uint64_t Length = SeqCollection->SeqRange[i];
        if (Length > BPLIB_CT_MAX_CCS_SEQUENCE_SPAN - Work)
            return BPLIB_STOR_PARAM_ERR;
        Work += Length;
    }
    if (Work != 0 && Work - 1 > UINT64_MAX - SeqCollection->FirstSeqNum)
        return BPLIB_STOR_PARAM_ERR;

    CurrSeqNum = SeqCollection->FirstSeqNum;

    /* 
    ** Normally we'd lock CT outside of this function but taking both the CT and STOR
    ** locks at the same time introduces the potential for deadlocks. Each thread should
    ** only ever have one of each lock at a time.
    */
    pthread_mutex_lock(&Inst->Ct.Lock);

    for (SeqRangeIdx = 0; SeqRangeIdx < SeqCollection->SeqRangeLen; SeqRangeIdx++)
    {
        for (uint64_t Offset = 0; Offset < SeqCollection->SeqRange[SeqRangeIdx]; ++Offset)
        {
            NextSeqNum = CurrSeqNum + Offset;
            DbEntry = NULL;
            TempStatus = BPLib_CT_GetEntryFromCtdbWithSeq(&Inst->Ct, SeqCollection->SeqId,
                                                            NextSeqNum, &DbEntry);
            if (TempStatus != BPLIB_SUCCESS || DbEntry == NULL)
            {
                /* Excluded sequences can be ignored */
                if (SeqRangeIdx % 2 != 0)
                {
                    TempStatus = BPLIB_SUCCESS;
                }
                else
                {
                    NotFoundErr = true;
                    Status = TempStatus == BPLIB_SUCCESS ? BPLIB_NOT_FOUND_ERR : TempStatus;
                }
            }
            else if (DbEntry->State == BPLib_CT_Transferred)
            {
                /* Duplicate custody signal detected, ignore it */
            }
            /* Even sequence range numbers indicate sequences that are *included* */
            else if (SeqRangeIdx % 2 == 0)
            {
                BundleId = DbEntry->BundleId;

                /* Positive disposition code indicates custody was accepted */
                if (SeqCollection->DispositionCode > 0)
                {
                    /* Storage must own the request before custody is transferred. */
                    pthread_mutex_unlock(&Inst->Ct.Lock);
                    Status = BPLib_STOR_AddToCustodialUpdateBatch(Inst, BundleId, BPLIB_CT_MARK_DELETE);
                    pthread_mutex_lock(&Inst->Ct.Lock);
                    if (Status != BPLIB_SUCCESS) goto finish;
                    /* The CT entry may have changed while its lock was released. */
                    TempStatus = BPLib_CT_GetEntryFromCtdbWithSeq(&Inst->Ct, SeqCollection->SeqId,
                                                               NextSeqNum, &DbEntry);
                    if (TempStatus == BPLIB_SUCCESS && DbEntry != NULL)
                    {
                        if (DbEntry->State != BPLib_CT_Initialized && DbEntry->State != BPLib_CT_Transferred)
                            Inst->Ct.BundleCountInCustody--;
                        DbEntry->State = BPLib_CT_Transferred;
                    }

                    BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_CUSTODY_TRANSFERRED, 1);
                }
                /* Negative disposition code indicates custody was rejected */
                else
                {
                    /* Request storage turn retransmit timer off */
                    pthread_mutex_unlock(&Inst->Ct.Lock);
                    Status = BPLib_STOR_AddToCustodialUpdateBatch(Inst, BundleId, BPLIB_CT_STOP_RETRANSMIT);
                    pthread_mutex_lock(&Inst->Ct.Lock);

                    if (Status != BPLIB_SUCCESS) goto finish;

                    BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_CUSTODY_REJECTED, 1);
                }
            }
            /* Odd sequence range numbers indicate sequences that are *excluded* */
            else
            {
                BundleId = DbEntry->BundleId;

                /* Request bundle retransmission from storage */
                pthread_mutex_unlock(&Inst->Ct.Lock);
                Status = BPLib_STOR_AddToCustodialUpdateBatch(Inst, BundleId, BPLIB_CT_START_RETRANSMIT);
                pthread_mutex_lock(&Inst->Ct.Lock);
                if (Status != BPLIB_SUCCESS) goto finish;
            }
        }

        if (NotFoundErr)
        {
            BPLib_EM_SendEvent(BPLIB_CT_INV_SEQ_NUM_DBG_EID, BPLib_EM_EventType_DEBUG,
                    "Error, at least one bundle with sequence ID %ld in sequence number range [%ld-%ld] could not be found in the CTDB.",
                    SeqCollection->SeqId, CurrSeqNum, 
                    CurrSeqNum + SeqCollection->SeqRange[SeqRangeIdx] - 1);

            NotFoundErr = false;
        }

        /* Additional operations on include ranges */
        if (SeqRangeIdx % 2 == 0)
        {
            BPLib_AS_Increment(BPLIB_EID_INSTANCE, BUNDLE_COUNT_RECEIVED_CUSTODY_SIGNAL, SeqCollection->SeqRange[SeqRangeIdx]);

            if (SeqCollection->DispositionCode < 0)
            {
                BPLib_EM_SendEvent(BPLIB_CT_REJECTED_DEBG_EID, BPLib_EM_EventType_DEBUG,
                    "Bundles with sequence ID %lu and sequence numbers [%lu-%lu] were rejected by downstream node",
                    SeqCollection->SeqId, CurrSeqNum,
                    (CurrSeqNum + SeqCollection->SeqRange[SeqRangeIdx]) - 1);

            }
        }

        CurrSeqNum += SeqCollection->SeqRange[SeqRangeIdx];
    }

finish:
    pthread_mutex_unlock(&Inst->Ct.Lock);

    /* Do remaining batch of storage operations */
    BPLib_STOR_UpdateCustodialBundles(Inst);

    return Status;
}