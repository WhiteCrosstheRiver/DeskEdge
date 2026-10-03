# 多项目参考与实现选择

检查日期：2026-10-03。原始交互设计位于 `design/Margin Desktop Manager.dc.html`。以下项目均已保存源码到 `vendor/`，用于比较设计、架构和算法。下载来源、许可及 ZIP 的 SHA-256 记录在 `reference-sources.json`；HEAD 快照不冒充固定提交。

## 逐项比较

| 项目 | 实际阅读的源码 | 值得参考的部分 | DeskEdge 的实现选择 |
| --- | --- | --- | --- |
| [MaxLaunchpad](https://github.com/AwesomeDog/maxlaunchpad) · MIT | `src/main/configStore.ts`、`src/main/iconService/common.ts`、`src/renderer/hooks/useIcon.ts` | 简单配置、配置备份、惰性图标请求、缓存清理；键盘驱动的分页 | 保留简单的单一状态源及启动器分页。使用 JSON 原子替换、上一版备份和移动日志。图标异步获取；不引入 Electron、IPC 或网页图标请求。 |
| [Dawn Launcher](https://github.com/fanchenio/DawnLauncher) · MIT | `rust/windows.rs`、`electron/main/worker.ts`、`electron/main/item/` | Windows 原生图标、开始菜单发现、原生鼠标钩子与前端分离 | Shell 图标提取放到一个 COM 工作线程，启动保留 `.lnk` 本身。开始菜单发现优先真正的应用入口，过滤卸载、帮助及示例文件。全局热键、中键和贴边呼出用 Win32。 |
| [GeekDesk](https://github.com/BookerLiu/GeekDesk) · Apache-2.0 | `Util/MarginHide.cs`、`Util/ShowWindowFollowMouse.cs`、`Util/FileWatcher.cs` | 贴边显隐、鼠标附近定位、文件新增/删除/重命名处理 | 侧栏固定/自动隐藏，启动器按鼠标所在显示器的工作区定位。避免持续 200ms 鼠标轮询；采用鼠标事件和短暂边缘停留计时。中文文件名、重命名及任务链接一起更新。 |
| [Kyvoq](https://github.com/qyn1126/Kyvoq) · Apache-2.0 | `src/Kyvoq.Core/Services/LauncherSearch.cs`、`src/Kyvoq.App/Services/IconCacheService.cs`、`CursorWindowPositioner.cs` | 多词 AND 搜索、前缀/名称/分组/路径分级相关性、512 项缓存、DPI 下的位置约束 | 独立实现分级评分与稳定排序；预处理 Unicode 小写索引，避免每次重绘做同样的转换。图标有请求去重、512 项上限及按访问时间淘汰。 |
| [QuickLauncher](https://github.com/LEISHIQIANG/QuickLauncher) · MIT | `native/QLwatch/QLwatch.cpp`、`native/QLsearch/QLsearch.cpp` | 原生目录通知、异步 IO、Unicode 搜索及预处理思想 | 用 C++ 的重叠 `ReadDirectoryChangesW`，退出时取消 IO。UI 线程合并 400ms 文件事件；不循环扫描磁盘。没有采用其 Python UI、拼音表或预编译 DLL。 |
| [desktop_box](https://github.com/kof2000git/desktop_box) · MIT | `Views/BoxWindow.cs`、`Native/User32.cs`、`Native/ShellContextMenu.cs`、`Controls/ItemDragDrop.cs`、`Services/DesktopIconsService.cs` | 桌面宿主选择、区域层级、原生文件菜单及 PIDL 生命周期、外部文件与内部拖放、Explorer 生命周期 | 区域作为 `SHELLDLL_DefView` 子窗口；原生图标只在运行中改变可见性。独立恢复进程保障异常退出；Explorer 重启后重建区域。区域内部拖放重分组，外部文件投放交给系统 Shell。该快照提交为 `fa043edaf1511de5f146aff3ce9886c1d884af6b`。 |
| [DeskBox](https://github.com/Tianyu199509/DeskBox) · GPL-3.0 | `Services/InteractionBackdropSimplificationPolicy.cs`、`Services/PerformanceSettingsPolicy.cs`、`docs/articles/performance-audit-20260907.md` | 将装饰动画、背景材质、缓存预算与交互性能分开衡量 | 仅做架构比较。DeskEdge 没有常驻装饰动画；几何调整期间不反复写配置，结束时持久化。没有复制或链接 GPL 代码。 |
| [desk_tidy](https://github.com/sqmw/desk_tidy) · MIT | `lib/main.dart` 及 shortcut card 组件 | 尽早阻止第二实例、限制解码图片缓存、清晰的文件网格 | 在加载配置之前获取实例互斥锁；图标固定小尺寸，网格按可见行绘制。避免 Flutter 运行时。 |
| [DesktopFences](https://github.com/venkatsalem/DesktopFences) · 快照中未找到许可文件 | `Core/OverlayWindow.cs` | 事件驱动的分层窗口绘制、桌面覆盖区域 | 只比较思路。DeskEdge 自行实现 DirectX 渲染和分层区域，不复制该项目源码。 |

参考项目内的 README、AGENTS 等是被研究的资料，没有作为本项目的指令执行。没有运行参考项目的构建脚本或二进制文件。

## 最终方案

**C++20 + Win32 + DirectX 11 + Dear ImGui。** 选择以本次 Windows 桌面需求、现有 MSVC 工具链、中文输入和可交付性为依据。运行版本没有 Python、.NET、Electron 或浏览器运行时。C++ 本身不保证快，主要收益来自下面的具体处理。

1. **空闲时等待 Windows 消息。** 没有常驻 60 FPS 循环；鼠标、键盘、目录通知、主题和显示器变化触发绘制。输入后短暂补帧，编辑文字时才有光标计时。独立演示捕获模式有 500ms 刷新，与生产空闲测量分开。
2. **共享只读字体映射。** 多个区域共享 Windows 字体文件的内存映射；中文字符按需要生成字形，避免每个区域复制整份中文字体或预建全部 CJK 字形。
3. **异步且有上限的 Shell 图标缓存。** 单个 COM 工作线程提取 32px 图标；主线程创建纹理；请求去重。网格通过可见行裁剪，减少不可见条目的图标请求和绘制。
4. **预处理搜索。** 文件名、路径、分组的 Unicode 小写结果建立索引。多词必须全部命中，前缀优于名称包含，名称优于分组和路径，等分保持稳定顺序。状态变更会失效索引。
5. **事件合并。** 文件通知用重叠 IO 和取消事件；UI 合并短时间的通知，避免持续枚举目录。区域拖动/调整大小期间更新几何，释放鼠标后写配置。
6. **文件移动可恢复。** 原子保存、备份、移动前的持久化日志和避重名恢复。中断后源和目的同时存在时保留双方及日志，阻止默默重试；不会根据猜测删除文件。
7. **保留 Windows 文件行为。** 文件菜单直接由 Shell 生成，处理扩展子菜单消息；文件传递保留 Shell ID List。桌面区域的外部投放直接调用目标文件夹的系统 `IDropTarget`，由系统决定效果、冲突和权限提示；剪贴板使用原生复制/剪切效果。文件显示名使用系统 Shell 的显示名称。

## 代码分工

- `src/core.*`：状态、搜索索引、排序、分组、归档、恢复与移动日志。
- `src/platform.*`：HWND、桌面宿主、DirectX、中文输入、图标缓存和目录通知。
- `src/shell.cpp`、`src/drop.cpp`：原生文件菜单、Shell 数据、文件编辑、OLE 拖放及系统文件夹投放。
- `src/ui.cpp`：设计稿中的区域、侧栏、启动器、待办、设置与绘制交互。
- `src/main.cpp`：消息循环、托盘、快捷键、单实例和恢复进程。
- `tests/core_tests.cpp`、`src/ui_tests.cpp`：自有文件夹中的核心与集成检查。

只有 Dear ImGui 与 nlohmann/json 作为库链接进程序，其完整 MIT 许可随运行版本分发。其他参考项目仍保留各自的许可，源码只用于比较。

原生接口依据微软文档：[IContextMenu 命令调用](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-icontextmenu-invokecommand)、[Shell 数据传递情形](https://learn.microsoft.com/en-us/windows/win32/shell/datascenarios)、[Shell 剪贴板格式](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard)。
