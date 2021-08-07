/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <aidl/android/security/legacykeystore/ILegacyKeystore.h>
#include <aidl/android/system/keystore2/IKeystoreOperation.h>
#include <aidl/android/system/keystore2/IKeystoreSecurityLevel.h>
#include <aidl/android/system/keystore2/IKeystoreService.h>
#include <aidl/android/system/keystore2/ResponseCode.h>
#include <android/binder_manager.h>
#include <android/system/wifi/keystore/1.0/IKeystore.h>
#include <binder/IServiceManager.h>
#include <cutils/properties.h>
#include <gtest/gtest.h>
#include <hidl/GtestPrinter.h>
#include <hidl/ServiceManagement.h>
#include <private/android_filesystem_config.h>
#include <utils/String16.h>

using namespace std;
using namespace ::testing;
using namespace android;
using android::system::wifi::keystore::V1_0::IKeystore;

namespace lks = ::aidl::android::security::legacykeystore;
namespace ks2 = ::aidl::android::system::keystore2;

int main(int argc, char** argv) {
    InitGoogleTest(&argc, argv);
    int status = RUN_ALL_TESTS();
    return status;
}

namespace {

enum KeyPurpose {
    ENCRYPTION,
    SIGNING,
};

// The fixture for testing the Wifi Keystore HAL legacy keystore integration.
class WifiLegacyKeystoreTest : public TestWithParam<std::string> {
   protected:
    void SetUp() override {
        wifiKeystoreHal = IKeystore::getService(GetParam());
        ASSERT_TRUE(wifiKeystoreHal);

        myRUid = getuid();
    }

    void TearDown() override {
        if (getuid() != myRUid) {
            ASSERT_EQ(0, seteuid(myRUid));
        }
    }

    bool isDebuggableBuild() {
        char value[PROPERTY_VALUE_MAX] = {0};
        property_get("ro.system.build.type", value, "");
        if (strcmp(value, "userdebug") == 0) {
            return true;
        }
        if (strcmp(value, "eng") == 0) {
            return true;
        }
        return false;
    }

