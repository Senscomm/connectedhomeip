/*
 *
 *    Copyright (c) 2022 Project CHIP Authors
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

#include <lib/support/CodeUtils.h>
#include <lib/support/SafeInt.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/senscomm/scm1612s/SCM1612SConfig.h>
#include <platform/senscomm/scm1612s/NetworkCommissioningWiFiDriver.h>

#include "wise_event_loop.h"
#include "wise_wifi_types.h"
#include "wise_err.h"
#include "scm_wifi.h"
#include "wise_wifi.h"

extern "C" uint8_t demo_get_scan_source(void);
extern "C" void demo_set_scan_source(uint8_t);
extern "C" void demo_set_inital_scan(bool);

#define SECURITY_OPEN 0
#define SECURITY_TKIP 2
#define SECURITY_CCMP 3
#define SECURITY_CCMP_256 4
#define SECURITY_SAE  6

using namespace ::chip;
using namespace ::chip::DeviceLayer::Internal;

namespace chip {
namespace DeviceLayer {
namespace NetworkCommissioning {

namespace {
constexpr char kWiFiSSIDKeyName[]        = "wifi-ssid";
constexpr char kWiFiCredentialsKeyName[] = "wifi-pass";
static uint8_t WiFiSSIDStr[DeviceLayer::Internal::kMaxWiFiSSIDLength];
constexpr uint16_t kMaxCachedWiFiScanAPs = kMaxWiFiScanAPs;

struct CachedWiFiScanResults
{
    std::array<scm_wifi_ap_info, kMaxCachedWiFiScanAPs> mResults = {};
    uint16_t mCount                                        = 0;
    decltype(System::SystemClock().GetMonotonicTimestamp()) mTimestamp = System::Clock::kZero;
    bool mValid                                            = false;
};

CachedWiFiScanResults gCachedWiFiScanResults;

bool HasValidCachedWiFiScanResults()
{
    if (!gCachedWiFiScanResults.mValid || gCachedWiFiScanResults.mTimestamp == System::Clock::kZero)
    {
        return false;
    }

    const auto now = System::SystemClock().GetMonotonicTimestamp();
    /* Set the valid time to 1010s */
    return now < (gCachedWiFiScanResults.mTimestamp + System::Clock::Seconds32(kWiFiScanNetworksTimeOutSeconds + 1000));
}

void InvalidateCachedWiFiScanResults()
{
    gCachedWiFiScanResults.mCount     = 0;
    gCachedWiFiScanResults.mTimestamp = System::Clock::kZero;
    gCachedWiFiScanResults.mValid     = false;
}

size_t GetWiFiSsidLength(const scm_wifi_ap_info & apInfo)
{
    return strnlen(apInfo.ssid, sizeof(apInfo.ssid));
}

// Deduplicate scan entries by BSSID so the cache keeps distinct APs even when SSIDs match.
bool IsSameAccessPoint(const scm_wifi_ap_info & lhs, const scm_wifi_ap_info & rhs)
{
    return memcmp(lhs.bssid, rhs.bssid, sizeof(lhs.bssid)) == 0;
}

void MergeWiFiScanResults(const scm_wifi_ap_info * apListBuffer, uint16_t count)
{
    VerifyOrReturn(apListBuffer != nullptr || count == 0);

    const uint16_t maxCount = static_cast<uint16_t>(gCachedWiFiScanResults.mResults.size());

    for (uint16_t index = 0; index < count; ++index)
    {
        const scm_wifi_ap_info & candidate = apListBuffer[index];
        bool updatedExisting               = false;

        for (uint16_t cachedIndex = 0; cachedIndex < gCachedWiFiScanResults.mCount; ++cachedIndex)
        {
            if (IsSameAccessPoint(gCachedWiFiScanResults.mResults[cachedIndex], candidate))
            {
                gCachedWiFiScanResults.mResults[cachedIndex] = candidate;
                updatedExisting                              = true;
                break;
            }
        }

        if (updatedExisting)
        {
            continue;
        }

        if (gCachedWiFiScanResults.mCount < maxCount)
        {
            gCachedWiFiScanResults.mResults[gCachedWiFiScanResults.mCount++] = candidate;
            continue;
        }

        auto weakestIt = std::min_element(gCachedWiFiScanResults.mResults.begin(),
                                          gCachedWiFiScanResults.mResults.begin() + gCachedWiFiScanResults.mCount,
                                          [](const scm_wifi_ap_info & lhs, const scm_wifi_ap_info & rhs) {
                                              return lhs.rssi < rhs.rssi;
                                          });
        if (weakestIt != gCachedWiFiScanResults.mResults.begin() + gCachedWiFiScanResults.mCount &&
            candidate.rssi > weakestIt->rssi)
        {
            *weakestIt = candidate;
        }
    }

    gCachedWiFiScanResults.mTimestamp = System::SystemClock().GetMonotonicTimestamp();
    gCachedWiFiScanResults.mValid     = gCachedWiFiScanResults.mCount > 0;
}

