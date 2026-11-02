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

#ifndef BPLIB_CT_H
#define BPLIB_CT_H

/*
** Include
*/

#include "bplib_api_types.h"
#include "bplib_rbt.h"
#include "bplib_bblocks.h"
#include <pthread.h>
#include "bplib_cfg.h"

/*
** Macros
*/

/**
 * \brief Length of unique bundle identifier array in \ref BPLib_CT_SetBundleId
 */
#define BPLIB_CT_BUNDLE_IDENTIFIER_ARRAY_LEN        (7u)

/**
 * \brief Maximum supported sequence range length. Must always be an odd number
 */
#define BPLIB_CT_MAX_SEQ_RANGE_LEN                  (11u)

/**
 * \brief Reserved unassigned sequence ID, skipped when allocating a fresh stream
 */
#define BPLIB_CT_SEQ_ID_ROLLOVER_VALUE              (100u)

/**
 * \brief Maximum number of open CCSs at any given time
 */
#define BPLIB_CT_MAX_OPEN_CCS                        (10u)

/**
 * \brief Maximum number of bundle sequence collections in a locally built CCS.
 *        Only two disposition codes are currently supported at this point so no
 *        more than two bundle sequence collections are needed.
 */
#define BPLIB_CT_MAX_SEQ_COLLECTIONS                (2u)

/**
 * \brief Maximum number of bundle sequence collections allowed in a CCS received from
 *        another node.
 */
#define BPLIB_CT_MAX_RECVD_SEQ_COLLECTIONS          (5u)

/**
 * \brief Size of a full CCS batch operation
 */
#define BPLIB_CT_BATCH_SIZE                         (100u)

/** Maximum sequence positions processed per incoming collection, including gaps.
 * Reject larger collections before any changes. Missions may override the bound;
 * peers must split larger spans to remain within the receiving mission's limit. */
#ifndef BPLIB_CT_MAX_CCS_SEQUENCE_SPAN
#define BPLIB_CT_MAX_CCS_SEQUENCE_SPAN BPLIB_CT_DB_MAX_ENTRIES
#endif
#if BPLIB_CT_MAX_CCS_SEQUENCE_SPAN < 1 || BPLIB_CT_MAX_CCS_SEQUENCE_SPAN > UINT32_MAX
#error "BPLIB_CT_MAX_CCS_SEQUENCE_SPAN must be a positive 32-bit value"
#endif

/**
 * \brief Minimum length of an encoded Compressed Custody Signal bundle in bytes
 * 
 * This is the length of a CCS acknowledging custody of a single bundle, with an age 
 * block added in case time is invalid. TODO VERIFY ME
 */
#define BPLIB_MINIMUM_ENCODED_CCS_LEN               (63u)

/**
 * \brief Maximum percent of CTDB entries that can be not in custody before a garbage
 *        collection cycle is triggered
 */
#define BPLIB_CT_DB_MAX_PERCENT_NONCUSTODIAL_ENTRIES    (90u)

/*
** Type Definitions
*/

/**
 * \brief Possible operations to be taken upon receiving and processing a CCS
 */
typedef enum
{
    BPLIB_CT_STOP_RETRANSMIT,           /** \brief Stop storage retransmission timer  */
    BPLIB_CT_MARK_DELETE,               /** \brief Mark bundle for deletion */
    BPLIB_CT_START_RETRANSMIT           /** \brief Trigger bundle retransmission */
} BPLib_CT_StorOp_t;

/**
 * \brief Disposition codes relating custody information of a bundle sequence collection
 *        in a CCS. A positive disposition code indicates custody acceptance, a negative
 *        disposition code indicates custody refusal.
 */
typedef enum
{
    BPLib_CT_CustodyAccepted     = 1,
    BPLib_CT_CustodyRefused      = -1,
    BPLib_CT_FirstAcceptDispCode = BPLib_CT_CustodyAccepted,
    BPLib_CT_LastRefuseDispCode  = BPLib_CT_CustodyRefused,
} BPLib_CT_DispositionCode_t;

