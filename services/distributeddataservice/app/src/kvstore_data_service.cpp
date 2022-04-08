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

#define LOG_TAG "KvStoreDataService"

#include "kvstore_data_service.h"

#include <directory_ex.h>
#include <file_ex.h>
#include <ipc_skeleton.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "auth/auth_delegate.h"
#include "auto_launch_export.h"
#include "bootstrap.h"
#include "checker/checker_manager.h"
#include "communication_provider.h"
#include "config_factory.h"
#include "constant.h"
#include "dds_trace.h"
#include "device_kvstore_impl.h"
#include "executor_factory.h"
#include "if_system_ability_manager.h"
#include "iservice_registry.h"
#include "kvstore_account_observer.h"
#include "kvstore_app_accessor.h"
#include "kvstore_device_listener.h"
#include "kvstore_meta_manager.h"
#include "kvstore_utils.h"
#include "log_print.h"
#include "metadata/meta_data_manager.h"
#include "permission_validator.h"
#include "process_communicator_impl.h"
#include "rdb_service_impl.h"
#include "reporter.h"
#include "route_head_handler_impl.h"
#include "system_ability_definition.h"
#include "uninstaller/uninstaller.h"
#include "upgrade_manager.h"
#include "user_delegate.h"
#include "utils/block_integer.h"
#include "utils/crypto.h"

namespace OHOS::DistributedKv {
using json = nlohmann::json;
using namespace std::chrono;
using namespace OHOS::DistributedData;
using KvStoreDelegateManager = DistributedDB::KvStoreDelegateManager;

REGISTER_SYSTEM_ABILITY_BY_ID(KvStoreDataService, DISTRIBUTED_KV_DATA_SERVICE_ABILITY_ID, true);

constexpr size_t MAX_APP_ID_LENGTH = 256;

KvStoreDataService::KvStoreDataService(bool runOnCreate)
    : SystemAbility(runOnCreate),
      accountMutex_(),
      deviceAccountMap_(),
      clientDeathObserverMutex_(),
      clientDeathObserverMap_()
{
    ZLOGI("begin.");
}

KvStoreDataService::KvStoreDataService(int32_t systemAbilityId, bool runOnCreate)
    : SystemAbility(systemAbilityId, runOnCreate),
      accountMutex_(),
      deviceAccountMap_(),
      clientDeathObserverMutex_(),
      clientDeathObserverMap_()
{
    ZLOGI("begin");
}

KvStoreDataService::~KvStoreDataService()
{
    ZLOGI("begin.");
    deviceAccountMap_.clear();
}

void KvStoreDataService::Initialize()
{
    ZLOGI("begin.");
#ifndef UT_TEST
    KvStoreDelegateManager::SetProcessLabel(Bootstrap::GetInstance().GetProcessLabel(), "default");
#endif
    auto communicator = std::make_shared<AppDistributedKv::ProcessCommunicatorImpl>(RouteHeadHandlerImpl::Create);
    auto ret = KvStoreDelegateManager::SetProcessCommunicator(communicator);
    ZLOGI("set communicator ret:%{public}d.", static_cast<int>(ret));
    auto syncActivationCheck = [this](const std::string &userId, const std::string &appId,
                                   const std::string &storeId) -> bool {
        return CheckSyncActivation(userId, appId, storeId);
    };
    ret = DistributedDB::KvStoreDelegateManager::SetSyncActivationCheckCallback(syncActivationCheck);
    ZLOGI("set sync activation check callback ret:%{public}d.", static_cast<int>(ret));

    InitSecurityAdapter();
    KvStoreMetaManager::GetInstance().InitMetaParameter();
    std::thread th = std::thread([]() {
        if (KvStoreMetaManager::GetInstance().CheckRootKeyExist() == Status::SUCCESS) {
            return;
        }
        constexpr int RETRY_MAX_TIMES = 100;
        int retryCount = 0;
        constexpr int RETRY_TIME_INTERVAL_MILLISECOND = 1 * 1000 * 1000; // retry after 1 second
        while (retryCount < RETRY_MAX_TIMES) {
            if (KvStoreMetaManager::GetInstance().GenerateRootKey() == Status::SUCCESS) {
                ZLOGI("GenerateRootKey success.");
                break;
            }
            retryCount++;
            ZLOGE("GenerateRootKey failed.");
            usleep(RETRY_TIME_INTERVAL_MILLISECOND);
        }
    });
    th.detach();

    accountEventObserver_ = std::make_shared<KvStoreAccountObserver>(*this);
    AccountDelegate::GetInstance()->Subscribe(accountEventObserver_);
    deviceInnerListener_ = std::make_unique<KvStoreDeviceListener>(*this);
    AppDistributedKv::CommunicationProvider::GetInstance().StartWatchDeviceChange(
        deviceInnerListener_.get(), { "innerListener" });
}

Status KvStoreDataService::GetKvStore(const Options &options, const AppId &appId, const StoreId &storeId,
                                      std::function<void(sptr<IKvStoreImpl>)> callback)
{
    ZLOGI("begin.");
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    if (!appId.IsValid() || !storeId.IsValid() || options.kvStoreType != KvStoreType::MULTI_VERSION) {
        ZLOGE("invalid argument type.");
        return Status::INVALID_ARGUMENT;
    }
    KVSTORE_ACCOUNT_EVENT_PROCESSING_CHECKER(Status::SYSTEM_ACCOUNT_EVENT_PROCESSING);
    KvStoreParam param;
    param.bundleName = appId.appId;
    param.storeId = storeId.storeId;
    const int32_t uid = IPCSkeleton::GetCallingUid();
    param.trueAppId = CheckerManager::GetInstance().GetAppId(appId.appId, uid);
    if (param.trueAppId.empty()) {
        ZLOGW("appId:%{public}s, uid:%{public}d, PERMISSION_DENIED", appId.appId.c_str(), uid);
        return Status::PERMISSION_DENIED;
    }

    param.userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    SecretKeyPara keyPara;
    Status status = KvStoreDataService::GetSecretKey(options, param, keyPara);
    if (status != Status::SUCCESS) {
        callback(nullptr);
        return status;
    }

    auto it = deviceAccountMap_.find(param.userId);
    if (it == deviceAccountMap_.end()) {
        auto result = deviceAccountMap_.emplace(std::piecewise_construct,
            std::forward_as_tuple(param.userId), std::forward_as_tuple(param.userId));
        if (!result.second) {
            ZLOGE("emplace failed.");
            FaultMsg msg = {FaultType::RUNTIME_FAULT, "user", __FUNCTION__, Fault::RF_GET_DB};
            Reporter::GetInstance()->ServiceFault()->Report(msg);
            callback(nullptr);
            return Status::ERROR;
        }
        it = result.first;
    }

    sptr<KvStoreImpl> store;
    param.status = (it->second).GetKvStore(options, param.bundleName, param.storeId, uid, keyPara.secretKey, store);
    if (keyPara.outdated) {
        KvStoreMetaManager::GetInstance().ReKey(param.userId, param.bundleName, param.storeId,
            KvStoreAppManager::ConvertPathType(param.uid, param.bundleName, options.securityLevel), store);
    }

    ZLOGD("get kvstore return status:%d, userId:[%s], bundleName:[%s].",
        param.status, KvStoreUtils::ToBeAnonymous(param.userId).c_str(),  appId.appId.c_str());
    if (param.status == Status::SUCCESS) {
        callback(std::move(store));
        return UpdateMetaData(options, param, keyPara.metaKey, it->second);
    }
    param.status = GetKvStoreFailDo(options, param, keyPara, it->second, store);
    callback(std::move(store));
    return param.status;
}

Status KvStoreDataService::GetSingleKvStore(const Options &options, const AppId &appId, const StoreId &storeId,
                                            std::function<void(sptr<ISingleKvStore>)> callback)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("begin.");
    KVSTORE_ACCOUNT_EVENT_PROCESSING_CHECKER(Status::SYSTEM_ACCOUNT_EVENT_PROCESSING);
    KvStoreParam param;
    Status status = FillStoreParam(options, appId, storeId, param);
    if (status != Status::SUCCESS) {
        callback(nullptr);
        return status;
    }

