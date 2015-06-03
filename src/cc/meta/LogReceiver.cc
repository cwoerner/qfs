//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2015/04/21
// Author: Mike Ovsiannikov
//
// Copyright 2015 Quantcast Corp.
//
// This file is part of Kosmos File System (KFS).
//
// Licensed under the Apache License, Version 2.0
// (the "License"); you may not use this file except in compliance with
// the License. You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
// implied. See the License for the specific language governing
// permissions and limitations under the License.
//
// Transaction log replication reciver.
//
//
//----------------------------------------------------------------------------

#include "LogReceiver.h"
#include "AuthContext.h"
#include "MetaRequest.h"
#include "util.h"

#include "common/kfstypes.h"
#include "common/MsgLogger.h"
#include "common/RequestParser.h"
#include "common/StBuffer.h"

#include "kfsio/NetManager.h"
#include "kfsio/NetConnection.h"
#include "kfsio/SslFilter.h"
#include "kfsio/Acceptor.h"
#include "kfsio/checksum.h"

#include "qcdio/QCUtils.h"
#include "qcdio/qcdebug.h"

#include <string>
#include <algorithm>
#include <iomanip>
#include <vector>

#include <time.h>
#include <errno.h>

namespace KFS
{
using std::string;
using std::vector;
using std::max;
using std::hex;

class LogReceiver::Impl :
    public IAcceptorOwner,
    public KfsCallbackObj,
    public ITimeout
{
private:
    class Connection;
public:
    typedef QCDLList<Connection> List;

    Impl()
        : IAcceptorOwner(),
          KfsCallbackObj(),
          ITimeout(),
          mReAuthTimeout(20),
          mMaxReadAhead(MAX_RPC_HEADER_LEN),
          mTimeout(60),
          mConnectionCount(0),
          mMaxConnectionCount(8 << 10),
          mIpV6OnlyFlag(false),
          mListenerAddress(),
          mAcceptorPtr(0),
          mAuthContext(),
          mCommittedLogSeq(-1),
          mLastWriteSeq(-1),
          mDeleteFlag(false),
          mAckBroadcastFlag(false),
          mParseBuffer(),
          mId(-1),
          mReplayerPtr(0),
          mWriteOpFreeListPtr(0),
          mCompletionQueueHeadPtr(0),
          mCompletionQueueTailPtr(0),
          mPendingSubmitHeadPtr(0),
          mPendingSubmitTailPtr(0)
    {
        List::Init(mConnectionsHeadPtr);
        mParseBuffer.Resize(mParseBuffer.Capacity());
        SET_HANDLER(this, &Impl::HandleEvent);
    }
    virtual KfsCallbackObj* CreateKfsCallbackObj(
        NetConnectionPtr& inConnectionPtr);
    bool SetParameters(
        const char*       inPrefixPtr,
        const Properties& inParameters)
    {
        Properties::String theParamName;
        if (inPrefixPtr) {
            theParamName.Append(inPrefixPtr);
        }
        const size_t thePrefixLen   = theParamName.GetSize();
        const bool   kHexFormatFlag = false;
        const string theListenOn    = inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "listenOn"), mListenerAddress.IsValid() ?
                mListenerAddress.ToString() : string());
        mListenerAddress.FromString(theListenOn, kHexFormatFlag);
        mReAuthTimeout = inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "reAuthTimeout"), mReAuthTimeout);
        mIpV6OnlyFlag = inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "ipV6OnlyFlag"), mIpV6OnlyFlag ? 1 : 0) != 0;
        mMaxReadAhead = max(512, min(64 << 20, inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "maxReadAhead"), mMaxReadAhead)));
        mMaxConnectionCount = inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "maxConnectionCount"), mMaxConnectionCount);
        mTimeout = inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "timeout"), mTimeout);
        mId = inParameters.getValue(
            theParamName.Truncate(thePrefixLen).Append(
            "id"), mId);
        mAuthContext.SetParameters(
            theParamName.Truncate(thePrefixLen).Append("auth.").c_str(),
            inParameters);
        return (! theListenOn.empty());
    }
    int Start(
        NetManager& inNetManager,
        Replayer&   inReplayer)
    {
        if (mDeleteFlag) {
            panic("LogReceiver::Impl::Start delete pending");
            return -EINVAL;
        }
        Shutdown();
        if (! inNetManager.IsRunning()) {
            KFS_LOG_STREAM_ERROR <<
                "net manager shutdown" <<
            KFS_LOG_EOM;
            return -EINVAL;
        }
        if (! mListenerAddress.IsValid()) {
            KFS_LOG_STREAM_ERROR <<
                "invalid listen address: " << mListenerAddress <<
            KFS_LOG_EOM;
            return -EINVAL;
        }
        if (mId < 0) {
            KFS_LOG_STREAM_ERROR <<
                "server id is not set: " << mId <<
            KFS_LOG_EOM;
            return -EINVAL;
        }
        const bool kBindOnlyFlag = false;
        mAcceptorPtr = new Acceptor(
            inNetManager,
            mListenerAddress,
            mIpV6OnlyFlag,
            this,
            kBindOnlyFlag
        );
        if (! mAcceptorPtr->IsAcceptorStarted()) {
            delete mAcceptorPtr;
            mAcceptorPtr = 0;
            KFS_LOG_STREAM_ERROR <<
                "failed to start acceptor: " << mListenerAddress <<
            KFS_LOG_EOM;
            return -ENOTCONN;
        }
        mReplayerPtr = &inReplayer;
        inNetManager.RegisterTimeoutHandler(this);
        return 0;
    }
    void Shutdown();
    int GetReAuthTimeout() const
        { return mReAuthTimeout; }
    int GetMaxReadAhead() const
        { return mMaxReadAhead; }
    int GetTimeout() const
        { return mTimeout; }
    AuthContext& GetAuthContext()
        { return mAuthContext; }
    void New(
        Connection& inConnection);
    void Done(
        Connection& inConnection);
    seq_t GetCommittedLogSeq() const
        { return mCommittedLogSeq; }
    seq_t GetLastWriteLogSeq() const
        { return mLastWriteSeq; }
    void Delete()
    {
        Shutdown();
        mDeleteFlag = true;
        if (0 < mConnectionCount) {
            return;
        }
        delete this;
    }
    int64_t GetId() const
        { return mId; }

    enum { kMaxBlockHeaderLen  = (int)sizeof(seq_t) * 2 + 1 + 16 };
    enum { kMinParseBufferSize = kMaxBlockHeaderLen <= MAX_RPC_HEADER_LEN ?
        MAX_RPC_HEADER_LEN : kMaxBlockHeaderLen };

    char* GetParseBufferPtr()
        { return mParseBuffer.GetPtr(); }
    char* GetParseBufferPtr(
        int inSize)
    {
        if (0 < inSize && mParseBuffer.GetSize() < (size_t)inSize) {
            mParseBuffer.Clear(); // To prevent copy.
            mParseBuffer.Resize(
                (inSize + kMinParseBufferSize - 1) /
                kMinParseBufferSize * kMinParseBufferSize
            );
        }
        return mParseBuffer.GetPtr();
    }
    void Replay(
        const char* inLinePtr,
        int         inLen)
        { mCommittedLogSeq = mReplayerPtr->Apply(inLinePtr, inLen); }
    MetaLogWriterControl* GetBlockWriteOp(
        seq_t  inStartSeq,
        seq_t& ioEndSeq)
    {
        if (ioEndSeq <= mLastWriteSeq || inStartSeq != mLastWriteSeq) {
            ioEndSeq = mLastWriteSeq;
            return 0;
        }
        MetaRequest* thePtr = mWriteOpFreeListPtr;
        if (thePtr) {
            mWriteOpFreeListPtr = thePtr->next;
            thePtr->next = 0;
        } else {
            thePtr = new MetaLogWriterControl(
                MetaLogWriterControl::kWriteBlock);
        }
        thePtr->clnt = this;
        return static_cast<MetaLogWriterControl*>(thePtr);
    }
    void Dispatch()
    {
        MetaRequest* thePtr              = mCompletionQueueHeadPtr;
        const bool   theAckBroadcastFlag = 0 != thePtr;
        mCompletionQueueHeadPtr = 0;
        mCompletionQueueTailPtr = 0;
        seq_t theNextSeq = (thePtr && thePtr->status != 0) ?
            static_cast<MetaLogWriterControl*>(thePtr)->blockStartSeq :
            mCommittedLogSeq;
        while (thePtr) {
            MetaLogWriterControl& theCur =
                *static_cast<MetaLogWriterControl*>(thePtr);
            thePtr = thePtr->next;
            theCur.next = 0;
            if (theCur.blockStartSeq != (theCur.status == 0 ?
                        mCommittedLogSeq : theNextSeq) ||
                    mLastWriteSeq < theCur.blockEndSeq ||
                    theCur.blockEndSeq < theCur.blockStartSeq) {
                panic("log write completion: invalid block sequence");
            }
            theNextSeq = theCur.blockEndSeq;
            if (theCur.status == 0) {
                mCommittedLogSeq = theNextSeq;
            }
            Release(theCur);
        }
        if (mCommittedLogSeq < theNextSeq) {
            // Log write failure, all subsequent write ops must fail too, as
            // those won't be adjacent in log sequence space.
            mLastWriteSeq = mCommittedLogSeq;
        }
        thePtr = mPendingSubmitHeadPtr;
        mPendingSubmitHeadPtr = 0;
        mPendingSubmitTailPtr = 0;
        while (thePtr) {
            MetaRequest& theCur = *thePtr;
            thePtr = thePtr->next;
            theCur.next = 0;
            submit_request(&theCur);
        }
        if (theAckBroadcastFlag) {
            mAckBroadcastFlag = mAckBroadcastFlag || theAckBroadcastFlag;
        }
    }
    virtual void Timeout()
    {
    }
    void Release(
        MetaLogWriterControl& inOp)
    {
        inOp.next = mWriteOpFreeListPtr;
        mWriteOpFreeListPtr = &inOp;
        inOp.blockData.Clear();
        inOp.blockLines.Clear();
        inOp.blockStartSeq = -1;
        inOp.blockEndSeq   = -1;
    }
    int HandleEvent(
        int   inType,
        void* inDataPtr)
    {
        if (inType != EVENT_CMD_DONE || ! inDataPtr) {
            panic("LogReceiver::Impl: unexpected event");
            return 0;
        }
        MetaLogWriterControl& theOp =
            *reinterpret_cast<MetaLogWriterControl*>(inDataPtr);
        const bool theWakeupFlag = ! IsAwake();
        if (mCompletionQueueTailPtr) {
            mCompletionQueueTailPtr->next = &theOp;
        } else {
            mCompletionQueueHeadPtr = &theOp;
        }
        mCompletionQueueTailPtr = &theOp;
        theOp.next = 0;
        if (theWakeupFlag) {
            Wakeup();
        }
        return 0;
    }
    void SubmitLogWrite(
        MetaLogWriterControl& inOp)
    {
        inOp.clnt = this;
        Submit(inOp);
    }
    void Submit(
        MetaRequest& inOp)
    {
        const bool theWakeupFlag = ! IsAwake();
        if (mPendingSubmitTailPtr) {
            mPendingSubmitTailPtr->next = &inOp;
        } else {
            mPendingSubmitHeadPtr = &inOp;
        }
        mPendingSubmitTailPtr = &inOp;
        inOp.next = 0;
        if (theWakeupFlag) {
            Wakeup();
        }
    }
    void Wakeup()
    {
        if (mReplayerPtr) {
            mReplayerPtr->Wakeup();
        } else {
            Dispatch();
        }
    }
