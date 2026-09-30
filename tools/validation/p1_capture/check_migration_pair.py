#!/usr/bin/env python3
"""Preflight provenance for an explicitly selected migration-capture pair.

This tool validates the supplied contract and manifests with the existing
capture comparator, then optionally checks that a same-host/same-backend pair
has the same measured runtime tuple.  It deliberately does not compare image
pixels: visual migration acceptance remains the responsibility of
``compare.py`` and its contract/policy/approval inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys
from typing import Any, Mapping


# ``compare.py`` is intentionally the single owner of the capture and
# contract schema.  The directory is not a Python package, so make its sibling
# importable for both direct execution and test/module loading.
_TOOL_DIRECTORY = pathlib.Path(__file__).resolve().parent
if str(_TOOL_DIRECTORY) not in sys.path:
    sys.path.insert(0, str(_TOOL_DIRECTORY))

import compare as capture_compare  # noqa: E402  (path setup is deliberate)


RUNTIME_MATCH_FIELDS = (
    "platform",
    "architecture",
    "engine_version",
    "backend",
    "graphics_api",
    "device",
    "driver",
    "driver_version",
)
RUNTIME_ALLOWED_DIFFERENCES = ("engine", "build_revision")
RUNTIME_FIELDS = tuple(sorted(capture_compare.RUNTIME_KEYS))
MODE_ALIASES = {
    "same-backend": "same-backend",
    "same-host": "same-backend",
    "same-host-same-backend": "same-backend",
    "cross-platform": "cross-platform",
    "cross-platform-contract": "cross-platform",
}


def _normalise_mode(mode: str) -> str:
    try:
        return MODE_ALIASES[mode.strip().lower()]
    except (AttributeError, KeyError) as error:
        choices = ", ".join(sorted(set(MODE_ALIASES)))
        raise ValueError(f"unsupported migration pair mode {mode!r}; choose one of {choices}") from error


def _runtime_value(value: Any) -> dict[str, Any]:
    """Return a stable, complete runtime tuple even for malformed manifests."""

    if not isinstance(value, Mapping):
        return {field: None for field in RUNTIME_FIELDS}
    return {field: value.get(field) for field in RUNTIME_FIELDS}


def _runtime_validation_errors(side: str, value: Any) -> list[dict[str, Any]]:
    """Use the comparator's text rule while retaining field-level diagnostics."""

    if not isinstance(value, Mapping):
        return [{
            "side": side,
            "field": "runtime",
            "kind": "missing",
            "value": None,
            "message": f"{side} manifest.runtime: expected an object",
        }]

    errors: list[dict[str, Any]] = []
    for field in RUNTIME_FIELDS:
        field_value = value.get(field)
        try:
            # Reuse the existing validator's exact empty/placeholder policy.
            capture_compare.require_text(field_value, f"{side} manifest.runtime.{field}")
        except capture_compare.HarnessError as error:
            kind = "missing" if field not in value else "empty-or-placeholder"
            errors.append({
                "side": side,
                "field": field,
                "kind": kind,
                "value": field_value,
                "message": str(error),
            })
    return errors


def _raw_manifest(path: pathlib.Path, side: str) -> tuple[Any | None, list[dict[str, Any]]]:
    try:
        value, _ = capture_compare.load_json(path, f"{side} manifest")
    except capture_compare.HarnessError as error:
        return None, [{
            "side": side,
            "field": "manifest",
            "kind": "invalid",
            "value": None,
            "message": str(error),
        }]
    return value, []


def _capture_summary(value: Any, capture: Mapping[str, Any] | None) -> dict[str, Any]:
    image: Any = value.get("image") if isinstance(value, Mapping) else None
    summary: dict[str, Any] = {
        "role": value.get("role") if isinstance(value, Mapping) else None,
        "image": image if isinstance(image, Mapping) else {},
    }
    if capture is not None:
        summary["image_sha256"] = capture.get("image_sha256")
    return summary