    SecretKeyPara keyPara;
    status = KvStoreDataService::GetSecretKey(options, param, keyPara);
    if (status != Status::SUCCESS) {
        callback(nullptr);
        return status;
    }

    auto it = deviceAccountMap_.find(param.userId);
    if (it == deviceAccountMap_.end()) {
        auto result = deviceAccountMap_.emplace(std::piecewise_construct,
            std::forward_as_tuple(param.userId), std::forward_as_tuple(param.userId));
        if (!result.second) {
            ZLOGE("emplace failed.");
            callback(nullptr);
            return Status::ERROR;
        }
        it = result.first;
    }
    sptr<SingleKvStoreImpl> store;
    param.status =
        (it->second).GetKvStore(options, param.bundleName, param.storeId, param.uid, keyPara.secretKey, store);
    if (keyPara.outdated) {
        KvStoreMetaManager::GetInstance().ReKey(param.userId, param.bundleName, param.storeId,
            KvStoreAppManager::ConvertPathType(param.uid, param.bundleName, options.securityLevel), store);
    }
    if (param.status == Status::SUCCESS) {
        status = UpdateMetaData(options, param, keyPara.metaKey, it->second);
        if (status != Status::SUCCESS) {
            ZLOGE("failed to write meta");
            callback(nullptr);
            return status;
        }
        callback(std::move(store));
        return status;
    }

    param.status =  GetSingleKvStoreFailDo(options, param, keyPara, it->second, store);
    callback(std::move(store));
    return param.status;
}

Status KvStoreDataService::FillStoreParam(
    const Options &options, const AppId &appId, const StoreId &storeId, KvStoreParam &param)
{
    if (!appId.IsValid() || !storeId.IsValid() || !options.IsValidType()
        || options.kvStoreType == KvStoreType::MULTI_VERSION) {
        ZLOGE("invalid argument type.");
        return Status::INVALID_ARGUMENT;
    }
    param.bundleName = appId.appId;
    param.storeId = storeId.storeId;
    param.uid = IPCSkeleton::GetCallingUid();
    param.trueAppId = CheckerManager::GetInstance().GetAppId(appId.appId, param.uid);
    ZLOGI("%{public}s, %{public}s", param.trueAppId.c_str(), param.bundleName.c_str());
    if (param.trueAppId.empty()) {
        ZLOGW("appId:%{public}s, uid:%{public}d, PERMISSION_DENIED", appId.appId.c_str(), param.uid);
        return PERMISSION_DENIED;
    }

    param.userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(param.uid);
    return SUCCESS;
}

Status KvStoreDataService::GetSecretKey(const Options &options, const KvStoreParam &kvParas,
    SecretKeyPara &secretKeyParas)
{
    std::string bundleName = kvParas.bundleName;
    std::string storeIdTmp = kvParas.storeId;
    std::lock_guard<std::mutex> lg(accountMutex_);
    auto metaKey = KvStoreMetaManager::GetMetaKey(kvParas.userId, "default", bundleName, storeIdTmp);
    if (!CheckOptions(options, metaKey)) {
        ZLOGE("encrypt type or kvStore type is not the same");
        return Status::INVALID_ARGUMENT;
    }
    std::vector<uint8_t> secretKey;
    std::unique_ptr<std::vector<uint8_t>, void (*)(std::vector<uint8_t> *)> cleanGuard(
        &secretKey, [](std::vector<uint8_t> *ptr) { ptr->assign(ptr->size(), 0); });

    std::vector<uint8_t> metaSecretKey;
    std::string secretKeyFile;
    if (options.kvStoreType == KvStoreType::MULTI_VERSION) {
        metaSecretKey = KvStoreMetaManager::GetMetaKey(kvParas.userId, "default", bundleName, storeIdTmp, "KEY");
        secretKeyFile = KvStoreMetaManager::GetSecretKeyFile(kvParas.userId, bundleName, storeIdTmp,
            KvStoreAppManager::ConvertPathType(kvParas.uid, bundleName, options.securityLevel));
    } else {
        metaSecretKey = KvStoreMetaManager::GetMetaKey(kvParas.userId, "default", bundleName, storeIdTmp, "SINGLE_KEY");
        secretKeyFile = KvStoreMetaManager::GetSecretSingleKeyFile(kvParas.userId, bundleName, storeIdTmp,
            KvStoreAppManager::ConvertPathType(kvParas.uid, bundleName, options.securityLevel));
    }

    bool outdated = false;
    Status alreadyCreated = KvStoreMetaManager::GetInstance().CheckUpdateServiceMeta(metaSecretKey, CHECK_EXIST_LOCAL);
    if (options.encrypt) {
        ZLOGI("Getting secret key");
        Status recStatus = RecoverSecretKey(alreadyCreated, outdated, metaSecretKey, secretKey, secretKeyFile);
        if (recStatus != Status::SUCCESS) {
            return recStatus;
        }
    } else {
        if (alreadyCreated == Status::SUCCESS || FileExists(secretKeyFile)) {
            ZLOGW("try to get an encrypted store with false option encrypt parameter");
            return Status::CRYPT_ERROR;
        }
    }

    SecretKeyPara kvStoreSecretKey;
    kvStoreSecretKey.metaKey = metaKey;
    kvStoreSecretKey.secretKey = secretKey;
    kvStoreSecretKey.metaSecretKey = metaSecretKey;
    kvStoreSecretKey.secretKeyFile = secretKeyFile;
    kvStoreSecretKey.alreadyCreated = alreadyCreated;
    kvStoreSecretKey.outdated = outdated;
    secretKeyParas = kvStoreSecretKey;

    return Status::SUCCESS;
}

Status KvStoreDataService::RecoverSecretKey(const Status &alreadyCreated, bool &outdated,
    const std::vector<uint8_t> &metaSecretKey, std::vector<uint8_t> &secretKey, const std::string &secretKeyFile)
{
    if (alreadyCreated != Status::SUCCESS) {
        KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
            secretKeyFile, metaSecretKey, secretKey, outdated);
        if (secretKey.empty()) {
            ZLOGI("new secret key");
            secretKey = Crypto::Random(32); // 32 is key length
            KvStoreMetaManager::GetInstance().WriteSecretKeyToMeta(metaSecretKey, secretKey);
            KvStoreMetaManager::GetInstance().WriteSecretKeyToFile(secretKeyFile, secretKey);
        }
    } else {
        KvStoreMetaManager::GetInstance().GetSecretKeyFromMeta(metaSecretKey, secretKey, outdated);
        if (secretKey.empty()) {
            ZLOGW("get secret key from meta failed, try to recover");
            KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
                secretKeyFile, metaSecretKey, secretKey, outdated);
        }
        if (secretKey.empty()) {
            ZLOGW("recover failed");
            return Status::CRYPT_ERROR;
        }
        KvStoreMetaManager::GetInstance().WriteSecretKeyToFile(secretKeyFile, secretKey);
    }
    return Status::SUCCESS;
}

