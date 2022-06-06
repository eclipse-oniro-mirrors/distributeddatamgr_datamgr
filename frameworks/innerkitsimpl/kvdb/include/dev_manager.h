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

#ifndef OHOS_DISTRIBUTED_DATA_FRAMEWORKS_KVDB_DEV_MANAGER_H
#define OHOS_DISTRIBUTED_DATA_FRAMEWORKS_KVDB_DEV_MANAGER_H
#include <string>

#include "types.h"
#include "concurrent_map.h"
namespace OHOS::DistributedKv {
class API_EXPORT DevManager {
public:
    struct DeviceInfo {
        std::string uuid;
        std::string udid;
        std::string networkId;
    };
    static DevManager &GetInstance();
    std::string ToUUID(const std::string &networkId) const;
    DeviceInfo GetLocalDevice();
    std::vector<DeviceInfo> GetRemoteDevices() const;
    DeviceInfo GetDeviceInfo(const std::string &id) const;
    std::string ToNodeID(const std::string &nodeId) const;
private:
    DeviceInfo localInfo_ {};
    mutable ConcurrentMap<std::string, DeviceInfo> deviceInfos_ {};
    std::string GetUuidByNodeId(const std::string &nodeId) const;
    std::string GetUdidByNodeId(const std::string &nodeId) const;
    DeviceInfo GetDeviceInfoFromCache(const std::string &id) const;
    DeviceInfo GetDeviceCacheInfo(const std::string &id) const;
    void UpdateDeviceCacheInfo() const;
};
} // namespace OHOS::DistributedKv
#endif // OHOS_DISTRIBUTED_DATA_FRAMEWORKS_KVDB_DEV_MANAGER_H
