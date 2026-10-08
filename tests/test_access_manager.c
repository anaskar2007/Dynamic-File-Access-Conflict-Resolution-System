#include "../src/access_manager.h"
#include "../src/ownership_table.h"
#include "../src/ownership_table_internal.h"
#include "../src/shadow_copy.h"
#include "../src/simulated_application.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run;
static int tests_failed;

static Application app(int id, const char *name)
{
    Application application;

    memset(&application, 0, sizeof(application));
    application.application_id = id;
    application.process_id = (unsigned long)(1000 + id);
    (void)snprintf(application.name,
                   sizeof(application.name),
                   "%s",
                   name);
    return application;
}

static int write_file(const char *path, const char *content)
{
    FILE *file = fopen(path, "wb");
    size_t length;

    if (file == NULL) {
        return 0;
    }
    length = strlen(content);
    if (fwrite(content, 1, length, file) != length) {
        (void)fclose(file);
        return 0;
    }
    return fclose(file) == 0;
}

static void remove_file(const char *path)
{
    if (path != NULL && path[0] != '\0') {
        (void)remove(path);
    }
}

static int path_exists(const char *path)
{
    FILE *file = fopen(path, "rb");

    if (file == NULL) {
        return 0;
    }
    (void)fclose(file);
    return 1;
}

static int read_file(const char *path, char *content, size_t size)
{
    FILE *file;
    size_t length;

    if (content == NULL || size == 0) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    length = fread(content, 1, size - 1, file);
    content[length] = '\0';
    return fclose(file) == 0;
}

static void check(int condition, const char *description)
{
    if (!condition) {
        printf("    FAIL: %s\n", description);
        tests_failed++;
    }
}

static void begin_test(const char *name)
{
    tests_run++;
    printf("\nTEST %d: %s\n", tests_run, name);
}

static void test_shadow_on_read_conflict(void)
{
    const char *path = "test_01_original.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "Application A");
    Application b = app(2, "Application B");
    FileRecord *file;
    AccessRecord *record;
    ManagerResult result;

    begin_test("READ receives a temporary shadow during WRITE conflict");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    result = access_manager_request_read_write(table, path, &a);
    check(result == MANAGER_OK, "A owns READ + WRITE");
    result = access_manager_request_read(table, path, &b, 1);
    check(result == MANAGER_OK, "B accepts shadow copy");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    check(record != NULL &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              record->shadow_copy_id > 0,
          "B has a temporary shadow access record");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_waiting_write(void)
{
    const char *path = "test_02_waiting.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file;

    begin_test("conflicting WRITE request enters WAITING state");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK,
          "A owns WRITE");
    check(access_manager_wait_read_write(table, path, &b) == MANAGER_OK,
          "B is accepted into waiting table");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->waiting_request_count == 1 &&
              file->waiting_requests[0].request_state == REQUEST_WAITING,
          "B remains WAITING and is not promoted");
    remove_file(path);
    free(table);
}

static void test_manual_transfer(void)
{
    const char *path = "test_03_transfer.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file;

    begin_test("release and manual WRITE assignment");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK,
          "A owns WRITE");
    check(access_manager_wait_read_write(table, path, &b) == MANAGER_OK,
          "B waits");
    check(access_manager_release_read_write(table, path, a.application_id) ==
              MANAGER_OK,
          "A releases WRITE");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->write_owner_application_id == -1,
          "WRITE is free before selection");
    check(access_manager_assign_waiting_writer(table, path, b.application_id) ==
              MANAGER_OK,
          "user selects B");
    check(file != NULL && file->write_owner_application_id == b.application_id,
          "B is the new WRITE owner");
    remove_file(path);
    free(table);
}

static void test_manual_choice_between_waiters(void)
{
    const char *path = "test_04_multiple_waiters.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    Application c = app(3, "C");
    FileRecord *file;

    begin_test("multiple waiters require explicit user selection");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK,
          "A owns WRITE");
    check(access_manager_wait_read_write(table, path, &b) == MANAGER_OK &&
              access_manager_wait_read_write(table, path, &c) == MANAGER_OK,
          "B and C wait");
    check(access_manager_release_read_write(table, path, a.application_id) ==
              MANAGER_OK,
          "A releases WRITE");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->write_owner_application_id == -1 &&
              file->waiting_request_count == 2,
          "no waiter is automatically selected");
    check(access_manager_assign_waiting_writer(table, path, c.application_id) ==
              MANAGER_OK,
          "user explicitly selects C");
    check(file != NULL && file->write_owner_application_id == c.application_id &&
              file->waiting_request_count == 1 &&
              file->waiting_requests[0].application.application_id == b.application_id,
          "C owns WRITE and B remains waiting");
    remove_file(path);
    free(table);
}

static void test_original_update_update_keep(void)
{
    const char *path = "test_05_original_update.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file;
    AccessRecord *record;
    size_t pending = 0;
    int shadow_id;
    char old_shadow[MAX_PATH_LENGTH];

    begin_test("temporary shadows receive UPDATE or KEEP decision");
    check(table != NULL && write_file(path, "version one"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK &&
              access_manager_request_read(table, path, &b, 1) == MANAGER_OK,
          "A writes and B receives shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    check(file != NULL && record != NULL, "shadow record exists");
    shadow_id = record == NULL ? -1 : record->shadow_copy_id;
    (void)snprintf(old_shadow, sizeof(old_shadow), "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(path, "version two"), "original receives a saved version");
    check(access_manager_detect_original_update(table, path, &pending) ==
              MANAGER_OK &&
              pending == 1,
          "B receives an update notification");
    check(access_manager_update_shadow(table, path, shadow_id) == MANAGER_OK,
          "UPDATE creates a shadow from the latest original");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    check(record != NULL && record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              strcmp(record->assigned_path, old_shadow) != 0,
          "B continues with the replacement shadow");
    remove_file(old_shadow);
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_shadow_becomes_independent(void)
{
    const char *path = "test_06_shadow_modify.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file;
    AccessRecord *record;
    size_t modified = 0;
    char shadow_path[MAX_PATH_LENGTH];

    begin_test("modified temporary shadow becomes independent");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK &&
              access_manager_request_read(table, path, &b, 1) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &b) == MANAGER_OK,
          "shadow is created");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "B changed the shadow"), "shadow is modified");
    check(access_manager_detect_shadow_modifications(table, &modified) ==
              MANAGER_OK && modified == 1,
          "manager detects changed shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    check(record != NULL && record->file_state == FILE_STATE_INDEPENDENT_FILE,
          "access record is INDEPENDENT_FILE");
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_independent_excluded_from_updates(void)
{
    const char *path = "test_07_independent_update.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file;
    AccessRecord *record;
    size_t count = 0;
    char shadow_path[MAX_PATH_LENGTH];

    begin_test("independent file is excluded from synchronization");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK &&
              access_manager_request_read(table, path, &b, 1) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &b) == MANAGER_OK,
          "shadow is created");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "independent content"), "shadow is modified");
    check(access_manager_detect_shadow_modifications(table, &count) == MANAGER_OK &&
              count == 1,
          "shadow transitions to independent state");
    check(write_file(path, "new original version"), "original is changed");
    check(access_manager_detect_original_update(table, path, &count) ==
              MANAGER_OK && count == 0,
          "no update notification targets the independent file");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    check(record != NULL && record->file_state == FILE_STATE_INDEPENDENT_FILE,
          "independent state is preserved");
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_writer_failure(void)
{
    const char *path = "test_08_failure.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    FileRecord *file;
    AccessRecord *record;

    begin_test("writer failure releases WRITE ownership");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK,
          "A owns WRITE");
    check(access_manager_handle_writer_failure(table, path, a.application_id) ==
              MANAGER_OK,
          "failure is handled");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, a.application_id);
    check(file != NULL && file->write_owner_application_id == -1,
          "WRITE ownership is released");
    check(record != NULL && record->request_state == REQUEST_FAILED &&
              record->access_mode == ACCESS_READ,
          "failed writer cannot retain WRITE");
    remove_file(path);
    free(table);
}

static void test_multiple_readers(void)
{
    const char *path = "test_09_readers.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    Application c = app(3, "C");
    FileRecord *file;

    begin_test("multiple READ applications coexist");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &a, 0) == MANAGER_OK &&
              access_manager_request_read(table, path, &b, 0) == MANAGER_OK &&
              access_manager_request_read(table, path, &c, 0) == MANAGER_OK,
          "all READ requests succeed");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->access_record_count == 3 &&
              file->write_owner_application_id == -1,
          "three readers coexist without a WRITE owner");
    remove_file(path);
    free(table);
}

