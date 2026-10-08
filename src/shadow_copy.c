#include "shadow_copy.h"
#include "ownership_table_internal.h"

#include <stdio.h>
#include <string.h>

static int force_delete_failure;

void shadow_copy_set_delete_failure_for_testing(int enabled)
{
    force_delete_failure = enabled != 0;
}

ManagerResult shadow_copy_delete_path(const char *path)
{
    if (path == NULL || path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }
    if (force_delete_failure || remove(path) != 0) {
        return MANAGER_FILE_COPY_FAILED;
    }
    return MANAGER_OK;
}

static ManagerResult read_stream_metadata(FILE *file, ShadowMetadata *metadata)
{
    unsigned char buffer[8192];
    size_t bytes_read;
    unsigned long long hash = 1469598103934665603ULL;
    unsigned long long size = 0;
    size_t i;

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        size += (unsigned long long)bytes_read;
        for (i = 0; i < bytes_read; ++i) {
            hash ^= (unsigned long long)buffer[i];
            hash *= 1099511628211ULL;
        }
    }

    if (ferror(file) != 0) {
        return MANAGER_METADATA_FAILED;
    }

    metadata->size = size;
    metadata->content_hash = hash;
    return MANAGER_OK;
}

static ManagerResult split_path(const char *path,
                                char *directory,
                                size_t directory_size,
                                char *filename,
                                size_t filename_size)
{
    const char *separator;
    const char *last_separator;
    size_t directory_length;

    separator = strrchr(path, '/');
    last_separator = strrchr(path, '\\');
    if (last_separator != NULL && (separator == NULL || last_separator > separator)) {
        separator = last_separator;
    }

    if (separator == NULL) {
        directory[0] = '\0';
        if (strlen(path) >= filename_size) {
            return MANAGER_INVALID_ARGUMENT;
        }
        (void)snprintf(filename, filename_size, "%s", path);
        return MANAGER_OK;
    }

    directory_length = (size_t)(separator - path);
    if (directory_length == 0 || directory_length >= directory_size ||
        strlen(separator + 1) >= filename_size) {
        return MANAGER_INVALID_ARGUMENT;
    }

    (void)snprintf(directory, directory_size, "%.*s",
                   (int)directory_length, path);
    (void)snprintf(filename, filename_size, "%s", separator + 1);
    return MANAGER_OK;
}

static ManagerResult build_candidate_path(const char *original_path,
                                          int suffix,
                                          char *candidate,
                                          size_t candidate_size)
{
    char directory[MAX_PATH_LENGTH];
    char filename[MAX_PATH_LENGTH];
    char stem[MAX_PATH_LENGTH];
    char extension[MAX_PATH_LENGTH];
    const char *dot;
    int written;

    if (original_path == NULL || candidate == NULL || suffix <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (split_path(original_path,
                   directory,
                   sizeof(directory),
                   filename,
                   sizeof(filename)) != MANAGER_OK) {
        return MANAGER_INVALID_ARGUMENT;
    }

    dot = strrchr(filename, '.');
    if (dot == NULL || dot == filename) {
        if (strlen(filename) >= sizeof(stem)) {
            return MANAGER_INVALID_ARGUMENT;
        }
        (void)snprintf(stem, sizeof(stem), "%s", filename);
        extension[0] = '\0';
    } else {
        size_t stem_length = (size_t)(dot - filename);

        if (stem_length >= sizeof(stem) ||
            strlen(dot) >= sizeof(extension)) {
            return MANAGER_INVALID_ARGUMENT;
        }
        (void)snprintf(stem, sizeof(stem), "%.*s", (int)stem_length, filename);
        (void)snprintf(extension, sizeof(extension), "%s", dot);
    }

    if (directory[0] == '\0') {
        written = snprintf(candidate,
                           candidate_size,
                           "%s_S_%d%s",
                           stem,
                           suffix,
                           extension);
    } else {
        written = snprintf(candidate,
                           candidate_size,
                           "%s\\%s_S_%d%s",
                           directory,
                           stem,
                           suffix,
                           extension);
    }

    if (written < 0 || (size_t)written >= candidate_size) {
        return MANAGER_INVALID_ARGUMENT;
    }

    return MANAGER_OK;
}

static int file_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }

    (void)fclose(file);
    return 1;
}

