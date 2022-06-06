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

#define LOG_TAG "DevManagerTest"
#include <gtest/gtest.h>

#include "dev_manager.h"
#include "types.h"
#include "log_print.h"
using namespace testing::ext;
using namespace OHOS::DistributedKv;

class DevManagerTest : public testing::Test {
public:
    static void SetUpTestCase(void);
    static void TearDownTestCase(void);
    static DevManager manager;

    void SetUp();
    void TearDown();
};

void DevManagerTest::SetUpTestCase(void)
{}

void DevManagerTest::TearDownTestCase(void)
{}

void DevManagerTest::SetUp(void)
{}

void DevManagerTest::TearDown(void)
{}

/**
* @tc.name: GetLocalDevice001
* @tc.desc: Get local device's infomation
* @tc.type: FUNC
* @tc.require:
* @tc.author: taoyuxin
*/
HWTEST_F(DevManagerTest, GetLocalDevice001, TestSize.Level1)
{
    ZLOGI("GetLocalDevice001 begin.");
    DevManager &devManager = manager.GetInstance();
    DevManager::DeviceInfo devInfo = devManager.GetLocalDevice();

    EXPECT_NE(devInfo.networkId, "");
    EXPECT_NE(devInfo.uuid, "");
    EXPECT_NE(devInfo.udid, "");
}

/**
* @tc.name: ToUUID001
* @tc.desc: Get uuid from networkId
* @tc.type: FUNC
* @tc.require:
* @tc.author: taoyuxin
*/
HWTEST_F(DevManagerTest, ToUUID001, TestSize.Level1)
{
    ZLOGI("ToUUID001 begin.");
    DevManager &devManager = manager.GetInstance();
    DevManager::DeviceInfo devInfo = devManager.GetLocalDevice();
    EXPECT_NE(devInfo.networkId, "");
    std::string uuid = devManager.ToUUID(devInfo.networkId);
    EXPECT_EQ(uuid, "");
    EXPECT_NE(uuid, devInfo.uuid);
}

/**
* @tc.name: GetRemoteDevices001
* @tc.desc: Get remote devices
* @tc.type: FUNC
* @tc.require:
* @tc.author: taoyuxin
*/
HWTEST_F(DevManagerTest, GetRemoteDevices001, TestSize.Level1)
{
    ZLOGI("GetRemoteDevices001 begin.");
    DevManager &devManager = manager.GetInstance();
    vector<DevManager::DeviceInfo> devInfo = devManager.GetRemoteDevices();
    EXPECT_EQ(devInfo.size(), 0);
}

/**
* @tc.name: GetDeviceInfo001
* @tc.desc: Get device info from nodeId
* @tc.type: FUNC
* @tc.require:
* @tc.author: taoyuxin
*/
HWTEST_F(DevManagerTest, GetDeviceInfo001, TestSize.Level1)
{
    ZLOGI("GetDeviceInfo001 begin.");
    DevManager &devManager = manager.GetInstance();
    std::string id = "abc";
    DevManager::DeviceInfo devInfo = devManager.GetDeviceInfo(id);
    EXPECT_EQ(devInfo.networkId, "");
}

/**
* @tc.name: ToNodeId001
* @tc.desc: Get networkId from uuid or udid
* @tc.type: FUNC
* @tc.require:
* @tc.author: taoyuxin
*/
HWTEST_F(DevManagerTest, ToNodeId001, TestSize.Level1)
{
    ZLOGI("ToNodeId001 begin.");
    DevManager &devManager = manager.GetInstance();
    std::string id = "abc";
    std::string networkId = devManager.ToNodeID(id);
    EXPECT_EQ(networkId, "");
}