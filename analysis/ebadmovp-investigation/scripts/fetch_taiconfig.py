"""Fetch ur0:tai/config.txt from the retail devkit over VitaCompanion FTP.
Usage: python3 fetch_taiconfig.py [out_path]
"""
import ftplib, os, sys

HOST = "192.168.1.146"
PORT = 1337
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(__file__), "..", "captures", "taiconfig_current.txt")

ftp = ftplib.FTP(timeout=10)
ftp.connect(HOST, PORT)
ftp.login()
buf = bytearray()
ftp.retrbinary("RETR ur0:tai/config.txt", buf.extend)
with open(OUT, "wb") as f:
    f.write(buf)
print(f"downloaded {len(buf)} bytes -> {OUT}")
ftp.quit()
