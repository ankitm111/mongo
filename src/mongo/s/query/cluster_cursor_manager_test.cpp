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

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_cursor_manager.h"

#include <vector>

#include "mongo/s/query/cluster_client_cursor_mock.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const NamespaceString nss("test.collection");

class ClusterCursorManagerTest : public unittest::Test {
protected:
    /**
     * Returns an unowned pointer to the manager owned by this test fixture.
     */
    ClusterCursorManager* getManager() {
        return &_manager;
    }

    /**
     * Allocates a mock cursor, which can be used with the 'isMockCursorKilled' method below.
     */
    std::unique_ptr<ClusterClientCursorMock> allocateMockCursor() {
        // Allocate a new boolean to our list to track when this cursor is killed.
        _cursorKilledFlags.push_back(false);

        // Allocate and return a cursor with a kill callback that sets the cursor's killed flag in
        // our list.  Note that it is safe to capture the kill flag in our list by reference
        // (std::list<>::push_back() does not invalidate references, and our list outlives the
        // manager).
        bool& killedFlag = _cursorKilledFlags.back();
        return stdx::make_unique<ClusterClientCursorMock>([&killedFlag]() { killedFlag = true; });
    }

    /**
     * Returns whether or not the i-th allocated cursor been killed.  'i' should be zero-indexed,
     * i.e. the initial allocated cursor can be checked for a kill with 'isMockCursorKilled(0)'.
     */
    bool isMockCursorKilled(size_t i) const {
        invariant(i < _cursorKilledFlags.size());
        return *std::next(_cursorKilledFlags.begin(), i);
    }

private:
    void tearDown() final {
        _manager.killAllCursors();
        _manager.reapZombieCursors();
    }

    // List of flags representing whether our allocated cursors have been killed yet.  The value of
    // the flag is true iff the cursor has been killed.
    //
    // We use std::list<> for this member (instead of std::vector<>, for example) so that we can
    // keep references that don't get invalidated as the list grows.
    std::list<bool> _cursorKilledFlags;

    ClusterCursorManager _manager;
};

// Test that registering a cursor returns a pin to the same cursor.
TEST_F(ClusterCursorManagerTest, RegisterCursor) {
    auto cursor = allocateMockCursor();
    cursor->queueResult(BSON("a" << 1));
    auto pinnedCursor =
        getManager()->registerCursor(std::move(cursor),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    auto nextResult = pinnedCursor.next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT(nextResult.getValue());
    ASSERT_EQ(BSON("a" << 1), *nextResult.getValue());
    nextResult = pinnedCursor.next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT_FALSE(nextResult.getValue());
}

// Test that checking out a cursor returns a pin to the correct cursor.
TEST_F(ClusterCursorManagerTest, CheckOutCursorBasic) {
    auto cursor = allocateMockCursor();
    cursor->queueResult(BSON("a" << 1));
    auto registeredCursor =
        getManager()->registerCursor(std::move(cursor),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    auto checkedOutCursor = getManager()->checkOutCursor(nss, cursorId);
    ASSERT_OK(checkedOutCursor.getStatus());
    auto nextResult = checkedOutCursor.getValue().next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT(nextResult.getValue());
    ASSERT_EQ(BSON("a" << 1), *nextResult.getValue());
    nextResult = checkedOutCursor.getValue().next();
    ASSERT_OK(nextResult.getStatus());
    ASSERT_FALSE(nextResult.getValue());
}

// Test that checking out a cursor returns a pin to the correct cursor, when multiple cursors are
// registered.
TEST_F(ClusterCursorManagerTest, CheckOutCursorMultipleCursors) {
    const int numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    for (int i = 0; i < numCursors; ++i) {
        auto cursor = allocateMockCursor();
        cursor->queueResult(BSON("a" << i));
        auto pinnedCursor =
            getManager()->registerCursor(std::move(cursor),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        cursorIds[i] = pinnedCursor.getCursorId();
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    for (int i = 0; i < numCursors; ++i) {
        auto pinnedCursor = getManager()->checkOutCursor(nss, cursorIds[i]);
        ASSERT_OK(pinnedCursor.getStatus());
        auto nextResult = pinnedCursor.getValue().next();
        ASSERT_OK(nextResult.getStatus());
        ASSERT(nextResult.getValue());
        ASSERT_EQ(BSON("a" << i), *nextResult.getValue());
        nextResult = pinnedCursor.getValue().next();
        ASSERT_OK(nextResult.getStatus());
        ASSERT_FALSE(nextResult.getValue());
    }
}

// Test that checking out a pinned cursor returns an error with code ErrorCodes::CursorInUse.
TEST_F(ClusterCursorManagerTest, CheckOutCursorPinned) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    ASSERT_EQ(ErrorCodes::CursorInUse, getManager()->checkOutCursor(nss, cursorId).getStatus());
}

// Test that checking out a killed cursor returns an error with code ErrorCodes::CursorNotFound.
TEST_F(ClusterCursorManagerTest, CheckOutCursorKilled) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_OK(getManager()->killCursor(nss, cursorId));
    ASSERT_EQ(ErrorCodes::CursorNotFound, getManager()->checkOutCursor(nss, cursorId).getStatus());
}

// Test that checking out an unknown cursor returns an error with code ErrorCodes::CursorNotFound.
TEST_F(ClusterCursorManagerTest, CheckOutCursorUnknown) {
    ASSERT_EQ(ErrorCodes::CursorNotFound, getManager()->checkOutCursor(nss, 5).getStatus());
}

// Test that checking out a unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same cursor id but a different namespace.
TEST_F(ClusterCursorManagerTest, CheckOutCursorWrongNamespace) {
    const NamespaceString correctNamespace("test.correct");
    const NamespaceString incorrectNamespace("test.incorrect");
    auto registeredCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     correctNamespace,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              getManager()->checkOutCursor(incorrectNamespace, cursorId).getStatus());
}

// Test that checking out a unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same namespace but a different cursor id.
TEST_F(ClusterCursorManagerTest, CheckOutCursorWrongCursorId) {
    auto registeredCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(ErrorCodes::CursorNotFound,
              getManager()->checkOutCursor(nss, cursorId + 1).getStatus());
}

// Test that killing a cursor by id successfully kills the cursor.
TEST_F(ClusterCursorManagerTest, KillCursorBasic) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    ASSERT_OK(getManager()->killCursor(nss, pinnedCursor.getCursorId()));
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(!isMockCursorKilled(0));
    getManager()->reapZombieCursors();
    ASSERT(isMockCursorKilled(0));
}

