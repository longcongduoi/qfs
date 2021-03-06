//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2007/01/17
// Author: Sriram Rao
//         Mike Ovsiannikov -- rework re-replication to protect against
// duplicate requests. Implement chunk recovery.
//
// Copyright 2008-2012 Quantcast Corp.
// Copyright 2007-2008 Kosmix Corp.
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
// \brief Code for dealing with chunk re-replication and recovery.
// The meta server instructs chunk server to obtain a copy of a chunk from a
// source chunk server, or recover chunk by reading other available chunks in
// the RS block and recomputing the missing chunk data. The chunk server reads
// the chunk data from the other chunk server(s) writes chunk replica to disk.
// At the end replication, the destination chunk server notifies the meta
// server.
//
//----------------------------------------------------------------------------

#include "Replicator.h"
#include "ChunkServer.h"
#include "utils.h"
#include "RemoteSyncSM.h"
#include "KfsOps.h"
#include "Logger.h"
#include "BufferManager.h"
#include "DiskIo.h"

#include "common/MsgLogger.h"
#include "common/StdAllocator.h"
#include "qcdio/qcstutils.h"
#include "kfsio/KfsCallbackObj.h"
#include "kfsio/NetConnection.h"
#include "kfsio/Globals.h"
#include "kfsio/checksum.h"
#include "libclient/KfsNetClient.h"
#include "libclient/Reader.h"
#include "libclient/KfsOps.h"

#include <string>
#include <sstream>

