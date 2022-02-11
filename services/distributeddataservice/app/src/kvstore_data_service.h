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

#ifndef KVSTORE_DATASERVICE_H
#define KVSTORE_DATASERVICE_H

#include <map>
#include <set>
#include <mutex>
#include "constant.h"
#include "ikvstore_data_service.h"
#include "kvstore_impl.h"
#include "kvstore_user_manager.h"
#include "single_kvstore_impl.h"
#include "system_ability.h"
#include "reporter.h"
#include "types.h"
#include "account_delegate.h"
#include "backup_handler.h"
#include "device_change_listener_impl.h"

namespace OHOS::DistributedRdb {
class IRdbService;
class RdbServiceImpl;
}

namespace OHOS::DistributedKv {
class KvStoreAccountObserver;
class KvStoreDataService : public SystemAbility, public KvStoreDataServiceStub {
    DECLARE_SYSTEM_ABILITY(KvStoreDataService);

public:
    // record kvstore meta version for compatible, should update when modify kvstore meta structure.
    static constexpr uint32_t STORE_VERSION = 0x03000001;

    explicit KvStoreDataService(bool runOnCreate = false);
    explicit KvStoreDataService(int32_t systemAbilityId, bool runOnCreate = false);
    virtual ~KvStoreDataService();

    Status GetKvStore(const Options &options, const AppId &appId, const StoreId &storeId,
                      std::function<void(sptr<IKvStoreImpl>)> callback) override;

    Status GetSingleKvStore(const Options &options, const AppId &appId, const StoreId &storeId,
                      std::function<void(sptr<ISingleKvStore>)> callback) override;

    void GetAllKvStoreId(const AppId &appId, std::function<void(Status, std::vector<StoreId> &)> callback) override;

    Status CloseKvStore(const AppId &appId, const StoreId &storeId) override;

    Status CloseAllKvStore(const AppId &appId) override;

    Status DeleteKvStore(const AppId &appId, const StoreId &storeId) override;

    Status DeleteAllKvStore(const AppId &appId) override;

    Status RegisterClientDeathObserver(const AppId &appId, sptr<IRemoteObject> observer) override;

    Status GetLocalDevice(DeviceInfo &device) override;
    Status GetDeviceList(std::vector<DeviceInfo> &deviceInfoList, DeviceFilterStrategy strategy) override;
    Status StartWatchDeviceChange(sptr<IDeviceStatusChangeListener> observer, DeviceFilterStrategy strategy) override;
    Status StopWatchDeviceChange(sptr<IDeviceStatusChangeListener> observer) override;
    sptr<DistributedRdb::IRdbService> GetRdbService() override;
    
    void OnDump() override;

    int Dump(int fd, const std::vector<std::u16string> &args) override;

    void OnStart() override;

    void OnStop() override;

    Status DeleteKvStoreOnly(const std::string &storeId, const std::string &userId, const std::string &bundleName);

    void AccountEventChanged(const AccountEventInfo &eventInfo);

    bool CheckBackupFileExist(const std::string &userId, const std::string &bundleName,
                              const std::string &storeId, int pathType);

    struct KvStoreParam {
        std::string bundleName;
        std::string storeId;
        std::string trueAppId;
        std::string userId;
        pid_t uid;
        Status status = Status::SUCCESS;
    };
    struct SecretKeyPara {
        std::vector<uint8_t> metaKey;
        std::vector<uint8_t> secretKey;
        std::vector<uint8_t> metaSecretKey;
        std::string secretKeyFile;
        Status alreadyCreated = Status::SUCCESS;
        bool outdated = false;
    };
private:
    class KvStoreClientDeathObserverImpl {
    public:
        KvStoreClientDeathObserverImpl(const AppId &appId, KvStoreDataService &service, sptr<IRemoteObject> observer);

        virtual ~KvStoreClientDeathObserverImpl();

    private:
        class KvStoreDeathRecipient : public IRemoteObject::DeathRecipient {
        public:
            explicit KvStoreDeathRecipient(KvStoreClientDeathObserverImpl &kvStoreClientDeathObserverImpl);
            virtual ~KvStoreDeathRecipient();
            void OnRemoteDied(const wptr<IRemoteObject> &remote) override;

