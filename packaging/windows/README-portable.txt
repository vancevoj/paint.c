paint.c (portable)

Run paintc.exe. Nothing needs to be installed; settings, recent files and
the autosave copies used for crash recovery live in your user profile
(%APPDATA%\paintc and %LOCALAPPDATA%\paintc), so the folder can stay
read-only. To keep everything next to the program instead, start it with
    paintc.exe --config-dir .\profile

The installer (paintc-<version>-windows-x64-setup.exe) adds Start menu
shortcuts and offers paint.c in "Open with" for every image type it reads.

paint.c is MIT licensed; the licenses folder has its license, the NOTICE
and the licenses of the libraries and the font it uses.
