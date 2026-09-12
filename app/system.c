#include "native.h"
static WCHAR testDirectory[MAX_PATH];
/* Append only local, fixed status messages and rotate at one megabyte. */
void write_log(const WCHAR *message) {
    WCHAR path[MAX_PATH], backup[MAX_PATH], line[700];
    SYSTEMTIME now;
    HANDLE f;
    DWORD n;
    char utf[2800];
    int size;
    data_path(path, L"CampusLogin.log");
    f = CreateFileW(path, FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSize(f, NULL) > 1048576) {
        CloseHandle(f);
        swprintf_s(backup, MAX_PATH, L"%s.previous", path);
        if (!MoveFileExW(path, backup, MOVEFILE_REPLACE_EXISTING))
            return;
        f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                        NULL);
        if (f == INVALID_HANDLE_VALUE)
            return;
    }
    GetLocalTime(&now);
    swprintf_s(line, 700, L"%04u-%02u-%02u %02u:%02u:%02u %s\r\n", now.wYear, now.wMonth, now.wDay, now.wHour,
               now.wMinute, now.wSecond, message);
    size = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf, sizeof(utf), NULL, NULL);
    if (size > 1)
        WriteFile(f, utf, size - 1, &n, NULL);
    CloseHandle(f);
}
/* Resolve only fixed app-owned filenames under the current user's local data folder. */
void data_path(WCHAR *out, const WCHAR *name) {
    if (testDirectory[0]) {
        swprintf_s(out, MAX_PATH, L"%s\\%s", testDirectory, name);
        return;
    }
    WCHAR root[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, root);
    swprintf_s(out, MAX_PATH, L"%s\\CampusNetworkLogin", root);
    CreateDirectoryW(out, NULL);
    StringCchCatW(out, MAX_PATH, L"\\");
    StringCchCatW(out, MAX_PATH, name);
}
/* Round-trip fictional credentials in a fresh temporary directory and leave real configuration untouched. */
BOOL system_tests(void) {
    WCHAR root[MAX_PATH], path[MAX_PATH];
    Config a = {0}, b = {0};
    BOOL ok = FALSE;
    HANDLE f;
    DWORD n;
    BYTE invalid[20] = {0};
    if (!GetTempPathW(MAX_PATH, root) || !GetTempFileNameW(root, L"nsu", 0, testDirectory))
        return FALSE;
    if (!DeleteFileW(testDirectory) || !CreateDirectoryW(testDirectory, NULL))
        goto done;
    StringCchCopyW(a.account, 256, L"fictional-account");
    StringCchCopyW(a.password, 256, L"fictional-password");
    StringCchCopyW(a.provider, 16, L"移动");
    if (!config_io(&a, TRUE) || !config_io(&b, FALSE) || memcmp(&a, &b, sizeof(a)))
        goto done;
    StringCchCopyW(a.password, 256, L"updated-password");
    if (!config_io(&a, TRUE) || !config_io(&b, FALSE) || memcmp(&a, &b, sizeof(a)))
        goto done;
    data_path(path, L"config.native");
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        goto done;
    WriteFile(f, invalid, sizeof(invalid), &n, NULL);
    CloseHandle(f);
    ok = !config_io(&b, FALSE);
    ok = remove_config() && remove_config() && ok;
done:
    data_path(path, L"config.native");
    DeleteFileW(path);
    data_path(path, L"config.native.tmp");
    DeleteFileW(path);
    RemoveDirectoryW(testDirectory);
    testDirectory[0] = 0;
    SecureZeroMemory(&a, sizeof(a));
    SecureZeroMemory(&b, sizeof(b));
    return ok;
}
/* Save a DPAPI-protected record atomically, or validate and decrypt the existing native record. */
BOOL config_io(Config *c, BOOL save) {
    WCHAR path[MAX_PATH], temp[MAX_PATH];
    HANDLE f = INVALID_HANDLE_VALUE;
    DATA_BLOB in = {0}, out = {0};
    DWORD n, size;
    BOOL ok = FALSE;
    data_path(path, L"config.native");
    swprintf_s(temp, MAX_PATH, L"%s.tmp", path);
    if (save) {
        c->version = 1;
        in.pbData = (BYTE *)c;
        in.cbData = sizeof(*c);
        if (!CryptProtectData(&in, L"NSU native config", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out))
            goto done;
        f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE)
            goto done;
        ok = WriteFile(f, out.pbData, out.cbData, &n, NULL) && n == out.cbData && FlushFileBuffers(f);
        CloseHandle(f);
        f = INVALID_HANDLE_VALUE;
        if (ok)
            ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok)
            DeleteFileW(temp);
    } else {
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                        NULL);
        if (f == INVALID_HANDLE_VALUE)
            goto done;
        size = GetFileSize(f, NULL);
        if (size < 16 || size > 16384)
            goto done;
        in.pbData = calloc(size, 1);
        in.cbData = size;
        if (!in.pbData)
            goto done;
        if (!ReadFile(f, in.pbData, size, &n, NULL) || n != size ||
            !CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out) ||
            out.cbData != sizeof(*c))
            goto done;
        memcpy(c, out.pbData, sizeof(*c));
        ok = c->version == 1 && c->account[0] && c->password[0] && !c->account[255] && !c->password[255] &&
             !c->provider[15] && !c->package[511];
        if (ok)
            ok = !wcscmp(c->provider, L"移动") || !wcscmp(c->provider, L"联通") ||
                 !wcscmp(c->provider, L"电信");
        if (!ok)
            SecureZeroMemory(c, sizeof(*c));
    }
