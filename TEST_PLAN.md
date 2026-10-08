# Test Plan

## Scope

This plan validates the integrated prototype's central Access Manager and its
interactive controller. It covers access arbitration, waiting requests,
manual WRITE transfer, temporary shadows, original updates, independent-file
conversion, failure handling, and concurrent-reader behavior.

The automated tests exercise the manager API directly with real files and
temporary shadow files. The manual tests exercise the same behavior through
`file_access_manager.exe`, including prompts such as `UPDATE`, `KEEP`, `WAIT`,
and manual owner selection.

## Test environment

- Windows with MinGW-w64 GCC
- C17 compilation
- A writable project directory
- No kernel driver, filesystem filter, dirty bit, or inactivity timer

Build and run the automated suite:

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

Expected summary:

```text
61 automated tests run: 61 passed, 0 failed.
```

Build the interactive controller:

```powershell
gcc -std=c17 -Wall -Wextra -Wpedantic -g `
    src\main.c `
    src\access_manager.c `
    src\ownership_table.c `
    src\shadow_copy.c `
    src\simulated_application.c `
    -o file_access_manager.exe
```

## Automated test cases

The test source currently registers 61 cases. The first 10 cases are detailed
below; the remaining cases follow the execution order in `main()` in
`tests\test_access_manager.c`:

11. Direct READ opens the original.
12. A normal READ application cannot modify the original.
13. Locally asserted WRITE state without manager authorization cannot modify
    the original.
14. The ownership table enforces its WRITE-owner invariant.
15. READ upgrades to READ + WRITE when available.
16. READ upgrade is blocked by an existing writer.
17. READ upgrade can enter WAITING.
18. Shadow copies are created only for workflows that require them.
19. A modified temporary shadow becomes independent after detection.
20. WRITE transfer synchronizes old and new applications' local state.
21. Active READ reassignment is rejected.
22. Active READ + WRITE reassignment is rejected.
23. Temporary-shadow cleanup removes temporary shadows and preserves
    independent files.
24. Failed access records do not block reopening.
25. Nonexistent originals are rejected.
26. Existing originals remain accessible and shadow copying succeeds.
27. Shadow scanning continues after an unreadable shadow and reports the
    failure.
28. Release removes the access record even if shadow deletion fails.
29. WRITE access requires ORIGINAL file state for original ownership.
30. Local WRITE state requires manager authorization for modification.
31. Releasing a READ + WRITE shadow preserves the original WRITE owner.
32. Releasing an independent shadow preserves its file.
33. UPDATE rejects an undetected modified shadow.
34. UPDATE preserves READ + WRITE shadow access mode.
35. Generic shadow upgrades do not claim original WRITE ownership.
36. Change-record capacity failure leaves the shadow temporary.
37. READ + WRITE shadow change tracking preserves original ownership.
38. Normal READ creates a temporary shadow.
39. Multiple normal READ requests receive separate shadows.
40. READ upgrades to READ + WRITE on the same shadow.
41. An upgraded READ shadow can be modified and detected as independent.
42. An upgraded shadow modification leaves the original unchanged.
43. WRITE transfer requires the current writer to save.
44. Temporary-shadow failure removes only the failed reader.
45. READ + WRITE shadow failure preserves original WRITE ownership.
46. Temporary-shadow cleanup removes related access records.
47. READ conflict requires a shadow decision.
48. Conflict-shadow creation preserves original WRITE ownership.
49. Closing a READ conflict without a shadow leaves the table unchanged.
50. WRITE ownership does not itself mean a modification is active.
51. Central active modification starts and ends explicitly.
52. Simulated modification ends active state and leaves the application dirty.
53. SAVE on a modified temporary shadow creates a `ChangeRecord`, promotes it
    to `INDEPENDENT_FILE`, and leaves ORIGINAL unchanged; SAVE does not create
    a physical diff copy.
54. SAVE without modification leaves the shadow temporary, creates no change
    record or diff copy, and normal release removes it.
55. SAVE on an independent assigned file writes there without modifying
    ORIGINAL; release preserves the independent file.
56. Shadow modification does not mark the original active.
57. Independent-file modification does not mark the original active.
58. Writer failure clears central active-modification state.
59. Repeated WRITE-transfer requests do not duplicate waiting state.
60. Pending-shadow cleanup retry failure and success are handled.
61. Multiple simulated applications complete manual WRITE-transfer cycles.

