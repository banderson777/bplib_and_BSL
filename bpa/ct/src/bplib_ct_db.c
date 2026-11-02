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
#include "bplib_bblocks.h"
#include "bplib_eid.h"
#include "bplib_mem.h"
#include "bplib_pdb.h"
#include "bplib_inst.h"

/*
** Function Definitions
*/

BPLib_CT_DbEntry_t *BPLib_CT_GetDbEntryFromSeqRbt(const BPLib_RBT_Link_t *Node)
{
    /* Get a pointer to the DB entry */
    return (BPLib_CT_DbEntry_t *)(void *)((uint8_t *) Node - offsetof(BPLib_CT_DbEntry_t, SeqRbtLink));
}

BPLib_CT_DbEntry_t *BPLib_CT_GetDbEntryFromIdRbt(const BPLib_RBT_Link_t *Node)
{
    /* Get a pointer to the DB entry */
    return (BPLib_CT_DbEntry_t *)(void *)((uint8_t *) Node - offsetof(BPLib_CT_DbEntry_t, IdRbtLink));
}


int BPLib_CT_CompareDbEntries(const BPLib_RBT_Link_t *Node, void *Arg)
{
    BPLib_CT_DbEntry_t *DbEntry;
    uint64_t *SeqNum; 

    SeqNum = (uint64_t *) Arg;

    DbEntry = BPLib_CT_GetDbEntryFromSeqRbt(Node);

    if (*SeqNum == DbEntry->SeqNum)
    {
        return 0;
    }
    else if (*SeqNum > DbEntry->SeqNum)
    {
        return 1;
    }

    return -1;
}

