// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyBucketWork.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SHA.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

#include <fstream>

namespace stellar
{

VerifyBucketWork::VerifyBucketWork(
    Application& app, std::map<std::string, std::shared_ptr<Bucket>>& buckets,
    std::string const& bucketFile, uint256 const& hash)
    : BasicWork(app, "verify-bucket-hash-" + bucketFile, RETRY_NEVER)
    , mBuckets(buckets)
    , mBucketFile(bucketFile)
    , mHash(hash)
    , mVerifyBucketSuccess(app.getMetrics().NewMeter(
          {"history", "verify-bucket", "success"}, "event"))
    , mVerifyBucketFailure(app.getMetrics().NewMeter(
          {"history", "verify-bucket", "failure"}, "event"))
{
}

BasicWork::State
VerifyBucketWork::onRun()
{
    if (mDone)
    {
        if (mEc)
        {
            mVerifyBucketFailure.Mark();
            return State::WORK_FAILURE;
        }

        adoptBucket();
        mVerifyBucketSuccess.Mark();
        return State::WORK_SUCCESS;
    }

    spawnVerifier();
    return State::WORK_WAITING;
}

void
VerifyBucketWork::adoptBucket()
{
    assert(mDone);
    assert(!mEc);

    auto b = mApp.getBucketManager().adoptFileAsBucket(mBucketFile, mHash,
                                                       /*objectsPut=*/0,
                                                       /*bytesPut=*/0);
    mBuckets[binToHex(mHash)] = b;
}

void
VerifyBucketWork::spawnVerifier()
{
    std::string filename = mBucketFile;
    uint256 hash = mHash;
    Application& app = this->mApp;
    std::weak_ptr<VerifyBucketWork> weak(
        std::static_pointer_cast<VerifyBucketWork>(shared_from_this()));
    app.postOnBackgroundThread(
        [&app, filename, weak, hash]() {
            auto hasher = SHA256::create();
            asio::error_code ec;
            {
                // ensure that the stream gets its own scope to avoid race with
                // main thread
                std::ifstream in(filename, std::ifstream::binary);
                char buf[4096];
                while (in)
                {
                    in.read(buf, sizeof(buf));
                    hasher->add(ByteSlice(buf, in.gcount()));
                }
                uint256 vHash = hasher->finish();
                if (vHash == hash)
                {
                    CLOG(DEBUG, "History")
                        << "Verified hash (" << hexAbbrev(hash) << ") for "
                        << filename;
                }
                else
                {
                    CLOG(WARNING, "History")
                        << "FAILED verifying hash for " << filename;
                    CLOG(WARNING, "History")
                        << "expected hash: " << binToHex(hash);
                    CLOG(WARNING, "History")
                        << "computed hash: " << binToHex(vHash);
                    ec = std::make_error_code(std::errc::io_error);
                }
            }

            // Not ideal, but needed to prevent race conditions with
            // main thread, since BasicWork's state is not thread-safe. This is
            // a temporary workaround, as a cleaner solution is needed.
            app.postOnMainThread(
                [weak, ec]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        self->mEc = ec;
                        self->mDone = true;
                        self->wakeUp();
                    }
                },
                "VerifyBucket: finish");
        },
        "VerifyBucket: start in background");
}
}