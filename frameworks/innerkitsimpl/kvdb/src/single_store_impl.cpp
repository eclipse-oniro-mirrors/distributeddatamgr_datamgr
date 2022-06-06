/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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
#define LOG_TAG "SingleStoreImpl"
#include "single_store_impl.h"

#include "dds_trace.h"
#include "dev_manager.h"
#include "log_print.h"
#include "store_result_set.h"
#include "store_util.h"

namespace OHOS::DistributedKv {
SingleStoreImpl::SingleStoreImpl(std::shared_ptr<DBStore> dbStore) : dbStore_(std::move(dbStore))
{
    syncObserver_ = std::make_shared<SyncObserver>();
}

StoreId SingleStoreImpl::GetStoreId() const
{
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return { storeId_ };
    }
    return { dbStore_->GetStoreId() };
}

Status SingleStoreImpl::Put(const Key &key, const Value &value)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    DistributedDB::Key dbKey = ConvertDBKey(key);
    if (dbKey.empty()) {
        ZLOGE("invalid key:%{public}s, size:%{public}zu", StoreUtil::Anonymous(key.ToString()).c_str(), key.Size());
        return INVALID_ARGUMENT;
    }
    auto dbStatus = dbStore_->Put(dbKey, value);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, key:%{public}s, value size:%{public}zu", status,
            StoreUtil::Anonymous(key.ToString()).c_str(), value.Size());
    }
    // do auto sync process
    return status;
}

Status SingleStoreImpl::PutBatch(const std::vector<Entry> &entries)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    std::vector<DistributedDB::Entry> dbEntries;
    DistributedDB::Entry dbEntry;
    for (const auto &entry : entries) {
        dbEntry.key = ConvertDBKey(entry.key);
        if (dbEntry.key.empty()) {
            ZLOGE("invalid key:%{public}s, size:%{public}zu", StoreUtil::Anonymous(entry.key.ToString()).c_str(),
                entry.key.Size());
            return INVALID_ARGUMENT;
        }
        dbEntry.value = entry.value;
        dbEntries.push_back(std::move(dbEntry));
    }
    auto dbStatus = dbStore_->PutBatch(dbEntries);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, entries size:%{public}zu", status, entries.size());
    }
    // do auto sync process
    return status;
}

Status SingleStoreImpl::Delete(const Key &key)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    DistributedDB::Key dbKey = ConvertDBKey(key);
    if (dbKey.empty()) {
        ZLOGE("invalid key:%{public}s, size:%{public}zu", StoreUtil::Anonymous(key.ToString()).c_str(), key.Size());
        return INVALID_ARGUMENT;
    }
    auto dbStatus = dbStore_->Delete(dbKey);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, key:%{public}s", status, StoreUtil::Anonymous(key.ToString()).c_str());
    }
    return status;
}

Status SingleStoreImpl::DeleteBatch(const std::vector<Key> &keys)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    std::vector<DistributedDB::Key> dbKeys;
    for (const auto &key : keys) {
        DistributedDB::Key dbKey = ConvertDBKey(key);
        if (dbKey.empty()) {
            ZLOGE("invalid key:%{public}s, size:%{public}zu", StoreUtil::Anonymous(key.ToString()).c_str(), key.Size());
            return INVALID_ARGUMENT;
        }
        dbKeys.push_back(std::move(dbKey));
    }
    auto dbStatus = dbStore_->DeleteBatch(dbKeys);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, keys size:%{public}zu", status, keys.size());
    }
    // do auto sync process
    return status;
}

Status SingleStoreImpl::StartTransaction()
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    auto dbStatus = dbStore_->StartTransaction();
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d", status);
    }
    return status;
}

Status SingleStoreImpl::Commit()
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    auto dbStatus = dbStore_->Commit();
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d", status);
    }
    return status;
}

Status SingleStoreImpl::Rollback()
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    auto dbStatus = dbStore_->Rollback();
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d", status);
    }
    return status;
}

