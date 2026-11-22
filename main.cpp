// HideFoldersUsingFolderIcons
// ---------------------------------------------------------------------------
// Camouflage a desktop folder: periodically sample the desktop background at the
// folder's icon position (wallpaper engines / Fences included) and apply it as
// the folder's icon via desktop.ini, so the folder blends into the wallpaper.
//
// - No tray icon. Closing the window just hides it; the app keeps running.
// - Launch the exe again to bring the window back (single instance).
// - Place the exe inside the folder you want to hide (default target = exe dir),
//   or pass the folder path as the first argument.
//
// Technique:
//   * Locate the folder's icon in explorer's desktop SysListView32 (cross-process
//     VirtualAllocEx + LVM_* messages), which also works for desktop-organizer
//     tools that keep using list views.
//   * Briefly move the icon off-screen, let the desktop repaint, BitBlt the
//     background at that exact rect, move the icon back.
//   * If the pixels changed, write a 32bpp .ico (cycling a few file names to
//     bypass the shell icon cache), update desktop.ini's IconResource and
//     SHChangeNotify the shell.
// ---------------------------------------------------------------------------

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wincodec.h>
#include <objbase.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <set>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

#ifndef DWMWA_CLOAKED
#define DWMWA_CLOAKED 13
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

namespace {

const wchar_t kClassName[] = L"HFUFO_HideFoldersUsingFolderIcons_Wnd";
const wchar_t kMutexName[] = L"Local\\HFUFO_HideFoldersUsingFolderIcons_SingleInstance";
constexpr UINT kWmShow = WM_APP + 1;
constexpr int kIconSlots = 4;   // cycling icon file names defeat the icon cache
constexpr DWORD kSettleMs = 60; // repaint wait while the icon is moved away

constexpr int IDB_HIDE = 1001;
constexpr int IDB_RESTORE = 1002;
constexpr int IDB_EXIT = 1003;
constexpr int IDC_INTERVAL = 1004;

HINSTANCE g_inst = nullptr;
HWND g_mainWnd = nullptr;
HWND g_pathEdit = nullptr;
HWND g_statusCtl = nullptr;
HWND g_intervalCombo = nullptr;
HFONT g_font = nullptr;
DWORD g_selfPid = 0;

std::wstring g_folder;      // folder to camouflage
std::wstring g_lnkPath;     // set: camouflage this desktop shortcut (.lnk) instead
std::wstring g_artifactsDir;// where wall_icon_*.ico files are written
UINT g_intervalMs = 200;
bool g_logEnabled = false;
bool g_onceMode = false;
bool g_locateMode = false;
bool g_helpOnly = false;
bool g_wallTrusted = false; // wallpaper-file sampling verified against screen

std::vector<BYTE> g_lastPx; // last sampled background
int g_lastW = 0, g_lastH = 0;
RECT g_lastRect{};
int g_iconGen = 0;
unsigned long long g_updates = 0;

// ----------------------------------------------------------------- logging --

std::wstring LogPath() {
    static std::wstring p;
    if (p.empty()) {
        wchar_t tmp[MAX_PATH + 1]{};
        GetTempPathW(MAX_PATH, tmp);
        p = std::wstring(tmp) + L"HFUFO_log.txt";
    }
    return p;
}

void Log(const wchar_t* fmt, ...) {
    if (!g_logEnabled) return;
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1100];
    swprintf(line, 1100, L"[%02u:%02u:%02u.%03u] %s\r\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, buf);
    OutputDebugStringW(line);
    HANDLE h = CreateFileW(LogPath().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)(lstrlenW(line) * sizeof(wchar_t)), &w, nullptr);
        CloseHandle(h);
    }
}

// ------------------------------------------------------------ path helpers --

std::wstring ExePathW() {
    wchar_t b[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return b;
}

std::wstring DirOf(const std::wstring& p) {
    size_t k = p.find_last_of(L"\\/");
    if (k == std::wstring::npos) return L".";
    if (k == 0) return p.substr(0, 1);
    return p.substr(0, k);
}

std::wstring BaseName(const std::wstring& p) {
    size_t end = p.size();
    while (end > 0 && (p[end - 1] == L'\\' || p[end - 1] == L'/')) end--;
    if (end == 0) return p;
    size_t start = p.find_last_of(L"\\/", end - 1);
    if (start == std::wstring::npos) return p.substr(0, end);
    return p.substr(start + 1, end - start - 1);
}

// ------------------------------------------------------- remote list access --

// The desktop icon view lives inside explorer.exe, so any message whose
// parameter is a pointer needs memory inside the target process.
class RemoteListView {
public:
    ~RemoteListView() { Detach(); }

    bool Attach(HWND lv) {
        m_lv = lv;
        DWORD pid = 0;
        if (!GetWindowThreadProcessId(lv, &pid) || !pid) return false;
        m_proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, pid);
        if (!m_proc) return false;
        m_buf1 = VirtualAllocEx(m_proc, nullptr, 1024, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        m_buf2 = VirtualAllocEx(m_proc, nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        return m_buf1 && m_buf2;
    }

    void Detach() {
        if (m_proc) {
            if (m_buf1) VirtualFreeEx(m_proc, m_buf1, 0, MEM_RELEASE);
            if (m_buf2) VirtualFreeEx(m_proc, m_buf2, 0, MEM_RELEASE);
            CloseHandle(m_proc);
        }
        m_proc = nullptr;
        m_buf1 = m_buf2 = nullptr;
        m_lv = nullptr;
    }

    int Count() const { return (int)Snd(LVM_GETITEMCOUNT, 0, 0); }

    bool Text(int i, std::wstring& out) {
        LVITEMW lvi{};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)m_buf2;
        lvi.cchTextMax = 4000;
        if (!Write(m_buf1, &lvi, sizeof lvi)) return false;
        Snd(LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)m_buf1); // returns text length
        wchar_t local[4000]{};
        if (!Read(m_buf2, local, sizeof local)) return false;
        local[3999] = 0;
        out = local;
        return true;
    }

    bool Pos(int i, POINT& pt) {
        if (!Snd(LVM_GETITEMPOSITION, (WPARAM)i, (LPARAM)m_buf2)) return false;
        return Read(m_buf2, &pt, sizeof pt);
    }

    bool IconRect(int i, RECT& rc) {
        rc = {LVIR_ICON, 0, 0, 0};
        if (!Write(m_buf2, &rc, sizeof rc)) return false;
        if (!Snd(LVM_GETITEMRECT, (WPARAM)i, (LPARAM)m_buf2)) return false;
        return Read(m_buf2, &rc, sizeof rc);
    }

    bool SetPos(int i, const POINT& pt) {
        if (!Write(m_buf2, &pt, sizeof pt)) return false;
        Snd(LVM_SETITEMPOSITION32, (WPARAM)i, (LPARAM)m_buf2);
        return true;
    }

private:
    LRESULT Snd(UINT m, WPARAM w, LPARAM l) const {
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(m_lv, m, w, l, SMTO_NORMAL, 3000, &r)) return 0;
        return (LRESULT)r;
    }
    bool Write(void* remote, const void* local, size_t n) const {
        SIZE_T w = 0;
        return WriteProcessMemory(m_proc, remote, local, n, &w) && w == n;
    }
    bool Read(const void* remote, void* local, size_t n) const {
        SIZE_T r = 0;
        return ReadProcessMemory(m_proc, remote, local, n, &r) && r == n;
    }

    HWND m_lv = nullptr;
    HANDLE m_proc = nullptr;
    void* m_buf1 = nullptr;
    void* m_buf2 = nullptr;
};