void LogCachedWiFiScanResults()
{
    ChipLogError(DeviceLayer, "Cached WiFi scan results: valid=%d count=%u", gCachedWiFiScanResults.mValid,
                    gCachedWiFiScanResults.mCount);

    for (uint16_t index = 0; index < gCachedWiFiScanResults.mCount; ++index)
    {
        const scm_wifi_ap_info & apInfo = gCachedWiFiScanResults.mResults[index];
        ChipLogProgress(DeviceLayer,
                        "Cached AP[%u]: ssid=%s channel=%u auth=%d rssi=%d bssid=%02x:%02x:%02x:%02x:%02x:%02x", index,
                        apInfo.ssid, apInfo.channel, apInfo.auth, apInfo.rssi, apInfo.bssid[0], apInfo.bssid[1],
                        apInfo.bssid[2], apInfo.bssid[3], apInfo.bssid[4], apInfo.bssid[5]);
    }
}

const scm_wifi_ap_info * FindCachedScanResult(const char * ssid, uint8_t ssidLen)
{
    if (!HasValidCachedWiFiScanResults())
    {
        return nullptr;
    }

    const scm_wifi_ap_info * bestMatch = nullptr;

    for (uint16_t index = 0; index < gCachedWiFiScanResults.mCount; ++index)
    {
        const scm_wifi_ap_info & apInfo = gCachedWiFiScanResults.mResults[index];
        const size_t cachedSsidLen      = GetWiFiSsidLength(apInfo);

        if (cachedSsidLen != ssidLen)
        {
            continue;
        }

        if (memcmp(apInfo.ssid, ssid, ssidLen) == 0)
        {
            if (bestMatch == nullptr || apInfo.rssi > bestMatch->rssi)
            {
                bestMatch = &apInfo;
            }
        }
    }

    return bestMatch;
}
} // namespace

