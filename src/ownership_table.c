#include "ownership_table.h"
#include "ownership_table_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int valid_table(const OwnershipTable *table)
{
    return table != NULL;
}

static int valid_file(const FileRecord *file)
{
    return file != NULL;
}

OwnershipTable *ownership_table_create(void)
{
    OwnershipTable *table = calloc(1, sizeof(*table));

    if (table != NULL) {
        ownership_table_init(table);
    }
    return table;
}

void ownership_table_destroy(OwnershipTable *table)
{
    free(table);
}

void ownership_table_init(OwnershipTable *table)
{
    if (table == NULL) {
        return;
    }

    memset(table, 0, sizeof(*table));
    table->next_file_id = 1;
    table->next_access_record_id = 1;
    table->next_request_id = 1;
    table->next_shadow_copy_id = 1;
    table->next_change_record_id = 1;
}

ManagerResult ownership_table_get_file_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    FileSnapshot *snapshot)
{
    const FileRecord *file;

    if (table == NULL || original_path == NULL || snapshot == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }
    file = ownership_table_find_file_const(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }
    snapshot->current_version = file->current_version;
    snapshot->write_owner_application_id = file->write_owner_application_id;
    snapshot->modification_active = file->modification_active;
    snapshot->access_record_count = file->access_record_count;
    snapshot->waiting_request_count = file->waiting_request_count;
    snapshot->shadow_copy_count = file->shadow_copy_count;
    return MANAGER_OK;
}

ManagerResult ownership_table_get_access_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    int application_id,
    AccessSnapshot *snapshot)
{
    const FileRecord *file;
    const AccessRecord *record;

    if (table == NULL || original_path == NULL || application_id <= 0 ||
        snapshot == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }
    file = ownership_table_find_file_const(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }
    record = ownership_table_find_active_access_const(file, application_id);
    if (record == NULL) {
        return MANAGER_NOT_FOUND;
    }
    snapshot->access_mode = record->access_mode;
    snapshot->file_state = record->file_state;
    snapshot->request_state = record->request_state;
    (void)snprintf(snapshot->assigned_path,
                   sizeof(snapshot->assigned_path),
                   "%s",
                   record->assigned_path);
    return MANAGER_OK;
}

ManagerResult ownership_table_get_waiting_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    size_t index,
    WaitingRequestSnapshot *snapshot)
{
    const FileRecord *file;
    const WaitingRequest *request;

    if (table == NULL || original_path == NULL || snapshot == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }
    file = ownership_table_find_file_const(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }
    if (index >= file->waiting_request_count) {
        return MANAGER_NOT_FOUND;
    }
    request = &file->waiting_requests[index];
    snapshot->request_id = request->request_id;
    snapshot->application = request->application;
    snapshot->requested_mode = request->requested_mode;
    snapshot->request_state = request->request_state;
    snapshot->transfer_requested = request->transfer_requested;
    return MANAGER_OK;
}

ManagerResult ownership_table_get_shadow_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    size_t index,
    ShadowCopySnapshot *snapshot)
{
    const FileRecord *file;
    const ShadowCopy *shadow;

    if (table == NULL || original_path == NULL || snapshot == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }
    file = ownership_table_find_file_const(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }
    if (index >= file->shadow_copy_count) {
        return MANAGER_NOT_FOUND;
    }
    shadow = &file->shadow_copies[index];
    snapshot->shadow_copy_id = shadow->shadow_copy_id;
    (void)snprintf(snapshot->shadow_path,
                   sizeof(snapshot->shadow_path),
                   "%s",
                   shadow->shadow_path);
    snapshot->owner = shadow->owner;
    snapshot->creation_version = shadow->creation_version;
    snapshot->is_temporary = shadow->is_temporary;
    snapshot->file_state = shadow->file_state;
    snapshot->access_mode = shadow->access_mode;
    return MANAGER_OK;
}

FileRecord *ownership_table_find_file(OwnershipTable *table,
                                      const char *original_path)
{
    size_t i;

    if (!valid_table(table) || original_path == NULL) {
        return NULL;
    }

    for (i = 0; i < table->file_count; ++i) {
        if (strcmp(table->files[i].original_path, original_path) == 0) {
            return &table->files[i];
        }
    }

    return NULL;
}

const FileRecord *ownership_table_find_file_const(const OwnershipTable *table,
                                                  const char *original_path)
{
    size_t i;

    if (!valid_table(table) || original_path == NULL) {
        return NULL;
    }

    for (i = 0; i < table->file_count; ++i) {
        if (strcmp(table->files[i].original_path, original_path) == 0) {
            return &table->files[i];
        }
    }

    return NULL;
}

