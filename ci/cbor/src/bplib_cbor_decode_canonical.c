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

#include <stdio.h>

#include "bplib_cbor_internal.h"


/*******************************************************************************
* RFC-9171 Canonical Block Parsing Definitions
*/

/* Shared Canonical Block State */
struct  _CanonicalBlockBaseParser
{
    QCBOR_UInt64Parser BlockTypeParser;
    QCBOR_UInt64Parser BlockNumberParser;
    QCBOR_UInt64Parser FlagsParser;
    QCBOR_UInt64Parser CRCTypeParser;
    QCBOR_CRCParser CRCParser;
};

/* Age Block Payload */
struct _AgeBlockDataParser
{
    QCBOR_UInt64Parser BundleAgeParser;
};

/* Prev Node Block Payload */
struct _PrevNodeBlockDataParser
{
    QCBOR_EIDParser EidForwardedParser;
};

/* Hop Count Block Payload */
struct _HopCountBlockDataParser
{
    QCBOR_UInt64Parser BundleHopLimitParser;
    QCBOR_UInt64Parser BundleHopCountParser;
};

/* Custody Transfer Block Payload */
struct _CustodyBlockDataParser
{
    QCBOR_UInt64Parser BundleSeqNumParser;
    QCBOR_UInt64Parser BundleSeqIdParser;
    QCBOR_EIDParser    BlockSrcAdminEidParser;
};

/* Administrative Record Parser */
struct _AdminRecordParser
{
    QCBOR_UInt64Parser             RecordTypeParser;
    QCBOR_AdminRecordContentParser ContentParser;
};

static struct _CanonicalBlockBaseParser CanonicalBlockParser = {
    .BlockTypeParser = BPLib_QCBOR_UInt64ParserImpl,
    .BlockNumberParser = BPLib_QCBOR_UInt64ParserImpl,
    .FlagsParser = BPLib_QCBOR_UInt64ParserImpl,
    .CRCTypeParser = BPLib_QCBOR_UInt64ParserImpl,
    .CRCParser = BPLib_QCBOR_CRCParserImpl
};


static struct _AgeBlockDataParser AgeBlockDataParser = {
    .BundleAgeParser = BPLib_QCBOR_UInt64ParserImpl
};


static struct _PrevNodeBlockDataParser PrevNodeBlockDataParser = {
    .EidForwardedParser = BPLib_QCBOR_EidDtnNoneParserImpl
};


static struct _HopCountBlockDataParser HopCountBlockDataParser = {
    .BundleHopLimitParser = BPLib_QCBOR_UInt64ParserImpl,
    .BundleHopCountParser = BPLib_QCBOR_UInt64ParserImpl
};

static struct _CustodyBlockDataParser CustodyBlockDataParser = {
    .BundleSeqNumParser     = BPLib_QCBOR_UInt64ParserImpl,
    .BundleSeqIdParser      = BPLib_QCBOR_UInt64ParserImpl,
    .BlockSrcAdminEidParser = BPLib_QCBOR_EidDtnNoneParserImpl,
};

static struct _AdminRecordParser AdminRecordDataParser = {
    .RecordTypeParser  = BPLib_QCBOR_UInt64ParserImpl,
    .ContentParser     = BPLib_QCBOR_AdminRecordContentParserImpl,
};

