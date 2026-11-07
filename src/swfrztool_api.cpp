/*
 * swfrztool_api.cpp
 * ------------------------------------------------------------------
 * 希沃冰点还原控制工具 —— R3（用户态）接口实现。
 * 依赖：Windows SDK（kernel32 / advapi32 / psapi）。
 * 请以管理员权限运行使用本接口的进程。
 */

#include "swfrztool_api.h"

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <winsvc.h>
#include <psapi.h>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "psapi.lib")

#define SWFRZ_COUNTOF(a) (sizeof(a) / sizeof((a)[0]))

/* ================================================================== */
/* MD5（RFC 1321，用于希沃配置文件完整性校验）                        */
/* ================================================================== */

typedef struct _SwfrzMd5Ctx {
    uint32_t state[4];
    uint64_t count;
    uint8_t  buffer[64];
} SwfrzMd5Ctx;

static const uint32_t SwfrzMd5K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const uint8_t SwfrzMd5S[64] = {
    7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,  7, 12, 17, 22,
    5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,  5,  9, 14, 20,
    4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,  4, 11, 16, 23,
    6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21,  6, 10, 15, 21
};

#define SWFRZ_ROTL32(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static uint32_t SwfrzMd5F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
static uint32_t SwfrzMd5G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
static uint32_t SwfrzMd5H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
static uint32_t SwfrzMd5I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

static void SwfrzMd5Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t m[16];

    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)block[i * 4] |
               ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;

        if (i < 16) {
            f = SwfrzMd5F(b, c, d);
            g = i;
        } else if (i < 32) {
            f = SwfrzMd5G(b, c, d);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = SwfrzMd5H(b, c, d);
            g = (3 * i + 5) & 15;
        } else {
            f = SwfrzMd5I(b, c, d);
            g = (7 * i) & 15;
        }

        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + SWFRZ_ROTL32(a + f + SwfrzMd5K[i] + m[g], SwfrzMd5S[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void SwfrzMd5Init(SwfrzMd5Ctx* ctx) {
    ctx->count = 0;
    ctx->state[0] = 0x67452301u;
    ctx->state[1] = 0xefcdab89u;
    ctx->state[2] = 0x98badcfeu;
    ctx->state[3] = 0x10325476u;
}

static void SwfrzMd5Update(SwfrzMd5Ctx* ctx, const uint8_t* data, size_t len) {
    size_t index = (size_t)(ctx->count & 63);
    ctx->count += (uint64_t)len;

    if (index > 0) {
        size_t space = 64 - index;
        size_t copy = (len < space) ? len : space;
        memcpy(ctx->buffer + index, data, copy);
        index += copy;
        data += copy;
        len -= copy;
        if (index < 64) {
            return;
        }
        SwfrzMd5Transform(ctx->state, ctx->buffer);
    }

    while (len >= 64) {
        SwfrzMd5Transform(ctx->state, data);
        data += 64;
        len -= 64;
    }

    if (len > 0) {
        memcpy(ctx->buffer, data, len);
    }
}

static void SwfrzMd5Final(SwfrzMd5Ctx* ctx, uint8_t digest[16]) {
    uint64_t bitCount = ctx->count * 8;
    uint8_t padding[64];
    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80;

    size_t index = (size_t)(ctx->count & 63);
    size_t padLen = (index < 56) ? (56 - index) : (120 - index);
    SwfrzMd5Update(ctx, padding, padLen);

    uint8_t lenBytes[8];
    for (int i = 0; i < 8; i++) {
        lenBytes[i] = (uint8_t)(bitCount >> (8 * i));
    }
    SwfrzMd5Update(ctx, lenBytes, 8);

    for (int i = 0; i < 4; i++) {
        digest[i * 4 + 0] = (uint8_t)(ctx->state[i]);
        digest[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(ctx->state[i] >> 24);
    }
}

static void SwfrzMd5(const uint8_t* data, size_t len, uint8_t digest[16]) {
    SwfrzMd5Ctx ctx;
    SwfrzMd5Init(&ctx);
    SwfrzMd5Update(&ctx, data, len);
    SwfrzMd5Final(&ctx, digest);
}

/* ================================================================== */
/* 内部辅助                                                           */
/* ================================================================== */

static BOOLEAN SwfrzIoctl(HANDLE hDevice, DWORD ioctlCode, PSWFRZ_PARAMS pParams) {
    if (!hDevice || hDevice == INVALID_HANDLE_VALUE) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (!pParams) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        hDevice,
        ioctlCode,
        pParams,
        sizeof(SWFRZ_PARAMS),
        NULL,
        0,
        &bytesReturned,
        NULL);

    return ok ? TRUE : FALSE;
}

static void SwfrzFreeVolumeInfoTable(PSWFRZ_FREEZE_INFO pInfo) {
    if (!pInfo) {
        return;
    }
    for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
        if (pInfo->volumeInfoList[i]) {
            free(pInfo->volumeInfoList[i]);
            pInfo->volumeInfoList[i] = NULL;
        }
    }
}

static BOOLEAN SwfrzInitVolumeInfoTable(PSWFRZ_FREEZE_INFO pInfo) {
    DWORD driveMask = GetLogicalDrives();
    if (driveMask == 0) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }

    for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
        if (driveMask & (1u << i)) {
            CHAR driveRoot[] = { (CHAR)('A' + i), ':', '\\', '\0' };
            if (GetDriveTypeA(driveRoot) == DRIVE_FIXED) {
                PSWFRZ_VOLUME_INFO pVol = (PSWFRZ_VOLUME_INFO)calloc(1, sizeof(SWFRZ_VOLUME_INFO));
                if (!pVol) {
                    SwfrzFreeVolumeInfoTable(pInfo);
                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                    return FALSE;
                }
                pVol->name = (CHAR)('A' + i);
                pInfo->volumeInfoList[i] = pVol;
            }
        }
    }

    return TRUE;
}

