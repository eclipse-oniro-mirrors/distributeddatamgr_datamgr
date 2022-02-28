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

#ifndef DISTRIBUTEDDATAMGR_USER_DELEGATE_H
#define DISTRIBUTEDDATAMGR_USER_DELEGATE_H

#include <memory>

#include "account_delegate.h"
#include "concurrent_map.h"
#include "metadata/user_meta_data.h"
#include "visibility.h"

namespace OHOS::DistributedData {
using AccountDelegate = DistributedKv::AccountDelegate;
using DistributedData::UserStatus;
class UserDelegate {
public:
    struct UserEvent {
        int id;
        bool isActive;
    };
    static UserDelegate &GetInstance();

    void Init();
    std::vector<UserStatus> GetLocalUserStatus();
    std::vector<UserStatus> GetRemoteUserStatus(const std::string &deviceId);
    bool InitLocalUserMeta();

private:
    class LocalUserObserver : public AccountDelegate::Observer {
    public:
        explicit LocalUserObserver(const std::shared_ptr<UserDelegate> &userDelegate);
        void OnAccountChanged(const DistributedKv::AccountEventInfo &eventInfo) override;
        std::string Name() override;

    private:
        std::shared_ptr<UserDelegate> userDelegate_;
    };
    std::vector<UserStatus> GetUsers(const std::string &deviceId);
    void UpdateUsers(const std::string &deviceId, const std::vector<UserStatus> &userStatus);
    void DeleteUsers(const std::string &deviceId);
    bool NotifyUserEvent(const UserEvent &userEvent);

    // device : { user : isActive }
    ConcurrentMap<std::string, std::map<int, bool>> deviceUserMap_;
};
} // namespace OHOS::DistributedData

#endif // DISTRIBUTEDDATAMGR_USER_DELEGATE_H
