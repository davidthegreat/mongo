/**
 *    Copyright 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include <initializer_list>
#include <utility>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/rollback_test_fixture.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace {

using namespace mongo;
using namespace mongo::repl;
using namespace mongo::repl::rollback_internal;

const auto kIndexVersion = IndexDescriptor::IndexVersion::kV2;

class RollbackSourceMock : public RollbackSource {
public:
    RollbackSourceMock(std::unique_ptr<OplogInterface> oplog);
    int getRollbackId() const override;
    const OplogInterface& getOplog() const override;
    const HostAndPort& getSource() const override;
    BSONObj getLastOperation() const override;
    BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const override;

    std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                      UUID uuid,
                                                      const BSONObj& filter) const override;

    void copyCollectionFromRemote(OperationContext* opCtx,
                                  const NamespaceString& nss) const override;
    StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db,
                                                const UUID& uuid) const override;
    StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;

private:
    std::unique_ptr<OplogInterface> _oplog;
    HostAndPort _source;
};

RollbackSourceMock::RollbackSourceMock(std::unique_ptr<OplogInterface> oplog)
    : _oplog(std::move(oplog)) {}

const OplogInterface& RollbackSourceMock::getOplog() const {
    return *_oplog;
}

const HostAndPort& RollbackSourceMock::getSource() const {
    return _source;
}

int RollbackSourceMock::getRollbackId() const {
    return 0;
}

BSONObj RollbackSourceMock::getLastOperation() const {
    auto iter = _oplog->makeIterator();
    auto result = iter->next();
    ASSERT_OK(result.getStatus());
    return result.getValue().first;
}

BSONObj RollbackSourceMock::findOne(const NamespaceString& nss, const BSONObj& filter) const {
    return BSONObj();
}

std::pair<BSONObj, NamespaceString> RollbackSourceMock::findOneByUUID(const std::string& db,
                                                                      UUID uuid,
                                                                      const BSONObj& filter) const {
    return {BSONObj(), NamespaceString()};
}

void RollbackSourceMock::copyCollectionFromRemote(OperationContext* opCtx,
                                                  const NamespaceString& nss) const {}

StatusWith<BSONObj> RollbackSourceMock::getCollectionInfo(const NamespaceString& nss) const {
    return BSON("name" << nss.ns() << "options" << BSONObj());
}

StatusWith<BSONObj> RollbackSourceMock::getCollectionInfoByUUID(const std::string& db,
                                                                const UUID& uuid) const {
    return BSON("options" << BSONObj() << "info" << BSON("uuid" << uuid));
}


class RSRollbackTest : public RollbackTest {
private:
    void setUp() override;
    void tearDown() override;
};

void RSRollbackTest::setUp() {
    RollbackTest::setUp();
    enableCollectionUUIDs = true;
}

void RSRollbackTest::tearDown() {
    RollbackTest::tearDown();
}

OplogInterfaceMock::Operation makeNoopOplogEntryAndRecordId(Seconds seconds) {
    OpTime ts(Timestamp(seconds, 0), 0);
    return std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId(1));
}

OplogInterfaceMock::Operation makeDropIndexOplogEntry(Collection* collection,
                                                      BSONObj key,
                                                      std::string indexName,
                                                      int time) {
    auto indexSpec =
        BSON("ns" << collection->ns().ns() << "key" << key << "name" << indexName << "v"
                  << static_cast<int>(kIndexVersion));

    return std::make_pair(
        BSON("ts" << Timestamp(Seconds(time), 0) << "h" << 1LL << "op"
                  << "c"
                  << "ui"
                  << collection->uuid().get()
                  << "ns"
                  << "test.$cmd"
                  << "o"
                  << BSON("dropIndexes" << collection->ns().coll() << "index" << indexName)
                  << "o2"
                  << indexSpec),
        RecordId(time));
}

OplogInterfaceMock::Operation makeCreateIndexOplogEntry(Collection* collection,
                                                        BSONObj key,
                                                        std::string indexName,
                                                        int time) {
    auto indexSpec = BSON("createIndexes"
                          << "t"
                          << "ns"
                          << collection->ns().ns()
                          << "v"
                          << static_cast<int>(kIndexVersion)
                          << "key"
                          << key
                          << "name"
                          << indexName);

    return std::make_pair(BSON("ts" << Timestamp(Seconds(time), 0) << "h" << 1LL << "op"
                                    << "c"
                                    << "ns"
                                    << "test.$cmd"
                                    << "ui"
                                    << collection->uuid().get()
                                    << "o"
                                    << indexSpec),
                          RecordId(time));
}

OplogInterfaceMock::Operation makeRenameCollectionOplogEntry(const NamespaceString& renameFrom,
                                                             const NamespaceString& renameTo,
                                                             const UUID collectionUUID,
                                                             OptionalCollectionUUID dropTarget,
                                                             OptionalCollectionUUID dropSource,
                                                             const bool stayTemp,
                                                             OpTime opTime) {
    BSONObjBuilder cmd;
    cmd.append("renameCollection", renameFrom.ns());
    cmd.append("to", renameTo.ns());
    cmd.append("stayTemp", stayTemp);

    BSONObj obj = cmd.obj();

    if (dropTarget) {
        obj = obj.addField(BSON("dropTarget" << *dropTarget).firstElement());
    } else {
        obj = obj.addField(BSON("dropTarget" << false).firstElement());
    }
    if (dropSource) {
        obj = obj.addField(BSON("dropSource" << *dropSource).firstElement());
    }

    return std::make_pair(
        BSON("ts" << opTime.getTimestamp() << "t" << opTime.getTerm() << "h" << 1LL << "op"
                  << "c"
                  << "ui"
                  << collectionUUID
                  << "ns"
                  << renameFrom.ns()
                  << "o"
                  << obj),
        RecordId(opTime.getTimestamp().getSecs()));
}

TEST_F(RSRollbackTest, InconsistentMinValid) {
    _replicationProcess->getConsistencyMarkers()->setAppliedThrough(
        _opCtx.get(), OpTime(Timestamp(Seconds(0), 0), 0));
    _replicationProcess->getConsistencyMarkers()->setMinValid(_opCtx.get(),
                                                              OpTime(Timestamp(Seconds(1), 0), 0));
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock(),
                               RollbackSourceMock(stdx::make_unique<OplogInterfaceMock>()),
                               {},
                               _coordinator,
                               _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
}

TEST_F(RSRollbackTest, OplogStartMissing) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
    OplogInterfaceMock::Operations remoteOperations({operation});
    auto remoteOplog = stdx::make_unique<OplogInterfaceMock>(remoteOperations);
    ASSERT_EQUALS(ErrorCodes::OplogStartMissing,
                  syncRollback(_opCtx.get(),
                               OplogInterfaceMock(),
                               RollbackSourceMock(std::move(remoteOplog)),
                               {},
                               _coordinator,
                               _replicationProcess.get())
                      .code());
}

TEST_F(RSRollbackTest, NoRemoteOpLog) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({operation}),
                               RollbackSourceMock(stdx::make_unique<OplogInterfaceMock>()),
                               {},
                               _coordinator,
                               _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
}

TEST_F(RSRollbackTest, RemoteGetRollbackIdThrows) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        int getRollbackId() const override {
            uassert(ErrorCodes::UnknownError, "getRollbackId() failed", false);
        }
    };
    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({operation}),
                                    RollbackSourceLocal(stdx::make_unique<OplogInterfaceMock>()),
                                    {},
                                    _coordinator,
                                    _replicationProcess.get())
                           .transitional_ignore(),
                       AssertionException,
                       ErrorCodes::UnknownError);
}

TEST_F(RSRollbackTest, RemoteGetRollbackIdDiffersFromRequiredRBID) {
    OpTime ts(Timestamp(Seconds(1), 0), 0);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        using RollbackSourceMock::RollbackSourceMock;
        int getRollbackId() const override {
            return 2;
        }
    };

    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({operation}),
                                    RollbackSourceLocal(stdx::make_unique<OplogInterfaceMock>()),
                                    1,
                                    _coordinator,
                                    _replicationProcess.get())
                           .transitional_ignore(),
                       AssertionException,
                       ErrorCodes::Error(40506));
}

TEST_F(RSRollbackTest, BothOplogsAtCommonPoint) {
    createOplog(_opCtx.get());
    OpTime ts(Timestamp(Seconds(1), 0), 1);
    auto operation =
        std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId(1));
    ASSERT_OK(
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({operation}),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         operation,
                     }))),
                     {},
                     _coordinator,
                     _replicationProcess.get()));
}

/**
 * Create test collection.
 * Returns collection.
 */