The tests include the private ownership-table header to assert internal
invariants; application/UI code uses manager and query APIs for state changes.

### TEST 1 - READ conflict creates a temporary shadow

- **Initial state:** Original test file exists. The ownership table is empty.
- **Operation:** A requests `READ + WRITE`. B requests `READ` without
  accepting a shadow, then repeats the request accepting a shadow.
- **Expected state:** The first B request returns `MANAGER_SHADOW_REQUIRED`.
  The accepted request creates one unique shadow. B has `READ`,
  `TEMPORARY_SHADOW`, and a valid shadow-copy ID; A remains the sole WRITE
  owner.
- **Actual result:** `PASS` - automated test confirmed the conflict response,
  shadow record, temporary state, and unique shadow ID.
- **Pass/fail:** **PASS**

### TEST 2 - Conflicting WRITE request waits

- **Initial state:** A owns `READ + WRITE` for the original.
- **Operation:** B requests `READ + WRITE` and chooses `WAIT`.
- **Expected state:** B is in the waiting-request table with requested mode
  `READ + WRITE` and state `WAITING`. A remains WRITE owner. B is not
  automatically promoted.
- **Actual result:** `PASS` - one waiting request was present with
  `REQUEST_WAITING`; the owner was unchanged.
- **Pass/fail:** **PASS**

### TEST 3 - Release followed by manual selection

- **Initial state:** A owns WRITE and B is waiting for WRITE.
- **Operation:** A releases WRITE. The user explicitly selects B.
- **Expected state:** WRITE is temporarily unowned after release. After
  selection, B is the sole WRITE owner.
- **Actual result:** `PASS` - automated test observed `-1` before selection and
  B's application ID after explicit assignment.
- **Pass/fail:** **PASS**

### TEST 4 - Multiple waiters require manual choice

- **Initial state:** A owns WRITE; B and C are waiting WRITE requests.
- **Operation:** A releases WRITE. The user selects C.
- **Expected state:** No waiter is selected automatically. After selection C
  owns WRITE and B remains in the waiting table.
- **Actual result:** `PASS` - the table retained both waiters while WRITE was
  free, then assigned only C and retained B.
- **Pass/fail:** **PASS**

### TEST 5 - Original update offers UPDATE / KEEP

- **Initial state:** A owns WRITE and B has a temporary shadow at original
  version 1.
- **Operation:** Save a changed version of the original and run original
  update detection. Exercise `UPDATE`; separately repeat the scenario and
  choose `KEEP` in the interactive controller.
- **Expected state:** Detection reports B as pending. `UPDATE` replaces B's
  shadow with a copy of the latest original and keeps B in
  `TEMPORARY_SHADOW`. `KEEP` leaves B's existing shadow and content unchanged.
- **Actual result:** `PASS` - automated test confirmed one pending update and a
  replacement temporary shadow with a new path. The interactive `KEEP` path is
  available through menu option 9 and leaves the record unchanged when chosen.
- **Pass/fail:** **PASS**

### TEST 6 - Shadow modification becomes independent

- **Initial state:** B has a `TEMPORARY_SHADOW` access record and its shadow
  metadata matches creation metadata.
- **Operation:** Modify the shadow file, then run shadow modification
  detection.
- **Expected state:** The shadow and B's access record become
  `INDEPENDENT_FILE`; the temporary flag is false and synchronization is
  disabled.
- **Actual result:** `PASS` - one modified shadow was detected and B's access
  record became `FILE_STATE_INDEPENDENT_FILE`.
- **Pass/fail:** **PASS**

### TEST 7 - Independent file is not synchronized

- **Initial state:** B's former shadow has already become
  `INDEPENDENT_FILE`.
- **Operation:** Save a new version of the original and run original update
  detection.
- **Expected state:** No update notification is pending for B. B remains
  `INDEPENDENT_FILE` and its path/content is not replaced.
- **Actual result:** `PASS` - update detection returned zero pending shadows and
  preserved B's independent state.
- **Pass/fail:** **PASS**

### TEST 8 - Writer failure releases WRITE

- **Initial state:** A owns `READ + WRITE` for the original.
- **Operation:** Simulate A's failure.
- **Expected state:** The WRITE owner is cleared. A's request is `FAILED` and
  cannot retain `READ + WRITE`. Waiting requests, if any, are displayed for
  manual selection.
- **Actual result:** `PASS` - the manager cleared the owner and marked A
  `REQUEST_FAILED` with `ACCESS_READ`.
