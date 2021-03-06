/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <limits>
#include <string>

#include "mongo/base/status.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

namespace mongo {

class RecoveryUnit;

class SnapshotName {
public:
    explicit SnapshotName(uint64_t value) : _value(value) {}

    /**
     * Returns a SnapshotName guaranteed to compare > all others.
     */
    static SnapshotName max() {
        return SnapshotName(std::numeric_limits<uint64_t>::max());
    }

    /**
     * Returns an unsigned number that compares with the same ordering as the SnapshotName.
     */
    uint64_t asU64() const {
        return _value;
    }

    bool operator==(const SnapshotName& rhs) const {
        return _value == rhs._value;
    }
    bool operator!=(const SnapshotName& rhs) const {
        return _value != rhs._value;
    }
    bool operator<(const SnapshotName& rhs) const {
        return _value < rhs._value;
    }
    bool operator<=(const SnapshotName& rhs) const {
        return _value <= rhs._value;
    }
    bool operator>(const SnapshotName& rhs) const {
        return _value > rhs._value;
    }
    bool operator>=(const SnapshotName& rhs) const {
        return _value >= rhs._value;
    }

private:
    uint64_t _value;
};

/**
 * Manages snapshots that can be read from at a later time.
 *
 * Implementations must be able to handle concurrent access to any methods. No methods are allowed
 * to acquire locks from the LockManager.
 */
class SnapshotManager {
public:
    /**
     * Prepares the passed-in OperationContext for snapshot creation.
     *
     * The passed-in OperationContext will be associated with a point-in-time that can be used
     * for creating a snapshot later.
     *
     * This must be the first method called after starting a ScopedTransaction, and it is
     * illegal to start a WriteUnitOfWork inside of the same ScopedTransaction.
     */
    virtual Status prepareForCreateSnapshot(OperationContext* txn) = 0;

    /**
     * Creates a new named snapshot representing the same point-in-time captured in
     * prepareForCreateSnapshot().
     *
     * Must be called in the same ScopedTransaction as prepareForCreateSnapshot.
     *
     * Caller guarantees that this name must compare greater than all existing snapshots.
     */
    virtual Status createSnapshot(OperationContext* txn, const SnapshotName& name) = 0;

    /**
     * Sets the snapshot to be used for committed reads. Once set, all older snapshots that are
     * not currently in use by any RecoveryUnit can be deleted.
     *
     * Implementations are allowed to assume that all older snapshots have names that compare
     * less than the passed in name, and newer ones compare greater.
     */
    virtual void setCommittedSnapshot(const SnapshotName& name) = 0;

    /**
     * Drops all snapshots and clears the "committed" snapshot.
     */
    virtual void dropAllSnapshots() = 0;

protected:
    /**
     * SnapshotManagers are not intended to be deleted through pointers to base type.
     * (virtual is just to suppress compiler warnings)
     */
    virtual ~SnapshotManager() = default;
};

}  // namespace mongo