Status SingleStoreImpl::SubscribeKvStore(SubscribeType type, std::shared_ptr<Observer> observer)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    Status status = SUCCESS;
    std::shared_ptr<ObserverBridge> bridge = nullptr;
    auto release = BridgeReleaser(type);
    observers_.Compute(uintptr_t(observer.get()),
        [type, observer, &bridge, &release](const auto &, std::map<int32_t, std::shared_ptr<ObserverBridge>> &bridges) {
            if (bridges.find(int32_t(type)) != bridges.end()) {
                return true;
            }
            bridge = std::shared_ptr<ObserverBridge>(new ObserverBridge(observer), release);
            bridges.emplace(int32_t(type), bridge);
            return true;
        });

    if (type == SubscribeType::SUBSCRIBE_TYPE_LOCAL || type == SubscribeType::SUBSCRIBE_TYPE_ALL) {
        auto dbStatus = dbStore_->RegisterObserver({}, ConvertMode(type), bridge.get());
        status = StoreUtil::ConvertStatus(dbStatus);
    }

    if (type == SubscribeType::SUBSCRIBE_TYPE_REMOTE || type == SubscribeType::SUBSCRIBE_TYPE_ALL) {
        // status = proxy_->RegisterObserver({}, ConvertMode(type), bridge);
    }

    if (status != SUCCESS) {
        ZLOGE("failed! type:%{public}d, status:%{public}d, observer:0x%x", type, status,
            StoreUtil::Anonymous(bridge.get()));
    }
    return status;
}

Status SingleStoreImpl::UnSubscribeKvStore(SubscribeType type, std::shared_ptr<Observer> observer)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_ptr<ObserverBridge> bridge = nullptr;
    observers_.ComputeIfPresent(uintptr_t(observer.get()),
        [type, observer, &bridge](const auto &, std::map<int32_t, std::shared_ptr<ObserverBridge>> &bridges) {
            auto it = bridges.find(int32_t(type));
            if (it != bridges.end()) {
                bridge = it->second;
                bridges.erase(it);
            }
            return !bridges.empty();
        });
    bridge = nullptr;
    return SUCCESS;
}

Status SingleStoreImpl::Get(const Key &key, Value &value)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    DistributedDB::Key dbKey = ConvertDBKey(key);
    if (dbKey.empty()) {
        ZLOGE("invalid key:%{public}s, size:%{public}zu", key.ToString().c_str(), key.Size());
        return INVALID_ARGUMENT;
    }
    DistributedDB::Value dbValue;
    auto dbStatus = dbStore_->Get(dbKey, dbValue);
    value = std::move(dbValue);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, key:%{public}s", status, key.ToString().c_str());
    }
    return status;
}

Status SingleStoreImpl::GetEntries(const Key &prefix, std::vector<Entry> &entries) const
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    DistributedDB::Key dbPrefix = ConvertDBKey(prefix);
    if (dbPrefix.empty() && !prefix.Empty()) {
        ZLOGE("invalid prefix:%{public}s, size:%{public}zu", prefix.ToString().c_str(), prefix.Size());
        return INVALID_ARGUMENT;
    }
    DistributedDB::Query dbQuery = DistributedDB::Query::Select();
    dbQuery.PrefixKey(dbPrefix);
    auto status = GetEntries(dbQuery, entries);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, prefix:%{public}s", status, prefix.ToString().c_str());
    }
    return status;
}

Status SingleStoreImpl::GetEntries(const DataQuery &query, std::vector<Entry> &entries) const
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    DistributedDB::Query dbQuery = *(query.query_);
    dbQuery.PrefixKey(GetPrefix(query));
    auto status = GetEntries(dbQuery, entries);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, query:%{public}s", status, query.ToString().c_str());
    }
    return status;
}

Status SingleStoreImpl::GetResultSet(const Key &prefix, std::shared_ptr<ResultSet> &resultSet) const
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    DistributedDB::Key dbPrefix = ConvertDBKey(prefix);
    if (dbPrefix.empty() && !prefix.Empty()) {
        ZLOGE("invalid prefix:%{public}s, size:%{public}zu", prefix.ToString().c_str(), prefix.Size());
        return INVALID_ARGUMENT;
    }
    DistributedDB::Query dbQuery = DistributedDB::Query::Select();
    dbQuery.PrefixKey(dbPrefix);
    auto status = GetResultSet(dbQuery, resultSet);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, prefix:%{public}s", status, prefix.ToString().c_str());
    }
    return status;
}