private:
    typedef StBufferT<char, kMinParseBufferSize> ParseBuffer;

    int            mReAuthTimeout;
    int            mMaxReadAhead;
    int            mTimeout;
    int            mConnectionCount;
    int            mMaxConnectionCount;
    bool           mIpV6OnlyFlag;
    ServerLocation mListenerAddress;
    Acceptor*      mAcceptorPtr;
    AuthContext    mAuthContext;
    seq_t          mCommittedLogSeq;
    seq_t          mLastWriteSeq;
    bool           mDeleteFlag;
    bool           mAckBroadcastFlag;
    ParseBuffer    mParseBuffer;
    int64_t        mId;
    Replayer*      mReplayerPtr;
    MetaRequest*   mWriteOpFreeListPtr;
    MetaRequest*   mCompletionQueueHeadPtr;
    MetaRequest*   mCompletionQueueTailPtr;
    MetaRequest*   mPendingSubmitHeadPtr;
    MetaRequest*   mPendingSubmitTailPtr;
    Connection*    mConnectionsHeadPtr[1];

    ~Impl()
    {
        if (mConnectionCount != 0) {
            panic("LogReceiver::~Impl: invalid connection count");
        }
        if (mAcceptorPtr) {
            mAcceptorPtr->GetNetManager().UnRegisterTimeoutHandler(this);
        }
        delete mAcceptorPtr;
        ClearQueues();
    }
    void ClearQueues()
    {
        ClearQueue(mCompletionQueueHeadPtr, mCompletionQueueTailPtr);
        ClearQueue(mPendingSubmitHeadPtr,   mPendingSubmitTailPtr);
        ClearQueue(mWriteOpFreeListPtr,     mWriteOpFreeListPtr);
    }
    static void ClearQueue(
        MetaRequest*& ioHeadPtr,
        MetaRequest*& ioTailPtr)
    {
        MetaRequest* thePtr = ioHeadPtr;
        ioHeadPtr = 0;
        ioTailPtr = 0;
        while (thePtr) {
            MetaRequest* const theCurPtr = thePtr;
            thePtr = thePtr->next;
            theCurPtr->next = 0;
            MetaRequest::Release(theCurPtr);
        }
    }
    bool IsAwake() const
        { return (0 != mPendingSubmitTailPtr || 0 != mCompletionQueueTailPtr); }
    void BroadcastAck();
