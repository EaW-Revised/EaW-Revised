"""Independent pinned-LSP "Show Effective Object" export and EAWR comparison (P0-05).

``run`` verifies the pinned pg-starwarsgame-lsp and eaw-schema checkouts, writes the public
selection, lays out a ``.pgproj`` workspace over the installed Remake mod (a directory junction,
never a copy or an edit), builds and runs ``tools/inventory/lsp_effective_export`` and, when an
``xml_scan`` binary is given, captures the EAWR side and compares. ``compare`` reruns only the
comparison. Everything content-rich stays under the ignored output directory.

The LSP side never sees EAWR output: the export harness receives only the workspace, the schema
and object IDs, and hashes each input buffer itself. EAWR data enters only ``compare``.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[2]
LSP_REVISION = "4461416d401b0f665bc1fe82aad60b90bd707fa9"
SCHEMA_REVISION = "3e1b825a124fbc13b2293665f34a36dd4d4be80f"
HARNESS = ROOT / "tools" / "inventory" / "lsp_effective_export"
PRODUCER = "tools/inventory/lsp_effective_export"
PROVENANCE_KINDS = ("own", "inherited", "added", "overridden", "merged")
# Differences in how the same data is represented, each explained where it is classified. They
# stay in the matrix and are counted separately; every other class is a divergence.
REPRESENTATION = ("type-name-granularity", "whitespace-trim", "eol-normalization", "nested-inner-text")
# The pinned XmlGameDocumentParser types every GameObjectFiles entry with this registry type name.
LSP_REGISTRY_TYPE = "GameObjectType"
# XML 1.0 white space (production S). HtmlAgilityPack InnerText.Trim() would also remove other
# Unicode spaces; such a value is deliberately left to fail as value-text rather than be assumed.
XML_WHITESPACE = " \t\r\n"
# Keys the extended xml_scan --sample emits for every effective occurrence and nested element.
EAWR_STRUCTURE_KEYS = ("attributes", "children")
# How a matched value or base value was matched.
MATCH_BASES = ("exact", "whitespace-trim", "eol-normalization", "nested-inner-text")
# Fields only EAWR scanner output carries; their presence in LSP-side data means contamination.
EAWR_ONLY_KEYS = ("raw_chain", "effective_values", "displaced_raw_text", "samples")
# Pinned server sources compiled into the harness by link; hashed so the report pins them.
LINKED_SERVER_SOURCES = (
    "PG.StarWarsGame.LSP.Server/Variants/GetEffectiveObjectHandler.cs",
    "PG.StarWarsGame.LSP.Server/Variants/GetEffectiveObjectParams.cs",
    "PG.StarWarsGame.LSP.Server/Variants/GetEffectiveObjectResult.cs",
    "PG.StarWarsGame.LSP.Server/LspConfigurationProvider.cs",
    "PG.StarWarsGame.LSP.Server/Project/IModProjectDetector.cs",
    "PG.StarWarsGame.LSP.Server/Project/ModProjectDetector.cs",
    "PG.StarWarsGame.LSP.Server/Project/ModProjectLoader.cs",
    "PG.StarWarsGame.LSP.Server/Project/ModProjectLoadException.cs",
    "PG.StarWarsGame.LSP.Server/Project/ModProjectResolver.cs",
    "PG.StarWarsGame.LSP.Server/Project/PgprojMigrations.cs",
    "PG.StarWarsGame.LSP.Server/Project/ProjectDependencyGraph.cs",
    # Ported, not linked: recorded so a reviewer can diff the port against the pinned original.
    "PG.StarWarsGame.LSP.Server/WorkspaceIndexer.cs",
    "PG.StarWarsGame.LSP.Server/Project/ModProjectReloadService.cs",
    # Genuine services the export drives.
    "PG.StarWarsGame.LSP.Core/Symbols/EffectiveObjectResolver.cs",
    "PG.StarWarsGame.LSP.Core/Symbols/EffectiveObjectXmlRenderer.cs",
    "PG.StarWarsGame.LSP.Core/Symbols/GameIndex.cs",
    "PG.StarWarsGame.LSP.Core/Symbols/GameIndexService.cs",
    "PG.StarWarsGame.LSP.Xml/Variants/WorkspaceVariantTagSource.cs",
    "PG.StarWarsGame.LSP.Xml/Util/XmlParseCache.cs",
    "PG.StarWarsGame.LSP.Xml/Parsing/XmlGameDocumentParser.cs",
)


class IndependenceError(ValueError):
    """The LSP-side evidence is not independent of EAWR output."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def _walk_keys(node: Any):
    if isinstance(node, dict):
        for key, value in node.items():
            yield key
            yield from _walk_keys(value)
    elif isinstance(node, list):
        for item in node:
            yield from _walk_keys(item)


