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
#include "sqlite_single_ver_relational_storage_executor.h"
#include "data_transformer.h"
#include "db_common.h"

namespace DistributedDB {
SQLiteSingleVerRelationalStorageExecutor::SQLiteSingleVerRelationalStorageExecutor(sqlite3 *dbHandle, bool writable)
    : SQLiteStorageExecutor(dbHandle, writable, false)
{}

int SQLiteSingleVerRelationalStorageExecutor::CreateDistributedTable(const std::string &tableName, TableInfo &table)
{
    if (dbHandle_ == nullptr) {
        LOGE("[CreateDistributedTable] Begin transaction failed, dbHandle is null.");
        return -E_INVALID_DB;
    }

    int historyDataCnt = 0;
    int errCode = SQLiteUtils::GetTableRecordCount(dbHandle_, tableName, historyDataCnt);
    if (errCode != E_OK) {
        LOGE("[CreateDistributedTable] Get the number of table [%s] rows failed. %d", tableName.c_str(), errCode);
        return errCode;
    }

    if (historyDataCnt > 0) { // 0 : create distributed table should on an empty table
        LOGE("[CreateDistributedTable] Create distributed table should on an empty table.");
        return -E_NOT_SUPPORT;
    }

    errCode = SQLiteUtils::AnalysisSchema(dbHandle_, tableName, table);
    if (errCode != E_OK) {
        LOGE("[CreateDistributedTable] analysis table schema failed");
        return errCode;
    }
    // create log table
    errCode = SQLiteUtils::CreateRelationalLogTable(dbHandle_, tableName);
    if (errCode != E_OK) {
        LOGE("[CreateDistributedTable] create log table failed");
        return errCode;
    }

    // add trigger
    errCode = SQLiteUtils::AddRelationalLogTableTrigger(dbHandle_, table);
    if (errCode != E_OK) {
        LOGE("[CreateDistributedTable] create log table failed");
        return errCode;
    }
    return E_OK;
}

int SQLiteSingleVerRelationalStorageExecutor::StartTransaction(TransactType type)
{
    if (dbHandle_ == nullptr) {
        LOGE("Begin transaction failed, dbHandle is null.");
        return -E_INVALID_DB;
    }
    int errCode = SQLiteUtils::BeginTransaction(dbHandle_, type);
    if (errCode != E_OK) {
        LOGE("Begin transaction failed, errCode = %d", errCode);
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::Commit()
{
    if (dbHandle_ == nullptr) {
        return -E_INVALID_DB;
    }

    return SQLiteUtils::CommitTransaction(dbHandle_);
}

int SQLiteSingleVerRelationalStorageExecutor::Rollback()
{
    if (dbHandle_ == nullptr) {
        return -E_INVALID_DB;
    }
    int errCode = SQLiteUtils::RollbackTransaction(dbHandle_);
    if (errCode != E_OK) {
        LOGE("sqlite single ver storage executor rollback fail! errCode = [%d]", errCode);
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::SetTableInfo(const QueryObject &query)
{

    int errCode = SQLiteUtils::AnalysisSchema(dbHandle_, query.GetTableName(), table_);
    if (errCode != E_OK) {
        LOGE("[CreateDistributedTable] analysis table schema failed");
    }
    return errCode;
}

static void GetDataValueByType(sqlite3_stmt *statement, DataValue &value, StorageType type, int cid)
{
    switch (type) {
        case StorageType::STORAGE_TYPE_BOOL: {
            value = static_cast<bool>(sqlite3_column_int64(statement, cid));
            break;
        }

        case StorageType::STORAGE_TYPE_INTEGER: {
            int64_t v = static_cast<int64_t>(sqlite3_column_int64(statement, cid));
            value = v;
            break;
        }

        case StorageType::STORAGE_TYPE_REAL: {
            value = sqlite3_column_double(statement, cid);
            break;
        }

        case StorageType::STORAGE_TYPE_TEXT: {
            const char *colValue = reinterpret_cast<const char*>(sqlite3_column_text(statement, cid));
            if (colValue == nullptr) {
                value = std::string();
            } else {
                value = std::string(colValue);
            }
            break;
        }

        case StorageType::STORAGE_TYPE_BLOB: {
            std::vector<uint8_t> blobValue;
            (void)SQLiteUtils::GetColumnBlobValue(statement, cid, blobValue);
            Blob blob;
            blob.WriteBlob(blobValue.data(), static_cast<uint32_t>(blobValue.size()));
            value = blob;
            break;
        }

        default:
            return;
    }
}

static void BindDataValueByType(sqlite3_stmt *statement,
    const std::optional<DataValue> &value, StorageType type, int cid)
{
    if (value == std::nullopt) {
        sqlite3_bind_null(statement, cid);
        return;
    }

    switch (type) {
        case StorageType::STORAGE_TYPE_BOOL: {
            bool boolData = false;
            value.value().GetBool(boolData);
            sqlite3_bind_int(statement, cid, boolData);
            break;
        }

        case StorageType::STORAGE_TYPE_INTEGER: {
            int64_t intData = 0;
            value.value().GetInt64(intData);
            sqlite3_bind_int64(statement, cid, intData);
            break;
        }

        case StorageType::STORAGE_TYPE_REAL: {
            double doubleData = 0;
            value.value().GetDouble(doubleData);
            sqlite3_bind_double(statement, cid, doubleData);
            break;
        }

        case StorageType::STORAGE_TYPE_TEXT: {
            std::string strData;
            value.value().GetText(strData);
            SQLiteUtils::BindTextToStatement(statement, cid, strData);
            break;
        }

        case StorageType::STORAGE_TYPE_BLOB: {
            Blob blob;
            value.value().GetBlob(blob);
            std::vector<uint8_t> blobData(blob.GetData(), blob.GetData() + blob.GetSize());
            SQLiteUtils::BindBlobToStatement(statement, cid, blobData, true);
            break;
        }

        default:
            return;
    }
}

static int GetLogData(sqlite3_stmt *logStatement, LogInfo &logInfo)
{
    logInfo.dataKey = sqlite3_column_int(logStatement, 0);

    std::vector<uint8_t> dev;
    int errCode = SQLiteUtils::GetColumnBlobValue(logStatement, 1, dev);
    if (errCode != E_OK) {
        return errCode;
    }
    logInfo.device = std::string(dev.begin(), dev.end());

    std::vector<uint8_t> oriDev;
    errCode = SQLiteUtils::GetColumnBlobValue(logStatement, 2, oriDev);
    if (errCode != E_OK) {
        return errCode;
    }
    logInfo.originDev = std::string(oriDev.begin(), oriDev.end());
    logInfo.timestamp = static_cast<uint64_t>(sqlite3_column_int64(logStatement, 3));
    logInfo.wTimeStamp = static_cast<uint64_t>(sqlite3_column_int64(logStatement, 4));
    logInfo.flag = static_cast<uint64_t>(sqlite3_column_int64(logStatement, 5));
    logInfo.flag &= (~DataItem::LOCAL_FLAG);

    std::vector<uint8_t> hashKey;
    errCode = SQLiteUtils::GetColumnBlobValue(logStatement, 6, hashKey);
    if (errCode != E_OK) {
        return errCode;
    }

    logInfo.hashKey = std::string(hashKey.begin(), hashKey.end());
    return errCode;
}

static size_t GetDataItemSerialSize(DataItem &item, size_t appendLen)
{
    // timestamp and local flag: 3 * uint64_t, version(uint32_t), key, value, origin dev and the padding size.
    // the size would not be very large.
    static const size_t maxOrigDevLength = 40;
    size_t devLength = std::max(maxOrigDevLength, item.origDev.size());
    size_t dataSize = (Parcel::GetUInt64Len() * 3 + Parcel::GetUInt32Len() + Parcel::GetVectorCharLen(item.key) +
                        Parcel::GetVectorCharLen(item.value) + devLength + appendLen);
    return dataSize;
}

int SQLiteSingleVerRelationalStorageExecutor::GetKvData(const Key &key, Value &value) const
{
    static const std::string SELECT_META_VALUE_SQL = "SELECT value FROM " + DBConstant::RELATIONAL_PREFIX +
        "metadata WHERE key=?;";
    sqlite3_stmt *statement = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, SELECT_META_VALUE_SQL, statement);
    if (errCode != E_OK) {
        goto END;
    }

    errCode = SQLiteUtils::BindBlobToStatement(statement, 1, key, false); // first arg.
    if (errCode != E_OK) {
        goto END;
    }

    errCode = SQLiteUtils::StepWithRetry(statement, isMemDb_);
    if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
        errCode = -E_NOT_FOUND;
        goto END;
    } else if (errCode != SQLiteUtils::MapSQLiteErrno(SQLITE_ROW)) {
        goto END;
    }

    errCode = SQLiteUtils::GetColumnBlobValue(statement, 0, value); // only one result.
    END:
    SQLiteUtils::ResetStatement(statement, true, errCode);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::PutKvData(const Key &key, const Value &value) const
{
    static const std::string INSERT_META_SQL = "INSERT OR REPLACE INTO " + DBConstant::RELATIONAL_PREFIX +
        "metadata VALUES(?,?);";
    sqlite3_stmt *statement = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, INSERT_META_SQL, statement);
    if (errCode != E_OK) {
        goto ERROR;
    }

    errCode = SQLiteUtils::BindBlobToStatement(statement, 1, key, false); // BIND_KV_KEY_INDEX
    if (errCode != E_OK) {
        LOGE("[SingleVerExe][BindPutKv]Bind key error:%d", errCode);
        goto ERROR;
    }

    errCode = SQLiteUtils::BindBlobToStatement(statement, 2, value, true); // BIND_KV_VAL_INDEX
    if (errCode != E_OK) {
        LOGE("[SingleVerExe][BindPutKv]Bind value error:%d", errCode);
        goto ERROR;
    }
    errCode = SQLiteUtils::StepWithRetry(statement, isMemDb_);
    if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
        errCode = E_OK;
    }
ERROR:
    SQLiteUtils::ResetStatement(statement, true, errCode);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::DeleteMetaData(const std::vector<Key> &keys) const
{
    static const std::string REMOVE_META_VALUE_SQL = "DELETE FROM " + DBConstant::RELATIONAL_PREFIX +
        "metadata WHERE key=?;";
    sqlite3_stmt *statement = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, REMOVE_META_VALUE_SQL, statement);
    if (errCode != E_OK) {
        return errCode;
    }

    for (const auto &key : keys) {
        errCode = SQLiteUtils::BindBlobToStatement(statement, 1, key, false); // first arg.
        if (errCode != E_OK) {
            break;
        }

        errCode = SQLiteUtils::StepWithRetry(statement, isMemDb_);
        if (errCode != SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
            break;
        }
        errCode = E_OK;
        SQLiteUtils::ResetStatement(statement, false, errCode);
    }
    SQLiteUtils::ResetStatement(statement, true, errCode);
    return CheckCorruptedStatus(errCode);
}

int SQLiteSingleVerRelationalStorageExecutor::DeleteMetaDataByPrefixKey(const Key &keyPrefix) const
{
    static const std::string REMOVE_META_VALUE_BY_KEY_PREFIX_SQL = "DELETE FROM " + DBConstant::RELATIONAL_PREFIX +
        "metadata WHERE key>=? AND key<=?;";
    sqlite3_stmt *statement = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, REMOVE_META_VALUE_BY_KEY_PREFIX_SQL, statement);
    if (errCode != E_OK) {
        return errCode;
    }

    errCode = SQLiteUtils::BindPrefixKey(statement, 1, keyPrefix); // 1 is first arg.
    if (errCode == E_OK) {
        errCode = SQLiteUtils::StepWithRetry(statement, isMemDb_);
        if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
            errCode = E_OK;
        }
    }
    SQLiteUtils::ResetStatement(statement, true, errCode);
    return CheckCorruptedStatus(errCode);
}