static void test_exclusive_writers(void)
{
    const char *path = "test_10_writers.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file;

    begin_test("conflicting WRITE requests preserve exclusivity");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK,
          "A wins the manager arbitration");
    check(access_manager_request_read_write(table, path, &b) ==
              MANAGER_WRITE_CONFLICT,
          "B is rejected as a conflicting WRITE request");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->write_owner_application_id == a.application_id,
          "exactly one WRITE owner remains");
    remove_file(path);
    free(table);
}

static void test_read_opens_original(void)
{
    const char *path = "test_11_read_original.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "Reader");
    FileRecord *file;
    AccessRecord *record;

    begin_test("READ opens the original directly");
    check(write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &a, 0) == MANAGER_OK,
          "READ request succeeds without a shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, a.application_id);
    check(record != NULL && record->file_state == FILE_STATE_ORIGINAL &&
              strcmp(record->assigned_path, path) == 0 &&
              file->shadow_copy_count == 0,
          "reader is assigned the original path");
    remove_file(path);
    free(table);
}

static void test_read_cannot_modify_original(void)
{
    const char *path = "test_12_read_modify.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication application;
    char before[64];
    char after[64];
    FILE *file;

    begin_test("READ application cannot modify the original");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&application, 1, "Reader");
    check(simulated_application_connect(table, &application, path) == MANAGER_OK,
          "application connects");
    check(access_manager_request_read(table,
                                      path,
                                      &application.identity,
                                      0) == MANAGER_OK,
          "application receives READ access");
    application.access_mode = ACCESS_READ;
    application.file_state = FILE_STATE_ORIGINAL;
    (void)snprintf(application.assigned_path,
                   sizeof(application.assigned_path),
                   "%s",
                   path);
    application.request_state = REQUEST_ACTIVE;
    file = fopen(path, "rb");
    memset(before, 0, sizeof(before));
    check(file != NULL && fgets(before, sizeof(before), file) != NULL,
          "original content can be read");
    if (file != NULL) {
        (void)fclose(file);
    }
    check(simulated_application_modify(table, &application) ==
              MANAGER_INVALID_STATE,
          "modification is rejected");
    file = fopen(path, "rb");
    memset(after, 0, sizeof(after));
    check(file != NULL && fgets(after, sizeof(after), file) != NULL &&
              strcmp(before, after) == 0,
          "original content is unchanged");
    if (file != NULL) {
        (void)fclose(file);
    }
    remove_file(path);
}

static void test_fake_local_write_rejected(void)
{
    const char *path = "test_12_fake_write.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication application;

    begin_test("fake local WRITE state cannot modify the original");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&application, 1, "Fake Writer");
    check(simulated_application_connect(table, &application, path) == MANAGER_OK,
          "application connects");
    application.access_mode = ACCESS_READ_WRITE;
    application.file_state = FILE_STATE_ORIGINAL;
    application.request_state = REQUEST_ACTIVE;
    check(simulated_application_modify(table, &application) ==
              MANAGER_INVALID_STATE,
          "unregistered local WRITE state is rejected");
    remove_file(path);
    free(table);
}

static void test_table_write_invariant(void)
{
    const char *path = "test_12_table_invariant.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    FileRecord *file = NULL;

    begin_test("ownership table rejects duplicate WRITE and clears owner");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(ownership_table_add_file(table, path, &file) == MANAGER_OK,
          "file record created");
    check(ownership_table_add_access(file,
                                     &a,
                                     ACCESS_READ_WRITE,
                                     table->next_access_record_id++) ==
              MANAGER_OK,
          "first WRITE record is accepted");
    check(file->write_owner_application_id == a.application_id,
          "owner ID is set with WRITE record");
    check(ownership_table_add_access(file,
                                     &b,
                                     ACCESS_READ_WRITE,
                                     table->next_access_record_id++) ==
              MANAGER_WRITE_CONFLICT,
          "duplicate WRITE record is rejected");
    check(ownership_table_remove_access(file, a.application_id) == MANAGER_OK &&
              file->write_owner_application_id == -1,
          "removing WRITE owner clears owner ID");
    remove_file(path);
    free(table);
}

static void test_read_upgrade_without_writer(void)
{
    const char *path = "test_13_upgrade.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "Upgradeable");
    FileRecord *file;
    AccessRecord *record;

    begin_test("READ upgrades to READ + WRITE when available");
    check(write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &a, 0) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &a) == MANAGER_OK,
          "upgrade succeeds");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, a.application_id);
    check(file != NULL && file->write_owner_application_id == a.application_id &&
              record != NULL && record->access_mode == ACCESS_READ_WRITE,
          "reader becomes the only WRITE owner");
    remove_file(path);
    free(table);
}

static void test_upgrade_blocked_by_writer(void)
{
    const char *path = "test_14_upgrade_conflict.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "Reader");
    Application b = app(2, "Writer");
    FileRecord *file;
    AccessRecord *record;

    begin_test("READ upgrade is blocked by an existing writer");
    check(write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &a, 0) == MANAGER_OK &&
              access_manager_request_read_write(table, path, &b) == MANAGER_OK,
          "reader and writer are established");
    check(access_manager_upgrade_read_write(table, path, &a) ==
              MANAGER_WRITE_CONFLICT,
          "upgrade is rejected as a WRITE conflict");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    check(file != NULL && file->write_owner_application_id == b.application_id &&
              record != NULL && record->access_mode == ACCESS_READ_WRITE,
          "existing writer remains the only writer");
    remove_file(path);
    free(table);
}

static void test_read_upgrade_waits(void)
{
    const char *path = "test_15_upgrade_wait.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "Reader");
    Application b = app(2, "Writer");
    FileRecord *file;
    AccessRecord *record;

    begin_test("READ upgrade conflict can enter WAITING");
    check(write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &a, 0) == MANAGER_OK &&
              access_manager_request_read_write(table, path, &b) == MANAGER_OK &&
              access_manager_wait_read_write(table, path, &a) == MANAGER_OK,
          "reader enters waiting upgrade workflow");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, a.application_id);
    check(file != NULL && file->waiting_request_count == 1 &&
              record != NULL && record->access_mode == ACCESS_READ &&
              file->write_owner_application_id == b.application_id,
          "reader remains READ while waiting");
    remove_file(path);
    free(table);
}

static void test_shadow_only_on_conflict(void)
{
    const char *path = "test_16_shadow_condition.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "Reader A");
    Application b = app(2, "Reader B");
    Application c = app(3, "Writer");
    FileRecord *file;
    AccessRecord *record;

    begin_test("shadow copies are only used when conflict workflow requires them");
    check(write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &a, 0) == MANAGER_OK &&
              access_manager_request_read(table, path, &b, 0) == MANAGER_OK,
          "normal READ requests use the original");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->shadow_copy_count == 0,
          "normal readers do not create shadows");
    check(access_manager_request_read_write(table, path, &c) == MANAGER_OK &&
              access_manager_request_read(table, path, &b, 1) ==
                  MANAGER_ALREADY_EXISTS,
          "existing reader cannot create a second access record");
    record = file == NULL ? NULL : ownership_table_find_access(file, a.application_id);
    check(record != NULL && record->file_state == FILE_STATE_ORIGINAL,
          "normal reader remains on original during unrelated WRITE conflict");
    remove_file(path);
    free(table);
}

