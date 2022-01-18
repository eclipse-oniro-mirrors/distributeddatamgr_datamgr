/*
 * Copyright (c) 2021 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include "distributeddb_data_generate_unit_test.h"
#include "distributeddb_tools_unit_test.h"
#include "log_print.h"
#include "relational_store_manager.h"

using namespace testing::ext;
using namespace DistributedDB;
using namespace DistributedDBUnitTest;
using namespace std;

namespace {
    constexpr const char* DB_SUFFIX = ".db";
    constexpr const char* STORE_ID = "Relational_Store_ID";
    std::string g_testDir;
    std::string g_dbDir;
    DistributedDB::RelationalStoreManager g_mgr(APP_ID, USER_ID);

    const std::string NORMAL_CREATE_TABLE_SQL = "CREATE TABLE IF NOT EXISTS sync_data(" \
        "key         BLOB NOT NULL," \
        "value       BLOB," \
        "timestamp   INT  NOT NULL," \
        "flag        INT  NOT NULL," \
        "device      BLOB," \
        "ori_device  BLOB," \
        "hash_key    BLOB PRIMARY KEY NOT NULL," \
        "w_timestamp INT);" \
        "CREATE INDEX key_index ON sync_data (key, flag);";
}

class DistributedInterfacesRelationalTest : public testing::Test {
public:
    static void SetUpTestCase(void);
    static void TearDownTestCase(void);
    void SetUp();
    void TearDown();
};

void DistributedInterfacesRelationalTest::SetUpTestCase(void)
{
    DistributedDBToolsUnitTest::TestDirInit(g_testDir);
    LOGD("Test dir is %s", g_testDir.c_str());
    g_dbDir = g_testDir + "/";
}

void DistributedInterfacesRelationalTest::TearDownTestCase(void)
{
}

void DistributedInterfacesRelationalTest::SetUp(void)
{
    DistributedDBToolsUnitTest::PrintTestCaseInfo();
}

void DistributedInterfacesRelationalTest::TearDown(void)
{
    DistributedDBToolsUnitTest::RemoveTestDbFiles(g_testDir);
}

/**
  * @tc.name: RelationalStoreTest001
  * @tc.desc: Test open store and create distributed db
  * @tc.type: FUNC
  * @tc.require: AR000GK58F
  * @tc.author: lianhuix
  */
HWTEST_F(DistributedInterfacesRelationalTest, RelationalStoreTest001, TestSize.Level1)
{
    /**
     * @tc.steps:step1. Prepare db file
     * @tc.expected: step1. Return OK.
     */
    sqlite3 *db = RelationalTestUtils::CreateDataBase(g_dbDir + STORE_ID + DB_SUFFIX);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, "PRAGMA journal_mode=WAL;"), SQLITE_OK);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, NORMAL_CREATE_TABLE_SQL), SQLITE_OK);
    EXPECT_EQ(sqlite3_close_v2(db), SQLITE_OK);

    /**
     * @tc.steps:step2. open relational store, create distributed table, close store
     * @tc.expected: step2. Return OK.
     */
    RelationalStoreDelegate *delegate = nullptr;
    DBStatus status = g_mgr.OpenStore(g_dbDir + STORE_ID + DB_SUFFIX, STORE_ID, {}, delegate);
    EXPECT_EQ(status, OK);
    ASSERT_NE(delegate, nullptr);

    status = delegate->CreateDistributedTable("sync_data");
    EXPECT_EQ(status, OK);

    // test create same table again
    status = delegate->CreateDistributedTable("sync_data");
    EXPECT_EQ(status, OK);

    status = g_mgr.CloseStore(delegate);
    EXPECT_EQ(status, OK);
    ASSERT_EQ(delegate, nullptr);

    /**
     * @tc.steps:step3. drop sync_data table
     * @tc.expected: step3. Return OK.
     */
    db = RelationalTestUtils::CreateDataBase(g_dbDir + STORE_ID + DB_SUFFIX);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, "drop table sync_data;"), SQLITE_OK);
    EXPECT_EQ(sqlite3_close_v2(db), SQLITE_OK);

    /**
     * @tc.steps:step4. open again, check auxiliary should be delete
     * @tc.expected: step4. Return OK.
     */
    delegate = nullptr;
    status = g_mgr.OpenStore(g_dbDir  + STORE_ID + DB_SUFFIX, STORE_ID, {}, delegate);
    EXPECT_EQ(status, OK);
    ASSERT_NE(delegate, nullptr);
    status = g_mgr.CloseStore(delegate);
    EXPECT_EQ(status, OK);
    ASSERT_EQ(delegate, nullptr);
}

