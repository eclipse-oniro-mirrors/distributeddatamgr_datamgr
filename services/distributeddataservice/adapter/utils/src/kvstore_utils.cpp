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

#define LOG_TAG "KvStoreUtils"

#include "kvstore_utils.h"
#include "permission_validator.h"

namespace OHOS {
namespace DistributedKv {
constexpr int32_t HEAD_SIZE = 3;
constexpr int32_t END_SIZE = 3;
constexpr int32_t MIN_SIZE = HEAD_SIZE + END_SIZE + 3;
constexpr const char *REPLACE_CHAIN = "***";
constexpr const char *DEFAULT_ANONYMOUS = "******";
std::atomic<uint64_t> KvStoreUtils::sequenceId_{ 0 };
std::string KvStoreUtils::ToBeAnonymous(const std::string &name)
{
    if (name.length() <= HEAD_SIZE) {
        return DEFAULT_ANONYMOUS;
    }

    if (name.length() < MIN_SIZE) {
        return (name.substr(0, HEAD_SIZE) + REPLACE_CHAIN);
    }

    return (name.substr(0, HEAD_SIZE) + REPLACE_CHAIN + name.substr(name.length() - END_SIZE, END_SIZE));
}

AppDistributedKv::CommunicationProvider &KvStoreUtils::GetProviderInstance()
{
#ifdef CONFIG_PUBLIC_VERSION
    return AppDistributedKv::CommunicationProvider::GetInstance();
#else
    return *(AppDistributedKv::CommunicationProvider::MakeCommunicationProvider().get());
#endif
}

uint64_t KvStoreUtils::GenerateSequenceId()
{
    ++sequenceId_;
    return sequenceId_;
}
} // namespace DistributedKv
} // namespace OHOS
