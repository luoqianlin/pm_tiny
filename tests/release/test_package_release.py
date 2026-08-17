#!/usr/bin/env python3

import argparse
import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODULE_PATH = ROOT / "scripts" / "release" / "package_release.py"
SPEC = importlib.util.spec_from_file_location("package_release", str(MODULE_PATH))
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleasePackageTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pm-tiny-package-test-")
        self.root = pathlib.Path(self.temporary.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        (self.repo / "VERSION").write_text("4.1.0\n", encoding="utf-8")
        self.policy = {
            "schema_version": 1,
            "project": "pm-tiny",
            "platforms": {
                "linux-x86_64": {
                    "archive": "tar.gz",
                    "architecture": "x86_64",
                    "binaries": ["pm_tiny", "pm"],
                    "repository_files": ["LICENSE", "README.md"],
                    "compatibility": {
                        "max_glibc": "2.17",
                        "max_glibcxx": "3.4.26",
                        "max_cxxabi": "1.3.11"
                    }
                }
            }
        }
        self.policy_path = self.root / "policy.json"
        self.policy_path.write_text(json.dumps(self.policy), encoding="utf-8")
        (self.repo / "LICENSE").write_text("license\n", encoding="utf-8")
        (self.repo / "README.md").write_text("readme\n", encoding="utf-8")
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        subprocess.run(["git", "config", "user.name", "Release Test"], cwd=self.repo, check=True)
        subprocess.run(["git", "config", "user.email", "release@example.invalid"], cwd=self.repo, check=True)
        subprocess.run(["git", "add", "."], cwd=self.repo, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", "fixture"], cwd=self.repo, check=True,
            env={"PATH": str(pathlib.Path("/usr/bin")), "GIT_AUTHOR_DATE": "2020-01-01T00:00:00Z",
                 "GIT_COMMITTER_DATE": "2020-01-01T00:00:00Z"},
        )
        self.commit = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=self.repo, text=True).strip()
        self.binaries = self.root / "bin"
        self.binaries.mkdir()
        for name in ("pm_tiny", "pm"):
            path = self.binaries / name
            path.write_bytes(b"fixture-" + name.encode("ascii"))
            path.chmod(0o755)

    def tearDown(self):
        self.temporary.cleanup()

    def package(self, output):
        args = argparse.Namespace(
            repo=self.repo,
            policy=self.policy_path,
            platform="linux-x86_64",
            binary_dir=self.binaries,
            output_dir=output,
            source_commit=self.commit,
            toolchain="fixture compiler",
        )
        with mock.patch.object(release, "validate_binaries"):
            release.package(args)
        return output / "pm-tiny-4.1.0-linux-x86_64.tar.gz"

    def test_deterministic_archive_and_metadata(self):
        first = self.package(self.root / "first")
        second = self.package(self.root / "second")
        self.assertEqual(release.sha256(first), release.sha256(second))
        with tempfile.TemporaryDirectory() as extracted:
            destination = pathlib.Path(extracted)
            release.safe_extract(first, destination)
            metadata = release.verify_metadata(
                destination / "pm-tiny-4.1.0-linux-x86_64",
                "4.1.0", self.commit, "linux-x86_64")
        self.assertEqual(metadata["toolchain"], "fixture compiler")
        self.assertNotIn(str(self.repo), json.dumps(metadata))

    def test_release_files_reject_symlinks(self):
        stage = self.root / "symlink-stage"
        stage.mkdir()
        (stage / "target").write_text("data", encoding="utf-8")
        (stage / "link").symlink_to("target")
        with self.assertRaisesRegex(ValueError, "symlink"):
            release.release_files(stage)

    def test_checksums_and_exact_asset_set(self):
        assets = self.root / "assets"
        archive = self.package(assets)
        args = argparse.Namespace(policy=self.policy_path, assets_dir=assets, version="4.1.0")
        release.checksums(args)
        verify = argparse.Namespace(
            policy=self.policy_path, assets_dir=assets, version="4.1.0",
            source_commit=self.commit)
        with mock.patch.object(release, "validate_binaries"):
            release.verify_assets(verify)
        (assets / "unexpected.txt").write_text("not allowed", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "asset set mismatch"):
            release.verify_assets(verify)
        self.assertTrue(archive.is_file())

    def test_checksum_tamper_is_rejected(self):
        assets = self.root / "tamper"
        archive = self.package(assets)
        release.checksums(argparse.Namespace(
            policy=self.policy_path, assets_dir=assets, version="4.1.0"))
        archive.write_bytes(archive.read_bytes() + b"tamper")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            release.verify_assets(argparse.Namespace(
                policy=self.policy_path, assets_dir=assets, version="4.1.0",
                source_commit=self.commit))


if __name__ == "__main__":
    unittest.main()