Collection* _createCollection(OperationContext* opCtx,
                              const NamespaceString& nss,
                              const CollectionOptions& options) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
    mongo::WriteUnitOfWork wuow(opCtx);
    auto db = dbHolder().openDb(opCtx, nss.db());
    ASSERT_TRUE(db);
    db->dropCollection(opCtx, nss.ns()).transitional_ignore();
    auto coll = db->createCollection(opCtx, nss.ns(), options);
    ASSERT_TRUE(coll);
    wuow.commit();
    return coll;
}

Collection* _createCollection(OperationContext* opCtx,
                              const std::string& nss,
                              const CollectionOptions& options) {
    return _createCollection(opCtx, NamespaceString(nss), options);
}

/**
 * Test function to roll back a delete operation.
 * Returns number of records in collection after rolling back delete operation.
 * If collection does not exist after rolling back, returns -1.
 */
int _testRollbackDelete(OperationContext* opCtx,
                        ReplicationCoordinator* coordinator,
                        ReplicationProcess* replicationProcess,
                        UUID uuid,
                        const BSONObj& documentAtSource) {
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto deleteOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "d"
                                 << "ui"
                                 << uuid
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("_id" << 0)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(const BSONObj& documentAtSource, std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)),
              called(false),
              _documentAtSource(documentAtSource) {}
        std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                          UUID uuid,
                                                          const BSONObj& filter) const override {
            called = true;
            return {_documentAtSource, NamespaceString()};
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(documentAtSource,
                                       std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                                           commonOperation,
                                       })));
    ASSERT_OK(syncRollback(opCtx,
                           OplogInterfaceMock({deleteOperation, commonOperation}),
                           rollbackSource,
                           {},
                           coordinator,
                           replicationProcess));
    ASSERT_TRUE(rollbackSource.called);

    Lock::DBLock dbLock(opCtx, "test", MODE_S);
    Lock::CollectionLock collLock(opCtx->lockState(), "test.t", MODE_S);
    auto db = dbHolder().get(opCtx, "test");
    ASSERT_TRUE(db);
    auto collection = db->getCollection(opCtx, "test.t");
    if (!collection) {
        return -1;
    }
    return collection->getRecordStore()->numRecords(opCtx);
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionDoesNotExist) {
    createOplog(_opCtx.get());
    ASSERT_EQUALS(
        -1,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), UUID::gen(), BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionExistsNonCapped) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    _testRollbackDelete(
        _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid().get(), BSONObj());
    ASSERT_EQUALS(
        0,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid().get(), BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteNoDocumentAtSourceCollectionExistsCapped) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    options.capped = true;
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    ASSERT_EQUALS(
        0,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid().get(), BSONObj()));
}

TEST_F(RSRollbackTest, RollbackDeleteRestoreDocument) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);
    BSONObj doc = BSON("_id" << 0 << "a" << 1);
    _testRollbackDelete(
        _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid().get(), doc);
    ASSERT_EQUALS(
        1,
        _testRollbackDelete(
            _opCtx.get(), _coordinator, _replicationProcess.get(), coll->uuid().get(), doc));
}

TEST_F(RSRollbackTest, RollbackInsertDocumentWithNoId) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ui"
                                 << UUID::gen()
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("a" << 1)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const {
            called = true;
            return BSONObj();
        }
        mutable bool called;

    private:
        BSONObj _documentAtSource;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({insertDocumentOperation, commonOperation}),
                               rollbackSource,
                               {},
                               _coordinator,
                               _replicationProcess.get());
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
    ASSERT_EQUALS(1, countLogLinesContaining("Cannot roll back op with no _id. ns: test.t,"));
    ASSERT_FALSE(rollbackSource.called);
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);
    auto indexSpec = BSON("ns"
                          << "test.t"
                          << "v"
                          << static_cast<int>(kIndexVersion)
                          << "key"
                          << BSON("a" << 1)
                          << "name"
                          << "a_1");
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_X);
        MultiIndexBlock indexer(_opCtx.get(), collection);
        ASSERT_OK(indexer.init(indexSpec).getStatus());
        WriteUnitOfWork wunit(_opCtx.get());
        indexer.commit();
        wunit.commit();
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    // Repeat index creation operation and confirm that rollback attempts to drop index just once.
    // This can happen when an index is re-created with different options.
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    startCapturingLogMessages();
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createIndexOperation, createIndexOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(
        1, countLogLinesContaining("Dropped index in rollback: collection = test.t, index = a_1"));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandIndexNotInCatalog) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);
    auto indexSpec = BSON("ns"
                          << "test.t"
                          << "key"
                          << BSON("a" << 1)
                          << "name"
                          << "a_1");
    // Skip index creation to trigger warning during rollback.
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({createIndexOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("Rollback failed to drop index a_1 in test.t"));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackDropIndexCommandWithOneIndex) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto dropIndexOperation = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({dropIndexOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollbackDropIndexCommandWithMultipleIndexes) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto dropIndexOperation1 = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);
    auto dropIndexOperation2 = makeDropIndexOplogEntry(collection, BSON("b" << 1), "b_1", 3);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({dropIndexOperation2, dropIndexOperation1, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(3, indexCatalog->numIndexesReady(_opCtx.get()));
    }
}

TEST_F(RSRollbackTest, RollingBackCreateAndDropOfSameIndexIgnoresBothCommands) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);

    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_X);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    auto dropIndexOperation = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 3);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({dropIndexOperation, createIndexOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(1, indexCatalog->numIndexesReady(_opCtx.get()));
        auto indexDescriptor = indexCatalog->findIndexByName(_opCtx.get(), "a_1", false);
        ASSERT(!indexDescriptor);
    }
}

TEST_F(RSRollbackTest, RollingBackDropAndCreateOfSameIndexNameWithDifferentSpecs) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);

    auto indexSpec = BSON("ns"
                          << "test.t"
                          << "v"
                          << static_cast<int>(kIndexVersion)
                          << "key"
                          << BSON("b" << 1)
                          << "name"
                          << "a_1");
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_X);
        MultiIndexBlock indexer(_opCtx.get(), collection);
        ASSERT_OK(indexer.init(indexSpec).getStatus());
        WriteUnitOfWork wunit(_opCtx.get());
        indexer.commit();
        wunit.commit();
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
    }

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto dropIndexOperation = makeDropIndexOplogEntry(collection, BSON("a" << 1), "a_1", 2);

    auto createIndexOperation = makeCreateIndexOplogEntry(collection, BSON("b" << 1), "a_1", 3);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    startCapturingLogMessages();
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createIndexOperation, dropIndexOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));
    stopCapturingLogMessages();
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto indexCatalog = collection->getIndexCatalog();
        ASSERT(indexCatalog);
        ASSERT_EQUALS(2, indexCatalog->numIndexesReady(_opCtx.get()));
        ASSERT_EQUALS(
            1,
            countLogLinesContaining("Dropped index in rollback: collection = test.t, index = a_1"));
        ASSERT_EQUALS(
            1,
            countLogLinesContaining("Created index in rollback: collection = test.t, index = a_1"));
        std::vector<IndexDescriptor*> indexes;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), BSON("a" << 1), false, &indexes);
        ASSERT(indexes.size() == 1);
        ASSERT(indexes[0]->indexName() == "a_1");

        std::vector<IndexDescriptor*> indexes2;
        indexCatalog->findIndexesByKeyPattern(_opCtx.get(), BSON("b" << 1), false, &indexes2);
        ASSERT(indexes2.size() == 0);
    }
}

