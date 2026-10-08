#ifndef SIMULATED_APPLICATION_H
#define SIMULATED_APPLICATION_H

#include "models.h"

typedef enum {
    SIMULATED_APP_DISCONNECTED = 1,
    SIMULATED_APP_CONNECTED = 2,
    SIMULATED_APP_RELEASED = 3,
    SIMULATED_APP_CRASHED = 4
} SimulatedApplicationState;

typedef struct {
    Application identity;
    unsigned long process_id;
    char selected_file[MAX_PATH_LENGTH];
    char assigned_path[MAX_PATH_LENGTH];
    AccessMode access_mode;
    FileState file_state;
    RequestState request_state;
    int dirty;
    int modification_active;
    SimulatedApplicationState connection_state;
    int connected_to_manager;
} SimulatedApplication;

void simulated_application_init(SimulatedApplication *application,
                                int application_id,
                                const char *name);

ManagerResult simulated_application_connect(
    const OwnershipTable *table,
    SimulatedApplication *application,
    const char *selected_file);

ManagerResult simulated_application_modify(
    OwnershipTable *table,
    SimulatedApplication *application);

ManagerResult simulated_application_save(
    OwnershipTable *table,
    SimulatedApplication *application);

void simulated_application_release(SimulatedApplication *application);
void simulated_application_crash(SimulatedApplication *application);
void simulated_application_print_status(
    const SimulatedApplication *application);

const char *simulated_application_state_string(
    SimulatedApplicationState state);

#endif