CHIP_ERROR WiseWiFiDriver::Init(NetworkStatusChangeCallback * networkStatusChangeCallback)
{
    CHIP_ERROR err;
    size_t ssidLen        = 0;
    size_t credentialsLen = 0;
    size_t outLen         = 0;
    mpScanCallback        = nullptr;
    mpConnectCallback     = nullptr;

    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::Init");


#if 1
    // Note: Used a tricky method to avoid a problem
    // If we directly use ConnectWiFiNetwork to Connect WiFi for HYD none matter BLE WiFi Provision
    // A quirky problem happens: scm_wifi_sta_set_config does not store key values and get trapped in a cycle:
    // SYSTEM_EVENT_STA_NO_NETWORK ==> Connect(failed) ==>SYSTEM_EVENT_STA_NO_NETWORK
    if (networkStatusChangeCallback == NULL) 
    {
        // mSavedNetwork.
        err = SCM1612SConfig::WriteConfigValueStr(SCM1612SConfig::kConfigKey_WiFiSSID, mTmpNetwork.ssid);
        VerifyOrReturnError(err == CHIP_NO_ERROR, CHIP_NO_ERROR);
        err = SCM1612SConfig::WriteConfigValueStr(SCM1612SConfig::kConfigKey_WiFiPSK, mTmpNetwork.credentials);
        VerifyOrReturnError(err == CHIP_NO_ERROR, CHIP_NO_ERROR);
        uint8_t auth = mTmpNetwork.auth_mode;
        err = SCM1612SConfig::WriteConfigValueBin(SCM1612SConfig::kConfigKey_WiFiSEC, &auth, sizeof(auth));
    }
#endif

    // If reading fails, wifi is not provisioned, no need to go further.
    err = SCM1612SConfig::ReadConfigValueStr(SCM1612SConfig::kConfigKey_WiFiSSID, mSavedNetwork.ssid, sizeof(mSavedNetwork.ssid),
                                           ssidLen);
    VerifyOrReturnError(err == CHIP_NO_ERROR, CHIP_NO_ERROR);

    err = SCM1612SConfig::ReadConfigValueStr(SCM1612SConfig::kConfigKey_WiFiPSK, mSavedNetwork.credentials,
                                           sizeof(mSavedNetwork.credentials), credentialsLen);
    VerifyOrReturnError(err == CHIP_NO_ERROR, CHIP_NO_ERROR);

    err = SCM1612SConfig::ReadConfigValueBin(SCM1612SConfig::kConfigKey_WiFiSEC, &mSavedNetwork.auth_mode,
                                           sizeof(mSavedNetwork.auth_mode), outLen);
    VerifyOrReturnError(err == CHIP_NO_ERROR, CHIP_NO_ERROR);

    mSavedNetwork.credentialsLen = credentialsLen;
    mSavedNetwork.ssidLen        = ssidLen;
    mStagingNetwork              = mSavedNetwork;
    // avoid inital wifi scan if wifi configs saved
    demo_set_inital_scan(false);
    ConnectWiFiNetwork(mSavedNetwork.ssid, ssidLen, mSavedNetwork.credentials, credentialsLen);
    return err;
}

CHIP_ERROR WiseWiFiDriver::CommitConfiguration()
{
    CHIP_ERROR err;

    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::CommitConfiguration");

    ReturnErrorOnFailure(SCM1612SConfig::WriteConfigValueStr(SCM1612SConfig::kConfigKey_WiFiSSID, mStagingNetwork.ssid));

    ReturnErrorOnFailure(SCM1612SConfig::WriteConfigValueStr(SCM1612SConfig::kConfigKey_WiFiPSK, mStagingNetwork.credentials));

    ReturnErrorOnFailure(SCM1612SConfig::WriteConfigValueBin(SCM1612SConfig::kConfigKey_WiFiSEC, &mStagingNetwork.auth_mode,
                                                           sizeof(mStagingNetwork.auth_mode)));

    mSavedNetwork = mStagingNetwork;

    return CHIP_NO_ERROR;
}

void WiseWiFiDriver::UpdateWiFiAuthmode()
{
    wifi_config_t config;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    scm_wifi_get_config(WIFI_IF_STA, &config);

    switch (config.sta.proto)
    {
        case WIFI_PROTO_WPA:
            authmode = WIFI_AUTH_WPA_PSK;
            break;
        case WIFI_PROTO_WPA2:
            switch (config.sta.alg)
            {
                case SECURITY_CCMP:
                    authmode = WIFI_AUTH_WPA2_PSK;
                    break;
                case SECURITY_SAE:
                    authmode = WIFI_AUTH_WPA3_SAE;
                    break;
                default:
                    authmode = WIFI_AUTH_OPEN;
                    break;
            }
            break;
        default:
            authmode = WIFI_AUTH_OPEN;
            break;
    }

    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::UpdateWiFiAuthmode, authmode %d %d", 
                    mStagingNetwork.auth_mode, authmode);

    if (mStagingNetwork.auth_mode != authmode)
    {
        mStagingNetwork.auth_mode = authmode;
        CommitConfiguration();
    }
}

CHIP_ERROR WiseWiFiDriver::RevertConfiguration()
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::RevertConfiguration");

    mStagingNetwork = mSavedNetwork;
    return CHIP_NO_ERROR;
}

