/* Host shims for the imports Sims2EP9.exe needs beyond The Void's set:
 * advapi32 security and user name, ws2_32 / wininet (offline: every
 * connection fails the way an unplugged machine's would), imm32 (no IME),
 * version.dll (the exe's own VS_VERSIONINFO), msvfw32 / avifil32 (in-game
 * video capture: no codec), oleaut32 / ole32 / shell32 odds and ends,
 * kernel32 resources, locale, time, console and named pipes, winmm mmio
 * (RIFF files), and steam_api (an offline Steam: interfaces are host
 * objects whose methods answer "not available"). */
#define _GNU_SOURCE
#include "host.h"
#include "zip_files.h"
#include "dircache.h"
#include "readcache.h"
#include "com.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

#define K32 "kernel32.dll"
#define ADV "advapi32.dll"
#define WS2 "ws2_32.dll"
#define INET "wininet.dll"
#define IMM "imm32.dll"
#define VER "version.dll"
#define VFW "msvfw32.dll"
#define AVI "avifil32.dll"
#define OLE "ole32.dll"
#define OLEAUT "oleaut32.dll"
#define SH "shell32.dll"
#define WINMM "winmm.dll"
#define STEAM "steam_api.dll"

/* ============================================================== advapi32 */
WINAPI_EXPORT(ADV, GetUserNameA, 2)
{
    const char *name = "Player";
    uint32_t need = (uint32_t)strlen(name) + 1, cap = MEM32(ARG(1));
    MEM32(ARG(1)) = need;
    if (cap < need) { void_set_last_error(ERROR_INSUFFICIENT_BUFFER_); return 0; }
    void_utf8_to_ga(name, ARG(0), cap);
    return 1;
}
WINAPI_EXPORT(ADV, GetUserNameW, 2)
{
    const char *name = "Player";
    uint32_t need = (uint32_t)strlen(name) + 1, cap = MEM32(ARG(1));
    MEM32(ARG(1)) = need;
    if (cap < need) { void_set_last_error(ERROR_INSUFFICIENT_BUFFER_); return 0; }
    void_utf8_to_gw(name, ARG(0), cap);
    return 1;
}
/* The security APIs: the player owns everything under C:, so every access
 * check grants what was asked for. */
WINAPI_EXPORT(ADV, InitializeAcl, 3)
{
    if (ARG(1) >= 8) { MEM8(ARG(0)) = (uint8_t)ARG(2); MEM8(ARG(0) + 1) = 0; MEM16(ARG(0) + 2) = (uint16_t)ARG(1); MEM32(ARG(0) + 4) = 0; }
    return 1;
}
WINAPI_EXPORT(ADV, AddAccessAllowedAce, 4) { return 1; }
WINAPI_EXPORT(ADV, GetLengthSid, 1) { return 8u + 4u * MEM8(ARG(0) + 1); }
WINAPI_EXPORT(ADV, FreeSid, 1) { if (ARG(0)) void_gfree(ARG(0)); return 0; }
WINAPI_EXPORT(ADV, AllocateAndInitializeSid, 11)
{
    uint32_t count = ARG(1) & 0xFFu;
    uint32_t sid = void_galloc(8u + 4u * (count ? count : 1));
    MEM8(sid) = 1;
    MEM8(sid + 1) = (uint8_t)count;
    memcpy(GPTR(sid + 2), GPTR(ARG(0)), 6);
    for (uint32_t i = 0; i < count && i < 8; ++i) MEM32(sid + 8 + 4 * i) = ARG(2 + i);
    MEM32(ARG(10)) = sid;
    return 1;
}
WINAPI_EXPORT(ADV, OpenProcessToken, 3)
{
    void_object *o = void_object_new(VOID_OBJ_OTHER, 0);
    MEM32(ARG(2)) = void_handle_new(o);
    return 1;
}
WINAPI_EXPORT(ADV, OpenThreadToken, 4) { void_set_last_error(1008u); return 0; }   /* ERROR_NO_TOKEN */
WINAPI_EXPORT(ADV, DuplicateToken, 3)
{
    void_object *o = void_object_new(VOID_OBJ_OTHER, 0);
    MEM32(ARG(2)) = void_handle_new(o);
    return 1;
}
WINAPI_EXPORT(ADV, AccessCheck, 8)
{
    if (ARG(6)) MEM32(ARG(6)) = ARG(2);     /* GrantedAccess = DesiredAccess */
    if (ARG(7)) MEM32(ARG(7)) = 1;          /* AccessStatus = TRUE */
    return 1;
}
WINAPI_EXPORT(ADV, SetSecurityDescriptorOwner, 3) { MEM32(ARG(0) + 4) = ARG(1); return 1; }
WINAPI_EXPORT(ADV, SetSecurityDescriptorGroup, 3) { MEM32(ARG(0) + 8) = ARG(1); return 1; }
WINAPI_EXPORT(ADV, IsValidSecurityDescriptor, 1) { return ARG(0) != 0; }
WINAPI_EXPORT(ADV, ConvertStringSecurityDescriptorToSecurityDescriptorW, 4)
{
    uint32_t sd = void_galloc(20);
    memset(GPTR(sd), 0, 20);
    MEM8(sd) = 1;
    MEM32(ARG(2)) = sd;
    if (ARG(3)) MEM32(ARG(3)) = 20;
    return 1;
}
WINAPI_EXPORT(ADV, BuildExplicitAccessWithNameW, 5)
{
    memset(GPTR(ARG(0)), 0, 32);
    MEM32(ARG(0)) = ARG(2);
    MEM32(ARG(0) + 4) = ARG(3);
    MEM32(ARG(0) + 8) = ARG(4);
    return 0;
}
WINAPI_EXPORT(ADV, BuildSecurityDescriptorW, 9)
{
    uint32_t sd = void_galloc(20);
    memset(GPTR(sd), 0, 20);
    MEM8(sd) = 1;
    if (ARG(7)) MEM32(ARG(7)) = 20;
    MEM32(ARG(8)) = sd;
    return ERROR_SUCCESS_;
}
/* RtlGenRandom: BOOLEAN in AL. */
WINAPI_EXPORT(ADV, SystemFunction036, 2)
{
    uint32_t buffer = ARG(0), length = ARG(1);
    uint8_t chunk[256];
    for (uint32_t done = 0; done < length;) {
        uint32_t n = length - done < sizeof chunk ? length - done : (uint32_t)sizeof chunk;
        if (getrandom(chunk, n, 0) != (ssize_t)n) for (uint32_t i = 0; i < n; ++i) chunk[i] = (uint8_t)rand();
        memcpy(GPTR(buffer + done), chunk, n);
        done += n;
    }
    return 1;
}

/* ================================================================= ws2_32 */
#define WSAENETDOWN_ 10050u
#define WSAEAFNOSUPPORT_ 10047u
#define WSANOTINITIALISED_ 10093u
#define SOCKET_ERROR_ 0xFFFFFFFFu
static uint32_t wsa_error = 0;