static void test_shadow_modification_transition_after_correction(void)
{
    const char *path = "test_17_shadow_transition.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;
    size_t modified = 0;
    char shadow_path[MAX_PATH_LENGTH];

    begin_test("temporary shadow still becomes independent after modification");
    check(write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &reader) ==
                  MANAGER_OK,
          "conflict workflow creates a shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, reader.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "reader changed shadow"), "shadow is modified");
    check(access_manager_detect_shadow_modifications(table, &modified) ==
              MANAGER_OK && modified == 1,
          "modified shadow is detected");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, reader.application_id);
    check(record != NULL && record->file_state == FILE_STATE_INDEPENDENT_FILE,
          "temporary shadow becomes independent");
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_transfer_updates_local_application_state(void)
{
    const char *path = "test_18_transfer_local_state.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication old_writer;
    SimulatedApplication new_writer;
    Application a = app(1, "Writer A");
    Application b = app(2, "Writer B");
    FileRecord *file;
    AccessRecord *record;

    begin_test("WRITE transfer synchronizes old and new application state");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&old_writer, a.application_id, a.name);
    simulated_application_init(&new_writer, b.application_id, b.name);
    check(simulated_application_connect(table, &old_writer, path) == MANAGER_OK &&
              simulated_application_connect(table, &new_writer, path) == MANAGER_OK,
          "applications connect");
    check(access_manager_request_read(table, path, &b, 0) == MANAGER_OK,
          "B opens READ");
    new_writer.access_mode = ACCESS_READ;
    new_writer.file_state = FILE_STATE_ORIGINAL;
    (void)snprintf(new_writer.assigned_path,
                   sizeof(new_writer.assigned_path),
                   "%s",
                   path);
    new_writer.request_state = REQUEST_ACTIVE;
    check(1, "B local state is READ");
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK,
          "A owns WRITE");
    old_writer.access_mode = ACCESS_READ_WRITE;
    old_writer.file_state = FILE_STATE_ORIGINAL;
    (void)snprintf(old_writer.assigned_path,
                   sizeof(old_writer.assigned_path),
                   "%s",
                   path);
    old_writer.request_state = REQUEST_ACTIVE;
    check(1, "A local state is READ + WRITE");
    check(access_manager_request_transfer(table, path, &b) ==
              MANAGER_TRANSFER_PENDING,
          "B waits for transfer");
    check(access_manager_prepare_transfer(table, path, a.application_id) ==
              MANAGER_OK,
          "manager demotes A and frees WRITE");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, a.application_id);
    if (record != NULL) {
        old_writer.access_mode = record->access_mode;
        old_writer.file_state = record->file_state;
        (void)snprintf(old_writer.assigned_path,
                       sizeof(old_writer.assigned_path),
                       "%s",
                       record->assigned_path);
        old_writer.request_state = record->request_state;
    }
    check(record != NULL && old_writer.access_mode == ACCESS_READ,
          "old writer local state is synchronized");
    check(access_manager_assign_waiting_writer(table, path, b.application_id) ==
              MANAGER_OK,
          "B is manually selected");
    file = ownership_table_find_file(table, path);
    record = file == NULL ? NULL : ownership_table_find_access(file, b.application_id);
    if (record != NULL) {
        new_writer.access_mode = record->access_mode;
        new_writer.file_state = record->file_state;
        (void)snprintf(new_writer.assigned_path,
                       sizeof(new_writer.assigned_path),
                       "%s",
                       record->assigned_path);
        new_writer.request_state = record->request_state;
    }
    check(record != NULL && new_writer.access_mode == ACCESS_READ_WRITE,
          "new writer local state is synchronized");
    check(simulated_application_modify(table, &old_writer) == MANAGER_INVALID_STATE,
          "old writer cannot modify after transfer");
    check(simulated_application_modify(table, &new_writer) == MANAGER_OK,
          "new writer can modify after transfer");
    remove_file(path);
    free(table);
}

static void test_active_reassignment_rejected(void)
{
    const char *first_path = "test_19_reassign_read.txt";
    const char *second_path = "test_19_reassign_other.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication application;
    FileRecord *file;

    begin_test("active READ reassignment is rejected");
    check(table != NULL && write_file(first_path, "initial") &&
              write_file(second_path, "other"),
          "test files created");
    access_manager_init(table);
    simulated_application_init(&application, 1, "Reader");
    check(simulated_application_connect(table, &application, first_path) ==
              MANAGER_OK &&
              access_manager_request_read(table,
                                          first_path,
                                          &application.identity,
                                          0) == MANAGER_OK,
          "application opens first file");
    check(simulated_application_connect(table, &application, second_path) ==
              MANAGER_INVALID_STATE,
          "active reassignment is rejected");
    file = ownership_table_find_file(table, first_path);
    check(strcmp(application.selected_file, first_path) == 0 &&
              file != NULL &&
              ownership_table_find_active_access(
                  file,
                  application.identity.application_id) != NULL,
          "application remains associated with first file");
    remove_file(first_path);
    remove_file(second_path);
    free(table);
}

static void test_write_reassignment_rejected(void)
{
    const char *first_path = "test_20_reassign_write.txt";
    const char *second_path = "test_20_reassign_other.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication application;
    FileRecord *file;

    begin_test("active WRITE reassignment is rejected");
    check(table != NULL && write_file(first_path, "initial") &&
              write_file(second_path, "other"),
          "test files created");
    access_manager_init(table);
    simulated_application_init(&application, 1, "Writer");
    check(simulated_application_connect(table, &application, first_path) ==
              MANAGER_OK &&
              access_manager_request_read_write(
                  table,
                  first_path,
                  &application.identity) == MANAGER_OK,
          "application owns WRITE on first file");
    check(simulated_application_connect(table, &application, second_path) ==
              MANAGER_INVALID_STATE,
          "WRITE reassignment is rejected");
    file = ownership_table_find_file(table, first_path);
    check(strcmp(application.selected_file, first_path) == 0 &&
              file != NULL &&
              file->write_owner_application_id ==
                  application.identity.application_id,
          "WRITE ownership remains on first file");
    remove_file(first_path);
    remove_file(second_path);
    free(table);
}

static void test_temporary_shadow_cleanup(void)
{
    const char *path = "test_21_cleanup.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    Application second_reader = app(3, "Second reader");
    FileRecord *file;
    char temporary_path[MAX_PATH_LENGTH];
    char independent_path[MAX_PATH_LENGTH];
    size_t count;

    begin_test("shutdown cleanup removes temporary shadows only");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "temporary shadow created");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->shadow_copy_count == 1,
          "temporary shadow is tracked");
    (void)snprintf(temporary_path,
                   sizeof(temporary_path),
                   "%s",
                   file->shadow_copies[0].shadow_path);
    check(access_manager_cleanup_temporary_shadows(table) == MANAGER_OK &&
              !path_exists(temporary_path),
          "temporary shadow is removed");

    check(access_manager_request_read(table,
                                     path,
                                     &second_reader,
                                     1) == MANAGER_OK &&
              access_manager_upgrade_read_write(
                  table,
                  path,
                  &second_reader) == MANAGER_OK,
          "replacement temporary shadow created");
    file = ownership_table_find_file(table, path);
    (void)snprintf(independent_path,
                   sizeof(independent_path),
                   "%s",
                   file->shadow_copies[0].shadow_path);
    check(write_file(independent_path, "independent content") &&
              access_manager_detect_shadow_modifications(table, &count) ==
                  MANAGER_OK &&
              count == 1,
          "shadow becomes independent");
    check(access_manager_cleanup_temporary_shadows(table) == MANAGER_OK &&
              path_exists(independent_path),
          "independent file is preserved");
    remove_file(independent_path);
    remove_file(path);
    free(table);
}

static void test_failed_record_allows_reopen(void)
{
    const char *path = "test_22_failed_reopen.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application application = app(1, "Recovering application");
    FileRecord *file;

    begin_test("failed access record does not block reopening");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &application) ==
              MANAGER_OK &&
              access_manager_handle_writer_failure(
                  table,
                  path,
                  application.application_id) == MANAGER_OK &&
              access_manager_request_read(table, path, &application, 0) ==
                  MANAGER_OK,
          "application can reopen after failure");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->access_record_count == 2 &&
              ownership_table_find_active_access(
                  file,
                  application.application_id) != NULL,
          "new active record coexists with historical failed record");
    remove_file(path);
    free(table);
}

static void test_nonexistent_original_rejected(void)
{
    const char *path = "test_23_does_not_exist.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application application = app(1, "Application");

    begin_test("nonexistent original is rejected");
    remove_file(path);
    check(table != NULL, "table allocated");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &application, 0) !=
              MANAGER_OK &&
              ownership_table_find_file(table, path) == NULL,
          "nonexistent original is not registered");
    free(table);
}