TEST_F(RSRollbackTest, RollbackCreateIndexCommandMissingIndexName) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.t", options);
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    BSONObj command = BSON("createIndexes"
                           << "t"
                           << "ns"
                           << "test.t"
                           << "v"
                           << static_cast<int>(kIndexVersion)
                           << "key"
                           << BSON("a" << 1));

    auto createIndexOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ns"
                                 << "test.$cmd"
                                 << "ui"
                                 << collection->uuid().get()
                                 << "o"
                                 << command),
                       RecordId(2));
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    auto status = syncRollback(_opCtx.get(),
                               OplogInterfaceMock({createIndexOperation, commonOperation}),
                               rollbackSource,
                               {},
                               _coordinator,
                               _replicationProcess.get());
    stopCapturingLogMessages();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
    ASSERT_EQUALS(1,
                  countLogLinesContaining(
                      "Missing index name in createIndexes operation on rollback, document: "));
}

TEST_F(RSRollbackTest, RollbackUnknownCommand) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto unknownCommandOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << UUID::gen()
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("convertToCapped"
                                         << "t")),
                       RecordId(2));

    auto status =
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({unknownCommandOperation, commonOperation}),
                     RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                         commonOperation,
                     }))),
                     {},
                     _coordinator,
                     _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "unable to determine common point");
}

TEST_F(RSRollbackTest, RollbackDropCollectionCommand) {
    createOplog(_opCtx.get());

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpns = NamespaceString("test.t").makeDropPendingNamespace(dropTime);
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), dpns, options);
    _dropPendingCollectionReaper->addDropPendingNamespace(dropTime, dpns);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto dropCollectionOperation =
        std::make_pair(
            BSON("ts" << dropTime.getTimestamp() << "t" << dropTime.getTerm() << "h" << 1LL << "op"
                      << "c"
                      << "ui"
                      << coll->uuid().get()
                      << "ns"
                      << "test.t"
                      << "o"
                      << BSON("drop"
                              << "t")),
            RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        void copyCollectionFromRemote(OperationContext* opCtx,
                                      const NamespaceString& nss) const override {
            called = true;
        }
        mutable bool called;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_TRUE(autoCollDropPending.getCollection());
        AutoGetCollectionForReadCommand autoColl(_opCtx.get(), NamespaceString("test.t"));
        ASSERT_FALSE(autoColl.getCollection());
    }
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({dropCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    ASSERT_FALSE(rollbackSource.called);
    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_FALSE(autoCollDropPending.getCollection());
        AutoGetCollectionForReadCommand autoColl(_opCtx.get(), NamespaceString("test.t"));
        ASSERT_TRUE(autoColl.getCollection());
    }
}

TEST_F(RSRollbackTest, RollbackRenameCollectionInSameDatabaseCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "test.y", options);
    UUID collectionUUID = collection->uuid().get();

    OpTime renameTime = OpTime(Timestamp(2, 0), 5);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                    NamespaceString("test.y"),
                                                                    collectionUUID,
                                                                    boost::none,
                                                                    boost::none,
                                                                    false,
                                                                    renameTime);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(renamedColl.getCollection());

        AutoGetCollectionForReadCommand oldCollName(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_FALSE(oldCollName.getCollection());
    }

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_FALSE(renamedColl.getCollection());

        AutoGetCollectionForReadCommand oldCollName(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(oldCollName.getCollection());
    }
}

// TODO: Un-comment this test once options.temp is properly resynced during rollback.
// See SERVER-30413

// TEST_F(RSRollbackTest,
// RollingBackRenameCollectionFromTempToPermanentCollectionSetsCollectionOptionToTemp) {
//    createOplog(_opCtx.get());
//
//    CollectionOptions options;
//    options.uuid = UUID::gen();
//    auto collection = _createCollection(_opCtx.get(), "test.y", options);
//    auto collectionUUID = collection->uuid().get();
//
//    class RollbackSourceLocal : public RollbackSourceMock {
//    public:
//        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
//            : RollbackSourceMock(std::move(oplog)) {}
//        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db, const UUID& uuid) const
//        {
//            getCollectionInfoCalled = true;
//            return BSON("info" << BSON("uuid" << uuid) << "options" << BSON("temp" << true)));
//        }
//        mutable bool getCollectionInfoCalled = false;
//    };
//
//    auto commonOperation =
//        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
//
//    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
//                                                                    NamespaceString("test.y"),
//                                                                    collectionUUID,
//                                                                    boost::none,
//                                                                    boost::none,
//                                                                    false,
//                                                                    OpTime(Timestamp(2, 0), 5));
//
//    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
//        commonOperation,
//    })));
//
//    ASSERT_OK(syncRollback(_opCtx.get(),
//                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
//                           rollbackSource,
//                           {},
//                           _coordinator,
//                           _replicationProcess.get()));
//
//    ASSERT_TRUE(rollbackSource.getCollectionInfoCalled);
//    ASSERT_EQUALS(options.temp, true);
//}

TEST_F(RSRollbackTest, RollbackRenameCollectionInDatabaseWithDropTargetTrueCommand) {
    createOplog(_opCtx.get());

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpns = NamespaceString("test.y").makeDropPendingNamespace(dropTime);
    CollectionOptions droppedCollOptions;
    droppedCollOptions.uuid = UUID::gen();
    auto droppedColl = _createCollection(_opCtx.get(), dpns, droppedCollOptions);
    _dropPendingCollectionReaper->addDropPendingNamespace(dropTime, dpns);
    auto droppedCollectionUUID = droppedColl->uuid().get();

    CollectionOptions renamedCollOptions;
    renamedCollOptions.uuid = UUID::gen();
    auto renamedCollection = _createCollection(_opCtx.get(), "test.y", renamedCollOptions);
    auto renamedCollectionUUID = renamedCollection->uuid().get();

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                    NamespaceString("test.y"),
                                                                    renamedCollectionUUID,
                                                                    droppedCollectionUUID,
                                                                    boost::none,
                                                                    false,
                                                                    dropTime);

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_TRUE(autoCollDropPending.getCollection());

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(renamedColl.getCollection());

        AutoGetCollectionForReadCommand oldCollName(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_FALSE(oldCollName.getCollection());
    }
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_FALSE(autoCollDropPending.getCollection());

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(renamedColl.getCollection());
        ASSERT_EQUALS(renamedColl.getCollection()->uuid().get(), renamedCollectionUUID);

        AutoGetCollectionForReadCommand droppedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(droppedColl.getCollection());
        ASSERT_EQUALS(droppedColl.getCollection()->uuid().get(), droppedCollectionUUID);
    }
}