static int GetAllKeys(sqlite3_stmt *statement, std::vector<Key> &keys)
{
    if (statement == nullptr) {
        return -E_INVALID_DB;
    }
    int errCode;
    do {
        errCode = SQLiteUtils::StepWithRetry(statement, false);
        if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_ROW)) {
            Key key;
            errCode = SQLiteUtils::GetColumnBlobValue(statement, 0, key);
            if (errCode != E_OK) {
                break;
            }

            keys.push_back(std::move(key));
        } else if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
            errCode = E_OK;
            break;
        } else {
            LOGE("SQLite step for getting all keys failed:%d", errCode);
            break;
        }
    } while (true);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::GetAllMetaKeys(std::vector<Key> &keys) const
{
    static const std::string SELECT_ALL_META_KEYS = "SELECT key FROM " + DBConstant::RELATIONAL_PREFIX + "metadata;";
    sqlite3_stmt *statement = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, SELECT_ALL_META_KEYS, statement);
    if (errCode != E_OK) {
        LOGE("[Relational][GetAllKey] Get statement failed:%d", errCode);
        return errCode;
    }
    errCode = GetAllKeys(statement, keys);
    SQLiteUtils::ResetStatement(statement, true, errCode);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::PrepareForSavingLog(const QueryObject &object,
    const std::string &deviceName, sqlite3_stmt *&logStmt) const
{
    std::string devName = DBCommon::TransferHashString(deviceName);
    const std::string tableName = DBConstant::RELATIONAL_PREFIX + object.GetTableName() + "_log";
    std::string dataFormat = "?, '" + deviceName + "', ?, ?, ?, ?, ?";

    std::string sql = "INSERT OR REPLACE INTO " + tableName +
        " (data_key, device, ori_device, timestamp, wtimestamp, flag, hash_key) VALUES (" + dataFormat + ");";
    int errCode = SQLiteUtils::GetStatement(dbHandle_, sql, logStmt);
    if (errCode != E_OK) {
        LOGE("[info statement] Get statement fail!");
        return -E_INVALID_QUERY_FORMAT;
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::PrepareForSavingData(const QueryObject &object,
    const std::string &deviceName, sqlite3_stmt *&statement) const
{
    // naturalbase_rdb_aux_userTableName_deviceHash
    // tableName
    std::string devName = DBCommon::TransferHashString(deviceName);
    const std::string tableName = DBConstant::RELATIONAL_PREFIX + object.GetTableName() + "_" +
        DBCommon::TransferStringToHex(devName);
    TableInfo table;
    int errCode = SQLiteUtils::AnalysisSchema(dbHandle_, tableName, table);
    if (errCode == -E_NOT_FOUND) {
        errCode = SQLiteUtils::CreateSameStuTable(dbHandle_, object.GetTableName(), tableName, false);
    }

    if (errCode != E_OK) {
        LOGE("[PrepareForSavingData] analysis table schema failed");
        return errCode;
    }

    std::string colName;
    std::string dataFormat;
    const std::map<std::string, FieldInfo> fields = table_.GetFields();
    for (const auto &field : fields) {
        colName += field.first + ",";
        dataFormat += "?,";
    }
    colName.pop_back();
    dataFormat.pop_back();

    std::string sql = "INSERT OR REPLACE INTO " + tableName + " (" + colName + ") VALUES (" + dataFormat + ");";
    errCode = SQLiteUtils::GetStatement(dbHandle_, sql, statement);
    if (errCode != E_OK) {
        LOGE("[info statement] Get statement fail!");
        errCode = -E_INVALID_QUERY_FORMAT;
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::SaveSyncLog(sqlite3_stmt *statement, const DataItem &dataItem,
    TimeStamp &maxTimestamp)
{
    Key hashKey;
    (void)DBCommon::CalcValueHash(dataItem.key, hashKey);
    std::string hash = std::string(hashKey.begin(), hashKey.end());
    std::string sql = "select * from " + DBConstant::RELATIONAL_PREFIX + table_.GetTableName() +
        "_log where hash_key = ?;";
    sqlite3_stmt *queryStmt = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, sql, queryStmt);
    if (errCode != E_OK) {
        LOGE("[info statement] Get statement fail!");
        return -E_INVALID_QUERY_FORMAT;
    }
    SQLiteUtils::BindTextToStatement(queryStmt, 1, hash);

    LogInfo logInfoGet;
    errCode = SQLiteUtils::StepWithRetry(queryStmt, isMemDb_);
    if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_ROW)) {
        errCode = -E_NOT_FOUND;
    } else {
        errCode = GetLogData(queryStmt, logInfoGet);
    }
    SQLiteUtils::ResetStatement(queryStmt, true, errCode);

    LogInfo logInfoBind;
    std::string key = std::string(dataItem.key.begin(), dataItem.key.end());
    logInfoBind.hashKey = hash;
    logInfoBind.device = dataItem.dev;
    logInfoBind.timestamp = dataItem.timeStamp;
    int dataKeyBind = -1;
    logInfoBind.flag = dataItem.flag;
    logInfoBind.wTimeStamp = maxTimestamp;

    if (errCode == -E_NOT_FOUND) { // insert
        logInfoBind.originDev = dataItem.dev;
    } else if (errCode == E_OK) { // update
        logInfoBind.wTimeStamp = logInfoGet.wTimeStamp;
        logInfoBind.originDev = logInfoGet.originDev;
    } else {
        return errCode;
    }

    // bind
    SQLiteUtils::BindInt64ToStatement(statement, 1, dataKeyBind);

    std::vector<uint8_t> originDev(logInfoBind.originDev.begin(), logInfoBind.originDev.end());
    SQLiteUtils::BindBlobToStatement(statement, 2, originDev);

    SQLiteUtils::BindInt64ToStatement(statement, 3, logInfoBind.timestamp);
    SQLiteUtils::BindInt64ToStatement(statement, 4, logInfoBind.wTimeStamp);
    SQLiteUtils::BindInt64ToStatement(statement, 5, logInfoBind.flag);

    SQLiteUtils::BindTextToStatement(statement, 6, logInfoBind.hashKey);

    errCode = SQLiteUtils::StepWithRetry(statement, isMemDb_);
    if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
        return E_OK;
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::DeleteSyncDataItem(const DataItem &dataItem)
{
    std::string devName = DBCommon::TransferHashString(dataItem.dev);
    const std::string tableName = DBConstant::RELATIONAL_PREFIX + table_.GetTableName() + "_" +
        DBCommon::TransferStringToHex(devName);
    std::string hashKey = std::string(dataItem.hashKey.begin(), dataItem.hashKey.end());
    std::string sql = "DELETE FROM " + tableName + " WHERE calc_hash(" + table_.GetPrimaryKey() + ")=" + hashKey + ";";
    sqlite3_stmt *stmt = nullptr;
    int errCode = SQLiteUtils::GetStatement(dbHandle_, sql, stmt);
    if (errCode != E_OK) {
        LOGE("[info statement] Get statement fail!");
        return -E_INVALID_QUERY_FORMAT;
    }

    errCode = SQLiteUtils::StepWithRetry(stmt, isMemDb_);
    if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
        errCode = E_OK;
    }
    SQLiteUtils::ResetStatement(stmt, true, errCode);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::SaveSyncDataItem(sqlite3_stmt *statement, const DataItem &dataItem)
{
    if ((dataItem.flag & DataItem::DELETE_FLAG) != 0) {
        return DeleteSyncDataItem(dataItem);
    }

    std::map<std::string, FieldInfo> colInfos = table_.GetFields();
    std::vector<FieldInfo> fieldInfos;
    for (const auto &col: colInfos) {
        fieldInfos.push_back(col.second);
    }

    std::vector<int> indexMapping;
    DataTransformer::ReduceMapping(fieldInfos, fieldInfos, indexMapping);

    OptRowDataWithLog data;
    int errCode = DataTransformer::DeSerializeDataItem(dataItem, data, fieldInfos, indexMapping);
    if (errCode != E_OK) {
        LOGE("[RelationalStorageExecutor] DeSerialize dataItem failed! errCode = [%d]", errCode);
        return errCode;
    }

    for (size_t index = 0; index < data.optionalData.size(); index++) {
        const auto &filedData = data.optionalData[index];
        if (filedData.has_value()) {
            (void)BindDataValueByType(statement, filedData,
                filedData.value().GetType(), fieldInfos[index].GetColumnId() + 1);
        } else {
            (void)BindDataValueByType(statement, filedData,
                StorageType::STORAGE_TYPE_NULL, fieldInfos[index].GetColumnId() + 1);
        }
    }

    errCode = SQLiteUtils::StepWithRetry(statement, isMemDb_);
    if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
        errCode = E_OK;
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::SaveSyncDataItems(const QueryObject &object,
    std::vector<DataItem> &dataItems, const std::string &deviceName, TimeStamp &maxTimestamp)
{
    sqlite3_stmt *statement = nullptr;
    int errCode = PrepareForSavingData(object, deviceName, statement);
    if (errCode != E_OK) {
        LOGE("[RelationalStorage] Get statement fail!");
        return errCode;
    }

    sqlite3_stmt *logStmt = nullptr;
    errCode = PrepareForSavingLog(object, deviceName, logStmt);
    if (errCode != E_OK) {
        LOGE("[RelationalStorage] Get statement fail!");
        return errCode;
    }

    for (auto &item : dataItems) {
        if (item.neglect) { // Do not save this record if it is neglected
            continue;
        }
        errCode = SaveSyncDataItem(statement, item);
        if (errCode != E_OK && errCode != -E_NOT_FOUND) {
            break;
        }

        item.dev = deviceName;
        errCode = SaveSyncLog(logStmt, item, maxTimestamp);
        if (errCode != E_OK) {
            break;
        }
        maxTimestamp = std::max(item.timeStamp, maxTimestamp);
        SQLiteUtils::ResetStatement(statement, false, errCode);
        SQLiteUtils::ResetStatement(logStmt, false, errCode);
    }

    if (errCode == -E_NOT_FOUND) {
        errCode = E_OK;
    }
    SQLiteUtils::ResetStatement(logStmt, true, errCode);
    SQLiteUtils::ResetStatement(statement, true, errCode);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::SaveSyncItems(const QueryObject &object, std::vector<DataItem> &dataItems,
    const std::string &deviceName, TimeStamp &timeStamp)
{
    int errCode = StartTransaction(TransactType::IMMEDIATE);
    if (errCode != E_OK) {
        return errCode;
    }

    errCode = SaveSyncDataItems(object, dataItems, deviceName, timeStamp);
    if (errCode == E_OK) {
        errCode = Commit();
    } else {
        (void)Rollback(); // Keep the error code of the first scene
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::GetDataItemForSync(sqlite3_stmt *stmt, DataItem &dataItem,
    bool isGettingDeletedData) const
{
    RowDataWithLog data;
    int errCode = GetLogData(stmt, data.logInfo);
    if (errCode != E_OK) {
        LOGE("relational data value transfer to kv fail");
        return errCode;
    }

    std::vector<FieldInfo> fieldInfos;
    if (!isGettingDeletedData) {
        for (const auto &col: table_.GetFields()) {
            LOGD("[GetDataItemForSync] field:%s type:%d cid:%d", col.second.GetFieldName().c_str(),
                col.second.GetStorageType(), col.second.GetColumnId() + 7);
            DataValue value;
            GetDataValueByType(stmt, value, col.second.GetStorageType(), col.second.GetColumnId() + 7);
            fieldInfos.push_back(col.second);
            data.rowData.push_back(value);
        }
    }

    errCode = DataTransformer::SerializeDataItem(data, fieldInfos, dataItem);
    if (errCode != E_OK) {
        LOGE("relational data value transfer to kv fail");
    }
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::GetSyncDataByQuery(std::vector<DataItem> &dataItems, size_t appendLength,
    const DataSizeSpecInfo &dataSizeInfo, std::function<int(sqlite3 *, sqlite3_stmt *&, bool &)> getStmt)
{
    sqlite3_stmt *stmt = nullptr;
    bool isGettingDeletedData = false;
    int errCode = getStmt(dbHandle_, stmt, isGettingDeletedData);
    if (errCode != E_OK) {
        return errCode;
    }

    size_t dataTotalSize = 0;
    do {
        DataItem item;
        errCode = SQLiteUtils::StepWithRetry(stmt, isMemDb_);
        if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_ROW)) {
            errCode = GetDataItemForSync(stmt, item, isGettingDeletedData);
        } else if (errCode == SQLiteUtils::MapSQLiteErrno(SQLITE_DONE)) {
            LOGD("Get sync data finished, size of packet:%zu, number of item:%zu", dataTotalSize, dataItems.size());
            errCode = -E_FINISHED;
            break;
        } else {
            LOGE("Get sync data error:%d", errCode);
            break;
        }

        // If dataTotalSize value is bigger than blockSize value , reserve the surplus data item.
        dataTotalSize += GetDataItemSerialSize(item, appendLength);
        if ((dataTotalSize > dataSizeInfo.blockSize && !dataItems.empty()) ||
            dataItems.size() >= dataSizeInfo.packetSize) {
            errCode = -E_UNFINISHED;
            break;
        } else {
            dataItems.push_back(std::move(item));
        }
    } while (true);

    SQLiteUtils::ResetStatement(stmt, true, errCode);
    return errCode;
}

int SQLiteSingleVerRelationalStorageExecutor::CheckDBModeForRelational()
{
    std::string journalMode;
    int errCode = SQLiteUtils::GetJournalMode(dbHandle_, journalMode);
    if (errCode != E_OK || journalMode != "wal") {
        LOGE("Not support journal mode %s for relational db, expect wal mode, %d", journalMode.c_str(), errCode);
        return -E_NOT_SUPPORT;
    }

    int synchronousMode;
    errCode = SQLiteUtils::GetSynchronousMode(dbHandle_, synchronousMode);
    if (errCode != E_OK || synchronousMode != 2) { // 2: FULL mode
        LOGE("Not support synchronous mode %d for relational db, expect FULL mode, %d", synchronousMode, errCode);
        return -E_NOT_SUPPORT;
    }

    return E_OK;
}
} // namespace DistributedDB
#endif
