#include "app.h"

#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <winioctl.h>
#include <algorithm>
#include <vector>
#include "raw_driver.h"   // 内嵌内核驱动映像（来自原 GUI 工程）

namespace {
size_t Strnlen(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n] != '\0') n++;
    return n;
}

/* 从 FSCTL_GET_RETRIEVAL_POINTERS 结果展开成簇号数组（移植自原 GUI utils.cpp） */
DWORD64* GetFileClusters(HANDLE hFile, DWORD* pCount) {
    *pCount = 0;
    DWORD bufferSize = 4096;
    while (true) {
        std::vector<BYTE> buf(bufferSize);
        DWORD bytesRet = 0;
        LARGE_INTEGER startingVcn = {};
        BOOL ok = DeviceIoControl(hFile, FSCTL_GET_RETRIEVAL_POINTERS,
                                  &startingVcn, sizeof(startingVcn),
                                  buf.data(), bufferSize, &bytesRet, NULL);
        if (ok) {
            RETRIEVAL_POINTERS_BUFFER* rp = (RETRIEVAL_POINTERS_BUFFER*)buf.data();
            DWORD total = 0;
            for (DWORD i = 0; i < rp->ExtentCount; i++) {
                DWORD64 prevVcn = (i == 0) ? rp->StartingVcn.QuadPart : rp->Extents[i - 1].NextVcn.QuadPart;
                total += (DWORD)(rp->Extents[i].NextVcn.QuadPart - prevVcn);
            }
            DWORD64* clusters = (DWORD64*)malloc(total * sizeof(DWORD64));
            if (!clusters) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
            DWORD idx = 0;
            for (DWORD i = 0; i < rp->ExtentCount; i++) {
                DWORD64 lcn = rp->Extents[i].Lcn.QuadPart;
                DWORD64 prevVcn = (i == 0) ? rp->StartingVcn.QuadPart : rp->Extents[i - 1].NextVcn.QuadPart;
                DWORD64 count = rp->Extents[i].NextVcn.QuadPart - prevVcn;
                for (DWORD64 j = 0; j < count; j++) clusters[idx++] = lcn + j;
            }
            *pCount = total;
            return clusters;
        }
        DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA || err == ERROR_INSUFFICIENT_BUFFER) {
            bufferSize *= 2;
            if (bufferSize > 1024 * 1024) return NULL;
        } else {
            return NULL;
        }
    }
}

DWORD64 GetFileRecordNumber(HANDLE hFile) {
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(hFile, &info)) return 0;
    DWORD64 index = ((DWORD64)info.nFileIndexHigh << 32) | info.nFileIndexLow;
    return index & 0x0000FFFFFFFFFFFFULL;
}

void MergeRanges(std::vector<SWFRZ_SECTOR_RANGE>& ranges) {
    if (ranges.empty()) return;
    std::sort(ranges.begin(), ranges.end(), [](const SWFRZ_SECTOR_RANGE& a, const SWFRZ_SECTOR_RANGE& b) {
        return a.start < b.start;
    });
    std::vector<SWFRZ_SECTOR_RANGE> merged;
    merged.reserve(ranges.size());
    for (auto& r : ranges) {
        if (merged.empty() || r.start > merged.back().start + merged.back().length) {
            merged.push_back(r);
        } else {
            DWORD64 end = r.start + r.length;
            DWORD64 oldEnd = merged.back().start + merged.back().length;
            if (end > oldEnd) merged.back().length = end - merged.back().start;
        }
    }
    ranges.swap(merged);
}

