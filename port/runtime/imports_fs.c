// nn::fs on Win32: files, directories and mounts. File and directory handles are host pointers/handles stored in the
// guest's 8-byte handle structs.
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"

// Paths: lifted il2cpp prefixes "rom:/" to anything without "<mount>:/"; the engine hands it Windows paths. Other
// mount names (save data, temporary storage) map to host directories registered with rt_fs_mount.
typedef struct { char name[16]; char dir[600]; } Mount;
#define MOUNT_MAX 64
static Mount g_mounts[MOUNT_MAX];
static SRWLOCK g_mount_lock = SRWLOCK_INIT;

void rt_fs_mount(const char *name, const char *host_dir) {   // host_dir with a trailing backslash; created if missing
    wchar_t w[1024];
    MultiByteToWideChar(CP_UTF8, 0, host_dir, -1, w, 1024);
    CreateDirectoryW(w, NULL);
    AcquireSRWLockExclusive(&g_mount_lock);
    Mount *m = NULL;
    for (int i = 0; i < MOUNT_MAX; i++) if (!strcmp(g_mounts[i].name, name)) m = &g_mounts[i];
    for (int i = 0; i < MOUNT_MAX && !m; i++) if (!g_mounts[i].name[0]) m = &g_mounts[i];
    if (!m) rt_fatal("fs: more than %d mounts", MOUNT_MAX);
    snprintf(m->name, sizeof m->name, "%s", name);
    snprintf(m->dir, sizeof m->dir, "%s", host_dir);
    ReleaseSRWLockExclusive(&g_mount_lock);
    rt_log("fs: mounted %s:/ at %s", name, host_dir);
}
int rt_fs_unmount(const char *name) {
    int found = 0;
    AcquireSRWLockExclusive(&g_mount_lock);
    for (int i = 0; i < MOUNT_MAX; i++) if (!strcmp(g_mounts[i].name, name)) { g_mounts[i].name[0] = 0; found = 1; }
    ReleaseSRWLockExclusive(&g_mount_lock);
    return found;
}
int rt_fs_mounted(const char *name) {
    int found = !strcmp(name, "rom");
    AcquireSRWLockShared(&g_mount_lock);
    for (int i = 0; i < MOUNT_MAX && !found; i++) found = g_mounts[i].name[0] && !strcmp(g_mounts[i].name, name);
    ReleaseSRWLockShared(&g_mount_lock);
    return found;
}

void rt_fs_path(const char *g, wchar_t *out, size_t cap) {
    char tmp[1200];
    static const char *after;   // BR_TRACE_AFTER=<substring>: switch host-call tracing on once a path contains it
    if (!after) { after = getenv("BR_TRACE_AFTER"); if (!after) after = ""; }
    if (*after && strstr(g, after)) rt_trace |= 9;
    const char *colon = strchr(g, ':');
    if (!strncmp(g, "rom:/", 5)) {
        const char *r = g + 5;
        if (((r[0] | 32) >= 'a' && (r[0] | 32) <= 'z') && r[1] == ':') snprintf(tmp, sizeof tmp, "%s", r);   // rom:/D:\...
        else if (!strncmp(r, "Data/", 5)) snprintf(tmp, sizeof tmp, "%s%s", rt_data_dir(), r + 5);
        else snprintf(tmp, sizeof tmp, "%s%s", rt_data_dir(), r);
    } else if (colon && colon - g > 1 && colon - g < 16) {
        char name[16] = {0};
        memcpy(name, g, (size_t)(colon - g));
        const char *rest = colon + 1;
        while (*rest == '/' || *rest == '\\') rest++;
        tmp[0] = 0;
        AcquireSRWLockShared(&g_mount_lock);
        for (int i = 0; i < MOUNT_MAX; i++)
            if (g_mounts[i].name[0] && !strcmp(g_mounts[i].name, name)) snprintf(tmp, sizeof tmp, "%s%s", g_mounts[i].dir, rest);
        ReleaseSRWLockShared(&g_mount_lock);
        if (!tmp[0]) snprintf(tmp, sizeof tmp, "?:\\unmounted\\%s", g);   // never exists
    } else snprintf(tmp, sizeof tmp, "%s", g);
    for (char *p = tmp; *p; p++) if (*p == '/') *p = '\\';
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, out, (int)cap);
    if (rt_trace & 8) rt_log("fs: path %s -> %s", g, tmp);
}

