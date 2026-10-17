#!/usr/bin/env python3
"""Worker watchdog: notices silent workers, idle PR READY workers, unreported failures and quiet coordinators.

    python tools/watchdog/worker_watchdog.py --config tools/watchdog/config.local.json          # service loop
    python tools/watchdog/worker_watchdog.py --config ... --once [--dry-run]                    # one poll, then exit

Every poll it lists the active workers of the lane runs (out/coord/chief/coordinators.json), reads only the NEW
transcript messages of each worker (the Orca worker-read cursor is kept in the state file), lists the open pull
requests and the Windows pool journals, runs the detectors and acts on the findings:

  1. append them to <coord>/watchdog/alerts.md and rewrite <coord>/watchdog/status.json,
  2. send ONE message per lane to that lane's coordinator,
  3. send the chief a status when the same finding survives `escalate_after_polls` polls in a row.

It never messages a worker and never stops, restarts or releases anything. docs/watchdog.md explains the alerts.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import copy
import datetime as dt
import json
import logging
import logging.handlers
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0
LOG = logging.getLogger("watchdog")

DEFAULTS = {
    "poll_seconds": 600,
    # Defaults below are derived from coord_dir when left empty.
    "coord_dir": "",
    "registry": "",
    "state_file": "",
    "out_dir": "",
    "pool_journal_dir": "",
    "thresholds": {
        "silent_minutes": 45,
        "pr_ready_minutes": 60,
        "failed_minutes": 30,
        "unread_mail_minutes": 15,
        "coordinator_quiet_minutes": 30,
        "pool_wait_target_minutes": 30,
        "pool_oldest_wait_minutes": 90,
        "pool_min_samples": 2,
        "dedupe_minutes": 30,
        "escalate_after_polls": 3,
        "chief_renotify_minutes": 60,
        "background_stale_minutes": 60,
        "read_fail_red_polls": 3,
    },
    "read_retry_pause_seconds": 2,
    "max_pages_per_worker": 30,
    "read_threads": 6,
    "mail_limit": 300,
    "mail_subject_regex": r"\b(?:FIX|RESUME|ACTION|REBASE|CONFLICT|NEEDED)\b",
    "waiter_script": "wait_real.py",
    "min_waiters": 1,
}

JOB_RE = re.compile(r"\b(jobs?|queued?|queues?|slots?|tickets?|windows_build|offload|pool|pwl\d+|monitor|build|waiting for)\b", re.I)
FAIL_RE = re.compile(r"\b(fail(?:ed|ure|ures|ing|s)?|timed out|time out|timeout|crash(?:ed|es)?|red|exit code [1-9]\d*)\b", re.I)
NO_FAIL_RE = re.compile(r"\b(?:0|no|zero|without|not)\s+(?:new\s+|further\s+|real\s+)?(?:fail\w*|crash\w*|timeouts?)\b", re.I)
FIXED_RE = re.compile(r"\b(fixed|resolved|green)\b", re.I)
REPORT_RE = re.compile(r"orchestration\s+(?:send|ask)\b|worker_done|worker-done", re.I)
# The protocol's status line is `PR READY <n> <sha>`: upper case, one space, an optional number.
PR_READY_RE = re.compile(r"(?<![\w#])PR READY(?:[ \t]+#?(\d{3,5}))?(?!\w)")
# "I will send PR READY once ..." is a plan, not a report.
PR_READY_PLAN_RE = re.compile(r"\b(?:will|once|until|before|when|if|not|next)\b[^.!?\n]{0,30}\bPR READY\b", re.I)
RECENT = 10
STATE_TABLES = ("workers", "coordinators", "logged", "sent", "streak", "chief_sent", "key_meta", "mail", "read_fail")
BACKGROUND_KEEP = 50  # open background tasks remembered per transcript


# --------------------------------------------------------------------------------------------- configuration

def load_config(path=None):
    cfg = copy.deepcopy(DEFAULTS)
    if path:
        user = json.loads(Path(path).read_text(encoding="utf-8-sig"))
        for key, value in user.items():
            if key == "thresholds":
                cfg["thresholds"].update(value)
            elif not key.startswith("_"):
                cfg[key] = value
    coord = Path(cfg["coord_dir"] or ROOT / "out" / "coord")
    cfg["coord_dir"] = str(coord)
    cfg["registry"] = cfg["registry"] or str(coord / "chief" / "coordinators.json")
    cfg["out_dir"] = cfg["out_dir"] or str(coord / "watchdog")
    cfg["state_file"] = cfg["state_file"] or str(Path(cfg["out_dir"]) / "state.json")
    return cfg


def load_registry(cfg):
    reg = json.loads(Path(cfg["registry"]).read_text(encoding="utf-8-sig"))
    lanes = [{"tag": lane["tag"].upper(), "name": lane.get("name", lane["tag"]), "run": lane["run"],
              "dispatch": lane.get("dispatch", "")} for lane in reg["lanes"] if lane.get("run")]
    return {"chief_run": reg["chief_run"], "repo": reg.get("repo", ""), "lanes": lanes}


# --------------------------------------------------------------------------------------------- live backend

class LiveBackend:
    """All outside-world access in one place; tests replace it with a fake."""

    def __init__(self, cfg, registry):
        self.cfg = cfg
        self.registry = registry
        self.orca_exe = shutil.which("orca.exe") or shutil.which("orca") or "orca"

    def _run(self, argv, timeout=300):
        return subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace",
                              timeout=timeout, creationflags=CREATE_NO_WINDOW)

    def _orca(self, *args, timeout=300):
        try:
            p = self._run([self.orca_exe, "orchestration", *args, "--json"], timeout)
        except subprocess.TimeoutExpired:
            return {"ok": False, "error": {"code": "timeout", "message": f"orca {args[0]} timed out after {timeout} s"}}
        out = (p.stdout or "") + (p.stderr if not p.stdout else "")
        i = out.find("{")
        if i < 0:
            return {"ok": False, "error": {"code": "no_json", "message": out.strip()[:300]}}
        try:
            return json.JSONDecoder().raw_decode(out[i:])[0]
        except ValueError:
            return {"ok": False, "error": {"code": "bad_json", "message": out[i:i + 300]}}

    def active_workers(self, run):
        """[{dispatch, name}] for every ready worker whose terminal is active."""
        workers, cursor = [], None
        for _ in range(10):
            args = ["worker-list", "--run", run, "--terminal-state", "active", "--limit", "100"]
            if cursor:
                args += ["--cursor", cursor]
            r = self._orca(*args)
            if not r.get("ok"):
                raise RuntimeError(f"worker-list {run}: {(r.get('error') or {}).get('message')}")
            res = r["result"]
            for w in res.get("workers", []):
                if w.get("workerState") == "ready" and w.get("terminalState") == "active":
                    wt = ((w.get("resource") or {}).get("worktreeId") or "")
                    workers.append({"dispatch": w["dispatchId"], "name": re.split(r"[\\/]", wt)[-1] or w["dispatchId"]})
            page = res.get("page") or {}
            cursor = page.get("nextCursor")
            if not page.get("hasMore") or not cursor:
                break
        return workers

    def read_new(self, dispatch, cursor):
        """New transcript messages after `cursor`: (messages, next_cursor). Raises when the cursor is refused."""
        messages, pages = [], 0
        while pages < self.cfg["max_pages_per_worker"]:
            args = ["worker-read", "--dispatch", dispatch, "--source", "transcript", "--limit", "50"]
            if cursor:
                args += ["--cursor", cursor]
            r = self._orca(*args)
            if not r.get("ok"):
                raise RuntimeError((r.get("error") or {}).get("code") or "worker-read failed")
            t = r["result"]["transcript"]
            page = t.get("messages") or []
            cursor = t.get("nextCursor") or cursor
            pages += 1
            messages.extend(page)
            if len(page) < 50:  # a short page is the end of the transcript; saves one empty read per worker
                break
        else:
            LOG.warning("%s: page cap reached after %d messages; the rest is read on the next poll", dispatch, len(messages))
        return messages, cursor

    def open_prs(self):
        repo = self.registry.get("repo")
        argv = ["gh", "pr", "list", "--state", "open", "--limit", "200",
                "--json", "number,title,isDraft,updatedAt,headRefName"]
        if repo:
            argv += ["--repo", repo]
        p = self._run(argv)
        if p.returncode != 0:
            raise RuntimeError("gh pr list: " + (p.stderr or "").strip()[:200])
        return json.loads(p.stdout)

    def waiter_count(self):
        script = self.cfg["waiter_script"].replace("'", "''")
        cmd = ("Get-CimInstance Win32_Process -Filter \"Name LIKE 'python%'\" | "
               f"Where-Object {{ $_.CommandLine -like '*{script}*' }} | Measure-Object | Select-Object -ExpandProperty Count")
        p = self._run(["powershell", "-NoProfile", "-NonInteractive", "-Command", cmd], timeout=60)
        try:
            return int(p.stdout.strip())
        except ValueError:
            return None

    def worker_mail(self):
        """Recent messages addressed to a dispatch: [{id, dispatch, created_ms, subject, type}]."""
        r = self._orca("inbox", "--limit", str(self.cfg["mail_limit"]), timeout=300)
        if not r.get("ok"):
            raise RuntimeError("inbox: " + str((r.get("error") or {}).get("message"))[:160])
        mail = []
        for m in (r.get("result") or {}).get("messages", []):
            to = m.get("to_handle") or ""
            created = m.get("created_at") or ""
            if not to.startswith("dispatch:"):
                continue
            try:
                created_ms = dt.datetime.fromisoformat(created.replace("Z", "+00:00")).timestamp() * 1000
            except ValueError:
                continue
            mail.append({"id": m.get("id"), "dispatch": to[len("dispatch:"):], "created_ms": created_ms,
                         "subject": m.get("subject") or "", "type": m.get("type") or ""})
        return mail

    def pool_status(self, now):
        sys.path.insert(0, str(ROOT / "tools" / "offload"))
        import client_journal
        import queue_wait_report
        directory = Path(self.cfg["pool_journal_dir"]) if self.cfg["pool_journal_dir"] else client_journal.journal_directory()
        if not directory.is_dir():
            raise RuntimeError(f"journal directory missing: {directory}")
        rep = queue_wait_report.report(directory, now)
        oldest = None
        for path in directory.glob("*.json"):
            try:
                row = json.loads(path.read_text(encoding="utf-8-sig"))
            except (OSError, ValueError):
                continue
            if not isinstance(row, dict) or row.get("platform") != "windows" or row.get("state") != "waiting":
                continue
            enq = (queue_wait_report.timestamp(row.get("enqueued_at")) or queue_wait_report.timestamp(row.get("submitted_utc"))
                   or queue_wait_report.timestamp(row.get("created_at")))
            if enq is not None and (oldest is None or enq < oldest["enqueued"]):
                oldest = {"enqueued": enq, "job": str(row.get("job", path.stem))}
        overall = rep["overall"]["1h"]
        return {"avg_1h_seconds": overall["average_seconds"], "samples_1h": overall["samples"],
                "queued": rep["overall"]["queued"], "oldest_waiting_seconds": (now - oldest["enqueued"]) if oldest else None,
                "oldest_job": oldest["job"] if oldest else None}

    def address(self, lane):
        """Where a lane's coordinator reads its mail. A coordinator is a dispatch of the chief run, so a dispatch
        address is refused from outside; its terminal handle is accepted and lands in the lane's own mailbox."""
        if lane["tag"] == "CHIEF":
            return f"run:{lane['run']}"
        r = self._orca("run-show", "--id", lane["run"])
        handle = ((r.get("result") or {}).get("run") or {}).get("coordinator_handle")
        if handle:
            return handle
        return f"dispatch:{lane['dispatch']}" if lane.get("dispatch") else f"run:{lane['run']}"

    def send(self, to, subject, body):
        """Send a status message; returns (ok, message_id, run_id, error)."""
        r = self._orca("send", "--to", to, "--type", "status", "--subject", subject, "--body", body)
        msg = (r.get("result") or {}).get("message") or {}
        err = (r.get("error") or {}).get("message")
        return bool(r.get("ok")), msg.get("id"), msg.get("run_id"), err


