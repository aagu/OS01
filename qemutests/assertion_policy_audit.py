#!/usr/bin/env python3
"""Static lint enforcing the assertion layering policy.

Detects Python tests/audits that read production source files
(``kernel/**``, ``boot/**``, ``user/**``, ``libc/**``, ``runtime/**``,
``include/**``) via ``open()`` / ``read_text()`` / ``read_bytes()`` and
subsequently match or assert on their content via ``re.search`` / ``in``
without a valid exemption in ``config/assertion-exemptions.json``.

Fail-closed:
- Missing or malformed exemption registry -> exit 1
- ``structural-invariant`` exemption missing non-empty ``policy_ref`` -> exit 1
- Any unexempted production source read + text assertion -> exit 1 with ``file:line``
"""

from __future__ import annotations

import argparse
import ast
import json
from pathlib import Path
import sys
from typing import Iterable

REPO_ROOT = Path(__file__).resolve().parents[1]
PROD_DIRS = ("kernel", "boot", "user", "libc", "runtime", "include")
DEFAULT_SCAN_DIRS = ("qemutests", "tools")
SELF_FILES = {
    "qemutests/assertion_policy_audit.py",
    "qemutests/test_assertion_policy_audit.py",
}
VALID_REASON_CLASSES = {"structural-invariant", "pending"}
REGEX_FUNCS = {"search", "match", "findall", "finditer", "fullmatch"}
ASSERT_FUNCS = {"assertIn", "assertNotIn", "assertRegex", "assertNotRegex"}


def _is_prod_string(s: str) -> bool:
    norm = s.replace("\\", "/")
    for d in PROD_DIRS:
        if norm == d or norm.startswith(d + "/") or ("/" + d + "/") in norm:
            return True
    return False


def _expr_has_prod_path(
    node: ast.AST,
    env: dict[str, list[ast.AST]],
    visited: set[str] | None = None,
    memo: dict[str, bool] | None = None,
) -> bool:
    if visited is None:
        visited = set()
    if memo is None:
        memo = {}
    for sub in ast.walk(node):
        if isinstance(sub, ast.Constant) and isinstance(sub.value, str):
            if _is_prod_string(sub.value):
                return True
        elif isinstance(sub, ast.Name) and sub.id in env:
            if sub.id in memo:
                if memo[sub.id]:
                    return True
                continue
            if sub.id in visited:
                continue
            visited.add(sub.id)
            found = False
            for bound in env[sub.id]:
                if _expr_has_prod_path(bound, env, visited, memo):
                    found = True
                    break
            memo[sub.id] = found
            if found:
                return True
    return False


def _is_write_open(call: ast.Call) -> bool:
    mode_node: ast.AST | None = None
    if len(call.args) >= 2:
        mode_node = call.args[1]
    else:
        for kw in call.keywords:
            if kw.arg == "mode":
                mode_node = kw.value
                break
    if isinstance(mode_node, ast.Constant) and isinstance(mode_node.value, str):
        return any(ch in mode_node.value for ch in ("w", "a", "x"))
    return False


def _collect_env(tree: ast.AST) -> dict[str, list[ast.AST]]:
    env: dict[str, list[ast.AST]] = {}

    def bind(target: ast.AST, value: ast.AST) -> None:
        if isinstance(target, ast.Name):
            env.setdefault(target.id, []).append(value)
        elif isinstance(target, (ast.Tuple, ast.List)):
            for elt in target.elts:
                bind(elt, value)

    for node in ast.walk(tree):
        if isinstance(node, ast.Assign):
            for tgt in node.targets:
                bind(tgt, node.value)
        elif isinstance(node, ast.AnnAssign) and node.value is not None:
            bind(node.target, node.value)
        elif isinstance(node, ast.For):
            bind(node.target, node.iter)
        elif isinstance(node, ast.comprehension):
            bind(node.target, node.iter)
    return env


def _collect_read_helpers(tree: ast.AST) -> dict[str, int]:
    """Return {func_name: param_index} for helpers that directly read a path param."""
    helpers: dict[str, int] = {}
    for node in ast.walk(tree):
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        params = [a.arg for a in node.args.args]
        if not params:
            continue
        for sub in ast.walk(node):
            if not isinstance(sub, ast.Call):
                continue
            fn = (
                sub.func.id
                if isinstance(sub.func, ast.Name)
                else (sub.func.attr if isinstance(sub.func, ast.Attribute) else "")
            )
            target_expr: ast.AST | None = None
            if fn in ("read_text", "read_bytes") and isinstance(sub.func, ast.Attribute):
                target_expr = sub.func.value
            elif fn == "open" and sub.args and not _is_write_open(sub):
                target_expr = sub.args[0]
            if isinstance(target_expr, ast.Name) and target_expr.id in params:
                helpers[node.name] = params.index(target_expr.id)
                break
    return helpers