bool WiseWiFiDriver::NetworkMatch(const WiFiNetwork & network, ByteSpan networkId)
{
    return networkId.size() == network.ssidLen && memcmp(networkId.data(), network.ssid, network.ssidLen) == 0;
}

Status WiseWiFiDriver::AddOrUpdateNetwork(ByteSpan ssid, ByteSpan credentials, MutableCharSpan & outDebugText,
                                           uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;

    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::AddOrUpdateNetwork");

    VerifyOrReturnError(mStagingNetwork.ssidLen == 0 || NetworkMatch(mStagingNetwork, ssid), Status::kBoundsExceeded);
    VerifyOrReturnError(credentials.size() <= sizeof(mStagingNetwork.credentials), Status::kOutOfRange);
    VerifyOrReturnError(ssid.size() <= sizeof(mStagingNetwork.ssid), Status::kOutOfRange);

    memset(mStagingNetwork.credentials, 0, sizeof(mStagingNetwork.credentials));
    memcpy(mStagingNetwork.credentials, credentials.data(), credentials.size());
    mStagingNetwork.credentialsLen = static_cast<decltype(mStagingNetwork.credentialsLen)>(credentials.size());

    memset(mStagingNetwork.ssid, 0, sizeof(mStagingNetwork.ssid));
    memcpy(mStagingNetwork.ssid, ssid.data(), ssid.size());
    mStagingNetwork.ssidLen = static_cast<decltype(mStagingNetwork.ssidLen)>(ssid.size());

    mStagingNetwork.auth_mode = credentials.size() != 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    ChipLogError(NetworkProvisioning, "AddOrUpdateNetwork ssid:%s, cred:%s\n", mStagingNetwork.ssid, mStagingNetwork.credentials);
    return Status::kSuccess;
}

Status WiseWiFiDriver::RemoveNetwork(ByteSpan networkId, MutableCharSpan & outDebugText, uint8_t & outNetworkIndex)
{
    outDebugText.reduce_size(0);
    outNetworkIndex = 0;

    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::RemoveNetwork");

    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);

    // Use empty ssid for representing invalid network
    mStagingNetwork.ssidLen = 0;
    return Status::kSuccess;
}

Status WiseWiFiDriver::ReorderNetwork(ByteSpan networkId, uint8_t index, MutableCharSpan & outDebugText)
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::ReorderNetwork");

    outDebugText.reduce_size(0);
    // Only one network is supported for now
    VerifyOrReturnError(index == 0, Status::kOutOfRange);
    VerifyOrReturnError(NetworkMatch(mStagingNetwork, networkId), Status::kNetworkIDNotFound);
    return Status::kSuccess;
}

