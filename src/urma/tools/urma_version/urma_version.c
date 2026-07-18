/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
 * Description: Query and verify Bazel-built URMA artifact versions
 * Author: URMA Team
 * Create: 2026-07-18
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define VERSION_FIELD_COUNT 7
#define INSTALL_FIELD_COUNT 4
#define VERSION_LINE_SIZE 2048
#define VERSION_METADATA_SIZE 512
#define VERSION_PATH_SIZE 4096
#define COMPARE_BUFFER_SIZE 8192

static const char s_metadata_prefix[] = "URMA_META_V1|";
static const char *const s_metadata_types[] = {
    "type=so|",
    "type=binary|",
    "type=header|",
};

typedef struct version_entry {
    char *type;
    char *component;
    char *path;
    char *version;
    char *soversion;
    char *soname;
    char *origin;
} version_entry_t;

typedef struct install_entry {
    char *type;
    char *path;
} install_entry_t;

static bool is_metadata_end(unsigned char value)
{
    return value == '\0' || value == '\n' || value == '"' || value == ' ';
}

static bool has_valid_metadata_type(const unsigned char *data, size_t size)
{
    size_t i;

    for (i = 0; i < sizeof(s_metadata_types) / sizeof(s_metadata_types[0]); i++) {
        size_t type_size = strlen(s_metadata_types[i]);
        if (size >= type_size && memcmp(data, s_metadata_types[i], type_size) == 0) {
            return true;
        }
    }
    return false;
}

static const unsigned char *find_metadata(const unsigned char *data, size_t size)
{
    size_t prefix_size = sizeof(s_metadata_prefix) - 1;
    size_t i;

    if (size < prefix_size) {
        return NULL;
    }
    for (i = 0; i <= size - prefix_size; i++) {
        if (memcmp(data + i, s_metadata_prefix, prefix_size) == 0 &&
            has_valid_metadata_type(data + i + prefix_size, size - i - prefix_size)) {
            return data + i;
        }
    }
    return NULL;
}

static int copy_metadata(const unsigned char *data, size_t size, char *metadata, size_t metadata_size)
{
    const unsigned char *start = find_metadata(data, size);
    size_t length = 0;

    if (start == NULL) {
        return -ENODATA;
    }
    while ((size_t)(start - data) + length < size && !is_metadata_end(start[length])) {
        if (length + 1 >= metadata_size) {
            return -EOVERFLOW;
        }
        metadata[length] = (char)start[length];
        length++;
    }
    metadata[length] = '\0';
    return 0;
}

static int read_metadata(const char *path, char *metadata, size_t metadata_size)
{
    struct stat file_stat;
    unsigned char *data = NULL;
    size_t size;
    int fd;
    int ret;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -errno;
    }
    if (fstat(fd, &file_stat) != 0) {
        ret = -errno;
        goto close_file;
    }
    if (file_stat.st_size <= 0 || (uintmax_t)file_stat.st_size > SIZE_MAX) {
        ret = -EFBIG;
        goto close_file;
    }
    size = (size_t)file_stat.st_size;
    data = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        ret = -errno;
        data = NULL;
        goto close_file;
    }
    ret = copy_metadata(data, size, metadata, metadata_size);
    if (munmap(data, size) != 0 && ret == 0) {
        ret = -errno;
    }

close_file:
    if (close(fd) != 0 && ret == 0) {
        ret = -errno;
    }
    return ret;
}

static bool is_safe_relative_path(const char *path)
{
    size_t length;

    if (path == NULL || path[0] == '\0' || path[0] == '/') {
        return false;
    }
    length = strlen(path);
    return strcmp(path, "..") != 0 && strncmp(path, "../", 3) != 0 &&
        strstr(path, "/../") == NULL && (length < 3 || strcmp(path + length - 3, "/..") != 0);
}

static int build_artifact_path(const char *root, const char *relative_path, char *path, size_t path_size)
{
    int length = snprintf(path, path_size, "%s/%s", root, relative_path);

    if (length < 0 || (size_t)length >= path_size) {
        return -EOVERFLOW;
    }
    return 0;
}

