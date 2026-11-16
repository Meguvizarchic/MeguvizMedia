@echo off
if not defined MGV_PYTHON goto standard
"%MGV_PYTHON%" "%~dp0mgv_integrate.py" %*
exit /b %errorlevel%
:standard
python "%~dp0mgv_integrate.py" %*
exit /b %errorlevel%