def check_lsp_independence(structured: Dict[str, Any], manifest: Dict[str, Any]) -> None:
    """Refuse LSP-side evidence that could have been derived from EAWR output."""
    if structured.get("producer") != PRODUCER:
        raise IndependenceError("structured results were not produced by the independent export harness")
    if not str(manifest.get("hashed_by", "")).startswith("lsp-effective-export pre-index"):
        raise IndependenceError("input hashes were not taken by the export harness before indexing")
    for document, name in ((structured, "structured-results"), (manifest, "input-manifest")):
        for key in _walk_keys(document):
            if key in EAWR_ONLY_KEYS:
                raise IndependenceError(f"{name} carries EAWR-only field {key!r}")
    for entry in manifest.get("chain_input_files", []):
        before, after = entry.get("sha256_before_index"), entry.get("sha256_after_export")
        lsp_hash = entry.get("lsp_project_file_hasher_sha256")
        if not before or before != after or before != lsp_hash:
            raise IndependenceError(f"input custody broken for {entry.get('logical_path')}")


def xml_strip(value: Optional[str]) -> Optional[str]:
    return None if value is None else value.strip(XML_WHITESPACE)


def eol_normalize(value: Optional[str]) -> Optional[str]:
    """XML 1.0 section 2.11 end-of-line handling: CRLF and lone CR become LF."""
    return None if value is None else value.replace("\r\n", "\n").replace("\r", "\n")


def fragment_tree(fragment: str, first_line: int) -> Dict[str, Any]:
    """Element tree of an LSP verbatim fragment: ordered attributes, direct text, all descendant
    text, children and absolute 1-based lines. Comments are dropped and line endings normalized,
    as any conforming XML processor does; tokens are otherwise untouched."""
    import xml.parsers.expat

    roots: List[Dict[str, Any]] = []
    stack: List[Dict[str, Any]] = []
    parser = xml.parsers.expat.ParserCreate()
    parser.ordered_attributes = True

    def start(name: str, attributes: List[str]) -> None:
        node = {"name": name, "text": "", "descendant_text": "",
                "line": first_line + parser.CurrentLineNumber - 1,
                "attributes": [[attributes[i], attributes[i + 1]] for i in range(0, len(attributes), 2)],
                "children": []}
        if stack:
            stack[-1]["children"].append(node)
        else:
            roots.append(node)
        stack.append(node)

    def end(_: str) -> None:
        stack.pop()

    def text(data: str) -> None:
        if stack:
            stack[-1]["text"] += data
            for open_node in stack:
                open_node["descendant_text"] += data

    parser.StartElementHandler = start
    parser.EndElementHandler = end
    parser.CharacterDataHandler = text
    parser.Parse(fragment, True)
    return roots[0]


def missing_structure(eawr_node: Dict[str, Any]) -> List[str]:
    return [key for key in EAWR_STRUCTURE_KEYS if key not in eawr_node]