Status KvStoreDataService::UpdateMetaData(const Options &options, const KvStoreParam &kvParas,
    const std::vector<uint8_t> &metaKey, KvStoreUserManager &kvStoreUserManager)
{
    auto localDeviceId = DeviceKvStoreImpl::GetLocalDeviceId();
    if (localDeviceId.empty()) {
        ZLOGE("failed to get local device id");
        return Status::ERROR;
    }
    KvStoreMetaData metaData;
    metaData.appId = kvParas.trueAppId;
    metaData.appType = "harmony";
    metaData.bundleName = kvParas.bundleName;
    metaData.deviceAccountId = kvParas.userId;
    metaData.deviceId = localDeviceId;
    metaData.isAutoSync = options.autoSync;
    metaData.isBackup = options.backup;
    metaData.isEncrypt = options.encrypt;
    metaData.kvStoreType = options.kvStoreType;
    metaData.schema = options.schema;
    metaData.storeId = kvParas.storeId;
    metaData.tokenId = IPCSkeleton::GetCallingTokenID();
    metaData.userId = AccountDelegate::GetInstance()->GetCurrentAccountId(kvParas.bundleName);
    metaData.uid = IPCSkeleton::GetCallingUid();
    metaData.version = STORE_VERSION;
    metaData.securityLevel = options.securityLevel;
    metaData.dataDir = kvStoreUserManager.GetDbDir(kvParas.bundleName, options);

    std::string jsonStr = metaData.Marshal();
    std::vector<uint8_t> jsonVec(jsonStr.begin(), jsonStr.end());
    return KvStoreMetaManager::GetInstance().CheckUpdateServiceMeta(metaKey, UPDATE, jsonVec);
}

Status KvStoreDataService::GetKvStoreFailDo(const Options &options, const KvStoreParam &kvParas,
    SecretKeyPara &secKeyParas, KvStoreUserManager &kvUserManager, sptr<KvStoreImpl> &store)
{
    Status statusTmp = kvParas.status;
    Status getKvStoreStatus = statusTmp;
    int32_t path = KvStoreAppManager::ConvertPathType(kvParas.uid, kvParas.bundleName, options.securityLevel);
    ZLOGW("getKvStore failed with status %d", static_cast<int>(getKvStoreStatus));
    if (getKvStoreStatus == Status::CRYPT_ERROR && options.encrypt) {
        if (secKeyParas.alreadyCreated != Status::SUCCESS) {
            // create encrypted store failed, remove secret key
            KvStoreMetaManager::GetInstance().RemoveSecretKey(kvParas.uid, kvParas.bundleName, kvParas.storeId);
            return Status::ERROR;
        }
        // get existing encrypted store failed, retry with key stored in file
        Status status = KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
            secKeyParas.secretKeyFile, secKeyParas.metaSecretKey, secKeyParas.secretKey, secKeyParas.outdated);
        if (status != Status::SUCCESS) {
            store = nullptr;
            return Status::CRYPT_ERROR;
        }
        // here callback is called twice
        statusTmp = kvUserManager.GetKvStore(
            options, kvParas.bundleName, kvParas.storeId, kvParas.uid, secKeyParas.secretKey, store);
        if (secKeyParas.outdated) {
            KvStoreMetaManager::GetInstance().ReKey(kvParas.userId, kvParas.bundleName, kvParas.storeId, path, store);
        }
    }

    // if kvstore damaged and no backup file, then return DB_ERROR
    if (statusTmp != Status::SUCCESS && getKvStoreStatus == Status::CRYPT_ERROR) {
        // if backup file not exist, don't need recover
        if (!CheckBackupFileExist(kvParas.userId, kvParas.bundleName, kvParas.storeId, path)) {
            return Status::CRYPT_ERROR;
        }
        // remove damaged database
        if (DeleteKvStoreOnly(kvParas.storeId, kvParas.uid, kvParas.bundleName) != Status::SUCCESS) {
            ZLOGE("DeleteKvStoreOnly failed.");
            return Status::DB_ERROR;
        }
        // recover database
        return RecoverKvStore(options, kvParas.bundleName, kvParas.storeId, secKeyParas.secretKey, store);
    }
    return statusTmp;
}

Status KvStoreDataService::GetSingleKvStoreFailDo(const Options &options, const KvStoreParam &kvParas,
    SecretKeyPara &secKeyParas, KvStoreUserManager &kvUserManager, sptr<SingleKvStoreImpl> &kvStore)
{
    Status statusTmp = kvParas.status;
    Status getKvStoreStatus = statusTmp;
    int32_t path = KvStoreAppManager::ConvertPathType(kvParas.uid, kvParas.bundleName, options.securityLevel);
    ZLOGW("getKvStore failed with status %d", static_cast<int>(getKvStoreStatus));
    if (getKvStoreStatus == Status::CRYPT_ERROR && options.encrypt) {
        if (secKeyParas.alreadyCreated != Status::SUCCESS) {
            // create encrypted store failed, remove secret key
            KvStoreMetaManager::GetInstance().RemoveSecretKey(kvParas.uid, kvParas.bundleName, kvParas.storeId);
            return Status::ERROR;
        }
        // get existing encrypted store failed, retry with key stored in file
        Status status = KvStoreMetaManager::GetInstance().RecoverSecretKeyFromFile(
            secKeyParas.secretKeyFile, secKeyParas.metaSecretKey, secKeyParas.secretKey, secKeyParas.outdated);
        if (status != Status::SUCCESS) {
            kvStore = nullptr;
            return Status::CRYPT_ERROR;
        }
        // here callback is called twice
        statusTmp = kvUserManager.GetKvStore(
            options, kvParas.bundleName, kvParas.storeId, kvParas.uid, secKeyParas.secretKey, kvStore);
        if (secKeyParas.outdated) {
            KvStoreMetaManager::GetInstance().ReKey(kvParas.userId, kvParas.bundleName, kvParas.storeId, path, kvStore);
        }
    }

    // if kvstore damaged and no backup file, then return DB_ERROR
    if (statusTmp != Status::SUCCESS && getKvStoreStatus == Status::CRYPT_ERROR) {
        // if backup file not exist, don't need recover
        if (!CheckBackupFileExist(kvParas.userId, kvParas.bundleName, kvParas.storeId, path)) {
            return Status::CRYPT_ERROR;
        }
        // remove damaged database
        if (DeleteKvStoreOnly(kvParas.storeId, kvParas.uid, kvParas.bundleName) != Status::SUCCESS) {
            ZLOGE("DeleteKvStoreOnly failed.");
            return Status::DB_ERROR;
        }
        // recover database
        return RecoverKvStore(options, kvParas.bundleName, kvParas.storeId, secKeyParas.secretKey, kvStore);
    }
    return statusTmp;
}

bool KvStoreDataService::CheckOptions(const Options &options, const std::vector<uint8_t> &metaKey) const
{
    ZLOGI("begin.");
    KvStoreMetaData metaData;
    metaData.version = 0;
    Status statusTmp = KvStoreMetaManager::GetInstance().GetKvStoreMeta(metaKey, metaData);
    if (statusTmp == Status::KEY_NOT_FOUND) {
        ZLOGI("get metaKey not found.");
        return true;
    }
    if (statusTmp != Status::SUCCESS) {
        ZLOGE("get metaKey failed.");
        return false;
    }
    ZLOGI("metaData encrypt is %d, kvStore type is %d, options encrypt is %d, kvStore type is %d",
          static_cast<int>(metaData.isEncrypt), static_cast<int>(metaData.kvStoreType),
          static_cast<int>(options.encrypt), static_cast<int>(options.kvStoreType));
    if (options.encrypt != metaData.isEncrypt) {
        ZLOGE("checkOptions encrypt type is not the same.");
        return false;
    }

    if (options.kvStoreType != metaData.kvStoreType && metaData.version != 0) {
        ZLOGE("checkOptions kvStoreType is not the same.");
        return false;
    }
    ZLOGI("end.");
    return true;
}