TEST_F(RSRollbackTest, RollbackRenamingCollectionsToEachOther) {
    createOplog(_opCtx.get());

    CollectionOptions coll1Options;
    coll1Options.uuid = UUID::gen();
    auto collection1 = _createCollection(_opCtx.get(), "test.y", coll1Options);
    auto collection1UUID = collection1->uuid().get();

    CollectionOptions coll2Options;
    coll2Options.uuid = UUID::gen();
    auto collection2 = _createCollection(_opCtx.get(), "test.x", coll2Options);
    auto collection2UUID = collection2->uuid().get();

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto renameCollectionOperationXtoZ = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                        NamespaceString("test.z"),
                                                                        collection1UUID,
                                                                        boost::none,
                                                                        boost::none,
                                                                        false,
                                                                        OpTime(Timestamp(2, 0), 5));

    auto renameCollectionOperationYtoX = makeRenameCollectionOplogEntry(NamespaceString("test.y"),
                                                                        NamespaceString("test.x"),
                                                                        collection2UUID,
                                                                        boost::none,
                                                                        boost::none,
                                                                        false,
                                                                        OpTime(Timestamp(3, 0), 5));

    auto renameCollectionOperationZtoY = makeRenameCollectionOplogEntry(NamespaceString("test.z"),
                                                                        NamespaceString("test.y"),
                                                                        collection1UUID,
                                                                        boost::none,
                                                                        boost::none,
                                                                        false,
                                                                        OpTime(Timestamp(4, 0), 5));

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperationZtoY,
                                               renameCollectionOperationYtoX,
                                               renameCollectionOperationXtoZ,
                                               commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    {

        AutoGetCollectionForReadCommand coll1(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(coll1.getCollection());
        ASSERT_EQUALS(coll1.getCollection()->uuid().get(), collection1UUID);

        AutoGetCollectionForReadCommand coll2(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(coll2.getCollection());
        ASSERT_EQUALS(coll2.getCollection()->uuid().get(), collection2UUID);
    }
}

TEST_F(RSRollbackTest, RollbackDropCollectionThenRenameCollectionToDroppedCollectionNS) {
    createOplog(_opCtx.get());

    CollectionOptions renamedCollOptions;
    renamedCollOptions.uuid = UUID::gen();
    auto renamedCollection = _createCollection(_opCtx.get(), "test.x", renamedCollOptions);
    auto renamedCollectionUUID = renamedCollection->uuid().get();

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpns = NamespaceString("test.x").makeDropPendingNamespace(dropTime);
    CollectionOptions droppedCollOptions;
    droppedCollOptions.uuid = UUID::gen();
    auto droppedCollection = _createCollection(_opCtx.get(), dpns, droppedCollOptions);
    auto droppedCollectionUUID = droppedCollection->uuid().get();
    _dropPendingCollectionReaper->addDropPendingNamespace(dropTime, dpns);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto dropCollectionOperation =
        std::make_pair(
            BSON("ts" << dropTime.getTimestamp() << "t" << dropTime.getTerm() << "h" << 1LL << "op"
                      << "c"
                      << "ui"
                      << droppedCollectionUUID
                      << "ns"
                      << "test.x"
                      << "o"
                      << BSON("drop"
                              << "x")),
            RecordId(2));

    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.y"),
                                                                    NamespaceString("test.x"),
                                                                    renamedCollectionUUID,
                                                                    boost::none,
                                                                    boost::none,
                                                                    false,
                                                                    OpTime(Timestamp(3, 0), 5));

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_TRUE(autoCollDropPending.getCollection());
        AutoGetCollectionForReadCommand autoCollX(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(autoCollX.getCollection());
        AutoGetCollectionForReadCommand autoCollY(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_FALSE(autoCollY.getCollection());
    }
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({renameCollectionOperation, dropCollectionOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_FALSE(autoCollDropPending.getCollection());

        AutoGetCollectionForReadCommand autoCollX(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(autoCollX.getCollection());
        ASSERT_EQUALS(autoCollX.getCollection()->uuid().get(), droppedCollectionUUID);

        AutoGetCollectionForReadCommand autoCollY(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(autoCollY.getCollection());
        ASSERT_EQUALS(autoCollY.getCollection()->uuid().get(), renamedCollectionUUID);
    }
}

TEST_F(RSRollbackTest, RollbackRenameCollectionThenCreateNewCollectionWithOldName) {
    createOplog(_opCtx.get());

    CollectionOptions renamedCollOptions;
    renamedCollOptions.uuid = UUID::gen();
    auto renamedCollection = _createCollection(_opCtx.get(), "test.y", renamedCollOptions);
    auto renamedCollectionUUID = renamedCollection->uuid().get();

    CollectionOptions createdCollOptions;
    createdCollOptions.uuid = UUID::gen();
    auto createdCollection = _createCollection(_opCtx.get(), "test.x", createdCollOptions);
    auto createdCollectionUUID = createdCollection->uuid().get();

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.x"),
                                                                    NamespaceString("test.y"),
                                                                    renamedCollectionUUID,
                                                                    boost::none,
                                                                    boost::none,
                                                                    false,
                                                                    OpTime(Timestamp(2, 0), 5));

    auto createCollectionOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(3), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << createdCollectionUUID
                                 << "ns"
                                 << "test.x"
                                 << "o"
                                 << BSON("create"
                                         << "x")),
                       RecordId(3));


    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_TRUE(renamedColl.getCollection());
        AutoGetCollectionForReadCommand createdColl(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(createdColl.getCollection());
    }
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({createCollectionOperation, renameCollectionOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));

    {

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.x"));
        ASSERT_TRUE(renamedColl.getCollection());
        ASSERT_EQUALS(renamedColl.getCollection()->uuid().get(), renamedCollectionUUID);

        AutoGetCollectionForReadCommand createdColl(_opCtx.get(), NamespaceString("test.y"));
        ASSERT_FALSE(createdColl.getCollection());
    }
}

TEST_F(RSRollbackTest, RollbackRenameCollectionAcrossDatabases) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto collection = _createCollection(_opCtx.get(), "foo.t", options);
    auto renamedCollectionUUID = collection->uuid().get();

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpSourceNs = NamespaceString("test.t").makeDropPendingNamespace(dropTime);
    CollectionOptions dpSourceOptions;
    dpSourceOptions.uuid = UUID::gen();
    auto droppedSource = _createCollection(_opCtx.get(), dpSourceNs, dpSourceOptions);
    _dropPendingCollectionReaper->addDropPendingNamespace(dropTime, dpSourceNs);
    auto droppedSourceUUID = droppedSource->uuid().get();

    auto dpTargetNs = NamespaceString("foo.t").makeDropPendingNamespace(dropTime);
    CollectionOptions dpTargetOptions;
    dpTargetOptions.uuid = UUID::gen();
    auto droppedTarget = _createCollection(_opCtx.get(), dpTargetNs, dpTargetOptions);
    _dropPendingCollectionReaper->addDropPendingNamespace(dropTime, dpTargetNs);
    auto droppedTargetUUID = droppedTarget->uuid().get();

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.t"),
                                                                    NamespaceString("foo.t"),
                                                                    renamedCollectionUUID,
                                                                    droppedTargetUUID,
                                                                    droppedSourceUUID,
                                                                    false,
                                                                    dropTime);
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoDropSource(_opCtx.get(), dpSourceNs);
        ASSERT_TRUE(autoDropSource.getCollection());
        AutoGetCollectionForReadCommand autoDropTarget(_opCtx.get(), dpTargetNs);
        ASSERT_TRUE(autoDropTarget.getCollection());
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("foo.t"));
        ASSERT_TRUE(renamedColl.getCollection());
        AutoGetCollectionForReadCommand droppedColl(_opCtx.get(), NamespaceString("test.t"));
        ASSERT_FALSE(droppedColl.getCollection());
    }
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({renameCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    {
        AutoGetCollectionForReadCommand autoDropSource(_opCtx.get(), dpSourceNs);
        ASSERT_FALSE(autoDropSource.getCollection());

        AutoGetCollectionForReadCommand autoDropTarget(_opCtx.get(), dpTargetNs);
        ASSERT_FALSE(autoDropTarget.getCollection());

        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.t"));
        ASSERT_TRUE(renamedColl.getCollection());
        ASSERT_EQUALS(renamedColl.getCollection()->uuid().get(), droppedSourceUUID);

        AutoGetCollectionForReadCommand droppedColl(_opCtx.get(), NamespaceString("foo.t"));
        ASSERT_TRUE(droppedColl.getCollection());
        ASSERT_EQUALS(droppedColl.getCollection()->uuid().get(), droppedTargetUUID);
    }
}

TEST_F(RSRollbackTest, NewDocumentsInsertedAfterRenamingCollectionAcrossDatabasesShouldBeDropped) {
    createOplog(_opCtx.get());
    Collection* collection = nullptr;
    {
        AutoGetOrCreateDb autoDb(_opCtx.get(), "foo", MODE_X);
        mongo::WriteUnitOfWork wuow(_opCtx.get());
        collection = autoDb.getDb()->getCollection(_opCtx.get(), "foo.t");
        if (!collection) {
            CollectionOptions options;
            options.uuid = UUID::gen();
            collection = _createCollection(_opCtx.get(), "foo.t", options);
        }
        ASSERT(collection);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 1 << "a" << 1)), nullOpDebug, false));
        wuow.commit();
    }

    auto renamedCollectionUUID = collection->uuid().get();
    ASSERT(collection->numRecords(_opCtx.get()) == 1);

    OpTime dropTime = OpTime(Timestamp(2, 0), 5);
    auto dpns = NamespaceString("test.t").makeDropPendingNamespace(dropTime);

    CollectionOptions droppedCollOptions;
    droppedCollOptions.uuid = UUID::gen();
    auto droppedColl = _createCollection(_opCtx.get(), dpns, droppedCollOptions);
    _dropPendingCollectionReaper->addDropPendingNamespace(dropTime, dpns);
    auto droppedCollectionUUID = droppedColl->uuid().get();

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto renameCollectionOperation = makeRenameCollectionOplogEntry(NamespaceString("test.t"),
                                                                    NamespaceString("foo.t"),
                                                                    renamedCollectionUUID,
                                                                    boost::none,
                                                                    droppedCollectionUUID,
                                                                    false,
                                                                    OpTime(Timestamp(2, 0), 5));

    auto insertDocumentOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(3), 0) << "h" << 1LL << "op"
                                 << "i"
                                 << "ui"
                                 << renamedCollectionUUID
                                 << "ns"
                                 << "foo.t"
                                 << "o"
                                 << BSON("_id" << 1 << "a" << 1)),
                       RecordId(3));

    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_TRUE(autoCollDropPending.getCollection());
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("foo.t"));
        ASSERT_TRUE(renamedColl.getCollection());
        AutoGetCollectionForReadCommand droppedColl(_opCtx.get(), NamespaceString("test.t"));
        ASSERT_FALSE(droppedColl.getCollection());
    }
    ASSERT_OK(syncRollback(
        _opCtx.get(),
        OplogInterfaceMock({insertDocumentOperation, renameCollectionOperation, commonOperation}),
        rollbackSource,
        {},
        _coordinator,
        _replicationProcess.get()));

    {
        AutoGetCollectionForReadCommand autoCollDropPending(_opCtx.get(), dpns);
        ASSERT_FALSE(autoCollDropPending.getCollection());
        AutoGetCollectionForReadCommand renamedColl(_opCtx.get(), NamespaceString("test.t"));
        ASSERT_TRUE(renamedColl.getCollection());
        ASSERT_EQUALS(renamedColl.getCollection()->uuid().get(), droppedCollectionUUID);
        ASSERT(renamedColl.getCollection()->numRecords(_opCtx.get()) == 0);
        AutoGetCollectionForReadCommand droppedColl(_opCtx.get(), NamespaceString("foo.t"));
        ASSERT_FALSE(droppedColl.getCollection());
    }
}