def compare_nested(lsp_node: Dict[str, Any], eawr_node: Dict[str, Any], path: str,
                   differences: List[Dict[str, Any]], index: int, stats: Dict[str, int]) -> int:
    """Compare nested element trees in order; returns the number of nested elements compared.

    An EAWR node without ``attributes``/``children`` (for example output of an older xml_scan)
    is a divergence, never an empty structure. ``stats["nested_text_trimmed"]`` counts nested
    texts that are equal only after XML end-of-line handling and trimming."""
    missing = missing_structure(eawr_node)
    if missing:
        differences.append(_difference(f"nested{path}", index, list(EAWR_STRUCTURE_KEYS), missing,
                                       "eawr-structure-missing"))
        return 0
    compared = 0
    lsp_attributes = [list(a) for a in lsp_node["attributes"]]
    eawr_attributes = [[a["name"], a["value"]] for a in eawr_node["attributes"]]
    if lsp_attributes != eawr_attributes:
        differences.append(_difference(f"nested{path}/@", index, lsp_attributes, eawr_attributes,
                                       "nested-content"))
    lsp_children, eawr_children = lsp_node["children"], eawr_node["children"]
    for position in range(max(len(lsp_children), len(eawr_children))):
        if position >= len(lsp_children) or position >= len(eawr_children):
            differences.append(_difference(
                f"nested{path}[{position}]", index,
                lsp_children[position]["name"] if position < len(lsp_children) else None,
                eawr_children[position]["name"] if position < len(eawr_children) else None,
                "nested-content"))
            continue
        a, b = lsp_children[position], eawr_children[position]
        child_path = f"{path}/{a['name']}[{position}]"
        compared += 1
        lsp_text, eawr_text = xml_strip(a["text"]), xml_strip(eol_normalize(b["raw_text"]))
        if lsp_text == eawr_text and a["text"] != b["raw_text"]:
            stats["nested_text_trimmed"] += 1
        for field, left, right in (("name", a["name"], b["name"]),
                                   ("text", lsp_text, eawr_text),
                                   ("line", a["line"], b["line"])):
            if left != right:
                differences.append(_difference(f"nested{child_path}.{field}", index, left, right,
                                               "nested-content"))
        compared += compare_nested(a, b, child_path, differences, index, stats)
    return compared


def _difference(field: str, index: Optional[int], lsp: Any, eawr: Any, classification: str) -> Dict[str, Any]:
    return {"field": field, "index": index, "lsp": lsp, "eawr": eawr, "classification": classification}