namespace KFS
{

using std::string;
using std::string;
using std::ostringstream;
using std::istringstream;
using std::pair;
using std::make_pair;
using std::max;
using std::min;
using KFS::libkfsio::globalNetManager;
using KFS::client::Reader;
using KFS::client::KfsNetClient;

class ReplicatorImpl :
    public KfsCallbackObj,
    public QCRefCountedObj,
    public BufferManager::Client
{
public:
    // Model for doing a chunk replication involves 3 steps:
    //  - First, figure out the size of the chunk.
    //  - Second in a loop:
    //        - read N bytes from the source
    //        - write N bytes to disk
    // - Third, notify the metaserver of the status (0 to mean
    // success, -1 on failure).
    //
    // During replication, the chunk isn't part of the chunkTable data
    // structure that is maintained locally.  This is done for
    // simplifying failure handling: if we die in the midst of
    // replication, upon restart, we will find an incomplete chunk, i.e.
    // chunk with with 0 version in the the dirty directory. Such chunks
    // will be deleted upon restart.
    //
    typedef Replicator::Counters Counters;
    static int GetNumReplications();
    static void CancelAll();
    static void SetParameters(const Properties& props)
    {
        sUseConnectionPoolFlag = props.getValue(
            "chunkServer.rsReader.meta.idleTimeoutSec",
            sUseConnectionPoolFlag ? 1 : 0
        ) != 0;
    }
    static void GetCounters(Replicator::Counters& counters);

    ReplicatorImpl(ReplicateChunkOp *op, const RemoteSyncSMPtr &peer);
    void Run();
    // Handle the callback for a size request
    int HandleStartDone(int code, void *data);
    // Handle the callback for a remote read request
    int HandleReadDone(int code, void *data);
    // Handle the callback for a write
    int HandleWriteDone(int code, void *data);
    // When replication done, we write out chunk meta-data; this is
    // the handler that gets called when this event is done.
    int HandleReplicationDone(int code, void *data);
    virtual void Granted(ByteCount byteCount);
    static Counters& Ctrs()
        { return sCounters; };
    static bool GetUseConnectionPoolFlag()
        { return sUseConnectionPoolFlag; }

protected:
    // Inputs from the metaserver
    kfsFileId_t        mFileId;
    kfsChunkId_t       mChunkId;
    kfsSeq_t           mChunkVersion;
    // What we obtain from the src from where we download the chunk.
    int64_t            mChunkSize;
    // The op that triggered this replication operation.
    ReplicateChunkOp*  mOwner;
    // What is the offset we are currently reading at
    int64_t            mOffset;
    // Handle to the peer from where we have to get data
    RemoteSyncSMPtr    mPeer;

    GetChunkMetadataOp mChunkMetadataOp;
    ReadOp             mReadOp;
    WriteOp            mWriteOp;
    // Are we done yet?
    bool               mDone;
    bool               mCancelFlag;

    virtual ~ReplicatorImpl();
    // Cleanup...
    void Terminate();
    string GetPeerName() const;
    // Start by sending out a size request
    virtual void Start();
    // Send out a read request to the peer
    virtual void Read();
    virtual void Cancel()
    {
        mCancelFlag = true;
        if (IsWaiting()) {
            // Cancel buffers wait, and fail the op.
            CancelRequest();
            Terminate();
        }
    }
    virtual ByteCount GetBufferBytesRequired() const;

private:
    typedef std::map<
        kfsChunkId_t, ReplicatorImpl*,
        std::less<kfsChunkId_t>,
        StdFastAllocator<
            std::pair<const kfsChunkId_t, ReplicatorImpl*>
        >
    > InFlightReplications;

    static InFlightReplications sInFlightReplications;
    static Counters             sCounters;
    static int                  sReplicationCount;
    static bool                 sUseConnectionPoolFlag;
private:
    // No copy.
    ReplicatorImpl(const ReplicatorImpl&);
    ReplicatorImpl& operator=(const ReplicatorImpl&);
};

const int kDefaultReplicationReadSize = (int)(
    ((1 << 20) + CHECKSUM_BLOCKSIZE - 1) /
    CHECKSUM_BLOCKSIZE * CHECKSUM_BLOCKSIZE);
ReplicatorImpl::InFlightReplications ReplicatorImpl::sInFlightReplications;
ReplicatorImpl::Counters             ReplicatorImpl::sCounters;
int                                  ReplicatorImpl::sReplicationCount = 0;
bool ReplicatorImpl::sUseConnectionPoolFlag = false;

int
ReplicatorImpl::GetNumReplications()
{
    if (sInFlightReplications.empty()) {
        sReplicationCount = 0;
    }
    return sReplicationCount;
}

void
ReplicatorImpl::CancelAll()
{
    for (InFlightReplications::iterator it = sInFlightReplications.begin();
            it != sInFlightReplications.end();
            ++it) {
        it->second->Cancel();
    }
    sReplicationCount = 0;
}

void ReplicatorImpl::GetCounters(ReplicatorImpl::Counters& counters)
{
    counters = sCounters;
}

ReplicatorImpl::ReplicatorImpl(ReplicateChunkOp *op, const RemoteSyncSMPtr &peer) :
    KfsCallbackObj(),
    QCRefCountedObj(),
    BufferManager::Client(),
    mFileId(op->fid),
    mChunkId(op->chunkId),
    mChunkVersion(op->chunkVersion),
    mOwner(op),
    mOffset(0),
    mPeer(peer),
    mChunkMetadataOp(0),
    mReadOp(0),
    mWriteOp(op->chunkId, op->chunkVersion),
    mDone(false),
    mCancelFlag(false)
{
    mReadOp.chunkId = op->chunkId;
    mReadOp.chunkVersion = op->chunkVersion;
    mReadOp.clnt = this;
    mWriteOp.clnt = this;
    mChunkMetadataOp.clnt = this;
    mWriteOp.Reset();
    mWriteOp.isFromReReplication = true;
    SET_HANDLER(&mReadOp, &ReadOp::HandleReplicatorDone);
    Ctrs().mReplicatorCount++;
}

ReplicatorImpl::~ReplicatorImpl()
{
    InFlightReplications::iterator const it =
        sInFlightReplications.find(mChunkId);
    if (it != sInFlightReplications.end() && it->second == this) {
        if (! mCancelFlag && sReplicationCount > 0) {
            sReplicationCount--;
        }
        sInFlightReplications.erase(it);
    }
    assert(! mOwner && Ctrs().mReplicatorCount > 0);
    Ctrs().mReplicatorCount--;
}

void
ReplicatorImpl::Run()
{
    pair<InFlightReplications::iterator, bool> const ret =
        sInFlightReplications.insert(make_pair(mChunkId, this));
    if (ret.second) {
        sReplicationCount++;
    } else {
        assert(ret.first->second && ret.first->second != this);
        ReplicatorImpl& other = *ret.first->second;
        KFS_LOG_STREAM_INFO << "replication:"
            " chunk: "   << ret.first->first <<
            " peer: "    << other.GetPeerName() <<
            " offset: "  << other.mOffset <<
            "canceling:" <<
            (other.mCancelFlag ? " already canceled?" : "") <<
            " restarting from"
            " peer: "    << GetPeerName() <<
        KFS_LOG_EOM;
        other.Cancel();
        // Cancel can delete the "other" replicator if it was waiting for
        // buffers for example, and make the iterator invalid.
        pair<InFlightReplications::iterator, bool> const res =
            sInFlightReplications.insert(make_pair(mChunkId, this));
        if (! res.second) {
            assert(ret == res);
            res.first->second = this;
        }
        if (mCancelFlag) {
            // Non debug version -- an attempt to restart? &other == this
            // Delete chunk and declare error.
            mCancelFlag = false;
            Terminate();
            return;
        }
    }

    const ByteCount kChunkHeaderSize = 16 << 10;
    const ByteCount bufBytes = max(kChunkHeaderSize, GetBufferBytesRequired());
    BufferManager&  bufMgr   = DiskIo::GetBufferManager();
    if (bufMgr.IsOverQuota(*this, bufBytes)) {
        KFS_LOG_STREAM_ERROR << "replication:"
            " chunk: "      << mChunkId <<
            " peer: "       << GetPeerName() <<
            " bytes: "      << bufBytes <<
            " total: "      << GetByteCount() <<
            " over quota: " << bufMgr.GetMaxClientQuota() <<
        KFS_LOG_EOM;
        Terminate();
        return;
    }
    if (bufMgr.GetForDiskIo(*this, bufBytes)) {
        Start();
        return;
    }
    KFS_LOG_STREAM_INFO << "replication:"
        " chunk: "     << mChunkId <<
        " peer: "      << GetPeerName() <<
        " denined: "   << bufBytes <<
        " waiting for buffers" <<
    KFS_LOG_EOM;
}

ReplicatorImpl::ByteCount
ReplicatorImpl::GetBufferBytesRequired() const
{
    return kDefaultReplicationReadSize;
}

void
ReplicatorImpl::Granted(ByteCount byteCount)
{
    KFS_LOG_STREAM_INFO << "replication:" 
        " chunk: "   << mChunkId <<
        " peer: "    << GetPeerName() <<
        " granted: " << byteCount <<
    KFS_LOG_EOM;
    Start();
}

void
ReplicatorImpl::Start()
{
    assert(mPeer);

    mChunkMetadataOp.chunkId = mChunkId;
    mChunkMetadataOp.readVerifyFlag = false;
    SET_HANDLER(this, &ReplicatorImpl::HandleStartDone);
    mPeer->Enqueue(&mChunkMetadataOp);
}

int
ReplicatorImpl::HandleStartDone(int code, void *data)
{
    if (mCancelFlag || mChunkMetadataOp.status < 0) {
        Terminate();
        return 0;
    }
    mChunkSize    = mChunkMetadataOp.chunkSize;
    mChunkVersion = mChunkMetadataOp.chunkVersion;
    if (mChunkSize < 0 || mChunkSize > (int64_t)CHUNKSIZE) {
        KFS_LOG_STREAM_INFO << "replication:"
            " invalid chunk size: " << mChunkSize <<
        KFS_LOG_EOM;
        Terminate();
        return 0;
    }

    mReadOp.chunkVersion = mChunkVersion;
    // Delete stale copy if it exists, before replication.
    // Replication request implicitly makes previous copy stale.
    const bool kDeleteOkFlag = true;
    gChunkManager.StaleChunk(mChunkId, kDeleteOkFlag);
    // set the version to a value that will never be used; if
    // replication is successful, we then bump up the counter.
    mWriteOp.chunkVersion = 0;
    if (gChunkManager.AllocChunk(mFileId, mChunkId, 0, true) < 0) {
        Terminate();
        return -1;
    }
    KFS_LOG_STREAM_INFO << "replication:"
        " chunk: "  << mChunkId <<
        " peer: "   << GetPeerName() <<
        " starting:"
        " size: "   << mChunkSize <<
    KFS_LOG_EOM;
    Read();
    return 0;
}

void
ReplicatorImpl::Read()
{
    assert(! mCancelFlag && mOwner);
    StRef ref(*this);

    if (mOffset >= mChunkSize) {
        mDone = mOffset == mChunkSize;
        KFS_LOG_STREAM(mDone ?
                MsgLogger::kLogLevelNOTICE :
                MsgLogger::kLogLevelERROR) << "replication:"
            " chunk: "    << mChunkId <<
            " peer: "     << GetPeerName() <<
            (mDone ? " done" : " failed") <<
            " position: " << mOffset <<
            " size: "     << mChunkSize <<
            " "           << mOwner->Show() <<
        KFS_LOG_EOM;
        Terminate();
        return;
    }

    assert(mPeer);
    SET_HANDLER(this, &ReplicatorImpl::HandleReadDone);
    mReadOp.checksum.clear();
    mReadOp.status     = 0;
    mReadOp.offset     = mOffset;
    mReadOp.numBytesIO = 0;
    mReadOp.numBytes   = (int)min(
        mChunkSize - mOffset, int64_t(kDefaultReplicationReadSize));
    mPeer->Enqueue(&mReadOp);
}

int
ReplicatorImpl::HandleReadDone(int code, void *data)
{
    assert(code == EVENT_CMD_DONE && data == &mReadOp);

    const int numRd = mReadOp.dataBuf ? mReadOp.dataBuf->BytesConsumable() : 0;
    if (mReadOp.status < 0) {
        KFS_LOG_STREAM_INFO << "replication:"
            " chunk: " << mChunkId <<
            " peer: "  << GetPeerName() <<
            " read failed:"
            " error: " << mReadOp.status <<
        KFS_LOG_EOM;
    } else if (! mCancelFlag &&
            numRd < (int)mReadOp.numBytes &&
            mOffset + numRd < mChunkSize) {
        KFS_LOG_STREAM_ERROR << "replication:"
            " chunk: "    << mChunkId <<
            " peer: "     << GetPeerName() <<
            " short read:"
            " got: "      << numRd <<
            " expected: " << mReadOp.numBytes <<
        KFS_LOG_EOM;
        mReadOp.status = -EINVAL;
    }
    if (mCancelFlag || mReadOp.status < 0 || mOffset == mChunkSize) {
        mDone = mOffset == mChunkSize && mReadOp.status >= 0 && ! mCancelFlag;
        Terminate();
        return 0;
    }

    const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
    assert(mOffset % kChecksumBlockSize == 0);
    // Swap read and write buffer pointers.
    IOBuffer* const dataBuf = mWriteOp.dataBuf;
    if (dataBuf) {
        dataBuf->Clear();
    }
    mWriteOp.Reset();
    mWriteOp.dataBuf             = mReadOp.dataBuf;
    mWriteOp.numBytes            = numRd;
    mWriteOp.offset              = mOffset;
    mWriteOp.isFromReReplication = true;
    mReadOp.dataBuf = dataBuf;

    // align the writes to checksum boundaries
    if (numRd > kChecksumBlockSize) {
        // Chunk manager only handles checksum block aligned writes.
        const int     numBytes = numRd % kChecksumBlockSize;
        const int64_t endPos   = mOffset + numRd;
        assert(numBytes == 0 || endPos == mChunkSize);
        mWriteOp.numBytes = numRd - numBytes;
        if (numBytes > 0 && endPos == mChunkSize) {
            // Swap buffers back, and move the tail back into the read buffer.
            IOBuffer* const dataBuf =
                mReadOp.dataBuf ? mReadOp.dataBuf : new IOBuffer();
            mReadOp.dataBuf  = mWriteOp.dataBuf;
            mWriteOp.dataBuf = dataBuf;
            mWriteOp.dataBuf->Move(mReadOp.dataBuf, mWriteOp.numBytes);
            mReadOp.dataBuf->MakeBuffersFull();
            mReadOp.offset     = mOffset + mWriteOp.numBytes;
            mReadOp.numBytesIO = numBytes;
            mReadOp.numBytes   = numBytes;
        }
    }

    SET_HANDLER(this, &ReplicatorImpl::HandleWriteDone);
    if (gChunkManager.WriteChunk(&mWriteOp) < 0) {
        // abort everything
        Terminate();
    }
    return 0;
}

int
ReplicatorImpl::HandleWriteDone(int code, void *data)
{
    assert(
        (code == EVENT_DISK_ERROR) ||
        (code == EVENT_DISK_WROTE) ||
        (code == EVENT_CMD_DONE && data == &mWriteOp)
    );
    StRef ref(*this);
    mWriteOp.diskIo.reset();
    if (mWriteOp.status < 0) {
        KFS_LOG_STREAM_ERROR << "replication:"
            " chunk: "  << mChunkId <<
            " peer:  "  << GetPeerName() <<
            " write failed:"
            " error: "  << mWriteOp.status <<
        KFS_LOG_EOM;
    }
    if (mCancelFlag || mWriteOp.status < 0) {
        Terminate();
        return 0;
    }
    mOffset += mWriteOp.numBytesIO;
    if (mReadOp.offset == mOffset &&
            mReadOp.dataBuf && ! mReadOp.dataBuf->IsEmpty()) {
        assert(mReadOp.dataBuf->BytesConsumable() < (int)CHECKSUM_BLOCKSIZE);
        // Write the remaining tail.
        HandleReadDone(EVENT_CMD_DONE, &mReadOp);
        return 0;
    }
    Read();
    return 0;
}

void
ReplicatorImpl::Terminate()
{
    int res = -1;
    if (mDone && ! mCancelFlag) {
        KFS_LOG_STREAM_INFO << "replication:"
            " chunk: "  << mChunkId <<
            " peer: "   << GetPeerName() <<
            " finished" <<
        KFS_LOG_EOM;
        // now that replication is all done, set the version appropriately, and write
        // meta data
        SET_HANDLER(this, &ReplicatorImpl::HandleReplicationDone);
        const bool stableFlag = true;
        res = gChunkManager.ChangeChunkVers(
            mChunkId, mChunkVersion, stableFlag, this);
        if (res == 0) {
            return;
        }
    }
    HandleReplicationDone(EVENT_DISK_ERROR, &res);
}

int
ReplicatorImpl::HandleReplicationDone(int code, void *data)
{
    assert(mOwner);

    const int status = data ? *reinterpret_cast<int*>(data) : 0;
    mOwner->status = status >= 0 ? 0 : -1;
    if (status < 0) {
        KFS_LOG_STREAM_ERROR << "replication:" <<
            " chunk: "  << mChunkId <<
            " peer: "   << GetPeerName() <<
            (mCancelFlag ? " cancelled" : " failed") <<
            " status: " << status <<
            " " << mOwner->Show() <<
        KFS_LOG_EOM;
    } else {
        const ChunkInfo_t* const ci = gChunkManager.GetChunkInfo(mChunkId);
        KFS_LOG_STREAM_NOTICE << mOwner->Show() <<
            " chunk size: " << (ci ? ci->chunkSize : -1) <<
        KFS_LOG_EOM;
    }
    bool notifyFlag = ! mCancelFlag;
    if (mCancelFlag) {
        InFlightReplications::iterator const it =
            sInFlightReplications.find(mChunkId);
        notifyFlag = it != sInFlightReplications.end() && it->second == this;
    }
    if (notifyFlag) {
        gChunkManager.ReplicationDone(mChunkId, status);
    }
    // Notify the owner of completion
    mOwner->chunkVersion = (! mCancelFlag && status >= 0) ? mChunkVersion : -1;
    if (mOwner->status < 0 || mCancelFlag) {
        if (mOwner->location.IsValid()) {
            if (mCancelFlag) {
                Ctrs().mReplicationCanceledCount++;
            } else {
                Ctrs().mReplicationErrorCount++;
            }
        } else {
            if (mCancelFlag) {
                Ctrs().mRecoveryCanceledCount++;
            } else {
                Ctrs().mRecoveryErrorCount++;
            }
        }
    }
    ReplicateChunkOp* const op = mOwner;
    mOwner = 0;
    UnRef();
    SubmitOpResponse(op);
    return 0;
}

string
ReplicatorImpl::GetPeerName() const
{
    return (mPeer ? mPeer->GetLocation().ToString() : "none");
}

class RSReplicatorImpl :
    public ReplicatorImpl,
    public Reader::Completion
{
public:
    static void SetParameters(const Properties& props)
    {
        const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
        sRSReaderMaxRetryCount = props.getValue(
            "chunkServer.rsReader.maxRetryCount",
            sRSReaderMaxRetryCount
        );
        sRSReaderTimeSecBetweenRetries = props.getValue(
            "chunkServer.rsReader.timeSecBetweenRetries",
            sRSReaderTimeSecBetweenRetries
        );
        sRSReaderOpTimeoutSec = props.getValue(
            "chunkServer.rsReader.opTimeoutSec",
            sRSReaderOpTimeoutSec
        );
        sRSReaderIdleTimeoutSec = props.getValue(
            "chunkServer.rsReader.idleTimeoutSec",
            sRSReaderIdleTimeoutSec
        );   
        sRSReaderMaxReadSize = (max(1, props.getValue(
            "chunkServer.rsReader.maxReadSize",
            sRSReaderMaxReadSize
        )) + kChecksumBlockSize - 1) / kChecksumBlockSize * kChecksumBlockSize;
        sRSReaderMaxChunkReadSize = props.getValue(
            "chunkServer.rsReader.maxChunkReadSize",
            max(sRSReaderMaxReadSize, sRSReaderMaxChunkReadSize)
        );
        sRSReaderLeaseRetryTimeout = props.getValue(
            "chunkServer.rsReader.leaseRetryTimeout",
            sRSReaderLeaseRetryTimeout
        );
        sRSReaderLeaseWaitTimeout = props.getValue(
            "chunkServer.rsReader.leaseWaitTimeout",
            sRSReaderLeaseWaitTimeout
        );
        sRSReaderMetaMaxRetryCount  = props.getValue(
            "chunkServer.rsReader.meta.maxRetryCount",
            sRSReaderMetaMaxRetryCount
        );
        sRSReaderMetaTimeSecBetweenRetries = props.getValue(
            "chunkServer.rsReader.meta.timeSecBetweenRetries",
            sRSReaderMetaTimeSecBetweenRetries
        );
        sRSReaderMetaOpTimeoutSec = props.getValue(
            "chunkServer.rsReader.meta.opTimeoutSec",
            sRSReaderMetaOpTimeoutSec
        );
        sRSReaderMetaIdleTimeoutSec = props.getValue(
            "chunkServer.rsReader.meta.idleTimeoutSec",
            sRSReaderMetaIdleTimeoutSec
        );
        sRSReaderMetaResetConnectionOnOpTimeoutFlag = props.getValue(
            "chunkServer.rsReader.meta.idleTimeoutSec",
            sRSReaderMetaResetConnectionOnOpTimeoutFlag ? 1 : 0
        ) != 0;
    }
    RSReplicatorImpl(ReplicateChunkOp* op)
        : ReplicatorImpl(op, RemoteSyncSMPtr()),
          Reader::Completion(),
          mReader(
            GetMetaserver(op->location.port),
            this,
            sRSReaderMaxRetryCount,
            sRSReaderTimeSecBetweenRetries,
            sRSReaderOpTimeoutSec,
            sRSReaderIdleTimeoutSec,
            sRSReaderMaxChunkReadSize,
            sRSReaderLeaseRetryTimeout,
            sRSReaderLeaseWaitTimeout,
            MakeLogPrefix(mChunkId),
            GetSeqNum()
        ),
        mReadTail(),
        mReadSize(GetReadSize(*op)),
        mReadInFlightFlag(false),
        mPendingCloseFlag(false)
    {
        assert(mReadSize % IOBufferData::GetDefaultBufferSize() == 0);
        mReadOp.clnt = 0; // Should not queue read op.
    }
    virtual void Start()
    {
        assert(mOwner);
        mChunkMetadataOp.chunkSize    = CHUNKSIZE;
        mChunkMetadataOp.chunkVersion = mOwner->chunkVersion;
        mReadOp.status                = 0;
        mReadOp.numBytes              = 0;
        const bool kSkipHolesFlag                 = true;
        const bool kUseDefaultBufferAllocatorFlag = true;
        mChunkMetadataOp.status = mReader.Open(
            mFileId,
            mOwner->pathName.c_str(),
            mOwner->fileSize,
            mOwner->striperType,
            mOwner->stripeSize,
            mOwner->numStripes,
            mOwner->numRecoveryStripes,
            kSkipHolesFlag,
            kUseDefaultBufferAllocatorFlag,
            mOwner->chunkOffset
        );
        HandleStartDone(EVENT_CMD_DONE, &mChunkMetadataOp);
    }
    virtual void Done(
        Reader&           inReader,
        int               inStatusCode,
        Reader::Offset    inOffset,
        Reader::Offset    inSize,
        IOBuffer*         inBufferPtr,
        Reader::RequestId inRequestId)
    {
        StRef ref(*this);

        if (&inReader != &mReader || (inBufferPtr &&
                (inRequestId.mPtr != this ||
                    inOffset < 0 ||
                    (mOwner && mOwner->chunkOffset + mOffset != inOffset) ||
                    inSize > (Reader::Offset)mReadOp.numBytes ||
                    ! mReadInFlightFlag))) {
            die("recovery: invalid read completion");
            mReadOp.status = -EINVAL;
        }
        if (mPendingCloseFlag) {
            if (! mReader.IsActive()) {
                KFS_LOG_STREAM_DEBUG << "recovery:"
                    " chunk: " << mChunkId <<
                    " chunk reader closed" <<
                KFS_LOG_EOM;
                mPendingCloseFlag = false;
                UnRef();
            }
            return;
        }
        if (! mReadInFlightFlag) {
            if (mReadOp.status >= 0 && inStatusCode < 0) {
                mReadOp.status = inStatusCode;
            }
            return;
        }
        mReadInFlightFlag = false;
        if (! mOwner) {
            return;
        }
        if (mReadOp.status != 0 || (! inBufferPtr && inStatusCode == 0)) {
            return;
        }
        assert(mReadOp.dataBuf);
        mReadOp.status = inStatusCode;
        if (mReadOp.status == 0 && inBufferPtr) {
            const bool endOfChunk =
                mReadSize > inBufferPtr->BytesConsumable() ||
                mOffset + mReadSize >= mChunkSize;
            IOBuffer& buf = *mReadOp.dataBuf;
            buf.Clear();
            if (endOfChunk) {
                buf.Move(&mReadTail);
                buf.Move(inBufferPtr);
                mReadOp.numBytes   = buf.BytesConsumable();
                mReadOp.numBytesIO = mReadOp.numBytes;
                mChunkSize = mOffset + mReadOp.numBytesIO;
                mReader.Close();
                if (mReader.IsActive()) {
                    mPendingCloseFlag = true;
                    Ref();
                }
            } else {
                const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
                int nmv = (mReadTail.BytesConsumable() +
                    inBufferPtr->BytesConsumable()) /
                    kChecksumBlockSize * kChecksumBlockSize;
                if (nmv <= 0) {
                    mReadTail.Move(inBufferPtr);
                    Read();
                    return;
                }
                nmv -= buf.Move(&mReadTail, nmv);
                buf.Move(inBufferPtr, nmv);
                mReadTail.Move(inBufferPtr);
                mReadOp.numBytes   = buf.BytesConsumable();
                mReadOp.numBytesIO = mReadOp.numBytes;
            }
        } else if (inStatusCode < 0 && inBufferPtr &&
                ! inBufferPtr->IsEmpty()) {
            // Report invalid stripes.
            const int     ns = mOwner->numStripes + mOwner->numRecoveryStripes;
            int           n  = 0;
            ostringstream os;
            while (! inBufferPtr->IsEmpty()) {
                if (n >= ns) {
                    die("recovery: completion: invalid number of bad stripes");
                    n = 0;
                    break;
                }
                int          idx          = -1;
                kfsChunkId_t chunkId      = -1;
                int64_t      chunkVersion = -1;
                ReadVal(*inBufferPtr, idx);
                ReadVal(*inBufferPtr, chunkId);
                ReadVal(*inBufferPtr, chunkVersion);
                if (idx < 0 || idx >= ns) {
                    die("recovery: completion: invalid bad stripe index");
                    n = 0;
                    break;
                }
                os << (n > 0 ? " " : "") << idx <<
                    " " << chunkId << " " << chunkVersion;
                n++;
            }
            if (n > 0) {
                mOwner->invalidStripeIdx = os.str();
                KFS_LOG_STREAM_ERROR << "recovery: "
                    " status: "          << inStatusCode <<
                    " invalid stripes: " << mOwner->invalidStripeIdx <<
                KFS_LOG_EOM;
            }
        }
        HandleReadDone(EVENT_CMD_DONE, &mReadOp);
    }
    static void CancelAll()
        { GetMetaserver(-1); }

private:
    Reader    mReader;
    IOBuffer  mReadTail;
    const int mReadSize;
    bool      mReadInFlightFlag;
    bool      mPendingCloseFlag;

    virtual ~RSReplicatorImpl()
    {
        KFS_LOG_STREAM_DEBUG << "~RSReplicatorImpl"
            " chunk: " << mChunkId <<
        KFS_LOG_EOM;
        mReader.Register(0);
        mReader.Shutdown();
    }
    virtual void Cancel()
    {
        StRef ref(*this);

        const int prevRef = GetRefCount();
        mReader.Unregister(this);
        mReader.Shutdown();
        ReplicatorImpl::Cancel();
        if (mReadInFlightFlag && prevRef <= GetRefCount()) {
            assert(mOwner);
            mReadInFlightFlag = false;
            mReadOp.status = -ETIMEDOUT;
            HandleReadDone(EVENT_CMD_DONE, &mReadOp);
        }
    }
    virtual void Read()
    {
        assert(! mCancelFlag && mOwner && ! mReadInFlightFlag);
        if (mOffset >= mChunkSize || mReadOp.status < 0) {
            ReplicatorImpl::Read();
            return;
        }

        StRef ref(*this);

        if (! mReadOp.dataBuf) {
            mReadOp.dataBuf = new IOBuffer();
        }
        mReadOp.status     = 0;
        mReadOp.numBytes   = mReadSize;
        mReadOp.numBytesIO = 0;
        mReadOp.offset     = mOffset;
        mReadOp.dataBuf->Clear();
        Reader::RequestId reqId = Reader::RequestId();
        reqId.mPtr = this;
        mReadInFlightFlag = true;
        IOBuffer buf;
        const int status = mReader.Read(
            buf,
            mReadSize,
            mOffset + mReadTail.BytesConsumable(),
            reqId
        );
        if (status != 0 && mReadInFlightFlag) {
            mReadInFlightFlag = false;
            mReadOp.status = status;
            HandleReadDone(EVENT_CMD_DONE, &mReadOp);
        }
    }
    virtual ByteCount GetBufferBytesRequired() const
    {
        return (mReadSize * (mOwner ? mOwner->numStripes + 1 : 0));
    }
    template<typename T> static void ReadVal(IOBuffer& buf, T& val)
    {
        const int len = (int)sizeof(val);
        if (buf.Consume(buf.CopyOut(
                reinterpret_cast<char*>(&val), len)) != len) {
            die("invalid buffer size");
        }
    }
    struct AddExtraClientHeaders
    {
        AddExtraClientHeaders(const char* hdrs)
        {
            client::KfsOp::AddExtraRequestHeaders(hdrs);
            client::KfsOp::AddDefaultRequestHeaders(
                kKfsUserRoot, kKfsGroupRoot);
        }
    };
    static KfsNetClient& GetMetaserver(int port)
    {
        static AddExtraClientHeaders sAddHdrs("From-chunk-server: 1\r\n");
        static KfsNetClient sMetaServerClient(
            globalNetManager(),
            string(), // inHost
            0,        // inPort
            sRSReaderMetaMaxRetryCount,
            sRSReaderMetaTimeSecBetweenRetries,
            sRSReaderMetaOpTimeoutSec,
            sRSReaderMetaIdleTimeoutSec,
            GetRandomSeq(),
            "RSR",
            sRSReaderMetaResetConnectionOnOpTimeoutFlag
        );
        static int sMetaPort = -1;
        if (port <= 0) {
            sMetaPort = -1;
            sMetaServerClient.Stop();
        } else if (sMetaPort != port) {
            if (sMetaPort > 0) {
                KFS_LOG_STREAM_INFO << "recovery:"
                    " meta server client port has changed"
                    " from: " << sMetaPort <<
                    " to: "   << port <<
                KFS_LOG_EOM;
            }
            sMetaPort = port;
            sMetaServerClient.SetServer(ServerLocation(
                gMetaServerSM.GetLocation().hostname, sMetaPort));
        }
        return sMetaServerClient;
    }
    static const char* MakeLogPrefix(kfsChunkId_t chunkId)
    {
        static ostringstream os;
        static string        pref;
        os.str(string());
        os << "CR: " << chunkId;
        pref = os.str();
        return pref.c_str();
    }
    static kfsSeq_t GetSeqNum()
    {
        static kfsSeq_t sInitialSeqNum = GetRandomSeq();
        static uint32_t sNextRand      = (uint32_t)sInitialSeqNum;
        sNextRand = sNextRand * 1103515245 + 12345;
        sInitialSeqNum += 100000 + ((uint32_t)(sNextRand / 65536) % 32768);
        return sInitialSeqNum;
    }
    static int GetReadSize(const ReplicateChunkOp& op)
    {
        // Align read on checksum block boundary, and align on stripe size,
        // if possible.
        const int kChecksumBlockSize = (int)CHECKSUM_BLOCKSIZE;
        const int kIoBufferSize      = IOBufferData::GetDefaultBufferSize();
        assert(
            sRSReaderMaxReadSize >= kChecksumBlockSize &&
            op.stripeSize > 0 &&
            sRSReaderMaxReadSize % kChecksumBlockSize == 0 &&
            kChecksumBlockSize % kIoBufferSize == 0
        );
        const int size = max(kChecksumBlockSize, (int)min(
            int64_t(sRSReaderMaxReadSize),
            (DiskIo::GetBufferManager().GetMaxClientQuota() /
                max(1, op.numStripes + 1)) /
            kChecksumBlockSize * kChecksumBlockSize)
        );
        if (size <= op.stripeSize) {
            KFS_LOG_STREAM_DEBUG << "recovery:"
                " large stripe: " << op.stripeSize <<
                " read size: "    << size <<
            KFS_LOG_EOM;
            return size;
        }
        int lcm = GetLcm(kChecksumBlockSize, op.stripeSize);
        if (lcm > size) {
            lcm = GetLcm(kIoBufferSize, op.stripeSize);
            if (lcm > size) {
                KFS_LOG_STREAM_WARN << "recovery:"
                    "invalid read parameters:"
                    " max read size:  " << sRSReaderMaxReadSize <<
                    " io buffer size: " << kIoBufferSize <<
                    " stripe size: "    << op.stripeSize <<
                    " set read size: "  << lcm <<
                KFS_LOG_EOM;
                return lcm;
            }
        }
        return (size / lcm * lcm);
    }
    static int GetGcd(int nl, int nr)
    {
        int a = nl;
        int b = nr;
        while (b != 0) {
            const int t = b;
            b = a % b;
            a = t;
        }
        return a;
    }
    static int GetLcm(int nl, int nr)
        { return ((nl == 0 || nr == 0) ? 0 : nl / GetGcd(nl, nr) * nr); }

    static int  sRSReaderMaxRetryCount;
    static int  sRSReaderTimeSecBetweenRetries;
    static int  sRSReaderOpTimeoutSec;
    static int  sRSReaderIdleTimeoutSec;
    static int  sRSReaderMaxChunkReadSize;
    static int  sRSReaderMaxReadSize;
    static int  sRSReaderLeaseRetryTimeout;
    static int  sRSReaderLeaseWaitTimeout;
    static int  sRSReaderMetaMaxRetryCount;
    static int  sRSReaderMetaTimeSecBetweenRetries;
    static int  sRSReaderMetaOpTimeoutSec;
    static int  sRSReaderMetaIdleTimeoutSec;
    static bool sRSReaderMetaResetConnectionOnOpTimeoutFlag;
private:
    // No copy.
    RSReplicatorImpl(const RSReplicatorImpl&);
    RSReplicatorImpl& operator=(const RSReplicatorImpl&);
};
int  RSReplicatorImpl::sRSReaderMaxRetryCount                      = 3;
int  RSReplicatorImpl::sRSReaderTimeSecBetweenRetries              = 10;
int  RSReplicatorImpl::sRSReaderOpTimeoutSec                       = 30;
int  RSReplicatorImpl::sRSReaderIdleTimeoutSec                     = 5 * 30;
int  RSReplicatorImpl::sRSReaderMaxReadSize                        =
    kDefaultReplicationReadSize;
int  RSReplicatorImpl::sRSReaderMaxChunkReadSize                   =
    max(kDefaultReplicationReadSize, 1 << 20);
int  RSReplicatorImpl::sRSReaderLeaseRetryTimeout                  = 3;
int  RSReplicatorImpl::sRSReaderLeaseWaitTimeout                   = 30;
int  RSReplicatorImpl::sRSReaderMetaMaxRetryCount                  = 2;
int  RSReplicatorImpl::sRSReaderMetaTimeSecBetweenRetries          = 10;
int  RSReplicatorImpl::sRSReaderMetaOpTimeoutSec                   = 4 * 60;
int  RSReplicatorImpl::sRSReaderMetaIdleTimeoutSec                 = 5 * 60;
bool RSReplicatorImpl::sRSReaderMetaResetConnectionOnOpTimeoutFlag = true;

int
Replicator::GetNumReplications()
{
    return ReplicatorImpl::GetNumReplications();
}

void
Replicator::CancelAll()
{
    ReplicatorImpl::CancelAll();
    RSReplicatorImpl::CancelAll();
}

void
Replicator::SetParameters(const Properties& props)
{
    ReplicatorImpl::SetParameters(props);
    RSReplicatorImpl::SetParameters(props);
}

void
Replicator::GetCounters(Replicator::Counters& counters)
{
    ReplicatorImpl::GetCounters(counters);
}

void
Replicator::Run(ReplicateChunkOp *op)
{
    assert(op);
    KFS_LOG_STREAM_DEBUG << op->Show() << KFS_LOG_EOM;

    ReplicatorImpl* impl = 0;
    if (op->location.IsValid()) {
        ReplicatorImpl::Ctrs().mReplicationCount++;
        RemoteSyncSMPtr peer;
        if (ReplicatorImpl::GetUseConnectionPoolFlag()) {
            peer = gChunkServer.FindServer(op->location);
        } else {
            peer.reset(new RemoteSyncSM(op->location));
            if (! peer->Connect()) {
                peer.reset();
            }
        }
        if (! peer) {
            KFS_LOG_STREAM_ERROR << "replication:"
                "unable to find peer: " << op->location.ToString() <<
                " " << op->Show() <<
            KFS_LOG_EOM;
            op->status = -1;
            ReplicatorImpl::Ctrs().mReplicationErrorCount++;
        } else {
            impl = new ReplicatorImpl(op, peer);
        }
    } else {
        ReplicatorImpl::Ctrs().mRecoveryCount++;
        if (op->chunkOffset < 0 ||
                op->chunkOffset % int64_t(CHUNKSIZE) != 0 ||
                op->striperType != KFS_STRIPED_FILE_TYPE_RS ||
                op->numStripes <= 0 ||
                op->numRecoveryStripes <= 0 ||
                op->stripeSize < KFS_MIN_STRIPE_SIZE ||
                op->stripeSize > KFS_MAX_STRIPE_SIZE ||
                CHUNKSIZE % op->stripeSize != 0 ||
                op->stripeSize % KFS_STRIPE_ALIGNMENT != 0 ||
                op->location.port <= 0) {
            op->status = -EINVAL;
            KFS_LOG_STREAM_ERROR << "replication:"
                "invalid request: " << op->Show() <<
            KFS_LOG_EOM;
            ReplicatorImpl::Ctrs().mRecoveryErrorCount++;
        } else {
            impl = new RSReplicatorImpl(op);
        }
    }
    if (impl) {
        impl->Ref();
        impl->Run();
    } else {
        SubmitOpResponse(op);
    }
}

} // namespace KFS
