//---------------------------------------------------------- -*- Mode: C++ -*-
// $Id$
//
// Created 2012/08/20
// Author: Mike Ovsiannikov
//
// Copyright 2012 Quantcast Corp.
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
// \brief Kfs tool.
//
//----------------------------------------------------------------------------

#include "FileSystem.h"
#include "common/MsgLogger.h"
#include "qcdio/QCUtils.h"

#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include <string>
#include <vector>
#include <ostream>
#include <iostream>
#include <algorithm>

namespace KFS
{
namespace tools
{

using std::string;
using std::cout;
using std::cerr;
using std::vector;
using std::pair;
using std::max;
using std::ostream;

class KfsTool
{
public:
    KfsTool()
        : mIoBufferSize(6 << 20),
          mIoBufferPtr(new char[mIoBufferSize])
        {}
    ~KfsTool()
    {
        delete [] mIoBufferPtr;
    }
    int Run(
        int    inArgCount,
        char** inArgsPtr)
    {
        string              theMetaHost;
        string              theMetaPort;
        bool                theHelpFlag = false;
        MsgLogger::LogLevel theLogLevel = MsgLogger::kLogLevelINFO;

        int theOpt;
        while ((theOpt = getopt(inArgCount, inArgsPtr, "hs:p:v")) != -1) {
            switch (theOpt) {
                case 's':
                    theMetaHost = optarg;
                    break;
                case 'p':
                    theMetaPort = optarg;
                    break;
                case 'h':
                    theHelpFlag = true;
                    break;
                case 'v':
                    theLogLevel = MsgLogger::kLogLevelDEBUG;
                    break;
                default:
                    cout << "Unrecognized option : " << char(theOpt) << "\n";
                    theHelpFlag = true;
                    break;
            }
        }

        if (theHelpFlag || (theMetaHost.empty() && ! theMetaPort.empty())) {
            cout <<
                "Usage: " << (inArgCount > 0 ? inArgsPtr[0] : "") << "\n"
                " [-s <meta server host>]\n"
                " [-p <meta server port>]\n"
            ;
            return 1;
        }
        MsgLogger::Init(0, theLogLevel);

        if (! theMetaHost.empty()) {
            string theUri = "qfs://" + theMetaHost;
            if (theMetaPort.empty()) {
                theUri += ":";
                theUri += theMetaPort;
            }
            const int theErr = FileSystem::SetDefault(theUri);
            if (theErr != 0) {
                cerr << theUri << ": " <<
                    FileSystem::GetStrError(-theErr) << "\n";
                return 1;
            }
        }
        int theErr = 0;
        if (optind < inArgCount) {
            const char* const theCmdPtr = inArgsPtr[optind];
            if (strcmp(theCmdPtr, "-cat") == 0) {
                theErr = Cat(inArgsPtr + optind + 1, inArgCount - optind - 1);
            } else if (strcmp(theCmdPtr, "-ls") == 0) {
                const bool kRecursiveFlag = false;
                theErr = List(inArgsPtr + optind + 1, inArgCount - optind - 1,
                    kRecursiveFlag);
            } else if (strcmp(theCmdPtr, "-lsr") == 0) {
                const bool kRecursiveFlag = true;
                theErr = List(inArgsPtr + optind + 1, inArgCount - optind - 1,
                    kRecursiveFlag);
            } else if (strcmp(theCmdPtr, "-mkdir") == 0) {
                const kfsMode_t kCreateMode    = 0777;
                const bool      kCreateAllFlag = true;
                theErr = Mkdir(inArgsPtr + optind + 1, inArgCount - optind - 1,
                    kCreateMode, kCreateAllFlag);
            } else {
                cerr << "unsupported option: " << theCmdPtr << "\n";
                theErr = EINVAL;
            }
        }
        return (theErr == 0 ? 0 : 1);
    }
private:
    static const char* GlobError(
        int inError)
    {
        switch (inError) {
            case GLOB_NOSPACE:
                return "out of memory";
            case GLOB_ABORTED:
                return "read error";
            case GLOB_NOMATCH:
                return "no matches found";
            case 0:
                return "no error";
            default:
                return "unspecified error";
        }
    }
    typedef vector<pair<FileSystem*, vector<string> > > GlobResult;
    static int Glob(
        char**       inArgsPtr,
        int          inArgCount,
        ostream&     inErrorStream,
        GlobResult&  outResult)
    {
        outResult.reserve(outResult.size() + max(0, inArgCount));
        int theRet = 0;
        for (int i = 0; i < inArgCount; i++) {
            const string theArg   = inArgsPtr[i];
            FileSystem*  theFsPtr = 0;
            string       thePath;
            int          theErr   = FileSystem::Get(theArg, theFsPtr, &thePath);
            if (theErr) {
                inErrorStream << theArg <<
                    ": " << FileSystem::GetStrError(theErr) << "\n";
                theRet = theErr;
                continue;
            }
            glob_t    theGlobRes = {0};
            const int kGlobFlags = GLOB_NOSORT | GLOB_NOCHECK;
            theErr = theFsPtr->Glob(
                thePath,
                kGlobFlags,
                0, // the err func.
                &theGlobRes
            );
            if (theErr == 0) {
                outResult.resize(outResult.size() + 1);
                outResult.back().first = theFsPtr;
                string thePrefix;
                if (thePath.empty() || thePath[0] != '/') {
                    string theCwd;
                    if ((theErr = theFsPtr->GetCwd(theCwd))) {
                        inErrorStream << theArg <<
                            ": " << theFsPtr->StrError(theErr) << "\n";
                        globfree(&theGlobRes);
                        theRet = theErr;
                        continue;
                    }
                    thePrefix += theCwd;
                    if (! thePrefix.empty() && *thePrefix.rbegin() != '/' &&
                            (theGlobRes.gl_pathc > 1 ||
                            (theGlobRes.gl_pathc == 1 &&
                            theGlobRes.gl_pathv[0][0] != 0))) {
                        thePrefix += "/";
                    }
                }
                vector<string>& theResult = outResult.back().second;
                theResult.reserve(theGlobRes.gl_pathc);
                for (size_t i = 0; i < theGlobRes.gl_pathc; i++) {
                    theResult.push_back(thePrefix + theGlobRes.gl_pathv[i]);
                }
            } else {
                inErrorStream << inArgsPtr[i] << ": " << GlobError(theErr) <<
                    " " << theErr << "\n";
            }
            globfree(&theGlobRes);
            if (theErr != 0) {
                theRet = theErr;
            }
        }
        return theRet;
    }
    template <typename FuncT> int Apply(
        char** inArgsPtr,
        int    inArgCount,
        FuncT& inFunctor)
    {
        GlobResult theResult;
        int theErr = Glob(inArgsPtr, inArgCount, cerr, theResult);
        if (! inFunctor.Init(theErr, theResult)) {
            return theErr;
        }
        for (GlobResult::const_iterator theFsIt = theResult.begin();
                theFsIt != theResult.end();
                ++theFsIt) {
            FileSystem& theFs = *(theFsIt->first);
            for (vector<string>::const_iterator theIt = theFsIt->second.begin();
                    theIt != theFsIt->second.end();
                    ++theIt) {
                if (! inFunctor.Apply(theFs, *theIt)) {
                    return inFunctor.GetStatus();
                }
            }
        }
        const int theStatus = inFunctor.GetStatus();
        return (theStatus != 0 ? theStatus : theErr);
    }
    class CatFunctor
    {
    public:
        CatFunctor(
            ostream&    inOutStream,
            const char* inOutStreamNamePtr,
            ostream&    inErrorStream,
            size_t      inIoBufferSize,
            char*       inIoBufferPtr)
            : mOutStream(inOutStream),
              mOutStreamNamePtr(inOutStreamNamePtr ? inOutStreamNamePtr : ""),
              mErrorStream(inErrorStream),
              mIoBufferSize(inIoBufferSize),
              mIoBufferPtr(inIoBufferPtr),
              mStatus(0)
            {}
        bool Init(
            int&              /* ioGlobError */,
            const GlobResult& /* inGlobResult */)
            { return true; }
        bool Apply(
            FileSystem&   inFs,
            const string& inPath)
        {
            if (! mOutStream) {
                return false;
            }
            const int theFd = inFs.Open(inPath, O_RDONLY, 0);
            if (theFd < 0) {
                mErrorStream << inFs.GetUri() << inPath <<
                    ": " << inFs.StrError(theFd) << "\n";
                mStatus = theFd;
                return true;
            }
            for (; ;) {
                const ssize_t theNRead =
                    inFs.Read(theFd, mIoBufferPtr, mIoBufferSize);
                if (theNRead == 0) {
                    break;
                }
                if (theNRead < 0) {
                    mStatus = (int)theNRead;
                    mErrorStream << inFs.GetUri() << inPath <<
                        ": " << inFs.StrError(mStatus) << "\n";
                    break;
                }
                if (! mOutStream.write(mIoBufferPtr, theNRead)) {
                    mStatus = errno;
                    mErrorStream << inFs.GetUri() << inPath <<
                        ": " << mOutStreamNamePtr <<
                        QCUtils::SysError(mStatus) << "\n";
                    break;
                }
            }
            inFs.Close(theFd);
            return true;
        }
        int GetStatus() const
            { return mStatus; }
    private:
        ostream&          mOutStream;
        const char* const mOutStreamNamePtr;
        ostream&          mErrorStream;
        const size_t      mIoBufferSize;
        char* const       mIoBufferPtr;
        int               mStatus;
    private:
        CatFunctor(
            const CatFunctor& inFunctor);
        CatFunctor& operator=(
            const CatFunctor& inFunctor);
    };
    int Cat(
        char** inArgsPtr,
        int    inArgCount)
    {
        CatFunctor theFunc(cout, "stdout", cerr, mIoBufferSize, mIoBufferPtr);
        return Apply(inArgsPtr, inArgCount, theFunc);
    }
    class ListFunctor
    {
    public:
        ListFunctor(
            ostream&    inOutStream,
            const char* inOutStreamNamePtr,
            ostream&    inErrorStream,
            bool        inRecursiveFlag)
            : mOutStream(inOutStream),
              mOutStreamNamePtr(inOutStreamNamePtr ? inOutStreamNamePtr : ""),
              mErrorStream(inErrorStream),
              mRecursiveFlag(inRecursiveFlag),
              mShowFsUriFlag(false),
              mEmptyStr(),
              mStat(),
              mStatus(0),
              mOwnerId(kKfsUserNone),
              mGroupId(kKfsGroupNone),
              mOwner("-"),
              mGroup("-"),
              mTime(0)
            { mTmBuf[0] = 0; }
        bool Init(
            int&           /* ioGlobError */,
            const GlobResult& inGlobResult)
        {
            mShowFsUriFlag = inGlobResult.size() > 1;
            return true;
        }
        bool Apply(
            FileSystem&   inFs,
            const string& inPath)
        {
            if (! mOutStream) {
                return false;
            }
            int theErr = inFs.Stat(inPath, mStat);
            if (theErr != 0) {
                mErrorStream << inFs.GetUri() << inPath <<
                    ": " << inFs.StrError(theErr) << "\n";
                mStatus = theErr;
                return true;
            }
            Show(inFs, inPath, string(), mStat);
            const string  kEmpty;
            if ((mStat.st_mode & S_IFDIR) != 0) {
                FileSystem::DirIterator* theItPtr = 0;
                const bool kFetchAttributesFlag = true;
                if ((theErr = inFs.Open(
                        inPath, kFetchAttributesFlag, theItPtr))) {
                    mErrorStream << inFs.GetUri() << inPath <<
                        ": " << inFs.StrError(theErr) << "\n";
                    mStatus = theErr;
                } else {
                    string theName;
                    mStat.Reset();
                    const string& thePath = inPath == "/" ? mEmptyStr: inPath;
                    while (mOutStream) {
                        const FileSystem::StatBuf* theStatPtr = 0;
                        if ((theErr = inFs.Next(
                                theItPtr, theName, theStatPtr))) {
                            mErrorStream << inFs.GetUri() << thePath <<
                                "/" << theName << ": " <<
                                inFs.StrError(theErr) << "\n";
                            mStatus = theErr;
                        }
                        if (theName.empty()) {
                            break;
                        }
                        if (theName == "." || theName == "..") {
                            continue;
                        }
                        Show(inFs, thePath, theName,
                            theStatPtr ? *theStatPtr : mStat);
                        if (mRecursiveFlag) {
                            Apply(inFs, thePath + "/" + theName);
                        }
                    }
                    inFs.Close(theItPtr);
                }
            }
            return true;
        }
        int GetStatus() const
            { return mStatus; }
    private:
        enum { kTmBufLen = 128 };
        ostream&            mOutStream;
        const char* const   mOutStreamNamePtr;
        ostream&            mErrorStream;
        const bool          mRecursiveFlag;
        bool                mShowFsUriFlag;
        const string        mEmptyStr;
        FileSystem::StatBuf mStat;
        int                 mStatus;
        kfsUid_t            mOwnerId;
        kfsGid_t            mGroupId;
        string              mOwner;
        string              mGroup;
        time_t              mTime;
        char                mTmBuf[kTmBufLen];