private:
    Impl(
        const Impl& inImpl);
    Impl& operator=(
        const Impl& inImpl);
};

class LogReceiver::Impl::Connection :
    public KfsCallbackObj,
    public SslFilterVerifyPeer
{
public:
    typedef Impl::List List;

    Connection(
        Impl&                   inImpl,
        const NetConnectionPtr& inConnectionPtr)
        : KfsCallbackObj(),
          SslFilterVerifyPeer(),
          mImpl(inImpl),
          mAuthName(),
          mSessionExpirationTime(0),
          mConnectionPtr(inConnectionPtr),
          mAuthenticateOpPtr(0),
          mAuthCount(0),
          mAuthCtxUpdateCount(0),
          mRecursionCount(0),
          mBlockLength(-1),
          mPendingOpsCount(0),
          mBlockChecksum(0),
          mBodyChecksum(0),
          mBlockStartSeq(-1),
          mBlockEndSeq(-1),
          mDownFlag(false),
          mIdSentFlag(false),
          mAuthPendingResponsesHeadPtr(0),
          mAuthPendingResponsesTailPtr(0),
          mIStream(),
          mOstream()
    {
        List::Init(*this);
        SET_HANDLER(this, &Connection::HandleEvent);
        if (! mConnectionPtr || ! mConnectionPtr->IsGood()) {
            panic("LogReceiver::Impl::Connection; invalid connection poiner");
        }
        mSessionExpirationTime = TimeNow() - int64_t(60) * 60 * 24 * 365 * 10;
        mConnectionPtr->SetInactivityTimeout(mImpl.GetTimeout());
        mConnectionPtr->SetMaxReadAhead(mImpl.GetMaxReadAhead());
        mImpl.New(*this);
    }
    ~Connection()
    {
        if (mRecursionCount != 0 || mConnectionPtr->IsGood() ||
                mPendingOpsCount != 0) {
            panic("LogReceiver::~Impl::Connection invalid invocation");
        }
        MetaRequest* thePtr = mAuthPendingResponsesHeadPtr;
        mAuthPendingResponsesHeadPtr = 0;
        mAuthPendingResponsesTailPtr = 0;
        while (thePtr) {
            MetaRequest& theReq = *thePtr;
            thePtr = theReq.next;
            theReq.next = 0;
            MetaRequest::Release(&theReq);
        }
        MetaRequest::Release(mAuthenticateOpPtr);
        mImpl.Done(*this);
        mRecursionCount  = 0xDEAD;
        mPendingOpsCount = 0xDEAD;
    }
    virtual bool Verify(
        string&       ioFilterAuthName,
        bool          inPreverifyOkFlag,
        int           inCurCertDepth,
        const string& inPeerName,
        int64_t       inEndTime,
        bool          inEndTimeValidFlag)
    {
        KFS_LOG_STREAM_DEBUG << GetPeerName() <<
            " log auth. verify:" <<
            " name: "           << inPeerName <<
            " prev: "           << ioFilterAuthName <<
            " preverify: "      << inPreverifyOkFlag <<
            " depth: "          << inCurCertDepth <<
            " end time: +"      << (inEndTime - time(0)) <<
            " end time valid: " << inEndTimeValidFlag <<
        KFS_LOG_EOM;
        // Do no allow to renegotiate and change the name.
        string theAuthName = inPeerName;
        if (! inPreverifyOkFlag ||
                (inCurCertDepth == 0 &&
                ((GetAuthContext().HasUserAndGroup() ?
                    GetAuthContext().GetUid(theAuthName) == kKfsUserNone :
                    ! GetAuthContext().RemapAndValidate(theAuthName)) ||
                (! mAuthName.empty() && theAuthName != mAuthName)))) {
            KFS_LOG_STREAM_ERROR << GetPeerName() <<
                " log receiver authentication failure:"
                " peer: "  << inPeerName <<
                " name: "  << theAuthName <<
                " depth: " << inCurCertDepth <<
                " is not valid" <<
                (mAuthName.empty() ? "" : "prev name: ") << mAuthName <<
            KFS_LOG_EOM;
            mAuthName.clear();
            ioFilterAuthName.clear();
            return false;
        }
        if (inCurCertDepth == 0) {
            ioFilterAuthName = inPeerName;
            mAuthName        = theAuthName;
            if (inEndTimeValidFlag && inEndTime < mSessionExpirationTime) {
                mSessionExpirationTime = inEndTime;
            }
        }
        return true;
    }
    int HandleEvent(
        int   inType,
        void* inDataPtr)
    {
        mRecursionCount++;
        QCASSERT(0 < mRecursionCount);
        switch (inType) {
            case EVENT_NET_READ:
                QCASSERT(&mConnectionPtr->GetInBuffer() == inDataPtr);
                HandleRead();
                break;
            case EVENT_NET_WROTE:
                if (mAuthenticateOpPtr) {
                    HandleAuthWrite();
                }
                break;
            case EVENT_CMD_DONE:
                if (! inDataPtr) {
                    panic("invalid null command completion");
                    break;
                }
                HandleCmdDone(*reinterpret_cast<MetaRequest*>(inDataPtr));
                break;
            case EVENT_NET_ERROR:
                if (HandleSslShutdown()) {
                    break;
                }
                Error("network error");
                break;
            case EVENT_INACTIVITY_TIMEOUT:
                Error("connection timed out");
                break;
            default:
                panic("LogReceiver: unexpected event");
                break;
        }
        mRecursionCount--;
        QCASSERT(0 <= mRecursionCount);
        if (mRecursionCount <= 0) {
            if (mConnectionPtr->IsGood()) {
                mConnectionPtr->StartFlush();
            } else if (! mDownFlag) {
                Error();
            }
            if (mDownFlag && mPendingOpsCount <= 0) {
                delete this;
            }
        }
        return 0;
    }
    void SendAck()
    {
        QCASSERT(0 == mRecursionCount);
        if (! mConnectionPtr) {
            return;
        }
        SendAck();
    }
private:
    typedef MetaLogWriterControl::Lines Lines;
    typedef uint32_t                    Checksum;

    Impl&                  mImpl;
    string                 mAuthName;
    int64_t                mSessionExpirationTime;
    NetConnectionPtr const mConnectionPtr;
    MetaAuthenticate*      mAuthenticateOpPtr;
    int64_t                mAuthCount;
    uint64_t               mAuthCtxUpdateCount;
    int                    mRecursionCount;
    int                    mBlockLength;
    int                    mPendingOpsCount;
    Checksum               mBlockChecksum;
    Checksum               mBodyChecksum;
    int64_t                mBlockStartSeq;
    int64_t                mBlockEndSeq;
    bool                   mDownFlag;
    bool                   mIdSentFlag;
    MetaRequest*           mAuthPendingResponsesHeadPtr;
    MetaRequest*           mAuthPendingResponsesTailPtr;
    IOBuffer::IStream      mIStream;
    IOBuffer::WOStream     mOstream;
    Connection*            mPrevPtr[1];
    Connection*            mNextPtr[1];

    friend class QCDLListOp<Connection>;

    string GetPeerName()
        { return mConnectionPtr->GetPeerName(); }
    time_t TimeNow()
        { return mConnectionPtr->TimeNow(); }
    AuthContext& GetAuthContext()
        { return mImpl.GetAuthContext(); }
    int Authenticate(
        IOBuffer& inBuffer)
    {
        if (! mAuthenticateOpPtr) {
            return 0;
        }
        if (mAuthenticateOpPtr->doneFlag) {
            if (mConnectionPtr->GetFilter()) {
                HandleEvent(EVENT_NET_WROTE, &mConnectionPtr->GetOutBuffer());
            }
            return 0;
        }
        if (mAuthenticateOpPtr->contentBufPos <= 0) {
            GetAuthContext().Validate(*mAuthenticateOpPtr);
        }
        const int theRem = mAuthenticateOpPtr->Read(inBuffer);
        if (0 < theRem) {
            mConnectionPtr->SetMaxReadAhead(theRem);
            return theRem;
        }
        if (! inBuffer.IsEmpty() && mAuthenticateOpPtr->status == 0) {
            mAuthenticateOpPtr->status    = -EINVAL;
            mAuthenticateOpPtr->statusMsg = "out of order data received";
        }
        GetAuthContext().Authenticate(*mAuthenticateOpPtr, this, 0);
        if (mAuthenticateOpPtr->status == 0) {
            if (mAuthName.empty()) {
                mAuthName = mAuthenticateOpPtr->authName;
            } else if (! mAuthenticateOpPtr->authName.empty() &&
                    mAuthName != mAuthenticateOpPtr->authName) {
                mAuthenticateOpPtr->status    = -EINVAL;
                mAuthenticateOpPtr->statusMsg = "authenticated name mismatch";
            } else if (! mAuthenticateOpPtr->filter &&
                    mConnectionPtr->GetFilter()) {
                // An attempt to downgrade to clear text connection.
                mAuthenticateOpPtr->status    = -EINVAL;
                mAuthenticateOpPtr->statusMsg =
                    "clear text communication not allowed";
            }
        }
        mAuthenticateOpPtr->doneFlag = true;
        mAuthCtxUpdateCount = GetAuthContext().GetUpdateCount();
        KFS_LOG_STREAM(mAuthenticateOpPtr->status == 0 ?
            MsgLogger::kLogLevelINFO : MsgLogger::kLogLevelERROR) <<
            GetPeerName()           << " log receiver authentication"
            " type: "               << mAuthenticateOpPtr->sendAuthType <<
            " name: "               << mAuthenticateOpPtr->authName <<
            " filter: "             <<
                reinterpret_cast<const void*>(mAuthenticateOpPtr->filter) <<
            " session expires in: " <<
                (mAuthenticateOpPtr->sessionExpirationTime - TimeNow()) <<
            " response length: "    << mAuthenticateOpPtr->sendContentLen <<
            " msg: "                << mAuthenticateOpPtr->statusMsg <<
        KFS_LOG_EOM;
        mConnectionPtr->SetMaxReadAhead(mImpl.GetMaxReadAhead());
        SendResponse(*mAuthenticateOpPtr);
        return (mDownFlag ? -1 : 0);
    }
    void HandleAuthWrite()
    {
        if (! mAuthenticateOpPtr) {
            return;
        }
        if (mConnectionPtr->IsWriteReady()) {
            return;
        }
        if (mAuthenticateOpPtr->status != 0 ||
                mConnectionPtr->HasPendingRead() ||
                mConnectionPtr->IsReadPending()) {
            const string theMsg = mAuthenticateOpPtr->statusMsg;
            Error(theMsg.empty() ?
                (mConnectionPtr->HasPendingRead() ?
                    "out of order data received" :
                    "authentication error") :
                theMsg.c_str()
            );
            return;
        }
        if (mConnectionPtr->GetFilter()) {
            if (! mAuthenticateOpPtr->filter) {
                Error("no clear text communication allowed");
            }
            // Wait for [ssl] shutdown with the current filter to complete.
            return;
        }
        if (mAuthenticateOpPtr->filter) {
            NetConnection::Filter* const theFilterPtr =
                mAuthenticateOpPtr->filter;
            mAuthenticateOpPtr->filter = 0;
            string    theErrMsg;
            const int theErr = mConnectionPtr->SetFilter(
                theFilterPtr, &theErrMsg);
            if (theErr) {
                if (theErrMsg.empty()) {
                    theErrMsg = QCUtils::SysError(theErr < 0 ? -theErr : theErr);
                }
                Error(theErrMsg.c_str());
                return;
            }
        }
        mSessionExpirationTime = mAuthenticateOpPtr->sessionExpirationTime;
        MetaRequest::Release(mAuthenticateOpPtr);
        mAuthenticateOpPtr  = 0;
        KFS_LOG_STREAM_INFO << GetPeerName() <<
            (0 < mAuthCount ? " re-" : " ") <<
            "authentication [" << mAuthCount << "]"
            " complete:"
            " session expires in: " <<
                (mSessionExpirationTime - TimeNow()) << " sec." <<
        KFS_LOG_EOM;
        mAuthCount++;
        MetaRequest* thePtr = mAuthPendingResponsesHeadPtr;
        mAuthPendingResponsesHeadPtr = 0;
        mAuthPendingResponsesTailPtr = 0;
        while (thePtr && ! mDownFlag) {
            MetaRequest& theReq = *thePtr;
            thePtr = theReq.next;
            theReq.next = 0;
            SendResponse(theReq);
            MetaRequest::Release(&theReq);
        }
    }
    bool HandleSslShutdown()
    {
        NetConnection::Filter* theFilterPtr;
        if (mDownFlag ||
                ! mAuthenticateOpPtr ||
                ! mConnectionPtr->IsGood() ||
                ! (theFilterPtr = mConnectionPtr->GetFilter()) ||
                ! theFilterPtr->IsShutdownReceived()) {
            return false;
        }
        // Do not allow to shutdown filter with data in flight.
        if (mConnectionPtr->GetInBuffer().IsEmpty() &&
                mConnectionPtr->GetOutBuffer().IsEmpty()) {
            // Ssl shutdown from the other side.
            if (mConnectionPtr->Shutdown() != 0) {
                return false;
            }
            KFS_LOG_STREAM_DEBUG << GetPeerName() <<
                " log receiver: shutdown filter: " <<
                    reinterpret_cast<const void*>(
                        mConnectionPtr->GetFilter()) <<
            KFS_LOG_EOM;
            if (mConnectionPtr->GetFilter()) {
                return false;
            }
            HandleAuthWrite();
            return (! mDownFlag);
        }
        KFS_LOG_STREAM_ERROR << GetPeerName() <<
            " log receiver: "
            " invalid filter (ssl) shutdown: "
            " error: " << mConnectionPtr->GetErrorMsg() <<
            " read: "  << mConnectionPtr->GetNumBytesToRead() <<
            " write: " << mConnectionPtr->GetNumBytesToWrite() <<
        KFS_LOG_EOM;
        return false;
    }
    void HandleRead()
    {
        IOBuffer& theBuf = mConnectionPtr->GetInBuffer();
        if (mAuthenticateOpPtr) {
            Authenticate(theBuf);
            if (mAuthenticateOpPtr || mDownFlag) {
                return;
            }
        }
        if (0 < mBlockLength) {
            const int theRet = ReceiveBlock(theBuf);
            if (0 != theRet || mDownFlag) {
                return;
            }
        }
        bool theMsgAvailableFlag;
        int  theMsgLen = 0;
        while ((theMsgAvailableFlag = IsMsgAvail(&theBuf, &theMsgLen))) {
            const int theRet = HandleMsg(theBuf, theMsgLen);
            if (theRet < 0) {
                theBuf.Clear();
                Error(mAuthenticateOpPtr ?
                    (mAuthenticateOpPtr->statusMsg.empty() ?
                        "invalid authenticate message" :
                        mAuthenticateOpPtr->statusMsg.c_str()) :
                    "request parse error"
                );
                return;
            }
            if (0 < theRet || mAuthenticateOpPtr || mDownFlag) {
                return; // Need more data, or down
            }
            theMsgLen = 0;
        }
        if (mBlockLength < 0 && ! mAuthenticateOpPtr && ! mDownFlag &&
                MAX_RPC_HEADER_LEN < theBuf.BytesConsumable()) {
            Error("header size exceeds max allowed");
        }
    }
    void HandleCmdDone(
        MetaRequest& inReq)
    {
        if (mPendingOpsCount <= 0) {
            panic("invalid outstanding ops count");
            return;
        }
        if (inReq.next) {
            panic("invalid request next field");
        }
        mPendingOpsCount--;
        if (mAuthenticateOpPtr && ! mDownFlag) {
            if (mAuthPendingResponsesTailPtr) {
                mAuthPendingResponsesTailPtr->next = &inReq;
            } else {
                mAuthPendingResponsesHeadPtr = &inReq;
            }
            mAuthPendingResponsesTailPtr = &inReq;
        }
        SendResponse(inReq);
        MetaRequest::Release(&inReq);
    }
    void SendResponse(
        MetaRequest& inReq)
    {
        if (mDownFlag) {
            return;
        }
        IOBuffer& theBuf = mConnectionPtr->GetOutBuffer();
        ReqOstream theStream(mOstream.Set(theBuf));
        inReq.response(theStream, theBuf);
        mOstream.Reset();
        if (mRecursionCount <= 0) {
            mConnectionPtr->StartFlush();
        }
    }
    int HandleMsg(
        IOBuffer& inBuffer,
        int       inMsgLen)
    {
        const int kSeparatorLen = 4;
        const int kPrefixLen    = 2;
        if (kSeparatorLen + kPrefixLen < inMsgLen) {
            int               theLen       = inMsgLen - kSeparatorLen;
            const char* const theHeaderPtr = inBuffer.CopyOutOrGetBufPtr(
                mImpl.GetParseBufferPtr(), theLen);
            QCRTASSERT(inMsgLen - kSeparatorLen == theLen);
            const char*       thePtr    = theHeaderPtr;
            const char* const theEndPtr = thePtr + inMsgLen - kSeparatorLen;
            if ('l' == (*thePtr++ & 0xFF) && ':' == (*thePtr++ & 0xFF) &&
                    HexIntParser::Parse(
                        thePtr, theEndPtr - thePtr, mBlockLength) &&
                    HexIntParser::Parse(
                        thePtr, theEndPtr - thePtr, mBlockChecksum)) {
                if (IsAuthError()) {
                    return -1;
                }
                if (mBlockLength < 0) {
                    Error("invalid negative block lenght");
                    return -1;
                }
                inBuffer.Consume(inMsgLen);
                return ReceiveBlock(inBuffer);
            }
        }
        MetaRequest* theReqPtr = 0;
        if (ParseLogRecvCommand(
                    inBuffer,
                    inMsgLen,
                    &theReqPtr,
                    mImpl.GetParseBufferPtr()) ||
                ! theReqPtr) {
            MetaRequest::Release(theReqPtr);
            const string thePrefix = GetPeerName() + " invalid request: ";
            MsgLogLines(
                MsgLogger::kLogLevelERROR,
                thePrefix.c_str(),
                inBuffer,
                inMsgLen
            );
            return -1;
        }
        inBuffer.Consume(inMsgLen);
        if (META_AUTHENTICATE == theReqPtr->op) {
            mAuthenticateOpPtr = static_cast<MetaAuthenticate*>(theReqPtr);
            return Authenticate(inBuffer);
        }
        if (IsAuthError()) {
            MetaRequest::Release(theReqPtr);
            return -1;
        }
        mPendingOpsCount++;
        mImpl.Submit(*theReqPtr);
        return 0;
    }
    int ReceiveBlock(
        IOBuffer& inBuffer)
    {
        if (mBlockLength < 0) {
            return -1;
        }
        const int theRem = mBlockLength - inBuffer.BytesConsumable();
        if (0 < theRem) {
            mConnectionPtr->SetMaxReadAhead(
                max(theRem, mImpl.GetMaxReadAhead()));
            return theRem;
        }
        int         theMaxHdrLen    =
            min(mBlockLength, (int)kMaxBlockHeaderLen);
        const char* theStartPtr     = inBuffer.CopyOutOrGetBufPtr(
                mImpl.GetParseBufferPtr(), theMaxHdrLen);
        const char* const theEndPtr = theStartPtr + theMaxHdrLen;
        int64_t     theBlockEndSeq  = -1;
        int         theBlockSeqLen  = -1;
        const char* thePtr          = theStartPtr;
        if (! HexIntParser::Parse(
                thePtr, theEndPtr - thePtr, theBlockEndSeq) ||
                ! HexIntParser::Parse(
                    thePtr, theEndPtr - thePtr, theBlockSeqLen) ||
                theBlockSeqLen < 0 ||
                theBlockEndSeq < theBlockSeqLen) {
            KFS_LOG_STREAM_ERROR << GetPeerName() <<
                " invalid block:"
                " start: "    << mBlockStartSeq <<
                " / "         << theBlockEndSeq <<
                " end: "      << mBlockEndSeq <<
                " / "         << theBlockSeqLen <<
                " length: "   << mBlockLength <<
            KFS_LOG_EOM;
            MsgLogLines(MsgLogger::kLogLevelERROR,
                "invalid block: ", inBuffer, mBlockLength);
            Error("invalid block");
            return -1;
        }
        while (thePtr < theEndPtr && (*thePtr & 0xFF) <= ' ') {
            thePtr++;
        }
        if (theEndPtr <= thePtr && theMaxHdrLen < mBlockLength) {
            KFS_LOG_STREAM_ERROR << GetPeerName() <<
                " invalid block header:" << theBlockEndSeq  <<
                " length: "              << mBlockLength <<
            KFS_LOG_EOM;
            MsgLogLines(MsgLogger::kLogLevelERROR,
                "invalid block header: ", inBuffer, mBlockLength);
            Error("invalid block header");
            return -1;
        }
        const int      theHdrLen       = (int)(thePtr - theStartPtr);
        const Checksum theHdrChecksum  =
            ComputeBlockChecksum(&inBuffer, theHdrLen);
        mBlockLength -= inBuffer.Consume(theHdrLen);
        mBodyChecksum = ComputeBlockChecksum(&inBuffer, mBlockLength);
        const Checksum theChecksum     = ChecksumBlocksCombine(
            theHdrChecksum, mBodyChecksum, mBlockLength);
        if (theChecksum != mBlockChecksum) {
            KFS_LOG_STREAM_ERROR << GetPeerName() <<
                " received block checksum: " << theChecksum <<
                " expected: "                << mBlockChecksum <<
                " length: "                  << mBlockLength <<
            KFS_LOG_EOM;
            Error("block checksum mimatch");
            return -1;
        }
        if (mBlockLength <= 0) {
            SendAckSelf();
            if (! mDownFlag) {
                mConnectionPtr->SetMaxReadAhead(mImpl.GetMaxReadAhead());
            }
            return (mDownFlag ? -1 : 0);
        }
        if (theBlockEndSeq < 0  ||
                    (0 <= mBlockEndSeq && theBlockEndSeq < mBlockEndSeq)) {
            KFS_LOG_STREAM_ERROR << GetPeerName() <<
                " invalid block:"
                " sequence: " << theBlockEndSeq <<
                " last: "     << mBlockEndSeq <<
                " length: "   << mBlockLength <<
            KFS_LOG_EOM;
            MsgLogLines(MsgLogger::kLogLevelERROR,
                "invalid block sequence: ", inBuffer, mBlockLength);
            Error("invalid block sequence");
            return -1;
        }
        mBlockEndSeq   = theBlockEndSeq;
        mBlockStartSeq = theBlockEndSeq - theBlockSeqLen;
        return ProcessBlock(inBuffer);
    }
    void Error(
        const char* inMsgPtr = 0)
    {
        if (mDownFlag) {
            return;
        }
        KFS_LOG_STREAM_ERROR << GetPeerName() <<
            " error: " << (inMsgPtr ? inMsgPtr : "")  <<
            " closing connection"
            " last block end: " << mBlockEndSeq <<
            " socket error: "   << mConnectionPtr->GetErrorMsg() <<
        KFS_LOG_EOM;
        mConnectionPtr->Close();
        mDownFlag = true;
    }
    void SendAckSelf()
    {
        if (mAuthenticateOpPtr) {
            return;
        }
        bool theReAuthFlag;
        if (GetAuthContext().IsAuthRequired()) {
            uint64_t const theUpdateCount = GetAuthContext().GetUpdateCount();
            theReAuthFlag = theUpdateCount != mAuthCtxUpdateCount ||
                mSessionExpirationTime < TimeNow() + mImpl.GetReAuthTimeout();
            if (theReAuthFlag) {
                KFS_LOG_STREAM_INFO << GetPeerName() <<
                    " requesting re-authentication:"
                    " update count: " << theUpdateCount <<
                    " / "             << mAuthCtxUpdateCount <<
                    " expires in: "   << (mSessionExpirationTime - TimeNow()) <<
                    " timeout: "      << mImpl.GetReAuthTimeout() <<
                KFS_LOG_EOM;
            }
        } else {
            theReAuthFlag = false;
        }
        uint64_t theAckFlags = 0;
        if (theReAuthFlag) {
            theAckFlags |= uint64_t(1) << kLogBlockAckReAuthFlagBit;
        }
        if (! mIdSentFlag) {
            theAckFlags |= uint64_t(1) << kLogBlockAckHasServerIdBit;
        }
        IOBuffer& theBuf = mConnectionPtr->GetOutBuffer();
        const int thePos = theBuf.BytesConsumable();
        ReqOstream theStream(mOstream.Set(theBuf));
        const seq_t theCommitted = mImpl.GetCommittedLogSeq();
        const seq_t theLastWrite = mImpl.GetLastWriteLogSeq();
        theStream << hex <<
            "A " << theCommitted <<
            " "  << max(seq_t(0), theLastWrite - theCommitted) <<
            " "  << theAckFlags;
        if (! mIdSentFlag) {
            mIdSentFlag = true;
            theStream << " " << mImpl.GetId() << " ";
            theStream.flush();
            const Checksum theChecksum = ComputeBlockChecksumAt(
                &theBuf, thePos, theBuf.BytesConsumable() - thePos);
            theStream << theChecksum;
        }
        theStream << "\r\n\r\n";
        theStream.flush();
        if (mRecursionCount <= 0) {
            mConnectionPtr->StartFlush();
        }
    }
    int ProcessBlock(
        IOBuffer& inBuffer)
    {
        if (mBlockLength < 0) {
            Error("invalid negative block length");
            return -1;
        }
        MetaLogWriterControl* const theOpPtr =
            0 < mBlockLength ? mImpl.GetBlockWriteOp(
                mBlockStartSeq, mBlockEndSeq) : 0;
        if (theOpPtr) {
            MetaLogWriterControl& theOp = *theOpPtr;
            Lines& theLines = theOp.blockLines;
            theLines.Clear();
            int  theRem        = mBlockLength;
            bool theAppendFlag = false;
            int  theLastSym    = 0;
            for (IOBuffer::iterator theIt = inBuffer.begin();
                    0 < theRem && theIt != inBuffer.end();
                    ++theIt) {
                const char* const theStartPtr = theIt->Consumer();
                const char* const theEndPtr   =
                    min(theIt->Producer(), theStartPtr + theRem);
                if (theEndPtr <= theStartPtr) {
                    continue;
                }
                theRem -= theEndPtr - theStartPtr;
                const char* thePtr  = theStartPtr;
                const char* theNPtr = thePtr;
                while (thePtr < theEndPtr &&
                        (theNPtr = reinterpret_cast<const char*>(
                            memchr(thePtr, '\n', theEndPtr - thePtr)))) {
                    ++theNPtr;
                    const int theLen = (int)(theNPtr - thePtr);
                    if (theAppendFlag) {
                        theAppendFlag = false;
                        theLines.Back() += theLen;
                    } else {
                        theLines.Append(theLen);
                    }
                    thePtr = theNPtr;
                }
                if (thePtr < theEndPtr) {
                    const int theLen = (int)(theNPtr - thePtr);
                    if (theAppendFlag) {
                        theLines.Back() += theLen;
                    } else {
                        theLines.Append(theLen);
                    }
                    theAppendFlag = true;
                    theLastSym = theEndPtr[-1] & 0xFF;
                }
            }
            if (theRem != 0) {
                panic("LogReceiver::Impl::Connection::ProcessBlock:"
                    " internal error");
                mImpl.Release(theOp);
                return -1;
            }
            if (! theAppendFlag || theLastSym != '/') {
                const char* theMsgPtr =
                    "invalid log block format: no trailing /";
                KFS_LOG_STREAM_ERROR <<
                    theMsgPtr <<
                    " lines: " << theLines.GetSize() <<
                KFS_LOG_EOM;
                mImpl.Release(theOp);
                Error(theMsgPtr);
                return -1;
            }
            theOp.blockChecksum = mBodyChecksum;
            theOp.blockStartSeq = mBlockStartSeq;
            theOp.blockEndSeq   = mBlockEndSeq;
            theOp.blockData.Move(&inBuffer, mBlockLength);
            mImpl.SubmitLogWrite(theOp);
        } else {
            inBuffer.Consume(mBlockLength);
        }
        mBlockLength = -1;
        SendAckSelf();
        mConnectionPtr->SetMaxReadAhead(mImpl.GetMaxReadAhead());
        return 0;
    }
    void MsgLogLines(
        MsgLogger::LogLevel inLogLevel,
        const char*         inPrefixPtr,
        IOBuffer&           inBuffer,
        int                 inBufLen,
        int                 inMaxLines = 64)
    {
        const char* const thePrefixPtr = inPrefixPtr ? inPrefixPtr : "";
        istream&          theStream    = mIStream.Set(inBuffer, inBufLen);
        int               theRemCnt    = inMaxLines;
        string            theLine;
        while (--theRemCnt >= 0 && getline(theStream, theLine)) {
            string::iterator theIt = theLine.end();
            if (theIt != theLine.begin() && *--theIt <= ' ') {
                theLine.erase(theIt);
            }
            KFS_LOG_STREAM(inLogLevel) <<
                thePrefixPtr << theLine <<
            KFS_LOG_EOM;
        }
        mIStream.Reset();
    }
    bool IsAuthError()
    {
        if (mAuthName.empty() && GetAuthContext().IsAuthRequired()) {
            Error("autentication required");
            return true;
        }
        return false;
    }
private:
    Connection(
        const Connection& inConnection);
    Connection& operator=(
        const Connection& inConnection);
};

    /* virtual */ KfsCallbackObj*