bool KvStoreDataService::CheckBackupFileExist(const std::string &userId, const std::string &bundleName,
                                              const std::string &storeId, int pathType)
{
    std::initializer_list<std::string> backupFileNameList = {Constant::DEFAULT_GROUP_ID, "_",
        bundleName, "_", storeId};
    auto backupFileName = Constant::Concatenate(backupFileNameList);
    std::initializer_list<std::string> backFileList = {BackupHandler::GetBackupPath(userId, pathType),
        "/", BackupHandler::GetHashedBackupName(backupFileName)};
    auto backFilePath = Constant::Concatenate(backFileList);
    if (!BackupHandler::FileExists(backFilePath)) {
        ZLOGE("BackupHandler file is not exist.");
        return false;
    }
    return true;
}
template<typename  T>
Status KvStoreDataService::RecoverKvStore(const Options &options, const std::string &bundleName,
    const std::string &storeId, const std::vector<uint8_t> &secretKey, sptr<T> &kvStore)
{
    // restore database
    std::string storeIdTmp = storeId;
    Options optionsTmp = options;
    optionsTmp.createIfMissing = true;
    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string deviceAccountId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    auto it = deviceAccountMap_.find(deviceAccountId);
    if (it == deviceAccountMap_.end()) {
        ZLOGD("deviceAccountId not found");
        return Status::INVALID_ARGUMENT;
    }

    Status statusTmp = (it->second).GetKvStore(optionsTmp, bundleName, storeIdTmp, uid, secretKey, kvStore);
    // restore database failed
    if (statusTmp != Status::SUCCESS || kvStore == nullptr) {
        ZLOGE("RecoverSingleKvStore reget GetSingleKvStore failed.");
        return Status::DB_ERROR;
    }
    // recover database from backup file
    bool importRet = kvStore->Import(bundleName);
    if (!importRet) {
        ZLOGE("RecoverSingleKvStore Import failed.");
        return Status::RECOVER_FAILED;
    }
    ZLOGD("RecoverSingleKvStore Import success.");
    return Status::RECOVER_SUCCESS;
}

void KvStoreDataService::GetAllKvStoreId(
    const AppId &appId, std::function<void(Status, std::vector<StoreId> &)> callback)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("GetAllKvStoreId begin.");
    std::string bundleName = Constant::TrimCopy<std::string>(appId.appId);
    std::vector<StoreId> storeIds;
    if (bundleName.empty() || bundleName.size() > MAX_APP_ID_LENGTH) {
        ZLOGE("invalid appId.");
        callback(Status::INVALID_ARGUMENT, storeIds);
        return;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    std::string prefix =
        StoreMetaData::GetPrefix({ DeviceKvStoreImpl::GetLocalDeviceId(), userId, "default", bundleName });
    DdsTrace traceDelegate(std::string(LOG_TAG "Delegate::") + std::string(__FUNCTION__));

    std::vector<StoreMetaData> metaDatum;
    if (!MetaDataManager::GetInstance().LoadMeta(prefix, metaDatum)) {
        ZLOGE("LoadKeys failed!");
        callback(Status::DB_ERROR, storeIds);
        return;
    }

    for (const auto &metaData : metaDatum) {
        if (metaData.storeId.empty()) {
            continue;
        }
        storeIds.push_back({ metaData.storeId });
    }
    callback(Status::SUCCESS, storeIds);
}

Status KvStoreDataService::CloseKvStore(const AppId &appId, const StoreId &storeId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("begin.");
    if (!appId.IsValid() || !storeId.IsValid()) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    std::string trueAppId = CheckerManager::GetInstance().GetAppId(appId.appId, uid);
    if (trueAppId.empty()) {
        ZLOGW("check appId:%{public}s uid:%{public}d failed.", appId.appId.c_str(), uid);
        return Status::PERMISSION_DENIED;
    }
    const std::string userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    std::lock_guard<std::mutex> lg(accountMutex_);
    auto it = deviceAccountMap_.find(userId);
    if (it != deviceAccountMap_.end()) {
        Status status = (it->second).CloseKvStore(appId.appId, storeId.storeId);
        if (status != Status::STORE_NOT_OPEN) {
            return status;
        }
    }
    FaultMsg msg = {FaultType::RUNTIME_FAULT, "user", __FUNCTION__, Fault::RF_CLOSE_DB};
    Reporter::GetInstance()->ServiceFault()->Report(msg);
    ZLOGE("return STORE_NOT_OPEN.");
    return Status::STORE_NOT_OPEN;
}

/* close all opened kvstore */
Status KvStoreDataService::CloseAllKvStore(const AppId &appId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGD("begin.");
    if (!appId.IsValid()) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    const int32_t uid = IPCSkeleton::GetCallingUid();
    std::string trueAppId = CheckerManager::GetInstance().GetAppId(appId.appId, uid);
    if (trueAppId.empty()) {
        ZLOGW("check appId:%{public}s uid:%{public}d failed.", appId.appId.c_str(), uid);
        return Status::PERMISSION_DENIED;
    }

    const std::string userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    std::lock_guard<std::mutex> lg(accountMutex_);
    auto it = deviceAccountMap_.find(userId);
    if (it != deviceAccountMap_.end()) {
        return (it->second).CloseAllKvStore(appId.appId);
    }
    ZLOGE("store not open.");
    return Status::STORE_NOT_OPEN;
}

Status KvStoreDataService::DeleteKvStore(const AppId &appId, const StoreId &storeId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    if (!appId.IsValid()) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }
    int32_t uid = IPCSkeleton::GetCallingUid();
    if (!CheckerManager::GetInstance().IsValid(appId.appId, uid)) {
        ZLOGE("get appId failed.");
        return Status::PERMISSION_DENIED;
    }

    // delete the backup file
    std::initializer_list<std::string> backFileList = {
        AccountDelegate::GetInstance()->GetCurrentAccountId(), "_", appId.appId, "_", storeId.storeId};
    auto backupFileName = Constant::Concatenate(backFileList);
    const std::string userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    std::initializer_list<std::string> backPathListDE = {BackupHandler::GetBackupPath(userId,
        KvStoreAppManager::PATH_DE), "/", BackupHandler::GetHashedBackupName(backupFileName)};
    auto backFilePath = Constant::Concatenate(backPathListDE);
    if (!BackupHandler::RemoveFile(backFilePath)) {
        ZLOGE("DeleteKvStore RemoveFile backFilePath failed.");
    }
    std::initializer_list<std::string> backPathListCE = {BackupHandler::GetBackupPath(userId,
        KvStoreAppManager::PATH_CE), "/", BackupHandler::GetHashedBackupName(backupFileName)};
    backFilePath = Constant::Concatenate(backPathListCE);
    if (!BackupHandler::RemoveFile(backFilePath)) {
        ZLOGE("DeleteKvStore RemoveFile backFilePath failed.");
    }
    return DeleteKvStore(appId.appId, storeId);
}