TEST_F(RSRollbackTest, RollbackCollModCommandFailsIfRBIDChangesWhileSyncingCollectionMetadata) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto collModOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << coll->uuid().get()
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("collMod"
                                         << "t"
                                         << "validationLevel"
                                         << "off")),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        using RollbackSourceMock::RollbackSourceMock;
        int getRollbackId() const override {
            return getCollectionInfoCalled ? 1 : 0;
        }
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db,
                                                    const UUID& uuid) const override {
            getCollectionInfoCalled = true;
            return BSONObj();
        }
        mutable bool getCollectionInfoCalled = false;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));

    ASSERT_THROWS_CODE(syncRollback(_opCtx.get(),
                                    OplogInterfaceMock({collModOperation, commonOperation}),
                                    rollbackSource,
                                    0,
                                    _coordinator,
                                    _replicationProcess.get())
                           .transitional_ignore(),
                       DBException,
                       40508);
    ASSERT(rollbackSource.getCollectionInfoCalled);
}

TEST_F(RSRollbackTest, RollbackDropDatabaseCommand) {
    createOplog(_opCtx.get());
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto dropDatabaseOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << UUID::gen()
                                 << "ns"
                                 << "test.$cmd"
                                 << "o"
                                 << BSON("dropDatabase" << 1)),
                       RecordId(2));
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({dropDatabaseOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
}

BSONObj makeApplyOpsOplogEntry(Timestamp ts, UUID uuid, std::initializer_list<BSONObj> ops) {
    BSONObjBuilder entry;
    entry << "ts" << ts << "h" << 1LL << "op"
          << "c"
          << "ui" << uuid << "ns"
          << "admin";
    {
        BSONObjBuilder cmd(entry.subobjStart("o"));
        BSONArrayBuilder subops(entry.subarrayStart("applyOps"));
        for (const auto& op : ops) {
            subops << op;
        }
    }
    return entry.obj();
}

OpTime getOpTimeFromOplogEntry(const BSONObj& entry) {
    const BSONElement tsElement = entry["ts"];
    const BSONElement termElement = entry["t"];
    const BSONElement hashElement = entry["h"];
    ASSERT_EQUALS(bsonTimestamp, tsElement.type()) << entry;
    ASSERT_TRUE(hashElement.isNumber()) << entry;
    ASSERT_TRUE(termElement.eoo() || termElement.isNumber()) << entry;
    long long term = hashElement.numberLong();
    if (!termElement.eoo()) {
        term = termElement.numberLong();
    }
    return OpTime(tsElement.timestamp(), term);
}

TEST_F(RSRollbackTest, RollbackApplyOpsCommand) {
    createOplog(_opCtx.get());
    Collection* coll = nullptr;
    CollectionOptions options;
    options.uuid = UUID::gen();
    {
        AutoGetOrCreateDb autoDb(_opCtx.get(), "test", MODE_X);
        mongo::WriteUnitOfWork wuow(_opCtx.get());
        coll = autoDb.getDb()->getCollection(_opCtx.get(), "test.t");
        if (!coll) {
            coll = autoDb.getDb()->createCollection(_opCtx.get(), "test.t", options);
        }
        ASSERT(coll);
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(coll->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 1 << "v" << 2)), nullOpDebug, false));
        ASSERT_OK(coll->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 2 << "v" << 4)), nullOpDebug, false));
        ASSERT_OK(coll->insertDocument(
            _opCtx.get(), InsertStatement(BSON("_id" << 4)), nullOpDebug, false));
        wuow.commit();
    }
    UUID uuid = coll->uuid().get();
    const auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    const auto applyOpsOperation =
        std::make_pair(makeApplyOpsOplogEntry(Timestamp(Seconds(2), 0),
                                              uuid,
                                              {BSON("op"
                                                    << "u"
                                                    << "ui"
                                                    << uuid
                                                    << "ts"
                                                    << Timestamp(1, 1)
                                                    << "t"
                                                    << 1LL
                                                    << "h"
                                                    << 2LL
                                                    << "ns"
                                                    << "test.t"
                                                    << "o2"
                                                    << BSON("_id" << 1)
                                                    << "o"
                                                    << BSON("_id" << 1 << "v" << 2)),
                                               BSON("op"
                                                    << "u"
                                                    << "ui"
                                                    << uuid
                                                    << "ts"
                                                    << Timestamp(2, 1)
                                                    << "t"
                                                    << 1LL
                                                    << "h"
                                                    << 2LL
                                                    << "ns"
                                                    << "test.t"
                                                    << "o2"
                                                    << BSON("_id" << 2)
                                                    << "o"
                                                    << BSON("_id" << 2 << "v" << 4)),
                                               BSON("op"
                                                    << "d"
                                                    << "ui"
                                                    << uuid
                                                    << "ts"
                                                    << Timestamp(3, 1)
                                                    << "t"
                                                    << 1LL
                                                    << "h"
                                                    << 2LL
                                                    << "ns"
                                                    << "test.t"
                                                    << "o"
                                                    << BSON("_id" << 3)),
                                               BSON("op"
                                                    << "i"
                                                    << "ui"
                                                    << uuid
                                                    << "ts"
                                                    << Timestamp(4, 1)
                                                    << "t"
                                                    << 1LL
                                                    << "h"
                                                    << 2LL
                                                    << "ns"
                                                    << "test.t"
                                                    << "o"
                                                    << BSON("_id" << 4))}),
                       RecordId(2));

    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}

        std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                          UUID uuid,
                                                          const BSONObj& filter) const override {
            int numFields = 0;
            for (const auto element : filter) {
                ++numFields;
                ASSERT_EQUALS("_id", element.fieldNameStringData()) << filter;
            }
            ASSERT_EQUALS(1, numFields) << filter;
            searchedIds.insert(filter.firstElement().numberInt());
            switch (filter.firstElement().numberInt()) {
                case 1:
                    return {BSON("_id" << 1 << "v" << 1), NamespaceString()};
                case 2:
                    return {BSON("_id" << 2 << "v" << 3), NamespaceString()};
                case 3:
                    return {BSON("_id" << 3 << "v" << 5), NamespaceString()};
                case 4:
                    return {};
            }
            FAIL("Unexpected findOne request") << filter;
            return {};  // Unreachable; why doesn't compiler know?
        }

        mutable std::multiset<int> searchedIds;
    } rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    _createCollection(_opCtx.get(), "test.t", options);
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({applyOpsOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    ASSERT_EQUALS(4U, rollbackSource.searchedIds.size());
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(1));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(2));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(3));
    ASSERT_EQUALS(1U, rollbackSource.searchedIds.count(4));

    AutoGetCollectionForReadCommand acr(_opCtx.get(), NamespaceString("test.t"));
    BSONObj result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 1), result));
    ASSERT_EQUALS(1, result["v"].numberInt()) << result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 2), result));
    ASSERT_EQUALS(3, result["v"].numberInt()) << result;
    ASSERT(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 3), result));
    ASSERT_EQUALS(5, result["v"].numberInt()) << result;
    ASSERT_FALSE(Helpers::findOne(_opCtx.get(), acr.getCollection(), BSON("_id" << 4), result))
        << result;
}

