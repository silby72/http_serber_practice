// http_server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT 8080
#define BACKLOG 5
#define RECV_BUFSIZE 8192

/* URLデコード（%XX をデコード）。'+'はそのまま残す（例題の "2+10" を優先） */
static char *url_decode(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    char *d = out;
    for (const char *p = s; *p; ++p) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            *d++ = (char) strtol(hex, NULL, 16);
            p += 2;
        } else {
            *d++ = *p;
        }
    }
    *d = '\0';
    return out;
}

/* シンプルな式評価: 整数と + - を左から順に処理する（例: 1+2-3+4） */
static int eval_simple_expression(const char *expr, long long *out_result) {
    const char *p = expr;
    char *endptr;
    errno = 0;
    long long acc = strtoll(p, &endptr, 10);
    if (p == endptr) return -1; // 数字で始まらない
    if (errno) return -1;
    p = endptr;

    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        char op = *p;
        if (op != '+' && op != '-') {
            return -1;
        }
        ++p;
        while (isspace((unsigned char)*p)) ++p;
        if (!(*p)) return -1;
        errno = 0;
        long long val = strtoll(p, &endptr, 10);
        if (p == endptr) return -1;
        if (errno) return -1;
        if (op == '+') acc += val;
        else acc -= val;
        p = endptr;
    }
    *out_result = acc;
    return 0;
}

/* HTTPレスポンスを書いてクローズするユーティリティ */
static void send_response_and_close(int sock, int status_code, const char *status_text, const char *body) {
    char header[512];
    size_t bodylen = body ? strlen(body) : 0;
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Content-Type: text/plain\r\n"
                     "\r\n",
                     status_code, status_text, bodylen);
    if (n < 0) {
        perror("snprintf");
        close(sock);
        return;
    }
    if (write(sock, header, (size_t)n) < 0) perror("write(header)");
    if (bodylen > 0) {
        if (write(sock, body, bodylen) < 0) perror("write(body)");
    }
    shutdown(sock, SHUT_RDWR);
    close(sock);
}

int main(void) {
    int rsock = -1, csock = -1;
    struct sockaddr_in addr;
    struct sockaddr_in cli;
    socklen_t cli_len = sizeof(cli);

    /* ソケット作成 */
    rsock = socket(AF_INET, SOCK_STREAM, 0);
    if (rsock < 0) { perror("socket"); return 1; }

    /* SO_REUSEADDR を有効にして再起動を容易にする（学習用） */
    int opt = 1;
    if (setsockopt(rsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(rsock);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(rsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(rsock);
        return 1;
    }

    if (listen(rsock, BACKLOG) < 0) {
        perror("listen");
        close(rsock);
        return 1;
    }

    printf("Listening on port %d ...\n", PORT);

    /* 1接続だけ連続的に受け付けて処理する（学習用、ループして複数処理するのも可） */
    for (;;) {
        csock = accept(rsock, (struct sockaddr*)&cli, &cli_len);
        if (csock < 0) {
            perror("accept");
            close(rsock);
            return 1;
        }

        /* リクエストを受け取る（簡易：単発readで済ませる） */
        char *buf = malloc(RECV_BUFSIZE);
        if (!buf) {
            perror("malloc");
            send_response_and_close(csock, 500, "Internal Server Error", "malloc failed");
            continue;
        }
        ssize_t r = read(csock, buf, RECV_BUFSIZE - 1);
        if (r < 0) {
            perror("read");
            free(buf);
            send_response_and_close(csock, 500, "Internal Server Error", "read failed");
            continue;
        }
        buf[r] = '\0';

        /* リクエストの先頭行（例: GET /calc?query=2+10 HTTP/1.1）を抽出 */
        char method[16] = {0}, path[1024] = {0}, proto[32] = {0};
        if (sscanf(buf, "%15s %1023s %31s", method, path, proto) != 3) {
            free(buf);
            send_response_and_close(csock, 400, "Bad Request", "invalid request-line");
            continue;
        }

        /* GET かどうかとパスが /calc かどうかを判定 */
        if (strcmp(method, "GET") != 0) {
            free(buf);
            send_response_and_close(csock, 405, "Method Not Allowed", "only GET allowed");
            continue;
        }

        /* path に ?query=... があるか探す */
        char *qpos = strstr(path, "/calc?");
        if (!qpos) {
            free(buf);
            send_response_and_close(csock, 404, "Not Found", "expected /calc?query=...");
            continue;
        }

        char *query_str = strstr(path, "query=");
        if (!query_str) {
            free(buf);
            send_response_and_close(csock, 400, "Bad Request", "missing query parameter");
            continue;
        }

        query_str += strlen("query="); /* query= の直後を指す */

        /* query の終端はスペースまたは&または文字列終端 */
        char qval[1024] = {0};
        size_t i = 0;
        while (query_str[i] && query_str[i] != ' ' && query_str[i] != '&' && i + 1 < sizeof(qval)) {
            qval[i] = query_str[i];
            ++i;
        }
        qval[i] = '\0';

        /* URLデコード（%XX を処理） */
        char *decoded = url_decode(qval);
        if (!decoded) {
            free(buf);
            send_response_and_close(csock, 500, "Internal Server Error", "url decode failed");
            continue;
        }

        /* 簡易式評価 */
        long long result;
        if (eval_simple_expression(decoded, &result) != 0) {
            free(decoded);
            free(buf);
            send_response_and_close(csock, 400, "Bad Request", "invalid expression");
            continue;
        }

        /* 結果を文字列にして返す */
        char body[64];
        int blen = snprintf(body, sizeof(body), "%lld", result);
        if (blen < 0) blen = 0;
        send_response_and_close(csock, 200, "OK", body);

        free(decoded);
        free(buf);

        /* 学習用: 一回処理して終了したければここでbreak; 連続で処理するならループ継続 */
        /* break; */
    }

    close(rsock);
    return 0;
}
