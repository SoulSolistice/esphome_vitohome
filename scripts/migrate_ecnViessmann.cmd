@echo off
setlocal EnableExtensions DisableDelayedExpansion
REM ============================================================================
REM  migrate_ecnViessmann.cmd
REM  One-time migration: SQL Server .mdf (ecnViessmann) -> SQLite (.db)
REM
REM  Rationale: if you have the Vitosoft XML, you already installed Vitosoft,
REM  which ships a SQL Server instance. So the engine is already here -- use it
REM  once to export the catalog to a portable SQLite that gen_catalog can read
REM  with stdlib sqlite3 (no SQL Server at runtime).
REM
REM  HARDENING vs the original:
REM    * Validation guards wrap the FULL consequence in ( ... ) so a present
REM      file does not still fall through to :fail (the bare "& goto" bug).
REM    * Works on COPIES of the .mdf/.ldf and uses sp_detach_db, never
REM      DROP DATABASE -- so the source files can never be deleted.
REM    * Resolves the instance for a Windows service OR SQL Express LocalDB,
REM      with an override.
REM    * Verifies PER-TABLE row counts (not just a grand total) and verifies
REM      TEXT ENCODING (German/UTF-8 present, no U+FFFD) -- the one failure
REM      mode a row-count check cannot catch.
REM    * Optional catalog-only slimming (OFF by default; opt in deliberately).
REM
REM  PREREQUISITES: sqlcmd, sqlite3, the export tool, PowerShell, and the two
REM  companion scripts verify_counts.ps1 and verify_encoding.ps1 in this folder.
REM  TEST on your machine -- cmd quoting and your export tool's exact behavior
REM  must be confirmed locally.
REM ============================================================================

REM ---- CONFIG ----------------------------------------------------------------
set "DB_NAME=ViessmannMigrateTemp"

REM SOURCE files = your standalone copies. They are COPIED, never attached
REM directly, so the originals are never at risk. Do NOT point these at the
REM live Vitosoft database files (that DB is open; attaching it is unsafe).
set "SRC_MDF=E:\29_Git_Soulsolistice\ecnViessmann.mdf"
set "SRC_LDF=E:\29_Git_Soulsolistice\ecnViessmann.ldf"

REM WORKING copies that actually get attached. Must be readable by the SQL
REM Server service account (see GRANT_SERVICE_READ). Deleted at the end.
set "WORK_DIR=%~dp0work"
set "WORK_MDF=%WORK_DIR%\ecnViessmann.mdf"
set "WORK_LDF=%WORK_DIR%\ecnViessmann.ldf"

REM Tools
set "EXPORT_TOOL=E:\29_Git_Soulsolistice\Export2SqlCE\Export2SqlCE-4.9.68.exe"
set "SQLITE_TOOL=E:\29_Git_Soulsolistice\sqlite\sqlite3.exe"

REM Outputs
set "DUMP_FILE=%~dp0ViessmannDump.sql"
set "SQLITE_DB=%~dp0ecnViessmann.db"
set "LOG_FILE=%~dp0migration.log"

REM Optional: force an instance, e.g. (localdb)\MSSQLLocalDB or .\SQLEXPRESS
set "SQL_INSTANCE_OVERRIDE="

REM Grant the SQL service read access to the working copies so ATTACH works.
REM 1 = grant Everyone:Read on the two work files (a temp copy of public
REM reference data; tighten if your policy requires). LocalDB runs as you, so
REM the grant is skipped automatically there.
set "GRANT_SERVICE_READ=1"

REM Optional catalog-only slimming. OFF by default so nothing needed is lost.
REM Set SUBSET=1 to DROP every table NOT in KEEP_TABLES, then VACUUM. Populate
REM KEEP_TABLES with the EXACT names printed by the row-count step (matching is
REM case-insensitive; names not present are simply ignored).
set "SUBSET=0"
set "KEEP_TABLES=ecnDatapointType ecnDatapointTypelabel ecnEventType ecnEventTypelabel ecnEventValueType ecnUnit ecnDatapointTypeErrorType ecnDataPointTypeEventTypeLink ecnEventTypeEventValueTypeLink ecnEventTypeGroup ecnDataPointTypeEventTypeGroupLink ecnVersion"

