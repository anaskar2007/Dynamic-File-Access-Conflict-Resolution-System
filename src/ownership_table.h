#ifndef OWNERSHIP_TABLE_H
#define OWNERSHIP_TABLE_H

#include "models.h"

OwnershipTable *ownership_table_create(void);
void ownership_table_destroy(OwnershipTable *table);
void ownership_table_init(OwnershipTable *table);

ManagerResult ownership_table_get_file_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    FileSnapshot *snapshot);

ManagerResult ownership_table_get_access_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    int application_id,
    AccessSnapshot *snapshot);

ManagerResult ownership_table_get_waiting_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    size_t index,
    WaitingRequestSnapshot *snapshot);

ManagerResult ownership_table_get_shadow_snapshot(
    const OwnershipTable *table,
    const char *original_path,
    size_t index,
    ShadowCopySnapshot *snapshot);

ManagerResult ownership_table_add_file(OwnershipTable *table,
                                       const char *original_path,
                                       FileRecord **out_file);

FileRecord *ownership_table_find_file(OwnershipTable *table,
                                      const char *original_path);

const FileRecord *ownership_table_find_file_const(const OwnershipTable *table,
                                                  const char *original_path);

AccessRecord *ownership_table_find_access(FileRecord *file,
                                          int application_id);

const AccessRecord *ownership_table_find_access_const(const FileRecord *file,
                                                      int application_id);

AccessRecord *ownership_table_find_active_access(FileRecord *file,
                                                 int application_id);

const AccessRecord *ownership_table_find_active_access_const(
    const FileRecord *file,
    int application_id);

WaitingRequest *ownership_table_find_waiting(FileRecord *file,
                                             int application_id);

ManagerResult ownership_table_add_access(FileRecord *file,
                                         const Application *application,
                                         AccessMode access_mode,
                                         int access_record_id);

ManagerResult ownership_table_add_access_with_state(
    FileRecord *file,
    const Application *application,
    AccessMode access_mode,
    FileState file_state,
    const char *assigned_path,
    int shadow_copy_id,
    int access_record_id);

ManagerResult ownership_table_remove_access(FileRecord *file,
                                            int application_id);

ManagerResult ownership_table_add_shadow(FileRecord *file,
                                         const ShadowCopy *shadow_copy);

ManagerResult ownership_table_remove_shadow(FileRecord *file,
                                            int shadow_copy_id);

ShadowCopy *ownership_table_find_shadow(FileRecord *file,
                                        int shadow_copy_id);

ManagerResult ownership_table_add_waiting(FileRecord *file,
                                          const Application *application,
                                          AccessMode requested_mode,
                                          int request_id,
                                          int transfer_requested);

ManagerResult ownership_table_remove_waiting(FileRecord *file,
                                             int application_id);

#endif
