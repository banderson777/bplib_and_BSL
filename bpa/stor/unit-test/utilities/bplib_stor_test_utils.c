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

#include "bplib_stor_test_utils.h"
#include "bplib_pi.h"
#include "bplib_cbor.h"
#include "bplib_cla.h"
#include "bplib_fwp.h"

#include <string.h>
#include <stdlib.h>
#include <sqlite3.h>

/*
** Global Data
*/

BPLib_Instance_t BplibInst;
BPLib_PI_ChannelTable_t TestChanTbl;
BPLib_CLA_ContactsTable_t TestContTbl;
BPLib_Bundle_t TestBundle;

static BPLib_Bundle_t LoadedBundle;
static BPLib_MEM_Block_t LoadedBlob;


/*
** Test Setup Helpers
*/
void BPLib_STOR_Test_CreateTestBundle(BPLib_Bundle_t* Bundle, uint32_t BundleId)
{
    memset(Bundle, 0, sizeof(*Bundle));
    /* Test Bundle (Minimal Metadata needed to store) */
    Bundle->blocks.PrimaryBlock.Timestamp.CreateTime = 797186475264;
    Bundle->blocks.PrimaryBlock.Lifetime = 5000;
    Bundle->blocks.PrimaryBlock.DestEID.Node = 100;
    Bundle->blocks.PrimaryBlock.DestEID.Service = 1;
    Bundle->blocks.PrimaryBlock.BundleId = BundleId;
    Bundle->Meta.TotalBytes = 1000;

    /* MEM is stubbed so the tests can check the call's in UTAssert 
    ** Create a single-block data blob with the string "CBOR Blob"
    */
    Bundle->blob = (BPLib_MEM_Block_t*)calloc(1, sizeof(BPLib_MEM_Block_t));
    Bundle->blob->next = NULL;
    strcpy((char*)Bundle->blob->user_data.BigData, "CBOR Blob");
    Bundle->blob->used_len = 10; // strlen + 1 for \0
}

void BPLib_STOR_Test_FreeTestBundle(BPLib_Bundle_t* Bundle)
{
    BPLib_MEM_Block_t* CurrBlock;
    BPLib_MEM_Block_t* NextBlock;

    if (Bundle == NULL)
    {
        return;
    }

    CurrBlock = Bundle->blob;
    while (CurrBlock != NULL)
    {
        NextBlock = CurrBlock->next;
        free(CurrBlock);
        CurrBlock = NextBlock;
    }
}

/*
** Function Definitions
*/
/* The storage suite tests SQL and egress independently of BP encoding. The
 * codec stub stores the fixture blob verbatim; real wire round trips are
 * covered by the CBOR and integration recovery suites. */
static int32 EncodeFixture(void *User, int32 Status, uint32 Calls, const UT_StubContext_t *Ctx)
{
    (void)User;
    (void)Calls;
    if (Status != BPLIB_SUCCESS) return Status;
    BPLib_Bundle_t *Bundle = UT_Hook_GetArgValueByName(Ctx, "StoredBundle", BPLib_Bundle_t *);
    unsigned char *Out = UT_Hook_GetArgValueByName(Ctx, "OutputBuffer", void *);
    size_t Capacity = UT_Hook_GetArgValueByName(Ctx, "OutputBufferSize", size_t);
    size_t *Length = UT_Hook_GetArgValueByName(Ctx, "NumBytesCopied", size_t *);
    *Length = 0;
    for (BPLib_MEM_Block_t *Block = Bundle->blob; Block; Block = Block->next)
    {
        if (Block->used_len > Capacity - *Length) return BPLIB_BUF_LEN_ERROR;
        memcpy(Out + *Length, Block->user_data.BigData, Block->used_len);
        *Length += Block->used_len;
    }
    return BPLIB_SUCCESS;
}

static int32 DecodeFixture(void *User, int32 Status, uint32 Calls, const UT_StubContext_t *Ctx)
{
    (void)User;
    (void)Calls;
    if (Status != BPLIB_SUCCESS) return Status;
    BPLib_Bundle_t *Bundle = UT_Hook_GetArgValueByName(Ctx, "bundle", BPLib_Bundle_t *);
    const void *Wire = UT_Hook_GetArgValueByName(Ctx, "CandBundle", const void *);
    size_t Length = UT_Hook_GetArgValueByName(Ctx, "CandBundleLen", size_t);
    if (Length > sizeof(LoadedBlob.user_data.BigData)) return BPLIB_BUF_LEN_ERROR;
    memcpy(LoadedBlob.user_data.BigData, Wire, Length);
    LoadedBlob.used_len = Length;
    Bundle->blob = &LoadedBlob;
    Bundle->blocks = TestBundle.blocks;
    return BPLIB_SUCCESS;
}