    sp<IKeystore> wifiKeystoreHal;
    uid_t myRUid;
};

INSTANTIATE_TEST_SUITE_P(
    PerInstance, WifiLegacyKeystoreTest,
    testing::ValuesIn(android::hardware::getAllHalInstanceNames(IKeystore::descriptor)),
    android::hardware::PrintInstanceNameToString);

constexpr const char kLegacyKeystoreServiceName[] = "android.security.legacykeystore";

static bool LegacyKeystoreRemove(const std::string& alias,
                                 int uid = lks::ILegacyKeystore::UID_SELF) {
    ::ndk::SpAIBinder keystoreBinder(AServiceManager_checkService(kLegacyKeystoreServiceName));
    auto legacyKeystore = lks::ILegacyKeystore::fromBinder(keystoreBinder);

    EXPECT_TRUE((bool)legacyKeystore);
    if (!legacyKeystore) {
        return false;
    }

    auto rc = legacyKeystore->remove(alias, uid);
    // Either the entry was successfully removed or the entry was not found.
    bool outcome =
        rc.isOk() || rc.getServiceSpecificError() == lks::ILegacyKeystore::ERROR_ENTRY_NOT_FOUND;
    EXPECT_TRUE(outcome) << "Description: " << rc.getDescription();
    return outcome;
}

static bool LegacyKeystorePut(const std::string& alias, const std::vector<uint8_t>& blob,
                              int uid = lks::ILegacyKeystore::UID_SELF) {
    ::ndk::SpAIBinder keystoreBinder(AServiceManager_checkService(kLegacyKeystoreServiceName));
    auto legacyKeystore = lks::ILegacyKeystore::fromBinder(keystoreBinder);

    EXPECT_TRUE((bool)legacyKeystore);
    if (!legacyKeystore) {
        return false;
    }

    auto rc = legacyKeystore->put(alias, uid, blob);
    EXPECT_TRUE(rc.isOk()) << "Description: " << rc.getDescription();
    return rc.isOk();
}

static std::optional<std::vector<uint8_t>> LegacyKeystoreGet(
    const std::string& alias, int uid = lks::ILegacyKeystore::UID_SELF) {
    ::ndk::SpAIBinder keystoreBinder(AServiceManager_checkService(kLegacyKeystoreServiceName));
    auto legacyKeystore = lks::ILegacyKeystore::fromBinder(keystoreBinder);

    EXPECT_TRUE((bool)legacyKeystore);
    if (!legacyKeystore) {
        return std::nullopt;
    }

    std::optional<std::vector<uint8_t>> blob(std::vector<uint8_t>{});
    auto rc = legacyKeystore->get(alias, uid, &*blob);
    EXPECT_TRUE(rc.isOk()) << "Description: " << rc.getDescription();
    return blob;
}

TEST_P(WifiLegacyKeystoreTest, Put_get_test) {
    if (!isDebuggableBuild() || getuid() != 0) {
        GTEST_SKIP() << "Device not running a debuggable build or not running as root. "
                     << "Cannot transition to AID_SYSTEM.";
    }

    // Only AID_SYSTEM (and AID_WIFI) is allowed to manipulate
    ASSERT_EQ(0, seteuid(AID_SYSTEM)) << "Failed to set uid to AID_SYSTEM: " << strerror(errno);

    const std::vector<uint8_t> TESTBLOB{1, 2, 3, 4};
    const std::string TESTALIAS = "LegacyKeystoreTestAlias";
    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
    ASSERT_TRUE(LegacyKeystorePut(TESTALIAS, TESTBLOB));
    auto blob = LegacyKeystoreGet(TESTALIAS);
    ASSERT_TRUE((bool)blob);
    ASSERT_EQ(*blob, TESTBLOB);
    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
}

TEST_P(WifiLegacyKeystoreTest, GetLegacyKeystoreTest) {
    if (!isDebuggableBuild() || getuid() != 0) {
        GTEST_SKIP() << "Device not running a debuggable build or not running as root. "
                     << "Cannot transition to AID_SYSTEM.";
    }

    // Only AID_SYSTEM (and AID_WIFI) is allowed to manipulate
    ASSERT_EQ(0, seteuid(AID_SYSTEM)) << "Failed to set uid to AID_SYSTEM: " << strerror(errno);

    // Some test certificate in PEM encoding.
    static const char kTestBlob[] = R"(-----BEGIN CERTIFICATE-----
MIICWDCCAcGgAwIBAgIUMpH52TRcL1gTknsm5eR+wvCGxNMwDQYJKoZIhvcNAQEL
BQAwPjELMAkGA1UEBhMCVVMxEzARBgNVBAgMClNvbWUtU3RhdGUxGjAYBgNVBAoM
EUFuZHJvaWQgVGVzdCBDZXJ0MB4XDTIxMDczMDAwMzY1OVoXDTIyMDczMDAwMzY1
OVowPjELMAkGA1UEBhMCVVMxEzARBgNVBAgMClNvbWUtU3RhdGUxGjAYBgNVBAoM
EUFuZHJvaWQgVGVzdCBDZXJ0MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDL
q7JTXvL3ErVX2ZU9hQ0PLnkyw984qweNhQw8xIvwzTs3hXtV0K4hmWJiPKxOv3H7
Q//TOcxI6+Qp4qOa79UUYDvmObjOCW1jQvZ9UQQfvdMO1WSa3BQoPJYQXiuyiuPs
+XM58Yl8TPV+IQ+Znx5axn5PxEmoqCUmeBv/wbJlDwIDAQABo1MwUTAdBgNVHQ4E
FgQUEbhF5fYkUPchj+GdWX1aoOHkH3owHwYDVR0jBBgwFoAUEbhF5fYkUPchj+Gd
WX1aoOHkH3owDwYDVR0TAQH/BAUwAwEB/zANBgkqhkiG9w0BAQsFAAOBgQChejph
iYWFBeQEQtPYGGwSNO1HgzRhvsdGKDJUtRDAvDPxlRO8jkGmrSaD3QJUY4bCkx5c
S9W7oRxyiUaxJFtw9Lbxkc4G3v0hpxYqfX4R4lzM8oU/50cPEpZGVaIZNrqBiXbd
wFzPSv/UTXFBKlR5grYTmsiHCBbEv0apNJNI0g==
-----END CERTIFICATE-----
)";
    const std::vector<uint8_t> TESTBLOB(std::begin(kTestBlob), std::end(kTestBlob));
    const std::string TESTALIAS = "LegacyKeystoreWifiTestAlias";

    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
    ASSERT_TRUE(LegacyKeystorePut(TESTALIAS, TESTBLOB, AID_WIFI));

    IKeystore::KeystoreStatusCode statusCode;
    std::vector<uint8_t> blob;
    auto rc = wifiKeystoreHal->getBlob(TESTALIAS,
                                       [&](IKeystore::KeystoreStatusCode status,
                                           const ::android::hardware::hidl_vec<uint8_t>& value) {
                                           statusCode = status;
                                           blob = value;
                                       });

