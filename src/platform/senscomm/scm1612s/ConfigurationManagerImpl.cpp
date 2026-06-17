/*
 *
 *    Copyright (c) 2020 Project CHIP Authors
 *    Copyright (c) 2019 Nest Labs, Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *        http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

/**
 *    @file
 *          Provides an implementation of the Device LayerConfigurationManager
 *          object for scm1612s platforms using the  scm1612s SDK.
 */

/* this file behaves like a config.h, comes first */
#include <platform/internal/CHIPDeviceLayerInternal.h>

#include <lib/support/Base64.h>
#include <lib/support/BytesToHex.h>
#include <platform/internal/GenericConfigurationManagerImpl.ipp>
#include <platform/DeviceInstanceInfoProvider.h>

#include <platform/ConfigurationManager.h>
#include <platform/DiagnosticDataProvider.h>
#include <platform/senscomm/scm1612s/SCM1612SConfig.h>

#include "FactoryDataProvider.h"
#include "stdio.h"

#include "wise_event_loop.h"
#include "wise_wifi_types.h"
#include "wise_err.h"
#include "wise_system.h"
#include "scm_wifi.h"
#include "scm_fs.h"

namespace chip {
namespace DeviceLayer {

using namespace ::chip::DeviceLayer::Internal;

CHIP_ERROR LoadFactoryUniqueId(uint8_t (&uniqueId)[ConfigurationManager::kRotatingDeviceIDUniqueIDLength])
{
    MutableByteSpan uniqueIdSpan(uniqueId);
    return LoadUniqueIdFromFactoryData(uniqueIdSpan);
}

ConfigurationManagerImpl & ConfigurationManagerImpl::GetDefaultInstance()
{
    static ConfigurationManagerImpl sInstance;
    return sInstance;
}

CHIP_ERROR ConfigurationManagerImpl::Init()
{
    CHIP_ERROR err;
    bool failSafeArmed;

#if 0
    /* already calls Internal::GenericConfigurationManagerImpl<SCM1612SConfig>::Init() */
    char uniqueId[32 + 1];
    // Generate Unique ID only if it is not present in the storage.
    if (GetUniqueId(uniqueId, sizeof(uniqueId)) != CHIP_NO_ERROR)
    {
        ReturnErrorOnFailure(GenerateUniqueId(uniqueId, sizeof(uniqueId)));
        printf("1212uniqueId:%s\n", uniqueId);
        ReturnErrorOnFailure(StoreUniqueId(uniqueId, strlen(uniqueId)));
    }
#endif
    // Initialize the generic implementation base class.
    err = Internal::GenericConfigurationManagerImpl<SCM1612SConfig>::Init();
    SuccessOrExit(err);
    // TODO: Initialize the global GroupKeyStore object here (#1626)
    IncreaseBootCount();

    // It is possible to configure the possible reset sources with RMU_ResetControl
    // In this case, we keep Reset control at default setting
    //    rebootCause = RMU_ResetCauseGet();
    //    RMU_ResetCauseClear();
    // If the fail-safe was armed when the device last shutdown, initiate a factory reset.
    if (GetFailSafeArmed(failSafeArmed) == CHIP_NO_ERROR && failSafeArmed)
    {
        ChipLogProgress(DeviceLayer, "Detected fail-safe armed on reboot; initiating factory reset");
        InitiateFactoryReset();
    }
    err = CHIP_NO_ERROR;

exit:
    return err;
}

bool ConfigurationManagerImpl::CanFactoryReset()
{
    // TODO: query the application to determine if factory reset is allowed.
    return true;
}

void ConfigurationManagerImpl::InitiateFactoryReset()
{
    PlatformMgr().ScheduleWork(DoFactoryReset);
}

CHIP_ERROR ConfigurationManagerImpl::GetRebootCount(uint32_t & rebootCount)
{
    return SCM1612SConfig::ReadConfigValue(SCM1612SConfig::kConfigKey_BootCount, rebootCount);
}

CHIP_ERROR ConfigurationManagerImpl::StoreRebootCount(uint32_t rebootCount)
{
    return SCM1612SConfig::WriteConfigValue(SCM1612SConfig::kConfigKey_BootCount, rebootCount);
}

CHIP_ERROR ConfigurationManagerImpl::IncreaseBootCount(void)
{
    CHIP_ERROR err;
    uint32_t bootCount = 0;
    if (SCM1612SConfig::ConfigValueExists(SCM1612SConfig::kConfigKey_BootCount))
    {
        err = GetRebootCount(bootCount);
        SuccessOrExit(err);

        err = StoreRebootCount(bootCount + 1);
        SuccessOrExit(err);
    }
    else
    {
        // The first boot after factory reset of the Node.
        err = StoreRebootCount(1);
        SuccessOrExit(err);
    }

exit:
    return err;
}

CHIP_ERROR ConfigurationManagerImpl::GetBootReason(uint32_t & bootReason)
{
    // rebootCause is obtained at bootup.
    BootReasonType matterBootCause;
    matterBootCause = BootReasonType::kUnspecified;
    bootReason = to_underlying(matterBootCause);
    return CHIP_NO_ERROR;
}

CHIP_ERROR ConfigurationManagerImpl::GetTotalOperationalHours(uint32_t & totalOperationalHours)
{
    if (!SCM1612SConfig::ConfigValueExists(SCM1612SConfig::kConfigKey_TotalOperationalHours))
    {
        totalOperationalHours = 0;
        return CHIP_NO_ERROR;
    }

    return SCM1612SConfig::ReadConfigValue(SCM1612SConfig::kConfigKey_TotalOperationalHours, totalOperationalHours);
}

CHIP_ERROR ConfigurationManagerImpl::StoreTotalOperationalHours(uint32_t totalOperationalHours)
{
    return SCM1612SConfig::WriteConfigValue(SCM1612SConfig::kConfigKey_TotalOperationalHours, totalOperationalHours);
}

CHIP_ERROR ConfigurationManagerImpl::ReadPersistedStorageValue(::chip::Platform::PersistedStorage::Key persistedStorageKey,
                                                               uint32_t & value)
{
    CHIP_ERROR err;
    SCM1612SConfig::Key configKey{ SCM1612SConfig::kConfigNamespace_ChipCounters, (char *) &persistedStorageKey };

    err = ReadConfigValue(configKey, value);
    if (err == CHIP_DEVICE_ERROR_CONFIG_NOT_FOUND)
    {
        err = CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    }
    SuccessOrExit(err);

exit:
    return err;
}

CHIP_ERROR ConfigurationManagerImpl::WritePersistedStorageValue(::chip::Platform::PersistedStorage::Key persistedStorageKey,
                                                                uint32_t value)
{
    CHIP_ERROR err;
    /* according to Key's format see CHIP_CONFIG_PERSISTED_STORAGE_KEY_TYPE */
    // SCM1612SConfig::Key configKey{ SCM1612SConfig::kConfigNamespace_ChipCounters, (char *) &persistedStorageKey };
    SCM1612SConfig::Key configKey{ SCM1612SConfig::kConfigNamespace_ChipCounters, persistedStorageKey };

    err = WriteConfigValue(configKey, value);
    {
        err = CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND;
    }
    SuccessOrExit(err);

exit:
    return err;
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, bool & val)
{
    return SCM1612SConfig::ReadConfigValue(key, val);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint32_t & val)
{
    return SCM1612SConfig::ReadConfigValue(key, val);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValue(Key key, uint64_t & val)
{
    return SCM1612SConfig::ReadConfigValue(key, val);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValueStr(Key key, char * buf, size_t bufSize, size_t & outLen)
{
    return SCM1612SConfig::ReadConfigValueStr(key, buf, bufSize, outLen);
}

CHIP_ERROR ConfigurationManagerImpl::ReadConfigValueBin(Key key, uint8_t * buf, size_t bufSize, size_t & outLen)
{
    return SCM1612SConfig::ReadConfigValueBin(key, buf, bufSize, outLen);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, bool val)
{
    return SCM1612SConfig::WriteConfigValue(key, val);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint32_t val)
{
    return SCM1612SConfig::WriteConfigValue(key, val);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValue(Key key, uint64_t val)
{
    return SCM1612SConfig::WriteConfigValue(key, val);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueStr(Key key, const char * str)
{
    return SCM1612SConfig::WriteConfigValueStr(key, str);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueStr(Key key, const char * str, size_t strLen)
{
    return SCM1612SConfig::WriteConfigValueStr(key, str, strLen);
}

CHIP_ERROR ConfigurationManagerImpl::WriteConfigValueBin(Key key, const uint8_t * data, size_t dataLen)
{
    return SCM1612SConfig::WriteConfigValueBin(key, data, dataLen);
}

void ConfigurationManagerImpl::RunConfigUnitTest(void)
{
    SCM1612SConfig::RunConfigUnitTest();
}

void ConfigurationManagerImpl::DoFactoryReset(intptr_t arg)
{
    CHIP_ERROR err;

    ChipLogProgress(DeviceLayer, "Performing factory reset");

    err = SCM1612SConfig::FactoryResetConfig();
    if (err != CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "FactoryResetConfig() failed: %s", chip::ErrorStr(err));
    }

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD

    ChipLogProgress(DeviceLayer, "Clearing Thread provision");
    ThreadStackMgr().ErasePersistentInfo();

#endif // CHIP_DEVICE_CONFIG_ENABLE_THREAD

    PersistedStorage::KeyValueStoreMgrImpl().ErasePartition();

#if CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    ChipLogProgress(DeviceLayer, "Clearing WiFi provision");

    scm_wifi_clear_config(WIFI_IF_STA);
    scm_fs_clear_all_config_value(NULL);

    SCM1612SConfig::FactoryResetConfig();

#endif // CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION

    // Restart the system.
    ChipLogProgress(DeviceLayer, "System restarting");
    wise_restart();
}

CHIP_ERROR ConfigurationManagerImpl::GetPrimaryWiFiMACAddress(uint8_t * buf)
{
    uint8_t *mac_addr = NULL;
    scm_wifi_get_wlan_mac(&mac_addr, 0);

    if (mac_addr)
        memcpy(buf, mac_addr, 6);
    else
        return CHIP_ERROR_INTERNAL;

    return CHIP_NO_ERROR;
}

ConfigurationManager & ConfigurationMgrImpl()
{
    return ConfigurationManagerImpl::GetDefaultInstance();
}

CHIP_ERROR ConfigurationManagerImpl::GetUniqueId(char * buf, size_t bufSize)
{
#if CONFIG_SENSCOMM_FACTORY_DATA_ENABLE || 1
    CHIP_ERROR err;
    size_t uniqueIdLen = 0;
    err = SCM1612SConfig::ReadConfigValueStr(SCM1612SConfig::kConfigKey_UniqueId, buf, bufSize, uniqueIdLen);

    ReturnErrorOnFailure(err);

    ReturnErrorCodeIf(uniqueIdLen >= bufSize, CHIP_ERROR_BUFFER_TOO_SMALL);
    buf[uniqueIdLen] = '\0';
    ReturnErrorCodeIf(buf[uniqueIdLen] != 0, CHIP_ERROR_INVALID_STRING_LENGTH);

    // get the flash uniqueID
    uint8_t uniqueIdStoreInFile[ConfigurationManager::kRotatingDeviceIDUniqueIDLength] = {};
    const uint8_t decodeLen =
        static_cast<uint8_t>(chip::Base64Decode32(buf, static_cast<uint32_t>(strlen(buf)), uniqueIdStoreInFile));
    ReturnErrorCodeIf(decodeLen != sizeof(uniqueIdStoreInFile), CHIP_ERROR_INVALID_STRING_LENGTH);

    uint8_t uniqueIdStoreInFactoryData[ConfigurationManager::kRotatingDeviceIDUniqueIDLength] = {};
    char uniqueIdStoreHex[sizeof(uniqueIdStoreInFile) * 2 + 1]               = { 0 };
    char uniqueIdStoreInFactoryDataHex[sizeof(uniqueIdStoreInFactoryData) * 2 + 1] = { 0 };

    err = LoadFactoryUniqueId(uniqueIdStoreInFactoryData);
    ReturnErrorOnFailure(err);

    ReturnErrorOnFailure(chip::Encoding::BytesToUppercaseHexBuffer(uniqueIdStoreInFile, 
        sizeof(uniqueIdStoreInFile), uniqueIdStoreHex,
        sizeof(uniqueIdStoreHex) - 1));

    ReturnErrorOnFailure(chip::Encoding::BytesToUppercaseHexBuffer(uniqueIdStoreInFactoryData, 
        sizeof(uniqueIdStoreInFactoryData),uniqueIdStoreInFactoryDataHex, 
        sizeof(uniqueIdStoreInFactoryDataHex) - 1));

    ChipLogProgress(DeviceLayer, "uniqueId in FS:%s; in Flash:%s", uniqueIdStoreHex, uniqueIdStoreInFactoryDataHex);

    const bool uniqueIdsMatch = memcmp(uniqueIdStoreInFile, uniqueIdStoreInFactoryData, sizeof(uniqueIdStoreInFile)) == 0;
    return uniqueIdsMatch ? CHIP_NO_ERROR : CHIP_ERROR_INCORRECT_STATE;
#else
    /* When use Ayla adm provider, do nothing */
    return CHIP_NO_ERROR;
#endif
}

CHIP_ERROR ConfigurationManagerImpl::StoreUniqueId(const char * uniqueId, size_t uniqueIdLen)
{
#if CONFIG_SENSCOMM_FACTORY_DATA_ENABLE || 1
    return SCM1612SConfig::WriteConfigValueStr(SCM1612SConfig::kConfigKey_UniqueId, uniqueId, uniqueIdLen);
#else
    /* When use Ayla adm provider, do nothing */
    return CHIP_NO_ERROR;
#endif
}

CHIP_ERROR ConfigurationManagerImpl::GenerateUniqueId(char * buf, size_t bufSize)
{
#if CONFIG_SENSCOMM_FACTORY_DATA_ENABLE || 1
    uint8_t uniqueId[ConfigurationManager::kRotatingDeviceIDUniqueIDLength] = {};
    CHIP_ERROR err = LoadFactoryUniqueId(uniqueId);

    ReturnErrorCodeIf(bufSize <= BASE64_ENCODED_LEN(sizeof(uniqueId)), CHIP_ERROR_BUFFER_TOO_SMALL);

    if (err == CHIP_NO_ERROR)
    {
        ChipLogError(DeviceLayer, "Loaded unique ID from factory flash");
    }
    else
    {
        constexpr uint8_t kMacDerivedUniqueIdPrefix[4] = { 0x15, 0xfe, 0x16, 0x12 };
        uint8_t wlanMac[6];

        ChipLogError(DeviceLayer, "Loading unique ID from factory flash failed: %s; fallback to MAC-derived unique ID",
                     ErrorStr(err));
        memcpy(uniqueId, kMacDerivedUniqueIdPrefix, sizeof(kMacDerivedUniqueIdPrefix));

        ReturnErrorOnFailure(chip::DeviceLayer::ConfigurationMgrImpl().GetPrimaryWiFiMACAddress(wlanMac));
        memcpy(uniqueId + 4, wlanMac, sizeof(wlanMac));
        memcpy(uniqueId + 10, wlanMac, sizeof(wlanMac));
    }

    const uint8_t len = chip::Base64Encode32(uniqueId, sizeof(uniqueId), buf);
    ReturnErrorCodeIf(static_cast<size_t>(len) >= bufSize, CHIP_ERROR_BUFFER_TOO_SMALL);
    buf[len] = '\0';
    ChipLogError(DeviceLayer, "Generated unique ID: %s", buf);
    return CHIP_NO_ERROR;
#else
    /* When use Ayla adm provider, do nothing */
    return CHIP_NO_ERROR;
#endif
}

} // namespace DeviceLayer
} // namespace chip
