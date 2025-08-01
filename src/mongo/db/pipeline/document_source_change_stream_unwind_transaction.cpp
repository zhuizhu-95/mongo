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


#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/change_stream_rewrite_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/transaction/transaction_history_iterator.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <list>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_INTERNAL_DOCUMENT_SOURCE(_internalChangeStreamUnwindTransaction,
                                  LiteParsedDocumentSourceChangeStreamInternal::parse,
                                  DocumentSourceChangeStreamUnwindTransaction::createFromBson,
                                  true);
ALLOCATE_DOCUMENT_SOURCE_ID(_internalChangeStreamUnwindTransaction,
                            DocumentSourceChangeStreamUnwindTransaction::id)

namespace {
const std::set<std::string> kUnwindExcludedFields = {"clusterTime", "lsid", "txnNumber"};
}

namespace change_stream_filter {
/**
 * Build a filter, similar to the optimized oplog filter, designed to reject individual transaction
 * entries that we know would eventually get rejected by the 'userMatch' filter if they continued
 * through the rest of the pipeline. We must also adjust the filter slightly for user rewrites, as
 * events within a transaction do not have certain fields that are common to other oplog entries.
 *
 * NB: The new filter may contain references to strings in the BSONObj that 'userMatch' originated
 * from. Callers that keep the new filter long-term should serialize and re-parse it to guard
 * against the possibility of stale string references.
 */
std::unique_ptr<MatchExpression> buildUnwindTransactionFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MatchExpression* userMatch,
    std::vector<BSONObj>& bsonObj) {
    // The transaction unwind filter is the same as the operation filter applied to the oplog. This
    // includes a namespace filter, which ensures that it will discard all documents that would be
    // filtered out by the default 'ns' filter this stage gets initialized with.
    auto unwindFilter =
        std::make_unique<AndMatchExpression>(buildOperationFilter(expCtx, nullptr, bsonObj));

    // To correctly handle filtering out entries of direct write operations on orphaned documents,
    // we include a filter for "fromMigrate" flagged operations, unless "fromMigrate" events are
    // explicitly requested in the spec.
    if (!expCtx->getChangeStreamSpec()->getShowMigrationEvents()) {
        unwindFilter->add(buildNotFromMigrateFilter(expCtx, userMatch, bsonObj));
    }

    // Attempt to rewrite the user's filter and combine it with the standard operation filter. We do
    // this separately because we need to exclude certain fields from the user's filters. Unwound
    // transaction events do not have these fields until we populate them from the commitTransaction
    // event. We already applied these predicates during the oplog scan, so we know that they match.
    if (auto rewrittenMatch = change_stream_rewrite::rewriteFilterForFields(
            expCtx, userMatch, bsonObj, {}, kUnwindExcludedFields)) {
        unwindFilter->add(std::move(rewrittenMatch));
    }
    return optimizeMatchExpression(std::move(unwindFilter));
}

/**
 * This filter is only used internally, to test the applicability of the EOT event we generate for
 * unprepared transactions.
 */
std::unique_ptr<MatchExpression> buildEndOfTransactionFilter(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (!expCtx->getChangeStreamSpec()->getShowExpandedEvents()) {
        return std::make_unique<AlwaysFalseMatchExpression>();
    }

    auto nsRegex = DocumentSourceChangeStream::getNsRegexForChangeStream(expCtx);
    return std::make_unique<RegexMatchExpression>(
        "o2.endOfTransaction"_sd, nsRegex, "" /*options*/);
}
}  // namespace change_stream_filter

boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction>
DocumentSourceChangeStreamUnwindTransaction::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::vector<BSONObj> bsonObj = std::vector<BSONObj>{};
    std::unique_ptr<MatchExpression> matchExpr =
        change_stream_filter::buildUnwindTransactionFilter(expCtx, nullptr, bsonObj);
    return new DocumentSourceChangeStreamUnwindTransaction(matchExpr->serialize(), expCtx);
}