/**
 * \brief Mapping of a disposition code to its index in the bundle sequence collections of
 *        the open CCSs.
 */
typedef enum
{
    BPLib_CT_CustodyAccepted_Idx = 0,
    BPLib_CT_CustodyRefused_Idx = 1
} BPLib_CT_SeqCollectionIdx_t;

/**
 * \brief The state of a bundle's CTDB entry
 */
typedef enum
{
    BPLib_CT_Initialized = 0,           /** \brief A CTDB entry has been created but the bundle is not in custody yet */
    BPLib_CT_InCustody = 1,             /** \brief The bundle was successfully stored and is in custody */
    BPLib_CT_Transmitted = 2,           /** \brief The bundle has been transmitted to the next node */
    BPLib_CT_Retransmitted = 3,         /** \brief The bundle has been retransmitted at least once */
    BPLib_CT_Transferred = 4,           /** \brief The bundle's custody has been transferred and it is marked for deletion */
    BPLib_CT_Delivered  = 5,            /** \brief The bundle was delivered locally */
} BPLib_CT_DbEntryState_t;

/**
 * \brief A bundle sequence collection and relevant information for building one.
 */
typedef struct
{
    uint64_t SeqId;
    uint64_t FirstSeqNum;
    uint64_t SeqRange[BPLIB_CT_MAX_SEQ_RANGE_LEN];

    /* Additional information */
    size_t   SeqRangeLen;
    uint64_t LastSeqNumAdded;
    BPLib_CT_DispositionCode_t DispositionCode;
} BPLib_CT_BundleSeqCollection_t;

/**
 * \brief The raw data needed to build a new CCS administrative record. 
 */
typedef struct
{
    bool                           InProgress;
    size_t                         Size;
    size_t                         MaxSize;
    size_t                         BundlesInCcs;
    int64_t                        MaxTime;
    BPLib_EID_t                    SourceAdminEid;
    BPLib_CT_BundleSeqCollection_t BundleSeqCollections[BPLIB_CT_MAX_SEQ_COLLECTIONS];
    int64_t                        CollectionStartTime;
    uint32_t                       ContactId;
} BPLib_CT_OpenCcs_t;

/**
 * \brief The deserialized administrative record data from a received CCS.
 */
typedef struct
{
    BPLib_EID_t                    SourceAdminEid;
    BPLib_CT_BundleSeqCollection_t BundleSeqCollections[BPLIB_CT_MAX_RECVD_SEQ_COLLECTIONS];
    size_t                         NumBundleSeqCollections;
} BPLib_CT_DeserializedCcs_t;

/**
 * \brief An entry in the Custody Transfer Database (CTDB)
 */
typedef struct
{
    BPLib_RBT_Link_t SeqRbtLink;        /** \brief RBT link to search based on sequence ID/number */
    BPLib_RBT_Link_t IdRbtLink;         /** \brief RBT link to search based on bundle ID */
    uint64_t SeqId;                     /** \brief Sequence ID */
    uint64_t SeqNum;                    /** \brief Sequence number */
    uint32_t BundleId;                  /** \brief Unique bundle ID */
    BPLib_CT_DbEntryState_t State;      /** \brief The custodial state of this bundle */
} BPLib_CT_DbEntry_t;

/**
 * \brief Sequence counter
 */
typedef struct
{
    uint64_t Id;
    uint64_t Counter;
} BPLib_SeqCounter_t;

/**
 * \brief The context information for performing custody transfer
 */
