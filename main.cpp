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
constexpr int kIconSlots = 8;   // cycling icon file names defeat the icon cache
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

std::wstring g_folder;      // folder to camouflage (or an explicit .lnk path)
std::wstring g_artifactsDir;// where wall_icon_*.ico files are written
UINT g_intervalMs = 200;
bool g_logEnabled = false;
bool g_onceMode = false;
bool g_locateMode = false;
bool g_helpOnly = false;
bool g_wallTrusted = false; // wallpaper-file sampling verified against screen

// Everything that gets camouflaged: the folder itself plus every desktop
// shortcut pointing at it (organizers ignore desktop.ini icons, so the
// shortcuts are usually the ones that visually matter).
struct CamTarget {
    bool isLnk = false;
    std::wstring lnk;      // .lnk path (shortcut targets)
    std::wstring name;     // display name without extension
    std::wstring nameExt;  // display name with extension (extensions visible)
};
std::vector<CamTarget> g_targets;

struct ItemCache {         // last applied sample per icon rect
    RECT rc{};
    int w = 0, h = 0;
    std::vector<BYTE> px;
};
std::vector<ItemCache> g_itemCache;
int g_iconGen = 0;
unsigned long long g_updates = 0;
std::set<int> g_probeApplied; // target indices already handled via probe this cycle

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

    // Overlay bits come back in the return value, so this needs no remote
    // buffer: shortcuts carry the arrow overlay, folders none.
    DWORD OverlayState(int i) {
        return (DWORD)Snd(LVM_GETITEMSTATE, (WPARAM)i, (LPARAM)LVIS_OVERLAYMASK);
    }

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
    std::wstring text;       // matched desktop display name
    DWORD overlay = 0;       // LVIS_OVERLAYMASK bits (shortcut arrow)
};

// Collect EVERY desktop item whose text matches one of `names`.
bool LocateAllOnDesktop(const std::vector<std::wstring>& names, const std::set<DWORD>& expPids, std::vector<FolderLoc>& outs) {
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
                    FolderLoc f;
                    f.lv = lv;
                    f.index = i;
                    f.iconRectScreen = rc;
                    f.slot = pt;
                    f.text = t;
                    f.overlay = rl.OverlayState(i);
                    outs.push_back(f);
                    Log(L"located '%s' item %d in %dx%d view, icon rect (%d,%d)-(%d,%d), overlay 0x%X",
                        t.c_str(), i, n, n, rc.left, rc.top, rc.right, rc.bottom, f.overlay);
                }
            }
        }
    };

    tryList(ctx.desktopLv, 1 << 20);
    tryList(ctx.otherLv, 2000); // organizer tools: don't scan huge lists
    return !outs.empty();
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
    // CREATE_ALWAYS on an existing HIDDEN/SYSTEM file (desktop.ini!) fails
    // with ERROR_ACCESS_DENIED unless the call carries those attributes too.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        Log(L"write failed '%s' err=%lu", path.c_str(), GetLastError());
        return;
    }
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

