# HideFoldersUsingFolderIcons

通过**定时截取文件夹所在位置的桌面背景**，并把截图设置为该文件夹的图标，让桌面上的文件夹与壁纸融为一体、肉眼不可见——但图标仍然在原位，双击照样能打开。

A Windows (C++/Win32) utility that **hides a desktop folder in plain sight**: it periodically samples the desktop background at the folder's icon position and applies that image as the folder's icon, so the folder visually disappears into the wallpaper while remaining fully clickable.

## 效果 Features

- ✅ 定时（默认 200ms，可调）采样壁纸并更新文件夹图标，动态壁纸（Wallpaper Engine 等）也能跟上
- ✅ **壁纸文件直读**：优先经 `IDesktopWallpaper` + WIC 直接解码壁纸文件采样，桌面被窗口遮挡时也照常出图；不可用时自动回退截图
- ✅ **快捷方式伪装**：桌面整理工具不认 desktop.ini 图标时，可伪装指向文件夹的 `.lnk` 快捷方式（自动发现**全部**指向该文件夹的快捷方式并逐一定位伪装，也可显式传入单个 `.lnk`）
- ✅ 兼容使用列表视图的桌面整理工具（Fences、腾讯桌面整理等），壁纸来源不限（壁纸引擎 / 幻灯片 / 普通壁纸均可）
- ✅ 运行时**无托盘图标**；点窗口 ✕ 只是**隐藏窗口**，程序继续后台刷新
- ✅ **再次运行 exe 即可唤出窗口**（单实例：第二次启动会激活已运行的实例）
- ✅ 背景无变化时不写盘；被其它窗口遮挡时自动暂停刷新，移开后恢复

配合名字为**不可见合法字符**（如全角空格 `　`）的文件夹效果最佳：图标融于壁纸、文件名不可见。

## 使用 Quick start

1. 编译得到 `HideFoldersUsingFolderIcons.exe`（见下），把它放进桌面上的目标文件夹，例如：

   ```
   C:\Users\<你>\Desktop\　\HideFoldersUsingFolderIcons.exe
   ```

2. 双击运行：程序立刻完成第一次伪装，窗口显示运行状态。
3. 点 ✕ 或「隐藏窗口」按钮 → 窗口隐藏，后台继续刷新（无托盘）。
4. 想再次看到窗口：**在文件夹里再运行一次 exe** 即可唤出。
5. 「恢复默认图标」还原文件夹/快捷方式图标；「退出」真正结束进程。

### 快捷方式模式 Shortcut mode

部分桌面整理工具不渲染 `desktop.ini` 的自定义文件夹图标，但**一定会渲染快捷方式的图标**。此时伪装"指向该文件夹的桌面快捷方式"即可：

