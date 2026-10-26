// ==WindhawkMod==
// @id              quick-save-and-open
// @name            Quick Save and Open
// @description     Search folders, recall recent locations, and follow Explorer in Windows Open/Save dialogs.
// @version         0.2.0
// @author          xpeng
// @github          https://github.com/xiaopeng12138
// @homepage        https://xpengs.com/
// @include         *
// @exclude         windhawk.exe
// @exclude         windhawk-ui.exe
// @exclude         *\Windhawk\UI\*
// @compilerOptions -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lcomctl32 -lcomdlg32 -lgdi32 -ladvapi32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Quick Save and Open

By [xpeng](https://xpengs.com/) ([GitHub](https://github.com/xiaopeng12138)).
Assisted by Codex.

A Listary-inspired folder navigator for File Explorer and standard modern and
legacy Windows Open/Save dialogs, plus modern folder pickers. Independent implementation;
not affiliated with Listary.

* **Ctrl+G** in a file dialog: jump to the last folder visited in Explorer.
* **Ctrl+Shift+G** in a dialog or Explorer: show the searchable folder popup.
* Click **QuickSO folders** in a modern dialog to show suggestions.
  Legacy dialogs use the popup and Ctrl+Shift+G.
* Click a folder in Explorer, then return to the file dialog: follow that folder.
  This can be turned off in Settings.
* Type to filter open/recent/pinned folders and search the Windows Search index.
* Paste an absolute path, UNC path, `%USERPROFILE%` path, or `~/...` to go there.
* Up/Down select a result, Enter navigates, Escape dismisses the popup.

The mod changes the folder using Shell APIs, without submitting the file dialog
or replacing its filename. You still click Open or Save yourself.

## Coverage

Windows 10/11, x86/x64. Supports modern IFileDialog dialogs and standard legacy
GetOpenFileName/GetSaveFileName dialogs, both ANSI and Unicode, including the
Explorer-style and old pre-Explorer templates. The oldest templates navigate
local/mapped drives and their current UNC share using native directory controls.
Arbitrary new UNC shares in those oldest templates are not supported.
Custom app dialogs, protected processes, and virtual Shell locations such as
Home and This PC are outside this version.
Other dialog-replacement mods may prevent detection. Only filesystem folders
are offered. The visible Shell view is used to distinguish Windows 11 tabs.
Windows Search results cover indexed folders only; when the service is off,
open/recent/pinned folders and direct paths still work. This is not a whole-disk
indexer or a copy of Listary's global file search.

Settings apply to newly opened dialogs. For reliable unloading, close file
dialogs before disabling/updating the mod (the hooked dialog calls are modal).

## Version 0.2.0

Fixes return-to-dialog detection across applications, including missing activation
window handles and delayed Explorer snapshots. Adds legacy dialog support.

## Local data

Recent paths and the last Explorer location are stored locally under
`HKEY_CURRENT_USER\Software\QuickSO`. No network service is used. To erase
history, close file dialogs, disable the mod, and delete that key.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- followExplorer: true
  $name: Follow Explorer when returning to a file dialog
  $description: After switching from an Explorer window back to a file dialog, navigate to that Explorer folder.
- showOnOpen: true
  $name: Show folder suggestions when a file dialog opens
- indexedSearch: true
  $name: Search indexed folders
  $description: Search the local Windows Search index after typing at least two characters. Open, pinned, and recent folders always remain available.
- rememberHistory: true
  $name: Remember visited folders
- maxRecent: 40
  $name: Maximum recent folders
  $description: Between 5 and 100 folders.
- pinnedFolders:
  - path: ""
  $name: Pinned folders
  $description: Add absolute folder paths. Environment variables are supported.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dlgs.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <exdisp.h>
#include <servprov.h>
#include <sddl.h>
#define DBINITCONSTANTS
#include <oledb.h>
#include <msdasc.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace quickso {

template<class T> class ComPtr {
    T* p_ = nullptr;
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : p_(std::exchange(other.p_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) { if (p_) p_->Release(); p_ = std::exchange(other.p_, nullptr); }
        return *this;
    }
    T* operator->() const { return p_; }
    T* get() const { return p_; }
    T** put() { if (p_) p_->Release(); p_ = nullptr; return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
};

struct Settings {
    bool followExplorer = true, showOnOpen = true, indexedSearch = true;
    bool rememberHistory = true;
    int maxRecent = 40;
    std::vector<std::wstring> pinned;
};
std::mutex g_settingsMutex;
Settings g_settings;
std::atomic<bool> g_stopping{false};
HINSTANCE g_instance = nullptr;
HANDLE g_historyMutex = nullptr;
#ifdef QUICKSO_TEST
constexpr wchar_t kRegistry[] = L"Software\\QuickSO.SmokeTests";
#else
constexpr wchar_t kRegistry[] = L"Software\\QuickSO";
#endif
constexpr wchar_t kPaletteClass[] = L"QuickSO.FolderPalette.0.1";
#ifdef QUICKSO_TEST
constexpr wchar_t kBrokerClass[] = L"QuickSO.ExplorerBroker.SmokeTests";
#else
constexpr wchar_t kBrokerClass[] = L"QuickSO.ExplorerBroker.0.1";
#endif
constexpr DWORD kButtonId = 0x71736F31;
constexpr UINT kShowPalette = WM_APP + 0x351;
constexpr UINT kJumpExplorer = WM_APP + 0x352;
constexpr UINT kSearchReady = WM_APP + 0x353;
constexpr UINT kDetach = WM_APP + 0x354;
constexpr UINT kExplorerPopup = WM_APP + 0x355;
constexpr UINT kRefreshExplorer = WM_APP + 0x356;
constexpr UINT kForegroundChanged = WM_APP + 0x357;
constexpr UINT_PTR kFollowTimer = 0x51534F;
constexpr UINT_PTR kObserveTimer = 0x515350;

Settings CurrentSettings() {
    std::lock_guard lock(g_settingsMutex);
    return g_settings;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

std::wstring Trim(std::wstring text) {
    auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    text = text.substr(first, text.find_last_not_of(L" \t\r\n") - first + 1);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"')
        text = text.substr(1, text.size() - 2);
    return text;
}

bool IsFilesystemPath(const std::wstring& p) {
    if (p.size() >= 3 && iswalpha(p[0]) && p[1] == L':' && p[2] == L'\\') return true;
    // Do not accept device namespace paths or incomplete server names.
    if (p.size() > 4 && p[0] == L'\\' && p[1] == L'\\' &&
        p[2] != L'?' && p[2] != L'.') {
        auto share = p.find(L'\\', 2);
        return share != std::wstring::npos && share > 2 && share + 1 < p.size();
    }
    return false;
}

std::wstring ExpandPath(std::wstring path) {
    path = Trim(std::move(path));
    if (path == L"~" || path.starts_with(L"~/") || path.starts_with(L"~\\"))
        path = L"%USERPROFILE%" + path.substr(1);
    DWORD count = ExpandEnvironmentStringsW(path.c_str(), nullptr, 0);
    if (!count || count > 32768) return {};
    std::vector<wchar_t> buffer(count);
    if (!ExpandEnvironmentStringsW(path.c_str(), buffer.data(), count)) return {};
    path.assign(buffer.data());
    std::replace(path.begin(), path.end(), L'/', L'\\');
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return IsFilesystemPath(path) ? path : std::wstring{};
}

void LoadSettings() {
    Settings s;
    s.followExplorer = Wh_GetIntSetting(L"followExplorer") != 0;
    s.showOnOpen = Wh_GetIntSetting(L"showOnOpen") != 0;
    s.indexedSearch = Wh_GetIntSetting(L"indexedSearch") != 0;
    s.rememberHistory = Wh_GetIntSetting(L"rememberHistory") != 0;
    s.maxRecent = std::clamp(Wh_GetIntSetting(L"maxRecent"), 5, 100);
    for (int i = 0; i < 100; ++i) {
        PCWSTR raw = Wh_GetStringSetting(L"pinnedFolders[%d].path", i);
        std::wstring value = raw ? raw : L"";
        Wh_FreeStringSetting(raw);
        if (value.empty()) break;
        auto path = ExpandPath(std::move(value));
        if (!path.empty()) s.pinned.push_back(std::move(path));
    }
    std::lock_guard lock(g_settingsMutex);
    g_settings = std::move(s);
}

std::vector<std::wstring> ReadPaths(PCWSTR name) {
    DWORD bytes = 0, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistry, name, RRF_RT_REG_MULTI_SZ,
                     &type, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < 2 * sizeof(wchar_t) || bytes > 1024 * 1024 || bytes % sizeof(wchar_t)) return {};
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistry, name, RRF_RT_REG_MULTI_SZ,
                     nullptr, buffer.data(), &bytes) != ERROR_SUCCESS) return {};
    std::vector<std::wstring> result;
    const wchar_t* cursor = buffer.data();
    const wchar_t* end = buffer.data() + bytes / sizeof(wchar_t);
    while (cursor < end && *cursor && result.size() < 256) {
        const wchar_t* finish = std::find(cursor, end, L'\0');
        if (finish == end) break;
        result.emplace_back(cursor, finish);
        cursor = finish + 1;
    }
    return result;
}

