#!/usr/bin/env python3
"""Independent loopback IMAP/SMTP and TLS fixture for ReliefMail protocol tests."""

from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path


USER = b"alice@reliefmail.test"
PASSWORD = b"phase-a-fixture-password"
PROJECT_WIRE = b"&mHl27g-"
PROJECT_RAW = (
    b"Received: from reliefmail.local (unknown [172.17.0.1])\r\n"
    b"\tby mail.reliefmail.test (Postfix) with ESMTPSA id 33297C0C46F8\r\n"
    b"\tfor <bob@reliefmail.test>; Sun, 18 Oct 2026 14:17:44 +0000 (UTC)\r\n"
    b"From: fixture@reliefmail.test\r\n"
    b"Subject: fixture-\r\n\r\nbody\r\n"
)
WORKER_FETCH_RAW = (
    b"From: fixture@reliefmail.test\r\n"
    b"To: alice@reliefmail.test\r\n"
    b"Subject: full body from real fixture\r\n"
    b"Date: Sun, 18 Oct 2026 14:17:44 +0000\r\n"
    b"Message-ID: <fetch-worker@example.test>\r\n"
    b"MIME-Version: 1.0\r\n"
    b"Content-Type: multipart/mixed; boundary=\"task5-fetch-boundary\"\r\n"
    b"\r\n"
    b"--task5-fetch-boundary\r\n"
    b"Content-Type: text/plain; charset=UTF-8\r\n"
    b"Content-Transfer-Encoding: 8bit\r\n"
    b"\r\n"
    b"full body from real fixture\r\n"
    b"--task5-fetch-boundary\r\n"
    b"Content-Type: application/octet-stream; name=\"task5.bin\"\r\n"
    b"Content-Disposition: attachment; filename=\"task5.bin\"\r\n"
    b"Content-Transfer-Encoding: base64\r\n"
    b"\r\n"
    b"YXR0YWNobWVudA==\r\n"
    b"--task5-fetch-boundary--\r\n"
)
SMTP_8BIT_RAW = (
    b"From: alice@reliefmail.test\r\n"
    b"To: bob@reliefmail.test\r\n"
    b"Subject: caf\xc3\xa9\r\n"
    b"Content-Type: text/plain; charset=UTF-8\r\n"
    b"Content-Transfer-Encoding: 8bit\r\n\r\n"
    b"caf\xc3\xa9 body\r\n"
)
SOURCE_RAW = (
    b"From: alice@reliefmail.test\n"
    b"To: bob@reliefmail.test\n"
    b"Subject: phase-a fixture\n"
    b"\n"
    b".leading-dot\n"
    b"final line\n"
)

SCENARIOS = (
    "tls-valid",
    "tls-expired",
    "tls-slow",
    "imap-starttls",
    "imap-starttls-no-extension",
    "imap-starttls-untrusted",
    "imap-split-literal",
    "imap-safe-move",
    "imap-unsafe-move",
    "imap-auth-capability-transition",
    "imap-auth-capability-reject",
    "imap-auth-capability-timeout",
    "imap-auth-capability-cancel",
    "imap-delete",
    "imap-no-uidplus-delete",
    "imap-worker-mail",
    "imap-login",
    "imap-auth-failure",
    "imap-charset-refused",
    "imap-fetch-budget",
    "imap-status-unrequested",
    "imap-append",
    "smtp-success",
    "smtp-worker",
    "smtp-worker-lost",
    "smtp-login",
    "smtp-starttls",
    "smtp-starttls-no-extension",
    "smtp-auth-failure",
    "smtp-recipient-rejected",
    "smtp-observer-refused",
    "smtp-final-lost",
    "smtp-final-inconclusive",
    "smtp-final-rejected",
    "smtp-malformed-mail",
    "smtp-malformed-data",
    "smtp-rset-negative-recipient",
    "smtp-rset-lost-observer",
    "smtp-8bit",
    "smtp-no-smtputf8",
    "smtp-no-8bitmime",
)


def _run_openssl(*args: str) -> None:
    try:
        result = subprocess.run(args, check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
    except OSError as exc:
        raise RuntimeError("openssl is required to prepare local TLS fixtures") from exc
    if result.returncode:
        raise RuntimeError("openssl fixture setup failed: " + result.stderr[-1200:])


def prepare_certificates(directory: Path) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "newcerts").mkdir(exist_ok=True)
    (directory / "index.txt").write_text("", encoding="ascii")
    (directory / "serial").write_text("1000\n", encoding="ascii")

    (directory / "ca-req.cnf").write_text(
        "[req]\n"
        "distinguished_name=dn\n"
        "x509_extensions=ca_ext\n"
        "prompt=no\n"
        "[dn]\nCN=ReliefMail Protocol Test CA\n"
        "[ca_ext]\n"
        "basicConstraints=critical,CA:TRUE\n"
        "keyUsage=critical,keyCertSign,cRLSign\n",
        encoding="ascii",
    )
    _run_openssl("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                 "-keyout", str(directory / "ca.key"), "-out", str(directory / "ca.crt"),
                 "-days", "3650", "-sha256", "-config", str(directory / "ca-req.cnf"))

    (directory / "ca.cnf").write_text(
        "[ca]\ndefault_ca=CA_default\n"
        "[CA_default]\n"
        f"dir={directory}\n"
        "database=$dir/index.txt\n"
        "new_certs_dir=$dir/newcerts\n"
        "certificate=$dir/ca.crt\n"
        "private_key=$dir/ca.key\n"
        "serial=$dir/serial\n"
        "default_md=sha256\n"
        "default_days=3650\n"
        "policy=policy_any\n"
        "x509_extensions=server_ext\n"
        "unique_subject=no\n"
        "copy_extensions=copy\n"
        "[policy_any]\ncommonName=supplied\n"
        "[server_ext]\n"
        "basicConstraints=critical,CA:FALSE\n"
        "keyUsage=critical,digitalSignature,keyEncipherment\n"
        "extendedKeyUsage=serverAuth\n"
        "subjectAltName=DNS:localhost\n",
        encoding="ascii",
    )
    for name, common_name in (("valid", "localhost"), ("expired", "expired.localhost")):
        _run_openssl("openssl", "req", "-newkey", "rsa:2048", "-nodes",
                     "-keyout", str(directory / f"{name}.key"),
                     "-out", str(directory / f"{name}.csr"), "-subj", f"/CN={common_name}")
        command = ["openssl", "ca", "-batch", "-config", str(directory / "ca.cnf"),
                   "-extensions", "server_ext", "-in", str(directory / f"{name}.csr"),
                   "-out", str(directory / f"{name}.crt")]
        if name == "expired":
            command.extend(("-startdate", "20200101000000Z", "-enddate", "20210101000000Z"))
        _run_openssl(*command)

    _run_openssl("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                 "-keyout", str(directory / "untrusted-ca.key"),
                 "-out", str(directory / "untrusted-ca.crt"), "-days", "3650", "-sha256",
                 "-subj", "/CN=ReliefMail Untrusted Test CA",
                 "-addext", "basicConstraints=critical,CA:TRUE",
                 "-addext", "keyUsage=critical,keyCertSign,cRLSign")