ManagerResult ownership_table_add_file(OwnershipTable *table,
                                       const char *original_path,
                                       FileRecord **out_file)
{
    FileRecord *file;

    if (!valid_table(table) || original_path == NULL || out_file == NULL ||
        original_path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (strlen(original_path) >= MAX_PATH_LENGTH) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (ownership_table_find_file(table, original_path) != NULL) {
        return MANAGER_ALREADY_EXISTS;
    }

    if (table->file_count >= MAX_FILES) {
        return MANAGER_CAPACITY_REACHED;
    }

    file = &table->files[table->file_count++];
    memset(file, 0, sizeof(*file));
    file->file_id = table->next_file_id++;
    file->current_version = 1;
    file->write_owner_application_id = -1;
    (void)snprintf(file->original_path,
                   sizeof(file->original_path),
                   "%s",
                   original_path);
    *out_file = file;

    return MANAGER_OK;
}

AccessRecord *ownership_table_find_access(FileRecord *file,
                                          int application_id)
{
    size_t i;

    if (!valid_file(file)) {
        return NULL;
    }

    for (i = 0; i < file->access_record_count; ++i) {
        if (file->access_records[i].application.application_id ==
            application_id) {
            return &file->access_records[i];
        }
    }

    return NULL;
}

const AccessRecord *ownership_table_find_access_const(const FileRecord *file,
                                                      int application_id)
{
    size_t i;

    if (!valid_file(file)) {
        return NULL;
    }

    for (i = 0; i < file->access_record_count; ++i) {
        if (file->access_records[i].application.application_id ==
            application_id) {
            return &file->access_records[i];
        }
    }

    return NULL;
}

AccessRecord *ownership_table_find_active_access(FileRecord *file,
                                                 int application_id)
{
    size_t i;

    if (!valid_file(file)) {
        return NULL;
    }

    for (i = 0; i < file->access_record_count; ++i) {
        AccessRecord *record = &file->access_records[i];
        if (record->application.application_id == application_id &&
            record->request_state == REQUEST_ACTIVE) {
            return record;
        }
    }
    return NULL;
}

const AccessRecord *ownership_table_find_active_access_const(
    const FileRecord *file,
    int application_id)
{
    size_t i;

    if (!valid_file(file)) {
        return NULL;
    }

    for (i = 0; i < file->access_record_count; ++i) {
        const AccessRecord *record = &file->access_records[i];
        if (record->application.application_id == application_id &&
            record->request_state == REQUEST_ACTIVE) {
            return record;
        }
    }
    return NULL;
}

WaitingRequest *ownership_table_find_waiting(FileRecord *file,
                                             int application_id)
{
    size_t i;

    if (!valid_file(file)) {
        return NULL;
    }

    for (i = 0; i < file->waiting_request_count; ++i) {
        if (file->waiting_requests[i].application.application_id ==
            application_id) {
            return &file->waiting_requests[i];
        }
    }

    return NULL;
}

ManagerResult ownership_table_add_access(FileRecord *file,
                                         const Application *application,
                                         AccessMode access_mode,
                                         int access_record_id)
{
    return ownership_table_add_access_with_state(file,
                                                 application,
                                                 access_mode,
                                                 FILE_STATE_ORIGINAL,
                                                 NULL,
                                                 -1,
                                                 access_record_id);
}

ManagerResult ownership_table_add_access_with_state(
    FileRecord *file,
    const Application *application,
    AccessMode access_mode,
    FileState file_state,
    const char *assigned_path,
    int shadow_copy_id,
    int access_record_id)
{
    AccessRecord *record;
    size_t i;

    if (!valid_file(file) || application == NULL ||
        (access_mode != ACCESS_READ &&
         access_mode != ACCESS_READ_WRITE) ||
        (file_state != FILE_STATE_ORIGINAL &&
         file_state != FILE_STATE_TEMPORARY_SHADOW &&
         file_state != FILE_STATE_INDEPENDENT_FILE) ||
        shadow_copy_id < -1 || access_record_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (assigned_path != NULL && strlen(assigned_path) >= MAX_PATH_LENGTH) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (ownership_table_find_active_access(file,
                                           application->application_id) != NULL) {
        return MANAGER_ALREADY_EXISTS;
    }

    if (access_mode == ACCESS_READ_WRITE &&
        file_state == FILE_STATE_ORIGINAL) {
        if (file->write_owner_application_id != -1) {
            return MANAGER_WRITE_CONFLICT;
        }
        for (i = 0; i < file->access_record_count; ++i) {
            if (file->access_records[i].request_state == REQUEST_ACTIVE &&
                file->access_records[i].access_mode == ACCESS_READ_WRITE) {
                return MANAGER_WRITE_CONFLICT;
            }
        }
    }

    if (file->access_record_count >= MAX_ACCESS_RECORDS) {
        return MANAGER_CAPACITY_REACHED;
    }

    record = &file->access_records[file->access_record_count++];
    memset(record, 0, sizeof(*record));
    record->access_record_id = access_record_id;
    record->application = *application;
    record->access_mode = access_mode;
    record->file_state = file_state;
    record->request_state = REQUEST_ACTIVE;
    (void)snprintf(record->original_path,
                   sizeof(record->original_path),
                   "%s",
                   file->original_path);
    (void)snprintf(record->assigned_path,
                   sizeof(record->assigned_path),
                   "%s",
                   assigned_path != NULL ? assigned_path : file->original_path);
    record->shadow_copy_id = shadow_copy_id;
    if (access_mode == ACCESS_READ_WRITE &&
        file_state == FILE_STATE_ORIGINAL) {
        file->write_owner_application_id = application->application_id;
    }

    return MANAGER_OK;
}

ManagerResult ownership_table_remove_access(FileRecord *file,
                                            int application_id)
{
    size_t i;

    if (!valid_file(file) || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (i = 0; i < file->access_record_count; ++i) {
        if (file->access_records[i].application.application_id ==
            application_id) {
            if (file->access_records[i].access_mode == ACCESS_READ_WRITE &&
                file->write_owner_application_id == application_id) {
                file->write_owner_application_id = -1;
            }
            file->access_records[i] =
                file->access_records[file->access_record_count - 1];
            file->access_record_count--;
            return MANAGER_OK;
        }
    }

    return MANAGER_NOT_FOUND;
}

ManagerResult ownership_table_add_shadow(FileRecord *file,
                                         const ShadowCopy *shadow_copy)
{
    size_t i;

    if (!valid_file(file) || shadow_copy == NULL ||
        shadow_copy->shadow_copy_id <= 0 ||
        shadow_copy->original_path[0] == '\0' ||
        shadow_copy->shadow_path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (file->shadow_copy_count >= MAX_SHADOW_COPIES) {
        return MANAGER_CAPACITY_REACHED;
    }

    for (i = 0; i < file->shadow_copy_count; ++i) {
        if (file->shadow_copies[i].shadow_copy_id ==
                shadow_copy->shadow_copy_id ||
            strcmp(file->shadow_copies[i].shadow_path,
                   shadow_copy->shadow_path) == 0) {
            return MANAGER_ALREADY_EXISTS;
        }
    }

    file->shadow_copies[file->shadow_copy_count++] = *shadow_copy;
    return MANAGER_OK;
}

ManagerResult ownership_table_remove_shadow(FileRecord *file,
                                            int shadow_copy_id)
{
    size_t i;

    if (!valid_file(file) || shadow_copy_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (i = 0; i < file->shadow_copy_count; ++i) {
        if (file->shadow_copies[i].shadow_copy_id == shadow_copy_id) {
            file->shadow_copies[i] =
                file->shadow_copies[file->shadow_copy_count - 1];
            file->shadow_copy_count--;
            return MANAGER_OK;
        }
    }

    return MANAGER_NOT_FOUND;
}

ShadowCopy *ownership_table_find_shadow(FileRecord *file,
                                        int shadow_copy_id)
{
    size_t i;

    if (!valid_file(file) || shadow_copy_id <= 0) {
        return NULL;
    }

    for (i = 0; i < file->shadow_copy_count; ++i) {
        if (file->shadow_copies[i].shadow_copy_id == shadow_copy_id) {
            return &file->shadow_copies[i];
        }
    }

    return NULL;
}

ManagerResult ownership_table_add_waiting(FileRecord *file,
                                          const Application *application,
                                          AccessMode requested_mode,
                                          int request_id,
                                          int transfer_requested)
{
    WaitingRequest *request;

    if (!valid_file(file) || application == NULL ||
        requested_mode != ACCESS_READ_WRITE || request_id <= 0 ||
        (transfer_requested != 0 && transfer_requested != 1)) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (ownership_table_find_waiting(file, application->application_id) != NULL) {
        return MANAGER_ALREADY_EXISTS;
    }

    {
        AccessRecord *access = ownership_table_find_active_access(
            file,
            application->application_id);
        if (access != NULL &&
            (access->access_mode != ACCESS_READ ||
             access->request_state != REQUEST_ACTIVE)) {
            return MANAGER_ALREADY_EXISTS;
        }
    }

    if (file->waiting_request_count >= MAX_WAITING_REQUESTS) {
        return MANAGER_CAPACITY_REACHED;
    }

    request = &file->waiting_requests[file->waiting_request_count++];
    memset(request, 0, sizeof(*request));
    request->request_id = request_id;
    request->application = *application;
    request->requested_mode = requested_mode;
    request->request_state = REQUEST_WAITING;
    request->transfer_requested = transfer_requested;

    return MANAGER_OK;
}

ManagerResult ownership_table_remove_waiting(FileRecord *file,
                                             int application_id)
{
    size_t i;

    if (!valid_file(file) || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (i = 0; i < file->waiting_request_count; ++i) {
        if (file->waiting_requests[i].application.application_id ==
            application_id) {
            file->waiting_requests[i] =
                file->waiting_requests[file->waiting_request_count - 1];
            file->waiting_request_count--;
            return MANAGER_OK;
        }
    }

    return MANAGER_NOT_FOUND;
}
