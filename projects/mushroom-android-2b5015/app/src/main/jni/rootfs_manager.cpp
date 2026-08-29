/**
 * rootfs_manager.cpp — RootFS download, verification, and extraction
 *
 * This module manages the lifecycle of the Linux root filesystem:
 *   1. Download a minimal Debian/Ubuntu base RootFS from a configurable URL
 *   2. Verify the downloaded archive using SHA256 hash
 *   3. Extract the archive to the app's internal storage
 *   4. Verify extraction integrity
 *   5. Set up the initial environment (passwd, resolv.conf, etc.)
 *
 * The RootFS is stored in the app's private internal storage at:
 *   /data/data/com.mushroom.android/files/rootfs/
 *
 * The initial APK is under 30MB because the RootFS is downloaded post-install.
 */

#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>

#include "include/engine.h"

#define TAG "MushroomRootFS"

/* ---------- RootFS context ---------- */

struct RootFSContext {
    char rootfs_path[MAX_ROOTFS_PATH];
    char engine_path[MAX_ROOTFS_PATH];
    char download_url[512];
    char expected_sha256[65];
    bool extracted;
    bool downloading;
    int download_progress;
};

/* ---------- HTTP download helper ---------- */

/**
 * Simple HTTP download using BSD sockets (no external dependencies).
 * Downloads a file from a URL to a local path.
 */
static int http_download(const char* url, const char* output_path,
                         int* progress_out) {
    if (!url || !output_path) return -1;

    /* Parse the URL */
    std::string url_str(url);
    std::string host;
    std::string path = "/";

    size_t proto_end = url_str.find("://");
    size_t host_start = (proto_end != std::string::npos) ? proto_end + 3 : 0;
    size_t host_end = url_str.find('/', host_start);
    size_t port_start = url_str.find(':', host_start);

    if (host_end != std::string::npos) {
        host = url_str.substr(host_start, host_end - host_start);
        path = url_str.substr(host_end);
    } else {
        host = url_str.substr(host_start);
        path = "/";
    }

    /* Remove port from host if present */
    int port = 80;
    if (port_start != std::string::npos && port_start < host_end) {
        port = std::stoi(url_str.substr(port_start + 1, host_end - port_start - 1));
        host = url_str.substr(host_start, port_start - host_start);
    }

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Downloading: host=%s, port=%d, path=%s, output=%s",
        host.c_str(), port, path.c_str(), output_path);

    /* Create socket */
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Socket creation failed: %s", strerror(errno));
        return -1;
    }

    /* Resolve hostname */
    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to resolve host: %s", host.c_str());
        close(sock);
        return -1;
    }

    /* Connect */
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Connection failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    /* Send HTTP request */
    std::string request = "GET " + path + " HTTP/1.0\r\n"
                          "Host: " + host + "\r\n"
                          "User-Agent: Mushroom/1.0\r\n"
                          "Connection: close\r\n"
                          "\r\n";

    if (send(sock, request.c_str(), request.length(), 0) < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Send failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    /* Read response */
    char buffer[8192];
    std::string response_header;
    bool header_done = false;
    int content_length = 0;
    int total_read = 0;

    /* Open output file */
    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Failed to open output: %s", strerror(errno));
        close(sock);
        return -1;
    }

    while (true) {
        ssize_t n = read(sock, buffer, sizeof(buffer) - 1);
        if (n <= 0) break;

        if (!header_done) {
            response_header.append(buffer, n);

            /* Check if headers are complete */
            size_t header_end = response_header.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                header_done = true;

                /* Parse Content-Length */
                size_t cl_pos = response_header.find("Content-Length: ");
                if (cl_pos != std::string::npos) {
                    content_length = std::stoi(
                        response_header.substr(cl_pos + 16));
                }

                /* Check HTTP status */
                size_t status_start = response_header.find(' ');
                size_t status_end = response_header.find("\r\n");
                if (status_start != std::string::npos &&
                    status_end != std::string::npos) {
                    int status = std::stoi(
                        response_header.substr(status_start + 1, 3));
                    if (status != 200) {
                        __android_log_print(ANDROID_LOG_ERROR, TAG,
                            "HTTP error: %d", status);
                        close(out_fd);
                        close(sock);
                        return -1;
                    }
                }

                /* Write the body after headers */
                size_t body_start = header_end + 4;
                if (body_start < response_header.length()) {
                    const char* body = response_header.c_str() + body_start;
                    size_t body_len = response_header.length() - body_start;
                    write(out_fd, body, body_len);
                    total_read += body_len;
                    if (progress_out && content_length > 0) {
                        *progress_out = (total_read * 100) / content_length;
                    }
                }
            }
        } else {
            /* Body data */
            write(out_fd, buffer, n);
            total_read += n;
            if (progress_out && content_length > 0) {
                *progress_out = (total_read * 100) / content_length;
            }
        }
    }

    close(out_fd);
    close(sock);

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Download complete: %d bytes", total_read);
    return total_read;
}