WINAPI_EXPORT_AS(WS2, ws2_WSAStartup, "WSAStartup", 115, 2)
{
    uint32_t data = ARG(1);
    if (data) {
        memset(GPTR(data), 0, 400);
        MEM16(data) = 0x0202;                 /* wVersion */
        MEM16(data + 2) = 0x0202;             /* wHighVersion */
        void_utf8_to_ga("WinSock 2.0", data + 4, 257);
        void_utf8_to_ga("Running", data + 261, 129);
    }
    return 0;
}
WINAPI_EXPORT_AS(WS2, ws2_WSACleanup, "WSACleanup", 116, 0) { return 0; }
WINAPI_EXPORT_AS(WS2, ws2_WSAGetLastError, "WSAGetLastError", 111, 0) { return wsa_error ? wsa_error : WSAENETDOWN_; }
WINAPI_EXPORT_AS(WS2, ws2_WSASetLastError, "WSASetLastError", 112, 1) { wsa_error = ARG(0); return 0; }
WINAPI_EXPORT_AS(WS2, ws2_socket, "socket", 23, 3) { wsa_error = WSAENETDOWN_; VLOG("[net] socket(%u, %u, %u): offline", ARG(0), ARG(1), ARG(2)); return 0xFFFFFFFFu; }
WINAPI_EXPORT_AS(WS2, ws2_closesocket, "closesocket", 3, 1) { return 0; }
WINAPI_EXPORT_AS(WS2, ws2_connect, "connect", 4, 3) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_bind, "bind", 2, 3) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_listen, "listen", 13, 2) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_accept, "accept", 1, 3) { wsa_error = WSAENETDOWN_; return 0xFFFFFFFFu; }
WINAPI_EXPORT_AS(WS2, ws2_send, "send", 19, 4) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_recv, "recv", 16, 4) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_sendto, "sendto", 20, 6) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_recvfrom, "recvfrom", 17, 6) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_shutdown, "shutdown", 22, 2) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_ioctlsocket, "ioctlsocket", 10, 3) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_setsockopt, "setsockopt", 21, 5) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_getsockopt, "getsockopt", 7, 5) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_getpeername, "getpeername", 5, 3) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_getsockname, "getsockname", 6, 3) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_WSAAsyncSelect, "WSAAsyncSelect", 101, 4) { wsa_error = WSAENETDOWN_; return SOCKET_ERROR_; }
WINAPI_EXPORT_AS(WS2, ws2_select, "select", 18, 5)
{
    /* Nothing is ever ready: clear the sets (fd_count = 0). */
    for (int i = 1; i <= 3; ++i) if (ARG(i)) MEM32(ARG(i)) = 0;
    return 0;
}
WINAPI_EXPORT_AS(WS2, ws2___WSAFDIsSet, "__WSAFDIsSet", 151, 2) { return 0; }
WINAPI_EXPORT_AS(WS2, ws2_gethostname, "gethostname", 57, 2)
{
    const char *name = "android";
    if (ARG(1) < strlen(name) + 1) { wsa_error = 10014u; return SOCKET_ERROR_; }
    void_utf8_to_ga(name, ARG(0), ARG(1));
    return 0;
}
WINAPI_EXPORT_AS(WS2, ws2_gethostbyname, "gethostbyname", 52, 1) { wsa_error = 11001u; return 0; }   /* WSAHOST_NOT_FOUND */
WINAPI_EXPORT_AS(WS2, ws2_inet_addr, "inet_addr", 11, 1)
{
    char *s = void_astr_utf8(ARG(0));
    unsigned a, b, c, d;
    uint32_t r = 0xFFFFFFFFu;
    if (s && sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
        r = a | b << 8 | c << 16 | d << 24;
    free(s);
    return r;
}
WINAPI_EXPORT_AS(WS2, ws2_inet_ntoa, "inet_ntoa", 12, 1)
{
    static uint32_t buffer;
    if (!buffer) buffer = void_galloc(16);
    uint32_t a = ARG(0);
    char text[16];
    snprintf(text, sizeof text, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
    void_utf8_to_ga(text, buffer, 16);
    return buffer;
}

/* ================================================================ wininet */
#define ERROR_INTERNET_CANNOT_CONNECT_ 12029u
WINAPI_EXPORT(INET, InternetOpenA, 5) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, InternetConnectA, 8) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, InternetCloseHandle, 1) { return 1; }
WINAPI_EXPORT(INET, HttpOpenRequestA, 8) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, HttpAddRequestHeadersA, 4) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, HttpSendRequestExA, 5) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, HttpEndRequestA, 4) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, HttpQueryInfoA, 5) { void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, InternetReadFile, 4) { if (ARG(3)) MEM32(ARG(3)) = 0; void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }
WINAPI_EXPORT(INET, InternetWriteFile, 4) { if (ARG(3)) MEM32(ARG(3)) = 0; void_set_last_error(ERROR_INTERNET_CANNOT_CONNECT_); return 0; }

/* ================================================================== imm32 */
WINAPI_EXPORT(IMM, ImmGetContext, 1) { return 0; }
WINAPI_EXPORT(IMM, ImmReleaseContext, 2) { return 1; }
WINAPI_EXPORT(IMM, ImmGetOpenStatus, 1) { return 0; }
WINAPI_EXPORT(IMM, ImmSetOpenStatus, 2) { return 1; }
WINAPI_EXPORT(IMM, ImmNotifyIME, 4) { return 0; }
WINAPI_EXPORT(IMM, ImmGetCompositionStringW, 4) { return 0; }
WINAPI_EXPORT(IMM, ImmSetCompositionStringW, 6) { return 0; }
WINAPI_EXPORT(IMM, ImmGetCandidateListW, 4) { return 0; }

/* ================================================================ version */
/* Version resources come from the exe's own RT_VERSION (ijl15.dll's for
 * that file); anything else has none. */
uint32_t void_resource_find(uint32_t module, uint32_t type, uint32_t name, uint32_t *size);

static uint32_t version_block(const char *path, uint32_t *size)
{
    uint32_t module = 0;
    const char *file = path;
    for (const char *p = path; p && *p; ++p) if (*p == '\\' || *p == '/') file = p + 1;
    if (!file || !*file || !strcasecmp(file, "Sims2EP9.exe")) module = void_module_exe_base();
    else module = void_module_handle(file);
    if (!module) return 0;
    return void_resource_find(module, 16u, 1u, size);
}

WINAPI_EXPORT(VER, GetFileVersionInfoSizeA, 2)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t size = 0;
    uint32_t block = version_block(path, &size);
    VLOG("[void] GetFileVersionInfoSize(%s) = %u", path ? path : "", block ? size : 0);
    free(path);
    if (ARG(1)) MEM32(ARG(1)) = 0;
    if (!block) { void_set_last_error(1812u); return 0; }    /* ERROR_RESOURCE_DATA_NOT_FOUND */
    return size;
}
WINAPI_EXPORT(VER, GetFileVersionInfoA, 4)
{
    char *path = void_astr_utf8(ARG(0));
    uint32_t size = 0;
    uint32_t block = version_block(path, &size);
    free(path);
    if (!block) { void_set_last_error(1812u); return 0; }
    memcpy(GPTR(ARG(3)), GPTR(block), size < ARG(2) ? size : ARG(2));
    return 1;
}