boost::intrusive_ptr<DocumentSourceChangeStreamUnwindTransaction>
DocumentSourceChangeStreamUnwindTransaction::createFromBson(
    const BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(5467605,
            str::stream() << "the '" << kStageName << "' stage spec must be an object",
            elem.type() == BSONType::object);
    auto parsedSpec = DocumentSourceChangeStreamUnwindTransactionSpec::parse(
        IDLParserContext("DocumentSourceChangeStreamUnwindTransactionSpec"), elem.Obj());
    return new DocumentSourceChangeStreamUnwindTransaction(parsedSpec.getFilter(), expCtx);
}

DocumentSourceChangeStreamUnwindTransaction::DocumentSourceChangeStreamUnwindTransaction(
    BSONObj filter, const boost::intrusive_ptr<ExpressionContext>& expCtx)
    : DocumentSourceInternalChangeStreamStage(kStageName, expCtx) {
    rebuild(std::move(filter));
}

void DocumentSourceChangeStreamUnwindTransaction::rebuild(BSONObj filter) {
    _filter = filter.getOwned();
    _expression = MatchExpressionParser::parseAndNormalize(filter, pExpCtx);
}

StageConstraints DocumentSourceChangeStreamUnwindTransaction::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints(StreamType::kStreaming,
                                 PositionRequirement::kNone,
                                 HostTypeRequirement::kNone,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kChangeStreamStage);
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

Value DocumentSourceChangeStreamUnwindTransaction::doSerialize(
    const SerializationOptions& opts) const {
    tassert(7481400, "expression has not been initialized", _expression);

    if (opts.isSerializingForExplain()) {
        BSONObjBuilder builder;
        builder.append("stage"_sd, "internalUnwindTransaction"_sd);
        builder.append(DocumentSourceChangeStreamUnwindTransactionSpec::kFilterFieldName,
                       _expression->serialize(opts));

        return Value(DOC(DocumentSourceChangeStream::kStageName << builder.obj()));
    }

    // 'SerializationOptions' are not required here, since serialization for explain and query
    // stats occur before this function call.
    return Value(Document{
        {kStageName, Value{DocumentSourceChangeStreamUnwindTransactionSpec{_filter}.toBSON()}}});
}