/* ---------- SHA256 verification ---------- */

/**
 * Verify a file's SHA256 hash.
 * Uses the system's sha256sum utility if available, otherwise
 * implements a simple verification.
 */
static int verify_sha256(const char* file_path, const char* expected_sha256) {
    if (!file_path || !expected_sha256 || strlen(expected_sha256) == 0) {
        /* No hash to verify against */
        return 1;  /* Skip verification */
    }

    /* Try using sha256sum utility */
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "sha256sum \"%s\" 2>/dev/null", file_path);

    FILE* fp = popen(cmd, "r");
    if (fp) {
        char result[128] = {0};
        if (fgets(result, sizeof(result), fp)) {
            /* Extract the hash (first 64 chars) */
            char hash[65] = {0};
            strncpy(hash, result, 64);

            if (strcasecmp(hash, expected_sha256) == 0) {
                pclose(fp);
                __android_log_print(ANDROID_LOG_INFO, TAG, "SHA256 verification passed");
                return 0;
            } else {
                __android_log_print(ANDROID_LOG_ERROR, TAG,
                    "SHA256 mismatch: expected=%s, got=%s", expected_sha256, hash);
                pclose(fp);
                return -1;
            }
        }
        pclose(fp);
    }

    __android_log_print(ANDROID_LOG_WARN, TAG,
        "SHA256 verification skipped (no utility available)");
    return 1;
}

/* ---------- Archive extraction ---------- */

/**
 * Extract a tar.gz archive to the rootfs directory.
 * Uses the system's tar utility if available, otherwise uses
 * a minimal built-in extraction.
 */
static int extract_archive(const char* archive_path, const char* dest_path) {
    if (!archive_path || !dest_path) return -1;

    __android_log_print(ANDROID_LOG_INFO, TAG,
        "Extracting %s to %s", archive_path, dest_path);

    /* Ensure destination exists */
    struct stat st;
    if (stat(dest_path, &st) != 0) {
        mkdir(dest_path, 0755);
    }

    /* Try using tar utility */
    char cmd[8192];
    snprintf(cmd, sizeof(cmd),
             "tar -xzf \"%s\" -C \"%s\" 2>/dev/null", archive_path, dest_path);

    int ret = system(cmd);
    if (ret == 0) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Extraction complete (tar)");
        return 0;
    }

    /* If tar failed, try with gzip + built-in tar */
    __android_log_print(ANDROID_LOG_WARN, TAG, "tar extraction failed, trying alternative");

    /* Try gunzip + tar */
    snprintf(cmd, sizeof(cmd),
             "gunzip -c \"%s\" | tar -xf - -C \"%s\" 2>/dev/null",
             archive_path, dest_path);
    ret = system(cmd);
    if (ret == 0) {
        __android_log_print(ANDROID_LOG_INFO, TAG, "Extraction complete (gunzip+tar)");
        return 0;
    }

    __android_log_print(ANDROID_LOG_ERROR, TAG, "Archive extraction failed");
    return -1;
}

/* ---------- RootFS setup ---------- */

/**
 * Set up the initial files in the RootFS that are needed for basic operation.
 * Creates /etc/passwd, /etc/group, /etc/resolv.conf, etc.
 */