BPLib_Status_t BPLib_CT_InitEntry(BPLib_Instance_t *Inst, uint32_t BundleId)
{
    BPLib_MEM_Block_t  *DbMemBlk = NULL;
    BPLib_CT_DbEntry_t *DbEntry  = NULL;

    if (Inst == NULL) return BPLIB_NULL_PTR_ERROR;
    if (Inst->Ct.CurrDbSize >= BPLIB_CT_DB_MAX_ENTRIES)
    {
        return BPLIB_CT_FULL_DB_ERR;
    }

    /* Get a CTDB entry to use */
    DbMemBlk = BPLib_MEM_BlockAlloc(&Inst->pool, BPLIB_MEM_SMALL_BLK_SIZE);
    if (DbMemBlk == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    DbEntry = &(DbMemBlk->user_data.DbEntry);

    memset(DbEntry, 0, sizeof(BPLib_CT_DbEntry_t));
    
    /* Set initial CTDB entry values - the sequence ID/number will be updated later */
    DbEntry->BundleId = BundleId;
    DbEntry->SeqId = BPLIB_CT_SEQ_ID_ROLLOVER_VALUE;
    DbEntry->State = BPLib_CT_Initialized;

    BPLib_Status_t Status = BPLib_RBT_InsertValueUnique(BundleId, &Inst->Ct.IdTreeRoot,
                                                     &DbEntry->IdRbtLink);
    if (Status == BPLIB_SUCCESS) Inst->Ct.CurrDbSize++;
    else BPLib_MEM_BlockFree(&Inst->pool, DbMemBlk);
    return Status;
}

BPLib_Status_t BPLib_CT_UpdateEntry(BPLib_Instance_t *Inst, BPLib_CT_DbEntry_t *DbEntry, 
                                        uint64_t SeqId, uint64_t SeqNum)
{
    if (Inst == NULL || DbEntry == NULL) return BPLIB_NULL_PTR_ERROR;
    uint64_t PreviousId = DbEntry->SeqId, PreviousNumber = DbEntry->SeqNum;
    DbEntry->SeqId = SeqId;
    DbEntry->SeqNum = SeqNum;
    BPLib_Status_t Status = BPLib_RBT_InsertValueGeneric(SeqId, &Inst->Ct.SeqTreeRoot,
                                                       &DbEntry->SeqRbtLink,
                                                       BPLib_CT_CompareDbEntries, &DbEntry->SeqNum);
    if (Status == BPLIB_SUCCESS) DbEntry->State = BPLib_CT_Transmitted;
    else
    {
        /* Failed duplicate insertion leaves the new link detached. An already
         * attached link is rejected without modification by the RBT API. */
        DbEntry->SeqId = PreviousId;
        DbEntry->SeqNum = PreviousNumber;
    }
    return Status;
}


BPLib_Status_t BPLib_CT_AddToCtdb(BPLib_Instance_t *Inst, uint64_t SeqId, 
                                                    uint64_t SeqNum, uint32_t BundleId)
{
    BPLib_MEM_Block_t  *DbMemBlk = NULL;
    BPLib_CT_DbEntry_t *DbEntry  = NULL;
    BPLib_Status_t Status;

    if (Inst == NULL) return BPLIB_NULL_PTR_ERROR;
    if (Inst->Ct.CurrDbSize >= BPLIB_CT_DB_MAX_ENTRIES)
    {
        return BPLIB_CT_FULL_DB_ERR;
    }

    /* Get a CTDB entry to use */
    DbMemBlk = BPLib_MEM_BlockAlloc(&Inst->pool, BPLIB_MEM_SMALL_BLK_SIZE);
    if (DbMemBlk == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    DbEntry = &(DbMemBlk->user_data.DbEntry);
    
    memset(DbEntry, 0, sizeof(*DbEntry));
    DbEntry->BundleId = BundleId;
    DbEntry->SeqId = SeqId;
    DbEntry->SeqNum = SeqNum;
    DbEntry->State = BPLib_CT_Initialized;

    Status = BPLib_RBT_InsertValueUnique(BundleId, &Inst->Ct.IdTreeRoot, &DbEntry->IdRbtLink);
    if (Status == BPLIB_SUCCESS)
    {
        Status = BPLib_RBT_InsertValueGeneric(SeqId, &Inst->Ct.SeqTreeRoot,
                                             &DbEntry->SeqRbtLink,
                                             BPLib_CT_CompareDbEntries, &DbEntry->SeqNum);
        if (Status != BPLIB_SUCCESS)
        {
            BPLib_Status_t Rollback = BPLib_RBT_ExtractNode(&Inst->Ct.IdTreeRoot, &DbEntry->IdRbtLink);
            if (Rollback != BPLIB_SUCCESS)
            {
                /* A corrupt tree must not retain a pointer to freed storage.
                 * Keep this initialized entry accounted for and report failure. */
                Inst->Ct.CurrDbSize++;
                return Rollback;
            }
        }
    }
    if (Status == BPLIB_SUCCESS)
    {
        DbEntry->State = BPLib_CT_Transmitted;
        Inst->Ct.CurrDbSize++;
    }
    else BPLib_MEM_BlockFree(&Inst->pool, DbMemBlk);
    return Status;
}

BPLib_Status_t BPLib_CT_GetEntryFromCtdbWithSeq(BPLib_CT_Context_t *Context, uint64_t SeqId, 
                                            uint64_t SeqNum, BPLib_CT_DbEntry_t **DbEntry)
{
    BPLib_Status_t Status = BPLIB_NOT_FOUND_ERR;
    BPLib_RBT_Link_t *RbtLink;

    if (DbEntry == NULL) return BPLIB_NULL_PTR_ERROR;
    *DbEntry = NULL;
    if (Context == NULL) return BPLIB_NULL_PTR_ERROR;

    RbtLink = BPLib_RBT_SearchGeneric(SeqId, &Context->SeqTreeRoot, 
                                                    BPLib_CT_CompareDbEntries, &SeqNum);

    if (RbtLink != NULL)
    {
        *DbEntry = BPLib_CT_GetDbEntryFromSeqRbt(RbtLink);
        Status = BPLIB_SUCCESS; 
    }

    return Status;
}

BPLib_Status_t BPLib_CT_GetEntryFromCtdbWithId(BPLib_CT_Context_t *Context, 
                                        uint32_t BundleId, BPLib_CT_DbEntry_t **DbEntry)
{
    BPLib_Status_t Status = BPLIB_NOT_FOUND_ERR;
    BPLib_RBT_Link_t *RbtLink;

    if (DbEntry == NULL) return BPLIB_NULL_PTR_ERROR;
    *DbEntry = NULL;
    if (Context == NULL) return BPLIB_NULL_PTR_ERROR;

    RbtLink = BPLib_RBT_SearchUnique(BundleId, &Context->IdTreeRoot);

    if (RbtLink != NULL)
    {
        *DbEntry = BPLib_CT_GetDbEntryFromIdRbt(RbtLink);
        Status = BPLIB_SUCCESS; 
    }

    return Status;
}

BPLib_Status_t BPLib_CT_RemoveFromCtdb(BPLib_Instance_t *Inst, BPLib_CT_DbEntry_t *DbEntry)
{
    BPLib_Status_t Status = BPLIB_SUCCESS;

    if (Inst == NULL || DbEntry == NULL) return BPLIB_NULL_PTR_ERROR;
    /* The sequence ID/num RBT node may or may not be initialized */
    if (DbEntry->State != BPLib_CT_Initialized && DbEntry->State != BPLib_CT_InCustody &&
        DbEntry->State != BPLib_CT_Delivered)
    {
        Status = BPLib_RBT_ExtractNode(&(Inst->Ct.SeqTreeRoot), &DbEntry->SeqRbtLink);
    }

    if (Status == BPLIB_SUCCESS)
    {
        Status = BPLib_RBT_ExtractNode(&(Inst->Ct.IdTreeRoot), &DbEntry->IdRbtLink);
        if (Status == BPLIB_SUCCESS)
        {
            if (DbEntry->State == BPLib_CT_InCustody || DbEntry->State == BPLib_CT_Transmitted ||
                DbEntry->State == BPLib_CT_Retransmitted)
            {
                Inst->Ct.BundleCountInCustody--;
            }

            Inst->Ct.CurrDbSize--;
            BPLib_MEM_BlockFree(&Inst->pool, BPLib_MEM_GetBlockFromUserData(DbEntry));
        }
    }

    return Status;
}

uint64_t BPLib_CT_GetSequenceId(BPLib_CT_Context_t *Context, uint32_t ContactId)
{
    return Context->SeqCounters[ContactId].Id;
}

uint64_t BPLib_CT_GetNextSequenceNum(BPLib_CT_Context_t *Context, uint32_t ContactId)
{
    return Context->SeqCounters[ContactId].Counter++;
}
