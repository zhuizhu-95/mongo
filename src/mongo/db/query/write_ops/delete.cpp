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

#include "mongo/db/query/write_ops/delete.h"

#include "mongo/db/curop.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/shard_role.h"
#include "mongo/util/assert_util.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

long long deleteObjects(OperationContext* opCtx,
                        const CollectionAcquisition& collection,
                        BSONObj pattern,
                        bool justOne,
                        bool god,
                        bool fromMigrate) {
    auto request = DeleteRequest{};
    request.setNsString(collection.nss());
    request.setQuery(pattern);
    request.setMulti(!justOne);
    request.setGod(god);
    request.setFromMigrate(fromMigrate);
    request.setYieldPolicy(PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY);

    ParsedDelete parsedDelete(opCtx, &request, collection.getCollectionPtr());
    uassertStatusOK(parsedDelete.parseRequest());

    auto exec = uassertStatusOK(getExecutorDelete(
        &CurOp::get(opCtx)->debug(), collection, &parsedDelete, boost::none /* verbosity */));

    return exec->executeDelete();
}

DeleteResult deleteObject(OperationContext* opCtx,
                          const CollectionAcquisition& collection,
                          const DeleteRequest& request) {
    ParsedDelete parsedDelete(opCtx, &request, collection.getCollectionPtr());
    uassertStatusOK(parsedDelete.parseRequest());

    auto exec = uassertStatusOK(getExecutorDelete(
        &CurOp::get(opCtx)->debug(), collection, &parsedDelete, boost::none /* verbosity */));

    if (!request.getReturnDeleted()) {
        return {exec->executeDelete(), boost::none};
    }

    // This method doesn't support multi-deletes when returning pre-images.
    invariant(!request.getMulti());

    BSONObj image;
    if (exec->getNext(&image, nullptr) == PlanExecutor::IS_EOF) {
        return {};
    }

    return {1, image.getOwned()};
}

}  // namespace mongo