TEST_F(RSRollbackTest, RollbackCreateCollectionCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto createCollectionOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << coll->uuid().get()
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("create"
                                         << "t")),
                       RecordId(2));
    RollbackSourceMock rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({createCollectionOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    {
        Lock::DBLock dbLock(_opCtx.get(), "test", MODE_S);
        auto db = dbHolder().get(_opCtx.get(), "test");
        ASSERT_TRUE(db);
        ASSERT_FALSE(db->getCollection(_opCtx.get(), "test.t"));
    }
}

TEST_F(RSRollbackTest, RollbackCollectionModificationCommand) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto collectionModificationOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << coll->uuid().get()
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("collMod"
                                         << "t"
                                         << "noPadding"
                                         << false)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)), called(false) {}
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db, const UUID& uuid) const {
            called = true;
            return RollbackSourceMock::getCollectionInfoByUUID(db, uuid);
        }
        mutable bool called;
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    startCapturingLogMessages();
    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));
    stopCapturingLogMessages();
    ASSERT_TRUE(rollbackSource.called);
    for (const auto& message : getCapturedLogMessages()) {
        ASSERT_TRUE(message.find("ignoring op with no _id during rollback. ns: test.t") ==
                    std::string::npos);
    }
}

TEST_F(RSRollbackTest, RollbackCollectionModificationCommandInvalidCollectionOptions) {
    createOplog(_opCtx.get());
    CollectionOptions options;
    options.uuid = UUID::gen();
    auto coll = _createCollection(_opCtx.get(), "test.t", options);

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    auto collectionModificationOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) << "h" << 1LL << "op"
                                 << "c"
                                 << "ui"
                                 << coll->uuid().get()
                                 << "ns"
                                 << "test.t"
                                 << "o"
                                 << BSON("collMod"
                                         << "t"
                                         << "noPadding"
                                         << false)),
                       RecordId(2));
    class RollbackSourceLocal : public RollbackSourceMock {
    public:
        RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
            : RollbackSourceMock(std::move(oplog)) {}
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db, const UUID& uuid) const {
            return BSON("options" << 12345);
        }
    };
    RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
        commonOperation,
    })));
    auto status =
        syncRollback(_opCtx.get(),
                     OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                     rollbackSource,
                     {},
                     _coordinator,
                     _replicationProcess.get());
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Failed to parse options");
}

TEST(RSRollbackTest, LocalEntryWithoutNsIsFatal) {
    const auto validOplogEntry = BSON("op"
                                      << "i"
                                      << "ui"
                                      << UUID::gen()
                                      << "ts"
                                      << Timestamp(1, 1)
                                      << "t"
                                      << 1LL
                                      << "h"
                                      << 1LL
                                      << "ns"
                                      << "test.t"
                                      << "o"
                                      << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));
    const auto invalidOplogEntry = BSON("op"
                                        << "i"
                                        << "ui"
                                        << UUID::gen()
                                        << "ts"
                                        << Timestamp(1, 1)
                                        << "t"
                                        << 1LL
                                        << "h"
                                        << 1LL
                                        << "ns"
                                        << ""
                                        << "o"
                                        << BSON("_id" << 1 << "a" << 1));
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, invalidOplogEntry).transitional_ignore(),
                  RSFatalException);
}

TEST(RSRollbackTest, LocalEntryWithoutOIsFatal) {
    const auto validOplogEntry = BSON("op"
                                      << "i"
                                      << "ui"
                                      << UUID::gen()
                                      << "ts"
                                      << Timestamp(1, 1)
                                      << "t"
                                      << 1LL
                                      << "h"
                                      << 1LL
                                      << "ns"
                                      << "test.t"
                                      << "o"
                                      << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));
    const auto invalidOplogEntry = BSON("op"
                                        << "i"
                                        << "ui"
                                        << UUID::gen()
                                        << "ts"
                                        << Timestamp(1, 1)
                                        << "t"
                                        << 1LL
                                        << "h"
                                        << 1LL
                                        << "ns"
                                        << "test.t"
                                        << "o"
                                        << BSONObj());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, invalidOplogEntry).transitional_ignore(),
                  RSFatalException);
}

