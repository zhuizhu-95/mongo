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

#include "mongo/s/write_ops/unified_write_executor/write_batch_executor.h"

#include "mongo/s/grid.h"
#include "mongo/s/transaction_router.h"
#include "mongo/s/write_ops/wc_error.h"
#include "mongo/s/write_ops/write_without_shard_key_util.h"

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace unified_write_executor {

WriteBatchResponse WriteBatchExecutor::execute(OperationContext* opCtx,
                                               RoutingContext& routingCtx,
                                               const WriteBatch& batch) {
    return std::visit([&](const auto& batchData) { return _execute(opCtx, routingCtx, batchData); },
                      batch.data);
}

std::vector<AsyncRequestsSender::Request> WriteBatchExecutor::buildBulkWriteRequests(
    OperationContext* opCtx, const SimpleWriteBatch& batch) const {
    std::vector<AsyncRequestsSender::Request> requestsToSend;
    for (auto& [shardId, shardRequest] : batch.requestByShardId) {
        std::vector<BulkWriteOpVariant> bulkOps;
        std::vector<NamespaceInfoEntry> nsInfos;
        std::map<NamespaceString, int> nsIndexMap;
        const bool inTransaction = static_cast<bool>(TransactionRouter::get(opCtx));
        const bool isRetryableWrite = opCtx->isRetryableWrite();
        std::vector<int> stmtIds;
        if (isRetryableWrite) {
            stmtIds.reserve(shardRequest.ops.size());
        }
        for (auto& op : shardRequest.ops) {
            auto& nss = op.getNss();
            auto versionIt = shardRequest.versionByNss.find(nss);
            tassert(10346801,
                    "The shard version info should be present in the batch",
                    versionIt != shardRequest.versionByNss.end());
            auto& version = versionIt->second;

            NamespaceInfoEntry nsInfo(nss);
            nsInfo.setShardVersion(version.shardVersion);
            nsInfo.setDatabaseVersion(version.databaseVersion);
            if (nsIndexMap.find(nsInfo.getNs()) == nsIndexMap.end()) {
                nsIndexMap[nsInfo.getNs()] = nsInfos.size();
                nsInfos.push_back(nsInfo);
            }
            auto nsIndex = nsIndexMap[nsInfo.getNs()];

            // Reassigns the namespace index for the list of ops.
            auto bulkOp = op.getBulkWriteOp();
            visit([&](auto& value) { return value.setNsInfoIdx(nsIndex); }, bulkOp);
            bulkOps.emplace_back(bulkOp);

            if (isRetryableWrite) {
                stmtIds.push_back(op.getEffectiveStmtId());
            }
        }

        auto bulkRequest = BulkWriteCommandRequest(std::move(bulkOps), std::move(nsInfos));
        bulkRequest.setOrdered(_context.getOrdered());
        bulkRequest.setBypassDocumentValidation(_context.getBypassDocumentValidation());
        bulkRequest.setLet(_context.getLet());
        if (_context.isBulkWriteCommand()) {
            bulkRequest.setErrorsOnly(_context.getErrorsOnly().value_or(false));
            bulkRequest.setComment(_context.getComment());
            bulkRequest.setMaxTimeMS(_context.getMaxTimeMS());
        }

        if (isRetryableWrite) {
            bulkRequest.setStmtIds(stmtIds);
        }
        BSONObjBuilder builder;
        bulkRequest.serialize(&builder);
        logical_session_id_helpers::serializeLsidAndTxnNumber(opCtx, &builder);
        auto writeConcern = getWriteConcernForShardRequest(opCtx);

        // We don't append write concern in a transaction.
        if (writeConcern && !inTransaction) {
            builder.append(WriteConcernOptions::kWriteConcernField, writeConcern->toBSON());
        }
        auto bulkRequestObj = builder.obj();
        LOGV2_DEBUG(10605503,
                    4,
                    "Constructed request for shard",
                    "request"_attr = bulkRequestObj,
                    "shardId"_attr = shardId);
        requestsToSend.emplace_back(shardId, std::move(bulkRequestObj));
    }
    return requestsToSend;
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const SimpleWriteBatch& batch) {
    std::vector<AsyncRequestsSender::Request> requestsToSend = buildBulkWriteRequests(opCtx, batch);

    // Note we check this rather than `isRetryableWrite()` because we do not want to retry
    // commands within retryable internal transactions.
    bool shouldRetry = opCtx->getTxnNumber() && !TransactionRouter::get(opCtx);
    auto sender = MultiStatementTransactionRequestsSender(
        opCtx,
        Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
        DatabaseName::kAdmin,
        std::move(requestsToSend),
        ReadPreferenceSetting(ReadPreference::PrimaryOnly),
        shouldRetry ? Shard::RetryPolicy::kIdempotent : Shard::RetryPolicy::kNoRetry);

    // Note that we sent a request for the involved namespaces after the requests get scheduled.
    for (const auto& nss : batch.getInvolvedNamespaces()) {
        routingCtx.onRequestSentForNss(nss);
    }

    SimpleWriteBatchResponse shardResponses;
    while (!sender.done()) {
        auto arsResponse = sender.next();
        ShardResponse shardResponse{std::move(arsResponse.swResponse),
                                    batch.requestByShardId.at(arsResponse.shardId).ops};
        shardResponses.emplace(std::move(arsResponse.shardId), std::move(shardResponse));
    }
    tassert(10346800,
            "There should same number of requests and responses from a simple write batch",
            shardResponses.size() == batch.requestByShardId.size());
    return shardResponses;
}

WriteBatchResponse WriteBatchExecutor::_execute(OperationContext* opCtx,
                                                RoutingContext& routingCtx,
                                                const NonTargetedWriteBatch& batch) {
    const WriteOp& writeOp = batch.op;
    // TODO SERVER-108144 maybe we shouldn't call release here or maybe we should acknowledge a
    // write based on the swRes.
    routingCtx.release(writeOp.getNss());
    auto bulkOp = writeOp.getBulkWriteOp();
    const auto& nss = writeOp.getNss();

    NamespaceInfoEntry nsInfo(nss);
    const int32_t nsIndex = 0;

    visit([&](auto& value) { return value.setNsInfoIdx(nsIndex); }, bulkOp);

    if (BulkWriteCRUDOp(bulkOp).getType() == BulkWriteCRUDOp::OpType::kUpdate) {
        const bool allowShardKeyUpdatesWithoutFullShardKeyInQuery =
            opCtx->isRetryableWrite() || opCtx->inMultiDocumentTransaction();

        get_if<BulkWriteUpdateOp>(&bulkOp)->setAllowShardKeyUpdatesWithoutFullShardKeyInQuery(
            allowShardKeyUpdatesWithoutFullShardKeyInQuery);
    }

    std::vector<BulkWriteOpVariant> bulkOps;
    std::vector<NamespaceInfoEntry> nsInfos;
    bulkOps.emplace_back(std::move(bulkOp));
    nsInfos.push_back(std::move(nsInfo));

    auto bulkRequest = BulkWriteCommandRequest(std::move(bulkOps), std::move(nsInfos));

    BSONObjBuilder builder;
    bulkRequest.serialize(&builder);
    BSONObj cmdObj = builder.obj();

    boost::optional<WriteConcernErrorDetail> wce;
    auto swRes =
        write_without_shard_key::runTwoPhaseWriteProtocol(opCtx, nss, std::move(cmdObj), wce);

    return NonTargetedWriteBatchResponse{std::move(swRes), std::move(wce), writeOp};
}

}  // namespace unified_write_executor
}  // namespace mongo
