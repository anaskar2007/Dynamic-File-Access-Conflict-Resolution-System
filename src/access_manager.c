#include "access_manager.h"

#include "ownership_table.h"
#include "ownership_table_internal.h"
#include "shadow_copy.h"

#include <stdio.h>
#include <string.h>

/*
 * Looks up or creates a FileRecord for original_path.
 *
 * Returns MANAGER_OK with *out_file set on success.
 * Returns MANAGER_NOT_FOUND if the file does not exist on disk.
 * Returns MANAGER_CAPACITY_REACHED if the ownership table is full.
 *
 * Separating these two failure codes prevents a missing file from being
 * misreported as "manager capacity reached".
 */
static ManagerResult get_or_create_file(OwnershipTable *table,
                                        const char *original_path,
                                        FileRecord **out_file)
{
    FileRecord *file;
    FILE *original;
    ManagerResult result;

    file = ownership_table_find_file(table, original_path);
    if (file != NULL) {
        *out_file = file;
        return MANAGER_OK;
    }

    original = fopen(original_path, "rb");
    if (original == NULL) {
        return MANAGER_NOT_FOUND;
    }
    (void)fclose(original);

    result = ownership_table_add_file(table, original_path, &file);
    if (result != MANAGER_OK) {
        return result; /* propagates MANAGER_CAPACITY_REACHED or MANAGER_INVALID_ARGUMENT */
    }

    if (shadow_copy_read_metadata(original_path,
                                  &file->original_metadata) == MANAGER_OK) {
        file->original_metadata_valid = 1;
    }

    *out_file = file;
    return MANAGER_OK;
}

void access_manager_init(OwnershipTable *table)
{
    ownership_table_init(table);
}

ManagerResult access_manager_request_read(OwnershipTable *table,
                                          const char *original_path,
                                          const Application *application,
                                          int accept_shadow_copy)
{
    FileRecord *file;
    ManagerResult result;
    ShadowCopy shadow;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    result = get_or_create_file(table, original_path, &file);
    if (result != MANAGER_OK) {
        return result;
    }

    if (file->write_owner_application_id != -1 &&
        !accept_shadow_copy) {
        return MANAGER_SHADOW_REQUIRED;
    }

    if (accept_shadow_copy) {
        result = shadow_copy_create(original_path,
                                    application,
                                    file->current_version,
                                    table->next_shadow_copy_id,
                                    &shadow);
        if (result != MANAGER_OK) {
            return result;
        }

        result = ownership_table_add_shadow(file, &shadow);
        if (result != MANAGER_OK) {
            (void)shadow_copy_delete(&shadow);
            return result;
        }

        result = ownership_table_add_access_with_state(
            file,
            application,
            ACCESS_READ,
            FILE_STATE_TEMPORARY_SHADOW,
            shadow.shadow_path,
            shadow.shadow_copy_id,
            table->next_access_record_id);
        if (result != MANAGER_OK) {
            (void)ownership_table_remove_shadow(file, shadow.shadow_copy_id);
            (void)shadow_copy_delete(&shadow);
            return result;
        }

        table->next_shadow_copy_id++;
        table->next_access_record_id++;
        return MANAGER_OK;
    }

    result = ownership_table_add_access(file,
                                        application,
                                        ACCESS_READ,
                                        table->next_access_record_id++);
    return result;
}

ManagerResult access_manager_request_read_write(OwnershipTable *table,
                                               const char *original_path,
                                               const Application *application)
{
    FileRecord *file;
    ManagerResult result;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    result = get_or_create_file(table, original_path, &file);
    if (result != MANAGER_OK) {
        return result;
    }

    if (file->write_owner_application_id != -1) {
        return MANAGER_WRITE_CONFLICT;
    }

    result = ownership_table_add_access(file,
                                        application,
                                        ACCESS_READ_WRITE,
                                        table->next_access_record_id++);
    if (result != MANAGER_OK) {
        return result;
    }

    file->write_owner_application_id = application->application_id;
    return MANAGER_OK;
}

