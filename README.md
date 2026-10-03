# DeskEdge · 桌沿

Windows 10/11 原生桌面区域管理器。C++20、Win32、DirectX 11 / DirectComposition，无 Python、Electron 或 .NET 运行时。当前版本 1.1.0。

## 使用

运行 `DeskEdge.exe`。`Ctrl+Space` 搜索桌面文件，`Ctrl+Num0` 或 `Ctrl+0` 呼出启动器。快捷键冲突会记录在日志中；搜索快捷键冲突时尝试 `Ctrl+Shift+Space`。托盘菜单提供搜索、启动器、绘制区域、查看壁纸、设置和退出。

区域标题可拖动、双击重命名、折叠，右下角可调整大小。文件双击打开，右键显示 Windows Shell 的真实文件菜单，包含系统关联和已安装的扩展；DeskEdge 功能放在额外的子菜单中。支持 Ctrl / Shift 多选、框选、批量拖放、Ctrl+A、Ctrl+C/X/V、F2、Delete 和 Shift+F10。文件名显示使用系统 Shell 的名称，保留扩展名显示偏好。

从资源管理器拖入桌面区域，交给 Windows 的文件夹拖放处理器：同盘默认移动、跨盘默认复制，Ctrl 复制、Shift 移动、Alt 或 Ctrl+Shift 创建快捷方式。同名文件、权限和进度提示由 Windows 处理。拖到区域中的文件夹会进入该文件夹；拖出使用真实 Shell 文件数据。区域之间普通拖动只改变分组；拖入启动器建立启动项引用。启动器单击或 Enter 打开文件，Tab / Shift+Tab 切换标签页。

每周整理只针对临时区域中超过设定天数的文件。保留会重新计时；归档移动到桌面 `DeskEdge Archive/YYYY-MM-DD`；恢复会保留原位置的同名文件，自动添加编号。移动前保存日志，启动时恢复中断操作。目录不做自动删除。

设置提供开机启动、中键呼出、原生桌面图标显示方式、整理阈值和侧栏显示器。底部切换语言和主题。首次运行读取当前用户和公共桌面，并从开始菜单选择已安装软件；不会创建虚假的应用入口。

### 1.1 玻璃与交互修复

- 区域使用按显示器缓存的壁纸模糊、饱和度调整、细颗粒和圆角亮边；移动时材质与壁纸位置同步。设置里的“玻璃浓度”支持 20–90% 实时预览，文字、图标保持不透明。
- 所有窗口通过 DirectComposition 在 GPU 上合成；日常绘制不再将画面从 GPU 读回 CPU 再提交分层窗口。
- 拖动直接更新当前窗口，松手保存一次；Esc 或丢失鼠标捕获取消。缩放输入合并到下一帧分配画面。启动器左上标题、设置标题也能拖动。
- 侧栏 280ms、启动器和设置 200ms 淡入与轻微滑入；140ms 淡出，区域折叠/展开 180ms。动画遵循 Windows 动画和高对比度偏好，也可在设置里关闭。

桌面区域的模糊来源是系统静态壁纸，不是视频壁纸或覆盖在后面的其他程序；浮动窗口使用 Windows 背景模糊。不同 Windows 版本的合成效果需在实际桌面确认。升级时先从托盘退出旧版，再运行新 EXE，原有数据继续使用。

## 数据

默认保存在 `%LOCALAPPDATA%/DeskEdge`：`state.json`、备份 `state.json.bak`、`DeskEdge.log`。损坏的主配置会保留并尝试读取备份。程序运行时隐藏原生桌面图标，退出时恢复；独立的恢复进程负责崩溃后的恢复。Windows Explorer 内部桌面窗口并非公开稳定接口，Explorer 重启后会尝试重新挂接。

演示运行：`DeskEdge.exe --demo --data-dir "C:\\path\\to\\demo-profile"`。所有演示文件在该配置目录中。`--no-desktop-host` 使用独立区域窗口，便于调试；`--quit-after 15` 自动退出并写入 `diagnostics.json`。

## 构建

需要 Visual Studio 2022 C++ 桌面工具和 Windows SDK，CMake 3.24+。

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist/DeskEdge
```

构建使用静态 MSVC CRT。运行版本只需单个 EXE；字体使用 Windows 自带 Segoe UI 和微软雅黑。

原始设计在 `design/`，参考分析、验收记录在 `docs/`。仓库自带 Dear ImGui 和 nlohmann/json，克隆后可直接构建。9 个比较项目的源码保留在开发机器的 `vendor/` 中，不提交到本仓库；来源、许可证及快照校验值见 `docs/reference-sources.json`。文件操作测试只使用独立配置目录中的自有文件。

当前原生接口验收环境为 Windows 10。Windows 11 使用 Shell 提供的传统文件菜单；尚未验证其新版紧凑菜单的外观一致性。系统采集工具超时，资源管理器到区域的完整鼠标拖放仍需实际桌面验收，见 `docs/ACCEPTANCE.md`。
