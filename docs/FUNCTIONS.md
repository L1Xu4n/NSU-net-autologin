# 代码导读

原生 EXE 由 `app/main.c`（窗口）、`app/network.c`（校园网协议）和 `app/system.c`（配置/自启）组成；`native.h` 声明共享结构和函数，`version.rc` 保存 EXE 版本信息。每个函数前都有用途注释。

## 原生 C 函数

| 函数 | 作用 |
| --- | --- |
| wWinMain | Windows 程序入口，区分主菜单、自动连接、自测及明确请求的 --install 修复入口；安装不发起校园网登录 |
| px | 按当前界面缩放比例转换坐标、间距和字体大小 |
| control | 创建带统一字体的按钮、文本框等控件 |
| draw_button | 绘制圆角按钮、主要操作颜色和键盘焦点框，保留原生按钮行为 |
| paint_text | 为主窗口及配置窗口绘制统一背景和文字颜色 |
| set_status | 同时更新状态正文与成功/失败颜色 |
| refresh_state | 读取实际配置和自启状态，切换注册/卸载按钮并提示下一步 |
| main_proc | 处理主窗口点击、后台完成消息与退出 |
| begin | 启动一个后台工作线程，禁用重复操作 |
| operate | 加互斥锁，执行连接、保存配置、注册或卸载；注册失败显示具体阶段和错误码 |
| configure | 打开配置窗口，创建失败时恢复主窗口 |
| config_proc | 读取配置表单、验证输入、保留空密码或取消编辑 |
| notify_message | 测量文字并显示完整提示；等待/登录中为蓝色且持续显示，结果按绿色或红色定时关闭 |
| notify_result | 后台操作结束后显示最终结果，避免读取仍在修改中的结果文本 |
| report_network_progress | 后台线程只发送检查、等待、网络就绪的阶段编号，不直接操作窗口 |
| show_network_progress | 在界面线程更新状态栏和右下角进度，使用独立的固定文本 |
| about_proc | 显示作者和可复制的仓库地址，只在用户点击时打开固定 GitHub 地址 |
| toast_proc | 处理提示关闭、计时器及用户点击验证网页 |
| preview | 仅渲染测试窗口到图片，不截图桌面或私人内容 |
| inside | 验证整个按钮或文字控件都位于父窗口的客户区内 |
| ui_test | 使用虚构状态验证三按钮顺序、自启状态切换、颜色和两种缩放下电话验证布局 |
| window | 根据内容区尺寸计算完整窗口尺寸，防止标题栏挤占按钮空间 |
| dispose_ui | 窗口退出后释放字体与画刷 |
| data_path | 获取当前用户下的固定数据文件路径；自测使用隔离临时目录 |
| write_log | 仅记录程序固定提示，超过 1 MB 时轮转 |
| config_io | DPAPI 加密整份配置，原子保存，读取时验证格式 |
| system_tests | 在隔离临时目录验证配置保存、更新、损坏拒绝与重复删除 |
| task_failure | 显示失败阶段和 HRESULT 错误码，不输出账号或任务 XML |
| verify_task | 回读已保存任务，核对启用状态、EXE 路径、启动参数、登录和解锁触发器 |
| xml_escape | 转义程序路径中的 XML 特殊字符 |
| task_change | 注册/删除当前用户任务；测试值 2 只验证 XML，不注册任务 |
| missing | 判断文件或路径是否已经不存在，不把权限错误当成不存在 |
| lookup_task | 通过原生 ITaskFolder 查询任务，区分存在、不存在和读取失败 |
| remove_file | 删除固定文件后再次检查，已经不存在也算成功 |
| remove_config | 在用户选择删除时移除新旧两份配置，并验证结果 |
| task_operation | 使用 Windows 原生强类型任务接口注册、查询、卸载，记录失败阶段并回读验证 |
| task_status | 查询当前用户的正式计划任务和旧版启动快捷方式 |
| task_tests | 使用禁用的 GUID 临时任务检查注册、重复注册更新、保存内容、卸载和重复卸载；最后清理 |
| space | 跳过 JSON 合法空白 |
| string_value | 解码 JSON 字符串和转义，拒绝无效内容 |
| value | 在深度和节点限制内解析 JSON 树 |
| integer | 只接受范围内的非负整数，拒绝小数和溢出 |
| parse | 校验 UTF-8 和完整 JSON 对象 |
| prop | 查找唯一属性，重复字段不擅自选择 |
| str | 只读取字符串类型字段 |
| success | 只接受 JSON 布尔 true |
| phone | 清理 HTML/空白与数字实体，识别电话验证要求 |
| quote | 将字符串安全编码为 JSON 字符串 |
| encode_password | 使用 CryptoAPI 生成网页协议所需的 DES/ECB/PKCS7 编码 |
| http_request | 请求固定校园网地址，禁用代理/重定向并限制大小 |
| request | 正常运行使用真实 HTTP，自测使用固定响应队列 |
| pause_ms | 正常运行等待，自测推进虚拟时钟以快速覆盖三分钟超时 |
| network_ticks | 正常运行使用单调时钟，自测使用虚拟时间 |
| record_progress | 离线测试记录阶段序列，核验检查、等待、自动继续及超时 |
| online | 检查接口的本机 IP 是否存在于在线设备列表 |
| package_name | 唯一匹配套餐，同时检查设备数限制 |
| connect_network | 执行登录协议及最终上线确认，通过可选回调发送网络进度；等待时仅重试 Check |
| scenario | 对照离线请求序列和结果，检查中断后没有额外请求 |
| native_tests | 运行 JSON、编码、套餐及登录/验证模拟测试 |

任务 XML 测试使用微软定义的 [TASK_VALIDATE_ONLY](https://learn.microsoft.com/en-us/windows/win32/api/taskschd/ne-taskschd-task_creation)，不会注册或覆盖任务。新版直接调用 `ITaskService`、`ITaskFolder`，使用 SDK 定义的 `TASK_LOGON_INTERACTIVE_TOKEN` 和 `TASK_CREATE_OR_UPDATE` 等常量，不再组装逆序的自动化参数数组。生命周期测试额外使用 `TASK_DISABLE`，任务名称为新生成的 GUID，不会自动执行登录。

## 构建与测试

旧版 PowerShell 登录实现、CMD 入口和对应测试已从当前源码中移除。保留的 PowerShell 文件仅用于开发构建与自动化测试，不进入发布 ZIP，程序运行时也不加载它们。

`scripts/Build-Exe.ps1` 使用 MSVC C 编译器和 `app/version.rc` 生成带版本信息的原生 EXE，并清理编译中间文件。`scripts/Build-Release.ps1` 只打包主 EXE，并生成 ZIP 的 SHA256 校验文件。两个脚本没有自定义函数。

`tests/Test-Exe.ps1` 验证版本号、原生 PE、DES 向量、协议/重试模拟、DPAPI、禁用临时任务的完整生命周期及窗口预览，并比较前后正式任务 XML。测试结束清理自己的临时文件和任务。

## 初学者需要理解的三个知识点

1. 浏览器按钮背后是网络请求；协议明确后，可以直接请求接口。
2. 本地加密存储与网络传输保护是两回事；DPAPI 不会把 HTTP 自动变成 HTTPS。
3. 代码、私人配置与生成的发布包分别保存；`.gitignore` 只是防止误提交，发布前仍需检查实际提交文件。