DepsTracker::State DocumentSourceChangeStreamUnwindTransaction::getDependencies(
    DepsTracker* deps) const {
    deps->fields.insert(std::string{repl::OplogEntry::kOpTypeFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTimestampFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kObjectFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kSessionIdFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTermFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kTxnNumberFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kWallClockTimeFieldName});
    deps->fields.insert(std::string{repl::OplogEntry::kMultiOpTypeFieldName});

    return DepsTracker::State::SEE_NEXT;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamUnwindTransaction::getModifiedPaths()
    const {
    return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
}

DocumentSource::GetNextResult DocumentSourceChangeStreamUnwindTransaction::doGetNext() {
    uassert(5543812,
            str::stream() << kStageName << " cannot be executed from router",
            !pExpCtx->getInRouter());

    while (true) {
        // If we're unwinding an 'applyOps' from a transaction, check if there are any documents
        // we have stored that can be returned.
        if (_txnIterator) {
            if (auto next = _txnIterator->getNextTransactionOp(pExpCtx->getOperationContext())) {
                return std::move(*next);
            }

            _txnIterator = boost::none;
        }

        // Get the next input document.
        auto input = pSource->getNext();
        if (!input.isAdvanced()) {
            return input;
        }

        auto doc = input.releaseDocument();

        // If the oplog entry is not part of a transaction, allow it to pass through.
        if (!_isTransactionOplogEntry(doc)) {
            return doc;
        }

        // The only two commands we will see here are an applyOps or a commit, which both mean
        // we need to open a "transaction context" representing a group of updates that all
        // occurred at once as part of a transaction. If we already have a transaction context
        // open, that would mean we are looking at an applyOps or commit nested within an
        // applyOps, which is not allowed in the oplog.
        tassert(5543801, "Transaction iterator not found", !_txnIterator);

        // Once we initialize the transaction iterator, we can loop back to the top in order to
        // call 'getNextTransactionOp' on it. Note that is possible for the transaction iterator
        // to be empty of any relevant operations, meaning that this loop may need to execute
        // multiple times before it encounters a relevant change to return.
        _txnIterator.emplace(pExpCtx, doc, _expression.get());
    }
}

bool DocumentSourceChangeStreamUnwindTransaction::_isTransactionOplogEntry(const Document& doc) {
    auto op = doc[repl::OplogEntry::kOpTypeFieldName];
    auto opType = repl::OpType_parse(IDLParserContext("ChangeStreamEntry.op"), op.getStringData());
    auto commandVal = doc["o"];

    if (opType != repl::OpTypeEnum::kCommand ||
        (commandVal["applyOps"].missing() && commandVal["commitTransaction"].missing())) {
        // We should never see an "abortTransaction" command at this point.
        tassert(5543802,
                str::stream() << "Unexpected op at " << doc["ts"].getTimestamp().toString(),
                opType != repl::OpTypeEnum::kCommand || commandVal["abortTransaction"].missing());
        return false;
    }

    return true;
}

DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::TransactionOpIterator(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const Document& input,
    const MatchExpression* expression)
    : _mongoProcessInterface(expCtx->getMongoProcessInterface()),
      _expression(expression),
      _endOfTransactionExpression(change_stream_filter::buildEndOfTransactionFilter(expCtx)) {

    Value multiOpTypeValue = input[repl::OplogEntry::kMultiOpTypeFieldName];
    DocumentSourceChangeStream::checkValueTypeOrMissing(
        multiOpTypeValue, repl::OplogEntry::kMultiOpTypeFieldName, BSONType::numberInt);
    const bool applyOpsAppliedSeparately = !multiOpTypeValue.missing() &&
        multiOpTypeValue.getInt() == int(repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);

    // The lsid and txnNumber can be missing in case of batched writes, and are ignored when
    // multiOpType is kApplyOpsAppliedSeparately.  The latter indicates an applyOps that is part of
    // a retryable write and not a multi-document transaction.
    if (!applyOpsAppliedSeparately) {
        Value lsidValue = input["lsid"];
        DocumentSourceChangeStream::checkValueTypeOrMissing(lsidValue, "lsid", BSONType::object);
        _lsid =
            lsidValue.missing() ? boost::none : boost::optional<Document>(lsidValue.getDocument());
        Value txnNumberValue = input["txnNumber"];
        DocumentSourceChangeStream::checkValueTypeOrMissing(
            txnNumberValue, "txnNumber", BSONType::numberLong);
        _txnNumber = txnNumberValue.missing()
            ? boost::none
            : boost::optional<TxnNumber>(txnNumberValue.getLong());
    }
    // We want to parse the OpTime out of this document using the BSON OpTime parser. Instead of
    // converting the entire Document back to BSON, we convert only the fields we need.
    repl::OpTime txnOpTime = repl::OpTime::parse(BSON(repl::OpTime::kTimestampFieldName
                                                      << input[repl::OpTime::kTimestampFieldName]
                                                      << repl::OpTime::kTermFieldName
                                                      << input[repl::OpTime::kTermFieldName]));
    _clusterTime = txnOpTime.getTimestamp();

    Value wallTime = input[repl::OplogEntry::kWallClockTimeFieldName];
    DocumentSourceChangeStream::checkValueType(
        wallTime, repl::OplogEntry::kWallClockTimeFieldName, BSONType::date);
    _wallTime = wallTime.getDate();

    auto commandObj = input["o"].getDocument();
    Value applyOps = commandObj["applyOps"];

    if (!applyOps.missing()) {
        // We found an applyOps that implicitly commits a transaction. We include it in the
        // '_txnOplogEntries' stack of applyOps entries that the change stream should process as
        // part of this transaction. There may be additional applyOps entries linked through the
        // 'prevOpTime' field, which will also get added to '_txnOplogEntries' later in this
        // function. Note that this style of transaction does not have a 'commitTransaction'
        // command.
        _txnOplogEntries.push(txnOpTime);
    } else {
        // This must be a "commitTransaction" command, which commits a prepared transaction.
        // This style of transaction does not have an applyOps entry that implicitly commits it,
        // as in the previous case. We're going to iterate through the other oplog entries in
        // the transaction, but this entry does not have any updates in it, so we do not include
        // it in the '_txnOplogEntries' stack.
        tassert(5543803,
                str::stream() << "Unexpected op at " << input["ts"].getTimestamp().toString(),
                !commandObj["commitTransaction"].missing());

        if (auto commitTimestamp = commandObj["commitTimestamp"]; !commitTimestamp.missing()) {
            // Track commit timestamp of the prepared transaction if it's present in the oplog
            // entry.
            _commitTimestamp = commitTimestamp.getTimestamp();
        }
    }

    // We need endOfTransaction only for unprepared transactions: so this must be an applyOps with
    // set lsid and txnNumber but not a retryable write.
    _needEndOfTransaction = feature_flags::gFeatureFlagEndOfTransactionChangeEvent.isEnabled(
                                serverGlobalParams.featureCompatibility.acquireFCVSnapshot()) &&
        !applyOps.missing() && !applyOpsAppliedSeparately && _lsid.has_value() &&
        _txnNumber.has_value();

    // If there's no previous optime, or if this applyOps is of the kApplyOpsAppliedSeparately
    // multiOptype, we don't need to collect other apply ops operations.
    if (!applyOpsAppliedSeparately &&
        BSONType::object ==
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getType()) {
        // As with the 'txnOpTime' parsing above, we convert a portion of 'input' back to BSON
        // in order to parse an OpTime, this time from the "prevOpTime" field.
        repl::OpTime prevOpTime = repl::OpTime::parse(
            input[repl::OplogEntry::kPrevWriteOpTimeInTransactionFieldName].getDocument().toBson());
        _collectAllOpTimesFromTransaction(expCtx->getOperationContext(), prevOpTime);
    }

    // Pop the first OpTime off the stack and use it to load the first oplog entry into the
    // '_currentApplyOps' field.
    tassert(5543804, "No transaction oplog entries found", _txnOplogEntries.size() > 0);
    const auto firstTimestamp = _txnOplogEntries.top();
    _txnOplogEntries.pop();

    if (firstTimestamp == txnOpTime) {
        // This transaction consists of only one oplog entry, from which we have already
        // extracted the "applyOps" array, so there is no need to do any more work.
        tassert(5543805,
                str::stream() << "Expected no transaction entries, found "
                              << _txnOplogEntries.size(),
                _txnOplogEntries.size() == 0);
        _currentApplyOps = std::move(applyOps);
    } else {
        // This transaction consists of multiple oplog entries; grab the chronologically first
        // entry and extract its "applyOps" array.
        auto firstApplyOpsEntry =
            _lookUpOplogEntryByOpTime(expCtx->getOperationContext(), firstTimestamp);

        auto bsonOp = firstApplyOpsEntry.getOperationToApply();
        tassert(5543806,
                str::stream() << "Expected 'applyOps' type " << BSONType::array << ", found "
                              << bsonOp["applyOps"].type(),
                BSONType::array == bsonOp["applyOps"].type());
        _currentApplyOps = Value(bsonOp["applyOps"]);
    }

    DocumentSourceChangeStream::checkValueType(_currentApplyOps, "applyOps", BSONType::array);

    // Initialize iterators at the beginning of the transaction.
    _currentApplyOpsIt = _currentApplyOps.getArray().begin();
    _currentApplyOpsTs = firstTimestamp.getTimestamp();
    _currentApplyOpsIndex = 0;
    _txnOpIndex = 0;
}

boost::optional<Document>
DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::getNextTransactionOp(
    OperationContext* opCtx) {
    while (true) {
        while (_currentApplyOpsIt != _currentApplyOps.getArray().end()) {
            Document doc = (_currentApplyOpsIt++)->getDocument();
            ++_currentApplyOpsIndex;
            ++_txnOpIndex;

            _assertExpectedTransactionEventFormat(doc);

            if (_needEndOfTransaction) {
                _addAffectedNamespaces(doc);
            }
            // If the document is relevant, update it with the required txn fields before returning.
            if (exec::matcher::matchesBSON(_expression, doc.toBson())) {
                return _addRequiredTransactionFields(doc);
            }
        }

        if (_txnOplogEntries.empty()) {
            // There are no more operations in this transaction.
            return _createEndOfTransactionIfNeeded();
        }

        // We've processed all the operations in the previous applyOps entry, but we have a new
        // one to process.
        auto applyOpsEntry = _lookUpOplogEntryByOpTime(opCtx, _txnOplogEntries.top());
        _txnOplogEntries.pop();

        auto bsonOp = applyOpsEntry.getOperationToApply();
        tassert(5543807,
                str::stream() << "Expected 'applyOps' type " << BSONType::array << ", found "
                              << bsonOp["applyOps"].type(),
                BSONType::array == bsonOp["applyOps"].type());

        _currentApplyOps = Value(bsonOp["applyOps"]);
        _currentApplyOpsTs = applyOpsEntry.getTimestamp();
        _currentApplyOpsIt = _currentApplyOps.getArray().begin();
        _currentApplyOpsIndex = 0;
    }
}

void DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::
    _assertExpectedTransactionEventFormat(const Document& d) const {
    tassert(5543808,
            str::stream() << "Unexpected format for entry within a transaction oplog entry: "
                             "'op' field was type "
                          << typeName(d["op"].getType()),
            d["op"].getType() == BSONType::string);
    tassert(5543809,
            str::stream() << "Unexpected noop entry within a transaction "
                          << redact(d["o"].toString()),
            ValueComparator::kInstance.evaluate(d["op"] != Value("n"_sd)));
}

Document
DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::_addRequiredTransactionFields(
    const Document& doc) const {
    MutableDocument newDoc(doc);
    // The 'getNextTransactionOp' increments the '_txnOpIndex' by 1, to point to the next
    // transaction number. The 'txnOpIndex()' must be called to get the current transaction number.
    newDoc.addField(DocumentSourceChangeStream::kTxnOpIndexField,
                    Value(static_cast<long long>(txnOpIndex())));

    // The 'getNextTransactionOp' updates the '_currentApplyOpsIndex' to point to the next entry in
    // the '_currentApplyOps' array. The 'applyOpsIndex()' method must be called to get the index of
    // the current entry.
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsIndexField,
                    Value(static_cast<long long>(applyOpsIndex())));
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsTsField, Value(applyOpsTs()));

    newDoc.addField(repl::OplogEntry::kTimestampFieldName, Value(_clusterTime));
    newDoc.addField(repl::OplogEntry::kWallClockTimeFieldName, Value(_wallTime));

    newDoc.addField(DocumentSourceChangeStream::kCommitTimestampField,
                    _commitTimestamp ? Value(*_commitTimestamp) : Value());

    newDoc.addField(repl::OplogEntry::kSessionIdFieldName, _lsid ? Value(*_lsid) : Value());
    newDoc.addField(repl::OplogEntry::kTxnNumberFieldName,
                    _txnNumber ? Value(static_cast<long long>(*_txnNumber)) : Value());

    return newDoc.freeze();
}

