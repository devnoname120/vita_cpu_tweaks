"""Fetch the plugin's boot log + current ur0:tai/config.txt after a test boot.
Usage: python3 fetch_results.py [log_out] [config_out]
"""
import ftplib, os, sys

HOST = "192.168.1.146"
PORT = 1337
CAPTURES = os.path.join(os.path.dirname(__file__), "..", "captures")
LOGOUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(CAPTURES, "boot_log_latest.log")
CFGOUT = sys.argv[2] if len(sys.argv) > 2 else os.path.join(CAPTURES, "taiconfig_after_latest.txt")

ftp = ftplib.FTP(timeout=10)
ftp.connect(HOST, PORT)
ftp.login()

buf = bytearray()
ftp.retrbinary("RETR ur0:data/prefetch_enabler.log", buf.extend)
with open(LOGOUT, "wb") as f:
    f.write(buf)
print(f"log: {len(buf)} bytes -> {LOGOUT}")

buf2 = bytearray()
ftp.retrbinary("RETR ur0:tai/config.txt", buf2.extend)
with open(CFGOUT, "wb") as f:
    f.write(buf2)
print(f"config.txt: {len(buf2)} bytes -> {CFGOUT}")

ftp.quit()
