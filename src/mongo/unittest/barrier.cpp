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

// IWYU pragma: no_include "cxxabi.h"


#include "mongo/unittest/barrier.h"

#include "mongo/util/assert_util.h"

namespace mongo {
namespace unittest {

Barrier::Barrier(size_t threadCount)
    : _threadCount(threadCount), _threadsWaiting(threadCount), _generation(0) {
    invariant(_threadCount > 0);
}

void Barrier::countDownAndWait() {
    stdx::unique_lock<stdx::mutex> _lock(_mutex);
    _threadsWaiting--;
    if (_threadsWaiting == 0) {
        _generation++;
        _threadsWaiting = _threadCount;
        _condition.notify_all();
    } else {
        uint64_t currentGeneration = _generation;
        while (currentGeneration == _generation) {
            _condition.wait(_lock);
        }
    }
}

}  // namespace unittest
}  // namespace mongo