def _new_record(name: str) -> dict:
    return {
        "scenario": name,
        "connections": 0,
        "commands": [],
        "auth": [],
        "insecure_auth": False,
        "unsafe_expunge": False,
        "saw_uid_expunge": False,
        "data_started": False,
        "data_completed": False,
        "final_reply_missing": False,
        "data_crlf": False,
        "dot_stuffed": False,
        "mail_size_present": False,
        "mail_parameter_sets": [],
        "strict_mail_parameters_valid": None,
        "rset_reply_codes": [],
        "status_targets": [],
        "fetch_fragmented": False,
        "search_queries": [],
        "targeted_expunge_uids": [],
        "uid_move_uids": [],
        "capability_auth_states": [],
        "append_count": 0,
        "status_container_count": 0,
    }


def _message_id(raw: bytes) -> str:
    for line in raw.split(b"\r\n"):
        if line.lower().startswith(b"message-id:"):
            return line.split(b":", 1)[1].strip().decode("ascii", "replace")
    return ""


def _send_fragmented(sock: socket.socket, data: bytes) -> None:
    widths = (1, 2, 5, 3, 8)
    offset = 0
    index = 0
    while offset < len(data):
        count = widths[index % len(widths)]
        sock.sendall(data[offset:offset + count])
        offset += count
        index += 1
        time.sleep(0.001)


def _send_line(sock: socket.socket, line: bytes, fragmented: bool = False) -> None:
    payload = line if line.endswith(b"\r\n") else line + b"\r\n"
    if fragmented:
        _send_fragmented(sock, payload)
    else:
        sock.sendall(payload)


def _read_line(sock: socket.socket, limit: int = 65536) -> bytes | None:
    data = bytearray()
    while len(data) < limit:
        byte = sock.recv(1)
        if not byte:
            return bytes(data) if data else None
        data.extend(byte)
        if data.endswith(b"\r\n"):
            return bytes(data)
    raise ValueError("fixture command line exceeded its bound")


def _read_exact(sock: socket.socket, size: int) -> bytes:
    if size < 0 or size > 64 * 1024 * 1024:
        raise ValueError("fixture literal length outside bound")
    output = bytearray()
    while len(output) < size:
        part = sock.recv(min(8192, size - len(output)))
        if not part:
            raise EOFError("connection ended inside fixture literal")
        output.extend(part)
    return bytes(output)


def _tag(line: bytes) -> bytes:
    fields = line.rstrip(b"\r\n").split(None, 2)
    return fields[0] if fields else b"A0001"


def _command_text(line: bytes, protocol: str, record: dict, tls_active: bool) -> str:
    fields = line.decode("utf-8", "replace").strip().split()
    if protocol == "imap":
        fields = fields[1:]
    if not fields:
        return ""
    command = fields[0].upper()
    if command in ("AUTH", "AUTHENTICATE", "LOGIN"):
        if command == "AUTHENTICATE" and len(fields) > 1:
            display = f"AUTHENTICATE {fields[1].upper()} <redacted>"
            mechanism = fields[1].upper()
        elif command == "AUTH" and len(fields) > 1:
            display = f"AUTH {fields[1].upper()} <redacted>"
            mechanism = fields[1].upper()
        else:
            display = "LOGIN <redacted>"
            mechanism = "LOGIN"
        record["auth"].append({"mechanism": mechanism, "tls": tls_active})
        if not tls_active:
            record["insecure_auth"] = True
        return display
    if protocol == "smtp" and command == "DATA":
        record["data_started"] = True
    if protocol == "imap" and command == "EXPUNGE":
        record["unsafe_expunge"] = True
    if protocol == "imap" and command == "UID" and len(fields) > 1:
        subcommand = fields[1].upper()
        if subcommand == "EXPUNGE":
            record["saw_uid_expunge"] = True
        return " ".join(fields[:4]) if subcommand in ("STORE", "COPY", "MOVE", "EXPUNGE") else " ".join(fields[:3])
    if protocol == "smtp" and command == "MAIL":
        closing = line.find(b">")
        parameters = line[closing + 1:].strip().split() if closing >= 0 else []
        parameter_names = [item.decode("ascii", "replace").upper() for item in parameters]
        record["mail_parameter_sets"].append(parameter_names)
        record["mail_size_present"] = any(item.startswith("SIZE=") for item in parameter_names)
        return "MAIL FROM <redacted>"
    if protocol == "smtp" and command == "RCPT":
        return "RCPT TO <redacted>"
    return " ".join(fields[:4])


def _record_command(line: bytes, protocol: str, record: dict, tls_active: bool) -> str:
    shown = _command_text(line, protocol, record, tls_active)
    if shown:
        record["commands"].append(shown)
    return line.decode("utf-8", "replace").strip()