static ManagerResult copy_file(const char *source_path, const char *target_path)
{
    FILE *source;
    FILE *target;
    unsigned char buffer[8192];
    size_t bytes_read;

    source = fopen(source_path, "rb");
    if (source == NULL) {
        return MANAGER_FILE_COPY_FAILED;
    }

    target = fopen(target_path, "wb");
    if (target == NULL) {
        (void)fclose(source);
        return MANAGER_FILE_COPY_FAILED;
    }

    while ((bytes_read = fread(buffer, 1, sizeof(buffer), source)) > 0) {
        if (fwrite(buffer, 1, bytes_read, target) != bytes_read) {
            (void)fclose(source);
            (void)fclose(target);
            (void)remove(target_path);
            return MANAGER_FILE_COPY_FAILED;
        }
    }

    if (ferror(source) != 0) {
        (void)fclose(source);
        (void)fclose(target);
        (void)remove(target_path);
        return MANAGER_FILE_COPY_FAILED;
    }

    {
        int source_close_result = fclose(source);
        int target_close_result = fclose(target);

        if (source_close_result != 0 || target_close_result != 0) {
            (void)remove(target_path);
            return MANAGER_FILE_COPY_FAILED;
        }
    }

    return MANAGER_OK;
}

ManagerResult shadow_copy_create(const char *original_path,
                                 const Application *owner,
                                 unsigned long creation_version,
                                 int shadow_copy_id,
                                 ShadowCopy *out_shadow_copy)
{
    char candidate[MAX_PATH_LENGTH];
    int suffix;
    ManagerResult result;

    if (original_path == NULL || owner == NULL || out_shadow_copy == NULL ||
        original_path[0] == '\0' || shadow_copy_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (!file_exists(original_path)) {
        return MANAGER_FILE_COPY_FAILED;
    }

    for (suffix = 1; suffix <= MAX_SHADOW_COPIES; ++suffix) {
        result = build_candidate_path(original_path,
                                      suffix,
                                      candidate,
                                      sizeof(candidate));
        if (result != MANAGER_OK) {
            return result;
        }

        if (!file_exists(candidate)) {
            result = copy_file(original_path, candidate);
            if (result != MANAGER_OK) {
                return result;
            }

            memset(out_shadow_copy, 0, sizeof(*out_shadow_copy));
            out_shadow_copy->shadow_copy_id = shadow_copy_id;
            (void)snprintf(out_shadow_copy->original_path,
                           sizeof(out_shadow_copy->original_path),
                           "%s",
                           original_path);
            (void)snprintf(out_shadow_copy->shadow_path,
                           sizeof(out_shadow_copy->shadow_path),
                           "%s",
                           candidate);
            out_shadow_copy->owner = *owner;
            out_shadow_copy->creation_version = creation_version;
            out_shadow_copy->is_temporary = 1;
            result = shadow_copy_read_metadata(candidate,
                                               &out_shadow_copy->creation_metadata);
            if (result != MANAGER_OK) {
                (void)remove(candidate);
                return result;
            }
            out_shadow_copy->current_metadata =
                out_shadow_copy->creation_metadata;
            out_shadow_copy->access_mode = ACCESS_READ;
            out_shadow_copy->file_state = FILE_STATE_TEMPORARY_SHADOW;
            out_shadow_copy->request_state = REQUEST_ACTIVE;
            return MANAGER_OK;
        }
    }

    return MANAGER_CAPACITY_REACHED;
}

ManagerResult shadow_copy_delete(const ShadowCopy *shadow_copy)
{
    if (shadow_copy == NULL || shadow_copy->shadow_path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }

    return shadow_copy_delete_path(shadow_copy->shadow_path);
}

ManagerResult shadow_copy_create_diff_copy(const char *shadow_path,
                                           int diff_id,
                                           char *diff_path,
                                           size_t diff_path_size)
{
    int written;

    if (shadow_path == NULL || diff_path == NULL || diff_id <= 0 ||
        diff_path_size == 0 || shadow_path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }

    written = snprintf(diff_path,
                       diff_path_size,
                       "%s.diff_%d",
                       shadow_path,
                       diff_id);
    if (written < 0 || (size_t)written >= diff_path_size) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (file_exists(diff_path)) {
        return MANAGER_ALREADY_EXISTS;
    }

    return copy_file(shadow_path, diff_path);
}

ManagerResult shadow_copy_read_metadata(const char *path,
                                        ShadowMetadata *metadata)
{
    FILE *file;
    ManagerResult result;

    if (path == NULL || metadata == NULL || path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = fopen(path, "rb");
    if (file == NULL) {
        return MANAGER_METADATA_FAILED;
    }

    result = read_stream_metadata(file, metadata);
    if (fclose(file) != 0 && result == MANAGER_OK) {
        return MANAGER_METADATA_FAILED;
    }

    return result;
}

int shadow_copy_metadata_equal(const ShadowMetadata *left,
                               const ShadowMetadata *right)
{
    return left != NULL && right != NULL &&
           left->size == right->size &&
           left->content_hash == right->content_hash;
}
