/*
 * DivallerBridge.dva — Wine shim that restores TLAC's Divaller support.
 *
 * Why this exists
 * ---------------
 * TLAC finds the Divaller (VID 0E8F / PID 2213) with SetupAPI, opens it with
 * CreateFileW and talks to it through WinUSB (read pipe 0x84, write pipe 0x03).
 * Under Wine none of that works: wineusb.sys does not publish
 * GUID_DEVINTERFACE_USB_DEVICE interfaces, so enumeration finds nothing, and
 * Wine's winusb.dll is nothing but stubs.
 *
 * What it does
 * ------------
 * PD-Loader loads every *.dva in its plugins folder. This one waits for
 * TLAC.dva to be loaded, then rewrites TLAC's import table so that its SetupAPI,
 * CreateFileW and WinUsb_* calls land here. A fake device is reported, and the
 * pipe reads/writes are forwarded over TCP 127.0.0.1 to divaller_bridge.py,
 * which owns the real device on the Linux side through libusb.
 *
 * Nothing outside TLAC.dva is touched, and any call that is not about the
 * Divaller is passed through to the real function.
 *
 * Environment variables (Wine passes Unix ones through):
 *   DIVALLER_BRIDGE_PORT    TCP port of the Linux bridge (default 45710)
 *   DIVALLER_BRIDGE_TARGET  module to patch (default TLAC.dva)
 *
 * Build (no CRT needed):
 *   x86_64-w64-mingw32-gcc -O2 -shared -nostdlib -mno-stack-arg-probe \
 *     -o DivallerBridge.dva divaller_bridge.c -Wl,-e,DllMain \
 *     -lkernel32 -lws2_32 -luser32
 */

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <setupapi.h>
#include <winusb.h>

/* ---------------------------------------------------------------- basics */

/* Built without a CRT, so provide what the compiler may emit calls to. */
void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    while (n--) *p++ = *q++;
    return d;
}

static int ieq_a(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
        if (!x) return 1;
    }
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int wcontains_i(const WCHAR *hay, const WCHAR *needle)
{
    if (!hay) return 0;
    for (; *hay; hay++) {
        const WCHAR *h = hay, *n = needle;
        for (; *h && *n; h++, n++) {
            WCHAR x = *h, y = *n;
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
            if (x != y) break;
        }
        if (!*n) return 1;
    }
    return 0;
}

static void blog(const char *fmt, ...)
{
    char buf[512];
    int n = wsprintfA(buf, "[DivallerBridge] ");
    va_list ap;
    va_start(ap, fmt);
    n += wvsprintfA(buf + n, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    buf[n] = 0;
    OutputDebugStringA(buf);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(out, buf, (DWORD)n, &w, NULL);
    }
}

/* ------------------------------------------------------- bridge client */

#define DEFAULT_PORT 45710
#define RETRY_MS     1000

static CRITICAL_SECTION g_lock;
static SOCKET g_sock = INVALID_SOCKET;
static DWORD g_last_fail_tick;
static int g_have_failed;
static int g_wsa_ok;
static unsigned short g_port = DEFAULT_PORT;

static void bridge_close(void)
{
    if (g_sock != INVALID_SOCKET) {
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
    }
}

static int send_all(const void *buf, int len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        int r = send(g_sock, p, len, 0);
        if (r <= 0) return 0;
        p += r; len -= r;
    }
    return 1;
}

static int recv_all(void *buf, int len)
{
    char *p = (char *)buf;
    while (len > 0) {
        int r = recv(g_sock, p, len, 0);
        if (r <= 0) return 0;
        p += r; len -= r;
    }
    return 1;
}

/* Connects if needed. Failed attempts are rate-limited, because TLAC retries
 * initialization on every tick while no Divaller is present. */