/**
  * @tc.name: RelationalStoreTest002
  * @tc.desc: Test open store with invalid path or store ID
  * @tc.type: FUNC
  * @tc.require: AR000GK58F
  * @tc.author: lianhuix
  */
HWTEST_F(DistributedInterfacesRelationalTest, RelationalStoreTest002, TestSize.Level1)
{
    /**
     * @tc.steps:step1. Prepare db file
     * @tc.expected: step1. Return OK.
     */
    sqlite3 *db = RelationalTestUtils::CreateDataBase(g_dbDir + STORE_ID + DB_SUFFIX);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, "PRAGMA journal_mode=WAL;"), SQLITE_OK);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, NORMAL_CREATE_TABLE_SQL), SQLITE_OK);
    EXPECT_EQ(sqlite3_close_v2(db), SQLITE_OK);

    /**
     * @tc.steps:step2. Test open store with invalid path or store ID
     * @tc.expected: step2. open store failed.
     */
    RelationalStoreDelegate *delegate = nullptr;

    // test open store with path not exist
    DBStatus status = g_mgr.OpenStore(g_dbDir + "tmp/" + STORE_ID + DB_SUFFIX, STORE_ID, {}, delegate);
    EXPECT_NE(status, OK);
    ASSERT_EQ(delegate, nullptr);

    // test open store with empty store_id
    status = g_mgr.OpenStore(g_dbDir + STORE_ID + DB_SUFFIX, {}, {}, delegate);
    EXPECT_NE(status, OK);
    ASSERT_EQ(delegate, nullptr);

    // test open store with path has invalid character
    status = g_mgr.OpenStore(g_dbDir + "t&m$p/" + STORE_ID + DB_SUFFIX, {}, {}, delegate);
    EXPECT_NE(status, OK);
    ASSERT_EQ(delegate, nullptr);

    // test open store with store_id has invalid character
    status = g_mgr.OpenStore(g_dbDir + STORE_ID + DB_SUFFIX, "Relation@al_S$tore_ID", {}, delegate);
    EXPECT_NE(status, OK);
    ASSERT_EQ(delegate, nullptr);

    // test open store with store_id length over MAX_STORE_ID_LENGTH
    status = g_mgr.OpenStore(g_dbDir + STORE_ID + DB_SUFFIX,
        std::string(DBConstant::MAX_STORE_ID_LENGTH + 1, 'a'), {}, delegate);
    EXPECT_NE(status, OK);
    ASSERT_EQ(delegate, nullptr);
}

/**
  * @tc.name: RelationalStoreTest003
  * @tc.desc: Test open store with journal_mode is not WAL
  * @tc.type: FUNC
  * @tc.require: AR000GK58F
  * @tc.author: lianhuix
  */
HWTEST_F(DistributedInterfacesRelationalTest, RelationalStoreTest003, TestSize.Level1)
{
    /**
     * @tc.steps:step1. Prepare db file with string is not WAL
     * @tc.expected: step1. Return OK.
     */
    sqlite3 *db = RelationalTestUtils::CreateDataBase(g_dbDir + STORE_ID + DB_SUFFIX);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, "PRAGMA journal_mode=PRESIST;"), SQLITE_OK);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, NORMAL_CREATE_TABLE_SQL), SQLITE_OK);
    EXPECT_EQ(sqlite3_close_v2(db), SQLITE_OK);

    /**
     * @tc.steps:step2. Test open store
     * @tc.expected: step2. Open store failed.
     */
    RelationalStoreDelegate *delegate = nullptr;

    // test open store with journal mode is not WAL
    DBStatus status = g_mgr.OpenStore(g_dbDir  + STORE_ID + DB_SUFFIX, STORE_ID, {}, delegate);
    EXPECT_NE(status, OK);
    ASSERT_EQ(delegate, nullptr);
}