def _decode_auth(value: bytes) -> bool:
    try:
        decoded = base64.b64decode(value, validate=True)
    except (ValueError, base64.binascii.Error):
        return False
    if b"\0" in decoded:
        parts = decoded.split(b"\0")
        return len(parts) >= 3 and parts[-2] == USER and parts[-1] == PASSWORD
    return decoded == USER or decoded == PASSWORD


def _do_auth(conn: socket.socket, line: bytes, protocol: str, record: dict,
             tls_active: bool) -> bool:
    text = _record_command(line, protocol, record, tls_active)
    fields = text.split()
    if protocol == "imap":
        tag = _tag(line)
        if len(fields) >= 2 and fields[1].upper() == "LOGIN":
            import shlex

            try:
                values = shlex.split(text)[2:]
            except ValueError:
                values = []
            valid = len(values) == 2 and values[0].encode() == USER and values[1].encode() == PASSWORD
            _send_line(conn, tag + (b" OK authenticated" if valid and tls_active else b" NO authentication failed"))
            return valid and tls_active
        mechanism = fields[2].upper() if len(fields) > 2 else ""
        if mechanism == "PLAIN":
            initial = fields[3].encode() if len(fields) > 3 else b""
            if not initial:
                _send_line(conn, b"+ ")
                answer = _read_line(conn)
                initial = answer.strip() if answer else b""
            valid = _decode_auth(initial)
        elif mechanism == "LOGIN":
            _send_line(conn, b"+ VXNlcm5hbWU6")
            username = _read_line(conn)
            _send_line(conn, b"+ UGFzc3dvcmQ6")
            password = _read_line(conn)
            valid = bool(username and password and _decode_auth(username.strip()) and _decode_auth(password.strip()))
        else:
            valid = False
        _send_line(conn, tag + (b" OK authenticated" if valid and tls_active else b" NO authentication failed"))
        return valid and tls_active

    if fields[0].upper() == "AUTH" and len(fields) > 1:
        mechanism = fields[1].upper()
        if mechanism == "PLAIN":
            response = fields[2].encode() if len(fields) > 2 else b""
            if not response:
                _send_line(conn, b"334 ")
                answer = _read_line(conn)
                response = answer.strip() if answer else b""
            valid = _decode_auth(response)
        elif mechanism == "LOGIN":
            _send_line(conn, b"334 VXNlcm5hbWU6")
            username = _read_line(conn)
            _send_line(conn, b"334 UGFzc3dvcmQ6")
            password = _read_line(conn)
            valid = bool(username and password and _decode_auth(username.strip()) and _decode_auth(password.strip()))
        else:
            valid = False
        _send_line(conn, b"235 2.7.0 authenticated" if valid and tls_active else b"535 5.7.8 authentication failed")
        return valid and tls_active
    return False


def _capabilities(name: str, authenticated: bool = False) -> bytes:
    if name == "imap-starttls-no-extension":
        return b"IMAP4rev1 AUTH=PLAIN"
    if name in ("imap-starttls", "imap-starttls-untrusted"):
        return b"IMAP4rev1 STARTTLS AUTH=PLAIN ID"
    if name == "imap-safe-move":
        return b"IMAP4rev1 AUTH=PLAIN ID UIDPLUS"
    if name == "imap-unsafe-move":
        return b"IMAP4rev1 AUTH=PLAIN ID"
    if name == "imap-no-uidplus-delete":
        return b"IMAP4rev1 AUTH=PLAIN ID"
    if name in ("imap-auth-capability-transition", "imap-auth-capability-reject",
                "imap-auth-capability-timeout", "imap-auth-capability-cancel"):
        if authenticated:
            return b"IMAP4rev1 AUTH=PLAIN AUTH=LOGIN ID MOVE UIDPLUS"
        return b"IMAP4rev1 AUTH=PLAIN AUTH=LOGIN ID"
    if name == "imap-login":
        return b"IMAP4rev1 AUTH=LOGIN ID UIDPLUS"
    if name == "imap-auth-failure":
        return b"IMAP4rev1 AUTH=PLAIN ID UIDPLUS"
    return b"IMAP4rev1 AUTH=PLAIN AUTH=LOGIN ID MOVE UIDPLUS"


