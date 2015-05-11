/******************************************************************************
* Copyright 2013, Qualcomm Connected Experiences, Inc.
*
******************************************************************************/
#define AJ_MODULE WIFI_CONTROL

#include "aj_target.h"
#include "aj_debug.h"
#include "aj_util.h"
#include "aj_status.h"
#include "aj_wifi_ctrl.h"

#include <qcom/qcom_wlan.h>
#include <qcom/qcom_network.h>
#include <qcom/qcom_sec.h>
#include <qcom/qcom_system.h>
#include <qcom/socket_api.h>
#include <qcom/qcom_ap.h>
#include <qcom/qcom_scan.h>
#include <qcom/qcom_sta.h>
#include <qcom/qcom_timer.h>
#include <qcom/qcom_misc.h>
#include <qcom/qcom_internal.h>

#ifndef NDEBUG
AJ_EXPORT uint8_t dbgWIFI_CONTROL = 1;
#endif

#define startIP 0xC0A80164
#define endIP   0xC0A801C7

#define IP_LEASE    (60 * 60 * 1000)
#define WIFI_CONNECTED    (2)
#define WIFI_DISCONNECTED (3)
#define WIFI_CONNECTING (4)
#define WIFI_DISCONNECTING (5)


A_UINT8 qcom_DeviceId = 0;

AJ_WiFiSecurityType AJ_WiFisecType;
AJ_WiFiCipherType AJ_WiFicipherType;

static AJ_WiFiConnectState connectState = AJ_WIFI_IDLE;

AJ_WiFiConnectState AJ_GetWifiConnectState(void)
{
    return connectState;
}

#define WIFI_CONNECT_TIMEOUT (10 * 1000)
#define WIFI_CONNECT_SLEEP (200)

#define NO_NETWORK_AVAIL (1)
#define DISCONNECT_CMD   (3)

static void WiFiCallback(A_UINT8 device_id, int val)
{
    AJ_InfoPrintf(("WiFiCallback dev %d, val %d\n", device_id, val));
    if (device_id != qcom_DeviceId) {
        return;
    }

    if (val == 0) {
        if (connectState == AJ_WIFI_DISCONNECTING || connectState == AJ_WIFI_CONNECT_OK) {
            connectState = AJ_WIFI_IDLE;
            AJ_InfoPrintf(("WiFi Disconnected\n"));
        } else if (connectState != AJ_WIFI_CONNECT_FAILED) {
            AJ_WarnPrintf(("WiFi Connect Failed old state %d\n", connectState));
            connectState = AJ_WIFI_CONNECT_FAILED;
        }
    } else if (val == 1) {
        /*
         * With WEP or no security a callback value == 1 means we are done. In the case of wEP this
         * means there is no way to tell if the association succeeded or failed.
         */
        if ((AJ_WiFisecType == AJ_WIFI_SECURITY_NONE) || (AJ_WiFisecType == AJ_WIFI_SECURITY_WEP)) {
            connectState = AJ_WIFI_CONNECT_OK;
            AJ_InfoPrintf(("\nConnected to AP\n"));
        } else {
            connectState = AJ_WIFI_CONNECTING;
            AJ_InfoPrintf(("Connecting to AP, waiting for auth\n"));
        }
    } else if (val == 10) {
        connectState = AJ_WIFI_AUTH_FAILED;
        AJ_WarnPrintf(("WiFi Auth Failed \n"));
    } else if (val == 16) {
        connectState = AJ_WIFI_CONNECT_OK;
        AJ_InfoPrintf(("Connected to AP\n"));
    }
}

