#pragma once
#define UNICODE
#define _UNICODE
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <commctrl.h>
#include <ctype.h>
#include <wtsapi32.h>

#include <oleauto.h>
#include <taskschd.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strsafe.h>
#include <wchar.h>
#include <wctype.h>
#include <wincrypt.h>
typedef struct {
    DWORD version;
    WCHAR account[256], password[256], provider[16], package[512];
} Config;
typedef struct {
    ULONG index;
    IN_ADDR address;
    WCHAR ip[16], mac[13];
} NetworkAdapter;
typedef struct {
    NetworkAdapter wired, wifi;
    BOOL wiredConnected, wifiConnected;
} NetworkLinks;
typedef struct { int selected, other; } NetworkSessionState;
typedef struct {
    NetworkAdapter adapter;
    char cookies[16][2048];
    int cookieCount;
} PortalTransport;
BOOL network_links(NetworkLinks *links);
BOOL same_adapter(const NetworkAdapter *a, const NetworkAdapter *b);
BOOL portal_post(PortalTransport *transport, const WCHAR *body, char *reply, size_t capacity);
BOOL transport_tests(void);
extern WCHAR errorText[512];
BOOL config_io(Config *c, BOOL save);
BOOL task_change(BOOL install);
void task_failure(WCHAR *out, size_t count);
int task_status(void);
BOOL task_tests(void);
BOOL remove_config(void);
#define PHONE_GUIDANCE                                                                                       \
    L"校园网要求电话验证，自动登录已暂停。\r\n\r\n请打开验证网页，按提示完成拨号或验证码验证，再点击“上线”。" \
    L"\r\n\r\n建议在校园网网页中将本机设为常用设备，可减少后续电话验证。"
typedef void (*NetworkProgress)(int stage);
#define NETWORK_CHECKING 0
#define NETWORK_WAITING 1
#define NETWORK_READY 2
#define WAIT_GUIDANCE                                                                                        \
    L"正在等待 Wi-Fi 或网线连接后自动登录。\r\n请连接校园网 "                                                \
    L"NSU-SDN，程序会在网络就绪后继续。\r\n最多等待约 3 分钟；关闭此提示不影响后台等待。"
#define NETWORK_CONNECT 0
#define NETWORK_CHECK_ONLY 1
#define NETWORK_DISCONNECT 2
#define NETWORK_INSPECT 3
void reset_network_session(void);
int connect_network(Config *c, int action, NetworkProgress progress,
                    const NetworkAdapter *selected, const NetworkAdapter *oldWifi, NetworkSessionState *status);
int native_tests(void);
void data_path(WCHAR *out, const WCHAR *name);
void write_log(const WCHAR *message);
BOOL system_tests(void);