/* delete all kv store */
Status KvStoreDataService::DeleteAllKvStore(const AppId &appId)
{
    DdsTrace trace(std::string(LOG_TAG "::") + std::string(__FUNCTION__));
    ZLOGI("%s", appId.appId.c_str());
    if (!appId.IsValid()) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }

    int32_t uid = IPCSkeleton::GetCallingUid();
    if (!CheckerManager::GetInstance().IsValid(appId.appId, uid)) {
        ZLOGE("check appId:%{public}s uid:%{public}d failed.", appId.appId.c_str(), uid);
        return Status::PERMISSION_DENIED;
    }

    Status statusTmp;
    std::vector<StoreId> existStoreIds;
    GetAllKvStoreId(appId, [&statusTmp, &existStoreIds](Status status, std::vector<StoreId> &storeIds) {
        statusTmp = status;
        existStoreIds = std::move(storeIds);
    });

    if (statusTmp != Status::SUCCESS) {
        ZLOGE("%s, error: %d ", appId.appId.c_str(), static_cast<int>(statusTmp));
        return statusTmp;
    }

    for (const auto &storeId : existStoreIds) {
        statusTmp = DeleteKvStore(appId, storeId);
        if (statusTmp != Status::SUCCESS) {
            ZLOGE("%s, error: %d ", appId.appId.c_str(), static_cast<int>(statusTmp));
            return statusTmp;
        }
    }

    return statusTmp;
}

/* RegisterClientDeathObserver */
Status KvStoreDataService::RegisterClientDeathObserver(const AppId &appId, sptr<IRemoteObject> observer)
{
    ZLOGD("begin.");
    KVSTORE_ACCOUNT_EVENT_PROCESSING_CHECKER(Status::SYSTEM_ACCOUNT_EVENT_PROCESSING);
    if (!appId.IsValid()) {
        ZLOGE("invalid bundleName.");
        return Status::INVALID_ARGUMENT;
    }

    int32_t uid = IPCSkeleton::GetCallingUid();
    if (!CheckerManager::GetInstance().IsValid(appId.appId, uid)) {
        ZLOGE("no permission bundleName:%{public}s, uid:%{public}d.", appId.appId.c_str(), uid);
        return Status::PERMISSION_DENIED;
    }

    std::lock_guard<std::mutex> lg(clientDeathObserverMutex_);
    auto it = clientDeathObserverMap_.emplace(std::piecewise_construct, std::forward_as_tuple(appId.appId),
        std::forward_as_tuple(appId, uid, *this, std::move(observer)));
    ZLOGI("map size: %{public}zu.", clientDeathObserverMap_.size());
    if (!it.second) {
        ZLOGI("insert failed");
        return Status::ERROR;
    }
    ZLOGI("insert success");
    const std::string userId = AccountDelegate::GetInstance()->GetCurrentAccountId();
    KvStoreTuple kvStoreTuple {userId, CheckerManager::GetInstance().GetAppId(appId.appId, uid)};
    AppThreadInfo appThreadInfo {IPCSkeleton::GetCallingPid(), IPCSkeleton::GetCallingUid()};
    PermissionValidator::RegisterPermissionChanged(kvStoreTuple, appThreadInfo);
    return Status::SUCCESS;
}

Status KvStoreDataService::AppExit(const AppId &appId, pid_t uid)
{
    ZLOGI("AppExit");
    // memory of parameter appId locates in a member of clientDeathObserverMap_ and will be freed after
    // clientDeathObserverMap_ erase, so we have to take a copy if we want to use this parameter after erase operation.
    AppId appIdTmp = appId;
    {
        std::lock_guard<std::mutex> lg(clientDeathObserverMutex_);
        clientDeathObserverMap_.erase(appIdTmp.appId);
        ZLOGI("map size: %zu.", clientDeathObserverMap_.size());
    }

    std::string trueAppId = CheckerManager::GetInstance().GetAppId(appIdTmp.appId, uid);
    if (trueAppId.empty()) {
        ZLOGW("check appId:%{public}s uid:%{public}d failed.", appIdTmp.appId.c_str(), uid);
        return Status::PERMISSION_DENIED;
    }
    const std::string userId = AccountDelegate::GetInstance()->GetCurrentAccountId(appIdTmp.appId);
    KvStoreTuple kvStoreTuple {userId, trueAppId};
    PermissionValidator::UnregisterPermissionChanged(kvStoreTuple);

    CloseAllKvStore(appIdTmp);
    return Status::SUCCESS;
}

void KvStoreDataService::OnDump()
{
    ZLOGD("begin.");
}

int KvStoreDataService::Dump(int fd, const std::vector<std::u16string> &args)
{
    int uid = static_cast<int>(IPCSkeleton::GetCallingUid());
    const int maxUid = 10000;
    if (uid > maxUid) {
        return 0;
    }
    dprintf(fd, "------------------------------------------------------------------\n");
    dprintf(fd, "DeviceAccount count : %u\n", static_cast<uint32_t>(deviceAccountMap_.size()));
    for (const auto &pair : deviceAccountMap_) {
        dprintf(fd, "DeviceAccountID    : %s\n", pair.first.c_str());
        pair.second.Dump(fd);
    }
    return 0;
}

void KvStoreDataService::OnStart()
{
    ZLOGI("distributeddata service onStart");
    static constexpr int32_t RETRY_TIMES = 10;
    static constexpr int32_t RETRY_INTERVAL = 500 * 1000; // unit is ms
    for (BlockInteger retry(RETRY_INTERVAL); retry < RETRY_TIMES; ++retry) {
        if (!DeviceKvStoreImpl::GetLocalDeviceId().empty()) {
            break;
        }
        ZLOGE("GetLocalDeviceId failed, retry count:%{public}d", static_cast<int>(retry));
    }
    Initialize();
    Bootstrap::GetInstance().LoadComponents();
    Bootstrap::GetInstance().LoadDirectory();
    Bootstrap::GetInstance().LoadCheckers();
    Bootstrap::GetInstance().LoadNetworks();
    auto samgr = SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager();
    if (samgr != nullptr) {
        ZLOGI("samgr exist.");
        auto remote = samgr->CheckSystemAbility(DISTRIBUTED_KV_DATA_SERVICE_ABILITY_ID);
        auto kvDataServiceProxy = iface_cast<IKvStoreDataService>(remote);
        if (kvDataServiceProxy != nullptr) {
            ZLOGI("service has been registered.");
            return;
        }
    }
    CreateRdbService();
    StartService();
}

