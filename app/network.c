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
/* Remove markup/spacing and recognize phone verification without trusting returned links. */
static BOOL phone(Json *j) {
    const WCHAR *p = str(j, prop(j, 1, L"Message"));
    WCHAR s[4096];
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
                if (!iswspace((WCHAR)v))
                    s[k++] = towlower((WCHAR)v);
                p = end + 1;
                continue;
            }
        }
        if (!tag && !wcsncmp(p, L"&nbsp;", 6)) {
            p += 6;
            continue;
        }
        if (*p == L'<')
            tag = TRUE;
        else if (*p == L'>')
            tag = FALSE;
        else if (!tag && !iswspace(*p))
            s[k++] = towlower(*p);
        p++;
    }
    s[k] = 0;
    return ((wcsstr(s, L"电话") || wcsstr(s, L"手机") || wcsstr(s, L"短信") || wcsstr(s, L"语音")) &&
            (wcsstr(s, L"验证") || wcsstr(s, L"认证") || wcsstr(s, L"拨打"))) ||
           wcsstr(s, L"phoneverification") || wcsstr(s, L"voicecode");
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
/* POST only to the fixed portal; disable redirects/proxies and bound response size. */
static int http_request(HINTERNET conn, const WCHAR *body, Json *j) {
    HINTERNET req = WinHttpOpenRequest(conn, L"POST", L"/Auth.ashx", NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    char *bytes = NULL, *reply = NULL;
    DWORD disable = WINHTTP_DISABLE_REDIRECTS, status = 0, n = sizeof(status), read = 0, total = 0;
    int result = 1, size;
    if (!req)
        return 1;
    WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
    {
        DWORD policy = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
        WinHttpSetOption(req, WINHTTP_OPTION_AUTOLOGON_POLICY, &policy, sizeof(policy));
    }
    size = WideCharToMultiByte(CP_UTF8, 0, body, -1, NULL, 0, NULL, NULL);
    bytes = calloc(size, 1);
    reply = calloc(65536, 1);
    if (!bytes || !reply)
        goto done;
    WideCharToMultiByte(CP_UTF8, 0, body, -1, bytes, size, NULL, NULL);
    if (!WinHttpSendRequest(req, L"Content-Type: application/json; charset=utf-8\r\n", (DWORD)-1, bytes,
                            size - 1, size - 1, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto done;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &n,
                             NULL) ||
        status != 200)
        goto done;
    do {
        if (total >= 65535)
            goto done;
        if (!WinHttpReadData(req, reply + total, 65535 - total, &read))
            goto done;
        total += read;
    } while (read);
    if (!parse(j, reply) || !prop(j, 1, L"Result"))
        goto done;
    result = phone(j) ? 3 : 0;
done:
    if (bytes) {
        SecureZeroMemory(bytes, size);
        free(bytes);
    }
    if (reply) {
        SecureZeroMemory(reply, 65536);
        free(reply);
    }
    WinHttpCloseHandle(req);
    return result;
}
static const char **testReplies;
static const WCHAR **testActions;
static int testIndex, testCount, testMismatch;
static ULONGLONG testClock;
/* Route offline self-tests through fixed fixtures; normal execution always uses WinHTTP. */
static int request(HINTERNET conn, const WCHAR *body, Json *j) {
    if (testReplies) {
        WCHAR expected[80];
        if (testIndex >= testCount) {
            testMismatch = 1;
            return 3;
        }
        swprintf_s(expected, 80, L"\"DoWhat\":\"%s\"", testActions[testIndex]);
        if (!wcsstr(body, expected))
            testMismatch = 1;
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
/* Run the bounded login protocol, stopping immediately for additional verification. */
int connect_network(Config *c, BOOL checkOnly, NetworkProgress progress) {
    HINTERNET session = NULL, conn = NULL;
    Json *j = calloc(1, sizeof(Json));
    int r = 1, i, state;
    BOOL waiting = FALSE;
    ULONGLONG until;
    WCHAR encrypted[4096], qname[1600], qpass[25000], body[30000], package[512], qpackage[3100], token[4096],
        qtoken[25000];
    StringCchCopyW(errorText, 512, L"无法连接校园网，请检查学校 Wi-Fi、账号及套餐配置。");
    if (!j)
        goto done;
    session = WinHttpOpen(L"NSU-Net-Autologin/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
    if (!session)
        goto done;
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    conn = WinHttpConnect(session, L"2.2.2.2", 80, 0);
    if (!conn)
        goto done;
    if (!checkOnly && progress)
        progress(NETWORK_CHECKING);
    until = network_ticks() + (checkOnly ? 0 : 180000);
    do {
        r = request(conn, L"{\"DoWhat\":\"Check\"}", j);
        if (r != 1)
            break;
        if (!checkOnly && !waiting) {
            waiting = TRUE;
            if (progress)
                progress(NETWORK_WAITING);
        }
        if (network_ticks() >= until) {
            if (!checkOnly)
                StringCchCopyW(errorText, 512,
                               L"等待校园网连接超时。请连接 NSU-SDN Wi-Fi 或网线后，再点击“立即连接”。");
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
    r = request(conn, L"{\"DoWhat\":\"GetInfo\"}", j);
    if (r)
        goto done;
    state = online(j);
    if (!success(j) || state < 0) {
        r = 1;
        goto done;
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
        if (success(j) && online(j) == 1)
            goto done;
        pause_ms(2000);
    }
    r = 1;
    StringCchCopyW(errorText, 512, L"校园网尚未确认本机上线，请在网页检查后重试。");
done:
    if (r == 3)
        StringCchCopyW(errorText, 512, PHONE_GUIDANCE);
    if (conn)
        WinHttpCloseHandle(conn);
    if (session)
        WinHttpCloseHandle(session);
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
    r = connect_network(c, FALSE, record_progress);
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