def compare_object(lsp: Dict[str, Any], eawr: Optional[Dict[str, Any]],
                   public_hashes: Optional[List[str]] = None) -> Dict[str, Any]:
    """Compare one LSP export against one EAWR sample, preserving and classifying every difference."""
    object_id = lsp["requested_id"]
    differences: List[Dict[str, Any]] = []
    row: Dict[str, Any] = {"object_id": object_id}
    if eawr is None or "error" in eawr:
        differences.append(_difference("object", None, lsp.get("found"), (eawr or {}).get("error"),
                                       "missing-in-eawr"))
        row.update({"differences": differences, "divergences": len(differences)})
        return row

    lsp_chain = [c["object_id"] for c in lsp["chain"]]
    rpc_chain = list(lsp["rpc_result"]["chain"])
    row["chain"] = lsp_chain
    row["chain_depth"] = len(lsp_chain)
    row["chain_equal"] = lsp_chain == eawr["chain"]
    if rpc_chain != lsp_chain:
        differences.append(_difference("rpc_chain", None, rpc_chain, lsp_chain, "lsp-internal-inconsistency"))
    if not row["chain_equal"]:
        differences.append(_difference("chain", None, lsp_chain, eawr["chain"], "chain"))
    # The pinned parser types every GameObjectFiles entry with the registry type (GameObjectType);
    # EAWR reports the authored element name. Only that known case is a granularity difference, and
    # only when the element the harness read at the LSP's own origin line is EAWR's type name.
    # Any other type mismatch, or a missing origin element, is a divergence.
    lsp_type, eawr_type = lsp.get("type_name"), eawr.get("type_name")
    origin_element = lsp["chain"][0].get("origin_element_name") if lsp["chain"] else None
    row["type_name"] = {"lsp": lsp_type, "eawr": eawr_type, "lsp_origin_element_name": origin_element}
    if lsp_type != eawr_type:
        known = lsp_type == LSP_REGISTRY_TYPE and origin_element is not None and origin_element == eawr_type
        differences.append(_difference("type_name", None, lsp_type, eawr_type,
                                       "type-name-granularity" if known else "type-name"))

    # Base-chain definitions: file, line and independently taken input hash per layer.
    raw_by_id = {r["id"].lower(): r for r in eawr.get("raw_chain", []) if r}
    layers = []
    hashes_equal = True
    for position, layer in enumerate(lsp["chain"]):
        raw = raw_by_id.get(layer["object_id"].lower())
        origin = layer.get("origin") or {}
        entry = {
            "object_id": layer["object_id"],
            "resolved_from": layer.get("resolved_from"),
            "winner_order_dependent": layer.get("winner_order_dependent"),
            "lsp_logical_path": origin.get("logical_path"),
            "lsp_line1": origin.get("line1"),
            "lsp_input_sha256": origin.get("input_sha256"),
        }
        if raw is None:
            differences.append(_difference("chain_definition", position, layer["object_id"], None,
                                           "missing-in-eawr"))
        else:
            source = raw["source"]
            entry.update({"eawr_logical_path": source["logical_path"], "eawr_line": source["line"],
                          "eawr_layer": source.get("layer"), "eawr_input_sha256": raw.get("input_sha256")})
            if origin.get("logical_path") != source["logical_path"]:
                differences.append(_difference("chain_definition.file", position, origin.get("logical_path"),
                                               source["logical_path"], "origin-file"))
            if origin.get("line1") != source["line"]:
                differences.append(_difference("chain_definition.line", position, origin.get("line1"),
                                               source["line"], "origin-line"))
            if not origin.get("input_sha256") or not raw.get("input_sha256"):
                hashes_equal = False
                differences.append(_difference("chain_definition.input_sha256", position,
                                               origin.get("input_sha256"), raw.get("input_sha256"),
                                               "input-hash-missing"))
            elif origin.get("input_sha256") != raw.get("input_sha256"):
                hashes_equal = False
                differences.append(_difference("chain_definition.input_sha256", position,
                                               origin.get("input_sha256"), raw.get("input_sha256"),
                                               "input-hash"))
        if layer.get("resolved_from") != "workspace":
            differences.append(_difference("chain_definition.resolved_from", position,
                                           layer.get("resolved_from"), "workspace", "baseline-or-missing"))
        if layer.get("winner_order_dependent"):
            differences.append(_difference("chain_definition.winner", position, True, False,
                                           "lsp-duplicate-order-dependent"))
        layers.append(entry)
    row["chain_definitions"] = layers

    lsp_hashes = sorted({l["lsp_input_sha256"] for l in layers if l.get("lsp_input_sha256")})
    row["lsp_input_sha256"] = lsp_hashes
    if public_hashes is not None:
        row["public_selection_hashes_equal"] = lsp_hashes == sorted(set(public_hashes))
        if not row["public_selection_hashes_equal"]:
            hashes_equal = False
            differences.append(_difference("input_sha256", None, lsp_hashes, sorted(set(public_hashes)),
                                           "input-hash"))
    row["input_hashes_equal"] = hashes_equal

    # Ordered effective values.
    lsp_values = lsp["values"]
    eawr_values = eawr["effective_values"]
    row["value_count"] = {"lsp": len(lsp_values), "eawr": len(eawr_values)}
    fields = ("name", "value", "provenance", "source_object_id", "origin_file", "origin_line", "base_value")
    matched = {name: 0 for name in fields}
    exact = {"value": 0, "base_value": 0}
    # How each matched value/base value was matched; every basis but "exact" is also a classified
    # difference in the matrix.
    basis = {field: {kind: 0 for kind in MATCH_BASES} for field in ("value", "base_value")}
    stats = {"nested_text_trimmed": 0}
    trimmed = 0
    nested = 0
    nested_compared = 0
    for i in range(max(len(lsp_values), len(eawr_values))):
        if i >= len(lsp_values) or i >= len(eawr_values):
            side = "missing-in-lsp" if i >= len(lsp_values) else "missing-in-eawr"
            differences.append(_difference("value", i, lsp_values[i]["name"] if i < len(lsp_values) else None,
                                           eawr_values[i]["name"] if i < len(eawr_values) else None, side))
            continue
        a, b = lsp_values[i], eawr_values[i]
        origin = a.get("origin") or {}
        eawr_source = b.get("source") or {}
        eawr_value = b["raw_text"]
        if eawr_value != xml_strip(eawr_value):
            trimmed += 1
        checks = (
            ("name", a["name"], b["name"], "ordered-name"),
            ("provenance", a["provenance"], b["provenance"], "provenance-kind"),
            ("source_object_id", a["source_object_id"], b["source_object_id"], "source-object"),
            ("origin_file", origin.get("logical_path"), eawr_source.get("logical_path"), "origin-file"),
            ("origin_line", origin.get("line1"), eawr_source.get("line"), "origin-line"),
        )
        for name, left, right, classification in checks:
            if left == right:
                matched[name] += 1
            else:
                differences.append(_difference(name, i, left, right, classification))

        # Occurrence structure: the LSP's verbatim fragment against EAWR's nested tree.
        tree = None
        try:
            tree = fragment_tree(a["fragment"], origin.get("line1") or 1)
        except Exception as error:  # noqa: BLE001 - an unparseable fragment is itself evidence
            differences.append(_difference("fragment", i, str(error), None, "lsp-fragment-unparseable"))
        structure_missing = missing_structure(b)
        has_nested = bool(tree and tree["children"]) or bool(b.get("children"))
        if has_nested:
            nested += 1
        if tree is not None:
            nested_compared += compare_nested(tree, b, "/" + a["name"], differences, i, stats)

        # Value. The pinned WorkspaceVariantTagSource stores HtmlAgilityPack InnerText.Trim() and
        # keeps the file's line endings; EAWR raw_text is the untrimmed direct text after XML
        # end-of-line handling. Each non-exact match is classified and kept: whitespace-trim when
        # only leading/trailing XML white space differs, eol-normalization when CRLF/CR also
        # differ, and nested-inner-text for an occurrence with child elements, whose LSP Value
        # joins all descendant text. That class needs the LSP Value to equal the fragment's own
        # descendant text and the fragment's direct text to equal EAWR raw_text; the descendants
        # themselves are covered by the nested comparison above.
        lsp_value = a["value"]
        if lsp_value == eawr_value:
            value_class = "exact"
        elif lsp_value == xml_strip(eawr_value):
            value_class = "whitespace-trim"
        elif has_nested and tree is not None and not structure_missing and \
                eol_normalize(lsp_value) == xml_strip(eol_normalize(tree["descendant_text"])) and \
                xml_strip(eol_normalize(tree["text"])) == xml_strip(eol_normalize(eawr_value)):
            value_class = "nested-inner-text"
        elif eol_normalize(lsp_value) == xml_strip(eol_normalize(eawr_value)):
            value_class = "eol-normalization"
        else:
            value_class = "value-text"
        _record_value("value", i, lsp_value, eawr_value, value_class, matched, exact, basis, differences)

        lsp_base, eawr_base = a.get("base_value"), b.get("displaced_raw_text")
        if lsp_base == eawr_base:
            base_class = "exact"
        elif lsp_base is not None and eawr_base is not None and lsp_base == xml_strip(eawr_base):
            base_class = "whitespace-trim"
        elif lsp_base is not None and eawr_base is not None and \
                eol_normalize(lsp_base) == xml_strip(eol_normalize(eawr_base)):
            base_class = "eol-normalization"
        else:
            base_class = "displaced-value"
        _record_value("base_value", i, lsp_base, eawr_base, base_class, matched, exact, basis, differences)
    row["matched_fields"] = matched
    row["exact_without_normalization"] = exact
    row["match_basis"] = basis
    row["eawr_values_trimmed_for_comparison"] = trimmed
    row["nested_texts_equal_only_after_trim"] = stats["nested_text_trimmed"]
    row["values_with_nested_elements"] = nested
    row["nested_elements_compared"] = nested_compared
    row["provenance_counts"] = {kind: sum(1 for v in lsp_values if v["provenance"] == kind)
                                for kind in PROVENANCE_KINDS}
    row["rpc_xml_sha256"] = lsp["rpc_result"]["xml_sha256"]
    row["structured_render_equals_rpc_xml"] = lsp["structured_render_equals_rpc_xml"]
    if not lsp["structured_render_equals_rpc_xml"]:
        differences.append(_difference("rendered_xml", None, False, True, "lsp-internal-inconsistency"))
    unverified = [v["index"] for v in lsp_values if v.get("origin_line_verified") is not True]
    if unverified:
        differences.append(_difference("origin_line_verified", None, unverified, [], "line-normalization"))

    row["differences"] = differences
    row["representation_differences"] = sum(1 for d in differences if d["classification"] in REPRESENTATION)
    row["divergences"] = sum(1 for d in differences if d["classification"] not in REPRESENTATION)
    return row


