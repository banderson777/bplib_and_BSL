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
#ifndef BPLIB_STOR_SQL_H
#define BPLIB_STOR_SQL_H

/* ======== */
/* Includes */
/* ======== */

#include "bplib_api_types.h"
#include "bplib_mem.h"
#include "bplib_eid.h"
#include "bplib_stor_loadbatch.h"

#include <sqlite3.h>

/* ======= */
/* Globals */
/* ======= */

/* Prepared statements are owned by individual calls, never shared globally. */

/* SQL query strings */

extern const char* GetNumBundlesSQL;
extern const char* TotalBytesSQL;
extern const char* DiscardExpiredSQL;
extern const char* ExpiredBytesSQL;
extern const char* DiscardEgressedSQL;
extern const char* EgressedBytesSQL;

/* ====== */
/* Macros */
/* ====== */

#define BPLIB_SQL_MAX_STRLEN 4096

/* ======== */
/* Typedefs */
/* ======== */

/*
** Result codes of SQLite3 executions:
** SQLITE_OK                      =  0  Successful result
** SQLITE_ERROR                   =  1  Generic error
** SQLITE_INTERNAL                =  2  Internal logic error in SQLite
** SQLITE_PERM                    =  3  Access permission denied
** SQLITE_ABORT                   =  4  Callback routine requested an abort
** SQLITE_BUSY                    =  5  The database file is locked
** SQLITE_LOCKED                  =  6  A table in the database is locked
** SQLITE_NOMEM                   =  7  A malloc() failed
** SQLITE_READONLY                =  8  Attempt to write a readonly database
** SQLITE_INTERRUPT               =  9  Operation terminated by sqlite3_interrupt(
** SQLITE_IOERR                   = 10  Some kind of disk I/O error occurred
** SQLITE_CORRUPT                 = 11  The database disk image is malformed
** SQLITE_NOTFOUND                = 12  Unknown opcode in sqlite3_file_control()
** SQLITE_FULL                    = 13  Insertion failed because database is full
** SQLITE_CANTOPEN                = 14  Unable to open the database file
** SQLITE_PROTOCOL                = 15  Database lock protocol error
** SQLITE_EMPTY                   = 16  Internal use only
** SQLITE_SCHEMA                  = 17  The database schema changed
** SQLITE_TOOBIG                  = 18  String or BLOB exceeds size limit
** SQLITE_CONSTRAINT              = 19  Abort due to constraint violation
** SQLITE_MISMATCH                = 20  Data type mismatch
** SQLITE_MISUSE                  = 21  Library used incorrectly
** SQLITE_NOLFS                   = 22  Uses OS features not supported on host
** SQLITE_AUTH                    = 23  Authorization denied
** SQLITE_FORMAT                  = 24  Not used
** SQLITE_RANGE                   = 25  2nd parameter to sqlite3_bind out of range
** SQLITE_NOTADB                  = 26  File opened that is not a database file
** SQLITE_NOTICE                  = 27  Notifications from sqlite3_log()
** SQLITE_WARNING                 = 28  Warnings from sqlite3_log()
** SQLITE_ROW                     = 100 sqlite3_step() has another row ready
** SQLITE_DONE                    = 101 sqlite3_step() has finished executing
** SQLITE_OK_LOAD_PERMANENTLY     = 256
** SQLITE_ERROR_MISSING_COLLSEQ   = 257
** SQLITE_BUSY_RECOVERY           = 261
** SQLITE_LOCKED_SHAREDCACHE      = 262
** SQLITE_READONLY_RECOVERY       = 264
** SQLITE_IOERR_READ              = 266
** SQLITE_CORRUPT_VTAB            = 267
** SQLITE_CANTOPEN_NOTEMPDIR      = 270
** SQLITE_CONSTRAINT_CHECK        = 275
** SQLITE_AUTH_USER               = 279
** SQLITE_NOTICE_RECOVER_WAL      = 283
** SQLITE_WARNING_AUTOINDEX       = 284
** SQLITE_ERROR_RETRY             = 513
** SQLITE_ABORT_ROLLBACK          = 516
** SQLITE_BUSY_SNAPSHOT           = 517
** SQLITE_LOCKED_VTAB             = 518
** SQLITE_READONLY_CANTLOCK       = 520
** SQLITE_IOERR_SHORT_READ        = 522
** SQLITE_CORRUPT_SEQUENCE        = 523
** SQLITE_CANTOPEN_ISDIR          = 526
** SQLITE_CONSTRAINT_COMMITHOOK   = 531
** SQLITE_NOTICE_RECOVER_ROLLBACK = 539
** SQLITE_ERROR_SNAPSHOT          = 769
** SQLITE_BUSY_TIMEOUT            = 773
** SQLITE_READONLY_ROLLBACK       = 776
** SQLITE_IOERR_WRITE             = 778
** SQLITE_CORRUPT_INDEX           = 779
** SQLITE_CANTOPEN_FULLPATH       = 782
** SQLITE_CONSTRAINT_FOREIGNKEY   = 787
** SQLITE_READONLY_DBMOVED        = 1032
** SQLITE_IOERR_FSYNC             = 1034
** SQLITE_CANTOPEN_CONVPATH       = 1038
** SQLITE_CONSTRAINT_FUNCTION     = 1043
** SQLITE_READONLY_CANTINIT       = 1288
** SQLITE_IOERR_DIR_FSYNC         = 1290
** SQLITE_CANTOPEN_DIRTYWAL       = 1294
** SQLITE_CONSTRAINT_NOTNULL      = 1299
** SQLITE_READONLY_DIRECTORY      = 1544
** SQLITE_IOERR_TRUNCATE          = 1546
** SQLITE_CANTOPEN_SYMLINK        = 1550
** SQLITE_CONSTRAINT_PRIMARYKEY   = 1555
** SQLITE_IOERR_FSTAT             = 1802
** SQLITE_CONSTRAINT_TRIGGER      = 1811
** SQLITE_IOERR_UNLOCK            = 2058
** SQLITE_CONSTRAINT_UNIQUE       = 2067
** SQLITE_IOERR_RDLOCK            = 2314
** SQLITE_CONSTRAINT_VTAB         = 2323
** SQLITE_IOERR_DELETE            = 2570
** SQLITE_CONSTRAINT_ROWID        = 2579
** SQLITE_IOERR_BLOCKED           = 2826
** SQLITE_CONSTRAINT_PINNED       = 2835
** SQLITE_IOERR_NOMEM             = 3082
** SQLITE_CONSTRAINT_DATATYPE     = 3091
** SQLITE_IOERR_ACCESS            = 3338
** SQLITE_IOERR_CHECKRESERVEDLOCK = 3594
** SQLITE_IOERR_LOCK              = 3850
** SQLITE_IOERR_CLOSE             = 4106
** SQLITE_IOERR_DIR_CLOSE         = 4362
** SQLITE_IOERR_SHMOPEN           = 4618
** SQLITE_IOERR_SHMSIZE           = 4874
** SQLITE_IOERR_SHMLOCK           = 5130
** SQLITE_IOERR_SHMMAP            = 5386
** SQLITE_IOERR_SEEK              = 5642
** SQLITE_IOERR_DELETE_NOENT      = 5898
** SQLITE_IOERR_MMAP              = 6154
** SQLITE_IOERR_GETTEMPPATH       = 6410
** SQLITE_IOERR_CONVPATH          = 6666
** SQLITE_IOERR_VNODE             = 6922
** SQLITE_IOERR_AUTH              = 7178
** SQLITE_IOERR_BEGIN_ATOMIC      = 7434
** SQLITE_IOERR_COMMIT_ATOMIC     = 7690
** SQLITE_IOERR_ROLLBACK_ATOMIC   = 7946
** SQLITE_IOERR_DATA              = 8202
** SQLITE_IOERR_CORRUPTFS         = 8458
*/
typedef uint16_t SQL_Status_t;