// Test that killing a cursor by id successfully kills the correct cursor, when multiple cursors are
// registered.
TEST_F(ClusterCursorManagerTest, KillCursorMultipleCursors) {
    const size_t numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    // Register cursors and populate 'cursorIds' with the returned cursor ids.
    for (size_t i = 0; i < numCursors; ++i) {
        auto pinnedCursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        cursorIds[i] = pinnedCursor.getCursorId();
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    // Kill each cursor and verify that it was successfully killed.
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT_OK(getManager()->killCursor(nss, cursorIds[i]));
        ASSERT(!isMockCursorKilled(i));
        getManager()->reapZombieCursors();
        ASSERT(isMockCursorKilled(i));
    }
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound.
TEST_F(ClusterCursorManagerTest, KillCursorUnknown) {
    Status killResult = getManager()->killCursor(nss, 5);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same cursor id but a different namespace.
TEST_F(ClusterCursorManagerTest, KillCursorWrongNamespace) {
    const NamespaceString correctNamespace("test.correct");
    const NamespaceString incorrectNamespace("test.incorrect");
    auto registeredCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     correctNamespace,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    Status killResult = getManager()->killCursor(incorrectNamespace, cursorId);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);
}

// Test that killing an unknown cursor returns an error with code ErrorCodes::CursorNotFound,
// even if there is an existing cursor with the same namespace but a different cursor id.
TEST_F(ClusterCursorManagerTest, KillCursorWrongCursorId) {
    auto registeredCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    Status killResult = getManager()->killCursor(nss, cursorId + 1);
    ASSERT_EQ(ErrorCodes::CursorNotFound, killResult);
}

// Test that killing all cursors successfully kills all cursors.
TEST_F(ClusterCursorManagerTest, KillAllCursors) {
    const size_t numCursors = 10;
    for (size_t i = 0; i < numCursors; ++i) {
        auto pinnedCursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    getManager()->killAllCursors();
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT(!isMockCursorKilled(i));
    }
    getManager()->reapZombieCursors();
    for (size_t i = 0; i < numCursors; ++i) {
        ASSERT(isMockCursorKilled(i));
    }
}

// Test that reaping correctly calls kill() on the underlying ClusterClientCursor for a killed
// cursor.
TEST_F(ClusterCursorManagerTest, ReapZombieCursorsBasic) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_OK(getManager()->killCursor(nss, cursorId));
    ASSERT(!isMockCursorKilled(0));
    getManager()->reapZombieCursors();
    ASSERT(isMockCursorKilled(0));
}