# --------------------------------------------------------------------------------------------- transcript digests

def new_worker_state():
    return {"cursor": None, "last_ts": None, "count": 0, "bg_open": {}, "recent": [],
            "last_text": None, "pr_ready": None, "last_report_ts": None, "last_user_ts": None}


def block_text(blocks):
    return " ".join(b["text"] for b in blocks if b.get("type") == "text" and b.get("text")).strip()


def pr_ready_in(text):
    """The PR number a `PR READY` report names (0 when it names none), or None when `text` reports nothing."""
    if PR_READY_PLAN_RE.search(text):
        return None
    m = PR_READY_RE.search(text)
    return None if not m else int(m.group(1) or 0)


def absorb(ws, messages):
    """Fold new transcript messages into one worker's compact state."""
    for m in messages:
        ts, role, blocks = m.get("timestamp"), m.get("role"), m.get("blocks") or []
        if not isinstance(ts, (int, float)):
            continue
        ws["last_ts"] = max(ws["last_ts"] or 0, ts)
        ws["count"] += 1
        for b in blocks:
            if b.get("type") == "background-task":
                task = str(b.get("taskId") or b.get("label") or "?")
                if b.get("state") == "working":  # every progress tick refreshes the task; any other state closes it
                    started = b.get("startedAt")
                    tick = max(ts, started) if _number(started) else ts
                    ws["bg_open"][task] = max(ws["bg_open"].get(task, 0), tick)
                else:
                    ws["bg_open"].pop(task, None)
        text = block_text(blocks)
        calls = [b for b in blocks if b.get("type") == "tool-call"]
        if role == "assistant" and calls:
            call = calls[-1]
            inp = call.get("input") or {}
            cmd = inp.get("command") or inp.get("description") or inp.get("prompt") or ""
            digest = {"ts": ts, "role": role, "kind": "tool", "running": call.get("state") == "running",
                      "text": f"{call.get('name', '')}: {str(cmd)[:200]}"}
            if REPORT_RE.search(str(cmd)):
                ws["last_report_ts"] = ts
        elif role == "assistant":
            digest = {"ts": ts, "role": role, "kind": "text", "running": False, "text": text[:600]}
            if text:
                ws["last_text"] = {"ts": ts, "text": text[:600]}
                number = pr_ready_in(text)
                if number is not None:
                    ws["pr_ready"] = {"ts": ts, "text": text[:600], "number": number}
        elif role == "user":
            ws["last_user_ts"] = ts
            ws["pr_ready"] = None
            digest = {"ts": ts, "role": role, "kind": "user", "running": False, "text": text[:240]}
        elif role == "reasoning":
            digest = {"ts": ts, "role": role, "kind": "reasoning", "running": False, "text": text[:240]}
        else:
            continue
        ws["recent"] = (ws["recent"] + [digest])[-RECENT:]
    newest = sorted(ws["bg_open"].items(), key=lambda kv: kv[1], reverse=True)[:BACKGROUND_KEEP]
    ws["bg_open"] = dict(newest)


