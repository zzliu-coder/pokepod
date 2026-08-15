#!/usr/bin/env python3
"""Validate the checked-in PokeCapsule schemas and JSON fixtures offline.

The repository deliberately keeps this validator dependency-free.  It covers
the JSON Schema vocabulary used by this protocol repository and, importantly,
resolves every local ``$ref`` before validating an instance.  A schema may
refer to another checked-in schema, but it may not silently reach outside the
protocol tree.
"""

from __future__ import annotations

import argparse
import copy
import datetime as _datetime
import json
from pathlib import Path
import re
import sys
import uuid
from urllib.parse import urldefrag


PROTOCOL_DIR = Path(__file__).resolve().parent
SCHEMA_SUFFIXES = (".schema.json",)
EXPECTED_INSTANCE_MAP = {
    "example/capsule.json": "capsule.schema.json",
    "example/move-command.json": "command.schema.json",
    "example/processing.json": "processing.schema.json",
    "fixtures/processing-v1-m4a.json": "processing.schema.json",
    "fixtures/processing-v2-m4a.json": "processing.schema.json",
    "fixtures/processing-v2-wav.json": "processing.schema.json",
}
EXPECTED_PROTOCOL_FILES = {
    "capsule.schema.json",
    "command.schema.json",
    "processing.schema.json",
    "processing-v1.schema.json",
    "processing-v2.schema.json",
    "trash.schema.json",
    "display-policy-fixtures.json",
    "protocol.json",
    "example/capsule.json",
    "example/move-command.json",
    "example/processing.json",
    "fixtures/processing-v1-m4a.json",
    "fixtures/processing-v2-m4a.json",
    "fixtures/processing-v2-wav.json",
}


class ValidationError(AssertionError):
    """A schema or instance failed the protocol contract."""


def load_json(path: Path) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ValidationError(f"invalid JSON: {path}: {exc}") from exc


def type_matches(value: object, expected: str) -> bool:
    if expected == "object":
        return isinstance(value, dict)
    if expected == "array":
        return isinstance(value, list)
    if expected == "string":
        return isinstance(value, str)
    if expected == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if expected == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if expected == "boolean":
        return isinstance(value, bool)
    if expected == "null":
        return value is None
    raise ValidationError(f"unsupported JSON Schema type: {expected}")


def json_pointer(root: object, fragment: str) -> object:
    if fragment in ("", "/"):
        return root
    if not fragment.startswith("/"):
        raise ValidationError(f"unsupported JSON pointer fragment: #{fragment}")
    current = root
    for raw_part in fragment[1:].split("/"):
        part = raw_part.replace("~1", "/").replace("~0", "~")
        if isinstance(current, dict) and part in current:
            current = current[part]
        elif isinstance(current, list) and part.isdigit() and int(part) < len(current):
            current = current[int(part)]
        else:
            raise ValidationError(f"JSON pointer does not exist: #{fragment}")
    return current


class SchemaStore:
    def __init__(self, root: Path):
        self.root = root.resolve()
        self.cache: dict[Path, object] = {}

    def _within_root(self, path: Path) -> Path:
        resolved = path.resolve()
        try:
            resolved.relative_to(self.root)
        except ValueError as exc:
            raise ValidationError(f"$ref escapes protocol root: {path}") from exc
        return resolved

    def load(self, path: Path) -> object:
        path = self._within_root(path)
        if path not in self.cache:
            if not path.is_file():
                raise ValidationError(f"$ref target missing: {path}")
            self.cache[path] = load_json(path)
        return self.cache[path]

    def resolve(self, current_file: Path, reference: str) -> tuple[Path, object]:
        raw_path, fragment = urldefrag(reference)
        if raw_path.startswith(("http:", "https:", "file:")):
            raise ValidationError(f"external $ref is forbidden: {reference}")
        target = current_file.parent / raw_path if raw_path else current_file
        document = self.load(target)
        return target.resolve(), json_pointer(document, fragment)


def check_format(value: object, format_name: str, location: str) -> None:
    if not isinstance(value, str):
        raise ValidationError(f"{location}: format {format_name} requires a string")
    if format_name == "uuid":
        try:
            uuid.UUID(value)
        except ValueError as exc:
            raise ValidationError(f"{location}: invalid UUID: {value!r}") from exc
        return
    if format_name == "date-time":
        try:
            parsed = value.replace("Z", "+00:00")
            _datetime.datetime.fromisoformat(parsed)
        except ValueError as exc:
            raise ValidationError(f"{location}: invalid date-time: {value!r}") from exc
        if "T" not in value and "t" not in value:
            raise ValidationError(f"{location}: date-time must include a time")
        return
    raise ValidationError(f"unsupported format: {format_name}")


