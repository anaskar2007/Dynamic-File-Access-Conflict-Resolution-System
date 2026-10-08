#include "simulated_application.h"
#include "access_manager.h"
#include "ownership_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INPUT_LENGTH 256
#define MAX_APPLICATIONS 16

static int stdin_reached_eof;

typedef struct {
    SimulatedApplication applications[MAX_APPLICATIONS];
    size_t count;
} ApplicationRegistry;

static int read_line(const char *prompt, char *buffer, size_t size)
{
    printf("%s", prompt);
    if (fgets(buffer, (int)size, stdin) == NULL) {
        stdin_reached_eof = 1;
        return 0;
    }
    buffer[strcspn(buffer, "\r\n")] = '\0';
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
    return *end == '\0' && value >= 0 && value <= 2147483647L
        ? (int)value
        : -1;
}

static SimulatedApplication *find_application(
    ApplicationRegistry *registry,
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
    int id = read_id("Application ID: ");
    SimulatedApplication *application = find_application(registry, id);

    if (application == NULL) {
        printf("Application is not registered.\n");
    }
    return application;
}

static void sync_application(OwnershipTable *table,
                             SimulatedApplication *application)
{
    AccessSnapshot access;

    if (ownership_table_get_access_snapshot(
            table,
            application->selected_file,
            application->identity.application_id,
            &access) != MANAGER_OK) {
        return;
    }
    application->access_mode = access.access_mode;
    application->file_state = access.file_state;
    application->request_state = access.request_state;
    (void)snprintf(application->assigned_path,
                   sizeof(application->assigned_path),
                   "%s",
                   access.assigned_path);
}

static void launch_application(ApplicationRegistry *registry,
                               OwnershipTable *table)
{
    SimulatedApplication *application;
    char name[MAX_APPLICATION_NAME];
    char path[MAX_PATH_LENGTH];
    int id;

    if (registry->count >= MAX_APPLICATIONS) {
        printf("Application capacity reached.\n");
        return;
    }
    id = read_id("Application ID: ");
    if (id <= 0 || find_application(registry, id) != NULL ||
        !read_line("Application name: ", name, sizeof(name)) ||
        !read_line("Initial file path: ", path, sizeof(path))) {
        printf("Invalid application registration.\n");
        return;
    }

    application = &registry->applications[registry->count++];
    simulated_application_init(application, id, name);
    if (simulated_application_connect(table, application, path) != MANAGER_OK) {
        registry->count--;
        printf("Application registration failed.\n");
        return;
    }
    printf("Registered Application %d (%s).\n", id, name);
}

static void choose_file(OwnershipTable *table,
                        SimulatedApplication *application)
{
    char path[MAX_PATH_LENGTH];

    if (application == NULL || !read_line("File path: ", path, sizeof(path))) {
        return;
    }
    if (simulated_application_connect(table, application, path) != MANAGER_OK) {
        printf("File selection rejected.\n");
    }
}

static void open_read(OwnershipTable *table,
                      ApplicationRegistry *registry)
{
    SimulatedApplication *application = select_application(registry);
    char choice[INPUT_LENGTH];
    ManagerResult result;

    if (application == NULL) {
        return;
    }
    choose_file(table, application);
    printf("READ mode [DIRECT/SHADOW]: ");
    if (!read_line("", choice, sizeof(choice))) {
        return;
    }
    result = access_manager_request_read(
        table,
        application->selected_file,
        &application->identity,
        strcmp(choice, "SHADOW") == 0 || strcmp(choice, "shadow") == 0);
    if (result == MANAGER_SHADOW_REQUIRED) {
        printf("Shadow Copy? (y/n): ");
        if (!read_line("", choice, sizeof(choice)) ||
            (choice[0] != 'y' && choice[0] != 'Y')) {
            printf("READ request cancelled.\n");
            return;
        }
        result = access_manager_request_read(
            table, application->selected_file, &application->identity, 1);
    }
    if (result == MANAGER_OK) {
        sync_application(table, application);
    }
    printf("Open READ result: %s.\n", manager_result_string(result));
}

static void open_read_write(OwnershipTable *table,
                            ApplicationRegistry *registry)
{
    SimulatedApplication *application = select_application(registry);
    AccessSnapshot access;
    ManagerResult result;
    char choice[INPUT_LENGTH];

    if (application == NULL) {
        return;
    }
    choose_file(table, application);
    result = ownership_table_get_access_snapshot(
                 table,
                 application->selected_file,
                 application->identity.application_id,
                 &access) != MANAGER_OK
        ? access_manager_request_read_write(
              table, application->selected_file, &application->identity)
        : access_manager_upgrade_read_write(
              table, application->selected_file, &application->identity);

    if (result == MANAGER_WRITE_CONFLICT) {
        printf("Shadow Copy? (y/n): ");
        if (read_line("", choice, sizeof(choice)) &&
            (choice[0] == 'y' || choice[0] == 'Y')) {
            result = access_manager_request_read_write_shadow(
                table, application->selected_file, &application->identity);
        } else {
            printf("[WAIT] [REQUEST TRANSFER] [CANCEL]\n");
            if (!read_line("Choice: ", choice, sizeof(choice))) {
                return;
            }
            if (strcmp(choice, "WAIT") == 0 ||
                strcmp(choice, "wait") == 0) {
                result = access_manager_wait_read_write(
                    table, application->selected_file, &application->identity);
            } else if (strcmp(choice, "REQUEST TRANSFER") == 0 ||
                       strcmp(choice, "request transfer") == 0) {
                result = access_manager_request_transfer(
                    table, application->selected_file, &application->identity);
            } else {
                printf("WRITE request cancelled.\n");
                return;
            }
        }
    }
    if (result == MANAGER_OK) {
        sync_application(table, application);
    } else if (result == MANAGER_OK || result == MANAGER_TRANSFER_PENDING) {
        application->request_state = REQUEST_WAITING;
    }
    printf("Open READ + WRITE result: %s.\n", manager_result_string(result));
}

