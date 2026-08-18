// ============================================================================
// FileWatcher.cpp（視窗化 + 開機自動背景執行 + 重複檔案偵測 + 多目錄監控版本）
//
// 功能：
//   1) 第一次執行：視窗會顯示一份「監控資料夾」清單，預設先放入程式啟動時
//      的工作目錄一筆。使用者可以按「新增資料夾...」加入更多資料夾，選取
//      清單中的某一列按「開始/停止監控」個別啟停，每個資料夾各自跑一條獨立
//      的背景監控執行緒，互不影響。
//      只要清單裡有任何資料夾被加入，程式就會：
//        a) 把目前整份資料夾清單存進設定檔（與 .exe 同資料夾的
//           filewatcher_config.txt，一行一個路徑）。
//        b) 在登錄檔 HKCU\Software\Microsoft\Windows\CurrentVersion\Run
//           寫入自己的啟動路徑，讓 Windows 開機（使用者登入）時自動啟動本程式。
//   2) 之後每次開機／登入，Windows 會自動啟動本程式；程式偵測到設定檔已存在，
//      就不顯示主視窗，直接在背景把清單裡的每個資料夾都開始監控（只在系統匣
//      顯示一個圖示），達到「背景常駐監控多個資料夾」的效果。
//   3) 監控邏輯（每個資料夾各自獨立）：先掃描目錄（含所有子目錄），把「檔名、
//      容量、記錄時間」寫入該目錄底下的 file_list.txt，之後新檔案移入即時
//      附加紀錄。**file_list.txt 是永久累積的歷史紀錄，不會在每次啟動時清
//      空**——這樣才能記得「這個檔案很久以前是不是已經記錄過」。
//   4) 重複檔案偵測：新檔案移入時，先比對它的相對路徑是否已經出現在該資料夾
//      的歷史紀錄裡：
//        - 從未記錄過 -> 直接記錄，不必理會（不會跳出任何提示）。
//        - 曾經記錄過（不管是剛才、還是好幾年前）-> 不會立刻寫入紀錄，而是
//          先收集起來；同一批次（同一次 ReadDirectoryChangesW 收到的事件）
//          收集到的所有「重複檔案」，會合併成一個視窗一次列出，支援多檔案
//          批次處理：使用者可以用核取方塊逐一勾選要「刪除」的檔案（未勾選
//          的視為「繼續移入」，會保留在資料夾並記錄一筆新紀錄）。
//   5) 「匯出檔案清單」按鈕：對清單中選取的資料夾，手動觸發一次遞迴掃描，
//      把還沒記錄過的檔案直接寫入該資料夾的 file_list.txt。不需要先啟動
//      監控也能用。
//   6) 系統匣圖示：
//        - 左鍵雙擊：顯示主視窗。
//        - 右鍵：跳出選單「顯示視窗 / 開始監控全部 / 停止監控全部 / 結束程式」。
//      直接按視窗右上角 X 只是把視窗縮到系統匣（監控繼續在背景執行），
//      真正要結束程式要用系統匣選單的「結束程式」。
//
// 使用平台：Windows（ReadDirectoryChangesW 做即時監控；Shell_NotifyIcon 做
//           系統匣圖示；登錄檔 Run 機碼做開機自動啟動；ListView 做資料夾
//           清單與重複檔案清單）。
//
// 編譯方式：
//   [MSVC / Visual Studio 開發人員命令提示字元]
//     cl /EHsc /std:c++17 FileWatcher.cpp /link /SUBSYSTEM:WINDOWS ole32.lib shell32.lib advapi32.lib comctl32.lib
//
//   [MinGW-w64 g++]
//     g++ -std=c++17 -O2 -municode -mwindows -static-libgcc -static-libstdc++ -static \
//         FileWatcher.cpp -o FileWatcher.exe -lole32 -lshell32 -ladvapi32 -lcomctl32
//
// 使用方式：
//   雙擊 FileWatcher.exe，在清單中選取資料夾按「開始/停止監控」即可（之後
//   每次開機會自動在背景監控清單裡的所有資料夾，不需要再手動開啟）。按
//   「新增資料夾...」可以加入更多資料夾一起監控；按「移除資料夾」可以把
//   已停止監控的資料夾從清單移除。
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#define _WIN32_IE 0x0600
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shellapi.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <cwctype>

namespace fs = std::filesystem;

using LogFn = std::function<void(const std::wstring&)>;
using FileHistory = std::map<std::wstring, std::wstring>; // key: 小寫化的相對路徑, value: 上次記錄時間

// ---------------------------------------------------------------------------
// 自訂視窗訊息 / 控制項 / 選單 ID
// ---------------------------------------------------------------------------

static const UINT WM_APP_LOG = WM_APP + 1;             // 背景執行緒 -> UI：附加一行紀錄
static const UINT WM_APP_MONITOR_STOPPED = WM_APP + 2; // 背景執行緒 -> UI：某個資料夾的監控已完全停止（wParam=entry id）
static const UINT WM_TRAYICON = WM_APP + 3;             // 系統匣圖示回呼訊息
static const UINT WM_APP_SCAN_DONE = WM_APP + 4;        // 背景執行緒 -> UI：「匯出檔案清單」已完成
static const UINT WM_APP_AUTOSTART_ALL = WM_APP + 5;    // wWinMain -> UI：開機靜默模式，啟動清單中所有資料夾的監控

static const int IDC_LIST_FOLDERS = 101;
static const int IDC_BTN_ADD_FOLDER = 102;
static const int IDC_BTN_REMOVE_FOLDER = 103;
static const int IDC_BTN_STARTSTOP = 104;
static const int IDC_BTN_EXPORT = 105;
static const int IDC_EDIT_LOG = 106;