static void SoftAPCallback(A_UINT8 device_id, int val)
{
    AJ_InfoPrintf(("SoftAPCallback dev %d, val %d\n", device_id, val));
    if (device_id != qcom_DeviceId) {
        return;
    }

    if (val == 0) {
        if (connectState == AJ_WIFI_DISCONNECTING || connectState == AJ_WIFI_SOFT_AP_UP) {
            connectState = AJ_WIFI_IDLE;
            AJ_InfoPrintf(("Soft AP Down\n"));
        } else if (connectState == AJ_WIFI_STATION_OK) {
            connectState = AJ_WIFI_SOFT_AP_UP;
            AJ_InfoPrintf(("Soft AP Station Disconnected\n"));
        } else {
            connectState = AJ_WIFI_CONNECT_FAILED;
            AJ_InfoPrintf(("Soft AP Connect Failed\n"));
        }
    } else if (val == 1) {
        if (connectState == AJ_WIFI_SOFT_AP_INIT) {
            AJ_InfoPrintf(("Soft AP Initialized\n"));
            connectState = AJ_WIFI_SOFT_AP_UP;
        } else {
            AJ_InfoPrintf(("Soft AP Station Connected\n"));
            connectState = AJ_WIFI_STATION_OK;
        }
    }
}


char wepPhrase[33];

#pragma weak AJ_ConnectWiFi
AJ_Status AJ_ConnectWiFi(const char* ssid, AJ_WiFiSecurityType secType, AJ_WiFiCipherType cipherType, const char* passphrase)
{
    AJ_Status status = AJ_ERR_CONNECT;
    uint8_t connected_status;
    int32_t timeout = WIFI_CONNECT_TIMEOUT;
    uint32_t passLen;

    /*
     * Clear the old connection state
     */
    connectState = AJ_WIFI_IDLE;
    status = AJ_DisconnectWiFi();
    if (status != AJ_OK) {
        return status;
    }
    passLen = strlen(passphrase);

    AJ_AlwaysPrintf(("Connecting to %s with password %s, passlength=%d\n", ssid, passphrase, passLen));
    // security and encryption will be inferred from the scan results
    qcom_set_connect_callback(qcom_DeviceId, WiFiCallback);
    qcom_op_set_mode(qcom_DeviceId, QCOM_WLAN_DEV_MODE_STATION); // station

    /* assign the global values so WiFiCallback can tell if security is needed */
    AJ_WiFisecType = secType;
    AJ_WiFicipherType = cipherType;

    switch (secType) {
    case AJ_WIFI_SECURITY_WEP: {
        memset(wepPhrase, 0, sizeof(wepPhrase));
        strncpy(wepPhrase, passphrase, min(passLen, sizeof(wepPhrase)-1));
        /* Workaround. From AP mode to STA mode, qcom_commit disconnect automatically which will clean the
           WEP info. So it should be disconnected first, after which we could set WEP info and commit.
           This change forces a call to disconnect regardless of our view of WiFi connection state.
        */
        qcom_disconnect(qcom_DeviceId);
        qcom_sec_set_wepkey(qcom_DeviceId, 1, wepPhrase);
        qcom_sec_set_wepkey_index(qcom_DeviceId, 1);
        qcom_sec_set_wep_mode(qcom_DeviceId, 2);
        break;
    }
    case AJ_WIFI_SECURITY_WPA2:
        qcom_sec_set_auth_mode(qcom_DeviceId, WLAN_AUTH_WPA2_PSK);
        qcom_sec_set_passphrase(qcom_DeviceId, (char*) passphrase);
        break;

    case AJ_WIFI_SECURITY_WPA:
        qcom_sec_set_auth_mode(qcom_DeviceId, WLAN_AUTH_WPA_PSK);
        qcom_sec_set_passphrase(qcom_DeviceId, (char*) passphrase);
        break;

    default:
        break;
    }


    switch (cipherType) {
    case AJ_WIFI_CIPHER_NONE:
        qcom_sec_set_encrypt_mode(qcom_DeviceId, WLAN_CRYPT_NONE);
        break;

    case AJ_WIFI_CIPHER_WEP:
        qcom_sec_set_encrypt_mode(qcom_DeviceId, WLAN_CRYPT_WEP_CRYPT);
        break;

    case AJ_WIFI_CIPHER_CCMP:
        qcom_sec_set_encrypt_mode(qcom_DeviceId, WLAN_CRYPT_AES_CRYPT);
        break;

    case AJ_WIFI_CIPHER_TKIP:
        qcom_sec_set_encrypt_mode(qcom_DeviceId, WLAN_CRYPT_TKIP_CRYPT);
        break;

    default:
        break;
    }


    qcom_set_ssid(qcom_DeviceId, (char*) ssid);

    qcom_commit(qcom_DeviceId);

    // TODO: no way of knowing why we failed!
    // no way of failing early; must rely on timeout
    do {
        qcom_get_state(qcom_DeviceId, &connected_status);
        AJ_InfoPrintf(("AJ_ConnectWiFi State: %u \n", connected_status));
        if (connected_status == QCOM_WLAN_LINK_STATE_CONNECTED_STATE) {
            break;
        }
        AJ_Sleep(WIFI_CONNECT_SLEEP);
        timeout -= WIFI_CONNECT_SLEEP;
    } while ((connected_status != QCOM_WLAN_LINK_STATE_CONNECTED_STATE) && (timeout > 0));

    AJ_InfoPrintf(("AJ_ConnectWiFi State: %u \n", connected_status));
    if (connected_status == QCOM_WLAN_LINK_STATE_CONNECTED_STATE) {
        connectState = AJ_WIFI_CONNECT_OK;
        qcom_dhcpc_enable(qcom_DeviceId, 1);  /* turn on dhcp client */
        return AJ_OK;
    } else {
        connectState = AJ_WIFI_CONNECT_FAILED;
        return AJ_ERR_CONNECT;
    }

    return status;
}