def open_background(ws, now_ms, th):
    """Background tasks still open: started, never reported done, and ticked within `background_stale_minutes`
    (real transcripts show progress ticks without a final `done`, so an old open task is not a live one)."""
    return sum(1 for tick in ws["bg_open"].values() if minutes(now_ms - tick) <= th["background_stale_minutes"])


# --------------------------------------------------------------------------------------------- detectors

def minutes(ms_delta):
    return ms_delta / 60000


def clip(text, n=110):
    text = " ".join((text or "").split())
    return text if len(text) <= n else text[:n - 1] + "…"


def finding(kind, tag, worker, dispatch, evidence, age=None):
    key = f"{kind}:{dispatch or tag}"
    return {"key": key, "kind": kind, "lane": tag, "worker": worker, "dispatch": dispatch,
            "evidence": evidence, "age_minutes": None if age is None else round(age)}


def match_pr(name, ready, prs):
    """The open PR a `PR READY` report is about: by branch name, then the number it names, then any #N in its text."""
    for pr in prs:
        head = pr.get("headRefName") or ""
        if head == name or head.endswith("/" + name):
            return pr
    by_number = {pr["number"]: pr for pr in prs}
    numbers = [ready.get("number") or 0] + [int(n) for n in re.findall(r"#(\d{3,5})\b", ready.get("text", ""))]
    return next((by_number[n] for n in numbers if n in by_number), None)


