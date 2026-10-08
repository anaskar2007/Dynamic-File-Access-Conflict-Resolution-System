# User-Controlled Concurrent File Access and Shadow Copy Manager

## Integrated prototype

The integrated prototype combines the central in-memory Access Manager with
controlled simulated application identities in one interactive C console
process. It demonstrates:

- Multiple simultaneous `READ` records for one original file
- A single `READ + WRITE` owner
- Rejection of conflicting `READ + WRITE` requests
- Explicitly queued waiting `READ + WRITE` requests
- Unique temporary shadow-copy creation when a writer is active
- Separate shadow copies for multiple readers
- Explicit `READ + WRITE` access to a conflict shadow without changing the
  original WRITE owner
- Shadow-copy metadata and isolated file paths
- Temporary shadow files are removed when their reader releases access.
If the operating system temporarily refuses deletion, active metadata is
removed, the path is recorded as cleanup-pending, and a later retry can
remove the physical artifact.
- Prototype-level shadow modification detection using file size and content hash
- Modified shadows transition to `INDEPENDENT_FILE`
- Original-file update detection with per-reader `UPDATE` / `KEEP` decisions
- Manual WRITE conflict handling with `WAIT` and `REQUEST TRANSFER`
- Save-before-transfer workflow and manual next-owner selection
- Normal writer release with save confirmation
- Simulated unexpected writer termination and FAILED state handling
- Release of `READ` access
- Release of `READ + WRITE` ownership
- Status output for files, readers, waiting requests, and shadow-copy records
- Simulated application identities, names, and Windows PIDs

IPC, process monitoring, and GUI behavior are intentionally deferred to later
phases. A shadow copy is a one-time binary copy until the reader selects
`UPDATE` after an original-file version change.
Use menu option `10` to scan temporary shadows. The scan compares each shadow
against its creation metadata; it does not use a dirty bit or kernel
filesystem monitoring. Once changed, the shadow remains an independent file
and is excluded from future automatic synchronization.
Use menu option `9` to detect a changed original. Temporary shadows receive
the `UPDATE` / `KEEP` decision. `UPDATE` replaces the shadow with a new copy
of the latest original; `KEEP` leaves the existing shadow unchanged.

## Build with MinGW-w64 GCC

From the project root:

```powershell
gcc -std=c17 -Wall -Wextra -Wpedantic -g `
    src\main.c `
    src\access_manager.c `
    src\ownership_table.c `
    src\shadow_copy.c `
    src\simulated_application.c `
    -o file_access_manager.exe
```

This command generates `file_access_manager.exe` in the project root. Run it
from PowerShell with `.\file_access_manager.exe`.

The standalone simulated-application console remains optional:

```powershell
gcc -std=c17 -Wall -Wextra -Wpedantic -g `
    src\main_application.c `
    src\simulated_application.c `
    src\access_manager.c `
    src\ownership_table.c `
    src\shadow_copy.c `
    -o simulated_application.exe
```

This command generates `simulated_application.exe` in the project root.

The integrated menu is:

1. Launch Application
2. Open File
3. Request READ
4. Request READ + WRITE
5. Modify File
6. Save File
7. Release Access
8. View Ownership Table
9. Check Original Updates
10. Check Shadow Copies
11. Transfer WRITE Ownership
12. Simulate Application Failure
13. Exit

The manager remains the authority for all ownership decisions. Waiting
applications are never promoted automatically: normal release, transfer, and
failure recovery display waiting requests and require manual selection.
Failure is explicitly simulated through the menu; real process-handle
monitoring and named-pipe IPC are future extensions.

Ownership-table records are private implementation types. Public callers use
the Access Manager and ownership-table query/mutation APIs; the internal
record definitions are kept in `src\ownership_table_internal.h`.

## Testing

The complete test plan, automated C regression harness, expected results, and
manual UI procedures are documented in [TEST_PLAN.md](./TEST_PLAN.md).

Build the automated manager tests with:

```powershell
gcc -std=c17 -Wall -Wextra -Wpedantic -g `
    tests\test_access_manager.c `
    src\access_manager.c `
    src\ownership_table.c `
    src\simulated_application.c `
    src\shadow_copy.c `
    -o test_access_manager.exe
.\test_access_manager.exe
```

This command generates `test_access_manager.exe` in the project root.
The automated suite covers the original access workflows, ownership
invariants, shadow lifecycle, and READ + WRITE conflict-shadow behavior.
The test source currently registers 61 cases. `MODIFY` writes to the
application's assigned path. `SAVE` clears the dirty state; for a modified
temporary shadow it also records a `ChangeRecord` and promotes the shadow to
`FILE_STATE_INDEPENDENT_FILE` without creating a physical diff copy or
modifying the original. An unchanged temporary shadow remains temporary and
is deleted by normal release cleanup. `SAVE` on the original or an already
independent file clears the dirty state without changing ownership.

### Sample test input

[`tests/test.txt`](./tests/test.txt) is an included sample/test input file.
Use its path when testing the system, or provide a path to your own test file
instead.

Menu option 9, **Check Original Updates**, detects changes to an original file
and offers `UPDATE` or `KEEP` for eligible temporary shadows. `UPDATE`
replaces the unchanged temporary shadow with a fresh copy of the current
original; `KEEP` retains the existing shadow. `UPDATE` rejects a shadow whose
contents have been modified.

Menu option 10, **Check Shadow Copies**, scans temporary shadows for changes.
A detected edit creates a physical `.diff_<id>` copy and a `ChangeRecord`,
then promotes the shadow to `FILE_STATE_INDEPENDENT_FILE`. Independent files
are not temporary shadows and are preserved by temporary-shadow cleanup.

The optional standalone simulator supports a registry of simulated
applications, waiting WRITE requests, manual waiting-writer selection,
release, failure simulation, and state display. It remains a single
controlled process; it does not provide real process IPC.

When no original WRITE owner exists, `READ` may be explicitly granted on the
original or through a temporary shadow. When an original WRITE owner exists,
direct original READ is rejected and the user must choose a temporary shadow
or cancel/move to the READ + WRITE conflict workflow.

## Standalone simulated application (optional)

```powershell
.\simulated_application.exe
```

Choose an identity such as `Word`, `PDF Viewer`, `Text Editor`, or
`Image Processor`. Each process displays its application ID, Windows PID,
selected file, access mode, file state, assigned path, and connection state.
The executable is a controlled application simulation; it does not modify
Microsoft Word or any other third-party application.