/* Walk VS_VERSIONINFO nodes: {wLength, wValueLength, wType, szKey, pad, value, pad, children}. */
static uint32_t ver_find(uint32_t node, const char *path, uint32_t *value, uint32_t *value_len)
{
    while (*path == '\\') ++path;
    if (!*path) {
        uint32_t key = node + 6, k = key;
        while (MEM16(k)) k += 2;
        uint32_t v = (k + 2 + 3) & ~3u;
        *value = v;
        *value_len = MEM16(node + 2);
        return 1;
    }
    const char *end = strchr(path, '\\');
    size_t len = end ? (size_t)(end - path) : strlen(path);
    uint32_t total = MEM16(node);
    uint32_t key = node + 6, k = key;
    while (MEM16(k)) k += 2;
    uint32_t child = ((k + 2 + 3) & ~3u);
    uint32_t vlen = MEM16(node + 2) * (MEM16(node + 4) ? 2u : 1u);
    child = (child + vlen + 3) & ~3u;
    while (child < node + total) {
        uint32_t clen = MEM16(child);
        if (!clen) break;
        char name[128];
        size_t n = 0;
        for (uint32_t p = child + 6; MEM16(p) && n < sizeof name - 1; p += 2) name[n++] = (char)MEM16(p);
        name[n] = 0;
        if (n == len && !strncasecmp(name, path, len)) return ver_find(child, path + len, value, value_len);
        child = (child + clen + 3) & ~3u;
    }
    return 0;
}

WINAPI_EXPORT(VER, VerQueryValueA, 4)
{
    char *sub = void_astr_utf8(ARG(1));
    uint32_t value = 0, value_len = 0;
    uint32_t ok = sub && ver_find(ARG(0), sub, &value, &value_len);
    if (ok && sub[0] && strncasecmp(sub, "\\VarFileInfo", 12) && strcmp(sub, "\\") && strcmp(sub, "\\\\")) {
        /* StringFileInfo values are UTF-16 in the resource; the A form
         * returns an ANSI copy. */
        char *text = void_wstr_utf8(value);
        uint32_t copy = void_galloc((uint32_t)strlen(text ? text : "") + 1);
        void_utf8_to_ga(text ? text : "", copy, (uint32_t)strlen(text ? text : "") + 1);
        value_len = (uint32_t)strlen(text ? text : "") + 1;
        value = copy;
        free(text);
    }
    VLOG("[void] VerQueryValueA(%s) = %u", sub ? sub : "", ok);
    free(sub);
    if (!ok) return 0;
    MEM32(ARG(2)) = value;
    if (ARG(3)) MEM32(ARG(3)) = value_len;
    return 1;
}

/* ======================================================== msvfw32/avifil32 */
WINAPI_EXPORT(VFW, ICOpen, 3) { return 0; }
WINAPI_EXPORT(VFW, ICClose, 1) { return 0; }
WINAPI_EXPORT(VFW, ICSendMessage, 4) { return 0xFFFFFFFEu; }       /* ICERR_UNSUPPORTED */
WINAPI_EXPORT(VFW, ICGetInfo, 3) { return 0; }
WINAPI_EXPORT(VFW, ICCompressorFree, 1) { return 0; }
WINAPI_EXPORT(VFW, ICSeqCompressFrameStart, 2) { return 0; }
WINAPI_EXPORT(VFW, ICSeqCompressFrameEnd, 1) { return 0; }
WINAPI_EXPORT(VFW, ICSeqCompressFrame, 5) { return 0; }
#define AVIERR_FILEOPEN_ 0x80044067u
WINAPI_EXPORT(AVI, AVIFileInit, 0) { return 0; }
WINAPI_EXPORT(AVI, AVIFileExit, 0) { return 0; }
WINAPI_EXPORT(AVI, AVIFileOpenA, 4) { MEM32(ARG(0)) = 0; return AVIERR_FILEOPEN_; }
WINAPI_EXPORT(AVI, AVIFileRelease, 1) { return 0; }
WINAPI_EXPORT(AVI, AVIFileCreateStreamA, 3) { MEM32(ARG(1)) = 0; return AVIERR_FILEOPEN_; }
WINAPI_EXPORT(AVI, AVIStreamSetFormat, 4) { return AVIERR_FILEOPEN_; }
WINAPI_EXPORT(AVI, AVIStreamWrite, 8) { return AVIERR_FILEOPEN_; }
WINAPI_EXPORT(AVI, AVIStreamRelease, 1) { return 0; }

/* ======================================================= ole32 / oleaut32 */
WINAPI_EXPORT(OLE, StringFromGUID2, 3)
{
    char text[40];
    void_guid_format(ARG(0), text);
    if (ARG(2) < 39) return 0;
    void_utf8_to_gw(text, ARG(1), ARG(2));
    return 39;
}
WINAPI_EXPORT(OLE, CoSetProxyBlanket, 8) { return S_OK_; }
WINAPI_EXPORT(OLE, CoInitializeSecurity, 9) { return S_OK_; }
WINAPI_EXPORT_AS(OLEAUT, SysAllocString, "SysAllocString", 2, 1)
{
    if (!ARG(0)) return 0;
    uint32_t n = (uint32_t)void_gwcslen(ARG(0));
    uint32_t block = void_galloc(4 + 2 * n + 2);
    MEM32(block) = 2 * n;
    memcpy(GPTR(block + 4), GPTR(ARG(0)), 2 * n);
    MEM16(block + 4 + 2 * n) = 0;
    return block + 4;
}
WINAPI_EXPORT_AS(OLEAUT, SysFreeString, "SysFreeString", 6, 1) { if (ARG(0)) void_gfree(ARG(0) - 4); return 0; }

/* ================================================================ shell32 */
/* An item ID list here is a private block: magic, CSIDL. */
#define PIDL_MAGIC 0x4C444950u
WINAPI_EXPORT(SH, SHGetSpecialFolderLocation, 3)
{
    uint32_t pidl = void_galloc(16);
    MEM32(pidl) = PIDL_MAGIC;
    MEM32(pidl + 4) = ARG(1);
    MEM32(pidl + 8) = 0;
    MEM32(ARG(2)) = pidl;
    return S_OK_;
}
uint32_t void_shell_folder_path(uint32_t csidl, char *out, size_t cap);
WINAPI_EXPORT(SH, SHGetPathFromIDListA, 2)
{
    uint32_t pidl = ARG(0);
    if (!pidl || MEM32(pidl) != PIDL_MAGIC) { MEM8(ARG(1)) = 0; return 0; }
    char path[1024];
    if (void_shell_folder_path(MEM32(pidl + 4), path, sizeof path)) { MEM8(ARG(1)) = 0; return 0; }
    void_utf8_to_ga(path, ARG(1), 260);
    return 1;
}
/* IMalloc over the process heap. */
static void_com_class malloc_class;
COM_METHOD(imalloc, QueryInterface, 3) { MEM32(ARG(2)) = THIS_; return S_OK_; }
COM_METHOD(imalloc, AddRef, 1) { return 2; }
COM_METHOD(imalloc, Release, 1) { return 1; }
COM_METHOD(imalloc, Alloc, 2) { return void_galloc(ARG(1)); }
COM_METHOD(imalloc, Realloc, 3) { return void_heap_realloc(void_process_heap(), ARG(1), ARG(2), 0, 0); }
COM_METHOD(imalloc, Free, 2) { if (ARG(1)) void_gfree(ARG(1)); return 0; }
COM_METHOD(imalloc, GetSize, 2) { return ARG(1) ? void_heap_size(void_process_heap(), ARG(1)) : 0xFFFFFFFFu; }
COM_METHOD(imalloc, DidAlloc, 2) { return 1; }
COM_METHOD(imalloc, HeapMinimize, 1) { return 0; }
static const void_fn malloc_methods[] = {
    imalloc_QueryInterface, imalloc_AddRef, imalloc_Release, imalloc_Alloc, imalloc_Realloc, imalloc_Free,
    imalloc_GetSize, imalloc_DidAlloc, imalloc_HeapMinimize,
};
static void_com_class malloc_class = { "IMalloc", 9, malloc_methods, 0 };
WINAPI_EXPORT(SH, SHGetMalloc, 1)
{
    static uint32_t instance;
    if (!instance) {
        void_com_object *o = (void_com_object *)calloc(1, sizeof *o);
        instance = void_com_wrap(o, &malloc_class, 0);
        o->refs = 1 << 30;
    }
    MEM32(ARG(0)) = instance;
    return S_OK_;
}
WINAPI_EXPORT(SH, ShellExecuteA, 6)
{
    char *op = void_astr_utf8(ARG(1)), *file = void_astr_utf8(ARG(2));
    void_log("[void] ShellExecute(%s, %s): not available", op ? op : "open", file ? file : "");
    free(op); free(file);
    return 2;      /* SE_ERR_FNF */
}

