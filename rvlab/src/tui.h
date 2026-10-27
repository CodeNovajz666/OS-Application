/* tui.h —— TUI 工作台入口 */
#ifndef RVLAB_TUI_H
#define RVLAB_TUI_H

#include "cmd.h"

/* 进入 TUI 主循环(阻塞直至 quit)。终端原始模式 + 备用屏, 退出时恢复。 */
void tui_run(app_t *a);

#endif