void KvStoreDataService::StartService()
{
    // register this to ServiceManager.
    KvStoreMetaManager::GetInstance().InitMetaListener();
    bool ret = SystemAbility::Publish(this);
    if (!ret) {
        FaultMsg msg = {FaultType::SERVICE_FAULT, "service", __FUNCTION__, Fault::SF_SERVICE_PUBLISH};
        Reporter::GetInstance()->ServiceFault()->Report(msg);
    }
    Uninstaller::GetInstance().Init(this);

    std::string backupPath = BackupHandler::GetBackupPath(
        AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(getuid()), KvStoreAppManager::PATH_DE);
    ZLOGI("backupPath is : %s ", backupPath.c_str());
    if (!ForceCreateDirectory(backupPath)) {
        ZLOGE("backup create directory failed");
    }
    // Initialize meta db delegate manager.
    KvStoreMetaManager::GetInstance().SubscribeMeta(
        KvStoreMetaRow::KEY_PREFIX, [this](const std::vector<uint8_t> &key, const std::vector<uint8_t> &value,
                                        CHANGE_FLAG flag) { OnStoreMetaChanged(key, value, flag); });
    UpgradeManager::GetInstance().Init();
    UserDelegate::GetInstance().Init();

    // subscribe account event listener to EventNotificationMgr
    AccountDelegate::GetInstance()->SubscribeAccountEvent();
    auto permissionCheckCallback =
        [this](const std::string &userId, const std::string &appId, const std::string
        &storeId, const std::string &deviceId, uint8_t flag) -> bool {
            // temp add permission whilelist for ddmp; this should be config in ddmp manifest.
            ZLOGD("checking sync permission start appid:%s, stid:%s.", appId.c_str(), storeId.c_str());
            return CheckPermissions(userId, appId, storeId, deviceId, flag);
        };
    auto dbStatus = KvStoreDelegateManager::SetPermissionCheckCallback(permissionCheckCallback);
    if (dbStatus != DistributedDB::DBStatus::OK) {
        ZLOGE("SetPermissionCheck callback failed.");
    }
    ZLOGI("autoLaunchRequestCallback start");
    auto autoLaunchRequestCallback =
        [this](const std::string &identifier, DistributedDB::AutoLaunchParam &param) -> bool {
            return ResolveAutoLaunchParamByIdentifier(identifier, param);
        };
    KvStoreDelegateManager::SetAutoLaunchRequestCallback(autoLaunchRequestCallback);

    backup_ = std::make_unique<BackupHandler>(this);
    backup_->BackSchedule();

    std::thread th = std::thread([]() {
        sleep(TEN_SEC);
        KvStoreAppAccessor::GetInstance().EnableKvStoreAutoLaunch();
    });
    th.detach();
    ZLOGI("Publish ret: %{public}d", static_cast<int>(ret));
}

void KvStoreDataService::OnStoreMetaChanged(
    const std::vector<uint8_t> &key, const std::vector<uint8_t> &value, CHANGE_FLAG flag)
{
    if (flag != CHANGE_FLAG::UPDATE) {
        return;
    }
    StoreMetaData metaData;
    metaData.Unmarshall({ value.begin(), value.end() });
    ZLOGD("meta data info appType:%{public}s, storeId:%{public}s isDirty:%{public}d", metaData.appType.c_str(),
        metaData.storeId.c_str(), metaData.isDirty);
    if (metaData.deviceId != DeviceKvStoreImpl::GetLocalDeviceId() || metaData.deviceId.empty()) {
        ZLOGD("ignore other device change or invalid meta device");
        return;
    }
    static constexpr const char *HARMONY_APP = "harmony";
    if (!metaData.isDirty || metaData.appType != HARMONY_APP) {
        return;
    }
    ZLOGI("dirty kv store. storeId:%{public}s", metaData.storeId.c_str());
    CloseKvStore({ metaData.bundleName }, { metaData.storeId });
    DeleteKvStore({ metaData.bundleName }, { metaData.storeId });
}

bool KvStoreDataService::ResolveAutoLaunchParamByIdentifier(
    const std::string &identifier, DistributedDB::AutoLaunchParam &param)
{
    ZLOGI("start");
    std::map<std::string, MetaData> entries;
    if (!KvStoreMetaManager::GetInstance().GetFullMetaData(entries)) {
        ZLOGE("get full meta failed");
        return false;
    }
    std::string localDeviceId = DeviceKvStoreImpl::GetLocalDeviceId();
    for (const auto &entry : entries) {
        auto &storeMeta = entry.second.kvStoreMetaData;
        if ((!param.userId.empty() && (param.userId != storeMeta.deviceAccountId))
            || (localDeviceId != storeMeta.deviceId)) {
            // judge local userid and local meta
            continue;
        }
        const std::string &itemTripleIdentifier = DistributedDB::KvStoreDelegateManager::GetKvStoreIdentifier(
            storeMeta.userId, storeMeta.appId, storeMeta.storeId, false);
        const std::string &itemDualIdentifier =
            DistributedDB::KvStoreDelegateManager::GetKvStoreIdentifier("", storeMeta.appId, storeMeta.storeId, true);
        if (identifier == itemTripleIdentifier) {
            // old triple tuple identifier, should SetEqualIdentifier
            ResolveAutoLaunchCompatible(entry.second, identifier);
        }
        if (identifier == itemDualIdentifier || identifier == itemTripleIdentifier) {
            ZLOGI("identifier  find");
            DistributedDB::AutoLaunchOption option;
            option.createIfNecessary = false;
            option.isEncryptedDb = storeMeta.isEncrypt;
            DistributedDB::CipherPassword password;
            const std::vector<uint8_t> &secretKey = entry.second.secretKeyMetaData.secretKey;
            if (password.SetValue(secretKey.data(), secretKey.size()) != DistributedDB::CipherPassword::OK) {
                ZLOGE("Get secret key failed.");
            }
            option.passwd = password;
            option.schema = storeMeta.schema;
            option.createDirByStoreIdOnly = true;
            option.dataDir = storeMeta.dataDir;
            option.secOption = KvStoreAppManager::ConvertSecurity(storeMeta.securityLevel);
            option.isAutoSync = storeMeta.isAutoSync;
            option.syncDualTupleMode = true; // dual tuple flag
            param.appId = storeMeta.appId;
            param.storeId = storeMeta.storeId;
            param.option = option;
            return true;
        }
    }
    ZLOGI("not find identifier");
    return false;
}

void KvStoreDataService::ResolveAutoLaunchCompatible(const MetaData &meta, const std::string &identifier)
{
    ZLOGI("AutoLaunch:peer device is old tuple, begin to open store");
    if (meta.kvStoreType >= KvStoreType::MULTI_VERSION) {
        ZLOGW("no longer support multi or higher version store type");
        return;
    }

    // open store and SetEqualIdentifier, then close store after 60s
    auto &storeMeta = meta.kvStoreMetaData;
    auto *delegateManager = new (std::nothrow)
        DistributedDB::KvStoreDelegateManager(storeMeta.appId, storeMeta.deviceAccountId);
    if (delegateManager == nullptr) {
        ZLOGE("get store delegate manager failed");
        return;
    }
    delegateManager->SetKvStoreConfig({ storeMeta.dataDir });
    Options options = {
        .encrypt = storeMeta.isEncrypt,
        .autoSync = storeMeta.isAutoSync,
        .securityLevel = storeMeta.securityLevel,
        .kvStoreType = static_cast<KvStoreType>(storeMeta.kvStoreType),
        .dataOwnership = true,
    };
    DistributedDB::KvStoreNbDelegate::Option dbOptions;
    KvStoreAppManager::InitNbDbOption(options, meta.secretKeyMetaData.secretKey, dbOptions);
    DistributedDB::KvStoreNbDelegate *store = nullptr;
    delegateManager->GetKvStore(storeMeta.storeId, dbOptions,
        [&identifier, &store, &storeMeta](int status, DistributedDB::KvStoreNbDelegate *delegate) {
            ZLOGI("temporary open db for equal identifier, ret:%{public}d", status);
            if (delegate != nullptr) {
                KvStoreTuple tuple = { storeMeta.deviceAccountId, storeMeta.appId, storeMeta.storeId };
                UpgradeManager::SetCompatibleIdentifyByType(delegate, tuple, IDENTICAL_ACCOUNT_GROUP);
                UpgradeManager::SetCompatibleIdentifyByType(delegate, tuple, PEER_TO_PEER_GROUP);
                store = delegate;
            }
        });
    KvStoreTask delayTask([delegateManager, store]() {
        constexpr const int CLOSE_STORE_DELAY_TIME = 60; // unit: seconds
        std::this_thread::sleep_for(std::chrono::seconds(CLOSE_STORE_DELAY_TIME));
        ZLOGI("AutoLaunch:close store after 60s while autolaunch finishied");
        delegateManager->CloseKvStore(store);
        delete delegateManager;
    });
    ExecutorFactory::GetInstance().Execute(std::move(delayTask));
}