def _record_value(field: str, index: int, lsp_value: Optional[str], eawr_value: Optional[str],
                  classification: str, matched: Dict[str, int], exact: Dict[str, int],
                  basis: Dict[str, Dict[str, int]], differences: List[Dict[str, Any]]) -> None:
    if classification in basis[field]:
        matched[field] += 1
        basis[field][classification] += 1
    if classification == "exact":
        exact[field] += 1
        return
    difference = _difference(field, index, lsp_value, eawr_value, classification)
    if classification in ("eol-normalization", "nested-inner-text"):
        # Recorded so that trimming inside these classes is visible too.
        difference["eawr_whitespace_trimmed"] = eawr_value != xml_strip(eawr_value)
    differences.append(difference)


def compare_custody(manifest: Dict[str, Any], receipt: Dict[str, Any]) -> List[Dict[str, Any]]:
    """LSP-side pre-index hashes of each chain file against EAWR's loaded-include receipt."""
    loaded: Dict[str, List[str]] = {}
    for row in receipt.get("includes", []):
        if row.get("outcome") == "loaded" and row.get("sha256"):
            loaded.setdefault(row["logical_path"].lower(), []).append(row["sha256"])
    rows = []
    for entry in manifest.get("chain_input_files", []):
        path = entry["logical_path"]
        digests = sorted(set(loaded.get(path, [])))
        rows.append({"logical_path": path, "lsp_sha256": entry["sha256_before_index"],
                     "eawr_receipt_sha256": digests,
                     "active_include": bool(digests),
                     "equal": digests == [entry["sha256_before_index"]]})
    return rows