def last_action_text(ws):
    """What the worker last said or did: its last assistant message only (plus the sentence that introduced a tool
    call). Older tool calls say nothing about what it waits for now."""
    last = next((d for d in reversed(ws["recent"]) if d["role"] == "assistant"), None)
    said = ws["last_text"]
    if last is None:
        return (said or {}).get("text", "")
    if last["kind"] == "tool" and said and 0 <= last["ts"] - said["ts"] <= 120000:
        return said["text"] + " " + last["text"]
    return last["text"]


def detect_worker(w, now_ms, th, prs):
    """Findings for one worker. `w` is {tag, name, dispatch, state}."""
    ws, found = w["state"], []
    if not ws["last_ts"]:
        return found
    age = minutes(now_ms - ws["last_ts"])
    recent = ws["recent"]
    last = recent[-1] if recent else None
    ready = ws["pr_ready"]

    if ready:
        pr = match_pr(w["name"], ready, prs)
        waited = minutes(now_ms - ready["ts"])
        if pr and waited > th["pr_ready_minutes"]:
            found.append(finding(
                "pr_ready_open", w["tag"], w["name"], w["dispatch"],
                f"PR READY {waited:.0f} min ago, PR #{pr['number']} still open ({'draft' if pr.get('isDraft') else 'ready'}, "
                f"updated {pr.get('updatedAt', '?')}); worker quiet {age:.0f} min", waited))
    elif age > th["silent_minutes"]:
        hint = bool(JOB_RE.search(last_action_text(ws))) or open_background(ws, now_ms, th) > 0
        tool_state = last.get("running") if last and last["kind"] == "tool" else False  # None: a digest without the field
        if hint:
            kind, label = "silent_waiting_job", "waiting on a pool job or background task"
        elif tool_state is None:
            kind, label = "silent_unknown", "last tool call has no recorded state"
        elif tool_state:
            kind, label = "silent_tool_running", "one tool call has been running since then"
        else:
            kind, label = "silent_turn_ended", "turn ended with no pending tool or background task"
        found.append(finding(kind, w["tag"], w["name"], w["dispatch"],
                             f"no transcript activity for {age:.0f} min ({label}); last {last['kind'] if last else '?'}: "
                             f"\"{clip((ws['last_text'] or {}).get('text') or (last or {}).get('text'))}\"", age))

    text = ws["last_text"]
    if text and not ready:
        stripped = NO_FAIL_RE.sub("", text["text"])
        since = minutes(now_ms - text["ts"])
        reported = ws["last_report_ts"] is not None and ws["last_report_ts"] >= text["ts"] - 300000
        if FAIL_RE.search(stripped) and not FIXED_RE.search(stripped) and since > th["failed_minutes"] and not reported:
            found.append(finding("failure_unreported", w["tag"], w["name"], w["dispatch"],
                                 f"last text mentions a failure {since:.0f} min ago and no report to the coordinator since: "
                                 f"\"{clip(text['text'])}\"", since))
    return found


def detect_coordinator(lane, coord_state, active_count, now_ms, th):
    last = coord_state.get("last_ts")
    if not last or not active_count or open_background(coord_state, now_ms, th):
        return []  # a coordinator blocked on its own background waiter is waiting, not quiet
    age = minutes(now_ms - last)
    if age > th["coordinator_quiet_minutes"]:
        return [finding("coordinator_quiet", lane["tag"], "coordinator", lane["dispatch"],
                        f"coordinator transcript quiet {age:.0f} min with {active_count} active workers", age)]
    return []


def detect_mail(w, mail, now_ms, th):
    """Mail addressed to an idle worker that it never acted on (idle sessions do not start a turn for mail)."""
    ws = w["state"]
    if not mail or not ws["last_ts"] or ws["last_ts"] >= mail["created_ms"]:
        return []
    waited = minutes(now_ms - mail["created_ms"])
    if waited <= th["unread_mail_minutes"]:
        return []
    when = dt.datetime.fromtimestamp(mail["created_ms"] / 1000).strftime("%H:%M")
    return [finding("unread_mail", w["tag"], w["name"], w["dispatch"],
                    f"worker {w['name']} has an unread mail since {when} (\"{clip(mail['subject'], 70)}\") and no transcript "
                    f"activity since: re-send as a question or restart with a RESUME spec in the same worktree", waited)]