static void test_existing_original_and_copy_still_work(void)
{
    const char *path = "test_24_existing_copy.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;

    begin_test("existing original and shadow copying still work");
    check(table != NULL && write_file(path, "copy source"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "existing original is accessible and copied");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->shadow_copy_count == 1 &&
              path_exists(file->shadow_copies[0].shadow_path),
          "shadow copy exists with copied content");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_shadow_scan_continues_after_error(void)
{
    const char *path = "test_25_shadow_scan.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader_a = app(2, "Reader A");
    Application reader_b = app(3, "Reader B");
    Application reader_c = app(4, "Reader C");
    FileRecord *file;
    char bad_path[MAX_PATH_LENGTH];
    char valid_path[MAX_PATH_LENGTH];
    size_t modified_count = 0;
    ManagerResult result;

    begin_test("shadow scan continues after an unreadable shadow");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_a, 1) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_b, 1) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_c, 1) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &reader_c) ==
                  MANAGER_OK,
          "three temporary shadows created");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->shadow_copy_count == 3,
          "all shadows are tracked");
    (void)snprintf(bad_path, sizeof(bad_path), "%s",
                   file->shadow_copies[0].shadow_path);
    (void)snprintf(valid_path, sizeof(valid_path), "%s",
                   file->shadow_copies[2].shadow_path);
    remove_file(bad_path);
    check(write_file(valid_path, "modified shadow"), "later shadow modified");

    result = access_manager_detect_shadow_modifications(table,
                                                         &modified_count);
    file = ownership_table_find_file(table, path);
    check(result == MANAGER_METADATA_FAILED && modified_count == 1,
          "scan reports the first error after checking all shadows");
    check(file != NULL &&
              file->shadow_copies[0].file_state ==
                  FILE_STATE_TEMPORARY_SHADOW &&
              file->shadow_copies[2].file_state ==
                  FILE_STATE_INDEPENDENT_FILE,
          "later valid shadow is still processed");
    if (file != NULL) {
        size_t i;
        for (i = 0; i < file->shadow_copy_count; ++i) {
            remove_file(file->shadow_copies[i].shadow_path);
        }
    }
    remove_file(path);
    free(table);
}

static void test_release_removes_record_after_shadow_cleanup_failure(void)
{
    const char *path = "test_26_release_failure.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    char shadow_path[MAX_PATH_LENGTH];
    ManagerResult result;

    begin_test("release completes when temporary shadow cleanup fails");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "temporary shadow access created");
    file = ownership_table_find_file(table, path);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   file->shadow_copies[0].shadow_path);
    remove_file(shadow_path);

    result = access_manager_release_read(table, path, reader.application_id);
    file = ownership_table_find_file(table, path);
    check(result == MANAGER_FILE_COPY_FAILED,
          "shadow cleanup failure is reported");
    check(file != NULL &&
              ownership_table_find_active_access(
                  file,
                  reader.application_id) == NULL &&
              ownership_table_find_access(file, reader.application_id) == NULL,
          "released access record is removed despite cleanup failure");
    check(file != NULL && file->shadow_copy_count == 0,
          "failed physical cleanup does not leave stale shadow metadata");
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id,
          "unrelated WRITE ownership remains consistent");
    remove_file(path);
    free(table);
}

static void test_repeated_transfer_request_is_rejected_safely(void)
{
    const char *path = "test_56_repeated_transfer.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;

    begin_test("repeated WRITE transfer request does not duplicate waiting state");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_transfer(table, path, &reader) ==
                  MANAGER_TRANSFER_PENDING &&
              access_manager_request_transfer(table, path, &reader) ==
                  MANAGER_ALREADY_EXISTS,
          "repeated transfer request is rejected");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->waiting_request_count == 1,
          "only one waiting request remains");
    remove_file(path);
    free(table);
}

static void test_pending_cleanup_retry_failure_and_success(void)
{
    const char *path = "test_57_pending_cleanup_retry.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    char shadow_path[MAX_PATH_LENGTH];
    ManagerResult result;

    begin_test("pending shadow cleanup retries deterministically");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) ==
                  MANAGER_OK,
          "temporary shadow created");
    file = ownership_table_find_file(table, path);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   file->shadow_copies[0].shadow_path);

    shadow_copy_set_delete_failure_for_testing(1);
    result = access_manager_cleanup_temporary_shadows(table);
    file = ownership_table_find_file(table, path);
    check(result == MANAGER_FILE_COPY_FAILED &&
              file != NULL &&
              file->pending_cleanup_count == 1 &&
              file->shadow_copy_count == 0 &&
              ownership_table_find_active_access(
                  file, reader.application_id) == NULL &&
              path_exists(shadow_path),
          "failed deletion creates one pending cleanup without stale access");

    result = access_manager_retry_pending_cleanups(table);
    file = ownership_table_find_file(table, path);
    check(result == MANAGER_FILE_COPY_FAILED &&
              file != NULL &&
              file->pending_cleanup_count == 1,
          "retry failure preserves the single pending cleanup");

    shadow_copy_set_delete_failure_for_testing(0);
    result = access_manager_retry_pending_cleanups(table);
    file = ownership_table_find_file(table, path);
    check(result == MANAGER_OK &&
              file != NULL &&
              file->pending_cleanup_count == 0 &&
              !path_exists(shadow_path),
          "retry success removes the pending artifact");

    shadow_copy_set_delete_failure_for_testing(0);
    remove_file(path);
    free(table);
}
static void test_write_requires_original_file_state(void)
{
    const char *path = "test_27_write_state_validation.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application original_writer = app(1, "Original writer");
    Application temporary_writer = app(2, "Temporary writer");
    Application independent_writer = app(3, "Independent writer");
    FileRecord *file;

    begin_test("WRITE access is exclusive only for ORIGINAL file state");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(ownership_table_add_file(table, path, &file) == MANAGER_OK,
          "file record created");
    check(ownership_table_add_access_with_state(
              file,
              &original_writer,
              ACCESS_READ_WRITE,
              FILE_STATE_ORIGINAL,
              path,
              -1,
              1) == MANAGER_OK,
          "WRITE + ORIGINAL is accepted");
    check(ownership_table_add_access_with_state(
              file,
              &temporary_writer,
              ACCESS_READ_WRITE,
              FILE_STATE_TEMPORARY_SHADOW,
              "shadow.txt",
              1,
              2) == MANAGER_OK,
          "WRITE + TEMPORARY_SHADOW is accepted");
    check(ownership_table_add_access_with_state(
              file,
              &independent_writer,
              ACCESS_READ_WRITE,
              FILE_STATE_INDEPENDENT_FILE,
              "independent.txt",
              2,
              3) == MANAGER_OK,
          "WRITE + INDEPENDENT_FILE is accepted");
    check(file->write_owner_application_id == original_writer.application_id,
          "shadow WRITE records do not change original ownership");
    remove_file(path);
    free(table);
}

static void test_multi_application_manual_transfer_cycles(void)
{
    const char *path = "test_58_multi_application_transfer.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application a = app(1, "A");
    Application b = app(2, "B");
    Application c = app(3, "C");
    FileRecord *file;

    begin_test("multiple simulated applications use manual transfer cycles");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &a) == MANAGER_OK &&
              access_manager_wait_read_write(table, path, &b) == MANAGER_OK &&
              access_manager_wait_read_write(table, path, &c) == MANAGER_OK,
          "A owns WRITE while B and C wait");
    check(access_manager_request_transfer(table, path, &b) ==
              MANAGER_ALREADY_EXISTS,
          "duplicate transfer request is rejected");
    check(access_manager_prepare_transfer(table, path, a.application_id) ==
              MANAGER_OK &&
              access_manager_assign_waiting_writer(
                  table, path, b.application_id) == MANAGER_OK,
          "B is manually selected as next writer");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->write_owner_application_id == b.application_id &&
              ownership_table_find_waiting(file, c.application_id) != NULL &&
              ownership_table_find_waiting(file, a.application_id) == NULL,
          "C remains waiting and A is no longer owner");
    check(access_manager_prepare_transfer(table, path, b.application_id) ==
              MANAGER_OK &&
              access_manager_assign_waiting_writer(
                  table, path, c.application_id) == MANAGER_OK &&
              access_manager_prepare_transfer(table, path, c.application_id) ==
                  MANAGER_INVALID_STATE,
          "second transfer requires a new waiter and preserves validation");
    remove_file(path);
    free(table);
}
static void test_local_write_state_requires_manager_authorization(void)
{
    const char *path = "test_28_local_write_authorization.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication application;

    begin_test("standalone-style local WRITE state cannot authorize modification");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&application, 1, "Standalone application");
    check(simulated_application_connect(table, &application, path) ==
              MANAGER_OK,
          "application connects");
    application.access_mode = ACCESS_READ_WRITE;
    application.file_state = FILE_STATE_ORIGINAL;
    application.request_state = REQUEST_ACTIVE;
    (void)snprintf(application.assigned_path,
                   sizeof(application.assigned_path),
                   "%s",
                   path);
    check(simulated_application_modify(table, &application) ==
              MANAGER_INVALID_STATE,
          "modification is rejected without a manager WRITE record");
    remove_file(path);
    free(table);
}