/*******************************************************************************
* RFC-9171 Canonical Block Parsers (Implementation)
*/
BPLib_Status_t BPLib_CBOR_DecodeCanonical(BPLib_Instance_t *Inst, QCBORDecodeContext* ctx, BPLib_Bundle_t* bundle,
                                    uint32_t CanonicalBlockIndex, const void *CandBundle)
{
    BPLib_Status_t          Status;
    BPLib_CanBlockHeader_t* CanonicalBlockHdr;
    QCBORItem               ArrayItem;
    QCBORItem               HopCountItem;
    QCBORItem               CustodyTransferItem;
    QCBORItem               AdminRecordItem;
    QCBORError              QStatus;
    uint64_t                BlockType;
    uint32_t                HeaderStartOffset;
    UsefulBufC              CanonBlockDataByteStrInfo;
    QCBORItem               PeekItem;
    uint64_t                TempAdminRecordType;
    BPLib_MEM_Block_t      *AdminRecordBlk;

    if ((ctx == NULL) || (bundle == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    if (CanonicalBlockIndex >= BPLIB_MAX_NUM_CANONICAL_BLOCKS)
    {
        return BPLIB_CBOR_DEC_CANON_BLOCK_INDEX_ERR;
    }

    /* Grab the current offset, to be kept in the canonical block's metadata */
    HeaderStartOffset = QCBORDecode_Tell(ctx);

    /* Enter the canonical block array */
    Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &ArrayItem);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_ENTER_ARRAY_ERR;
    }

    /* Block Type */
    Status = CanonicalBlockParser.BlockTypeParser(ctx, &BlockType);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_BLOCK_TYPE_DEC_ERR;
    }

    /* Once we know the block type, we can be smarter about decoding data directly into `bundle` */
    if (BlockType == BPLib_BlockType_Payload)
    {
        CanonicalBlockHdr = &bundle->blocks.PayloadHeader;
    }
    else
    {
        /* This is an extension block  */
        if (CanonicalBlockIndex >= BPLIB_MAX_NUM_EXTENSION_BLOCKS)
        {
            /* Only the payload has storage at this final canonical index.
             * A spare header cannot protect the indexed extension data below. */
            return BPLIB_CBOR_DEC_CANON_BLOCK_INDEX_ERR;
        }
        else
        {
            CanonicalBlockHdr = &bundle->blocks.ExtBlocks[CanonicalBlockIndex].Header;
        }
    }
    CanonicalBlockHdr->BlockType = BlockType;
    CanonicalBlockHdr->BlockOffsetStart = HeaderStartOffset;

    /* Block Number */
    Status = CanonicalBlockParser.BlockNumberParser(ctx, &CanonicalBlockHdr->BlockNum);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_BLOCK_NUM_DEC_ERR;
    }

    /* A payload block's block number must be 1 */
    if (BlockType == BPLib_BlockType_Payload && CanonicalBlockHdr->BlockNum != 1)
    {
        return BPLIB_CBOR_DEC_CANON_BLOCK_NUM_DEC_ERR;
    }

    /* Block number cannot be 0 */
    if (CanonicalBlockHdr->BlockNum == 0)
    {
        return BPLIB_CBOR_DEC_CANON_BLOCK_NUM_DEC_ERR;
    }

    /* Flags */
    Status = CanonicalBlockParser.FlagsParser(ctx, &CanonicalBlockHdr->BlockProcFlags);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_BLOCK_FLAG_DEC_ERR;
    }

    /* CRC Type */
    Status = CanonicalBlockParser.CRCTypeParser(ctx, &CanonicalBlockHdr->CrcType);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_CRC_TYPE_DEC_ERR;
    }

    /* Validate CRC Type */
    if (CanonicalBlockHdr->CrcType != BPLib_CRC_Type_None &&
        CanonicalBlockHdr->CrcType != BPLib_CRC_Type_CRC16 &&
        CanonicalBlockHdr->CrcType != BPLib_CRC_Type_CRC32C)
    {
        return BPLIB_CBOR_DEC_CANON_CRC_TYPE_DEC_ERR;
    }

    /*
    ** Next should be the canonical-block-specific data
    ** this should be wrapped in a CBOR byte-string
    */
    QCBORDecode_EnterBstrWrapped(ctx, QCBOR_TAG_REQUIREMENT_NOT_A_TAG, &CanonBlockDataByteStrInfo);
    QStatus = QCBORDecode_GetError(ctx);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_ENTER_BYTE_STR_ERR;
    }

    /*
    ** Grab the current offset, to be kept in the canonical block's metadata
    ** this should be after we enter the byte string
    ** This will be used for ADU delivery
    */
    CanonicalBlockHdr->DataOffsetStart = QCBORDecode_Tell(ctx);
    CanonicalBlockHdr->DataSize = CanonBlockDataByteStrInfo.len;

    /* Set discard to false by default */
    CanonicalBlockHdr->RequiresDiscard = false;

    if (CanonicalBlockHdr->BlockType == BPLib_BlockType_PrevNode)
    {
        Status = PrevNodeBlockDataParser.EidForwardedParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.PrevNodeBlockData.PrevNodeId);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_PREV_NODE_EID_DEC_ERR;
        }
    }
    else if (CanonicalBlockHdr->BlockType == BPLib_BlockType_Age)
    {
        Status = AgeBlockDataParser.BundleAgeParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.AgeBlockData.Age);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_AGE_BLOCK_DEC_ERR;
        }
    }
    else if (CanonicalBlockHdr->BlockType == BPLib_BlockType_HopCount)
    {
        /* Enter the hop count block data array */
        Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &HopCountItem);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_HOP_BLOCK_ENTER_ARRAY_ERR;
        }

        Status = HopCountBlockDataParser.BundleHopLimitParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopLimit);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_HOP_BLOCK_HOP_LIMIT_DEC_ERR;
        }

        /* Validate hop limit is within allowed range */
        if (bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopLimit < 1 ||
            bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopLimit > 255)
        {
            return BPLIB_CBOR_DEC_HOP_BLOCK_INVALID_DEC_ERR;
        }

        Status = HopCountBlockDataParser.BundleHopCountParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopCount);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_HOP_BLOCK_HOP_COUNT_DEC_ERR;
        }

        /* Validate hop count has not exceeded hop limit */
        if (bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopCount >
            bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopLimit)
        {
            return BPLIB_CBOR_DEC_HOP_BLOCK_EXCEEDED_ERR;
        }

        /* Exit the hop count block data array */
        Status = BPLib_QCBOR_ExitArray(ctx);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_HOP_BLOCK_EXIT_ARRAY_ERR;
        }
    }
    else if (CanonicalBlockHdr->BlockType == BPLib_BlockType_CTEB)
    {
        /* Enter the custody transfer block data array */
        Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &CustodyTransferItem);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_CUSTODY_BLOCK_ENTER_ARRAY_ERR;
        }

        Status = CustodyBlockDataParser.BundleSeqNumParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BundleSeqNum);

        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_CUSTODY_BLOCK_SEQ_NUM_DEC_ERR;
        }

        /* TODO: Validate bundle sequence number, if applicable */

        Status = CustodyBlockDataParser.BundleSeqIdParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BundleSeqId);

        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_CUSTODY_BLOCK_SEQ_ID_DEC_ERR;
        }

        /* TODO: Validate bundle sequence ID, if applicable */

        Status = CustodyBlockDataParser.BlockSrcAdminEidParser(ctx,
            &bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BlockSrcAdminEID);

        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_CUSTODY_BLOCK_SRC_EID_DEC_ERR;
        }

        /* TODO: Validate block source administrative endpoint ID, if applicable */

        /* Exit the custody transfer block data array */
        Status = BPLib_QCBOR_ExitArray(ctx);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_CUSTODY_BLOCK_EXIT_ARRAY_ERR;
        }
    }
    else if (CanonicalBlockHdr->BlockType == BPLib_BlockType_Payload)
    {
        /* Payload blocks shouldn't need to be re-encoded */
        CanonicalBlockHdr->RequiresEncode = false;

        if (bundle->blocks.PrimaryBlock.BundleProcFlags & BPLIB_BUNDLE_PROC_ADMIN_RECORD_FLAG &&
            BPLib_EID_IsMatch(&(bundle->blocks.PrimaryBlock.DestEID), &BPLIB_EID_INSTANCE))
        { 
            /* Payload block is an administrative record */
            AdminRecordBlk = BPLib_MEM_BlockAlloc(&(Inst->pool), BPLIB_MEM_BIG_BLK_SIZE);
            if (AdminRecordBlk == NULL)
            {
                return BPLIB_CBOR_DEC_ADMIN_RECORD_NULL_ERR;
            }

            bundle->blocks.AdminRecordPayload = &(AdminRecordBlk->user_data.AdminRecord);

            /* Enter admin record array */
            Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &AdminRecordItem);
            if (Status != BPLIB_SUCCESS)
            {
                return BPLIB_CBOR_DEC_CANON_ADMIN_REC_ENTER_ARR_ERR;
            }

            /* Parse admin record type */
            Status = AdminRecordDataParser.RecordTypeParser(ctx, &TempAdminRecordType);
            if (Status != BPLIB_SUCCESS)
            {
                return BPLIB_CBOR_DEC_CANON_ADMIN_REC_REC_TYPE_ERR;
            }

            bundle->blocks.AdminRecordPayload->AdminRecordType = (BPLib_ARP_AdminRecordTypeCode_t) TempAdminRecordType;

            /* Parse admin record content */
            Status = AdminRecordDataParser.ContentParser(ctx, bundle->blocks.AdminRecordPayload);
            if (Status != BPLIB_SUCCESS)
            {
                return BPLIB_CBOR_DEC_CANON_ADMIN_REC_CONT_ERR;
            }

            /* Exit admin record array */
            Status = BPLib_QCBOR_ExitArray(ctx);
            if (Status != BPLIB_SUCCESS)
            {
                return BPLIB_CBOR_DEC_CANON_ADMIN_REC_EXIT_ARR_ERR;
            }
        }
    }
    else
    {
        /* Override externally assigned block type value with our unknown block flag */
        CanonicalBlockHdr->BlockType = BPLib_BlockType_UNKNOWN;

        /* Block control flag says to delete bundle when block can't be processed */
        if (CanonicalBlockHdr->BlockProcFlags & BPLIB_BLOCK_PROC_DELETE_BUNDLE_FLAG)
        {
            return BPLIB_CBOR_DEC_UNKNOWN_BLOCK_DEC_ERR;
        }
        /* Block control flag says to discard just this block when it can't be processed */
        if (CanonicalBlockHdr->BlockProcFlags & BPLIB_BLOCK_PROC_DISCARD_BLOCK_FLAG)
        {
            CanonicalBlockHdr->RequiresDiscard = true;
        }

        /* If neither flag is set, the block will be kept and copied out on egress */
    }

    /* Exit the byte-string */
    QCBORDecode_ExitBstrWrapped(ctx);
    QStatus = QCBORDecode_GetError(ctx);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_EXIT_BYTE_STR_ERR;
    }

    /* CRC Value */
    Status = CanonicalBlockParser.CRCParser(ctx, &CanonicalBlockHdr->CrcVal,
                                CanonicalBlockHdr->CrcType);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_CRC_VAL_DEC_ERR;
    }

    /* Exit the canonical block array */
    Status = BPLib_QCBOR_ExitArray(ctx);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_CANON_EXIT_ARRAY_ERR;
    }

    /*
    ** Grab the end-of-block offset
    ** TODO: Figure out why we have to subtract 2 if the block is a payload block!
    */
    if (CanonicalBlockHdr->BlockType == BPLib_BlockType_Payload)
    {
        CanonicalBlockHdr->BlockOffsetEnd = QCBORDecode_Tell(ctx) - 2;

        /* Verify there is nothing following the payload block */
        QStatus = QCBORDecode_PeekNext(ctx, &PeekItem);
        if (QStatus != QCBOR_ERR_NO_MORE_ITEMS)
        {
            return BPLIB_CBOR_DEC_EXTRA_DATA_DEC_ERR;
        }
    }
    else
    {
        CanonicalBlockHdr->BlockOffsetEnd = QCBORDecode_Tell(ctx) - 1;
    }

    /* Validate the block's CRC */
    Status = BPLib_CBOR_ValidateBlockCrc(CandBundle,
                    CanonicalBlockHdr->CrcType, CanonicalBlockHdr->CrcVal,
                    CanonicalBlockHdr->BlockOffsetStart,
                    CanonicalBlockHdr->BlockOffsetEnd -
                    CanonicalBlockHdr->BlockOffsetStart + 1);
    if (Status != BPLIB_SUCCESS)
    {
        return Status;
    }

    #if (BPLIB_CBOR_DEBUG_PRINTS_ENABLED)
    printf("Canonical Block [%u]: \n", CanonicalBlockIndex);
    printf("\t Block Type: %lu\n", CanonicalBlockHdr->BlockType);
    printf("\t Block Number: %lu\n", CanonicalBlockHdr->BlockNum);
    printf("\t Flags: %lu\n", CanonicalBlockHdr->BlockProcFlags);
    printf("\t CRC Type: %lu\n", CanonicalBlockHdr->CrcType);
    printf("\t Block Offset Start: %lu\n", CanonicalBlockHdr->BlockOffsetStart);
    printf("\t Data Offset Start: %lu\n", CanonicalBlockHdr->DataOffsetStart);
    printf("\t Data Size: %lu\n", CanonicalBlockHdr->DataSize);
    printf("\t Block Offset End: %lu\n", CanonicalBlockHdr->BlockOffsetEnd);
    printf("\t Block Size: %lu\n", CanonicalBlockHdr->BlockOffsetEnd - CanonicalBlockHdr->BlockOffsetStart + 1);
    switch (CanonicalBlockHdr->BlockType)
    {
        case BPLib_BlockType_PrevNode:
            printf("\t Prev Node Block Data: \n");
            printf("\t\t Prev Node Block MetaData Length: %lu\n", sizeof(BPLib_PrevNodeBlockData_t));
            printf("\t\t EID Forwarded (scheme.node.service): %lu.%lu.%lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.PrevNodeBlockData.PrevNodeId.Scheme,
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.PrevNodeBlockData.PrevNodeId.Node,
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.PrevNodeBlockData.PrevNodeId.Service);
            break;
        case BPLib_BlockType_Age:
            printf("\t Age Block Data: \n");
            printf("\t\t Age Block MetaData Length: %lu\n", sizeof(BPLib_AgeBlockData_t));
            printf("\t\t Age (in milliseconds): %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.AgeBlockData.Age);
            break;
        case BPLib_BlockType_HopCount:
            printf("\t Hop Count Block Data: \n");
            printf("\t\t Hop Count MetaData Length: %lu\n", sizeof(BPLib_HopCountData_t));
           printf("\t\t Hop Count Definite Array Length: %d\n", HopCountItem.val.uCount);
            printf("\t\t Hop Limit: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopLimit);
            printf("\t\t Hop Count: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.HopCountData.HopCount);
            break;
        case BPLib_BlockType_CTEB:
            printf("\t Custody Transfer Block Data:\n");
            printf("\t\t Custody Transfer Block MetaData Length: %lu\n", sizeof(BPLib_CustodyBlockData_t));
            printf("\t\t Custody Transfer Block Definite Array Length: %d\n", CustodyTransferItem.val.uCount);
            printf("\t\t Bundle Sequence Number: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BundleSeqNum);
            printf("\t\t Bundle Sequence ID: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BundleSeqId);
            printf("\t\t Block Source Administrative Endpoint ID:\n");
            printf("\t\t\t Scheme: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BlockSrcAdminEID.Scheme);
            printf("\t\t\t IPN SSP Format: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BlockSrcAdminEID.IpnSspFormat);
            printf("\t\t\t Allocator: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BlockSrcAdminEID.Allocator);
            printf("\t\t\t Node: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BlockSrcAdminEID.Node);
            printf("\t\t\t Service: %lu\n",
                bundle->blocks.ExtBlocks[CanonicalBlockIndex].BlockData.CustodyBlockData.BlockSrcAdminEID.Service);
            break;
        case BPLib_BlockType_CREB:
            printf("\t CREB Block Data Parsing Skipped!\n");
            break;
        case BPLib_BlockType_Payload:
            if (bundle->blocks.PrimaryBlock.BundleProcFlags & BPLIB_BUNDLE_PROC_ADMIN_RECORD_FLAG &&
                BPLib_EID_IsMatch(&(bundle->blocks.PrimaryBlock.DestEID), &BPLIB_EID_INSTANCE))
            {
                printf("Compressed Custody Signal Data:\n");
                printf("\tMetadata Length: %lu\n", sizeof(BPLib_ARP_AdminRecord_t));
                printf("\tDefinite Array Length: %d\n", AdminRecordItem.val.uCount);
            }
            else
            {
                printf("\t Payload Data Length: %lu bytes\n",
                CanonicalBlockHdr->DataSize);
            }
            break;
        default:
            printf("\t Unrecognized Block (%lu) Data Parsing Skipped!\n", CanonicalBlockHdr->BlockType);
            break;
    }
    printf("\t CRC Value: 0x%lX\n", CanonicalBlockHdr->CrcVal);
    #endif

    return BPLIB_SUCCESS;
}