def detect_pool(pool, th):
    if not pool:
        return []
    bits = []
    avg, samples = pool.get("avg_1h_seconds"), pool.get("samples_1h", 0)
    if avg is not None and samples >= th["pool_min_samples"] and avg / 60 > th["pool_wait_target_minutes"]:
        bits.append(f"1 h average wait {avg / 60:.0f} min over {samples} jobs (target {th['pool_wait_target_minutes']})")
    oldest = pool.get("oldest_waiting_seconds")
    if oldest is not None and oldest / 60 > th["pool_oldest_wait_minutes"]:
        bits.append(f"oldest queued job {oldest / 60:.0f} min ({pool.get('oldest_job')}); {pool.get('queued')} queued")
    if not bits:
        return []
    return [finding("pool_wait", "POOL", "pool", "", "; ".join(bits))]


# --------------------------------------------------------------------------------------------- state file

def _number(value):
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _clean_worker(entry):
    """A stored worker digest with every field of the right type, or None (the worker is then re-read from the start)."""
    if not isinstance(entry, dict):
        return None
    ws = {**new_worker_state(), **entry}
    ws = {key: ws[key] for key in new_worker_state()}
    if not (ws["cursor"] is None or isinstance(ws["cursor"], str)):
        return None
    if not all(ws[k] is None or _number(ws[k]) for k in ("last_ts", "last_report_ts", "last_user_ts")):
        return None
    if not (_number(ws["count"]) and isinstance(ws["recent"], list)):
        return None
    # an older state file held a counter here: that worker is re-read so the open set is rebuilt
    if not (isinstance(ws["bg_open"], dict) and all(isinstance(k, str) and _number(v) for k, v in ws["bg_open"].items())):
        return None
    for digest in ws["recent"]:
        if not (isinstance(digest, dict) and _number(digest.get("ts")) and isinstance(digest.get("text"), str)
                and digest.get("role") in ("assistant", "user", "reasoning") and isinstance(digest.get("kind"), str)
                and isinstance(digest.get("running", False), bool)):  # a missing `running` is tolerated (unknown)
            return None
    for key in ("last_text", "pr_ready"):
        value = ws[key]
        if value is not None and not (isinstance(value, dict) and _number(value.get("ts")) and isinstance(value.get("text"), str)):
            return None
    return ws


def sanitize_state(raw):
    """Whatever the state file held, a dict with every table present and well-formed; bad entries are dropped
    (a dropped worker is re-read from the start, a dropped timestamp only repeats one alert)."""
    raw = raw if isinstance(raw, dict) else {}
    tables = {name: (raw.get(name) if isinstance(raw.get(name), dict) else {}) for name in STATE_TABLES}
    for name in ("workers", "coordinators"):
        cleaned = {d: _clean_worker(e) for d, e in tables[name].items()}
        tables[name] = {d: e for d, e in cleaned.items() if e is not None}
    for name in ("logged", "sent", "chief_sent", "streak", "read_fail"):
        tables[name] = {k: v for k, v in tables[name].items() if _number(v)}
    tables["key_meta"] = {k: v for k, v in tables["key_meta"].items()
                          if isinstance(v, dict) and all(isinstance(v.get(f), str) for f in ("kind", "lane", "dispatch"))}
    tables["mail"] = {k: v for k, v in tables["mail"].items()
                      if isinstance(v, dict) and _number(v.get("created_ms")) and isinstance(v.get("subject"), str)}
    return tables


# --------------------------------------------------------------------------------------------- the watchdog

