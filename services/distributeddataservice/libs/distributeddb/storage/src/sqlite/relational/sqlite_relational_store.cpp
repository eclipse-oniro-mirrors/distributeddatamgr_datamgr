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
#ifdef RELATIONAL_STORE
#include "sqlite_relational_store.h"

#include "db_common.h"
#include "db_errno.h"
#include "log_print.h"
#include "db_types.h"
#include "sqlite_relational_store_connection.h"
#include "storage_engine_manager.h"

namespace DistributedDB {
namespace {
    constexpr const char *RELATIONAL_SCHEMA_KEY = "relational_schema";
    constexpr const char *LOG_TABLE_VERSION_KEY = "log_table_versoin";
    constexpr const char *LOG_TABLE_VERSION_1 = "1.0";
}

SQLiteRelationalStore::~SQLiteRelationalStore()
{
    delete sqliteStorageEngine_;
    sqliteStorageEngine_ = nullptr;
}

// Called when a new connection created.
void SQLiteRelationalStore::IncreaseConnectionCounter()
{
    connectionCount_.fetch_add(1, std::memory_order_seq_cst);
    if (connectionCount_.load() > 0) {
        sqliteStorageEngine_->SetConnectionFlag(true);
    }
}

RelationalStoreConnection *SQLiteRelationalStore::GetDBConnection(int &errCode)
{
    std::lock_guard<std::mutex> lock(connectMutex_);
    RelationalStoreConnection* connection = new (std::nothrow) SQLiteRelationalStoreConnection(this);
    if (connection == nullptr) {
        errCode = -E_OUT_OF_MEMORY;
        return nullptr;
    }
    IncObjRef(this);
    IncreaseConnectionCounter();
    return connection;
}

static void InitDataBaseOption(const RelationalDBProperties &properties, OpenDbProperties &option)
{
    option.uri = properties.GetStringProp(DBProperties::DATA_DIR, "");
    option.createIfNecessary = properties.GetBoolProp(DBProperties::CREATE_IF_NECESSARY, false);
}

int SQLiteRelationalStore::InitStorageEngine(const RelationalDBProperties &properties)
{
    OpenDbProperties option;
    InitDataBaseOption(properties, option);

    StorageEngineAttr poolSize = {1, 1, 0, 16}; // at most 1 write 16 read.
    int errCode = sqliteStorageEngine_->InitSQLiteStorageEngine(poolSize, option);
    if (errCode != E_OK) {
        LOGE("Init the sqlite storage engine failed:%d", errCode);
    }
    return errCode;
}

void SQLiteRelationalStore::ReleaseResources()
{
    if (sqliteStorageEngine_ != nullptr) {
        sqliteStorageEngine_->ClearEnginePasswd();
        (void)StorageEngineManager::ReleaseStorageEngine(sqliteStorageEngine_);
    }
}

int SQLiteRelationalStore::CheckDBMode()
{
    int errCode = E_OK;
    auto *handle = GetHandle(false, errCode);
    if (handle == nullptr) {
        return errCode;
    }
    errCode = handle->CheckDBModeForRelational();
    if (errCode != E_OK) {
        LOGE("check relational DB mode failed. %d", errCode);
    }

    ReleaseHandle(handle);
    return errCode;
}

int SQLiteRelationalStore::GetSchemaFromMeta()
{
    const Key schemaKey(RELATIONAL_SCHEMA_KEY, RELATIONAL_SCHEMA_KEY + strlen(RELATIONAL_SCHEMA_KEY));
    Value schemaVal;
    int errCode = storageEngine_->GetMetaData(schemaKey, schemaVal);
    if (errCode != E_OK && errCode != -E_NOT_FOUND) {
        LOGE("Get relational schema from meta table failed. %d", errCode);
        return errCode;
    } else if (errCode == -E_NOT_FOUND || schemaVal.empty()) {
        LOGW("No relational schema info was found.");
        return E_OK;
    }

    std::string schemaStr;
    DBCommon::VectorToString(schemaVal, schemaStr);
    RelationalSchemaObject schema;
    errCode = schema.ParseFromSchemaString(schemaStr);
    if (errCode != E_OK) {
        LOGE("Parse schema string from mata table failed.");
        return errCode;
    }

    std::lock_guard<std::mutex> lock(schemaMutex_);
    properties_.SetSchema(schema);
    return E_OK;
}

int SQLiteRelationalStore::SaveSchemaToMeta()
{
    const Key schemaKey(RELATIONAL_SCHEMA_KEY, RELATIONAL_SCHEMA_KEY + strlen(RELATIONAL_SCHEMA_KEY));
    Value schemaVal;
    DBCommon::StringToVector(properties_.GetSchema().ToSchemaString(), schemaVal);
    int errCode = storageEngine_->PutMetaData(schemaKey, schemaVal);
    if (errCode != E_OK) {
        LOGE("Save relational schema to meta table failed. %d", errCode);
    }
    return errCode;
}

int SQLiteRelationalStore::SaveLogTableVersionToMeta()
{
    LOGD("save log table version to meta table, key: %s, val: %s", LOG_TABLE_VERSION_KEY, LOG_TABLE_VERSION_1);
    return E_OK;
}

int SQLiteRelationalStore::CleanDistributedDeviceTable()
{
    return E_OK;
}

int SQLiteRelationalStore::Open(const RelationalDBProperties &properties)
{
    std::lock_guard<std::mutex> lock(initalMutex_);
    if (isInitialized_) {
        LOGD("[RelationalStore][Open] relational db was already inited.");
        return E_OK;
    }

    sqliteStorageEngine_ = new (std::nothrow) SQLiteSingleRelationalStorageEngine();
    if (sqliteStorageEngine_ == nullptr) {
        LOGE("[RelationalStore][Open] Create storage engine failed");
        return -E_OUT_OF_MEMORY;
    }

    int errCode = E_OK;
    do {
        errCode = InitStorageEngine(properties);
        if (errCode != E_OK) {
            LOGE("[RelationalStore][Open] Init database context fail! errCode = [%d]", errCode);
            break;
        }

        storageEngine_ = new (std::nothrow) RelationalSyncAbleStorage(sqliteStorageEngine_);
        if (storageEngine_ == nullptr) {
            LOGE("[RelationalStore][Open] Create syncable storage failed");
            errCode = -E_OUT_OF_MEMORY;
            break;
        }

        errCode = CheckDBMode();
        if (errCode != E_OK) {
            break;
        }

        properties_ = properties;
        errCode = GetSchemaFromMeta();
        if (errCode != E_OK) {
            break;
        }

        errCode = SaveLogTableVersionToMeta();
        if (errCode != E_OK) {
            break;
        }

        errCode = CleanDistributedDeviceTable();
        if (errCode != E_OK) {
            break;
        }

        syncEngine_ = std::make_unique<SyncAbleEngine>(storageEngine_);
        isInitialized_ = true;
        return E_OK;
    } while (false);

    ReleaseResources();
    return errCode;
}

void SQLiteRelationalStore::OnClose(const std::function<void(void)> &notifier)
{
    AutoLock lockGuard(this);
    if (notifier) {
        closeNotifiers_.push_back(notifier);
    } else {
        LOGW("Register 'Close()' notifier failed, notifier is null.");
    }
}

SQLiteSingleVerRelationalStorageExecutor *SQLiteRelationalStore::GetHandle(bool isWrite, int &errCode) const
{
    if (sqliteStorageEngine_ == nullptr) {
        errCode = -E_INVALID_DB;
        return nullptr;
    }

    return static_cast<SQLiteSingleVerRelationalStorageExecutor *>(sqliteStorageEngine_->FindExecutor(isWrite,
        OperatePerm::NORMAL_PERM, errCode));
}
void SQLiteRelationalStore::ReleaseHandle(SQLiteSingleVerRelationalStorageExecutor *&handle) const
{
    if (handle == nullptr) {
        return;
    }

    if (sqliteStorageEngine_ != nullptr) {
        StorageExecutor *databaseHandle = handle;
        sqliteStorageEngine_->Recycle(databaseHandle);
        handle = nullptr;
    }
}

int SQLiteRelationalStore::Sync(const ISyncer::SyncParma &syncParam)
{
    return syncEngine_->Sync(syncParam);
}

// Called when a connection released.
void SQLiteRelationalStore::DecreaseConnectionCounter()
{
    int count = connectionCount_.fetch_sub(1, std::memory_order_seq_cst);
    if (count <= 0) {
        LOGF("Decrease db connection counter failed, count <= 0.");
        return;
    }
    if (count != 1) {
        return;
    }

    LockObj();
    auto notifiers = std::move(closeNotifiers_);
    UnlockObj();

    for (auto &notifier : notifiers) {
        if (notifier) {
            notifier();
        }
    }

    // Sync Close
    syncEngine_->Close();

    if (sqliteStorageEngine_ != nullptr) {
        delete sqliteStorageEngine_;
        sqliteStorageEngine_ = nullptr;
    }
    // close will dec sync ref of storageEngine_
    DecObjRef(storageEngine_);
}

void SQLiteRelationalStore::ReleaseDBConnection(RelationalStoreConnection *connection)
{
    if (connectionCount_.load() == 1) {
        sqliteStorageEngine_->SetConnectionFlag(false);
    }

    connectMutex_.lock();
    if (connection != nullptr) {
        KillAndDecObjRef(connection);
        DecreaseConnectionCounter();
        connectMutex_.unlock();
        KillAndDecObjRef(this);
    } else {
        connectMutex_.unlock();
    }
}

void SQLiteRelationalStore::WakeUpSyncer()
{
    syncEngine_->WakeUpSyncer();
}


int SQLiteRelationalStore::CreateDistributedTable(const std::string &tableName)
{
    int errCode = E_OK;
    std::lock_guard<std::mutex> lock(schemaMutex_);
    auto schema = properties_.GetSchema();
    if (schema.GetTable(tableName).GetTableName() == tableName) {
        LOGW("distributed table was already created.");
        return E_OK;
    }

    if (schema.GetTables().size() >= DBConstant::MAX_DISTRIBUTED_TABLE_COUNT) {
        LOGW("The number of distributed tables is exceeds limit.");
        return -E_MAX_LIMITS;
    }

    LOGD("Create distributed table.");
    auto *handle = GetHandle(true, errCode);
    if (handle == nullptr) {
        return errCode;
    }

    errCode = handle->StartTransaction(TransactType::IMMEDIATE);
    if (errCode != E_OK) {
        ReleaseHandle(handle);
        return errCode;
    }

    TableInfo table;
    errCode = handle->CreateDistributedTable(tableName, table);
    if (errCode != E_OK) {
        LOGE("create distributed table failed. %d", errCode);
        (void)handle->Rollback();
        ReleaseHandle(handle);
        return errCode;
    }
    schema.AddRelationalTable(table);
    properties_.SetSchema(schema);
    (void)handle->Commit();

    ReleaseHandle(handle);
    return SaveSchemaToMeta();
}
}
#endif