void WritePaths(PCWSTR name, const std::vector<std::wstring>& paths) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistry, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    std::vector<wchar_t> buffer;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        buffer.insert(buffer.end(), path.begin(), path.end());
        buffer.push_back(L'\0');
    }
    buffer.push_back(L'\0');
    if (buffer.size() == 1) buffer.push_back(L'\0');
    RegSetValueExW(key, name, 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(buffer.data()),
                   static_cast<DWORD>(buffer.size() * sizeof(wchar_t)));
    RegCloseKey(key);
}

void Remember(const std::wstring& path) {
    Settings s = CurrentSettings();
    if (!s.rememberHistory || !IsFilesystemPath(path) || !g_historyMutex) return;
    DWORD wait = WaitForSingleObject(g_historyMutex, 40);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    auto paths = ReadPaths(L"RecentFolders");
    paths.erase(std::remove_if(paths.begin(), paths.end(), [&](const auto& p) {
        return SamePath(p, path);
    }), paths.end());
    paths.insert(paths.begin(), path);
    if (paths.size() > static_cast<size_t>(s.maxRecent)) paths.resize(s.maxRecent);
    WritePaths(L"RecentFolders", paths);
    ReleaseMutex(g_historyMutex);
}

std::wstring UserMutexName() {
    HANDLE token = nullptr;
    std::wstring name = L"Local\\QuickSO.History.";
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    if (GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes)) {
        PWSTR sid = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) {
            name += sid;
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return name.back() == L'.' ? std::wstring{} : name;
}

bool IsExplorer(HWND hwnd) {
    wchar_t cls[64]{};
    GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
    return wcscmp(cls, L"CabinetWClass") == 0 || wcscmp(cls, L"ExploreWClass") == 0;
}

struct ExplorerSnapshot {
    std::wstring path;
    HWND hwnd = nullptr;
    ULONGLONG capturedAt = 0;
};

ExplorerSnapshot LastExplorer() {
    auto data = ReadPaths(L"LastExplorer");
    if (data.size() < 2 || !IsFilesystemPath(data[0])) return {};
    wchar_t* end = nullptr;
    auto value = wcstoull(data[1].c_str(), &end, 16);
    if (!end || *end || !value) return {};
    HWND hwnd = reinterpret_cast<HWND>(static_cast<UINT_PTR>(value));
    if (!IsExplorer(hwnd)) return {}; // Do not resurrect stale locations after Explorer exits.
    ULONGLONG capturedAt = data.size() >= 3 ? wcstoull(data[2].c_str(), nullptr, 10) : 0;
    return {data[0], hwnd, capturedAt};
}

struct Entry { std::wstring path, source; };

void AddEntry(std::vector<Entry>& entries, std::wstring path, PCWSTR source) {
    if (!IsFilesystemPath(path)) return;
    if (std::any_of(entries.begin(), entries.end(), [&](const auto& e) { return SamePath(e.path, path); })) return;
    entries.push_back({std::move(path), source});
}

std::vector<Entry> Suggestions() {
    std::vector<Entry> result;
    auto last = LastExplorer();
    AddEntry(result, last.path, L"Last Explorer folder");
    for (const auto& p : CurrentSettings().pinned) AddEntry(result, p, L"Pinned");
    for (const auto& p : ReadPaths(L"OpenFolders")) AddEntry(result, p, L"Open in Explorer");
    for (const auto& p : ReadPaths(L"RecentFolders")) AddEntry(result, p, L"Recent");
    const KNOWNFOLDERID* ids[] = {&FOLDERID_Desktop, &FOLDERID_Downloads, &FOLDERID_Documents, &FOLDERID_Profile};
    for (auto id : ids) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*id, KF_FLAG_DONT_VERIFY, nullptr, &path))) {
            AddEntry(result, path, L"Known folder");
            CoTaskMemFree(path);
        }
    }
    return result;
}

bool Matches(const std::wstring& path, const std::wstring& query) {
    // Every whitespace-separated term must occur in the path; order is unimportant.
    size_t offset = 0;
    while (offset < query.size()) {
        auto end = query.find_first_of(L" \t", offset);
        auto word = query.substr(offset, end == std::wstring::npos ? end : end - offset);
        if (!word.empty() && !StrStrIW(path.c_str(), word.c_str())) return false;
        if (end == std::wstring::npos) break;
        offset = end + 1;
    }
    return true;
}

std::wstring SearchSql(const std::wstring& query) {
    std::wstring sql = L"SELECT TOP 80 System.ItemPathDisplay FROM SYSTEMINDEX WHERE System.ItemType = 'Directory'";
    size_t offset = 0;
    while (offset < query.size()) {
        size_t end = query.find_first_of(L" \t", offset);
        auto word = query.substr(offset, end == std::wstring::npos ? end : end - offset);
        if (!word.empty()) {
            std::wstring escaped;
            for (wchar_t ch : word) { escaped += ch; if (ch == L'\'') escaped += L'\''; }
            sql += L" AND System.ItemPathDisplay LIKE '%" + escaped + L"%'";
        }
        if (end == std::wstring::npos) break;
        offset = end + 1;
    }
    return sql;
}

// OLE DB runs only on a dedicated search thread. No disk crawl or COM query on
// the dialog's input thread. Results are bounded; providers receive a timeout.
HRESULT IndexedFolders(const std::wstring& query, std::vector<std::wstring>& paths,
                       const std::atomic<bool>& stop) {
    ComPtr<IDataInitialize> init;
    HRESULT hr = CoCreateInstance(CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IDataInitialize, reinterpret_cast<void**>(init.put()));
    if (FAILED(hr)) return hr;
    ComPtr<IDBInitialize> db;
    hr = init->GetDataSource(nullptr, CLSCTX_INPROC_SERVER,
        L"Provider=Search.CollatorDSO;Extended Properties='Application=Windows';",
        IID_IDBInitialize, reinterpret_cast<IUnknown**>(db.put()));
    if (FAILED(hr)) return hr;
    hr = db->Initialize();
    if (FAILED(hr)) return hr;
    ComPtr<IDBCreateSession> factory;
    hr = db->QueryInterface(IID_IDBCreateSession, reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) return hr;
    ComPtr<IDBCreateCommand> session;
    hr = factory->CreateSession(nullptr, IID_IDBCreateCommand,
                                reinterpret_cast<IUnknown**>(session.put()));
    if (FAILED(hr)) return hr;
    ComPtr<ICommandText> command;
    hr = session->CreateCommand(nullptr, IID_ICommandText,
                                reinterpret_cast<IUnknown**>(command.put()));
    if (FAILED(hr)) return hr;
    ComPtr<ICommandProperties> properties;
    if (SUCCEEDED(command->QueryInterface(IID_ICommandProperties, reinterpret_cast<void**>(properties.put())))) {
        DBPROP prop{};
        prop.dwPropertyID = DBPROP_COMMANDTIMEOUT;
        prop.dwOptions = DBPROPOPTIONS_OPTIONAL;
        prop.vValue.vt = VT_I4;
        prop.vValue.lVal = 3;
        DBPROPSET set{&prop, 1, DBPROPSET_ROWSET};
        properties->SetProperties(1, &set);
    }
    auto sql = SearchSql(query);
    hr = command->SetCommandText(DBGUID_DEFAULT, sql.c_str());
    if (FAILED(hr) || stop.load()) return FAILED(hr) ? hr : E_ABORT;
    ComPtr<IRowset> rows;
    hr = command->Execute(nullptr, IID_IRowset, nullptr, nullptr,
                          reinterpret_cast<IUnknown**>(rows.put()));
    if (FAILED(hr)) return hr;
    ComPtr<IAccessor> accessor;
    hr = rows->QueryInterface(IID_IAccessor, reinterpret_cast<void**>(accessor.put()));
    if (FAILED(hr)) return hr;
    struct Row { DBSTATUS status; DBLENGTH length; wchar_t path[32768]; };
    auto row = std::make_unique<Row>();
    DBBINDING binding{};
    binding.iOrdinal = 1;
    binding.obStatus = offsetof(Row, status);
    binding.obLength = offsetof(Row, length);
    binding.obValue = offsetof(Row, path);
    binding.dwPart = DBPART_VALUE | DBPART_LENGTH | DBPART_STATUS;
    binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
    binding.eParamIO = DBPARAMIO_NOTPARAM;
    binding.cbMaxLen = sizeof(row->path);
    binding.wType = DBTYPE_WSTR;
    HACCESSOR handle = 0;
    hr = accessor->CreateAccessor(DBACCESSOR_ROWDATA, 1, &binding, sizeof(Row), &handle, nullptr);
    if (FAILED(hr)) return hr;
    while (!stop.load() && paths.size() < 80) {
        DBCOUNTITEM count = 0;
        HROW* fetched = nullptr;
        hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &count, &fetched);
        if (FAILED(hr) || !count) { CoTaskMemFree(fetched); break; }
        *row = {};
        HRESULT dataHr = rows->GetData(fetched[0], handle, row.get());
        if (SUCCEEDED(dataHr) && row->status == DBSTATUS_S_OK &&
            row->length < sizeof(row->path) && IsFilesystemPath(row->path)) paths.emplace_back(row->path);
        rows->ReleaseRows(count, fetched, nullptr, nullptr, nullptr);
        CoTaskMemFree(fetched);
    }
    accessor->ReleaseAccessor(handle, nullptr);
    return FAILED(hr) ? hr : S_OK;
}

