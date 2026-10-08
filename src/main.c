#include "access_manager.h"
#include "ownership_table.h"
#include "simulated_application.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_LENGTH 256
#define MAX_SIMULATED_APPLICATIONS 32

typedef struct {
    SimulatedApplication applications[MAX_SIMULATED_APPLICATIONS];
    size_t count;
} ApplicationRegistry;

static void select_waiting_writer(ApplicationRegistry *registry,
                                  OwnershipTable *table,
                                  const char *path);
static void transfer_write(ApplicationRegistry *registry,
                           OwnershipTable *table);
static void open_read_write_for_application(
    ApplicationRegistry *registry,
    OwnershipTable *table,
    SimulatedApplication *app);

static void trim_newline(char *text)
{
    text[strcspn(text, "\r\n")] = '\0';
}

static int read_line(const char *prompt, char *buffer, size_t size)
{
    printf("%s", prompt);
    if (fgets(buffer, (int)size, stdin) == NULL) {
        return 0;
    }
    trim_newline(buffer);
    return buffer[0] != '\0';
}

static int read_id(const char *prompt)
{
    char input[INPUT_LENGTH];
    char *end;
    long value;

    if (!read_line(prompt, input, sizeof(input))) {
        return -1;
    }
    value = strtol(input, &end, 10);
    if (*end != '\0' || value <= 0 || value > 2147483647L) {
        return -1;
    }
    return (int)value;
}

static SimulatedApplication *find_application(ApplicationRegistry *registry,
                                              int application_id)
{
    size_t i;

    for (i = 0; i < registry->count; ++i) {
        if (registry->applications[i].identity.application_id ==
            application_id) {
            return &registry->applications[i];
        }
    }
    return NULL;
}

static SimulatedApplication *select_application(ApplicationRegistry *registry)
{
    int application_id = read_id("Application ID: ");
    SimulatedApplication *application;

    if (application_id <= 0) {
        printf("Error: invalid application ID.\n");
        return NULL;
    }

    application = find_application(registry, application_id);
    if (application == NULL) {
        printf("Error: application is not launched.\n");
    }
    return application;
}

static void sync_application_from_manager(OwnershipTable *table,
                                          SimulatedApplication *app)
{
    AccessSnapshot access;

    if (app == NULL) {
        return;
    }

    if (ownership_table_get_access_snapshot(
            table,
            app->selected_file,
            app->identity.application_id,
            &access) != MANAGER_OK) {
        return;
    }

    app->access_mode = access.access_mode;
    app->file_state = access.file_state;
    app->request_state = access.request_state;
    (void)snprintf(app->assigned_path,
                   sizeof(app->assigned_path),
                   "%s",
                   access.assigned_path);
}

static void launch_application(ApplicationRegistry *registry)
{
    char name[MAX_APPLICATION_NAME];
    int application_id;
    SimulatedApplication *application;

    if (registry->count >= MAX_SIMULATED_APPLICATIONS) {
        printf("Error: application capacity reached.\n");
        return;
    }

    application_id = read_id("Application ID: ");
    if (application_id <= 0 ||
        find_application(registry, application_id) != NULL) {
        printf("Error: application ID is invalid or already launched.\n");
        return;
    }

    if (!read_line("Application name (Word/PDF Viewer/Text Editor/"
                   "Image Processor): ",
                   name,
                   sizeof(name))) {
        printf("Error: application name is required.\n");
        return;
    }

    application = &registry->applications[registry->count];
    simulated_application_init(application, application_id, name);
    application->connection_state = SIMULATED_APP_CONNECTED;
    application->connected_to_manager = 1;
    registry->count++;

    printf("Launched %s with simulated PID %lu.\n",
           application->identity.name,
           application->process_id);
}

static SimulatedApplication *select_and_assign_file(
    ApplicationRegistry *registry,
    OwnershipTable *table)
{
    SimulatedApplication *application = select_application(registry);
    char path[MAX_PATH_LENGTH];
    ManagerResult result;

    if (application == NULL) {
        return NULL;
    }

    if (!read_line("File path: ", path, sizeof(path))) {
        printf("Error: file path is required.\n");
        return NULL;
    }

    result = simulated_application_connect(table, application, path);
    if (result != MANAGER_OK) {
        if (result == MANAGER_INVALID_STATE) {
            printf("Application has active access to its current file. "
                   "Release the current access before selecting another file.\n");
        }
        printf("File selection failed: %d.\n", result);
        return NULL;
    }
    return application;
}

