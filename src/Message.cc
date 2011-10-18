/**
 * @file
 *
 * This file implements the _Message class
 */

/******************************************************************************
 * Copyright 2009-2011, Qualcomm Innovation Center, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 ******************************************************************************/

#include <qcc/platform.h>

#include <assert.h>
#include <ctype.h>
#include <limits>

#include <qcc/String.h>
#include <qcc/Mutex.h>
#include <qcc/String.h>
#include <qcc/StringUtil.h>
#include <qcc/time.h>
#include <qcc/Util.h>
#include <qcc/Debug.h>

#include <alljoyn/Message.h>
#include <alljoyn/BusAttachment.h>

#include "BusInternal.h"
#include "BusUtil.h"

#define QCC_MODULE "ALLJOYN"


#define MAX_NAME_LEN 256


using namespace qcc;
using namespace std;

namespace ajn {

static const char* HdrId[] = {
    "INVALID",
    "PATH",
    "INTERFACE",
    "MEMBER",
    "ERROR_NAME",
    "REPLY_SERIAL",
    "DESTINATION",
    "SENDER",
    "SIGNATURE",
    "HANDLES",
    "TIMESTAMP",
    "TIME_TO_LIVE",
    "COMPRESSION_TOKEN",
    "SESSION_ID"
};

static const char* MsgId[] = {
    "INVALID",
    "METHOD_CALL",
    "METHOD_RET",
    "ERROR",
    "SIGNAL"
};

qcc::String _Message::ToString() const
{
    return ToString(msgArgs, numMsgArgs);
}

HeaderFields::HeaderFields(const HeaderFields& other)
{
    for (size_t i = 0; i < ArraySize(field); ++i) {
        field[i] = other.field[i];
    }
}

HeaderFields& HeaderFields::operator=(const HeaderFields& other)
{
    if (this != &other) {
        for (size_t i = 0; i < ArraySize(field); ++i) {
            field[i] = other.field[i];
        }
    }
    return *this;
}

const AllJoynTypeId HeaderFields::FieldType[] = {
    ALLJOYN_INVALID,     /* ALLJOYN_HDR_FIELD_INVALID - not allowed  */
    ALLJOYN_OBJECT_PATH, /* ALLJOYN_HDR_FIELD_PATH                   */
    ALLJOYN_STRING,      /* ALLJOYN_HDR_FIELD_INTERFACE              */
    ALLJOYN_STRING,      /* ALLJOYN_HDR_FIELD_MEMBER                 */
    ALLJOYN_STRING,      /* ALLJOYN_HDR_FIELD_ERROR_NAME             */
    ALLJOYN_UINT32,      /* ALLJOYN_HDR_FIELD_REPLY_SERIAL           */
    ALLJOYN_STRING,      /* ALLJOYN_HDR_FIELD_DESTINATION            */
    ALLJOYN_STRING,      /* ALLJOYN_HDR_FIELD_SENDER                 */
    ALLJOYN_SIGNATURE,   /* ALLJOYN_HDR_FIELD_SIGNATURE              */
    ALLJOYN_UINT32,      /* ALLJOYN_HDR_FIELD_HANDLES                */
    ALLJOYN_UINT32,      /* ALLJOYN_HDR_FIELD_TIMESTAMP              */
    ALLJOYN_UINT16,      /* ALLJOYN_HDR_FIELD_TIME_TO_LIVE           */
    ALLJOYN_UINT32,      /* ALLJOYN_HDR_FIELD_COMPRESSION_TOKEN      */
    ALLJOYN_UINT32,      /* ALLJOYN_HDR_FIELD_SESSION_ID             */
    ALLJOYN_INVALID      /* ALLJOYN_HDR_FIELD_UNKNOWN                */
};

const bool HeaderFields::Compressible[] = {
    false,            /* ALLJOYN_HDR_FIELD_INVALID           */
    true,             /* ALLJOYN_HDR_FIELD_PATH              */
    true,             /* ALLJOYN_HDR_FIELD_INTERFACE         */
    true,             /* ALLJOYN_HDR_FIELD_MEMBER,           */
    false,            /* ALLJOYN_HDR_FIELD_ERROR_NAME        */
    false,            /* ALLJOYN_HDR_FIELD_REPLY_SERIAL      */
    true,             /* ALLJOYN_HDR_FIELD_DESTINATION       */
    true,             /* ALLJOYN_HDR_FIELD_SENDER            */
    true,             /* ALLJOYN_HDR_FIELD_SIGNATURE         */
    false,            /* ALLJOYN_HDR_FIELD_HANDLES           */
    false,            /* ALLJOYN_HDR_FIELD_TIMESTAMP         */
    true,             /* ALLJOYN_HDR_FIELD_TIME_TO_LIVE      */
    false,            /* ALLJOYN_HDR_FIELD_COMPRESSION_TOKEN */
    true,             /* ALLJOYN_HDR_FIELD_SESSION_ID        */
    false             /* ALLJOYN_HDR_FIELD_UNKNOWN           */
};

qcc::String HeaderFields::ToString(size_t indent) const
{
    qcc::String in = qcc::String(indent, ' ');
    qcc::String str;
    for (size_t i = ALLJOYN_HDR_FIELD_PATH; i < ALLJOYN_HDR_FIELD_UNKNOWN; i++) {
        if (field[i].typeId != ALLJOYN_INVALID) {
            str += in + "<header field=\"" + qcc::String(HdrId[i]) + "\">\n";
            str += field[i].ToString(indent + 2) + "\n";
            str += in + "</header>\n";
        }
    }
    return str;
}

/*
 * A brief description of the message
 */
qcc::String _Message::Description() const
{
    qcc::String outStr;
    outStr += qcc::String(msgHeader.msgType <= MESSAGE_SIGNAL ? MsgId[msgHeader.msgType] : MsgId[0]);
    switch (msgHeader.msgType) {
    case MESSAGE_METHOD_CALL:
        outStr = outStr + "[" + U32ToString(msgHeader.serialNum) + "] ";
        if (hdrFields.field[ALLJOYN_HDR_FIELD_INTERFACE].typeId == ALLJOYN_STRING) {
            outStr = outStr + hdrFields.field[ALLJOYN_HDR_FIELD_INTERFACE].v_string.str + ".";
        }
        if (hdrFields.field[ALLJOYN_HDR_FIELD_MEMBER].typeId == ALLJOYN_STRING) {
            outStr = outStr + hdrFields.field[ALLJOYN_HDR_FIELD_MEMBER].v_string.str;
        }
        if (hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].typeId == ALLJOYN_SIGNATURE) {
            outStr = outStr + "(" + hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].v_string.str + ")";
        } else {
            outStr += "()";
        }
        break;