/* ============================================================== kernel32 */
WINAPI_EXPORT(K32, lstrcpyA, 2)
{
    size_t n = void_gstrlen(ARG(1));
    memmove(GPTR(ARG(0)), GPTR(ARG(1)), n + 1);
    return ARG(0);
}
WINAPI_EXPORT(K32, lstrcpynA, 3)
{
    uint32_t dst = ARG(0), src = ARG(1), cap = ARG(2);
    if (!cap) return dst;
    uint32_t i = 0;
    for (; i + 1 < cap && MEM8(src + i); ++i) MEM8(dst + i) = MEM8(src + i);
    MEM8(dst + i) = 0;
    return dst;
}
WINAPI_EXPORT(K32, lstrlenA, 1) { return ARG(0) ? (uint32_t)void_gstrlen(ARG(0)) : 0; }
WINAPI_EXPORT(K32, lstrcpyW, 2)
{
    size_t n = void_gwcslen(ARG(1));
    memmove(GPTR(ARG(0)), GPTR(ARG(1)), 2 * n + 2);
    return ARG(0);
}

/* Resources: type/name are integer ids (MAKEINTRESOURCE) or strings, a
 * "#123" string standing for the id. */
static int resource_matches(uint32_t root, uint32_t id, uint32_t want, const char *want_name)
{
    if (!(id & 0x80000000u)) return !want_name && id == want;
    if (!want_name) return 0;
    uint32_t s = root + (id & 0x7FFFFFFFu);
    uint32_t len = MEM16(s);
    if (strlen(want_name) != len) return 0;
    for (uint32_t i = 0; i < len; ++i)
        if (towupper(MEM16(s + 2 + 2 * i)) != (wint_t)toupper((unsigned char)want_name[i])) return 0;
    return 1;
}

static void resource_key(uint32_t va, int wide, uint32_t *id, char *name, size_t cap)
{
    name[0] = 0;
    *id = 0;
    if (va < 0x10000u) { *id = va; return; }
    char *s = wide ? void_wstr_utf8(va) : void_astr_utf8(va);
    if (s && s[0] == '#') *id = (uint32_t)strtoul(s + 1, NULL, 10);
    else snprintf(name, cap, "%s", s ? s : "");
    free(s);
}

/* The IMAGE_RESOURCE_DATA_ENTRY for (type, name), first language. */
static uint32_t resource_entry(uint32_t module, uint32_t type_va, uint32_t name_va, int wide)
{
    if (!module) module = void_module_exe_base();
    uint32_t nt = module + MEM32(module + 0x3C);
    uint32_t rva = MEM32(nt + 0x78 + 8 * 2);
    if (!rva) return 0;
    uint32_t root = module + rva;
    uint32_t ids[2];
    char names[2][128];
    resource_key(type_va, wide, &ids[0], names[0], sizeof names[0]);
    resource_key(name_va, wide, &ids[1], names[1], sizeof names[1]);
    uint32_t dir = root;
    for (int level = 0; level < 3; ++level) {
        uint32_t count = MEM16(dir + 12) + MEM16(dir + 14), entry = dir + 16, found = 0;
        for (uint32_t i = 0; i < count; ++i, entry += 8) {
            uint32_t id = MEM32(entry);
            if (level == 2 || resource_matches(root, id, ids[level], names[level][0] ? names[level] : NULL)) {
                found = MEM32(entry + 4);
                break;
            }
        }
        if (!found) return 0;
        if (found & 0x80000000u) dir = root + (found & 0x7FFFFFFFu);
        else return root + found;
    }
    return 0;
}
static uint32_t find_resource(int wide)
{
    uint32_t entry = resource_entry(ARG(0), ARG(2), ARG(1), wide);
    if (!entry) void_set_last_error(1814u);   /* ERROR_RESOURCE_NAME_NOT_FOUND */
    return entry;
}
WINAPI_EXPORT(K32, FindResourceA, 3) { return find_resource(0); }
WINAPI_EXPORT(K32, FindResourceW, 3) { return find_resource(1); }
WINAPI_EXPORT(K32, LoadResource, 2)
{
    uint32_t module = ARG(0) ? ARG(0) : void_module_exe_base();
    return ARG(1) ? module + MEM32(ARG(1)) : 0;
}
WINAPI_EXPORT(K32, LockResource, 1) { return ARG(0); }
WINAPI_EXPORT(K32, SizeofResource, 2) { return ARG(1) ? MEM32(ARG(1) + 4) : 0; }
WINAPI_EXPORT(K32, FreeResource, 1) { return 0; }
/* EnumResourceNamesA(module, type, callback, param): BOOL CALLBACK(HMODULE, LPCSTR type, LPSTR name, LONG_PTR). */
WINAPI_EXPORT(K32, EnumResourceNamesA, 4)
{
    uint32_t module = ARG(0) ? ARG(0) : void_module_exe_base(), type_va = ARG(1), callback = ARG(2), param = ARG(3);
    uint32_t nt = module + MEM32(module + 0x3C);
    uint32_t rva = MEM32(nt + 0x78 + 8 * 2);
    if (!rva) return 0;
    uint32_t root = module + rva, type_id;
    char type_name[128];
    resource_key(type_va, 0, &type_id, type_name, sizeof type_name);
    uint32_t count = MEM16(root + 12) + MEM16(root + 14), entry = root + 16, names = 0;
    for (uint32_t i = 0; i < count; ++i, entry += 8)
        if (resource_matches(root, MEM32(entry), type_id, type_name[0] ? type_name : NULL)) { names = MEM32(entry + 4); break; }
    if (!(names & 0x80000000u)) { void_set_last_error(1813u); return 0; }   /* ERROR_RESOURCE_TYPE_NOT_FOUND */
    uint32_t dir = root + (names & 0x7FFFFFFFu);
    count = MEM16(dir + 12) + MEM16(dir + 14);
    entry = dir + 16;
    for (uint32_t i = 0; i < count; ++i, entry += 8) {
        uint32_t id = MEM32(entry), name = id;
        if (id & 0x80000000u) {
            uint32_t s = root + (id & 0x7FFFFFFFu), len = MEM16(s);
            name = void_galloc(len + 1);
            for (uint32_t k = 0; k < len; ++k) MEM8(name + k) = (uint8_t)MEM16(s + 2 + 2 * k);
            MEM8(name + len) = 0;
        }
        uint32_t args[4] = { module, type_va, name, param };
        uint32_t keep = void_call_guest(callback, 4, args, 1);
        if (!keep) break;
    }
    return 1;
}

WINAPI_EXPORT(K32, SetThreadExecutionState, 1) { return 0x80000000u; }
WINAPI_EXPORT(K32, SetPriorityClass, 2) { return 1; }
WINAPI_EXPORT(K32, GetPriorityClass, 1) { return 0x20u; }   /* NORMAL_PRIORITY_CLASS */
WINAPI_EXPORT(K32, SuspendThread, 1)
{
    void_log("[void] SuspendThread(%08X) from %08X: not supported, reported as done", ARG(0), g_cur_func);
    return 0;
}