static void open_read(ApplicationRegistry *registry,
                      OwnershipTable *table)
{
    SimulatedApplication *app = select_and_assign_file(registry, table);
    ManagerResult result;
    char choice[INPUT_LENGTH];

    if (app == NULL) {
        return;
    }

    result = access_manager_request_read(table,
                                         app->selected_file,
                                         &app->identity,
                                         0);
    if (result == MANAGER_SHADOW_REQUIRED) {
        printf("Shadow Copy? (y/n): ");
        if (!read_line("", choice, sizeof(choice))) {
            return;
        }
        if (choice[0] == 'y' || choice[0] == 'Y') {
            result = access_manager_request_read(table,
                                                 app->selected_file,
                                                 &app->identity,
                                                 1);
        } else {
            printf("Options: [CLOSE] [MOVE TO READ+WRITE]\n");
            if (!read_line("Choice: ", choice, sizeof(choice))) {
                return;
            }
            if (strcmp(choice, "MOVE TO READ+WRITE") == 0 ||
                strcmp(choice, "move to read+write") == 0) {
                open_read_write_for_application(registry, table, app);
                return;
            }
            printf("READ request cancelled.\n");
            return;
        }
    } else if (result != MANAGER_OK) {
        printf("READ request rejected: %s.\n",
               manager_result_string(result));
        return;
    }

    if (result == MANAGER_OK) {
        sync_application_from_manager(table, app);
        printf("%s opened %s for READ.\n",
               app->identity.name,
               app->file_state == FILE_STATE_TEMPORARY_SHADOW
                   ? "a temporary shadow"
                   : "the original");
    } else {
        printf("READ request rejected: %s.\n",
               manager_result_string(result));
    }
}

static void open_read_write_for_application(
    ApplicationRegistry *registry,
    OwnershipTable *table,
    SimulatedApplication *app)
{
    ManagerResult result;
    char choice[INPUT_LENGTH];

    if (app == NULL) {
        return;
    }

    {
        AccessSnapshot access;
        result = ownership_table_get_access_snapshot(
                     table,
                     app->selected_file,
                     app->identity.application_id,
                     &access) != MANAGER_OK
            ? access_manager_request_read_write(
                  table, app->selected_file, &app->identity)
            : access_manager_upgrade_read_write(
                  table, app->selected_file, &app->identity);
    }
    if (result == MANAGER_WRITE_CONFLICT) {
        printf("WRITE access is currently owned by another application.\n");
        printf("Shadow Copy? (y/n): ");
        if (!read_line("", choice, sizeof(choice))) {
            return;
        }

        if (choice[0] == 'y' || choice[0] == 'Y') {
            result = access_manager_request_read_write_shadow(
                table,
                app->selected_file,
                &app->identity);
            if (result == MANAGER_OK) {
                sync_application_from_manager(table, app);
                printf("%s opened a temporary shadow for READ + WRITE.\n",
                       app->identity.name);
                return;
            }
        } else {
            printf("Options: [WAIT] [REQUEST TRANSFER] [CANCEL]\n");
            if (!read_line("Choice: ", choice, sizeof(choice))) {
                return;
            }
        }

        if (strcmp(choice, "WAIT") == 0 ||
            strcmp(choice, "wait") == 0) {
            result = access_manager_wait_read_write(table,
                                                    app->selected_file,
                                                    &app->identity);
            if (result == MANAGER_OK) {
                app->request_state = REQUEST_WAITING;
                printf("%s is waiting for WRITE access.\n",
                       app->identity.name);
                return;
            }
        } else if (strcmp(choice, "REQUEST TRANSFER") == 0 ||
                   strcmp(choice, "request transfer") == 0) {
            result = access_manager_request_transfer(table,
                                                     app->selected_file,
                                                     &app->identity);
            if (result == MANAGER_TRANSFER_PENDING) {
                app->request_state = REQUEST_WAITING;
                printf("%s has requested a WRITE transfer.\n",
                       app->identity.name);
                transfer_write(registry, table);
            } else {
                printf("Transfer request rejected: %s.\n",
                       manager_result_string(result));
            }
            return;
        } else {
            printf("WRITE request cancelled.\n");
            return;
        }
    }

    if (result == MANAGER_OK) {
        sync_application_from_manager(table, app);
        printf("%s opened %s for READ + WRITE.\n",
               app->identity.name,
               app->file_state == FILE_STATE_TEMPORARY_SHADOW
                   ? "a temporary shadow"
                   : "the original");
    } else if (result == MANAGER_TRANSFER_PENDING) {
        app->request_state = REQUEST_WAITING;
        printf("%s is pending a user-controlled WRITE transfer.\n",
               app->identity.name);
    } else {
        printf("WRITE request rejected: %s.\n",
               manager_result_string(result));
    }
}