static const UINT ID_TRAY_SHOW = 3001;
static const UINT ID_TRAY_START_ALL = 3002;
static const UINT ID_TRAY_STOP_ALL = 3003;
static const UINT ID_TRAY_EXIT = 3004;

static const UINT TRAY_UID = 1;

static const int IDC_DUP_LIST = 5001;
static const int IDC_DUP_SELECTALL = 5002;
static const int IDC_DUP_SELECTNONE = 5003;
static const int IDC_DUP_OK = 5004;

// ---------------------------------------------------------------------------
// 一個「監控項目」代表清單中的一個資料夾，各自有獨立的背景執行緒與停止事件
// ---------------------------------------------------------------------------

struct MonitorEntry {
    int id = 0;
    std::wstring targetDir;
    HANDLE hStopEvent = nullptr;
    std::thread thread;
    std::atomic<bool> isMonitoring{ false };
};

// ---------------------------------------------------------------------------
// 全域狀態
// ---------------------------------------------------------------------------

static HWND g_hListFolders = nullptr;
static HWND g_hBtnStartStop = nullptr;
static HWND g_hBtnExport = nullptr;
static HWND g_hEditLog = nullptr;

static std::vector<std::unique_ptr<MonitorEntry>> g_entries;
static int g_nextEntryId = 1;

static std::thread g_exportThread;
static std::atomic<bool> g_isExporting{ false };

static bool g_silentAutoStart = false;  // 本次啟動是否因為「已有設定檔」而要在背景自動開始監控
static bool g_forceExit = false;        // true 時 WM_CLOSE 要真正結束程式，而不是縮到系統匣
static bool g_hasShownTrayHint = false; // 是否已經顯示過「已縮到系統匣」的提示氣球

static NOTIFYICONDATAW g_nid = {};
static bool g_trayIconAdded = false;

static HFONT g_hFont = nullptr; // 統一套用的介面字型（取自系統目前的訊息框字型，例如 Segoe UI / 微軟正黑體）

// ---------------------------------------------------------------------------
// 工具函式
// ---------------------------------------------------------------------------

// 取得目前時間字串 (YYYY-MM-DD HH:MM:SS)
std::wstring NowString() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04d-%02d-%02d %02d:%02d:%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::wstring(buf);
}

// wstring -> UTF-8 string（確保中文檔名可以正確寫入文字檔）
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int size = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), size, nullptr, nullptr);
    return out;
}

// UTF-8 string -> wstring
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), size);
    return out;
}

// 取得檔案容量（bytes），失敗回傳 -1
long long GetFileSizeSafe(const fs::path& p) {
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec) return -1;
    return (long long)sz;
}

// 相對路徑比對用的小寫化 key（Windows 檔案系統不分大小寫）
std::wstring ToLowerKey(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) c = (wchar_t)towlower(c);
    return out;
}

// 把訊息安全地送回 UI 執行緒顯示（PostMessage 是非同步、跨執行緒安全的）
static void PostUiLog(HWND hwndMain, const std::wstring& text) {
    wchar_t* buf = new wchar_t[text.size() + 1];
    wcscpy_s(buf, text.size() + 1, text.c_str());
    PostMessageW(hwndMain, WM_APP_LOG, 0, (LPARAM)buf);
}