// --------------------------------------------------- desktop item discovery --

std::set<DWORD> ExplorerPids() {
    std::set<DWORD> r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) r.insert(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return r;
}

struct EnumCtx {
    const std::set<DWORD>* pids;
    std::vector<HWND> desktopLv; // list views rooted at Progman/WorkerW (the desktop)
    std::vector<HWND> otherLv;   // other explorer list views (desktop organizer tools)
};

BOOL CALLBACK EnumChildCb(HWND h, LPARAM lp) {
    auto* c = (EnumCtx*)lp;
    wchar_t cn[64];
    if (GetClassNameW(h, cn, 64) && wcscmp(cn, L"SysListView32") == 0) {
        HWND root = GetAncestor(h, GA_ROOT);
        wchar_t rn[64]{};
        GetClassNameW(root, rn, 64);
        if (!wcscmp(rn, L"Progman") || !wcscmp(rn, L"WorkerW")) c->desktopLv.push_back(h);
        else c->otherLv.push_back(h);
    }
    return TRUE;
}

BOOL CALLBACK EnumTopCb(HWND h, LPARAM lp) {
    auto* c = (EnumCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (c->pids->count(pid)) EnumChildWindows(h, EnumChildCb, lp);
    return TRUE;
}

struct FolderLoc {
    HWND lv = nullptr;
    int index = -1;
    RECT iconRectScreen{};
    POINT slot{};
};

bool LocateOnDesktop(const std::vector<std::wstring>& names, const std::set<DWORD>& expPids, FolderLoc& out) {
    EnumCtx ctx;
    ctx.pids = &expPids;
    EnumWindows(EnumTopCb, (LPARAM)&ctx);

    auto matches = [&](const std::wstring& t) {
        for (const auto& n : names)
            if (lstrcmpiW(t.c_str(), n.c_str()) == 0) return true;
        return false;
    };

    auto tryList = [&](std::vector<HWND>& lvs, int cap) {
        for (HWND lv : lvs) {
            RemoteListView rl;
            if (!rl.Attach(lv)) continue;
            int n = rl.Count();
            if (n <= 0 || n > cap) continue;
            for (int i = 0; i < n; ++i) {
                std::wstring t;
                if (!rl.Text(i, t) || !matches(t)) continue;
                RECT rc{};
                POINT pt{};
                if (rl.IconRect(i, rc) && rl.Pos(i, pt)) {
                    MapWindowPoints(lv, HWND_DESKTOP, (POINT*)&rc, 2);
                    out.lv = lv;
                    out.index = i;
                    out.iconRectScreen = rc;
                    out.slot = pt;
                    Log(L"located '%s' item %d in %dx%d view, icon rect (%d,%d)-(%d,%d)",
                        t.c_str(), i, n, n, rc.left, rc.top, rc.right, rc.bottom);
                    return true;
                }
            }
        }
        return false;
    };

    if (tryList(ctx.desktopLv, 1 << 20)) return true;
    return tryList(ctx.otherLv, 2000); // organizer tools: don't scan huge lists
}

// --------------------------------------------------- occlusion detection --

// While another window covers the target rect the true background isn't
// visible, so pause refreshing (the folder is hidden behind it anyway).
struct CoverCtx {
    const RECT* rc;
    bool covered = false;
    const std::set<DWORD>* expPids;
};

BOOL CALLBACK CoverCb(HWND h, LPARAM lp) {
    auto* c = (CoverCtx*)lp;
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cn[64]{};
    GetClassNameW(h, cn, 64);
    bool desktopRoot = !wcscmp(cn, L"Progman") || !wcscmp(cn, L"WorkerW");
    RECT wr, inter;
    bool hit = !desktopRoot && GetWindowRect(h, &wr) && IntersectRect(&inter, &wr, c->rc);
    if (!hit) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_selfPid) { c->covered = true; return FALSE; } // our own window
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return TRUE;
    if (!c->expPids->count(pid)) { c->covered = true; return FALSE; }
    // explorer-owned windows (desktop organizer backdrops) are part of the look
    return TRUE;
}

// ---------------------------------------------------------------- capture --

struct Shot {
    int w = 0, h = 0;
    std::vector<BYTE> px; // 32bpp BGRA, bottom-up rows (exactly ICO XOR order)
};

bool CaptureRect(const RECT& rc, Shot& s) {
    s.w = rc.right - rc.left;
    s.h = rc.bottom - rc.top;
    if (s.w <= 0 || s.h <= 0 || s.w > 8192 || s.h > 8192) return false;
    HDC scr = GetDC(nullptr);
    if (!scr) return false;
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = s.w;
    bmi.bmiHeader.biHeight = s.h; // positive = bottom-up
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (hb && bits) {
        HGDIOBJ old = SelectObject(mem, hb);
        ok = BitBlt(mem, 0, 0, s.w, s.h, scr, rc.left, rc.top, SRCCOPY) != FALSE;
        SelectObject(mem, old);
        if (ok) {
            s.px.resize((size_t)s.w * s.h * 4);
            memcpy(s.px.data(), bits, s.px.size());
            for (size_t i = 3; i < s.px.size(); i += 4) s.px[i] = 0xFF; // opaque
        }
    }
    if (hb) DeleteObject(hb);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return ok;
}

// ------------------------------------------------------------------- ico --