static void select_waiting_writer(ApplicationRegistry *registry,
                                  OwnershipTable *table)
{
    char path[MAX_PATH_LENGTH];
    int application_id;
    ManagerResult result;

    if (!read_line("Original file path: ", path, sizeof(path))) {
        return;
    }
    application_id = read_id("Waiting application ID: ");
    result = access_manager_assign_waiting_writer(
        table, path, application_id);
    if (result == MANAGER_OK) {
        SimulatedApplication *application =
            find_application(registry, application_id);
        if (application != NULL) {
            sync_application(table, application);
        }
    }
    printf("Manual WRITE selection result: %s.\n",
           manager_result_string(result));
}

static void print_menu(void)
{
    printf("\nStandalone Access Manager Simulator\n"
           "1. Register application\n"
           "2. Open READ\n"
           "3. Open READ + WRITE\n"
           "4. Select waiting WRITE owner\n"
           "5. Release application access\n"
           "6. Simulate application failure\n"
           "7. Modify application file\n"
           "8. Save application\n"
           "9. Show application status\n"
           "10. Show ownership table\n"
           "0. Exit\n");
}

int main(void)
{
    OwnershipTable *table;
    ApplicationRegistry registry;
    int choice;

    table = ownership_table_create();
    if (table == NULL) {
        fprintf(stderr, "Unable to allocate ownership table.\n");
        return EXIT_FAILURE;
    }
    memset(&registry, 0, sizeof(registry));
    access_manager_init(table);
    for (;;) {
        print_menu();
        choice = read_id("Select option: ");
        if (stdin_reached_eof) {
            (void)access_manager_cleanup_temporary_shadows(table);
            (void)access_manager_retry_pending_cleanups(table);
            ownership_table_destroy(table);
            return EXIT_SUCCESS;
        }
        if (choice == 0) {
            (void)access_manager_cleanup_temporary_shadows(table);
            (void)access_manager_retry_pending_cleanups(table);
            ownership_table_destroy(table);
            return EXIT_SUCCESS;
        }
        switch (choice) {
        case 1:
            launch_application(&registry, table);
            break;
        case 2:
            open_read(table, &registry);
            break;
        case 3:
            open_read_write(table, &registry);
            break;
        case 4:
            select_waiting_writer(&registry, table);
            break;
        case 5:
        {
            SimulatedApplication *application = select_application(&registry);
            ManagerResult result;
            if (application == NULL) {
                break;
            }
            result = application->access_mode == ACCESS_READ_WRITE
                ? access_manager_release_read_write(
                      table,
                      application->selected_file,
                      application->identity.application_id)
                : access_manager_release_read(
                      table,
                      application->selected_file,
                      application->identity.application_id);
            if (result == MANAGER_OK) {
                simulated_application_release(application);
            }
            printf("Release result: %s.\n", manager_result_string(result));
            break;
        }
        case 6:
        {
            SimulatedApplication *application = select_application(&registry);
            ManagerResult result;
            if (application == NULL) {
                break;
            }
            result = access_manager_handle_application_failure(
                table,
                application->selected_file,
                application->identity.application_id);
            if (result == MANAGER_OK) {
                simulated_application_crash(application);
            }
            printf("Failure result: %s.\n", manager_result_string(result));
            break;
        }
        case 7:
        {
            SimulatedApplication *application = select_application(&registry);
            if (application != NULL) {
                printf("Modify result: %s.\n",
                       manager_result_string(
                           simulated_application_modify(
                               table, application)));
            }
            break;
        }
        case 8:
        {
            SimulatedApplication *application = select_application(&registry);
            if (application != NULL) {
                ManagerResult result = simulated_application_save(table, application);
                printf("Save result: %s.\n", manager_result_string(result));
            }
            break;
        }
        case 9:
        {
            SimulatedApplication *application = select_application(&registry);
            if (application != NULL) {
                sync_application(table, application);
                simulated_application_print_status(application);
            }
            break;
        }
        case 10:
            access_manager_print_status(table);
            break;
        default:
            printf("Unknown option.\n");
            break;
        }
        if (stdin_reached_eof) {
            (void)access_manager_cleanup_temporary_shadows(table);
            (void)access_manager_retry_pending_cleanups(table);
            ownership_table_destroy(table);
            return EXIT_SUCCESS;
        }
    }
}
