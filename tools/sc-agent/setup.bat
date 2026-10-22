@echo off
rem setup.bat - create tools\sc-agent\.venv and install the requirements (run once, again after Python upgrades).
rem Uses the py launcher or python on PATH; set SC_PYTHON to the full path of python.exe to choose another one.
rem An existing .venv is reused only when it still starts and has the same Python major.minor version.
setlocal
cd /d "%~dp0"

if defined SC_PYTHON (
    set PY="%SC_PYTHON%"
) else (
    where py >nul 2>nul
    if errorlevel 1 (set PY=python) else (set PY=py -3)
)

%PY% -c "import sys; sys.exit(sys.version_info < (3, 10))"
if errorlevel 1 (
    echo sc-agent needs Python 3.10 or newer. Set SC_PYTHON to the full path of a suitable python.exe.
    exit /b 1
)

if not exist ".venv\Scripts\python.exe" goto create

rem The venv must start, and its version must match the chosen Python; otherwise rebuild it.
%PY% -c "import subprocess, sys; r = subprocess.run([r'.venv\Scripts\python.exe', '-c', 'import sys; print(sys.version_info[:2])'], capture_output=True, text=True); sys.exit(r.returncode != 0 or r.stdout.strip() != str(sys.version_info[:2]))" >nul 2>nul
if not errorlevel 1 goto install

echo The existing .venv does not start or uses another Python version, recreating it ...
echo If this fails, stop sc-agent first: it runs from this .venv.
%PY% -m venv --clear .venv
if errorlevel 1 exit /b 1
goto install

:create
echo Creating .venv ...
%PY% -m venv .venv
if errorlevel 1 exit /b 1

:install
".venv\Scripts\python.exe" -m pip install --disable-pip-version-check -r requirements.txt
if errorlevel 1 exit /b 1

echo.
echo sc-agent is ready. Start it with start-sc-agent.bat, or start-sc-agent.bat --console to watch the log.