/* 计算单个文件的占用扇区（文件数据 + MFT 记录 + 父目录数据），移植并扩展自原 GUI GetFileSectorList */
bool GetFileSectorList(const std::wstring& filePath, std::vector<SWFRZ_SECTOR_RANGE>& out) {
    out.clear();
    if (filePath.size() < 3 || filePath[1] != L':') {
        SetLastError(ERROR_BAD_PATHNAME);
        return false;
    }

    wchar_t driveRoot[4] = { filePath[0], L':', L'\\', L'\0' };
    DWORD sectorsPerCluster = 0, bytesPerSector = 0;
    if (!GetDiskFreeSpaceW(driveRoot, &sectorsPerCluster, &bytesPerSector, NULL, NULL)) return false;
    DWORD bytesPerCluster = sectorsPerCluster * bytesPerSector;

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    std::vector<SWFRZ_SECTOR_RANGE> ranges;

    DWORD count = 0;
    DWORD64* clusters = GetFileClusters(hFile, &count);
    if (clusters) {
        for (DWORD i = 0; i < count; i++) {
            SWFRZ_SECTOR_RANGE r{};
            r.start = clusters[i] * sectorsPerCluster;
            r.length = sectorsPerCluster;
            ranges.push_back(r);
        }
        free(clusters);
    }

    DWORD64 recordNum = GetFileRecordNumber(hFile);
    if (recordNum != 0) {
        HANDLE hVol = CreateFileW(driveRoot, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hVol != INVALID_HANDLE_VALUE) {
            NTFS_VOLUME_DATA_BUFFER volData;
            DWORD bytesRet = 0;
            if (DeviceIoControl(hVol, FSCTL_GET_NTFS_VOLUME_DATA, NULL, 0,
                                &volData, sizeof(volData), &bytesRet, NULL)) {
                DWORD64 mftStartLcn = volData.MftStartLcn.QuadPart;
                DWORD mftRecordSize = volData.BytesPerFileRecordSegment;
                DWORD64 offsetInMft = recordNum * mftRecordSize;
                DWORD64 clusterIndex = offsetInMft / bytesPerCluster;
                DWORD64 offsetInCluster = offsetInMft % bytesPerCluster;
                DWORD64 lcn = mftStartLcn + clusterIndex;
                DWORD64 sectorStart = lcn * sectorsPerCluster + offsetInCluster / bytesPerSector;
                DWORD sectorsPerRecord = mftRecordSize / bytesPerSector;
                SWFRZ_SECTOR_RANGE r{};
                r.start = sectorStart;
                r.length = sectorsPerRecord;
                ranges.push_back(r);
            }
            CloseHandle(hVol);
        }
    }

    /* 父目录数据扇区 */
    std::wstring parent = filePath;
    size_t slash = parent.find_last_of(L'\\');
    if (slash != std::wstring::npos) {
        if (slash == 2 && parent[1] == L':') {
            parent = parent.substr(0, 3);   // 盘符根目录
        } else {
            parent = parent.substr(0, slash);
        }
    } else {
        parent.clear();
    }

    if (!parent.empty()) {
        HANDLE hParent = CreateFileW(parent.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (hParent != INVALID_HANDLE_VALUE) {
            DWORD parentCount = 0;
            DWORD64* parentClusters = GetFileClusters(hParent, &parentCount);
            if (parentClusters) {
                for (DWORD i = 0; i < parentCount; i++) {
                    SWFRZ_SECTOR_RANGE r{};
                    r.start = parentClusters[i] * sectorsPerCluster;
                    r.length = sectorsPerCluster;
                    ranges.push_back(r);
                }
                free(parentClusters);
            }
            CloseHandle(hParent);
        }
    }

    CloseHandle(hFile);

    MergeRanges(ranges);
    if (ranges.empty()) {
        SetLastError(ERROR_NO_DATA);
        return false;
    }
    out.swap(ranges);
    return true;
}

void EnumerateFiles(const std::wstring& dir, std::vector<std::wstring>& files) {
    std::wstring search = dir;
    if (!search.empty() && search.back() != L'\\') search += L'\\';
    search += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subDirs;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;  // 避免目录环
        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subDirs.push_back(full);
        else files.push_back(full);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);

    for (auto& d : subDirs) EnumerateFiles(d, files);
}

DWORD WINAPI FolderWhiteListWorker(LPVOID p) {
    App* self = (App*)p;

    std::vector<std::wstring> files;
    EnumerateFiles(self->wlWorkPath, files);

    if (files.empty()) {
        self->wlOk = false;
        self->wlMessage = L"文件夹中没有可计算的文件";
        self->wlDone = true;
        self->wlBusy = false;
        return 0;
    }

    std::vector<SWFRZ_SECTOR_RANGE> ranges;
    for (size_t i = 0; i < files.size(); i++) {
        std::vector<SWFRZ_SECTOR_RANGE> one;
        if (GetFileSectorList(files[i], one)) {
            for (auto& r : one) ranges.push_back(r);
        }
        self->wlProgress = (int)((i + 1) * 100 / files.size());
    }

    if (ranges.empty()) {
        self->wlOk = false;
        self->wlMessage = L"未计算出任何扇区";
        self->wlDone = true;
        self->wlBusy = false;
        return 0;
    }

    MergeRanges(ranges);
    self->wlProgress = 100;

    bool ok = SWFRZ_SetWhiteListRanges(self->device, (DWORD)self->selected,
                                       ranges.data(), ranges.size(), self->wlWorkValue);
    self->wlOk = ok;
    self->wlMessage = ok ? (L"白名单写入成功，共 " + std::to_wstring(ranges.size()) + L" 个扇区段")
                         : (L"修改白名单失败: " + std::to_wstring(SWFRZ_GetLastError()));
    self->wlDone = true;
    self->wlBusy = false;
    return 0;
}

} // namespace