static void SwfrzBuildFreezeConfig(PSWFRZ_FREEZE_CONFIG pConfig, SWFRZ_VOLUMES volumeMask) {
    DWORD v = (DWORD)volumeMask;
    pConfig->volumeProtected = v;
    pConfig->VolumeProtected2 = v;
    SwfrzMd5((const uint8_t*)pConfig + 16, SWFRZ_CONFIG_FILE_SIZE - 16, pConfig->md5);
}

static BOOLEAN SwfrzWriteRegistryDword(PCWSTR pszPath, PCWSTR pszValueName, DWORD dwValue) {
    HKEY hKey = NULL;
    LONG l = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE,
        pszPath,
        0,
        NULL,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        NULL,
        &hKey,
        NULL);
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        return FALSE;
    }

    l = RegSetValueExW(hKey, pszValueName, 0, REG_DWORD, (const BYTE*)&dwValue, sizeof(dwValue));
    RegCloseKey(hKey);
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN SwfrzWriteDriverFile(const BYTE* pImage, DWORD cbImage) {
    HANDLE hFile = CreateFileW(
        SWFRZ_DRIVER_PATH,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, pImage, cbImage, &written, NULL);
    CloseHandle(hFile);

    if (!ok || written != cbImage) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return TRUE;
}

static BOOLEAN SwfrzSetMiniFilterRegistry(PCWSTR pszServiceName, PCWSTR pszAltitude) {
    HKEY hKeyService = NULL;
    HKEY hKeyInstances = NULL;
    HKEY hKeyInstance = NULL;
    BOOLEAN ok = FALSE;

    WCHAR szKeyPath[512];
    WCHAR szInstanceName[256];
    swprintf_s(szKeyPath, SWFRZ_COUNTOF(szKeyPath), L"SYSTEM\\CurrentControlSet\\Services\\%s", (PWSTR)pszServiceName);
    swprintf_s(szInstanceName, SWFRZ_COUNTOF(szInstanceName), L"%s Instance", (PWSTR)pszServiceName);

    LONG l = RegOpenKeyExW(HKEY_LOCAL_MACHINE, szKeyPath, 0, KEY_SET_VALUE | KEY_CREATE_SUB_KEY, &hKeyService);
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        goto Cleanup;
    }

    l = RegCreateKeyExW(hKeyService, L"Instances", 0, NULL, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_CREATE_SUB_KEY, NULL, &hKeyInstances, NULL);
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        goto Cleanup;
    }

    l = RegSetValueExW(hKeyInstances, L"DefaultInstance", 0, REG_SZ,
        (const BYTE*)szInstanceName, (DWORD)((wcslen(szInstanceName) + 1) * sizeof(WCHAR)));
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        goto Cleanup;
    }

    l = RegCreateKeyExW(hKeyInstances, szInstanceName, 0, NULL, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE, NULL, &hKeyInstance, NULL);
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        goto Cleanup;
    }

    l = RegSetValueExW(hKeyInstance, L"Altitude", 0, REG_SZ,
        (const BYTE*)pszAltitude, (DWORD)((wcslen(pszAltitude) + 1) * sizeof(WCHAR)));
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        goto Cleanup;
    }

    DWORD flags = 0;
    l = RegSetValueExW(hKeyInstance, L"Flags", 0, REG_DWORD, (const BYTE*)&flags, sizeof(flags));
    if (l != ERROR_SUCCESS) {
        SetLastError((DWORD)l);
        goto Cleanup;
    }

    ok = TRUE;