bool WriteIcoFile(const std::wstring& path, const Shot& s) {
    const DWORD andRow = (DWORD)(((s.w + 31) / 32) * 4);
    const DWORD andSize = andRow * (DWORD)s.h;
    const DWORD xorSize = (DWORD)((size_t)s.w * s.h * 4);
    const DWORD imgSize = 40 + xorSize + andSize;
    std::vector<BYTE> f(6 + 16 + imgSize, 0);
    f[2] = 1; // type: icon
    f[4] = 1; // count
    BYTE* e = f.data() + 6;
    e[0] = s.w >= 256 ? 0 : (BYTE)s.w;
    e[1] = s.h >= 256 ? 0 : (BYTE)s.h;
    e[4] = 1;   // planes
    e[6] = 32;  // bpp
    *(DWORD*)(e + 8) = imgSize;
    *(DWORD*)(e + 12) = 22;
    auto* bh = (BITMAPINFOHEADER*)(f.data() + 22);
    bh->biSize = 40;
    bh->biWidth = s.w;
    bh->biHeight = s.h * 2; // XOR + AND
    bh->biPlanes = 1;
    bh->biBitCount = 32;
    bh->biCompression = BI_RGB;
    bh->biSizeImage = xorSize + andSize;
    memcpy(f.data() + 22 + 40, s.px.data(), xorSize);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, f.data(), (DWORD)f.size(), &w, nullptr);
    CloseHandle(h);
    return ok && w == f.size();
}

// -------------------------------------------------------------- shell bits --

void WriteUtf16File(const std::wstring& path, const std::wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WORD bom = 0xFEFF;
    WriteFile(h, &bom, 2, &w, nullptr);
    WriteFile(h, text.c_str(), (DWORD)(text.size() * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(h);
}

void NotifyShell(const std::wstring& folder) {
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATH | SHCNF_FLUSH, folder.c_str(), nullptr);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATH | SHCNF_FLUSH, DirOf(folder).c_str(), nullptr);
}

void ApplyFolderIcon(const std::wstring& folder, const std::wstring& ico) {
    std::wstring ini = folder + L"\\desktop.ini";
    WriteUtf16File(ini, L"[.ShellClassInfo]\r\nIconResource=" + ico + L",0\r\n");
    SetFileAttributesW(ini.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    DWORD at = GetFileAttributesW(folder.c_str());
    if (at != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(folder.c_str(), at | FILE_ATTRIBUTE_SYSTEM);
    NotifyShell(folder);
}

void DeleteArtifacts();

void RestoreFolderIcon(const std::wstring& folder) {
    DeleteFileW((folder + L"\\desktop.ini").c_str());
    DeleteArtifacts();
    DWORD at = GetFileAttributesW(folder.c_str());
    if (at != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(folder.c_str(), at & ~FILE_ATTRIBUTE_SYSTEM);
    g_lastW = 0;
    g_lastH = 0;
    g_lastPx.clear();
    g_wallTrusted = false;
    NotifyShell(folder);
}

// ------------------------------------------------------- shortcut (.lnk) mode --
// Some desktop organizers ignore desktop.ini folder icons but happily show
// shortcut icons, so the target can also be a desktop shortcut whose icon
// we rewrite (IShellLinkW::SetIconLocation) every refresh.

bool HasExt(const std::wstring& p, const wchar_t* ext) {
    size_t n = p.size(), m = wcslen(ext);
    return n > m && _wcsicmp(p.c_str() + n - m, ext) == 0;
}

bool ResolveLnkTarget(const std::wstring& lnk, std::wstring& target) {
    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
        SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) &&
        SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
        WCHAR buf[MAX_PATH]{};
        if (SUCCEEDED(sl->GetPath(buf, MAX_PATH, nullptr, SLGP_RAWPATH)) && buf[0]) {
            target = buf;
            ok = true;
        }
    }
    if (pf) pf->Release();
    if (sl) sl->Release();
    return ok;
}

bool SetLnkIconLocation(const std::wstring& lnk, const std::wstring& ico) {
    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
        SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) &&
        SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
        sl->SetIconLocation(ico.c_str(), 0);
        if (SUCCEEDED(pf->Save(lnk.c_str(), TRUE))) {
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATH | SHCNF_FLUSH, lnk.c_str(), nullptr);
            ok = true;
        }
    }
    if (pf) pf->Release();
    if (sl) sl->Release();
    return ok;
}

bool ResetLnkIcon(const std::wstring& lnk) {
    IShellLinkW* sl = nullptr;
    IPersistFile* pf = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
        SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf))) &&
        SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
        sl->SetIconLocation(L"", 0); // fall back to the target's own icon
        if (SUCCEEDED(pf->Save(lnk.c_str(), TRUE))) {
            SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATH | SHCNF_FLUSH, lnk.c_str(), nullptr);
            ok = true;
        }
    }
    if (pf) pf->Release();
    if (sl) sl->Release();
    return ok;
}

void DesktopDirs(std::vector<std::wstring>& dirs) {
    for (const KNOWNFOLDERID& id : {FOLDERID_Desktop, FOLDERID_PublicDesktop}) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) {
            dirs.push_back(p);
            CoTaskMemFree(p);
        }
    }
}