REM ---- scratch files ----
set "MSSQL_COUNTS=%TEMP%\vm_mssql_counts_%RANDOM%.csv"
set "SQLITE_COUNTS=%TEMP%\vm_sqlite_counts_%RANDOM%.csv"
set "COUNT_QUERY=%TEMP%\vm_count_query_%RANDOM%.sql"
set "TMP_SQL=%TEMP%\vm_import_%RANDOM%.sql"

call :log "==== Migration start ===="

REM ---- VALIDATION (note the parentheses: the full consequence is guarded) ----
if not exist "%SRC_MDF%" ( call :error "Missing source MDF: %SRC_MDF%" & goto :fail )
if not exist "%SRC_LDF%" ( call :error "Missing source LDF: %SRC_LDF%" & goto :fail )
if not exist "%EXPORT_TOOL%" ( call :error "Missing export tool: %EXPORT_TOOL%" & goto :fail )
if not exist "%~dp0verify_counts.ps1" ( call :error "verify_counts.ps1 not found next to this script" & goto :fail )
if not exist "%~dp0verify_encoding.ps1" ( call :error "verify_encoding.ps1 not found next to this script" & goto :fail )
where sqlcmd >nul 2>&1 || ( call :error "sqlcmd not found on PATH" & goto :fail )
if not exist "%SQLITE_TOOL%" (
    where sqlite3 >nul 2>&1 || ( call :error "sqlite3 not found (set SQLITE_TOOL or add it to PATH)" & goto :fail )
    set "SQLITE_TOOL=sqlite3"
)

REM ---- CLEANUP previous outputs ----
del "%DUMP_FILE%" "%SQLITE_DB%" "%TMP_SQL%" "%COUNT_QUERY%" "%MSSQL_COUNTS%" "%SQLITE_COUNTS%" >nul 2>&1