static int bridge_connect(void)
{
    if (g_sock != INVALID_SOCKET) return 1;
    if (g_have_failed && GetTickCount() - g_last_fail_tick < RETRY_MS) return 0;

    if (!g_wsa_ok) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) goto fail;
        g_wsa_ok = 1;
    }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) goto fail;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        goto fail;
    }

    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    DWORD timeout = 500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    g_sock = s;
    g_have_failed = 0;
    blog("connected to bridge on 127.0.0.1:%d", (int)g_port);
    return 1;

fail:
    if (!g_have_failed) blog("bridge not reachable on 127.0.0.1:%d (is divaller_bridge.py running?)", (int)g_port);
    g_have_failed = 1;
    g_last_fail_tick = GetTickCount();
    return 0;
}

/* Protocol (all requests start with one opcode byte):
 *   'P'                -> status(1)                      device present?
 *   'R' ep(1) max(1)   -> status(1) len(1) data[len]     latest IN packet
 *   'W' ep(1) len(1) data[len] -> status(1)              queue OUT packet
 */
static int bridge_present(void)
{
    int ok = 0;
    EnterCriticalSection(&g_lock);
    if (bridge_connect()) {
        unsigned char op = 'P', st = 0;
        if (send_all(&op, 1) && recv_all(&st, 1)) ok = st == 1;
        else bridge_close();
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

static int bridge_read(UCHAR ep, PUCHAR buf, ULONG len, PULONG transferred)
{
    int ok = 0;
    EnterCriticalSection(&g_lock);
    if (bridge_connect()) {
        unsigned char req[3] = { 'R', ep, (unsigned char)(len > 255 ? 255 : len) };
        unsigned char hdr[2];
        unsigned char data[256];
        if (send_all(req, 3) && recv_all(hdr, 2) && (hdr[1] == 0 || recv_all(data, hdr[1]))) {
            if (hdr[0] == 1) {
                ULONG n = hdr[1] < len ? hdr[1] : len;
                memcpy(buf, data, n);
                if (transferred) *transferred = n;
                ok = 1;
            }
        } else {
            bridge_close();
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

static int bridge_write(UCHAR ep, PUCHAR buf, ULONG len, PULONG transferred)
{
    int ok = 0;
    if (len > 255) len = 255;
    EnterCriticalSection(&g_lock);
    if (bridge_connect()) {
        unsigned char req[3] = { 'W', ep, (unsigned char)len };
        unsigned char st = 0;
        if (send_all(req, 3) && send_all(buf, (int)len) && recv_all(&st, 1)) {
            if (st == 1) {
                if (transferred) *transferred = len;
                ok = 1;
            }
        } else {
            bridge_close();
        }
    }
    LeaveCriticalSection(&g_lock);
    return ok;
}

/* ------------------------------------------------------- fake device */

/* {A5DCBF10-6530-11D2-901F-00C04FB951ED} */
static const GUID kUsbDeviceGuid =
    { 0xA5DCBF10, 0x6530, 0x11D2, { 0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED } };

static const WCHAR kFakePath[] =
    L"\\\\?\\usb#vid_0e8f&pid_2213#divallerbridge#{a5dcbf10-6530-11d2-901f-00c04fb951ed}";

static int g_devinfo_tag;   /* its address is the fake HDEVINFO */
static int g_iface_tag;     /* its address is the fake WINUSB_INTERFACE_HANDLE */
static HANDLE g_fake_file;  /* a real (event) handle so TLAC's CloseHandle works */

#define FAKE_DEVINFO ((HDEVINFO)&g_devinfo_tag)
#define FAKE_IFACE   ((WINUSB_INTERFACE_HANDLE)&g_iface_tag)

static int guid_eq(const GUID *a, const GUID *b)
{
    const unsigned char *x = (const unsigned char *)a, *y = (const unsigned char *)b;
    for (int i = 0; i < (int)sizeof(GUID); i++) if (x[i] != y[i]) return 0;
    return 1;
}

/* Originals, filled in while patching. */
typedef HDEVINFO (WINAPI *pfnGetClassDevsW)(const GUID *, PCWSTR, HWND, DWORD);
typedef BOOL (WINAPI *pfnEnumDeviceInterfaces)(HDEVINFO, PSP_DEVINFO_DATA, const GUID *, DWORD, PSP_DEVICE_INTERFACE_DATA);
typedef BOOL (WINAPI *pfnGetDeviceInterfaceDetailW)(HDEVINFO, PSP_DEVICE_INTERFACE_DATA, PSP_DEVICE_INTERFACE_DETAIL_DATA_W, DWORD, PDWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *pfnDestroyDeviceInfoList)(HDEVINFO);
typedef HANDLE (WINAPI *pfnCreateFileW)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
typedef BOOL (WINAPI *pfnWinUsbInitialize)(HANDLE, PWINUSB_INTERFACE_HANDLE);
typedef BOOL (WINAPI *pfnWinUsbFree)(WINUSB_INTERFACE_HANDLE);
typedef BOOL (WINAPI *pfnWinUsbQueryInterfaceSettings)(WINUSB_INTERFACE_HANDLE, UCHAR, PUSB_INTERFACE_DESCRIPTOR);
typedef BOOL (WINAPI *pfnWinUsbPipe)(WINUSB_INTERFACE_HANDLE, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);

static pfnGetClassDevsW               o_GetClassDevsW;
static pfnEnumDeviceInterfaces        o_EnumDeviceInterfaces;
static pfnGetDeviceInterfaceDetailW   o_GetDeviceInterfaceDetailW;
static pfnDestroyDeviceInfoList       o_DestroyDeviceInfoList;
static pfnCreateFileW                 o_CreateFileW;
static pfnWinUsbInitialize            o_WinUsbInitialize;
static pfnWinUsbFree                  o_WinUsbFree;
static pfnWinUsbQueryInterfaceSettings o_WinUsbQueryInterfaceSettings;
static pfnWinUsbPipe                  o_WinUsbReadPipe;
static pfnWinUsbPipe                  o_WinUsbWritePipe;

static HDEVINFO WINAPI h_GetClassDevsW(const GUID *g, PCWSTR e, HWND w, DWORD f)
{
    if (g && guid_eq(g, &kUsbDeviceGuid) && (f & DIGCF_DEVICEINTERFACE))
        return FAKE_DEVINFO;
    return o_GetClassDevsW(g, e, w, f);
}

static BOOL WINAPI h_EnumDeviceInterfaces(HDEVINFO h, PSP_DEVINFO_DATA d, const GUID *g, DWORD idx, PSP_DEVICE_INTERFACE_DATA out)
{
    if (h != FAKE_DEVINFO) return o_EnumDeviceInterfaces(h, d, g, idx, out);
    if (idx != 0 || !bridge_present()) {
        SetLastError(ERROR_NO_MORE_ITEMS);
        return FALSE;
    }
    out->InterfaceClassGuid = kUsbDeviceGuid;
    out->Flags = SPINT_ACTIVE;
    out->Reserved = 0;
    return TRUE;
}

static BOOL WINAPI h_GetDeviceInterfaceDetailW(HDEVINFO h, PSP_DEVICE_INTERFACE_DATA d,
    PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail, DWORD size, PDWORD required, PSP_DEVINFO_DATA info)
{
    if (h != FAKE_DEVINFO) return o_GetDeviceInterfaceDetailW(h, d, detail, size, required, info);
    DWORD need = (DWORD)(FIELD_OFFSET(SP_DEVICE_INTERFACE_DETAIL_DATA_W, DevicePath) + sizeof(kFakePath));
    if (required) *required = need;
    if (!detail || size < need) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    memcpy(detail->DevicePath, kFakePath, sizeof(kFakePath));
    return TRUE;
}

static BOOL WINAPI h_DestroyDeviceInfoList(HDEVINFO h)
{
    if (h == FAKE_DEVINFO) return TRUE;
    return o_DestroyDeviceInfoList(h);
}

static HANDLE WINAPI h_CreateFileW(LPCWSTR name, DWORD acc, DWORD share, LPSECURITY_ATTRIBUTES sa,
    DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (wcontains_i(name, L"#divallerbridge#")) {
        HANDLE ev = CreateEventW(NULL, TRUE, FALSE, NULL);
        g_fake_file = ev;
        return ev ? ev : INVALID_HANDLE_VALUE;
    }
    return o_CreateFileW(name, acc, share, sa, disp, flags, tmpl);
}

static BOOL WINAPI h_WinUsbInitialize(HANDLE dev, PWINUSB_INTERFACE_HANDLE out)
{
    if (dev != g_fake_file || !dev) return o_WinUsbInitialize(dev, out);
    if (!bridge_present()) {
        SetLastError(ERROR_DEVICE_NOT_CONNECTED);
        return FALSE;
    }
    *out = FAKE_IFACE;
    blog("Divaller handed to TLAC");
    return TRUE;
}

static BOOL WINAPI h_WinUsbFree(WINUSB_INTERFACE_HANDLE h)
{
    if (h == FAKE_IFACE) return TRUE;
    return o_WinUsbFree(h);
}

static BOOL WINAPI h_WinUsbQueryInterfaceSettings(WINUSB_INTERFACE_HANDLE h, UCHAR alt, PUSB_INTERFACE_DESCRIPTOR d)
{
    if (h != FAKE_IFACE) return o_WinUsbQueryInterfaceSettings(h, alt, d);
    memset(d, 0, sizeof(*d));
    d->bLength = 9;
    d->bDescriptorType = 4;      /* INTERFACE */
    d->bAlternateSetting = alt;
    d->bNumEndpoints = 2;
    d->bInterfaceClass = 0xFF;   /* vendor specific */
    return TRUE;
}

static BOOL WINAPI h_WinUsbReadPipe(WINUSB_INTERFACE_HANDLE h, UCHAR ep, PUCHAR buf, ULONG len, PULONG got, LPOVERLAPPED ov)
{
    if (h != FAKE_IFACE) return o_WinUsbReadPipe(h, ep, buf, len, got, ov);
    if (got) *got = 0;
    if (!bridge_read(ep, buf, len, got)) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI h_WinUsbWritePipe(WINUSB_INTERFACE_HANDLE h, UCHAR ep, PUCHAR buf, ULONG len, PULONG put, LPOVERLAPPED ov)
{
    if (h != FAKE_IFACE) return o_WinUsbWritePipe(h, ep, buf, len, put, ov);
    if (put) *put = 0;
    if (!bridge_write(ep, buf, len, put)) {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------- IAT patching */

struct hook {
    const char *dll;     /* import module, case-insensitive */
    const char *name;    /* imported function name */
    void *replacement;
    void **original;
};

static struct hook g_hooks[] = {
    { "setupapi.dll", "SetupDiGetClassDevsW",             (void *)h_GetClassDevsW,              (void **)&o_GetClassDevsW },
    { "setupapi.dll", "SetupDiEnumDeviceInterfaces",      (void *)h_EnumDeviceInterfaces,       (void **)&o_EnumDeviceInterfaces },
    { "setupapi.dll", "SetupDiGetDeviceInterfaceDetailW", (void *)h_GetDeviceInterfaceDetailW,  (void **)&o_GetDeviceInterfaceDetailW },
    { "setupapi.dll", "SetupDiDestroyDeviceInfoList",     (void *)h_DestroyDeviceInfoList,      (void **)&o_DestroyDeviceInfoList },
    { "kernel32.dll", "CreateFileW",                      (void *)h_CreateFileW,                (void **)&o_CreateFileW },
    { "winusb.dll",   "WinUsb_Initialize",                (void *)h_WinUsbInitialize,           (void **)&o_WinUsbInitialize },
    { "winusb.dll",   "WinUsb_Free",                      (void *)h_WinUsbFree,                 (void **)&o_WinUsbFree },
    { "winusb.dll",   "WinUsb_QueryInterfaceSettings",    (void *)h_WinUsbQueryInterfaceSettings, (void **)&o_WinUsbQueryInterfaceSettings },
    { "winusb.dll",   "WinUsb_ReadPipe",                  (void *)h_WinUsbReadPipe,             (void **)&o_WinUsbReadPipe },
    { "winusb.dll",   "WinUsb_WritePipe",                 (void *)h_WinUsbWritePipe,            (void **)&o_WinUsbWritePipe },
};
#define NHOOKS (sizeof(g_hooks) / sizeof(g_hooks[0]))

static int patch_module(HMODULE mod)
{
    unsigned char *base = (unsigned char *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;

    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; imp++) {
        const char *dll = (const char *)(base + imp->Name);
        if (!imp->OriginalFirstThunk) continue;  /* need names to match on */
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);

        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const char *fn = (const char *)((IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData))->Name;
            for (unsigned i = 0; i < NHOOKS; i++) {
                struct hook *h = &g_hooks[i];
                if (!ieq_a(dll, h->dll) || !str_eq(fn, h->name)) continue;
                DWORD old;
                if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) continue;
                *h->original = (void *)iat->u1.Function;
                iat->u1.Function = (ULONG_PTR)h->replacement;
                VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                patched++;
            }
        }
    }
    return patched;
}

/* Any hook TLAC doesn't import still needs a valid original for pass-through. */
static void fill_missing_originals(void)
{
    for (unsigned i = 0; i < NHOOKS; i++) {
        struct hook *h = &g_hooks[i];
        if (*h->original) continue;
        WCHAR wdll[32];
        int j = 0;
        for (; h->dll[j] && j < 31; j++) wdll[j] = (WCHAR)h->dll[j];
        wdll[j] = 0;
        HMODULE m = LoadLibraryW(wdll);
        if (m) *h->original = (void *)GetProcAddress(m, h->name);
    }
}

static WCHAR g_target[MAX_PATH] = L"TLAC.dva";
static volatile LONG g_patched;

static int try_patch(void)
{
    if (g_patched) return 1;
    HMODULE m = GetModuleHandleW(g_target);
    if (!m) return 0;
    if (InterlockedExchange(&g_patched, 1)) return 1;
    int n = patch_module(m);
    fill_missing_originals();
    blog("patched %d imports in TLAC", n);
    if (n == 0) blog("nothing to patch: this TLAC build may not include Divaller support");
    return 1;
}

static DWORD WINAPI patch_thread(LPVOID unused)
{
    (void)unused;
    /* PD-Loader loads plugins in directory order, so TLAC may come after us. */
    for (int i = 0; i < 600 && !try_patch(); i++) Sleep(100);
    if (!g_patched) blog("TLAC.dva never loaded; shim inactive");
    return 0;
}

static void read_config(void)
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA("DIVALLER_BRIDGE_PORT", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        unsigned v = 0;
        for (DWORD i = 0; i < n && buf[i] >= '0' && buf[i] <= '9'; i++) v = v * 10 + (buf[i] - '0');
        if (v > 0 && v < 65536) g_port = (unsigned short)v;
    }
    WCHAR t[MAX_PATH];
    n = GetEnvironmentVariableW(L"DIVALLER_BRIDGE_TARGET", t, MAX_PATH);
    if (n > 0 && n < MAX_PATH) memcpy(g_target, t, (n + 1) * sizeof(WCHAR));
}

/* PD-Loader calls this after loading the plugin. */
__declspec(dllexport) void InitializeDVA(void)
{
    try_patch();
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        InitializeCriticalSection(&g_lock);
        read_config();
        HANDLE t = CreateThread(NULL, 0, patch_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
