#include "native.h"

/* Compare interface identity and DHCP address so a new lease triggers reconnection. */
BOOL same_adapter(const NetworkAdapter *a, const NetworkAdapter *b) {
    return a->index == b->index && a->address.S_un.S_addr == b->address.S_un.S_addr &&
           !wcscmp(a->mac, b->mac);
}

/* Find connected physical Ethernet/Wi-Fi adapters with an IPv4 route to the portal. */
BOOL network_links(NetworkLinks *links) {
    ULONG size = 16384, result;
    IP_ADAPTER_ADDRESSES *list = NULL, *a;
    SOCKADDR_INET destination = {0}, source;
    MIB_IPFORWARD_ROW2 route;
    ULONG wiredMetric = MAXDWORD, wifiMetric = MAXDWORD;
    ZeroMemory(links, sizeof(*links));
    destination.Ipv4.sin_family = AF_INET;
    InetPtonW(AF_INET, L"2.2.2.2", &destination.Ipv4.sin_addr);
    for (int attempt = 0; attempt < 3; attempt++) {
        free(list);
        list = malloc(size);
        if (!list)
            return FALSE;
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER, NULL, list, &size);
        if (result != ERROR_BUFFER_OVERFLOW)
            break;
    }
    if (result != NO_ERROR) {
        free(list);
        return result == ERROR_NO_DATA;
    }
    for (a = list; a; a = a->Next) {
        MIB_IF_ROW2 row = {0};
        NetworkAdapter *target;
        ULONG *metric;
        if (a->OperStatus != IfOperStatusUp || a->PhysicalAddressLength != 6 ||
            (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211))
            continue;
        row.InterfaceLuid = a->Luid;
        if (GetIfEntry2(&row) != NO_ERROR || !row.InterfaceAndOperStatusFlags.HardwareInterface ||
            row.InterfaceAndOperStatusFlags.FilterInterface)
            continue;
        if (a->IfType == IF_TYPE_IEEE80211) links->wifiConnected = TRUE;
        else links->wiredConnected = TRUE;
        if (GetBestRoute2(&a->Luid, 0, NULL, &destination, 0, &route, &source) != NO_ERROR ||
            !source.Ipv4.sin_addr.S_un.S_addr ||
            (ntohl(source.Ipv4.sin_addr.S_un.S_addr) >> 16) == 0xa9fe)
            continue;
        target = a->IfType == IF_TYPE_IEEE80211 ? &links->wifi : &links->wired;
        metric = a->IfType == IF_TYPE_IEEE80211 ? &wifiMetric : &wiredMetric;
        if (target->index && a->Ipv4Metric >= *metric)
            continue;
        *metric = a->Ipv4Metric;
        target->index = a->IfIndex;
        target->address = source.Ipv4.sin_addr;
        InetNtopW(AF_INET, &target->address, target->ip, 16);
        swprintf_s(target->mac, 13, L"%02x%02x%02x%02x%02x%02x", a->PhysicalAddress[0],
                   a->PhysicalAddress[1], a->PhysicalAddress[2], a->PhysicalAddress[3],
                   a->PhysicalAddress[4], a->PhysicalAddress[5]);
    }
    free(list);
    return TRUE;
}

/* Keep only bounded cookie name/value pairs; replace rotated cookies within this login attempt. */
static BOOL save_cookie(PortalTransport *t, const char *value, size_t length) {
    size_t n = 0, key;
    int slot;
    while (n < length && value[n] != ';') n++;
    const char *eq = memchr(value, '=', n);
    if (!eq || eq == value || n >= sizeof(t->cookies[0])) return FALSE;
    key = (size_t)(eq - value) + 1;
    for (slot = 0; slot < t->cookieCount; slot++)
        if (!strncmp(t->cookies[slot], value, key)) break;
    if (slot == 16) return FALSE;
    if (slot == t->cookieCount) t->cookieCount++;
    memcpy(t->cookies[slot], value, n);
    t->cookies[slot][n] = 0;
    return TRUE;
}

/* Decode bounded HTTP framing and session cookies without accepting redirects or compressed bodies. */
static BOOL decode_response(PortalTransport *t, char *raw, size_t length, char *out, size_t cap) {
    char *headerEnd = strstr(raw, "\r\n\r\n"), *line = strstr(raw, "\r\n");
    size_t contentLength = 0, used = 0;
    BOOL hasLength = FALSE, chunked = FALSE;
    if (!headerEnd || !line || line > headerEnd ||
        (strncmp(raw, "HTTP/1.1 200 ", 13) && strncmp(raw, "HTTP/1.0 200 ", 13))) return FALSE;
    line += 2;
    while (line < headerEnd) {
        char *end = strstr(line, "\r\n"), *value = strchr(line, ':');
        if (!end || !value || value >= end) return FALSE;
        size_t key = (size_t)(value - line);
        for (value++; value < end && (*value == ' ' || *value == '\t'); value++) {}
        if (key == 10 && !_strnicmp(line, "Set-Cookie", key)) {
            if (!save_cookie(t, value, (size_t)(end - value))) return FALSE;
        } else if (key == 14 && !_strnicmp(line, "Content-Length", key)) {
            char *tail;
            unsigned long parsed = strtoul(value, &tail, 10);
            if (hasLength || tail != end || value == tail || parsed >= cap) return FALSE;
            contentLength = parsed;
            hasLength = TRUE;
        } else if (key == 17 && !_strnicmp(line, "Transfer-Encoding", key)) {
            if (end - value != 7 || _strnicmp(value, "chunked", 7)) return FALSE;
            chunked = TRUE;
        } else if (key == 16 && !_strnicmp(line, "Content-Encoding", key)) return FALSE;
        line = end + 2;
    }
    char *body = headerEnd + 4, *limit = raw + length;
    if (chunked) {
        if (hasLength) return FALSE;
        while (body < limit) {
            char *tail, *end = strstr(body, "\r\n");
            if (!end || end - body > 16 || !isxdigit((unsigned char)*body)) return FALSE;
            unsigned long n = strtoul(body, &tail, 16);
            if (tail != end || n >= cap - used) return FALSE;
            body = end + 2;
            if (!n) { out[used] = 0; return TRUE; }
            if ((size_t)(limit - body) < (size_t)n + 2 || memcmp(body + n, "\r\n", 2)) return FALSE;
            memcpy(out + used, body, n);
            used += n;
            body += n + 2;
        }
        return FALSE;
    }
    used = (size_t)(limit - body);
    if (used >= cap || (hasLength && used != contentLength)) return FALSE;
    memcpy(out, body, used);
    out[used] = 0;
    return TRUE;
}