def _handle_imap(conn: socket.socket, name: str, record: dict,
                 tls_active: bool) -> None:
    caps = _capabilities(name)
    _send_line(conn, b"* OK [CAPABILITY " + caps + b"] ReliefMail fixture")
    authenticated = False
    selected_name = b""
    while True:
        line = _read_line(conn)
        if line is None:
            return
        text = _record_command(line, "imap", record, tls_active)
        fields = text.split()
        if len(fields) < 2:
            continue
        tag = _tag(line)
        command = fields[1].upper()
        if command == "CAPABILITY":
            record["capability_auth_states"].append(authenticated)
            if name == "imap-auth-capability-reject" and authenticated:
                _send_line(conn, tag + b" NO capability refresh refused")
                continue
            if name in ("imap-auth-capability-timeout", "imap-auth-capability-cancel") and authenticated:
                time.sleep(1.5)
            caps = _capabilities(name, authenticated)
            _send_line(conn, b"* CAPABILITY " + caps, fragmented=True)
            _send_line(conn, tag + b" OK CAPABILITY completed")
        elif command == "ID":
            _send_line(conn, b"* ID NIL")
            _send_line(conn, tag + b" OK ID completed")
        elif command == "STARTTLS":
            if b"STARTTLS" not in caps or tls_active:
                _send_line(conn, tag + b" BAD STARTTLS unavailable")
                continue
            _send_line(conn, tag + b" OK Begin TLS")
            try:
                conn = _server_tls(conn, name)
            except (ssl.SSLError, OSError):
                record["tls_handshake_failed"] = True
                return
            tls_active = True
        elif command in ("AUTHENTICATE", "LOGIN"):
            authenticated = _do_auth(conn, line, "imap", record, tls_active)
        elif command == "LIST":
            if name == "imap-worker-mail":
                _send_line(conn, b'* LIST (\\Noselect \\HasChildren) "/" "Projects"', fragmented=True)
                _send_line(conn, b'* LIST (\\HasNoChildren) "/" "Projects/Inbox"', fragmented=True)
                _send_line(conn, b'* LIST (\\HasNoChildren) "/" "INBOX"', fragmented=True)
            else:
                _send_line(conn, b'* LIST (\\HasNoChildren) "/" "INBOX"', fragmented=True)
                _send_line(conn, b'* LIST (\\HasNoChildren) "/" "A \\"quoted\\" folder"', fragmented=True)
                prefix = b'* LIST (\\HasNoChildren) "/" {' + str(len(PROJECT_WIRE)).encode() + b"}\r\n"
                _send_fragmented(conn, prefix + PROJECT_WIRE + b"\r\n")
            _send_line(conn, tag + b" OK LIST completed")
        elif command == "STATUS":
            import shlex

            try:
                target = shlex.split(line.decode("utf-8", "strict"))[2].encode("utf-8")
            except (IndexError, UnicodeError, ValueError):
                _send_line(conn, tag + b" BAD malformed STATUS target")
                continue
            record["status_targets"].append(target.decode("ascii", "replace"))
            if name == "imap-worker-mail" and target == b"Projects":
                record["status_container_count"] += 1
                _send_line(conn, tag + b" NO [NONEXISTENT] hierarchy container is not selectable")
                continue
            if name == "imap-status-unrequested":
                _send_line(conn, b'* STATUS "Archive MESSAGES 999 UNSEEN 998" (MESSAGES 81 UNSEEN 80 X-FUTURE 77)')
                if target.lower() == b"inbox":
                    requested = b'"inbox"'
                elif target == PROJECT_WIRE:
                    requested = b"{8}\r\n" + PROJECT_WIRE
                else:
                    requested = b'"A \\\"quoted\\\" folder"'
                response = b"* STATUS " + requested + b" (MESSAGES 2 UNSEEN 1 UIDNEXT 9 UIDVALIDITY 4242 X-FUTURE 77)"
                _send_fragmented(conn, response + b"\r\n")
                _send_line(conn, b'* STATUS "Other MESSAGES 81 UNSEEN 80" (MESSAGES 71 UNSEEN 70)')
            else:
                _send_line(conn, b"* STATUS " + target + b" (MESSAGES 2 UNSEEN 1 UIDNEXT 9 UIDVALIDITY 4242)")
            _send_line(conn, tag + b" OK STATUS completed")
        elif command == "SELECT" or command == "EXAMINE":
            selected_name = text.partition(" ")[2].partition(" ")[2].strip().strip('"').encode()
            selected_uidvalidity = 4242
            if name == "imap-worker-mail" and selected_name == b"Projects/Inbox":
                selected_count = record.get("worker_projects_selects", 0) + 1
                record["worker_projects_selects"] = selected_count
                if selected_count >= 2:
                    selected_uidvalidity = 4243
            _send_line(conn, b"* FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)", fragmented=True)
            _send_line(conn, b"* 2 EXISTS", fragmented=True)
            _send_line(conn, b"* 0 RECENT")
            _send_line(conn, b"* OK [UIDVALIDITY " + str(selected_uidvalidity).encode() + b"] stable mailbox")
            _send_line(conn, b"* OK [UIDNEXT 9] next uid")
            _send_line(conn, tag + b" OK [READ-WRITE] SELECT completed")
        elif command == "UID" and len(fields) > 2 and fields[2].upper() == "SEARCH":
            search_tail = line.rstrip(b"\r\n").split(None, 3)
            if len(search_tail) == 4:
                record["search_queries"].append(search_tail[3].decode("utf-8", "replace"))
            if name == "imap-charset-refused" and b"CHARSET UTF-8" in line.upper():
                _send_line(conn, tag + b" NO [BADCHARSET] UTF-8 is unsupported")
                continue
            if name == "imap-worker-mail":
                stored = record.get("_appended_message", b"")
                stored_id = _message_id(stored) if stored else ""
                criterion_bytes = search_tail[3].strip() if len(search_tail) == 4 else b""
                criterion = criterion_bytes.decode("utf-8", "replace")
                if criterion == "ALL":
                    _send_line(conn, b"* SEARCH 7", fragmented=True)
                    _send_line(conn, tag + b" OK SEARCH completed")
                    continue
                matched = bool(stored_id and criterion == f'HEADER Message-ID "{stored_id}"')
                record.setdefault("worker_archive_search_checks", []).append({
                    "stored_message_id": stored_id,
                    "criterion": criterion,
                    "matched": matched,
                })
                if matched:
                    record["matched_message_id"] = stored_id
                    _send_line(conn, b"* SEARCH 20", fragmented=True)
                else:
                    _send_line(conn, b"* SEARCH", fragmented=True)
                _send_line(conn, tag + b" OK SEARCH completed")
                continue
            _send_line(conn, b"* 3 EXISTS", fragmented=True)
            _send_line(conn, b"* SEARCH 7 8", fragmented=True)
            _send_line(conn, tag + b" OK SEARCH completed")
        elif command == "UID" and len(fields) > 2 and fields[2].upper() == "FETCH":
            record["fetch_fragmented"] = True
            headers_only = b"BODY.PEEK[HEADER]" in line.upper()
            if name == "imap-worker-mail" and selected_name.upper() in (b"INBOX", b"PROJECTS/INBOX") and \
                    not record.get("_appended_message") and fields[3].strip() == "7":
                literal = WORKER_FETCH_RAW
                body = literal.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n" if headers_only else literal
                item = b"BODY[HEADER]" if headers_only else b"BODY[]"
                if headers_only:
                    record["worker_fetch_headers"] = record.get("worker_fetch_headers", 0) + 1
                else:
                    record["worker_fetch_full"] = record.get("worker_fetch_full", 0) + 1
                head = (b"* 1 FETCH (UID 7 FLAGS (\\Seen \\Flagged) INTERNALDATE \"18-Oct-2026 14:17:44 +0000\" "
                        b"RFC822.SIZE " + str(len(literal)).encode() + b" " + item + b" {" +
                        str(len(body)).encode() + b"}\r\n")
                _send_fragmented(conn, head + body + b")\r\n")
                _send_line(conn, tag + b" OK FETCH completed")
                continue
            if name == "imap-worker-mail" and record.get("_appended_message"):
                literal = record["_appended_message"]
                item = b"BODY[HEADER]" if headers_only else b"BODY[]"
                body = literal.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n" if headers_only else literal
                record["verified_fetch_message_id"] = _message_id(literal)
                head = (b"* 1 FETCH (UID 20 FLAGS (\\Seen) INTERNALDATE \"19-Oct-2026 12:30:00 +0000\" "
                        b"RFC822.SIZE " + str(len(literal)).encode() + b" " + item + b" {" +
                        str(len(body)).encode() + b"}\r\n")
                _send_fragmented(conn, head + body + b")\r\n")
                _send_line(conn, tag + b" OK FETCH completed")
                continue
            budget_case = name == "imap-fetch-budget" and not headers_only
            for uid in (7, 8, 9):
                message = PROJECT_RAW.replace(b"fixture-", b"fixture-" + str(uid).encode())
                if budget_case and uid == 7:
                    literal_size = 64 * 1024 * 1024
                    literal = b"From: budget@reliefmail.test\r\nSubject: large\r\n\r\n"
                    head = (b"* 1 FETCH (UID 7 FLAGS (\\Seen) INTERNALDATE \"18-Oct-2026 10:20:30 +0800\" "
                            b"RFC822.SIZE " + str(literal_size).encode() + b" BODY[] {" +
                            str(literal_size).encode() + b"}\r\n")
                    conn.sendall(head)
                    conn.sendall(literal)
                    remaining = literal_size - len(literal)
                    fill = b"x" * 65536
                    while remaining:
                        part = fill[:min(remaining, len(fill))]
                        conn.sendall(part)
                        remaining -= len(part)
                    conn.sendall(b")\r\n")
                    continue
                if budget_case and uid == 8:
                    literal = b"x"
                    head = (b"* 2 FETCH (UID 8 FLAGS (\\Seen) INTERNALDATE \"18-Oct-2026 10:20:30 +0800\" "
                            b"RFC822.SIZE 1 BODY[] {1}\r\n")
                    conn.sendall(head + literal + b")\r\n")
                    continue
                literal = (message.split(b"\r\n\r\n", 1)[0] + b"\r\n\r\n") if headers_only else message
                item = b"BODY[HEADER]" if headers_only else b"BODY[]"
                head = (b"* 1 FETCH (UID " + str(uid).encode() +
                        b" FLAGS (\\Seen \\Flagged) INTERNALDATE \"18-Oct-2026 10:20:30 +0800\" "
                        b"RFC822.SIZE " + str(len(message)).encode() + b" " + item + b" {" +
                        str(len(literal)).encode() + b"}\r\n")
                _send_fragmented(conn, head + literal + b")\r\n")
            _send_line(conn, tag + b" OK FETCH completed")
        elif command == "UID" and len(fields) > 2 and fields[2].upper() in ("STORE", "COPY", "MOVE", "EXPUNGE"):
            subcommand = fields[2].upper()
            if subcommand == "COPY":
                _send_line(conn, tag + b" OK COPY completed")
            elif subcommand == "STORE":
                _send_line(conn, tag + b" OK STORE completed")
            elif subcommand == "MOVE" and b"MOVE" in _capabilities(name, authenticated):
                record["uid_move_uids"].append(fields[3] if len(fields) > 3 else "")
                _send_line(conn, tag + b" OK MOVE completed")
            elif subcommand == "EXPUNGE" and b"UIDPLUS" in _capabilities(name, authenticated):
                record["targeted_expunge_uids"].append(fields[3] if len(fields) > 3 else "")
                _send_line(conn, b"* 1 EXPUNGE")
                _send_line(conn, tag + b" OK UID EXPUNGE completed")
            else:
                _send_line(conn, tag + b" BAD extension unavailable")
        elif command == "EXPUNGE":
            _send_line(conn, b"* 1 EXPUNGE")
            _send_line(conn, tag + b" OK EXPUNGE completed")
        elif command == "APPEND":
            marker = text.rsplit("{", 1)[-1].rstrip("}")
            try:
                length = int(marker)
            except ValueError:
                _send_line(conn, tag + b" BAD literal required")
                continue
            _send_line(conn, b"+ Ready for literal")
            literal = _read_exact(conn, length)
            trailing = _read_line(conn)
            record["append_bytes"] = len(literal)
            record["append_crlf"] = literal.endswith(b"\r\n")
            record["append_count"] += 1
            if name == "imap-worker-mail" and trailing in (b"\r\n", b"", None):
                record["_appended_message"] = literal
                record["append_message_id"] = _message_id(literal)
                record["append_ack_lost"] = True
                return
            if trailing not in (b"\r\n", b"", None):
                _send_line(conn, tag + b" BAD malformed literal terminator")
            else:
                _send_line(conn, tag + b" OK [APPENDUID 4242 20] appended")
        elif command in ("CREATE", "RENAME", "SUBSCRIBE", "UNSUBSCRIBE", "DELETE"):
            _send_line(conn, tag + b" OK folder operation completed")
        elif command == "LOGOUT":
            _send_line(conn, b"* BYE fixture closing")
            _send_line(conn, tag + b" OK LOGOUT completed")
            return
        else:
            _send_line(conn, tag + b" BAD unsupported fixture command")


