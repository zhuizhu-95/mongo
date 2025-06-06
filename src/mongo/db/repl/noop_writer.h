/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/optime.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/duration.h"

#include <memory>

namespace mongo {
namespace repl {


/**
 * Once scheduled, the NoopWriter will periodically write a noop to the oplog if the replication
 * coordinator's optime has not changed since the last time it did a write.
 */
class NoopWriter {
    NoopWriter(const NoopWriter&) = delete;
    NoopWriter& operator=(const NoopWriter&) = delete;

public:
    NoopWriter(Seconds waitTime);

    ~NoopWriter();

    Status startWritingPeriodicNoops(OpTime lastKnownOpTime);

    void stopWritingPeriodicNoops();

private:
    class PeriodicNoopRunner;

    void _writeNoop(OperationContext*);

    /**
     * NoopWriter will write a noop to the oplog every _writeInterval seconds.
     */
    const Seconds _writeInterval;

    /**
     * The last optime that has been seen by the noop writer.
     */
    OpTime _lastKnownOpTime;

    /**
     * Protects member data of this class during start and stop. There is no need to synchronize
     * access once its running because its run by a one thread only.
     */
    mutable stdx::mutex _mutex;

    std::unique_ptr<PeriodicNoopRunner> _noopRunner;
};

}  // namespace repl
}  // namespace mongo