typedef struct 
{
    BPLib_CT_OpenCcs_t OpenCcss[BPLIB_CT_MAX_OPEN_CCS];         /** \brief All CCSs that are currently being constructed */

    BPLib_SeqCounter_t SeqCounters[BPLIB_MAX_NUM_CONTACTS];     /** \brief Currently active sequence counters */
    uint64_t LastSeqCounterId;                                  /** \brief Last sequence counter ID assigned to a contact */
    /** Optional durable allocator, installed before contacts start. Called under
     * Lock; return a never-reused, nonzero ID other than the unassigned sentinel.
     * NULL retains the default volatile sequence allocation behavior. */
    BPLib_Status_t (*AllocateSequenceId)(uint64_t *Id);

    size_t CurrDbSize;                                          /** \brief Number of entries in CTDB */
    size_t BundleCountInCustody;                                /** \brief Number of bundles in custody. Bundles may be in the CTDB but until they are stored they are not actually in custody */
    BPLib_RBT_Root_t SeqTreeRoot;                               /** \brief An RBT that allows for CTDB queries based on sequence ID/number */
    BPLib_RBT_Root_t IdTreeRoot;                                /** \brief An RBT that allows for CTDB queries based on bundle ID */

    pthread_mutex_t Lock;                                     /** \brief Read/write lock on CTDB and open CCSs */
} BPLib_CT_Context_t;

/*
** Exported Functions
*/

/**
 * \brief Initialize custody transfer
 *
 *  \par Description
 *       Initializes custody transfer context to an empty starting state
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Initialization was successful
 *  \retval BPLIB_NULL_PTR_ERR The instance was null
 */
BPLib_Status_t BPLib_CT_Init(BPLib_Instance_t *Inst);

/**
 * \brief Set bundle ID
 *
 *  \par Description
 *       Calculate and set a bundle ID for the provided bundle. The bundle ID is calculated
 *       with a CRC-32C calculation done over the bundle's unique identifiers: source
 *       EID, creation time, and sequence number.
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Bundle Pointer to the bundle
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 *  \retval BPLIB_NULL_PTR_ERR The bundle was null
 */
BPLib_Status_t BPLib_CT_SetBundleId(BPLib_Bundle_t *Bundle);

/**
 * \brief Process a new bundle
 *
 *  \par Description
 *       Sets the bundle ID and if a bundle is custodial, check if PDB can accept
 *       custody, if its a duplicate, and if there's storage space available for it. If 
 *       the bundle is a duplicate, accept it but return an error to mark it for
 *       deletion. If the bundle is rejected, add it to a custody refused CCS. If the
 *       bundle is not a duplicate and it is accepted, add it to the CTDB. It will
 *       not be added to a custody accepted CCS until the bundle is successfully
 *       stored.
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Bundle Pointer to the bundle
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 *  \retval BPLIB_NO_STOR_ERR Bundle could not be accepted due to a lack of storage
 *  \retval BPLIB_CT_CUSTODY_REFUSED_ERR Bundle custody could not be accepted
 *  \retval BPLIB_CT_DUPLICATE_ERR Bundle is a duplicate of an existing custodial bundle
 */
BPLib_Status_t BPLib_CT_ProcessNewBundle(BPLib_Instance_t* Inst, BPLib_Bundle_t *Bundle);

/**
 * \brief Signal custody
 *
 *  \par Description
 *       If a bundle is custodial and was not created locally, add it to its 
 *       corresponding CCS. Otherwise, do nothing. If this is a custody rejection signal, 
 *       it is also removed from the CTBD (if it is in there). All relevant counters are 
 *       incremented.
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Bundle Pointer to the bundle
 *  \param[in] Inst Pointer to the BPLib instance
 *  \param[in] DispCode Disposition code of the custody signal
 *  \param[in] StoreBundle Whether or not the bundle will be stored
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 *  \retval BPLIB_CT_CUSTODY_REFUSED_ERR Bundle custody could not be accepted
 */
BPLib_Status_t BPLib_CT_SignalCustody(BPLib_Instance_t *Inst, BPLib_Bundle_t *Bundle,
                                    BPLib_CT_DispositionCode_t DispCode, bool StoreBundle);