bool App::Initialize() {
    device = NULL;
    ready = false;

    // 1. 先尝试直接打开设备（驱动可能已在运行）
    if (SWFRZ_OpenDevice(&device)) {
        ready = true;
    } else {
        // 2. 否则从内存映像安装并启动驱动，再打开
        SetStatus(L"正在安装/启动内核驱动...");
        if (SWFRZ_InstallAndStartDriver(raw_driver, sizeof(raw_driver))) {
            if (SWFRZ_OpenDevice(&device)) {
                ready = true;
            } else {
                SetStatus(L"驱动已安装，但打开设备失败: " + std::to_wstring(SWFRZ_GetLastError()));
            }
        } else {
            SetStatus(L"安装驱动失败: " + std::to_wstring(SWFRZ_GetLastError()));
        }
    }

    if (ready) {
        Refresh();
        /* 原 GUI 启动时会读取一次内核日志；这里也先读一次并缓存，
           避免驱动日志之后被清空导致日志窗口为空 */
        FetchLog();
    } else {
        /* 驱动不可用也要枚举系统卷并读取配置文件，保留冰点配置等纯 R3 功能 */
        DWORD err = SWFRZ_GetLastError();
        Refresh();
        SetStatus(L"驱动未安装或加载失败: " + std::to_wstring(err));
    }
    return ready;
}

void App::Shutdown() {
    SWFRZ_FreeFreezeInfo(&info);
    SWFRZ_CloseDevice(device);
    device = NULL;
    ready = false;
}

void App::Refresh() {
    SWFRZ_FreeFreezeInfo(&info);
    ZeroMemory(&info, sizeof(info));

    if (ready) {
        if (SWFRZ_GetFreezeInfo(device, &info)) {
            freezeReady = true;
        } else {
            /* 驱动已加载但内核冰点信息不可用（例如未初始化冰点还原）时，回退到系统卷枚举 */
            freezeReady = false;
            if (!SWFRZ_GetFreezeInfoFallback(&info)) {
                SetStatus(L"查询冰点信息失败: " + std::to_wstring(SWFRZ_GetLastError()));
                return;
            }
        }
    } else {
        freezeReady = false;
        if (!SWFRZ_GetFreezeInfoFallback(&info)) {
            SetStatus(L"读取系统卷信息失败");
            return;
        }
    }

    // 保证 selected 有效
    if (selected < 0 || selected >= SWFRZ_MAX_VOLUMES || !info.volumeInfoList[selected]) {
        selected = -1;
        for (int i = 0; i < SWFRZ_MAX_VOLUMES; i++) {
            if (info.volumeInfoList[i]) {
                selected = i;
                break;
            }
        }
    }

    filterEnabled = (info.filterInfo == SWFRZ_FILTER_ENABLED);
    SetStatus(L"刷新成功");
}

void App::OpenDialog(Dialog d) {
    dialog = d;
    if (d == Dialog::Log) {
        FetchLog();
    }
}

void App::CloseDialog() {
    dialog = Dialog::None;
}

void App::ToggleProtection(int idx) {
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法修改保护状态"); return; }
    if (!HasVolume(idx)) return;

    bool protect = (ProtectStatus(idx) != SWFRZ_VOL_PROTECTED);

    /* 未启用保护的卷（isProtected2 为假）无法直接设为保护 */
    if (protect && ProtectStatus(idx) == SWFRZ_VOL_UNPROTECTED) {
        SetStatus(L"该卷未启用保护，无法直接设为保护");
        Notify(L"该卷未启用保护，无法直接设为保护；请先写入冰点配置并重启");
        return;
    }

    if (!SWFRZ_SetVolumeProtection(device, SWFRZ_VolumeMaskFromLetter(DriveLetter(idx)), protect)) {
        SetStatus(L"修改保护状态失败: " + std::to_wstring(SWFRZ_GetLastError()));
        Notify(status);
        return;
    }
    Refresh();
}

void App::SetConfigProtect(int idx, bool protect) {
    if (!HasVolume(idx)) return;
    DWORD64 cur = (info.config == (DWORD64)-1) ? 0 : info.config;
    DWORD64 bit = 1ULL << idx;
    DWORD64 next = protect ? (cur | bit) : (cur & ~bit);
    if (!SWFRZ_UpdateFreezeConfig(next)) {
        SetStatus(L"写入真实配置失败: " + std::to_wstring(SWFRZ_GetLastError()));
        return;
    }
    Refresh();
}