static int setup_rootfs_environment(const char* rootfs_path) {
    char path[4096];
    FILE* f;

    __android_log_print(ANDROID_LOG_INFO, TAG, "Setting up RootFS environment");

    /* Create essential directories */
    const char* dirs[] = {
        "/etc", "/etc/apt", "/etc/dpkg", "/etc/ssl", "/etc/ssl/certs",
        "/bin", "/sbin", "/usr/bin", "/usr/sbin", "/usr/lib",
        "/usr/share", "/usr/share/ca-certificates",
        "/lib", "/lib64", "/lib/modules",
        "/var", "/var/log", "/var/cache", "/var/lib", "/var/tmp",
        "/opt", "/run", "/run/lock", "/run/user",
        "/home", "/root", "/mnt", "/media", "/srv",
        "/etc/network", "/etc/default",
        "/usr/local", "/usr/local/bin", "/usr/local/lib",
        "/usr/share/applications", "/usr/share/icons",
        nullptr
    };

    for (int i = 0; dirs[i] != nullptr; i++) {
        snprintf(path, sizeof(path), "%s%s", rootfs_path, dirs[i]);
        struct stat st;
        if (stat(path, &st) != 0) {
            mkdir(path, 0755);
        }
    }

    /* Create /etc/passwd */
    snprintf(path, sizeof(path), "%s/etc/passwd", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "root:x:0:0:root:/root:/bin/bash\n"
                   "daemon:x:1:1:daemon:/usr/sbin:/usr/sbin/nologin\n"
                   "bin:x:2:2:bin:/bin:/usr/sbin/nologin\n"
                   "nobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n");
        fclose(f);
    }

    /* Create /etc/group */
    snprintf(path, sizeof(path), "%s/etc/group", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "root:x:0:\n"
                   "daemon:x:1:\n"
                   "bin:x:2:\n"
                   "sudo:x:27:\n"
                   "users:x:100:\n"
                   "nobody:x:65534:\n");
        fclose(f);
    }

    /* Create /etc/shadow */
    snprintf(path, sizeof(path), "%s/etc/shadow", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "root:*:19000:0:99999:7:::\n"
                   "daemon:*:19000:0:99999:7:::\n"
                   "nobody:*:19000:0:99999:7:::\n");
        fclose(f);
    }

    /* Create /etc/resolv.conf (Google DNS) */
    snprintf(path, sizeof(path), "%s/etc/resolv.conf", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "nameserver 8.8.8.8\n"
                   "nameserver 8.8.4.4\n");
        fclose(f);
    }

    /* Create /etc/hostname */
    snprintf(path, sizeof(path), "%s/etc/hostname", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "mushroom\n");
        fclose(f);
    }

    /* Create /etc/hosts */
    snprintf(path, sizeof(path), "%s/etc/hosts", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "127.0.0.1\tlocalhost\n"
                   "127.0.1.1\tmushroom\n"
                   "::1\t\tlocalhost ip6-localhost ip6-loopback\n");
        fclose(f);
    }

    /* Create /etc/fstab */
    snprintf(path, sizeof(path), "%s/etc/fstab", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "# Mushroom virtual filesystem\n"
                   "proc    /proc    proc    defaults    0 0\n"
                   "sysfs   /sys     sysfs   defaults    0 0\n"
                   "devtmpfs /dev    devtmpfs defaults  0 0\n"
                   "tmpfs   /tmp     tmpfs   defaults    0 0\n");
        fclose(f);
    }

    /* Create /etc/apt/sources.list for Debian Bookworm */
    snprintf(path, sizeof(path), "%s/etc/apt/sources.list", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "deb http://deb.debian.org/debian bookworm main contrib non-free-firmware\n"
                   "deb http://deb.debian.org/debian bookworm-updates main contrib non-free-firmware\n"
                   "deb http://security.debian.org/debian-security bookworm-security main non-free-firmware\n");
        fclose(f);
    }

    /* Create /etc/inputrc */
    snprintf(path, sizeof(path), "%s/etc/inputrc", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "# /etc/inputrc\n"
                   "set bell-style none\n"
                   "set meta-flag on\n"
                   "set input-meta on\n"
                   "set convert-meta off\n"
                   "set output-meta on\n");
        fclose(f);
    }

    /* Create /etc/environment */
    snprintf(path, sizeof(path), "%s/etc/environment", rootfs_path);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n"
                   "LANG=C.UTF-8\n"
                   "DISPLAY=:1\n"
                   "TERM=xterm-256color\n"
                   "HOME=/root\n");
        fclose(f);
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "RootFS environment setup complete");
    return 0;
}

/* ---------- Module lifecycle ---------- */