done:
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    if (!save && in.pbData) {
        SecureZeroMemory(in.pbData, in.cbData);
        free(in.pbData);
    }
    if (out.pbData) {
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
    }
    return ok;
}
/* Keep the latest task failure per thread so background status reads cannot erase it. */
static __declspec(thread) HRESULT taskError;
static __declspec(thread) const WCHAR *taskStage;

/* Format an actionable task failure without account data, XML or credentials. */
void task_failure(WCHAR *out, size_t count) {
    StringCchPrintfW(out, count, L"自启动操作失败：%s（0x%08lX）。请检查任务计划程序或安全软件的拦截记录。",
                     taskStage ? taskStage : L"未知阶段", (unsigned long)taskError);
}
/* Escape executable paths before embedding them in task XML. */
static BOOL xml_escape(const WCHAR *s, WCHAR *out, size_t cap) {
    out[0] = 0;
    while (*s) {
        WCHAR one[2] = {*s++, 0};
        const WCHAR *piece = one;
        switch (one[0]) {
        case L'&':
            piece = L"&amp;";
            break;
        case L'<':
            piece = L"&lt;";
            break;
        case L'>':
            piece = L"&gt;";
            break;
        case L'"':
            piece = L"&quot;";
            break;
        }
        if (FAILED(StringCchCatW(out, cap, piece)))
            return FALSE;
    }
    return TRUE;
}
/* Distinguish a missing task/path from access errors; absence is already the desired uninstall state. */
static BOOL missing(HRESULT h) {
    return h == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || h == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}
