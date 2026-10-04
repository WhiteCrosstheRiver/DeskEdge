# 验收记录

日期：2026-10-04。环境：Windows 10 19045，MSVC 19.44，x64 Release，2560 × 1440，125% DPI，32 个逻辑处理器。

1.3 交付结果：核心 51 项检查通过；原生桌面界面与接口 114 项检查通过，失败 0 项。包含区域窗口网格、占位冲突、辅助层的透明度与输入行为，以及原有图标、原生文件操作与动画回归。详见 [窗口网格与卡片调整](WINDOW_GRID_1.3.md)、[图标与网格修复](ICON_GRID_1.2.md) 和 [玻璃与交互修复](GLASS_MOTION_1.1.md)。

## 验证方式

核心测试在独立临时目录中运行。集成测试通过本程序的 ImGui 输入队列操作真实界面控件，使用真实 HWND、DirectX、ShellExecute 和目录通知；文件全部来自独立的演示配置目录。真实快捷方式测试启动本程序的专用标记模式，检查 `.lnk` 中的参数是否传递成功。

系统画面采集工具多次返回 `window capture timed out: timed out waiting on channel`。因此导出的 PNG 是程序自身的 DirectX 渲染目标，不能当作桌面合成截图。系统鼠标从资源管理器拖入、物理多显示器之间移动、Windows 11 和 Explorer 重启过程尚未通过外部 UI 自动化验收。

## 已检查的功能

- 核心：Unicode 与大小写搜索、AND 分词、重复引用、跨分组移动、排序后拖放、跨标签搜索、保留计时、真实归档和恢复、同名文件保护、文件占用失败、链接更新、中断移动恢复、模糊移动日志保留、删除最后区域后的归组、配置损坏及结构错误时读备份。
- 界面：默认区域尺寸、DPI 比例、整理展开、保留/归档/恢复按钮、中文待办输入和回车、搜索及 Esc、折叠/展开、启动器尺寸、Tab 切换、排序、拖放命中区域、真实 `.lnk` 参数传递、打开后关闭、中英文、深色主题、设置开关、绘制新区域及标题编辑、查看壁纸、原生新增/重命名事件。
- 原生宿主：1.3 完整套件在实际桌面子窗口上运行，并确认辅助层与区域共享桌面父窗口。`native-host.json` 保留 1.2 的独立诊断基线：四个区域为有效 HWND，父类 `SHELLDLL_DefView`，125% 下区域 395 × 245 像素、临时区域 395 × 267 像素。1.3 默认区域对齐后为 420 × 270 像素。
- 文件菜单：真实 `IShellFolder::GetUIObjectOf` / `IContextMenu`；检查系统提供的 copy、cut、delete、properties、rename 命令，支持 `IContextMenu2/3` 的子菜单和自绘消息。菜单对象保留选择和 PIDL 的完整生命周期。额外功能位于 DeskEdge 子菜单中。
- 拖放：真实 Shell `IDataObject` 保留 Shell ID List；桌面投放使用系统 `IDropTarget`，验证 Ctrl 复制、同盘普通拖动移动、Ctrl+Shift 建立真实 `.lnk`、投放到文件夹、多个文件在区域之间归组。测试通过原生 COM 接口调用执行真实文件操作，不等同于资源管理器的鼠标端到端验收。
- 文件编辑：F2 和 Enter 原位重命名，Unicode 路径、扩展名和文件身份保留；多文件重命名添加编号并保留各自扩展名；Ctrl+V 读取系统剪贴板的复制/剪切效果。测试期间保存并恢复原剪贴板。
- 单实例：同一配置目录第二次运行退出码为 0，配置文件 SHA-256 没有变化。
- 恢复：正常退出和强制停止主进程后，恢复进程均记录 `icons_restored: true`。
- 1.1 拖动：系统阈值、完整初始位移、只更新目标 HWND、移动中不改模型、取消恢复、松手单次提交、DPI 单位保存、缩放合并到下一帧。
- 1.1 渲染与动画：四个真实桌面子窗口均具有 DirectComposition 目标；缓存壁纸材质建立成功；折叠结束尺寸正确、淡出完成隐藏 HWND、淡出中重新打开取消隐藏、动画计时器按时停止。测试强制动画只作用于独立测试进程，不改系统偏好。
- 1.2 图标：64 与 96 像素请求分别缓存；64 像素请求对应纹理尺寸至少 64 × 64；渲染预览检查了 Chrome、Excel、文件夹与快捷方式叠加箭头。
- 1.2 网格：默认开启、旧配置分配、保存与重载、空洞保留、重复与无效位置处理、主动整理、跨区域和多选锚点；真实窗口检查指针到格点换算、原生内部空格投放和普通占位交换、外部 Shell 复制文件的落位、程序快捷方式原生处理器效果不变。
- 1.3 区域：DPI 下移动和尺寸吸附、捕获/焦点保留、辅助层不激活和透明命中、GPU Alpha 与局部表面、单次提交、重启重载、取消还原、完整工作区边界、占位冲突拒绝、新区域网格对齐；首次迁移与关闭吸附的核心检查。

逐项结果与渲染预览保存在项目的 `artifacts/`，交付副本位于 `docs/evidence/` 和 `docs/previews/`。

## 测量

| 项目 | 结果 | 范围 |
| --- | --- | --- |
| 1 万条记录搜索 | 1.3 平均约 0.84ms | 同一测试程序，20 次查询的均值，包括首次建索引；本机结果。 |
| 原生桌面模式工作集 | 67,895,296 字节，约 64.7MiB | 1.1，4 个区域、侧栏、21 个图标，字体使用共享映射。 |
| 空闲 CPU | 5.025 秒采样中累计 CPU 时间增量 0.0 秒 | 1.1 清理旧进程、预热后、本机采样精度下；不代表任何情况下都没有 CPU 消耗。 |
| 进程私有内存 | 76,587,008 字节，约 73.0MiB | 1.1 同一次空闲采样；与工作集含义不同。 |

没有用强制裁剪工作集掩盖占用。绘制和交互时需要正常的 CPU/GPU 工作。1.1 日常渲染使用 GPU 内拷贝及 DirectComposition，不再使用 GPU 到 CPU 分层窗口位图的传输。显式导出测试预览仍需一次读回。

## 系统兼容边界

本机为 Windows 10，其文件菜单来自系统和已安装的 Shell 扩展。Windows 11 的传统 Shell 菜单与新版紧凑菜单存在外观差异，尚不能声明两者完全相同。第三方菜单扩展的行为和兼容性由实际安装环境决定。跨盘默认行为直接委托系统处理，当前测试没有跨两个物理盘移动测试文件。

右键拖动的操作选择也由系统处理。测试修正了投放时再次发送已松开鼠标的 DragOver 状态会触发操作选择菜单的问题；普通左键投放保持正确的鼠标状态。

## 可复现命令

```powershell
ctest --test-dir build -C Release --output-on-failure
build/Release/DeskEdgeTests.exe
build/bin/Release/DeskEdge.exe --self-test-ui --data-dir "C:\path\to\fresh-test-profile"
build/bin/Release/DeskEdge.exe --self-test-ui-native --data-dir "C:\path\to\fresh-native-ui-profile"
build/bin/Release/DeskEdge.exe --render-previews --data-dir "C:\path\to\fresh-preview-profile"
build/bin/Release/DeskEdge.exe --demo --quit-after 12 --data-dir "C:\path\to\fresh-native-profile"
```

集成测试需要全新的配置目录，因为会创建待办、恢复文件、变更主题及新建区域。正常运行的数据目录为 `%LOCALAPPDATA%/DeskEdge`。