LogReceiver::Impl::CreateKfsCallbackObj(
    NetConnectionPtr& inConnectionPtr)
{
    if (! inConnectionPtr || ! inConnectionPtr->IsGood()) {
        return 0;
    }
    if (mMaxConnectionCount <= mConnectionCount) {
        KFS_LOG_STREAM_ERROR <<
            "log receiver: reached connections limit"
            " of: "                 << mMaxConnectionCount <<
            " connections: "        << mConnectionCount <<
            " closing connection: " << inConnectionPtr->GetPeerName() <<
        KFS_LOG_EOM;
        return 0;
    }
    return new Connection(*this, inConnectionPtr);
}

    void
LogReceiver::Impl::New(
    Connection& inConnection)
{
    mConnectionCount++;
    List::PushBack(mConnectionsHeadPtr, inConnection);
    if (mConnectionCount <= 0) {
        panic("LogReceiver::Impl::New: invalid connections count");
    }
}

    void
LogReceiver::Impl::Done(
    Connection& inConnection)
{
    if (mConnectionCount <= 0) {
        panic("LogReceiver::Impl::Done: invalid connections count");
    }
    List::Remove(mConnectionsHeadPtr, inConnection);
    mConnectionCount--;
    if (mDeleteFlag && mConnectionCount <= 0) {
        delete this;
    }
}

    void