void BPLib_STOR_Test_Setup(void)
{
    /* Initialize test environment to default state for every test */
    UT_ResetState(0);

    memset(&BplibInst, 0, sizeof(BplibInst));
    memset(&Context_BPLib_AS_Increment, 0, sizeof(BPLib_AS_IncrementDecrementContext_t));

    BPLib_NC_ConfigPtrs.ChanConfigPtr = &TestChanTbl;
    BPLib_NC_ConfigPtrs.ContactsConfigPtr = &TestContTbl;
    BPLib_FWP_ProxyCallbacks.BPA_TIMEP_GetHostTime = BPA_TIMEP_GetHostTime;
    UT_SetDefaultReturnValue(UT_KEY(BPLib_QM_IsSystemIdle), (UT_IntReturn_t) true);
    UT_SetDefaultReturnValue(UT_KEY(BPLib_QM_IsIngressIdle), (UT_IntReturn_t) true);
    UT_SetDefaultReturnValue(UT_KEY(BPLib_QM_WaitQueueTryPush), (UT_IntReturn_t) true);

    UT_SetHandlerFunction(UT_KEY(BPLib_EM_SendEvent), UT_Handler_BPLib_EM_SendEvent, NULL);
    UT_SetHandlerFunction(UT_KEY(BPLib_QM_WaitQueueTryPush), UT_Handler_BPLib_QM_WaitQueueTryPush, NULL);
    UT_SetHandlerFunction(UT_KEY(BPLib_AS_Increment), UT_Handler_BPLib_AS_Increment, NULL);
    UT_SetHandlerFunction(UT_KEY(BPLib_AS_Decrement), UT_Handler_BPLib_AS_Decrement, NULL);

    UT_SetDefaultReturnValue(UT_KEY(BPLib_NC_GetNodeConfigValue), BPLIB_MAX_LIFETIME_ALLOWED);

    UT_SetHookFunction(UT_KEY(BPLib_CBOR_EncodeBundle), EncodeFixture, NULL);

    /* Init Storage */
    BPLib_STOR_Init(&BplibInst);
}

void BPLib_STOR_Test_Teardown(void)
{
    /* Destroy Storage */
    BPLib_STOR_Destroy(&BplibInst);
}

void BPLib_STOR_Test_SetupOneBundleStored(void)
{
    BPLib_STOR_Test_Setup();
    BPLib_STOR_Test_CreateTestBundle(&TestBundle, 0);

    /* 
    ** Assume bundle creation time is valid and DTN time is valid; assume a
    ** nominal configuration. Also set it up such that the bundle expires in
    ** TestBundle.blocks.PrimaryBlock.Lifetime milliseconds
    */
    UT_SetDeferredRetcode(UT_KEY(BPLib_TIME_GetCurrentDtnTime), 1, TestBundle.blocks.PrimaryBlock.Timestamp.CreateTime);
    UT_SetDeferredRetcode(UT_KEY(BPLib_TIME_GetCurrentDtnTime), 1, TestBundle.blocks.PrimaryBlock.Timestamp.CreateTime);
    UT_SetDeferredRetcode(UT_KEY(BPLib_TIME_GetMonotonicTime), 1, TestBundle.blocks.PrimaryBlock.Timestamp.CreateTime);
    BPLib_STOR_StoreBundle(&BplibInst, &TestBundle);

    BPLib_STOR_FlushPending(&BplibInst);

    memset(&LoadedBundle, 0, sizeof(LoadedBundle));
    memset(&LoadedBlob, 0, sizeof(LoadedBlob));
    UT_SetDeferredRetcode(UT_KEY(BPLib_MEM_BundleAlloc), 1, (uintptr_t)&LoadedBundle);
    UT_SetHookFunction(UT_KEY(BPLib_CBOR_DecodeBundle), DecodeFixture, NULL);
}

void BPLib_STOR_Test_TeardownOneBundleStored(void)
{
    BPLib_STOR_Test_FreeTestBundle(&TestBundle);
    BPLib_STOR_Test_Teardown();
    /* SQL is run with :memory:, so the bundle automatically is deleted after test exit */
}

void UtTest_Setup(void)
{
    TestBplib_STOR_Register();
    TestBplib_STOR_Store_Register();
    TestBplib_STOR_LoadBatch_Register();
    TestBplib_STOR_Load_Register();
}