class Watchdog:
    def __init__(self, cfg, registry, backend, dry_run=False):
        self.cfg, self.registry, self.backend, self.dry_run = cfg, registry, backend, dry_run
        self.out_dir = Path(cfg["out_dir"])
        self.state = self.load_state()

    def load_state(self):
        try:
            raw = json.loads(Path(self.cfg["state_file"]).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            raw = {}
        return sanitize_state(raw)

    def save_state(self):
        if self.dry_run:
            return
        path = Path(self.cfg["state_file"])
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.state, indent=1), encoding="utf-8")
        os.replace(tmp, path)

    def read(self, dispatch, cursor):
        """One transcript read. A busy transcript can change under a from-start read (`source_changed`): retry that
        once. A refused cursor is not retried; the caller falls back to the from-start read."""
        try:
            return self.backend.read_new(dispatch, cursor)
        except Exception as exc:  # noqa: BLE001
            if cursor or "source_changed" not in str(exc):
                raise
            LOG.info("transcript of %s changed during the read; retrying once", dispatch)
            time.sleep(self.cfg["read_retry_pause_seconds"])
            return self.backend.read_new(dispatch, cursor)

    def fetch(self, store, dispatch):
        """Network half of a transcript refresh (runs in a thread): (reset, messages, cursor, error)."""
        cursor = (store.get(dispatch) or {}).get("cursor")
        try:
            return False, *self.read(dispatch, cursor), None
        except Exception as exc:  # noqa: BLE001 - one worker must not hide the others
            if not cursor:
                return False, [], None, str(exc)[:120]
            LOG.info("cursor refused for %s (%s); rereading from the start", dispatch, exc)
        try:
            return True, *self.read(dispatch, None), None
        except Exception as exc:  # noqa: BLE001
            return True, [], None, str(exc)[:120]

    def refresh_all(self, items, errors, blind):
        """items: [(store, dispatch)]. Reads in parallel, folds results in order; returns {dispatch: worker state}.
        A dispatch whose read failed goes into blind["dispatches"]: its findings are unknown this poll."""
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, self.cfg["read_threads"])) as pool:
            results = list(pool.map(lambda item: self.fetch(*item), items))
        states = {}
        fails, limit = self.state["read_fail"], self.cfg["thresholds"]["read_fail_red_polls"]
        listed = {item[1] for item in items}
        for dispatch in [d for d in fails if d not in listed]:
            del fails[dispatch]
        for (store, dispatch), (reset, messages, cursor, error) in zip(items, results):
            if error:
                # Unknown for this poll either way (see act()); only a read that keeps failing turns the status red.
                fails[dispatch] = fails.get(dispatch, 0) + 1
                if fails[dispatch] >= limit:
                    errors.append(f"{dispatch}: {error} ({fails[dispatch]} polls in a row)")
                else:
                    LOG.info("read of %s failed (%s); poll %d of %d before it counts as an error", dispatch, error, fails[dispatch], limit)
                blind["dispatches"].add(dispatch)
                continue
            fails.pop(dispatch, None)
            ws = store[dispatch] = new_worker_state() if reset or dispatch not in store else store[dispatch]
            absorb(ws, messages)
            ws["cursor"] = cursor
            states[dispatch] = ws
        return states

    def poll(self, now):
        th, errors, findings = self.cfg["thresholds"], [], []
        now_ms = now * 1000
        # What this poll could not look at. Findings that depend on it are neither cleared nor repeated (see act()).
        blind = {"lanes": set(), "dispatches": set(), "prs": False, "mail": False, "waiter": False, "pool": False}
        try:
            prs = self.backend.open_prs()
        except Exception as exc:  # noqa: BLE001
            prs = []
            blind["prs"] = True
            errors.append(f"pull requests: {str(exc)[:160]}")
        failed_lanes = blind["lanes"]
        lane_summary, seen, polled, listed = [], set(), [], []
        for lane in self.registry["lanes"]:
            try:
                listed.append((lane, self.backend.active_workers(lane["run"])))
            except Exception as exc:  # noqa: BLE001
                errors.append(f"{lane['tag']}: {str(exc)[:160]}")
                failed_lanes.add(lane["tag"])
        items = [(self.state["workers"], w["dispatch"]) for _, workers in listed for w in workers]
        items += [(self.state["coordinators"], lane["dispatch"]) for lane, _ in listed if lane["dispatch"]]
        states = self.refresh_all(items, errors, blind)
        for lane, workers in listed:
            checked = 0
            for w in workers:
                seen.add(w["dispatch"])
                ws = states.get(w["dispatch"])
                if ws is None:
                    continue
                checked += 1
                view = {"tag": lane["tag"], "name": w["name"], "dispatch": w["dispatch"], "state": ws}
                polled.append(view)
                findings += detect_worker(view, now_ms, th, prs)
            coord_age = None
            cs = states.get(lane["dispatch"]) if lane["dispatch"] else None
            if cs is not None:
                findings += detect_coordinator(lane, cs, len(workers), now_ms, th)
                coord_age = None if not cs["last_ts"] else round(minutes(now_ms - cs["last_ts"]))
            lane_summary.append({"tag": lane["tag"], "active_workers": len(workers), "checked": checked,
                                 "coordinator_quiet_minutes": coord_age})
        for dispatch in [d for d in self.state["workers"] if d not in seen]:
            if not failed_lanes:
                del self.state["workers"][dispatch]
                self.state["mail"].pop(dispatch, None)

        try:
            self.note_mail(self.backend.worker_mail())
        except Exception as exc:  # noqa: BLE001
            blind["mail"] = True
            errors.append(f"mail check: {str(exc)[:120]}")
        for view in polled:
            findings += detect_mail(view, self.state["mail"].get(view["dispatch"]), now_ms, th)

        waiters = None
        try:
            waiters = self.backend.waiter_count()
        except Exception as exc:  # noqa: BLE001
            errors.append(f"waiter check: {str(exc)[:120]}")
        if waiters is None:
            blind["waiter"] = True
        elif waiters < self.cfg["min_waiters"]:
            findings.append(finding("no_waiter", "ALL", "waiters", "",
                                    f"{waiters} {self.cfg['waiter_script']} processes on this host; no coordinator can wake on mail"))
        try:
            findings += detect_pool(self.backend.pool_status(now), th)
        except Exception as exc:  # noqa: BLE001
            blind["pool"] = True
            errors.append(f"pool queue check skipped: {str(exc)[:120]}")

        self.act(findings, blind, now)
        self.write_status(findings, lane_summary, errors, now)
        self.save_state()
        return findings, errors

    def note_mail(self, messages):
        """Remember, per dispatch, the newest mail that needs the worker to act (a question or a FIX-like subject)."""
        subject_re = re.compile(self.cfg["mail_subject_regex"])  # case-sensitive: protocol words are upper case
        for m in messages:
            if m["type"] != "question" and not subject_re.search(m["subject"]):
                continue
            current = self.state["mail"].get(m["dispatch"])
            if current is None or m["created_ms"] > current["created_ms"]:
                self.state["mail"][m["dispatch"]] = m

    # ---------------------------------------------------------------- acting
    @staticmethod
    def is_blind(meta, blind):
        """True when the source a finding depends on could not be read this poll, so its absence proves nothing."""
        return (meta["lane"] in blind["lanes"] or meta["dispatch"] in blind["dispatches"]
                or (meta["kind"] == "pr_ready_open" and blind["prs"]) or (meta["kind"] == "unread_mail" and blind["mail"])
                or (meta["kind"] == "no_waiter" and blind["waiter"]) or (meta["kind"] == "pool_wait" and blind["pool"]))

    def act(self, findings, blind, now):
        th, st = self.cfg["thresholds"], self.state
        current = {f["key"] for f in findings}
        for key in list(st["streak"]):
            meta = st["key_meta"].get(key)
            # A finding that vanished because a read failed keeps its streak and its de-dup times: the next good poll
            # must not count it as new (that would mail it again and restart the chief escalation).
            if key not in current and not (meta and self.is_blind(meta, blind)):
                for table in ("streak", "logged", "sent", "chief_sent", "key_meta"):
                    st[table].pop(key, None)
        for f in findings:
            st["streak"][f["key"]] = st["streak"].get(f["key"], 0) + 1
            st["key_meta"][f["key"]] = {"kind": f["kind"], "lane": f["lane"], "dispatch": f["dispatch"]}
            f["streak"] = st["streak"][f["key"]]
        dedupe = th["dedupe_minutes"] * 60
        to_log = [f for f in findings if now - st["logged"].get(f["key"], 0) >= dedupe]
        self.append_alerts(to_log, now)
        for f in to_log:
            st["logged"][f["key"]] = now

        lane_by_tag = {l["tag"]: l for l in self.registry["lanes"]}
        for tag, lane in lane_by_tag.items():
            due = [f for f in findings if f["lane"] == tag and now - st["sent"].get(f["key"], 0) >= dedupe]
            if not due:
                continue
            subject = f"[WATCHDOG] {tag}: {len(due)} stuck worker(s)" if any(f["worker"] != "coordinator" for f in due) \
                else f"[WATCHDOG] {tag}: coordinator quiet"
            body = ("The worker watchdog found these. It sent nothing to the workers and changed nothing.\n\n"
                    + "\n".join(f"- {f['worker']} ({f['kind']}): {f['evidence']}" for f in due))
            if self.deliver(lane, subject, body):
                for f in due:
                    st["sent"][f["key"]] = now

        chief_lines = []
        for f in findings:
            routed_to_chief = f["lane"] not in lane_by_tag
            escalated = f["lane"] in lane_by_tag and f["streak"] >= th["escalate_after_polls"]
            if (routed_to_chief or escalated) and now - st["chief_sent"].get(f["key"], 0) >= th["chief_renotify_minutes"] * 60:
                chief_lines.append(f)
        if chief_lines:
            body = "\n".join(f"- {f['lane']} {f['worker']} ({f['kind']}, flagged on {f['streak']} polls): {f['evidence']}"
                             for f in chief_lines)
            body = ("The worker watchdog still sees these stuck after its lane coordinator was told:\n\n" + body
                    if any(f["lane"] in lane_by_tag for f in chief_lines) else
                    "The worker watchdog sees a host-level problem:\n\n" + body)
            if self.deliver({"tag": "CHIEF", "run": self.registry["chief_run"], "dispatch": ""},
                            f"[WATCHDOG] {len(chief_lines)} finding(s) need the chief", body):
                for f in chief_lines:
                    st["chief_sent"][f["key"]] = now

    def deliver(self, lane, subject, body):
        if self.dry_run:
            target = f"dispatch:{lane['dispatch']}" if lane.get("dispatch") else f"run:{lane['run']}"
            LOG.info("DRY RUN: would send to %s: %s", target, subject)
            print(f"[dry-run] to {target}: {subject}\n{body}\n")
            return False
        try:
            target = self.backend.address(lane)
            ok, msg_id, run_id, err = self.backend.send(target, subject, body)
        except Exception as exc:  # noqa: BLE001 - a failed send is retried on the next poll
            LOG.warning("send to %s failed: %s", lane["tag"], exc)
            return False
        if ok and run_id and run_id != lane["run"]:
            LOG.warning("message %s to %s landed in %s, expected %s", msg_id, lane["tag"], run_id, lane["run"])
            ok = False
        LOG.info("send to %s: ok=%s id=%s %s", lane["tag"], ok, msg_id, err or "")
        return ok

    def append_alerts(self, to_log, now):
        if not to_log or self.dry_run:
            return
        self.out_dir.mkdir(parents=True, exist_ok=True)
        stamp = dt.datetime.fromtimestamp(now).strftime("%Y-%m-%d %H:%M")
        with (self.out_dir / "alerts.md").open("a", encoding="utf-8") as fh:
            for f in to_log:
                fh.write(f"- {stamp} | {f['lane']} | {f['worker']} | {f['kind']} | {f['evidence']}\n")

    def write_status(self, findings, lane_summary, errors, now):
        """`ok` is true only for a poll that looked at everything and found nothing. A poll that could not read a
        source (Orca down, gh failing) is not green: `reason` says what is wrong."""
        reasons = []
        if findings:
            reasons.append(f"{len(findings)} finding(s)")
        if errors:
            reasons.append("watchdog could not check everything: " + "; ".join(errors[:3]) + ("; ..." if len(errors) > 3 else ""))
        self.write_status_file({"findings": findings, "lanes": lane_summary, "errors": errors,
                                "ok": not reasons, "reason": " / ".join(reasons)}, now)

    def write_poll_failure(self, exc, now):
        """The service loop caught an unexpected error: leave a red status instead of the previous poll's green."""
        reason = f"poll crashed: {type(exc).__name__}: {str(exc)[:160]}"
        self.write_status_file({"findings": [], "lanes": [], "errors": [reason], "ok": False, "reason": reason}, now)

    def write_status_file(self, body, now):
        if self.dry_run:
            return
        self.out_dir.mkdir(parents=True, exist_ok=True)
        status = {"updated": dt.datetime.fromtimestamp(now, dt.timezone.utc).isoformat(timespec="seconds"),
                  "updated_epoch": int(now), "poll_seconds": self.cfg["poll_seconds"], **body}
        tmp = self.out_dir / "status.json.tmp"
        tmp.write_text(json.dumps(status, indent=1), encoding="utf-8")
        os.replace(tmp, self.out_dir / "status.json")