Cleanup:
    if (hKeyInstance) RegCloseKey(hKeyInstance);
    if (hKeyInstances) RegCloseKey(hKeyInstances);
    if (hKeyService) RegCloseKey(hKeyService);
    return ok;
}

/* ================================================================== */
/* 公开接口实现                                                       */
/* ================================================================== */

DWORD SWFRZ_GetLastError(void) {
    return GetLastError();
}

BOOLEAN SWFRZ_IsDriverLoaded(PCWSTR pszDriverFileName) {
    if (!pszDriverFileName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    LPVOID drivers[1024];
    DWORD cbNeeded = 0;
    if (!EnumDeviceDrivers(drivers, sizeof(drivers), &cbNeeded)) {
        return FALSE;
    }

    int count = (int)(cbNeeded / sizeof(drivers[0]));
    for (int i = 0; i < count; i++) {
        WCHAR szName[MAX_PATH] = { 0 };
        if (GetDeviceDriverFileNameW(drivers[i], szName, MAX_PATH)) {
            WCHAR* p = wcsrchr(szName, L'\\');
            p = p ? p + 1 : szName;
            if (_wcsicmp(p, pszDriverFileName) == 0) {
                return TRUE;
            }
        }
    }

    SetLastError(ERROR_NOT_FOUND);
    return FALSE;
}

BOOLEAN SWFRZ_InstallAndStartDriver(const BYTE* pDriverImage, DWORD cbDriverImage) {
    if (!pDriverImage || cbDriverImage == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    if (SWFRZ_IsDriverLoaded(SWFRZ_DRIVER_NAME)) {
        return TRUE;
    }

    if (!SwfrzWriteDriverFile(pDriverImage, cbDriverImage)) {
        return FALSE;
    }

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCM) {
        return FALSE;
    }

    BOOLEAN success = FALSE;
    SC_HANDLE hService = NULL;

    hService = CreateServiceW(
        hSCM,
        SWFRZ_SERVICE_NAME,
        SWFRZ_SERVICE_NAME,
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,
        SERVICE_SYSTEM_START,
        SERVICE_ERROR_NORMAL,
        SWFRZ_DRIVER_PATH,
        NULL, NULL, NULL, NULL, NULL);

    if (!hService) {
        if (GetLastError() != ERROR_SERVICE_EXISTS) {
            goto Cleanup;
        }

        hService = OpenServiceW(hSCM, SWFRZ_SERVICE_NAME, SERVICE_ALL_ACCESS);
        if (!hService) {
            goto Cleanup;
        }

        if (!ChangeServiceConfigW(
                hService,
                SERVICE_KERNEL_DRIVER,
                SERVICE_SYSTEM_START,
                SERVICE_ERROR_NORMAL,
                SWFRZ_DRIVER_PATH,
                NULL, NULL, NULL, NULL, NULL,
                SWFRZ_SERVICE_NAME)) {
            goto Cleanup;
        }
    }

    if (!SwfrzSetMiniFilterRegistry(SWFRZ_SERVICE_NAME, SWFRZ_ALTITUDE)) {
        goto Cleanup;
    }

    if (!StartServiceW(hService, 0, NULL)) {
        if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            goto Cleanup;
        }
    }

    success = TRUE;

Cleanup:
    if (hService) CloseServiceHandle(hService);
    if (hSCM) CloseServiceHandle(hSCM);
    return success;
}