/* ULONGLONG VerSetConditionMask(ULONGLONG mask, DWORD type, BYTE condition): 3 bits per type bit. */
WINAPI_EXPORT(K32, VerSetConditionMask, 4)
{
    uint64_t mask = (uint64_t)ARG(0) | ((uint64_t)ARG(1) << 32);
    uint32_t type = ARG(2), condition = ARG(3) & 7u;
    for (int bit = 0; bit < 8; ++bit)
        if (type & (1u << bit)) mask = (mask & ~(7ull << (3 * bit))) | ((uint64_t)condition << (3 * bit));
    g_edx = (uint32_t)(mask >> 32);
    return (uint32_t)mask;
}
/* VerifyVersionInfoW against Windows 10.0.19045 (SP 0). */
static int version_compare(uint64_t have, uint64_t want, uint32_t condition)
{
    switch (condition) {
    case 1: return have == want;
    case 2: return have > want;
    case 3: return have >= want;
    case 4: return have < want;
    case 5: return have <= want;
    default: return 1;
    }
}
WINAPI_EXPORT(K32, VerifyVersionInfoW, 4)
{
    uint32_t info = ARG(0), type = ARG(1);
    uint64_t mask = (uint64_t)ARG(2) | ((uint64_t)ARG(3) << 32);
    uint32_t major = MEM32(info + 4), minor = MEM32(info + 8), build = MEM32(info + 12);
    uint32_t sp_major = MEM16(info + 4 + 4 * 4 + 256), sp_minor = MEM16(info + 4 + 4 * 4 + 256 + 2);
    int ok = 1;
    /* Major, minor and service pack compare as one version when several are asked for. */
    if (type & 3u) {
        uint32_t c = (uint32_t)((mask >> ((type & 2u) ? 3 : 0)) & 7u);
        uint64_t have = (10ull << 32) | 0u, want = ((uint64_t)major << 32) | minor;
        if (!(type & 2u)) { have = 0; want = minor; }
        ok &= version_compare(have, want, c);
    }
    if (type & 4u) ok &= version_compare(19045, build, (uint32_t)((mask >> 6) & 7u));
    if (type & 0x30u) ok &= version_compare(0, ((uint64_t)sp_major << 16) | sp_minor, (uint32_t)((mask >> 15) & 7u));
    if (!ok) void_set_last_error(1150u);   /* ERROR_OLD_WIN_VERSION */
    return (uint32_t)ok;
}

/* MEMORYSTATUSEX: a 4 GiB phone with 3 GiB free, a 4 GiB LAA address space. */
WINAPI_EXPORT(K32, GlobalMemoryStatusEx, 1)
{
    uint32_t out = ARG(0);
    uint64_t total = 4ull << 30, avail = 3ull << 30;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long kb;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kb) == 1) total = kb * 1024ull;
            else if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) avail = kb * 1024ull;
        }
        fclose(f);
    }
    MEM32(out) = 64;
    MEM32(out + 4) = total ? (uint32_t)(100u - (uint32_t)(avail * 100u / total)) : 0;
    MEM64(out + 8) = total;
    MEM64(out + 16) = avail;
    MEM64(out + 24) = total * 2;
    MEM64(out + 32) = avail * 2;
    MEM64(out + 40) = 0xFFFE0000ull;
    MEM64(out + 48) = 0xC0000000ull;
    MEM64(out + 56) = 0;
    return 1;
}

WINAPI_EXPORT(K32, InitializeSListHead, 1) { MEM32(ARG(0)) = 0; MEM32(ARG(0) + 4) = 0; return 0; }

/* GetModuleHandleExW(flags, name or address, HMODULE *). */
WINAPI_EXPORT(K32, GetModuleHandleExW, 3)
{
    uint32_t flags = ARG(0), out = ARG(2), h = 0;
    if (flags & 4u) {                      /* GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS */
        uint32_t exe = void_module_exe_base();
        if (ARG(1) >= exe && ARG(1) < exe + 0x0120F000u) h = exe;
        else if (ARG(1) >= 0x60000000u && ARG(1) < 0x60058000u) h = 0x60000000u;
    } else if (!ARG(1)) {
        h = void_module_exe_base();
    } else {
        char *name = void_wstr_utf8(ARG(1));
        h = void_module_handle(name);
        free(name);
    }
    if (out) MEM32(out) = h;
    if (!h) void_set_last_error(ERROR_MOD_NOT_FOUND_);
    return h != 0;
}
WINAPI_EXPORT(K32, GetModuleHandleExA, 3)
{
    uint32_t flags = ARG(0), out = ARG(2), h = 0;
    if (flags & 4u) h = void_module_exe_base();
    else if (!ARG(1)) h = void_module_exe_base();
    else { char *name = void_astr_utf8(ARG(1)); h = void_module_handle(name); free(name); }
    if (out) MEM32(out) = h;
    if (!h) void_set_last_error(ERROR_MOD_NOT_FOUND_);
    return h != 0;
}

/* Named pipes (crashpad's client): there is no handler process to talk to. */
WINAPI_EXPORT(K32, CreateNamedPipeW, 8) { void_set_last_error(ERROR_ACCESS_DENIED_); return WIN_INVALID_HANDLE; }
WINAPI_EXPORT(K32, WaitNamedPipeW, 2) { void_set_last_error(ERROR_FILE_NOT_FOUND_); return 0; }
WINAPI_EXPORT(K32, SetNamedPipeHandleState, 4) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(K32, TransactNamedPipe, 7) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(K32, PeekNamedPipe, 6) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(K32, OpenEventA, 3) { void_set_last_error(ERROR_FILE_NOT_FOUND_); return 0; }
WINAPI_EXPORT(K32, OpenEventW, 3) { void_set_last_error(ERROR_FILE_NOT_FOUND_); return 0; }

