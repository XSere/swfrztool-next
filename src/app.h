#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <atomic>
#include "swfrztool_api.h"

// 当前打开的对话框
enum class Dialog {
    None,
    MjFunc,
    Filter,
    WhiteList,
    Log
};

struct App {
    HANDLE device = NULL;
    bool ready = false;          // 驱动设备是否已打开
    bool freezeReady = false;    // 内核冰点信息是否可用（卷信息/保护/白名单等功能）
    std::wstring status = L"就绪";

    SWFRZ_FREEZE_INFO info{};
    int selected = -1;          // 当前选中的卷下标（0=A, 1=B, ...）

    Dialog dialog = Dialog::None;

    // 状态伪装对话框状态
    bool filterEnabled = false;

    // 白名单对话框输入
    int wlTab = 0;               // 0=按文件 1=按文件夹 2=按扇区范围
    std::wstring wsStart = L"0";
    std::wstring wsCount = L"0";
    bool wlValue = true;
    std::wstring wlFilePath;     // 按文件页的路径
    std::wstring wlFolderPath;   // 按文件夹页的路径
    std::wstring wlWorkPath;     // 后台计算使用的路径快照
    bool wlWorkValue = true;     // 后台计算使用的写入/清除快照

    std::atomic<bool> wlBusy{false};
    std::atomic<int> wlProgress{0};
    std::atomic<bool> wlDone{false};
    bool wlOk = false;
    std::wstring wlMessage;
    std::wstring wlProgressText;

    void ApplyFileWhiteList();
    void StartFolderWhiteList();
    void FinishWhiteList();
    bool PathMatchesSelectedVolume(const std::wstring& p) const;

    // 内核日志
    std::string logText;

    // 顶部灵动岛提示
    std::wstring toast;
    unsigned long long toastSeq = 0;
    void Notify(const std::wstring& msg) { toast = msg; toastSeq++; }
    /* 状态提示统一走灵动岛：设置状态时同时触发顶部提示 */
    void SetStatus(const std::wstring& s) { status = s; Notify(s); }

    bool Initialize();          // 打开设备（必要时安装驱动）并首次刷新
    void Shutdown();
    void Refresh();             // 重新查询内核信息

    void OpenDialog(Dialog d);
    void CloseDialog();

    void ToggleProtection(int idx);
    void SetConfigProtect(int idx, bool protect);
    void SetRedirect(int idx, bool protect);
    void SetFilterStatus(bool enable);
    void SetMjFunc(SWFRZ_MJFUNC f);
    void ApplyWhiteList();
    void FetchLog();

    bool HasVolume(int idx) const;
    char DriveLetter(int idx) const;
    SWFRZ_VOLUME_PROTECTION_STATUS ProtectStatus(int idx) const;
    bool ConfigProtect(int idx) const;    // 真实配置里该卷是否保护
    bool RedirectProtect(int idx) const;  // 伪装配置里该卷是否保护
};