BOOLEAN SWFRZ_StopDriver(PCWSTR pszServiceName) {
    if (!pszServiceName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) {
        return FALSE;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, pszServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!hService) {
        CloseServiceHandle(hSCM);
        return FALSE;
    }

    BOOLEAN ok = TRUE;
    SERVICE_STATUS ss = { 0 };

    if (!ControlService(hService, SERVICE_CONTROL_STOP, &ss)) {
        if (GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
            ok = TRUE;
        } else {
            ok = FALSE;
        }
    }

    if (ok) {
        for (int i = 0; i < 50; i++) {
            if (!QueryServiceStatus(hService, &ss)) {
                ok = FALSE;
                break;
            }
            if (ss.dwCurrentState == SERVICE_STOPPED) {
                break;
            }
            Sleep(200);
        }
        if (ok && ss.dwCurrentState != SERVICE_STOPPED) {
            SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
            ok = FALSE;
        }
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return ok;
}

BOOLEAN SWFRZ_RemoveDriver(PCWSTR pszServiceName) {
    if (!pszServiceName) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SC_HANDLE hSCM = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) {
        return FALSE;
    }

    SC_HANDLE hService = OpenServiceW(hSCM, pszServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!hService) {
        DWORD err = GetLastError();
        CloseServiceHandle(hSCM);
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            return TRUE;
        }
        return FALSE;
    }

    SERVICE_STATUS ss = { 0 };
    ControlService(hService, SERVICE_CONTROL_STOP, &ss);

    for (int i = 0; i < 50; i++) {
        if (!QueryServiceStatus(hService, &ss)) {
            break;
        }
        if (ss.dwCurrentState == SERVICE_STOPPED) {
            break;
        }
        Sleep(200);
    }

    BOOLEAN ok = DeleteService(hService);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return ok;
}

BOOLEAN SWFRZ_OpenDevice(HANDLE* phDevice) {
    if (!phDevice) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    HANDLE h = CreateFileW(
        SWFRZ_DEVICE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);

    if (h == INVALID_HANDLE_VALUE) {
        *phDevice = NULL;
        return FALSE;
    }

    *phDevice = h;
    return TRUE;
}

void SWFRZ_CloseDevice(HANDLE hDevice) {
    if (hDevice && hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(hDevice);
    }
}

static void SwfrzFillMissingVolumeSize(PSWFRZ_VOLUME_INFO pVol, CHAR letter) {
    if (!pVol || pVol->volumeTotalBytes != 0) {
        return;
    }

    CHAR root[] = { letter, ':', '\\', '\0' };
    ULARGE_INTEGER total{};
    if (GetDiskFreeSpaceExA(root, NULL, &total, NULL)) {
        pVol->volumeTotalBytes = total.QuadPart;
        if (pVol->volumeSectorCount == 0) {
            pVol->volumeSectorCount = total.QuadPart / 512;
        }
    }
}

BOOLEAN SWFRZ_QueryFreezeInfo(HANDLE hDevice, PSWFRZ_FREEZE_INFO pInfo) {
    if (!pInfo) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    ZeroMemory(pInfo, sizeof(SWFRZ_FREEZE_INFO));

    if (!SwfrzInitVolumeInfoTable(pInfo)) {
        return FALSE;
    }

    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.info.pFrzInfo = pInfo;

    if (!SwfrzIoctl(hDevice, SWFRZ_IOCTL_FRZ_INFO, &params)) {
        SwfrzFreeVolumeInfoTable(pInfo);
        return FALSE;
    }

    /* 未启用冰点保护的卷，R0 返回的大小可能为 0，这里单独从 Windows 重新获取 */
    for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
        if (pInfo->volumeInfoList[i]) {
            SwfrzFillMissingVolumeSize(pInfo->volumeInfoList[i], (CHAR)('A' + i));
        }
    }

    return TRUE;
}

void SWFRZ_LoadConfigInfo(PSWFRZ_FREEZE_INFO pInfo) {
    if (!pInfo) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return;
    }

    SWFRZ_FREEZE_CONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));

    pInfo->config = SWFRZ_ReadFreezeConfig(SWFRZ_CONFIG_PATH, &cfg)
                        ? (DWORD64)cfg.volumeProtected
                        : (DWORD64)-1;

    if (SWFRZ_ReadFreezeConfig(SWFRZ_REDIRECT_PATH, &cfg)) {
        pInfo->redirect = (DWORD64)cfg.volumeProtected;
        return;
    }

    /* 伪装配置尚未初始化：R0 接口检测到未初始化时，伪装状态显示为未启用 */
    pInfo->redirect = (DWORD64)-1;
    pInfo->filterInfo = SWFRZ_FILTER_DISABLED;
}