struct Navigator {
    virtual HRESULT Navigate(const std::wstring& path) = 0;
    virtual ~Navigator() = default;
};

class Palette {
    friend bool RegisterClasses();
    Navigator* navigator_;
    HWND owner_;
    HWND hwnd_ = nullptr, edit_ = nullptr, list_ = nullptr, status_ = nullptr;
    std::atomic<HWND> notifyWindow_{nullptr};
    HFONT font_ = nullptr, smallFont_ = nullptr;
    int dpi_ = 96;
    std::vector<Entry> base_, visible_;
    std::wstring query_;
    std::mutex searchMutex_;
    std::condition_variable searchCv_;
    std::atomic<bool> stopSearch_{false};
    std::thread searchThread_;
    std::wstring pendingQuery_;
    unsigned generation_ = 0, pendingGeneration_ = 0, resultGeneration_ = 0;
    bool pending_ = false;
    HRESULT searchHr_ = S_OK;
    std::vector<std::wstring> searchResult_;

    int D(int px) const { return MulDiv(px, dpi_, 96); }
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void Layout();
    void Filter();
    void SearchLoop();
    void QueryChanged();
    void DrawItem(const DRAWITEMSTRUCT*);
public:
    Palette(HWND owner, Navigator* navigator) : navigator_(navigator), owner_(owner) {}
    ~Palette();
    HWND window() const { return hwnd_; }
    bool Create();
    void Show(bool focus = true);
    void Hide() { if (hwnd_) ShowWindow(hwnd_, SW_HIDE); }
    void JumpExplorer();
    void Accept();
};