void App::SetRedirect(int idx, bool protect) {
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法修改伪装配置"); return; }
    if (!HasVolume(idx)) return;

    /* 首次使用伪装配置时，先自动启用状态伪装（原 GUI 逻辑需要先启用伪装） */
    if (!filterEnabled) {
        if (!SWFRZ_SetFilterStatus(device, true)) {
            SetStatus(L"启用状态伪装失败: " + std::to_wstring(SWFRZ_GetLastError()));
            Notify(status);
            return;
        }
        filterEnabled = true;
    }

    DWORD64 cur = (info.redirect == (DWORD64)-1) ? 0 : info.redirect;
    DWORD64 bit = 1ULL << idx;
    DWORD64 next = protect ? (cur | bit) : (cur & ~bit);
    if (!SWFRZ_SetRedirectConfig(next)) {
        SetStatus(L"写入伪装配置失败: " + std::to_wstring(SWFRZ_GetLastError()));
        Notify(status);
        return;
    }
    Refresh();
}

void App::SetFilterStatus(bool enable) {
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法切换状态伪装"); return; }
    if (!SWFRZ_SetFilterStatus(device, enable)) {
        SetStatus(L"设置状态伪装失败: " + std::to_wstring(SWFRZ_GetLastError()));
        Notify(status);
        return;
    }
    Refresh();
}

void App::SetMjFunc(SWFRZ_MJFUNC f) {
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法设置分发例程"); return; }
    if (!SWFRZ_SetMjFunc(device, f)) {
        SetStatus(L"设置分发例程失败: " + std::to_wstring(SWFRZ_GetLastError()));
        Notify(status);
        return;
    }
    SetStatus((f == SWFRZ_MJFUNC_ORIGINAL) ? L"已恢复原始 disk.sys 分发例程"
                                          : L"已恢复 SWFreeze 分发例程");
}

void App::ApplyWhiteList() {
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法修改白名单"); return; }
    if (!HasVolume(selected)) {
        SetStatus(L"请先选择一个卷");
        return;
    }

    if (ProtectStatus(selected) == SWFRZ_VOL_UNPROTECTED) {
        SetStatus(L"该卷未启用保护，无法修改白名单");
        Notify(L"该卷未启用保护，无法修改白名单");
        return;
    }

    wchar_t* end1 = nullptr;
    wchar_t* end2 = nullptr;
    unsigned long long start = wcstoull(wsStart.c_str(), &end1, 10);
    unsigned long long count = wcstoull(wsCount.c_str(), &end2, 10);
    if (end1 == wsStart.c_str() || end2 == wsCount.c_str() || count == 0) {
        SetStatus(L"请输入有效的起始扇区和扇区数");
        return;
    }

    SWFRZ_SECTOR_RANGE range{};
    range.start = (DWORD64)start;
    range.length = (DWORD64)count;

    if (!SWFRZ_SetWhiteListRanges(device, (DWORD)selected, &range, 1, wlValue)) {
        SetStatus(L"修改白名单失败: " + std::to_wstring(SWFRZ_GetLastError()));
        return;
    }
    SetStatus(wlValue ? L"白名单写入成功" : L"白名单清除成功");
    CloseDialog();
}

void App::ApplyFileWhiteList() {
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法修改白名单"); return; }
    if (!HasVolume(selected)) {
        SetStatus(L"请先选择一个卷");
        return;
    }

    if (ProtectStatus(selected) == SWFRZ_VOL_UNPROTECTED) {
        SetStatus(L"该卷未启用保护，无法修改白名单");
        Notify(L"该卷未启用保护，无法修改白名单");
        return;
    }

    if (wlFilePath.empty()) {
        SetStatus(L"请先输入文件路径");
        return;
    }

    if (!PathMatchesSelectedVolume(wlFilePath)) {
        SetStatus(L"文件路径所在卷与当前选择的卷不一致");
        Notify(L"文件路径必须位于当前选中的卷上");
        return;
    }

    std::vector<SWFRZ_SECTOR_RANGE> ranges;
    if (!GetFileSectorList(wlFilePath, ranges)) {
        SetStatus(L"计算文件扇区失败");
        Notify(L"计算文件扇区失败，请确认文件存在且位于 NTFS 卷");
        return;
    }

    if (!SWFRZ_SetWhiteListRanges(device, (DWORD)selected, ranges.data(), ranges.size(), wlValue)) {
        SetStatus(L"修改白名单失败: " + std::to_wstring(SWFRZ_GetLastError()));
        return;
    }

    SetStatus(wlValue ? L"白名单写入成功" : L"白名单清除成功");
    CloseDialog();
}