// Test that reaping does not call kill() on the underlying ClusterClientCursor for a killed cursor
// that is still pinned.
TEST_F(ClusterCursorManagerTest, ReapZombieCursorsSkipPinned) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    ASSERT(!isMockCursorKilled(0));
    getManager()->reapZombieCursors();
    ASSERT(!isMockCursorKilled(0));
}

// Test that reaping does not call kill() on the underlying ClusterClientCursor for cursors that
// haven't been killed.
TEST_F(ClusterCursorManagerTest, ReapZombieCursorsSkipNonZombies) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT(!isMockCursorKilled(0));
    getManager()->reapZombieCursors();
    ASSERT(!isMockCursorKilled(0));
}

// Test that a new ClusterCursorManager's stats() is initially zero for the cursor counts.
TEST_F(ClusterCursorManagerTest, StatsInitAsZero) {
    ASSERT_EQ(0U, getManager()->stats().cursorsSharded);
    ASSERT_EQ(0U, getManager()->stats().cursorsNotSharded);
}

// Test that registering a sharded cursor updates the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsRegisterShardedCursor) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(1U, getManager()->stats().cursorsSharded);
}

// Test that registering a not-sharded cursor updates the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsRegisterNotShardedCursor) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(1U, getManager()->stats().cursorsNotSharded);
}

// Test that registering multiple sharded and not-sharded cursors updates the corresponding
// counters in stats().
TEST_F(ClusterCursorManagerTest, StatsRegisterMultipleCursors) {
    const size_t numShardedCursors = 10;
    for (size_t i = 0; i < numShardedCursors; ++i) {
        auto pinnedCursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
        ASSERT_EQ(i + 1, getManager()->stats().cursorsSharded);
        ASSERT_EQ(0U, getManager()->stats().cursorsNotSharded);
    }
    const size_t numNotShardedCursors = 10;
    for (size_t i = 0; i < numNotShardedCursors; ++i) {
        auto pinnedCursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
        ASSERT_EQ(numShardedCursors, getManager()->stats().cursorsSharded);
        ASSERT_EQ(i + 1, getManager()->stats().cursorsNotSharded);
    }
}

// Test that killing a sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsKillShardedCursor) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);

    ASSERT_EQ(1U, getManager()->stats().cursorsSharded);
    ASSERT_OK(getManager()->killCursor(nss, cursorId));
    ASSERT_EQ(0U, getManager()->stats().cursorsSharded);
}

// Test that killing a not-sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsKillNotShardedCursor) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);

    ASSERT_EQ(1U, getManager()->stats().cursorsNotSharded);
    ASSERT_OK(getManager()->killCursor(nss, cursorId));
    ASSERT_EQ(0U, getManager()->stats().cursorsNotSharded);
}

// Test that exhausting a sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsExhaustShardedCursor) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    ASSERT_OK(pinnedCursor.next().getStatus());
    ASSERT_EQ(1U, getManager()->stats().cursorsSharded);
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0U, getManager()->stats().cursorsSharded);
}

// Test that exhausting a not-sharded cursor decrements the corresponding counter in stats().
TEST_F(ClusterCursorManagerTest, StatsExhaustNotShardedCursor) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    ASSERT_OK(pinnedCursor.next().getStatus());
    ASSERT_EQ(1U, getManager()->stats().cursorsNotSharded);
    pinnedCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0U, getManager()->stats().cursorsNotSharded);
}

// Test that getting the namespace for a cursor returns the correct namespace.
TEST_F(ClusterCursorManagerTest, GetNamespaceForCursorIdBasic) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = pinnedCursor.getCursorId();
    boost::optional<NamespaceString> cursorNamespace =
        getManager()->getNamespaceForCursorId(cursorId);
    ASSERT(cursorNamespace);
    ASSERT_EQ(nss.ns(), cursorNamespace->ns());
}