def _handle_smtp(conn: socket.socket, name: str, record: dict, tls_active: bool) -> None:
    _send_line(conn, b"220 localhost ESMTP ReliefMail fixture")
    authenticated = False
    while True:
        line = _read_line(conn)
        if line is None:
            return
        text = _record_command(line, "smtp", record, tls_active)
        fields = text.split()
        command = fields[0].upper() if fields else ""
        if command in ("EHLO", "HELO"):
            auth = b"LOGIN" if name == "smtp-login" else b"PLAIN"
            _send_line(conn, b"250-localhost")
            if name == "smtp-starttls-no-extension":
                _send_line(conn, b"250 SIZE 1048576")
            elif name == "smtp-starttls" and not tls_active:
                _send_line(conn, b"250-STARTTLS", fragmented=True)
                _send_line(conn, b"250 SIZE 1048576")
            elif tls_active:
                _send_line(conn, b"250-SIZE 1048576", fragmented=True)
                _send_line(conn, b"250-AUTH " + auth)
                if name != "smtp-no-smtputf8":
                    _send_line(conn, b"250-SMTPUTF8")
                if name != "smtp-no-8bitmime":
                    _send_line(conn, b"250 8BITMIME")
                elif name == "smtp-no-smtputf8":
                    _send_line(conn, b"250 8BITMIME")
                elif name == "smtp-no-8bitmime":
                    # Keep the final extension line syntactically complete.
                    _send_line(conn, b"250 PIPELINING")
            else:
                _send_line(conn, b"250 SIZE 1048576")
        elif command == "STARTTLS":
            if name != "smtp-starttls" or tls_active:
                _send_line(conn, b"454 4.7.0 TLS unavailable")
                continue
            _send_line(conn, b"220 2.0.0 begin TLS")
            try:
                conn = _server_tls(conn, name)
            except (ssl.SSLError, OSError):
                record["tls_handshake_failed"] = True
                return
            tls_active = True
        elif command == "AUTH":
            authenticated = _do_auth(conn, line, "smtp", record, tls_active)
        elif command == "MAIL":
            if name == "smtp-8bit":
                expected = [f"SIZE={len(SMTP_8BIT_RAW)}", "SMTPUTF8", "BODY=8BITMIME"]
                actual = record["mail_parameter_sets"][-1]
                record["strict_mail_parameters_valid"] = actual == expected
                if actual != expected:
                    _send_line(conn, b"555 5.5.4 required SMTP parameters missing or incorrect")
                    continue
            if name == "smtp-malformed-mail":
                _send_line(conn, b"not an SMTP status")
                continue
            _send_line(conn, b"250 2.1.0 sender accepted")
        elif command == "RCPT":
            if name in ("smtp-recipient-rejected", "smtp-rset-negative-recipient") and \
                    b"refused@reliefmail.test" in line.lower():
                _send_line(conn, b"550 5.1.1 recipient rejected")
            else:
                _send_line(conn, b"250 2.1.5 recipient accepted")
        elif command == "DATA":
            if name == "smtp-malformed-data":
                _send_line(conn, b"not an SMTP status")
                continue
            _send_line(conn, b"354 end with <CRLF>.<CRLF>")
            data_lines = []
            while True:
                data_line = _read_line(conn)
                if data_line is None:
                    return
                if data_line == b".\r\n":
                    break
                data_lines.append(data_line)
            record["data_completed"] = True
            record["data_crlf"] = bool(data_lines) and all(item.endswith(b"\r\n") for item in data_lines)
            record["dot_stuffed"] = any(item.startswith(b"..leading-dot\r\n") for item in data_lines)
            record["data_octets"] = sum(map(len, data_lines))
            if name == "smtp-worker":
                record["worker_attachment_filename_seen"] = any(b"task5.bin" in item for item in data_lines)
                record["worker_attachment_payload_marker_seen"] = any(
                    b"QUFBQUFBQUFB" in item for item in data_lines
                )
            if name in ("smtp-final-lost", "smtp-worker-lost"):
                record["final_reply_missing"] = True
                return
            if name == "smtp-final-inconclusive":
                _send_line(conn, b"354 unexpected final response")
                _send_line(conn, b"250 2.0.0 late final response")
                continue
            if name == "smtp-final-rejected":
                _send_line(conn, b"554 5.7.1 message rejected")
                continue
            _send_line(conn, b"250 2.0.0 queued")
        elif command == "RSET":
            if name == "smtp-rset-lost-observer":
                return
            code = 554 if name == "smtp-rset-negative-recipient" else 250
            record["rset_reply_codes"].append(code)
            _send_line(conn, ("%d reset\r\n" % code).encode("ascii"))
        elif command == "NOOP":
            _send_line(conn, b"250 ok")
        elif command == "QUIT":
            _send_line(conn, b"221 bye")
            return
        else:
            _send_line(conn, b"500 unsupported fixture command")


