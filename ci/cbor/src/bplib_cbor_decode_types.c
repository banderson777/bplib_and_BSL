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

#include "bplib_cbor_internal.h"
#include "bplib_eid.h"


/*******************************************************************************
* Exported Parsing Helpers
*/

BPLib_Status_t BPLib_QCBOR_EnterIndefiniteArray(QCBORDecodeContext* ctx, QCBORItem* ArrayItem)
{
    QCBORError QStatus;

    if ((ctx == NULL) || (ArrayItem == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Ensure ctx is pointing to an array. */
    QCBORDecode_EnterArray(ctx, ArrayItem);
    QStatus = QCBORDecode_GetError(ctx);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_ENTER_INDEF_ARRAY_ERR;
    }

    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_EnterDefiniteArray(QCBORDecodeContext* ctx, QCBORItem* ArrayItem)
{
    BPLib_Status_t Status;

    Status = BPLib_QCBOR_EnterIndefiniteArray(ctx, ArrayItem);
    if (Status != BPLIB_SUCCESS)
    {
        return Status;
    }

    /* Ensure the length isn't indicating this is an indefinite array
    ** Note: According QCBOR docs, UINT16_MAX is used to indicate indefinte length
    */
    if (ArrayItem->val.uCount > QCBOR_MAX_ITEMS_IN_ARRAY)
    {
        ArrayItem->val.uCount = 0;
        return BPLIB_CBOR_DEC_TYPES_ENTER_DEF_ARRAY_COUNT_ERR;
    }

    /* Now it is safe to conclude this is a definite array */
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_ExitArray(QCBORDecodeContext* ctx)
{
    QCBORError QStatus;

    if (ctx == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    QCBORDecode_ExitArray(ctx);
    QStatus = QCBORDecode_GetError(ctx);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EXIT_DEF_ARRAY_QCBOR_ERR;
    }

    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_EnterMap(QCBORDecodeContext* Context, QCBORItem* MapItem)
{
    QCBORError QStatus;

    if (Context == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Make sure the next item is a map */
    QCBORDecode_EnterMap(Context, MapItem);

    QStatus = QCBORDecode_GetError(Context);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_ENTER_MAP_ERR;
    }

    /* Ensure that this is a definite map */
    if (MapItem->val.uCount > QCBOR_MAX_ITEMS_IN_ARRAY)
    {
        MapItem->val.uCount = 0;
        return BPLIB_CBOR_DEC_TYPES_ENTER_MAP_COUNT_ERR;
    }

    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_ExitMap(QCBORDecodeContext* Context)
{
    QCBORError QStatus;

    if (Context == NULL)
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    QCBORDecode_ExitMap(Context);

    QStatus = QCBORDecode_GetError(Context);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EXIT_MAP_ERR;
    }

    return BPLIB_SUCCESS;
}

/*******************************************************************************
* RFC-9171 Type Parsers (Implementation)
*/

BPLib_Status_t BPLib_QCBOR_UInt64ParserImpl(QCBORDecodeContext* ctx, uint64_t* parsed)
{
    QCBORError QStatus;

    if ((ctx == NULL) || (parsed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    QCBORDecode_GetUInt64(ctx, parsed);
    QStatus = QCBORDecode_GetError(ctx);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_GET_UINT64_QCBOR_ERR;
    }
    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_EidDtnNoneParserImpl(QCBORDecodeContext* ctx, BPLib_EID_t* parsed)
{
    BPLib_Status_t Status;
    QCBORItem      ArrayItem;

    if ((ctx == NULL) || (parsed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Enter EID Array */
    Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &ArrayItem);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_ENTER_OUTER_ARRAY_ERR;
    }

    /* Parse URI Type: Note only IPN accepted */
    Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Scheme);
    if (Status != BPLIB_SUCCESS)
    {
        return Status;
    }
    if (parsed->Scheme != BPLIB_EID_SCHEME_IPN)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_SCHEME_NOT_IMPL_ERR;
    }
    /* Jam these extra fields, while we only support IPN two-digit EIDs */
    parsed->IpnSspFormat = BPLIB_EID_IPN_SSP_FORMAT_TWO_DIGIT;
    parsed->Allocator = 0;

    /* Enter SSP Array */
    Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &ArrayItem);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_SCHEME_NOT_IMPL_ERR;
    }

    /* Node Number */
    Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Node);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_IPN_NODE_DEC_ERR;
    }

    /* Node Service */
    Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Service);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_IPN_SERV_DEC_ERR;
    }

    /* Exit SSP Array */
    Status = BPLib_QCBOR_ExitArray(ctx);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_EXIT_SSP_ARRAY_ERR;
    }

    /* Exit EID Array */
    Status = BPLib_QCBOR_ExitArray(ctx);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_EXIT_OUTER_ARRAY_ERR;
    }

    return BPLIB_SUCCESS;
}