/**
  * @tc.name: RelationalStoreTest004
  * @tc.desc: Test create distributed table with over limit
  * @tc.type: FUNC
  * @tc.require: AR000GK58F
  * @tc.author: lianhuix
  */
HWTEST_F(DistributedInterfacesRelationalTest, RelationalStoreTest004, TestSize.Level1)
{
    /**
     * @tc.steps:step1. Prepare db file with multiple tables
     * @tc.expected: step1. Return OK.
     */
    sqlite3 *db = RelationalTestUtils::CreateDataBase(g_dbDir + STORE_ID + DB_SUFFIX);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, "PRAGMA journal_mode=WAL;"), SQLITE_OK);
    const int tableCount = DBConstant::MAX_DISTRIBUTED_TABLE_COUNT + 10; // 10: additional size for test abnormal scene
    for (int i=0; i<tableCount; i++) {
        std::string sql = "CREATE TABLE TEST_" + std::to_string(i) + "(id INT PRIMARY KEY, value TEXT);";
        EXPECT_EQ(RelationalTestUtils::ExecSql(db, sql), SQLITE_OK);
    }
    EXPECT_EQ(sqlite3_close_v2(db), SQLITE_OK);

    RelationalStoreDelegate *delegate = nullptr;

    /**
     * @tc.steps:step2. Open store and create multiple distributed table
     * @tc.expected: step2. The tables in limited quantity were created successfully, the others failed.
     */
    DBStatus status = g_mgr.OpenStore(g_dbDir  + STORE_ID + DB_SUFFIX, STORE_ID, {}, delegate);
    EXPECT_EQ(status, OK);
    ASSERT_NE(delegate, nullptr);

    for (int i=0; i<tableCount; i++) {
        if (i < DBConstant::MAX_DISTRIBUTED_TABLE_COUNT) {
            EXPECT_EQ(delegate->CreateDistributedTable("TEST_" + std::to_string(i)), OK);
        } else {
            EXPECT_NE(delegate->CreateDistributedTable("TEST_" + std::to_string(i)), OK);
        }
    }

    /**
     * @tc.steps:step3. Close store
     * @tc.expected: step3. Return OK.
     */
    status = g_mgr.CloseStore(delegate);
    EXPECT_EQ(status, OK);
    ASSERT_EQ(delegate, nullptr);
}

/**
  * @tc.name: RelationalStoreTest005
  * @tc.desc: Test create distributed table with invalid table name
  * @tc.type: FUNC
  * @tc.require: AR000GK58F
  * @tc.author: lianhuix
  */
HWTEST_F(DistributedInterfacesRelationalTest, RelationalStoreTest005, TestSize.Level1)
{
    /**
     * @tc.steps:step1. Prepare db file
     * @tc.expected: step1. Return OK.
     */
    sqlite3 *db = RelationalTestUtils::CreateDataBase(g_dbDir + STORE_ID + DB_SUFFIX);
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, "PRAGMA journal_mode=WAL;"), SQLITE_OK);
    EXPECT_EQ(RelationalTestUtils::ExecSql(db, NORMAL_CREATE_TABLE_SQL), SQLITE_OK);
    EXPECT_EQ(sqlite3_close_v2(db), SQLITE_OK);

    /**
     * @tc.steps:step2. Open store
     * @tc.expected: step2. return OK
     */
    RelationalStoreDelegate *delegate = nullptr;
    DBStatus status = g_mgr.OpenStore(g_dbDir  + STORE_ID + DB_SUFFIX, STORE_ID, {}, delegate);
    EXPECT_EQ(status, OK);
    ASSERT_NE(delegate, nullptr);

    /**
     * @tc.steps:step3. Create distirbuted table with invald table name
     * @tc.expected: step3. Create distributed table failed.
     */
    EXPECT_NE(delegate->CreateDistributedTable(DBConstant::SYSTEM_TABLE_PREFIX + "_tmp"), OK);

    /**
     * @tc.steps:step4. Close store
     * @tc.expected: step4. Return OK.
     */
    status = g_mgr.CloseStore(delegate);
    EXPECT_EQ(status, OK);
    ASSERT_EQ(delegate, nullptr);
}