def compare(structured: Dict[str, Any], manifest: Dict[str, Any], eawr_report: Dict[str, Any],
            public_selection: List[Dict[str, Any]],
            receipt: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
    check_lsp_independence(structured, manifest)
    expected = [s["object_id"].lower() for s in public_selection]
    if len(set(expected)) != len(expected):
        raise IndependenceError("selection IDs are duplicated")
    actual_lsp = [r["requested_id"].lower() for r in structured["results"]]
    if actual_lsp != expected:
        raise IndependenceError("LSP result IDs do not match selection")
    actual_eawr = [s.get("requested_id", s.get("object_id", "")).lower() for s in eawr_report.get("samples", [])]
    if actual_eawr != expected:
        raise IndependenceError("EAWR sample IDs do not match selection")
    samples = {s.get("requested_id", s.get("object_id", "")).lower(): s for s in eawr_report.get("samples", [])}
    public = {s["object_id"].lower(): s for s in public_selection}
    rows = []
    for result in structured["results"]:
        key = result["requested_id"].lower()
        rows.append(compare_object(result, samples.get(key),
                                   public.get(key, {}).get("input_sha256")))
    classes: Dict[str, int] = {}
    for row in rows:
        for difference in row["differences"]:
            classes[difference["classification"]] = classes.get(difference["classification"], 0) + 1
    totals = {kind: sum(r.get("provenance_counts", {}).get(kind, 0) for r in rows) for kind in PROVENANCE_KINDS}
    return {
        "schema_version": 1,
        "lsp_revision": LSP_REVISION,
        "schema_revision": SCHEMA_REVISION,
        "selection_count": len(public_selection),
        "compared": len(rows),
        "objects_without_divergence": sum(1 for r in rows if r["divergences"] == 0),
        "objects_without_any_difference_beyond_type_name": sum(
            1 for r in rows
            if all(d["classification"] == "type-name-granularity" for d in r["differences"])),
        "values_compared": sum(min(r["value_count"]["lsp"], r["value_count"]["eawr"])
                               for r in rows if "value_count" in r),
        "nested_elements_compared": sum(r.get("nested_elements_compared", 0) for r in rows),
        "nested_texts_equal_only_after_trim": sum(r.get("nested_texts_equal_only_after_trim", 0) for r in rows),
        "match_basis_totals": {
            field: {kind: sum(r["match_basis"][field][kind] for r in rows if "match_basis" in r)
                    for kind in MATCH_BASES}
            for field in ("value", "base_value")},
        "representation_classes": list(REPRESENTATION),
        "lsp_provenance_totals": totals,
        "difference_classes": dict(sorted(classes.items())),
        "input_custody": compare_custody(manifest, receipt) if receipt is not None else None,
        "matrix": rows,
    }


def _git(path: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(path), *args], check=True, capture_output=True,
                          text=True).stdout.strip()


