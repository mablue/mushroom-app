/**
 * RootFS Manager
 * 
 * Handles downloading, verifying, and extracting Linux root filesystem
 * from remote sources to internal storage.
 */

#include <jni.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <curl/curl.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <android/log.h>
#include <dirent.h>
#include <openssl/sha.h>

#define LOG_TAG "Mushroom/RootFS"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Configuration - Replace with actual URL and SHA256 for production
#define ROOTFS_URL "https://images.mushroom-linux.dev/base/debian-base.tar.gz"
#define ROOTFS_SHA256 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define MIN_ROOTFS_SIZE (100 * 1024 * 1024)  // 100MB minimum

typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} MemoryBuffer;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t real_size = size * nmemb;
    MemoryBuffer* mem = (MemoryBuffer*)userp;
    
    if (mem->size + real_size + 1 > mem->capacity) {
        mem->capacity = (mem->capacity == 0) ? 1024 * 1024 : mem->capacity * 2;
        while (mem->size + real_size + 1 > mem->capacity) {
            mem->capacity *= 2;
        }
        char* tmp = (char*)realloc(mem->data, mem->capacity);
        if (tmp == NULL) {
            return 0;
        }
        mem->data = tmp;
    }
    
    memcpy(&(mem->data[mem->size]), contents, real_size);
    mem->size += real_size;
    mem->data[mem->size] = '\0';
    
    return real_size;
}

extern "C" {

int download_and_extract_rootfs(const char* dest_path) {
    CURL* curl;
    CURLcode res;
    MemoryBuffer chunk = { .data = NULL, .size = 0, .capacity = 0 };
    
    // Check if rootfs already exists and is valid
    struct stat st;
    long current_size = get_directory_size(dest_path);
    if (current_size >= MIN_ROOTFS_SIZE) {
        // Verify essential directories exist
        const char* essentials[] = {"/bin", "/usr", "/lib", "/etc", NULL};
        bool all_exist = true;
        for (int i = 0; essentials[i]; i++) {
            char check_path[4096];
            snprintf(check_path, sizeof(check_path), "%s%s", dest_path, essentials[i]);
            if (stat(check_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
                all_exist = false;
                break;
            }
        }
        if (all_exist) {
            LOGI("RootFS already exists at %s (%ld bytes)", dest_path, current_size);
            return 0;
        }
    }
    
    // Create destination directory
    mkdir(dest_path, 0755);
    
    // Download the tarball
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    
    if (!curl) {
        LOGE("Failed to initialize CURL");
        return -1;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, ROOTFS_URL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &chunk);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        LOGE("Download failed: %s", curl_easy_strerror(res));
        curl_easy_cleanup(curl);
        free(chunk.data);
        return -1;
    }
    
    curl_easy_cleanup(curl);
    
    // Verify SHA256
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)chunk.data, chunk.size, hash);
    
    char hex_hash[65];
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        sprintf(hex_hash + i * 2, "%02x", hash[i]);
    }
    hex_hash[64] = '\0';
    
    if (strcmp(hex_hash, ROOTFS_SHA256) != 0) {
        LOGE("SHA256 mismatch! Expected: %s Got: %s", ROOTFS_SHA256, hex_hash);
        free(chunk.data);
        return -1;
    }
    
    // Extract tar.gz to destination
    int ret = extract_tar_gz_chunked(chunk.data, chunk.size, dest_path);
    
    free(chunk.data);
    curl_global_cleanup();
    
    return ret;
}

int extract_tar_gz_chunked(const char* data, size_t size, const char* dest) {
    // Use system tar command for extraction
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), 
             "mkdir -p '%s' && "
             "cd '%s' && "
             "echo '%s' | base64 -d | tar xz", 
             dest, dest, "");
    
    // Alternative: use gunzip and tar directly on the downloaded file
    // For simplicity, we'll use a temporary file approach
    
    // Write data to temp file
    char tmpfile[] = "/tmp/mushroom_rootfs_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        LOGE("Failed to create temp file");
        return -1;
    }
    
    write(fd, data, size);
    close(fd);
    
    // Extract using tar
    char extract_cmd[8192];
    snprintf(extract_cmd, sizeof(extract_cmd), 
             "tar xzf '%s' -C '%s' 2>&1", 
             tmpfile, dest);
    
    int ret = system(extract_cmd);
    
    unlink(tmpfile);
    
    return (ret == 0) ? 0 : -1;
}

long get_directory_size(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    
    long total_size = st.st_size;
    
    DIR* dir = opendir(path);
    if (!dir) return total_size;
    
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, ent->d_name);
        
        struct stat entry_stat;
        if (stat(full_path, &entry_stat) == 0) {
            if (S_ISDIR(entry_stat.st_mode)) {
                total_size += get_directory_size(full_path);
            } else {
                total_size += entry_stat.st_size;
            }
        }
    }
    closedir(dir);
    
    return total_size;
}

int check_rootfs_exists(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0;
    }
    
    const char* essentials[] = {"/bin", "/usr", "/lib", "/etc", NULL};
    for (int i = 0; essentials[i]; i++) {
        char check_path[4096];
        snprintf(check_path, sizeof(check_path), "%s%s", path, essentials[i]);
        if (stat(check_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            return 0;
        }
    }
    
    return 1;
}

} // extern "C"
