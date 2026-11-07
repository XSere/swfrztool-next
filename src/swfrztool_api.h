/*
 * swfrztool_api.h
 * ------------------------------------------------------------------
 * 希沃冰点还原控制工具 —— R3（用户态）接口
 *
 * 注意：
 *  1. 所有操作均需要管理员权限。
 *  2. 内核驱动使用 METHOD_BUFFERED，调用约定以本头文件中的 SWFRZ_PARAMS
 *  3. 卷掩码 (SWFRZ_VOLUMES) 使用位 0 对应 A 盘、位 1 对应 B 盘，依次类推。
 *     白名单接口的 volumeIndex 则使用 0 起始卷下标 (0=A, 1=B, ... 25=Z)。
 */

#ifndef SWFRZTOOL_API_H
#define SWFRZTOOL_API_H

#include <windows.h>
#include <winioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

#define SWFRZ_DEVICE_NAME        L"\\\\.\\FRZMGR"
#define SWFRZ_SERVICE_NAME       L"SWFrzToolSvc"
#define SWFRZ_DRIVER_NAME        L"swfrztool.sys"
#define SWFRZ_DRIVER_PATH        L"C:\\Windows\\System32\\drivers\\swfrztool.sys"
#define SWFRZ_ALTITUDE           L"99999999"

#define SWFRZ_REG_PATH           L"System\\CurrentControlSet\\Services\\SwFrzTool\\Filter"
#define SWFRZ_REG_VALUE_NAME     L"FilterStatus"

/* 希沃冰点配置文件路径 */
#define SWFRZ_CONFIG_PATH        L"C:\\ProgramData\\SeewoFreezeKernelConfig\\VolumeInfo.config"
#define SWFRZ_REDIRECT_PATH      L"C:\\ProgramData\\SeewoFreezeKernelConfig\\redirect.config"

#define SWFRZ_CONFIG_FILE_SIZE   1024
#define SWFRZ_LOG_BUFFER_SIZE    8192
#define SWFRZ_MAX_VOLUMES        26

#define SWFRZ_IOCTL_FLT_STATUS   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_MJFUNC       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_BMP_RANGES   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_BMP_INPUT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_BMP_OUTPUT   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_VOL_PROT     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_FRZ_INFO     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define SWFRZ_IOCTL_GET_LOG      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* ------------------------------------------------------------------ */
/* 类型定义                                                           */
/* ------------------------------------------------------------------ */

typedef DWORD64 SWFRZ_VOLUMES;   /* 卷位掩码：位 0=A, 位 1=B, ... */

typedef enum _SWFRZ_VOLUME_PROTECTION_STATUS {
    SWFRZ_VOL_UNKNOWN     = 0,   /* 未知状态 */
    SWFRZ_VOL_UNPROTECTED = 1,   /* 未保护 */
    SWFRZ_VOL_PROTECTED   = 2,   /* 受保护 */
    SWFRZ_VOL_BYPASS      = 3    /* 绕过保护 */
} SWFRZ_VOLUME_PROTECTION_STATUS;

typedef enum _SWFRZ_FILTER_STATUS {
    SWFRZ_FILTER_ENABLED  = 0,   /* 冰点状态伪装已启用 */
    SWFRZ_FILTER_DISABLED = 1    /* 冰点状态伪装已禁用 */
} SWFRZ_FILTER_STATUS;

typedef enum _SWFRZ_MJFUNC {
    SWFRZ_MJFUNC_ORIGINAL = 0,   /* 恢复 disk.sys 原始读写分发例程 */
    SWFRZ_MJFUNC_SWFREEZE = 1    /* 恢复 SWFreeze 读写分发例程 */
} SWFRZ_MJFUNC;

typedef struct _SWFRZ_VOLUME_INFO {
    CHAR                            name;                    /* 卷名，如 'C' */
    SWFRZ_VOLUME_PROTECTION_STATUS  protectStatus;           /* 卷保护状态 */
    DWORD64                         physicalStartingOffset;  /* 卷物理起始偏移量 */
    DWORD64                         volumeTotalBytes;        /* 卷总字节数 */
    DWORD64                         volumeSectorCount;       /* 卷总扇区数 */
} SWFRZ_VOLUME_INFO, *PSWFRZ_VOLUME_INFO;

typedef struct _SWFRZ_SECTOR_RANGE {
    DWORD64 start;   /* 起始扇区 */
    DWORD64 length;  /* 扇区数量 */
} SWFRZ_SECTOR_RANGE, *PSWFRZ_SECTOR_RANGE;