std::wstring FindLnkForFolder(const std::wstring& dir) {
    std::vector<std::wstring> dirs;
    DesktopDirs(dirs);
    for (const auto& d : dirs) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((d + L"\\*.lnk").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring lnk = d + L"\\" + fd.cFileName;
            std::wstring t;
            if (ResolveLnkTarget(lnk, t) && _wcsicmp(t.c_str(), dir.c_str()) == 0) return lnk;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return L"";
}

bool DirExists(const std::wstring& p) {
    DWORD at = GetFileAttributesW(p.c_str());
    return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    wchar_t ca[MAX_PATH]{}, cb[MAX_PATH]{};
    if (!_wfullpath(ca, a.c_str(), MAX_PATH) || !_wfullpath(cb, b.c_str(), MAX_PATH)) return false;
    return _wcsicmp(ca, cb) == 0;
}

bool IsDesktopDir(const std::wstring& dir) {
    std::vector<std::wstring> dirs;
    DesktopDirs(dirs);
    for (const auto& d : dirs)
        if (SamePath(dir, d)) return true;
    return false;
}

void DeleteArtifactsIn(const std::wstring& dir) {
    for (int i = 0; i < kIconSlots; ++i)
        DeleteFileW((dir + L"\\wall_icon_" + std::to_wstring(i) + L".ico").c_str());
}

// Decide what to camouflage: an explicit .lnk argument, a desktop shortcut
// pointing at the folder argument (organizer-friendly), or the folder itself.
void InitTargetMode() {
    if (!g_folder.empty() && HasExt(g_folder, L".lnk")) {
        g_lnkPath = g_folder;
    } else if (DirExists(g_folder)) {
        std::wstring lnk = FindLnkForFolder(g_folder);
        if (!lnk.empty()) g_lnkPath = lnk;
    }
    std::wstring dir;
    if (!g_lnkPath.empty()) {
        std::wstring t;
        if (ResolveLnkTarget(g_lnkPath, t) && DirExists(t)) dir = t;
    } else if (DirExists(g_folder)) {
        dir = g_folder;
    }
    // wall_icon_*.ico must never sit on the desktop itself
    std::wstring exeDir = DirOf(ExePathW());
    if (dir.empty() || SamePath(dir, exeDir) == false && IsDesktopDir(dir)) {
        dir = exeDir;
        if (IsDesktopDir(dir)) dir.clear();
    }
    if (dir.empty()) {
        wchar_t tp[MAX_PATH]{};
        if (GetTempPathW(MAX_PATH, tp)) {
            dir = std::wstring(tp) + L"HFUFO_icons";
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        }
    }
    g_artifactsDir = dir;
    Log(L"target: folder=%ls shortcut=%ls artifacts=%ls mode=%s",
        g_folder.c_str(), g_lnkPath.c_str(), g_artifactsDir.c_str(),
        g_lnkPath.empty() ? L"desktop.ini" : L"shortcut icon");
}

std::vector<std::wstring> LocateNames() {
    std::vector<std::wstring> v;
    if (!g_lnkPath.empty()) {
        std::wstring base = BaseName(g_lnkPath);
        if (HasExt(base, L".lnk")) base = base.substr(0, base.size() - 4);
        v.push_back(base);                 // explorer usually hides ".lnk"
        v.push_back(BaseName(g_lnkPath));  // shown when extensions are visible
    } else {
        v.push_back(BaseName(g_folder));
    }
    return v;
}

void DeleteArtifacts() { DeleteArtifactsIn(g_artifactsDir); }

// -------------------------------------------------------- wallpaper source --
// Preferred sampling path: read the wallpaper FILE (IDesktopWallpaper) and
// crop what the desktop shows at the icon rect. Works while other windows
// cover the desktop and without moving icons at all. Pixel output is
// verified against a real screenshot once the spot is visible; a mismatch
// (wallpaper engines render dynamically) disables file mode for the session.
namespace wall {

struct Info {
    std::wstring path;
    FILETIME mtime{};
    std::vector<BYTE> px; // 32bpp BGRA top-down
    int w = 0, h = 0;
    bool solid = false;
    DWORD back = 0;
    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
} g_info;

double MeanDiff(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    if (a.size() != b.size() || a.empty()) return 100.0;
    long long sum = 0;
    const size_t px = a.size() / 4;
    for (size_t i = 0; i < a.size(); i += 4)
        sum += std::abs((int)a[i] - (int)b[i]) + std::abs((int)a[i + 1] - (int)b[i + 1]) +
               std::abs((int)a[i + 2] - (int)b[i + 2]);
    return 100.0 * (double)sum / ((double)px * 3 * 255);
}

bool DecodeImageFile(const std::wstring& path, std::vector<BYTE>& px, int& w, int& h) {
    static IWICImagingFactory* wf = nullptr;
    if (!wf && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wf))))
        return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wf->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) &&
        SUCCEEDED(wf->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
        UINT uw = 0, uh = 0;
        conv->GetSize(&uw, &uh);
        if (uw && uh && uw <= 16384 && uh <= 16384) {
            px.resize((size_t)uw * uh * 4);
            if (SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, (UINT)px.size(), px.data()))) {
                w = (int)uw;
                h = (int)uh;
                ok = true;
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

// Refresh cached info for the monitor containing rc; true if usable
// (decoded image or a plain solid color).
bool RefreshFor(const RECT& rc) {
    // IDesktopWallpaper is declared with a manual CLSID/IID for MinGW-friendliness
    static const CLSID clsidDwp = {0xc2cf3110, 0x460e, 0x4fc1,
                                   {0xb9, 0xd0, 0x8a, 0x1c, 0x0c, 0x9c, 0xc4, 0xbd}};
    static const IID iidDwp = {0xb92b56a9, 0x8b55, 0x4e14,
                               {0x9a, 0x89, 0x01, 0x99, 0xbb, 0xb6, 0xf9, 0x3b}};
    IDesktopWallpaper* dwp = nullptr;
    if (FAILED(CoCreateInstance(clsidDwp, nullptr, CLSCTX_ALL, iidDwp, (void**)&dwp)) || !dwp)
        return false;

    DESKTOP_WALLPAPER_POSITION pos = DWPOS_FILL;
    dwp->GetPosition(&pos);
    COLORREF back = 0;
    dwp->GetBackgroundColor(&back);

    // wallpaper path for the monitor that contains the icon rect
    std::wstring path;
    POINT c{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    HMONITOR hm = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(hm, &mi);
    for (UINT i = 0;; ++i) {
        LPWSTR dev = nullptr;
        if (FAILED(dwp->GetMonitorDevicePathAt(i, &dev)) || !dev) break;
        RECT mr{};
        dwp->GetMonitorRECT(dev, &mr); // returns S_FALSE on the last monitor; rect is still valid
        if (mr.right > mr.left && EqualRect(&mr, &mi.rcMonitor)) {
            LPWSTR wpath = nullptr;
            if (SUCCEEDED(dwp->GetWallpaper(dev, &wpath)) && wpath && wpath[0]) path = wpath;
            if (wpath) CoTaskMemFree(wpath);
            CoTaskMemFree(dev);
            break;
        }
        CoTaskMemFree(dev);
    }
    if (path.empty()) { // fall back to the default/primary wallpaper
        LPWSTR wpath = nullptr;
        if (SUCCEEDED(dwp->GetWallpaper(nullptr, &wpath)) && wpath && wpath[0]) path = wpath;
        if (wpath) CoTaskMemFree(wpath);
    }
    dwp->Release();

    if (path.empty()) { // plain solid-color desktop
        if (!g_info.solid) {
            g_info = Info{};
            g_info.solid = true;
        }
        g_info.back = back;
        g_info.pos = pos;
        return true;
    }

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return false;
    if (g_info.path == path && g_info.mtime.dwLowDateTime == fa.ftLastWriteTime.dwLowDateTime &&
        g_info.mtime.dwHighDateTime == fa.ftLastWriteTime.dwHighDateTime && !g_info.px.empty())
        return true; // cache hit

    Info ni{};
    ni.path = path;
    ni.mtime = fa.ftLastWriteTime;
    ni.back = back;
    ni.pos = pos;
    if (!DecodeImageFile(path, ni.px, ni.w, ni.h)) return false;
    g_info = ni;
    return true;
}

// Fill s with what the wallpaper shows at screen rect rc.
bool SampleRect(const RECT& rc, Shot& s) {
    s.w = rc.right - rc.left;
    s.h = rc.bottom - rc.top;
    if (s.w <= 0 || s.h <= 0) return false;
    s.px.resize((size_t)s.w * s.h * 4);

    if (g_info.solid) {
        BYTE b = GetBValue(g_info.back), g = GetGValue(g_info.back), r = GetRValue(g_info.back);
        for (size_t i = 0; i < s.px.size(); i += 4) {
            s.px[i] = b; s.px[i + 1] = g; s.px[i + 2] = r; s.px[i + 3] = 0xFF;
        }
        return true;
    }

    // destination area the wallpaper is mapped onto
    RECT area{};
    if (g_info.pos == DWPOS_SPAN) {
        area.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        area.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        area.right = area.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        area.bottom = area.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    } else {
        HMONITOR hm = MonitorFromPoint({(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2},
                                       MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(hm, &mi);
        area = mi.rcMonitor;
    }
    const double Aw = area.right - area.left, Ah = area.bottom - area.top;
    const double iw = g_info.w, ih = g_info.h;
    BYTE backB = GetBValue(g_info.back), backG = GetGValue(g_info.back), backR = GetRValue(g_info.back);

    // how the image is laid out over the area, and the source-rect per dst pixel
    double ox = 0, oy = 0, sx = 1.0, sy = 1.0; // image origin (screen) + dst->src scale
    bool repeat = false;
    switch (g_info.pos) {
        case DWPOS_FILL:
        case DWPOS_SPAN: {
            double sc = std::max(Aw / iw, Ah / ih);
            sx = sy = sc;
            ox = area.left + (Aw - iw * sc) / 2;
            oy = area.top + (Ah - ih * sc) / 2;
            break;
        }
        case DWPOS_FIT: {
            double sc = std::min(Aw / iw, Ah / ih);
            sx = sy = sc;
            ox = area.left + (Aw - iw * sc) / 2;
            oy = area.top + (Ah - ih * sc) / 2;
            break;
        }
        case DWPOS_STRETCH:
            sx = Aw / iw;
            sy = Ah / ih;
            ox = (double)area.left;
            oy = (double)area.top;
            break;
        case DWPOS_TILE:
            repeat = true;
            ox = area.left + std::floor((Aw - iw) / 2);
            oy = area.top + std::floor((Ah - ih) / 2);
            break;
        case DWPOS_CENTER:
        default:
            ox = area.left + (Aw - iw) / 2;
            oy = area.top + (Ah - ih) / 2;
            break;
    }

    for (int y = 0; y < s.h; ++y) {
        for (int x = 0; x < s.w; ++x) {
            double fx = (rc.left + x - ox) / sx; // source coords (image px)
            double fy = (rc.top + y - oy) / sy;
            // store bottom-up like CaptureRect's DIB, matching ICO XOR order
            BYTE* d = &s.px[((size_t)(s.h - 1 - y) * s.w + x) * 4];
            double kx = 1.0 / sx, ky = 1.0 / sy; // source px covered by one dst px

            auto inside = [&](double X, double Y) {
                return X >= 0 && Y >= 0 && X < iw && Y < ih;
            };
            if (repeat) {
                fx = std::fmod(fx, iw); if (fx < 0) fx += iw;
                fy = std::fmod(fy, ih); if (fy < 0) fy += ih;
            }
            if (!inside(fx, fy) && !repeat) {
                d[0] = backB; d[1] = backG; d[2] = backR; d[3] = 0xFF;
                continue;
            }

            // area-weighted box filter over the covered source footprint:
            // exact box when downscaling, reduces to bilinear when upscaling
            auto span = [&](double start, double len, double lim, int& i0, double* wt, double& sum) {
                int first = (int)std::floor(start);
                int count = (int)std::ceil(start + len) - first;
                if (count > 4096) count = 4096;
                sum = 0;
                for (int k = 0; k < count; ++k) {
                    int idx = first + k;
                    double lo = std::max((double)idx, start);
                    double hi = std::min((double)idx + 1.0, start + len);
                    double ov = hi - lo;
                    wt[k] = 0;
                    if (repeat) {
                        if (ov > 0) { wt[k] = ov; sum += ov; }
                    } else {
                        if (idx < 0 || idx >= (int)lim || ov <= 0) continue;
                        wt[k] = ov;
                        sum += ov;
                    }
                }
                i0 = first;
                return count;
            };
            double wx[4096], wy[4096], swx = 0, swy = 0;
            int ix0, iy0, cx, cy;
            cx = span(fx, std::max(kx, 1e-9), iw, ix0, wx, swx);
            cy = span(fy, std::max(ky, 1e-9), ih, iy0, wy, swy);
            if (swx <= 0 || swy <= 0) {
                d[0] = backB; d[1] = backG; d[2] = backR; d[3] = 0xFF;
                continue;
            }
            long long bb = 0, gg = 0, rr = 0;
            double tw = 0;
            for (int yy = 0; yy < cy; ++yy) {
                if (wy[yy] <= 0) continue;
                int syi = iy0 + yy;
                if (repeat) { syi %= (int)ih; if (syi < 0) syi += (int)ih; }
                for (int xx = 0; xx < cx; ++xx) {
                    if (wx[xx] <= 0) continue;
                    int sxi = ix0 + xx;
                    if (repeat) { sxi %= (int)iw; if (sxi < 0) sxi += (int)iw; }
                    const BYTE* p = &g_info.px[((size_t)syi * g_info.w + sxi) * 4];
                    double w = wx[xx] * wy[yy];
                    bb += (long long)(p[0] * w); gg += (long long)(p[1] * w); rr += (long long)(p[2] * w);
                    tw += w;
                }
            }
            if (tw <= 0) {
                d[0] = backB; d[1] = backG; d[2] = backR; d[3] = 0xFF;
                continue;
            }
            d[0] = (BYTE)(bb / tw + 0.5); d[1] = (BYTE)(gg / tw + 0.5); d[2] = (BYTE)(rr / tw + 0.5); d[3] = 0xFF;
        }
    }
    return true;
}

} // namespace wall

// ------------------------------------------------------------------ logic --

void UpdateStatus(const std::wstring& s) {
    if (g_statusCtl && IsWindow(g_statusCtl)) SetWindowTextW(g_statusCtl, s.c_str());
    Log(L"status: %s", s.c_str());
}

std::wstring MakeStatus(int w, int h, const RECT& rc, bool unchanged, const wchar_t* mode) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t t[16];
    swprintf(t, 16, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    std::wstring r = unchanged ? L"背景无变化 no change · " : L"已伪装 camouflaged · ";
    r += mode;
    r += g_lnkPath.empty() ? L" · 文件夹" : L" · 快捷方式";
    r += L" · " + std::to_wstring(w) + L"\u00d7" + std::to_wstring(h) + L" @ (" +
         std::to_wstring(rc.left) + L"," + std::to_wstring(rc.top) + L") · #" +
         std::to_wstring(g_updates) + L" · " + t;
    return r;
}

bool RectEq(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool RectCovered(const RECT& rc, const std::set<DWORD>& expPids) {
    CoverCtx cc{&rc, false, &expPids};
    EnumWindows(CoverCb, (LPARAM)&cc);
    return cc.covered;
}

bool DoCycle() {
    if (g_folder.empty()) return false;
    std::vector<std::wstring> names = LocateNames();
    std::set<DWORD> expPids = ExplorerPids();

    FolderLoc loc;
    if (!LocateOnDesktop(names, expPids, loc)) {
        UpdateStatus(g_lnkPath.empty()
                         ? L"未能在桌面定位该文件夹（需要 explorer 桌面且图标可见）Folder not found on desktop"
                         : L"未能在桌面定位该快捷方式（需要 explorer 桌面且图标可见）Shortcut not found on desktop");
        return false;
    }

    const bool covered = RectCovered(loc.iconRectScreen, expPids);
    Shot s;
    bool fromFile = false;

    // Preferred path: render the wallpaper FILE at the icon rect. Needs no
    // icon moving and works while other windows cover the desktop.
    if (wall::RefreshFor(loc.iconRectScreen)) {
        // Drift probe: once trusted and visible, the live screen must still
        // equal our last sample. A mismatch means the wallpaper changed or an
        // engine renders dynamically -> fall back to real captures.
        if (g_wallTrusted && !covered && g_lastW > 0 && !g_lastPx.empty() &&
            RectEq(g_lastRect, loc.iconRectScreen)) {
            Shot now;
            if (CaptureRect(g_lastRect, now) && now.w == g_lastW && now.h == g_lastH) {
                double d = wall::MeanDiff(now.px, g_lastPx);
                if (d > 4.0) {
                    g_wallTrusted = false;
                    Log(L"wallpaper drift %.2f%% -> recalibrating from screen", d);
                }
            }
        }
        // Optimistically use the file even uncalibrated while covered: the
        // icon is hidden behind a window anyway; calibrate once visible.
        if (g_wallTrusted || covered) {
            if (wall::SampleRect(loc.iconRectScreen, s)) fromFile = true;
        }
    }

    if (!fromFile) {
        // Fallback: capture the real desktop. Never sample through a covering
        // window - the true background is not visible there.
        if (covered) {
            UpdateStatus(L"目标位置被窗口遮挡且壁纸文件不可用，暂停刷新（移开后自动恢复）Occluded, paused");
            return false;
        }
        RemoteListView rl;
        if (!rl.Attach(loc.lv)) {
            UpdateStatus(L"无法访问桌面列表视图 Cannot access desktop list view");
            return false;
        }
        // Move the icon off any monitor, let the desktop repaint, capture the
        // pure background (wallpaper engines render there too), then restore.
        POINT away{GetSystemMetrics(SM_XVIRTUALSCREEN) - 8192,
                   GetSystemMetrics(SM_YVIRTUALSCREEN) - 8192};
        rl.SetPos(loc.index, away);
        Sleep(kSettleMs);
        bool ok = CaptureRect(loc.iconRectScreen, s);
        rl.SetPos(loc.index, loc.slot);
        // Verify the icon really returned to its slot; desktop organizers
        // (Fences etc.) can snap a moved icon to a different cell.
        POINT back{};
        for (int t = 0; t < 2; ++t) {
            if (!rl.Pos(loc.index, back)) break;
            if (std::abs(back.x - loc.slot.x) <= 2 && std::abs(back.y - loc.slot.y) <= 2) break;
            rl.SetPos(loc.index, loc.slot);
        }
        if (std::abs(back.x - loc.slot.x) > 2 || std::abs(back.y - loc.slot.y) > 2)
            Log(L"restore mismatch: pos (%d,%d) wanted (%d,%d)", back.x, back.y,
                loc.slot.x, loc.slot.y);
        if (!ok) {
            UpdateStatus(L"屏幕截图失败 Screen capture failed");
            return false;
        }
        // Calibrate: does the file-rendered wallpaper match reality? If yes,
        // later cycles can switch to the (much cheaper, occlusion-proof)
        // file path; a wallpaper engine fails this check permanently.
        Shot fs;
        if (wall::RefreshFor(loc.iconRectScreen) && wall::SampleRect(loc.iconRectScreen, fs) &&
            fs.w == s.w && fs.h == s.h) {
            double d = wall::MeanDiff(fs.px, s.px);
            g_wallTrusted = d < 4.0;
            Log(L"wallpaper-file calibration diff %.2f%% -> %s", d,
                g_wallTrusted ? L"trusted" : L"untrusted");
        } else {
            g_wallTrusted = false;
        }
    }

    g_lastRect = loc.iconRectScreen;

    const bool unchanged = (s.w == g_lastW && s.h == g_lastH && s.px == g_lastPx);
    const wchar_t* mode = fromFile ? (g_wallTrusted ? L"壁纸文件 wallpaper-file"
                                                   : L"壁纸文件(未校准) wallpaper-file (uncalibrated)")
                                  : L"截屏 screenshot";
    if (unchanged) {
        UpdateStatus(MakeStatus(s.w, s.h, loc.iconRectScreen, true, mode));
        return true;
    }

    std::wstring ico = g_artifactsDir + L"\\wall_icon_" + std::to_wstring(g_iconGen % kIconSlots) + L".ico";
    if (!WriteIcoFile(ico, s)) {
        UpdateStatus(L"写入 ICO 文件失败 Failed to write .ico");
        return false;
    }
    if (!g_lnkPath.empty()) SetLnkIconLocation(g_lnkPath, ico);
    else ApplyFolderIcon(g_folder, ico);
    g_iconGen++;
    g_lastW = s.w;
    g_lastH = s.h;
    g_lastPx = std::move(s.px);
    g_updates++;
    UpdateStatus(MakeStatus(g_lastW, g_lastH, loc.iconRectScreen, false, mode));
    Log(L"icon updated #%llu %ls (%s)", g_updates, ico.c_str(), mode);
    return true;
}

// -------------------------------------------------------------------- UI --

int WndDpi(HWND h) {
    using Fn = UINT(WINAPI*)(HWND);
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        if (Fn fn = (Fn)(void*)GetProcAddress(u, "GetDpiForWindow")) {
            UINT d = fn(h);
            if (d) return (int)d;
        }
    }
    HDC dc = GetDC(nullptr);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

void CreateControls(HWND hwnd) {
    int dpi = WndDpi(hwnd);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                 S(x), S(y), S(w), S(h), hwnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
        if (g_font) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        return c;
    };

    mk(L"STATIC", L"目标文件夹 Target folder", SS_LEFT, 16, 12, 400, 20, 0);
    g_pathEdit = mk(L"EDIT", g_folder.c_str(), ES_READONLY | ES_AUTOHSCROLL, 16, 34, 408, 24, 0);

    mk(L"STATIC", L"刷新间隔 Interval (ms)", SS_LEFT, 16, 70, 200, 20, 0);
    g_intervalCombo = mk(L"COMBOBOX", nullptr, CBS_DROPDOWNLIST | WS_VSCROLL, 16, 90, 140, 220, IDC_INTERVAL);
    static const int kVals[] = {100, 200, 300, 500, 1000, 2000};
    int best = 0;
    for (int i = 0; i < 6; ++i) {
        std::wstring t = std::to_wstring(kVals[i]);
        SendMessageW(g_intervalCombo, CB_ADDSTRING, 0, (LPARAM)t.c_str());
        if (abs(kVals[i] - (int)g_intervalMs) < abs(kVals[best] - (int)g_intervalMs)) best = i;
    }
    SendMessageW(g_intervalCombo, CB_SETCURSEL, best, 0);

    mk(L"STATIC", L"状态 Status", SS_LEFT, 16, 124, 300, 20, 0);
    g_statusCtl = mk(L"STATIC", L"等待首次刷新 waiting for first pass...",
                     SS_LEFT | WS_BORDER, 16, 144, 408, 84, 0);

    mk(L"BUTTON", L"隐藏窗口 Hide", 0, 16, 244, 120, 30, IDB_HIDE);
    mk(L"BUTTON", L"恢复默认图标 Restore", 0, 144, 244, 160, 30, IDB_RESTORE);
    mk(L"BUTTON", L"退出 Exit", 0, 312, 244, 112, 30, IDB_EXIT);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        CreateControls(h);
        return 0;
    case WM_TIMER:
        if (w == 1) DoCycle();
        return 0;
    case WM_CLOSE: // closing hides instead of exiting
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if (id == IDB_HIDE && code == BN_CLICKED) {
            ShowWindow(h, SW_HIDE);
        } else if (id == IDB_RESTORE && code == BN_CLICKED) {
            if (!g_lnkPath.empty()) {
                ResetLnkIcon(g_lnkPath);
                DeleteArtifacts();
                g_lastW = 0;
                g_lastH = 0;
                g_lastPx.clear();
                g_wallTrusted = false;
            } else {
                RestoreFolderIcon(g_folder);
            }
            UpdateStatus(L"已恢复默认图标 Icon restored（后台继续伪装中）");
        } else if (id == IDB_EXIT && code == BN_CLICKED) {
            DestroyWindow(h);
        } else if (id == IDC_INTERVAL && code == CBN_SELCHANGE) {
            static const int kVals[] = {100, 200, 300, 500, 1000, 2000};
            int sel = (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < 6) {
                g_intervalMs = (UINT)kVals[sel];
                SetTimer(h, 1, g_intervalMs, nullptr);
                Log(L"interval -> %u ms", g_intervalMs);
            }
        }
        return 0;
    }
    case kWmShow: // second instance asked us to show up
        ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ------------------------------------------------------------------ CLI --

void ParseCommandLine() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            std::wstring a = argv[i];
            bool flag = a.rfind(L"--", 0) == 0 || a.rfind(L"/", 0) == 0;
            if (_wcsicmp(a.c_str(), L"--once") == 0 || _wcsicmp(a.c_str(), L"/once") == 0) {
                g_onceMode = true;
            } else if (_wcsicmp(a.c_str(), L"--locate") == 0 || _wcsicmp(a.c_str(), L"/locate") == 0) {
                g_locateMode = true;
            } else if (_wcsicmp(a.c_str(), L"--log") == 0 || _wcsicmp(a.c_str(), L"/log") == 0) {
                g_logEnabled = true;
            } else if (_wcsicmp(a.c_str(), L"--help") == 0 || _wcsicmp(a.c_str(), L"/?") == 0) {
                g_helpOnly = true;
            } else if (a.rfind(L"--interval=", 0) == 0) {
                g_intervalMs = (UINT)_wtoi(a.c_str() + 11);
            } else if ((_wcsicmp(a.c_str(), L"--interval") == 0 || _wcsicmp(a.c_str(), L"/interval") == 0) && i + 1 < argc) {
                g_intervalMs = (UINT)_wtoi(argv[++i]);
            } else if (!flag && g_folder.empty()) {
                g_folder = a;
            }
        }
        LocalFree(argv);
    }
    if (g_folder.empty()) g_folder = DirOf(ExePathW());
    while (g_folder.size() > 3 && (g_folder.back() == L'\\' || g_folder.back() == L'/'))
        g_folder.pop_back();
    if (g_intervalMs < 50) g_intervalMs = 50;
    if (g_intervalMs > 60000) g_intervalMs = 60000;
}