/* =================== */
/* Function Prototypes */
/* =================== */

SQL_Status_t BPLib_SQL_InitDb(const char* DbName, sqlite3** ActiveDbPtr);

SQL_Status_t BPLib_SQL_InitTable(BPLib_Instance_t* Inst);

BPLib_Status_t BPLib_SQL_Init(BPLib_Instance_t* Inst, const char* DbName);

SQL_Status_t BPLib_SQL_GetNumStoredBundles(sqlite3 *db, uint32_t *BundleCnt);

SQL_Status_t BPLib_SQL_GetNumEgressed(sqlite3* db, size_t *EgressCnt);

SQL_Status_t BPLib_SQL_GetTotalBundleBytes(sqlite3* db, uint64_t* TotalBytes);

BPLib_Status_t BPLib_SQL_GetDbSize(BPLib_Instance_t *Inst, size_t *DbSize);

BPLib_Status_t BPLib_SQL_DiscardExpired(BPLib_Instance_t* Inst, size_t* NumDiscarded);

SQL_Status_t BPLib_SQL_DiscardExpiredImpl(sqlite3* db, size_t* NumDiscarded, BPLib_Instance_t* Inst);

BPLib_Status_t BPLib_SQL_DiscardEgressed(BPLib_Instance_t* Inst, size_t* NumDiscarded);

SQL_Status_t BPLib_SQL_DiscardEgressedImpl(BPLib_Instance_t *Inst, size_t* NumDiscarded);

BPLib_Status_t BPLib_SQL_Cleanup(BPLib_Instance_t* Inst);

BPLib_Status_t BPLib_SQL_GetDestEidWhereClause(BPLib_EID_Pattern_t* DestEIDs, size_t NumEIDs, 
                                                                        char *WhereClause);

#endif /* BPLIB_STOR_SQL_H */