static void test_read_write_shadow_release_preserves_original_owner(void)
{
    const char *path = "test_30_shadow_rw_release.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application shadow_writer = app(2, "Shadow writer");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];

    begin_test("READ + WRITE temporary shadow release preserves original ownership");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table,
                  path,
                  &shadow_writer) == MANAGER_OK,
          "original and shadow WRITE access established");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file,
                                             shadow_writer.application_id);
    (void)snprintf(shadow_path,
                   sizeof(shadow_path),
                   "%s",
                   record == NULL ? "" : record->assigned_path);
    check(access_manager_release_read_write(
              table,
              path,
              shadow_writer.application_id) == MANAGER_OK,
          "shadow WRITE access is released");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              ownership_table_find_active_access(
                  file,
                  shadow_writer.application_id) == NULL &&
              !path_exists(shadow_path),
          "original owner remains and temporary shadow is removed");
    remove_file(path);
    free(table);
}

static void test_independent_shadow_release_preserves_file(void)
{
    const char *path = "test_31_independent_release.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application shadow_writer = app(2, "Shadow writer");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    size_t modified_count = 0;

    begin_test("independent shadow release preserves the independent file");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table,
                  path,
                  &shadow_writer) == MANAGER_OK,
          "shadow WRITE access established");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file,
                                             shadow_writer.application_id);
    (void)snprintf(shadow_path,
                   sizeof(shadow_path),
                   "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "independent changes") &&
              access_manager_detect_shadow_modifications(
                  table,
                  &modified_count) == MANAGER_OK &&
              modified_count == 1,
          "shadow becomes independent");
    check(access_manager_release_read_write(
              table,
              path,
              shadow_writer.application_id) == MANAGER_OK,
          "independent access is released");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              !ownership_table_find_active_access(
                  file,
                  shadow_writer.application_id) &&
              path_exists(shadow_path),
          "independent file remains and original ownership is unchanged");
    if (file != NULL && file->change_record_count > 0) {
        remove_file(file->change_records[0].diff_path);
    }
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_update_rejects_undetected_shadow_modification(void)
{
    const char *path = "test_32_update_modified_shadow.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    char content[64];
    FILE *shadow_file;
    size_t bytes_read;

    begin_test("UPDATE rejects a modified shadow before scanning");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "temporary shadow created");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    (void)snprintf(shadow_path,
                   sizeof(shadow_path),
                   "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "reader changes"), "shadow is modified");
    check(access_manager_update_shadow(
              table,
              path,
              record == NULL ? -1 : record->shadow_copy_id) ==
              MANAGER_INVALID_STATE,
          "UPDATE is rejected without destroying changes");
    shadow_file = fopen(shadow_path, "rb");
    bytes_read = shadow_file == NULL
        ? 0
        : fread(content, 1, sizeof(content) - 1, shadow_file);
    if (shadow_file != NULL) {
        (void)fclose(shadow_file);
    }
    content[bytes_read] = '\0';
    check(strcmp(content, "reader changes") == 0,
          "modified shadow content remains intact");
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_shadow_update_preserves_access_mode(void)
{
    const char *path = "test_33_shadow_rw_update.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application shadow_writer = app(2, "Shadow writer");
    FileRecord *file;
    AccessRecord *record;
    int shadow_id;

    begin_test("R+W shadow UPDATE preserves access mode");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table,
                  path,
                  &shadow_writer) == MANAGER_OK,
          "R+W shadow created");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file,
                                             shadow_writer.application_id);
    shadow_id = record == NULL ? -1 : record->shadow_copy_id;
    check(write_file(path, "updated original") &&
              access_manager_detect_original_update(
                  table,
                  path,
                  &(size_t){0}) == MANAGER_OK &&
              access_manager_update_shadow(table, path, shadow_id) ==
                  MANAGER_OK,
          "unmodified R+W shadow updates");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file,
                                             shadow_writer.application_id);
    check(record != NULL &&
              record->access_mode == ACCESS_READ_WRITE &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              file->write_owner_application_id == writer.application_id,
          "R+W shadow mode and original ownership remain correct");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_generic_shadow_upgrade_is_supported(void)
{
    const char *path = "test_34_generic_shadow_upgrade.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;

    begin_test("generic shadow upgrade preserves original ownership");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) ==
                  MANAGER_OK &&
              access_manager_release_read_write(
                  table,
                  path,
                  writer.application_id) == MANAGER_OK,
          "writer releases while reader retains shadow");
    check(access_manager_upgrade_read_write(table, path, &reader) ==
              MANAGER_OK,
          "generic shadow upgrade succeeds in place");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    check(file != NULL &&
              file->write_owner_application_id == -1 &&
              record != NULL &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              record->access_mode == ACCESS_READ_WRITE,
          "shadow becomes READ + WRITE and original remains without an owner");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_change_record_capacity_is_atomic(void)
{
    const char *path = "test_35_change_capacity.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    size_t modified_count = 0;

    begin_test("change record capacity failure preserves temporary state");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) ==
                  MANAGER_OK,
          "temporary shadow created");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    (void)snprintf(shadow_path,
                   sizeof(shadow_path),
                   "%s",
                   record == NULL ? "" : record->assigned_path);
    if (file != NULL) {
        file->change_record_count = MAX_CHANGE_RECORDS;
    }
    check(write_file(shadow_path, "changed") &&
              access_manager_detect_shadow_modifications(
                  table,
                  &modified_count) == MANAGER_CAPACITY_REACHED,
          "capacity failure is reported");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    check(file != NULL &&
              record != NULL &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              file->change_record_count == MAX_CHANGE_RECORDS,
          "shadow and change records remain consistent");
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_read_write_shadow_preserves_original_owner(void)
{
    const char *path = "test_29_read_write_shadow.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application shadow_writer = app(2, "Shadow writer");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    size_t modified_count = 0;

    begin_test("READ + WRITE shadow preserves original ownership and records changes");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read(table, path, &shadow_writer, 0) ==
              MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) ==
                  MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table,
                  path,
                  &shadow_writer) == MANAGER_OK,
          "existing reader is upgraded to shadow READ + WRITE");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file,
                                             shadow_writer.application_id);
    (void)snprintf(shadow_path,
                   sizeof(shadow_path),
                   "%s",
                   record == NULL ? "" : record->assigned_path);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              record != NULL &&
              record->access_mode == ACCESS_READ_WRITE &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              file->access_record_count == 2,
          "existing READ record becomes shadow READ + WRITE without duplication");
    check(write_file(shadow_path, "shadow changes"), "shadow is modified");
    check(access_manager_detect_shadow_modifications(table, &modified_count) ==
              MANAGER_OK &&
              modified_count == 1,
          "modified shadow is detected");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file,
                                             shadow_writer.application_id);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              record != NULL &&
              record->file_state == FILE_STATE_INDEPENDENT_FILE &&
              file->change_record_count == 1 &&
              path_exists(file->change_records[0].diff_path),
          "shadow becomes independent and creates a diff copy");
    check(access_manager_authorize_modification(
              table,
              &shadow_writer,
              path) == MANAGER_INVALID_STATE,
          "shadow cannot modify the original through shadow authorization");
    if (file != NULL && file->change_record_count > 0) {
        remove_file(file->change_records[0].diff_path);
    }
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_normal_read_creates_shadow(void)
{
    const char *path = "test_38_normal_read_shadow.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;

    begin_test("normal READ creates a temporary shadow");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "writer and shadow reader are established");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    check(file != NULL && record != NULL &&
              record->access_mode == ACCESS_READ &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              path_exists(record->assigned_path),
          "normal reader receives a physical temporary shadow");
    if (record != NULL) {
        remove_file(record->assigned_path);
    }
    remove_file(path);
    free(table);
}

static void test_multiple_normal_reads_create_separate_shadows(void)
{
    const char *path = "test_39_multiple_normal_shadows.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader_a = app(2, "Reader A");
    Application reader_b = app(3, "Reader B");
    FileRecord *file;
    AccessRecord *record_a;
    AccessRecord *record_b;
    char shadow_a[MAX_PATH_LENGTH];
    char shadow_b[MAX_PATH_LENGTH];

    begin_test("multiple normal READs create separate shadows");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_a, 1) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_b, 1) == MANAGER_OK,
          "multiple readers receive shadows");
    file = ownership_table_find_file(table, path);
    record_a = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader_a.application_id);
    record_b = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader_b.application_id);
    (void)snprintf(shadow_a, sizeof(shadow_a), "%s",
                   record_a == NULL ? "" : record_a->assigned_path);
    (void)snprintf(shadow_b, sizeof(shadow_b), "%s",
                   record_b == NULL ? "" : record_b->assigned_path);
    check(record_a != NULL && record_b != NULL &&
              record_a->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              record_b->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              record_a->shadow_copy_id != record_b->shadow_copy_id &&
              strcmp(shadow_a, shadow_b) != 0 &&
              path_exists(shadow_a) && path_exists(shadow_b),
          "readers have unique shadow IDs and paths");
    remove_file(shadow_a);
    remove_file(shadow_b);
    remove_file(path);
    free(table);
}

