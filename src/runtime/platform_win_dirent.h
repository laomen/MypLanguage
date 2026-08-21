/* platform_win_dirent.h — dirent（目录遍历）Windows 模拟（MinGW-w64）
 *
 * 从 platform_win.h 提取的独立头：runtime.c（经 platform_win.h）与编译器
 * mypc（src/main.cpp）都要遍历目录（listCFiles/listLibFiles），共用此实现，
 * 避免把 termios/ioctl/poll/mkdir 宏等运行时专属模拟拖进编译器 TU。
 *
 * 依赖：windows.h（FindFirstFileA/FindNextFileA/FindClose）、stdio.h、stdlib.h。
 * 注意：d_name 上限 260（MAX_PATH 风格），与 POSIX dirent 语义一致即可。
 */
#ifndef MYP_PLATFORM_WIN_DIRENT_H
#define MYP_PLATFORM_WIN_DIRENT_H

#if defined(_WIN32)

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dirent { char d_name[260]; };
typedef struct {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    struct dirent ent;
    int first;
} DIR;
static inline DIR* opendir(const char* path) {
    DIR* d = (DIR*)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    char pattern[512];
    _snprintf(pattern, sizeof(pattern), "%s\\*", (path && path[0]) ? path : ".");
    d->h = FindFirstFileA(pattern, &d->fd);
    d->first = 1;
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    return d;
}
static inline struct dirent* readdir(DIR* d) {
    if (!d) return NULL;
    for (;;) {
        int ok = d->first ? (d->h != INVALID_HANDLE_VALUE) : FindNextFileA(d->h, &d->fd);
        d->first = 0;
        if (!ok) return NULL;
        if (strcmp(d->fd.cFileName, ".") == 0 || strcmp(d->fd.cFileName, "..") == 0)
            continue;
        _snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", d->fd.cFileName);
        return &d->ent;
    }
}
static inline int closedir(DIR* d) {
    if (!d) return -1;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
    return 0;
}

#endif /* _WIN32 */
#endif /* MYP_PLATFORM_WIN_DIRENT_H */