// Test that getting the namespace for a cursor returns the correct namespace, when there are
// multiple cursors registered on that namespace.
TEST_F(ClusterCursorManagerTest, GetNamespaceForCursorIdMultipleCursorsSameNamespace) {
    const size_t numCursors = 10;
    std::vector<CursorId> cursorIds(numCursors);
    for (size_t i = 0; i < numCursors; ++i) {
        auto cursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        cursorIds[i] = cursor.getCursorId();
        cursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    for (size_t i = 0; i < numCursors; ++i) {
        boost::optional<NamespaceString> cursorNamespace =
            getManager()->getNamespaceForCursorId(cursorIds[i]);
        ASSERT(cursorNamespace);
        ASSERT_EQ(nss.ns(), cursorNamespace->ns());
    }
}

// Test that getting the namespace for a cursor returns the correct namespace, when there are
// multiple cursors registered on different namespaces.
TEST_F(ClusterCursorManagerTest, GetNamespaceForCursorIdMultipleCursorsDifferentNamespaces) {
    const size_t numCursors = 10;
    std::vector<std::pair<NamespaceString, CursorId>> cursors(numCursors);
    for (size_t i = 0; i < numCursors; ++i) {
        NamespaceString cursorNamespace(std::string(str::stream() << "test.collection" << i));
        auto cursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         cursorNamespace,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
        cursors[i] = {cursorNamespace, cursor.getCursorId()};
        cursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    }
    for (size_t i = 0; i < numCursors; ++i) {
        boost::optional<NamespaceString> cursorNamespace =
            getManager()->getNamespaceForCursorId(cursors[i].second);
        ASSERT(cursorNamespace);
        ASSERT_EQ(cursors[i].first.ns(), cursorNamespace->ns());
    }
}

// Test that getting the namespace for an unknown cursor returns boost::none.
TEST_F(ClusterCursorManagerTest, GetNamespaceForCursorIdUnknown) {
    boost::optional<NamespaceString> cursorNamespace = getManager()->getNamespaceForCursorId(5);
    ASSERT_FALSE(cursorNamespace);
}

// Test that the PinnedCursor default constructor creates a pin that owns no cursor.
TEST_F(ClusterCursorManagerTest, PinnedCursorDefaultConstructor) {
    ClusterCursorManager::PinnedCursor pinnedCursor;
    ASSERT_EQ(0, pinnedCursor.getCursorId());
}

// Test that returning a pinned cursor correctly unpins the cursor, and leaves the pin owning no
// cursor.
TEST_F(ClusterCursorManagerTest, PinnedCursorReturnCursorNotExhausted) {
    auto registeredCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    ASSERT_NE(0, cursorId);
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::NotExhausted);
    ASSERT_EQ(0, registeredCursor.getCursorId());
    auto checkedOutCursor = getManager()->checkOutCursor(nss, cursorId);
    ASSERT_OK(checkedOutCursor.getStatus());
}

// Test that returning a pinned cursor with 'Exhausted' correctly de-registers the cursor, and
// leaves the pin owning no cursor.
TEST_F(ClusterCursorManagerTest, PinnedCursorReturnCursorExhausted) {
    auto registeredCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    CursorId cursorId = registeredCursor.getCursorId();
    ASSERT_NE(0, cursorId);
    ASSERT_OK(registeredCursor.next().getStatus());
    registeredCursor.returnCursor(ClusterCursorManager::CursorState::Exhausted);
    ASSERT_EQ(0, registeredCursor.getCursorId());
    ASSERT_NOT_OK(getManager()->checkOutCursor(nss, cursorId).getStatus());
}

// Test that the PinnedCursor move assignment operator correctly kills the cursor if it has not yet
// been returned.
TEST_F(ClusterCursorManagerTest, PinnedCursorMoveAssignmentKill) {
    auto pinnedCursor =
        getManager()->registerCursor(allocateMockCursor(),
                                     nss,
                                     ClusterCursorManager::CursorType::NamespaceNotSharded,
                                     ClusterCursorManager::CursorLifetime::Mortal);
    pinnedCursor = ClusterCursorManager::PinnedCursor();
    ASSERT(!isMockCursorKilled(0));
    getManager()->reapZombieCursors();
    ASSERT(isMockCursorKilled(0));
}

// Test that the PinnedCursor destructor correctly kills the cursor if it has not yet been returned.
TEST_F(ClusterCursorManagerTest, PinnedCursorDestructorKill) {
    {
        auto pinnedCursor =
            getManager()->registerCursor(allocateMockCursor(),
                                         nss,
                                         ClusterCursorManager::CursorType::NamespaceNotSharded,
                                         ClusterCursorManager::CursorLifetime::Mortal);
    }
    ASSERT(!isMockCursorKilled(0));
    getManager()->reapZombieCursors();
    ASSERT(isMockCursorKilled(0));
}

}  // namespace

}  // namespace mongo