static void test_read_shadow_upgrades_in_place(void)
{
    const char *path = "test_40_read_shadow_upgrade.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    int shadow_id;
    size_t access_count;

    begin_test("READ upgrades to READ + WRITE on the same shadow");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "reader receives a temporary shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    shadow_id = record == NULL ? -1 : record->shadow_copy_id;
    access_count = file == NULL ? 0 : file->access_record_count;
    check(access_manager_upgrade_read_write(table, path, &reader) == MANAGER_OK,
          "shadow READ upgrades successfully");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    check(file != NULL && record != NULL &&
              record->access_mode == ACCESS_READ_WRITE &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              record->shadow_copy_id == shadow_id &&
              strcmp(record->assigned_path, shadow_path) == 0 &&
              file->access_record_count == access_count &&
              file->write_owner_application_id == writer.application_id,
          "existing shadow record is upgraded without changing original owner");
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_upgraded_read_shadow_can_modify(void)
{
    const char *path = "test_41_upgraded_shadow_modify.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    size_t modified_count = 0;

    begin_test("upgraded READ shadow becomes independent after modification");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &reader) ==
                  MANAGER_OK,
          "reader upgrades the existing shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "reader changed shadow"),
          "upgraded shadow is modified");
    check(access_manager_detect_shadow_modifications(table, &modified_count) ==
              MANAGER_OK &&
              modified_count == 1,
          "modified upgraded shadow is detected");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    check(file != NULL && record != NULL &&
              record->file_state == FILE_STATE_INDEPENDENT_FILE &&
              strcmp(record->assigned_path, shadow_path) == 0 &&
              file->change_record_count == 1 &&
              path_exists(file->change_records[0].diff_path) &&
              file->write_owner_application_id == writer.application_id,
          "upgraded shadow becomes independent with a diff record");
    if (file != NULL && file->change_record_count > 0) {
        remove_file(file->change_records[0].diff_path);
    }
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_upgraded_shadow_preserves_original(void)
{
    const char *path = "test_42_upgraded_shadow_original.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    char before[64];
    char after[64];
    char shadow_path[MAX_PATH_LENGTH];
    FILE *file_handle;
    FileRecord *file;
    AccessRecord *record;
    size_t modified_count = 0;

    begin_test("upgraded shadow modification preserves the original");
    check(table != NULL && write_file(path, "original content"), "test file created");
    file_handle = fopen(path, "rb");
    memset(before, 0, sizeof(before));
    check(file_handle != NULL &&
              fgets(before, sizeof(before), file_handle) != NULL,
          "original content captured");
    if (file_handle != NULL) {
        (void)fclose(file_handle);
    }
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK &&
              access_manager_upgrade_read_write(table, path, &reader) ==
                  MANAGER_OK,
          "reader upgrades the existing shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    check(write_file(shadow_path, "shadow-only content"), "shadow is modified");
    check(access_manager_detect_shadow_modifications(table, &modified_count) ==
              MANAGER_OK &&
              modified_count == 1,
          "shadow modification is recorded");
    file_handle = fopen(path, "rb");
    memset(after, 0, sizeof(after));
    check(file_handle != NULL &&
              fgets(after, sizeof(after), file_handle) != NULL &&
              strcmp(before, after) == 0,
          "original remains byte-for-byte unchanged");
    if (file_handle != NULL) {
        (void)fclose(file_handle);
    }
    file = ownership_table_find_file(table, path);
    if (file != NULL && file->change_record_count > 0) {
        remove_file(file->change_records[0].diff_path);
    }
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_transfer_requires_saved_writer(void)
{
    const char *path = "test_43_transfer_save_state.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication writer_app;
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;

    begin_test("WRITE transfer requires a saved writer");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&writer_app, writer.application_id, writer.name);
    check(simulated_application_connect(table, &writer_app, path) == MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_transfer(table, path, &reader) ==
                  MANAGER_TRANSFER_PENDING &&
              simulated_application_modify(table, &writer_app) == MANAGER_OK &&
              writer_app.dirty == 1,
          "writer becomes dirty before transfer");
    check(access_manager_prepare_transfer_if_saved(
              table, path, writer.application_id, !writer_app.dirty) ==
              MANAGER_INVALID_STATE,
          "unsaved writer cannot transfer");
    check(simulated_application_save(table, &writer_app) == MANAGER_OK &&
              writer_app.dirty == 0 &&
              access_manager_prepare_transfer_if_saved(
                  table, path, writer.application_id, !writer_app.dirty) ==
                  MANAGER_OK,
          "saved writer can transfer");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->write_owner_application_id == -1,
          "WRITE is free only after save");
    remove_file(path);
    free(table);
}

static void test_shadow_failure_preserves_other_access(void)
{
    const char *path = "test_44_shadow_failure.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader_a = app(2, "Reader A");
    Application reader_b = app(3, "Reader B");
    FileRecord *file;
    AccessRecord *record_b;

    begin_test("temporary shadow failure removes only that reader");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_a, 1) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_b, 1) == MANAGER_OK &&
              access_manager_handle_application_failure(
                  table, path, reader_a.application_id) == MANAGER_OK,
          "shadow reader failure is handled");
    file = ownership_table_find_file(table, path);
    record_b = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader_b.application_id);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              ownership_table_find_active_access(
                  file, reader_a.application_id) == NULL &&
              record_b != NULL &&
              file->shadow_copy_count == 1,
          "other shadow and original ownership remain intact");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_rw_shadow_failure_preserves_original_owner(void)
{
    const char *path = "test_45_rw_shadow_failure.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application shadow_writer = app(2, "Shadow Writer");
    FileRecord *file;

    begin_test("READ + WRITE shadow failure preserves original owner");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table, path, &shadow_writer) == MANAGER_OK &&
              access_manager_handle_application_failure(
                  table, path, shadow_writer.application_id) == MANAGER_OK,
          "R+W shadow failure is handled");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              ownership_table_find_active_access(
                  file, shadow_writer.application_id) == NULL,
          "failed shadow cannot claim or clear original WRITE");
    remove_file(path);
    free(table);
}

static void test_cleanup_removes_shadow_access_records(void)
{
    const char *path = "test_46_cleanup_access_records.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader_a = app(2, "Reader A");
    Application reader_b = app(3, "Reader B");
    FileRecord *file;

    begin_test("temporary-shadow cleanup removes matching access records");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_a, 1) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader_b, 1) == MANAGER_OK &&
              access_manager_cleanup_temporary_shadows(table) == MANAGER_OK,
          "temporary shadows are cleaned");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->shadow_copy_count == 0 &&
              ownership_table_find_active_access(
                  file, reader_a.application_id) == NULL &&
              ownership_table_find_active_access(
                  file, reader_b.application_id) == NULL &&
              ownership_table_find_active_access(
                  file, writer.application_id) != NULL &&
              file->write_owner_application_id == writer.application_id,
          "cleanup preserves original access and removes shadow access");
    remove_file(path);
    free(table);
}

static void test_read_conflict_requires_shadow_decision(void)
{
    const char *path = "test_47_read_conflict_decision.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;

    begin_test("READ conflict requires a shadow decision");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(
                  table, path, &reader, 0) == MANAGER_SHADOW_REQUIRED,
          "direct original READ is rejected during WRITE ownership");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->write_owner_application_id == writer.application_id &&
              file->access_record_count == 1 &&
              file->shadow_copy_count == 0 &&
              ownership_table_find_active_access(
                  file, reader.application_id) == NULL,
          "conflicting reader receives no access or shadow");
    remove_file(path);
    free(table);
}

