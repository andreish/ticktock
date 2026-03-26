/*
 * HTTP/1.1 pipelining test — stronger version.
 *
 * What makes this a real pipelining test vs. pipeline_test.cpp:
 *
 *   1. All requests are delivered in a SINGLE send() call so the server sees
 *      them all at once inside one recv() call, exercising the
 *      process_requests() loop directly.
 *
 *   2. The three GET requests target DIFFERENT endpoints (/api/version,
 *      /api/stats, /api/version again) so a response-ordering bug is
 *      immediately visible: the middle response must look like stats output,
 *      not a second version response.
 *
 *   3. Response parsing is COMPLETE: after reading the header we read exactly
 *      Content-Length bytes of body, so there is no risk of one response's
 *      body bleeding into the next response's header.
 *
 *   4. A second sub-test pipelines a POST immediately followed by a GET.
 *      POST /api/version has no registered handler (→ 404) and GET
 *      /api/version returns 200 JSON; verifying that the server keeps the
 *      correct ordering even across mixed HTTP methods.
 *
 * Assumes a TickTock server is already running on [::1]:8080.
 * Start with: bin/tt -c conf/tt.conf --http.server.port=8080
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string>
#include <stdexcept>

static const char    *SERVER_IP   = "::1";
static const uint16_t SERVER_PORT = 8080;

/* ---- socket helpers ------------------------------------------------------ */

static int connect_to_server()
{
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_port   = htons(SERVER_PORT);
    inet_pton(AF_INET6, SERVER_IP, &addr.sin6_addr);
    if (connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return -1;
    }
    return fd;
}

static void write_all(int fd, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n <= 0) { perror("write"); exit(1); }
        sent += n;
    }
}

/* ---- HTTP response parsing ------------------------------------------------ */

/* Read one byte at a time until the 4-byte sequence \r\n\r\n is seen. */
static std::string read_header(int fd)
{
    std::string hdr;
    char c;
    while (true) {
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) throw std::runtime_error("connection closed reading header");
        hdr.push_back(c);
        if (hdr.size() >= 4 &&
            hdr.compare(hdr.size() - 4, 4, "\r\n\r\n") == 0)
            break;
    }
    return hdr;
}

/* Parse Content-Length value from a response header string. */
static size_t parse_content_length(const std::string &hdr)
{
    const char *needle = "Content-Length:";
    auto pos = hdr.find(needle);
    if (pos == std::string::npos) return 0;
    pos += strlen(needle);
    while (pos < hdr.size() && hdr[pos] == ' ') ++pos;
    auto end = hdr.find('\r', pos);
    if (end == std::string::npos) end = hdr.find('\n', pos);
    if (end == std::string::npos) return 0;
    return std::stoul(hdr.substr(pos, end - pos));
}

/* Read exactly 'len' bytes. */
static std::string read_body(int fd, size_t len)
{
    std::string buf(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, &buf[got], len - got);
        if (n <= 0) throw std::runtime_error("connection closed reading body");
        got += n;
    }
    return buf;
}

/* Read one complete HTTP response (header + body). */
static std::pair<std::string,std::string> read_response(int fd)
{
    std::string hdr  = read_header(fd);
    std::string body = read_body(fd, parse_content_length(hdr));
    return {hdr, body};
}

/* ---- sub-test 1: three GETs in one write --------------------------------- */
/*
 * Sends GET /api/version, GET /api/stats, GET /api/version in a single
 * write() call.  Verifies:
 *   - resp1 is 200 and body contains "version"
 *   - resp2 is 200 and body does NOT contain "version"
 *     (it is the stats response — proves the server did not swap order)
 *   - resp3 is 200 and body contains "version"
 */
static int test_three_gets(int fd)
{
    printf("  test_three_gets: ");

    const char *req =
        "GET /api/version HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        "GET /api/stats   HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n"
        "GET /api/version HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    write_all(fd, req, strlen(req));

    auto [h1, b1] = read_response(fd);
    auto [h2, b2] = read_response(fd);
    auto [h3, b3] = read_response(fd);

    if (h1.find("200") == std::string::npos) {
        fprintf(stderr, "FAIL: resp1 not 200\n"); return 1;
    }
    if (b1.find("\"version\":") == std::string::npos) {
        fprintf(stderr, "FAIL: resp1 body is not a version response\n%s\n", b1.c_str());
        return 1;
    }

    if (h2.find("200") == std::string::npos) {
        fprintf(stderr, "FAIL: resp2 not 200\n"); return 1;
    }
    /* resp2 must be the stats response, NOT a second version response */
    if (b2.find("\"version\":") != std::string::npos) {
        fprintf(stderr, "FAIL: resp2 looks like /api/version — responses are out of order\n");
        return 1;
    }

    if (h3.find("200") == std::string::npos) {
        fprintf(stderr, "FAIL: resp3 not 200\n"); return 1;
    }
    if (b3.find("\"version\":") == std::string::npos) {
        fprintf(stderr, "FAIL: resp3 body is not a version response\n%s\n", b3.c_str());
        return 1;
    }

    printf("PASSED\n");
    return 0;
}

/* ---- sub-test 2: POST immediately followed by GET in one write ----------- */
/*
 * POSTs to /api/version (no POST handler → 404), then GETs /api/version.
 * Both are sent in a single write() call.  Verifies:
 *   - resp1 is 404  (the POST)
 *   - resp2 is 200 with version JSON  (the GET)
 *
 * If the server confuses the order it would either return 200 first (wrong)
 * or hang waiting for data that never arrives.
 */
static int test_post_then_get(int fd)
{
    printf("  test_post_then_get: ");

    /* A POST with a small body.  /api/version has no POST handler → 404. */
    const char *post_body = "ping";
    char post_req[256];
    snprintf(post_req, sizeof(post_req),
        "POST /api/version HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        strlen(post_body), post_body);

    const char *get_req =
        "GET /api/version HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";

    std::string combined(post_req);
    combined += get_req;
    write_all(fd, combined.c_str(), combined.size());

    auto [h1, b1] = read_response(fd);
    auto [h2, b2] = read_response(fd);

    /* First response must be for the POST (404). */
    if (h1.find("404") == std::string::npos) {
        fprintf(stderr, "FAIL: resp1 (POST) expected 404, got: %.80s\n", h1.c_str());
        return 1;
    }

    /* Second response must be for the GET (200 with version JSON). */
    if (h2.find("200") == std::string::npos) {
        fprintf(stderr, "FAIL: resp2 (GET) expected 200, got: %.80s\n", h2.c_str());
        return 1;
    }
    if (b2.find("\"version\":") == std::string::npos) {
        fprintf(stderr, "FAIL: resp2 body is not version JSON: %s\n", b2.c_str());
        return 1;
    }

    printf("PASSED\n");
    return 0;
}

/* -------------------------------------------------------------------------- */

int main()
{
    printf("connecting to [%s]:%d\n", SERVER_IP, SERVER_PORT);

    /* sub-test 1 */
    {
        int fd = connect_to_server();
        if (fd < 0) { fprintf(stderr, "failed to connect\n"); return 1; }
        int rc = test_three_gets(fd);
        close(fd);
        if (rc) return rc;
    }

    /* sub-test 2 */
    {
        int fd = connect_to_server();
        if (fd < 0) { fprintf(stderr, "failed to connect\n"); return 1; }
        int rc = test_post_then_get(fd);
        close(fd);
        if (rc) return rc;
    }

    printf("All pipeline tests passed\n");
    return 0;
}