BOOLEAN SWFRZ_GetFreezeInfo(HANDLE hDevice, PSWFRZ_FREEZE_INFO pInfo) {
    if (!SWFRZ_QueryFreezeInfo(hDevice, pInfo)) {
        return FALSE;
    }
    SWFRZ_LoadConfigInfo(pInfo);
    return TRUE;
}

BOOLEAN SWFRZ_GetFreezeInfoFallback(PSWFRZ_FREEZE_INFO pInfo) {
    if (!pInfo) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    ZeroMemory(pInfo, sizeof(SWFRZ_FREEZE_INFO));

    if (!SwfrzInitVolumeInfoTable(pInfo)) {
        return FALSE;
    }

    for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
        if (pInfo->volumeInfoList[i]) {
            SwfrzFillMissingVolumeSize(pInfo->volumeInfoList[i], (CHAR)('A' + i));
        }
    }

    pInfo->filterInfo = SWFRZ_FILTER_DISABLED;
    SWFRZ_LoadConfigInfo(pInfo);
    return TRUE;
}

void SWFRZ_FreeFreezeInfo(PSWFRZ_FREEZE_INFO pInfo) {
    if (!pInfo) {
        return;
    }
    SwfrzFreeVolumeInfoTable(pInfo);
    pInfo->config = 0;
    pInfo->redirect = 0;
    pInfo->filterInfo = SWFRZ_FILTER_DISABLED;
}

BOOLEAN SWFRZ_SetVolumeProtection(HANDLE hDevice, SWFRZ_VOLUMES volumeMask, BOOLEAN protect) {
    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.volumeProtection.volume = volumeMask;
    params.volumeProtection.protection = protect;
    return SwfrzIoctl(hDevice, SWFRZ_IOCTL_VOL_PROT, &params);
}

BOOLEAN SWFRZ_SetMjFunc(HANDLE hDevice, SWFRZ_MJFUNC mjfunc) {
    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.mjfunc = mjfunc;
    return SwfrzIoctl(hDevice, SWFRZ_IOCTL_MJFUNC, &params);
}

BOOLEAN SWFRZ_SetFilterStatus(HANDLE hDevice, BOOLEAN enableRedirect) {
    if (!SwfrzWriteRegistryDword(SWFRZ_REG_PATH, SWFRZ_REG_VALUE_NAME, enableRedirect ? 1 : 0)) {
        return FALSE;
    }

    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.flt.isRedirect = enableRedirect;
    return SwfrzIoctl(hDevice, SWFRZ_IOCTL_FLT_STATUS, &params);
}

BOOLEAN SWFRZ_GetLog(HANDLE hDevice, PCHAR buffer, DWORD cbBuffer) {
    if (!buffer || cbBuffer == 0 || cbBuffer > SWFRZ_LOG_BUFFER_SIZE) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    ZeroMemory(buffer, cbBuffer);

    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.log.buf = buffer;
    params.log.size = cbBuffer;

    if (!SwfrzIoctl(hDevice, SWFRZ_IOCTL_GET_LOG, &params)) {
        if (GetLastError() != ERROR_INVALID_FUNCTION) {
            return FALSE;
        }
    }

    return TRUE;
}

BOOLEAN SWFRZ_SetWhiteListRanges(
    HANDLE hDevice,
    DWORD volumeIndex,
    const SWFRZ_SECTOR_RANGE* ranges,
    DWORD64 rangeCount,
    BOOLEAN value)
{
    if ((!ranges && rangeCount != 0) || volumeIndex >= SWFRZ_MAX_VOLUMES) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.whiteList.volumeIndex = volumeIndex;
    params.whiteList.ranges = (PSWFRZ_SECTOR_RANGE)ranges;
    params.whiteList.length = rangeCount;
    params.whiteList.value = value;
    return SwfrzIoctl(hDevice, SWFRZ_IOCTL_BMP_RANGES, &params);
}

BOOLEAN SWFRZ_SetWhiteListBitmap(
    HANDLE hDevice,
    DWORD volumeIndex,
    DWORD64 startSector,
    DWORD64 sectorCount,
    const void* bitmap)
{
    if (!bitmap || volumeIndex >= SWFRZ_MAX_VOLUMES) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.whiteList.volumeIndex = volumeIndex;
    params.whiteList.startSector = startSector;
    params.whiteList.sectorCount = sectorCount;
    params.whiteList.buffer = (PVOID)bitmap;
    return SwfrzIoctl(hDevice, SWFRZ_IOCTL_BMP_INPUT, &params);
}

