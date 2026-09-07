# 本文件只定义函数；加载它不会登录、弹窗或修改开机启动。

# 记录运行结果，不记录用户名、密码、密文、Cookie 或接口完整响应。
function Write-CampusLog([string]$Message) {
    [void][System.IO.Directory]::CreateDirectory($script:CampusDataDir)
    $path = Join-Path $script:CampusDataDir 'CampusLogin.log'
    if ((Test-Path -LiteralPath $path) -and (Get-Item -LiteralPath $path).Length -gt 1048576) {
        Move-Item -LiteralPath $path -Destination ($path + '.previous') -Force
    }
    Write-Host $Message
    Add-Content -LiteralPath $path -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $Message) -Encoding UTF8
}

# 读取当前用户的配置；PSCredential 的密码由 Windows DPAPI 解密。
function Read-CampusConfig {
    $path = Join-Path $script:CampusDataDir 'config.clixml'
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $config = Import-Clixml -LiteralPath $path
    if ($config.Version -ne 2 -or $config.Operator -notin @('移动','联通','电信') -or
        $config.PackageName -isnot [string] -or $config.Credential -isnot [System.Management.Automation.PSCredential] -or
        [string]::IsNullOrWhiteSpace($config.Credential.UserName) -or $config.Credential.Password.Length -eq 0) {
        throw '配置无效，请双击 Configure.cmd 重新配置。'
    }
    return $config
}

# 原子保存配置，避免保存中断破坏原配置；密码不写入明文 JSON。
function Save-CampusConfig($Config) {
    [void][System.IO.Directory]::CreateDirectory($script:CampusDataDir)
    $path = Join-Path $script:CampusDataDir 'config.clixml'
    $temporary = Join-Path $script:CampusDataDir ([guid]::NewGuid().ToString() + '.tmp')
    try {
        $Config | Export-Clixml -LiteralPath $temporary -Encoding UTF8 -Depth 5
        if (Test-Path -LiteralPath $path) {
            # Windows PowerShell 会把传给该重载的 null 转为空路径，使用明确的临时备份名。
            $backup = $temporary + '.previous'
            [System.IO.File]::Replace($temporary, $path, $backup)
            Remove-Item -LiteralPath $backup
        }
        else { [System.IO.File]::Move($temporary, $path) }
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
    }
}

# 向配置窗口添加一组标题与文本框，返回文本框供后续读取。
function Add-CampusField($Form, [string]$Label, [int]$Top, [bool]$Password = $false) {
    $caption = [System.Windows.Forms.Label]::new()
    $caption.Text = $Label
    $caption.SetBounds(24, $Top, 410, 24)
    [void]$Form.Controls.Add($caption)
    $field = [System.Windows.Forms.TextBox]::new()
    $field.SetBounds(24, ($Top + 26), 410, 28)
    $field.UseSystemPasswordChar = $Password
    [void]$Form.Controls.Add($field)
    return $field
}