repl::OplogEntry
DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::_lookUpOplogEntryByOpTime(
    OperationContext* opCtx, repl::OpTime lookupTime) const {
    tassert(5543811, "Cannot look up transaction entry with null op time", !lookupTime.isNull());

    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(lookupTime));

    try {
        return iterator->next(opCtx);
    } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>& ex) {
        ex.addContext(
            "Oplog no longer has history necessary for $changeStream to observe operations "
            "from a "
            "committed transaction.");
        uasserted(ErrorCodes::ChangeStreamHistoryLost, ex.reason());
    }
}

void DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::
    _collectAllOpTimesFromTransaction(OperationContext* opCtx, repl::OpTime firstOpTime) {
    std::unique_ptr<TransactionHistoryIteratorBase> iterator(
        _mongoProcessInterface->createTransactionHistoryIterator(firstOpTime));

    try {
        while (iterator->hasNext()) {
            _txnOplogEntries.push(iterator->nextOpTime(opCtx));
        }
    } catch (ExceptionFor<ErrorCodes::IncompleteTransactionHistory>& ex) {
        ex.addContext(
            "Oplog no longer has history necessary for $changeStream to observe operations "
            "from a "
            "committed transaction.");
        uasserted(ErrorCodes::ChangeStreamHistoryLost, ex.reason());
    }
}

void DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::_addAffectedNamespaces(
    const Document& doc) {
    // TODO SERVER-78670:  move namespace extraction to change_stream_helpers.cpp and make generic
    const auto tid = [&]() -> boost::optional<TenantId> {
        if (!gMultitenancySupport) {
            return boost::none;
        }
        const auto tidField = doc["tid"];
        return !tidField.missing() ? boost::make_optional(TenantId(tidField.getOid()))
                                   : boost::none;
    }();

    const auto dbCmdNs = NamespaceStringUtil::deserialize(
        tid, doc["ns"].getStringData(), SerializationContext::stateDefault());
    if (doc["op"].getStringData() != "c") {
        _affectedNamespaces.insert(dbCmdNs);
        return;
    }

    constexpr std::array<StringData, 2> kCollectionField = {"create"_sd, "createIndexes"_sd};
    const Document& object = doc["o"].getDocument();
    for (const auto& fieldName : kCollectionField) {
        const auto field = object[fieldName];
        if (field.getType() == BSONType::string) {
            _affectedNamespaces.insert(
                NamespaceStringUtil::deserialize(dbCmdNs.dbName(), field.getStringData()));
            return;
        }
    }

    tasserted(7694300,
              str::stream() << "Unexpected op in applyOps at " << _currentApplyOpsTs << " index "
                            << _currentApplyOpsIndex << " object " << redact(object.toString()));
}