def _server_tls(conn: socket.socket, name: str) -> ssl.SSLSocket:
    cert_type = "expired" if name == "tls-expired" else "valid"
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    cert_dir = Path(os.environ["RELIEFMAIL_FIXTURE_CERT_DIR"])
    context.load_cert_chain(str(cert_dir / f"{cert_type}.crt"), str(cert_dir / f"{cert_type}.key"))
    return context.wrap_socket(conn, server_side=True)


def _serve_one(name: str, listener: socket.socket, record: dict) -> None:
    listener.settimeout(0.4)
    max_connections = 10 if name == "imap-worker-mail" else 4 if name == "tls-valid" else 1 if name == "smtp-worker-lost" else 2
    accept_deadline = time.monotonic() + 120.0
    for _ in range(max_connections):
        conn = None
        while conn is None and time.monotonic() < accept_deadline:
            try:
                conn, _ = listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
        if conn is None:
            return
        record["connections"] += 1
        conn.settimeout(4.0)
        tls_active = name.startswith("tls-") or (
            name.startswith("smtp-") and name not in ("smtp-starttls", "smtp-starttls-no-extension")
        ) or (name.startswith("imap-") and not name.startswith("imap-starttls"))
        try:
            if tls_active:
                try:
                    conn = _server_tls(conn, name)
                except (ssl.SSLError, OSError):
                    record["tls_handshake_failed"] = True
                    conn.close()
                    continue
            if name.startswith("tls-"):
                if name == "tls-slow":
                    time.sleep(1.0)
                    try:
                        conn.sendall(b"slow\r\n")
                    except OSError:
                        pass
                conn.close()
                continue
            if name.startswith("imap-"):
                _handle_imap(conn, name, record, tls_active)
            else:
                _handle_smtp(conn, name, record, tls_active)
        except (EOFError, OSError, ssl.SSLError, ValueError):
            pass
        finally:
            try:
                conn.close()
            except OSError:
                pass


