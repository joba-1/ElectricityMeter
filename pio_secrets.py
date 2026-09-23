# Pre-build: admin password for the admin endpoints (/update, /reset, POST /api/netconfig).
#
# Read from secrets.ini (untracked, mode 600):
#     [secrets]
#     admin_password = <password>
# Without the file the firmware builds without the lock (and says so in
# /api/netstatus: "adminLock": false). The same password is used by
# upload_script.py for OTA uploads.
import configparser
import os

Import("env")  # noqa: F821

path = os.path.join(env.subst("$PROJECT_DIR"), "secrets.ini")  # noqa: F821
password = ""
if os.path.exists(path):
    cp = configparser.ConfigParser()
    cp.read(path)
    password = cp.get("secrets", "admin_password", fallback="").strip()
if password:
    env.Append(CPPDEFINES=[("ADMIN_PASSWORD", env.StringifyMacro(password))])  # noqa: F821
else:
    print("secrets.py: no admin_password in secrets.ini - building WITHOUT admin lock")