/* Query one exact task and release the returned COM object without exposing its contents. */
static int lookup_task(ITaskFolder *folder, const WCHAR *name) {
    BSTR taskName = SysAllocString(name);
    IRegisteredTask *task = NULL;
    HRESULT h = taskName ? ITaskFolder_GetTask(folder, taskName, &task) : E_OUTOFMEMORY;
    if (task)
        IRegisteredTask_Release(task);
    SysFreeString(taskName);
    if (FAILED(h) && !missing(h))
        taskError = h;
    return SUCCEEDED(h) ? 1 : (missing(h) ? 0 : -1);
}
/* Delete a fixed file and confirm absence; missing files count as successfully removed. */
static BOOL remove_file(const WCHAR *path) {
    if (!DeleteFileW(path) && !missing(HRESULT_FROM_WIN32(GetLastError())))
        return FALSE;
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
        return FALSE;
    return missing(HRESULT_FROM_WIN32(GetLastError()));
}
/* Remove only the current and legacy account files after the user chooses to delete configuration. */
BOOL remove_config(void) {
    WCHAR path[MAX_PATH];
    BOOL ok;
    data_path(path, L"config.native");
    ok = remove_file(path);
    data_path(path, L"config.clixml");
    return remove_file(path) && ok;
}
/* Read the persisted task back: verify enabled state, executable, argument and both trigger kinds. */
static BOOL verify_task(ITaskFolder *folder, BSTR name, const WCHAR *exe, BOOL disabled) {
    IRegisteredTask *task = NULL;
    ITaskDefinition *definition = NULL;
    IActionCollection *actions = NULL;
    IAction *action = NULL;
    IExecAction *exec = NULL;
    ITriggerCollection *triggers = NULL;
    ITrigger *trigger = NULL;
    BSTR path = NULL, arguments = NULL;
    LONG count = 0;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    BOOL ok = FALSE, logon = FALSE, unlock = FALSE;
    HRESULT h = ITaskFolder_GetTask(folder, name, &task);
    if (FAILED(h))
        goto done;
    h = IRegisteredTask_get_Enabled(task, &enabled);
    if (FAILED(h) || (enabled != VARIANT_FALSE) == disabled)
        goto done;
    h = IRegisteredTask_get_Definition(task, &definition);
    if (FAILED(h))
        goto done;
    h = ITaskDefinition_get_Actions(definition, &actions);
    if (FAILED(h))
        goto done;
    h = IActionCollection_get_Count(actions, &count);
    if (FAILED(h) || count != 1)
        goto done;
    h = IActionCollection_get_Item(actions, 1, &action);
    if (FAILED(h))
        goto done;
    h = IAction_QueryInterface(action, &IID_IExecAction, (void **)&exec);
    if (FAILED(h))
        goto done;
    h = IExecAction_get_Path(exec, &path);
    if (FAILED(h) || !path || _wcsicmp(path, exe))
        goto done;
    h = IExecAction_get_Arguments(exec, &arguments);
    if (FAILED(h) || !arguments || wcscmp(arguments, L"--startup"))
        goto done;
    h = ITaskDefinition_get_Triggers(definition, &triggers);
    if (FAILED(h))
        goto done;
    h = ITriggerCollection_get_Count(triggers, &count);
    if (FAILED(h) || count != 2)
        goto done;
    for (LONG i = 1; i <= count; i++) {
        TASK_TRIGGER_TYPE2 type;
        h = ITriggerCollection_get_Item(triggers, i, &trigger);
        if (FAILED(h))
            goto done;
        h = ITrigger_get_Type(trigger, &type);
        if (FAILED(h))
            goto done;
        if (type == TASK_TRIGGER_LOGON)
            logon = TRUE;
        if (type == TASK_TRIGGER_SESSION_STATE_CHANGE) {
            ISessionStateChangeTrigger *session = NULL;
            TASK_SESSION_STATE_CHANGE_TYPE change;
            h = ITrigger_QueryInterface(trigger, &IID_ISessionStateChangeTrigger, (void **)&session);
            if (FAILED(h))
                goto done;
            h = ISessionStateChangeTrigger_get_StateChange(session, &change);
            ISessionStateChangeTrigger_Release(session);
            if (FAILED(h))
                goto done;
            unlock = change == TASK_SESSION_UNLOCK;
        }
        ITrigger_Release(trigger);
        trigger = NULL;
    }
    ok = logon && unlock;
done:
    if (!ok)
        taskError = FAILED(h) ? h : E_FAIL;
    if (trigger)
        ITrigger_Release(trigger);
    if (triggers)
        ITriggerCollection_Release(triggers);
    if (exec)
        IExecAction_Release(exec);
    if (action)
        IAction_Release(action);
    if (actions)
        IActionCollection_Release(actions);
    if (definition)
        ITaskDefinition_Release(definition);
    if (task)
        IRegisteredTask_Release(task);
    SysFreeString(path);
    SysFreeString(arguments);
    return ok;
}
/* Run a task operation; mode 3 queries, mode 2 validates, and testName isolates lifecycle tests. */
static int task_operation(int install, const WCHAR *testName) {
    ITaskService *service = NULL;
    ITaskFolder *folder = NULL;
    IRegisteredTask *registered = NULL;
    VARIANT empty, user;
    HRESULT h = E_FAIL;
    HANDLE token = NULL;
    BYTE buf[512];
    DWORD size;
    LPWSTR sid = NULL;
    BSTR root = NULL, taskName = NULL, taskXml = NULL;
    WCHAR name[160], exe[MAX_PATH], escaped[1600], xml[8192], link[MAX_PATH];
    int ok = install == 3 ? -1 : FALSE;
    VariantInit(&empty);
    VariantInit(&user);
    taskError = S_OK;
    taskStage = L"读取当前用户";
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) ||
        !GetTokenInformation(token, TokenUser, buf, sizeof(buf), &size) ||
        !ConvertSidToStringSidW(((TOKEN_USER *)buf)->User.Sid, &sid)) {
        h = HRESULT_FROM_WIN32(GetLastError());
        goto done;
    }
    swprintf_s(name, 160, L"NSU-net-autologin-%s", sid);
    if (testName)
        StringCchCopyW(name, 160, testName);
    root = SysAllocString(L"\\");
    taskName = SysAllocString(name);
    user.vt = VT_BSTR;
    user.bstrVal = SysAllocString(sid);
    if (!root || !taskName || !user.bstrVal) {
        h = E_OUTOFMEMORY;
        goto done;
    }
    taskStage = L"连接任务计划程序";
    h = CoCreateInstance(&CLSID_TaskScheduler, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskService,
                         (void **)&service);
    if (FAILED(h))
        goto done;
    h = ITaskService_Connect(service, empty, empty, empty, empty);
    if (FAILED(h))
        goto done;
    h = ITaskService_GetFolder(service, root, &folder);
    if (FAILED(h))
        goto done;
    taskStage = L"读取自启动任务";
    if (install == 3) {
        ok = lookup_task(folder, name);
        if (ok >= 0 && !testName) {
            h = SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, link);
            if (FAILED(h)) {
                ok = -1;
                goto done;
            }
            StringCchCatW(link, MAX_PATH, L"\\CampusNetworkLogin.lnk");
            if (GetFileAttributesW(link) != INVALID_FILE_ATTRIBUTES)
                ok = 1;
            else {
                h = HRESULT_FROM_WIN32(GetLastError());
                if (!missing(h))
                    ok = -1;
                else
                    h = S_OK;
            }
        }
    } else if (!install) {
        int state = lookup_task(folder, name);
        if (state < 0) {
            h = taskError;
            goto done;
        }
        taskStage = L"删除自启动任务";
        if (state == 1) {
            h = ITaskFolder_DeleteTask(folder, taskName, 0);
            if (FAILED(h) && !missing(h))
                goto done;
        }
        taskStage = L"核对卸载结果";
        ok = lookup_task(folder, name) == 0;
    } else {
        DWORD length = GetModuleFileNameW(NULL, exe, MAX_PATH);
        taskStage = L"准备自启动任务";
        if (!length || length >= MAX_PATH || !xml_escape(exe, escaped, 1600)) {
            h = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            goto done;
        }
        swprintf_s(
            xml, 8192,
            L"<?xml version=\"1.0\"?><Task version=\"1.2\" "
            L"xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/"
            L"task\"><Triggers><LogonTrigger><Enabled>true</Enabled><UserId>%s</UserId></"
            L"LogonTrigger><SessionStateChangeTrigger><Enabled>true</Enabled><StateChange>SessionUnlock</"
            L"StateChange><UserId>%s</UserId></SessionStateChangeTrigger></Triggers><Principals><Principal "
            L"id=\"Author\"><UserId>%s</UserId><LogonType>InteractiveToken</"
            L"LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></"
            L"Principals><Settings><MultipleInstancesPolicy>IgnoreNew</"
            L"MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</"
            L"DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</"
            L"StopIfGoingOnBatteries><StartWhenAvailable>true</StartWhenAvailable><ExecutionTimeLimit>PT6M</"
            L"ExecutionTimeLimit></Settings><Actions "
            L"Context=\"Author\"><Exec><Command>%s</Command><Arguments>--startup</Arguments></Exec></"
            L"Actions></Task>",
            sid, sid, sid, escaped);

        taskXml = SysAllocString(xml);
        if (!taskXml) {
            h = E_OUTOFMEMORY;
            goto done;
        }
        taskStage = install == 2 ? L"校验自启动任务" : L"注册自启动任务";
        /* Use typed COM parameters and SDK constants instead of late-bound reversed VARIANT arrays. */
        h = ITaskFolder_RegisterTask(folder, taskName, taskXml,
                                     install == 2 ? TASK_VALIDATE_ONLY
                                                  : (TASK_CREATE_OR_UPDATE | (testName ? TASK_DISABLE : 0)),
                                     user, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &registered);
        if (FAILED(h))
            goto done;
        ok = TRUE;
        if (install != 2) {
            taskStage = L"核对注册结果";
            ok = verify_task(folder, taskName, exe, testName != NULL);
        }
    }
    if (ok > 0 && install != 2 && install != 3 && !testName) {
        taskStage = L"清理旧版启动快捷方式";
        h = SHGetFolderPathW(NULL, CSIDL_STARTUP, NULL, 0, link);
        if (FAILED(h)) {
            ok = FALSE;
            goto done;
        }
        StringCchCatW(link, MAX_PATH, L"\\CampusNetworkLogin.lnk");
        if (!remove_file(link)) {
            ok = FALSE;
            h = HRESULT_FROM_WIN32(GetLastError());
        }
    }
