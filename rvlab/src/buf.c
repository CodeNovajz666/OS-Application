/* buf.c —— 编辑器文本缓冲实现 */
#include "buf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int ensure(buf_t *b)
{
    if (b->n < b->cap)
        return 0;
    size_t nc = b->cap ? b->cap * 2 : 128;
    char **nl = realloc(b->lines, nc * sizeof *nl);
    if (!nl)
        return -1;
    b->lines = nl;
    b->cap = nc;
    return 0;
}

int buf_open(buf_t *b, const char *path)
{
    memset(b, 0, sizeof *b);
    snprintf(b->path, sizeof b->path, "%s", path);
    if (ensure(b) != 0)
        return -1;
    FILE *f = fopen(path, "r");
    if (!f) {                     /* 新文件 */
        b->lines[b->n++] = xstrdup("");
        return 0;
    }
    char ln[4096];
    while (fgets(ln, sizeof ln, f)) {
        size_t l = strlen(ln);
        while (l && (ln[l - 1] == '\n' || ln[l - 1] == '\r'))
            ln[--l] = 0;
        if (ensure(b) != 0) { fclose(f); return -1; }
        b->lines[b->n++] = xstrdup(ln);
    }
    fclose(f);
    if (b->n == 0 && ensure(b) == 0)
        b->lines[b->n++] = xstrdup("");
    return 0;
}

void buf_free(buf_t *b)
{
    for (size_t i = 0; i < b->n; i++)
        free(b->lines[i]);
    free(b->lines);
    memset(b, 0, sizeof *b);
}

int buf_save(buf_t *b)
{
    FILE *f = fopen(b->path, "w");
    if (!f)
        return -1;
    for (size_t i = 0; i < b->n; i++)
        fprintf(f, "%s\n", b->lines[i]);
    fclose(f);
    b->dirty = 0;
    return 0;
}

static void clamp_cursor(buf_t *b)
{
    if (b->cy >= b->n)
        b->cy = b->n ? b->n - 1 : 0;
    size_t len = strlen(buf_line(b, b->cy));
    if (b->cx > len)
        b->cx = len;
}

void buf_insert(buf_t *b, char ch)
{
    if (ensure(b) != 0)
        return;
    char *ln = b->lines[b->cy];
    size_t len = strlen(ln);
    char *nl = malloc(len + 2);
    if (!nl)
        return;
    memcpy(nl, ln, b->cx);
    nl[b->cx] = ch;
    memcpy(nl + b->cx + 1, ln + b->cx, len - b->cx);
    nl[len + 1] = 0;
    free(ln);
    b->lines[b->cy] = nl;
    b->cx++;
    b->dirty = 1;
}

void buf_backspace(buf_t *b)
{
    if (b->cy >= b->n)
        return;
    if (b->cx > 0) {
        char *ln = b->lines[b->cy];
        memmove(ln + b->cx - 1, ln + b->cx, strlen(ln) - b->cx + 1);
        b->cx--;
        b->dirty = 1;
    } else if (b->cy > 0) {                 /* 并到上一行 */
        char *prev = b->lines[b->cy - 1];
        char *cur = b->lines[b->cy];
        size_t pl = strlen(prev);
        char *m = realloc(prev, pl + strlen(cur) + 1);
        if (!m)
            return;
        strcpy(m + pl, cur);
        b->lines[b->cy - 1] = m;
        free(cur);
        memmove(&b->lines[b->cy], &b->lines[b->cy + 1],
                (b->n - b->cy - 1) * sizeof *b->lines);
        b->n--;
        b->cy--;
        b->cx = pl;
        b->dirty = 1;
    }
}

void buf_delete(buf_t *b)
{
    if (b->cy >= b->n)
        return;
    char *ln = b->lines[b->cy];
    size_t len = strlen(ln);
    if (b->cx < len) {
        memmove(ln + b->cx, ln + b->cx + 1, len - b->cx);
        b->dirty = 1;
    } else if (b->cy + 1 < b->n) {          /* 下一行并入 */
        char *cur = b->lines[b->cy];
        char *next = b->lines[b->cy + 1];
        char *m = realloc(cur, len + strlen(next) + 1);
        if (!m)
            return;
        strcpy(m + len, next);
        b->lines[b->cy] = m;
        free(next);
        memmove(&b->lines[b->cy + 1], &b->lines[b->cy + 2],
                (b->n - b->cy - 2) * sizeof *b->lines);
        b->n--;
        b->dirty = 1;
    }
}

void buf_enter(buf_t *b)
{
    if (ensure(b) != 0)
        return;
    char *ln = b->lines[b->cy];
    char *tail = xstrdup(ln + b->cx);       /* 光标后半段成为新行 */
    if (!tail)
        return;
    char *cut = realloc(ln, b->cx + 1);
    if (!cut) { free(tail); return; }
    cut[b->cx] = 0;
    b->lines[b->cy] = cut;
    memmove(&b->lines[b->cy + 2], &b->lines[b->cy + 1],
            (b->n - b->cy - 1) * sizeof *b->lines);
    b->lines[b->cy + 1] = tail;
    b->n++;
    b->cy++;
    b->cx = 0;
    b->dirty = 1;
}

void buf_arrow(buf_t *b, int dir, size_t page)
{
    switch (dir) {
    case 0: if (b->cy > 0) b->cy--; break;
    case 1: if (b->cy + 1 < b->n) b->cy++; break;
    case 2: if (b->cx > 0) b->cx--; break;
    case 3: b->cx++; break;                 /* clamp 兜底 */
    case 4: b->cx = 0; break;
    case 5: b->cx = strlen(buf_line(b, b->cy)); break;
    case 6: b->cy = b->cy > page ? b->cy - page : 0; break;
    case 7: if (b->cy + page < b->n) b->cy += page; break;
    }
    clamp_cursor(b);
}

const char *buf_line(buf_t *b, size_t i)
{
    if (!b->lines || i >= b->n)
        return "";
    return b->lines[i];
}