BPLib_Status_t BPLib_QCBOR_ReportToEidParserImpl(QCBORDecodeContext* ctx, BPLib_EID_t* parsed)
{
    BPLib_Status_t Status;
    QCBORItem      ArrayItem;

    if ((ctx == NULL) || (parsed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Enter EID Array */
    Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &ArrayItem);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_ENTER_OUTER_ARRAY_ERR;
    }

    /* Parse URI Type: Note only IPN accepted */
    Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Scheme);
    if (Status != BPLIB_SUCCESS)
    {
        return Status;
    }
    if (parsed->Scheme == BPLIB_EID_SCHEME_IPN)
    {
        /* Jam these extra fields, while we only support IPN two-digit EIDs */
        parsed->IpnSspFormat = BPLIB_EID_IPN_SSP_FORMAT_TWO_DIGIT;
        parsed->Allocator = 0;

        /* Enter SSP Array */
        Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &ArrayItem);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_EID_SCHEME_NOT_IMPL_ERR;
        }

        /* Node Number */
        Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Node);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_EID_IPN_NODE_DEC_ERR;
        }

        /* Node Service */
        Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Service);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_EID_IPN_SERV_DEC_ERR;
        }

        /* Exit SSP Array */
        Status = BPLib_QCBOR_ExitArray(ctx);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_EID_EXIT_SSP_ARRAY_ERR;
        }
    }
    else if (parsed->Scheme == BPLIB_EID_SCHEME_DTN)
    {
        Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->Node);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_EID_DTN_ERR;
        }

        /* Only dtn:none EID allowed for DTN scheme */
        if (parsed->Node != 0)
        {
            return BPLIB_CBOR_DEC_TYPES_EID_DTN_ERR;
        }

        /* Set EID to DTN None */
        BPLib_EID_CopyEids(parsed, BPLIB_EID_DTN_NONE);
    }
    else
    {
        return BPLIB_CBOR_DEC_TYPES_EID_SCHEME_NOT_IMPL_ERR;
    }

    /* Exit EID Array */
    Status = BPLib_QCBOR_ExitArray(ctx);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_EID_EXIT_OUTER_ARRAY_ERR;
    }

    return BPLIB_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_TimestampParserImpl(QCBORDecodeContext* ctx, BPLib_CreationTimeStamp_t* parsed)
{
    BPLib_Status_t Status;
    QCBORItem      ArrayItem;

    if ((ctx == NULL) || (parsed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Enter Timestamp Array */
    Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &ArrayItem);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_TIMESTAMP_ENTER_ARRAY_ERR;
    }

    /* First Element of the array should be creation timestamp */
    Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->CreateTime);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_TIMESTAMP_CREATE_DEC_ERR;
    }

    /* Second Element of the array should be creation timestamp */
    Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &parsed->SequenceNumber);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_TIMESTAMP_SEQ_NUM_DEC_ERR;
    }

    /* Exit timestamp Array */
    Status = BPLib_QCBOR_ExitArray(ctx);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_TIMESTAMP_EXIT_ARRAY_ERR;
    }

    return QCBOR_SUCCESS;
}