// 取得目前 Windows 系統實際使用的訊息框字型（Segoe UI / 微軟正黑體等），
// 而不是 Win32 控制項預設的舊點陣字型（System font），避免介面字型看起來很醜。
HFONT CreateUIFont() {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);
        if (font) return font;
    }
    return (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

// 幫控制項套用統一介面字型
void ApplyFont(HWND hCtrl) {
    if (g_hFont) SendMessageW(hCtrl, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

// 依 id 找出對應的監控項目
MonitorEntry* FindEntryById(int id) {
    for (auto& e : g_entries) {
        if (e->id == id) return e.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 設定檔（記住目前的監控資料夾清單，一行一個路徑）
// ---------------------------------------------------------------------------

fs::path GetExePath() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    return fs::path(exePath);
}

fs::path GetConfigPath() {
    return GetExePath().parent_path() / L"filewatcher_config.txt";
}

void SaveConfig(const std::vector<std::wstring>& folders) {
    fs::path cfgPath = GetConfigPath();
    std::ofstream ofs(cfgPath, std::ios::trunc | std::ios::binary);
    if (!ofs) return;
    unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    ofs.write((char*)bom, sizeof(bom));
    for (const auto& f : folders) {
        std::string line = WideToUtf8(f) + "\r\n";
        ofs.write(line.c_str(), line.size());
    }
}

// 讀取設定檔內記錄的資料夾路徑清單（一行一個）；沒有設定檔則回傳空清單
std::vector<std::wstring> LoadConfig() {
    std::vector<std::wstring> result;
    fs::path cfgPath = GetConfigPath();
    std::ifstream ifs(cfgPath, std::ios::binary);
    if (!ifs) return result;

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
        content = content.substr(3);
    }

    size_t pos = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            result.push_back(Utf8ToWide(line));
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return result;
}

// 把目前清單裡的所有資料夾存進設定檔，並確保開機自動啟動已註冊
void PersistFolderList() {
    std::vector<std::wstring> folders;
    for (auto& e : g_entries) folders.push_back(e->targetDir);
    SaveConfig(folders);
}

// 在登錄檔 Run 機碼寫入本程式路徑，讓 Windows 開機（使用者登入）時自動啟動
void RegisterAutoStart(const fs::path& exePath) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    std::wstring quoted = L"\"" + exePath.wstring() + L"\"";
    RegSetValueExW(hKey, L"FileWatcher", 0, REG_SZ,
        (const BYTE*)quoted.c_str(), (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
}

// ---------------------------------------------------------------------------
// 記錄檔操作（file_list.txt 是永久累積的歷史紀錄，不會被清空／覆寫）
// ---------------------------------------------------------------------------

std::mutex g_logMutex;

// 附加一筆紀錄： 檔名(相對路徑) <TAB> 容量 <TAB> 記錄時間
void AppendRecord(const fs::path& logFile, const std::string& relNameUtf8, long long size, const LogFn& uiLog) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::ofstream ofs(logFile, std::ios::app | std::ios::binary);
    if (!ofs) {
        if (uiLog) uiLog(L"[錯誤] 無法開啟記錄檔: " + logFile.wstring());
        return;
    }
    std::string line = relNameUtf8 + "\t" + std::to_string(size) + " bytes\t" + WideToUtf8(NowString()) + "\r\n";
    ofs.write(line.c_str(), line.size());
}

// 讀取既有 file_list.txt 的內容，把每一筆「相對路徑 -> 最後一次記錄時間」載入 history；
// 如果檔案還不存在，就建立一份只有 UTF-8 BOM + 表頭的新檔案。
void LoadOrCreateLogFile(const fs::path& logFile, FileHistory& history) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    history.clear();

    std::ifstream ifs(logFile, std::ios::binary);
    if (!ifs) {
        std::ofstream ofs(logFile, std::ios::trunc | std::ios::binary);
        unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
        ofs.write((char*)bom, sizeof(bom));
        std::string header = "檔名(相對路徑)\t容量\t記錄時間\r\n";
        ofs.write(header.c_str(), header.size());
        return;
    }

    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    ifs.close();
    if (content.size() >= 3 &&
        (unsigned char)content[0] == 0xEF && (unsigned char)content[1] == 0xBB && (unsigned char)content[2] == 0xBF) {
        content = content.substr(3);
    }

    size_t pos = 0;
    bool isHeaderLine = true;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string line = (nl == std::string::npos) ? content.substr(pos) : content.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (!line.empty()) {
            if (isHeaderLine) {
                isHeaderLine = false; // 第一行是表頭，略過
            } else {
                size_t t1 = line.find('\t');
                if (t1 != std::string::npos) {
                    size_t t2 = line.find('\t', t1 + 1);
                    std::string relPathUtf8 = line.substr(0, t1);
                    std::string timeUtf8 = (t2 != std::string::npos) ? line.substr(t2 + 1) : "";
                    std::wstring relPath = Utf8ToWide(relPathUtf8);
                    std::wstring time = Utf8ToWide(timeUtf8);
                    if (!relPath.empty()) {
                        history[ToLowerKey(relPath)] = time; // 後面的紀錄覆蓋前面，保留「最後一次記錄時間」
                    }
                }
            }
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
}

// 遞迴掃描目錄下所有檔案（含子目錄），把「歷史紀錄裡從未出現過」的檔案直接附加記錄；
// 已經在歷史紀錄裡的檔案（表示上次關閉程式前就已經在資料夾裡）直接略過，不重複記錄。
void ScanForNewFiles(const fs::path& rootDir, const fs::path& logFile, FileHistory& history) {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(rootDir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }

        const auto& entry = *it;
        std::error_code fec;
        if (!entry.is_regular_file(fec) || fec) continue;

        // 跳過清單檔案自己
        if (fs::equivalent(entry.path(), logFile, fec)) continue;

        long long size = GetFileSizeSafe(entry.path());
        if (size < 0) continue;

        std::wstring relPath = fs::relative(entry.path(), rootDir, fec).wstring();
        std::wstring key = ToLowerKey(relPath);
        if (history.find(key) != history.end()) continue; // 歷史上已經記錄過，略過

        std::string relPathUtf8 = WideToUtf8(relPath);
        AppendRecord(logFile, relPathUtf8, size, nullptr);
        history[key] = NowString();
    }
}

// ---------------------------------------------------------------------------
// 「匯出檔案清單」按鈕：手動觸發一次遞迴掃描（含所有子目錄／子子目錄），
// 把選定資料夾下「歷史紀錄裡還沒有的檔案」直接記錄進同一份 file_list.txt
// （跟監控啟動時的初始掃描共用同一套邏輯 ScanForNewFiles，不會另外開新檔案，
// 也不會影響重複檔案偵測的歷史比對）。不需要先按「開始監控」也能用。
// ---------------------------------------------------------------------------

struct ExportParams {
    std::wstring targetDir;
    HWND hwndMain;
};

void ExportWorkerProc(ExportParams params) {
    fs::path root(params.targetDir);
    fs::path logFile = root / L"file_list.txt";

    LogFn uiLog = [hwnd = params.hwndMain](const std::wstring& text) {
        PostUiLog(hwnd, text);
    };

    uiLog(L"[匯出清單] 開始掃描 " + root.wstring() + L"（含所有子目錄），寫入 " + logFile.wstring() + L" ...");

    FileHistory history;
    LoadOrCreateLogFile(logFile, history);
    size_t beforeCount = history.size();
    ScanForNewFiles(root, logFile, history);
    size_t addedCount = history.size() - beforeCount;

    uiLog(L"[匯出清單] 完成，新增 " + std::to_wstring(addedCount) + L" 筆紀錄（file_list.txt 目前共 " +
        std::to_wstring(history.size()) + L" 筆歷史紀錄）。");

    PostMessageW(params.hwndMain, WM_APP_SCAN_DONE, 0, 0);
}

// ---------------------------------------------------------------------------
// 重複檔案清單視窗（同一批次移入的檔案中，凡是歷史上已記錄過的，都會列在這裡
// 讓使用者一次決定：勾選 = 刪除，未勾選 = 保留並記錄一筆新紀錄）
// ---------------------------------------------------------------------------

struct DupCandidate {
    std::wstring relPath;
    fs::path fullPath;
    long long size;
    std::wstring lastRecordedTime;
};

struct DupDialogState {
    std::vector<DupCandidate>* items;
    std::vector<bool> deleteFlags;
    HWND hList;
    bool done;
};

LRESULT CALLBACK DupDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DupDialogState* state = (DupDialogState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        state = (DupDialogState*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)state);

        HWND hLabel = CreateWindowExW(0, L"STATIC",
            L"以下檔案先前已經記錄過，這次又被移入監控資料夾。\r\n"
            L"請勾選要「刪除」的檔案（未勾選的會保留在資料夾中，並記錄為新的一筆紀錄）：",
            WS_CHILD | WS_VISIBLE,
            10, 10, 570, 40, hwnd, nullptr, nullptr, nullptr);
        ApplyFont(hLabel);

        state->hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
            10, 55, 570, 260, hwnd, (HMENU)(INT_PTR)IDC_DUP_LIST, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(state->hList,
            LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ApplyFont(state->hList);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 360;
        col.pszText = (LPWSTR)L"檔名(相對路徑)";
        ListView_InsertColumn(state->hList, 0, &col);
        col.cx = 180;
        col.pszText = (LPWSTR)L"上次記錄時間";
        ListView_InsertColumn(state->hList, 1, &col);

        for (size_t i = 0; i < state->items->size(); ++i) {
            const DupCandidate& d = (*state->items)[i];
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = (int)i;
            item.pszText = (LPWSTR)d.relPath.c_str();
            ListView_InsertItem(state->hList, &item);
            ListView_SetItemText(state->hList, (int)i, 1, (LPWSTR)d.lastRecordedTime.c_str());
        }

        HWND hSelectAll = CreateWindowExW(0, L"BUTTON", L"全選(刪除)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            10, 325, 100, 26, hwnd, (HMENU)(INT_PTR)IDC_DUP_SELECTALL, nullptr, nullptr);
        HWND hSelectNone = CreateWindowExW(0, L"BUTTON", L"全不選(保留)", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            120, 325, 110, 26, hwnd, (HMENU)(INT_PTR)IDC_DUP_SELECTNONE, nullptr, nullptr);
        HWND hOkBtn = CreateWindowExW(0, L"BUTTON", L"確定", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            500, 325, 80, 26, hwnd, (HMENU)(INT_PTR)IDC_DUP_OK, nullptr, nullptr);
        ApplyFont(hSelectAll);
        ApplyFont(hSelectNone);
        ApplyFont(hOkBtn);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int count = ListView_GetItemCount(state->hList);
        if (id == IDC_DUP_SELECTALL) {
            for (int i = 0; i < count; ++i) ListView_SetCheckState(state->hList, i, TRUE);
        } else if (id == IDC_DUP_SELECTNONE) {
            for (int i = 0; i < count; ++i) ListView_SetCheckState(state->hList, i, FALSE);
        } else if (id == IDC_DUP_OK) {
            state->deleteFlags.resize(state->items->size());
            for (int i = 0; i < count; ++i) {
                state->deleteFlags[i] = ListView_GetCheckState(state->hList, i) != 0;
            }
            state->done = true;
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_CLOSE:
        // 直接關閉視窗視同「全部保留」，不刪除任何檔案
        state->deleteFlags.assign(state->items->size(), false);
        state->done = true;
        DestroyWindow(hwnd);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// 顯示重複檔案清單視窗並等待使用者選擇（在呼叫它的執行緒上跑自己的訊息迴圈，會阻塞呼叫者）。
// 回傳與 items 一一對應的 bool：true = 使用者勾選要刪除，false = 保留並記錄。
std::vector<bool> RunDuplicateDialog(std::vector<DupCandidate>& items, HWND owner) {
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DupDlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"FileWatcherDupDlg";
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    DupDialogState state = {};
    state.items = &items;
    state.done = false;

    if (owner) EnableWindow(owner, FALSE);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"FileWatcherDupDlg", L"發現重複移入的檔案",
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        CW_USEDEFAULT, CW_USEDEFAULT, 610, 400,
        owner, nullptr, GetModuleHandleW(nullptr), &state);

    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    FlashWindow(dlg, TRUE);

    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) EnableWindow(owner, TRUE);

    if (state.deleteFlags.size() != items.size()) {
        state.deleteFlags.assign(items.size(), false); // 保險：異常結束一律視為保留
    }
    return state.deleteFlags;
}

// 處理一批重複檔案：跳出清單視窗讓使用者決定，再依決定刪除或記錄。
void HandleDuplicateFiles(std::vector<DupCandidate>& duplicates, const fs::path& logFile,
                           FileHistory& history, HWND hwndMain, const LogFn& uiLog) {
    uiLog(L"[提醒] 偵測到 " + std::to_wstring(duplicates.size()) + L" 個先前已記錄過的檔案又被移入，等待使用者選擇...");

    std::vector<bool> deleteFlags = RunDuplicateDialog(duplicates, hwndMain);

    for (size_t i = 0; i < duplicates.size(); ++i) {
        const DupCandidate& d = duplicates[i];
        std::error_code ec;

        if (deleteFlags[i]) {
            if (fs::exists(d.fullPath, ec)) {
                fs::remove(d.fullPath, ec);
                if (ec) {
                    uiLog(L"[錯誤] 無法刪除重複檔案: " + d.fullPath.wstring());
                } else {
                    uiLog(L"[已刪除重複檔案] " + d.fullPath.wstring() + L"（先前於 " + d.lastRecordedTime + L" 記錄過）");
                }
            } else {
                uiLog(L"[提醒] 檔案已不存在，略過刪除: " + d.fullPath.wstring());
            }
        } else {
            if (fs::exists(d.fullPath, ec) && fs::is_regular_file(d.fullPath, ec)) {
                long long size = GetFileSizeSafe(d.fullPath);
                if (size >= 0) {
                    std::string relPathUtf8 = WideToUtf8(d.relPath);
                    AppendRecord(logFile, relPathUtf8, size, uiLog);
                    history[ToLowerKey(d.relPath)] = NowString();
                    uiLog(L"[新增-重複但保留] " + d.fullPath.wstring() + L"（先前於 " + d.lastRecordedTime + L" 記錄過，本次選擇保留並重新記錄）");
                }
            } else {
                uiLog(L"[提醒] 檔案已不存在，略過記錄: " + d.fullPath.wstring());
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 目錄監控 (ReadDirectoryChangesW，Overlapped I/O + 可中斷)
// ---------------------------------------------------------------------------

void MonitorDirectory(const std::wstring& dirPath, const fs::path& logFile, HANDLE hStopEvent,
                       const LogFn& uiLog, HWND hwndMain, FileHistory& history) {
    HANDLE hDir = CreateFileW(
        dirPath.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,   // 開啟「目錄」+ 非同步 I/O
        nullptr);

    if (hDir == INVALID_HANDLE_VALUE) {
        uiLog(L"[錯誤] 無法開啟目錄進行監控: " + dirPath + L"（錯誤碼 " + std::to_wstring(GetLastError()) + L"）");
        return;
    }

    std::vector<BYTE> buffer(64 * 1024);
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    uiLog(L"[監控中] " + dirPath + L"（含所有子目錄）。");

    while (true) {
        DWORD bytesReturned = 0;
        ResetEvent(ov.hEvent);

        BOOL ok = ReadDirectoryChangesW(
            hDir,
            buffer.data(),
            (DWORD)buffer.size(),
            TRUE,               // 監控子目錄
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE,
            &bytesReturned,
            &ov,
            nullptr);

        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err != ERROR_IO_PENDING) {
            uiLog(L"[錯誤] ReadDirectoryChangesW 失敗，錯誤碼 " + std::to_wstring(err));
            break;
        }

        HANDLE waitHandles[2] = { ov.hEvent, hStopEvent };
        DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);

        if (wait == WAIT_OBJECT_0 + 1) {
            // 使用者要求停止
            CancelIoEx(hDir, &ov);
            DWORD dummy = 0;
            GetOverlappedResult(hDir, &ov, &dummy, TRUE);
            break;
        }

        DWORD transferred = 0;
        if (!GetOverlappedResult(hDir, &ov, &transferred, FALSE) || transferred == 0) {
            continue;
        }

        std::vector<DupCandidate> duplicates;

        BYTE* ptr = buffer.data();
        while (true) {
            FILE_NOTIFY_INFORMATION* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr);

            std::wstring relName(info->FileName, info->FileNameLength / sizeof(WCHAR));
            fs::path fullPath = fs::path(dirPath) / relName;

            // 只處理「新檔案出現」的情況：
            //   FILE_ACTION_ADDED            -> 新增 / 從別的磁碟或位置移入、複製進來
            //   FILE_ACTION_RENAMED_NEW_NAME -> 同一磁碟內移動或改名進來，也會觸發這個
            if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
                // 略作重試等待，確保檔案搬移/寫入已完成，並確認它是「檔案」而非資料夾
                for (int retry = 0; retry < 10; ++retry) {
                    std::error_code ec;
                    if (fs::exists(fullPath, ec) && fs::is_regular_file(fullPath, ec)) {
                        if (!fs::equivalent(fullPath, logFile, ec)) {
                            long long size = GetFileSizeSafe(fullPath);
                            if (size >= 0) {
                                std::wstring key = ToLowerKey(relName);
                                auto histIt = history.find(key);
                                if (histIt == history.end()) {
                                    // 從未記錄過：直接記錄，不必理會
                                    std::string relNameUtf8 = WideToUtf8(relName);
                                    AppendRecord(logFile, relNameUtf8, size, uiLog);
                                    history[key] = NowString();
                                    uiLog(L"[新增] " + fullPath.wstring());
                                } else {
                                    // 曾經記錄過：先收集起來，等這批事件都處理完再一次跳出提示
                                    duplicates.push_back({ relName, fullPath, size, histIt->second });
                                }
                            }
                        }
                        break;
                    }
                    Sleep(100);
                }
            }

            if (info->NextEntryOffset == 0) break;
            ptr += info->NextEntryOffset;
        }

        if (!duplicates.empty()) {
            HandleDuplicateFiles(duplicates, logFile, history, hwndMain, uiLog);
        }
    }

    CloseHandle(ov.hEvent);
    CloseHandle(hDir);
}

// ---------------------------------------------------------------------------
// 背景監控執行緒（每個資料夾各自跑一份）
// ---------------------------------------------------------------------------

struct StartParams {
    int entryId;
    std::wstring targetDir;
    HWND hwndMain;
    HANDLE hStopEvent;
};

void WorkerThreadProc(StartParams params) {
    fs::path root(params.targetDir);
    fs::path logFile = root / L"file_list.txt";

    LogFn uiLog = [hwnd = params.hwndMain](const std::wstring& text) {
        PostUiLog(hwnd, text);
    };

    FileHistory history; // 本次監控 session 用的歷史紀錄（相對路徑 -> 上次記錄時間）

    uiLog(L"目標目錄: " + root.wstring());
    uiLog(L"清單檔案: " + logFile.wstring());
    uiLog(L"正在讀取歷史紀錄並掃描目前檔案...");
    LoadOrCreateLogFile(logFile, history);
    ScanForNewFiles(root, logFile, history);
    uiLog(L"初始掃描完成（" + root.wstring() + L" 歷史紀錄共 " + std::to_wstring(history.size()) + L" 筆）。");

    MonitorDirectory(root.wstring(), logFile, params.hStopEvent, uiLog, params.hwndMain, history);

    PostMessageW(params.hwndMain, WM_APP_MONITOR_STOPPED, (WPARAM)params.entryId, 0);
}

// ---------------------------------------------------------------------------
// 資料夾選擇對話框
// ---------------------------------------------------------------------------

std::wstring BrowseForFolder(HWND owner) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = L"請選擇要監控的資料夾";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";

    std::wstring result;
    wchar_t path[MAX_PATH];
    if (SHGetPathFromIDListW(pidl, path)) {
        result = path;
    }
    CoTaskMemFree(pidl);
    return result;
}

// ---------------------------------------------------------------------------
// 系統匣圖示
// ---------------------------------------------------------------------------

void AddTrayIcon(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = TRAY_UID;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, L"FileWatcher 檔案監控程式");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayIconAdded = true;
}

void RemoveTrayIcon() {
    if (g_trayIconAdded) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconAdded = false;
    }
}