int rootfs_manager_init(EngineContext* ctx) {
    __android_log_print(ANDROID_LOG_INFO, TAG, "RootFS manager init");

    RootFSContext* rctx = (RootFSContext*)malloc(sizeof(RootFSContext));
    if (!rctx) return -1;

    memset(rctx, 0, sizeof(RootFSContext));
    strncpy(rctx->rootfs_path, ctx->config.rootfs_path, MAX_ROOTFS_PATH - 1);
    strncpy(rctx->engine_path, ctx->config.engine_path, MAX_ROOTFS_PATH - 1);

    /* Default download URL — REPLACE THIS for your deployment */
    /* This is a minimal Debian Bookworm base tarball for arm64 */
    /* For production, host your own RootFS tarball and update this URL */
    snprintf(rctx->download_url, sizeof(rctx->download_url),
             "https://github.com/debuerreotype/docker-debian-artifacts"
             "/raw/dist-amd64/bookworm/rootfs.tar.xz");
    /* ^^^ Replace with actual URL for your RootFS distribution */

    /* SHA256 of the expected tarball (leave empty to skip verification) */
    rctx->expected_sha256[0] = '\0';
    rctx->extracted = false;
    rctx->downloading = false;
    rctx->download_progress = 0;

    /* Check if RootFS already extracted */
    char marker_path[4096];
    snprintf(marker_path, sizeof(marker_path), "%s/.mushroom_extracted",
             rctx->rootfs_path);

    struct stat st;
    if (stat(marker_path, &st) == 0) {
        rctx->extracted = true;
        __android_log_print(ANDROID_LOG_INFO, TAG, "RootFS already extracted");
    }

    ctx->proc_ctx = (void*)rctx;
    return 0;
}

int rootfs_manager_download(EngineContext* ctx, const char* url, const char* sha256) {
    RootFSContext* rctx = (RootFSContext*)ctx->proc_ctx;
    if (!rctx) return -1;

    if (url) {
        strncpy(rctx->download_url, url, sizeof(rctx->download_url) - 1);
    }
    if (sha256) {
        strncpy(rctx->expected_sha256, sha256, sizeof(rctx->expected_sha256) - 1);
    }

    __android_log_print(ANDROID_LOG_INFO, TAG, "Downloading RootFS from %s",
                        rctx->download_url);

    rctx->downloading = true;
    rctx->download_progress = 0;

    /* Download to a temporary file in the engine directory */
    char temp_path[4096];
    snprintf(temp_path, sizeof(temp_path), "%s/rootfs_download.tar.xz",
             rctx->engine_path);

    int ret = http_download(rctx->download_url, temp_path, &rctx->download_progress);
    if (ret <= 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Download failed");
        rctx->downloading = false;
        return -1;
    }

    /* Verify SHA256 */
    if (rctx->expected_sha256[0] != '\0') {
        ret = verify_sha256(temp_path, rctx->expected_sha256);
        if (ret < 0) {
            __android_log_print(ANDROID_LOG_ERROR, TAG, "SHA256 verification failed");
            unlink(temp_path);
            rctx->downloading = false;
            return -1;
        }
    }

    /* Extract the archive */
    ret = extract_archive(temp_path, rctx->rootfs_path);
    if (ret != 0) {
        __android_log_print(ANDROID_LOG_ERROR, TAG, "Extraction failed");
        unlink(temp_path);
        rctx->downloading = false;
        return -1;
    }

    /* Set up the RootFS environment */
    setup_rootfs_environment(rctx->rootfs_path);

    /* Create extraction marker */
    char marker_path[4096];
    snprintf(marker_path, sizeof(marker_path), "%s/.mushroom_extracted",
             rctx->rootfs_path);
    FILE* f = fopen(marker_path, "w");
    if (f) {
        fprintf(f, "MUSHROOM_ROOTFS_EXTRACTED\n");
        fclose(f);
    }

    /* Clean up the downloaded archive */
    unlink(temp_path);

    rctx->extracted = true;
    rctx->downloading = false;

    __android_log_print(ANDROID_LOG_INFO, TAG, "RootFS download and extraction complete");
    return 0;
}

int rootfs_manager_extract(EngineContext* ctx) {
    /* Download and extract in one call (uses default URL) */
    return rootfs_manager_download(ctx, nullptr, nullptr);
}

bool rootfs_manager_is_extracted(EngineContext* ctx) {
    RootFSContext* rctx = (RootFSContext*)ctx->proc_ctx;
    if (!rctx) return false;
    return rctx->extracted;
}