AJ_Status AJ_DisconnectWiFi(void)
{
    A_STATUS status = A_OK;
    AJ_WiFiConnectState oldState = AJ_GetWifiConnectState();

    if (connectState != AJ_WIFI_DISCONNECTING) {
        if (connectState != AJ_WIFI_IDLE) {
            connectState = AJ_WIFI_DISCONNECTING;
        }
        status = qcom_disconnect(qcom_DeviceId);

        if (status != A_OK) {
            connectState = oldState;
        }
    }
    return AJ_OK;
}

#pragma weak AJ_EnableSoftAP
AJ_Status AJ_EnableSoftAP(const char* ssid, uint8_t hidden, const char* passphrase, uint32_t timeout)
{
    AJ_Status status = AJ_OK;
    uint8_t connected_status;
    WLAN_AUTH_MODE authMode = WLAN_AUTH_NONE;
    WLAN_CRYPT_TYPE encryptMode = WLAN_CRYPT_NONE;
    const char* pKey = "dummyKey";

    AJ_InfoPrintf(("AJ_EnableSoftAP\n"));
    /*
     * Clear the current connection
     */
    connectState = AJ_WIFI_IDLE;
    status = AJ_DisconnectWiFi();
    if (status != AJ_OK) {
        return status;
    }

    /* assign the global values so WiFiCallback can tell if security is needed */
    AJ_WiFisecType = AJ_WIFI_SECURITY_NONE;
    AJ_WiFicipherType = AJ_WIFI_CIPHER_NONE;

    if (passphrase == NULL) {
        passphrase = "";
    }

    if (passphrase && strlen(passphrase) > 0) {
        authMode = WLAN_AUTH_WPA2_PSK;
        encryptMode = WLAN_CRYPT_AES_CRYPT;
        pKey = passphrase;
        AJ_WiFisecType = AJ_WIFI_SECURITY_WPA2;
        AJ_WiFicipherType = AJ_WIFI_CIPHER_CCMP;
    }

    AJ_WarnPrintf(("AJ_EnableSoftAP: ssid=%s, key=%s, auth=%d, encrypt=%d\n", ssid, pKey, authMode, encryptMode));
    connectState = AJ_WIFI_SOFT_AP_INIT;

    qcom_set_connect_callback(qcom_DeviceId, SoftAPCallback);

    qcom_op_set_mode(qcom_DeviceId, QCOM_WLAN_DEV_MODE_AP);

    if (hidden) {
        qcom_ap_hidden_mode_enable(qcom_DeviceId, hidden);
    }

    if (authMode != WLAN_AUTH_NONE) {
        qcom_sec_set_auth_mode(qcom_DeviceId, authMode);
        qcom_sec_set_encrypt_mode(qcom_DeviceId, encryptMode);
        qcom_sec_set_passphrase(qcom_DeviceId, (char*) pKey);
    }

    /*
     * Set the IP range for DHCP
     */
    qcom_dhcps_set_pool(qcom_DeviceId, startIP, endIP, IP_LEASE);

	qcom_ap_start(qcom_DeviceId, (char *)ssid);

    AJ_Sleep(10);

    connectState = AJ_WIFI_SOFT_AP_UP;

    do {
        qcom_get_state(qcom_DeviceId, &connected_status);
        AJ_Sleep(100);
    } while (connected_status == QCOM_WLAN_LINK_STATE_DISCONNECTED_STATE);

    connectState = AJ_WIFI_STATION_OK;

    return status;
}