# 首次运行或修改设置时显示本地窗口；取消不改配置，编辑时密码留空则保留。
function Show-CampusConfig {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [System.Windows.Forms.Application]::EnableVisualStyles()
    $existing = $null
    try { $existing = Read-CampusConfig } catch { }
    $form = [System.Windows.Forms.Form]::new()
    $form.Text = '校园网自动登录 · 配置'
    $form.ClientSize = [System.Drawing.Size]::new(465, 470)
    $form.StartPosition = 'CenterScreen'
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.MinimizeBox = $false
    $form.Font = [System.Drawing.Font]::new('Microsoft YaHei UI', 10)
    $account = Add-CampusField $form '校园网账号' 20
    $passwordTitle = '校园网密码'
    if ($null -ne $existing) { $passwordTitle = '校园网密码（不修改请留空）' }
    $password = Add-CampusField $form $passwordTitle 92 $true
    $caption = [System.Windows.Forms.Label]::new()
    $caption.Text = '运营商（请选择）'
    $caption.SetBounds(24, 164, 410, 24)
    [void]$form.Controls.Add($caption)
    $operator = [System.Windows.Forms.ComboBox]::new()
    $operator.SetBounds(24, 190, 410, 30)
    $operator.DropDownStyle = 'DropDownList'
    [void]$operator.Items.AddRange([object[]]@('移动','联通','电信'))
    [void]$form.Controls.Add($operator)
    $package = Add-CampusField $form '完整套餐名（可选；同运营商有多个套餐时填写）' 236
    $notice = [System.Windows.Forms.Label]::new()
    $notice.SetBounds(24, 310, 415, 70)
    $notice.Text = '密码由当前 Windows 用户加密保存。首次填写后自动登录，无需打开浏览器。以后双击 Configure.cmd 可修改。'
    [void]$form.Controls.Add($notice)
    if ($null -ne $existing) {
        $account.Text = $existing.Credential.UserName
        $operator.SelectedItem = $existing.Operator
        $package.Text = $existing.PackageName
    }
    $save = [System.Windows.Forms.Button]::new()
    $save.Text = '保存'
    $save.SetBounds(222, 405, 100, 36)
    $cancel = [System.Windows.Forms.Button]::new()
    $cancel.Text = '取消'
    $cancel.SetBounds(334, 405, 100, 36)
    $cancel.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
    [void]$form.Controls.Add($save)
    [void]$form.Controls.Add($cancel)
    $form.AcceptButton = $save
    $form.CancelButton = $cancel
    # 只有输入合法并成功写盘后才关闭窗口；事件不会输出敏感数据。
    $save.Add_Click({
        try {
            $name = $account.Text.Trim()
            if ($name.Length -eq 0 -or $operator.SelectedIndex -lt 0) { throw '请填写账号并选择运营商。' }
            if ($password.Text.Length -gt 0) { $secret = ConvertTo-SecureString $password.Text -AsPlainText -Force }
            elseif ($null -ne $existing -and $name -eq $existing.Credential.UserName) { $secret = $existing.Credential.Password }
            else { throw '首次配置或更换账号时，请输入密码。' }
            $value = [pscustomobject]@{ Version = 2; Operator = [string]$operator.SelectedItem; PackageName = $package.Text.Trim(); Credential = [pscredential]::new($name, $secret) }
            Save-CampusConfig $value
            $form.Tag = $value
            $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
            $form.Close()
        } catch {
            [void][System.Windows.Forms.MessageBox]::Show('未保存：' + $_.Exception.Message, '配置提示')
        }
    })
    try {
        if ($form.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $form.Tag }
        return $null
    } finally {
        $password.Clear()
        $form.Dispose()
    }
}

# 复现网页的 CryptoJS DES/ECB/PKCS7：UTF-8 密钥取前八字节，输出小写十六进制。
function ConvertTo-WebCCPassword([pscredential]$Credential) {
    $name = $Credential.UserName
    $suffix = $name.Substring([Math]::Max(0, $name.Length - 4))
    $bytes = [Text.Encoding]::UTF8.GetBytes($suffix + $name + '12345678')
    $key = [byte[]]$bytes[0..7]
    $des = [Security.Cryptography.DES]::Create()
    $plain = $null
    $transform = $null
    try {
        $des.Mode = [Security.Cryptography.CipherMode]::ECB
        $des.Padding = [Security.Cryptography.PaddingMode]::PKCS7
        $des.Key = $key
        $plain = [Text.Encoding]::UTF8.GetBytes($Credential.GetNetworkCredential().Password)
        $transform = $des.CreateEncryptor()
        $encrypted = $transform.TransformFinalBlock($plain, 0, $plain.Length)
        return [BitConverter]::ToString($encrypted).Replace('-', '').ToLowerInvariant()
    } finally {
        if ($null -ne $plain) { [Array]::Clear($plain, 0, $plain.Length) }
        [Array]::Clear($key, 0, $key.Length)
        [Array]::Clear($bytes, 0, $bytes.Length)
        if ($null -ne $transform) { $transform.Dispose() }
        $des.Dispose()
    }
}

# 创建不走系统代理的独立 HTTP 会话；只连接已核实的校园网服务，不跨站重定向。
function New-WebCCClient {
    Add-Type -AssemblyName System.Net.Http
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $handler.UseProxy = $false
    $handler.AllowAutoRedirect = $false
    $handler.CookieContainer = [System.Net.CookieContainer]::new()
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(8)
    return $client
}

# 调用已核实的 WebCC JSON 接口并保留会话 Cookie；禁止下线、改密等额外操作。
function Invoke-WebCC($Client, [string]$Action, [hashtable]$Fields = @{}) {
    if ($Action -notin @('Check','Login','GetInfo','OpenNet','ReConnect')) { throw '不支持的校园网操作。' }
    $body = @{ DoWhat = $Action }
    foreach ($key in $Fields.Keys) {
        if ($key -eq 'DoWhat') { throw '请求参数无效。' }
        $body[$key] = $Fields[$key]
    }
    $content = [System.Net.Http.StringContent]::new(($body | ConvertTo-Json -Compress), [Text.Encoding]::UTF8, 'application/json')
    $response = $null
    try {
        $response = $Client.PostAsync('http://2.2.2.2/Auth.ashx', $content).GetAwaiter().GetResult()
        if (-not $response.IsSuccessStatusCode) { throw ('校园网 HTTP 请求失败：' + [int]$response.StatusCode) }
        $json = $response.Content.ReadAsStringAsync().GetAwaiter().GetResult()
        $value = ConvertFrom-Json -InputObject $json
        if ($null -eq $value -or $null -eq $value.PSObject.Properties['Result']) { throw '校园网返回了无法识别的响应。' }
        return $value
    } finally {
        $content.Dispose()
        if ($null -ne $response) { $response.Dispose() }
        $body.Clear()
    }
}