CHIP_ERROR WiseWiFiDriver::ConnectWiFiNetwork(const char * ssid, uint8_t ssidLen, const char * key, uint8_t keyLen)
{
    scm_wifi_assoc_request req = {0};
    uint8_t pmk_stored[WISE_PMK_LEN] = {0};
    scm_wifi_fast_assoc_request fast_request = {0};
    const scm_wifi_ap_info * cachedApInfo = nullptr;
    int ret;
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::ConnectWiFiNetwork");

    ReturnErrorOnFailure(ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Enabled));

    memcpy(req.ssid, ssid, ssidLen);
    memcpy(req.key, key, keyLen);
    switch (mStagingNetwork.auth_mode)
    {
        case WIFI_AUTH_WPA3_SAE:
            req.auth = SCM_WIFI_SECURITY_SAE;
            break;
        case WIFI_AUTH_WPA2_PSK:
            req.auth = SCM_WIFI_SECURITY_WPA2PSK;
            break;
        case WIFI_AUTH_OPEN:
            req.auth = SCM_WIFI_SECURITY_OPEN;
            break;
        case SCM_WIFI_SECURITY_UNKNOWN:
        default:
            req.auth = SCM_WIFI_SECURITY_WPA2PSK;
            break;
    }
    req.pairwise = SCM_WIFI_PAIRWISE_AES;

    scm_wifi_sta_set_config(&req, NULL);
    ret = scm_wifi_get_options(SCM_WIFI_STA_GET_PSK, pmk_stored);

    ChipLogProgress(NetworkProvisioning, "Setting up connection for WiFi SSID: %s", ssid);

    //ReturnErrorOnFailure(ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Disabled));
    //ReturnErrorOnFailure(ConnectivityMgr().SetWiFiStationMode(ConnectivityManager::kWiFiStationMode_Enabled));

    ChipLogProgress(DeviceLayer, "Attempting to connect WiFi station interface");
    cachedApInfo = FindCachedScanResult(ssid, ssidLen);
    if (cachedApInfo != nullptr && ret == WISE_OK)
    {
        memcpy(fast_request.req.ssid, ssid, ssidLen);
        fast_request.req.auth     = req.auth;
        fast_request.req.pairwise = req.pairwise;
        memcpy(fast_request.req.bssid, cachedApInfo->bssid, sizeof(fast_request.req.bssid));
        fast_request.channel = static_cast<unsigned char>(cachedApInfo->channel);
        memcpy(fast_request.psk, pmk_stored, WISE_PMK_LEN);

        ChipLogError(DeviceLayer, "Using fast connect for cached WiFi SSID: %s on channel %u", ssid, fast_request.channel);
        ret = scm_wifi_sta_fast_connect(&fast_request);
        if (ret != WISE_OK)
        {
            ChipLogError(DeviceLayer, "scm_wifi_sta_fast_connect() failed, fallback to generic connect");
            ret = scm_wifi_sta_connect();
        }
    }
    else
    {
        if (cachedApInfo == nullptr)
        {
            ChipLogError(DeviceLayer, "No cached scan hit for WiFi SSID: %s, use generic connect", ssid);
        }
        else
        {
            ChipLogError(DeviceLayer, "No cached PSK for WiFi SSID: %s, use generic connect", ssid);
        }

        ret = scm_wifi_sta_connect();
    }

    if (ret != WISE_OK)
    {
        ChipLogError(DeviceLayer, "scm_wifi_connect() failed");
        return CHIP_ERROR_INTERNAL;
    }

    return CHIP_NO_ERROR;
}

void WiseWiFiDriver::OnConnectWiFiNetwork()
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::OnConnectWiFiNetwork");

    if (mpConnectCallback)
    {
        CommitConfiguration();
        mpConnectCallback->OnResult(Status::kSuccess, CharSpan(), 0);
        mpConnectCallback = nullptr;
    }
}

CHIP_ERROR WiseWiFiDriver::SetLastDisconnectReason(const ChipDeviceEvent * event)
{
    // todo: confirm the reason details ?
    lastDisconnectedReason = event->Platform.SCMSystemEvent.event.event_info.disconnected.reason;
    return CHIP_NO_ERROR;
}

int16_t WiseWiFiDriver::GetLastDisconnectReason()
{
    return lastDisconnectedReason;
}

void WiseWiFiDriver::ConnectNetwork(ByteSpan networkId, ConnectCallback * callback)
{
    CHIP_ERROR err          = CHIP_NO_ERROR;
    Status networkingStatus = Status::kUnknownError;

    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::ConnectNetwork");

    VerifyOrExit(NetworkMatch(mStagingNetwork, networkId), networkingStatus = Status::kNetworkIDNotFound);
    VerifyOrExit(mpConnectCallback == nullptr, networkingStatus = Status::kUnknownError);

    err = ConnectWiFiNetwork(mStagingNetwork.ssid, mStagingNetwork.ssidLen, mStagingNetwork.credentials,
                             mStagingNetwork.credentialsLen);
    if (err == CHIP_NO_ERROR)
    {
        mpConnectCallback = callback;
        networkingStatus  = Status::kSuccess;
    }

exit:
    if (networkingStatus != Status::kSuccess)
    {
        ChipLogError(NetworkProvisioning, "Failed to connect to WiFi network:%s", chip::ErrorStr(err));
        mpConnectCallback = nullptr;
        callback->OnResult(networkingStatus, CharSpan(), 0);
    }
}

