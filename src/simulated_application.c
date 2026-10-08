#include "simulated_application.h"
#include "access_manager.h"
#include "ownership_table.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

static unsigned long current_process_id(void)
{
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return 0;
#endif
}

static int valid_application(const SimulatedApplication *application)
{
    return application != NULL &&
           application->identity.application_id > 0;
}

void simulated_application_init(SimulatedApplication *application,
                                int application_id,
                                const char *name)
{
    if (application == NULL) {
        return;
    }

    memset(application, 0, sizeof(*application));
    application->identity.application_id = application_id;
    application->process_id = current_process_id();
    application->identity.process_id = application->process_id;
    application->access_mode = ACCESS_READ;
    application->file_state = FILE_STATE_ORIGINAL;
    application->request_state = REQUEST_RELEASED;
    application->connection_state = SIMULATED_APP_DISCONNECTED;

    if (name != NULL) {
        (void)snprintf(application->identity.name,
                       sizeof(application->identity.name),
                       "%s",
                       name);
    }
}

ManagerResult simulated_application_connect(
    const OwnershipTable *table,
    SimulatedApplication *application,
    const char *selected_file)
{
    if (!valid_application(application) || selected_file == NULL ||
        selected_file[0] == '\0' ||
        strlen(selected_file) >= MAX_PATH_LENGTH) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (table == NULL) {
        return MANAGER_INVALID_ARGUMENT;
    }

    if (application->selected_file[0] != '\0' &&
        strcmp(application->selected_file, selected_file) != 0) {
        AccessSnapshot access;
        if (ownership_table_get_access_snapshot(
                table,
                application->selected_file,
                application->identity.application_id,
                &access) == MANAGER_OK) {
            return MANAGER_INVALID_STATE;
        }
    }

    (void)snprintf(application->selected_file,
                   sizeof(application->selected_file),
                   "%s",
                   selected_file);
    (void)snprintf(application->assigned_path,
                   sizeof(application->assigned_path),
                   "%s",
                   selected_file);
    application->connection_state = SIMULATED_APP_CONNECTED;
    application->connected_to_manager = 1;
    application->request_state = REQUEST_ACTIVE;
    return MANAGER_OK;
}

ManagerResult simulated_application_modify(
    OwnershipTable *table,
    SimulatedApplication *application)
{
    FILE *file;
    const char *path;

    if (table == NULL || !valid_application(application) ||
        application->connection_state != SIMULATED_APP_CONNECTED ||
        application->request_state != REQUEST_ACTIVE ||
        access_manager_begin_modification(table,
                                          &application->identity,
                                          application->assigned_path) !=
            MANAGER_OK) {
        return MANAGER_INVALID_STATE;
    }

    path = application->assigned_path;
    application->modification_active = 1;
    file = fopen(path, "ab");
    if (file == NULL) {
        (void)access_manager_end_modification(
            table, &application->identity, application->assigned_path);
        application->modification_active = 0;
        return MANAGER_FILE_COPY_FAILED;
    }

    if (fputs("\n[modified by simulated application]\n", file) == EOF) {
        (void)fclose(file);
        (void)access_manager_end_modification(
            table, &application->identity, application->assigned_path);
        application->modification_active = 0;
        return MANAGER_FILE_COPY_FAILED;
    }
    if (fclose(file) != 0) {
        (void)access_manager_end_modification(
            table, &application->identity, application->assigned_path);
        application->modification_active = 0;
        return MANAGER_FILE_COPY_FAILED;
    }

    (void)access_manager_end_modification(
        table, &application->identity, application->assigned_path);
    application->modification_active = 0;
    application->dirty = 1;
    return MANAGER_OK;
}

ManagerResult simulated_application_save(
    OwnershipTable *table,
    SimulatedApplication *application)
{
    AccessSnapshot access;
    ManagerResult result;

    if (table == NULL || !valid_application(application) ||
        application->connection_state != SIMULATED_APP_CONNECTED ||
        application->request_state != REQUEST_ACTIVE) {
        return MANAGER_INVALID_STATE;
    }

    if (ownership_table_get_access_snapshot(
            table,
            application->selected_file,
            application->identity.application_id,
            &access) != MANAGER_OK ||
        access.request_state != REQUEST_ACTIVE) {
        return MANAGER_INVALID_STATE;
    }

    if (access.file_state == FILE_STATE_TEMPORARY_SHADOW) {
        result = access_manager_save_shadow(
            table,
            application->selected_file,
            &application->identity);
        if (result != MANAGER_OK) {
            return result;
        }
        if (ownership_table_get_access_snapshot(
                table,
                application->selected_file,
                application->identity.application_id,
                &access) != MANAGER_OK) {
            return MANAGER_INVALID_STATE;
        }
    }

    application->access_mode = access.access_mode;
    application->file_state = access.file_state;
    application->request_state = access.request_state;
    (void)snprintf(application->assigned_path,
                   sizeof(application->assigned_path),
                   "%s",
                   access.assigned_path);
    application->dirty = 0;
    return MANAGER_OK;
}

void simulated_application_release(SimulatedApplication *application)
{
    if (!valid_application(application)) {
        return;
    }

    application->request_state = REQUEST_RELEASED;
    application->connection_state = SIMULATED_APP_RELEASED;
    application->connected_to_manager = 0;
    application->dirty = 0;
    application->modification_active = 0;
}

void simulated_application_crash(SimulatedApplication *application)
{
    if (!valid_application(application)) {
        return;
    }

    application->request_state = REQUEST_FAILED;
    application->connection_state = SIMULATED_APP_CRASHED;
    application->connected_to_manager = 0;
    application->dirty = 0;
    application->modification_active = 0;
}

static const char *access_mode_string(AccessMode mode)
{
    return mode == ACCESS_READ_WRITE ? "READ + WRITE" : "READ";
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

const char *simulated_application_state_string(
    SimulatedApplicationState state)
{
    switch (state) {
    case SIMULATED_APP_DISCONNECTED:
        return "DISCONNECTED";
    case SIMULATED_APP_CONNECTED:
        return "CONNECTED";
    case SIMULATED_APP_RELEASED:
        return "RELEASED";
    case SIMULATED_APP_CRASHED:
        return "CRASHED";
    default:
        return "UNKNOWN";
    }
}

void simulated_application_print_status(
    const SimulatedApplication *application)
{
    if (!valid_application(application)) {
        printf("Application state is unavailable.\n");
        return;
    }

    printf("\n========== SIMULATED APPLICATION ==========\n");
    printf("Application ID: %d\n", application->identity.application_id);
    printf("Application name: %s\n", application->identity.name);
    printf("Process ID: %lu\n", application->process_id);
    printf("Connection: %s\n",
           application->connected_to_manager ? "connected" : "disconnected");
    printf("Connection state: %s\n",
           simulated_application_state_string(
               application->connection_state));
    printf("Selected file: %s\n",
           application->selected_file[0] != '\0'
               ? application->selected_file
               : "(none)");
    printf("Assigned path: %s\n",
           application->assigned_path[0] != '\0'
               ? application->assigned_path
               : "(none)");
    printf("Access mode: %s\n", access_mode_string(application->access_mode));
    printf("File state: %s\n", file_state_string(application->file_state));
    printf("Request state: %d\n", application->request_state);
    printf("===========================================\n");
}