        void Show(
            FileSystem&                inFs,
            const string&              inPath,
            const string&              inName,
            const FileSystem::StatBuf& inStat)
        {
            for (int i = 8; i > 0; ) {
                const char* kPerms[2] = {"---", "rwx"};
                for (int k = 0; k < 3; k++) {
                    mOutStream << kPerms[(inStat.st_mode >> i--) & 1][k];
                }
            }
#ifdef S_ISVTX
            const mode_t kSticky = S_IFDIR | S_ISVTX;
            if ((inStat.st_mode & kSticky) == kSticky) {
                mOutStream << "t";
            } else {
                mOutStream << " ";
            }
#endif
            mOutStream << " ";
            if ((inStat.st_mode & S_IFDIR) != 0) {
                mOutStream << "<dir>";
            } else {
                if (inStat.mStripeSize > 0) {
                    if (inStat.mNumRecoveryStripes > 0) {
                        mOutStream << "<rs ";
                    } else {
                        mOutStream << "<s ";
                    }
                    mOutStream << inStat.mNumReplicas <<
                         "," << inStat.mNumStripes;
                    if (inStat.mNumRecoveryStripes > 0) {
                        mOutStream << "+" << inStat.mNumRecoveryStripes;
                    }
                } else {
                    mOutStream << "<r " << max(1, (int)inStat.mNumReplicas);
                }
                mOutStream << ">";
            }
            if (mOwnerId != inStat.st_uid || mOwner.empty() ||
                    mGroupId != inStat.st_gid || mGroup.empty()) {
                UpdateUserAndGroup(inFs, inStat.st_uid, inStat.st_gid);
            }
            mOutStream << " " << mOwner << " " << mGroup;
            mOutStream << " " << max(int64_t(0), (int64_t)inStat.st_size);
#ifndef KFS_OS_NAME_DARWIN
            const time_t theTime = inStat.st_mtime;
#else
            const time_t theTime = inStat.st_mtimespec.tv_sec;
#endif
            if (mTmBuf[0] == 0 || mTime != theTime) {
                struct tm theLocalTime = {0};
                localtime_r(&theTime, &theLocalTime);
                strftime(mTmBuf, kTmBufLen, "%b %e %H:%M", &theLocalTime);
                mTime = theTime;
            }
            mOutStream << " " << mTmBuf << " ";
            if (mShowFsUriFlag) {
                mOutStream << inFs.GetUri();
            }
            mOutStream << inPath;
            if (! inName.empty()) {
                mOutStream << "/" << inName;
            }
            mOutStream << "\n";
        }
        void UpdateUserAndGroup(
            FileSystem& inFs,
            kfsUid_t    inUid,
            kfsGid_t    inGid)
        {
            int theErr;
            if ((inUid != kKfsUserNone || inGid != kKfsGroupNone) &&
                    (theErr = inFs.GetUserAndGroupNames(
                        inUid, inGid, mOwner, mGroup))) {
                mErrorStream << inFs.GetUri() << " userId: " << inUid <<
                    " groupId: " << inGid << " : " << inFs.StrError(theErr) <<
                "\n";
                mOwner = "?";
                mGroup = "?";
                if (mStatus == 0) {
                    mStatus = theErr;
                }
                return;
            }
            if (inUid == kKfsUserNone) {
                mOwner = "-";
            }
            if (inGid == kKfsGroupNone) {
                mGroup = "-";
            }
        }
    private:
        ListFunctor(
            const ListFunctor& inFunctor);
        ListFunctor& operator=(
            const ListFunctor& inFunctor);
    };
    int List(
        char** inArgsPtr,
        int    inArgCount,
        bool   inRecursiveFlag)
    {
        ListFunctor theFunc(cout, "stdout", cerr, inRecursiveFlag);
        return Apply(inArgsPtr, inArgCount, theFunc);
    }
    class ErrorReporter : public FileSystem::ErrorHandler
    {
    public:
        ErrorReporter(
            FileSystem& inFs,
            ostream&    inErrorStream,
            bool        inStopOnErrorFlag = false)
            : mFs(inFs),
              mErrorStream(inErrorStream),
              mStopOnErrorFlag(inStopOnErrorFlag),
              mStatus(0)
            {}
        int operator()(
            const string& inPath,
            int           inStatus)
        {
            mErrorStream << mFs.GetUri() << inPath << ": " <<
                mFs.StrError(inStatus) << "\n";
            mStatus = inStatus;
            return (mStopOnErrorFlag ? inStatus : 0);
        }
        int GetStatus() const
            { return mStatus; }
    private:
        FileSystem& mFs;
        ostream&    mErrorStream;
        const bool  mStopOnErrorFlag;
        int         mStatus;
    private:
        ErrorReporter(
            const ErrorReporter& inReporter);
        ErrorReporter& operator=(
            const ErrorReporter& inReporter);
    };
    class DefaultInitFunctor
    {
    public:
        DefaultInitFunctor()
            {}
        bool operator()(
            int&              /* ioGlobError */,
            const GlobResult& /* inGlobResult */)
            { return true; }
    private:
        DefaultInitFunctor(
            const DefaultInitFunctor& inFunct);
        DefaultInitFunctor& operator=(
            const DefaultInitFunctor& inFunct);
    };
    template<
        typename T,
        typename TInit            = DefaultInitFunctor,
        bool     TStopIfErrorFlag = false
    > class FunctorT
    {
    public:
        FunctorT(
            T&       inFunctor,
            ostream& inErrorStream)
            : mFunctor(inFunctor),
              mInitFunctor(),
              mErrorStream(inErrorStream),
              mStatus(0)
            {}
        bool Init(
            int&        ioGlobError,
            GlobResult& inGlobResult)
        {
            return mInitFunctor(ioGlobError, inGlobResult);
        }
        bool Apply(
            FileSystem&   inFs,
            const string& inPath)
        {
            ErrorReporter theErrorReporter(inFs, mErrorStream);
            const int     theError = mFunctor(inFs, inPath, theErrorReporter);
            if (theError != 0) {
                theErrorReporter(inPath, theError);
            }
            mStatus = theErrorReporter.GetStatus();
            return (! TStopIfErrorFlag || mStatus == 0);
        }
        int GetStatus() const
            { return mStatus; }
        TInit& GetInit()
            { return mInitFunctor; }
    private:
        T&       mFunctor;
        TInit    mInitFunctor;
        ostream& mErrorStream;
        int      mStatus;
    private:
        FunctorT(
            const FunctorT& inFunctor);
        FunctorT& operator=(
            const FunctorT& inFunctor);
    };
    class ChownFunctor
    {
    public:
        ChownFunctor(
            kfsUid_t inUid,
            kfsGid_t inGid,
            bool     inRecursiveFlag)
            : mUid(inUid),
              mGid(inGid),
              mRecursiveFlag(inRecursiveFlag)
            {}
        int operator()(
            FileSystem&    inFs,
            const string&  inPath,
            ErrorReporter& inErrorReporter)
        {
            return inFs.Chown(inPath, mUid, mGid,
                mRecursiveFlag, &inErrorReporter);
        }
    private:
        const kfsUid_t mUid;
        const kfsGid_t mGid;
        const bool     mRecursiveFlag;
    private:
        ChownFunctor(
            const ChownFunctor& inFunctor);
        ChownFunctor& operator=(
            const ChownFunctor& inFunctor);
    };
    int Chown(
        char**   inArgsPtr,
        int      inArgCount,
        kfsUid_t inUid,
        kfsGid_t inGid,
        bool     inRecursiveFlag)
    {
        ChownFunctor           theChownFunc(inUid, inGid, inRecursiveFlag);
        FunctorT<ChownFunctor> theFunc(theChownFunc, cerr);
        return Apply(inArgsPtr, inArgCount, theFunc);
    }
    class ChmodFunctor
    {
    public:
        ChmodFunctor(
            kfsMode_t inMode,
            bool      inRecursiveFlag)
            : mMode(inMode),
              mRecursiveFlag(inRecursiveFlag)
            {}
        int operator()(
            FileSystem&    inFs,
            const string&  inPath,
            ErrorReporter& inErrorReporter)
        {
            return inFs.Chmod(inPath, mMode, mRecursiveFlag, &inErrorReporter);
        }
    private:
        const kfsMode_t mMode;
        const bool      mRecursiveFlag;
    private:
        ChmodFunctor(
            const ChmodFunctor& inFunctor);
        ChmodFunctor& operator=(
            const ChmodFunctor& inFunctor);
    };
    int Chmod(
        char**    inArgsPtr,
        int       inArgCount,
        kfsMode_t inMode,
        bool      inRecursiveFlag)
    {
        ChmodFunctor           theChmodFunc(inMode, inRecursiveFlag);
        FunctorT<ChmodFunctor> theFunc(theChmodFunc, cerr);
        return Apply(inArgsPtr, inArgCount, theFunc);
    }
    class MkdirFunctor
    {
    public:
        MkdirFunctor(
            kfsMode_t inMode,
            bool      inCreateAllFlag)
            : mMode(inMode),
              mCreateAllFlag(inCreateAllFlag)
            {}
        int operator()(
            FileSystem&    inFs,
            const string&  inPath,
            ErrorReporter& /* inErrorReporter */)
        {
            return inFs.Mkdir(inPath, mMode, mCreateAllFlag);
        }
    private:
        const kfsMode_t mMode;
        const bool      mCreateAllFlag;
    private:
        MkdirFunctor(
            const MkdirFunctor& inFunctor);
        MkdirFunctor& operator=(
            const MkdirFunctor& inFunctor);
    };
    int Mkdir(
        char**    inArgsPtr,
        int       inArgCount,
        kfsMode_t inMode,
        bool      inCreateAllFlag)
    {
        MkdirFunctor           theMkdirFunc(inMode, inCreateAllFlag);
        FunctorT<MkdirFunctor> theFunc(theMkdirFunc, cerr);
        return Apply(inArgsPtr, inArgCount, theFunc);
    }
    class GetGlobLastEntry
    {
    public:
        GetGlobLastEntry()
            : mFsPtr(0),
              mPathName()
            {}
        bool operator()(
            int&        ioGlobError,
            GlobResult& inGlobResult)
        {
            if (inGlobResult.size() <= 1 &&
                    inGlobResult.back().second.size() <= 1) {
                ioGlobError = -EINVAL;
                return false;
            }
            mFsPtr    = inGlobResult.back().first;
            mPathName = inGlobResult.back().second.back();
            inGlobResult.back().second.pop_back();
            return true;
        }
        FileSystem* GetFsPtr() const
            { return mFsPtr; }
        const string& GetPathName() const
            { return mPathName; }
    private:
        FileSystem* mFsPtr;
        string      mPathName;
    private:
        GetGlobLastEntry(
            const GetGlobLastEntry& inFunctor);
        GetGlobLastEntry& operator=(
            const GetGlobLastEntry& inFunctor);
    };
    class CopyFunctor
    {
    public:
        CopyFunctor()
            {}
        int operator()(
            FileSystem&    inFs,
            const string&  inPath,
            ErrorReporter& /* inErrorReporter */)
        {
            // Implement copy.
            return 1;
        }
    private:
        CopyFunctor(
            const CopyFunctor& inFunctor);
        CopyFunctor& operator=(
            const CopyFunctor& inFunctor);
    };
    int Copy(
        char** inArgsPtr,
        int    inArgCount)
    {
        CopyFunctor theCopyFunc;
        FunctorT
        <
            CopyFunctor,
            GetGlobLastEntry,
            true>   theFunc(theCopyFunc, cerr);
        return Apply(inArgsPtr, inArgCount, theFunc);
    }
private:
    size_t mIoBufferSize;
    char*  mIoBufferPtr;
private:
    KfsTool(const KfsTool& inTool);
    KfsTool& operator=(const KfsTool& inTool);
};

}
}

int
main(int argc, char** argv)
{
    KFS::tools::KfsTool theTool;
    return theTool.Run(argc, argv);
}