typedef struct _SWFRZ_FREEZE_INFO {
    PSWFRZ_VOLUME_INFO volumeInfoList[SWFRZ_MAX_VOLUMES]; /* 卷信息表（由接口分配） */
    SWFRZ_FILTER_STATUS filterInfo;                        /* 冰点状态伪装功能状态 */
    DWORD64             config;                            /* 真实受保护卷位掩码，-1 表示无效 */
    DWORD64             redirect;                          /* 伪装受保护卷位掩码，-1 表示无效 */
} SWFRZ_FREEZE_INFO, *PSWFRZ_FREEZE_INFO;

/* 希沃冰点配置文件（1024 字节，前 16 字节为后 1008 字节的 MD5） */
#pragma pack(push, 1)
typedef struct _SWFRZ_FREEZE_CONFIG {
    BYTE    md5[16];
    DWORD   volumeProtected;
    DWORD   align1;
    BYTE    byte_18DA8;
    DWORD   isStopProtect;
    DWORD   dword_18DAD;
    DWORD64 qword_18DB1;
    BYTE    align2[40];
    DWORD   dword_18DE1;
    BYTE    align3[54];
    BYTE    configVersion;
    DWORD   VolumeProtected2;
    DWORD   updatingTimeSet;
    DWORD   UpdateExpirationTime;
    BYTE    align4[872];
} SWFRZ_FREEZE_CONFIG, *PSWFRZ_FREEZE_CONFIG;
#pragma pack(pop)

typedef struct _SWFRZ_PARAMS {
    union {
        SWFRZ_MJFUNC mjfunc;                       /* IOCTL_MJFUNC */

        struct {                                   /* IOCTL_BMP_RANGES / _INPUT / _OUTPUT */
            DWORD64 volumeIndex;                   /* 卷下标：0=A, 1=B, ... */
            union {
                struct {
                    PSWFRZ_SECTOR_RANGE ranges;    /* 扇区范围表 */
                    DWORD64 length;                /* 表项数量 */
                    BOOLEAN value;                 /* 在位图中设置的值 */
                };
                struct {
                    DWORD64 startSector;           /* 起始扇区 */
                    DWORD64 sectorCount;           /* 扇区数量（实现按 /8 换算为字节） */
                    PVOID   buffer;                /* 位图缓冲区 */
                };
            };
        } whiteList;

        struct {                                   /* IOCTL_VOL_PROT */
            DWORD64 volume;                        /* 卷位掩码 */
            BOOLEAN protection;                    /* TRUE=保护, FALSE=取消保护 */
        } volumeProtection;

        struct {                                   /* IOCTL_FLT_STATUS */
            BOOLEAN isRedirect;                    /* TRUE=启用状态伪装 */
        } flt;

        struct {                                   /* IOCTL_FRZ_INFO */
            PSWFRZ_FREEZE_INFO pFrzInfo;           /* 冰点信息结构指针 */
        } info;

        struct {                                   /* IOCTL_GET_LOG */
            PCHAR  buf;                            /* 日志缓冲区 */
            SIZE_T size;                           /* 缓冲区大小，<= 8192 */
        } log;
    };
} SWFRZ_PARAMS, *PSWFRZ_PARAMS;

/* ------------------------------------------------------------------ */
/* 错误查询                                                           */
/* ------------------------------------------------------------------ */

/* 返回最近一次失败对应的 Win32 错误码（等价于 GetLastError()） */
DWORD SWFRZ_GetLastError(void);

/* ------------------------------------------------------------------ */
/* 驱动服务管理                                                       */
/* ------------------------------------------------------------------ */

/* 判断驱动文件是否已加载。driverFileName 例如 L"swfrztool.sys" */
BOOLEAN SWFRZ_IsDriverLoaded(PCWSTR pszDriverFileName);

/* 从内存映像安装并启动内核驱动（需管理员权限）。
 * pDriverImage: 驱动文件内容；cbDriverImage: 字节数。
 * 会自动写入驱动文件、创建/更新服务、写入微过滤器注册表并启动。 */
BOOLEAN SWFRZ_InstallAndStartDriver(const BYTE* pDriverImage, DWORD cbDriverImage);

/* 停止指定驱动服务 */
BOOLEAN SWFRZ_StopDriver(PCWSTR pszServiceName);

/* 删除指定驱动服务（不删除磁盘上的 .sys 文件） */
BOOLEAN SWFRZ_RemoveDriver(PCWSTR pszServiceName);

/* ------------------------------------------------------------------ */
/* 设备打开/关闭                                                      */
/* ------------------------------------------------------------------ */

/* 打开设备 \\\\.\\FRZMGR。成功时 *phDevice 返回句柄。 */
BOOLEAN SWFRZ_OpenDevice(HANDLE* phDevice);

/* 关闭设备句柄 */
void SWFRZ_CloseDevice(HANDLE hDevice);

/* ------------------------------------------------------------------ */
/* 冰点信息查询                                                       */
/* ------------------------------------------------------------------ */