# --------------------------------------------------------------------------------------------- service

def hold_single_instance(out_dir):
    """Exclusive lock held for the process lifetime; returns the open file or None when another instance runs."""
    out_dir.mkdir(parents=True, exist_ok=True)
    fh = open(out_dir / "watchdog.lock", "a+b")
    try:
        if os.name == "nt":
            import msvcrt
            fh.seek(0)
            msvcrt.locking(fh.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(fh, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        fh.close()
        return None
    return fh


def write_pid_file(out_dir):
    (out_dir / "watchdog.pid").write_text(json.dumps({
        "pid": os.getpid(), "script": Path(__file__).name,
        "started_epoch": int(time.time())}), encoding="utf-8")


def print_findings(findings, errors):
    if not findings:
        print("watchdog: no findings")
    for f in findings:
        print(f"{f['lane']:5} {f['worker']:36} {f['kind']:20} {f['evidence']}")
    for e in errors:
        print(f"note: {e}")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--config", help="JSON config (see tools/watchdog/config.example.json)")
    ap.add_argument("--once", action="store_true", help="poll once, act, print the findings and exit")
    ap.add_argument("--dry-run", action="store_true", help="with --once: print what would be sent, write nothing")
    ap.add_argument("--poll-seconds", type=int)
    ap.add_argument("--print-out-dir", action="store_true", help="print the output folder of this config and exit (for the scripts)")
    args = ap.parse_args(argv)
    cfg = load_config(args.config)
    if args.print_out_dir:
        print(cfg["out_dir"])
        return 0
    if args.poll_seconds:
        cfg["poll_seconds"] = args.poll_seconds
    registry = load_registry(cfg)
    out_dir = Path(cfg["out_dir"])
    out_dir.mkdir(parents=True, exist_ok=True)
    handlers = [logging.StreamHandler(sys.stdout)] if args.once else [
        logging.handlers.RotatingFileHandler(out_dir / "watchdog.log", maxBytes=1_000_000, backupCount=3, encoding="utf-8")]
    logging.basicConfig(level=logging.INFO, handlers=handlers, format="%(asctime)s %(levelname)s %(message)s")
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    backend = LiveBackend(cfg, registry)

    if args.once:
        lock = hold_single_instance(out_dir) if not args.dry_run else True
        if lock is None:
            print("watchdog: the service is running; --once would race its state file. Use --dry-run or stop it.")
            return 3
        findings, errors = Watchdog(cfg, registry, backend, dry_run=args.dry_run).poll(time.time())
        print_findings(findings, errors)
        return 0

    lock = hold_single_instance(out_dir)
    if lock is None:
        print("watchdog: already running")
        return 3
    write_pid_file(out_dir)
    LOG.info("watchdog started: %d lanes, poll every %d s", len(registry["lanes"]), cfg["poll_seconds"])
    dog = Watchdog(cfg, registry, backend)
    while True:
        started = time.time()
        try:
            findings, errors = dog.poll(started)
            LOG.info("poll done: %d findings, %d notes", len(findings), len(errors))
            for e in errors:
                LOG.info("note: %s", e)
        except Exception as exc:  # noqa: BLE001 - the service must survive any one bad poll
            LOG.exception("poll failed")
            try:
                dog.write_poll_failure(exc, time.time())
            except OSError:
                pass
        time.sleep(max(30, cfg["poll_seconds"] - (time.time() - started)))


if __name__ == "__main__":
    sys.exit(main())