ManagerResult access_manager_request_read_write_shadow(
    OwnershipTable *table,
    const char *original_path,
    const Application *application)
{
    FileRecord *file;
    AccessRecord *existing;
    ShadowCopy shadow;
    ManagerResult result;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    result = get_or_create_file(table, original_path, &file);
    if (result != MANAGER_OK) {
        return result;
    }
    if (file->write_owner_application_id == -1) {
        return MANAGER_INVALID_STATE;
    }

    existing = ownership_table_find_active_access(
        file,
        application->application_id);
    if (existing != NULL && existing->access_mode != ACCESS_READ) {
        return MANAGER_ALREADY_EXISTS;
    }
    if (existing != NULL &&
        existing->file_state == FILE_STATE_TEMPORARY_SHADOW) {
        ShadowCopy *existing_shadow = ownership_table_find_shadow(
            file,
            existing->shadow_copy_id);
        if (existing_shadow == NULL) {
            return MANAGER_INVALID_STATE;
        }
        existing->access_mode = ACCESS_READ_WRITE;
        existing_shadow->access_mode = ACCESS_READ_WRITE;
        return MANAGER_OK;
    }
    if (existing != NULL && existing->file_state != FILE_STATE_ORIGINAL) {
        return MANAGER_INVALID_STATE;
    }

    result = shadow_copy_create(original_path,
                                application,
                                file->current_version,
                                table->next_shadow_copy_id,
                                &shadow);
    if (result != MANAGER_OK) {
        return result;
    }
    shadow.access_mode = ACCESS_READ_WRITE;

    result = ownership_table_add_shadow(file, &shadow);
    if (result != MANAGER_OK) {
        (void)shadow_copy_delete(&shadow);
        return result;
    }

    if (existing != NULL) {
        existing->access_mode = ACCESS_READ_WRITE;
        existing->file_state = FILE_STATE_TEMPORARY_SHADOW;
        (void)snprintf(existing->assigned_path,
                       sizeof(existing->assigned_path),
                       "%s",
                       shadow.shadow_path);
        existing->shadow_copy_id = shadow.shadow_copy_id;
    } else {
        result = ownership_table_add_access_with_state(
            file,
            application,
            ACCESS_READ_WRITE,
            FILE_STATE_TEMPORARY_SHADOW,
            shadow.shadow_path,
            shadow.shadow_copy_id,
            table->next_access_record_id);
        if (result != MANAGER_OK) {
            (void)ownership_table_remove_shadow(file, shadow.shadow_copy_id);
            (void)shadow_copy_delete(&shadow);
            return result;
        }
        table->next_access_record_id++;
    }

    table->next_shadow_copy_id++;
    return MANAGER_OK;
}

ManagerResult access_manager_upgrade_read_write(
    OwnershipTable *table,
    const char *original_path,
    const Application *application)
{
    FileRecord *file;
    AccessRecord *record;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    record = ownership_table_find_active_access(file,
                                                application->application_id);
    if (record == NULL || record->access_mode != ACCESS_READ ||
        record->request_state != REQUEST_ACTIVE) {
        return MANAGER_INVALID_STATE;
    }

    if (record->file_state == FILE_STATE_TEMPORARY_SHADOW) {
        ShadowCopy *shadow = ownership_table_find_shadow(
            file,
            record->shadow_copy_id);

        if (shadow == NULL) {
            return MANAGER_INVALID_STATE;
        }

        record->access_mode = ACCESS_READ_WRITE;
        shadow->access_mode = ACCESS_READ_WRITE;
        return MANAGER_OK;
    }

    if (record->file_state != FILE_STATE_ORIGINAL) {
        return MANAGER_INVALID_STATE;
    }

    if (file->write_owner_application_id != -1) {
        return MANAGER_WRITE_CONFLICT;
    }

    record->access_mode = ACCESS_READ_WRITE;
    file->write_owner_application_id = application->application_id;
    return MANAGER_OK;
}

