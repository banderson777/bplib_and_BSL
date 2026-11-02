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

#ifndef BPLIB_STOR_SQL_LOAD_H
#define BPLIB_STOR_SQL_LOAD_H

/* ======== */
/* Includes */
/* ======== */

#include "bplib_stor_sql.h"

/* Statement and generated query ownership is local to each invocation.
 * Callers must continue to serialize access to each instance/connection. */

/* =================== */
/* Function Prototypes */
/* =================== */

BPLib_Status_t BPLib_SQL_FindForEIDs(BPLib_Instance_t* Inst, BPLib_STOR_LoadBatch_t* Batch,
                        BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs, bool LocalDelivery);

SQL_Status_t BPLib_SQL_FindForEIDsImpl(BPLib_Instance_t* Inst, BPLib_STOR_LoadBatch_t* Batch,
                                        BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs,
                                        size_t MaxBundles, bool LocalDelivery, sqlite3_stmt *FindForEgressIDStmt);

BPLib_Status_t BPLib_SQL_MarkBatchEgressed(BPLib_Instance_t* Inst, 
                            BPLib_STOR_LoadBatch_t* Batch, bool LocalDelivery);

SQL_Status_t BPLib_SQL_MarkBatchEgressedImpl(BPLib_Instance_t* Inst, 
                                    BPLib_STOR_LoadBatch_t* Batch, bool LocalDelivery,
                                    sqlite3_stmt *MarkEgressedStmt, sqlite3_stmt *ResetRetransmitStmt);

BPLib_Status_t BPLib_SQL_LoadBundle(BPLib_Instance_t* Inst, int64_t BundleRowID,
                                    BPLib_Bundle_t** Bundle);

SQL_Status_t BPLib_SQL_LoadBundleImpl(BPLib_Instance_t* Inst, int64_t BundleRowID,
                                        BPLib_Bundle_t** Bundle, sqlite3_stmt *FindBlobStmt);

#endif