bool Palette::Create() {
    dpi_ = GetDpiForWindow(owner_);
    if (!dpi_) dpi_ = 96;
    font_ = CreateFontW(-D(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    smallFont_ = CreateFontW(-D(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Segoe UI");
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, kPaletteClass,
        L"QuickSO folders", WS_POPUP | WS_BORDER | WS_CLIPCHILDREN,
        0, 0, D(640), D(370), owner_, nullptr, g_instance, this);
    notifyWindow_.store(hwnd_);
    return hwnd_ != nullptr;
}

Palette::~Palette() {
    stopSearch_.store(true);
    searchCv_.notify_all();
    if (searchThread_.joinable()) searchThread_.join();
    if (hwnd_) DestroyWindow(hwnd_);
    if (font_) DeleteObject(font_);
    if (smallFont_) DeleteObject(smallFont_);
}

void Palette::Layout() {
    RECT r{}; GetClientRect(hwnd_, &r);
    int width = r.right;
    MoveWindow(edit_, D(12), D(39), width - D(24), D(29), TRUE);
    MoveWindow(list_, D(12), D(78), width - D(24), r.bottom - D(120), TRUE);
    MoveWindow(status_, D(12), r.bottom - D(34), width - D(24), D(28), TRUE);
    MoveWindow(GetDlgItem(hwnd_, 103), width - D(225), D(7), D(175), D(25), TRUE);
    MoveWindow(GetDlgItem(hwnd_, 104), width - D(43), D(7), D(30), D(25), TRUE);
}

void Palette::Show(bool focus) {
    if (!hwnd_ && !Create()) return;
    base_ = Suggestions();
    SetWindowTextW(edit_, L"");
    QueryChanged();
    RECT owner{}; GetWindowRect(owner_, &owner);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(owner_, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = std::min<int>(D(640), monitor.rcWork.right - monitor.rcWork.left);
    int height = std::min<int>(D(370), monitor.rcWork.bottom - monitor.rcWork.top);
    int x = std::clamp(owner.left + (owner.right - owner.left - width) / 2,
                       monitor.rcWork.left, monitor.rcWork.right - width);
    // Keep the popup on screen even with a maximized file dialog.
    int y = owner.bottom + D(4);
    if (y + height > monitor.rcWork.bottom) y = owner.bottom - height - D(8);
    y = std::clamp<int>(y, monitor.rcWork.top, monitor.rcWork.bottom - height);
    SetWindowPos(hwnd_, HWND_TOP, x, y, width, height,
                 SWP_SHOWWINDOW | (focus ? 0 : SWP_NOACTIVATE));
    if (focus) { SetForegroundWindow(hwnd_); SetFocus(edit_); }
}

void Palette::Filter() {
    visible_.clear();
    auto direct = ExpandPath(query_);
    if (!direct.empty()) AddEntry(visible_, direct, L"Go to path");
    for (const auto& entry : base_)
        if (query_.empty() || Matches(entry.path, query_)) AddEntry(visible_, entry.path, entry.source.c_str());
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (const auto& entry : visible_) SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.path.c_str()));
    if (!visible_.empty()) SendMessageW(list_, LB_SETCURSEL, 0, 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
}

void Palette::QueryChanged() {
    KillTimer(hwnd_, 1);
    wchar_t buffer[1025]{}; GetWindowTextW(edit_, buffer, ARRAYSIZE(buffer));
    query_ = Trim(buffer);
    ++generation_;
    // Remove results from a previous indexed query, retaining the local suggestions.
    base_.erase(std::remove_if(base_.begin(), base_.end(), [](const auto& e) { return e.source == L"Windows Search"; }), base_.end());
    Filter();
    SetWindowTextW(status_, L"Open / recent / pinned folders  |  Enter: go  |  Esc: close");
    if (CurrentSettings().indexedSearch && query_.size() >= 2 && query_.size() <= 128 && ExpandPath(query_).empty())
        SetTimer(hwnd_, 1, 250, nullptr);
}

void Palette::SearchLoop() {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        std::wstring query;
        unsigned generation;
        {
            std::unique_lock lock(searchMutex_);
            searchCv_.wait(lock, [&] { return pending_ || stopSearch_.load(); });
            if (stopSearch_.load()) break;
            query = pendingQuery_;
            generation = pendingGeneration_;
            pending_ = false;
        }
        std::vector<std::wstring> results;
        HRESULT hr = SUCCEEDED(apartment) ? IndexedFolders(query, results, stopSearch_) : apartment;
        {
            std::lock_guard lock(searchMutex_);
            searchResult_ = std::move(results);
            searchHr_ = hr;
            resultGeneration_ = generation;
        }
        if (!stopSearch_.load()) {
            if (HWND notify = notifyWindow_.load()) PostMessageW(notify, kSearchReady, generation, 0);
        }
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
}

void Palette::JumpExplorer() {
    auto last = LastExplorer();
    if (last.path.empty()) {
        SetWindowTextW(status_, L"Visit a filesystem folder in Explorer first.");
        return;
    }
    HRESULT hr = navigator_->Navigate(last.path);
    if (SUCCEEDED(hr)) { Hide(); SetForegroundWindow(owner_); }
    else { Show(); SetWindowTextW(status_, L"Could not open the Explorer folder. Check that it is available."); }
}

void Palette::Accept() {
    LRESULT index = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    std::wstring path;
    if (index >= 0 && static_cast<size_t>(index) < visible_.size()) path = visible_[index].path;
    else path = ExpandPath(query_);
    if (path.empty()) { SetWindowTextW(status_, L"Select a folder or enter an absolute folder path."); return; }
    HRESULT hr = navigator_->Navigate(path);
    if (SUCCEEDED(hr)) { Remember(path); Hide(); SetForegroundWindow(owner_); }
    else SetWindowTextW(status_, L"Could not open this folder. It may be unavailable or access may be denied.");
}

void Palette::DrawItem(const DRAWITEMSTRUCT* item) {
    if (item->itemID == static_cast<UINT>(-1) || item->itemID >= visible_.size()) return;
    bool selected = (item->itemState & ODS_SELECTED) != 0;
    FillRect(item->hDC, &item->rcItem, GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
    SetBkMode(item->hDC, TRANSPARENT);
    SetTextColor(item->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    const auto& entry = visible_[item->itemID];
    auto separator = entry.path.find_last_of(L'\\');
    std::wstring label = separator != std::wstring::npos && separator + 1 < entry.path.size()
        ? entry.path.substr(separator + 1) : entry.path;
    label += L"   [" + entry.source + L"]";
    RECT text = item->rcItem; text.left += D(9); text.right -= D(8); text.top += D(4);
    HGDIOBJ old = SelectObject(item->hDC, font_);
    DrawTextW(item->hDC, label.c_str(), -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    text.top += D(20);
    SelectObject(item->hDC, smallFont_);
    if (!selected) SetTextColor(item->hDC, GetSysColor(COLOR_GRAYTEXT));
    DrawTextW(item->hDC, entry.path.c_str(), -1, &text, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(item->hDC, old);
}

LRESULT CALLBACK Palette::InputProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                    UINT_PTR id, DWORD_PTR data) {
    auto self = reinterpret_cast<Palette*>(data);
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    if (msg == WM_KEYDOWN) {
        if (wp == 'G' && (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000)) {
            if (GetKeyState(VK_SHIFT) & 0x8000) self->Hide();
            else self->JumpExplorer();
            return 0;
        }
        if (wp == VK_RETURN) { self->Accept(); return 0; }
        if (wp == VK_ESCAPE) { self->Hide(); SetForegroundWindow(self->owner_); return 0; }
        if (wp == VK_TAB) { SetFocus(hwnd == self->edit_ ? self->list_ : self->edit_); return 0; }
        if (hwnd == self->edit_ && (wp == VK_DOWN || wp == VK_UP)) {
            int count = static_cast<int>(self->visible_.size());
            int index = static_cast<int>(SendMessageW(self->list_, LB_GETCURSEL, 0, 0));
            if (count) SendMessageW(self->list_, LB_SETCURSEL,
                std::clamp(index + (wp == VK_DOWN ? 1 : -1), 0, count - 1), 0);
            return 0;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_TAB)) return 0;
    if (hwnd == self->list_ && msg == WM_LBUTTONUP) {
        LRESULT result = DefSubclassProc(hwnd, msg, wp, lp);
        LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lp);
        RECT item{};
        POINT point{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        if (HIWORD(hit) == 0 && LOWORD(hit) < self->visible_.size() &&
            SendMessageW(hwnd, LB_GETITEMRECT, LOWORD(hit), reinterpret_cast<LPARAM>(&item)) != LB_ERR &&
            PtInRect(&item, point)) self->Accept();
        return result;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, InputProc, id);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK Palette::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Palette* self = reinterpret_cast<Palette*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Palette*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CREATE: {
        auto child = [&](PCWSTR cls, PCWSTR text, DWORD style, int id) {
            HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                0, 0, 0, 0, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(self->font_), TRUE);
            return h;
        };
        self->edit_ = child(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 100);
        SendMessageW(self->edit_, EM_SETLIMITTEXT, 1024, 0);
        SendMessageW(self->edit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(L"Search folders or paste a path..."));
        self->list_ = child(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
            LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 101);
        SendMessageW(self->list_, LB_SETITEMHEIGHT, 0, self->D(48));
        self->status_ = child(L"STATIC", L"", SS_LEFT, 102);
        SendMessageW(self->status_, WM_SETFONT, reinterpret_cast<WPARAM>(self->smallFont_), FALSE);
        child(L"BUTTON", L"Explorer (Ctrl+G)", WS_TABSTOP | BS_PUSHBUTTON, 103);
        child(L"BUTTON", L"X", WS_TABSTOP | BS_PUSHBUTTON, 104);
        SetWindowSubclass(self->edit_, InputProc, 1, reinterpret_cast<DWORD_PTR>(self));
        SetWindowSubclass(self->list_, InputProc, 1, reinterpret_cast<DWORD_PTR>(self));
        self->Layout();
        return 0;
    }
    case WM_SIZE: self->Layout(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
        RECT r{}; GetClientRect(hwnd, &r);
        FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
        r.left = self->D(13); r.top = self->D(10);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        HGDIOBJ old = SelectObject(dc, self->font_);
        DrawTextW(dc, L"QuickSO  |  Folders", -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old); EndPaint(hwnd, &ps); return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_WINDOW));
        SetTextColor(reinterpret_cast<HDC>(wp), GetSysColor(COLOR_GRAYTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight = self->D(48); return TRUE;
    case WM_DRAWITEM: self->DrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp)); return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) self->QueryChanged();
        if (LOWORD(wp) == 101 && HIWORD(wp) == LBN_DBLCLK) self->Accept();
        if (LOWORD(wp) == 103) self->JumpExplorer();
        if (LOWORD(wp) == 104 || LOWORD(wp) == IDCANCEL) self->Hide();
        return 0;
    case WM_TIMER:
        if (wp == 1) {
            KillTimer(hwnd, 1);
            if (!self->searchThread_.joinable()) self->searchThread_ = std::thread(&Palette::SearchLoop, self);
            {
                std::lock_guard lock(self->searchMutex_);
                self->pendingQuery_ = self->query_;
                self->pendingGeneration_ = self->generation_;
                self->pending_ = true;
            }
            SetWindowTextW(self->status_, L"Searching indexed folders...");
            self->searchCv_.notify_one();
        }
        return 0;
    case kSearchReady: {
        std::lock_guard lock(self->searchMutex_);
        if (self->resultGeneration_ != self->generation_) return 0;
        for (const auto& path : self->searchResult_) AddEntry(self->base_, path, L"Windows Search");
        self->Filter();
        if (FAILED(self->searchHr_)) SetWindowTextW(self->status_, L"Windows Search unavailable. Showing open / recent / pinned folders.");
        else if (self->visible_.empty()) SetWindowTextW(self->status_, L"No matching indexed folders. Paste a full path to visit another folder.");
        else SetWindowTextW(self->status_, L"Includes indexed folders  |  Enter: go  |  Esc: close");
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) self->Hide();
        return 0;
    case WM_CLOSE: self->Hide(); return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        self->notifyWindow_.store(nullptr);
        self->hwnd_ = nullptr;
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterClasses() {
    WNDCLASSW cls{};
    cls.hInstance = g_instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    cls.lpszClassName = kPaletteClass;
    cls.lpfnWndProc = Palette::WindowProc;
    return RegisterClassW(&cls) != 0;
}

std::mutex g_dialogMutex;
std::vector<HWND> g_dialogWindows;
class DialogSession;
thread_local std::vector<DialogSession*> t_dialogs;

class DialogSession final : public IFileDialogEvents,
                            public IFileDialogControlEvents, public Navigator {
    LONG references_ = 1;
    IFileDialog* dialog_; // Borrowed for the duration of the intercepted Show call.
    DWORD cookie_ = 0;
    HWND hwnd_ = nullptr, followFrom_ = nullptr;
    HWND previousForeground_ = nullptr;
    HHOOK keyHook_ = nullptr;
    HWINEVENTHOOK foregroundHook_ = nullptr;
    ULONGLONG followRequestedAt_ = 0, followDeadline_ = 0;
    std::unique_ptr<Palette> palette_;
    bool detached_ = false;
    Settings settings_ = CurrentSettings();
    static LRESULT CALLBACK DialogProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK KeyProc(int, WPARAM, LPARAM);
    static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    void Attach();
    void AttachWindow(HWND hwnd);
    void ObserveForeground(HWND foreground);
    void ArmFollow(HWND source);
    void CancelFollow();
    HRESULT NavigateLegacy(const std::wstring& path);
public:
    explicit DialogSession(IFileDialog* dialog) : dialog_(dialog) {}
    ~DialogSession() override = default;
    bool HotkeysEnabled() const { return keyHook_ != nullptr; }
    HWND window() const { return hwnd_; }
    void AttachLegacy(HWND hwnd) { AttachWindow(hwnd); }
    void Start() {
        if (!dialog_) return;
        // The customization API integrates the entry point with the native
        // dialog layout instead of covering any existing buttons.
        ComPtr<IFileDialogCustomize> customize;
        if (SUCCEEDED(dialog_->QueryInterface(IID_PPV_ARGS(customize.put())))) {
            CDCONTROLSTATEF state{};
            if (FAILED(customize->GetControlState(kButtonId, &state)))
                customize->AddPushButton(kButtonId, L"QuickSO folders (Ctrl+Shift+G)");
            else customize->SetControlState(kButtonId, CDCS_ENABLEDVISIBLE);
        }
        if (FAILED(dialog_->Advise(this, &cookie_))) cookie_ = 0;
    }
    void Detach() {
        if (detached_) return;
        detached_ = true;
        if (foregroundHook_) { UnhookWinEvent(foregroundHook_); foregroundHook_ = nullptr; }
        if (keyHook_) { UnhookWindowsHookEx(keyHook_); keyHook_ = nullptr; }
        palette_.reset();
        if (hwnd_) {
            KillTimer(hwnd_, kFollowTimer);
            KillTimer(hwnd_, kObserveTimer);
            RemoveWindowSubclass(hwnd_, DialogProc, reinterpret_cast<UINT_PTR>(this));
            std::lock_guard lock(g_dialogMutex);
            g_dialogWindows.erase(std::remove(g_dialogWindows.begin(), g_dialogWindows.end(), hwnd_), g_dialogWindows.end());
            hwnd_ = nullptr;
        }
        ComPtr<IFileDialogCustomize> customize;
        if (dialog_ && SUCCEEDED(dialog_->QueryInterface(IID_PPV_ARGS(customize.put()))))
            customize->SetControlState(kButtonId, CDCS_INACTIVE);
        if (cookie_) { DWORD cookie = std::exchange(cookie_, 0); dialog_->Unadvise(cookie); }
    }
    HRESULT Navigate(const std::wstring& path) override {
        if (detached_ || !IsFilesystemPath(path)) return E_INVALIDARG;
        if (!dialog_) return NavigateLegacy(path);
        ComPtr<IShellItem> folder;
        HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(folder.put()));
        if (FAILED(hr)) return hr;
        SFGAOF attributes = 0;
        hr = folder->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
        if (FAILED(hr) || (attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM))
            return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        PWSTR filename = nullptr;
        FILEOPENDIALOGOPTIONS options{};
        dialog_->GetOptions(&options);
        HRESULT nameHr = (options & FOS_PICKFOLDERS) ? E_NOTIMPL : dialog_->GetFileName(&filename);
        hr = dialog_->SetFolder(folder.get());
        if (SUCCEEDED(hr) && SUCCEEDED(nameHr) && filename) dialog_->SetFileName(filename);
        CoTaskMemFree(filename);
        return hr;
    }
    IFACEMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents)
            *out = static_cast<IFileDialogEvents*>(this);
        else if (iid == IID_IFileDialogControlEvents)
            *out = static_cast<IFileDialogControlEvents*>(this);
        if (!*out) return E_NOINTERFACE;
        AddRef(); return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&references_); }
    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG value = InterlockedDecrement(&references_);
        if (!value) delete this;
        return value;
    }
    IFACEMETHODIMP OnFolderChange(IFileDialog*) override {
        if (detached_ || g_stopping.load()) return S_OK;
        Attach();
        ComPtr<IShellItem> folder;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog_->GetFolder(folder.put())) &&
            SUCCEEDED(folder->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            Remember(path);
            CoTaskMemFree(path);
        }
        return S_OK;
    }
    IFACEMETHODIMP OnButtonClicked(IFileDialogCustomize*, DWORD id) override {
        if (id == kButtonId && !detached_ && !g_stopping.load()) {
            Attach();
            if (palette_) palette_->Show();
        }
        return S_OK;
    }
    IFACEMETHODIMP OnFileOk(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return S_OK; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override { return S_OK; }
    IFACEMETHODIMP OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return S_OK; }
    IFACEMETHODIMP OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }
};

