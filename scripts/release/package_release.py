#!/usr/bin/env python3
"""Create and verify deterministic PM_Tiny binary release archives."""

import argparse
import datetime
import gzip
import hashlib
import json
import os
import pathlib
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
DEFAULT_REPO = SCRIPT_DIR.parent.parent
DEFAULT_POLICY = SCRIPT_DIR / "release-policy.json"
METADATA_NAME = "RELEASE-METADATA.json"


def fail(message):
    raise ValueError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(arguments, cwd=None):
    result = subprocess.run(
        arguments, cwd=str(cwd) if cwd else None, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    if result.returncode:
        fail("command failed ({}): {}".format(result.returncode, " ".join(arguments)))
    return result.stdout.strip()


def load_policy(path):
    with path.open(encoding="utf-8") as stream:
        policy = json.load(stream)
    if policy.get("schema_version") != 1 or policy.get("project") != "pm-tiny":
        fail("unsupported release policy")
    return policy


def read_version(repo):
    version = (repo / "VERSION").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        fail("VERSION is not a semantic version")
    return version


def version_tuple(value):
    return tuple(int(item) for item in value.split("."))


def git_commit(repo, requested):
    actual = command_output(["git", "rev-parse", "HEAD"], repo)
    resolved = command_output(["git", "rev-parse", requested], repo)
    if actual != resolved:
        fail("release source must be the checked-out HEAD: {} != {}".format(resolved, actual))
    return resolved


def git_epoch(repo, commit):
    return int(command_output(["git", "show", "-s", "--format=%ct", commit], repo))


def artifact_name(version, platform, archive_type):
    suffix = ".tar.gz" if archive_type == "tar.gz" else ".zip"
    return "pm-tiny-{}-{}{}".format(version, platform, suffix)


def root_name(version, platform):
    return "pm-tiny-{}-{}".format(version, platform)


def destination_for_repository_file(relative):
    path = pathlib.PurePosixPath(relative)
    if path.parts[:3] == ("examples", "config", "linux"):
        return pathlib.PurePosixPath("config", path.name)
    if path.parts[:3] == ("examples", "config", "windows"):
        return pathlib.PurePosixPath("config", path.name)
    return path


def release_files(root):
    files = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            fail("release contains a symlink: {}".format(path.relative_to(root)))
        if path.is_file() and path.name != METADATA_NAME:
            relative = path.relative_to(root).as_posix()
            files.append({
                "path": relative,
                "size": path.stat().st_size,
                "sha256": sha256(path),
            })
    return files


def read_elf_machine(path):
    with path.open("rb") as stream:
        header = stream.read(20)
    if len(header) < 20 or header[:4] != b"\x7fELF":
        fail("not an ELF binary: {}".format(path))
    endian = "<" if header[5] == 1 else ">"
    return struct.unpack(endian + "H", header[18:20])[0]


def read_pe_machine(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) < 64 or header[:2] != b"MZ":
            fail("not a PE binary: {}".format(path))
        offset = struct.unpack("<I", header[60:64])[0]
        stream.seek(offset)
        pe = stream.read(6)
    if len(pe) != 6 or pe[:4] != b"PE\0\0":
        fail("invalid PE header: {}".format(path))
    return struct.unpack("<H", pe[4:6])[0]


def maximum_symbol_version(output, prefix):
    values = re.findall(r"{}([0-9]+(?:\.[0-9]+)+)".format(re.escape(prefix)), output)
    return max(values, key=version_tuple) if values else "0"


def validate_linux(binary_dir, version, compatibility):
    for name in ("pm_tiny", "pm"):
        path = binary_dir / name
        if not path.is_file() or read_elf_machine(path) != 62:
            fail("Linux release requires x86_64 ELF binary: {}".format(name))
    daemon_version = command_output([str(binary_dir / "pm_tiny"), "--version"])
    cli_version = command_output([str(binary_dir / "pm"), "--version"])
    if version not in daemon_version or "pm_tiny: {}".format(version) not in cli_version:
        fail("Linux binary version does not match VERSION")
    for name in ("pm_tiny", "pm"):
        output = command_output(["readelf", "--version-info", str(binary_dir / name)])
        checks = (
            ("GLIBC_", "max_glibc"),
            ("GLIBCXX_", "max_glibcxx"),
            ("CXXABI_", "max_cxxabi"),
        )
        for prefix, key in checks:
            actual = maximum_symbol_version(output, prefix)
            if version_tuple(actual) > version_tuple(compatibility[key]):
                fail("{} requires {}{} above policy {}".format(
                    name, prefix, actual, compatibility[key]))


def validate_android(binary_dir):
    for name in ("pm_tiny", "pm2"):
        path = binary_dir / name
        if not path.is_file() or read_elf_machine(path) != 183:
            fail("Android release requires AArch64 ELF binary: {}".format(name))
        dynamic = command_output(["readelf", "-d", str(path)])
        if "libc++_shared.so" in dynamic:
            fail("Android binary depends on libc++_shared.so: {}".format(name))
    if (binary_dir / "pm").exists():
        fail("Android release must not contain pm")


def validate_windows(binary_dir):
    for name in ("pm_tiny.exe", "pm.exe"):
        path = binary_dir / name
        if not path.is_file() or read_pe_machine(path) != 0x8664:
            fail("Windows release requires x64 PE binary: {}".format(name))
    if (binary_dir / "pm2.exe").exists():
        fail("Windows release must not contain pm2.exe")


def validate_binaries(platform, binary_dir, version, entry):
    expected = set(entry["binaries"])
    actual = {path.name for path in binary_dir.iterdir() if path.is_file()}
    missing = expected - actual
    if missing:
        fail("release binaries are missing: {}".format(", ".join(sorted(missing))))
    if platform == "linux-x86_64":
        validate_linux(binary_dir, version, entry["compatibility"])
    elif platform == "android-arm64-v8a":
        validate_android(binary_dir)
    elif platform == "windows-x64-msvc":
        validate_windows(binary_dir)


def copy_release_tree(repo, binary_dir, stage, entry):
    (stage / "bin").mkdir(parents=True)
    for name in entry["binaries"]:
        shutil.copy2(str(binary_dir / name), str(stage / "bin" / name))
        if not name.endswith(".exe"):
            (stage / "bin" / name).chmod(0o755)
    for relative in entry["repository_files"]:
        source = repo / pathlib.PurePosixPath(relative)
        if not source.is_file():
            fail("required public release file is missing: {}".format(relative))
        destination = stage / destination_for_repository_file(relative)
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(str(source), str(destination))


def write_metadata(stage, version, platform, source_commit, toolchain, entry):
    document = {
        "schema_version": 1,
        "project": "pm-tiny",
        "version": version,
        "source_commit": source_commit,
        "platform": platform,
        "architecture": entry["architecture"],
        "build_type": "Release",
        "toolchain": toolchain,
        "compatibility": entry.get("compatibility", {}),
        "android_api": entry.get("android_api"),
        "android_ndk": entry.get("ndk_version"),
        "files": release_files(stage),
    }
    (stage / METADATA_NAME).write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def normalized_mode(path):
    relative = path.as_posix()
    return 0o755 if relative.startswith("bin/") or relative.endswith((".sh", ".py")) else 0o644


def create_tar(stage, archive, epoch):
    with archive.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w") as output:
                root = stage.name
                directory = tarfile.TarInfo(root + "/")
                directory.type = tarfile.DIRTYPE
                directory.mode = 0o755
                directory.mtime = epoch
                output.addfile(directory)
                for path in sorted(stage.rglob("*")):
                    relative = pathlib.PurePosixPath(root) / pathlib.PurePosixPath(path.relative_to(stage).as_posix())
                    info = output.gettarinfo(str(path), arcname=relative.as_posix())
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    info.mtime = epoch
                    info.mode = 0o755 if path.is_dir() else normalized_mode(path.relative_to(stage))
                    if path.is_file():
                        with path.open("rb") as stream:
                            output.addfile(info, stream)
                    else:
                        output.addfile(info)


def create_zip(stage, archive, epoch):
    stamp = datetime.datetime.utcfromtimestamp(max(epoch, 315532800))
    date_time = (stamp.year, stamp.month, stamp.day, stamp.hour, stamp.minute, stamp.second)
    with zipfile.ZipFile(str(archive), "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as output:
        root = stage.name
        paths = [stage] + sorted(stage.rglob("*"))
        for path in paths:
            relative = pathlib.PurePosixPath(root)
            if path != stage:
                relative /= pathlib.PurePosixPath(path.relative_to(stage).as_posix())
            is_directory = path.is_dir()
            name = relative.as_posix() + ("/" if is_directory else "")
            info = zipfile.ZipInfo(name, date_time=date_time)
            info.create_system = 3
            mode = 0o755 if is_directory else normalized_mode(path.relative_to(stage))
            info.external_attr = ((stat.S_IFDIR if is_directory else stat.S_IFREG) | mode) << 16
            output.writestr(info, b"" if is_directory else path.read_bytes(), compress_type=zipfile.ZIP_DEFLATED)


def verify_metadata(root, expected_version=None, expected_commit=None, expected_platform=None):
    with (root / METADATA_NAME).open(encoding="utf-8") as stream:
        metadata = json.load(stream)
    if metadata.get("schema_version") != 1 or metadata.get("project") != "pm-tiny":
        fail("unsupported release metadata")
    if expected_version and metadata.get("version") != expected_version:
        fail("release metadata version mismatch")
    if expected_commit and metadata.get("source_commit") != expected_commit:
        fail("release metadata source commit mismatch")
    if expected_platform and metadata.get("platform") != expected_platform:
        fail("release metadata platform mismatch")
    if metadata.get("files") != release_files(root):
        fail("release metadata file hashes do not match archive contents")
    return metadata


def safe_extract(archive, destination):
    if archive.name.endswith(".tar.gz"):
        with tarfile.open(str(archive), "r:gz") as source:
            for member in source.getmembers():
                path = pathlib.PurePosixPath(member.name)
                if path.is_absolute() or ".." in path.parts or member.issym() or member.islnk():
                    fail("unsafe tar member: {}".format(member.name))
            source.extractall(str(destination))
    else:
        with zipfile.ZipFile(str(archive)) as source:
            for member in source.infolist():
                path = pathlib.PurePosixPath(member.filename)
                mode = member.external_attr >> 16
                if path.is_absolute() or ".." in path.parts or stat.S_ISLNK(mode):
                    fail("unsafe zip member: {}".format(member.filename))
            source.extractall(str(destination))


def package(args):
    repo = args.repo.resolve()
    policy = load_policy(args.policy)
    entry = policy["platforms"].get(args.platform)
    if not entry:
        fail("unknown release platform: {}".format(args.platform))
    version = read_version(repo)
    commit = git_commit(repo, args.source_commit)
    binary_dir = args.binary_dir.resolve()
    validate_binaries(args.platform, binary_dir, version, entry)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    stage_parent = output / "stage"
    stage_parent.mkdir(exist_ok=True)
    stage = stage_parent / root_name(version, args.platform)
    if stage.exists():
        shutil.rmtree(str(stage))
    stage.mkdir()
    copy_release_tree(repo, binary_dir, stage, entry)
    write_metadata(stage, version, args.platform, commit, args.toolchain, entry)
    archive = output / artifact_name(version, args.platform, entry["archive"])
    temporary = archive.with_name(archive.name + ".tmp")
    if temporary.exists():
        temporary.unlink()
    epoch = git_epoch(repo, commit)
    if entry["archive"] == "tar.gz":
        create_tar(stage, temporary, epoch)
    else:
        create_zip(stage, temporary, epoch)
    os.replace(str(temporary), str(archive))
    print(str(archive))


def checksums(args):
    policy = load_policy(args.policy)
    version = args.version
    names = [artifact_name(version, platform, entry["archive"])
             for platform, entry in policy["platforms"].items()]
    output = args.assets_dir.resolve() / "SHA256SUMS"
    lines = []
    for name in sorted(names):
        path = args.assets_dir.resolve() / name
        if not path.is_file():
            fail("release asset is missing: {}".format(name))
        lines.append("{}  {}".format(sha256(path), name))
    output.write_text("\n".join(lines) + "\n", encoding="ascii")
    print(str(output))


def verify_assets(args):
    policy = load_policy(args.policy)
    assets = args.assets_dir.resolve()
    expected = {"SHA256SUMS"}
    for platform, entry in policy["platforms"].items():
        expected.add(artifact_name(args.version, platform, entry["archive"]))
    actual = {path.name for path in assets.iterdir() if path.is_file()}
    if actual != expected:
        fail("release asset set mismatch: expected {}, actual {}".format(
            sorted(expected), sorted(actual)))
    checksum_lines = (assets / "SHA256SUMS").read_text(encoding="ascii").splitlines()
    checks = {}
    for line in checksum_lines:
        match = re.fullmatch(r"([0-9a-f]{64})  ([^/]+)", line)
        if not match:
            fail("invalid SHA256SUMS line")
        checks[match.group(2)] = match.group(1)
    if set(checks) != expected - {"SHA256SUMS"}:
        fail("SHA256SUMS asset set mismatch")
    for name, digest in checks.items():
        if sha256(assets / name) != digest:
            fail("release asset checksum mismatch: {}".format(name))
    for platform, entry in policy["platforms"].items():
        name = artifact_name(args.version, platform, entry["archive"])
        with tempfile.TemporaryDirectory(prefix="pm-tiny-release-verify-") as temporary:
            root = pathlib.Path(temporary)
            safe_extract(assets / name, root)
            extracted = root / root_name(args.version, platform)
            metadata = verify_metadata(extracted, args.version, args.source_commit, platform)
            validate_binaries(platform, extracted / "bin", args.version, entry)
            if metadata["architecture"] != entry["architecture"]:
                fail("release architecture metadata mismatch")
    print("release assets: OK")


def parser():
    result = argparse.ArgumentParser()
    result.add_argument("--policy", type=pathlib.Path, default=DEFAULT_POLICY)
    subparsers = result.add_subparsers(dest="command", required=True)
    create = subparsers.add_parser("package")
    create.add_argument("--repo", type=pathlib.Path, default=DEFAULT_REPO)
    create.add_argument("--platform", required=True)
    create.add_argument("--binary-dir", type=pathlib.Path, required=True)
    create.add_argument("--output-dir", type=pathlib.Path, required=True)
    create.add_argument("--source-commit", required=True)
    create.add_argument("--toolchain", required=True)
    create.set_defaults(function=package)
    digest = subparsers.add_parser("checksums")
    digest.add_argument("--assets-dir", type=pathlib.Path, required=True)
    digest.add_argument("--version", required=True)
    digest.set_defaults(function=checksums)
    verify = subparsers.add_parser("verify")
    verify.add_argument("--assets-dir", type=pathlib.Path, required=True)
    verify.add_argument("--version", required=True)
    verify.add_argument("--source-commit", required=True)
    verify.set_defaults(function=verify_assets)
    return result


def main():
    args = parser().parse_args()
    try:
        args.function(args)
    except (OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as error:
        print("package_release: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