REM ---- RESOLVE SQL INSTANCE (override -> running DB-engine service -> LocalDB) -
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "$o=$env:SQL_INSTANCE_OVERRIDE; if($o){$o}else{ $r=$null; foreach($x in @(Get-Service -Name 'MSSQL*' -ErrorAction SilentlyContinue)){ if($x.Status -eq 'Running' -and ($x.Name -eq 'MSSQLSERVER' -or $x.Name -like 'MSSQL$*')){ $r=$x.Name; break } }; if($r){ if($r -eq 'MSSQLSERVER'){'.'} else {'.\'+($r -replace '^MSSQL\$','')} } else {'(localdb)\MSSQLLocalDB'} }"`) do set "SQL_INSTANCE=%%I"
if not defined SQL_INSTANCE ( call :error "Could not resolve a SQL Server instance" & goto :fail )
call :log "Using SQL instance: %SQL_INSTANCE%"

set "IS_LOCALDB="
echo %SQL_INSTANCE%| findstr /I "localdb" >nul && set "IS_LOCALDB=1"

REM ---- MAKE WORKING COPIES (originals are never attached) ----
if not exist "%WORK_DIR%" mkdir "%WORK_DIR%"
call :log "Copying source DB to working copies..."
copy /Y "%SRC_MDF%" "%WORK_MDF%" >nul || ( call :error "Failed to copy MDF (is the source locked / open in Vitosoft?)" & goto :fail )
copy /Y "%SRC_LDF%" "%WORK_LDF%" >nul || ( call :error "Failed to copy LDF" & goto :fail )

if "%GRANT_SERVICE_READ%"=="1" if not defined IS_LOCALDB (
    icacls "%WORK_MDF%" /grant *S-1-1-0:R >nul 2>&1
    icacls "%WORK_LDF%" /grant *S-1-1-0:R >nul 2>&1
)

REM ---- DETACH any leftover temp DB (sp_detach_db preserves files; DROP would delete them) -
call :log "Detaching any previous %DB_NAME%..."
sqlcmd -S "%SQL_INSTANCE%" -E -b -Q "IF DB_ID('%DB_NAME%') IS NOT NULL EXEC sp_detach_db '%DB_NAME%';" >nul 2>&1

REM ---- ATTACH the working copies ----
call :log "Attaching working copies as %DB_NAME%..."
sqlcmd -S "%SQL_INSTANCE%" -E -b -Q "CREATE DATABASE [%DB_NAME%] ON (FILENAME=N'%WORK_MDF%'),(FILENAME=N'%WORK_LDF%') FOR ATTACH;"
if errorlevel 1 ( call :error "Attach failed (service account may lack read on %WORK_DIR%; or set SQL_INSTANCE_OVERRIDE)" & goto :fail )

REM ---- SQL SERVER per-table row counts (the verification source of truth) ----
call :log "Counting SQL Server rows per table..."
sqlcmd -S "%SQL_INSTANCE%" -E -b -h -1 -W -s "," -Q "SET NOCOUNT ON; SELECT t.name, SUM(p.rows) FROM sys.tables t JOIN sys.partitions p ON t.object_id=p.object_id WHERE p.index_id IN (0,1) AND t.name <> 'sysdiagrams' GROUP BY t.name ORDER BY t.name;" > "%MSSQL_COUNTS%"
if errorlevel 1 ( call :error "SQL Server row-count query failed" & goto :fail )

REM ---- EXPORT to a SQLite SQL dump ----
REM NOTE: the export tool (ExportSqlCE / Export2SQLCE) is the linchpin. Confirm
REM its exact CLI and that it emits UTF-8 for nvarchar -- the encoding check
REM below is what guards a bad export.
call :log "Exporting to SQLite SQL dump..."
"%EXPORT_TOOL%" "Data Source=%SQL_INSTANCE%;Initial Catalog=%DB_NAME%;Integrated Security=True" "%DUMP_FILE%" sqlite preservedateanddatetime2
if errorlevel 1 ( call :error "Export tool failed" & goto :fail )

REM ---- VALIDATE dump ----
if not exist "%DUMP_FILE%" ( call :error "Dump not created" & goto :fail )
for %%A in ("%DUMP_FILE%") do if %%~zA EQU 0 ( call :error "Dump is empty" & goto :fail )
findstr /I /C:"CREATE TABLE" "%DUMP_FILE%" >nul || ( call :error "Dump contains no CREATE TABLE" & goto :fail )

REM ---- DETACH + remove working copies (we have the dump now) ----
call :cleanup

REM ---- IMPORT into SQLite (wrap in a transaction only if the dump doesn't) ----
set "WRAP=1"
findstr /R /I /C:"BEGIN TRANSACTION" "%DUMP_FILE%" >nul && set "WRAP=0"
> "%TMP_SQL%" (
    echo PRAGMA synchronous=OFF;
    echo PRAGMA journal_mode=MEMORY;
    if "%WRAP%"=="1" echo BEGIN TRANSACTION;
    echo .read "%DUMP_FILE:\=/%"
    if "%WRAP%"=="1" echo COMMIT;
)
call :log "Importing into SQLite..."
"%SQLITE_TOOL%" "%SQLITE_DB%" < "%TMP_SQL%"
if errorlevel 1 ( call :error "SQLite import failed" & goto :fail )

REM ---- SQLite per-table row counts ----
call :log "Counting SQLite rows per table..."
"%SQLITE_TOOL%" -noheader "%SQLITE_DB%" "SELECT group_concat('SELECT '''||name||''' AS tbl, COUNT(*) AS n FROM [' || name || ']', ' UNION ALL ') || ';' FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%%' AND name<>'sysdiagrams';" > "%COUNT_QUERY%"
"%SQLITE_TOOL%" -csv "%SQLITE_DB%" < "%COUNT_QUERY%" > "%SQLITE_COUNTS%"

REM ---- VERIFY: per-table count diff (robust, in PowerShell) ----
call :log "Verifying row counts (per table)..."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify_counts.ps1" -Mssql "%MSSQL_COUNTS%" -Sqlite "%SQLITE_COUNTS%"
if errorlevel 1 ( call :error "Row-count verification failed (see table list above)" & goto :fail )

REM ---- VERIFY: text encoding (UTF-8 German/degree present, no U+FFFD) ----
call :log "Verifying text encoding..."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0verify_encoding.ps1" -Db "%SQLITE_DB%"
if errorlevel 1 ( call :error "Encoding verification failed (umlauts/degree sign were transcoded lossily)" & goto :fail )

REM ---- OPTIONAL: slim to catalog tables only ----
if "%SUBSET%"=="1" call :subset

REM ---- FINAL size sanity ----
for %%A in ("%SQLITE_DB%") do set "DBSIZE=%%~zA"
if not defined DBSIZE ( call :error "SQLite DB missing" & goto :fail )
if %DBSIZE% LSS 1000 ( call :error "SQLite DB suspiciously small (%DBSIZE% bytes)" & goto :fail )

call :log "SUCCESS -> %SQLITE_DB% (%DBSIZE% bytes)"
echo.
echo  VERIFIED SUCCESS: %SQLITE_DB%
goto :end

REM ============================================================================
REM  FUNCTIONS
REM ============================================================================
:cleanup
REM Detach (never drop) and remove the working copies. Safe to call repeatedly.
sqlcmd -S "%SQL_INSTANCE%" -E -Q "IF DB_ID('%DB_NAME%') IS NOT NULL EXEC sp_detach_db '%DB_NAME%';" >nul 2>&1
del "%WORK_MDF%" "%WORK_LDF%" >nul 2>&1
rmdir "%WORK_DIR%" >nul 2>&1
exit /b 0

:subset
call :log "Slimming to catalog tables (SUBSET=1)..."
set "KEEP_FILE=%TEMP%\vm_keep_%RANDOM%.txt"
set "GEN_SQL=%TEMP%\vm_gen_%RANDOM%.sql"
set "RUN_SQL=%TEMP%\vm_run_%RANDOM%.sql"
break> "%KEEP_FILE%"
for %%T in (%KEEP_TABLES%) do >> "%KEEP_FILE%" echo %%T
REM Generate DROP statements for every table not in the keep list.
> "%GEN_SQL%" (
    echo .mode csv
    echo CREATE TEMP TABLE _keep^(name TEXT^);
    echo .import "%KEEP_FILE:\=/%" _keep
    echo .mode list
    echo SELECT 'DROP TABLE IF EXISTS [' ^|^| m.name ^|^| '];' FROM sqlite_master m WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%%' AND lower^(m.name^) NOT IN ^(SELECT lower^(name^) FROM _keep^);
)
"%SQLITE_TOOL%" -noheader "%SQLITE_DB%" < "%GEN_SQL%" > "%RUN_SQL%"
>> "%RUN_SQL%" echo VACUUM;
"%SQLITE_TOOL%" "%SQLITE_DB%" < "%RUN_SQL%"
del "%GEN_SQL%" "%RUN_SQL%" "%KEEP_FILE%" >nul 2>&1
exit /b 0

:log
>> "%LOG_FILE%" echo [%DATE% %TIME%] %~1
echo [%DATE% %TIME%] %~1
exit /b 0

:error
>> "%LOG_FILE%" echo [%DATE% %TIME%] ERROR: %~1
echo [ERROR] %~1
exit /b 0

:fail
call :cleanup
call :log "FAILED"
echo.
echo  FAILED - see %LOG_FILE%
goto :end

:end
echo.
pause
endlocal
exit /b