TEST(RSRollbackTest, LocalEntryWithoutO2IsFatal) {
    const auto validOplogEntry = BSON("op"
                                      << "u"
                                      << "ui"
                                      << UUID::gen()
                                      << "ts"
                                      << Timestamp(1, 1)
                                      << "t"
                                      << 1LL
                                      << "h"
                                      << 1LL
                                      << "ns"
                                      << "test.t"
                                      << "o"
                                      << BSON("_id" << 1 << "a" << 1)
                                      << "o2"
                                      << BSON("_id" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));
    const auto invalidOplogEntry = BSON("op"
                                        << "u"
                                        << "ui"
                                        << UUID::gen()
                                        << "ts"
                                        << Timestamp(1, 1)
                                        << "t"
                                        << 1LL
                                        << "h"
                                        << 1LL
                                        << "ns"
                                        << "test.t"
                                        << "o"
                                        << BSON("_id" << 1 << "a" << 1)
                                        << "o2"
                                        << BSONObj());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, invalidOplogEntry).transitional_ignore(),
                  RSFatalException);
}

DEATH_TEST_F(RSRollbackTest, LocalEntryWithTxnNumberWithoutSessionIdIsFatal, "invariant") {
    auto validOplogEntry = BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "h" << 1LL << "op"
                                     << "i"
                                     << "ui"
                                     << UUID::gen()
                                     << "ns"
                                     << "test.t"
                                     << "o"
                                     << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));

    const auto txnNumber = BSON("txnNumber" << 1LL);
    const auto noSessionIdOrStmtId = validOplogEntry.addField(txnNumber.firstElement());

    const auto stmtId = BSON("stmtId" << 1);
    const auto noSessionId = noSessionIdOrStmtId.addField(stmtId.firstElement());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, noSessionId).transitional_ignore(),
                  RSFatalException);
}

DEATH_TEST_F(RSRollbackTest, LocalEntryWithTxnNumberWithoutStmtIdIsFatal, "invariant") {
    auto validOplogEntry = BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "h" << 1LL << "op"
                                     << "i"
                                     << "ui"
                                     << UUID::gen()
                                     << "ns"
                                     << "test.t"
                                     << "o"
                                     << BSON("_id" << 1 << "a" << 1));
    FixUpInfo fui;
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, validOplogEntry));

    const auto txnNumber = BSON("txnNumber" << 1LL);
    const auto noSessionIdOrStmtId = validOplogEntry.addField(txnNumber.firstElement());

    const auto lsid = makeLogicalSessionIdForTest();
    const auto sessionId = BSON("lsid" << lsid.toBSON());
    const auto noStmtId = noSessionIdOrStmtId.addField(sessionId.firstElement());
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, noStmtId).transitional_ignore(),
                  RSFatalException);
}

TEST_F(RSRollbackTest, LocalEntryWithTxnNumberWithoutTxnTableUUIDIsFatal) {
    // If txnNumber is present, but the transaction collection has no UUID, rollback fails.
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "h" << 1LL << "op"
                  << "i"
                  << "ui"
                  << uuid
                  << "ns"
                  << "test.t"
                  << "o"
                  << BSON("_id" << 1 << "a" << 1)
                  << "txnNumber"
                  << 1LL
                  << "stmtId"
                  << 1
                  << "lsid"
                  << lsid.toBSON());

    FixUpInfo fui;
    ASSERT_THROWS(updateFixUpInfoFromLocalOplogEntry(fui, entryWithTxnNumber).ignore(),
                  RSFatalException);
}

TEST_F(RSRollbackTest, LocalEntryWithTxnNumberAddsTransactionTableDocToBeRefetched) {
    FixUpInfo fui;

    // With no txnNumber present, no extra documents need to be refetched.
    auto entryWithoutTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "h" << 1LL << "op"
                  << "i"
                  << "ui"
                  << UUID::gen()
                  << "ns"
                  << "test.t2"
                  << "o"
                  << BSON("_id" << 2 << "a" << 2));

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, entryWithoutTxnNumber));
    ASSERT_EQ(fui.docsToRefetch.size(), 1U);

    // If txnNumber is present, and the transaction table exists and has a UUID, the session
    // transactions table document corresponding to the oplog entry's sessionId also needs to be
    // refetched.
    UUID uuid = UUID::gen();
    auto lsid = makeLogicalSessionIdForTest();
    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(1), 0) << "t" << 1LL << "h" << 1LL << "op"
                  << "i"
                  << "ui"
                  << uuid
                  << "ns"
                  << "test.t"
                  << "o"
                  << BSON("_id" << 1 << "a" << 1)
                  << "txnNumber"
                  << 1LL
                  << "stmtId"
                  << 1
                  << "lsid"
                  << lsid.toBSON());
    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, entryWithTxnNumber));
    ASSERT_EQ(fui.docsToRefetch.size(), 3U);

    auto expectedObj = BSON("_id" << lsid.toBSON());
    DocID expectedTxnDoc(expectedObj, expectedObj.firstElement(), transactionTableUUID);
    ASSERT_TRUE(fui.docsToRefetch.find(expectedTxnDoc) != fui.docsToRefetch.end());
}

TEST_F(RSRollbackTest, RollbackFailsIfTransactionDocumentRefetchReturnsDifferentNamespace) {
    createOplog(_opCtx.get());

    // Create a valid FixUpInfo struct for rolling back a single CRUD operation that has a
    // transaction number and session id.
    FixUpInfo fui;

    auto entryWithTxnNumber =
        BSON("ts" << Timestamp(Seconds(2), 0) << "t" << 1LL << "h" << 1LL << "op"
                  << "i"
                  << "ui"
                  << UUID::gen()
                  << "ns"
                  << "test.t"
                  << "o"
                  << BSON("_id" << 1 << "a" << 1)
                  << "txnNumber"
                  << 1LL
                  << "stmtId"
                  << 1
                  << "lsid"
                  << makeLogicalSessionIdForTest().toBSON());

    UUID transactionTableUUID = UUID::gen();
    fui.transactionTableUUID = transactionTableUUID;

    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
    fui.commonPoint = OpTime(Timestamp(Seconds(1), 0), 1LL);
    fui.commonPointOurDiskloc = RecordId(1);

    fui.rbid = 1;

    // The FixUpInfo will have an extra doc to refetch: the corresponding transaction table entry.
    ASSERT_OK(updateFixUpInfoFromLocalOplogEntry(fui, entryWithTxnNumber));
    ASSERT_EQ(fui.docsToRefetch.size(), 2U);

    {
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)) {}
            std::pair<BSONObj, NamespaceString> findOneByUUID(
                const std::string& db, UUID uuid, const BSONObj& filter) const override {
                return {BSONObj(), NamespaceString::kSessionTransactionsTableNamespace};
            }
            int getRollbackId() const override {
                return 1;
            }
        };

        // Should not throw, since findOneByUUID will return the expected namespace.
        syncFixUp(_opCtx.get(),
                  fui,
                  RollbackSourceLocal(
                      std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation}))),
                  _coordinator,
                  _replicationProcess.get());
    }

    {
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)) {}
            std::pair<BSONObj, NamespaceString> findOneByUUID(
                const std::string& db, UUID uuid, const BSONObj& filter) const override {
                return {BSONObj(), NamespaceString("foo.bar")};
            }
            int getRollbackId() const override {
                return 1;
            }
        };

        // The returned namespace will not be the expected one, implying a rename/drop of the
        // transactions collection across this node and the sync source, so rollback should fail.
        ASSERT_THROWS(syncFixUp(_opCtx.get(),
                                fui,
                                RollbackSourceLocal(std::unique_ptr<OplogInterface>(
                                    new OplogInterfaceMock({commonOperation}))),
                                _coordinator,
                                _replicationProcess.get()),
                      RSFatalException);
    }
}

