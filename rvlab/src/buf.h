/* buf.h —— 编辑器文本缓冲（行数组模型） */
#ifndef RVLAB_BUF_H
#define RVLAB_BUF_H

#include <stddef.h>

typedef struct {
    char  **lines;
    size_t  n, cap;
    size_t  cx, cy;          /* 光标(列/行, 列按字节计) */
    size_t  scroll;          /* 视口首行 */
    char    path[256];
    int     dirty;
} buf_t;

int  buf_open(buf_t *b, const char *path);      /* 不存在则新建空缓冲 */
void buf_free(buf_t *b);
int  buf_save(buf_t *b);

void buf_insert(buf_t *b, char ch);
void buf_backspace(buf_t *b);
void buf_delete(buf_t *b);
void buf_enter(buf_t *b);
/* dir: 0 上 1 下 2 左 3 右 4 Home 5 End 6 PgUp 7 PgDn(按 page 行) */
void buf_arrow(buf_t *b, int dir, size_t page);

const char *buf_line(buf_t *b, size_t i);       /* 越界返回 "" */

#endif