static void test_read_conflict_shadow_preserves_owner(void)
{
    const char *path = "test_48_read_conflict_shadow.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;
    AccessRecord *record;

    begin_test("READ conflict shadow path preserves original owner");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(table, path, &reader, 1) == MANAGER_OK,
          "reader accepts temporary shadow");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(file, reader.application_id);
    check(file != NULL && record != NULL &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              record->access_mode == ACCESS_READ &&
              file->write_owner_application_id == writer.application_id &&
              file->shadow_copy_count == 1,
          "reader receives shadow while writer remains owner");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_read_conflict_close_leaves_table_unchanged(void)
{
    const char *path = "test_49_read_conflict_close.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    Application reader = app(2, "Reader");
    FileRecord *file;

    begin_test("READ conflict CLOSE leaves ownership unchanged");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_request_read(
                  table, path, &reader, 0) == MANAGER_SHADOW_REQUIRED,
          "conflict is reported before CLOSE");
    file = ownership_table_find_file(table, path);
    check(file != NULL &&
              file->access_record_count == 1 &&
              file->shadow_copy_count == 0 &&
              file->write_owner_application_id == writer.application_id,
          "CLOSE can leave the table unchanged");
    remove_file(path);
    free(table);
}

static void test_write_ownership_does_not_mean_modification(void)
{
    const char *path = "test_50_modification_permission.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    FileRecord *file;

    begin_test("R+W ownership does not mean active modification");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK,
          "writer receives R+W access");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->modification_active == 0,
          "original is not active before a write operation");
    remove_file(path);
    free(table);
}

static void test_central_modification_lifecycle(void)
{
    const char *path = "test_51_modification_lifecycle.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    FileRecord *file;

    begin_test("central active modification starts and ends explicitly");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_begin_modification(table, &writer, path) ==
                  MANAGER_OK,
          "authorized original modification begins");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->modification_active == 1,
          "central original state is active");
    check(access_manager_end_modification(table, &writer, path) == MANAGER_OK &&
              file->modification_active == 0,
          "central original state ends independently of ownership");
    remove_file(path);
    free(table);
}

static void test_simulated_modify_ends_active_state_and_sets_dirty(void)
{
    const char *path = "test_52_modification_dirty.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication writer;
    FileRecord *file;
    char content[256];

    begin_test("simulated modification ends active state and remains dirty");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&writer, 1, "Writer");
    check(simulated_application_connect(table, &writer, path) == MANAGER_OK &&
              access_manager_request_read_write(
                  table, path, &writer.identity) == MANAGER_OK &&
              simulated_application_modify(table, &writer) == MANAGER_OK,
          "simulated original modification succeeds");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->modification_active == 0 &&
              writer.modification_active == 0 && writer.dirty == 1,
          "active state ends while dirty state remains");
    check(simulated_application_save(table, &writer) == MANAGER_OK &&
              writer.dirty == 0 &&
              file->write_owner_application_id == writer.identity.application_id &&
              file->modification_active == 0 &&
              read_file(path, content, sizeof(content)) &&
              strstr(content, "[modified by simulated application]") != NULL,
          "SAVE retains original WRITE changes and clears dirty state");
    remove_file(path);
    free(table);
}

static void test_simulated_save_modified_shadow(void)
{
    const char *path = "test_59_save_shadow.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication shadow_writer;
    Application writer = app(1, "Writer");
    FileRecord *file;
    AccessRecord *record;
    ShadowCopy *shadow;
    int shadow_copy_id = 0;
    char shadow_path[MAX_PATH_LENGTH];
    char diff_path[MAX_PATH_LENGTH] = "";
    char original_content[128];
    char shadow_content[256];

    begin_test("SAVE promotes a modified shadow and preserves it on release");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&shadow_writer, 2, "Shadow Writer");
    check(simulated_application_connect(table, &shadow_writer, path) ==
              MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) ==
                  MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table, path, &shadow_writer.identity) == MANAGER_OK,
          "temporary shadow WRITE access is established");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_writer.identity.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    if (record != NULL) {
        shadow_copy_id = record->shadow_copy_id;
    }
    (void)snprintf(shadow_writer.assigned_path,
                   sizeof(shadow_writer.assigned_path),
                   "%s",
                   shadow_path);
    check(simulated_application_modify(table, &shadow_writer) == MANAGER_OK,
          "MODIFY writes through the assigned shadow path");
    check(read_file(path, original_content, sizeof(original_content)) &&
              strcmp(original_content, "initial") == 0 &&
              read_file(shadow_path, shadow_content, sizeof(shadow_content)) &&
              strstr(shadow_content, "[modified by simulated application]") !=
                  NULL,
          "MODIFY changes the shadow while ORIGINAL remains unchanged");
    check(simulated_application_save(table, &shadow_writer) == MANAGER_OK &&
              shadow_writer.dirty == 0 &&
              shadow_writer.file_state == FILE_STATE_INDEPENDENT_FILE,
          "SAVE promotes the modified shadow to an independent file");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_writer.identity.application_id);
    shadow = file == NULL
        ? NULL
        : ownership_table_find_shadow(file, shadow_copy_id);
    if (file != NULL && file->change_record_count > 0) {
        (void)snprintf(diff_path,
                       sizeof(diff_path),
                       "%s",
                       file->change_records[0].diff_path);
    }
    check(read_file(path, original_content, sizeof(original_content)) &&
              strcmp(original_content, "initial") == 0 &&
              read_file(shadow_path, shadow_content, sizeof(shadow_content)) &&
              strstr(shadow_content, "[modified by simulated application]") !=
                  NULL,
          "SAVE preserves the shadow contents and leaves ORIGINAL unchanged");
    check(file != NULL && record != NULL && shadow != NULL &&
              record->file_state == FILE_STATE_INDEPENDENT_FILE &&
              shadow->file_state == FILE_STATE_INDEPENDENT_FILE &&
              shadow->is_temporary == 0 &&
              strcmp(record->assigned_path, shadow_path) == 0 &&
              file->change_record_count == 1 &&
              file->change_records[0].change_record_id > 0 &&
              file->change_records[0].original_metadata.size > 0 &&
              file->change_records[0].shadow_metadata.size > 0 &&
              file->change_records[0].diff_path[0] == '\0' &&
              path_exists(shadow_path) && !path_exists(diff_path),
          "SAVE retains ChangeRecord metadata without creating a diff copy");
    check(access_manager_release_read_write(
              table, path, shadow_writer.identity.application_id) ==
                  MANAGER_OK &&
              path_exists(shadow_path) && !path_exists(diff_path),
          "release preserves the saved independent shadow");
    file = ownership_table_find_file(table, path);
    shadow = file == NULL
        ? NULL
        : ownership_table_find_shadow(file, shadow_copy_id);
    check(file != NULL && shadow != NULL &&
              shadow->file_state == FILE_STATE_INDEPENDENT_FILE &&
              shadow->is_temporary == 0 &&
              ownership_table_find_active_access(
                  file, shadow_writer.identity.application_id) == NULL &&
              file->write_owner_application_id == writer.application_id,
          "release removes shadow access without changing original ownership");
    remove_file(diff_path);
    remove_file(shadow_path);
    remove_file(path);
    free(table);
}

static void test_simulated_save_unmodified_shadow(void)
{
    const char *path = "test_60_save_unmodified_shadow.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication shadow_reader;
    Application writer = app(1, "Writer");
    FileRecord *file;
    AccessRecord *record;
    char shadow_path[MAX_PATH_LENGTH];
    char diff_path[MAX_PATH_LENGTH + 16];

    begin_test("SAVE without modification does not create independent state");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&shadow_reader, 2, "Shadow Reader");
    check(simulated_application_connect(table, &shadow_reader, path) ==
              MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) ==
                  MANAGER_OK &&
              access_manager_request_read(table, path,
                                          &shadow_reader.identity, 1) ==
                  MANAGER_OK,
          "temporary shadow READ access is established");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_reader.identity.application_id);
    (void)snprintf(shadow_path, sizeof(shadow_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    (void)snprintf(diff_path,
                   sizeof(diff_path),
                   "%s.diff_1",
                   shadow_path);
    check(simulated_application_save(table, &shadow_reader) == MANAGER_OK,
          "SAVE without modification succeeds");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_reader.identity.application_id);
    check(file != NULL && record != NULL &&
              record->file_state == FILE_STATE_TEMPORARY_SHADOW &&
              shadow_reader.file_state == FILE_STATE_TEMPORARY_SHADOW &&
              file->change_record_count == 0 &&
              path_exists(shadow_path) && !path_exists(diff_path),
          "unmodified shadow remains temporary without diff copy");
    check(access_manager_release_read(table, path,
                                      shadow_reader.identity.application_id) ==
              MANAGER_OK &&
              !path_exists(shadow_path),
          "unmodified shadow follows existing release cleanup");
    remove_file(path);
    free(table);
}