static int parse_entry(char *line, version_entry_t *entry)
{
    char *fields[VERSION_FIELD_COUNT];
    char *save = NULL;
    char *field = strtok_r(line, "\t", &save);
    size_t i = 0;

    while (field != NULL && i < VERSION_FIELD_COUNT) {
        fields[i++] = field;
        field = strtok_r(NULL, "\t", &save);
    }
    if (i != VERSION_FIELD_COUNT || field != NULL) {
        return -EINVAL;
    }
    entry->type = fields[0];
    entry->component = fields[1];
    entry->path = fields[2];
    entry->version = fields[3];
    entry->soversion = fields[4];
    entry->soname = fields[5];
    entry->origin = fields[6];
    return is_safe_relative_path(entry->path) ? 0 : -EINVAL;
}

static int build_expected_metadata(const version_entry_t *entry, char *expected, size_t expected_size)
{
    int length = snprintf(expected, expected_size,
        "URMA_META_V1|type=%s|component=%s|version=%s|soversion=%s|soname=%s|origin=%s",
        entry->type, entry->component, entry->version, entry->soversion, entry->soname, entry->origin);

    if (length < 0 || (size_t)length >= expected_size) {
        return -EOVERFLOW;
    }
    return 0;
}

static int check_entry(const char *root, const version_entry_t *entry)
{
    char expected[VERSION_METADATA_SIZE];
    char metadata[VERSION_METADATA_SIZE];
    char path[VERSION_PATH_SIZE];
    int ret;

    ret = build_artifact_path(root, entry->path, path, sizeof(path));
    if (ret != 0) {
        return ret;
    }
    ret = build_expected_metadata(entry, expected, sizeof(expected));
    if (ret != 0) {
        return ret;
    }
    ret = read_metadata(path, metadata, sizeof(metadata));
    if (ret != 0) {
        (void)fprintf(stderr, "Cannot read version metadata from %s: %s\n", path, strerror(-ret));
        return ret;
    }
    if (strcmp(metadata, expected) != 0) {
        (void)fprintf(stderr, "Version mismatch in %s\nExpected: %s\nActual:   %s\n", path, expected, metadata);
        return -EINVAL;
    }
    return 0;
}

static int process_manifest_line(char *line, const char *root, unsigned int line_number)
{
    version_entry_t entry;
    int ret;

    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0' || strncmp(line, "type\t", 5) == 0) {
        return 0;
    }
    ret = parse_entry(line, &entry);
    if (ret != 0) {
        (void)fprintf(stderr, "Invalid version manifest line %u\n", line_number);
        return ret;
    }
    ret = check_entry(root, &entry);
    return ret == 0 ? 1 : ret;
}

static int check_manifest(const char *root, const char *manifest)
{
    char line[VERSION_LINE_SIZE];
    unsigned int line_number = 0;
    unsigned int checked = 0;
    FILE *file = fopen(manifest, "r");
    int ret = 0;

    if (file == NULL) {
        return -errno;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            ret = -EOVERFLOW;
            break;
        }
        ret = process_manifest_line(line, root, line_number);
        if (ret < 0) {
            break;
        }
        checked += (unsigned int)ret;
        ret = 0;
    }
    if (ferror(file) != 0 && ret == 0) {
        ret = -EIO;
    }
    if (fclose(file) != 0 && ret == 0) {
        ret = -errno;
    }
    if (ret == 0) {
        (void)printf("Verified %u Bazel artifact version entries.\n", checked);
    }
    return ret;
}

static bool has_suffix(const char *value, const char *suffix)
{
    size_t value_length = strlen(value);
    size_t suffix_length = strlen(suffix);

    return value_length >= suffix_length && strcmp(value + value_length - suffix_length, suffix) == 0;
}