LogReceiver::Impl::Shutdown()
{
    if (mAcceptorPtr) {
        mAcceptorPtr->GetNetManager().UnRegisterTimeoutHandler(this);
    }
    delete mAcceptorPtr;
    mAcceptorPtr = 0;
    List::Iterator theIt(mConnectionsHeadPtr);
    Connection* thePtr;
    while (0 < mConnectionCount && (thePtr = theIt.Next())) {
        thePtr->HandleEvent(EVENT_NET_ERROR, 0);
    }
    mAckBroadcastFlag = false;
    ClearQueues();
}

    void
LogReceiver::Impl::BroadcastAck()
{
    List::Iterator theIt(mConnectionsHeadPtr);
    Connection* thePtr;
    while (0 < mConnectionCount && (thePtr = theIt.Next())) {
        thePtr->SendAck();
    }
}

LogReceiver::LogReceiver()
    : mImpl(*(new Impl()))
{
}

LogReceiver::~LogReceiver()
{
    mImpl.Delete();
}

    bool
LogReceiver::SetParameters(
    const char*       inPrefixPtr,
    const Properties& inParameters)
{
    return mImpl.SetParameters(inPrefixPtr, inParameters);
}

    int
LogReceiver::Start(
    NetManager&            inNetManager,
    LogReceiver::Replayer& inReplayer)
{
    return mImpl.Start(inNetManager, inReplayer);
}

    void
LogReceiver::Shutdown()
{
    mImpl.Shutdown();
}

    void
LogReceiver::Dispatch()
{
    mImpl.Dispatch();
}

} // namespace KFS