# 只接受 JSON 布尔 true，避免 PowerShell 把数字或字符串误判为成功。
function Test-WebCCSuccess($Response) {
    # PowerShell 会把数字与布尔值隐式转换；接口成功必须是 JSON 布尔 true。
    return ($null -ne $Response -and $Response.Result -is [bool] -and $Response.Result)
}

# 验证在线字段，确保只有本机 IP 出现在在线列表时才算成功。
function Test-WebCCOnline($Data) {
    if ($null -eq $Data -or [string]::IsNullOrWhiteSpace([string]$Data.IP) -or $null -eq $Data.PSObject.Properties['OIA']) {
        throw '校园网在线状态字段缺失，停止操作。'
    }
    return (@($Data.OIA | Where-Object { $_.IP -eq $Data.IP }).Count -gt 0)
}

# 在可用套餐中唯一匹配指定名称或运营商；零个或多个匹配均不擅自选择。
function Select-WebCCPackage($Data, $Config) {
    if ($null -eq $Data.PSObject.Properties['KXTC']) { throw '校园网未返回可用套餐。' }
    $choices = @($Data.KXTC | ForEach-Object { [string]$_.套餐名称 } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if (-not [string]::IsNullOrWhiteSpace($Config.PackageName)) {
        $choices = @($choices | Where-Object { $_ -eq $Config.PackageName })
    } else { $choices = @($choices | Where-Object { $_.Contains($Config.Operator) }) }
    if ($choices.Count -ne 1) { throw '套餐不存在或有多个匹配，请运行 Configure.cmd 修改运营商或完整套餐名。' }
    return $choices[0]
}

# 完成会话检查、至多一次密码登录、套餐开通和在线验证，不主动下线任何设备。
function Invoke-CampusLogin($Client, $Config) {
    $check = Invoke-WebCC $Client 'Check'
    if (-not (Test-WebCCSuccess $check)) {
        if ($check.Result -eq 'needQRLogin') { throw '校园网要求扫码验证，需手动完成后再试。' }
        Write-CampusLog '正在验证校园网账号……'
        $encoded = ConvertTo-WebCCPassword $Config.Credential
        try { $login = Invoke-WebCC $Client 'Login' @{ username = $Config.Credential.UserName; password = $encoded; remember = $false } }
        finally { $encoded = $null }
        if (-not (Test-WebCCSuccess $login)) { throw '账号验证失败。请检查配置中的账号密码，或查看校园网是否要求额外验证。' }
    }
    $info = Invoke-WebCC $Client 'GetInfo'
    if (-not (Test-WebCCSuccess $info)) { throw '无法读取校园网账号状态。' }
    if (Test-WebCCOnline $info.Data) { Write-CampusLog '成功：校园网接口确认本机已上线。'; return }
    if ($null -eq $info.Data.PSObject.Properties['MOC']) { throw '校园网未返回设备数量限制。' }
    if (@($info.Data.OIA).Count -ge [int]$info.Data.MOC) { throw '在线设备数量已达到上限，请先手动处理其他设备。' }
    $package = Select-WebCCPackage $info.Data $Config
    Write-CampusLog '账号验证完成，正在开通所选运营商套餐……'
    $opened = Invoke-WebCC $Client 'OpenNet' @{ Package = $package }
    if ($opened.Result -isnot [bool] -and $opened.Result -eq 192) {
        if ([string]::IsNullOrWhiteSpace([string]$opened.Token)) { throw '校园网设备切换响应缺少令牌。' }
        # 新设备可能需要网页协议中的 ReConnect，最多轮询 60 秒且不写入令牌。
        $limit = [Math]::Min(60, [Math]::Max(1, [int]$opened.Sec))
        $until = [DateTime]::UtcNow.AddSeconds($limit)
        do {
            Start-Sleep -Seconds 1
            $reconnect = Invoke-WebCC $Client 'ReConnect' @{ Token = $opened.Token }
            if (Test-WebCCSuccess $reconnect) { break }
            if ($reconnect.Result -ne 'wait') { throw '校园网设备切换失败，请手动检查连接。' }
        } while ([DateTime]::UtcNow -lt $until)
    } elseif (-not (Test-WebCCSuccess $opened)) { throw '套餐开通失败，请检查套餐配置或校园网账号状态。' }
    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        $info = Invoke-WebCC $Client 'GetInfo'
        if ((Test-WebCCSuccess $info) -and (Test-WebCCOnline $info.Data)) { Write-CampusLog '成功：套餐已开通，本机已上线。'; return }
        Start-Sleep -Seconds 2
    }
    throw '套餐请求已提交，但校园网尚未确认本机上线。请查看状态后再试。'
}