def _base_result(mode: str, contract: Mapping[str, Any] | None,
                 contract_hash: str | None, baseline_value: Any,
                 candidate_value: Any, baseline_capture: Mapping[str, Any] | None,
                 candidate_capture: Mapping[str, Any] | None) -> dict[str, Any]:
    baseline_runtime = baseline_value.get("runtime") if isinstance(baseline_value, Mapping) else None
    candidate_runtime = candidate_value.get("runtime") if isinstance(candidate_value, Mapping) else None
    baseline_tuple = _runtime_value(baseline_runtime)
    candidate_tuple = _runtime_value(candidate_runtime)
    result: dict[str, Any] = {
        "schema_version": 1,
        "tool": "check_migration_pair",
        "mode": mode,
        "contract_id": contract.get("contract_id") if isinstance(contract, Mapping) else None,
        "contract_sha256": contract_hash,
        "status": "failed",
        "passed": False,
        "evaluated": False,
        "visual_comparison": False,
        "scope": "provenance-preflight-only",
        "captures": {
            "baseline": _capture_summary(baseline_value, baseline_capture),
            "candidate": _capture_summary(candidate_value, candidate_capture),
        },
        # Keep complete tuples in a dedicated field even when a manifest is
        # malformed; diagnosis must not require reopening the two manifests.
        "runtime_tuples": {
            "baseline": baseline_tuple,
            "candidate": candidate_tuple,
        },
        "provenance": {
            "baseline": baseline_runtime if isinstance(baseline_runtime, Mapping) else {},
            "candidate": candidate_runtime if isinstance(candidate_runtime, Mapping) else {},
            "required_match_fields": list(RUNTIME_MATCH_FIELDS)
            if mode == "same-backend" else [],
            "allowed_different_fields": list(RUNTIME_ALLOWED_DIFFERENCES),
            "same_backend_check_applied": mode == "same-backend",
        },
    }
    return result


def _failure(result: dict[str, Any], reason_code: str, reason: str,
             errors: list[dict[str, Any]]) -> dict[str, Any]:
    result["reason_code"] = reason_code
    result["reason"] = reason
    result["errors"] = errors
    return result


def check_migration_pair(contract_path: pathlib.Path,
                         baseline_path: pathlib.Path,
                         candidate_path: pathlib.Path,
                         mode: str) -> dict[str, Any]:
    """Validate a pair and, for ``same-backend``, enforce tuple equality.

    The function returns a structured result for pair/evidence failures rather
    than raising, so callers can archive the two runtime tuples alongside the
    blocked result.  Contract/read failures are represented the same way.
    """

    contract_path = pathlib.Path(contract_path)
    baseline_path = pathlib.Path(baseline_path)
    candidate_path = pathlib.Path(candidate_path)

    try:
        canonical_mode = _normalise_mode(mode)
    except ValueError as error:
        return {
            "schema_version": 1,
            "tool": "check_migration_pair",
            "mode": mode,
            "status": "error",
            "passed": False,
            "evaluated": False,
            "visual_comparison": False,
            "scope": "provenance-preflight-only",
            "reason_code": "unsupported_mode",
            "reason": str(error),
            "errors": [{"kind": "mode", "message": str(error)}],
        }

    contract_value: Any = None
    contract_hash: str | None = None
    contract_errors: list[dict[str, Any]] = []
    try:
        contract_value, contract_raw = capture_compare.load_json(contract_path, "contract")
        contract_hash = hashlib.sha256(contract_raw).hexdigest()
    except capture_compare.HarnessError as error:
        contract_errors.append({
            "side": "contract",
            "field": "contract",
            "kind": "invalid",
            "value": None,
            "message": str(error),
        })

    baseline_value, baseline_read_errors = _raw_manifest(baseline_path, "baseline")
    candidate_value, candidate_read_errors = _raw_manifest(candidate_path, "candidate")
    baseline_capture: Mapping[str, Any] | None = None
    candidate_capture: Mapping[str, Any] | None = None
    capture_errors: list[dict[str, Any]] = []
    contract: Mapping[str, Any] | None = None

    if not contract_errors:
        try:
            contract = capture_compare.validate_contract(contract_value)
        except capture_compare.HarnessError as error:
            contract_errors.append({
                "side": "contract",
                "field": "contract",
                "kind": "invalid",
                "value": None,
                "message": str(error),
            })

    if not contract_errors and contract is not None and contract_hash is not None:
        if baseline_value is not None:
            try:
                baseline_capture = capture_compare.load_capture(
                    baseline_path, contract, contract_hash, "baseline"
                )
            except capture_compare.HarnessError as error:
                capture_errors.append({
                    "side": "baseline",
                    "field": "capture",
                    "kind": "invalid",
                    "value": None,
                    "message": str(error),
                })
        if candidate_value is not None:
            try:
                candidate_capture = capture_compare.load_capture(
                    candidate_path, contract, contract_hash, "candidate"
                )
            except capture_compare.HarnessError as error:
                capture_errors.append({
                    "side": "candidate",
                    "field": "capture",
                    "kind": "invalid",
                    "value": None,
                    "message": str(error),
                })

    result = _base_result(
        canonical_mode,
        contract,
        contract_hash,
        baseline_value,
        candidate_value,
        baseline_capture,
        candidate_capture,
    )

    runtime_errors = (
        _runtime_validation_errors(
            "baseline",
            baseline_value.get("runtime") if isinstance(baseline_value, Mapping) else None,
        )
        + _runtime_validation_errors(
            "candidate",
            candidate_value.get("runtime") if isinstance(candidate_value, Mapping) else None,
        )
    )
    read_errors = baseline_read_errors + candidate_read_errors
    all_capture_errors = read_errors + capture_errors
    if contract_errors:
        return _failure(result, "contract_validation_error", "contract validation failed", contract_errors)
    if read_errors:
        return _failure(result, "capture_input_error", "capture manifest input could not be read", read_errors)
    if runtime_errors:
        return _failure(
            result,
            "runtime_provenance_mismatch",
            "required runtime provenance is missing, empty, or a placeholder",
            runtime_errors + capture_errors,
        )
    if all_capture_errors:
        return _failure(
            result,
            "capture_validation_error",
            "capture manifest, identity, or image digest validation failed",
            all_capture_errors,
        )

    baseline_runtime = result["runtime_tuples"]["baseline"]
    candidate_runtime = result["runtime_tuples"]["candidate"]
    mismatches: list[dict[str, Any]] = []
    if canonical_mode == "same-backend":
        for field in RUNTIME_MATCH_FIELDS:
            baseline_value_for_field = baseline_runtime[field]
            candidate_value_for_field = candidate_runtime[field]
            if baseline_value_for_field != candidate_value_for_field:
                mismatches.append({
                    "field": field,
                    "kind": "mismatch",
                    "baseline": baseline_value_for_field,
                    "candidate": candidate_value_for_field,
                    "message": (
                        f"baseline and candidate {field} must match "
                        f"({baseline_value_for_field!r} != {candidate_value_for_field!r})"
                    ),
                })
    result["provenance"]["matched_fields"] = [
        field for field in RUNTIME_MATCH_FIELDS
        if baseline_runtime[field] == candidate_runtime[field]
    ]
    result["provenance"]["allowed_differences"] = {
        field: {
            "baseline": baseline_runtime[field],
            "candidate": candidate_runtime[field],
            "different": baseline_runtime[field] != candidate_runtime[field],
        }
        for field in RUNTIME_ALLOWED_DIFFERENCES
    }
    if mismatches:
        return _failure(
            result,
            "runtime_provenance_mismatch",
            "same-backend migration pair runtime provenance does not match",
            mismatches,
        )

    result.update({
        "status": "passed",
        "passed": True,
        "reason_code": "provenance_match" if canonical_mode == "same-backend"
        else "cross_platform_tuple_check_not_applicable",
        "reason": (
            "same-host/same-backend runtime provenance matches"
            if canonical_mode == "same-backend"
            else "cross-platform contract selected; runtime tuple equality was not applied"
        ),
    })
    return result


