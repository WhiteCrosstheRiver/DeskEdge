# 1.1 玻璃与窗口交互修复

## 修复内容

旧版桌面区域只有半透明底色。每帧使用 GPU staging texture → Map → CPU 位图 → UpdateLayeredWindow；拖动还会在渲染中查询显示器并同步全部窗口。

新版将所有窗口连接到 DirectComposition，使用预乘 Alpha 的 BGRA flip sequential swapchain。正常渲染只进行 GPU 内的纹理拷贝；只有显式导出测试 PNG 才读回 CPU。窗口大小变化先记录最新尺寸，下一次绘制再分配一次纹理。

区域背景读取系统静态壁纸，按显示器及壁纸填充方式定位，在四分之一分辨率上做三轮水平/垂直滑动盒式模糊，再调整饱和度。缓存纹理用于整个显示器，区域移动时更新采样坐标。外观叠加可调浅色/深色底、细颗粒、圆角边缘和顶部高光。背景浓度不会降低文字和图标的不透明度；浓度拖动实时预览、松手保存。

拖动在按钮按下时记录初始点并捕获鼠标，保留 Windows 拖动阈值。移动期间直接更新一个 HWND，不改 JSON、不写磁盘、不重排其他窗口。松手只提交一次；Esc、取消或捕获丢失恢复原位置。缩放同样合并输入。启动器左上标题、设置窗口标题支持移动，侧栏保留贴边定位。

开合使用 ease-out cubic：侧栏 280ms，启动器/设置 200ms，淡出 140ms，区域折叠/展开 180ms。透明度和轻微滑入由系统合成器执行；连续开关从当前透明度续接。隐藏及折叠的计时器在结束时清除。Windows 关闭动画、高对比度或应用内关闭动画时直接切换。

## 验证及边界

新增检查覆盖真实 HWND 的拖动阈值、初始位移、单窗口更新、模型延迟提交、取消恢复、单次保存、缩放合并、DirectComposition 目标和缓存材质，以及动画结束/重开取消隐藏。动画测试可在隔离测试进程内绕过系统动画偏好，以验证完整生命周期；不会修改 Windows 设置。原有原生 Shell 菜单、复制/移动/链接、文件夹投放、重命名与剪贴板检查继续运行。

测试中的“120 次 geometry updates”仅测原生几何调用耗时，不是鼠标到显示器延迟，也不能折算成帧率。

区域玻璃采用静态壁纸缓存，不能反映视频壁纸或其他区域/应用的实时内容；浮动窗口仍使用 Windows 背景模糊，其外观受系统影响。Desktop child HWND 不支持顶层窗口 DWM 模糊路径，因此没有把该路径误当成区域模糊。

本机桌面采集接口继续返回 FrameArrived timeout。PNG 是应用自己渲染的纹理；可检查图标、排版、底层材质，但不包含系统合成出的浮动窗口背景和阴影。实际鼠标手感、物理多显示器和 Windows 11 外观尚不能据此宣称完全验收。

## API 依据

- [DirectComposition target 支持子窗口](https://learn.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-idcompositiondesktopdevice-createtargetforhwnd)
- [DWM blur 的顶层窗口限制](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmenableblurbehindwindow)
- [DirectComposition 透明度动画](https://learn.microsoft.com/en-us/windows/win32/directcomp/how-to--animate-a-visual)
