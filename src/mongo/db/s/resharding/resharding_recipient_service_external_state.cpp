/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_recipient_service_external_state.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/resharding/resharding_donor_recipient_common.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/grid.h"
#include "mongo/s/resharding/common_types_gen.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"

#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

namespace mongo {
namespace {

const WriteConcernOptions kMajorityWriteConcern{
    WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::UNSET, Seconds(0)};

}  // namespace

void ReshardingRecipientService::RecipientStateMachineExternalState::
    ensureTempReshardingCollectionExistsWithIndexes(OperationContext* opCtx,
                                                    const CommonReshardingMetadata& metadata,
                                                    Timestamp cloneTimestamp) {
    LOGV2_DEBUG(5002300,
                1,
                "Creating temporary resharding collection",
                "sourceNamespace"_attr = metadata.getSourceNss());

    // The CatalogCache may have a collection entry from before the source collection was sharded.
    // We force a refresh to have this shard realize the collection is now sharded.
    refreshCatalogCache(opCtx, metadata.getSourceNss());

    auto [collOptions, unusedUUID] = getCollectionOptions(
        opCtx,
        metadata.getSourceNss(),
        metadata.getSourceUUID(),
        cloneTimestamp,
        "loading collection options to create temporary resharding collection"_sd);

    auto [indexes, idIndex] =
        getCollectionIndexes(opCtx,
                             metadata.getSourceNss(),
                             metadata.getSourceUUID(),
                             cloneTimestamp,
                             "loading indexes to create temporary resharding collection"_sd);

    // Set the temporary resharding collection's UUID to the resharding UUID. Note that
    // BSONObj::addFields() replaces any fields that already exist.
    collOptions = collOptions.addFields(BSON("uuid" << metadata.getReshardingUUID()));
    CollectionOptionsAndIndexes collOptionsAndIndexes{metadata.getReshardingUUID(),
                                                      std::move(indexes),
                                                      std::move(idIndex),
                                                      std::move(collOptions)};
    // The indexSpecs are cleared here so we don't create those indexes when creating temp
    // collections. These indexes will be fetched and built during building-index stage.
    collOptionsAndIndexes.indexSpecs = {};
    MigrationDestinationManager::cloneCollectionIndexesAndOptions(
        opCtx, metadata.getTempReshardingNss(), collOptionsAndIndexes);

    const auto coll = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, metadata.getTempReshardingNss(), AcquisitionPrerequisites::kWrite),
        MODE_IX);
    CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(
        opCtx, metadata.getTempReshardingNss())
        ->clearFilteringMetadata(opCtx);
}

ShardId RecipientStateMachineExternalStateImpl::myShardId(ServiceContext* serviceContext) const {
    return ShardingState::get(serviceContext)->shardId();
}

void RecipientStateMachineExternalStateImpl::refreshCatalogCache(OperationContext* opCtx,
                                                                 const NamespaceString& nss) {
    uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));
}

CollectionRoutingInfo RecipientStateMachineExternalStateImpl::getTrackedCollectionRoutingInfo(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto cri =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                          << " to be tracked",
            cri.hasRoutingTable());
    return cri;
}

MigrationDestinationManager::CollectionOptionsAndUUID
RecipientStateMachineExternalStateImpl::getCollectionOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& uuid,
    boost::optional<Timestamp> afterClusterTime,
    StringData reason) {
    // Load the collection options from the primary shard for the database.
    sharding::router::DBPrimaryRouter router(opCtx->getServiceContext(), nss.dbName());
    return router.route(opCtx, reason, [&](OperationContext* opCtx, const CachedDatabaseInfo& cdb) {
        return MigrationDestinationManager::getCollectionOptions(
            opCtx,
            NamespaceStringOrUUID{nss.dbName(), uuid},
            cdb->getPrimary(),
            cdb->getVersion(),
            afterClusterTime);
    });
}

MigrationDestinationManager::CollectionOptionsAndUUID
RecipientStateMachineExternalStateImpl::getCollectionOptions(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const UUID& uuid,
    boost::optional<Timestamp> afterClusterTime,
    StringData reason,
    const ShardId& fromShardId) {
    // Load the collection options from the specified shard for the database.
    const auto nssOrUUID = NamespaceStringOrUUID{nss.dbName(), uuid};
    return MigrationDestinationManager::getCollectionOptions(
        opCtx, nssOrUUID, fromShardId, boost::none, afterClusterTime);
}

MigrationDestinationManager::IndexesAndIdIndex
RecipientStateMachineExternalStateImpl::getCollectionIndexes(OperationContext* opCtx,
                                                             const NamespaceString& nss,
                                                             const UUID& uuid,
                                                             Timestamp afterClusterTime,
                                                             StringData reason,
                                                             bool expandSimpleCollation) {
    // Load the list of indexes from the shard which owns the global minimum chunk.
    sharding::router::CollectionRouter router(opCtx->getServiceContext(), nss);
    return router.route(
        opCtx, reason, [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
            uassert(ErrorCodes::NamespaceNotFound,
                    str::stream() << "Expected collection " << nss.toStringForErrorMsg()
                                  << " to be tracked",
                    cri.hasRoutingTable());
            return MigrationDestinationManager::getCollectionIndexes(
                opCtx,
                nss,
                cri.getChunkManager().getMinKeyShardIdWithSimpleCollation(),
                cri,
                afterClusterTime,
                expandSimpleCollation);
        });
}

/**
 * A wrapper method that routes to `CollectionRouter`, primarily intended for use within
 * `RecipientStateMachineExternalState`. It facilitates testing and mocking scenarios,
 * particularly in unit tests such as `resharding_recipient_service_test.cpp`.
 */
void RecipientStateMachineExternalStateImpl::route(
    OperationContext* opCtx,
    const NamespaceString& nss,
    StringData reason,
    unique_function<void(OperationContext* opCtx, const CollectionRoutingInfo& cri)> callback) {
    sharding::router::CollectionRouter router(opCtx->getServiceContext(), nss);
    router.route(opCtx, reason, callback);
}

void RecipientStateMachineExternalStateImpl::updateCoordinatorDocument(OperationContext* opCtx,
                                                                       const BSONObj& query,
                                                                       const BSONObj& update) {
    auto catalogClient = Grid::get(opCtx)->catalogClient();
    auto docWasModified = uassertStatusOK(
        catalogClient->updateConfigDocument(opCtx,
                                            NamespaceString::kConfigReshardingOperationsNamespace,
                                            query,
                                            update,
                                            false, /* upsert */
                                            kMajorityWriteConcern,
                                            Milliseconds::max()));

    if (!docWasModified) {
        LOGV2_DEBUG(
            5543401,
            1,
            "Resharding coordinator document was not modified by the recipient's update; this is "
            "expected when the update had previously been interrupted due to a stepdown",
            "query"_attr = query,
            "update"_attr = update);
    }
}

void RecipientStateMachineExternalStateImpl::clearFilteringMetadataOnTempReshardingCollection(
    OperationContext* opCtx, const NamespaceString& tempReshardingNss) {
    stdx::unordered_set<NamespaceString> namespacesToRefresh{tempReshardingNss};
    resharding::clearFilteringMetadata(opCtx, namespacesToRefresh, true /* scheduleAsyncRefresh */);
}

}  // namespace mongo
