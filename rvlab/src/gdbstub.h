/* gdbstub.h —— GDB 远程串行协议(RSP)服务 */
#ifndef RVLAB_GDBSTUB_H
#define RVLAB_GDBSTUB_H

#include "cmd.h"

/* 在 port 上监听(仅回环), 服务一个客户端直至 k/D/断开。返回 0 正常结束 */
int gdbstub_serve(app_t *a, int port);

#endif