        private:
            KvStoreClientDeathObserverImpl &kvStoreClientDeathObserverImpl_;
        };
        void NotifyClientDie();
        AppId appId_;
        KvStoreDataService &dataService_;
        sptr<IRemoteObject> observerProxy_;
        sptr<KvStoreDeathRecipient> deathRecipient_;
    };

    void AddPermission() const;

    void Initialize();

    void StartService();

    Status DeleteKvStore(const std::string &bundleName, const StoreId &storeId);

    template<class T>
    Status RecoverKvStore(const Options &options, const std::string &bundleName, const std::string &storeId,
        const std::vector<uint8_t> &secretKey, sptr<T> &kvStore);
    Status GetSecretKey(const Options &options, const KvStoreParam &KvParas, SecretKeyPara &secretKeyParas);

    Status RecoverSecretKey(const Status &alreadyCreated, bool &outdated, const std::vector<uint8_t> &metaSecretKey,
        std::vector<uint8_t> &secretKey, const std::string &secretKeyFile);

    Status UpdateMetaData(const Options &options, const KvStoreParam &kvParas,
        const std::vector<uint8_t> &metaKey, KvStoreUserManager &kvStoreUserManager);

    Status GetKvStoreFailDo(const Options &options, const KvStoreParam &kvParas, SecretKeyPara &secKeyParas,
        KvStoreUserManager &kvUserManager, sptr<KvStoreImpl> &kvStore);

    Status GetSingleKvStoreFailDo(const Options &options, const KvStoreParam &kvParas, SecretKeyPara &secKeyParas,
        KvStoreUserManager &kvUserManager, sptr<SingleKvStoreImpl> &kvStore);

    Status AppExit(const AppId &appId);

    bool CheckPermissions(const std::string &userId, const std::string &appId, const std::string &storeId,
                          const std::string &deviceId, uint8_t flag) const;
    void ResolveAutoLaunchParamByIdentifier(const std::string &identifier, DistributedDB::AutoLaunchParam &param);

    bool CheckOptions(const Options &options, const std::vector<uint8_t> &metaKey) const;
    
    void CreateRdbService();

    static constexpr int TEN_SEC = 10;

    std::mutex accountMutex_;
    std::map<std::string, KvStoreUserManager> deviceAccountMap_;
    std::mutex clientDeathObserverMutex_;
    std::map<std::string, KvStoreClientDeathObserverImpl> clientDeathObserverMap_;
    std::shared_ptr<KvStoreAccountObserver> accountEventObserver_;
    std::unique_ptr<BackupHandler> backup_;
    std::map<IRemoteObject *, sptr<IDeviceStatusChangeListener>> deviceListeners_;
    std::mutex deviceListenerMutex_;
    std::shared_ptr<DeviceChangeListenerImpl> deviceListener_;
    
    sptr<DistributedRdb::RdbServiceImpl> rdbService_;
};

class DbMetaCallbackDelegateMgr : public DbMetaCallbackDelegate {
public:
    using Option = DistributedDB::KvStoreNbDelegate::Option;
    virtual ~DbMetaCallbackDelegateMgr() {}

    explicit DbMetaCallbackDelegateMgr(DistributedDB::KvStoreDelegateManager *delegate)
        : delegate_(delegate) {}
    bool GetKvStoreDiskSize(const std::string &storeId, uint64_t &size) override;
    void GetKvStoreKeys(std::vector<StoreInfo> &dbStats) override;
    bool IsDestruct()
    {
        return delegate_ == nullptr;
    }

private:
    void Split(const std::string &str, const std::string &delimiter, std::vector<std::string> &out)
    {
        size_t start;
        size_t end = 0;
        while ((start = str.find_first_not_of(delimiter, end)) != std::string::npos) {
            end = str.find(delimiter, start);
            if (end == std::string::npos) {
                end = str.size();
            }
            out.push_back(str.substr(start, end - start));
        }
    }

    DistributedDB::KvStoreDelegateManager *delegate_ {};
    static const inline int USER_ID = 0;
    static const inline int APP_ID = 1;
    static const inline int STORE_ID = 2;
    static const inline int VECTOR_SIZE = 2;
};
}
#endif  // KVSTORE_DATASERVICE_H
