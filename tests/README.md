# Regression checks

From PowerShell in the repository:

```powershell
wsl -d Ubuntu -- python3 tests/test_regressions.py
wsl -d Ubuntu -- bash ./tools/build-psp.sh
```

The host tests compile the production state handlers with mocked PSP APIs.
They exercise unsaved-order exit choices, save failure handling, category
rename/merge results, animation allocation/read failures, and resource paths
after renaming the category that contains the running app. They do not emulate
PSP graphics or controller input.

On a PSP or Vita with CFW, check:

1. Press Triangle with one app selected, then with several apps checked, and
   on the category list. A blocking `Loading...` modal should cover menu
   preparation and disappear when the operations menu is ready.
2. Reorder apps or categories and leave the list or category sort mode. Try
   Save, Discard, and Cancel (including Circle). An unchanged order, an order
   moved back to its original positions, and an explicitly saved order should
   exit without another prompt. Verify a saved order after reopening the app.
3. Install the app inside a category. Save a category order that changes that
   category's numbered prefix, then return home. The animation should load
   from the renamed path without entering USB mode. Also try changing the
   category prefix setting and repeated move/copy/delete operations. If an
   animation load temporarily fails, automatic retries are spaced two seconds
   apart; they do not replace the resource-path fix.

The older SDK currently emits an import-stub ordering warning when linking
the main app. Successful compilation and these host tests do not establish
runtime compatibility on every CFW.