def serve(ready_file: Path, audit_file: Path, cert_dir: Path) -> None:
    os.environ["RELIEFMAIL_FIXTURE_CERT_DIR"] = str(cert_dir)
    listeners: dict[str, socket.socket] = {}
    records = {name: _new_record(name) for name in SCENARIOS}
    threads = []
    stop = threading.Event()

    def request_stop(_signum, _frame):
        stop.set()

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    try:
        for name in SCENARIOS:
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", 0))
            listener.listen(4)
            listeners[name] = listener
        ready_file.parent.mkdir(parents=True, exist_ok=True)
        ready_file.write_text("".join(f"{name} {sock.getsockname()[1]}\n"
                                       for name, sock in listeners.items()), encoding="ascii")
        for name, listener in listeners.items():
            thread = threading.Thread(target=_serve_one, args=(name, listener, records[name]), daemon=True)
            thread.start()
            threads.append(thread)
        print(f"protocol fixture ready ({len(listeners)} loopback scripts)", flush=True)
        while not stop.wait(0.1):
            pass
    finally:
        for listener in listeners.values():
            try:
                listener.close()
            except OSError:
                pass
        for thread in threads:
            thread.join(timeout=2.0)
        audit_file.parent.mkdir(parents=True, exist_ok=True)
        for record in records.values():
            record.pop("_appended_message", None)
        audit_file.write_text(json.dumps(records, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def verify_audit(path: Path) -> None:
    records = json.loads(path.read_text(encoding="utf-8"))
    failures = []
    for name, record in records.items():
        for auth in record.get("auth", []):
            if not auth.get("tls"):
                failures.append(f"{name}: AUTH appeared before TLS")
        if record.get("insecure_auth"):
            failures.append(f"{name}: plaintext AUTH was observed")
    safe = records["imap-safe-move"]
    if safe.get("unsafe_expunge") or not safe.get("saw_uid_expunge"):
        failures.append("UIDPLUS fallback did not use only targeted UID EXPUNGE")
    unsafe = records["imap-unsafe-move"]
    if unsafe.get("unsafe_expunge") or unsafe.get("saw_uid_expunge"):
        failures.append("MOVE fallback attempted deletion without targeted UIDPLUS support")
    deleted = records["imap-delete"]
    if deleted.get("unsafe_expunge") or deleted.get("targeted_expunge_uids") != ["23"]:
        failures.append("targeted delete did not use the exact UID EXPUNGE identity")
    no_uidplus_delete = records["imap-no-uidplus-delete"]
    if no_uidplus_delete.get("unsafe_expunge") or no_uidplus_delete.get("saw_uid_expunge") or any(
        item.startswith("UID STORE") for item in no_uidplus_delete.get("commands", [])
    ):
        failures.append("delete attempted a mutation without UIDPLUS")
    transition = records["imap-auth-capability-transition"]
    if transition.get("capability_auth_states") != [False, True] or \
            transition.get("uid_move_uids") != ["5"] or \
            transition.get("targeted_expunge_uids") != ["23"] or transition.get("unsafe_expunge"):
        failures.append("authenticated MOVE/UIDPLUS capabilities were not refreshed and used for exact UIDs")
    for name in ("imap-auth-capability-reject", "imap-auth-capability-timeout",
                 "imap-auth-capability-cancel"):
        record = records[name]
        if record.get("capability_auth_states") != [False, True] or record.get("uid_move_uids") or \
                record.get("targeted_expunge_uids") or record.get("unsafe_expunge"):
            failures.append(f"{name}: failed capability refresh allowed an IMAP mutation")
    for name in ("imap-starttls-no-extension", "imap-starttls-untrusted"):
        if records[name].get("auth"):
            failures.append(f"{name}: authentication followed failed mandatory STARTTLS")
    split = records["imap-split-literal"]
    for command in ("LIST", "SELECT", "UID SEARCH", "UID FETCH"):
        if not any(item.startswith(command) for item in split.get("commands", [])):
            failures.append(f"imap-split-literal did not exercise {command}")
    if not split.get("fetch_fragmented"):
        failures.append("fragmented FETCH literal was not exercised")
    if 'CREATE "&ZbBe+g-"' not in split.get("commands", []):
        failures.append("UTF-8 mailbox creation was not encoded as modified UTF-7")
    queries = split.get("search_queries", [])
    expected_queries = (
        'HEADER Message-ID "<sent-candidate@example.test>"',
        'SUBJECT "a \\"quoted\\" \\\\ phrase"',
        'FROM "alice@reliefmail.test"',
        'BODY "body phrase"',
    )
    for query in expected_queries:
        if query not in queries:
            failures.append(f"IMAP typed search wire query missing: {query}")
    charset_refused = records["imap-charset-refused"]
    if not any("CHARSET UTF-8 SUBJECT" in query for query in charset_refused.get("search_queries", [])):
        failures.append("UTF-8 text search did not declare its required charset")
    if records["imap-fetch-budget"].get("fetch_fragmented") is not True:
        failures.append("aggregate fetch budget fixture was not exercised")
    status = records["imap-status-unrequested"]
    if status.get("status_targets") != ["INBOX", 'A "quoted" folder', PROJECT_WIRE.decode()]:
        failures.append("STATUS mailbox fixture did not cover the requested quoted and literal names")
    smtp_ok = records["smtp-success"]
    if not smtp_ok.get("data_completed") or not smtp_ok.get("data_crlf"):
        failures.append("successful SMTP DATA was not complete CRLF-delimited data")
    if not smtp_ok.get("dot_stuffed") or not smtp_ok.get("mail_size_present"):
        failures.append("successful SMTP DATA missed dot-stuffing or SIZE")
    for name in ("smtp-recipient-rejected", "smtp-observer-refused"):
        item = records[name]
        mail_commands = sum(command == "MAIL FROM <redacted>" for command in item.get("commands", []))
        if 250 not in item.get("rset_reply_codes", []) or mail_commands != 3 or not item.get("data_completed"):
            failures.append(f"{name}: successful RSET did not permit one clean subsequent submission")
    lost = records["smtp-final-lost"]
    if not lost.get("data_completed") or not lost.get("final_reply_missing"):
        failures.append("lost-final fixture did not reach the uncertain DATA boundary")
    worker_lost = records["smtp-worker-lost"]
    if not worker_lost.get("data_completed") or not worker_lost.get("final_reply_missing") or sum(
        command == "MAIL FROM <redacted>" for command in worker_lost.get("commands", [])
    ) != 1:
        failures.append("worker uncertain-send restart fixture did not preserve one delivery attempt")
    worker_smtp = records["smtp-worker"]
    if not worker_smtp.get("data_completed") or sum(
        command == "MAIL FROM <redacted>" for command in worker_smtp.get("commands", [])
    ) != 1:
        failures.append("worker archive-retry fixture did not preserve one SMTP delivery")
    if not worker_smtp.get("worker_attachment_filename_seen") or \
            not worker_smtp.get("worker_attachment_payload_marker_seen"):
        failures.append("worker SMTP DATA omitted the materialized selected-file attachment")
    worker_imap = records["imap-worker-mail"]
    expected_worker_id = "<task5-send@example.test>"
    if worker_imap.get("append_count") != 1 or not worker_imap.get("append_ack_lost") or \
            worker_imap.get("append_message_id") != expected_worker_id or \
            worker_imap.get("matched_message_id") != expected_worker_id or \
            worker_imap.get("verified_fetch_message_id") != expected_worker_id or \
            worker_imap.get("status_container_count"):
        failures.append("worker archive retry did not find and verify the one uncertain Sent APPEND")
    if worker_imap.get("worker_fetch_headers") != 3 or worker_imap.get("worker_fetch_full") != 1:
        failures.append("worker header-to-full/stale-validity cases missed expected header or full reads")
    if len(worker_imap.get("search_queries", [])) < 2:
        failures.append("worker Sent archive retry did not search by Message-ID after restart")
    if records["smtp-final-rejected"].get("data_completed") is not True:
        failures.append("explicit SMTP final rejection fixture did not complete DATA")
    for name in ("smtp-final-inconclusive", "smtp-malformed-mail", "smtp-malformed-data",
                 "smtp-rset-negative-recipient", "smtp-rset-lost-observer"):
        mail_commands = sum(command == "MAIL FROM <redacted>"
                            for command in records[name].get("commands", []))
        if mail_commands != 1:
            failures.append(f"{name}: invalidated SMTP session accepted another MAIL command")
    if records["smtp-rset-negative-recipient"].get("rset_reply_codes") != [554]:
        failures.append("negative RSET response was not exercised")
    if records["smtp-rset-lost-observer"].get("rset_reply_codes"):
        failures.append("lost RSET fixture unexpectedly sent a successful reset reply")
    if records["smtp-8bit"].get("strict_mail_parameters_valid") is not True:
        failures.append("8-bit SMTP MAIL command omitted or reordered required parameters")
    for name in ("smtp-no-smtputf8", "smtp-no-8bitmime"):
        if any(command == "MAIL FROM <redacted>" for command in records[name].get("commands", [])):
            failures.append(f"{name}: message was not refused during preflight")
    for name in ("imap-auth-failure", "smtp-auth-failure"):
        if not records[name].get("auth") or any(not item.get("tls") for item in records[name]["auth"]):
            failures.append(f"{name}: failed authentication was not exercised only after TLS")
    for name in ("smtp-starttls-no-extension",):
        if records[name].get("auth"):
            failures.append(f"{name}: authentication followed failed mandatory STARTTLS")
    serialized = path.read_text(encoding="utf-8")
    for forbidden in (PASSWORD.decode(), base64.b64encode(PASSWORD).decode(),
                      base64.b64encode(b"\0" + USER + b"\0" + PASSWORD).decode()):
        if forbidden in serialized:
            failures.append("audit transcript contains an AUTH payload")
    if failures:
        raise RuntimeError("\n".join(failures))
    auth_count = sum(len(item.get("auth", [])) for item in records.values())
    print(f"Protocol fixture audit passed: {auth_count} AUTH exchanges were encrypted; transcripts are redacted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--prepare-certs", type=Path)
    parser.add_argument("--serve", action="store_true")
    parser.add_argument("--cert-dir", type=Path)
    parser.add_argument("--ready-file", type=Path)
    parser.add_argument("--audit-file", type=Path)
    parser.add_argument("--verify-audit", type=Path)
    args = parser.parse_args()
    try:
        if args.prepare_certs:
            prepare_certificates(args.prepare_certs)
            return 0
        if args.verify_audit:
            verify_audit(args.verify_audit)
            return 0
        if args.serve and args.cert_dir and args.ready_file and args.audit_file:
            serve(args.ready_file, args.audit_file, args.cert_dir)
            return 0
        parser.error("select --prepare-certs, --verify-audit, or --serve with all required paths")
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"protocol fixture: {exc}", file=sys.stderr)
        return 1
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