    ASSERT_TRUE(rc.isOk()) << "Description: " << rc.description();
    ASSERT_EQ(IKeystore::KeystoreStatusCode::SUCCESS, statusCode);
    ASSERT_EQ(TESTBLOB, blob);

    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
}

/*
 * This tests checks that a DER encoded certificate is always returned in PEM encoding by getBlob.
 */
TEST_P(WifiLegacyKeystoreTest, IKeystoreGetAlwaysReturnsPem) {
    if (!isDebuggableBuild() || getuid() != 0) {
        GTEST_SKIP() << "Device not running a debuggable build or not running as root. "
                     << "Cannot transition to AID_SYSTEM.";
    }

    // Only AID_SYSTEM (and AID_WIFI) is allowed to manipulate
    ASSERT_EQ(0, seteuid(AID_SYSTEM)) << "Failed to set uid to AID_SYSTEM: " << strerror(errno);

    // Some test certificate in DER encoding.
    const std::vector<uint8_t> TESTBLOB_DER{
        0x30, 0x82, 0x02, 0x58, 0x30, 0x82, 0x01, 0xc1, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14,
        0x32, 0x91, 0xf9, 0xd9, 0x34, 0x5c, 0x2f, 0x58, 0x13, 0x92, 0x7b, 0x26, 0xe5, 0xe4, 0x7e,
        0xc2, 0xf0, 0x86, 0xc4, 0xd3, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
        0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x3e, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04,
        0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x0c,
        0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61, 0x74, 0x65, 0x31, 0x1a, 0x30, 0x18,
        0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e, 0x64, 0x72, 0x6f, 0x69, 0x64, 0x20,
        0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x31,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x17, 0x0d, 0x32, 0x32,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x30, 0x3e, 0x31, 0x0b,
        0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11,
        0x06, 0x03, 0x55, 0x04, 0x08, 0x0c, 0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61,
        0x74, 0x65, 0x31, 0x1a, 0x30, 0x18, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e,
        0x64, 0x72, 0x6f, 0x69, 0x64, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74,
        0x30, 0x81, 0x9f, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
        0x01, 0x05, 0x00, 0x03, 0x81, 0x8d, 0x00, 0x30, 0x81, 0x89, 0x02, 0x81, 0x81, 0x00, 0xcb,
        0xab, 0xb2, 0x53, 0x5e, 0xf2, 0xf7, 0x12, 0xb5, 0x57, 0xd9, 0x95, 0x3d, 0x85, 0x0d, 0x0f,
        0x2e, 0x79, 0x32, 0xc3, 0xdf, 0x38, 0xab, 0x07, 0x8d, 0x85, 0x0c, 0x3c, 0xc4, 0x8b, 0xf0,
        0xcd, 0x3b, 0x37, 0x85, 0x7b, 0x55, 0xd0, 0xae, 0x21, 0x99, 0x62, 0x62, 0x3c, 0xac, 0x4e,
        0xbf, 0x71, 0xfb, 0x43, 0xff, 0xd3, 0x39, 0xcc, 0x48, 0xeb, 0xe4, 0x29, 0xe2, 0xa3, 0x9a,
        0xef, 0xd5, 0x14, 0x60, 0x3b, 0xe6, 0x39, 0xb8, 0xce, 0x09, 0x6d, 0x63, 0x42, 0xf6, 0x7d,
        0x51, 0x04, 0x1f, 0xbd, 0xd3, 0x0e, 0xd5, 0x64, 0x9a, 0xdc, 0x14, 0x28, 0x3c, 0x96, 0x10,
        0x5e, 0x2b, 0xb2, 0x8a, 0xe3, 0xec, 0xf9, 0x73, 0x39, 0xf1, 0x89, 0x7c, 0x4c, 0xf5, 0x7e,
        0x21, 0x0f, 0x99, 0x9f, 0x1e, 0x5a, 0xc6, 0x7e, 0x4f, 0xc4, 0x49, 0xa8, 0xa8, 0x25, 0x26,
        0x78, 0x1b, 0xff, 0xc1, 0xb2, 0x65, 0x0f, 0x02, 0x03, 0x01, 0x00, 0x01, 0xa3, 0x53, 0x30,
        0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x11, 0xb8, 0x45,
        0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a, 0xa0, 0xe1, 0xe4,
        0x1f, 0x7a, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
        0x11, 0xb8, 0x45, 0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a,
        0xa0, 0xe1, 0xe4, 0x1f, 0x7a, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff,
        0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
        0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x03, 0x81, 0x81, 0x00, 0xa1, 0x7a, 0x3a, 0x61,
        0x89, 0x85, 0x85, 0x05, 0xe4, 0x04, 0x42, 0xd3, 0xd8, 0x18, 0x6c, 0x12, 0x34, 0xed, 0x47,
        0x83, 0x34, 0x61, 0xbe, 0xc7, 0x46, 0x28, 0x32, 0x54, 0xb5, 0x10, 0xc0, 0xbc, 0x33, 0xf1,
        0x95, 0x13, 0xbc, 0x8e, 0x41, 0xa6, 0xad, 0x26, 0x83, 0xdd, 0x02, 0x54, 0x63, 0x86, 0xc2,
        0x93, 0x1e, 0x5c, 0x4b, 0xd5, 0xbb, 0xa1, 0x1c, 0x72, 0x89, 0x46, 0xb1, 0x24, 0x5b, 0x70,
        0xf4, 0xb6, 0xf1, 0x91, 0xce, 0x06, 0xde, 0xfd, 0x21, 0xa7, 0x16, 0x2a, 0x7d, 0x7e, 0x11,
        0xe2, 0x5c, 0xcc, 0xf2, 0x85, 0x3f, 0xe7, 0x47, 0x0f, 0x12, 0x96, 0x46, 0x55, 0xa2, 0x19,
        0x36, 0xba, 0x81, 0x89, 0x76, 0xdd, 0xc0, 0x5c, 0xcf, 0x4a, 0xff, 0xd4, 0x4d, 0x71, 0x41,
        0x2a, 0x54, 0x79, 0x82, 0xb6, 0x13, 0x9a, 0xc8, 0x87, 0x08, 0x16, 0xc4, 0xbf, 0x46, 0xa9,
        0x34, 0x93, 0x48, 0xd2};

    const std::string TESTALIAS = "LegacyKeystoreWifiTestAlias";

    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
    ASSERT_TRUE(LegacyKeystorePut(TESTALIAS, TESTBLOB_DER, AID_WIFI));

    IKeystore::KeystoreStatusCode statusCode;
    std::vector<uint8_t> blob;
    auto rc = wifiKeystoreHal->getBlob(TESTALIAS,
                                       [&](IKeystore::KeystoreStatusCode status,
                                           const ::android::hardware::hidl_vec<uint8_t>& value) {
                                           statusCode = status;
                                           blob = value;
                                       });

    ASSERT_TRUE(rc.isOk()) << "Description: " << rc.description();
    ASSERT_EQ(IKeystore::KeystoreStatusCode::SUCCESS, statusCode);

    std::string blob_str(reinterpret_cast<const char*>(blob.data()),
                         reinterpret_cast<const char*>(blob.data()) + blob.size());
    ASSERT_EQ(blob_str.rfind("-----BEGIN CERTIFICATE-----", 0), 0);
    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
}