bool KvStoreDataService::CheckPermissions(const std::string &userId, const std::string &appId,
                                          const std::string &storeId, const std::string &deviceId, uint8_t flag) const
{
    ZLOGI("userId=%{public}.6s appId=%{public}s storeId=%{public}s flag=%{public}d deviceId=%{public}.4s",
          userId.c_str(), appId.c_str(), storeId.c_str(), flag, deviceId.c_str()); // only print 4 chars of device id
    auto &instance = KvStoreMetaManager::GetInstance();
    KvStoreMetaData metaData;
    auto localDevId = DeviceKvStoreImpl::GetLocalDeviceId();
    auto qstatus = instance.QueryKvStoreMetaDataByDeviceIdAndAppId(localDevId, appId, metaData);
    if (qstatus != Status::SUCCESS) {
        qstatus = instance.QueryKvStoreMetaDataByDeviceIdAndAppId("", appId, metaData); // local device id maybe null
        if (qstatus != Status::SUCCESS) {
            ZLOGW("query appId failed.");
            return false;
        }
    }
    if (metaData.appType.compare("default") == 0) {
        ZLOGD("default, don't check sync permission.");
        return true;
    }
    Status status = instance.CheckSyncPermission(userId, appId, storeId, flag, deviceId);
    if (status != Status::SUCCESS) {
        ZLOGW("PermissionCheck failed.");
        return false;
    }

    if (metaData.appType.compare("harmony") != 0) {
        ZLOGD("it's A app, don't check sync permission.");
        return true;
    }

    if (PermissionValidator::IsAutoLaunchEnabled(appId)) {
        return true;
    }
    bool ret = PermissionValidator::CheckSyncPermission(userId, appId, metaData.tokenId);
    ZLOGD("checking sync permission ret:%{public}d.", ret);
    return ret;
}

void KvStoreDataService::OnStop()
{
    ZLOGI("begin.");
    if (backup_ != nullptr) {
        backup_.reset();
        backup_ = nullptr;
    }
}

KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreClientDeathObserverImpl(
    const AppId &appId, pid_t uid, KvStoreDataService &service, sptr<IRemoteObject> observer)
    : appId_(appId), uid_(uid), dataService_(service), observerProxy_(std::move(observer)),
    deathRecipient_(new KvStoreDeathRecipient(*this))
{
    ZLOGI("KvStoreClientDeathObserverImpl");

    if (observerProxy_ != nullptr) {
        ZLOGI("add death recipient");
        observerProxy_->AddDeathRecipient(deathRecipient_);
    } else {
        ZLOGW("observerProxy_ is nullptr");
    }
}

KvStoreDataService::KvStoreClientDeathObserverImpl::~KvStoreClientDeathObserverImpl()
{
    ZLOGI("~KvStoreClientDeathObserverImpl");
    if (deathRecipient_ != nullptr && observerProxy_ != nullptr) {
        ZLOGI("remove death recipient");
        observerProxy_->RemoveDeathRecipient(deathRecipient_);
    }
}

void KvStoreDataService::KvStoreClientDeathObserverImpl::NotifyClientDie()
{
    ZLOGI("appId: %{public}s uid:%{public}d", appId_.appId.c_str(), uid_);
    dataService_.AppExit(appId_, uid_);
}

KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreDeathRecipient::KvStoreDeathRecipient(
    KvStoreClientDeathObserverImpl &kvStoreClientDeathObserverImpl)
    : kvStoreClientDeathObserverImpl_(kvStoreClientDeathObserverImpl)
{
    ZLOGI("KvStore Client Death Observer");
}

KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreDeathRecipient::~KvStoreDeathRecipient()
{
    ZLOGI("KvStore Client Death Observer");
}

void KvStoreDataService::KvStoreClientDeathObserverImpl::KvStoreDeathRecipient::OnRemoteDied(
    const wptr<IRemoteObject> &remote)
{
    (void) remote;
    ZLOGI("begin");
    kvStoreClientDeathObserverImpl_.NotifyClientDie();
}

Status KvStoreDataService::DeleteKvStore(const std::string &bundleName, const StoreId &storeId)
{
    ZLOGI("begin.");
    if (!storeId.IsValid()) {
        ZLOGE("invalid storeId.");
        return Status::INVALID_ARGUMENT;
    }

    const int32_t uid = IPCSkeleton::GetCallingUid();
    const std::string userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    std::lock_guard<std::mutex> lg(accountMutex_);
    Status status;
    auto it = deviceAccountMap_.find(userId);
    if (it != deviceAccountMap_.end()) {
        status = (it->second).DeleteKvStore(bundleName, uid, storeId.storeId);
    } else {
        KvStoreUserManager kvStoreUserManager(userId);
        status = kvStoreUserManager.DeleteKvStore(bundleName, uid, storeId.storeId);
    }

    if (status == Status::SUCCESS) {
        auto metaKey = KvStoreMetaManager::GetMetaKey(userId, "default", bundleName, storeId.storeId);
        status = KvStoreMetaManager::GetInstance().CheckUpdateServiceMeta(metaKey, DELETE);
        if (status != Status::SUCCESS) {
            ZLOGW("Remove Kvstore Metakey failed.");
        }
        KvStoreMetaManager::GetInstance().RemoveSecretKey(uid, bundleName, storeId.storeId);
        KvStoreMetaManager::GetInstance().DeleteStrategyMeta(bundleName, storeId.storeId, userId);
    }
    return status;
}

Status KvStoreDataService::DeleteKvStoreOnly(const std::string &bundleName, pid_t uid, const std::string &storeId)
{
    ZLOGI("DeleteKvStoreOnly begin.");
    auto userId = AccountDelegate::GetInstance()->GetDeviceAccountIdByUID(uid);
    auto it = deviceAccountMap_.find(userId);
    if (it != deviceAccountMap_.end()) {
        return (it->second).DeleteKvStore(bundleName, uid, storeId);
    }
    KvStoreUserManager kvStoreUserManager(userId);
    return kvStoreUserManager.DeleteKvStore(bundleName, uid, storeId);
}

void KvStoreDataService::AccountEventChanged(const AccountEventInfo &eventInfo)
{
    ZLOGI("account event %{public}d changed process, begin.", eventInfo.status);
    std::lock_guard<std::mutex> lg(accountMutex_);
    switch (eventInfo.status) {
        case AccountStatus::DEVICE_ACCOUNT_DELETE: {
            g_kvStoreAccountEventStatus = 1;
            // delete all kvstore belong to this device account
            for (auto &it : deviceAccountMap_) {
                (it.second).DeleteAllKvStore();
            }
            auto it = deviceAccountMap_.find(eventInfo.deviceAccountId);
            if (it != deviceAccountMap_.end()) {
                deviceAccountMap_.erase(eventInfo.deviceAccountId);
            }
            std::initializer_list<std::string> dirList = {Constant::ROOT_PATH_DE, "/",
                Constant::SERVICE_NAME, "/", eventInfo.deviceAccountId};
            std::string deviceAccountKvStoreDataDir = Constant::Concatenate(dirList);
            ForceRemoveDirectory(deviceAccountKvStoreDataDir);
            dirList = {Constant::ROOT_PATH_CE, "/", Constant::SERVICE_NAME, "/", eventInfo.deviceAccountId};
            deviceAccountKvStoreDataDir = Constant::Concatenate(dirList);
            ForceRemoveDirectory(deviceAccountKvStoreDataDir);
            g_kvStoreAccountEventStatus = 0;
            break;
        }
        case AccountStatus::DEVICE_ACCOUNT_SWITCHED: {
            auto ret = DistributedDB::KvStoreDelegateManager::NotifyUserChanged();
            ZLOGI("notify delegate manager result:%{public}d", ret);
            break;
        }
        default: {
            break;
        }
    }
    ZLOGI("account event %{public}d changed process, end.", eventInfo.status);
}