static bool is_shared_object_path(const char *path)
{
    const char *name;
    const char *marker;

    if (strncmp(path, "usr/lib64/", strlen("usr/lib64/")) != 0) {
        return false;
    }
    name = strrchr(path, '/');
    name = name == NULL ? path : name + 1;
    marker = strstr(name, ".so");
    return marker != NULL && (marker[3] == '\0' || marker[3] == '.');
}

static bool is_compared_artifact(const install_entry_t *entry)
{
    bool supported_type = strcmp(entry->type, "f") == 0 || strcmp(entry->type, "l") == 0;

    if (!supported_type) {
        return false;
    }
    return is_shared_object_path(entry->path) ||
        strncmp(entry->path, "usr/bin/", strlen("usr/bin/")) == 0 ||
        (strncmp(entry->path, "usr/include/", strlen("usr/include/")) == 0 && has_suffix(entry->path, ".h"));
}

static int parse_install_entry(char *line, install_entry_t *entry)
{
    char *fields[INSTALL_FIELD_COUNT];
    char *save = NULL;
    char *field = strtok_r(line, "\t", &save);
    size_t i = 0;

    while (field != NULL && i < INSTALL_FIELD_COUNT) {
        fields[i++] = field;
        field = strtok_r(NULL, "\t", &save);
    }
    if (i != INSTALL_FIELD_COUNT || field != NULL) {
        return -EINVAL;
    }
    entry->type = fields[0];
    entry->path = fields[2];
    return is_safe_relative_path(entry->path) ? 0 : -EINVAL;
}

static ssize_t read_retry(int fd, void *buffer, size_t size)
{
    ssize_t length;

    do {
        length = read(fd, buffer, size);
    } while (length < 0 && errno == EINTR);
    return length;
}

static int compare_regular_files(const char *package_path, const char *environment_path)
{
    unsigned char package_data[COMPARE_BUFFER_SIZE];
    unsigned char environment_data[COMPARE_BUFFER_SIZE];
    ssize_t package_length;
    ssize_t environment_length;
    int package_fd = -1;
    int environment_fd = -1;
    int ret = 0;

    package_fd = open(package_path, O_RDONLY);
    if (package_fd < 0) {
        return -errno;
    }
    environment_fd = open(environment_path, O_RDONLY);
    if (environment_fd < 0) {
        ret = -errno;
        goto close_files;
    }
    do {
        package_length = read_retry(package_fd, package_data, sizeof(package_data));
        if (package_length < 0) {
            ret = -errno;
            break;
        }
        environment_length = read_retry(environment_fd, environment_data, sizeof(environment_data));
        if (environment_length < 0) {
            ret = -errno;
            break;
        }
        if (package_length != environment_length ||
            memcmp(package_data, environment_data, (size_t)package_length) != 0) {
            ret = -EINVAL;
            break;
        }
    } while (package_length > 0);

close_files:
    if (environment_fd >= 0 && close(environment_fd) != 0 && ret == 0) {
        ret = -errno;
    }
    if (close(package_fd) != 0 && ret == 0) {
        ret = -errno;
    }
    return ret;
}

static int compare_symlinks(const char *package_path, const char *environment_path)
{
    char package_target[VERSION_PATH_SIZE];
    char environment_target[VERSION_PATH_SIZE];
    ssize_t package_length;
    ssize_t environment_length;

    package_length = readlink(package_path, package_target, sizeof(package_target) - 1);
    if (package_length < 0) {
        return -errno;
    }
    if ((size_t)package_length >= sizeof(package_target) - 1) {
        return -EOVERFLOW;
    }
    environment_length = readlink(environment_path, environment_target, sizeof(environment_target) - 1);
    if (environment_length < 0) {
        return -errno;
    }
    if ((size_t)environment_length >= sizeof(environment_target) - 1) {
        return -EOVERFLOW;
    }
    package_target[package_length] = '\0';
    environment_target[environment_length] = '\0';
    return strcmp(package_target, environment_target) == 0 ? 0 : -EINVAL;
}