chip::BitFlags<WiFiSecurityBitmap> ConvertSecurityType(scm_wifi_auth_mode auth_mode)
{
    chip::BitFlags<WiFiSecurityBitmap> securityType;
    if (auth_mode == SCM_WIFI_SECURITY_OPEN)
    {
        securityType = WiFiSecurity::kUnencrypted;
    }
    else if (auth_mode == SCM_WIFI_SECURITY_WPAPSK)
    {
        securityType = WiFiSecurity::kWpaPersonal;
    }
    else if (auth_mode == SCM_WIFI_SECURITY_WPA2PSK)
    {
        securityType = WiFiSecurity::kWpa2Personal;
    }
    else if (auth_mode == SCM_WIFI_SECURITY_SAE)
    {
        securityType = WiFiSecurity::kWpa3Personal;
    }
    else
    {
        securityType = WiFiSecurity::kUnencrypted;
    }

    return securityType;
}

bool WiseWiFiDriver::StartScanWiFiNetworks(ByteSpan ssid)
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::StartScanWiFiNetworks");

    ChipLogProgress(DeviceLayer, "Start Scan WiFi Networks");

    if (!ssid.empty()) // ssid is given, only scan this network
    {
        scm_wifi_scan_params sp = {0};
        sp.scan_type = SCM_WIFI_SSID_SCAN;
        sp.ssid_len = ssid.size();
        memcpy(sp.ssid, ssid.data(), ssid.size());
        scm_wifi_sta_advance_scan(&sp);
    }
    else // scan all networks
    {
        bool CacheValid = HasValidCachedWiFiScanResults();
        ChipLogError(DeviceLayer, "Cache scan results %s", CacheValid ? "valid" : "invalid");
        if (CacheValid)
        {
            ChipDeviceEvent e = { 0 };
            e.Type = DeviceEventType::kSCMSystemEvent;
            e.Platform.SCMSystemEvent.event.event_id = SYSTEM_EVENT_SCAN_DONE;
            (void) PlatformMgr().PostEvent(&e);
        }
        else
        {
            scm_wifi_sta_scan();
            demo_set_scan_source(3);
        }
    }

    return true;
}

void WiseWiFiDriver::OnScanWiFiNetworkDone()
{
    uint16_t apNumber = kMaxWiFiScanAPs;
    uint16_t num = 0;
    if (!GetInstance().mpScanCallback)
    {
        ChipLogProgress(DeviceLayer, "No scan callback");
        if (demo_get_scan_source() == 1)
        {
            std::array<scm_wifi_ap_info, kMaxWiFiScanAPs> apBuffer = {};

            if (scm_wifi_sta_scan_results(apBuffer.data(), &apNumber, apBuffer.size()) == WISE_OK)
            {
                MergeWiFiScanResults(apBuffer.data(), apNumber);
                ChipLogProgress(DeviceLayer, "Merged %u WiFi scan results without callback, cache count=%u", apNumber,
                                gCachedWiFiScanResults.mCount);
                LogCachedWiFiScanResults();
            }
            else
            {
                ChipLogError(DeviceLayer, "Failed to cache WiFi scan results without callback");
                InvalidateCachedWiFiScanResults();
            }
        }
        return;
    }

    scm_wifi_ap_info * ap_list_buffer = new scm_wifi_ap_info[apNumber]();
    if (ap_list_buffer == nullptr)
    {
        ChipLogError(DeviceLayer, "can't malloc memory for ap_list_buffer");
        GetInstance().mpScanCallback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
        GetInstance().mpScanCallback = nullptr;
        return;
    }

    if (scm_wifi_sta_scan_results(ap_list_buffer, &num, apNumber) == WISE_OK)
    {
        MergeWiFiScanResults(ap_list_buffer, num);
        const bool hasCachedResults   = HasValidCachedWiFiScanResults();
        const bool useCachedResults   = hasCachedResults || num == 0;
        const uint16_t resultCount    = useCachedResults && hasCachedResults ? gCachedWiFiScanResults.mCount : num;
        const scm_wifi_ap_info * data = useCachedResults && hasCachedResults ? gCachedWiFiScanResults.mResults.data() : ap_list_buffer;

        if (CHIP_NO_ERROR == DeviceLayer::SystemLayer().ScheduleLambda([resultCount, ap_list_buffer, data, useCachedResults]() {
                std::unique_ptr<scm_wifi_ap_info[]> auto_free(ap_list_buffer);
                WiseScanResponseIterator iter(resultCount, data);
                if (GetInstance().mpScanCallback)
                {
                    GetInstance().mpScanCallback->OnFinished(Status::kSuccess, CharSpan(), &iter);
                    GetInstance().mpScanCallback = nullptr;
                }
                else
                {
                    ChipLogError(DeviceLayer, "can't find the ScanCallback function");
                }
            }))
        {
            if (useCachedResults)
            {
                ChipLogError(DeviceLayer, "Reported merged WiFi scan cache, fresh scan count: %u cache count: %u", num,
                             gCachedWiFiScanResults.mCount);
            }
        }
        else
        {
            delete[] ap_list_buffer;
            ChipLogError(DeviceLayer, "can't schedule the scan result processing");
            GetInstance().mpScanCallback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
            GetInstance().mpScanCallback = nullptr;
        }
    }
    else
    {
        delete[] ap_list_buffer;
        ChipLogError(DeviceLayer, "can't get ap_records ");
        GetInstance().mpScanCallback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
        GetInstance().mpScanCallback = nullptr;
    }
}