static void open_read_write(ApplicationRegistry *registry,
                            OwnershipTable *table)
{
    SimulatedApplication *app = select_and_assign_file(registry, table);

    if (app != NULL) {
        open_read_write_for_application(registry, table, app);
    }
}

static void modify_application(ApplicationRegistry *registry,
                               OwnershipTable *table)
{
    SimulatedApplication *app = select_application(registry);
    ManagerResult result;

    if (app == NULL) {
        return;
    }
    result = simulated_application_modify(table, app);
    printf("Modify: %s.\n",
           result == MANAGER_OK ? "completed" : manager_result_string(result));
}

static void save_application(ApplicationRegistry *registry,
                             OwnershipTable *table)
{
    SimulatedApplication *app = select_application(registry);
    ManagerResult result;

    if (app == NULL) {
        return;
    }
    result = simulated_application_save(table, app);
    printf("Save: %s.\n",
           result == MANAGER_OK ? "completed" : manager_result_string(result));
}

static void release_application(ApplicationRegistry *registry,
                                OwnershipTable *table)
{
    SimulatedApplication *app = select_application(registry);
    ManagerResult result;
    char save_choice[INPUT_LENGTH];

    if (app == NULL) {
        return;
    }

    if (app->access_mode == ACCESS_READ_WRITE) {
        if (!read_line("Have changes been saved? (y/n): ",
                       save_choice,
                       sizeof(save_choice)) ||
            (save_choice[0] != 'y' && save_choice[0] != 'Y')) {
            printf("Release cancelled.\n");
            return;
        }
        result = access_manager_release_read_write(
            table,
            app->selected_file,
            app->identity.application_id);
    } else {
        result = access_manager_release_read(table,
                                             app->selected_file,
                                             app->identity.application_id);
    }

    if (result == MANAGER_OK) {
        simulated_application_release(app);
        printf("%s released its file access.\n", app->identity.name);
        if (app->selected_file[0] != '\0' &&
            access_manager_has_waiting_requests(table, app->selected_file)) {
            printf("Waiting WRITE requests are available.\n");
            select_waiting_writer(registry, table, app->selected_file);
        }
    } else {
        printf("Release rejected: %s.\n",
               manager_result_string(result));
    }
}

static void simulate_failure(ApplicationRegistry *registry,
                             OwnershipTable *table)
{
    SimulatedApplication *app = select_application(registry);
    ManagerResult result;

    if (app == NULL || app->selected_file[0] == '\0') {
        printf("Error: select an application with a file first.\n");
        return;
    }

    result = access_manager_handle_application_failure(
        table,
        app->selected_file,
        app->identity.application_id);
    if (result != MANAGER_OK) {
        printf("Failure simulation rejected: %s.\n",
               manager_result_string(result));
        return;
    }

    simulated_application_crash(app);
    printf("%s (PID %lu) has been marked as failed.\n",
           app->identity.name,
           app->process_id);
    printf("WRITE ownership is released.\n");
    if (access_manager_has_waiting_requests(table, app->selected_file)) {
        select_waiting_writer(registry, table, app->selected_file);
    }
}

static void select_waiting_writer(ApplicationRegistry *registry,
                                  OwnershipTable *table,
                                  const char *path)
{
    FileSnapshot file;
    char input[INPUT_LENGTH];
    char *end;
    long application_id;

    if (ownership_table_get_file_snapshot(table, path, &file) != MANAGER_OK ||
        file.waiting_request_count == 0) {
        printf("No waiting WRITE requests exist for this file.\n");
        return;
    }

    printf("Waiting WRITE requests:\n");
    for (size_t i = 0; i < file.waiting_request_count; ++i) {
        WaitingRequestSnapshot waiting;

        if (ownership_table_get_waiting_snapshot(
                table, path, i, &waiting) != MANAGER_OK) {
            continue;
        }
        printf("  Application %d (%s) - %s\n",
               waiting.application.application_id,
               waiting.application.name,
               waiting.transfer_requested
                   ? "REQUEST TRANSFER"
                   : "WAIT");
    }

    if (!read_line("Select application ID: ", input, sizeof(input))) {
        return;
    }
    application_id = strtol(input, &end, 10);
    if (*end != '\0' || application_id <= 0) {
        printf("Error: invalid application ID.\n");
        return;
    }

    if (access_manager_assign_waiting_writer(table,
                                             path,
                                             (int)application_id) != MANAGER_OK) {
        printf("WRITE assignment failed.\n");
        return;
    }

    {
        SimulatedApplication *app = find_application(registry,
                                                     (int)application_id);
        if (app != NULL) {
            sync_application_from_manager(table, app);
        }
    }
    printf("Application %ld is now the READ + WRITE owner.\n",
           application_id);
}

