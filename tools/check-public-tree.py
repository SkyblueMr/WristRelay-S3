#!/usr/bin/env python3
"""Check a source export, including ignored files and staged Git blobs."""

import argparse
import os
import re
import stat
import subprocess
from pathlib import Path
from urllib.parse import parse_qsl, urlsplit


TEXT_EXTENSIONS = {".bat", ".c", ".cmake", ".cpp", ".csv", ".h", ".hpp", ".json",
                   ".md", ".ps1", ".py", ".sh", ".toml", ".txt", ".yaml", ".yml"}
TEXT_NAMES = {".gitattributes", ".gitignore", "license", "sdkconfig.defaults", "sdkconfig.wakenet", "kconfig.projbuild",
              "dependencies.lock", "dependencies.lock.wakenet", "cmakelists.txt", "makefile"}
FORBIDDEN_DIRS = {".cache", ".idea", ".pytest_cache", ".venv", ".vscode", "__pycache__",
                  "build", "captures", "dist", "managed_components", "models", "node_modules",
                  "private", "recordings", "signing"}
PUBLIC_HOSTS = {"docs.espressif.com", "components.espressif.com", "github.com", "docs.python.org",
                "downloads.xiph.org",
                "docs.waveshare.com", "files.waveshare.com", "www.waveshare.com", "pool.ntp.org",
                "localhost", "127.0.0.1", "::1", "0.0.0.0", "example.com", "example.org",
                "example.net", "apache.org", "www.apache.org"}
PUBLIC_URLS = {"https://ftp.osuosl.org/pub/xiph/releases/opus/opus-1.5.2.tar.gz"}
RULES = {
    "key or certificate material": r"-----BEGIN (?:(?:[A-Z0-9]+ )*PRIVATE KEY|CERTIFICATE)-----",
    "service credential": r"\b(?:gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|sk-[A-Za-z0-9_-]{20,}|AKIA[A-Z0-9]{16}|eyJ[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,}\.[A-Za-z0-9_-]{12,})\b",
    "inline credential": r"""(?ix)\b(?:api[_-]?key|access[_-]?token|client[_-]?secret|wifi[_-]?(?:pass(?:word)?|psk)|password|psk)["']?\s*[:=]\s*["'](?!change.me["']|replace.me["']|<[^>]+>["'])[^"'\r\n]+["']""",
    "populated local firmware setting": r'(?im)^\s*CONFIG_WR_(?:WIFI_SSID|WIFI_PASSPHRASE|VOICE_URI|VOICE_TOKEN)\s*=\s*"[^"]+"',
    "host filesystem path": r"(?i)(?<![\w/])[a-z]:[/\\]+[a-z0-9_]|/(?:home|Users|mnt/[a-z])/[A-Za-z0-9_.-]+",
    "hardware address": r"(?i)(?<![:\w])(?:[a-f0-9]{2}[:-]){5}[a-f0-9]{2}(?![:\w])",
    "private browser or machine state": r"(?i)\b(?:storageStat[e]|cookies\.sqlite|login dat[a]|known_host[s])\b",
}
URL = re.compile(r"""(?i)\b(?:https?|wss?|ssh|ftp)://[^<>"'\x60\s)\]}]+""")
IPV4 = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])")


