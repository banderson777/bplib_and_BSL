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

#ifndef BPLIB_STOR_SQL_CUST_H
#define BPLIB_STOR_SQL_CUST_H

/* ======== */
/* Includes */
/* ======== */

#include "bplib_stor.h"
#include "bplib_stor_sql.h"

/* Each invocation owns its statements and query text. Callers serialize a
 * connection; successful counters are published only after transaction commit. */
typedef struct
{
    sqlite3_stmt *Start;
    sqlite3_stmt *Stop;
    sqlite3_stmt *Delete;
} BPLib_SQL_CustodyStatements_t;

BPLib_Status_t BPLib_SQL_SetNewRetransmitTrigger(BPLib_Instance_t *Inst, uint32_t ContactId,
                BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs, size_t RetransmitTrigger, size_t *NumUpdated);

SQL_Status_t BPLib_SQL_SetNewRetransmitTriggerImpl(sqlite3* db, BPLib_EID_Pattern_t* DestEIDs, 
                            size_t NumEIDs, size_t RetransmitTrigger, size_t *NumUpdated, sqlite3_stmt *SetRetransmitAllStmt);

BPLib_Status_t BPLib_SQL_UpdateCustodialBundles(BPLib_Instance_t *Inst, BPLib_STOR_CtUpdateBatch_t *Batch);

SQL_Status_t BPLib_SQL_UpdateCustodialBundlesImpl(sqlite3* db, BPLib_STOR_CtUpdateBatch_t *Batch, size_t *NumEgressed, BPLib_SQL_CustodyStatements_t *Statements);

#endif