def scan_python_file(path: Path, rel: str) -> list[tuple[int, str]]:
    """Return [(lineno, detail), ...] if file reads production source and asserts on content."""
    try:
        src = path.read_text(encoding="utf-8", errors="replace")
        tree = ast.parse(src, filename=rel)
    except (OSError, SyntaxError):
        return []

    env = _collect_env(tree)
    read_helpers = _collect_read_helpers(tree)
    prod_reads: list[tuple[int, str]] = []
    has_text_assert = False

    for node in ast.walk(tree):
        if isinstance(node, ast.Compare):
            if any(isinstance(op, (ast.In, ast.NotIn)) for op in node.ops):
                has_text_assert = True
        elif isinstance(node, ast.Call):
            fn = (
                node.func.id
                if isinstance(node.func, ast.Name)
                else (node.func.attr if isinstance(node.func, ast.Attribute) else "")
            )
            if fn in REGEX_FUNCS or fn in ASSERT_FUNCS:
                has_text_assert = True

            if fn in ("read_text", "read_bytes") and isinstance(node.func, ast.Attribute):
                if _expr_has_prod_path(node.func.value, env):
                    prod_reads.append((node.lineno, f"{fn}() on production path"))
            elif fn == "open" and node.args and not _is_write_open(node):
                if _expr_has_prod_path(node.args[0], env):
                    prod_reads.append((node.lineno, "open() on production path"))
            elif isinstance(node.func, ast.Name) and node.func.id in read_helpers:
                idx = read_helpers[node.func.id]
                if idx < len(node.args) and _expr_has_prod_path(node.args[idx], env):
                    prod_reads.append(
                        (node.lineno, f"{node.func.id}() on production path")
                    )

    if prod_reads and has_text_assert:
        # Report the first production read line in the file for clear file:line diagnostics.
        prod_reads.sort(key=lambda item: item[0])
        return [prod_reads[0]]
    return []


def load_exemptions(exemptions_path: Path) -> tuple[dict[str, dict], list[str]]:
    errors: list[str] = []
    if not exemptions_path.is_file():
        return {}, [f"{exemptions_path}: missing assertion exemptions file"]
    try:
        data = json.loads(exemptions_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return {}, [f"{exemptions_path}: failed to parse JSON: {exc}"]

    if not isinstance(data, dict) or not isinstance(data.get("exemptions"), list):
        return {}, [f"{exemptions_path}: expected object with 'exemptions' list"]

    by_path: dict[str, dict] = {}
    for idx, entry in enumerate(data["exemptions"]):
        if not isinstance(entry, dict):
            errors.append(f"{exemptions_path}: exemptions[{idx}] must be an object")
            continue
        rel = entry.get("path")
        reason_class = entry.get("reason_class")
        if not isinstance(rel, str) or not rel.strip():
            errors.append(f"{exemptions_path}: exemptions[{idx}] missing 'path'")
            continue
        if rel in by_path:
            errors.append(f"{exemptions_path}: duplicate exemption path '{rel}'")
            continue
        if reason_class not in VALID_REASON_CLASSES:
            errors.append(
                f"{exemptions_path}: '{rel}' has invalid reason_class {reason_class!r}"
            )
            continue
        if reason_class == "structural-invariant":
            policy_ref = entry.get("policy_ref")
            if not isinstance(policy_ref, str) or not policy_ref.strip():
                errors.append(
                    f"{exemptions_path}: '{rel}' has reason_class='structural-invariant' "
                    "without non-empty 'policy_ref'"
                )
                continue
        by_path[rel] = entry
    return by_path, errors


def iter_python_files(repo_root: Path, scan_dirs: Iterable[str]) -> Iterable[tuple[Path, str]]:
    for rel_dir in scan_dirs:
        base = repo_root / rel_dir
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*.py")):
            if "__pycache__" in path.parts:
                continue
            rel = path.relative_to(repo_root).as_posix()
            if rel in SELF_FILES:
                continue
            yield path, rel


def run_audit(
    repo_root: Path,
    exemptions_path: Path,
    scan_dirs: Iterable[str] = DEFAULT_SCAN_DIRS,
) -> tuple[int, list[str]]:
    exemptions, errors = load_exemptions(exemptions_path)
    violations: list[str] = list(errors)

    for path, rel in iter_python_files(repo_root, scan_dirs):
        hits = scan_python_file(path, rel)
        if not hits:
            continue
        if rel in exemptions:
            continue
        for lineno, detail in hits:
            violations.append(
                f"{rel}:{lineno}: {detail} with text assertion "
                "(not exempted in config/assertion-exemptions.json)"
            )

    return (1 if violations else 0), violations


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", type=Path, default=REPO_ROOT)
    ap.add_argument("--exemptions", type=Path, default=None)
    ap.add_argument("--scan-dirs", nargs="+", default=list(DEFAULT_SCAN_DIRS))
    args = ap.parse_args(argv)

    repo_root = args.repo_root.resolve()
    exemptions_path = (
        args.exemptions.resolve()
        if args.exemptions is not None
        else repo_root / "config" / "assertion-exemptions.json"
    )
    rc, messages = run_audit(repo_root, exemptions_path, args.scan_dirs)
    for msg in messages:
        print(msg, file=sys.stderr)
    if rc == 0:
        print("assertion-policy-audit: PASS")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