void ShowTrayBalloon(const std::wstring& title, const std::wstring& text) {
    if (!g_trayIconAdded) return;
    wcscpy_s(g_nid.szInfoTitle, title.c_str());
    wcscpy_s(g_nid.szInfo, text.c_str());
    g_nid.dwInfoFlags = NIIF_INFO;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; // 還原，避免下次 NIM_MODIFY 又跳氣球
}

// ---------------------------------------------------------------------------
// 資料夾清單（ListView）輔助函式
// ---------------------------------------------------------------------------

int FindListRowByEntryId(HWND hList, int id) {
    int count = ListView_GetItemCount(hList);
    for (int i = 0; i < count; ++i) {
        LVITEMW item = {};
        item.mask = LVIF_PARAM;
        item.iItem = i;
        ListView_GetItem(hList, &item);
        if ((int)item.lParam == id) return i;
    }
    return -1;
}

void AddFolderRow(HWND hList, int id, const std::wstring& folder, const std::wstring& status) {
    LVITEMW item = {};
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(hList);
    item.pszText = (LPWSTR)folder.c_str();
    item.lParam = (LPARAM)id;
    int idx = ListView_InsertItem(hList, &item);
    ListView_SetItemText(hList, idx, 1, (LPWSTR)status.c_str());
}

