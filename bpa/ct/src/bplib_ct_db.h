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

#ifndef BPLIB_CT_DB_H
#define BPLIB_CT_DB_H

/*
** Include
*/

#include "bplib_api_types.h"
#include "bplib_mem.h"


/*
** Exported Functions
*/

/**
 * \brief Get pointer to the CTDB entry from the RBT link
 *
 *  \par Description
 *       Given a pointer to a red-black tree link, return a pointer to the CTDB entry
 *       that the link corresponds to. The link should be for the *sequence ID/number* RBT
 *       specifically.
 *
 *  \par Assumptions, External Events, and Notes:
 *       Somewhat sketchy pointer logic but since the CTDB is the only implementation
 *       of a RBT in the code, the assumption that there's an associated CTDB entry
 *       for the provided RBT link is safe.
 * 
 *  \param[in] Node Pointer to a sequence ID/number red-black tree link
 *
 *  \return Pointer to a CTDB entry
 */
BPLib_CT_DbEntry_t *BPLib_CT_GetDbEntryFromSeqRbt(const BPLib_RBT_Link_t *Node);


/**
 * \brief Get pointer to the CTDB entry from the RBT link
 *
 *  \par Description
 *       Given a pointer to a red-black tree link, return a pointer to the CTDB entry
 *       that the link corresponds to. The link should be for the bundle ID RBT
 *       specifically.
 *
 *  \par Assumptions, External Events, and Notes:
 *       Somewhat sketchy pointer logic but since the CTDB is the only implementation
 *       of a RBT in the code, the assumption that there's an associated CTDB entry
 *       for the provided RBT link is safe.
 * 
 *  \param[in] Node Pointer to a bundle ID red-black tree link
 *
 *  \return Pointer to a CTDB entry
 */
BPLib_CT_DbEntry_t *BPLib_CT_GetDbEntryFromIdRbt(const BPLib_RBT_Link_t *Node);

/**
 * \brief Compare some value with a CTDB entry
 *
 *  \par Description
 *       This function is used by RBT as a function pointer for generic search and insert
 *       comparisons in the CTDB. We are currently using sequence IDs as the primary key
 *       for the CTDB RBT, and then if two nodes' keys match, making further comparisons
 *       against their respective sequence numbers.
 *
 *  \par Assumptions, External Events, and Notes:
 *       This function is called as a function pointer by \ref BPLib_RBT_InsertValueGeneric
 *       and \ref BPLib_RBT_SearchGeneric.
 * 
 *  \param[in] Node Pointer to a red-black tree link
 *  \param[in] Arg Pointer to a sequence number to compare against
 *
 *  \retval 0 if this node matches the sequence number (Arg)
 *  \retval 1 if the sequence number (Arg) is greater than the current node's sequence number
 *  \retval -1 if the sequence number (Arg) is less than the current node's sequence number
 */
int BPLib_CT_CompareDbEntries(const BPLib_RBT_Link_t *Node, void *Arg);

/* Index helpers require the caller to hold Ct.Lock (or exclude concurrent use).
 * InitEntry/AddToCtdb publish CurrDbSize only after their required indexes succeed;
 * normal allocation/duplicate failures free the attempted entry. AddToCtdb
 * initializes a transmitted entry in both trees; custody acceptance accounting
 * (BundleCountInCustody) remains the caller's responsibility.
 * UpdateEntry preserves sequence fields/state on insertion failure. Lookups clear
 * their output pointer on failure. These functions do not commit durable SQL.
 */
BPLib_Status_t BPLib_CT_AddToCtdb(BPLib_Instance_t *Inst, uint64_t SeqId, 
                                                    uint64_t SeqNum, uint32_t BundleId);

BPLib_Status_t BPLib_CT_GetEntryFromCtdbWithSeq(BPLib_CT_Context_t *Context, uint64_t SeqId, 
                                            uint64_t SeqNum, BPLib_CT_DbEntry_t **DbEntry);

BPLib_Status_t BPLib_CT_GetEntryFromCtdbWithId(BPLib_CT_Context_t *Context, 
                                        uint32_t BundleId, BPLib_CT_DbEntry_t **DbEntry);

BPLib_Status_t BPLib_CT_RemoveFromCtdb(BPLib_Instance_t *Inst, BPLib_CT_DbEntry_t *DbEntry);

uint64_t BPLib_CT_GetSequenceId(BPLib_CT_Context_t *Context, uint32_t ContactId);

uint64_t BPLib_CT_GetNextSequenceNum(BPLib_CT_Context_t *Context, uint32_t ContactId);

BPLib_Status_t BPLib_CT_InitEntry(BPLib_Instance_t *Inst, uint32_t BundleId);

BPLib_Status_t BPLib_CT_UpdateEntry(BPLib_Instance_t *Inst, BPLib_CT_DbEntry_t *DbEntry, 
                                        uint64_t SeqId, uint64_t SeqNum);

#endif /* BPLIB_CT_DB_H */