- 把快捷方式（如 `　.lnk`、`　　.lnk` …，目标均为隐身文件夹）放在桌面，程序运行时在目标文件夹内启动；
- 程序会自动发现桌面上**所有**指向目标文件夹的快捷方式，**连同文件夹本身一起伪装**：每个图标分别采样**各自位置**的壁纸并写入各自专属的 `wall_icon_*.ico`，不会互相覆盖；也可以显式把某个 `.lnk` 路径作为第一个参数传入，只伪装该快捷方式；
- 快捷方式图标通过 `IShellLink::SetIconLocation` 写入，`wall_icon_*.ico` 生成在目标文件夹（或 exe 目录 / `%TEMP%\HFUFO_icons\`）；「恢复默认图标」会还原文件夹与全部快捷方式。

注意两点：

- Windows 会在快捷方式图标左下角叠加**小箭头**（系统行为，`IconLocation` 无法去除），该角会有轻微可见痕迹；
- 若文件夹与某个快捷方式的**显示名完全相同**（如文件夹 `　` 与 `　.lnk`），程序通过快捷方式图标左下角**小箭头的 overlay 状态位**（`LVM_GETITEMSTATE` / `LVIS_OVERLAYMASK`）自动区分二者：快捷方式图标取快捷方式所在位置的壁纸，`desktop.ini` 取文件夹所在位置的壁纸，互不串位。

状态栏会显示：采样来源（壁纸文件 / 壁纸文件(未校准) / 截屏）+ 本轮匹配到的图标数量（更新 / 无变化）+ 尺寸与位置等。

## 命令行 Command line

```
HideFoldersUsingFolderIcons.exe [目标路径] [--interval <ms>] [--once] [--locate] [--log] [--help]
```

| 参数 | 说明 |
| --- | --- |
| `目标路径` | 文件夹路径或桌面快捷方式（`.lnk`）路径。默认为 exe 所在文件夹；传入文件夹时会自动伪装它本身与桌面上**所有**指向它的快捷方式，传入 `.lnk` 时只伪装该快捷方式 |
| `--interval <ms>` | 刷新间隔，默认 200，范围 50–60000（动态壁纸建议 100–500） |
| `--once` | 只跑一轮采样并自验证后退出（调试用；控制台输出结论） |
| `--locate` | 只定位目标图标并观察其位置是否稳定，不做任何修改（调试用） |
| `--log` | 写日志到 `%TEMP%\HFUFO_log.txt`（`--once` 自动开启） |
| `--help` | 弹出用法说明 |

测试示例：

```powershell
HideFoldersUsingFolderIcons.exe ("C:\Users\Administrator\Desktop\" + [char]0x3000) --once
```

## 原理 How it works

1. **定位图标**：在 explorer 桌面视图（`Progman/WorkerW → SHELLDLL_DefView → SysListView32`，以及桌面整理工具的列表视图）里，用跨进程 `VirtualAllocEx` + `LVM_*` 消息按显示名找到**所有**匹配项（文件夹本身 + 每个指向它的快捷方式），逐一取其图标矩形（`LVM_GETITEMRECT` / `LVIR_ICON`）；同名项（文件夹 vs 快捷方式）再以 **overlay 状态位**（`LVM_GETITEMSTATE`，快捷方式带小箭头 overlay）区分，确保各自采样各自位置。
2. **采样背景（两种方式，自动选择）**：
   - **壁纸文件直读（优先）**：通过 `IDesktopWallpaper` 拿到当前壁纸文件与铺放方式（FILL / FIT / STRETCH / CENTER / TILE / SPAN），WIC 解码后按面积加权盒式滤波直接采样目标矩形对应的壁纸区域。**桌面被窗口完全遮挡时也能照常出图**；纯色桌面取 `IDesktopWallpaper::GetBackgroundColor`。可见时会用一次真实截图自动**校准**（与文件采样逐像素比对，差异 < 4% 才信任文件模式；壁纸被换或动态壁纸引擎接管时自动回退截图模式）。
   - **截图回退**：`LVM_SETITEMPOSITION32` 把该图标暂时移出屏幕 → 等待约 60ms 重绘 → `BitBlt` 截取该矩形（此时就是纯背景，包含壁纸引擎实际渲染的画面）→ 把图标移回原位（移回后会校验位置，被桌面整理工具重排时自动纠正）。采样前会先检测目标位置是否被其它窗口遮挡；遮挡且文件模式不可用时本轮跳过、绝不把遮挡窗口的画面写进图标。
3. **生成图标**：每个图标矩形的采样与上次**逐字节比较**（按矩形分别缓存），有变化才把 32bpp BGRA 数据写成单帧 `.ico`（文件名在 `wall_icon_0..7.ico` 间轮换，规避 shell 图标缓存）。
4. **应用图标（按匹配到的显示名分发）**：
   - **快捷方式**：`IShellLink::SetIconLocation` 修改每个显示名匹配的桌面 `.lnk`，保存后 `SHChangeNotify` 刷新。**桌面整理工具改用快捷方式图标伪装**（部分工具不渲染 desktop.ini 自定义图标，但都会正常渲染 .lnk 的 IconLocation）。
   - **文件夹**：写入 `desktop.ini`（UTF-16）`[.ShellClassInfo] IconResource=<ico>,0`，设置 `desktop.ini` 隐藏+系统属性、文件夹系统属性，并 `SHChangeNotify(SHCNE_UPDATEITEM/UPDATEDIR)` 通知桌面刷新。

生成的 `wall_icon_*.ico` 存放位置：优先目标文件夹；exe 不在桌面时存 exe 所在目录；都不行时落到 `%TEMP%\HFUFO_icons\`。文件夹模式还会写 `desktop.ini`（均在"隐身"文件夹里，不影响观感）。

窗口行为：单实例命名互斥体 + 自定义窗口消息；关闭 = `SW_HIDE`；二次启动 → 唤出已有实例窗口后自身退出。

## 构建 Build

需要 CMake ≥ 3.20 和 MSVC 或 MinGW-w64（Windows 10/11）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

或直接用 CLion 打开本工程构建。产物为 `build/HideFoldersUsingFolderIcons.exe`。

MSVC 命令行示例：

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## 限制与说明 Notes

- 需要 explorer.exe 管理桌面；若「显示桌面图标」被关闭，将无法定位目标（状态栏会提示）。
- 多显示器混合 DPI 时坐标换算可能有细微偏差；单屏任意缩放最稳。
- 不建议与桌面「自动排列」同时使用（临时移开图标会触发重排）。
- 程序会向 explorer 进程申请内存以查询桌面图标（与桌面布局备份工具同款技术），个别杀软可能误报，可加白名单。
- 图标刷新偶尔受 shell 图标缓存影响，如未立刻生效可按 F5 刷新桌面。
- 首次写入 `desktop.ini` 时，个别桌面整理工具可能把图标重排到新位置；程序下一轮会自动在新位置重新伪装，无需干预。
- 快捷方式模式：左下角的系统小箭头无法去除；壁纸直读模式在动态壁纸引擎（Wallpaper Engine 等）渲染时会自动回退截图模式。
- 「隐身」只是视觉层面：地址栏输入路径、搜索、dir 都能看到该文件夹。

## License

[GPL-3.0](LICENSE)