done:
    if (install == 3 ? ok < 0 : !ok) {
        if (FAILED(h))
            taskError = h;
        else if (SUCCEEDED(taskError))
            taskError = E_FAIL;
    }
    if ((testName || install == 2) && (install == 3 ? ok < 0 : !ok))
        fprintf(stderr, "Task operation %d failed (HRESULT 0x%08lx).\n", install, (unsigned long)taskError);
    if (registered)
        IRegisteredTask_Release(registered);
    if (folder)
        ITaskFolder_Release(folder);
    if (service)
        ITaskService_Release(service);
    VariantClear(&user);
    SysFreeString(root);
    SysFreeString(taskName);
    SysFreeString(taskXml);
    if (sid)
        LocalFree(sid);
    if (token)
        CloseHandle(token);
    return ok;
}
/* Read the actual current-user task/legacy shortcut state: 1 present, 0 absent, -1 unknown. */
int task_status(void) { return task_operation(3, NULL); }
/* Register or uninstall the app's current-user startup entry and verify the resulting state. */
BOOL task_change(BOOL install) { return task_operation(install, NULL) != 0; }
/* Test create/query/delete/delete-again using a disabled GUID task, never the real startup entry. */
BOOL task_tests(void) {
    GUID id;
    WCHAR guid[40], name[160];
    BOOL ok;
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(&id, guid, 40))
        return FALSE;
    swprintf_s(name, 160, L"NSU-native-test-%s", guid);
    if (task_operation(3, name) != 0)
        return FALSE;
    ok = task_operation(1, name) == 1;
    if (ok)
        ok = task_operation(3, name) == 1;
    if (ok)
        ok = task_operation(1, name) == 1; /* Re-registration must update an existing task. */
    if (!task_operation(0, name))
        return FALSE;
    return task_operation(3, name) == 0 && task_operation(0, name) == 1 && ok;
}
