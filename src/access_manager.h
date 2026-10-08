#ifndef ACCESS_MANAGER_H
#define ACCESS_MANAGER_H

#include "models.h"

void access_manager_init(OwnershipTable *table);

ManagerResult access_manager_request_read(OwnershipTable *table,
                                          const char *original_path,
                                          const Application *application,
                                          int accept_shadow_copy);

ManagerResult access_manager_request_read_write(OwnershipTable *table,
                                               const char *original_path,
                                               const Application *application);

ManagerResult access_manager_request_read_write_shadow(
    OwnershipTable *table,
    const char *original_path,
    const Application *application);

ManagerResult access_manager_upgrade_read_write(
    OwnershipTable *table,
    const char *original_path,
    const Application *application);

ManagerResult access_manager_authorize_modification(
    const OwnershipTable *table,
    const Application *application,
    const char *assigned_path);

ManagerResult access_manager_begin_modification(
    OwnershipTable *table,
    const Application *application,
    const char *assigned_path);

ManagerResult access_manager_end_modification(
    OwnershipTable *table,
    const Application *application,
    const char *assigned_path);

ManagerResult access_manager_wait_read_write(OwnershipTable *table,
                                            const char *original_path,
                                            const Application *application);

ManagerResult access_manager_request_transfer(OwnershipTable *table,
                                              const char *original_path,
                                              const Application *application);

ManagerResult access_manager_prepare_transfer(OwnershipTable *table,
                                              const char *original_path,
                                              int current_writer_id);

ManagerResult access_manager_assign_waiting_writer(OwnershipTable *table,
                                                   const char *original_path,
                                                   int application_id);

int access_manager_has_waiting_requests(const OwnershipTable *table,
                                        const char *original_path);

ManagerResult access_manager_release_read(OwnershipTable *table,
                                          const char *original_path,
                                          int application_id);

ManagerResult access_manager_release_read_write(OwnershipTable *table,
                                               const char *original_path,
                                               int application_id);

ManagerResult access_manager_handle_writer_failure(
    OwnershipTable *table,
    const char *original_path,
    int application_id);

ManagerResult access_manager_handle_application_failure(
    OwnershipTable *table,
    const char *original_path,
    int application_id);

ManagerResult access_manager_prepare_transfer_if_saved(
    OwnershipTable *table,
    const char *original_path,
    int current_writer_id,
    int saved);

ManagerResult access_manager_detect_shadow_modifications(
    OwnershipTable *table,
    size_t *modified_count);

ManagerResult access_manager_detect_original_update(
    OwnershipTable *table,
    const char *original_path,
    size_t *pending_shadow_count);

ManagerResult access_manager_update_shadow(
    OwnershipTable *table,
    const char *original_path,
    int shadow_copy_id);

ManagerResult access_manager_cleanup_temporary_shadows(OwnershipTable *table);

ManagerResult access_manager_retry_pending_cleanups(OwnershipTable *table);

ManagerResult access_manager_save_shadow(OwnershipTable *table,
                                          const char *original_path,
                                          const Application *application);

void access_manager_print_status(const OwnershipTable *table);
const char *manager_result_string(ManagerResult result);

#endif