    case MESSAGE_METHOD_RET:
        outStr = outStr + "[" + U32ToString(hdrFields.field[ALLJOYN_HDR_FIELD_REPLY_SERIAL].v_uint32) + "]";
        if (hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].typeId == ALLJOYN_SIGNATURE) {
            outStr = outStr + "(" + hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].v_string.str + ")";
        }
        break;

    case MESSAGE_ERROR:
        outStr = outStr + "[" + U32ToString(hdrFields.field[ALLJOYN_HDR_FIELD_REPLY_SERIAL].v_uint32) + "] ";
        if (hdrFields.field[ALLJOYN_HDR_FIELD_ERROR_NAME].typeId == ALLJOYN_STRING) {
            outStr = outStr + hdrFields.field[ALLJOYN_HDR_FIELD_ERROR_NAME].v_string.str;
        }
        break;

    case MESSAGE_SIGNAL:
        outStr = outStr + "[" + U32ToString(msgHeader.serialNum) + "] ";
        if (hdrFields.field[ALLJOYN_HDR_FIELD_INTERFACE].typeId == ALLJOYN_STRING) {
            outStr = outStr + hdrFields.field[ALLJOYN_HDR_FIELD_INTERFACE].v_string.str + ".";
        }
        if (hdrFields.field[ALLJOYN_HDR_FIELD_MEMBER].typeId == ALLJOYN_STRING) {
            outStr = outStr + hdrFields.field[ALLJOYN_HDR_FIELD_MEMBER].v_string.str;
        }
        if (hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].typeId == ALLJOYN_SIGNATURE) {
            outStr = outStr + "(" + hdrFields.field[ALLJOYN_HDR_FIELD_SIGNATURE].v_string.str + ")";
        }
        break;

    default:
        break;
    }
    return outStr;
}

static qcc::String FlagBits(uint8_t flags)
{
    qcc::String f;
    while (flags) {
        f.insert((size_t)0, (flags & 1) ? "1" : "0");
        flags >>= 1;
    }
    return f;
}

