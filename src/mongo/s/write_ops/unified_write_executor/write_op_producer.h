/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/unified_write_executor/write_op.h"

#include <absl/container/btree_set.h>
#include <boost/optional.hpp>

namespace mongo {
namespace unified_write_executor {

/**
 * This class returns a set of write ops one at a time with incrementing id. It is possible to mark
 * any returned inactive write op to be active again in order for the producer to return it another
 * time.
 */
class WriteOpProducer {
public:
    /**
     * Peek the current active write op without advancing the internal pointer. Repeated calls
     * return the same write op. When no active write op is left, return empty.
     */
    virtual boost::optional<WriteOp> peekNext() = 0;

    /**
     * Mark the current write op as inactive and advance the internal pointer to the next active
     * write op.
     */
    virtual void advance();

    /**
     * Mark a write op as active. The internal pointer will be updated to the active write op with
     * the lowest id.
     */
    virtual void markOpReprocess(const WriteOp& op);

protected:
    absl::btree_set<size_t> _activeIndices;
};

template <typename RequestType>
class MultiWriteOpProducer : public WriteOpProducer {
public:
    MultiWriteOpProducer(const RequestType& request) : _request(request) {
        size_t numOps = extractNumOperations(_request);
        populateActiveIndices(numOps);
    }

    boost::optional<WriteOp> peekNext() override {
        if (_activeIndices.empty()) {
            return boost::none;
        }
        return WriteOp(_request, *_activeIndices.begin());
    }

private:
    size_t extractNumOperations(const BulkWriteCommandRequest& request) const {
        return request.getOps().size();
    }
    size_t extractNumOperations(const BatchedCommandRequest& request) const {
        switch (request.getBatchType()) {
            case BatchedCommandRequest::BatchType_Insert:
                return request.getInsertRequest().getDocuments().size();
            case BatchedCommandRequest::BatchType_Update:
                return request.getUpdateRequest().getUpdates().size();
            case BatchedCommandRequest::BatchType_Delete:
                return request.getDeleteRequest().getDeletes().size();
            default:
                uasserted(ErrorCodes::BadValue, "Unknown operation type");
        }
    }
    void populateActiveIndices(size_t numOps) {
        for (size_t i = 0; i < numOps; ++i) {
            _activeIndices.insert(i);
        }
    }
    const RequestType& _request;
};

}  // namespace unified_write_executor
}  // namespace mongo