#define AJ_WIFI_SCAN_TIMEOUT 8 // seconds
#pragma weak AJ_WiFiScan
AJ_Status AJ_WiFiScan(void* context, AJ_WiFiScanResult callback, uint8_t maxAPs)
{
    AJ_Status status = AJ_OK;
    QCOM_BSS_SCAN_INFO* head = NULL;
    uint32_t count;
    uint8_t bssid[6];
    char ssid[32];
    uint8_t rssi;
    int i = 0;
    int j = 0;
    int* apIndex;

    qcom_scan_all_bss_start(qcom_DeviceId);
    AJ_Sleep(AJ_WIFI_SCAN_TIMEOUT * 1000);

    qcom_bss_scan_result_show(qcom_DeviceId);

    //wlan_get_ap_list(&head, &count);
	qcom_scan_get_bss_number(qcom_DeviceId, &count);
    AJ_InfoPrintf(("Number of APs = %d\n", count));

    if (count <= 0) {
    	AJ_ErrPrintf(("AJ_WiFiScan return AJ_ERR_FAILURE\n"));
        return AJ_ERR_FAILURE;
    }

    apIndex = AJ_Malloc(sizeof(int) * count);
    if (!apIndex) {
    	AJ_ErrPrintf(("AJ_WiFiScan return AJ_ERR_RESOURCES\n"));
        return AJ_ERR_RESOURCES;
    }

    head = AJ_Malloc(sizeof(QCOM_BSS_SCAN_INFO) * count);
    if (head == NULL) {
	    AJ_ErrPrintf(("AJ_WiFiScan return AJ_ERR_RESOURCES\n"));
        return AJ_ERR_RESOURCES;
    }

    for (i = 0; i < count; ++i) {
        apIndex[i] = i;
		qcom_scan_get_bss_info(qcom_DeviceId, i, &head[i]);
    }
    // simple bubble sort
    for (i = 0; i < count; ++i) {
        for (j = i + 1; j < count; ++j) {
            if (head[apIndex[j]].rssi > head[apIndex[i]].rssi) {
                int tmp = apIndex[j];
                apIndex[j] = apIndex[i];
                apIndex[i] = tmp;
            }
        }
    }

    for (i = 0; i < count; ++i) {
        int ind = apIndex[i];
        AJ_WiFiSecurityType secType = AJ_WIFI_SECURITY_NONE;
        AJ_WiFiCipherType cipherType = AJ_WIFI_CIPHER_NONE;
        strncpy(ssid, (char*) head[ind].ssid, sizeof(ssid));
        memcpy(bssid, head[ind].bssid, sizeof(bssid));
        rssi = head[ind].rssi;
        if (head[ind].security_enabled) {
            if (head[ind].rsn_auth) {
                secType = AJ_WIFI_SECURITY_WPA2;
                if (head[ind].rsn_cipher & ATH_CIPHER_TYPE_CCMP) {
                    cipherType = AJ_WIFI_CIPHER_CCMP;
                } else if (head[ind].rsn_cipher & ATH_CIPHER_TYPE_TKIP) {
                    cipherType = AJ_WIFI_CIPHER_TKIP;
                }
            } else if (head[ind].wpa_auth) {
                secType = AJ_WIFI_SECURITY_WPA;
                if (head[ind].wpa_cipher & ATH_CIPHER_TYPE_CCMP) {
                    cipherType = AJ_WIFI_CIPHER_CCMP;
                } else if (head[ind].wpa_cipher & ATH_CIPHER_TYPE_TKIP) {
                    cipherType = AJ_WIFI_CIPHER_TKIP;
                }
            } else {
                secType = AJ_WIFI_SECURITY_WEP;
                cipherType = AJ_WIFI_CIPHER_WEP;
            }
        }
        callback(context, ssid, bssid, rssi, secType, cipherType);
    }

    AJ_Free(head);
    AJ_Free(apIndex);
    return status;
}