// Minimal icon reset for ONE folder: drop desktop.ini only, keep the shared
// wall_icon_*.ico artifacts other targets still point at.
void ResetFolderIconOnly(const std::wstring& folder) {
    DeleteFileW((folder + L"\\desktop.ini").c_str());
    DWORD at = GetFileAttributesW(folder.c_str());
    if (at != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(folder.c_str(), at & ~FILE_ATTRIBUTE_SYSTEM);
    NotifyShell(folder);
}

void RestoreFolderIcon(const std::wstring& folder) {
    DeleteFileW((folder + L"\\desktop.ini").c_str());
    DeleteArtifacts();
    DWORD at = GetFileAttributesW(folder.c_str());
    if (at != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(folder.c_str(), at & ~FILE_ATTRIBUTE_SYSTEM);
    g_itemCache.clear();
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

void FindLnksForFolder(const std::wstring& dir, std::vector<std::wstring>& out) {
    std::vector<std::wstring> dirs;
    DesktopDirs(dirs);
    for (const auto& d : dirs) {
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW((d + L"\\*.lnk").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring lnk = d + L"\\" + fd.cFileName;
            std::wstring t;
            if (ResolveLnkTarget(lnk, t) && _wcsicmp(t.c_str(), dir.c_str()) == 0)
                out.push_back(lnk);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
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
    for (int i = 0; i < 16; ++i) // marker probes
        DeleteFileW((dir + L"\\marker_" + std::to_wstring(i) + L".ico").c_str());
}

// Decide what to camouflage: an explicit .lnk argument (single shortcut),
// or a folder — in which case EVERY desktop shortcut pointing at it is
// included too, so all look-alike icons disappear, not just one.
void InitTargetMode() {
    std::wstring dir;
    if (!g_folder.empty() && HasExt(g_folder, L".lnk")) {
        CamTarget t;
        t.isLnk = true;
        t.lnk = g_folder;
        t.nameExt = BaseName(g_folder);
        t.name = HasExt(t.nameExt, L".lnk") ? t.nameExt.substr(0, t.nameExt.size() - 4)
                                            : t.nameExt;
        g_targets.push_back(std::move(t));
        std::wstring tgt;
        if (ResolveLnkTarget(g_folder, tgt) && DirExists(tgt)) dir = tgt;
    } else if (DirExists(g_folder)) {
        dir = g_folder;
        CamTarget tf;
        tf.name = BaseName(g_folder);
        tf.nameExt = tf.name;
        g_targets.push_back(std::move(tf));
        std::vector<std::wstring> lnks;
        FindLnksForFolder(g_folder, lnks);
        for (const auto& l : lnks) {
            CamTarget t;
            t.isLnk = true;
            t.lnk = l;
            t.nameExt = BaseName(l);
            t.name = HasExt(t.nameExt, L".lnk") ? t.nameExt.substr(0, t.nameExt.size() - 4)
                                                : t.nameExt;
            g_targets.push_back(std::move(t));
            Log(L"shortcut target: %ls", l.c_str());
        }
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
    size_t lnkCount = 0;
    for (const auto& t : g_targets) lnkCount += t.isLnk ? 1 : 0;
    Log(L"target: folder=%ls artifacts=%ls mode=multi (%d target(s), %d shortcut(s))",
        g_folder.c_str(), g_artifactsDir.c_str(), (int)g_targets.size(), (int)lnkCount);
}

std::vector<std::wstring> LocateNames() {
    std::vector<std::wstring> v;
    for (const auto& t : g_targets) {
        v.push_back(t.name);
        if (t.isLnk) v.push_back(t.nameExt); // shown when extensions are visible
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

// Does target t display under desktop text `text`?
bool NameMatch(const CamTarget& t, const std::wstring& text) {
    return lstrcmpiW(text.c_str(), t.name.c_str()) == 0 ||
           (t.isLnk && lstrcmpiW(text.c_str(), t.nameExt.c_str()) == 0);
}

// Apply one sampled icon to every target belonging to located item `loc`.
// When several items share a display name (a folder and a shortcut can show
// identical text, e.g. both "　"), the shortcut-arrow OVERLAY tells them
// apart: the shortcut item carries overlay bits, the folder does not. That
// keeps each icon's content at its own position instead of a sibling's.
void ApplyToTargets(const FolderLoc& loc, const std::wstring& ico,
                    const std::vector<FolderLoc>& locs) {
    int sameLocs = 0, shortcutLocs = 0, folderLocs = 0, shortcutTargets = 0, folderTargets = 0;
    for (const auto& o : locs) {
        if (lstrcmpiW(o.text.c_str(), loc.text.c_str()) != 0) continue;
        ++sameLocs;
        if (o.overlay & LVIS_OVERLAYMASK) ++shortcutLocs; else ++folderLocs;
    }
    for (const auto& t : g_targets) {
        if (!NameMatch(t, loc.text)) continue;
        if (t.isLnk) ++shortcutTargets; else ++folderTargets;
    }
    const bool ambiguous = sameLocs > 1;
    bool useOverlay = false;
    if (ambiguous) {
        useOverlay = (shortcutLocs == shortcutTargets && folderLocs == folderTargets &&
                      shortcutTargets + folderTargets > 0);
        if (!useOverlay)
            Log(L"same-name overlay split failed for '%s' (items %d lnk/%d dir, targets %d lnk/%d dir)",
                loc.text.c_str(), shortcutLocs, folderLocs, shortcutTargets, folderTargets);
    }
    const bool locIsShortcut = (loc.overlay & LVIS_OVERLAYMASK) != 0;

    for (size_t ti = 0; ti < g_targets.size(); ++ti) {
        const CamTarget& t = g_targets[ti];
        if (!NameMatch(t, loc.text)) continue;
        // probe-calibrated targets already carry their measured icon this
        // cycle; applying list-view-rect content here would undo the fix
        if (g_probeApplied.count((int)ti)) continue;
        if (ambiguous && useOverlay && t.isLnk != locIsShortcut) continue;
        if (t.isLnk) SetLnkIconLocation(t.lnk, ico);
        else ApplyFolderIcon(g_folder, ico);
    }
    if (ambiguous && useOverlay)
        Log(L"'%s' item %d is the %s", loc.text.c_str(), loc.index,
            locIsShortcut ? L"shortcut" : L"folder");
}

void DeleteArtifacts() { DeleteArtifactsIn(g_artifactsDir); }

// -------------------------------------------------------- probe calibration --
// Desktop organizers decide themselves where icons render - list-view rects
void UpdateStatus(const std::wstring& s); // defined later; marker needs it
// can be plain wrong once an organizer is active - and some organizers hide
// the shortcut arrow overlay too. The only source that is always right is
// what the desktop actually PAINTS: temporarily set every target's icon to a
// unique saturated probe pattern, capture the desktop's own windows with
// PrintWindow (no screen screenshot - works even while other windows cover
// the desktop), find the probe pixels, and read position AND size from where
// each probe really rendered.
namespace marker {

#ifndef PW_CLIENTONLY
#define PW_CLIENTONLY 0x01
#endif
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x02
#endif

constexpr int kProbeSize = 64; // probe source icon size (px)
struct Pal { BYTE b, g, r; };
const Pal kAnchor{255, 0, 255}; // magenta: left half of every probe
// right-half colors identify the target; saturated hues a photo wallpaper
// never averages to, kept far apart from each other and from the anchor
const Pal kUnique[] = {
    {255, 255, 0}, {0, 0, 255}, {0, 255, 0},   {255, 0, 0},
    {0, 255, 255}, {0, 128, 255}, {255, 0, 192}, {255, 128, 0},
    {128, 255, 0}, {128, 0, 255}, {255, 128, 128}, {128, 255, 255},
};

bool PalClose(const BYTE* p, const Pal& c) {
    return std::abs((int)p[0] - (int)c.b) <= 80 && std::abs((int)p[1] - (int)c.g) <= 80 &&
           std::abs((int)p[2] - (int)c.r) <= 80;
}

std::wstring ProbePath(int ti) {
    return g_artifactsDir + L"\\marker_" + std::to_wstring(ti) + L".ico";
}

// A square probe: anchor-colored left half + unique-colored right half.
void WriteProbe(int ti) {
    Shot s;
    s.w = s.h = kProbeSize;
    s.px.resize((size_t)kProbeSize * kProbeSize * 4);
    for (int y = 0; y < kProbeSize; ++y)
        for (int x = 0; x < kProbeSize; ++x) {
            const Pal& c = x < kProbeSize / 2 ? kAnchor : kUnique[ti % ARRAYSIZE(kUnique)];
            BYTE* d = &s.px[((size_t)(kProbeSize - 1 - y) * kProbeSize + x) * 4];
            d[0] = c.b; d[1] = c.g; d[2] = c.r; d[3] = 0xFF;
        }
    WriteIcoFile(ProbePath(ti), s);
}

void ApplyProbe(const CamTarget& t, int ti) {
    if (t.isLnk) SetLnkIconLocation(t.lnk, ProbePath(ti));
    else ApplyFolderIcon(g_folder, ProbePath(ti));
}

// Capture a window's client area WITHOUT a screen screenshot: PrintWindow
// makes the target window paint into our DC, so covering windows do not
// matter. Tries DWM full-content rendering first, classic GDI second.
bool CaptureWindowClient(HWND wnd, Shot& s) {
    RECT cr{};
    if (!GetClientRect(wnd, &cr)) return false;
    s.w = cr.right - cr.left;
    s.h = cr.bottom - cr.top;
    if (s.w < 8 || s.h < 8 || s.w > 8192 || s.h > 8192) return false;
    HDC scr = GetDC(nullptr);
    if (!scr) return false;
    HDC mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = s.w;
    bmi.bmiHeader.biHeight = s.h; // positive = bottom-up, like CaptureRect
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (hb && bits) {
        HGDIOBJ old = SelectObject(mem, hb);
        for (int pass = 0; pass < 2 && !ok; ++pass) {
            UINT flags = pass == 0 ? (PW_CLIENTONLY | PW_RENDERFULLCONTENT) : PW_CLIENTONLY;
            memset(bits, 0, (size_t)s.w * s.h * 4);
            if (!PrintWindow(wnd, mem, (UINT)flags)) continue;
            // reject blank captures (unsupported renderer)
            size_t lit = 0, total = (size_t)s.w * s.h;
            const DWORD* dw = (const DWORD*)bits;
            for (size_t i = 0; i < total; i += 16) // subsampled check
                if (dw[i]) ++lit;
            if (lit == 0) continue;
            s.px.resize(total * 4);
            memcpy(s.px.data(), bits, total * 4);
            ok = true;
        }
        SelectObject(mem, old);
    }
    if (hb) DeleteObject(hb);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    return ok;
}

struct ProbeHit { int targetIdx; RECT screenRect; };

// Scan a captured image (image origin ox,oy in screen coords) for probe
// patterns: cluster anchor-colored pixels, validate the cluster shape, then
// identify the target by the unique color next to the anchor half.
void FindProbes(const Shot& s, int ox, int oy, const std::vector<int>& wantIds,
                std::vector<ProbeHit>& out) {
    if (s.w < 16 || s.h < 16) return;
    auto pxAt = [&](int x, int y) -> const BYTE* {
        return &s.px[((size_t)(s.h - 1 - y) * s.w + x) * 4]; // bottom-up storage
    };
    struct Cl { int x0, y0, x1, y1, n; }; // real pixel coords, top-down
    std::vector<Cl> cls;
    for (int y = 0; y < s.h; y += 2)
        for (int x = 0; x < s.w; x += 2) {
            if (!PalClose(pxAt(x, y), kAnchor)) continue;
            if (cls.size() > 256) return; // wallpaper far too magenta: bail
            bool merged = false;
            for (auto& c : cls) {
                if (x >= c.x0 - 16 && x <= c.x1 + 16 && y >= c.y0 - 16 && y <= c.y1 + 16) {
                    c.x0 = std::min(c.x0, x); c.y0 = std::min(c.y0, y);
                    c.x1 = std::max(c.x1, x); c.y1 = std::max(c.y1, y);
                    ++c.n;
                    merged = true;
                    break;
                }
            }
            if (!merged) cls.push_back({x, y, x, y, 1});
        }
    for (const auto& c : cls) {
        const int aw = c.x1 - c.x0 + 1, ah = c.y1 - c.y0 + 1; // anchor half
        if (c.n < 40 || aw < 8 || ah < 8 || aw > 512 || ah > 512) continue;
        if (std::abs(ah - aw * 2) > ah / 2) continue; // icon should be ~square
        const int ix0 = c.x0, iy0 = c.y0, ix1 = c.x0 + aw * 2 - 1, iy1 = c.y1;
        if (ix1 >= s.w) continue;
        int best = -1;
        size_t bestCnt = 0, tot = 0;
        std::vector<size_t> cnt(wantIds.size(), 0);
        for (int y = iy0; y <= iy1; y += 2)
            for (int x = ix0 + aw; x <= ix1; x += 2) {
                const BYTE* p = pxAt(x, y);
                ++tot;
                for (size_t k = 0; k < wantIds.size(); ++k)
                    if (PalClose(p, kUnique[wantIds[k] % ARRAYSIZE(kUnique)])) ++cnt[k];
            }
        if (!tot) continue;
        for (size_t k = 0; k < cnt.size(); ++k)
            if (cnt[k] > bestCnt) { bestCnt = cnt[k]; best = (int)k; }
        if (best < 0 || bestCnt * 10 < tot) continue; // <10% of the half: noise
        ProbeHit h;
        h.targetIdx = wantIds[best];
        h.screenRect = {ox + ix0, oy + iy0, ox + ix1 + 1, oy + iy1 + 1};
        out.push_back(h);
        Log(L"probe: target#%d rendered at (%d,%d)-(%d,%d) %dx%d", h.targetIdx,
            h.screenRect.left, h.screenRect.top, h.screenRect.right, h.screenRect.bottom,
            h.screenRect.right - h.screenRect.left, h.screenRect.bottom - h.screenRect.top);
    }
}

struct CandCtx { std::vector<HWND>* v; };

BOOL CALLBACK CandTopCb(HWND h, LPARAM lp) {
    auto* c = (CandCtx*)lp;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_selfPid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    RECT wr{};
    if (!GetWindowRect(h, &wr)) return TRUE;
    HMONITOR hm = MonitorFromRect(&wr, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof mi};
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    const long long wa = (long long)(wr.right - wr.left) * (wr.bottom - wr.top);
    const long long ma = (long long)(mi.rcMonitor.right - mi.rcMonitor.left) *
                         (mi.rcMonitor.bottom - mi.rcMonitor.top);
    if (wa * 2 < ma) return TRUE; // not desktop-sized
    for (HWND x : *c->v)
        if (x == h) return TRUE;
    c->v->push_back(h); // EnumWindows yields topmost first
    return TRUE;
}

// Windows that might be painting the desktop icons: any visible
// desktop-sized window (organizers draw icons in their own layer, ABOVE
// explorer), then the located list views' roots as a last resort.
std::vector<HWND> CaptureCandidates(const std::vector<FolderLoc>& locs) {
    std::vector<HWND> v;
    CandCtx ctx{&v};
    EnumWindows(CandTopCb, (LPARAM)&ctx);
    for (const auto& l : locs) {
        HWND add[] = {GetAncestor(l.lv, GA_ROOT), l.lv};
        for (HWND h : add) {
            if (!h) continue;
            bool dup = false;
            for (HWND x : v)
                if (x == h) { dup = true; break; }
            if (!dup) v.push_back(h);
        }
    }
    return v;
}

// One full probe pass: apply probe icons, wait for the repaint, then hunt
// the probe pixels in every candidate window. Fills `out` per target.
// Simulate the Win+D "show desktop" chord (toggles back on a second press).
void PressWinD() {
    INPUT in[4]{};
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_LWIN;
    in[1].ki.wVk = 'D';
    in[2].ki.wVk = 'D';
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = VK_LWIN;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// Organizers don't reload a changed icon until the desktop is visible and
// refreshed: switch to the desktop (Win+D) and send F5, exactly like the
// manual fix. The caller toggles windows back afterwards.
void ShowDesktopAndRefresh() {
    PressWinD();
    Sleep(350); // let the switch settle so F5 reaches the desktop
    INPUT f5[2]{};
    for (auto& i : f5) i.type = INPUT_KEYBOARD;
    f5[0].ki.wVk = VK_F5;
    f5[1].ki.wVk = VK_F5;
    f5[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, f5, sizeof(INPUT));
}

// --- "please refresh manually" prompt --------------------------------------
// If the synthetic F5 didn't make the organizer reload the icons, the user
// can do it by hand: a small topmost window asks for it while the capture
// loop below keeps scanning for the probe markers.
namespace prompt {
wchar_t kCls[] = L"HFUFO_prompt";
HWND g_w = nullptr;

LRESULT CALLBACK Prc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        SetBkMode(dc, TRANSPARENT);
        HFONT f = (HFONT)CreateFontW(-26, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                     0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        HFONT of = (HFONT)SelectObject(dc, f);
        SetTextColor(dc, RGB(0, 0, 0));
        DrawTextW(dc, L"请手动刷新桌面：在桌面按 F5\nPress F5 on the desktop", -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        SelectObject(dc, of);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH b = CreateSolidBrush(RGB(255, 250, 205));
        FillRect((HDC)w, &rc, b);
        DeleteObject(b);
        return 1;
    }
    return DefWindowProcW(h, m, w, l);
}

void Show() {
    if (!g_w) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Prc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kCls;
        RegisterClassW(&wc);
        const int w = 460, h = 92;
        const int sx = GetSystemMetrics(SM_CXSCREEN), sy = GetSystemMetrics(SM_CYSCREEN);
        g_w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kCls, L"",
                              WS_POPUP | WS_BORDER, (sx - w) / 2, sy - h - 140, w, h,
                              nullptr, nullptr, wc.hInstance, nullptr);
    }
    ShowWindow(g_w, SW_SHOWNOACTIVATE);
    UpdateWindow(g_w);
}

void Hide() {
    if (g_w) ShowWindow(g_w, SW_HIDE);
}
} // namespace prompt

// Sleep while keeping windows alive (prompt painting, main dialog input).
void PumpSleep(int totalMs) {
    for (int t = 0; t < totalMs; t += 50) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (g_mainWnd && IsDialogMessageW(g_mainWnd, &m)) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(50);
    }
}

bool RunProbePass(const std::vector<FolderLoc>& locs, std::vector<ProbeHit>& out) {
    std::vector<int> wantIds;
    const int n = (int)g_targets.size();
    for (int i = 0; i < n && i < (int)ARRAYSIZE(kUnique); ++i) {
        WriteProbe(i);
        ApplyProbe(g_targets[i], i);
        wantIds.push_back(i);
    }
    if (wantIds.empty()) return false;

    // force icon reload on organizers: desktop visible + F5 refresh
    Log(L"probe: show desktop + F5 refresh");
    ShowDesktopAndRefresh();
    // if the synthetic F5 didn't take effect the user can refresh by hand:
    // ask for it and keep capturing until the markers really show up
    prompt::Show();
    UpdateStatus(L"探针校准中：如长时间无反应，请在桌面手动按 F5 Probe: press F5 on the desktop");

    const std::vector<HWND> cands = CaptureCandidates(locs);
    bool ok = false;
    for (int attempt = 0; attempt < 120 && !ok; ++attempt) { // up to ~30s
        PumpSleep(250);
        for (HWND c : cands) {
            Shot s;
            if (!CaptureWindowClient(c, s)) continue;
            POINT org{};
            ClientToScreen(c, &org);
            std::vector<ProbeHit> hits;
            FindProbes(s, org.x, org.y, wantIds, hits);
            for (const auto& h : hits) {
                bool have = false;
                for (const auto& o : out)
                    if (o.targetIdx == h.targetIdx) { have = true; break; }
                if (!have) out.push_back(h);
            }
        }
        if (out.size() == wantIds.size()) ok = true; // all found
        // some renderers never draw a target (desktop.ini under organizers):
        // accept a partial result after a second attempt
        else if (!out.empty() && attempt >= 1) ok = true;
    }
    prompt::Hide();
    PressWinD(); // bring the user's windows back
    return !out.empty();
}

} // namespace marker

bool g_probeValid = false;                  // probe rects measured and fresh
std::vector<marker::ProbeHit> g_probeRects; // per-target rendered rects
std::wstring g_probeWallPath;               // wallpaper the rects were measured on
FILETIME g_probeWallMtime{};
unsigned g_cycleCount = 0;
unsigned g_probeRetryAt = 0;                // cycle number to retry probing

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

// Current wallpaper identity (empty path = solid-color desktop).
void CurrentWallpaper(std::wstring& path, FILETIME& mt) {
    path = g_info.path;
    mt = g_info.mtime;
}

} // namespace wall

// ------------------------------------------------------------------ logic --

void UpdateStatus(const std::wstring& s) {
    if (g_statusCtl && IsWindow(g_statusCtl)) SetWindowTextW(g_statusCtl, s.c_str());
    Log(L"status: %s", s.c_str());
}

std::wstring MakeStatus(int nLocated, int nUpdated, int nUnchanged, int w, int h,
                        const RECT& rc, const wchar_t* mode) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t t[16];
    swprintf(t, 16, L"%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
    std::wstring r = nUpdated ? L"已伪装 camouflaged · " : L"背景无变化 no change · ";
    r += mode;
    r += L" · " + std::to_wstring(nLocated) + L" 图标 icon(s)";
    if (nUpdated) r += L"（更新 updated " + std::to_wstring(nUpdated) + L"）";
    if (nUnchanged) r += L"（无变化 no-change " + std::to_wstring(nUnchanged) + L"）";
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

ItemCache* FindItemCache(const RECT& rc) {
    for (auto& c : g_itemCache)
        if (RectEq(c.rc, rc)) return &c;
    return nullptr;
}

void StoreItemCache(const RECT& rc, int w, int h, std::vector<BYTE> px) {
    if (ItemCache* c = FindItemCache(rc)) {
        c->w = w;
        c->h = h;
        c->px = std::move(px);
        return;
    }
    if (g_itemCache.size() >= 16) g_itemCache.erase(g_itemCache.begin());
    ItemCache c;
    c.rc = rc;
    c.w = w;
    c.h = h;
    c.px = std::move(px);
    g_itemCache.push_back(std::move(c));
}

// Was target ti located by the last successful probe pass?
bool probeFoundIn(int ti) {
    for (const auto& h : g_probeRects)
        if (h.targetIdx == ti) return true;
    return false;
}

static bool DoCycleInner() {
    if (g_folder.empty() || g_targets.empty()) return false;
    std::vector<std::wstring> names = LocateNames();
    std::set<DWORD> expPids = ExplorerPids();

    std::vector<FolderLoc> locs;
    if (!LocateAllOnDesktop(names, expPids, locs)) {
        UpdateStatus(L"未能在桌面定位目标图标（需要 explorer 桌面且图标可见）No matching icon found on desktop");
        return false;
    }

    int updated = 0, unchangedCount = 0, skipped = 0;
    const wchar_t* mode = L"";
    RECT lastRc{};
    int lastW = 0, lastH = 0;

    // Probe calibration (namespace marker): sample where the icons REALLY
    // render instead of trusting list-view geometry. Re-measure when the
    // wallpaper changes; retry occasionally after a failed pass.
    ++g_cycleCount;
    g_probeApplied.clear();
    if (wall::RefreshFor(locs[0].iconRectScreen)) {
        std::wstring wp;
        FILETIME wmt{};
        wall::CurrentWallpaper(wp, wmt);
        if (g_probeValid &&
            (wp != g_probeWallPath || memcmp(&wmt, &g_probeWallMtime, sizeof wmt) != 0)) {
            g_probeValid = false;
            Log(L"wallpaper changed -> probe re-measure");
        }
    }
    if (!g_probeValid && g_cycleCount >= g_probeRetryAt) {
        std::vector<marker::ProbeHit> pr;
        if (marker::RunProbePass(locs, pr)) {
            g_probeRects = std::move(pr);
            g_probeValid = true;
            wall::CurrentWallpaper(g_probeWallPath, g_probeWallMtime);
            Log(L"probe calibrated: %d target(s)", (int)g_probeRects.size());
            // A successful pass is authoritative for EVERY target: anything
            // whose probe never rendered isn't drawn by this desktop at all
            // (organizers ignore desktop.ini icons). Reset those to their
            // default icon instead of leaving them on a probe marker; the
            // list-view fallback loop skips them below.
            for (size_t ti = 0; ti < g_targets.size(); ++ti) {
                if (probeFoundIn((int)ti)) continue;
                if (g_targets[ti].isLnk) ResetLnkIcon(g_targets[ti].lnk);
                else ResetFolderIconOnly(g_folder);
                Log(L"probe: target#%d not rendered -> default icon", (int)ti);
            }
        } else {
            g_probeRetryAt = g_cycleCount + 50; // ~10s at the default interval
            Log(L"probe pass found nothing; retry in %u cycle(s)",
                g_probeRetryAt - g_cycleCount);
        }
    }
    if (g_probeValid) {
        for (const auto& h : g_probeRects) {
            Shot s;
            if (!wall::RefreshFor(h.screenRect) || !wall::SampleRect(h.screenRect, s)) continue;
            lastRc = h.screenRect;
            lastW = s.w;
            lastH = s.h;
            ItemCache* cache = FindItemCache(h.screenRect);
            if (cache && cache->w == s.w && cache->h == s.h && cache->px == s.px) {
                ++unchangedCount;
                continue;
            }
            std::wstring ico = g_artifactsDir + L"\\wall_icon_" +
                               std::to_wstring(g_iconGen % kIconSlots) + L".ico";
            if (!WriteIcoFile(ico, s)) continue;
            const CamTarget& t = g_targets[h.targetIdx];
            if (t.isLnk) SetLnkIconLocation(t.lnk, ico);
            else ApplyFolderIcon(g_folder, ico);
            g_iconGen++;
            StoreItemCache(h.screenRect, s.w, s.h, std::move(s.px));
            g_updates++;
            ++updated;
            g_probeApplied.insert(h.targetIdx);
            Log(L"icon updated #%llu target#%d -> %ls (probe rect %d,%d)", g_updates,
                h.targetIdx, ico.c_str(), h.screenRect.left, h.screenRect.top);
        }
        mode = L"探针 probe · 壁纸文件 wallpaper-file";
    }

    for (const auto& loc : locs) {
        // A successful probe pass is authoritative for EVERY target: found
        // ones carry (or already equal) their measured icon this cycle,
        // unrendered ones were reset to their default icon. The list-view
        // fallback below exists only for when probing failed - running it
        // anyway would emit .ico files nobody consumes AND overwrite a
        // probe-corrected icon with wrong list-view-rect content.
        if (g_probeValid) continue;

        const bool covered = RectCovered(loc.iconRectScreen, expPids);
        Shot s;
        bool fromFile = false;

        // Preferred path: render the wallpaper FILE at the icon rect. Needs no
        // icon moving and works while other windows cover the desktop.
        if (wall::RefreshFor(loc.iconRectScreen)) {
            // Drift probe: once trusted and visible, the live screen must still
            // equal our last sample for THIS rect.
            ItemCache* cache = FindItemCache(loc.iconRectScreen);
            if (g_wallTrusted && !covered && cache && cache->w > 0 && !cache->px.empty()) {
                Shot now;
                if (CaptureRect(cache->rc, now) && now.w == cache->w && now.h == cache->h) {
                    double d = wall::MeanDiff(now.px, cache->px);
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
            // Fallback: capture the real desktop. Never sample through a
            // covering window - the true background is not visible there.
            if (covered) {
                ++skipped;
                continue;
            }
            RemoteListView rl;
            if (!rl.Attach(loc.lv)) {
                ++skipped;
                continue;
            }
            // Move the icon off any monitor, let the desktop repaint, capture
            // the pure background, then restore.
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
                ++skipped;
                continue;
            }
            // Calibrate: does the file-rendered wallpaper match reality?
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

        mode = fromFile ? (g_wallTrusted ? L"壁纸文件 wallpaper-file"
                                         : L"壁纸文件(未校准) wallpaper-file (uncalibrated)")
                        : L"截屏 screenshot";
        lastRc = loc.iconRectScreen;
        lastW = s.w;
        lastH = s.h;

        ItemCache* cache = FindItemCache(loc.iconRectScreen);
        if (cache && cache->w == s.w && cache->h == s.h && cache->px == s.px) {
            ++unchangedCount;
            continue;
        }

        std::wstring ico = g_artifactsDir + L"\\wall_icon_" +
                           std::to_wstring(g_iconGen % kIconSlots) + L".ico";
        if (!WriteIcoFile(ico, s)) {
            ++skipped;
            continue;
        }
        ApplyToTargets(loc, ico, locs);
        g_iconGen++;
        StoreItemCache(loc.iconRectScreen, s.w, s.h, std::move(s.px));
        g_updates++;
        ++updated;
        Log(L"icon updated #%llu '%s' -> %ls (%s)", g_updates, loc.text.c_str(),
            ico.c_str(), mode);
    }

    if (updated == 0 && unchangedCount == 0) {
        UpdateStatus(L"目标位置被窗口遮挡且壁纸文件不可用，暂停刷新（移开后自动恢复）Occluded, paused");
        return false;
    }
    UpdateStatus(MakeStatus((int)locs.size(), updated, unchangedCount, lastW, lastH, lastRc, mode));
    return true;
}

// Reentrancy guard: the probe pass pumps messages while waiting for a manual
// desktop refresh, which can dispatch a WM_TIMER that calls DoCycle again.
bool DoCycle() {
    static bool busy = false;
    if (busy) return true;
    busy = true;
    bool r = DoCycleInner();
    busy = false;
    return r;
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
    mk(L"BUTTON", L"刷新 Refresh", 0, 144, 244, 160, 30, IDB_RESTORE);
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
            // Manual refresh: forget every cache so the next cycle re-probes
            // the real icon positions and rewrites the icons from scratch.
            g_itemCache.clear();
            g_wallTrusted = false;
            g_probeValid = false;
            UpdateStatus(L"已请求刷新 Refresh requested（下一轮重新校准并重写图标）");
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
    size_t lnkCount = 0;
    for (const auto& t : g_targets) {
        if (t.isLnk) {
            ++lnkCount;
            printf("target : shortcut %ls\n", t.lnk.c_str());
        } else {
            printf("target : folder  %ls (desktop.ini)\n", g_folder.c_str());
        }
    }
    printf("mode   : multi-target (%d shortcut(s))\n", (int)lnkCount);
    Log(L"--locate start folder=%ls", g_folder.c_str());

    std::vector<std::wstring> names = LocateNames();
    for (int k = 0; k < 5; ++k) {
        std::set<DWORD> pids = ExplorerPids();
        std::vector<FolderLoc> locs;
        if (LocateAllOnDesktop(names, pids, locs)) {
            printf("sample %d: %d item(s)\n", k, (int)locs.size());
            for (const auto& loc : locs) {
                char cls[64] = {}, rootCls[64] = {};
                GetClassNameA(loc.lv, cls, 63);
                HWND root = GetAncestor(loc.lv, GA_ROOT);
                GetClassNameA(root, rootCls, 63);
                DWORD pid = 0;
                GetWindowThreadProcessId(loc.lv, &pid);
                printf("  '%ls' item %d rect (%d,%d)-(%d,%d) slot (%d,%d) root=%s pid=%lu overlay=0x%lX%s\n",
                       loc.text.c_str(), loc.index, loc.iconRectScreen.left, loc.iconRectScreen.top,
                       loc.iconRectScreen.right, loc.iconRectScreen.bottom,
                       loc.slot.x, loc.slot.y, rootCls, pid, loc.overlay,
                       (loc.overlay & LVIS_OVERLAYMASK) ? "  <- shortcut" : "");
            }
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
    size_t lnkCount = 0;
    for (const auto& t : g_targets) lnkCount += t.isLnk ? 1 : 0;
    printf("targets: %d (%d shortcut(s))\nartifacts: %ls\n",
           (int)g_targets.size(), (int)lnkCount, g_artifactsDir.c_str());
    Log(L"--once start folder=%ls", g_folder.c_str());

    bool ok1 = DoCycle();
    printf("pass 1 : %s\n", ok1 ? "ok" : "no update (occluded / not found)");
    Sleep(1200);
    DoCycle(); // second pass fixes any first-pass occlusion artifact
    Sleep(1200);

    int pass = 0, check = 0, cover = 0;
    if (!g_itemCache.empty()) {
        std::set<DWORD> pids = ExplorerPids();
        for (const auto& c : g_itemCache) {
            if (RectCovered(c.rc, pids)) {
                ++cover;
                continue;
            }
            Shot now;
            if (CaptureRect(c.rc, now) && now.w == c.w && now.h == c.h &&
                now.px.size() == c.px.size()) {
                double diff = wall::MeanDiff(now.px, c.px);
                printf("verify : rect (%d,%d) %dx%d diff %.2f%% (%s)\n",
                       c.rc.left, c.rc.top, c.w, c.h, diff,
                       diff < 3.0 ? "PASS" : "CHECK: shell may not have refreshed yet");
                if (diff < 3.0) ++pass; else ++check;
                Log(L"verify rect(%d,%d) diff=%.2f%%", c.rc.left, c.rc.top, diff);
            } else {
                printf("verify : rect (%d,%d) could not capture\n", c.rc.left, c.rc.top);
            }
        }
    }
    if (cover) printf("note   : %d icon rect(s) covered by a window; skipped verify\n", cover);
    printf("log file: %%TEMP%%\\HFUFO_log.txt\n");
    return !g_itemCache.empty() && check == 0 ? 0 : (!g_itemCache.empty() ? 1 : 2);
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
        L"默认目标为 exe 所在文件夹，文件夹及其桌面上所有指向它的\n"
        L"快捷方式都会被同时伪装（每个图标取各自位置的壁纸）。\n"
        L"也可显式传入某个 .lnk 只伪装该快捷方式。\n"
        L"By default the folder containing the exe is camouflaged, along with\n"
        L"every desktop shortcut pointing at it (each icon samples the wallpaper\n"
        L"at its own position). Pass a .lnk path to camouflage just that one.",
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