/* Console input: the game has no console. */
WINAPI_EXPORT(K32, ReadConsoleW, 5) { if (ARG(3)) MEM32(ARG(3)) = 0; void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(K32, ReadConsoleInputW, 4) { if (ARG(3)) MEM32(ARG(3)) = 0; void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(K32, FlushConsoleInputBuffer, 1) { return 1; }
WINAPI_EXPORT(K32, SetConsoleMode, 2) { void_set_last_error(ERROR_INVALID_HANDLE_); return 0; }
WINAPI_EXPORT(K32, SetConsoleCtrlHandler, 2) { return 1; }

/* SYSTEMTIME <-> FILETIME, and the local zone through the host's tz. */
static uint64_t systemtime_to_ft(uint32_t st)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = MEM16(st) - 1900;
    tm.tm_mon = MEM16(st + 2) - 1;
    tm.tm_mday = MEM16(st + 6);
    tm.tm_hour = MEM16(st + 8);
    tm.tm_min = MEM16(st + 10);
    tm.tm_sec = MEM16(st + 12);
    time_t t = timegm(&tm);
    return (uint64_t)((int64_t)t * 10000000ll + 116444736000000000ll) + (uint64_t)MEM16(st + 14) * 10000u;
}
static void ft_to_systemtime(uint64_t ft, uint32_t st)
{
    int64_t ticks = (int64_t)(ft - 116444736000000000ull);
    time_t t = (time_t)(ticks / 10000000ll);
    struct tm tm;
    gmtime_r(&t, &tm);
    MEM16(st) = (uint16_t)(tm.tm_year + 1900); MEM16(st + 2) = (uint16_t)(tm.tm_mon + 1);
    MEM16(st + 4) = (uint16_t)tm.tm_wday; MEM16(st + 6) = (uint16_t)tm.tm_mday;
    MEM16(st + 8) = (uint16_t)tm.tm_hour; MEM16(st + 10) = (uint16_t)tm.tm_min;
    MEM16(st + 12) = (uint16_t)tm.tm_sec; MEM16(st + 14) = (uint16_t)((ticks / 10000ll) % 1000);
}
static int64_t utc_offset_seconds(time_t at)
{
    struct tm local;
    localtime_r(&at, &local);
    return (int64_t)local.tm_gmtoff;
}
WINAPI_EXPORT(K32, SystemTimeToFileTime, 2)
{
    MEM64(ARG(1)) = systemtime_to_ft(ARG(0));
    return 1;
}
WINAPI_EXPORT(K32, SystemTimeToTzSpecificLocalTime, 3)
{
    uint64_t ft = systemtime_to_ft(ARG(1));
    time_t t = (time_t)((int64_t)(ft - 116444736000000000ull) / 10000000ll);
    ft_to_systemtime(ft + (uint64_t)(utc_offset_seconds(t) * 10000000ll), ARG(2));
    return 1;
}
WINAPI_EXPORT(K32, TzSpecificLocalTimeToSystemTime, 3)
{
    uint64_t ft = systemtime_to_ft(ARG(1));
    time_t t = (time_t)((int64_t)(ft - 116444736000000000ull) / 10000000ll);
    ft_to_systemtime(ft - (uint64_t)(utc_offset_seconds(t) * 10000000ll), ARG(2));
    return 1;
}
WINAPI_EXPORT(K32, GetProcessTimes, 5)
{
    struct timespec cpu;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu);
    uint64_t user = (uint64_t)cpu.tv_sec * 10000000ull + (uint64_t)cpu.tv_nsec / 100u;
    if (ARG(1)) MEM64(ARG(1)) = void_now_100ns() - user;
    if (ARG(2)) MEM64(ARG(2)) = 0;
    if (ARG(3)) MEM64(ARG(3)) = 0;
    if (ARG(4)) MEM64(ARG(4)) = user;
    return 1;
}

/* EnumSystemLocalesW(callback, flags): BOOL CALLBACK(LPWSTR "00000409"). */
WINAPI_EXPORT(K32, EnumSystemLocalesW, 2)
{
    static uint32_t text;
    if (!text) { text = void_galloc(32); void_utf8_to_gw("00000409", text, 16); }
    uint32_t args[1] = { text };
    void_call_guest(ARG(0), 1, args, 1);
    return 1;
}

/* LCMapStringEx(locale name, flags, src, n, dst, cap, version, reserved, sort handle). */
WINAPI_EXPORT(K32, LCMapStringEx, 9)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4);
    int32_t n = (int32_t)ARG(3), cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)void_gwcslen(src) + 1;
    if (flags & 0x400u) {                 /* LCMAP_SORTKEY */
        int32_t need = n * 2 + 1;
        if (!cap) return (uint32_t)need;
        if (cap < need) { void_set_last_error(ERROR_INSUFFICIENT_BUFFER_); return 0; }
        for (int32_t i = 0; i < n; ++i) {
            uint32_t c = (uint32_t)towupper((wint_t)MEM16(src + 2 * (uint32_t)i));
            MEM8(dst + 2 * (uint32_t)i) = (uint8_t)(c >> 8);
            MEM8(dst + 2 * (uint32_t)i + 1) = (uint8_t)c;
        }
        MEM8(dst + 2 * (uint32_t)n) = 0;
        return (uint32_t)need;
    }
    if (!cap) return (uint32_t)n;
    if (cap < n) { void_set_last_error(ERROR_INSUFFICIENT_BUFFER_); return 0; }
    for (int32_t i = 0; i < n; ++i) {
        uint32_t c = MEM16(src + 2 * (uint32_t)i);
        if (flags & 0x100u) c = (uint32_t)towlower((wint_t)c);
        else if (flags & 0x200u) c = (uint32_t)towupper((wint_t)c);
        MEM16(dst + 2 * (uint32_t)i) = (uint16_t)c;
    }
    return (uint32_t)n;
}

/* ================================================================== winmm */
/* mmio over host files, enough for RIFF readers: open/read/seek/close and
 * chunk descent. Buffered I/O (GetInfo/Advance/SetInfo) reads through a
 * private buffer the MMIOINFO points at. */
