@echo off
rem adX - double-click to open the application; drop a .adx file on it to open that project.
rem
rem It runs the code in this folder as last built. Every phase ends by rebuilding
rem (py -3.12 -m pip install -e .[dev]) and checking this launcher starts the app
rem (plans/STATE.md, "When you finish a phase"), so it is always the latest build.
setlocal
cd /d "%~dp0"
py -3.12 -c "import adx, adx_engine" >nul 2>&1
if errorlevel 1 (
  echo adX is not built for Python 3.12 on this machine. Build it once, from this folder:
  echo.
  echo     py -3.12 -m pip install -e .[dev]
  echo.
  pause
  exit /b 1
)
start "" pyw -3.12 -m adx gui %*
