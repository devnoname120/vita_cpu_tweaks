"""Re-enable the plugin's own (self-disabled) line in ur0:tai/config.txt,
flash a fresh build, reboot, and wait for the device to come back.

Since main.c's module_start() comments out its own config.txt line as the
very first thing it does (see self_disable_in_config() - the dead-man's
switch), every successful test boot leaves it disabled again for next
time. This script re-enables it before each test cycle.

Usage: python3 deploy_and_test.py [skprx_path]
"""
import ftplib
import io
import socket
import sys
import time
import os

HOST = "192.168.1.146"
FTP_PORT = 1337
CMD_PORT = 1338
NEEDLE = b"ur0:tai/cpu_tweaks.skprx"
SKPRX_LOCAL = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "build", "cpu_tweaks.skprx")

ftp = ftplib.FTP(timeout=10)
ftp.connect(HOST, FTP_PORT)
ftp.login()

print("--- fetching + re-enabling our own config.txt line ---")
buf = bytearray()
ftp.retrbinary("RETR ur0:tai/config.txt", buf.extend)
commented = b"#" + NEEDLE
if commented in buf:
    buf = buf.replace(commented, NEEDLE, 1)
    print("line was commented out (self-disabled from a prior run) - re-enabled")
elif NEEDLE in buf:
    print("line was already active - leaving as is")
else:
    print("ERROR: no line matching our skprx name found in config.txt - aborting")
    ftp.quit()
    sys.exit(1)
ftp.storbinary("STOR ur0:tai/config.txt", io.BytesIO(buf))
print("uploaded config.txt, size now:", ftp.size("ur0:tai/config.txt"))

print("--- uploading new skprx ---")
with open(SKPRX_LOCAL, "rb") as f:
    ftp.storbinary("STOR ur0:tai/cpu_tweaks.skprx", f)
print("uploaded skprx, size now:", ftp.size("ur0:tai/cpu_tweaks.skprx"))

ftp.quit()

print("--- rebooting ---")
s = socket.create_connection((HOST, CMD_PORT), timeout=8)
s.sendall(b"reboot\n")
s.settimeout(3)
try:
    print("reboot response:", s.recv(4096))
except socket.timeout:
    print("reboot sent, no response before timeout (expected)")
s.close()

print("waiting for device to go down and FTP to come back...")
time.sleep(10)
deadline = time.time() + 90
up = False
while time.time() < deadline:
    try:
        f2 = ftplib.FTP(timeout=4)
        f2.connect(HOST, FTP_PORT)
        f2.login()
        f2.quit()
        up = True
        break
    except Exception:
        time.sleep(3)

if not up:
    print("FTP did not come back within the wait window")
    sys.exit(1)

print("FTP back up, boot completed")
