# 1.2 图标清晰度与默认网格

日期：2026-10-04。Windows 10 19045，2560 × 1440，125% DPI。

本文记录 1.2 的图标网格实现。1.3 同时默认启用区域窗口网格和浅色卡片，见 [窗口网格与卡片调整](WINDOW_GRID_1.3.md)。

## 图标

旧版将 Shell 图标统一绘制为 32 × 32 纹理，再放大到实际显示尺寸，导致细节模糊。新版按物理像素需求选择 48、64、96、128 或 256 的提取尺寸，在后台 STA 线程调用 `IShellItemImageFactory::GetImage`。纹理保留 Shell 实际返回的尺寸；缓存按文件路径和尺寸区分，绘制坐标对齐物理像素。

区域图标由 34 DIP 调整为 40 DIP；在 125% 下显示为 50 像素，请求至少 64 像素来源。快捷方式箭头等叠加标记来自系统图像列表。旧式图标通过原生掩码重建透明度，避免黑色细节被误当作透明。刷新时的缓存代际检查防止旧任务覆盖新图标。

源文件本身只有小尺寸图标时，不能凭提取接口补出不存在的细节。这里只请求图标，不提取文档缩略图。

## 格点占位

默认启用 `settings.grid_mode`。每个桌面区域图标保存 `grid.column` 和 `grid.row`，格子宽 72 DIP、高 83 DIP。首次升级为已有文件分配空格；后续新增文件使用可用空格，不把已有图标挤紧。

- 空格投放：将拖动图标放到目标格，原格留空，其他图标不动。
- 普通文件占位：单图标交换位置；多图标优先将被占位图标安置到腾出的格子。
- 多选投放：以实际拖起的图标为锚点，尽量保持相对排列；越界时换行并避免重叠。
- 原生目标：文件夹和 Shell 提供拖放处理器的文件仍使用系统行为。普通文件没有处理器时作为格子占位；检测结果在一次拖动内缓存。
- 外部文件：实际复制、移动和链接仍由 Windows 完成，新导入图标放到指示格。
- 整理：区域菜单中的“重新整理图标”主动填紧空位。关闭网格时显示连续排列；保留已保存的格点数据供重新开启使用。

区域变窄时先处理可显示列数，避免重叠；提交配置后保存布局。滚动条出现不改变列数。布局绘制使用稀疏占位表和可见行裁剪，空洞不会要求逐个绘制所有隐藏格子。区域窗口本身仍自由移动。

拖放提示在 OLE 的嵌套消息循环中更新；渲染深度守卫防止重入同一 ImGui/D3D 帧。

## 验证

核心检查包括默认开启、旧配置分配、空位保留、重载、交换、多选锚点、跨区域、窄区域预览、无效格点及主动整理。集成检查调用真实 HWND、D3D 和 Shell COM 接口，覆盖空格落位、占位交换、多选相对布局、外部复制的物理文件和位置、尺寸独立的图标纹理，以及程序快捷方式原生拖放效果。

完整结果见 `evidence/core-test.txt` 和 `evidence/ui-test.json`。`previews/grid-landing.png` 与 `previews/grid-occupied.png` 是自有 GPU 渲染目标导出，检查过图标和格子位置。它们不能当作系统合成截图；资源管理器鼠标到区域的完整拖动仍需用户桌面验收。

## API 依据

- [IShellItemImageFactory::GetImage：提取尺寸、图标标志与后台线程建议](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ishellitemimagefactory-getimage)
- [SHGetImageList：系统图标尺寸与 DPI](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetimagelist)
- [SHGetFileInfoW：原生叠加标记索引](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shgetfileinfow)
- [IImageList::GetOverlayImage：叠加标记图像](https://learn.microsoft.com/en-us/windows/win32/api/commoncontrols/nf-commoncontrols-iimagelist-getoverlayimage)