void WiseWiFiDriver::ScanNetworks(ByteSpan ssid, WiFiDriver::ScanCallback * callback)
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::ScanNetworks");

    if (callback != nullptr)
    {
        mpScanCallback = callback;
        if (!StartScanWiFiNetworks(ssid))
        {
            ChipLogError(DeviceLayer, "ScanWiFiNetworks failed to start");
            mpScanCallback = nullptr;
            callback->OnFinished(Status::kUnknownError, CharSpan(), nullptr);
        }
    }
}

CHIP_ERROR GetConnectedNetwork(Network & network)
{
    ChipLogProgress(NetworkProvisioning, "GetConnectedNetwork");

    wifi_config_t stationConfig;
    scm_wifi_get_config(WIFI_IF_STA, &stationConfig);
    uint8_t length = strnlen(reinterpret_cast<const char*>(stationConfig.sta.ssid), DeviceLayer::Internal::kMaxWiFiSSIDLength);
    if (length > sizeof(network.networkID))
    {
        ChipLogError(DeviceLayer, "SSID too long");
        return CHIP_ERROR_INTERNAL;
    }

    memcpy(network.networkID, stationConfig.sta.ssid, length);
    network.networkIDLen = length;

    return CHIP_NO_ERROR;
}

size_t WiseWiFiDriver::WiFiNetworkIterator::Count()
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::WiFiNetworkIterator::Count");

    return mDriver->mStagingNetwork.ssidLen == 0 ? 0 : 1;
}

bool WiseWiFiDriver::WiFiNetworkIterator::Next(Network & item)
{
    ChipLogProgress(NetworkProvisioning, "WiseWiFiDriver::WiFiNetworkIterator::Next");

    if (mExhausted || mDriver->mStagingNetwork.ssidLen == 0)
    {
        return false;
    }
    memcpy(item.networkID, mDriver->mStagingNetwork.ssid, mDriver->mStagingNetwork.ssidLen);
    item.networkIDLen = mDriver->mStagingNetwork.ssidLen;
    item.connected    = false;
    mExhausted        = true;

    Network connectedNetwork;
    CHIP_ERROR err = GetConnectedNetwork(connectedNetwork);
    if (err == CHIP_NO_ERROR)
    {
        if (connectedNetwork.networkIDLen == item.networkIDLen &&
            memcmp(connectedNetwork.networkID, item.networkID, item.networkIDLen) == 0)
        {
            item.connected = true;
        }
    }
    return true;
}

} // namespace NetworkCommissioning
} // namespace DeviceLayer
} // namespace chip