#define PATH_CAP 1200
#define FS_NOT_FOUND 0x202u            // nn::fs::ResultPathNotFound (module 2, description 1)
#define FS_EXISTS 0x402u               // ResultPathAlreadyExists (2)
#define FS_TARGET_NOT_FOUND 0x7d402u   // ResultTargetNotFound (1002)
#define FS_MOUNT_EXISTS 0x7802u        // ResultMountNameAlreadyExists (60)
#define FS_NOT_MOUNTED 0x196002u       // ResultNotMounted (6448)
#define FS_IO 0x2ee202u

HOSTFN(_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci) {   // mode: 1 read, 2 write, 4 allow append
    wchar_t w[PATH_CAP];
    rt_fs_path(STR(1), w, PATH_CAP);
    DWORD acc = (W(2) & 1 ? GENERIC_READ : 0) | (W(2) & 2 ? GENERIC_WRITE : 0);
    HANDLE h = CreateFileW(w, acc, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (rt_trace & 8) rt_log("fs: OpenFile(%s, mode %u) -> %s", STR(1), W(2), h == INVALID_HANDLE_VALUE ? "not found" : "ok");
    if (h == INVALID_HANDLE_VALUE) { RET(FS_NOT_FOUND); return; }
    ST64(X(0), (uint64_t)(uintptr_t)h);
    RET(0);
}
HOSTFN(_ZN2nn2fs9CloseFileENS0_10FileHandleE) { CloseHandle((HANDLE)PTR(0)); }
static uint64_t fs_read(HANDLE h, uint64_t off, void *buf, uint64_t n) {
    OVERLAPPED ov = {0};
    uint64_t done = 0;
    while (done < n) {
        DWORD want = n - done > (1u << 30) ? (1u << 30) : (DWORD)(n - done), got = 0;
        ov.Offset = (DWORD)(off + done); ov.OffsetHigh = (DWORD)((off + done) >> 32);
        if (!ReadFile(h, (char *)buf + done, want, &got, &ov) || !got) break;
        done += got;
    }
    return done;
}
HOSTFN(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm) { uint64_t n = fs_read((HANDLE)PTR(1), X(2), PTR(3), X(4)); ST64(X(0), n); RET(0); }
HOSTALIAS(_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE, _ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm)
HOSTFN(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm) { fs_read((HANDLE)PTR(0), X(1), PTR(2), X(3)); RET(0); }
HOSTALIAS(_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE, _ZN2nn2fs8ReadFileENS0_10FileHandleElPvm)
HOSTFN(_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE) {
    OVERLAPPED ov = {0};
    DWORD got;
    ov.Offset = (DWORD)X(1); ov.OffsetHigh = (DWORD)(X(1) >> 32);
    RET(WriteFile((HANDLE)PTR(0), PTR(2), (DWORD)X(3), &got, &ov) ? 0 : FS_IO);
}
HOSTFN(_ZN2nn2fs9FlushFileENS0_10FileHandleE) { FlushFileBuffers((HANDLE)PTR(0)); RET(0); }
HOSTFN(_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE) { LARGE_INTEGER sz = {0}; GetFileSizeEx((HANDLE)PTR(1), &sz); ST64(X(0), (uint64_t)sz.QuadPart); RET(0); }
HOSTFN(_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl) {
    LARGE_INTEGER sz; sz.QuadPart = (LONGLONG)X(1);
    RET(SetFilePointerEx((HANDLE)PTR(0), sz, NULL, FILE_BEGIN) && SetEndOfFile((HANDLE)PTR(0)) ? 0 : FS_IO);
}
HOSTFN(_ZN2nn2fs10CreateFileEPKcl) {
    wchar_t w[PATH_CAP];
    rt_fs_path(STR(0), w, PATH_CAP);
    HANDLE h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (rt_trace & 8) rt_log("fs: CreateFile(%s, %lld) -> %s", STR(0), (long long)X(1), h == INVALID_HANDLE_VALUE ? "failed" : "ok");
    if (h == INVALID_HANDLE_VALUE) { RET(GetLastError() == ERROR_FILE_EXISTS ? FS_EXISTS : FS_NOT_FOUND); return; }
    LARGE_INTEGER sz; sz.QuadPart = (LONGLONG)X(1);
    SetFilePointerEx(h, sz, NULL, FILE_BEGIN); SetEndOfFile(h);
    CloseHandle(h);
    RET(0);
}
HOSTFN(_ZN2nn2fs10DeleteFileEPKc) {
    wchar_t w[PATH_CAP];
    rt_fs_path(STR(0), w, PATH_CAP);
    BOOL ok = DeleteFileW(w);
    if (rt_trace & 8) rt_log("fs: DeleteFile(%s) -> %s", STR(0), ok ? "ok" : "not found");
    RET(ok ? 0 : FS_NOT_FOUND);
}
HOSTFN(_ZN2nn2fs10RenameFileEPKcS2_) {
    wchar_t a[PATH_CAP], b[PATH_CAP];
    rt_fs_path(STR(0), a, PATH_CAP);
    rt_fs_path(STR(1), b, PATH_CAP);
    RET(MoveFileW(a, b) ? 0 : GetLastError() == ERROR_ALREADY_EXISTS ? FS_EXISTS : FS_NOT_FOUND);
}
HOSTFN(_ZN2nn2fs15CreateDirectoryEPKc) {
    wchar_t w[PATH_CAP];
    rt_fs_path(STR(0), w, PATH_CAP);
    RET(CreateDirectoryW(w, NULL) ? 0 : GetLastError() == ERROR_ALREADY_EXISTS ? FS_EXISTS : FS_NOT_FOUND);
}
HOSTFN(_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc) {   // 0 directory, 1 file
    wchar_t w[PATH_CAP];
    rt_fs_path(STR(1), w, PATH_CAP);
    DWORD a = GetFileAttributesW(w);
    if (rt_trace & 8) rt_log("fs: GetEntryType(%s) -> %s", STR(1), a == INVALID_FILE_ATTRIBUTES ? "not found" : a & FILE_ATTRIBUTE_DIRECTORY ? "directory" : "file");
    if (a == INVALID_FILE_ATTRIBUTES) { RET(FS_NOT_FOUND); return; }
    ST32(X(0), a & FILE_ATTRIBUTE_DIRECTORY ? 0 : 1);
    RET(0);
}
HOSTFN(_ZN2nn2fs16GetFreeSpaceSizeEPlPKc) { ST64(X(0), 1ull << 30); RET(0); }

// Directories. nn::fs::DirectoryEntry: char name[769]; s8 type at +0x304 (0 directory, 1 file); s64 size at +0x308;
// 0x310 bytes. Open mode: 1 directories, 2 files, 3 both. The listing is taken when the directory is opened.
typedef struct { char name[769]; int8_t type; int64_t size; } DirEnt;
typedef struct { DirEnt *e; int64_t n, pos; } Dir;
HOSTFN(_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci) {
    wchar_t w[PATH_CAP + 8];
    rt_fs_path(STR(1), w, PATH_CAP);
    size_t len = wcslen(w);
    if (len && w[len - 1] != L'\\') w[len++] = L'\\';
    w[len] = L'*'; w[len + 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (rt_trace & 8) rt_log("fs: OpenDirectory(%s, mode %u) -> %s", STR(1), W(2), h == INVALID_HANDLE_VALUE ? "not found" : "ok");
    if (h == INVALID_HANDLE_VALUE) { RET(FS_NOT_FOUND); return; }
    Dir *d = calloc(1, sizeof *d);
    int64_t cap = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!(W(2) & (isdir ? 1 : 2))) continue;
        if (d->n == cap) { cap = cap ? cap * 2 : 32; d->e = realloc(d->e, (size_t)cap * sizeof *d->e); }
        DirEnt *e = &d->e[d->n++];
        memset(e, 0, sizeof *e);
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, e->name, sizeof e->name - 1, NULL, NULL);
        e->type = isdir ? 0 : 1;
        e->size = isdir ? 0 : (int64_t)((uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    ST64(X(0), (uint64_t)(uintptr_t)d);
    RET(0);
}
HOSTFN(_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE) { ST64(X(0), (uint64_t)((Dir *)PTR(1))->n); RET(0); }
HOSTFN(_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl) {
    Dir *d = PTR(2);
    int64_t n = 0, max = (int64_t)X(3);
    for (; n < max && d->pos < d->n; n++, d->pos++) {
        uint8_t *out = (uint8_t *)PTR(1) + 0x310 * n;
        memset(out, 0, 0x310);
        memcpy(out, d->e[d->pos].name, 769);
        out[0x304] = (uint8_t)d->e[d->pos].type;
        memcpy(out + 0x308, &d->e[d->pos].size, 8);
    }
    ST64(X(0), (uint64_t)n);
    RET(0);
}
HOSTFN(_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE) { Dir *d = PTR(0); if (d) { free(d->e); free(d); } }

// Mounts. Save data of the one local user lives in <user dir>save\, temporary storage in <user dir>temp\
// (rt_user_dir: the profile's Saved Games folder, or the game folder when portable). Commit has
// nothing to do (files are written in place). Add-on content <index> is the folder <exe dir>dlc\<index>
// (see aoc_scan in imports_plat.c); most packs are unlock keys with an empty file system.
static uint32_t mount_host(const char *name, const char *sub) {
    char dir[600];
    if (rt_fs_mounted(name)) return FS_MOUNT_EXISTS;
    // BR_PROFILE=<suffix>: a second copy of the game on the same PC keeps its own save and temp folders (save<suffix>\)
    const char *prof = getenv("BR_PROFILE");
    int own = prof && *prof && strlen(prof) < 16 && !strpbrk(prof, "\\/:.") && (!strcmp(sub, "save") || !strcmp(sub, "temp"));
    int user = !strcmp(sub, "save") || !strcmp(sub, "temp");
    snprintf(dir, sizeof dir, "%s%s%s\\", user ? rt_user_dir() : rt_exe_dir(), sub, own ? prof : "");
    if (own || user) CreateDirectoryA(dir, NULL);
    rt_fs_mount(name, dir);
    return 0;
}
HOSTFN(_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE) { RET(mount_host(STR(0), "save")); }
HOSTFN(_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE) { RET(mount_host(STR(0), "save")); }
HOSTFN(_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE) { RET(1); }
HOSTFN(_ZN2nn2fs21MountTemporaryStorageEPKc) { RET(mount_host(STR(0), "temp")); }
HOSTFN(_ZN2nn2fs6CommitEPKc) { RET(rt_fs_mounted(STR(0)) ? 0 : FS_NOT_MOUNTED); }
HOSTFN(_ZN2nn2fs7UnmountEPKc) { if (rt_trace & 8) rt_log("fs: Unmount(%s)", STR(0)); rt_fs_unmount(STR(0)); }
HOSTFN(_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi) { ST64(X(0), 0); RET(0); }
HOSTFN(_ZN2nn2fs17MountAddOnContentEPKciPvm) {
    char sub[32], dir[600];
    wchar_t w[PATH_CAP];
    snprintf(sub, sizeof sub, "dlc\\%d", (int)W(1));
    snprintf(dir, sizeof dir, "%s%s\\", rt_exe_dir(), sub);
    MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, PATH_CAP);
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { RET(FS_TARGET_NOT_FOUND); return; }
    RET(mount_host(STR(0), sub));
}
HOSTFN(_ZN2nn2fs9IsMountedEPKc) { RET(rt_fs_mounted(STR(0))); }
HOSTFN(engine_is_mounted) {   // main+0x10fd1a0 (name, length): is "<name>:" a mounted file system
    char name[16] = {0};
    size_t n = X(1) < 15 ? X(1) : 15;
    memcpy(name, PTR(0), n);
    RET(rt_fs_mounted(name) || (n == 1 && (name[0] | 32) >= 'a' && (name[0] | 32) <= 'z'));   // Windows drive letters count
}