static int compare_install_entry(const char *package_root, const char *environment_root,
    const install_entry_t *entry)
{
    char package_path[VERSION_PATH_SIZE];
    char environment_path[VERSION_PATH_SIZE];
    struct stat package_stat;
    struct stat environment_stat;
    int ret;

    ret = build_artifact_path(package_root, entry->path, package_path, sizeof(package_path));
    if (ret == 0) {
        ret = build_artifact_path(environment_root, entry->path, environment_path, sizeof(environment_path));
    }
    if (ret != 0) {
        return ret;
    }
    if (lstat(package_path, &package_stat) != 0 || lstat(environment_path, &environment_stat) != 0) {
        ret = -errno;
    } else if (strcmp(entry->type, "l") == 0) {
        ret = S_ISLNK(package_stat.st_mode) && S_ISLNK(environment_stat.st_mode) ?
            compare_symlinks(package_path, environment_path) : -EINVAL;
    } else if (!S_ISREG(package_stat.st_mode) || !S_ISREG(environment_stat.st_mode) ||
        package_stat.st_size != environment_stat.st_size || package_stat.st_size < 0) {
        ret = -EINVAL;
    } else {
        ret = compare_regular_files(package_path, environment_path);
    }
    if (ret != 0) {
        (void)fprintf(stderr, "Installed artifact differs from package: %s\n", environment_path);
    }
    return ret;
}

static int process_install_manifest_line(char *line, const char *package_root, const char *environment_root,
    unsigned int line_number)
{
    install_entry_t entry;
    int ret;

    line[strcspn(line, "\r\n")] = '\0';
    if (line[0] == '\0') {
        return 0;
    }
    ret = parse_install_entry(line, &entry);
    if (ret != 0) {
        (void)fprintf(stderr, "Invalid install manifest line %u\n", line_number);
        return ret;
    }
    if (!is_compared_artifact(&entry)) {
        return 0;
    }
    ret = compare_install_entry(package_root, environment_root, &entry);
    return ret == 0 ? 1 : ret;
}

static int compare_install_manifest(const char *package_root, const char *environment_root, const char *manifest)
{
    char line[VERSION_LINE_SIZE];
    unsigned int line_number = 0;
    unsigned int compared = 0;
    FILE *file = fopen(manifest, "r");
    int ret = 0;

    if (file == NULL) {
        return -errno;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;
        if (strchr(line, '\n') == NULL && !feof(file)) {
            ret = -EOVERFLOW;
            break;
        }
        ret = process_install_manifest_line(line, package_root, environment_root, line_number);
        if (ret < 0) {
            break;
        }
        compared += (unsigned int)ret;
        ret = 0;
    }
    if (ferror(file) != 0 && ret == 0) {
        ret = -EIO;
    }
    if (fclose(file) != 0 && ret == 0) {
        ret = -errno;
    }
    if (ret != 0) {
        return ret;
    }
    if (compared == 0) {
        return -ENODATA;
    }
    (void)printf("Compared %u installed artifacts with package contents.\n", compared);
    return 0;
}

static void usage(const char *program)
{
    (void)printf("Usage:\n  %s <artifact>\n  %s --check-root <root> <version-manifest>\n"
        "  %s --compare-roots <package-root> <environment-root> <install-manifest>\n",
        program, program, program);
}

int main(int argc, char *argv[])
{
    char metadata[VERSION_METADATA_SIZE];
    const char *operation = "Version operation";
    int ret;

    if (argc == 2) {
        ret = read_metadata(argv[1], metadata, sizeof(metadata));
        if (ret == 0) {
            (void)printf("%s\n", metadata);
        }
    } else if (argc == 4 && strcmp(argv[1], "--check-root") == 0) {
        ret = check_manifest(argv[2], argv[3]);
    } else if (argc == 5 && strcmp(argv[1], "--compare-roots") == 0) {
        operation = "Artifact comparison";
        ret = compare_install_manifest(argv[2], argv[3], argv[4]);
    } else {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (ret != 0) {
        (void)fprintf(stderr, "%s failed: %s\n", operation, strerror(-ret));
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
