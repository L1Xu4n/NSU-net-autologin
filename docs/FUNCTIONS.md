# 代码导读

`CampusLogin.ps1` 是入口；`CampusLogin.Core.ps1` 只定义函数，加载它不会直接登录、弹窗或修改开机启动。

| 函数 | 作用 |
| --- | --- |
| Write-CampusLog | 保存不含敏感信息的运行日志，超限时轮转 |
| Read-CampusConfig | 读取并校验个人加密配置 |
| Save-CampusConfig | 使用 DPAPI 和原子文件替换保存配置 |
| Add-CampusField | 给本地配置窗口添加标题与输入框 |
| Show-CampusConfig | 显示首次/后续配置窗口并保存有效输入 |
| ConvertTo-WebCCPassword | 按网页协议进行 DES/ECB/PKCS7 编码 |
| New-WebCCClient | 创建固定超时、独立 Cookie、不走代理的 HTTP 会话 |
| Invoke-WebCC | 调用允许的校园网操作并解析 JSON |
| Test-WebCCSuccess | 只接受 JSON 布尔 true，避免隐式类型转换 |
| Test-WebCCOnline | 判断本机 IP 是否出现在在线列表 |
| Select-WebCCPackage | 唯一匹配完整套餐名或运营商 |
| Invoke-CampusLogin | 执行会话检查、登录、套餐开通及在线验证 |
| Show-CampusNotification | 显示不抢焦点、自动关闭的登录结果提示 |
| New-CampusStartupTask | 构建当前用户登录触发、无额外延迟的计划任务定义 |
| Set-CampusStartup | 注册/删除计划任务，并在成功安装后清理旧快捷方式 |
| Start-CampusApp | 处理命令参数、配置、互斥运行、网络等待与异常 |

## 测试辅助函数

`tests/Test-Core.ps1` 中的 `Assert-True` 检查结果；模拟的 `Invoke-WebCC` 只消费预设响应；模拟 `Start-Sleep` 跳过等待；`Set-Scenario` 为每个场景重置响应队列和操作记录。它们不进行真实网络操作。

`tests/make-des-vectors.cjs` 的 `makeVector` 使用相同子密钥的 TripleDES 生成与 DES 等效的独立测试向量，只使用虚构数据。

## 初学者需要理解的三个知识点

1. 浏览器按钮背后是网络请求；协议明确后，可以直接请求接口。
2. 本地加密存储与网络传输保护是两回事；DPAPI 不会把 HTTP 自动变成 HTTPS。
3. 代码、私人配置与生成的发布包分别保存；`.gitignore` 只是防止误提交，发布前仍需检查实际提交文件。