BOOLEAN SWFRZ_GetWhiteListBitmap(
    HANDLE hDevice,
    DWORD volumeIndex,
    DWORD64 startSector,
    DWORD64 sectorCount,
    void* bitmap)
{
    if (!bitmap || volumeIndex >= SWFRZ_MAX_VOLUMES) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    SWFRZ_PARAMS params;
    ZeroMemory(&params, sizeof(params));
    params.whiteList.volumeIndex = volumeIndex;
    params.whiteList.startSector = startSector;
    params.whiteList.sectorCount = sectorCount;
    params.whiteList.buffer = bitmap;
    return SwfrzIoctl(hDevice, SWFRZ_IOCTL_BMP_OUTPUT, &params);
}

BOOLEAN SWFRZ_ReadFreezeConfig(PCWSTR pszPath, PSWFRZ_FREEZE_CONFIG pConfig) {
    if (!pszPath || !pConfig) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* FILE_FLAG_SEQUENTIAL_SCAN 是驱动里预留的后门：
       当状态伪装开启时，用它打开真实配置路径不会被重定向到伪装配置 */
    HANDLE hFile = CreateFileW(
        pszPath,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD read = 0;
    BOOL ok = ReadFile(hFile, pConfig, SWFRZ_CONFIG_FILE_SIZE, &read, NULL);
    CloseHandle(hFile);

    if (!ok || read != SWFRZ_CONFIG_FILE_SIZE) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    uint8_t md5[16];
    SwfrzMd5((const uint8_t*)pConfig + 16, SWFRZ_CONFIG_FILE_SIZE - 16, md5);
    if (memcmp(md5, pConfig->md5, 16) != 0) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }

    return TRUE;
}

BOOLEAN SWFRZ_WriteFreezeConfig(PCWSTR pszPath, const SWFRZ_FREEZE_CONFIG* pConfig) {
    if (!pszPath || !pConfig) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* 同样使用后门标志，避免写入真实配置时被重定向到伪装配置 */
    HANDLE hFile = CreateFileW(
        pszPath,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_FLAG_SEQUENTIAL_SCAN,
        NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return FALSE;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, pConfig, SWFRZ_CONFIG_FILE_SIZE, &written, NULL);
    if (ok) {
        FlushFileBuffers(hFile);
    }
    CloseHandle(hFile);

    if (!ok || written != SWFRZ_CONFIG_FILE_SIZE) {
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    return TRUE;
}

BOOLEAN SWFRZ_UpdateFreezeConfig(SWFRZ_VOLUMES volumeMask) {
    SWFRZ_FREEZE_CONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));

    SWFRZ_ReadFreezeConfig(SWFRZ_CONFIG_PATH, &cfg);

    SwfrzBuildFreezeConfig(&cfg, volumeMask);
    return SWFRZ_WriteFreezeConfig(SWFRZ_CONFIG_PATH, &cfg);
}

BOOLEAN SWFRZ_SetRedirectConfig(SWFRZ_VOLUMES volumeMask) {
    SWFRZ_FREEZE_CONFIG cfg;
    ZeroMemory(&cfg, sizeof(cfg));

    SWFRZ_ReadFreezeConfig(SWFRZ_CONFIG_PATH, &cfg);

    SwfrzBuildFreezeConfig(&cfg, volumeMask);
    return SWFRZ_WriteFreezeConfig(SWFRZ_REDIRECT_PATH, &cfg);
}

SWFRZ_VOLUMES SWFRZ_VolumeMaskFromLetter(CHAR driveLetter) {
    CHAR c = (CHAR)toupper((unsigned char)driveLetter);
    if (c < 'A' || c > 'Z') {
        return 0;
    }
    return (SWFRZ_VOLUMES)1 << (c - 'A');
}

SWFRZ_VOLUMES SWFRZ_VolumeMask(int count, ...) {
    va_list args;
    va_start(args, count);

    SWFRZ_VOLUMES mask = 0;
    for (int i = 0; i < count; i++) {
        int ch = va_arg(args, int);
        mask |= SWFRZ_VolumeMaskFromLetter((CHAR)ch);
    }
    va_end(args);
    return mask;
}