void DialogSession::Attach() {
    if (hwnd_ || detached_ || g_stopping.load()) return;
    ComPtr<IOleWindow> window;
    HWND hwnd = nullptr;
    if (FAILED(dialog_->QueryInterface(IID_PPV_ARGS(window.put()))) ||
        FAILED(window->GetWindow(&hwnd)) || !hwnd) return;
    AttachWindow(hwnd);
}

void DialogSession::AttachWindow(HWND hwnd) {
    if (hwnd_ || detached_ || g_stopping.load()) return;
    if (!SetWindowSubclass(hwnd, DialogProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this))) return;
    hwnd_ = hwnd;
    {
        std::lock_guard lock(g_dialogMutex);
        g_dialogWindows.push_back(hwnd);
    }
    keyHook_ = SetWindowsHookExW(WH_GETMESSAGE, KeyProc, nullptr, GetCurrentThreadId());
    palette_ = std::make_unique<Palette>(hwnd, this);
    if (!palette_->Create()) palette_.reset();
    previousForeground_ = GetAncestor(GetForegroundWindow(), GA_ROOT);
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    // Also sample foreground state if an application delays accessibility events.
    if (settings_.followExplorer) SetTimer(hwnd, kObserveTimer, 100, nullptr);
    if (settings_.showOnOpen && palette_) PostMessageW(hwnd, kShowPalette, 0, 0);
}

void DialogSession::CancelFollow() {
    if (hwnd_) KillTimer(hwnd_, kFollowTimer);
    followFrom_ = nullptr;
    followRequestedAt_ = followDeadline_ = 0;
}

void DialogSession::ArmFollow(HWND source) {
    if (!settings_.followExplorer || detached_ || g_stopping.load() || !IsExplorer(source)) return;
    if (followFrom_ == source && followDeadline_) return;
    followFrom_ = source;
    followRequestedAt_ = GetTickCount64();
    followDeadline_ = followRequestedAt_ + 2500;
    // Request a fresh snapshot instead of racing the Explorer broker's polling.
    if (HWND broker = FindWindowExW(HWND_MESSAGE, nullptr, kBrokerClass, nullptr))
        PostMessageW(broker, kRefreshExplorer, reinterpret_cast<WPARAM>(source), 0);
    SetTimer(hwnd_, kFollowTimer, 120, nullptr);
}

void DialogSession::ObserveForeground(HWND foreground) {
    HWND root = GetAncestor(foreground, GA_ROOT);
    if (!root || root == previousForeground_) return;
    bool ours = root == hwnd_ || (palette_ && root == palette_->window());
    if (ours && IsExplorer(previousForeground_)) ArmFollow(previousForeground_);
    else if (!ours) CancelFollow();
    previousForeground_ = root;
}

void CALLBACK DialogSession::ForegroundProc(HWINEVENTHOOK hook, DWORD, HWND hwnd,
                                            LONG, LONG, DWORD, DWORD) {
    if (g_stopping.load()) return;
    for (auto session : t_dialogs)
        if (session->foregroundHook_ == hook && session->hwnd_)
            PostMessageW(session->hwnd_, kForegroundChanged, reinterpret_cast<WPARAM>(hwnd), 0);
}