qcc::String _Message::ToString(const MsgArg* args, size_t numArgs) const
{
    qcc::String outStr;
    size_t indent = 2;
    qcc::String in = qcc::String(indent, ' ');

    if (msgHeader.endian == 0) {
        outStr = "<message/>";
    } else {
        outStr = "<message";
        outStr += " endianness=\"" + qcc::String(msgHeader.endian == ALLJOYN_LITTLE_ENDIAN ? "LITTLE" : "BIG") + "\"";
        outStr += " type=\"" + qcc::String(msgHeader.msgType <= MESSAGE_SIGNAL ? MsgId[msgHeader.msgType] : MsgId[0]) + "\"";
        outStr += " version=\"" + U32ToString(msgHeader.majorVersion) + "\"";
        outStr += " body_len=\"" + U32ToString(msgHeader.bodyLen) + "\"";
        outStr += " serial=\"" + U32ToString(msgHeader.serialNum) + "\"";
        if (msgHeader.flags) {
            outStr += " flags=\"" + FlagBits(msgHeader.flags) + "\"";
        }
        outStr += ">\n";
        outStr += in + "<header_fields>\n" + hdrFields.ToString(indent + 2) + in + "</header_fields>\n";
        if (numArgs > 0) {
            outStr += in + "<body>\n";
            for (size_t i = 0; i < numArgs; i++) {
                outStr += args[i].ToString(2 + indent) + "\n";
            }
            outStr += in + "</body>\n";
        }
        outStr += "</message>";
    }
    return outStr;
}

const char* _Message::GetErrorName(qcc::String* errorMessage) const
{
    if (msgHeader.msgType ==  MESSAGE_ERROR) {
        if (hdrFields.field[ALLJOYN_HDR_FIELD_ERROR_NAME].typeId == ALLJOYN_STRING) {
            if (errorMessage != NULL) {
                errorMessage->clear();
                for (size_t i = 0; i < numMsgArgs; i++) {
                    if (msgArgs[i].typeId == ALLJOYN_STRING) {
                        errorMessage->append(msgArgs[i].v_string.str);
                    }
                }
            }
            return hdrFields.field[ALLJOYN_HDR_FIELD_ERROR_NAME].v_string.str;
        }
    }
    return NULL;
}

QStatus _Message::GetArgs(const char* signature, ...)
{
    size_t sigLen = (signature ? strlen(signature) : 0);
    if (sigLen == 0) {
        return ER_BAD_ARG_1;
    }
    va_list argp;
    va_start(argp, signature);
    QStatus status = MsgArg::VParseArgs(signature, sigLen, msgArgs, numMsgArgs, &argp);
    va_end(argp);
    return status;
}

_Message::_Message(BusAttachment& bus) :
    bus(bus),
    endianSwap(false),
    msgBuf(NULL),
    msgArgs(NULL),
    numMsgArgs(0),
    ttl(0),
    handles(NULL),
    numHandles(0),
    encrypt(false)
{
    msgHeader.msgType = MESSAGE_INVALID;
    msgHeader.endian = myEndian;
}

_Message::_Message(const _Message& other)
    : bus(other.bus),
    endianSwap(other.endianSwap),
    msgHeader(other.msgHeader),
    msgBuf(other.msgBuf ? new uint64_t[other.bufSize / 8] : NULL),
    msgArgs((other.numMsgArgs && other.msgArgs) ? new MsgArg[other.numMsgArgs] : NULL),
    numMsgArgs(other.numMsgArgs),
    bufSize(other.bufSize),
    bufEOD((other.msgBuf && other.bufEOD) ? ((uint8_t*)msgBuf) + (other.bufEOD - ((uint8_t*)other.msgBuf)) : NULL),
    bufPos((other.msgBuf && other.bufPos) ? ((uint8_t*)msgBuf) + (other.bufPos - ((uint8_t*)other.msgBuf)) : NULL),
    bodyPtr((other.msgBuf && other.bodyPtr) ? ((uint8_t*)msgBuf) + (other.bodyPtr - ((uint8_t*)other.msgBuf)) : NULL),
    ttl(other.ttl),
    timestamp(other.timestamp),
    replySignature(other.replySignature),
    authMechanism(other.authMechanism),
    rcvEndpointName(other.rcvEndpointName),
    handles(other.numHandles ? new qcc::SocketFd[other.numHandles] : NULL),
    numHandles(other.numHandles),
    encrypt(other.encrypt),
    hdrFields(other.hdrFields)
{
    // Copy msgBuf
    if (msgBuf) {
        ::memcpy(msgBuf, other.msgBuf, bufSize);
    }

    // Copy msgArgs
    if (msgArgs) {
        for (size_t i = 0; i < numMsgArgs; ++i) {
            msgArgs[i] = other.msgArgs[i];
        }
    }

    // Copy handles
    if (handles) {
        for (size_t i = 0; i < numHandles; ++i) {
            SocketDup(other.handles[i], handles[i]);
        }
    }
}

_Message::~_Message(void)
{
    delete [] msgBuf;
    delete [] msgArgs;
    while (numHandles) {
        qcc::Close(handles[--numHandles]);
    }
    delete [] handles;
}

