#ifndef OWNERSHIP_TABLE_INTERNAL_H
#define OWNERSHIP_TABLE_INTERNAL_H

#include "models.h"

struct AccessRecord {
    int access_record_id;
    Application application;
    AccessMode access_mode;
    FileState file_state;
    RequestState request_state;
    char original_path[MAX_PATH_LENGTH];
    char assigned_path[MAX_PATH_LENGTH];
    int shadow_copy_id;
};

struct WaitingRequest {
    int request_id;
    Application application;
    AccessMode requested_mode;
    RequestState request_state;
    int transfer_requested;
};

struct ShadowCopy {
    int shadow_copy_id;
    char original_path[MAX_PATH_LENGTH];
    char shadow_path[MAX_PATH_LENGTH];
    Application owner;
    unsigned long creation_version;
    int is_temporary;
    ShadowMetadata creation_metadata;
    ShadowMetadata current_metadata;
    AccessMode access_mode;
    FileState file_state;
    RequestState request_state;
};

struct PendingCleanup {
    char shadow_path[MAX_PATH_LENGTH];
    char original_path[MAX_PATH_LENGTH];
    int shadow_copy_id;
    int application_id;
    RequestState request_state;
};

typedef struct {
    int change_record_id;
    char original_path[MAX_PATH_LENGTH];
    char shadow_path[MAX_PATH_LENGTH];
    char diff_path[MAX_PATH_LENGTH];
    Application application;
    unsigned long original_version;
    ShadowMetadata original_metadata;
    ShadowMetadata shadow_metadata;
} ChangeRecord;

struct FileRecord {
    int file_id;
    char original_path[MAX_PATH_LENGTH];
    unsigned long current_version;
    ShadowMetadata original_metadata;
    int original_metadata_valid;
    int write_owner_application_id;
    int modification_active;
    AccessRecord access_records[MAX_ACCESS_RECORDS];
    size_t access_record_count;
    WaitingRequest waiting_requests[MAX_WAITING_REQUESTS];
    size_t waiting_request_count;
    ShadowCopy shadow_copies[MAX_SHADOW_COPIES];
    size_t shadow_copy_count;
    ChangeRecord change_records[MAX_CHANGE_RECORDS];
    size_t change_record_count;
    PendingCleanup pending_cleanups[MAX_PENDING_CLEANUPS];
    size_t pending_cleanup_count;
};

struct OwnershipTable {
    FileRecord files[MAX_FILES];
    size_t file_count;
    int next_file_id;
    int next_access_record_id;
    int next_request_id;
    int next_shadow_copy_id;
    int next_change_record_id;
};

#endif