/*
 * This tests checks that a DER encoded certificate is always returned in PEM encoding by getBlob.
 */
TEST_P(WifiLegacyKeystoreTest, IKeystoreGetAlwaysReturnsPemWithChain) {
    if (!isDebuggableBuild() || getuid() != 0) {
        GTEST_SKIP() << "Device not running a debuggable build or not running as root. "
                     << "Cannot transition to AID_SYSTEM.";
    }

    // Only AID_SYSTEM (and AID_WIFI) is allowed to manipulate
    ASSERT_EQ(0, seteuid(AID_SYSTEM)) << "Failed to set uid to AID_SYSTEM: " << strerror(errno);

    // Some test certificate in DER encoding, this is three times the same cert.
    const std::vector<uint8_t> TESTBLOB_DER_3CERT{
        0x30, 0x82, 0x02, 0x58, 0x30, 0x82, 0x01, 0xc1, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14,
        0x32, 0x91, 0xf9, 0xd9, 0x34, 0x5c, 0x2f, 0x58, 0x13, 0x92, 0x7b, 0x26, 0xe5, 0xe4, 0x7e,
        0xc2, 0xf0, 0x86, 0xc4, 0xd3, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
        0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x3e, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04,
        0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x0c,
        0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61, 0x74, 0x65, 0x31, 0x1a, 0x30, 0x18,
        0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e, 0x64, 0x72, 0x6f, 0x69, 0x64, 0x20,
        0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x31,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x17, 0x0d, 0x32, 0x32,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x30, 0x3e, 0x31, 0x0b,
        0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11,
        0x06, 0x03, 0x55, 0x04, 0x08, 0x0c, 0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61,
        0x74, 0x65, 0x31, 0x1a, 0x30, 0x18, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e,
        0x64, 0x72, 0x6f, 0x69, 0x64, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74,
        0x30, 0x81, 0x9f, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
        0x01, 0x05, 0x00, 0x03, 0x81, 0x8d, 0x00, 0x30, 0x81, 0x89, 0x02, 0x81, 0x81, 0x00, 0xcb,
        0xab, 0xb2, 0x53, 0x5e, 0xf2, 0xf7, 0x12, 0xb5, 0x57, 0xd9, 0x95, 0x3d, 0x85, 0x0d, 0x0f,
        0x2e, 0x79, 0x32, 0xc3, 0xdf, 0x38, 0xab, 0x07, 0x8d, 0x85, 0x0c, 0x3c, 0xc4, 0x8b, 0xf0,
        0xcd, 0x3b, 0x37, 0x85, 0x7b, 0x55, 0xd0, 0xae, 0x21, 0x99, 0x62, 0x62, 0x3c, 0xac, 0x4e,
        0xbf, 0x71, 0xfb, 0x43, 0xff, 0xd3, 0x39, 0xcc, 0x48, 0xeb, 0xe4, 0x29, 0xe2, 0xa3, 0x9a,
        0xef, 0xd5, 0x14, 0x60, 0x3b, 0xe6, 0x39, 0xb8, 0xce, 0x09, 0x6d, 0x63, 0x42, 0xf6, 0x7d,
        0x51, 0x04, 0x1f, 0xbd, 0xd3, 0x0e, 0xd5, 0x64, 0x9a, 0xdc, 0x14, 0x28, 0x3c, 0x96, 0x10,
        0x5e, 0x2b, 0xb2, 0x8a, 0xe3, 0xec, 0xf9, 0x73, 0x39, 0xf1, 0x89, 0x7c, 0x4c, 0xf5, 0x7e,
        0x21, 0x0f, 0x99, 0x9f, 0x1e, 0x5a, 0xc6, 0x7e, 0x4f, 0xc4, 0x49, 0xa8, 0xa8, 0x25, 0x26,
        0x78, 0x1b, 0xff, 0xc1, 0xb2, 0x65, 0x0f, 0x02, 0x03, 0x01, 0x00, 0x01, 0xa3, 0x53, 0x30,
        0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x11, 0xb8, 0x45,
        0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a, 0xa0, 0xe1, 0xe4,
        0x1f, 0x7a, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
        0x11, 0xb8, 0x45, 0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a,
        0xa0, 0xe1, 0xe4, 0x1f, 0x7a, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff,
        0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
        0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x03, 0x81, 0x81, 0x00, 0xa1, 0x7a, 0x3a, 0x61,
        0x89, 0x85, 0x85, 0x05, 0xe4, 0x04, 0x42, 0xd3, 0xd8, 0x18, 0x6c, 0x12, 0x34, 0xed, 0x47,
        0x83, 0x34, 0x61, 0xbe, 0xc7, 0x46, 0x28, 0x32, 0x54, 0xb5, 0x10, 0xc0, 0xbc, 0x33, 0xf1,
        0x95, 0x13, 0xbc, 0x8e, 0x41, 0xa6, 0xad, 0x26, 0x83, 0xdd, 0x02, 0x54, 0x63, 0x86, 0xc2,
        0x93, 0x1e, 0x5c, 0x4b, 0xd5, 0xbb, 0xa1, 0x1c, 0x72, 0x89, 0x46, 0xb1, 0x24, 0x5b, 0x70,
        0xf4, 0xb6, 0xf1, 0x91, 0xce, 0x06, 0xde, 0xfd, 0x21, 0xa7, 0x16, 0x2a, 0x7d, 0x7e, 0x11,
        0xe2, 0x5c, 0xcc, 0xf2, 0x85, 0x3f, 0xe7, 0x47, 0x0f, 0x12, 0x96, 0x46, 0x55, 0xa2, 0x19,
        0x36, 0xba, 0x81, 0x89, 0x76, 0xdd, 0xc0, 0x5c, 0xcf, 0x4a, 0xff, 0xd4, 0x4d, 0x71, 0x41,
        0x2a, 0x54, 0x79, 0x82, 0xb6, 0x13, 0x9a, 0xc8, 0x87, 0x08, 0x16, 0xc4, 0xbf, 0x46, 0xa9,
        0x34, 0x93, 0x48, 0xd2,  // End of first cert.
        0x30, 0x82, 0x02, 0x58, 0x30, 0x82, 0x01, 0xc1, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14,
        0x32, 0x91, 0xf9, 0xd9, 0x34, 0x5c, 0x2f, 0x58, 0x13, 0x92, 0x7b, 0x26, 0xe5, 0xe4, 0x7e,
        0xc2, 0xf0, 0x86, 0xc4, 0xd3, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
        0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x3e, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04,
        0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x0c,
        0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61, 0x74, 0x65, 0x31, 0x1a, 0x30, 0x18,
        0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e, 0x64, 0x72, 0x6f, 0x69, 0x64, 0x20,
        0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x31,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x17, 0x0d, 0x32, 0x32,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x30, 0x3e, 0x31, 0x0b,
        0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11,
        0x06, 0x03, 0x55, 0x04, 0x08, 0x0c, 0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61,
        0x74, 0x65, 0x31, 0x1a, 0x30, 0x18, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e,
        0x64, 0x72, 0x6f, 0x69, 0x64, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74,
        0x30, 0x81, 0x9f, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
        0x01, 0x05, 0x00, 0x03, 0x81, 0x8d, 0x00, 0x30, 0x81, 0x89, 0x02, 0x81, 0x81, 0x00, 0xcb,
        0xab, 0xb2, 0x53, 0x5e, 0xf2, 0xf7, 0x12, 0xb5, 0x57, 0xd9, 0x95, 0x3d, 0x85, 0x0d, 0x0f,
        0x2e, 0x79, 0x32, 0xc3, 0xdf, 0x38, 0xab, 0x07, 0x8d, 0x85, 0x0c, 0x3c, 0xc4, 0x8b, 0xf0,
        0xcd, 0x3b, 0x37, 0x85, 0x7b, 0x55, 0xd0, 0xae, 0x21, 0x99, 0x62, 0x62, 0x3c, 0xac, 0x4e,
        0xbf, 0x71, 0xfb, 0x43, 0xff, 0xd3, 0x39, 0xcc, 0x48, 0xeb, 0xe4, 0x29, 0xe2, 0xa3, 0x9a,
        0xef, 0xd5, 0x14, 0x60, 0x3b, 0xe6, 0x39, 0xb8, 0xce, 0x09, 0x6d, 0x63, 0x42, 0xf6, 0x7d,
        0x51, 0x04, 0x1f, 0xbd, 0xd3, 0x0e, 0xd5, 0x64, 0x9a, 0xdc, 0x14, 0x28, 0x3c, 0x96, 0x10,
        0x5e, 0x2b, 0xb2, 0x8a, 0xe3, 0xec, 0xf9, 0x73, 0x39, 0xf1, 0x89, 0x7c, 0x4c, 0xf5, 0x7e,
        0x21, 0x0f, 0x99, 0x9f, 0x1e, 0x5a, 0xc6, 0x7e, 0x4f, 0xc4, 0x49, 0xa8, 0xa8, 0x25, 0x26,
        0x78, 0x1b, 0xff, 0xc1, 0xb2, 0x65, 0x0f, 0x02, 0x03, 0x01, 0x00, 0x01, 0xa3, 0x53, 0x30,
        0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x11, 0xb8, 0x45,
        0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a, 0xa0, 0xe1, 0xe4,
        0x1f, 0x7a, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
        0x11, 0xb8, 0x45, 0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a,
        0xa0, 0xe1, 0xe4, 0x1f, 0x7a, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff,
        0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
        0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x03, 0x81, 0x81, 0x00, 0xa1, 0x7a, 0x3a, 0x61,
        0x89, 0x85, 0x85, 0x05, 0xe4, 0x04, 0x42, 0xd3, 0xd8, 0x18, 0x6c, 0x12, 0x34, 0xed, 0x47,
        0x83, 0x34, 0x61, 0xbe, 0xc7, 0x46, 0x28, 0x32, 0x54, 0xb5, 0x10, 0xc0, 0xbc, 0x33, 0xf1,
        0x95, 0x13, 0xbc, 0x8e, 0x41, 0xa6, 0xad, 0x26, 0x83, 0xdd, 0x02, 0x54, 0x63, 0x86, 0xc2,
        0x93, 0x1e, 0x5c, 0x4b, 0xd5, 0xbb, 0xa1, 0x1c, 0x72, 0x89, 0x46, 0xb1, 0x24, 0x5b, 0x70,
        0xf4, 0xb6, 0xf1, 0x91, 0xce, 0x06, 0xde, 0xfd, 0x21, 0xa7, 0x16, 0x2a, 0x7d, 0x7e, 0x11,
        0xe2, 0x5c, 0xcc, 0xf2, 0x85, 0x3f, 0xe7, 0x47, 0x0f, 0x12, 0x96, 0x46, 0x55, 0xa2, 0x19,
        0x36, 0xba, 0x81, 0x89, 0x76, 0xdd, 0xc0, 0x5c, 0xcf, 0x4a, 0xff, 0xd4, 0x4d, 0x71, 0x41,
        0x2a, 0x54, 0x79, 0x82, 0xb6, 0x13, 0x9a, 0xc8, 0x87, 0x08, 0x16, 0xc4, 0xbf, 0x46, 0xa9,
        0x34, 0x93, 0x48, 0xd2,  // End of second.
        0x30, 0x82, 0x02, 0x58, 0x30, 0x82, 0x01, 0xc1, 0xa0, 0x03, 0x02, 0x01, 0x02, 0x02, 0x14,
        0x32, 0x91, 0xf9, 0xd9, 0x34, 0x5c, 0x2f, 0x58, 0x13, 0x92, 0x7b, 0x26, 0xe5, 0xe4, 0x7e,
        0xc2, 0xf0, 0x86, 0xc4, 0xd3, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d,
        0x01, 0x01, 0x0b, 0x05, 0x00, 0x30, 0x3e, 0x31, 0x0b, 0x30, 0x09, 0x06, 0x03, 0x55, 0x04,
        0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11, 0x06, 0x03, 0x55, 0x04, 0x08, 0x0c,
        0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61, 0x74, 0x65, 0x31, 0x1a, 0x30, 0x18,
        0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e, 0x64, 0x72, 0x6f, 0x69, 0x64, 0x20,
        0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74, 0x30, 0x1e, 0x17, 0x0d, 0x32, 0x31,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x17, 0x0d, 0x32, 0x32,
        0x30, 0x37, 0x33, 0x30, 0x30, 0x30, 0x33, 0x36, 0x35, 0x39, 0x5a, 0x30, 0x3e, 0x31, 0x0b,
        0x30, 0x09, 0x06, 0x03, 0x55, 0x04, 0x06, 0x13, 0x02, 0x55, 0x53, 0x31, 0x13, 0x30, 0x11,
        0x06, 0x03, 0x55, 0x04, 0x08, 0x0c, 0x0a, 0x53, 0x6f, 0x6d, 0x65, 0x2d, 0x53, 0x74, 0x61,
        0x74, 0x65, 0x31, 0x1a, 0x30, 0x18, 0x06, 0x03, 0x55, 0x04, 0x0a, 0x0c, 0x11, 0x41, 0x6e,
        0x64, 0x72, 0x6f, 0x69, 0x64, 0x20, 0x54, 0x65, 0x73, 0x74, 0x20, 0x43, 0x65, 0x72, 0x74,
        0x30, 0x81, 0x9f, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01,
        0x01, 0x05, 0x00, 0x03, 0x81, 0x8d, 0x00, 0x30, 0x81, 0x89, 0x02, 0x81, 0x81, 0x00, 0xcb,
        0xab, 0xb2, 0x53, 0x5e, 0xf2, 0xf7, 0x12, 0xb5, 0x57, 0xd9, 0x95, 0x3d, 0x85, 0x0d, 0x0f,
        0x2e, 0x79, 0x32, 0xc3, 0xdf, 0x38, 0xab, 0x07, 0x8d, 0x85, 0x0c, 0x3c, 0xc4, 0x8b, 0xf0,
        0xcd, 0x3b, 0x37, 0x85, 0x7b, 0x55, 0xd0, 0xae, 0x21, 0x99, 0x62, 0x62, 0x3c, 0xac, 0x4e,
        0xbf, 0x71, 0xfb, 0x43, 0xff, 0xd3, 0x39, 0xcc, 0x48, 0xeb, 0xe4, 0x29, 0xe2, 0xa3, 0x9a,
        0xef, 0xd5, 0x14, 0x60, 0x3b, 0xe6, 0x39, 0xb8, 0xce, 0x09, 0x6d, 0x63, 0x42, 0xf6, 0x7d,
        0x51, 0x04, 0x1f, 0xbd, 0xd3, 0x0e, 0xd5, 0x64, 0x9a, 0xdc, 0x14, 0x28, 0x3c, 0x96, 0x10,
        0x5e, 0x2b, 0xb2, 0x8a, 0xe3, 0xec, 0xf9, 0x73, 0x39, 0xf1, 0x89, 0x7c, 0x4c, 0xf5, 0x7e,
        0x21, 0x0f, 0x99, 0x9f, 0x1e, 0x5a, 0xc6, 0x7e, 0x4f, 0xc4, 0x49, 0xa8, 0xa8, 0x25, 0x26,
        0x78, 0x1b, 0xff, 0xc1, 0xb2, 0x65, 0x0f, 0x02, 0x03, 0x01, 0x00, 0x01, 0xa3, 0x53, 0x30,
        0x51, 0x30, 0x1d, 0x06, 0x03, 0x55, 0x1d, 0x0e, 0x04, 0x16, 0x04, 0x14, 0x11, 0xb8, 0x45,
        0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a, 0xa0, 0xe1, 0xe4,
        0x1f, 0x7a, 0x30, 0x1f, 0x06, 0x03, 0x55, 0x1d, 0x23, 0x04, 0x18, 0x30, 0x16, 0x80, 0x14,
        0x11, 0xb8, 0x45, 0xe5, 0xf6, 0x24, 0x50, 0xf7, 0x21, 0x8f, 0xe1, 0x9d, 0x59, 0x7d, 0x5a,
        0xa0, 0xe1, 0xe4, 0x1f, 0x7a, 0x30, 0x0f, 0x06, 0x03, 0x55, 0x1d, 0x13, 0x01, 0x01, 0xff,
        0x04, 0x05, 0x30, 0x03, 0x01, 0x01, 0xff, 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
        0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00, 0x03, 0x81, 0x81, 0x00, 0xa1, 0x7a, 0x3a, 0x61,
        0x89, 0x85, 0x85, 0x05, 0xe4, 0x04, 0x42, 0xd3, 0xd8, 0x18, 0x6c, 0x12, 0x34, 0xed, 0x47,
        0x83, 0x34, 0x61, 0xbe, 0xc7, 0x46, 0x28, 0x32, 0x54, 0xb5, 0x10, 0xc0, 0xbc, 0x33, 0xf1,
        0x95, 0x13, 0xbc, 0x8e, 0x41, 0xa6, 0xad, 0x26, 0x83, 0xdd, 0x02, 0x54, 0x63, 0x86, 0xc2,
        0x93, 0x1e, 0x5c, 0x4b, 0xd5, 0xbb, 0xa1, 0x1c, 0x72, 0x89, 0x46, 0xb1, 0x24, 0x5b, 0x70,
        0xf4, 0xb6, 0xf1, 0x91, 0xce, 0x06, 0xde, 0xfd, 0x21, 0xa7, 0x16, 0x2a, 0x7d, 0x7e, 0x11,
        0xe2, 0x5c, 0xcc, 0xf2, 0x85, 0x3f, 0xe7, 0x47, 0x0f, 0x12, 0x96, 0x46, 0x55, 0xa2, 0x19,
        0x36, 0xba, 0x81, 0x89, 0x76, 0xdd, 0xc0, 0x5c, 0xcf, 0x4a, 0xff, 0xd4, 0x4d, 0x71, 0x41,
        0x2a, 0x54, 0x79, 0x82, 0xb6, 0x13, 0x9a, 0xc8, 0x87, 0x08, 0x16, 0xc4, 0xbf, 0x46, 0xa9,
        0x34, 0x93, 0x48, 0xd2,
    };

    const std::string TESTALIAS = "LegacyKeystoreWifiTestAlias";

    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
    ASSERT_TRUE(LegacyKeystorePut(TESTALIAS, TESTBLOB_DER_3CERT, AID_WIFI));

    IKeystore::KeystoreStatusCode statusCode;
    std::vector<uint8_t> blob;
    auto rc = wifiKeystoreHal->getBlob(TESTALIAS,
                                       [&](IKeystore::KeystoreStatusCode status,
                                           const ::android::hardware::hidl_vec<uint8_t>& value) {
                                           statusCode = status;
                                           blob = value;
                                       });

    ASSERT_TRUE(rc.isOk()) << "Description: " << rc.description();
    ASSERT_EQ(IKeystore::KeystoreStatusCode::SUCCESS, statusCode);

    std::string blob_str(reinterpret_cast<const char*>(blob.data()),
                         reinterpret_cast<const char*>(blob.data()) + blob.size());

    // The output must include exactly three PEM certificate begin markers.
    auto pos = blob_str.find("-----BEGIN CERTIFICATE-----", 0);
    ASSERT_NE(pos, std::string::npos);
    pos = blob_str.find("-----BEGIN CERTIFICATE-----", pos + 1);
    ASSERT_NE(pos, std::string::npos);
    pos = blob_str.find("-----BEGIN CERTIFICATE-----", pos + 1);
    ASSERT_NE(pos, std::string::npos);
    pos = blob_str.find("-----BEGIN CERTIFICATE-----", pos + 1);
    ASSERT_EQ(pos, std::string::npos);

    ASSERT_TRUE(LegacyKeystoreRemove(TESTALIAS, AID_WIFI));
}

}  // namespace