# A short alias makes the function convenient for callers that call this a
# preflight rather than a check, without introducing another implementation.
preflight = check_migration_pair


def _write_report(path: pathlib.Path, result: Mapping[str, Any]) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as error:
        raise capture_compare.HarnessError(f"cannot write report {path}: {error}") from error


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    arguments = list(sys.argv[1:] if argv is None else argv)
    # Support the documented ``check``/``preflight`` spelling without making a
    # subparser mandatory; this also keeps direct option invocation convenient.
    if arguments and arguments[0] in {"check", "preflight"}:
        arguments = arguments[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--contract", type=pathlib.Path, required=True)
    parser.add_argument("--baseline", type=pathlib.Path, required=True)
    parser.add_argument("--candidate", type=pathlib.Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--mode", "--comparison", dest="mode", choices=sorted(MODE_ALIASES))
    mode.add_argument("--same-backend", dest="mode", action="store_const", const="same-backend")
    mode.add_argument("--cross-platform", dest="mode", action="store_const", const="cross-platform")
    parser.add_argument("--report", type=pathlib.Path)
    return parser.parse_args(arguments)


def run(argv: list[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        result = check_migration_pair(
            args.contract,
            args.baseline,
            args.candidate,
            args.mode,
        )
        if args.report is not None:
            _write_report(args.report, result)
        print(json.dumps(result, indent=2, sort_keys=True))
        if result.get("status") == "passed":
            return 0
        if result.get("status") == "failed":
            return 1
        return 2
    except (capture_compare.HarnessError, OSError, ValueError) as error:
        result = {
            "schema_version": 1,
            "tool": "check_migration_pair",
            "status": "error",
            "passed": False,
            "evaluated": False,
            "visual_comparison": False,
            "scope": "provenance-preflight-only",
            "reason_code": "tool_error",
            "reason": str(error),
        }
        print(json.dumps(result, indent=2, sort_keys=True))
        return 2


if __name__ == "__main__":
    raise SystemExit(run())