Status SingleStoreImpl::GetResultSet(const DataQuery &query, std::shared_ptr<ResultSet> &resultSet) const
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    DistributedDB::Query dbQuery = *(query.query_);
    dbQuery.PrefixKey(GetPrefix(query));
    auto status = GetResultSet(dbQuery, resultSet);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, query:%{public}s", status, query.ToString().c_str());
    }
    return status;
}

Status SingleStoreImpl::CloseResultSet(std::shared_ptr<ResultSet> &resultSet)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    if (resultSet == nullptr) {
        ZLOGE("input is nullptr");
        return INVALID_ARGUMENT;
    }
    auto status = resultSet->Close();
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d", status);
    }
    resultSet = nullptr;
    return status;
}

Status SingleStoreImpl::GetCount(const DataQuery &query, int &result) const
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s is already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }
    DistributedDB::Query dbQuery = *(query.query_);
    dbQuery.PrefixKey(GetPrefix(query));
    auto dbStatus = dbStore_->GetCount(dbQuery, result);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, query:%{public}s", status, query.ToString().c_str());
    }
    return status;
}

Status SingleStoreImpl::GetSecurityLevel(SecurityLevel &securityLevel) const
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s is already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }
    DistributedDB::SecurityOption option;
    auto dbStatus = dbStore_->GetSecurityOption(option);
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, security:[%{public}d, %{public}d]", status, option.securityFlag,
            option.securityLabel);
    }
    return status;
}

Status SingleStoreImpl::RemoveDeviceData(const std::string &device)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s is already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    auto dbStatus = dbStore_->RemoveDeviceData(DevManager::GetInstance().ToUUID(device));
    auto status = StoreUtil::ConvertStatus(dbStatus);
    if (status != SUCCESS) {
        ZLOGE("failed! status:%{public}d, device:%{public}s", status, StoreUtil::Anonymous(device).c_str());
    }
    return status;
}

Status SingleStoreImpl::Sync(const std::vector<std::string> &devices, SyncMode mode, uint32_t allowedDelayMs)
{
    // do immediately full sync process
    return NOT_SUPPORT;
}

Status SingleStoreImpl::Sync(const std::vector<std::string> &devices, SyncMode mode, const DataQuery &query,
    std::shared_ptr<SyncCallback> syncCallback)
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::RegisterSyncCallback(std::shared_ptr<SyncCallback> callback)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__),
        TraceSwitch::BYTRACE_ON | TraceSwitch::API_PERFORMANCE_TRACE_ON);
    if (callback == nullptr) {
        ZLOGW("return INVALID_ARGUMENT.");
        return INVALID_ARGUMENT;
    }
    syncObserver_->Add(callback);
    return SUCCESS;
}

Status SingleStoreImpl::UnRegisterSyncCallback()
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__),
        TraceSwitch::BYTRACE_ON | TraceSwitch::API_PERFORMANCE_TRACE_ON);
    syncObserver_->Clean();
    return SUCCESS;
}

Status SingleStoreImpl::SetSyncParam(const KvSyncParam &syncParam)
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::GetSyncParam(KvSyncParam &syncParam)
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::SetCapabilityEnabled(bool enabled) const
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::SetCapabilityRange(
    const std::vector<std::string> &localLabels, const std::vector<std::string> &remoteLabels) const
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::SubscribeWithQuery(const std::vector<std::string> &devices, const DataQuery &query)
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::UnsubscribeWithQuery(const std::vector<std::string> &devices, const DataQuery &query)
{
    return NOT_SUPPORT;
}

Status SingleStoreImpl::Close()
{
    observers_.Clear();
    syncObserver_->Clean();
    std::unique_lock<decltype(mutex_)> lock;
    if (dbStore_ != nullptr) {
        storeId_ = dbStore_->GetStoreId();
        dbStore_ = nullptr;
    }
    return SUCCESS;
}