int RunLocate() {
    bool con = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    if (con) freopen("CONOUT$", "w", stdout);
    printf("HideFoldersUsingFolderIcons --locate (read-only)\n");
    printf("folder : %ls\n", g_folder.c_str());
    if (!g_lnkPath.empty())
        printf("shortcut: %ls\ntarget : %ls\n", g_lnkPath.c_str(), g_folder.c_str());
    printf("mode   : %s\n", g_lnkPath.empty() ? "desktop.ini folder icon"
                                              : "shortcut (.lnk) icon");
    Log(L"--locate start folder=%ls shortcut=%ls", g_folder.c_str(), g_lnkPath.c_str());

    std::vector<std::wstring> names = LocateNames();
    RECT prev{};
    bool have = false;
    for (int k = 0; k < 5; ++k) {
        std::set<DWORD> pids = ExplorerPids();
        FolderLoc loc;
        if (LocateOnDesktop(names, pids, loc)) {
            char cls[64] = {}, rootCls[64] = {};
            GetClassNameA(loc.lv, cls, 63);
            HWND root = GetAncestor(loc.lv, GA_ROOT);
            GetClassNameA(root, rootCls, 63);
            DWORD pid = 0;
            GetWindowThreadProcessId(loc.lv, &pid);
            bool moved = have && (loc.iconRectScreen.left != prev.left ||
                                  loc.iconRectScreen.top != prev.top);
            printf("sample %d: item %d rect (%d,%d)-(%d,%d) slot (%d,%d) root=%s pid=%lu%s\n",
                   k, loc.index, loc.iconRectScreen.left, loc.iconRectScreen.top,
                   loc.iconRectScreen.right, loc.iconRectScreen.bottom,
                   loc.slot.x, loc.slot.y, rootCls, pid,
                   moved ? "  <-- POSITION CHANGED" : "");
            prev = loc.iconRectScreen;
            have = true;
        } else {
            printf("sample %d: not found on any desktop list view\n", k);
        }
        if (k < 4) Sleep(1000);
    }
    printf("log file: %%TEMP%%\\HFUFO_log.txt\n");
    return 0;
}