/* 仅向内核查询卷信息。成功后需调用 SWFRZ_FreeFreezeInfo 释放内部内存。 */
BOOLEAN SWFRZ_QueryFreezeInfo(HANDLE hDevice, PSWFRZ_FREEZE_INFO pInfo);

/* 从本地配置文件读取 config / redirect 并填充到 pInfo（纯用户态操作）。 */
void SWFRZ_LoadConfigInfo(PSWFRZ_FREEZE_INFO pInfo);

/* 查询内核卷信息 + 读取本地配置文件，等于 Query + LoadConfig。 */
BOOLEAN SWFRZ_GetFreezeInfo(HANDLE hDevice, PSWFRZ_FREEZE_INFO pInfo);

/* 驱动不可用时的回退：枚举所有固定卷并读取配置文件，保留纯 R3 功能（如冰点配置修改） */
BOOLEAN SWFRZ_GetFreezeInfoFallback(PSWFRZ_FREEZE_INFO pInfo);

/* 释放 SWFRZ_GetFreezeInfo / SWFRZ_QueryFreezeInfo 分配的内存 */
void SWFRZ_FreeFreezeInfo(PSWFRZ_FREEZE_INFO pInfo);

/* ------------------------------------------------------------------ */
/* 控制操作                                                           */
/* ------------------------------------------------------------------ */

/* 修改卷保护状态。volumeMask 为卷位掩码；protect=TRUE 保护，FALSE 取消保护 */
BOOLEAN SWFRZ_SetVolumeProtection(HANDLE hDevice, SWFRZ_VOLUMES volumeMask, BOOLEAN protect);

/* 设置 disk.sys 读写分发例程来源（ORIGINAL / SWFREEZE） */
BOOLEAN SWFRZ_SetMjFunc(HANDLE hDevice, SWFRZ_MJFUNC mjfunc);

/* 启用/禁用冰点状态伪装。会同时写入注册表并通知内核。 */
BOOLEAN SWFRZ_SetFilterStatus(HANDLE hDevice, BOOLEAN enableRedirect);

/* 获取内核日志。buffer 大小不超过 SWFRZ_LOG_BUFFER_SIZE。 */
BOOLEAN SWFRZ_GetLog(HANDLE hDevice, PCHAR buffer, DWORD cbBuffer);

/* ------------------------------------------------------------------ */
/* 白名单位图                                                         */
/* ------------------------------------------------------------------ */

/* 按扇区范围批量设置白名单位图（推荐）。 */
BOOLEAN SWFRZ_SetWhiteListRanges(
    HANDLE hDevice,
    DWORD volumeIndex,
    const SWFRZ_SECTOR_RANGE* ranges,
    DWORD64 rangeCount,
    BOOLEAN value);

/* 按位图字节块写入白名单位图。 */
BOOLEAN SWFRZ_SetWhiteListBitmap(
    HANDLE hDevice,
    DWORD volumeIndex,
    DWORD64 startSector,
    DWORD64 sectorCount,
    const void* bitmap);

/* 读取白名单位图字节块。 */
BOOLEAN SWFRZ_GetWhiteListBitmap(
    HANDLE hDevice,
    DWORD volumeIndex,
    DWORD64 startSector,
    DWORD64 sectorCount,
    void* bitmap);

/* ------------------------------------------------------------------ */
/* 配置文件操作                                                       */
/* ------------------------------------------------------------------ */

/* 读取并校验冰点配置文件（1024 字节，MD5 校验） */
BOOLEAN SWFRZ_ReadFreezeConfig(PCWSTR pszPath, PSWFRZ_FREEZE_CONFIG pConfig);

/* 写入冰点配置文件 */
BOOLEAN SWFRZ_WriteFreezeConfig(PCWSTR pszPath, const SWFRZ_FREEZE_CONFIG* pConfig);

/* 修改真实受保护卷并写回主配置文件 VolumeInfo.config */
BOOLEAN SWFRZ_UpdateFreezeConfig(SWFRZ_VOLUMES volumeMask);

/* 修改伪装受保护卷并写回 redirect.config */
BOOLEAN SWFRZ_SetRedirectConfig(SWFRZ_VOLUMES volumeMask);

/* ------------------------------------------------------------------ */
/* 工具函数                                                           */
/* ------------------------------------------------------------------ */

/* 单个盘符转卷位掩码：'C' -> 1 << 2。非法字符返回 0。 */
SWFRZ_VOLUMES SWFRZ_VolumeMaskFromLetter(CHAR driveLetter);

/* 多个盘符转卷位掩码。示例：SWFRZ_VolumeMask(2, 'C', 'D') */
SWFRZ_VOLUMES SWFRZ_VolumeMask(int count, ...);

#ifdef __cplusplus
}
#endif

#endif /* SWFRZTOOL_API_H */
