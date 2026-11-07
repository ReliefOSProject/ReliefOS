#!/usr/bin/env python3
import hashlib
import re
import sys
from email import policy
from email.parser import BytesParser
from email.utils import parseaddr, parsedate_to_datetime

WEEKDAYS = ("Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun")
MONTHS = ("Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec")
DATE_PATTERN = re.compile(
    r"(?P<weekday>Mon|Tue|Wed|Thu|Fri|Sat|Sun),\s+"
    r"(?P<day>0?[1-9]|[12][0-9]|3[01])\s+"
    r"(?P<month>Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec)\s+"
    r"(?P<year>[0-9]{4})\s+(?P<time>[0-9]{2}:[0-9]{2}:[0-9]{2})\s+"
    r"(?P<zone>[+-][0-9]{4})"
)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def validate_date_value(date_text):
    match = DATE_PATTERN.fullmatch(date_text)
    require(match is not None, "Date must use explicit RFC 5322 English fields and numeric timezone")
    parsed_date = parsedate_to_datetime(date_text)
    require(parsed_date.tzinfo is not None and parsed_date.utcoffset() is not None,
            "Date must include an explicit timezone")
    require(match.group("weekday") == WEEKDAYS[parsed_date.weekday()],
            "Date weekday must use a consistent English weekday token")
    require(match.group("month") in MONTHS, "Date month must use an English month token")
    require(int(match.group("day")) == parsed_date.day,
            "Date day field must agree with the parsed calendar date")
    return parsed_date


def main(path):
    with open(path, "rb") as source:
        raw = source.read()
    message = BytesParser(policy=policy.default).parsebytes(raw)

    require(message["Subject"] == "Interop 中文", "RFC 2047 Subject did not decode")
    require(parseaddr(str(message["From"]))[1] == "sender@example.com", "From mailbox mismatch")
    require(parseaddr(str(message["To"]))[1] == "to@example.net", "To mailbox mismatch")
    require(parseaddr(str(message["To"]))[0] == "收件人", "To display name did not decode")
    require(parseaddr(str(message["Cc"]))[1] == "copy@example.net", "Cc mailbox mismatch")
    require("Bcc" not in message, "Bcc leaked into serialized message headers")
    require(message["Message-ID"] == "<new@example.com>", "Message-ID mismatch")
    dates = message.get_all("Date", [])
    require(len(dates) == 1, "outgoing MIME must contain exactly one Date field")
    date_text = str(dates[0])
    for sample in ("Sun, 4 Oct 2026 14:26:14 +0000",
                   "Sun, 18 Oct 2026 14:26:14 +0000"):
        validate_date_value(sample)
    validate_date_value(date_text)
    require(message["In-Reply-To"] == "<parent@example.com>", "In-Reply-To mismatch")
    require(str(message["References"]) == "<root@example.com> <parent@example.com>",
            "References mismatch")

    parts = list(message.walk())
    alternative = next((part for part in parts
                        if part.get_content_type() == "multipart/alternative"), None)
    require(alternative is not None, "plain and rich alternatives are missing")
    alternative_parts = list(alternative.walk())
    plain = [part for part in alternative_parts if part.get_content_type() == "text/plain"]
    html = [part for part in alternative_parts if part.get_content_type() == "text/html"]
    attachments = [part for part in parts if part.get_content_disposition() == "attachment"]
    require(len(plain) == 1 and plain[0].get_content() == "Plain text body",
            "quoted-printable plain alternative mismatch")
    require(len(html) == 1 and "<p>Rich body</p>" in html[0].get_content(),
            "quoted-printable HTML alternative mismatch")
    require(len(attachments) == 2, "expected binary and forwarded-message attachments")
    attachment = next((part for part in attachments if part.get_filename() == "数据.bin"), None)
    require(attachment is not None, "binary attachment is missing")
    payload = attachment.get_payload(decode=True)
    expected = bytes([0, 1, 2, 253, 254, 13, 10])
    require(attachment.get_filename() == "数据.bin", "RFC 2231 filename did not decode")
    require(payload == expected, "decoded attachment bytes mismatch")
    digest = hashlib.sha256(payload).hexdigest()
    require(digest == "53be565da9e5aef95c28f77a36b208e213d70f9b723972d7b8aec2938fe7b24b",
            "attachment SHA-256 mismatch")
    forwarded = next((part for part in attachments if part.get_filename() == "forwarded.eml"), None)
    require(forwarded is not None, "forwarded .eml attachment is missing")
    require(forwarded.get_content_type() == "application/octet-stream",
            "Base64 .eml attachment must use application/octet-stream")
    require(forwarded.get("Content-Transfer-Encoding", "").lower() == "base64",
            ".eml payload must retain its binary-safe Base64 transfer encoding")
    require(forwarded.get_payload(decode=True) ==
            b"From: forwarded@example.test\r\nSubject: Attached message\r\n\r\nOriginal bytes\r\n",
            ".eml attachment bytes changed during serialization")
    require(not any(part.get_content_type() == "message/rfc822" and
                    part.get("Content-Transfer-Encoding", "").lower() == "base64"
                    for part in parts),
            "message/rfc822 cannot use Base64 transfer encoding")
    print("Python email parser interoperability passed")
    print("Independent attachment SHA-256: " + digest)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_reliefmail_mime_interop.py MESSAGE.eml")
    main(sys.argv[1])