/* Wait within a total request deadline, including a nonblocking TCP connection. */
static BOOL socket_ready(SOCKET socket, BOOL writing, ULONGLONG deadline) {
    fd_set set;
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) return FALSE;
    DWORD remaining = (DWORD)(deadline - now);
    struct timeval timeout = {(long)(remaining / 1000), (long)(remaining % 1000) * 1000};
    FD_ZERO(&set);
    FD_SET(socket, &set);
    return select(0, writing ? NULL : &set, writing ? &set : NULL, NULL, &timeout) > 0;
}

/* POST to the fixed portal through the selected NIC; never alter system routes or adapter MACs. */
BOOL portal_post(PortalTransport *t, const WCHAR *body, char *reply, size_t capacity) {
    SOCKET sock = INVALID_SOCKET;
    struct sockaddr_in local = {0}, remote = {0};
    char *request = calloc(200000, 1), *raw = calloc(98304, 1), *utf = calloc(120000, 1);
    BOOL ok = FALSE;
    ULONG nonblocking = 1, index = htonl(t->adapter.index);
    int size, sent = 0, received = 0, error = 0, errorSize = sizeof(error);
    ULONGLONG deadline = GetTickCount64() + 12000;
    if (!request || !raw || !utf || !t->adapter.index) goto done;
    size = WideCharToMultiByte(CP_UTF8, 0, body, -1, utf, 120000, NULL, NULL);
    if (!size) goto done;
    sprintf_s(request, 200000, "POST /Auth.ashx HTTP/1.0\r\nHost: 2.2.2.2\r\n"
              "User-Agent: NSU-Net-Autologin/1.3\r\nContent-Type: application/json; charset=utf-8\r\n"
              "Connection: close\r\nContent-Length: %d\r\n", size - 1);
    if (t->cookieCount) {
        strcat_s(request, 200000, "Cookie: ");
        for (int i = 0; i < t->cookieCount; i++) {
            if (i) strcat_s(request, 200000, "; ");
            strcat_s(request, 200000, t->cookies[i]);
        }
        strcat_s(request, 200000, "\r\n");
    }
    strcat_s(request, 200000, "\r\n");
    strcat_s(request, 200000, utf);
    size = (int)strlen(request);
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    local.sin_family = remote.sin_family = AF_INET;
    local.sin_addr = t->adapter.address;
    remote.sin_port = htons(80);
    InetPtonW(AF_INET, L"2.2.2.2", &remote.sin_addr);
    if (sock == INVALID_SOCKET || setsockopt(sock, IPPROTO_IP, IP_UNICAST_IF, (char *)&index, sizeof(index)) ||
        bind(sock, (struct sockaddr *)&local, sizeof(local)) || ioctlsocket(sock, FIONBIO, &nonblocking)) goto done;
    if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEWOULDBLOCK) goto done;
    if (!socket_ready(sock, TRUE, deadline) ||
        getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&error, &errorSize) || error) goto done;
    while (sent < size) {
        if (!socket_ready(sock, TRUE, deadline)) goto done;
        int n = send(sock, request + sent, size - sent, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n <= 0) goto done;
        sent += n;
    }
    while (received < 98303) {
        if (!socket_ready(sock, FALSE, deadline)) goto done;
        int n = recv(sock, raw + received, 98303 - received, 0);
        if (n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
        if (n == SOCKET_ERROR) goto done;
        if (!n) { ok = decode_response(t, raw, received, reply, capacity); break; }
        received += n;
    }
done:
    if (sock != INVALID_SOCKET) closesocket(sock);
    if (request) { SecureZeroMemory(request, 200000); free(request); }
    if (raw) { SecureZeroMemory(raw, 98304); free(raw); }
    if (utf) { SecureZeroMemory(utf, 120000); free(utf); }
    return ok;
}

/* Cover framing, truncation, cookies and adapter changes without sending network traffic. */
BOOL transport_tests(void) {
    PortalTransport t = {0};
    char output[64];
    char fixed[] = "HTTP/1.1 200 OK\r\nSet-Cookie: sid=one; Path=/\r\nContent-Length: 2\r\n\r\n{}";
    char chunked[] = "HTTP/1.1 200 OK\r\nSet-Cookie: sid=two; Path=/\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n";
    BOOL ok = decode_response(&t, fixed, strlen(fixed), output, sizeof(output)) && !strcmp(output, "{}");
    ok = ok && decode_response(&t, chunked, strlen(chunked), output, sizeof(output)) &&
         !strcmp(output, "{}") && t.cookieCount == 1 && !strcmp(t.cookies[0], "sid=two");
    ok = ok && !decode_response(&t, fixed, strlen(fixed) - 1, output, sizeof(output));
    NetworkAdapter a = {0}, b = {0};
    ok = ok && same_adapter(&a, &b);
    b.index = 1;
    return ok && !same_adapter(&a, &b);
}