- **Pass/fail:** **PASS**

### TEST 9 - Multiple readers coexist

- **Initial state:** Original file exists with no WRITE owner.
- **Operation:** A, B, and C each request `READ`.
- **Expected state:** All three access records are active with `READ`; no
  shadow is required and no WRITE owner exists.
- **Actual result:** `PASS` - three active READ records coexisted and the owner
  remained unset.
- **Pass/fail:** **PASS**

### TEST 10 - WRITE ownership is exclusive

- **Initial state:** Original file exists with no access records.
- **Operation:** A and B attempt `READ + WRITE` requests in the same manager
  session. The manager serializes requests in arrival order.
- **Expected state:** Exactly one request succeeds. The other returns
  `MANAGER_WRITE_CONFLICT`; the table has one WRITE owner.
- **Actual result:** `PASS` - A was accepted and B was rejected; the invariant
  remained one WRITE owner.
- **Pass/fail:** **PASS**

## Manual UI test procedures

Use separate application IDs in one controller session. For each procedure,
create a small file such as `manual_original.txt`, launch applications from
menu option 1, and select the file through menu option 2.

### Manual conflict and shadow checks

1. Launch A as `Word`, launch B as `PDF Viewer`.
2. Open the same file for A and choose menu option 4.
3. Choose menu option 3 for B. Confirm the shadow prompt and answer `y`.
4. Choose menu option 8.
5. Verify A is `READ + WRITE`, B is `READ`, and B has a unique
   `TEMPORARY_SHADOW` path.

### Manual WAIT and transfer checks

1. With A as WRITE owner, choose menu option 4 for B.
2. Choose `WAIT` when prompted.
3. Verify option 8 shows B under waiting WRITE requests.
4. Release A through menu option 7 and confirm the save prompt.
5. Select B when the waiting-request list is displayed.
6. Verify B is the only WRITE owner in option 8.

### Manual no-automatic-queue check

1. Repeat the WAIT procedure with B and C waiting.
2. Release A.
3. Verify the manager displays both choices and assigns neither before input.
4. Select either B or C.
5. Verify the other remains waiting.

### Manual UPDATE / KEEP checks

1. Give B a temporary shadow as described above.
2. Modify and save the original using the writer or an external controlled
   file edit.
3. Choose menu option 9.
4. Select `UPDATE` and verify B receives a replacement shadow path.
5. Repeat with a fresh shadow, select `KEEP`, and verify the old shadow path
   and content remain unchanged.

### Manual independent-file checks

1. Give B a temporary shadow.
2. Choose menu option 5 for B to modify the assigned shadow.
3. Choose menu option 6 for B to SAVE. SAVE records a `ChangeRecord` and
   promotes the modified temporary shadow to `INDEPENDENT_FILE`; it does not
   create the physical diff copy made by option 10.
4. Choose menu option 8 and verify B is `INDEPENDENT_FILE`.
5. Change the original and choose menu option 9.
6. Verify B receives no UPDATE / KEEP prompt because it is no longer a
   temporary shadow.

### Manual shadow-scan checks

1. Give B a temporary shadow.
2. Choose menu option 5 for B to modify it, but do not SAVE.
3. Choose menu option 10. The scan detects the modified shadow, creates a
   physical `.diff_<id>` copy and a `ChangeRecord`, and promotes the shadow to
   `INDEPENDENT_FILE`.
4. Choose menu option 8 to verify the state. Exit cleanup removes temporary
   shadows; independent files are retained.

### Manual failure and reader checks

1. Give A WRITE and B a waiting WRITE request.
2. Choose menu option 12 for A.
3. Verify A is marked failed, WRITE is released, and the waiting list is
   displayed for manual selection.
4. In a fresh file session, launch three applications and request READ for all
   three.
5. Verify all three READ records coexist in option 8.

## Invariants checked

- `write_owner_application_id` is either `-1` or identifies exactly one active
  `READ + WRITE` record.
- Multiple `READ` records may be active simultaneously.
- A waiting request is never promoted without explicit user selection.
- Only `TEMPORARY_SHADOW` records participate in original update handling.
- A modified shadow remains an isolated `INDEPENDENT_FILE`.
- A failed writer cannot retain WRITE ownership.

## Cleanup

After each test session, release active applications and remove generated
`*_S_*.ext` shadow files. The automated harness removes its own test files and
shadow files before exiting.
