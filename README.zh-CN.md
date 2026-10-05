# OpenCode Session Manager

[![Release](https://img.shields.io/github/v/release/Linecos/opencode-session-manager?sort=semver&color=2ea44f)](https://github.com/Linecos/opencode-session-manager/releases/latest)
[![Build](https://github.com/Linecos/opencode-session-manager/actions/workflows/build.yml/badge.svg)](https://github.com/Linecos/opencode-session-manager/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows-0078D6.svg)](#环境要求)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](#环境要求)
[![Dear ImGui 1.92](https://img.shields.io/badge/Dear%20ImGui-1.92-9b59b6.svg)](https://github.com/ocornut/imgui)
[![Tests](https://img.shields.io/badge/tests-96%20passing-2ea44f)](tests/test_data.cpp)

[English](README.md) · **简体中文**

用于可视化浏览与清理 [OpenCode](https://opencode.ai) 的旧会话、孤儿快照目录和过期 `session_diff` 文件的桌面工具；同时提供**终端 TUI** 与**图形界面 GUI** 两个版本，使用 C++17 编写，面向 Windows。

## 功能

- 浏览所有 OpenCode 会话、快照目录及其占用体积。
- **两个前端功能完全一致**，并且共用同一份过滤/排序代码（`session_view.cpp`），不会出现一边改了另一边没改的情况。
- **会话视图**：id、标题、项目 / 工作树、消息条数、创建与更新时间。
- **快照视图**：标注 `in use`（仍有会话引用该 项目）或 `no session`（孤儿），并显示目录体积。
- 可多选后批量删除，确认框会逐条列出将要删除的内容。
- **不会直接抹掉数据**：文件默认送进 Windows **回收站**。
- 支持按 id / 标题 / 项目 / 路径实时搜索，表头点击排序，以及"只看孤儿"筛选。
- 每行可**复制**续接命令 `opencode -s <id>`、**跳转**到新控制台里续接该会话（工作树还在时会在其中打开）、**dir / open** 在资源管理器中定位。
- 快照体积按每帧几个目录渐进测量，数据目录很大时两个前端都不会卡住。
- 中文/CJK 字体支持（自动加载微软雅黑等），在缩放显示器上 DPI 正确，跨不同缩放的显示器拖动也能跟随。
- 孤儿 `session_diff/*.json` 清理：可随时手动触发，也可选择退出时清理（默认关闭）。
- **数据库视图**：看清磁盘到底被什么占了，以及三种安全的回收手段（孤儿事件 / 事件日志压缩 / VACUUM）。在一台真实的 4.6 GB 库上，它找出了 3.9 GB。
- 会话内容预览（看对话尾部）与选中会话导出为 JSON。
- 规则化批量选择：`--older-than`、`--max-messages`，然后"选中匹配项"一键选中再删除。
- 界面能做的所有事都有对应的命令行参数，既方便脚本化，也让全部逻辑可被测试覆盖。

## 安全性

- 删除走 `SHFileOperation(FOF_ALLOWUNDO)`，也就是**回收站**，点错可以恢复。设置 `OPENCODE_SM_NO_RECYCLE=1`（或给 TUI 传 `--no-recycle`）则改为永久删除。
- 两个前端的"退出时清理"**默认关闭**；可以勾选复选框、点 `Clean orphan diffs` 按钮，或传 `--cleanup`。
- 删除一个会话时，同时清理属于它的数据行（`message`、`part`、`session_message`、`session_input`、`todo` 等）。opencode 的 schema 只为其中少数几张表声明了外键，所以本工具同时把"任何带有 `session_id` 列的表"视为会话范围数据。
- 删除在单个事务内完成，带 5 秒 busy timeout，并且**如实报告失败**而不是假装成功（例如 opencode 正持有数据库时）。

## 环境要求

- Windows（路径与控制台行为均为 Windows 专有）。
- [MinGW-w64](https://www.mingw-w64.org/) 的 `g++`，需支持 C++17（已在 GCC 8.1 上验证）。
- sqlite3 开发库（下文默认路径为 `C:\MinGW\opt`）。
- TUI 还需要 `ncursesw`。
- [Dear ImGui](https://github.com/ocornut/imgui) 已**内置于本仓库**（`imgui/`）。

## 构建

```bat
rem TUI（终端版）  -> dist\opencode-session-manager.exe
build.bat

rem GUI（Win32 + OpenGL3 + ImGui） -> dist\opencode-session-manager-gui.exe
build_gui.bat

rem 数据层测试    -> dist\test_data.exe（编译后直接运行）
build_tests.bat
```

GUI 是**单文件、不依赖任何额外 DLL**（只链接 `opengl32.dll` 这类 Windows 系统库）。

如果你的工具链不在默认位置，可以直接覆盖——脚本已不再写死 `C:\MinGW`：

```bat
set MINGW_ROOT=D:\msys64\ucrt64
set MINGW_OPT=D:\msys64\ucrt64\opt
build_gui.bat
```

安装了 `mingw32-make` 的话，`Makefile` 提供同样的能力：

```bat
mingw32-make            rem TUI + GUI + 测试
mingw32-make gui
mingw32-make test       rem 编译并运行测试
mingw32-make clean
```

编译进可执行文件的版本号来自唯一一个文件 `version.h`，`resource.rc` 与 C++ 源码都包含它。

## 测试

`build_tests.bat`（或 `mingw32-make test`）会编译并运行 `tests/test_data.cpp`，覆盖的都是"容易写错、而且靠点界面根本发现不了"的地方：

- 非 ASCII 路径（中文 `%USERPROFILE%`、中文工作树与快照目录）；
- 按真实 schema 形态构造的删除级联（子表只带 `session_id` 列、却不声明外键）；
- 数据库被占用时（删除必须明确报错，而不是静默成功）；
- 共享视图模型：过滤（含 CJK 与 ASCII 大小写折叠）、每个排序字段与方向、tie-break 的确定性，以及构建视图不会改动输入数据；
- 数据库维护模块：空间报告、压缩**计划**（并验证"做计划"是只读的）、孤儿事件删除、计划应用、VACUUM、会话预览与 JSON 导出；
- 孤儿 diff 清理、UTF-8 安全截断、续接命令行参数校验。

```
133 checks, 0 failed
```

每次推送都会由 GitHub Actions 构建 TUI、GUI 并跑测试（`.github/workflows/build.yml`）。

另外提供了一个用于检查真实数据库的辅助模式：

```bat
dist\test_data.exe --delete <path\to\opencode.db> <session-id> [<session-id> ...]
```

## 运行

本工具读取与 OpenCode 相同的数据：

- Windows 数据目录：`%USERPROFILE%\.local\share\opencode`

测试时可以覆盖：

```bat
set OPENCODE_DATA_DIR=C:\some\opencode\data
dist\opencode-session-manager-gui.exe
```

其他环境变量：

| 变量 | 作用 |
|------|------|
| `OPENCODE_DATA_DIR` | 要读取的数据目录（含 `opencode.db`、`storage/`、`snapshot/`） |
| `OPENCODE_SM_NO_RECYCLE` | 设为 `1` 时永久删除，不使用回收站 |

设置项（目前只有字号）按用户保存在 `%APPDATA%\opencode-session-manager\settings.ini`。

### TUI 快捷键

| 按键 | 作用 |
|------|------|
| `上/下` / `j` / `k` | 移动光标 |
| `PgUp` / `PgDn`、`Home` / `End` | 翻页、跳到首/末行 |
| `Space` | 切换选择 |
| `Ctrl+A` | 全选当前可见行 |
| `Ctrl+D` | 清空选择 |
| `Tab` | 切换视图（会话 / 快照 / 数据库） |
| `F5` | 重新加载数据 |
| `/` | 过滤（边打边筛，`Enter` 应用，`Esc` 取消） |
| `s` / `S` | 循环切换排序字段 / 反转排序方向 |
| `d` 或 `Enter` | 删除所选（按 `y` 确认，按 `n` 取消） |
| `c` | 复制续接命令 `opencode -s <id>` |
| `o` | 在新控制台中续接光标所在会话 |
| `r` | 在资源管理器中定位工作树 / 快照目录 |
| `O` | 快照视图：只看孤儿目录 |
| `p` | 会话视图：预览该会话尾部内容 |
| `e` | 把选中的会话导出为 JSON |
| `x` | 立即清理孤儿 `session_diff` 文件 |
| `Ctrl+E` | 切换"退出时清理" |
| `Esc` | 有过滤时先清过滤，否则退出 |
| `q` | 退出 |

数据库视图按键：`r` 扫描、`c` 分析事件日志（dry-run）、`x` 清理孤儿事件、`b` 压缩（先备份）、`v` VACUUM，`j`/`k`/`PgUp`/`PgDn` 滚动。

顶部状态行会显示"已过滤/总行数"、当前排序方式，以及孤儿快照目录占用的空间。快照体积是分批测量的，数据目录很大时界面依然流畅；测量期间会在标签栏下方显示进度条（百分比 + 已完成/总数），未算完的行显示 `...`。

### 无界面列表模式

同一条加载/过滤/排序管线也可以脱离界面使用，因此本工具可以写进脚本：

```bat
rem 占空间最大的 10 个孤儿快照目录
opencode-session-manager.exe --list --snapshots --orphans --limit 10

rem 按消息数排序，只看提到某个项目的会话
opencode-session-manager.exe --list --filter nixStats --sort msgs
```

| 参数 | 作用 |
|------|------|
| `--list` | 打印当前视图后退出 |
| `--snapshots` | 列出快照目录（默认列出会话） |
| `--orphans` | 配合 `--snapshots`：只列出没有会话引用的目录 |
| `--filter <文本>` | 匹配 id / 标题 / 项目 / 工作树（快照为 路径 / 名称） |
| `--sort <字段>` | `created`、`updated`、`msgs`、`size`、`status`、`id`、`name`、`project` |
| `--asc` | 升序（默认降序） |
| `--limit <n>` | 最多打印 n 行 |
| `--older-than <days>` | 只列出创建时间早于 n 天的会话 |
| `--max-messages <n>` | 只列出消息数不超过 n 的会话 |
| `--preview <id>` | 打印某个会话的尾部内容 |
| `--export <dir>` | 把列出的会话导出为 JSON 文件 |
| `--cleanup` | 界面退出后顺便清理孤儿 `session_diff` 文件 |
| `--no-recycle` | 永久删除，不使用回收站 |
| `--version` | 打印版本号后退出 |

## 数据库维护

上面那些功能清理的是会话、快照和 diff——在一台真实机器上通常只有几 MB。之所以要有**数据库视图**，是因为磁盘几乎总是被别的东西吃掉了：

```
$ opencode-session-manager --db-report
database file   : 4.4G  (1141873 pages x 4096 bytes)
  free pages    : 214M   <- reclaimable by VACUUM
largest tables (estimated from content bytes when dbstat is unavailable):
  event                       3.6G
  part                        57.6M
event log 3.6G (232110 rows) by type:
  message.part.updated.1    191168 rows      3.6G
  message.updated.1          31481 rows     86.4M
```

`event` 是 opencode 的 append-only 事件日志，而 `message.part.updated.1` 会在**每次更新时把整个 part 的完整快照再存一份**——上面这个库里**单个 part 有 87,121 个版本**，自己就占了 2.8 GB。删除会话完全不会碰这些行，所以文件永远不缩小。

三种手段，按胆量递增：

| 操作 | 做什么 | 安全性 |
|------|--------|--------|
| `--clean-orphan-events` | 删除所属会话已不存在的 event 行 | 安全：聚合已经没了，没有任何东西会回放它 |
| `--compact-events` | 每个 part 只保留最新版本 | **会重写事件日志。** 默认 dry-run；要执行必须提供备份 |
| `--vacuum` | 重写文件，把空闲页还给文件系统 | 需要约等于文件大小的临时空间，耗时数分钟；先关掉 opencode |

```bat
rem 先看清空间去向（只读）
opencode-session-manager.exe --db-report

rem 先回收确定无疑的垃圾，再压缩，最后缩小文件
opencode-session-manager.exe --clean-orphan-events --yes
opencode-session-manager.exe --compact-events                      rem dry run
opencode-session-manager.exe --compact-events --yes --backup D:\opencode.db.bak
opencode-session-manager.exe --vacuum --yes
```

关于压缩的几点说明（它是唯一会改动 opencode 数据的操作）：

- **不加 `--yes` 就只是 dry-run**；而加 `--yes` 时必须同时给 `--backup <文件>`。GUI 里执行前也总是先自动备份。
- 读不出 part id 的行（体积超大的那些）**永远不会被删除**。
- 它按 `seq` 保留最新版本，所以每个 part 的最终状态都被完整保留。至于 opencode 的回放能否接受旧版本被删掉，只能拿结果去跑一次 opencode 才能证明——这正是备份不可选的原因。
- 两个前端共用同一份实现，并且都作为**后台任务**执行、带进度显示，所以几分钟的 VACUUM 不会把界面卡死。
- 空间扫描是**一次顺序遍历**事件日志，聚合在 C++ 侧完成而不是交给 SQL。opencode 的 schema 里没有以 `type` 开头的索引，`GROUP BY type` 只能退化到 sorter，而这个 sorter 会把整列 `data` 一起带上：在 4.4 GB 的库上实测，这条查询要跑约 80 秒、并产生约 **34 GB 的临时文件读写**（SQLite 会立刻 unlink 临时文件，但 Windows 要等最后一个句柄关闭才把空间还回来，所以运行期间系统盘看起来在飞速变满）。改成流式后约 15 秒完成，内存峰值几百 MB，且**不产生任何临时文件**。

### GUI 使用说明

- **View** 按钮切换会话 / 快照 / 数据库；菜单栏显示总数以及孤儿快照目录占用空间。
- 工具栏：**Refresh**、**Select All**、**Clear Sel**、**Delete Selected**、**Clean orphan diffs**、**Cleanup on exit** 复选框、搜索框、**Orphans only** 筛选、规则化选择（`Older than [n] days`、`max msgs`、**Select matching**）、**Preview** 开关，以及字号控件。
- 点击表头排序（默认：会话按创建时间，快照按体积从大到小——这也是打开这个工具最常见的目的）。
- 快捷键：`F5` 刷新，`Del` 删除所选，`Ctrl+A` 全选，`Ctrl+D` 清空选择，`Ctrl+F` 聚焦搜索框，`Ctrl++` / `Ctrl+-` / `Ctrl+0` 调整字号，`Esc` 清除搜索 / 选择。
- 每行操作：**copy**（复制续接命令）、**jump**（在新控制台续接）、**dir** / **open**（在资源管理器中定位）。
- **Database** 页有扫描按钮和三种维护操作，每个都带确认框，并在后台带进度条执行。
- **字号**可在工具栏调整（`Text: 24 px  -  +  reset`），设置保存在 `%APPDATA%\opencode-session-manager\settings.ini`。默认值与旧版本渲染出来的观感一致——详见下节。

### 关于字号与 DPI 的说明

Dear ImGui 1.92 的字号公式是
`style.FontSizeBase * style.FontScaleMain * style.FontScaleDpi`。早期版本既把一个已经乘过 DPI 的像素尺寸交给字体图集，又设置了 `FontScaleDpi`，于是文字始终按 `scale²` 渲染——在 150% 缩放显示器上是 36px 而不是 24px。这个问题已经修掉（显示器缩放只应用一次），但仅仅修掉它会让字比大家习惯的小，因此字号变成了一个设置项，其默认值与原外观一致。按 `Ctrl+0` 或 **reset** 回到该默认值；继续按 `Ctrl+-` 可以降到"标准 16px 界面在 100% 下"的大小。

列宽是根据当前字体度量实时计算的，所以在任何字号下操作按钮都不会被裁切。

## 许可证

[MIT](LICENSE) © [Linecos](https://github.com/Linecos)

内置的第三方库各自保留其许可证：Dear ImGui 为 MIT，sqlite3 为公有领域。
