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
#ifndef SQLITE_RELATIONAL_STORE_H
#define SQLITE_RELATIONAL_STORE_H

#include <functional>
#include <memory.h>
#include <vector>

#include "irelational_store.h"
#include "sqlite_single_relational_storage_engine.h"
#include "isyncer.h"
#include "sync_able_engine.h"
#include "relational_sync_able_storage.h"

namespace DistributedDB {
class SQLiteRelationalStore : public IRelationalStore {
public:
    SQLiteRelationalStore() = default;
    ~SQLiteRelationalStore() override;

    RelationalStoreConnection *GetDBConnection(int &errCode) override;
    int Open(const DBProperties &properties) override;
    void OnClose(const std::function<void(void)> &notifier);

    SQLiteSingleVerRelationalStorageExecutor *GetHandle(bool isWrite, int &errCode) const;
    void ReleaseHandle(SQLiteSingleVerRelationalStorageExecutor *&handle) const;

    int Sync(const ISyncer::SyncParma &syncParam);

    void ReleaseDBConnection(RelationalStoreConnection *connection);

    void WakeUpSyncer();

    // for test mock
    const RelationalSyncAbleStorage *GetStorageEngine()
    {
        return storageEngine_;
    }
private:
    // 1 store 1 connection
    void DecreaseConnectionCounter();

    // use for sync Interactive
    std::shared_ptr<SyncAbleEngine> syncEngine_ = nullptr; // For storage operate sync function
    // use ref obj same as kv
    RelationalSyncAbleStorage *storageEngine_ = nullptr; // For storage operate data
    SQLiteSingleRelationalStorageEngine *sqliteStorageEngine_ = nullptr;

    void IncreaseConnectionCounter();
    int InitStorageEngine(const DBProperties &kvDBProp);
    std::mutex connectMutex_;
    std::atomic<int> connectionCount_ = 0;
    std::vector<std::function<void(void)>> closeNotifiers_;
};
}  // namespace DistributedDB
#endif // SQLITE_RELATIONAL_STORE_H
#endif