QStatus _Message::ReMarshal(const char* senderName, bool newSerial)
{
    if (senderName) {
        hdrFields.field[ALLJOYN_HDR_FIELD_SENDER].Set("s", senderName);
    }

    if (newSerial) {
        msgHeader.serialNum = bus.GetInternal().NextSerial();
    }

    /*
     * Remarshal invalidates any unmarshalled message args.
     */
    delete [] msgArgs;
    msgArgs = NULL;
    numMsgArgs = 0;

    /*
     * We delete the current buffer after we have copied the body data
     */
    uint64_t* savBuf = msgBuf;

    /*
     * Compute the new header sizes
     */
    ComputeHeaderLen();
    /*
     * Padding the end of the buffer ensures we can unmarshal a few bytes beyond the end of the
     * message reducing the places where we need to check for bufEOD when unmarshaling the body.
     */
    bufSize = sizeof(msgHeader) + ((((msgHeader.headerLen + 7) & ~7) + msgHeader.bodyLen + 7) & ~7) + 8;
    msgBuf = new uint64_t[bufSize / 8];
    bufPos = (uint8_t*)msgBuf;
    memcpy(bufPos, &msgHeader, sizeof(msgHeader));
    bufPos += sizeof(msgHeader);
    /*
     * If we need to do an endian-swap do so directly in the buffer
     */
    if (endianSwap) {
        MessageHeader* hdr = (MessageHeader*)msgBuf;
        EndianSwap32(hdr->bodyLen);
        EndianSwap32(hdr->serialNum);
        EndianSwap32(hdr->headerLen);
    }
    /*
     * Marshal the header fields
     */
    MarshalHeaderFields();
    assert(((size_t)bufPos & 7) == 0);
    /*
     * Copy in the body if there was one
     */
    if (msgHeader.bodyLen != 0) {
        memcpy(bufPos, bodyPtr, msgHeader.bodyLen);
    }
    bodyPtr = bufPos;
    bufPos += msgHeader.bodyLen;
    bufEOD = bufPos;
    /*
     * Zero fill the pad at the end of the buffer
     */
    assert((size_t)(bufEOD - (uint8_t*)msgBuf) < bufSize);
    memset(bufEOD, 0, (uint8_t*)msgBuf + bufSize - bufEOD);
    delete [] savBuf;
    return ER_OK;
}

bool _Message::IsExpired(uint32_t* tillExpireMS) const
{
    uint32_t expires;

    /* If the mssage has a TTL check if it has expired */
    if (ttl) {
        /* timestamp can be larger than 'now' due to clock drift adjustment */
        uint32_t now = GetTimestamp();
        uint32_t elapsed = (now > timestamp) ? now - timestamp : 0;
        if (ttl > elapsed) {
            expires = ttl - elapsed;
            QCC_DbgHLPrintf(("Message expires in %d milliseconds", expires));
        } else {
            QCC_DbgHLPrintf(("Message expired %u milliseconds ago", elapsed - ttl));
            expires = 0;
        }
    } else {
        expires = (numeric_limits<uint32_t>::max)();
    }
    if (tillExpireMS) {
        *tillExpireMS = expires;
    }
    return expires == 0;
}

/*
 * Clear the header fields - this also frees any data allocated to them.
 */
void _Message::ClearHeader()
{
    if (msgHeader.msgType != MESSAGE_INVALID) {
        for (uint32_t fieldId = ALLJOYN_HDR_FIELD_INVALID; fieldId < ArraySize(hdrFields.field); fieldId++) {
            hdrFields.field[fieldId].Clear();
        }
        delete [] msgArgs;
        msgArgs = NULL;
        numMsgArgs = 0;
        ttl = 0;
        msgHeader.msgType = MESSAGE_INVALID;
        while (numHandles) {
            qcc::Close(handles[--numHandles]);
        }
        delete [] handles;
        handles = NULL;
        encrypt = false;
        authMechanism.clear();
    }
}

}

struct _alljoyn_message_handle {
    _alljoyn_message_handle(ajn::BusAttachment& bus) : msg(bus) { }
    _alljoyn_message_handle(const ajn::_Message& other) : msg(other) { }

    ajn::Message msg;
};

alljoyn_message alljoyn_message_create(alljoyn_busattachment bus)
{
    return new struct _alljoyn_message_handle (*((ajn::BusAttachment*)bus));
}

void alljoyn_message_destroy(alljoyn_message msg)
{
    delete msg;
}

const alljoyn_msgargs alljoyn_message_getarg(alljoyn_message msg, size_t argN)
{
    return (alljoyn_msgargs)msg->msg->GetArg(argN);
}