def validate_instance(
    value: object,
    schema: object,
    store: SchemaStore,
    current_file: Path,
    location: str,
) -> None:
    if not isinstance(schema, dict):
        raise ValidationError(f"{location}: schema must be an object")

    if "$ref" in schema:
        reference = schema["$ref"]
        if not isinstance(reference, str):
            raise ValidationError(f"{location}: $ref must be a string")
        target_file, target_schema = store.resolve(current_file, reference)
        validate_instance(value, target_schema, store, target_file, location)

    if "allOf" in schema:
        for index, branch in enumerate(schema["allOf"]):
            validate_instance(value, branch, store, current_file, f"{location}.allOf[{index}]")

    if "anyOf" in schema:
        failures = []
        for index, branch in enumerate(schema["anyOf"]):
            try:
                validate_instance(value, branch, store, current_file, location)
            except ValidationError as exc:
                failures.append(f"branch {index}: {exc}")
        if len(failures) == len(schema["anyOf"]):
            raise ValidationError(f"{location}: anyOf failed; {'; '.join(failures)}")

    if "oneOf" in schema:
        passed = 0
        failures = []
        for index, branch in enumerate(schema["oneOf"]):
            try:
                validate_instance(value, branch, store, current_file, location)
                passed += 1
            except ValidationError as exc:
                failures.append(f"branch {index}: {exc}")
        if passed != 1:
            raise ValidationError(
                f"{location}: oneOf expected one branch, got {passed}; {'; '.join(failures)}"
            )

    if "if" in schema:
        condition_passed = True
        try:
            validate_instance(value, schema["if"], store, current_file, location)
        except ValidationError:
            condition_passed = False
        selected = schema.get("then") if condition_passed else schema.get("else")
        if selected is not None:
            validate_instance(value, selected, store, current_file, location)

    if "not" in schema:
        try:
            validate_instance(value, schema["not"], store, current_file, location)
        except ValidationError:
            pass
        else:
            raise ValidationError(f"{location}: not constraint matched")

    if "const" in schema and value != schema["const"]:
        raise ValidationError(f"{location}: expected const {schema['const']!r}, got {value!r}")
    if "enum" in schema and value not in schema["enum"]:
        raise ValidationError(f"{location}: value {value!r} is outside enum")

    expected_type = schema.get("type")
    if expected_type is not None:
        expected_types = expected_type if isinstance(expected_type, list) else [expected_type]
        if not any(type_matches(value, item) for item in expected_types):
            raise ValidationError(f"{location}: expected {expected_types}, got {type(value).__name__}")

    if "format" in schema:
        check_format(value, schema["format"], location)

    if isinstance(value, str):
        if "minLength" in schema and len(value) < schema["minLength"]:
            raise ValidationError(f"{location}: shorter than minLength")
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            raise ValidationError(f"{location}: longer than maxLength")
        if "pattern" in schema and re.search(schema["pattern"], value) is None:
            raise ValidationError(f"{location}: pattern mismatch")

    if isinstance(value, (int, float)) and not isinstance(value, bool):
        if "minimum" in schema and value < schema["minimum"]:
            raise ValidationError(f"{location}: below minimum")
        if "maximum" in schema and value > schema["maximum"]:
            raise ValidationError(f"{location}: above maximum")

    if isinstance(value, list):
        if "minItems" in schema and len(value) < schema["minItems"]:
            raise ValidationError(f"{location}: fewer than minItems")
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            raise ValidationError(f"{location}: more than maxItems")
        if schema.get("uniqueItems"):
            encoded = [json.dumps(item, sort_keys=True, ensure_ascii=False) for item in value]
            if len(set(encoded)) != len(encoded):
                raise ValidationError(f"{location}: duplicate array item")
        item_schema = schema.get("items")
        if item_schema is not None:
            for index, item in enumerate(value):
                validate_instance(item, item_schema, store, current_file, f"{location}[{index}]")

    if isinstance(value, dict):
        if "minProperties" in schema and len(value) < schema["minProperties"]:
            raise ValidationError(f"{location}: fewer than minProperties")
        if "maxProperties" in schema and len(value) > schema["maxProperties"]:
            raise ValidationError(f"{location}: more than maxProperties")
        required = schema.get("required", [])
        for name in required:
            if name not in value:
                raise ValidationError(f"{location}: missing required property {name!r}")
        properties = schema.get("properties", {})
        for name, property_schema in properties.items():
            if name in value:
                validate_instance(value[name], property_schema, store, current_file, f"{location}.{name}")
        if schema.get("additionalProperties") is False:
            unknown = sorted(set(value) - set(properties))
            if unknown:
                raise ValidationError(f"{location}: unknown properties {unknown}")
        additional = schema.get("additionalProperties")
        if isinstance(additional, dict):
            for name, item in value.items():
                if name not in properties:
                    validate_instance(item, additional, store, current_file, f"{location}.{name}")
        property_names = schema.get("propertyNames")
        if property_names is not None:
            for name in value:
                validate_instance(name, property_names, store, current_file, f"{location}.<property>")