bool SingleStoreImpl::IsValidKey(const Key &key) const
{
    return key.Empty() || key.Size() > MAX_KEY_LENGTH || std::isspace(key[0]) || std::isspace(key[key.Size() - 1]);
}

std::vector<uint8_t> SingleStoreImpl::ConvertDBKey(const Key &key) const
{
    return IsValidKey(key) ? std::vector<uint8_t>(key) : std::vector<uint8_t>();
}

Key SingleStoreImpl::ConvertKey(DistributedDB::Key &&key) const
{
    return std::move(key);
}

int SingleStoreImpl::ConvertMode(SubscribeType type) const
{
    int mode;
    if (type == SubscribeType::SUBSCRIBE_TYPE_LOCAL) {
        mode = DistributedDB::OBSERVER_CHANGES_NATIVE;
    } else if (type == SubscribeType::SUBSCRIBE_TYPE_REMOTE) {
        mode = DistributedDB::OBSERVER_CHANGES_FOREIGN;
    } else {
        mode = DistributedDB::OBSERVER_CHANGES_FOREIGN | DistributedDB::OBSERVER_CHANGES_NATIVE;
    }
    return mode;
}

std::function<void(ObserverBridge *)> SingleStoreImpl::BridgeReleaser(SubscribeType type)
{
    return [this, type](ObserverBridge *obj) {
        Status status = SUCCESS;
        if (obj == nullptr) {
            return;
        }

        if (type == SubscribeType::SUBSCRIBE_TYPE_LOCAL || type == SubscribeType::SUBSCRIBE_TYPE_ALL) {
            std::shared_lock<decltype(mutex_)> lock;
            status = ALREADY_CLOSED;
            if (dbStore_ != nullptr) {
                auto dbStatus = dbStore_->UnRegisterObserver(obj);
                status = StoreUtil::ConvertStatus(dbStatus);
            }
        }
        if (type == SubscribeType::SUBSCRIBE_TYPE_REMOTE || type == SubscribeType::SUBSCRIBE_TYPE_ALL) {
            // status = proxy_->UnregisterObserver({}, ConvertMode(type), bridge);
        }
        if (status != SUCCESS) {
            ZLOGE("failed! type:%{public}d, status:%{public}d, observer:0x%x", type, status, StoreUtil::Anonymous(obj));
        }
        delete obj;
    };
}

Status SingleStoreImpl::GetResultSet(const DistributedDB::Query &query, std::shared_ptr<ResultSet> &resultSet) const
{
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s is already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    DistributedDB::KvStoreResultSet *dbResultSet = nullptr;
    auto status = dbStore_->GetEntries(query, dbResultSet);
    if (dbResultSet == nullptr) {
        return StoreUtil::ConvertStatus(status);
    }
    resultSet = std::make_shared<StoreResultSet>(dbResultSet, dbStore_);
    return SUCCESS;
}

Status SingleStoreImpl::GetEntries(const DistributedDB::Query &query, std::vector<Entry> &entries) const
{
    std::shared_lock<decltype(mutex_)> lock;
    if (dbStore_ == nullptr) {
        ZLOGE("failed! the db:%{public}s is already closed!", storeId_.c_str());
        return ALREADY_CLOSED;
    }

    std::vector<DistributedDB::Entry> dbEntries;
    auto dbStatus = dbStore_->GetEntries(query, dbEntries);
    entries.resize(dbEntries.size());
    auto it = entries.begin();
    for (auto &dbEntry : dbEntries) {
        auto &entry = *it;
        entry.key = ConvertKey(std::move(dbEntry.key));
        entry.value = std::move(dbEntry.value);
    }
    return StoreUtil::ConvertStatus(dbStatus);
}

std::vector<uint8_t> SingleStoreImpl::GetPrefix(const DataQuery &query) const
{
    std::string prefix = DevManager::GetInstance().ToUUID(query.deviceId_) + query.prefix_;
    return { prefix.begin(), prefix.end() };
}
} // namespace OHOS::DistributedKv