void UpdateFolderRowStatus(int id, const std::wstring& status) {
    int idx = FindListRowByEntryId(g_hListFolders, id);
    if (idx >= 0) {
        ListView_SetItemText(g_hListFolders, idx, 1, (LPWSTR)status.c_str());
    }
}

// 取得目前在清單中被選取的監控項目；沒有選取則回傳 nullptr
MonitorEntry* GetSelectedEntry() {
    int sel = ListView_GetNextItem(g_hListFolders, -1, LVNI_SELECTED);
    if (sel < 0) return nullptr;
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = sel;
    ListView_GetItem(g_hListFolders, &item);
    return FindEntryById((int)item.lParam);
}

// ---------------------------------------------------------------------------
// 視窗介面
// ---------------------------------------------------------------------------

static void AppendLogLine(HWND hEdit, const std::wstring& line) {
    int len = GetWindowTextLengthW(hEdit);
    SendMessageW(hEdit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring withNewline = line + L"\r\n";
    SendMessageW(hEdit, EM_REPLACESEL, FALSE, (LPARAM)withNewline.c_str());
}

static void LayoutControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    MoveWindow(g_hListFolders, 10, 10, w - 20, 150, TRUE);
    MoveWindow(g_hEditLog, 10, 228, w - 20, h - 238, TRUE);
}