BPLib_Status_t BPLib_QCBOR_CRCParserImpl(QCBORDecodeContext* ctx, uint64_t* parsed, uint64_t crc_type)
{
    UsefulBufC TemporaryByteStringBuffer;
    QCBORError QStatus;

    if ((ctx == NULL) || (parsed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    if (crc_type == BPLib_CRC_Type_None)
    {
        /* if the CRC is of type "none", assume there is no CRC and skip further decoding */
        *parsed = 0;
        return BPLIB_SUCCESS;
    }

    /* get the CRC as a byte string */
    QCBORDecode_GetByteString(ctx, &TemporaryByteStringBuffer);
    QStatus = QCBORDecode_GetError(ctx);
    if (QStatus != QCBOR_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_CRC_ENTER_BYTE_STR_ERR;
    }

    if (crc_type == BPLib_CRC_Type_CRC16)
    {
        if (TemporaryByteStringBuffer.len != 2)
        {
            return BPLIB_CBOR_DEC_TYPES_CRC_16_LEN_ERR;
        }
        else
        {
            /* unpack the network-byte-ordered 16-bit value, storing into the 64-bit destination */
            *parsed  = (uint64_t) ((uint8_t*)TemporaryByteStringBuffer.ptr)[0] <<  8u;
            *parsed |= (uint64_t) ((uint8_t*)TemporaryByteStringBuffer.ptr)[1];
            return BPLIB_SUCCESS;
        }

    }
    else if (crc_type == BPLib_CRC_Type_CRC32C)
    {
        if (TemporaryByteStringBuffer.len != 4)
        {
            return BPLIB_CBOR_DEC_TYPES_CRC_32_LEN_ERR;
        }
        else
        {
            /* unpack the network-byte-ordered 32-bit value, storing into the 64-bit destination */
            *parsed  = (uint64_t) ((uint8_t*)TemporaryByteStringBuffer.ptr)[0] << 24u;
            *parsed |= (uint64_t) ((uint8_t*)TemporaryByteStringBuffer.ptr)[1] << 16u;
            *parsed |= (uint64_t) ((uint8_t*)TemporaryByteStringBuffer.ptr)[2] <<  8u;
            *parsed |= (uint64_t) ((uint8_t*)TemporaryByteStringBuffer.ptr)[3];
            return BPLIB_SUCCESS;
        }
    }
    else
    {
        return BPLIB_CBOR_DEC_TYPES_CRC_UNSUPPORTED_TYPE_ERR;
    }
}

BPLib_Status_t BPLib_QCBOR_AdminRecordContentParserImpl(QCBORDecodeContext* ctx, BPLib_ARP_AdminRecord_t* Parsed)
{
    BPLib_Status_t Status = BPLIB_SUCCESS;

    /* NOTE: The admin record array entering and exiting is handled in BPLib_CBOR_DecodeCanonical() */

    if ((ctx == NULL) || (Parsed == NULL))
    {
        Status = BPLIB_NULL_PTR_ERROR;
    }

    if (Status == BPLIB_SUCCESS)
    {
        switch (Parsed->AdminRecordType)
        {
            case BPLib_CT_BsrRecordTypeCode:
                /* TODO: Parse BSR admin record contents */
                break;

            case BPLib_CT_CrsRecordTypeCode:
                /* TODO: Parse CRS admin record contents */
                break;

            case BPLib_CT_CcsRecordTypeCode:
                Status = BPLib_QCBOR_BundleSeqCollectionParserImpl(ctx, Parsed->AdminRecordBody.CCS.BundleSeqCollections, &(Parsed->AdminRecordBody.CCS.NumBundleSeqCollections));
                break;

            default:
                Status = BPLIB_CBOR_DEC_TYPES_ADMIN_REC_INV_REC_TYPE;
                break;
        }
    }

    return Status;
}

BPLib_Status_t BPLib_QCBOR_BundleSeqCollectionParserImpl(QCBORDecodeContext* ctx, BPLib_CT_BundleSeqCollection_t* Parsed, size_t* NumSeqCollections)
{
    BPLib_Status_t Status;
    QCBORItem      MapItem;
    uint64_t       SeqRangeIdx;
    size_t         DispositionIdx;
    size_t         NumCollections;

    if (NumSeqCollections == NULL) return BPLIB_NULL_PTR_ERROR;
    *NumSeqCollections = 0;
    if ((ctx == NULL) || (Parsed == NULL))
    {
        return BPLIB_NULL_PTR_ERROR;
    }

    /* Enter disposition codes, bundle sequence collections map */
    Status = BPLib_QCBOR_EnterMap(ctx, &MapItem);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_ENTER_MAP_ERR;
    }

    /* Verify allowable number of bundle sequence collections were received */
    if (MapItem.val.uCount > BPLIB_CT_MAX_RECVD_SEQ_COLLECTIONS)
    {
        return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_MAP_SIZE_ERR;
    }

    NumCollections = MapItem.val.uCount;

    for (DispositionIdx = 0; DispositionIdx < NumCollections; DispositionIdx++)
    {
        Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &MapItem);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_MAP_ENTR_ARR_ERR;
        }

        memset((void*) &Parsed[DispositionIdx], 0, sizeof(BPLib_CT_BundleSeqCollection_t));

        /* === Set DispositionCode === */
        if (MapItem.uLabelType != QCBOR_TYPE_INT64 || MapItem.label.int64 == 0)
            return BPLIB_CBOR_DEC_INV_DISP_CODE_ERR;
        Parsed[DispositionIdx].DispositionCode = (BPLib_CT_DispositionCode_t) MapItem.label.int64;
        if ((int64_t)Parsed[DispositionIdx].DispositionCode != MapItem.label.int64)
            return BPLIB_CBOR_DEC_INV_DISP_CODE_ERR;

        /* Disposition codes can be negative or positive but not 0 */
        if (Parsed[DispositionIdx].DispositionCode == 0)
        {
            return BPLIB_CBOR_DEC_INV_DISP_CODE_ERR;
        }

        /* === Jam LastSeqNumAdded to an innocuous initial value === */
        Parsed[DispositionIdx].LastSeqNumAdded = 0;

        /* === Find SeqId === */
        Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &(Parsed[DispositionIdx].SeqId));
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_ID_ERR;
        }

        /* === Assign FirstSeqNum === */
        Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &(Parsed[DispositionIdx].FirstSeqNum));
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_NUM_ERR;
        }

        /* === Populate SeqRange === */
        Status = BPLib_QCBOR_EnterDefiniteArray(ctx, &MapItem);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_RNGE_ENTER_ERR;
        }

        Parsed[DispositionIdx].SeqRangeLen = MapItem.val.uCount;

        /* Check that the length of the bundle sequence range is within bounds */
        if (Parsed[DispositionIdx].SeqRangeLen > BPLIB_CT_MAX_SEQ_RANGE_LEN)
        {
            return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_RNGE_SIZE_ERR;
        }

        for (SeqRangeIdx = 0; SeqRangeIdx < Parsed[DispositionIdx].SeqRangeLen; SeqRangeIdx++)
        {
            Status = BPLib_QCBOR_UInt64ParserImpl(ctx, &(Parsed[DispositionIdx].SeqRange[SeqRangeIdx]));
            if (Status != BPLIB_SUCCESS)
            {
                return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_RNGE_ERR;
            }
        }

        Status = BPLib_QCBOR_ExitArray(ctx);
        if (Status != BPLIB_SUCCESS)
        {
            return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_RNGE_EXIT_ERR;
        }

        Status = BPLib_QCBOR_ExitArray(ctx);
        if (Status != BPLIB_SUCCESS)
        {
           return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_MAP_EXIT_ARR_ERR;
        }
    }

    /* Exit the admin record map */
    Status = BPLib_QCBOR_ExitMap(ctx);
    if (Status != BPLIB_SUCCESS)
    {
        return BPLIB_CBOR_DEC_TYPES_BNDL_SEQ_EXIT_MAP_ERR;
    }

    *NumSeqCollections = NumCollections;
    return BPLIB_SUCCESS;
}