static void transfer_write(ApplicationRegistry *registry,
                           OwnershipTable *table)
{
    SimulatedApplication *requester = select_application(registry);
    FileSnapshot file;
    char save_choice[INPUT_LENGTH];
    ManagerResult result;
    int previous_writer_id;

    if (requester == NULL || requester->selected_file[0] == '\0') {
        return;
    }

    if (ownership_table_get_file_snapshot(
            table, requester->selected_file, &file) != MANAGER_OK ||
        file.write_owner_application_id == -1) {
        printf("Transfer request rejected: no active WRITE owner exists.\n");
        return;
    }

    {
        size_t i;
        int found = 0;
        WaitingRequestSnapshot waiting;

        for (i = 0; i < file.waiting_request_count; ++i) {
            if (ownership_table_get_waiting_snapshot(
                    table, requester->selected_file, i, &waiting) ==
                    MANAGER_OK &&
                waiting.application.application_id ==
                    requester->identity.application_id &&
                waiting.requested_mode == ACCESS_READ_WRITE &&
                waiting.request_state == REQUEST_WAITING) {
                found = 1;
                break;
            }
        }
        if (!found) {
            printf("Transfer request rejected: application has no waiting "
                   "READ + WRITE request.\n");
            return;
        }
    }

    previous_writer_id = file.write_owner_application_id;
    printf("Current writer is application %d and must save changes.\n",
           previous_writer_id);
    if (!read_line("Has the current writer saved? (y/n): ",
                   save_choice,
                   sizeof(save_choice)) ||
        (save_choice[0] != 'y' && save_choice[0] != 'Y')) {
        printf("Transfer remains pending.\n");
        requester->request_state = REQUEST_WAITING;
        return;
    }

    {
        SimulatedApplication *previous_writer =
            find_application(registry, previous_writer_id);
        result = access_manager_prepare_transfer_if_saved(
            table,
            requester->selected_file,
            previous_writer_id,
            previous_writer != NULL && !previous_writer->dirty);
    }
    if (result != MANAGER_OK) {
        printf("Transfer preparation failed: %s.\n",
               manager_result_string(result));
        return;
    }

    {
        SimulatedApplication *previous_writer =
            find_application(registry, previous_writer_id);
        if (previous_writer != NULL) {
            sync_application_from_manager(table, previous_writer);
        }
    }

    printf("WRITE ownership is temporarily free.\n");
    select_waiting_writer(registry, table, requester->selected_file);
}

static void cleanup_before_exit(OwnershipTable *table)
{
    ManagerResult result = access_manager_cleanup_temporary_shadows(table);

    if (result != MANAGER_OK) {
        printf("Warning: some temporary shadows could not be removed.\n");
    }
    result = access_manager_retry_pending_cleanups(table);
    if (result != MANAGER_OK) {
        printf("Warning: pending shadow cleanup remains.\n");
    }
}