def verify_pin(path: Path, revision: str) -> Dict[str, Any]:
    head = _git(path, "rev-parse", "HEAD")
    dirty = _git(path, "status", "--porcelain", "--untracked-files=no")
    if head != revision or dirty:
        raise SystemExit(f"{path} is not a clean checkout of {revision} (HEAD {head}, dirty={bool(dirty)})")
    return {"revision": head, "clean_tracked_tree": True}


class CommandLog:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.lines: List[str] = []

    def run(self, command: List[str], **kwargs: Any) -> subprocess.CompletedProcess:
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        self.lines.append(f"# {stamp}\n" + subprocess.list2cmdline(command))
        result = subprocess.run(command, **kwargs)
        self.lines.append(f"# exit {result.returncode}")
        self.flush()
        return result

    def note(self, text: str) -> None:
        self.lines.append("# " + text)
        self.flush()

    def flush(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text("\n".join(self.lines) + "\n", encoding="utf-8")


def make_workspace(out: Path, mod_root: Path, log: CommandLog) -> Path:
    workspace = out / "workspace"
    workspace.mkdir(parents=True, exist_ok=True)
    link = workspace / "remake"
    if not link.exists():
        if os.name == "nt":
            import _winapi  # directory junction: a read-only view, the mod itself is untouched
            _winapi.CreateJunction(str(mod_root), str(link))
        else:
            link.symlink_to(mod_root, target_is_directory=True)
        log.note(f"linked workspace/remake -> {mod_root}")
    pgproj = {"name": "EmpireAtWarRemake", "directories": {"xml": ["remake/Data/XML"]}}
    (workspace / "remake.pgproj").write_text(json.dumps(pgproj) + "\n", encoding="utf-8")
    return workspace


def run(args: argparse.Namespace) -> int:
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    log = CommandLog(out / "commands.txt")
    lsp_root, schema_root = Path(args.lsp_root).resolve(), Path(args.schema_root).resolve()
    pins = {"lsp": verify_pin(lsp_root, LSP_REVISION), "schema": verify_pin(schema_root, SCHEMA_REVISION)}
    pins["lsp"]["source_sha256"] = {
        rel: sha256_bytes((lsp_root / rel).read_bytes()) for rel in LINKED_SERVER_SOURCES}
    pins["eawr_commit"] = _git(ROOT, "rev-parse", "HEAD")
    # Evidence is tied to eawr_commit only when the harness, comparator and scanner sources are
    # that commit's; a modified tracked tree is refused unless explicitly allowed and recorded.
    dirty = _git(ROOT, "status", "--porcelain", "--untracked-files=no")
    if dirty and not args.allow_dirty:
        raise SystemExit(f"{ROOT} has uncommitted tracked changes; commit first or pass --allow-dirty")
    pins["eawr_tracked_tree_clean"] = not dirty
    log.note(f"pins lsp={LSP_REVISION} schema={SCHEMA_REVISION} eawr={pins['eawr_commit']} "
             f"eawr_tracked_tree_clean={not dirty}")

    summary = json.loads((ROOT / "plan/inventories/xml-load-summary.json").read_text(encoding="utf-8"))
    public_selection = summary["lsp_reference_comparison"]["selection"]
    selection = {
        "source": "plan/inventories/xml-load-summary.json#lsp_reference_comparison.selection",
        "profile": summary["lsp_reference_comparison"]["profile"],
        "object_ids": [s["object_id"] for s in public_selection],
        "pins": pins,
    }
    write_json(out / "selection.json", selection)

    workspace = make_workspace(out, Path(args.mod_root).resolve(), log)
    dotnet = shutil.which("dotnet") or "dotnet"
    build = log.run([dotnet, "build", str(HARNESS / "LspEffectiveExport.csproj"), "-c", "Release",
                     f"-p:LspRoot={lsp_root}"])
    if build.returncode:
        return build.returncode
    exe = ROOT / "out/lsp_effective_export/bin/Release/net10.0" / (
        "LspEffectiveExport.exe" if os.name == "nt" else "LspEffectiveExport")
    schema_eaw = schema_root / "eaw"
    test = log.run([str(exe), "--self-test", "--schema", str(schema_eaw)])
    if test.returncode:
        return test.returncode
    export = log.run([str(exe), "--workspace", str(workspace), "--schema", str(schema_eaw),
                      "--selection", str(out / "selection.json"), "--out", str(out)])
    if not args.xml_scan:
        log.note("no --xml-scan given; EAWR side and comparison skipped")
        return export.returncode

    eawr_dir = out / "eawr"
    xml_scan = Path(args.xml_scan).resolve()
    log.note(f"xml_scan sha256={sha256_bytes(xml_scan.read_bytes())}")
    command = [str(xml_scan), "--profile", "remake", "--game-root", args.game_root, "--mod-root",
               args.mod_root, "--report", str(eawr_dir / "remake-samples.json"),
               "--input-receipt", str(eawr_dir / "remake-input-receipt.json")]
    for object_id in selection["object_ids"]:
        command += ["--sample", object_id]
    eawr_dir.mkdir(parents=True, exist_ok=True)
    log.run(command)  # exits 1 for retained corpus findings; the report is still complete
    return compare_files(out, log) or export.returncode


def compare_files(out: Path, log: Optional[CommandLog] = None) -> int:
    structured = json.loads((out / "structured-results.json").read_text(encoding="utf-8"))
    manifest = json.loads((out / "input-manifest.json").read_text(encoding="utf-8"))
    eawr = json.loads((out / "eawr" / "remake-samples.json").read_text(encoding="utf-8"))
    summary = json.loads((ROOT / "plan/inventories/xml-load-summary.json").read_text(encoding="utf-8"))
    receipt_path = out / "eawr" / "remake-input-receipt.json"
    receipt = json.loads(receipt_path.read_text(encoding="utf-8")) if receipt_path.exists() else None
    result = compare(structured, manifest, eawr, summary["lsp_reference_comparison"]["selection"], receipt)
    if receipt is not None:
        result["eawr_input_receipt_sha256"] = sha256_bytes(receipt_path.read_bytes())
        result["eawr_input_receipt_identity_sha256"] = receipt.get("identity_sha256")
    result["structured_results_sha256"] = sha256_bytes((out / "structured-results.json").read_bytes())
    result["input_manifest_sha256"] = sha256_bytes((out / "input-manifest.json").read_bytes())
    result["eawr_samples_sha256"] = sha256_bytes((out / "eawr" / "remake-samples.json").read_bytes())
    write_json(out / "comparison.json", result)
    if log is not None:
        log.note("comparison written to comparison.json")
    print(f"compared={result['compared']} without_divergence={result['objects_without_divergence']} "
          f"values={result['values_compared']} classes={result['difference_classes']}")
    complete = result["compared"] == result["selection_count"] == 20
    custody = result["input_custody"] is not None and all(r["equal"] for r in result["input_custody"])
    return 0 if complete and custody and result["objects_without_divergence"] == result["compared"] else 1


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run_parser = sub.add_parser("run")
    run_parser.add_argument("--lsp-root", required=True)
    run_parser.add_argument("--schema-root", required=True)
    run_parser.add_argument("--mod-root", required=True)
    run_parser.add_argument("--game-root", required=True)
    run_parser.add_argument("--xml-scan")
    run_parser.add_argument("--allow-dirty", action="store_true",
                            help="run from a tree with uncommitted tracked changes (recorded in selection.json)")
    run_parser.add_argument("--out", default=str(ROOT / "out/p0-05-independent-lsp"))
    compare_parser = sub.add_parser("compare")
    compare_parser.add_argument("--out", default=str(ROOT / "out/p0-05-independent-lsp"))
    args = parser.parse_args(argv)
    if args.command == "run":
        return run(args)
    return compare_files(Path(args.out).resolve())


if __name__ == "__main__":
    sys.exit(main())
