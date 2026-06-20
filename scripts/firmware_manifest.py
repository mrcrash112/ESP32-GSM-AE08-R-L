#!/usr/bin/env python3
import hashlib
import json
import pathlib
import argparse
import io
import re
import shutil
import tarfile

parser = argparse.ArgumentParser()
parser.add_argument("firmware", type=pathlib.Path)
parser.add_argument("version")
parser.add_argument("base_url")
parser.add_argument("--recovery", type=pathlib.Path)
parser.add_argument("--recovery-version")
parser.add_argument("--www-dir", type=pathlib.Path, default=pathlib.Path("data/www"))
args = parser.parse_args()

firmware = args.firmware
version = args.version
base_url = args.base_url.rstrip("/")
www_dir = args.www_dir
if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", version):
    parser.error("version must use semantic versioning, for example 1.2.3")
main_asset = f"mione-main-{version}.bin"
shutil.copyfile(firmware, firmware.parent / main_asset)
manifest = {
    "product": "NORVI-GSM-AE08-R-L",
    "version": version,
    "firmware": {
        "url": f"{base_url}/{main_asset}",
        "size": firmware.stat().st_size,
        "md5": hashlib.md5(firmware.read_bytes()).hexdigest(),
    },
}
required_web_files = ("index.html", "config.css", "config.js")
if not www_dir.is_dir() or any(not (www_dir / name).is_file() for name in required_web_files):
    parser.error("--www-dir must contain index.html, config.css and config.js")
web_asset = f"mione-www-{version}.tar"
web_path = firmware.parent / web_asset
with tarfile.open(web_path, "w", format=tarfile.USTAR_FORMAT) as archive:
    for source in sorted(www_dir.rglob("*")):
        if source.is_symlink():
            parser.error("www directory must not contain symbolic links")
        relative = source.relative_to(www_dir).as_posix()
        if relative == "version.json":
            continue
        info = archive.gettarinfo(str(source), arcname=relative)
        info.uid = 0
        info.gid = 0
        info.uname = ""
        info.gname = ""
        info.mtime = 0
        if source.is_file():
            with source.open("rb") as content:
                archive.addfile(info, content)
        else:
            archive.addfile(info)
    web_version = json.dumps({
        "product": "NORVI-GSM-AE08-R-L",
        "version": version,
        "firmwareVersion": version,
    }, indent=2).encode("ascii") + b"\n"
    info = tarfile.TarInfo("version.json")
    info.size = len(web_version)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    archive.addfile(info, io.BytesIO(web_version))
manifest["web"] = {
    "version": version,
    "format": "tar",
    "url": f"{base_url}/{web_asset}",
    "size": web_path.stat().st_size,
    "md5": hashlib.md5(web_path.read_bytes()).hexdigest(),
}
if args.recovery:
    if not args.recovery_version:
        parser.error("--recovery-version is required with --recovery")
    if not re.fullmatch(r"\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?", args.recovery_version):
        parser.error("recovery version must use semantic versioning")
    recovery_asset = f"mione-recovery-{args.recovery_version}.bin"
    shutil.copyfile(args.recovery, firmware.parent / recovery_asset)
    manifest["recovery"] = {
        "version": args.recovery_version,
        "url": f"{base_url}/{recovery_asset}",
        "size": args.recovery.stat().st_size,
        "md5": hashlib.md5(args.recovery.read_bytes()).hexdigest(),
    }
output = firmware.with_suffix(".json")
output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="ascii")
print(output)
print(firmware.parent / main_asset)
print(web_path)
if args.recovery:
    print(firmware.parent / recovery_asset)