LRESULT CALLBACK DialogSession::KeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE && !g_stopping.load() && !t_dialogs.empty()) {
        auto self = t_dialogs.back();
        auto msg = reinterpret_cast<MSG*>(lp);
        HWND root = GetAncestor(msg->hwnd, GA_ROOT);
        bool ourWindow = root == self->hwnd_ ||
            (self->palette_ && root == self->palette_->window());
        if (ourWindow && msg->message == WM_KEYDOWN && msg->wParam == 'G' &&
            (GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
            !(GetKeyState(VK_LWIN) & 0x8000) && !(GetKeyState(VK_RWIN) & 0x8000)) {
            if (!(msg->lParam & (1LL << 30)))
                PostMessageW(self->hwnd_, (GetKeyState(VK_SHIFT) & 0x8000) ? kShowPalette : kJumpExplorer, 1, 0);
            msg->message = WM_NULL;
            msg->wParam = msg->lParam = 0;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK DialogSession::DialogProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR id, DWORD_PTR data) {
    auto self = reinterpret_cast<DialogSession*>(data);
    if (msg == kDetach) { self->Detach(); return 0; }
    if (msg == kShowPalette) {
        if (!g_stopping.load() && self->palette_) self->palette_->Show(wp != 0);
        return 0;
    }
    if (msg == kJumpExplorer) {
        if (self->palette_) self->palette_->JumpExplorer();
        return 0;
    }
    if (msg == kForegroundChanged) {
        self->ObserveForeground(reinterpret_cast<HWND>(wp));
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && self->settings_.followExplorer &&
        !g_stopping.load() && IsExplorer(reinterpret_cast<HWND>(lp))) {
        self->ArmFollow(reinterpret_cast<HWND>(lp));
    }
    if (msg == WM_TIMER && wp == kObserveTimer) {
        self->ObserveForeground(GetForegroundWindow());
        return 0;
    }
    if (msg == WM_TIMER && wp == kFollowTimer) {
        HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        bool ours = foreground == hwnd || (self->palette_ && foreground == self->palette_->window());
        auto last = LastExplorer();
        if (g_stopping.load() || !ours || GetTickCount64() > self->followDeadline_) self->CancelFollow();
        else if (last.hwnd == self->followFrom_ &&
                 (!last.capturedAt || last.capturedAt >= self->followRequestedAt_) && IsWindowEnabled(hwnd)) {
            // Do not hold pending state through reentrant Shell notifications.
            self->CancelFollow();
            HRESULT hr = self->Navigate(last.path);
            Wh_Log(L"Follow Explorer navigation: 0x%08X", static_cast<unsigned>(hr));
        }
        return 0;
    }
    if (self->palette_) {
        if (msg == WM_SHOWWINDOW && !wp) self->palette_->Hide();
        if (msg == WM_WINDOWPOSCHANGING) {
            auto position = reinterpret_cast<WINDOWPOS*>(lp);
            if (!(position->flags & SWP_NOMOVE) || !(position->flags & SWP_NOSIZE) ||
                (position->flags & SWP_HIDEWINDOW)) self->palette_->Hide();
        }
    }
    if (msg == WM_NCDESTROY) {
        // Leave the COM sink subscribed until Show returns. It is still owned
        // by the caller, even though the native dialog HWND has been destroyed.
        KillTimer(hwnd, kFollowTimer);
        KillTimer(hwnd, kObserveTimer);
        if (self->foregroundHook_) { UnhookWinEvent(self->foregroundHook_); self->foregroundHook_ = nullptr; }
        RemoveWindowSubclass(hwnd, DialogProc, id);
        if (self->keyHook_) { UnhookWindowsHookEx(self->keyHook_); self->keyHook_ = nullptr; }
        self->palette_.reset();
        {
            std::lock_guard lock(g_dialogMutex);
            g_dialogWindows.erase(std::remove(g_dialogWindows.begin(), g_dialogWindows.end(), hwnd), g_dialogWindows.end());
        }
        self->hwnd_ = nullptr;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

using ShowFn = HRESULT (STDMETHODCALLTYPE*)(IFileDialog*, HWND);
ShowFn g_openShow = nullptr, g_saveShow = nullptr;
std::vector<HMODULE> g_shellModules;

HRESULT ShowWithQuickSO(IFileDialog* dialog, HWND owner, ShowFn original) {
    if (g_stopping.load()) return original(dialog, owner);
    auto session = new DialogSession(dialog);
    session->Start();
    t_dialogs.push_back(session);
    HRESULT hr = original(dialog, owner);
    t_dialogs.pop_back();
    session->Detach();
    session->Release();
    return hr;
}

HRESULT STDMETHODCALLTYPE OpenShowHook(IFileDialog* dialog, HWND owner) {
    return ShowWithQuickSO(dialog, owner, g_openShow);
}
HRESULT STDMETHODCALLTYPE SaveShowHook(IFileDialog* dialog, HWND owner) {
    return ShowWithQuickSO(dialog, owner, g_saveShow);
}

HWND FindChildClass(HWND parent, PCWSTR name) {
    struct Search { PCWSTR name; HWND result; } search{name, nullptr};
    EnumChildWindows(parent, [](HWND child, LPARAM data) WINAPI -> BOOL {
        auto search = reinterpret_cast<Search*>(data);
        wchar_t cls[128]{}; GetClassNameW(child, cls, ARRAYSIZE(cls));
        if (_wcsicmp(cls, search->name) == 0) { search->result = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.result;
}

HWND LegacyFilenameControl(HWND hwnd) {
    if (HWND edit = GetDlgItem(hwnd, edt1)) return edit;
    return GetDlgItem(hwnd, cmb13);
}

std::wstring WindowText(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    if (length < 0 || length > 32767) return {};
    std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
    GetWindowTextW(hwnd, text.data(), static_cast<int>(text.size()));
    return text.data();
}

ComPtr<IShellBrowser> LegacyShellBrowser(HWND hwnd) {
    ComPtr<IShellBrowser> browser;
    if (!FindChildClass(hwnd, L"SHELLDLL_DefView")) return browser;
    // The legacy Shell-view host exposes its browser with this Shell message.
    // Restrict it to confirmed, in-process common-file-dialog Shell hosts.
    auto borrowed = reinterpret_cast<IShellBrowser*>(SendMessageW(hwnd, WM_USER + 7, 0, 0));
    if (borrowed) borrowed->QueryInterface(IID_PPV_ARGS(browser.put()));
    return browser;
}

std::wstring LegacyFolder(HWND hwnd) {
    auto browser = LegacyShellBrowser(hwnd);
    if (browser) {
        ComPtr<IShellView> view;
        ComPtr<IFolderView> folderView;
        ComPtr<IPersistFolder2> folder;
        if (SUCCEEDED(browser->QueryActiveShellView(view.put())) &&
            SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(folderView.put()))) &&
            SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(folder.put())))) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(folder->GetCurFolder(&pidl))) {
                ComPtr<IShellItem> item;
                HRESULT hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(item.put()));
                CoTaskMemFree(pidl);
                PWSTR path = nullptr;
                if (SUCCEEDED(hr) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    std::wstring result = path; CoTaskMemFree(path); return result;
                }
            }
        }
    }
    if (GetDlgItem(hwnd, lst2)) {
        // Pre-Explorer common dialogs use the process current directory while
        // they are open. Read it; let the native directory controls change it.
        std::vector<wchar_t> path(32768);
        DWORD count = GetCurrentDirectoryW(static_cast<DWORD>(path.size()), path.data());
        if (count && count < path.size()) {
            std::vector<wchar_t> expanded(32768);
            DWORD length = GetLongPathNameW(path.data(), expanded.data(), static_cast<DWORD>(expanded.size()));
            return length && length < expanded.size() ? expanded.data() : path.data();
        }
    }
    return {};
}

HRESULT NavigateClassicFolder(HWND hwnd, const std::wstring& path) {
    HWND directories = GetDlgItem(hwnd, lst2), drives = GetDlgItem(hwnd, cmb2);
    if (!directories || !(GetWindowLongPtrW(directories, GWL_STYLE) & LBS_HASSTRINGS)) return E_NOTIMPL;
    size_t start = 3;
    if (path.starts_with(L"\\\\")) {
        size_t shareEnd = path.find(L'\\', path.find(L'\\', 2) + 1);
        auto root = path.substr(0, shareEnd);
        auto current = LegacyFolder(hwnd);
        if (current.size() < root.size() || !SamePath(current.substr(0, root.size()), root)) return E_NOTIMPL;
        start = shareEnd == std::wstring::npos ? path.size() : shareEnd + 1;
    } else if (drives) {
        int count = static_cast<int>(SendMessageW(drives, CB_GETCOUNT, 0, 0));
        bool selected = false;
        for (int i = 0; i < count && i < 512; ++i) {
            LRESULT length = SendMessageW(drives, CB_GETLBTEXTLEN, i, 0);
            if (length <= 0 || length > 32767) continue;
            std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
            SendMessageW(drives, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(text.data()));
            std::wstring label = text.data();
            auto colon = label.find(L':');
            bool match = colon > 0 && colon != std::wstring::npos &&
                         towupper(label[colon - 1]) == towupper(path[0]);
            if (!match && label.size() >= 5 && label.starts_with(L"[-")) match = towupper(label[2]) == towupper(path[0]);
            if (!match) continue;
            SendMessageW(drives, CB_SETCURSEL, i, 0);
            SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(cmb2, CBN_SELCHANGE), reinterpret_cast<LPARAM>(drives));
            selected = true; break;
        }
        if (!selected) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
    }
    // Selecting directory entries changes folders without ever sending IDOK.
    auto select = [&](int index) {
        if (SendMessageW(directories, LB_SETCURSEL, index, 0) == LB_ERR) return false;
        SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(lst2, LBN_DBLCLK), reinterpret_cast<LPARAM>(directories));
        return IsWindow(hwnd) != FALSE;
    };
    if (!select(0)) return E_FAIL;
    while (start < path.size()) {
        size_t end = path.find(L'\\', start);
        auto part = path.substr(start, end == std::wstring::npos ? end : end - start);
        if (!part.empty()) {
            int count = static_cast<int>(SendMessageW(directories, LB_GETCOUNT, 0, 0));
            int found = -1;
            for (int i = 0; i < count && i < 100000; ++i) {
                LRESULT length = SendMessageW(directories, LB_GETTEXTLEN, i, 0);
                if (length <= 0 || length > 32767) continue;
                std::vector<wchar_t> text(static_cast<size_t>(length) + 1);
                SendMessageW(directories, LB_GETTEXT, i, reinterpret_cast<LPARAM>(text.data()));
                if (SamePath(Trim(text.data()), part)) { found = i; break; }
            }
            if (found < 0 || !select(found)) return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return SamePath(ExpandPath(LegacyFolder(hwnd)), ExpandPath(path)) ? S_OK : E_FAIL;
}

HRESULT DialogSession::NavigateLegacy(const std::wstring& path) {
    ComPtr<IShellItem> item;
    HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()));
    if (FAILED(hr)) return hr;
    SFGAOF attributes = 0;
    hr = item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes);
    if (FAILED(hr) || (attributes & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM))
        return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
    HWND filename = LegacyFilenameControl(hwnd_);
    auto saved = WindowText(filename);
    auto browser = LegacyShellBrowser(hwnd_);
    if (browser) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHGetIDListFromObject(item.get(), &pidl);
        if (SUCCEEDED(hr)) {
            hr = browser->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
            CoTaskMemFree(pidl);
        }
    } else hr = NavigateClassicFolder(hwnd_, path);
    if (IsWindow(filename)) SetWindowTextW(filename, saved.c_str());
    if (SUCCEEDED(hr)) Remember(path);
    return hr;
}