void App::StartFolderWhiteList() {
    if (wlBusy) return;
    if (!freezeReady) { SetStatus(L"冰点还原未就绪，无法修改白名单"); return; }
    if (!HasVolume(selected)) {
        SetStatus(L"请先选择一个卷");
        return;
    }

    if (ProtectStatus(selected) == SWFRZ_VOL_UNPROTECTED) {
        SetStatus(L"该卷未启用保护，无法修改白名单");
        Notify(L"该卷未启用保护，无法修改白名单");
        return;
    }

    if (wlFolderPath.empty()) {
        SetStatus(L"请先输入文件夹路径");
        return;
    }

    if (!PathMatchesSelectedVolume(wlFolderPath)) {
        SetStatus(L"文件夹路径所在卷与当前选择的卷不一致");
        Notify(L"文件夹路径必须位于当前选中的卷上");
        return;
    }

    wlWorkPath = wlFolderPath;
    wlWorkValue = wlValue;
    wlDone = false;
    wlOk = false;
    wlProgress = 0;
    wlBusy = true;

    HANDLE h = CreateThread(NULL, 0, FolderWhiteListWorker, this, 0, NULL);
    if (!h) {
        wlBusy = false;
        SetStatus(L"启动计算线程失败");
        return;
    }
    CloseHandle(h);
}

void App::FinishWhiteList() {
    if (!wlDone) return;
    wlDone = false;

    if (wlOk) {
        SetStatus(wlMessage);
        CloseDialog();
    } else {
        SetStatus(wlMessage);
    }
}

void App::FetchLog() {
    if (!ready) {
        if (logText.empty()) logText = "(驱动未安装，无法获取内核日志)";
        return;
    }

    char buf[SWFRZ_LOG_BUFFER_SIZE];
    ZeroMemory(buf, sizeof(buf));
    if (!SWFRZ_GetLog(device, buf, sizeof(buf))) {
        SetStatus(L"获取日志失败: " + std::to_wstring(SWFRZ_GetLastError()));
        if (logText.empty()) logText = "(读取内核日志失败)";
        return;
    }

    std::string out;
    for (char* p = buf; p < buf + sizeof(buf);) {
        if (*p == '\0') { p++; continue; }
        size_t n = Strnlen(p, (size_t)(buf + sizeof(buf) - p));
        if (n == 0) { p++; continue; }
        out.append(p, n);
        if (p[n - 1] != '\n') out.push_back('\n');
        p += n + 1;
    }

    if (out.empty()) {
        if (logText.empty()) logText = "(暂无内核日志)";
        return;
    }

    /* 驱动每次读取后会清空日志，这里在程序内累积保存 */
    if (logText == "(无日志)" || logText == "(暂无内核日志)" || logText == "(读取内核日志失败)") {
        logText.clear();
    }
    if (!logText.empty()) {
        logText += "\n----\n";
    }
    logText += out;
}

bool App::HasVolume(int idx) const {
    return idx >= 0 && idx < SWFRZ_MAX_VOLUMES && info.volumeInfoList[idx] != nullptr;
}

char App::DriveLetter(int idx) const {
    return HasVolume(idx) ? info.volumeInfoList[idx]->name : '?';
}

bool App::PathMatchesSelectedVolume(const std::wstring& p) const {
    if (!HasVolume(selected)) return false;
    if (p.size() < 2 || p[1] != L':') return false;
    wchar_t volLetter = (wchar_t)DriveLetter(selected);
    return towupper((wint_t)p[0]) == towupper((wint_t)volLetter);
}

SWFRZ_VOLUME_PROTECTION_STATUS App::ProtectStatus(int idx) const {
    return HasVolume(idx) ? info.volumeInfoList[idx]->protectStatus : SWFRZ_VOL_UNKNOWN;
}

bool App::ConfigProtect(int idx) const {
    if (info.config == (DWORD64)-1 || !HasVolume(idx)) return false;
    return (info.config & (1ULL << idx)) != 0;
}

bool App::RedirectProtect(int idx) const {
    if (info.redirect == (DWORD64)-1 || !HasVolume(idx)) return false;
    return (info.redirect & (1ULL << idx)) != 0;
}
