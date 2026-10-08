#ifndef MODELS_H
#define MODELS_H

#include <stddef.h>

#define MAX_PATH_LENGTH 260
#define MAX_APPLICATION_NAME 64
#define MAX_FILES 32
#define MAX_ACCESS_RECORDS 64
#define MAX_WAITING_REQUESTS 64
#define MAX_SHADOW_COPIES 64
#define MAX_CHANGE_RECORDS 64
#define MAX_PENDING_CLEANUPS 64

typedef enum {
    ACCESS_READ = 1,
    ACCESS_READ_WRITE = 2
} AccessMode;

typedef enum {
    FILE_STATE_ORIGINAL = 1,
    FILE_STATE_TEMPORARY_SHADOW = 2,
    FILE_STATE_INDEPENDENT_FILE = 3
} FileState;

typedef enum {
    REQUEST_ACTIVE = 1,
    REQUEST_WAITING = 2,
    REQUEST_RELEASED = 3,
    REQUEST_FAILED = 4
} RequestState;

typedef enum {
    MANAGER_OK = 0,
    MANAGER_INVALID_ARGUMENT,
    MANAGER_NOT_FOUND,
    MANAGER_ALREADY_EXISTS,
    MANAGER_CAPACITY_REACHED,
    MANAGER_WRITE_CONFLICT,
    MANAGER_INVALID_STATE,
    MANAGER_SHADOW_REQUIRED,
    MANAGER_FILE_COPY_FAILED,
    MANAGER_METADATA_FAILED,
    MANAGER_TRANSFER_PENDING
} ManagerResult;

typedef struct AccessRecord AccessRecord;
typedef struct WaitingRequest WaitingRequest;
typedef struct ShadowCopy ShadowCopy;
typedef struct PendingCleanup PendingCleanup;
typedef struct FileRecord FileRecord;
typedef struct OwnershipTable OwnershipTable;

typedef struct {
    unsigned long long size;
    unsigned long long content_hash;
} ShadowMetadata;

typedef struct {
    int application_id;
    unsigned long process_id;
    char name[MAX_APPLICATION_NAME];
} Application;

typedef struct {
    AccessMode access_mode;
    FileState file_state;
    RequestState request_state;
    char assigned_path[MAX_PATH_LENGTH];
} AccessSnapshot;

typedef struct {
    int request_id;
    Application application;
    AccessMode requested_mode;
    RequestState request_state;
    int transfer_requested;
} WaitingRequestSnapshot;

typedef struct {
    int shadow_copy_id;
    char shadow_path[MAX_PATH_LENGTH];
    Application owner;
    unsigned long creation_version;
    int is_temporary;
    FileState file_state;
    AccessMode access_mode;
} ShadowCopySnapshot;

typedef struct {
    unsigned long current_version;
    int write_owner_application_id;
    int modification_active;
    size_t access_record_count;
    size_t waiting_request_count;
    size_t shadow_copy_count;
} FileSnapshot;

#endif