struct LegacyInvocation {
    DialogSession* session = nullptr;
    HHOOK cbt = nullptr;
};
thread_local LegacyInvocation* t_legacy = nullptr;

LRESULT CALLBACK LegacyCbtProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HCBT_ACTIVATE && t_legacy && !t_legacy->session && !g_stopping.load()) {
        HWND hwnd = reinterpret_cast<HWND>(wp);
        wchar_t cls[64]{}; GetClassNameW(hwnd, cls, ARRAYSIZE(cls));
        bool attached = false;
        { std::lock_guard lock(g_dialogMutex); attached = std::find(g_dialogWindows.begin(), g_dialogWindows.end(), hwnd) != g_dialogWindows.end(); }
        if (!attached && wcscmp(cls, L"#32770") == 0 && LegacyFilenameControl(hwnd) &&
            GetDlgItem(hwnd, IDOK) && GetDlgItem(hwnd, IDCANCEL)) {
            auto session = new DialogSession(nullptr);
            t_legacy->session = session;
            t_dialogs.push_back(session);
            session->AttachLegacy(hwnd);
            Remember(LegacyFolder(hwnd));
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

template<class T, class Fn> BOOL LegacyWithQuickSO(T* ofn, Fn original) {
    if (g_stopping.load() || t_legacy) return original(ofn);
    LegacyInvocation invocation;
    t_legacy = &invocation;
    invocation.cbt = SetWindowsHookExW(WH_CBT, LegacyCbtProc, nullptr, GetCurrentThreadId());
    BOOL result = original(ofn);
    if (invocation.cbt) UnhookWindowsHookEx(invocation.cbt);
    if (invocation.session) {
        t_dialogs.pop_back();
        invocation.session->Detach();
        invocation.session->Release();
    }
    t_legacy = nullptr;
    return result;
}

decltype(&GetOpenFileNameW) g_legacyOpenW = nullptr;
decltype(&GetOpenFileNameA) g_legacyOpenA = nullptr;
decltype(&GetSaveFileNameW) g_legacySaveW = nullptr;
decltype(&GetSaveFileNameA) g_legacySaveA = nullptr;
HMODULE g_comdlgModule = nullptr;

BOOL WINAPI LegacyOpenWHook(OPENFILENAMEW* ofn) { return LegacyWithQuickSO(ofn, g_legacyOpenW); }
BOOL WINAPI LegacyOpenAHook(OPENFILENAMEA* ofn) { return LegacyWithQuickSO(ofn, g_legacyOpenA); }
BOOL WINAPI LegacySaveWHook(OPENFILENAMEW* ofn) { return LegacyWithQuickSO(ofn, g_legacySaveW); }
BOOL WINAPI LegacySaveAHook(OPENFILENAMEA* ofn) { return LegacyWithQuickSO(ofn, g_legacySaveA); }

bool InstallLegacyHooks() {
    g_comdlgModule = LoadLibraryExW(L"comdlg32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!g_comdlgModule) return false;
    bool installed = false;
    auto hook = [&](PCSTR name, auto replacement, auto& original) {
        auto target = GetProcAddress(g_comdlgModule, name);
        if (target && Wh_SetFunctionHook(reinterpret_cast<void*>(target), reinterpret_cast<void*>(replacement),
                                        reinterpret_cast<void**>(&original))) installed = true;
    };
    hook("GetOpenFileNameW", LegacyOpenWHook, g_legacyOpenW);
    hook("GetOpenFileNameA", LegacyOpenAHook, g_legacyOpenA);
    hook("GetSaveFileNameW", LegacySaveWHook, g_legacySaveW);
    hook("GetSaveFileNameA", LegacySaveAHook, g_legacySaveA);
    return installed;
}

struct FolderWindow {
    HWND hwnd = nullptr;
    std::wstring path;
    ComPtr<IShellBrowser> browser;
};

std::vector<FolderWindow> ExplorerFolders(IShellWindows* windows) {
    std::vector<FolderWindow> result;
    long count = 0;
    if (!windows || FAILED(windows->get_Count(&count))) return result;
    for (long i = 0; i < count; ++i) {
        VARIANT index{}; index.vt = VT_I4; index.lVal = i;
        ComPtr<IDispatch> dispatch;
        ComPtr<IWebBrowserApp> app;
        if (FAILED(windows->Item(index, dispatch.put())) || !dispatch ||
            FAILED(dispatch->QueryInterface(IID_PPV_ARGS(app.put())))) continue;
        SHANDLE_PTR value = 0;
        if (FAILED(app->get_HWND(&value))) continue;
        HWND hwnd = reinterpret_cast<HWND>(value);
        if (!IsExplorer(hwnd)) continue;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> view;
        ComPtr<IFolderView> folderView;
        ComPtr<IPersistFolder2> folder;
        if (FAILED(app->QueryInterface(IID_PPV_ARGS(services.put()))) ||
            FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(browser.put()))) ||
            FAILED(browser->QueryActiveShellView(view.put()))) continue;
        HWND viewHwnd = nullptr;
        if (FAILED(view->GetWindow(&viewHwnd)) || !viewHwnd || !IsWindowVisible(viewHwnd)) continue;
        if (FAILED(view->QueryInterface(IID_PPV_ARGS(folderView.put()))) ||
            FAILED(folderView->GetFolder(IID_PPV_ARGS(folder.put())))) continue;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (FAILED(folder->GetCurFolder(&pidl))) continue;
        ComPtr<IShellItem> item;
        HRESULT hr = SHCreateItemFromIDList(pidl, IID_PPV_ARGS(item.put()));
        CoTaskMemFree(pidl);
        PWSTR path = nullptr;
        if (SUCCEEDED(hr) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
            if (IsFilesystemPath(path)) result.push_back({hwnd, path, std::move(browser)});
            CoTaskMemFree(path);
        }
    }
    return result;
}

struct ExplorerNavigator : Navigator {
    ComPtr<IShellBrowser> browser;
    explicit ExplorerNavigator(ComPtr<IShellBrowser> value) : browser(std::move(value)) {}
    HRESULT Navigate(const std::wstring& path) override {
        ComPtr<IShellItem> item;
        HRESULT hr = SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(item.put()));
        if (FAILED(hr)) return hr;
        SFGAOF flags = 0;
        hr = item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &flags);
        if (FAILED(hr) || (flags & (SFGAO_FOLDER | SFGAO_FILESYSTEM)) != (SFGAO_FOLDER | SFGAO_FILESYSTEM))
            return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHGetIDListFromObject(item.get(), &pidl);
        if (FAILED(hr)) return hr;
        hr = browser->BrowseObject(pidl, SBSP_ABSOLUTE | SBSP_SAMEBROWSER);
        CoTaskMemFree(pidl);
        return hr;
    }
};

std::atomic<HWND> g_brokerWindow{nullptr};
HANDLE g_brokerThread = nullptr;

struct ExplorerBroker {
    HWND hwnd = nullptr, previous = nullptr;
    HWINEVENTHOOK eventHook = nullptr;
    ComPtr<IShellWindows> windows;
    std::vector<std::pair<DWORD, HHOOK>> keyHooks;
    std::unique_ptr<ExplorerNavigator> navigator;
    std::unique_ptr<Palette> palette;
    std::wstring lastPath;
    std::vector<std::wstring> lastOpen;
    bool publishedOpen = false;
    HWND lastWindow = nullptr;
    ULONGLONG lastScan = 0;