ManagerResult access_manager_authorize_modification(
    const OwnershipTable *table,
    const Application *application,
    const char *assigned_path)
{
    size_t file_index;

    if (table == NULL || application == NULL || assigned_path == NULL ||
        application->application_id <= 0 || assigned_path[0] == '\0') {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (file_index = 0; file_index < table->file_count; ++file_index) {
        const FileRecord *file = &table->files[file_index];
        const AccessRecord *record = ownership_table_find_active_access_const(
            file,
            application->application_id);

        if (record == NULL ||
            record->request_state != REQUEST_ACTIVE ||
            strcmp(record->assigned_path, assigned_path) != 0) {
            continue;
        }

        if (record->file_state == FILE_STATE_ORIGINAL) {
            if (record->access_mode == ACCESS_READ_WRITE &&
                file->write_owner_application_id ==
                    application->application_id) {
                return MANAGER_OK;
            }
            return MANAGER_INVALID_STATE;
        }

        if (record->file_state == FILE_STATE_TEMPORARY_SHADOW) {
            return record->access_mode == ACCESS_READ_WRITE
                ? MANAGER_OK
                : MANAGER_INVALID_STATE;
        }

        if (record->file_state == FILE_STATE_INDEPENDENT_FILE) {
            return MANAGER_OK;
        }
    }

    return MANAGER_INVALID_STATE;
}

static FileRecord *find_file_for_assigned_path(
    OwnershipTable *table,
    const Application *application,
    const char *assigned_path)
{
    size_t file_index;

    for (file_index = 0; file_index < table->file_count; ++file_index) {
        FileRecord *file = &table->files[file_index];
        AccessRecord *record = ownership_table_find_active_access(
            file,
            application->application_id);

        if (record != NULL && strcmp(record->assigned_path, assigned_path) == 0) {
            return file;
        }
    }
    return NULL;
}

ManagerResult access_manager_begin_modification(
    OwnershipTable *table,
    const Application *application,
    const char *assigned_path)
{
    FileRecord *file;
    AccessRecord *record;
    ManagerResult result;

    if (table == NULL || application == NULL || assigned_path == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    result = access_manager_authorize_modification(
        table, application, assigned_path);
    if (result != MANAGER_OK) {
        return result;
    }

    file = find_file_for_assigned_path(table, application, assigned_path);
    if (file == NULL) {
        return MANAGER_INVALID_STATE;
    }

    record = ownership_table_find_active_access(
        file, application->application_id);
    if (record != NULL && record->file_state == FILE_STATE_ORIGINAL) {
        file->modification_active = 1;
    }
    return MANAGER_OK;
}

ManagerResult access_manager_end_modification(
    OwnershipTable *table,
    const Application *application,
    const char *assigned_path)
{
    FileRecord *file;

    if (table == NULL || application == NULL || assigned_path == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = find_file_for_assigned_path(table, application, assigned_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    if (strcmp(file->original_path, assigned_path) == 0) {
        file->modification_active = 0;
    }
    return MANAGER_OK;
}

ManagerResult access_manager_wait_read_write(OwnershipTable *table,
                                            const char *original_path,
                                            const Application *application)
{
    FileRecord *file;
    ManagerResult result;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    result = get_or_create_file(table, original_path, &file);
    if (result != MANAGER_OK) {
        return result;
    }

    if (file->write_owner_application_id == -1) {
        return MANAGER_INVALID_STATE;
    }

    {
        AccessRecord *access = ownership_table_find_active_access(
            file,
            application->application_id);
        if (access != NULL && access->access_mode != ACCESS_READ) {
            return MANAGER_INVALID_STATE;
        }
    }

    return ownership_table_add_waiting(file,
                                       application,
                                       ACCESS_READ_WRITE,
                                       table->next_request_id++,
                                       0);
}

ManagerResult access_manager_request_transfer(OwnershipTable *table,
                                              const char *original_path,
                                              const Application *application)
{
    FileRecord *file;
    ManagerResult result;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    result = get_or_create_file(table, original_path, &file);
    if (result != MANAGER_OK) {
        return result;
    }

    if (file->write_owner_application_id == -1) {
        return MANAGER_INVALID_STATE;
    }

    {
        AccessRecord *access = ownership_table_find_active_access(
            file,
            application->application_id);
        if ((access != NULL && access->access_mode != ACCESS_READ) ||
            ownership_table_find_waiting(file, application->application_id) != NULL) {
            return MANAGER_ALREADY_EXISTS;
        }
    }

    result = ownership_table_add_waiting(
        file,
        application,
        ACCESS_READ_WRITE,
        table->next_request_id++,
        1);

    if (result != MANAGER_OK) {
        return result;
    }

    return MANAGER_TRANSFER_PENDING;
}

ManagerResult access_manager_prepare_transfer(OwnershipTable *table,
                                              const char *original_path,
                                              int current_writer_id)
{
    return access_manager_prepare_transfer_if_saved(
        table,
        original_path,
        current_writer_id,
        1);
}

ManagerResult access_manager_prepare_transfer_if_saved(
    OwnershipTable *table,
    const char *original_path,
    int current_writer_id,
    int saved)
{
    FileRecord *file;
    AccessRecord *writer;

    if (table == NULL || original_path == NULL || current_writer_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }
    if (!saved) {
        return MANAGER_INVALID_STATE;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    if (file->write_owner_application_id != current_writer_id ||
        !access_manager_has_waiting_requests(table, original_path)) {
        return MANAGER_INVALID_STATE;
    }

    writer = ownership_table_find_active_access(file, current_writer_id);
    if (writer == NULL || writer->access_mode != ACCESS_READ_WRITE) {
        return MANAGER_INVALID_STATE;
    }

    writer->access_mode = ACCESS_READ;
    file->modification_active = 0;
    file->write_owner_application_id = -1;
    return MANAGER_OK;
}

ManagerResult access_manager_assign_waiting_writer(OwnershipTable *table,
                                                   const char *original_path,
                                                   int application_id)
{
    FileRecord *file;
    WaitingRequest *request;
    ManagerResult result;

    if (table == NULL || original_path == NULL || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    if (file->write_owner_application_id != -1) {
        return MANAGER_WRITE_CONFLICT;
    }

    request = ownership_table_find_waiting(file, application_id);
    if (request == NULL) {
        return MANAGER_NOT_FOUND;
    }

    {
        AccessRecord *existing = ownership_table_find_active_access(
            file,
            application_id);
        if (existing != NULL) {
            if (existing->access_mode != ACCESS_READ ||
                existing->request_state != REQUEST_ACTIVE) {
                return MANAGER_INVALID_STATE;
            }
            existing->access_mode = ACCESS_READ_WRITE;
        } else {
            result = ownership_table_add_access(
                file,
                &request->application,
                ACCESS_READ_WRITE,
                table->next_access_record_id++);
            if (result != MANAGER_OK) {
                return result;
            }
        }
    }

    file->write_owner_application_id = application_id;
    return ownership_table_remove_waiting(file, application_id);
}

int access_manager_has_waiting_requests(const OwnershipTable *table,
                                        const char *original_path)
{
    const FileRecord *file;

    if (table == NULL || original_path == NULL) {
        return 0;
    }

    file = ownership_table_find_file_const(table, original_path);
    return file != NULL && file->waiting_request_count > 0;
}

ManagerResult access_manager_release_read(OwnershipTable *table,
                                          const char *original_path,
                                          int application_id)
{
    FileRecord *file;
    AccessRecord *record;
    ManagerResult result;
    ManagerResult cleanup_result = MANAGER_OK;

    if (table == NULL || original_path == NULL || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    record = ownership_table_find_active_access(file, application_id);
    if (record == NULL || record->access_mode != ACCESS_READ) {
        return MANAGER_INVALID_STATE;
    }

    if (record->file_state == FILE_STATE_TEMPORARY_SHADOW) {
        ShadowCopy *shadow = NULL;
        int shadow_id = record->shadow_copy_id;
        size_t i;

        for (i = 0; i < file->shadow_copy_count; ++i) {
            if (file->shadow_copies[i].shadow_copy_id ==
                record->shadow_copy_id) {
                shadow = &file->shadow_copies[i];
                break;
            }
        }

        if (shadow != NULL) {
            cleanup_result = shadow_copy_delete(shadow);
            result = ownership_table_remove_shadow(file, shadow_id);
            if (result != MANAGER_OK && cleanup_result == MANAGER_OK) {
                cleanup_result = result;
            }
        }
    }

    result = ownership_table_remove_access(file, application_id);
    if (result != MANAGER_OK) {
        return result;
    }
    return cleanup_result;
}

ManagerResult access_manager_release_read_write(OwnershipTable *table,
                                               const char *original_path,
                                               int application_id)
{
    FileRecord *file;
    AccessRecord *record;

    if (table == NULL || original_path == NULL || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    record = ownership_table_find_active_access(file,     application_id);
    if (record == NULL || record->access_mode != ACCESS_READ_WRITE) {
        return MANAGER_INVALID_STATE;
    }

    if (record->file_state != FILE_STATE_ORIGINAL) {
        ManagerResult cleanup_result = MANAGER_OK;

        if (record->file_state == FILE_STATE_TEMPORARY_SHADOW) {
            ShadowCopy *shadow = ownership_table_find_shadow(
                file,
                record->shadow_copy_id);

            if (shadow == NULL) {
                return MANAGER_INVALID_STATE;
            }

            cleanup_result = shadow_copy_delete(shadow);
            {
                ManagerResult remove_result = ownership_table_remove_shadow(
                    file, record->shadow_copy_id);
                if (cleanup_result == MANAGER_OK) {
                    cleanup_result = remove_result;
                }
            }
        }

        if (ownership_table_remove_access(file, application_id) !=
            MANAGER_OK) {
            return MANAGER_INVALID_STATE;
        }
        return cleanup_result;
    }

    if (file->write_owner_application_id != application_id) {
        return MANAGER_INVALID_STATE;
    }

    file->modification_active = 0;
    record->request_state = REQUEST_RELEASED;
    file->write_owner_application_id = -1;
    return ownership_table_remove_access(file, application_id);
}

ManagerResult access_manager_handle_writer_failure(
    OwnershipTable *table,
    const char *original_path,
    int application_id)
{
    FileRecord *file;
    AccessRecord *record;

    if (table == NULL || original_path == NULL || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    if (file->write_owner_application_id != application_id) {
        return MANAGER_INVALID_STATE;
    }

    file->modification_active = 0;
    record = ownership_table_find_active_access(file, application_id);
    if (record == NULL || record->access_mode != ACCESS_READ_WRITE ||
        record->request_state != REQUEST_ACTIVE) {
        return MANAGER_INVALID_STATE;
    }

    record->access_mode = ACCESS_READ;
    record->request_state = REQUEST_FAILED;
    file->write_owner_application_id = -1;
    return MANAGER_OK;
}

ManagerResult access_manager_handle_application_failure(
    OwnershipTable *table,
    const char *original_path,
    int application_id)
{
    FileRecord *file;
    AccessRecord *record;

    if (table == NULL || original_path == NULL || application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    file->modification_active = 0;

    if (file->write_owner_application_id == application_id) {
        return access_manager_handle_writer_failure(
            table,
            original_path,
            application_id);
    }

    record = ownership_table_find_active_access(file, application_id);
    if (record == NULL) {
        return MANAGER_NOT_FOUND;
    }

    if (record->file_state == FILE_STATE_TEMPORARY_SHADOW) {
        ShadowCopy *shadow = ownership_table_find_shadow(
            file,
            record->shadow_copy_id);

        if (shadow == NULL) {
            return MANAGER_INVALID_STATE;
        }
        ManagerResult cleanup_result = shadow_copy_delete(shadow);
        ManagerResult remove_result = ownership_table_remove_shadow(
            file, record->shadow_copy_id);
        if (remove_result != MANAGER_OK) {
            return MANAGER_INVALID_STATE;
        }
        if (cleanup_result != MANAGER_OK) {
            record->request_state = REQUEST_FAILED;
            return ownership_table_remove_access(file, application_id);
        }
    }

    record->request_state = REQUEST_FAILED;
    return ownership_table_remove_access(file, application_id);
}

ManagerResult access_manager_detect_shadow_modifications(
    OwnershipTable *table,
    size_t *modified_count)
{
    size_t file_index;
    size_t shadow_index;
    size_t count = 0;
    ManagerResult first_error = MANAGER_OK;

    if (table == NULL || modified_count == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (file_index = 0; file_index < table->file_count; ++file_index) {
        FileRecord *file = &table->files[file_index];

        for (shadow_index = 0;
             shadow_index < file->shadow_copy_count;
             ++shadow_index) {
            ShadowCopy *shadow = &file->shadow_copies[shadow_index];
            ShadowMetadata observed_metadata;
            AccessRecord *access;
            ManagerResult result;

            if (!shadow->is_temporary ||
                shadow->file_state != FILE_STATE_TEMPORARY_SHADOW) {
                continue;
            }

            result = shadow_copy_read_metadata(shadow->shadow_path,
                                               &observed_metadata);
            if (result != MANAGER_OK) {
                if (first_error == MANAGER_OK) {
                    first_error = result;
                }
                continue;
            }

            shadow->current_metadata = observed_metadata;
            if (shadow_copy_metadata_equal(&shadow->creation_metadata,
                                           &observed_metadata)) {
                continue;
            }

            if (file->change_record_count >= MAX_CHANGE_RECORDS) {
                if (first_error == MANAGER_OK) {
                    first_error = MANAGER_CAPACITY_REACHED;
                }
                continue;
            }
            {
                ChangeRecord *change =
                    &file->change_records[file->change_record_count];
                char diff_path[MAX_PATH_LENGTH];
                char original_path[MAX_PATH_LENGTH];
                char shadow_path[MAX_PATH_LENGTH];

                (void)snprintf(original_path,
                               sizeof(original_path),
                               "%s",
                               shadow->original_path);
                (void)snprintf(shadow_path,
                               sizeof(shadow_path),
                               "%s",
                               shadow->shadow_path);

                result = shadow_copy_create_diff_copy(
                    shadow_path,
                    table->next_change_record_id,
                    diff_path,
                    sizeof(diff_path));
                if (result != MANAGER_OK) {
                    if (first_error == MANAGER_OK) {
                        first_error = result;
                    }
                    continue;
                }

                shadow->file_state = FILE_STATE_INDEPENDENT_FILE;
                shadow->is_temporary = 0;
                memset(change, 0, sizeof(*change));
                change->change_record_id = table->next_change_record_id++;
                (void)snprintf(change->original_path,
                               sizeof(change->original_path),
                               "%s",
                               original_path);
                (void)snprintf(change->shadow_path,
                               sizeof(change->shadow_path),
                               "%s",
                               shadow_path);
                (void)snprintf(change->diff_path,
                               sizeof(change->diff_path),
                               "%s",
                               diff_path);
                change->application = shadow->owner;
                change->original_version = shadow->creation_version;
                change->original_metadata = shadow->creation_metadata;
                change->shadow_metadata = observed_metadata;
                file->change_record_count++;
            }
            access = ownership_table_find_active_access(
                file,
                shadow->owner.application_id);
            if (access != NULL &&
                access->shadow_copy_id == shadow->shadow_copy_id) {
                access->file_state = FILE_STATE_INDEPENDENT_FILE;
            }
            count++;
        }
    }

    *modified_count = count;
    return first_error;
}

ManagerResult access_manager_detect_original_update(
    OwnershipTable *table,
    const char *original_path,
    size_t *pending_shadow_count)
{
    FileRecord *file;
    ShadowMetadata observed_metadata;
    size_t i;
    size_t count = 0;
    ManagerResult result;

    if (table == NULL || original_path == NULL ||
        pending_shadow_count == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    result = shadow_copy_read_metadata(original_path, &observed_metadata);
    if (result != MANAGER_OK) {
        return result;
    }

    if (file->original_metadata_valid &&
        shadow_copy_metadata_equal(&file->original_metadata,
                                   &observed_metadata)) {
        *pending_shadow_count = 0;
        return MANAGER_OK;
    }

    file->original_metadata = observed_metadata;
    file->original_metadata_valid = 1;
    file->current_version++;

    for (i = 0; i < file->shadow_copy_count; ++i) {
        ShadowCopy *shadow = &file->shadow_copies[i];

        if (shadow->is_temporary &&
            shadow->file_state == FILE_STATE_TEMPORARY_SHADOW &&
            shadow->creation_version < file->current_version) {
            count++;
        }
    }

    *pending_shadow_count = count;
    return MANAGER_OK;
}

ManagerResult access_manager_update_shadow(OwnershipTable *table,
                                           const char *original_path,
                                           int shadow_copy_id)
{
    FileRecord *file;
    ShadowCopy *old_shadow;
    ShadowCopy replacement;
    AccessRecord *access;
    ManagerResult result;
    char old_shadow_path[MAX_PATH_LENGTH];
    int old_shadow_id;

    if (table == NULL || original_path == NULL || shadow_copy_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    old_shadow = ownership_table_find_shadow(file, shadow_copy_id);
    if (old_shadow == NULL ||
        !old_shadow->is_temporary ||
        old_shadow->file_state != FILE_STATE_TEMPORARY_SHADOW) {
        return MANAGER_INVALID_STATE;
    }

    {
        ShadowMetadata current_metadata;

        result = shadow_copy_read_metadata(old_shadow->shadow_path,
                                           &current_metadata);
        if (result != MANAGER_OK) {
            return result;
        }
        if (!shadow_copy_metadata_equal(&old_shadow->creation_metadata,
                                        &current_metadata)) {
            return MANAGER_INVALID_STATE;
        }
    }

    result = shadow_copy_create(original_path,
                                &old_shadow->owner,
                                file->current_version,
                                table->next_shadow_copy_id,
                                &replacement);
    if (result != MANAGER_OK) {
        return result;
    }
    replacement.access_mode = old_shadow->access_mode;

    (void)snprintf(old_shadow_path,
                   sizeof(old_shadow_path),
                   "%s",
                   old_shadow->shadow_path);
    old_shadow_id = old_shadow->shadow_copy_id;

    if (shadow_copy_delete(old_shadow) != MANAGER_OK) {
        (void)shadow_copy_delete(&replacement);
        return MANAGER_FILE_COPY_FAILED;
    }

    *old_shadow = replacement;
    table->next_shadow_copy_id++;

    access = ownership_table_find_active_access(
        file,
        replacement.owner.application_id);
    if (access != NULL && access->shadow_copy_id == old_shadow_id) {
        access->shadow_copy_id = replacement.shadow_copy_id;
        (void)snprintf(access->assigned_path,
                       sizeof(access->assigned_path),
                       "%s",
                       replacement.shadow_path);
        access->file_state = FILE_STATE_TEMPORARY_SHADOW;
    }

    return MANAGER_OK;
}

ManagerResult access_manager_cleanup_temporary_shadows(OwnershipTable *table)
{
    size_t file_index;
    ManagerResult first_error = MANAGER_OK;

    if (table == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (file_index = 0; file_index < table->file_count; ++file_index) {
        FileRecord *file = &table->files[file_index];
        size_t shadow_index = 0;

        while (shadow_index < file->shadow_copy_count) {
            ShadowCopy *shadow = &file->shadow_copies[shadow_index];
            int shadow_id;
            int shadow_owner_id;

            if (!shadow->is_temporary ||
                shadow->file_state != FILE_STATE_TEMPORARY_SHADOW) {
                shadow_index++;
                continue;
            }
            shadow_id = shadow->shadow_copy_id;
            shadow_owner_id = shadow->owner.application_id;

            if (shadow_copy_delete(shadow) != MANAGER_OK) {
                if (file->pending_cleanup_count < MAX_PENDING_CLEANUPS) {
                    PendingCleanup *pending =
                        &file->pending_cleanups[file->pending_cleanup_count++];
                    char pending_shadow_path[MAX_PATH_LENGTH];
                    char pending_original_path[MAX_PATH_LENGTH];

                    (void)snprintf(pending_shadow_path,
                                   sizeof(pending_shadow_path),
                                   "%s",
                                   shadow->shadow_path);
                    (void)snprintf(pending_original_path,
                                   sizeof(pending_original_path),
                                   "%s",
                                   shadow->original_path);
                    memset(pending, 0, sizeof(*pending));
                    (void)snprintf(pending->shadow_path,
                                   sizeof(pending->shadow_path),
                                   "%s",
                                   pending_shadow_path);
                    (void)snprintf(pending->original_path,
                                   sizeof(pending->original_path),
                                   "%s",
                                   pending_original_path);
                    pending->shadow_copy_id = shadow->shadow_copy_id;
                    pending->application_id = shadow->owner.application_id;
                    pending->request_state = REQUEST_WAITING;
                }
                (void)ownership_table_remove_shadow(file, shadow->shadow_copy_id);
                (void)ownership_table_remove_access(file, shadow->owner.application_id);
                first_error = MANAGER_FILE_COPY_FAILED;
                shadow_index++;
                continue;
            }

            if (ownership_table_remove_shadow(file,
                                              shadow_id) !=
                MANAGER_OK) {
                first_error = MANAGER_INVALID_STATE;
                shadow_index++;
                continue;
            }

            {
                AccessRecord *access = ownership_table_find_active_access(
                    file,
                    shadow_owner_id);
                if (access != NULL &&
                    access->shadow_copy_id == shadow_id) {
                    if (ownership_table_remove_access(
                            file,
                            shadow_owner_id) != MANAGER_OK) {
                        first_error = MANAGER_INVALID_STATE;
                    }
                }
            }
        }
    }

    return first_error;
}

ManagerResult access_manager_retry_pending_cleanups(OwnershipTable *table)
{
    size_t file_index;
    ManagerResult first_error = MANAGER_OK;

    if (table == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    for (file_index = 0; file_index < table->file_count; ++file_index) {
        FileRecord *file = &table->files[file_index];
        size_t index = 0;

        while (index < file->pending_cleanup_count) {
            PendingCleanup *pending = &file->pending_cleanups[index];

            if (shadow_copy_delete_path(pending->shadow_path) != MANAGER_OK) {
                first_error = MANAGER_FILE_COPY_FAILED;
                index++;
                continue;
            }

            file->pending_cleanups[index] =
                file->pending_cleanups[file->pending_cleanup_count - 1];
            file->pending_cleanup_count--;
        }
    }

    return first_error;
}

ManagerResult access_manager_save_shadow(OwnershipTable *table,
                                          const char *original_path,
                                          const Application *application)
{
    FileRecord *file;
    AccessRecord *record;
    ShadowCopy *shadow;
    ShadowMetadata observed_metadata;
    ManagerResult result;
    ChangeRecord *change;

    if (table == NULL || original_path == NULL || application == NULL ||
        application->application_id <= 0) {
        return MANAGER_INVALID_ARGUMENT;
    }

    file = ownership_table_find_file(table, original_path);
    if (file == NULL) {
        return MANAGER_NOT_FOUND;
    }

    record = ownership_table_find_active_access(
        file, application->application_id);
    if (record == NULL || record->request_state != REQUEST_ACTIVE ||
        record->file_state != FILE_STATE_TEMPORARY_SHADOW) {
        return MANAGER_INVALID_STATE;
    }

    shadow = ownership_table_find_shadow(file, record->shadow_copy_id);
    if (shadow == NULL || !shadow->is_temporary ||
        shadow->file_state != FILE_STATE_TEMPORARY_SHADOW) {
        return MANAGER_INVALID_STATE;
    }

    result = shadow_copy_read_metadata(shadow->shadow_path,
                                       &observed_metadata);
    if (result != MANAGER_OK) {
        return result;
    }

    shadow->current_metadata = observed_metadata;
    if (shadow_copy_metadata_equal(&shadow->creation_metadata,
                                   &observed_metadata)) {
        return MANAGER_OK;
    }

    if (file->change_record_count >= MAX_CHANGE_RECORDS) {
        return MANAGER_CAPACITY_REACHED;
    }

    change = &file->change_records[file->change_record_count];
    memset(change, 0, sizeof(*change));
    change->change_record_id = table->next_change_record_id++;
    (void)snprintf(change->original_path,
                   sizeof(change->original_path),
                   "%s",
                   shadow->original_path);
    (void)snprintf(change->shadow_path,
                   sizeof(change->shadow_path),
                   "%s",
                   shadow->shadow_path);
    change->application = shadow->owner;
    change->original_version = shadow->creation_version;
    change->original_metadata = shadow->creation_metadata;
    change->shadow_metadata = observed_metadata;
    file->change_record_count++;

    shadow->file_state = FILE_STATE_INDEPENDENT_FILE;
    shadow->is_temporary = 0;
    record->file_state = FILE_STATE_INDEPENDENT_FILE;
    return MANAGER_OK;
}

static const char *access_mode_string(AccessMode mode)
{
    switch (mode) {
    case ACCESS_READ:
        return "READ";
    case ACCESS_READ_WRITE:
        return "READ + WRITE";
    default:
        return "UNKNOWN";
    }
}

static const char *file_state_string(FileState state)
{
    switch (state) {
    case FILE_STATE_ORIGINAL:
        return "ORIGINAL";
    case FILE_STATE_TEMPORARY_SHADOW:
        return "TEMPORARY_SHADOW";
    case FILE_STATE_INDEPENDENT_FILE:
        return "INDEPENDENT_FILE";
    default:
        return "UNKNOWN";
    }
}

static const char *request_state_string(RequestState state)
{
    switch (state) {
    case REQUEST_ACTIVE:
        return "ACTIVE";
    case REQUEST_WAITING:
        return "WAITING";
    case REQUEST_RELEASED:
        return "RELEASED";
    case REQUEST_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

void access_manager_print_status(const OwnershipTable *table)
{
    size_t i;
    size_t j;

    if (table == NULL) {
        printf("Error: ownership table is unavailable.\n");
        return;
    }

    printf("\n========== ACCESS MANAGER STATUS ==========\n");
    printf("Managed original files: %zu\n", table->file_count);

    for (i = 0; i < table->file_count; ++i) {
        const FileRecord *file = &table->files[i];

        printf("\nFile #%d: %s\n", file->file_id, file->original_path);
        printf("  Original version: %lu\n", file->current_version);
        if (file->write_owner_application_id == -1) {
            printf("  WRITE owner: none\n");
        } else {
            printf("  WRITE owner: application %d\n",
                   file->write_owner_application_id);
        }

        {
            size_t active_count = 0;
            for (j = 0; j < file->access_record_count; ++j) {
                if (file->access_records[j].request_state == REQUEST_ACTIVE) {
                    active_count++;
                }
            }
            printf("  Active access records: %zu\n", active_count);
        }
        for (j = 0; j < file->access_record_count; ++j) {
            const AccessRecord *record = &file->access_records[j];

            printf("    Application %d (%s), PID=%lu: mode=%s, state=%s, "
                   "request=%s\n",
                   record->application.application_id,
                   record->application.name,
                   record->application.process_id,
                   access_mode_string(record->access_mode),
                   file_state_string(record->file_state),
                   request_state_string(record->request_state));
            printf("      Assigned path: %s\n", record->assigned_path);
        }

        printf("  Waiting WRITE requests: %zu\n",
               file->waiting_request_count);
        for (j = 0; j < file->waiting_request_count; ++j) {
            const WaitingRequest *request = &file->waiting_requests[j];

            printf("    Request %d: application %d (%s), type=%s\n",
                   request->request_id,
                   request->application.application_id,
                   request->application.name,
                   request->transfer_requested
                       ? "REQUEST TRANSFER"
                       : "WAIT");
        }

        printf("  Shadow copies: %zu\n", file->shadow_copy_count);
        for (j = 0; j < file->shadow_copy_count; ++j) {
            const ShadowCopy *shadow = &file->shadow_copies[j];

            printf("    Shadow #%d: %s, owner=%d, source version=%lu, "
                   "state=%s, temporary=%s\n",
                   shadow->shadow_copy_id,
                   shadow->shadow_path,
                   shadow->owner.application_id,
                   shadow->creation_version,
                   file_state_string(shadow->file_state),
                   shadow->is_temporary ? "yes" : "no");
        }
    }

    printf("===========================================\n");
}

const char *manager_result_string(ManagerResult result)
{
    switch (result) {
    case MANAGER_OK:
        return "operation completed";
    case MANAGER_INVALID_ARGUMENT:
        return "invalid argument";
    case MANAGER_NOT_FOUND:
        return "record not found";
    case MANAGER_ALREADY_EXISTS:
        return "application already has a record for this file";
    case MANAGER_CAPACITY_REACHED:
        return "manager capacity reached";
    case MANAGER_WRITE_CONFLICT:
        return "READ + WRITE conflict: another application owns WRITE";
    case MANAGER_INVALID_STATE:
        return "invalid access state for this operation";
    case MANAGER_SHADOW_REQUIRED:
        return "a WRITE owner exists; a shadow copy may be offered";
    case MANAGER_FILE_COPY_FAILED:
        return "file copy operation failed";
    case MANAGER_METADATA_FAILED:
        return "unable to read file metadata";
    case MANAGER_TRANSFER_PENDING:
        return "WRITE transfer request is pending current-writer save";
    default:
        return "unknown manager error";
    }
}