static void check_original_updates(ApplicationRegistry *registry,
                                   OwnershipTable *table)
{
    char path[MAX_PATH_LENGTH];
    FileSnapshot file;
    size_t pending_count;
    ManagerResult result;

    if (!read_line("Original file path: ", path, sizeof(path))) {
        return;
    }

    result = access_manager_detect_original_update(table,
                                                   path,
                                                   &pending_count);
    if (result != MANAGER_OK) {
        printf("Original update check failed: %s.\n",
               manager_result_string(result));
        return;
    }
    if (pending_count == 0) {
        printf("No temporary shadow readers require an update decision.\n");
        return;
    }

    printf("The original file has been updated.\n");
    if (ownership_table_get_file_snapshot(table, path, &file) != MANAGER_OK) {
        return;
    }
    for (size_t i = 0; i < file.shadow_copy_count; ++i) {
        ShadowCopySnapshot shadow;
        char choice[INPUT_LENGTH];

        if (ownership_table_get_shadow_snapshot(
                table, path, i, &shadow) != MANAGER_OK ||
            !shadow.is_temporary ||
            shadow.file_state != FILE_STATE_TEMPORARY_SHADOW ||
            shadow.creation_version >= file.current_version) {
            continue;
        }

        printf("Application %d (%s), shadow %s: [UPDATE] [KEEP]\n",
               shadow.owner.application_id,
               shadow.owner.name,
               shadow.shadow_path);
        if (!read_line("Choice: ", choice, sizeof(choice))) {
            continue;
        }
        if (strcmp(choice, "UPDATE") == 0 ||
            strcmp(choice, "update") == 0) {
            result = access_manager_update_shadow(table,
                                                  path,
                                                  shadow.shadow_copy_id);
            if (result == MANAGER_OK) {
                SimulatedApplication *app = find_application(
                    registry,
                    shadow.owner.application_id);
                if (app != NULL) {
                    sync_application_from_manager(table, app);
                }
                printf("Shadow updated to original version %lu.\n",
                       file.current_version);
            } else {
                printf("UPDATE failed: %s.\n",
                       manager_result_string(result));
            }
        } else {
            printf("Existing shadow retained.\n");
        }
    }
}

static void print_overview(const OwnershipTable *table)
{
    printf("\n========== FILE ACCESS MANAGER ==========\n");
    access_manager_print_status(table);
}

static void print_menu(void)
{
    printf("\n------------------------------------------\n");
    printf("1. Launch Application\n");
    printf("2. Open File\n");
    printf("3. Request READ\n");
    printf("4. Request READ + WRITE\n");
    printf("5. Modify File\n");
    printf("6. Save File\n");
    printf("7. Release Access\n");
    printf("8. View Ownership Table\n");
    printf("9. Check Original Updates\n");
    printf("10. Check Shadow Copies\n");
    printf("11. Transfer WRITE Ownership\n");
    printf("12. Simulate Application Failure\n");
    printf("13. Exit\n");
    printf("------------------------------------------\n");
}

int main(void)
{
    OwnershipTable *table;
    ApplicationRegistry registry;
    char input[INPUT_LENGTH];
    char *end;
    long choice;

    table = ownership_table_create();
    if (table == NULL) {
        fprintf(stderr, "Error: unable to allocate ownership table.\n");
        return EXIT_FAILURE;
    }
    memset(&registry, 0, sizeof(registry));
    access_manager_init(table);

    printf("========== FILE ACCESS MANAGER ==========\n");
    printf("Central Access Manager authority started.\n");

    for (;;) {
        print_menu();
        if (!read_line("Select an option: ", input, sizeof(input))) {
            break;
        }
        choice = strtol(input, &end, 10);
        if (*end != '\0') {
            printf("Error: enter a numeric menu option.\n");
            continue;
        }

        switch (choice) {
        case 1:
            launch_application(&registry);
            break;
        case 2:
        {
            SimulatedApplication *app =
                select_and_assign_file(&registry, table);
            if (app != NULL) {
                printf("%s selected %s.\n",
                       app->identity.name,
                       app->selected_file);
            }
            break;
        }
        case 3:
            open_read(&registry, table);
            break;
        case 4:
            open_read_write(&registry, table);
            break;
        case 5:
            modify_application(&registry, table);
            break;
        case 6:
            save_application(&registry, table);
            break;
        case 7:
            release_application(&registry, table);
            break;
        case 8:
            print_overview(table);
            break;
        case 9:
            check_original_updates(&registry, table);
            break;
        case 10:
        {
            size_t modified_count;
            ManagerResult result =
                access_manager_detect_shadow_modifications(table,
                                                           &modified_count);
            printf("Shadow check: %s (%zu modified).\n",
                   manager_result_string(result),
                   modified_count);
            break;
        }
        case 11:
            transfer_write(&registry, table);
            break;
        case 12:
            simulate_failure(&registry, table);
            break;
        case 13:
            cleanup_before_exit(table);
            free(table);
            return EXIT_SUCCESS;
        default:
            printf("Error: unknown option.\n");
            break;
        }
    }

    cleanup_before_exit(table);
    free(table);
    return EXIT_SUCCESS;
}
