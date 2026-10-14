"""Download the community modding wiki (focumentation.fandom.com) for local research.

The wiki's text is CC BY-SA. The download is research input only: it goes to an ignored
folder (default `out/research/focumentation/`) and is never committed. Committed notes cite
the page title and URL and describe findings in the project's own words.

Writes one `pages/<title>.wiki` file per page (latest revision wikitext) and a
`MANIFEST.json` with the namespace, page and revision IDs, timestamp and SHA-256 of each.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path

API = "https://focumentation.fandom.com/api.php"
SITE = "https://focumentation.fandom.com/wiki/"
# Main, Project, Template, Category. File pages carry no text beyond captions.
NAMESPACES = (0, 4, 10, 14)
USER_AGENT = "EmpireAtWar-Revised research fetch (https://github.com/EaW-Revised/EaW-Revised)"


def api(params: dict[str, str]) -> dict:
    query = urllib.parse.urlencode({**params, "format": "json", "formatversion": "2"})
    request = urllib.request.Request(f"{API}?{query}", headers={"User-Agent": USER_AGENT})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, timeout=60) as response:
                return json.load(response)
        except OSError:
            if attempt == 3:
                raise
            time.sleep(2 ** (attempt + 1))
    raise AssertionError("unreachable")


def all_titles(namespace: int) -> list[str]:
    titles: list[str] = []
    params = {"action": "query", "list": "allpages", "apnamespace": str(namespace), "aplimit": "500"}
    while True:
        data = api(params)
        titles += [page["title"] for page in data["query"]["allpages"]]
        if "continue" not in data:
            return titles
        params.update(data["continue"])


def fetch_batch(titles: list[str]) -> list[dict]:
    data = api({
        "action": "query",
        "prop": "revisions",
        "rvprop": "ids|timestamp|content",
        "rvslots": "main",
        "titles": "|".join(titles),
    })
    return data["query"]["pages"]


def file_name(title: str) -> str:
    return re.sub(r'[<>:"/\\|?*]', "_", title) + ".wiki"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, default=Path("out/research/focumentation"))
    args = parser.parse_args()

    pages_dir = args.out / "pages"
    pages_dir.mkdir(parents=True, exist_ok=True)
    manifest = []
    for namespace in NAMESPACES:
        titles = all_titles(namespace)
        for start in range(0, len(titles), 50):
            for page in fetch_batch(titles[start:start + 50]):
                revision = page["revisions"][0]
                text = revision["slots"]["main"]["content"]
                name = file_name(page["title"])
                (pages_dir / name).write_text(text, encoding="utf-8")
                manifest.append({
                    "title": page["title"],
                    "namespace": namespace,
                    "page_id": page["pageid"],
                    "revision_id": revision["revid"],
                    "timestamp": revision["timestamp"],
                    "url": SITE + urllib.parse.quote(page["title"].replace(" ", "_")),
                    "file": f"pages/{name}",
                    "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
                })
    manifest.sort(key=lambda row: (row["namespace"], row["title"]))
    (args.out / "MANIFEST.json").write_text(json.dumps(manifest, indent=1) + "\n", encoding="utf-8")
    print(f"{len(manifest)} pages -> {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
