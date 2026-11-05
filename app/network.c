#include "native.h"
typedef struct {
    int type, next, child;
    WCHAR *s, *end;
} Node;
typedef struct {
    WCHAR text[65536];
    Node n[4096];
    int count;
    WCHAR *p;
} Json;
enum { OBJ = 1, ARR, STR, NUM, TRUE_VALUE, FALSE_VALUE, NULL_VALUE };
/* Skip JSON whitespace only. */
static void space(Json *j) {
    while (*j->p && wcschr(L" \r\n\t", *j->p))
        j->p++;
}
/* Decode a JSON string in place, rejecting invalid escapes and controls. */
static WCHAR *string_value(Json *j) {
    WCHAR *start = ++j->p, *out = start;
    while (*j->p && *j->p != L'"') {
        WCHAR c = *j->p++;
        if (c < 32)
            return NULL;
        if (c == L'\\') {
            c = *j->p++;
            if (c == L'u') {
                unsigned v = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    WCHAR h = *j->p;
                    if (!iswxdigit(h))
                        return NULL;
                    j->p++;
                    v = v * 16 + (h <= L'9' ? h - L'0' : towlower(h) - L'a' + 10);
                }
                if (!v)
                    return NULL;
                c = (WCHAR)v;
            } else if (c == L'n')
                c = L'\n';
            else if (c == L'r')
                c = L'\r';
            else if (c == L't')
                c = L'\t';
            else if (c == L'b')
                c = L'\b';
            else if (c == L'f')
                c = L'\f';
            else if (c != L'"' && c != L'\\' && c != L'/')
                return NULL;
        }
        *out++ = c;
    }
    if (*j->p != L'"')
        return NULL;
    j->p++;
    *out = 0;
    return start;
}
/* Parse a bounded JSON tree; depth and node limits prevent untrusted response exhaustion. */
static int value(Json *j, int depth) {
    int id, last = 0;
    Node *n;
    WCHAR end;
    space(j);
    if (depth > 32 || j->count >= 4095)
        return 0;
    id = ++j->count;
    n = &j->n[id];
    if (*j->p == L'"') {
        n->type = STR;
        n->s = string_value(j);
        if (!n->s)
            return 0;
    } else if (*j->p == L'{' || *j->p == L'[') {
        n->type = *j->p == L'{' ? OBJ : ARR;
        end = *j->p++ == L'{' ? L'}' : L']';
        space(j);
        if (*j->p != end)
            for (;;) {
                int child = value(j, depth + 1);
                if (!child)
                    return 0;
                if (last)
                    j->n[last].next = child;
                else
                    n->child = child;
                last = child;
                if (n->type == OBJ) {
                    int v;
                    if (j->n[child].type != STR)
                        return 0;
                    space(j);
                    if (*j->p++ != L':')
                        return 0;
                    v = value(j, depth + 1);
                    if (!v)
                        return 0;
                    j->n[child].child = v;
                }
                space(j);
                if (*j->p != L',')
                    break;
                j->p++;
                space(j);
            }
        if (*j->p++ != end)
            return 0;
    } else if (!wcsncmp(j->p, L"true", 4)) {
        n->type = TRUE_VALUE;
        j->p += 4;
    } else if (!wcsncmp(j->p, L"false", 5)) {
        n->type = FALSE_VALUE;
        j->p += 5;
    } else if (!wcsncmp(j->p, L"null", 4)) {
        n->type = NULL_VALUE;
        j->p += 4;
    } else {
        n->type = NUM;
        n->s = j->p;
        if (*j->p == L'-')
            j->p++;
        if (*j->p == L'0')
            j->p++;
        else {
            if (*j->p < L'1' || *j->p > L'9')
                return 0;
            while (iswdigit(*j->p))
                j->p++;
        }
        if (*j->p == L'.') {
            j->p++;
            if (!iswdigit(*j->p))
                return 0;
            while (iswdigit(*j->p))
                j->p++;
        }
        if (*j->p == L'e' || *j->p == L'E') {
            j->p++;
            if (*j->p == L'+' || *j->p == L'-')
                j->p++;
            if (!iswdigit(*j->p))
                return 0;
            while (iswdigit(*j->p))
                j->p++;
        }
    }
    n->end = j->p;
    return id;
}
/* Accept bounded nonnegative integer fields, never fractional or overflowing numbers. */
static int integer(Json *j, int id) {
    Node *n = &j->n[id];
    WCHAR *p = n->s;
    int v = 0;
    if (n->type != NUM || p == n->end)
        return -1;
    while (p < n->end) {
        if (*p < L'0' || *p > L'9' || v > 1000000)
            return -1;
        v = v * 10 + *p++ - L'0';
    }
    return v;
}
/* Decode UTF-8 and require exactly one complete JSON object. */
static BOOL parse(Json *j, const char *s) {
    ZeroMemory(j, sizeof(*j));
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, j->text, 65536))
        return FALSE;
    j->p = j->text;
    if (value(j, 0) != 1 || j->n[1].type != OBJ)
        return FALSE;
    space(j);
    return !*j->p;
}
/* Find a unique property, rejecting duplicates instead of guessing. */
static int prop(Json *j, int obj, const WCHAR *key) {
    int i, found = 0;
    if (!obj || j->n[obj].type != OBJ)
        return 0;
    for (i = j->n[obj].child; i; i = j->n[i].next)
        if (!wcscmp(j->n[i].s, key)) {
            if (found)
                return 0;
            found = j->n[i].child;
        }
    return found;
}
/* Return only string-typed fields. */
static const WCHAR *str(Json *j, int id) { return id && j->n[id].type == STR ? j->n[id].s : L""; }
/* Require a literal JSON boolean true. */
static BOOL success(Json *j) { return j->n[prop(j, 1, L"Result")].type == TRUE_VALUE; }
/* Decode visible portal text, removing HTML while keeping word boundaries and phone digits. */
static void message_text(Json *j, WCHAR *s) {
    const WCHAR *p = str(j, prop(j, 1, L"Message"));
    int k = 0;
    BOOL tag = FALSE;
    while (*p && k < 4095) {
        if (!tag && p[0] == L'&' && p[1] == L'#') {
            WCHAR *end;
            unsigned long v;
            const WCHAR *digits = p + 2;
            int base = 10;
            if (*digits == L'x' || *digits == L'X') {
                digits++;
                base = 16;
            }
            v = wcstoul(digits, &end, base);
            if (end > digits && *end == L';' && v > 0 && v <= 65535) {
                s[k++] = iswspace((WCHAR)v) ? L' ' : towlower((WCHAR)v);
                p = end + 1;
                continue;
            }
        }
        if (!tag && !wcsncmp(p, L"&nbsp;", 6)) {
            s[k++] = L' ';
            p += 6;
            continue;
        }
        if (*p == L'<')
            tag = TRUE;
        else if (*p == L'>')
            tag = FALSE;
        else if (!tag)
            s[k++] = iswspace(*p) ? L' ' : towlower(*p);
        p++;
    }
    s[k] = 0;
}
/* Recognize verification messages after normalizing markup and spacing. */
static BOOL phone(Json *j) {
    WCHAR s[4096];
    int out = 0;
    message_text(j, s);
    for (int i = 0; s[i]; i++)
        if (!iswspace(s[i])) s[out++] = s[i];
    s[out] = 0;
    return ((wcsstr(s, L"电话") || wcsstr(s, L"手机") || wcsstr(s, L"短信") || wcsstr(s, L"语音")) &&
            (wcsstr(s, L"验证") || wcsstr(s, L"认证") || wcsstr(s, L"拨打"))) ||
           wcsstr(s, L"phoneverification") || wcsstr(s, L"voicecode");
}
/* Extract only a number following a dialing instruction, never a user's unrelated phone number. */
static void phone_guidance(Json *j) {
    WCHAR message[4096], number[48] = {0};
    const WCHAR *cue = NULL;
    const WCHAR *cues[] = {L"拨打", L"致电", L"呼叫", L"dial", L"call"};
    message_text(j, message);
    for (int i = 0; i < 5 && !cue; i++) cue = wcsstr(message, cues[i]);
    if (cue) {
        const WCHAR *p = cue;
        int distance = 0, n = 0, digits = 0;
        while (*p && !iswdigit(*p) && distance++ < 100) p++;
        while (*p && n < 47 && (iswdigit(*p) || wcschr(L" -()+", *p))) {
            if (iswdigit(*p)) digits++;
            number[n++] = *p++;
        }
        while (n && number[n - 1] == L' ') number[--n] = 0;
        if (digits < 5 || digits > 15 || iswdigit(*p)) number[0] = 0;
    }
    if (*number)
        swprintf_s(errorText, 512, L"需要电话验证，请拨打：%s\r\n\r\n按校园网网页提示完成验证后，再点击“上线”。", number);
    else
        StringCchCopyW(errorText, 512, L"校园网要求电话验证，但未返回可识别的拨号号码。\r\n\r\n请点击“打开验证网页”查看号码并完成验证，再点击“上线”。");
}
/* Encode all UTF-16 units as JSON escapes, keeping request construction bounded. */
static BOOL quote(const WCHAR *s, WCHAR *out, size_t cap) {
    size_t n = 0;
    if (cap < 3)
        return FALSE;
    out[n++] = L'"';
    while (*s) {
        if (n + 8 > cap)
            return FALSE;
        swprintf_s(out + n, cap - n, L"\\u%04x", (unsigned)*s++);
        n += 6;
    }
    out[n++] = L'"';
    out[n] = 0;
    return TRUE;
}
/* Reproduce the portal DES/ECB/PKCS7 password encoding using Windows CryptoAPI. */
static BOOL encode_password(Config *c, WCHAR *hex, size_t cap) {
    HCRYPTPROV provider = 0;
    HCRYPTKEY key = 0;
    BOOL ok = FALSE;
    DWORD mode = CRYPT_MODE_ECB, size;
    size_t len = wcslen(c->account);
    int k;
    struct {
        BLOBHEADER h;
        DWORD size;
        BYTE bytes[8];
    } blob;
    WCHAR seed[530];
    char utf[2200];
    BYTE plain[2048];
    ZeroMemory(&blob, sizeof(blob));
    ZeroMemory(plain, sizeof(plain));
    swprintf_s(seed, 530, L"%s%s12345678", c->account + (len > 4 ? len - 4 : 0), c->account);
    if (!WideCharToMultiByte(CP_UTF8, 0, seed, -1, utf, sizeof(utf), NULL, NULL))
        goto done;
    blob.h.bType = PLAINTEXTKEYBLOB;
    blob.h.bVersion = CUR_BLOB_VERSION;
    blob.h.aiKeyAlg = CALG_DES;
    blob.size = 8;
    memcpy(blob.bytes, utf, 8);
    size = WideCharToMultiByte(CP_UTF8, 0, c->password, -1, (char *)plain, sizeof(plain), NULL, NULL);
    if (!size)
        goto done;
    size--;
    if (!CryptAcquireContextW(&provider, NULL, MS_ENHANCED_PROV_W, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptImportKey(provider, (BYTE *)&blob, sizeof(blob), 0, 0, &key) ||
        !CryptSetKeyParam(key, KP_MODE, (BYTE *)&mode, 0) ||
        !CryptEncrypt(key, 0, TRUE, 0, plain, &size, sizeof(plain)) || cap <= size * 2)
        goto done;
    for (k = 0; k < (int)size; k++)
        swprintf_s(hex + k * 2, cap - k * 2, L"%02x", plain[k]);
    ok = TRUE;
done:
    if (key)
        CryptDestroyKey(key);
    if (provider)
        CryptReleaseContext(provider, 0);
    SecureZeroMemory(plain, sizeof(plain));
    SecureZeroMemory(&blob, sizeof(blob));
    SecureZeroMemory(utf, sizeof(utf));
    SecureZeroMemory(seed, sizeof(seed));
    return ok;
}
/* Decode a bounded reply from the transport bound to the selected physical adapter. */
static int http_request(PortalTransport *conn, const WCHAR *body, Json *j) {
    char *reply = calloc(65536, 1);
    int result = 1;
    if (reply && portal_post(conn, body, reply, 65536) && parse(j, reply) && prop(j, 1, L"Result"))
        result = phone(j) ? 3 : 0;
    if (reply) { SecureZeroMemory(reply, 65536); free(reply); }
    return result;
}
static const char **testReplies;
static const WCHAR **testActions;
static int testIndex, testCount, testMismatch;
static int testAction;
static const NetworkAdapter *testSelected, *testWifi;
static ULONGLONG testClock;
/* Route offline self-tests through fixed fixtures; normal execution uses the bound socket transport. */
static int request(PortalTransport *conn, const WCHAR *body, Json *j) {
    if (testReplies) {
        WCHAR expected[80];
        if (testIndex >= testCount) {
            testMismatch = 1;
            return 3;
        }
        swprintf_s(expected, 80, L"\"DoWhat\":\"%s\"", testActions[testIndex]);
        if (!wcsstr(body, expected))
            testMismatch = 1;
        if (!wcscmp(testActions[testIndex], L"CloseNet")) {
            Json *sent = calloc(1, sizeof(Json));
            char utf[512];
            const NetworkAdapter *target = testAction == NETWORK_DISCONNECT ? testSelected : testWifi;
            if (!sent || !target || !WideCharToMultiByte(CP_UTF8, 0, body, -1, utf, 512, NULL, NULL) ||
                !parse(sent, utf) || wcscmp(str(sent, prop(sent, 1, L"IP")), target->ip)) testMismatch = 1;
            free(sent);
        }
        const char *reply = testReplies[testIndex++];
        if (!reply)
            return 1; /* NULL fixtures represent a transport failure. */
        if (!parse(j, reply)) {
            testMismatch = 1;
            return 3;
        }
        return phone(j) ? 3 : 0;
    }
    return http_request(conn, body, j);
}
/* Avoid wall-clock waits only during in-process offline fixtures. */
static void pause_ms(DWORD ms) {
    if (!testReplies)
        Sleep(ms);
    else
        testClock += ms;
}
/* Use virtual time for offline retry tests, without changing production deadlines. */
static ULONGLONG network_ticks(void) { return testReplies ? testClock : GetTickCount64(); }
/* Validate the IP and online-device array before deciding whether this device is online. */
static int online(Json *j) {
    int data = prop(j, 1, L"Data"), arr = prop(j, data, L"OIA"), i;
    const WCHAR *ip = str(j, prop(j, data, L"IP"));
    if (!*ip || j->n[arr].type != ARR)
        return -1;
    for (i = j->n[arr].child; i; i = j->n[i].next)
        if (!wcscmp(ip, str(j, prop(j, i, L"IP"))))
            return 1;
    return 0;
}
/* Match one and only one configured package and enforce the device limit. */
static BOOL package_name(Json *j, Config *c, WCHAR *out) {
    int d = prop(j, 1, L"Data"), a = prop(j, d, L"KXTC"), o = prop(j, d, L"OIA"), m = prop(j, d, L"MOC"), i,
        count = 0, matches = 0;
    if (j->n[a].type != ARR || j->n[o].type != ARR || integer(j, m) <= 0)
        return FALSE;
    for (i = j->n[o].child; i; i = j->n[i].next)
        count++;
    if (count >= integer(j, m))
        return FALSE;
    for (i = j->n[a].child; i; i = j->n[i].next) {
        const WCHAR *s = str(j, prop(j, i, L"套餐名称"));
        if (*s && (c->package[0] ? !wcscmp(c->package, s) : wcsstr(s, c->provider) != NULL)) {
            if (FAILED(StringCchCopyW(out, 512, s)))
                return FALSE;
            matches++;
        }
    }
    return matches == 1;
}
/* Compare a portal MAC with the selected adapter, accepting common separator formats. */
static BOOL mac_matches(const WCHAR *portal, const WCHAR *local) {
    WCHAR normalized[13];
    int n = 0;
    for (; *portal; portal++) {
        if (wcschr(L":- .", *portal)) continue;
        if (!iswxdigit(*portal) || n == 12) return FALSE;
        normalized[n++] = towlower(*portal);
    }
    normalized[n] = 0;
    return n == 12 && !_wcsicmp(normalized, local);
}
/* Require both local IP and adapter MAC before treating an online entry as this device's session. */
static BOOL adapter_online(Json *j, const NetworkAdapter *wifi) {
    int data = prop(j, 1, L"Data"), arr = prop(j, data, L"OIA");
    if (!wifi || !wifi->index || j->n[arr].type != ARR) return FALSE;
    for (int i = j->n[arr].child; i; i = j->n[i].next)
        if (!wcscmp(wifi->ip, str(j, prop(j, i, L"IP"))) &&
            mac_matches(str(j, prop(j, i, L"MAC")), wifi->mac)) return TRUE;
    return FALSE;
}
/* Require the selected source IP; an empty MAC is normal only when the portal confirms this IP is offline. */
static BOOL selected_identity(Json *j, const NetworkAdapter *selected) {
    int data = prop(j, 1, L"Data"), mac = prop(j, data, L"MAC");
    const WCHAR *portalMac = str(j, mac);
    if (wcscmp(selected->ip, str(j, prop(j, data, L"IP"))) || j->n[mac].type != STR)
        return FALSE;
    return *portalMac ? mac_matches(portalMac, selected->mac) : online(j) == 0;
}
static PortalTransport cachedPortal;
static BOOL cachedPortalValid;
static WCHAR cachedAccount[256];
/* Clear in-memory portal cookies after account edits; credentials and cookies are never persisted here. */
void reset_network_session(void) {
    SecureZeroMemory(&cachedPortal, sizeof(cachedPortal));
    SecureZeroMemory(cachedAccount, sizeof(cachedAccount));
    cachedPortalValid = FALSE;
}
/* Run the bounded login protocol, stopping immediately for additional verification. */
int connect_network(Config *c, int action, NetworkProgress progress,
                    const NetworkAdapter *selected, const NetworkAdapter *oldWifi, NetworkSessionState *status) {
    PortalTransport *conn = calloc(1, sizeof(PortalTransport));
    Json *j = calloc(1, sizeof(Json));
    int r = 1, i, state;
    BOOL waiting = FALSE, removedWifi = FALSE, disconnectSent = FALSE;
    BOOL checkOnly = action == NETWORK_CHECK_ONLY;
    BOOL inspectOnly = action == NETWORK_INSPECT, authenticated = FALSE;
    int offlineChecks = 0;
    NetworkAdapter wifi = {0};
    ULONGLONG until;
    WCHAR encrypted[4096], qname[1600], qpass[25000], body[30000], package[512], qpackage[3100], token[4096],
        qtoken[25000];
    StringCchCopyW(errorText, 512, L"无法连接校园网，请检查学校 Wi-Fi、账号及套餐配置。");
    if (status) status->selected = status->other = -1;
    if (!j || !conn)
        goto done;
    if (selected) conn->adapter = *selected;
    if (!testReplies && cachedPortalValid && !wcscmp(cachedAccount, c->account) &&
        same_adapter(&cachedPortal.adapter, &conn->adapter))
        *conn = cachedPortal;
    if (oldWifi) wifi = *oldWifi;
    if ((action == NETWORK_DISCONNECT || inspectOnly) && !conn->adapter.index) {
        StringCchCopyW(errorText, 512, L"所选网卡尚未连接，无法确认下线目标。");
        goto done;
    }
    if (!checkOnly && progress)
        progress(NETWORK_CHECKING);
    until = network_ticks() + (checkOnly || inspectOnly ? 0 : 180000);
authenticate:
    do {
        if (!testReplies && !conn->adapter.index) {
            NetworkLinks links;
            if (network_links(&links)) {
                conn->adapter = links.wired.index ? links.wired : links.wifi;
                if (links.wired.index && links.wifi.index) wifi = links.wifi;
            }
        }
        r = (!testReplies && !conn->adapter.index) ? 1 : request(conn, L"{\"DoWhat\":\"Check\"}", j);
        if (r != 1)
            break;
        if (!checkOnly && !inspectOnly && !waiting) {
            waiting = TRUE;
            if (progress)
                progress(NETWORK_WAITING);
        }
        if (network_ticks() >= until) {
            if (!checkOnly && !inspectOnly)
                StringCchCopyW(errorText, 512,
                               L"等待校园网连接超时。请连接 NSU-SDN Wi-Fi 或网线后，再点击“上线”。");
            goto done;
        }
        pause_ms(3000);
    } while (TRUE);
    if (r)
        goto done;
    if (checkOnly)
        goto done;
    if (progress)
        progress(NETWORK_READY);
    if (!success(j)) {
        if (!wcscmp(str(j, prop(j, 1, L"Result")), L"needQRLogin")) {
            StringCchCopyW(errorText, 512, L"校园网要求扫码验证，请打开 http://2.2.2.2/ 手动完成。");
            r = 1;
            goto done;
        }
        if (!encode_password(c, encrypted, 4096) || !quote(c->account, qname, 1600) ||
            !quote(encrypted, qpass, 25000)) {
            r = 1;
            goto done;
        }
        swprintf_s(body, 30000, L"{\"DoWhat\":\"Login\",\"username\":%s,\"password\":%s,\"remember\":false}",
                   qname, qpass);
        r = request(conn, body, j);
        if (r)
            goto done;
        if (!success(j)) {
            r = 1;
            goto done;
        }
    }
    authenticated = TRUE;
    r = request(conn, L"{\"DoWhat\":\"GetInfo\"}", j);
    if (r)
        goto done;
    state = online(j);
    if (!success(j) || state < 0) {
        r = 1;
        goto done;
    }
    if (conn->adapter.index && !selected_identity(j, &conn->adapter)) {
        StringCchCopyW(errorText, 512, L"校园网返回的 IP / MAC 与所选网卡不一致，已停止切换。请确认网线或 Wi-Fi 直接连接校园网。");
        r = 1;
        goto done;
    }
    if (conn->adapter.index && state == 1 && !adapter_online(j, &conn->adapter)) {
        StringCchCopyW(errorText, 512, L"在线记录的 MAC 与所选网卡不符，已停止操作。");
        r = 1;
        goto done;
    }
    if (status) {
        status->selected = state;
        status->other = wifi.index ? (adapter_online(j, &wifi) ? 1 : 0) : -1;
    }
    /* Status inspection authenticates the portal if needed, but never changes network sessions. */
    if (inspectOnly) goto done;
    if (action == NETWORK_DISCONNECT) {
        if (state == 0) goto done;
        if (disconnectSent) {
            if (++offlineChecks >= 5) {
                StringCchCopyW(errorText, 512, L"校园网尚未确认下线，请在网页检查后重试。");
                r = 1;
                goto done;
            }
            pause_ms(1000);
            goto authenticate;
        }
        WCHAR quotedIp[128];
        if (!quote(conn->adapter.ip, quotedIp, 128)) { r = 1; goto done; }
        swprintf_s(body, 30000, L"{\"DoWhat\":\"CloseNet\",\"IP\":%s}", quotedIp);
        r = request(conn, body, j);
        if (r) goto done;
        if (!success(j)) {
            StringCchCopyW(errorText, 512, L"校园网拒绝下线，请在网页检查后重试。");
            r = 1;
            goto done;
        }
        disconnectSent = TRUE;
        pause_ms(1000);
        goto authenticate;
    }
    if (wifi.index && !same_adapter(&wifi, &conn->adapter) && adapter_online(j, &wifi)) {
        if (removedWifi) {
            StringCchCopyW(errorText, 512, L"校园网尚未确认另一网卡的本机会话下线，已停止切换，请稍后重试。");
            r = 1;
            goto done;
        }
        WCHAR quotedIp[128];
        if (!quote(wifi.ip, quotedIp, 128)) { r = 1; goto done; }
        swprintf_s(body, 30000, L"{\"DoWhat\":\"CloseNet\",\"IP\":%s}", quotedIp);
        r = request(conn, body, j);
        if (r) goto done;
        if (!success(j)) {
            StringCchCopyW(errorText, 512, L"另一网卡的本机会话下线失败，已停止切换。请在校园网网页检查后重试。");
            r = 1;
            goto done;
        }
        removedWifi = TRUE;
        pause_ms(1000);
        goto authenticate;
    }
    if (state == 1)
        goto done;
    if (!package_name(j, c, package)) {
        StringCchCopyW(errorText, 512,
                       L"套餐未唯一匹配或设备达到上限。请调整完整套餐名，或在网页检查在线设备。");
        r = 1;
        goto done;
    }
    if (!quote(package, qpackage, 3100)) {
        r = 1;
        goto done;
    }
    swprintf_s(body, 30000, L"{\"DoWhat\":\"OpenNet\",\"Package\":%s}", qpackage);
    r = request(conn, body, j);
    if (r)
        goto done;
    i = prop(j, 1, L"Result");
    if (integer(j, i) == 192) {
        int seconds = 60, sec = prop(j, 1, L"Sec");
        if (integer(j, sec) >= 0)
            seconds = integer(j, sec);
        if (seconds < 1)
            seconds = 1;
        if (seconds > 60)
            seconds = 60;
        if (FAILED(StringCchCopyW(token, 4096, str(j, prop(j, 1, L"Token")))) || !*token ||
            !quote(token, qtoken, 25000)) {
            r = 1;
            goto done;
        }
        swprintf_s(body, 30000, L"{\"DoWhat\":\"ReConnect\",\"Token\":%s}", qtoken);
        until = network_ticks() + seconds * 1000;
        do {
            pause_ms(1000);
            r = request(conn, body, j);
            if (r)
                goto done;
            if (success(j))
                break;
            if (wcscmp(str(j, prop(j, 1, L"Result")), L"wait")) {
                r = 1;
                goto done;
            }
        } while (network_ticks() < until);
    } else if (!success(j)) {
        r = 1;
        goto done;
    }
    for (i = 0; i < 10; i++) {
        r = request(conn, L"{\"DoWhat\":\"GetInfo\"}", j);
        if (r)
            goto done;
        if (success(j) && online(j) == 1 &&
            (!conn->adapter.index || (selected_identity(j, &conn->adapter) && adapter_online(j, &conn->adapter)))) {
            if (status) {
                status->selected = 1;
                status->other = wifi.index ? (adapter_online(j, &wifi) ? 1 : 0) : -1;
            }
            goto done;
        }
        pause_ms(2000);
    }
    r = 1;
    StringCchCopyW(errorText, 512, L"校园网尚未确认本机上线，请在网页检查后重试。");
done:
    if (status && r) status->selected = status->other = -1;
    if (r == 3)
        phone_guidance(j);
    if (!testReplies && authenticated && conn) {
        cachedPortal = *conn;
        StringCchCopyW(cachedAccount, 256, c->account);
        cachedPortalValid = TRUE;
    }
    if (conn) { SecureZeroMemory(conn, sizeof(*conn)); free(conn); }
    if (j) {
        SecureZeroMemory(j, sizeof(*j));
        free(j);
    }
    SecureZeroMemory(encrypted, sizeof(encrypted));
    SecureZeroMemory(body, sizeof(body));
    SecureZeroMemory(qpass, sizeof(qpass));
    SecureZeroMemory(token, sizeof(token));
    SecureZeroMemory(qtoken, sizeof(qtoken));
    return r;
}
static int testProgress[8], testProgressCount;
static NetworkSessionState testSession;
/* Record progress stages for retry tests without creating windows or accessing user data. */
static void record_progress(int stage) {
    if (testProgressCount < 8)
        testProgress[testProgressCount] = stage;
    testProgressCount++;
}
/* Check the complete sequence and ensure no extra request follows a terminal response. */
static BOOL scenario(Config *c, const WCHAR **actions, const char **replies, int count, int expected) {
    int r;
    testReplies = replies;
    testActions = actions;
    testCount = count;
    testIndex = 0;
    testMismatch = 0;
    testClock = 0;
    testProgressCount = 0;
    r = connect_network(c, testAction, record_progress, testSelected, testWifi, &testSession);
    testReplies = NULL;
    return r == expected && testIndex == count && !testMismatch;
}
/* Exercise parser/type checks, verification and exact protocol encoding without network or personal data. */
int native_tests(void) {
    Json *j = calloc(1, sizeof(Json));
    Config c = {0};
    WCHAR hex[256], q[128];
    int fail = 0;
    if (!j)
        return 1;
    fail += !parse(j, "{\"Result\":true}") || !success(j);
    fail += !parse(j, "{\"Result\":\"true\"}") || success(j);
    fail += parse(j, "{\"Result\":true}garbage");
    fail += parse(j, "{\"Result\":true,}");
    fail += !parse(j, "{\"Result\":true,\"Result\":false}") || success(j);
    fail += !parse(j, "{\"Message\":\"\\u624b\\u673a\\u9a8c\\u8bc1\"}") || !phone(j);
    fail += !parse(j, "{\"Message\":\"&#25163;&#26426;&nbsp;&#39564;&#35777;\"}") || !phone(j);
    parse(j, "{\"Message\":\"电话验证：请使用绑定手机 13800138000 拨打 <b>028-12345678</b> 完成验证\"}");
    phone_guidance(j);
    fail += !phone(j) || !wcsstr(errorText, L"028-12345678") || wcsstr(errorText, L"13800138000") != NULL;
    parse(j, "{\"Message\":\"phone verification\"}");
    phone_guidance(j);
    fail += !wcsstr(errorText, L"未返回");
    fail += !parse(j, "{\"Data\":{\"IP\":\"1\",\"OIA\":[{\"IP\":\"1\"}]}}") || online(j) != 1;
    fail += !parse(j, "{\"Data\":{\"IP\":\"1\"}}") || online(j) != -1;
    fail += !quote(L"\"\\\n", q, 128) || wcscmp(q, L"\"\\u0022\\u005c\\u000a\"") != 0;
    StringCchCopyW(c.account, 256, L"2023123456");
    StringCchCopyW(c.password, 256, L"test123");
    fail += !encode_password(
        &c, hex, 256); /* The external test compares this result against the independent .NET vector. */
    {
        const char *yes = "{\"Result\":true}";
        const char *verify = "{\"Result\":false,\"Message\":\"phone verification\"}";
        const char *on = "{\"Result\":true,\"Data\":{\"IP\":\"1\",\"OIA\":[{\"IP\":\"1\"}]}}";
        const char *off = "{\"Result\":true,\"Data\":{\"IP\":\"1\",\"OIA\":[],\"MOC\":2,\"KXTC\":[{"
                          "\"\\u5957\\u9910\\u540d\\u79f0\":\"test\"}]}}";
        const WCHAR *a[] = {L"Check", L"Login", L"GetInfo", L"OpenNet", L"ReConnect", L"GetInfo"};
        const WCHAR *b[] = {L"Check", L"GetInfo"};
        const char *r1[] = {yes, on};
        const char *r2[] = {"{\"Result\":false}", yes, off, yes, on};
        const WCHAR *a2[] = {L"Check", L"Login", L"GetInfo", L"OpenNet", L"GetInfo"};
        const char *r3[] = {
            "{\"Result\":false}", yes, off, "{\"Result\":192,\"Token\":\"fixture\",\"Sec\":1}", yes, on};
        const char *r4[] = {verify};
        const char *r5[] = {"{\"Result\":false}", verify};
        const char *r6[] = {yes, verify};
        const char *r7[] = {"{\"Result\":false}", yes, off, verify};
        StringCchCopyW(c.provider, 16, L"test");
        fail += !scenario(&c, b, r1, 2, 0);
        fail +=
            testProgressCount != 2 || testProgress[0] != NETWORK_CHECKING || testProgress[1] != NETWORK_READY;
        {
            const WCHAR *retryActions[] = {L"Check",   L"Check",   L"Check",  L"Login",
                                           L"GetInfo", L"OpenNet", L"GetInfo"};
            const char *retryReplies[] = {NULL, NULL, "{\"Result\":false}", yes, off, yes, on};
            fail += !scenario(&c, retryActions, retryReplies, 7, 0);
            fail += testProgressCount != 3 || testProgress[0] != NETWORK_CHECKING ||
                    testProgress[1] != NETWORK_WAITING || testProgress[2] != NETWORK_READY;
            const WCHAR *timeoutActions[61];
            const char *timeoutReplies[61] = {0};
            for (int t = 0; t < 61; t++)
                timeoutActions[t] = L"Check";
            fail += !scenario(&c, timeoutActions, timeoutReplies, 61, 1);
            fail +=
                testProgressCount != 2 || testProgress[1] != NETWORK_WAITING || !wcsstr(errorText, L"超时");
        }
        fail += !scenario(&c, a2, r2, 5, 0);
        fail += !scenario(&c, a, r3, 6, 0);
        fail += !scenario(&c, a, r4, 1, 3);
        fail += !scenario(&c, a, r5, 2, 3);
        fail += !scenario(&c, b, r6, 2, 3);
        fail += !scenario(&c, a, r7, 4, 3);
        {
            const char *r8[] = {"{\"Result\":false}", "{\"Result\":false}"};
            fail += !scenario(&c, a, r8, 2, 1);
        }
        parse(j, off);
        StringCchCopyW(c.package, 512, L"missing");
        fail += package_name(j, &c, q);
        c.package[0] = 0;
        {
            NetworkAdapter wired = {0}, wifi = {0};
            wired.index = 1; wifi.index = 2;
            StringCchCopyW(wired.ip, 16, L"10.0.0.2");
            StringCchCopyW(wired.mac, 13, L"001122334455");
            StringCchCopyW(wifi.ip, 16, L"10.0.0.1");
            StringCchCopyW(wifi.mac, 13, L"aabbccddeeff");
            const char *both = "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.2\",\"MAC\":\"00-11-22-33-44-55\",\"OIA\":[{\"IP\":\"10.0.0.1\",\"MAC\":\"aabbccddeeff\"}]}}";
            /* The real portal clears Data.MAC after CloseNet; keep this regression in both directions. */
            const char *wiredOff = "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.2\",\"MAC\":\"\",\"OIA\":[],\"MOC\":2,\"KXTC\":[{\"套餐名称\":\"test\"}]}}";
            const char *wiredOn = "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.2\",\"MAC\":\"001122334455\",\"OIA\":[{\"IP\":\"10.0.0.2\",\"MAC\":\"001122334455\"}]}}";
            const WCHAR *switchActions[] = {L"Check", L"GetInfo", L"CloseNet", L"Check", L"GetInfo", L"OpenNet", L"GetInfo"};
            const char *switched[] = {yes, both, yes, yes, wiredOff, yes, wiredOn};
            testSelected = &wired; testWifi = &wifi;
            fail += !scenario(&c, switchActions, switched, 7, 0);
            const WCHAR *resumeActions[] = {L"Check", L"GetInfo", L"OpenNet", L"GetInfo"};
            const char *resumed[] = {yes, wiredOff, yes, wiredOn};
            fail += !scenario(&c, resumeActions, resumed, 4, 0);
            const char *unsafeIdentity[] = {
                "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.9\",\"MAC\":\"\",\"OIA\":[]}}",
                "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.2\",\"MAC\":\"aabbccddeeff\",\"OIA\":[]}}",
                "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.2\",\"MAC\":\"\",\"OIA\":[{\"IP\":\"10.0.0.2\",\"MAC\":\"001122334455\"}]}}"
            };
            for (int t = 0; t < 3; t++) {
                const char *rejected[] = {yes, unsafeIdentity[t]};
                fail += !scenario(&c, b, rejected, 2, 1);
            }
            const char *closeFailure[] = {yes, both, "{\"Result\":false}"};
            fail += !scenario(&c, switchActions, closeFailure, 3, 1);
            const char *closeVerify[] = {yes, both, verify};
            fail += !scenario(&c, switchActions, closeVerify, 3, 3);
            const char *notOffline[] = {yes, both, yes, yes, both};
            fail += !scenario(&c, switchActions, notOffline, 5, 1);
            const char *mismatch[] = {yes, on};
            fail += !scenario(&c, b, mismatch, 2, 1);
            parse(j, both);
            wifi.mac[0] = L'0';
            fail += adapter_online(j, &wifi);
            const char *alreadyWired[] = {yes, wiredOn};
            fail += !scenario(&c, b, alreadyWired, 2, 0);
            wifi.mac[0] = L'a';
            testAction = NETWORK_INSPECT;
            const char *inspectOff[] = {yes, wiredOff};
            fail += !scenario(&c, b, inspectOff, 2, 0) || testSession.selected != 0 || testSession.other != 0;
            const char *inspectOther[] = {yes, both};
            fail += !scenario(&c, b, inspectOther, 2, 0) || testSession.selected != 0 || testSession.other != 1;
            const char *inspectAuthenticated[] = {"{\"Result\":false}", yes, wiredOn};
            const WCHAR *inspectActions[] = {L"Check", L"Login", L"GetInfo"};
            fail += !scenario(&c, inspectActions, inspectAuthenticated, 3, 0) || testSession.selected != 1;
            testAction = NETWORK_DISCONNECT;
            const char *disconnected[] = {yes, wiredOn, yes, yes, wiredOff};
            fail += !scenario(&c, switchActions, disconnected, 5, 0);
            const char *offlineAlready[] = {yes, wiredOff};
            fail += !scenario(&c, b, offlineAlready, 2, 0);
            const char *disconnectFailed[] = {yes, wiredOn, "{\"Result\":false}"};
            fail += !scenario(&c, switchActions, disconnectFailed, 3, 1);
            const char *disconnectPhone[] = {yes, wiredOn, verify};
            fail += !scenario(&c, switchActions, disconnectPhone, 3, 3);
            fail += !scenario(&c, b, mismatch, 2, 1);
            testAction = NETWORK_CONNECT;
            wifi.mac[0] = L'a';
            testSelected = &wifi; testWifi = &wired;
            const char *wifiSwitch[] = {
                yes,
                "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.1\",\"MAC\":\"aabbccddeeff\",\"OIA\":[{\"IP\":\"10.0.0.2\",\"MAC\":\"001122334455\"}]}}",
                yes, yes,
                "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.1\",\"MAC\":\"aabbccddeeff\",\"OIA\":[],\"MOC\":2,\"KXTC\":[{\"套餐名称\":\"test\"}]}}",
                yes,
                "{\"Result\":true,\"Data\":{\"IP\":\"10.0.0.1\",\"MAC\":\"aabbccddeeff\",\"OIA\":[{\"IP\":\"10.0.0.1\",\"MAC\":\"aabbccddeeff\"}]}}"
            };
            fail += !scenario(&c, switchActions, wifiSwitch, 7, 0);
            testSelected = testWifi = NULL;
        }
    }
    if (!fail) {
        DWORD n;
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        char utf[512];
        int len = WideCharToMultiByte(CP_UTF8, 0, hex, -1, utf, 512, NULL, NULL);
        if (out != INVALID_HANDLE_VALUE)
            WriteFile(out, utf, len - 1, &n, NULL);
    }
    SecureZeroMemory(&c, sizeof(c));
    free(j);
    return fail ? 1 : 0;
}