typedef struct { int fd; uint32_t info; uint32_t buffer; uint32_t buffer_size; } mmio_t;
static mmio_t *mmio_get(uint32_t h)
{
    void_object *o = void_handle_get(h, VOID_OBJ_OTHER);
    return o && o->data ? (mmio_t *)o->data : NULL;
}
static void mmio_destroy(void_object *o)
{
    mmio_t *m = (mmio_t *)o->data;
    if (m->fd >= 0) close(m->fd);
    if (m->buffer) void_gfree(m->buffer);
    if (m->info) void_gfree(m->info);
}
#define MMIO_READ_ 0x0000u
#define MMIO_WRITE_ 0x0001u
#define MMIO_READWRITE_ 0x0002u
#define MMIO_CREATE_ 0x1000u
#define MMIO_EXIST_ 0x4000u
#define MMIO_FINDCHUNK_ 0x0010u
#define MMIO_FINDRIFF_ 0x0020u
#define MMIO_FINDLIST_ 0x0040u
#define MMIOERR_CHUNKNOTFOUND_ 265u
WINAPI_EXPORT(WINMM, mmioOpenA, 3)
{
    char *name = void_astr_utf8(ARG(0));
    uint32_t flags = ARG(2), info = ARG(1);
    char *host = name ? void_path_to_host(name) : NULL;
    int mode = (flags & MMIO_READWRITE_) ? O_RDWR : (flags & MMIO_WRITE_) ? O_WRONLY : O_RDONLY;
    if (flags & MMIO_CREATE_) mode |= O_CREAT | O_TRUNC;
    if (host) void_readcache_flush_path(host);
    int fd = host ? open(host, mode, 0664) : -1;
    const zm_entry *archived = fd < 0 && host && mode == O_RDONLY ? s2_zip_find(host) : NULL;
    if (archived) fd = s2_zip_memory_fd(archived);
    if (fd >= 0 && (mode & O_CREAT)) dircache_created(host, 0);
    if (fd >= 0 && (mode & O_ACCMODE) != O_RDONLY) void_readcache_changed(host);
    VLOG("[void] mmioOpen(%s, %08X) = %d", name ? name : "", flags, fd);
    free(name);
    free(host);
    if (fd < 0) { if (info) MEM32(info + 12) = 257u; return 0; }   /* wErrorRet = MMIOERR_CANNOTOPEN */
    if (flags & MMIO_EXIST_) { close(fd); return 1; }
    void_object *o = void_object_new(VOID_OBJ_OTHER, sizeof(mmio_t));
    mmio_t *m = (mmio_t *)o->data;
    m->fd = fd;
    o->destroy = mmio_destroy;
    return void_handle_new(o);
}
WINAPI_EXPORT(WINMM, mmioClose, 2) { return void_handle_close(ARG(0)) ? 0 : 258u; }
WINAPI_EXPORT(WINMM, mmioRead, 3)
{
    mmio_t *m = mmio_get(ARG(0));
    if (!m) return 0xFFFFFFFFu;
    ssize_t n = read(m->fd, GPTR(ARG(1)), ARG(2));
    return n < 0 ? 0xFFFFFFFFu : (uint32_t)n;
}
WINAPI_EXPORT(WINMM, mmioSeek, 3)
{
    mmio_t *m = mmio_get(ARG(0));
    if (!m) return 0xFFFFFFFFu;
    off_t r = lseek(m->fd, (off_t)(int32_t)ARG(1), ARG(2) == 0 ? SEEK_SET : ARG(2) == 1 ? SEEK_CUR : SEEK_END);
    return r < 0 ? 0xFFFFFFFFu : (uint32_t)r;
}
/* MMCKINFO: ckid, cksize, fccType, dwDataOffset, dwFlags. */
WINAPI_EXPORT(WINMM, mmioDescend, 4)
{
    mmio_t *m = mmio_get(ARG(0));
    uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!m) return 258u;
    uint32_t want = MEM32(ck), want_type = MEM32(ck + 8);
    off_t end = parent ? (off_t)(MEM32(parent + 12) + MEM32(parent + 4)) : (off_t)0x7FFFFFFF;
    for (;;) {
        off_t at = lseek(m->fd, 0, SEEK_CUR);
        if (at + 8 > end) return MMIOERR_CHUNKNOTFOUND_;
        uint32_t head[3] = {0, 0, 0};
        if (read(m->fd, head, 8) != 8) return MMIOERR_CHUNKNOTFOUND_;
        uint32_t id = head[0], size = head[1], type = 0;
        int list = id == 0x46464952u || id == 0x5453494Cu;   /* "RIFF", "LIST" */
        if (list) { if (read(m->fd, &type, 4) != 4) return MMIOERR_CHUNKNOTFOUND_; }
        int match = 1;
        if (flags & MMIO_FINDRIFF_) match = id == 0x46464952u && (!want_type || type == want_type);
        else if (flags & MMIO_FINDLIST_) match = id == 0x5453494Cu && (!want_type || type == want_type);
        else if (flags & MMIO_FINDCHUNK_) match = id == want;
        if (match) {
            MEM32(ck) = id;
            MEM32(ck + 4) = size;
            MEM32(ck + 8) = type;
            MEM32(ck + 12) = (uint32_t)(at + 8);
            MEM32(ck + 16) = 0;
            if (!list) lseek(m->fd, at + 8, SEEK_SET);
            return 0;
        }
        lseek(m->fd, at + 8 + (off_t)((size + 1u) & ~1u), SEEK_SET);
    }
}
WINAPI_EXPORT(WINMM, mmioAscend, 3)
{
    mmio_t *m = mmio_get(ARG(0));
    if (!m) return 258u;
    uint32_t ck = ARG(1);
    lseek(m->fd, (off_t)(MEM32(ck + 12) + ((MEM32(ck + 4) + 1u) & ~1u)), SEEK_SET);
    return 0;
}
/* MMIOINFO (72 bytes): pchBuffer +16, pchNext +20, pchEndRead +24, pchEndWrite +28, lBufOffset +32, lDiskOffset +36. */
WINAPI_EXPORT(WINMM, mmioGetInfo, 3)
{
    mmio_t *m = mmio_get(ARG(0));
    if (!m) return 258u;
    if (!m->buffer) { m->buffer_size = 0x2000; m->buffer = void_galloc(m->buffer_size); }
    off_t at = lseek(m->fd, 0, SEEK_CUR);
    uint32_t info = ARG(1);
    memset(GPTR(info), 0, 72);
    MEM32(info + 12) = m->buffer_size;           /* cchBuffer */
    MEM32(info + 16) = m->buffer;
    MEM32(info + 20) = m->buffer;
    MEM32(info + 24) = m->buffer;                /* empty: the first Advance fills it */
    MEM32(info + 28) = m->buffer + m->buffer_size;
    MEM32(info + 32) = (uint32_t)at;
    MEM32(info + 36) = (uint32_t)at;
    MEM32(info + 64) = ARG(0);                   /* hmmio */
    return 0;
}
WINAPI_EXPORT(WINMM, mmioAdvance, 3)
{
    mmio_t *m = mmio_get(ARG(0));
    uint32_t info = ARG(1);
    if (!m || !info) return 258u;
    /* Consume what the caller read, then refill from the file position after it. */
    uint32_t consumed = MEM32(info + 20) - MEM32(info + 16);
    off_t at = (off_t)MEM32(info + 32) + consumed;
    lseek(m->fd, at, SEEK_SET);
    ssize_t n = read(m->fd, GPTR(m->buffer), m->buffer_size);
    if (n < 0) n = 0;
    MEM32(info + 20) = m->buffer;
    MEM32(info + 24) = m->buffer + (uint32_t)n;
    MEM32(info + 32) = (uint32_t)at;
    MEM32(info + 36) = (uint32_t)(at + n);
    return 0;
}
WINAPI_EXPORT(WINMM, mmioSetInfo, 3)
{
    mmio_t *m = mmio_get(ARG(0));
    uint32_t info = ARG(1);
    if (!m || !info) return 258u;
    /* The file position follows what the caller consumed. */
    uint32_t consumed = MEM32(info + 20) - MEM32(info + 16);
    lseek(m->fd, (off_t)MEM32(info + 32) + consumed, SEEK_SET);
    return 0;
}

/* ============================================================== steam_api */
/* An offline Steam. Sims2EP9.exe uses two interfaces (found by scanning its
 * code: tools notes in STATE.md): SteamApps008 slot 4 GetCurrentGameLanguage
 * (0xA0C29ABC, language selection) and SteamUser023 slot 2 GetSteamID
 * (0xA0C2B932, sret pointer on the stack). Every other slot traps with its
 * name, so a new use shows up instead of unbalancing the guest stack. */
/* IDs from the original 3314070_install.vdf; Steam and registry must agree. */
static const struct { const char *name; uint32_t id; } game_languages[] = {
    {"english", 1}, {"french", 2}, {"german", 3}, {"italian", 4},
    {"spanish", 5}, {"swedish", 6}, {"finnish", 7}, {"dutch", 8},
    {"danish", 9}, {"brazilian", 10}, {"czech", 11}, {"japanese", 14},
    {"koreana", 15}, {"russian", 16}, {"schinese", 17}, {"tchinese", 18},
    {"polish", 20}, {"thai", 21}, {"norwegian", 22}, {"portuguese", 23},
    {"hungarian", 25}
};
static int game_language;

static int language_choice(const char *text)
{
    if (!text || !*text) return -1;
    char *end;
    unsigned long id = strtoul(text, &end, 10);
    for (size_t i = 0; i < sizeof game_languages / sizeof game_languages[0]; ++i)
        if (!strcasecmp(text, game_languages[i].name) || (isdigit((unsigned char)*text) && !*end && id == game_languages[i].id)) return (int)i;
    return -1;
}

