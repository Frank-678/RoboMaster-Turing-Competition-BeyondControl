"""冻结真实提交的离线策略worker；不改当前工作区，不复制practice，不导出凭据。"""
import hashlib
import json
from pathlib import Path
import re
import subprocess

from run_scenarios import ROOT, digest

CPP = ["src/rmtc_bot/src/strategy.cpp", "src/rmtc_bot/src/tactics.cpp", "src/rmtc_bot/src/events.cpp"]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n")


def git_sources(repository, ref):
    """从Git对象读固定提交；即使本地有未提交改动也不混进历史版。"""
    def git(*args):
        return subprocess.run(["git", "-C", str(repository), *args], capture_output=True, check=True).stdout
    commit = git("rev-parse", "--verify", ref + "^{commit}").decode().strip()
    paths = git("ls-tree", "-r", "--name-only", commit, "src/rmtc_bot/include").decode().splitlines() + CPP
    sources = {path: git("show", commit + ":" + path) for path in paths}
    return sources, {"kind": "git_commit", "commit": commit}


def locked_sources(directory, lock_path):
    """仅供已从远端取得的快照：逐个校验Git blob，不把目录旁写的SHA当作证据。"""
    lock = json.loads(Path(lock_path).read_text())
    if not re.fullmatch(r"[0-9a-f]{40}", lock["commit"]):
        raise ValueError("snapshot lock requires full commit SHA")
    sources = {}
    for path, expected in lock["git_blobs"].items():
        relative = Path(path)
        if relative.is_absolute() or ".." in relative.parts or not (path in CPP or path.startswith("src/rmtc_bot/include/")):
            raise ValueError("snapshot lock path outside strategy sources")
        content = (Path(directory) / path).read_bytes()
        blob = hashlib.sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest()
        if blob != expected:
            raise ValueError("snapshot Git blob mismatch: " + path)
        sources[path] = content
    if not set(CPP).issubset(sources):
        raise ValueError("snapshot lock lacks strategy/tactics/events source")
    return sources, {"kind": "locked_git_snapshot", "commit": lock["commit"], "repository": lock["repository"],
                     "lock_sha256": sha256(lock_path)}


def parameter_names(header):
    """读取静态competition()之前的算术成员，生成编译期字段访问；遇到新类型就报错。"""
    declarations = header.split("struct StrategyParameters", 1)[1].split("static StrategyParameters", 1)[0]
    declarations = re.sub(r"//[^\n]*|/\*.*?\*/", "", declarations, flags=re.S).lstrip(" \n{")
    names = []
    for declaration in declarations.split(";"):
        declaration = declaration.strip()
        if not declaration:
            continue
        match = re.fullmatch(r"(?:double|int|bool)\s+(.+)", declaration, re.S)
        if not match:
            raise ValueError("unsupported StrategyParameters declaration: " + declaration)
        for field in match[1].split(","):
            name = field.strip().split("=", 1)[0].strip()
            if not re.fullmatch(r"[A-Za-z_]\w*", name):
                raise ValueError("unsupported strategy field: " + field)
            names.append(name)
    if not names or len(names) != len(set(names)):
        raise ValueError("empty/duplicate strategy parameters")
    return names


def freeze(label, sources, provenance, destination, include=None):
    """每个label只能对应一个源码+适配器+编译器组合；创建后拒绝覆盖，改版请用新label。"""
    if not re.fullmatch(r"[A-Za-z0-9_-]+", label):
        raise ValueError("version label allows letters/digits/_/-")
    folder = Path(destination).resolve() / label
    if folder.exists():
        raise ValueError("version already frozen; use another label: " + label)
    compiler = subprocess.run(["g++", "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    adapter = ROOT / "tests/version_worker.cpp"
    header = sources["src/rmtc_bot/include/rmtc_bot/strategy_parameters.hpp"].decode()
    fields = parameter_names(header)
    generated = "\n".join('values["' + name + '"]=parameters.' + name + ';' for name in fields) + "\n"
    source_hashes = {path: hashlib.sha256(content).hexdigest() for path, content in sorted(sources.items())}
    manifest = {"schema": 1, "label": label, "provenance": provenance, "source_sha256": source_hashes,
                "adapter_sha256": sha256(adapter), "compiler": compiler,
                "flags": ["-std=c++17", "-O1", "-Wall", "-Wextra", "-Wpedantic", "-Werror"],
                "scope": "offline strategy worker, no ROS/HTTP; adapter is current harness"}
    json_header = Path(include) / "nlohmann/json.hpp" if include else Path("/usr/include/nlohmann/json.hpp")
    manifest["json_header_sha256"] = sha256(json_header)
    manifest["build_id"] = digest(manifest)
    # 直到检查全部通过才创建冻结目录。编译失败留下failed.json，不能误当成可用版本。
    folder.mkdir(parents=True)
    for path, content in sources.items():
        target = folder / "source" / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(content)
    (folder / "version_parameters.inc").write_text(generated)
    (folder / "version_worker.cpp").write_bytes(adapter.read_bytes())
    binary = folder / "worker"
    command = ["g++", *manifest["flags"], "-I", str(folder / "source/src/rmtc_bot/include"), "-I", str(folder)]
    if include:
        command += ["-I", str(Path(include).resolve())]
    try:
        subprocess.run(command + [str(folder / "source" / path) for path in CPP] +
                       [str(folder / "version_worker.cpp"), "-o", str(binary)], check=True)
        manifest["binary_sha256"] = sha256(binary)
        completed = subprocess.run([str(binary)], input='{"op":"init","stage":2}\n',
                                   capture_output=True, text=True, timeout=10, check=True)
        manifest["parameters"] = json.loads(completed.stdout)["parameters"]
        write_json(folder / "manifest.json", manifest)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        write_json(folder / "failed.json", {"error": str(error), "build": manifest})
        raise
    return folder, manifest


def load_version(folder):
    """每场前验证二进制/源码/适配器未变；不悄悄重编译历史冠军。"""
    folder = Path(folder).resolve()
    manifest = json.loads((folder / "manifest.json").read_text())
    identity_fields = {k: value for k, value in manifest.items() if k not in ("build_id", "binary_sha256", "parameters")}
    if digest(identity_fields) != manifest["build_id"]:
        raise ValueError("frozen manifest changed")
    if sha256(folder / "worker") != manifest["binary_sha256"]:
        raise ValueError("frozen binary changed: " + str(folder))
    for path, expected in manifest["source_sha256"].items():
        if sha256(folder / "source" / path) != expected:
            raise ValueError("frozen source changed: " + path)
    if sha256(folder / "version_worker.cpp") != manifest["adapter_sha256"]:
        raise ValueError("frozen adapter changed")
    return folder / "worker", manifest
