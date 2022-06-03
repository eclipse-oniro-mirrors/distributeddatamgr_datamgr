/*
 * Copyright (c) 2022-2022 Huawei Device Co., Ltd.
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

#ifndef DISTRIBUTEDDATA_SERVICE_DUMPE_HELPER_H
#define DISTRIBUTEDDATA_SERVICE_DUMPE_HELPER_H

#include <string>
#include <vector>
#include <map>
#include <list>
#include "store_errno.h"
#include "singleton.h"

namespace OHOS {
namespace DistributedKv {
enum class DumpFlag {
    UNKNOW = 0,
    GET_HELP,
    GET_USER_INFO,
    GET_APP_INFO,
    GET_STORE_INFO,
    GET_ERROR_INFO,
};

struct HidumpParam {
    DumpFlag dumpFlag = DumpFlag::UNKNOW;
    std::string args;
};


class KvStoreDataService;
class DumpHelper : public Singleton<DumpHelper> {
public:
    DumpHelper() = default;
    virtual ~DumpHelper() = default;
    void AddErrorInfo(std::string &error);
    void ShowError(int fd);
    bool Dump(int fd, KvStoreDataService &kvStoreDataService, const std::vector<std::string> &args);

private:
    Status DumpAll(int fd, KvStoreDataService &kvStoreDataService);
    void ShowHelp(int fd);
    void ShowIllealInfomation(int fd);
    mutable std::mutex hidumperMutex_;
    std::list<std::string> g_errorInfo;
};
}  // namespace DistributedKv
}  // namespace OHOS
#endif  // DISTRIBUTEDDATA_SERVICE_DUMPE_HELPER_H