// 開始監控某個資料夾（供主視窗按鈕、系統匣「開始監控全部」、開機自動啟動共用）。
// silent=true 時（開機自動啟動、系統匣「開始監控全部」等批次情境），資料夾目前無法
// 存取（例如是抽取式磁碟／隨身碟根目錄，這次開機沒有接上）不會跳出錯誤視窗，只會
// 靜靜略過並記一筆訊息，清單裡該資料夾仍然保留，等下次啟動再重新偵測一次。
void StartMonitoringEntry(MonitorEntry* entry, HWND hwndMain, bool silent = false) {
    if (!entry || entry->isMonitoring) return;

    std::error_code ec;
    if (!fs::exists(entry->targetDir, ec) || !fs::is_directory(entry->targetDir, ec)) {
        if (silent) {
            UpdateFolderRowStatus(entry->id, L"無法存取(略過)");
            AppendLogLine(g_hEditLog, L"[略過] " + entry->targetDir +
                L" 目前無法存取（可能是磁碟機/隨身碟尚未接上），本次啟動先略過此資料夾，下次啟動會重新偵測。");
        } else {
            MessageBoxW(hwndMain, (L"資料夾不存在或無效：\n" + entry->targetDir).c_str(), L"錯誤", MB_OK | MB_ICONERROR);
        }
        return;
    }

    // 記住目前整份清單，並註冊開機自動啟動，下次開機會自動在背景監控清單裡的所有資料夾
    PersistFolderList();
    RegisterAutoStart(GetExePath());

    if (!entry->hStopEvent) entry->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(entry->hStopEvent);
    entry->isMonitoring = true;
    UpdateFolderRowStatus(entry->id, L"監控中");

    StartParams params{ entry->id, entry->targetDir, hwndMain, entry->hStopEvent };
    entry->thread = std::thread(WorkerThreadProc, params);
}