/**
 * \brief Update a bundle on egress
 *
 *  \par Description
 *       If an egressing bundle has a CTEB, give it a new sequence number, update its CTEB
 *       fields, and add it to the CTDB.
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Bundle Pointer to the bundle
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_CT_UpdateBundle(BPLib_Instance_t* Inst, BPLib_Bundle_t *Bundle);

/**
 * \brief Process a new CCS
 *
 *  \par Description
 *       Process a new CCS by validating it, removing any included sequence numbers from
 *       the CTDB and requesting Storage delete them, requesting Storage retransmit any
 *       missing sequence numbers, and incrementing all relevant counters.
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] CCS Pointer to the received CCS
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_CT_ProcessCcs(BPLib_Instance_t *Inst, BPLib_CT_DeserializedCcs_t *Ccs);

/**
 * \brief Assign a new sequence counter
 *
 *  \par Description
 *       Assign a new sequence identifier and reset its counter to zero. The
 *       default allocator skips reserved identifier 100 and fails on exhaustion.
 *       Missions requiring uniqueness across restart must install a durable
 *       allocator callback. Allocation failure leaves contact state unchanged.
 *
 *  \par Assumptions, External Events, and Notes:
 *       This is done by CLA on the contact-start directive
 * 
 *  \param[in] ContactId ID of the contact requesting a new sequence counter
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_CT_AssignSeqCounter(BPLib_Instance_t *Inst, uint32_t ContactId);

/**
 * \brief Delete bundle from CTDB
 *
 *  \par Description 
 *       Search for a bundle ID in the CTDB and if a bundle is found, delete it from the 
 *       CTDB. This issues an error event if the bundle cannot be found in the CTDB
 *
 *  \par Assumptions, External Events, and Notes:
 *       The BundleCountInCustody counter that CT tracks decrements after a successful
 *       CTDB removal. 
 * 
 *  \param[in] Inst Pointer to the BPLib instance
 *  \param[in] BundleId Unique bundle identifier used by CTDB
 *
 *  \return void
 */
void BPLib_CT_DeleteBundleFromCtdb(BPLib_Instance_t *Inst, uint32_t BundleId);

/**
 * \brief     Wrapper to make BPLib_CT_BuildAndSendOpenCcs_Impl publicly callable
 * \param[in] Instance Abstraction of the node that will be used for putting the
 *                     bundle with a CCS in the payload on the job queue. Instance
 *                     also contains the memory pool used to create the bundle
 * \param[in] OpenCcs  An open CCS that has reached a configured limit
 * \return    void
 */
void BPLib_CT_BuildAndSendOpenCcs(BPLib_Instance_t* Instance, BPLib_CT_OpenCcs_t* OpenCcs);

/**
 * \brief Check CCS timeout triggers
 *
 *  \par Description
 *       Iterate through all open CCSs and check if the maximum time trigger has been 
 *       reached. If so, trigger the generation of that CCS
 * 
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return void
 */
void BPLib_CT_CheckCcsTimeout(BPLib_Instance_t* Instance);

/**
 * \brief Complete delivery of custodial bundle
 *
 *  \par Description
 *       To complete the delivery of a custodial bundle, mark it as transferred in the CTDB
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Bundle Pointer to the bundle
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return Execution status
 *  \retval BPLIB_SUCCESS Operation was successful
 */
BPLib_Status_t BPLib_CT_CompleteDelivery(BPLib_Instance_t *Inst, BPLib_Bundle_t *Bundle);

/**
 * \brief Whether to trigger garbage collection of custodial memory
 *
 *  \par Description
 *       To prevent old CTDB entries from holding onto system memory for forever,
 *       this checks if the percent of CTDB entries that are no longer in custody is
 *       greater than the maximum \ref BPLIB_CT_DB_MAX_PERCENT_NONCUSTODIAL_ENTRIES.
 *       This function is intended to be used by garbage collection functions to determine
 *       when garbage collection should be run to free up memory in both the CTDB and in 
 *       storage. Note that bundles that have been received but not stored will show up 
 *       as not custodial, but CTDB entries are in this state for a very short period of 
 *       time
 *
 *  \par Assumptions, External Events, and Notes:
 *       None
 * 
 *  \param[in] Inst Pointer to the BPLib instance
 *
 *  \return True if custodial garbage collection should be triggered, else false
 */
bool BPLib_CT_TriggerCustodialGarbageCollection(BPLib_Instance_t *Inst);

#endif /* BPLIB_CT_H */