# 创建或移除当前用户的开机启动项；后台运行失败时可查看本地日志。
function Set-CampusStartup([bool]$Enabled, [string]$ScriptPath) {
    $path = Join-Path ([Environment]::GetFolderPath('Startup')) 'CampusNetworkLogin.lnk'
    if (-not $Enabled) {
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
        return
    }
    $shell = New-Object -ComObject WScript.Shell
    $shortcut = $shell.CreateShortcut($path)
    $shortcut.TargetPath = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $shortcut.Arguments = '-NoProfile -STA -WindowStyle Hidden -ExecutionPolicy Bypass -File "' + $ScriptPath + '"'
    $shortcut.WorkingDirectory = Split-Path -Parent $ScriptPath
    $shortcut.WindowStyle = 7
    $shortcut.Save()
}

# 主入口：优先处理配置和开机设置，再带互斥锁等待网络并执行无浏览器登录。
function Start-CampusApp([string]$ScriptPath, [bool]$Configure, [bool]$Install, [bool]$Uninstall, [bool]$CheckOnly) {
    $script:CampusDataDir = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'CampusNetworkLogin'
    $client = $null
    $mutex = [Threading.Mutex]::new($false, 'Local\CampusNetworkLogin_2222')
    $locked = $false
    try {
        $locked = $mutex.WaitOne(0)
        if (-not $locked) { Write-Host '校园网脚本或配置窗口已经在运行。'; return 6 }
        if ($Uninstall) { Set-CampusStartup $false $ScriptPath; Write-CampusLog '已取消开机自动登录，保留原配置。'; return 0 }
        if ($CheckOnly) {
            $client = New-WebCCClient
            $check = Invoke-WebCC $client 'Check'
            Write-CampusLog '检查通过：无需浏览器即可访问校园网 JSON 接口。'
            if (Test-WebCCSuccess $check) {
                $info = Invoke-WebCC $client 'GetInfo'
                if ((Test-WebCCSuccess $info) -and (Test-WebCCOnline $info.Data)) { Write-CampusLog '只读检查：本机已经在线。' }
            }
            return 0
        }
        $config = $null
        try { $config = Read-CampusConfig } catch { Write-CampusLog '配置无法读取，请重新填写。' }
        if ($Configure -or $null -eq $config) {
            $config = Show-CampusConfig
            if ($null -eq $config) { Write-CampusLog '已取消配置，未执行登录。'; return 2 }
            Write-CampusLog '配置已加密保存。'
            if ($Configure) { return 0 }
        }
        if ($Install) { Set-CampusStartup $true $ScriptPath; Write-CampusLog '已启用当前用户的开机自动登录。'; return 0 }
        $client = New-WebCCClient
        Write-CampusLog '正在连接校园网入口，无需打开浏览器……'
        # 仅重试无副作用的 Check；密码错误和套餐开通失败不会重复提交。
        $ready = $false
        for ($attempt = 0; $attempt -lt 12; $attempt++) {
            try { $null = Invoke-WebCC $client 'Check'; $ready = $true; break } catch { }
            Start-Sleep -Seconds 5
        }
        if (-not $ready) { throw '无法连接校园网入口，请确认已连接学校 Wi-Fi 或网线。' }
        Invoke-CampusLogin $client $config
        return 0
    } catch {
        # 外部异常仅记类型，防止网络异常附带敏感请求；自身明确提示可以展示。
        if ($_.Exception -is [Management.Automation.RuntimeException] -and $_.Exception.Message -notmatch 'http|password|username') {
            Write-CampusLog ('失败：' + $_.Exception.Message)
        } else { Write-CampusLog ('运行失败，错误类型：' + $_.Exception.GetType().Name) }
        return 1
    } finally {
        if ($null -ne $client) { $client.Dispose() }
        if ($locked) { $mutex.ReleaseMutex() }
        $mutex.Dispose()
    }
}