// 停止監控某個資料夾（非同步：只是發出停止訊號，實際停止完成由 WM_APP_MONITOR_STOPPED 通知）
void StopMonitoringEntry(MonitorEntry* entry) {
    if (!entry || !entry->isMonitoring) return;
    SetEvent(entry->hStopEvent);
    UpdateFolderRowStatus(entry->id, L"停止中...");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_hListFolders = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL,
            10, 10, 640, 150, hwnd, (HMENU)(INT_PTR)IDC_LIST_FOLDERS, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(g_hListFolders, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
        ApplyFont(g_hListFolders);

        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 460;
        col.pszText = (LPWSTR)L"監控資料夾";
        ListView_InsertColumn(g_hListFolders, 0, &col);
        col.cx = 160;
        col.pszText = (LPWSTR)L"狀態";
        ListView_InsertColumn(g_hListFolders, 1, &col);

        HWND hBtnAdd = CreateWindowExW(0, L"BUTTON", L"新增資料夾...",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            10, 168, 130, 26, hwnd, (HMENU)(INT_PTR)IDC_BTN_ADD_FOLDER, nullptr, nullptr);
        HWND hBtnRemove = CreateWindowExW(0, L"BUTTON", L"移除資料夾",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            146, 168, 110, 26, hwnd, (HMENU)(INT_PTR)IDC_BTN_REMOVE_FOLDER, nullptr, nullptr);
        g_hBtnStartStop = CreateWindowExW(0, L"BUTTON", L"開始/停止監控",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            262, 168, 130, 26, hwnd, (HMENU)(INT_PTR)IDC_BTN_STARTSTOP, nullptr, nullptr);
        g_hBtnExport = CreateWindowExW(0, L"BUTTON", L"匯出檔案清單",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            398, 168, 130, 26, hwnd, (HMENU)(INT_PTR)IDC_BTN_EXPORT, nullptr, nullptr);
        ApplyFont(hBtnAdd);
        ApplyFont(hBtnRemove);
        ApplyFont(g_hBtnStartStop);
        ApplyFont(g_hBtnExport);

        HWND hLabelLog = CreateWindowExW(0, L"STATIC", L"監控紀錄：", WS_CHILD | WS_VISIBLE,
            10, 204, 100, 20, hwnd, nullptr, nullptr, nullptr);
        ApplyFont(hLabelLog);

        g_hEditLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
            10, 228, 640, 260, hwnd, (HMENU)(INT_PTR)IDC_EDIT_LOG, nullptr, nullptr);
        ApplyFont(g_hEditLog);

        // 載入設定檔（可能有多個資料夾）。注意：目前「無法存取」的路徑（例如隨身碟/外接
        // 磁碟這次開機沒有接上）**不會**從清單或設定檔移除——只是這次啟動時略過它的監控，
        // 清單裡繼續保留，下次啟動（例如裝置接上之後）會重新偵測一次。是否移除只由使用者
        // 自己按「移除資料夾」決定。
        std::vector<std::wstring> savedFolders = LoadConfig();
        std::error_code ec;

        if (!savedFolders.empty()) {
            for (auto& f : savedFolders) {
                auto entry = std::make_unique<MonitorEntry>();
                entry->id = g_nextEntryId++;
                entry->targetDir = f;
                entry->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                bool available = fs::exists(f, ec) && fs::is_directory(f, ec);
                AddFolderRow(g_hListFolders, entry->id, f, available ? L"已停止" : L"無法存取(略過)");
                g_entries.push_back(std::move(entry));
            }
            g_silentAutoStart = true;
        } else {
            // 第一次執行：預設加入目前工作目錄一筆，方便使用者直接按「開始/停止監控」
            wchar_t cwd[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, cwd);
            auto entry = std::make_unique<MonitorEntry>();
            entry->id = g_nextEntryId++;
            entry->targetDir = cwd;
            entry->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            AddFolderRow(g_hListFolders, entry->id, cwd, L"已停止");
            g_entries.push_back(std::move(entry));
            g_silentAutoStart = false;
        }

        AddTrayIcon(hwnd);

        LayoutControls(hwnd);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == IDC_BTN_ADD_FOLDER) {
            std::wstring folder = BrowseForFolder(hwnd);
            if (!folder.empty()) {
                std::wstring key = ToLowerKey(folder);
                bool alreadyExists = false;
                for (auto& e : g_entries) {
                    if (ToLowerKey(e->targetDir) == key) { alreadyExists = true; break; }
                }
                if (alreadyExists) {
                    MessageBoxW(hwnd, L"這個資料夾已經在清單裡了。", L"提示", MB_OK | MB_ICONINFORMATION);
                } else {
                    auto entry = std::make_unique<MonitorEntry>();
                    entry->id = g_nextEntryId++;
                    entry->targetDir = folder;
                    entry->hStopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                    AddFolderRow(g_hListFolders, entry->id, folder, L"已停止");
                    g_entries.push_back(std::move(entry));
                    PersistFolderList();
                }
            }
        } else if (id == IDC_BTN_REMOVE_FOLDER) {
            MonitorEntry* entry = GetSelectedEntry();
            if (!entry) {
                MessageBoxW(hwnd, L"請先在清單中選擇要移除的資料夾。", L"提示", MB_OK | MB_ICONINFORMATION);
            } else if (entry->isMonitoring) {
                MessageBoxW(hwnd, L"這個資料夾正在監控中，請先按「開始/停止監控」停止後再移除。", L"提示", MB_OK | MB_ICONINFORMATION);
            } else {
                int row = FindListRowByEntryId(g_hListFolders, entry->id);
                if (row >= 0) ListView_DeleteItem(g_hListFolders, row);
                if (entry->hStopEvent) CloseHandle(entry->hStopEvent);
                int removeId = entry->id;
                g_entries.erase(std::remove_if(g_entries.begin(), g_entries.end(),
                    [removeId](const std::unique_ptr<MonitorEntry>& e) { return e->id == removeId; }),
                    g_entries.end());
                PersistFolderList();
            }
        } else if (id == IDC_BTN_STARTSTOP) {
            MonitorEntry* entry = GetSelectedEntry();
            if (!entry) {
                MessageBoxW(hwnd, L"請先在清單中選擇一個資料夾，或按「新增資料夾...」加入。", L"提示", MB_OK | MB_ICONINFORMATION);
            } else if (entry->isMonitoring) {
                StopMonitoringEntry(entry);
            } else {
                StartMonitoringEntry(entry, hwnd);
            }
        } else if (id == IDC_BTN_EXPORT) {
            MonitorEntry* entry = GetSelectedEntry();
            if (!entry) {
                MessageBoxW(hwnd, L"請先在清單中選擇要掃描的資料夾。", L"提示", MB_OK | MB_ICONINFORMATION);
            } else if (g_isExporting) {
                MessageBoxW(hwnd, L"正在匯出中，請稍候。", L"提示", MB_OK | MB_ICONINFORMATION);
            } else {
                std::error_code ec;
                if (!fs::exists(entry->targetDir, ec) || !fs::is_directory(entry->targetDir, ec)) {
                    MessageBoxW(hwnd, L"選擇的路徑不是有效的資料夾。", L"錯誤", MB_OK | MB_ICONERROR);
                } else {
                    g_isExporting = true;
                    EnableWindow(g_hBtnExport, FALSE);
                    ExportParams params{ entry->targetDir, hwnd };
                    g_exportThread = std::thread(ExportWorkerProc, params);
                }
            }
        } else if (id == ID_TRAY_SHOW) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (id == ID_TRAY_START_ALL) {
            for (auto& e : g_entries) StartMonitoringEntry(e.get(), hwnd, /*silent=*/true);
        } else if (id == ID_TRAY_STOP_ALL) {
            for (auto& e : g_entries) StopMonitoringEntry(e.get());
        } else if (id == ID_TRAY_EXIT) {
            g_forceExit = true;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_TRAYICON: {
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"顯示視窗");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_START_ALL, L"開始監控全部");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_STOP_ALL, L"停止監控全部");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"結束程式");
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
            PostMessageW(hwnd, WM_NULL, 0, 0);
            DestroyMenu(hMenu);
        }
        return 0;
    }

    case WM_APP_LOG: {
        wchar_t* text = (wchar_t*)lParam;
        AppendLogLine(g_hEditLog, text);
        delete[] text;
        return 0;
    }

    case WM_APP_MONITOR_STOPPED: {
        int entryId = (int)wParam;
        MonitorEntry* entry = FindEntryById(entryId);
        if (entry) {
            if (entry->thread.joinable()) entry->thread.join();
            entry->isMonitoring = false;
            UpdateFolderRowStatus(entryId, L"已停止");
            AppendLogLine(g_hEditLog, L"[已停止] " + entry->targetDir + L" 的監控已結束。");
        }
        return 0;
    }

    case WM_APP_SCAN_DONE: {
        if (g_exportThread.joinable()) g_exportThread.join();
        g_isExporting = false;
        EnableWindow(g_hBtnExport, TRUE);
        return 0;
    }

    case WM_APP_AUTOSTART_ALL: {
        for (auto& e : g_entries) StartMonitoringEntry(e.get(), hwnd, /*silent=*/true);
        return 0;
    }

    case WM_CLOSE: {
        if (g_forceExit) {
            for (auto& e : g_entries) {
                if (e->isMonitoring) SetEvent(e->hStopEvent);
            }
            for (auto& e : g_entries) {
                if (e->thread.joinable()) e->thread.join();
            }
            if (g_exportThread.joinable()) g_exportThread.join();
            RemoveTrayIcon();
            DestroyWindow(hwnd);
        } else {
            // 按 X 只是縮到系統匣，監控繼續在背景執行；真正結束要用系統匣選單。
            ShowWindow(hwnd, SW_HIDE);
            if (!g_hasShownTrayHint) {
                ShowTrayBalloon(L"FileWatcher 仍在背景執行",
                    L"程式已縮小到系統匣，監控仍持續進行。要完全結束請在系統匣圖示按右鍵選「結束程式」。");
                g_hasShownTrayHint = true;
            }
        }
        return 0;
    }

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// 主程式進入點
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    g_hFont = CreateUIFont();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"FileWatcherMainWnd";
    RegisterClassExW(&wc);

    // 注意：不加 WS_VISIBLE，視窗預設是隱藏的；要不要顯示由下面依 g_silentAutoStart 決定。
    HWND hwnd = CreateWindowExW(0, L"FileWatcherMainWnd", L"FileWatcher 檔案監控程式",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 560,
        nullptr, nullptr, hInstance, nullptr);

    if (g_silentAutoStart) {
        // 已有設定檔（表示之前設定過）：不顯示視窗，直接在背景開始監控清單裡的所有資料夾。
        PostMessageW(hwnd, WM_APP_AUTOSTART_ALL, 0, 0);
    } else {
        // 第一次執行（尚未設定過）：正常顯示視窗，讓使用者新增資料夾、按「開始/停止監控」。
        ShowWindow(hwnd, nCmdShow);
        UpdateWindow(hwnd);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hFont) DeleteObject(g_hFont);
    CoUninitialize();
    return (int)msg.wParam;
}