AJ_Status AJ_ResetWiFi(void)
{
	return AJ_OK;
}

AJ_Status AJ_GetIPAddress(uint32_t* ip, uint32_t* mask, uint32_t* gateway)
{
    A_INT32 ret;
    // set to zero first
    *ip = *mask = *gateway = 0;

    ret = qcom_ip_address_get(qcom_DeviceId, ip, mask, gateway);
    return (ret == A_OK ? AJ_OK : AJ_ERR_DHCP);
}


#define DHCP_WAIT       100

AJ_Status AJ_AcquireIPAddress(uint32_t* ip, uint32_t* mask, uint32_t* gateway, int32_t timeout)
{
    A_INT32 ret;
    AJ_Status status;
    AJ_WiFiConnectState wifi_state = AJ_GetWifiConnectState();

    switch (wifi_state) {
    case AJ_WIFI_CONNECT_OK:
        break;

    // no need to do anything in Soft-AP mode
    case AJ_WIFI_SOFT_AP_INIT:
    case AJ_WIFI_SOFT_AP_UP:
    case AJ_WIFI_STATION_OK:
        return AJ_OK;

    // shouldn't call this function unless already connected!
    case AJ_WIFI_IDLE:
    case AJ_WIFI_CONNECTING:
    case AJ_WIFI_CONNECT_FAILED:
    case AJ_WIFI_AUTH_FAILED:
    case AJ_WIFI_DISCONNECTING:
        return AJ_ERR_DHCP;
    }


    /*
     * zero out the assigned IP address
     */
    AJ_SetIPAddress(0, 0, 0);
    *ip = *mask = *gateway = 0;

    /*
     * This call kicks off DHCP but we need to poll until the values are populated
     */
    AJ_InfoPrintf(("Sending DHCP request\n"));
    qcom_ipconfig(qcom_DeviceId, IP_CONFIG_DHCP, ip, mask, gateway);
    while (0 == *ip) {
        if (timeout < 0) {
            AJ_ErrPrintf(("AJ_AcquireIPAddress(): DHCP Timeout\n"));
            return AJ_ERR_TIMEOUT;
        }


        AJ_Sleep(DHCP_WAIT);
        status = AJ_GetIPAddress(ip, mask, gateway);
        if (status != AJ_OK) {
            return status;
        }
        timeout -= DHCP_WAIT;
    }

    if (status == AJ_OK) {
        AJ_InfoPrintf(("*********** DHCP succeeded %s\n", AddrStr(*ip)));
    }

    return status;
}

AJ_Status AJ_PrintFWVersion()
{
    char date[20];
    char time[20];
    char ver[20];
    char cl[20];
    qcom_firmware_version_get(date, time, ver, cl);

    AJ_InfoPrintf(("Host version       :  Hostless\n"));
    AJ_InfoPrintf(("Target version     :  \n"));
    AJ_InfoPrintf(("Firmware version   :  %s\n", &ver));
    AJ_InfoPrintf(("Firmware changlist :  %s\n", &cl));
    AJ_InfoPrintf(("Built on: %s %s\n", &date, &time));
}


AJ_Status AJ_SetIPAddress(uint32_t ip, uint32_t mask, uint32_t gateway)
{
    qcom_ipconfig(qcom_DeviceId, IP_CONFIG_SET, &ip, &mask, &gateway);
    return A_OK;
}

