@echo off
rem start-sc-agent.bat - start sc-agent.
rem   start-sc-agent.bat             in the background (pythonw, no window); log: %%USERPROFILE%%\.sc-agent\agent.log
rem   start-sc-agent.bat --console   in the foreground with the log in this window; Ctrl+C stops it
rem Further arguments (for example --verbose) are passed to sc_agent.py.
setlocal
set "HERE=%~dp0"

if not exist "%HERE%.venv\Scripts\pythonw.exe" (
    echo sc-agent: .venv not found, run setup.bat first.
    exit /b 1
)

if /i "%~1"=="--console" (
    "%HERE%.venv\Scripts\python.exe" "%HERE%sc_agent.py" %*
    exit /b
)

start "sc-agent" "%HERE%.venv\Scripts\pythonw.exe" "%HERE%sc_agent.py" %*
