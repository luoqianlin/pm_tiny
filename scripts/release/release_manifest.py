#!/usr/bin/env python3
"""Create and verify deterministic PM_Tiny release manifests."""

import argparse
import hashlib
import json
import os
import pathlib
import platform
import sys


MANIFEST_NAME = "release-manifest.json"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def release_files(root):
    result = []
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError("release must not contain symlinks: {}".format(path.relative_to(root)))
        if path.is_file() and path.name != MANIFEST_NAME:
            relative = path.relative_to(root).as_posix()
            result.append({
                "path": relative,
                "size": path.stat().st_size,
                "sha256": sha256(path),
            })
    return result


def create(args):
    root = pathlib.Path(args.release_dir).resolve()
    if not root.is_dir():
        raise ValueError("release directory does not exist: {}".format(root))
    files = release_files(root)
    required = {"pm_tiny.exe", "pm.exe"} if args.platform == "windows" else {"pm_tiny", "pm"}
    names = {entry["path"] for entry in files}
    missing = sorted(required - names)
    if missing:
        raise ValueError("release is missing required files: {}".format(", ".join(missing)))
    document = {
        "schema_version": 1,
        "release": {
            "id": args.release_id,
            "version": args.version,
            "platform": args.platform,
            "arch": args.arch,
            "protocol_version": args.protocol_version,
            "config_schema_version": args.config_schema_version,
        },
        "files": files,
    }
    output = root / MANIFEST_NAME
    temporary = output.with_name(output.name + ".tmp")
    temporary.write_text(json.dumps(document, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(str(temporary), str(output))
    print(str(output))


def verify(args):
    root = pathlib.Path(args.release_dir).resolve()
    manifest = root / MANIFEST_NAME
    with manifest.open(encoding="utf-8") as stream:
        document = json.load(stream)
    if document.get("schema_version") != 1:
        raise ValueError("unsupported manifest schema")
    release = document.get("release", {})
    for key in ("id", "version", "platform", "arch", "protocol_version", "config_schema_version"):
        if key not in release:
            raise ValueError("manifest release field is missing: {}".format(key))
    if args.release_id and release["id"] != args.release_id:
        raise ValueError("release id mismatch")
    if args.platform and release["platform"] != args.platform:
        raise ValueError("release platform mismatch")
    expected = document.get("files")
    if not isinstance(expected, list) or not expected:
        raise ValueError("manifest file list is empty")
    seen = set()
    for entry in expected:
        relative = entry.get("path", "")
        candidate = pathlib.PurePosixPath(relative)
        if not relative or candidate.is_absolute() or ".." in candidate.parts or relative in seen:
            raise ValueError("unsafe or duplicate manifest path: {}".format(relative))
        seen.add(relative)
        path = root.joinpath(*candidate.parts)
        if path.is_symlink():
            raise ValueError("manifest file must not be a symlink: {}".format(relative))
        if not path.is_file():
            raise ValueError("manifest file is missing: {}".format(relative))
        if path.stat().st_size != entry.get("size") or sha256(path) != entry.get("sha256"):
            raise ValueError("manifest hash mismatch: {}".format(relative))
    actual = {entry["path"] for entry in release_files(root)}
    if actual != seen:
        detail = sorted(actual.symmetric_difference(seen))
        raise ValueError("release contents do not match manifest: {}".format(", ".join(detail)))
    print(json.dumps(release, sort_keys=True))


def main():
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    create_parser = subparsers.add_parser("create")
    create_parser.add_argument("--release-dir", required=True)
    create_parser.add_argument("--release-id", required=True)
    create_parser.add_argument("--version", required=True)
    create_parser.add_argument("--platform", choices=("linux", "windows"), required=True)
    create_parser.add_argument("--arch", default=platform.machine())
    create_parser.add_argument("--protocol-version", type=int, default=3)
    create_parser.add_argument("--config-schema-version", type=int, default=1)
    create_parser.set_defaults(function=create)
    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("--release-dir", required=True)
    verify_parser.add_argument("--release-id")
    verify_parser.add_argument("--platform", choices=("linux", "windows"))
    verify_parser.set_defaults(function=verify)
    args = parser.parse_args()
    try:
        args.function(args)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print("release_manifest: {}".format(error), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