def git(root, *args):
    return subprocess.run(["git", "-C", str(root), *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=True).stdout


def is_source(path):
    name = Path(path).name.lower()
    return name in TEXT_NAMES or Path(name).suffix in TEXT_EXTENSIONS or name.endswith(".example")


def display(path):
    if any(re.search(pattern, path) for pattern in RULES.values()):
        return "<redacted path>"
    return repr(path)


def inspect(data, label, failures):
    try:
        value = data.decode("utf-8-sig")
    except UnicodeError:
        failures.append((label, "not UTF-8 source text"))
        return
    if any(ord(c) < 32 and c not in "\t\n\r" for c in value):
        failures.append((label, "binary or control content"))
        return
    local_config = False
    for number, line in enumerate(value.splitlines(), 1):
        where = f"{label}:{number}"
        setting = re.match(r"\s*config\s+(\w+)\s*$", line)
        if setting:
            local_config = setting[1] in {"WR_WIFI_SSID", "WR_WIFI_PASSPHRASE",
                                          "WR_VOICE_URI", "WR_VOICE_TOKEN"}
        if local_config and re.match(r'\s*default\s+"[^"]+"', line):
            failures.append((where, "populated default for local firmware setting"))
        for name, pattern in RULES.items():
            if re.search(pattern, line):
                failures.append((where, name))
        for url in URL.findall(line):
            try:
                parsed = urlsplit(url)
                if parsed.username or parsed.password or (
                    parsed.hostname not in PUBLIC_HOSTS and url not in PUBLIC_URLS
                ):
                    failures.append((where, "unreviewed endpoint or URL credentials"))
                if any(value and re.search(r"(?i)(?:token|secret|signature|password|api[_-]?key)", key)
                       for key, value in parse_qsl(parsed.query)):
                    failures.append((where, "credential-like URL query parameter"))
            except ValueError:
                failures.append((where, "malformed endpoint"))
        for ip in IPV4.findall(line):
            if ip not in PUBLIC_HOSTS and all(int(part) <= 255 for part in ip.split(".")):
                failures.append((where, "unreviewed IPv4 address"))


def inspect_index(root, first_release, failures):
    metadata = root / ".git"
    if not metadata.exists():
        if first_release:
            failures.append((".git", "first-release check requires a Git repository"))
        return 0, 0
    if not metadata.is_dir() or metadata.is_symlink() or (
        getattr(metadata.lstat(), "st_file_attributes", 0) & 0x400
    ) or (metadata / "objects" / "info" / "alternates").exists():
        failures.append((".git", "Git metadata is not standalone"))
        return 0, 0
    if git(root, "remote").strip():
        failures.append((".git", "configured Git remote in local export"))
    for name in ("config", "config.worktree", "FETCH_HEAD"):
        path = metadata / name
        if path.exists():
            inspect(path.read_bytes(), ".git/" + name, failures)
    indexed = set()
    for entry in filter(None, git(root, "ls-files", "--stage", "-z").split(b"\0")):
        info, raw_path = entry.split(b"\t", 1)
        mode, object_id, stage = info.decode("ascii").split()
        path = os.fsdecode(raw_path)
        label = "index:" + display(path)
        if mode not in {"100644", "100755"} or stage != "0" or not is_source(path):
            failures.append((label, "non-source or unmerged index entry"))
        else:
            inspect(git(root, "cat-file", "blob", object_id), label, failures)
        indexed.add(object_id)
    head = None
    release_tree = None
    if first_release:
        head = git(root, "rev-parse", "--verify", "HEAD^{commit}").decode("ascii").strip()
        commit = git(root, "cat-file", "commit", head)
        headers = commit.split(b"\n\n", 1)[0].splitlines()
        if any(line.startswith(b"parent ") for line in headers):
            failures.append(("HEAD", "first-release commit is not a root commit"))
        inspect(commit, "HEAD", failures)
        release_tree = {git(root, "rev-parse", "HEAD^{tree}").decode("ascii").strip()}
        for line in git(root, "ls-tree", "-r", "-t", "HEAD").splitlines():
            info = line.split(b"\t", 1)[0].decode("ascii").split()
            if info[1] == "tree":
                release_tree.add(info[2])
        if git(root, "diff", "--cached", "--name-only", "HEAD"):
            failures.append(("index", "index differs from first-release commit"))
        normal_entries = all(entry.startswith(b"H ") for entry in filter(
            None, git(root, "ls-files", "-v", "-z").split(b"\0")
        ))
        if not normal_entries or git(root, "status", "--porcelain=v1", "--untracked-files=all", "--ignored"):
            failures.append(("index", "working tree has changes, untracked files, or hidden index flags"))
    blobs = 0
    objects = git(root, "cat-file", "--batch-all-objects", "--batch-check=%(objectname) %(objecttype)")
    for item in objects.decode("ascii").splitlines():
        object_id, kind = item.split()
        label = f"git:{object_id[:12]}"
        if kind in {"commit", "tag"} and object_id != head:
            failures.append((label, "existing Git history or tag"))
        elif kind == "tree" and release_tree is not None and object_id not in release_tree:
            failures.append((label, "tree outside first-release commit"))
        elif kind == "blob":
            blobs += 1
            if object_id not in indexed:
                inspect(git(root, "cat-file", "blob", object_id), label, failures)
                failures.append((label, "blob outside current index"))
    return len(indexed), blobs


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, nargs="?", default=Path(__file__).resolve().parents[1])
    parser.add_argument("--first-release", action="store_true",
                        help="allow exactly one matching root commit, before adding a remote")
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    failures = []
    count = 0

    def walk_error(error):
        failures.append(("<filesystem>", type(error).__name__))

    for directory, dirs, files in os.walk(root, onerror=walk_error):
        for name in dirs + files:
            path = Path(directory) / name
            label = display(path.relative_to(root).as_posix())
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
                failures.append((label, "symbolic link or reparse point"))
                if name in dirs:
                    dirs.remove(name)
            elif name == ".git" and path == root / ".git" and name in dirs:
                dirs.remove(name)
            elif name in dirs:
                if name.lower() in FORBIDDEN_DIRS or name.lower().startswith(("build-", "build_")) or name == ".git":
                    failures.append((label, "generated or nested metadata directory"))
            elif not stat.S_ISREG(info.st_mode) or not is_source(name):
                failures.append((label, "file not on source allowlist"))
            else:
                count += 1
                inspect(path.read_bytes(), label, failures)

    try:
        indexed, blobs = inspect_index(root, args.first_release, failures)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        indexed, blobs = 0, 0
        failures.append((".git", type(error).__name__))
    for label, issue in failures:
        print(f"{label}: {issue}")
    print(f"{count} working-tree source files; {indexed} indexed source objects; "
          f"{blobs} Git blobs; {len(failures)} findings")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