    void Capture(HWND target, bool force = false) {
        if (!windows) CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put()));
        if (!IsExplorer(target)) return;
        auto folders = ExplorerFolders(windows.get());
        std::vector<std::wstring> open;
        bool found = false;
        for (const auto& folder : folders) {
            open.push_back(folder.path);
            if (folder.hwnd != target) continue;
            found = true;
            if (force || folder.hwnd != lastWindow || !SamePath(folder.path, lastPath)) {
                wchar_t value[32]{};
                swprintf_s(value, L"%llx", static_cast<unsigned long long>(reinterpret_cast<UINT_PTR>(folder.hwnd)));
                WritePaths(L"LastExplorer", {folder.path, value, std::to_wstring(GetTickCount64())});
                Remember(folder.path);
                lastPath = folder.path;
                lastWindow = folder.hwnd;
            }
        }
        if (!found) {
            // Home, This PC, search results, and other virtual views must not
            // cause a dialog to jump to an older filesystem location.
            WritePaths(L"LastExplorer", {});
            lastPath.clear(); lastWindow = nullptr;
        }
        if (!publishedOpen || open != lastOpen) {
            WritePaths(L"OpenFolders", open); lastOpen = std::move(open); publishedOpen = true;
        }
    }
    void Popup(HWND target) {
        if (!IsExplorer(target)) return;
        if (!windows) CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(windows.put()));
        auto folders = ExplorerFolders(windows.get());
        for (auto& folder : folders) {
            if (folder.hwnd != target) continue;
            palette.reset();
            navigator = std::make_unique<ExplorerNavigator>(std::move(folder.browser));
            palette = std::make_unique<Palette>(target, navigator.get());
            palette->Show();
            return;
        }
    }
    void ScanThreads();
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK KeyProc(int code, WPARAM wp, LPARAM lp) {
        if (code >= 0 && wp == PM_REMOVE && !g_stopping.load()) {
            auto msg = reinterpret_cast<MSG*>(lp);
            HWND root = GetAncestor(msg->hwnd, GA_ROOT);
            if (IsExplorer(root) && msg->message == WM_KEYDOWN && msg->wParam == 'G' &&
                (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) &&
                !(GetKeyState(VK_MENU) & 0x8000) && !(GetKeyState(VK_LWIN) & 0x8000) &&
                !(GetKeyState(VK_RWIN) & 0x8000)) {
                if (!(msg->lParam & (1LL << 30)))
                    PostMessageW(g_brokerWindow.load(), kExplorerPopup, reinterpret_cast<WPARAM>(root), 0);
                msg->message = WM_NULL; msg->wParam = msg->lParam = 0;
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }
    static void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
        if (!g_stopping.load()) PostMessageW(g_brokerWindow.load(), WM_APP + 20, reinterpret_cast<WPARAM>(hwnd), 0);
    }
};

BOOL CALLBACK CollectExplorerThread(HWND window, LPARAM param) {
    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(window, &pid);
    if (pid == GetCurrentProcessId() && IsExplorer(window)) {
        auto list = reinterpret_cast<std::vector<DWORD>*>(param);
        if (std::find(list->begin(), list->end(), tid) == list->end()) list->push_back(tid);
    }
    return TRUE;
}

void ExplorerBroker::ScanThreads() {
    std::vector<DWORD> threads;
    EnumWindows(CollectExplorerThread, reinterpret_cast<LPARAM>(&threads));
    for (auto it = keyHooks.begin(); it != keyHooks.end();) {
        if (std::find(threads.begin(), threads.end(), it->first) == threads.end()) {
            UnhookWindowsHookEx(it->second); it = keyHooks.erase(it);
        } else ++it;
    }
    for (DWORD tid : threads) {
        if (std::any_of(keyHooks.begin(), keyHooks.end(), [&](auto pair) { return pair.first == tid; })) continue;
        HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, KeyProc, nullptr, tid);
        if (hook) keyHooks.emplace_back(tid, hook);
    }
}

LRESULT CALLBACK ExplorerBroker::WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto self = reinterpret_cast<ExplorerBroker*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<ExplorerBroker*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_TIMER && !g_stopping.load()) {
        HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
        if (IsExplorer(foreground)) self->Capture(foreground);
        if (GetTickCount64() - self->lastScan >= 1500) {
            self->ScanThreads(); self->lastScan = GetTickCount64();
            if (!IsExplorer(foreground) && self->windows) {
                std::vector<std::wstring> open;
                for (const auto& folder : ExplorerFolders(self->windows.get())) open.push_back(folder.path);
                if (!self->publishedOpen || open != self->lastOpen) {
                    WritePaths(L"OpenFolders", open); self->lastOpen = std::move(open); self->publishedOpen = true;
                }
            }
        }
        return 0;
    }
    if (msg == WM_APP + 20 && !g_stopping.load()) {
        if (IsExplorer(self->previous)) self->Capture(self->previous, true);
        self->previous = reinterpret_cast<HWND>(wp);
        if (IsExplorer(self->previous)) self->Capture(self->previous, true);
        return 0;
    }
    if (msg == kExplorerPopup && !g_stopping.load()) { self->Popup(reinterpret_cast<HWND>(wp)); return 0; }
    if (msg == kRefreshExplorer && !g_stopping.load()) { self->Capture(reinterpret_cast<HWND>(wp), true); return 0; }
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

DWORD WINAPI ExplorerThread(void*) {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment)) return 1;
    {
        ExplorerBroker broker;
        WNDCLASSW cls{};
        cls.hInstance = g_instance; cls.lpszClassName = kBrokerClass;
        cls.lpfnWndProc = ExplorerBroker::WindowProc;
        RegisterClassW(&cls);
        HWND hwnd = CreateWindowExW(0, kBrokerClass, L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, g_instance, &broker);
        if (hwnd) {
            g_brokerWindow.store(hwnd);
            broker.ScanThreads();
            broker.previous = GetAncestor(GetForegroundWindow(), GA_ROOT);
            broker.Capture(broker.previous);
            broker.eventHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                nullptr, ExplorerBroker::ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
            SetTimer(hwnd, 1, 250, nullptr);
            if (g_stopping.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            MSG msg{};
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (broker.eventHook) UnhookWinEvent(broker.eventHook);
            for (const auto& pair : broker.keyHooks) UnhookWindowsHookEx(pair.second);
            broker.palette.reset();
            g_brokerWindow.store(nullptr);
        }
        UnregisterClassW(kBrokerClass, g_instance);
    }
    CoUninitialize();
    return 0;
}

bool InstallDialogHooks() {
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) return false;
    void* openTarget = nullptr;
    ComPtr<IFileDialog> open;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(open.put())))) {
        openTarget = (*reinterpret_cast<void***>(open.get()))[3];
        if (!Wh_SetFunctionHook(openTarget, reinterpret_cast<void*>(OpenShowHook),
                                reinterpret_cast<void**>(&g_openShow))) openTarget = nullptr;
    }
    ComPtr<IFileDialog> save;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(save.put())))) {
        void* target = (*reinterpret_cast<void***>(save.get()))[3];
        if (target != openTarget && !Wh_SetFunctionHook(target, reinterpret_cast<void*>(SaveShowHook),
                                                       reinterpret_cast<void**>(&g_saveShow))) target = nullptr;
        if (target) {
            HMODULE module = nullptr;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<PCWSTR>(target), &module)) g_shellModules.push_back(module);
        }
    }
    if (openTarget) {
        HMODULE module = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<PCWSTR>(openTarget), &module)) g_shellModules.push_back(module);
    }
    // Release COM interfaces before uninitializing their apartment.
    open = {}; save = {};
    if (SUCCEEDED(apartment)) CoUninitialize();
    return g_openShow || g_saveShow;
}

} // namespace quickso

BOOL Wh_ModInit() {
    using namespace quickso;
    g_stopping.store(false);
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<PCWSTR>(&Wh_ModInit), &g_instance);
    LoadSettings();
    auto mutexName = UserMutexName();
    if (!mutexName.empty()) g_historyMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    if (!RegisterClasses()) {
        if (g_historyMutex) { CloseHandle(g_historyMutex); g_historyMutex = nullptr; }
        return FALSE;
    }
    bool hooked = InstallDialogHooks();
    hooked = InstallLegacyHooks() || hooked;
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    if (_wcsicmp(PathFindFileNameW(path), L"explorer.exe") == 0)
        g_brokerThread = CreateThread(nullptr, 0, ExplorerThread, nullptr, 0, nullptr);
    if (!hooked && !g_brokerThread) {
        UnregisterClassW(kPaletteClass, g_instance);
        if (g_historyMutex) { CloseHandle(g_historyMutex); g_historyMutex = nullptr; }
        for (HMODULE module : g_shellModules) FreeLibrary(module);
        g_shellModules.clear();
        if (g_comdlgModule) { FreeLibrary(g_comdlgModule); g_comdlgModule = nullptr; }
        return FALSE;
    }
    return TRUE;
}

void Wh_ModSettingsChanged() { quickso::LoadSettings(); }

void Wh_ModBeforeUninit() {
    using namespace quickso;
    g_stopping.store(true);
    std::vector<HWND> dialogs;
    { std::lock_guard lock(g_dialogMutex); dialogs = g_dialogWindows; }
    for (HWND hwnd : dialogs) SendMessageW(hwnd, kDetach, 0, 0);
    if (g_brokerThread) {
        if (HWND hwnd = g_brokerWindow.load()) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        WaitForSingleObject(g_brokerThread, INFINITE);
        CloseHandle(g_brokerThread); g_brokerThread = nullptr;
    }
}

void Wh_ModUninit() {
    using namespace quickso;
    UnregisterClassW(kPaletteClass, g_instance);
    if (g_historyMutex) { CloseHandle(g_historyMutex); g_historyMutex = nullptr; }
    for (HMODULE module : g_shellModules) FreeLibrary(module);
    g_shellModules.clear();
    if (g_comdlgModule) { FreeLibrary(g_comdlgModule); g_comdlgModule = nullptr; }
}