int RunOnce() {
    bool con = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
    if (con) freopen("CONOUT$", "w", stdout);
    printf("HideFoldersUsingFolderIcons --once\n");
    printf("folder : %ls\ninterval: %u ms\n", g_folder.c_str(), g_intervalMs);
    if (!g_lnkPath.empty())
        printf("shortcut: %ls\nartifacts: %ls\nmode   : shortcut (.lnk) icon\n",
               g_lnkPath.c_str(), g_artifactsDir.c_str());
    Log(L"--once start folder=%ls shortcut=%ls", g_folder.c_str(), g_lnkPath.c_str());

    bool ok1 = DoCycle();
    printf("pass 1 : %s\n", ok1 ? "ok" : "no update (occluded / not found)");
    Sleep(1200);
    DoCycle(); // second pass fixes any first-pass occlusion artifact
    Sleep(1200);

    if (g_lastW > 0) {
        if (RectCovered(g_lastRect, ExplorerPids()))
            printf("note   : icon rect is covered by a window; the numbers below compare that window, not the wallpaper\n");
        Shot now;
        if (CaptureRect(g_lastRect, now) && now.w == g_lastW && now.h == g_lastH &&
            now.px.size() == g_lastPx.size()) {
            double diff = wall::MeanDiff(now.px, g_lastPx);
            printf("verify : mean pixel diff = %.2f%% (%s)\n", diff,
                   diff < 3.0 ? "PASS: folder icon matches the wallpaper"
                              : "CHECK: shell may not have refreshed the icon yet");
            Log(L"verify diff=%.2f%%", diff);
        } else {
            printf("verify : could not capture the icon rect\n");
        }
    }
    printf("log file: %%TEMP%%\\HFUFO_log.txt\n");
    return g_lastW > 0 ? 0 : 2;
}