def validate_schema_document(path: Path, store: SchemaStore) -> None:
    document = store.load(path)
    if not isinstance(document, dict):
        raise ValidationError(f"schema is not an object: {path}")
    if "$schema" not in document or "$id" not in document:
        raise ValidationError(f"schema missing $schema/$id: {path}")
    # Walk every object so missing local refs are caught even when no example
    # happens to exercise an optional branch.
    def walk(node: object, current: Path) -> None:
        if isinstance(node, dict):
            if "$ref" in node:
                reference = node["$ref"]
                if not isinstance(reference, str):
                    raise ValidationError(f"non-string $ref in {current}")
                store.resolve(current, reference)
            for child in node.values():
                walk(child, current)
        elif isinstance(node, list):
            for child in node:
                walk(child, current)

    walk(document, path)


def validate_protocol_contract(store: SchemaStore) -> None:
    payload = load_json(PROTOCOL_DIR / "protocol.json")
    if not isinstance(payload, dict):
        raise ValidationError("protocol.json must be an object")
    expected = {
        "name": "PokeCapsule",
        "schemaVersion": 2,
        "root": "/sdcard/PokeCapsule",
        "reservedFolders": ["Inbox", "Archive"],
        "maxUserFolderDepth": 2,
    }
    for key, value in expected.items():
        if payload.get(key) != value:
            raise ValidationError(f"protocol.json.{key} does not match the frozen contract")
    files = payload.get("capsuleFiles")
    if not isinstance(files, dict) or files.get("metadata") != "capsule.json":
        raise ValidationError("protocol.json capsuleFiles metadata contract is invalid")
    if files.get("processing") != "processing.json":
        raise ValidationError("protocol.json processing filename contract is invalid")
    if files.get("audioCandidates") != ["audio.m4a", "audio.wav"]:
        raise ValidationError("protocol.json audio candidate contract is invalid")

    display = load_json(PROTOCOL_DIR / "display-policy-fixtures.json")
    if not isinstance(display, dict) or display.get("schemaVersion") != 1:
        raise ValidationError("display-policy-fixtures.json schemaVersion is invalid")
    cases = display.get("cases")
    if not isinstance(cases, list) or not cases:
        raise ValidationError("display-policy-fixtures.json has no cases")
    for index, case in enumerate(cases):
        if not isinstance(case, dict):
            raise ValidationError(f"display policy case {index} is not an object")
        for key in ("name", "durationMs", "status", "finalText", "polishedText", "rawText", "expected"):
            if key not in case:
                raise ValidationError(f"display policy case {index} missing {key}")
        if not isinstance(case["durationMs"], int) or case["durationMs"] < 0:
            raise ValidationError(f"display policy case {index} duration is invalid")


def validate_tree(root: Path) -> list[str]:
    root = root.resolve()
    store = SchemaStore(root)
    schema_paths = sorted(root.glob("*.schema.json"))
    if not schema_paths:
        raise ValidationError(f"no protocol schemas found in {root}")
    for path in schema_paths:
        validate_schema_document(path, store)

    observed = {
        path.relative_to(root).as_posix()
        for path in root.rglob("*.json")
        if path.is_file()
    }
    unknown = sorted(observed - EXPECTED_PROTOCOL_FILES)
    missing = sorted(EXPECTED_PROTOCOL_FILES - observed)
    if unknown:
        raise ValidationError(f"unregistered protocol JSON files: {unknown}")
    if missing:
        raise ValidationError(f"protocol JSON files missing from validator contract: {missing}")

    for relative_name, schema_name in EXPECTED_INSTANCE_MAP.items():
        instance_path = root / relative_name
        schema_path = root / schema_name
        validate_instance(
            load_json(instance_path),
            store.load(schema_path),
            store,
            schema_path,
            relative_name,
        )
    validate_protocol_contract(store)
    return [path.relative_to(root).as_posix() for path in schema_paths]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=PROTOCOL_DIR,
        help="protocol directory (defaults to this script's directory)",
    )
    args = parser.parse_args(argv)
    try:
        schemas = validate_tree(args.root)
    except ValidationError as exc:
        print(f"FAIL protocol_fixtures: {exc}", file=sys.stderr)
        return 1
    print(f"PASS protocol_fixtures schemas={len(schemas)} instances={len(EXPECTED_INSTANCE_MAP)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
