/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_projection.h"

#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"

namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(_internalProjection, DocumentSourceInternalProjection::id)

namespace {
ProjectionPolicies lookUpPolicies(InternalProjectionPolicyEnum policiesId) {
    switch (policiesId) {
        case InternalProjectionPolicyEnum::kAggregate:
            return ProjectionPolicies::aggregateProjectionPolicies();
        case InternalProjectionPolicyEnum::kAddFields:
            return ProjectionPolicies::addFieldsProjectionPolicies();
    }
    MONGO_UNREACHABLE;
}
}  // namespace

/**
 * DocumentSourceInternalProjectionSpec constructor.
 */
DocumentSourceInternalProjection::DocumentSourceInternalProjection(
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    DocumentSourceInternalProjectionSpec spec)
    : DocumentSource(kStageNameInternal, pExpCtx),
      exec::agg::Stage(kStageNameInternal, pExpCtx),
      _stageSpec(std::move(spec)),
      _projection(projection_ast::parseAndAnalyze(
          pExpCtx, _stageSpec.getSpec(), lookUpPolicies(_stageSpec.getPolicies()))) {}

const char* DocumentSourceInternalProjection::getSourceName() const {
    return kStageNameInternal.data();
}

DocumentSourceContainer::iterator DocumentSourceInternalProjection::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    invariant(*itr == this);
    return itr;
}

Value DocumentSourceInternalProjection::serialize(const SerializationOptions& opts) const {
    return Value(Document{{getSourceName(), _stageSpec.toBSON()}});
}

DocumentSource::GetNextResult DocumentSourceInternalProjection::doGetNext() {
    tasserted(7824602, "Execution reached non-executable pipeline stage");
}
}  // namespace mongo