void EnableDpiAwareness() {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        using Fn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (Fn fn = (Fn)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext"))
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    }
    SetProcessDPIAware();
}

void ShowHelp() {
    MessageBoxW(nullptr,
        L"用法 Usage:\n"
        L"  HideFoldersUsingFolderIcons.exe [文件夹/快捷方式 folder or .lnk] [选项 options]\n\n"
        L"  --interval <ms>  刷新间隔 refresh interval (default 200)\n"
        L"  --once           采样一次并验证后退出 run one pass and exit (debug)\n"
        L"  --locate         只定位不修改 locate only, no changes (debug)\n"
        L"  --log            写日志 log to %TEMP%\\HFUFO_log.txt\n\n"
        L"默认目标为 exe 所在文件夹。\n"
        L"若桌面上存在指向该文件夹的快捷方式，自动改为伪装快捷方式图标。\n"
        L"By default the folder containing the exe is camouflaged. A desktop\n"
        L"shortcut pointing at the folder is camouflaged instead when present.",
        L"HideFoldersUsingFolderIcons", MB_ICONINFORMATION);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    g_inst = hInst;
    g_selfPid = GetCurrentProcessId();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    EnableDpiAwareness();
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);
    ParseCommandLine();
    InitTargetMode();

    if (g_helpOnly) {
        ShowHelp();
        return 0;
    }
    if (g_locateMode) {
        g_logEnabled = true;
        DeleteFileW(LogPath().c_str());
        return RunLocate();
    }
    if (g_onceMode) {
        g_logEnabled = true;
        DeleteFileW(LogPath().c_str());
        return RunOnce();
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running: bring the existing window up and exit.
        for (int i = 0; i < 20; ++i) {
            HWND h = FindWindowW(kClassName, nullptr);
            if (h) {
                SendMessageW(h, kWmShow, 0, 0);
                SetForegroundWindow(h);
                break;
            }
            Sleep(100);
        }
        return 0;
    }

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0))
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);

    int dpi = WndDpi(nullptr);
    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };
    RECT rc{0, 0, S(440), S(282)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    g_mainWnd = CreateWindowExW(0, kClassName,
                                L"HideFoldersUsingFolderIcons — 文件夹壁纸伪装",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top,
                                nullptr, nullptr, g_inst, nullptr);
    if (!g_mainWnd) return 1;

    ShowWindow(g_mainWnd, nCmdShow);
    UpdateWindow(g_mainWnd);
    SetTimer(g_mainWnd, 1, g_intervalMs, nullptr);
    DoCycle(); // camouflage immediately

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_mainWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return (int)msg.wParam;
}