TEST_F(RSRollbackTest, RollbackReturnsImmediatelyOnFailureToTransitionToRollback) {
    // On failing to transition to ROLLBACK, rollback() should return immediately and not call
    // syncRollback(). We provide an empty oplog so that if syncRollback() is called erroneously,
    // we would go fatal.
    OplogInterfaceMock localOplogWithSingleOplogEntry({makeNoopOplogEntryAndRecordId(Seconds(1))});
    RollbackSourceMock rollbackSourceWithInvalidOplog(stdx::make_unique<OplogInterfaceMock>());

    // Inject ReplicationCoordinator::setFollowerMode() error. We set the current member state
    // because it will be logged by rollback() on failing to transition to ROLLBACK.
    ASSERT_OK(_coordinator->setFollowerMode(MemberState::RS_SECONDARY));
    _coordinator->failSettingFollowerMode(MemberState::RS_ROLLBACK, ErrorCodes::NotSecondary);

    startCapturingLogMessages();
    rollback(_opCtx.get(),
             localOplogWithSingleOplogEntry,
             rollbackSourceWithInvalidOplog,
             {},
             _coordinator,
             _replicationProcess.get());
    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countLogLinesContaining("Cannot transition from SECONDARY to ROLLBACK"));
    ASSERT_EQUALS(MemberState(MemberState::RS_SECONDARY), _coordinator->getMemberState());
}

DEATH_TEST_F(
    RSRollbackTest,
    RollbackUnrecoverableRollbackErrorTriggersFatalAssertion,
    "Unable to complete rollback. A full resync may be needed: "
    "UnrecoverableRollbackError: need to rollback, but unable to determine common point "
    "between local and remote oplog: InvalidSyncSource: remote oplog empty or unreadable") {
    // rollback() should abort on getting UnrecoverableRollbackError from syncRollback(). An empty
    // local oplog will make syncRollback() return the intended error.
    OplogInterfaceMock localOplogWithSingleOplogEntry({makeNoopOplogEntryAndRecordId(Seconds(1))});
    RollbackSourceMock rollbackSourceWithInvalidOplog(stdx::make_unique<OplogInterfaceMock>());

    rollback(_opCtx.get(),
             localOplogWithSingleOplogEntry,
             rollbackSourceWithInvalidOplog,
             {},
             _coordinator,
             _replicationProcess.get());
}

TEST_F(RSRollbackTest, RollbackLogsRetryMessageAndReturnsOnNonUnrecoverableRollbackError) {
    // If local oplog is empty, syncRollback() returns OplogStartMissing (instead of
    // UnrecoverableRollbackError when the remote oplog is missing). rollback() should log a message
    // about retrying rollback later before returning.
    OplogInterfaceMock localOplogWithNoEntries;
    OplogInterfaceMock::Operations remoteOperations({makeNoopOplogEntryAndRecordId(Seconds(1))});
    auto remoteOplog = stdx::make_unique<OplogInterfaceMock>(remoteOperations);
    RollbackSourceMock rollbackSourceWithValidOplog(std::move(remoteOplog));
    auto noopSleepSecsFn = [](int) {};

    startCapturingLogMessages();
    rollback(_opCtx.get(),
             localOplogWithNoEntries,
             rollbackSourceWithValidOplog,
             {},
             _coordinator,
             _replicationProcess.get(),
             noopSleepSecsFn);
    stopCapturingLogMessages();

    ASSERT_EQUALS(
        1, countLogLinesContaining("Rollback cannot complete at this time (retrying later)"));
    ASSERT_EQUALS(MemberState(MemberState::RS_RECOVERING), _coordinator->getMemberState());
}

DEATH_TEST_F(RSRollbackTest,
             RollbackTriggersFatalAssertionOnDetectingShardIdentityDocumentRollback,
             "shardIdentity document rollback detected.  Shutting down to clear in-memory sharding "
             "state.  Restarting this process should safely return it to a healthy state") {
    auto commonOperation = makeNoopOplogEntryAndRecordId(Seconds(1));
    OplogInterfaceMock localOplog({commonOperation});
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    ASSERT_FALSE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());
    ShardIdentityRollbackNotifier::get(_opCtx.get())->recordThatRollbackHappened();
    ASSERT_TRUE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());

    createOplog(_opCtx.get());
    rollback(_opCtx.get(), localOplog, rollbackSource, {}, _coordinator, _replicationProcess.get());
}

DEATH_TEST_F(
    RSRollbackTest,
    RollbackTriggersFatalAssertionOnFailingToTransitionToRecoveringAfterSyncRollbackReturns,
    "Failed to transition into RECOVERING; expected to be in state ROLLBACK; found self in "
    "ROLLBACK") {
    auto commonOperation = makeNoopOplogEntryAndRecordId(Seconds(1));
    OplogInterfaceMock localOplog({commonOperation});
    RollbackSourceMock rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})));

    _coordinator->failSettingFollowerMode(MemberState::RS_RECOVERING, ErrorCodes::IllegalOperation);

    createOplog(_opCtx.get());
    rollback(_opCtx.get(), localOplog, rollbackSource, {}, _coordinator, _replicationProcess.get());
}

// The testcases used here are trying to detect off-by-one errors in
// FixUpInfo::removeAllDocsToRefectchFor.
TEST(FixUpInfoTest, RemoveAllDocsToRefetchForWorks) {
    const auto normalHolder = BSON("" << OID::gen());
    const auto normalKey = normalHolder.firstElement();

    UUID uuid1 = UUID::gen();
    UUID uuid2 = UUID::gen();
    UUID uuid3 = UUID::gen();

    // Can't use ASSERT_EQ with this since it isn't ostream-able. Failures will at least give you
    // the size. If that isn't enough, use GDB.
    using DocSet = std::set<DocID>;

    FixUpInfo fui;
    fui.docsToRefetch = {
        DocID::minFor(uuid1),
        DocID{{}, normalKey, uuid1},
        DocID::maxFor(uuid1),

        DocID::minFor(uuid2),
        DocID{{}, normalKey, uuid2},
        DocID::maxFor(uuid2),

        DocID::minFor(uuid3),
        DocID{{}, normalKey, uuid3},
        DocID::maxFor(uuid3),
    };

    // Remove from the middle.
    fui.removeAllDocsToRefetchFor(uuid2);
    ASSERT((fui.docsToRefetch ==
            DocSet{
                DocID::minFor(uuid1),
                DocID{{}, normalKey, uuid1},
                DocID::maxFor(uuid1),

                DocID::minFor(uuid3),
                DocID{{}, normalKey, uuid3},
                DocID::maxFor(uuid3),
            }))
        << "remaining docs: " << fui.docsToRefetch.size();

    // Remove from the end.
    fui.removeAllDocsToRefetchFor(uuid3);
    ASSERT((fui.docsToRefetch ==
            DocSet{
                DocID::minFor(uuid1),  // This comment helps clang-format.
                DocID{{}, normalKey, uuid1},
                DocID::maxFor(uuid1),
            }))
        << "remaining docs: " << fui.docsToRefetch.size();

    // Everything else.
    fui.removeAllDocsToRefetchFor(uuid1);
    ASSERT((fui.docsToRefetch == DocSet{})) << "remaining docs: " << fui.docsToRefetch.size();
}

}  // namespace