Status KvStoreDataService::GetLocalDevice(DeviceInfo &device)
{
    auto tmpDevice = AppDistributedKv::CommunicationProvider::GetInstance().GetLocalBasicInfo();
    device = {tmpDevice.deviceId, tmpDevice.deviceName, tmpDevice.deviceType};
    return Status::SUCCESS;
}

Status KvStoreDataService::GetDeviceList(std::vector<DeviceInfo> &deviceInfoList, DeviceFilterStrategy strategy)
{
    auto devices = AppDistributedKv::CommunicationProvider::GetInstance().GetRemoteNodesBasicInfo();
    for (auto const &device : devices) {
        DeviceInfo deviceInfo = {device.deviceId, device.deviceName, device.deviceType};
        deviceInfoList.push_back(deviceInfo);
    }
    ZLOGD("strategy is %{public}d.", strategy);
    return Status::SUCCESS;
}

void KvStoreDataService::InitSecurityAdapter()
{
    auto ret = DATASL_OnStart();
    ZLOGI("datasl on start ret:%d", ret);
    security_ = std::make_shared<Security>();
    if (security_ == nullptr) {
        ZLOGD("Security is nullptr.");
        return;
    }

    auto dbStatus = DistributedDB::KvStoreDelegateManager::SetProcessSystemAPIAdapter(security_);
    ZLOGD("set distributed db system api adapter: %d.", static_cast<int>(dbStatus));

    auto status = AppDistributedKv::CommunicationProvider::GetInstance().StartWatchDeviceChange(
        security_.get(), {"security"});
    if (status != AppDistributedKv::Status::SUCCESS) {
        ZLOGD("security register device change failed, status:%d", static_cast<int>(status));
    }
}

Status KvStoreDataService::StartWatchDeviceChange(sptr<IDeviceStatusChangeListener> observer,
                                                  DeviceFilterStrategy strategy)
{
    if (observer == nullptr) {
        ZLOGD("observer is null");
        return Status::INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lck(deviceListenerMutex_);
    if (deviceListener_ == nullptr) {
        deviceListener_ = std::make_shared<DeviceChangeListenerImpl>(deviceListeners_);
        AppDistributedKv::CommunicationProvider::GetInstance().StartWatchDeviceChange(
            deviceListener_.get(), {"serviceWatcher"});
    }
    IRemoteObject *objectPtr = observer->AsObject().GetRefPtr();
    auto listenerPair = std::make_pair(objectPtr, observer);
    deviceListeners_.insert(listenerPair);
    ZLOGD("strategy is %{public}d.", strategy);
    return Status::SUCCESS;
}

Status KvStoreDataService::StopWatchDeviceChange(sptr<IDeviceStatusChangeListener> observer)
{
    if (observer == nullptr) {
        ZLOGD("observer is null");
        return Status::INVALID_ARGUMENT;
    }
    std::lock_guard<std::mutex> lck(deviceListenerMutex_);
    IRemoteObject *objectPtr = observer->AsObject().GetRefPtr();
    auto it = deviceListeners_.find(objectPtr);
    if (it == deviceListeners_.end()) {
        return Status::ILLEGAL_STATE;
    }
    deviceListeners_.erase(it->first);
    return Status::SUCCESS;
}

bool KvStoreDataService::IsStoreOpened(const std::string &userId, const std::string &appId, const std::string &storeId)
{
    auto it = deviceAccountMap_.find(userId);
    return it != deviceAccountMap_.end() && it->second.IsStoreOpened(appId, storeId);
}

void KvStoreDataService::SetCompatibleIdentify(const AppDistributedKv::DeviceInfo &info) const
{
    for (const auto &item : deviceAccountMap_) {
        item.second.SetCompatibleIdentify(info.deviceId);
    }
}

bool KvStoreDataService::CheckSyncActivation(
    const std::string &userId, const std::string &appId, const std::string &storeId)
{
    ZLOGD("user:%{public}s, app:%{public}s, store:%{public}s", userId.c_str(), appId.c_str(), storeId.c_str());
    std::vector<UserStatus> users = UserDelegate::GetInstance().GetLocalUserStatus();
    // active sync feature with single active user
    for (const auto &user : users) {
        if (userId == std::to_string(user.id)) {
            if (!user.isActive) {
                ZLOGD("the store is not in active user");
                return false;
            }
            // check store in other active user
            continue;
        }
        if (IsStoreOpened(std::to_string(user.id), appId, storeId)) {
            ZLOGD("the store already opened in user %{public}d", user.id);
            return false;
        }
    }
    ZLOGD("sync permitted");
    return true;
}

void KvStoreDataService::CreateRdbService()
{
    rdbService_ = new(std::nothrow) DistributedRdb::RdbServiceImpl();
    if (rdbService_ != nullptr) {
        ZLOGI("create rdb service success");
    }
}

sptr<IRemoteObject> KvStoreDataService::GetRdbService()
{
    return rdbService_->AsObject().GetRefPtr();
}

bool DbMetaCallbackDelegateMgr::GetKvStoreDiskSize(const std::string &storeId, uint64_t &size)
{
    if (IsDestruct()) {
        return false;
    }
    DistributedDB::DBStatus ret = delegate_->GetKvStoreDiskSize(storeId, size);
    return (ret == DistributedDB::DBStatus::OK);
}

void DbMetaCallbackDelegateMgr::GetKvStoreKeys(std::vector<StoreInfo> &dbStats)
{
    if (IsDestruct()) {
        return;
    }
    DistributedDB::DBStatus dbStatusTmp;
    Option option {.createIfNecessary = true, .isMemoryDb = false, .isEncryptedDb = false};
    DistributedDB::KvStoreNbDelegate *kvStoreNbDelegatePtr = nullptr;
    delegate_->GetKvStore(
        Constant::SERVICE_META_DB_NAME, option,
        [&kvStoreNbDelegatePtr, &dbStatusTmp](DistributedDB::DBStatus dbStatus,
                                              DistributedDB::KvStoreNbDelegate *kvStoreNbDelegate) {
            kvStoreNbDelegatePtr = kvStoreNbDelegate;
            dbStatusTmp = dbStatus;
        });

    if (dbStatusTmp != DistributedDB::DBStatus::OK) {
        return;
    }
    DistributedDB::Key dbKey = KvStoreMetaRow::GetKeyFor("");
    std::vector<DistributedDB::Entry> entries;
    kvStoreNbDelegatePtr->GetEntries(dbKey, entries);
    if (entries.empty()) {
        delegate_->CloseKvStore(kvStoreNbDelegatePtr);
        return;
    }
    for (auto const &entry : entries) {
        std::string key = std::string(entry.key.begin(), entry.key.end());
        std::vector<std::string> out;
        Split(key, Constant::KEY_SEPARATOR, out);
        if (out.size() >= VECTOR_SIZE) {
            StoreInfo storeInfo = {out[USER_ID], out[APP_ID], out[STORE_ID]};
            dbStats.push_back(std::move(storeInfo));
        }
    }
    delegate_->CloseKvStore(kvStoreNbDelegatePtr);
}
} // namespace OHOS::DistributedKv