boost::optional<Document> DocumentSourceChangeStreamUnwindTransaction::TransactionOpIterator::
    _createEndOfTransactionIfNeeded() {
    if (!_needEndOfTransaction || _endOfTransactionReturned) {
        return boost::none;
    }
    ++_currentApplyOpsIndex;
    ++_txnOpIndex;
    _endOfTransactionReturned = true;

    std::vector<NamespaceString> namespaces{_affectedNamespaces.begin(), _affectedNamespaces.end()};
    auto lsid = LogicalSessionId::parse(IDLParserContext("LogicalSessionId"), _lsid->toBson());
    repl::MutableOplogEntry oplogEntry = change_stream::createEndOfTransactionOplogEntry(
        lsid, *_txnNumber, namespaces, _clusterTime, _wallTime);

    MutableDocument newDoc(Document(oplogEntry.toBSON()));
    newDoc.addField(DocumentSourceChangeStream::kTxnOpIndexField,
                    Value(static_cast<long long>(txnOpIndex())));
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsIndexField,
                    Value(static_cast<long long>(applyOpsIndex())));
    newDoc.addField(DocumentSourceChangeStream::kApplyOpsTsField, Value(applyOpsTs()));

    Document endOfTransaction = newDoc.freeze();
    return {
        exec::matcher::matchesBSON(_endOfTransactionExpression.get(), endOfTransaction.toBson()),
        endOfTransaction};
}

DocumentSourceContainer::iterator DocumentSourceChangeStreamUnwindTransaction::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(5687205, "Iterator mismatch during optimization", *itr == this);

    auto nextChangeStreamStageItr = std::next(itr);

    // The additional filtering added by this optimization may incorrectly filter out events if it
    // runs with the non-simple collation.
    if (pExpCtx->getCollator()) {
        return nextChangeStreamStageItr;
    }

    // Seek to the stage that immediately follows the change streams stages.
    itr = std::find_if_not(itr, container->end(), [](const auto& stage) {
        return stage->constraints().isChangeStreamStage();
    });

    if (itr == container->end()) {
        // This pipeline is just the change stream.
        return itr;
    }

    auto matchStage = dynamic_cast<DocumentSourceMatch*>(itr->get());
    if (!matchStage) {
        // This function only attempts to optimize a $match that immediately follows expanded
        // $changeStream stages. That does not apply here, and we resume optimization at the last
        // change stream stage, in case a "swap" optimization can apply between it and the stage
        // that follows it. For example, $project stages can swap in front of the last change stream
        // stages.
        return std::prev(itr);
    }

    // Build a filter which discards unwound transaction operations which would have been filtered
    // out later in the pipeline by the user's $match filter.
    std::vector<BSONObj> bsonObj = std::vector<BSONObj>{};
    auto unwindTransactionFilter = change_stream_filter::buildUnwindTransactionFilter(
        pExpCtx, matchStage->getMatchExpression(), bsonObj);

    // Replace the default filter with the new, more restrictive one. Note that the resulting
    // expression must be serialized then deserialized to ensure it does not retain unsafe
    // references to strings in the 'matchStage' filter.
    rebuild(unwindTransactionFilter->serialize());

    // Continue optimization at the next change stream stage.
    return nextChangeStreamStageItr;
}
}  // namespace mongo