uint32_t s2_language_init(const char *data_dir)
{
    char path[1200], text[64] = "";
    snprintf(path, sizeof path, "%s/language.txt", data_dir);
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(text, 1, sizeof text - 1, f);
        text[n] = 0;
        fclose(f);
        while (n && isspace((unsigned char)text[n - 1])) text[--n] = 0;
    }
    const char *selected = getenv("S2_STEAM_LANGUAGE");
    if (!selected || !*selected) selected = getenv("S2_LANGUAGE");
    if (!selected || !*selected) selected = getenv("PORT_OPTION_GAME_LANGUAGE");
    if (selected && !strcmp(selected, "system")) selected = getenv("S2_SYSTEM_LANGUAGE");
    if (!selected || !*selected) selected = text;
    int choice = language_choice(selected);
    if (choice < 0 && selected && *selected) void_log("[language] unsupported selection '%s', using english", selected);
    game_language = choice < 0 ? 0 : choice;
    void_log("[language] %s (registry %u)", game_languages[game_language].name, game_languages[game_language].id);
    return game_languages[game_language].id;
}

static const char *steam_language(void) { return game_languages[game_language].name; }

void sub_A01EC080_lifted(void);
void sub_A01EC080(void)
{
    uint32_t self = g_ecx, out = MEM32(g_esp + 4), index = MEM32(g_esp + 8);
    uint8_t language = MEM8(g_esp + 12);
    sub_A01EC080_lifted();
    if (!(g_eax & 0xFF) || index != 2 || (language != 13 && language != 255) ||
        game_languages[game_language].id != 16 || MEM32(self + 0x0C) != 0x43545353u ||
        !void_vm_readable(out, 28) || MEM32(out) != 0xA0D9B470u) return;
    uint32_t length = MEM32(out + 0x14);
    uint32_t data = MEM32(out + 0x18) > 15 ? MEM32(out + 4) : out + 4;
    if (!length || length > 512 || !void_vm_readable(data, length)) return;
    uint64_t hash = 14695981039346656037ull;
    for (uint32_t i = 0; i < length; ++i) hash = (hash ^ MEM8(data + i)) * 1099511628211ull;
    static const struct { uint64_t hash; const char *text; } descriptions[] = {
#include "sims2_university_ru.inc"
    };
    for (size_t i = 0; i < sizeof descriptions / sizeof descriptions[0]; ++i) {
        if (descriptions[i].hash != hash) continue;
        const char *translation = descriptions[i].text;
        uint32_t text = void_gstrdup(translation);
        if (text) {
            uint32_t args[] = { text, (uint32_t)strlen(translation) };
            void_call_guest_this(0xA01ACFBEu, out, 2, args);
            void_gfree(text);
        }
        return;
    }
}

/* Android commits UTF-8 bytes; the ANSI window's keyboard code page would
 * reinterpret them. The original handler already has a UTF-8 branch (8). */
void sub_A0156260_lifted(void);
void sub_A0156260(void)
{
    uint32_t self = g_ecx, encoding = MEM32(self + 0xB4);
    MEM32(self + 0xB4) = 8;
    sub_A0156260_lifted();
    MEM32(self + 0xB4) = encoding;
}

static void steam_unknown(void)
{
    uint32_t va = g_icall_trace[(g_icall_trace_idx - 1) & (ICALL_TRACE_SIZE - 1)];
    const char *name = void_thunk_name(va);
    void_log("[steam] unimplemented %s called from %08X", name ? name : "?", g_cur_func);
    void_lift_trap(va, "unimplemented Steam interface method");
}

/* ISteamApps::GetCurrentGameLanguage(): thiscall, no stack arguments. */
static void steam_apps_language(void)
{
    static uint32_t text;
    if (!text) text = void_gstrdup(steam_language());
    g_eax = text;
    g_esp += 4u;
}
/* ISteamApps::BIsSubscribed / BIsDlcInstalled(AppId) and friends: true. */
static void steam_apps_true0(void) { g_eax = 1; g_esp += 4u; }
static void steam_apps_true1(void) { g_eax = 1; g_esp += 8u; }
/* ISteamUser::GetSteamID(): the CSteamID comes back through a hidden pointer. */
static void steam_user_id(void)
{
    uint32_t out = MEM32(g_esp + 4);
    MEM32(out) = 0x00000001u;                /* account id */
    MEM32(out + 4) = 0x01100001u;            /* individual, public universe */
    g_eax = out;
    g_esp += 8u;
}
static void steam_user_logged_on(void) { g_eax = 0; g_esp += 4u; }
static void steam_user_hsteamuser(void) { g_eax = 1; g_esp += 4u; }

#define STEAM_SLOTS 40
static uint32_t steam_object(const char *name, const void_fn *known, int count)
{
    uint32_t table = void_galloc(4 * STEAM_SLOTS);
    for (int i = 0; i < STEAM_SLOTS; ++i) {
        char *label = (char *)malloc(64);
        snprintf(label, 64, "%s#%d", name, i);
        MEM32(table + 4u * (uint32_t)i) = void_thunk(i < count && known[i] ? known[i] : steam_unknown, label);
    }
    uint32_t object = void_galloc(16);
    MEM32(object) = table;
    return object;
}

static uint32_t steam_interface(const char *version)
{
    static uint32_t apps, user;
    if (!strcmp(version, "STEAMAPPS_INTERFACE_VERSION008")) {
        /* 0 BIsSubscribed, 4 GetCurrentGameLanguage, 6 BIsSubscribedApp, 7 BIsDlcInstalled */
        static const void_fn methods[8] = { steam_apps_true0, NULL, NULL, NULL, steam_apps_language, NULL,
                                            steam_apps_true1, steam_apps_true1 };
        if (!apps) apps = steam_object("ISteamApps", methods, 8);
        return apps;
    }
    if (!strcmp(version, "SteamUser023")) {
        /* 0 GetHSteamUser, 1 BLoggedOn, 2 GetSteamID */
        static const void_fn methods[3] = { steam_user_hsteamuser, steam_user_logged_on, steam_user_id };
        if (!user) user = steam_object("ISteamUser", methods, 3);
        return user;
    }
    void_log("[steam] interface %s is not provided", version);
    return 0;
}

/* ESteamAPIInitResult SteamInternal_SteamAPI_Init(const char *versions, SteamErrMsg *err): 0 = OK. */
CDECL_EXPORT(STEAM, SteamInternal_SteamAPI_Init)
{
    void_log("[steam] SteamAPI_Init: offline, language %s", steam_language());
    if (ARG(1)) MEM8(ARG(1)) = 0;
    return 0;
}
CDECL_EXPORT(STEAM, SteamAPI_Shutdown) { return 0; }
CDECL_EXPORT(STEAM, SteamAPI_RunCallbacks) { return 0; }
/* The game registers GameOverlayActivated_t (331); no overlay ever opens. */
CDECL_EXPORT(STEAM, SteamAPI_RegisterCallback) { return 0; }
CDECL_EXPORT(STEAM, SteamAPI_UnregisterCallback) { return 0; }
CDECL_EXPORT(STEAM, SteamAPI_GetHSteamUser) { return 1; }
CDECL_EXPORT(STEAM, SteamAPI_GetHSteamPipe) { return 1; }
CDECL_EXPORT(STEAM, SteamInternal_FindOrCreateUserInterface)
{
    char *version = void_astr_utf8(ARG(1));
    uint32_t r = steam_interface(version ? version : "?");
    free(version);
    return r;
}
/* void *SteamInternal_ContextInit(void *data): data = { void (*init)(void *slot); counter; slot }.
 * The first call runs init(&slot) (cdecl), which stores the interface. */
CDECL_EXPORT(STEAM, SteamInternal_ContextInit)
{
    uint32_t data = ARG(0);
    if (MEM32(data + 4) != 1) {
        MEM32(data + 4) = 1;
        uint32_t args[1] = { data + 8 };
        void_call_guest(MEM32(data), 1, args, 0);
    }
    return data + 8;
}