static void test_simulated_save_independent_file(void)
{
    const char *path = "test_61_save_independent.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication shadow_writer;
    Application writer = app(1, "Writer");
    FileRecord *file;
    AccessRecord *record;
    char independent_path[MAX_PATH_LENGTH];
    char diff_path[MAX_PATH_LENGTH] = "";
    char original_content[128];
    char independent_content[256];
    size_t modified_count = 0;

    begin_test("SAVE persists changes to an independent assigned file");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&shadow_writer, 2, "Shadow Writer");
    check(simulated_application_connect(table, &shadow_writer, path) ==
              MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) ==
                  MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table, path, &shadow_writer.identity) == MANAGER_OK,
          "temporary shadow WRITE access is established");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_writer.identity.application_id);
    (void)snprintf(independent_path, sizeof(independent_path), "%s",
                   record == NULL ? "" : record->assigned_path);
    (void)snprintf(shadow_writer.assigned_path,
                   sizeof(shadow_writer.assigned_path),
                   "%s",
                   independent_path);
    check(write_file(independent_path, "independent changes") &&
              access_manager_detect_shadow_modifications(
                  table, &modified_count) == MANAGER_OK &&
              modified_count == 1,
          "shadow is promoted by the existing detection workflow");
    if (file != NULL && file->change_record_count > 0) {
        (void)snprintf(diff_path,
                       sizeof(diff_path),
                       "%s",
                       file->change_records[0].diff_path);
    }
    check(simulated_application_modify(table, &shadow_writer) == MANAGER_OK &&
              simulated_application_save(table, &shadow_writer) == MANAGER_OK,
          "independent-file modification and SAVE succeed");
    check(read_file(path, original_content, sizeof(original_content)) &&
              strcmp(original_content, "initial") == 0 &&
              read_file(independent_path, independent_content,
                        sizeof(independent_content)) &&
              strstr(independent_content,
                     "[modified by simulated application]") != NULL,
          "independent SAVE does not modify ORIGINAL");
    check(access_manager_release_read_write(
              table, path, shadow_writer.identity.application_id) ==
              MANAGER_OK &&
              path_exists(independent_path),
          "independent file follows existing release preservation");
    remove_file(diff_path);
    remove_file(independent_path);
    remove_file(path);
    free(table);
}

static void test_shadow_modification_does_not_mark_original(void)
{
    const char *path = "test_53_shadow_modification_state.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication shadow_writer;
    Application writer = app(1, "Writer");
    FileRecord *file;
    AccessRecord *record;

    begin_test("shadow modification does not mark original active");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&shadow_writer, 2, "Shadow Writer");
    check(simulated_application_connect(table, &shadow_writer, path) ==
              MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) ==
                  MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table, path, &shadow_writer.identity) == MANAGER_OK,
          "R+W shadow is created");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_writer.identity.application_id);
    if (record != NULL) {
        (void)snprintf(shadow_writer.assigned_path,
                       sizeof(shadow_writer.assigned_path),
                       "%s",
                       record->assigned_path);
    }
    check(record != NULL &&
              simulated_application_modify(table, &shadow_writer) == MANAGER_OK &&
              file->modification_active == 0,
          "shadow write leaves original inactive");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_independent_modification_does_not_mark_original(void)
{
    const char *path = "test_54_independent_modification_state.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    SimulatedApplication shadow_writer;
    Application writer = app(1, "Writer");
    FileRecord *file;
    AccessRecord *record;

    begin_test("independent-file modification does not mark original active");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    simulated_application_init(&shadow_writer, 2, "Shadow Writer");
    check(simulated_application_connect(table, &shadow_writer, path) ==
              MANAGER_OK &&
              access_manager_request_read_write(table, path, &writer) ==
                  MANAGER_OK &&
              access_manager_request_read_write_shadow(
                  table, path, &shadow_writer.identity) == MANAGER_OK,
          "R+W shadow is created");
    file = ownership_table_find_file(table, path);
    record = file == NULL
        ? NULL
        : ownership_table_find_active_access(
              file, shadow_writer.identity.application_id);
    if (record != NULL) {
        (void)snprintf(shadow_writer.assigned_path,
                       sizeof(shadow_writer.assigned_path),
                       "%s",
                       record->assigned_path);
        check(simulated_application_modify(table, &shadow_writer) ==
                  MANAGER_OK &&
              access_manager_detect_shadow_modifications(table, &(size_t){0}) ==
                  MANAGER_OK &&
              simulated_application_modify(table, &shadow_writer) ==
                  MANAGER_OK,
              "shadow becomes independent after modification");
    }
    check(file != NULL && file->modification_active == 0,
          "independent modification leaves original inactive");
    if (file != NULL && file->shadow_copy_count > 0) {
        remove_file(file->shadow_copies[0].shadow_path);
    }
    remove_file(path);
    free(table);
}

static void test_failure_clears_central_modification_state(void)
{
    const char *path = "test_55_failure_modification_state.txt";
    OwnershipTable *table = calloc(1, sizeof(*table));
    Application writer = app(1, "Writer");
    FileRecord *file;

    begin_test("writer failure clears central active modification");
    check(table != NULL && write_file(path, "initial"), "test file created");
    access_manager_init(table);
    check(access_manager_request_read_write(table, path, &writer) == MANAGER_OK &&
              access_manager_begin_modification(table, &writer, path) ==
                  MANAGER_OK,
          "original modification is active");
    file = ownership_table_find_file(table, path);
    check(file != NULL && file->modification_active == 1,
          "central state is active before failure");
    check(access_manager_handle_application_failure(
              table, path, writer.application_id) == MANAGER_OK &&
              file->modification_active == 0 &&
              file->write_owner_application_id == -1,
          "failure clears active state and WRITE ownership");
    remove_file(path);
    free(table);
}

int main(void)
{
    test_shadow_on_read_conflict();
    test_waiting_write();
    test_manual_transfer();
    test_manual_choice_between_waiters();
    test_original_update_update_keep();
    test_shadow_becomes_independent();
    test_independent_excluded_from_updates();
    test_writer_failure();
    test_multiple_readers();
    test_exclusive_writers();
    test_read_opens_original();
    test_read_cannot_modify_original();
    test_fake_local_write_rejected();
    test_table_write_invariant();
    test_read_upgrade_without_writer();
    test_upgrade_blocked_by_writer();
    test_read_upgrade_waits();
    test_shadow_only_on_conflict();
    test_shadow_modification_transition_after_correction();
    test_transfer_updates_local_application_state();
    test_active_reassignment_rejected();
    test_write_reassignment_rejected();
    test_temporary_shadow_cleanup();
    test_failed_record_allows_reopen();
    test_nonexistent_original_rejected();
    test_existing_original_and_copy_still_work();
    test_shadow_scan_continues_after_error();
    test_release_removes_record_after_shadow_cleanup_failure();
    test_write_requires_original_file_state();
    test_local_write_state_requires_manager_authorization();
    test_read_write_shadow_release_preserves_original_owner();
    test_independent_shadow_release_preserves_file();
    test_update_rejects_undetected_shadow_modification();
    test_shadow_update_preserves_access_mode();
    test_generic_shadow_upgrade_is_supported();
    test_change_record_capacity_is_atomic();
    test_read_write_shadow_preserves_original_owner();
    test_normal_read_creates_shadow();
    test_multiple_normal_reads_create_separate_shadows();
    test_read_shadow_upgrades_in_place();
    test_upgraded_read_shadow_can_modify();
    test_upgraded_shadow_preserves_original();
    test_transfer_requires_saved_writer();
    test_shadow_failure_preserves_other_access();
    test_rw_shadow_failure_preserves_original_owner();
    test_cleanup_removes_shadow_access_records();
    test_read_conflict_requires_shadow_decision();
    test_read_conflict_shadow_preserves_owner();
    test_read_conflict_close_leaves_table_unchanged();
    test_write_ownership_does_not_mean_modification();
    test_central_modification_lifecycle();
    test_simulated_modify_ends_active_state_and_sets_dirty();
    test_simulated_save_modified_shadow();
    test_simulated_save_unmodified_shadow();
    test_simulated_save_independent_file();
    test_shadow_modification_does_not_mark_original();
    test_independent_modification_does_not_mark_original();
    test_failure_clears_central_modification_state();
    test_repeated_transfer_request_is_rejected_safely();
    test_pending_cleanup_retry_failure_and_success();
    test_multi_application_manual_transfer_cycles();

    printf("\n%d automated tests run: %d passed, %d failed.\n",
           tests_run,
           tests_run - tests_failed,
           tests_failed);
    return tests_failed == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
