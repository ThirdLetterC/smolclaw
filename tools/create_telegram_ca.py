#!/usr/bin/env python3.14
"""Create a small CA bundle for Telegram and Gemini HTTPS requests.

The generated bundle contains the non-leaf CA certificates currently sent by the
configured endpoints. By default it includes Telegram plus several Google APIs
hostnames so the bundle captures common alternate Google Trust Services chains.
It can be used as a process-wide SmolClaw CA bundle:

    python3.14 tools/create_telegram_ca.py -o telegram-ca.pem
    SC_CA_BUNDLE=$PWD/telegram-ca.pem build/smolclaw-c/smolclaw chat

This script does not embed certificate material. It fetches the live TLS chain
with openssl, writes only CA certificates, and verifies every observed leaf
certificate against the resulting bundle before replacing the output file.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path


PEM_RE = re.compile(
    rb"-----BEGIN CERTIFICATE-----\r?\n.*?\r?\n-----END CERTIFICATE-----\r?\n?",
    re.DOTALL,
)
DEFAULT_HOSTS = (
    "api.telegram.org",
    "generativelanguage.googleapis.com",
    "www.googleapis.com",
    "oauth2.googleapis.com",
    "googleapis.com",
)


def run_openssl(args: list[str], input_data: bytes | None = None) -> bytes:
    try:
        completed = subprocess.run(
            ["openssl", *args],
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True,
        )
    except FileNotFoundError:
        raise SystemExit("openssl was not found in PATH") from None
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr.decode("utf-8", errors="replace").strip()
        raise SystemExit(f"openssl {' '.join(args)} failed: {stderr}") from None
    return completed.stdout


def fetch_server_chain(host: str, port: int, timeout_seconds: int) -> list[bytes]:
    connect = f"{host}:{port}"
    try:
        completed = subprocess.run(
            [
                "openssl",
                "s_client",
                "-showcerts",
                "-servername",
                host,
                "-connect",
                connect,
                "-verify_return_error",
            ],
            input=b"",
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout_seconds,
            check=False,
        )
    except FileNotFoundError:
        raise SystemExit("openssl was not found in PATH") from None
    except subprocess.TimeoutExpired:
        raise SystemExit(f"timed out fetching TLS chain from {connect}") from None

    output = completed.stdout + completed.stderr
    certs = PEM_RE.findall(output)
    if not certs and completed.returncode != 0:
        detail = completed.stderr.decode("utf-8", errors="replace").strip()
        raise SystemExit(
            f"failed to fetch TLS chain from {connect}: {detail}"
        ) from None
    if len(certs) < 2:
        raise SystemExit(f"{connect} did not provide a usable CA chain")
    return certs


def cert_text(cert: bytes) -> str:
    return run_openssl(["x509", "-noout", "-subject", "-issuer", "-text"], cert).decode(
        "utf-8",
        errors="replace",
    )


def is_ca_certificate(cert: bytes) -> bool:
    text = cert_text(cert)
    return "CA:TRUE" in text


def describe_cert(cert: bytes) -> str:
    text = run_openssl(["x509", "-noout", "-subject", "-issuer"], cert).decode(
        "utf-8",
        errors="replace",
    )
    return " ".join(line.strip() for line in text.splitlines() if line.strip())


def cert_fingerprint(cert: bytes) -> str:
    text = run_openssl(["x509", "-noout", "-sha256", "-fingerprint"], cert).decode(
        "utf-8",
        errors="replace",
    )
    return text.strip()


def verify_bundle(leaf: bytes, intermediates: list[bytes], ca_bundle: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="telegram-ca-verify-") as tmp:
        tmpdir = Path(tmp)
        leaf_path = tmpdir / "leaf.pem"
        untrusted_path = tmpdir / "untrusted.pem"
        leaf_path.write_bytes(leaf)
        untrusted_path.write_bytes(b"".join(intermediates))
        run_openssl(
            [
                "verify",
                "-partial_chain",
                "-CAfile",
                str(ca_bundle),
                "-untrusted",
                str(untrusted_path),
                str(leaf_path),
            ]
        )


def write_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(
        mode="wb",
        prefix=f".{path.name}.",
        dir=path.parent,
        delete=False,
    ) as tmp:
        tmp.write(data)
        tmp_path = Path(tmp.name)
    os.replace(tmp_path, path)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "-o", "--output", default="telegram-ca.pem", help="output PEM path"
    )
    parser.add_argument(
        "--host",
        action="append",
        default=None,
        help="TLS hostname to include; repeat for multiple hosts",
    )
    parser.add_argument("--port", type=int, default=443, help="TLS port for all hosts")
    parser.add_argument(
        "--timeout", type=int, default=10, help="TLS fetch timeout in seconds"
    )
    parser.add_argument(
        "--samples",
        type=int,
        default=3,
        help="TLS chain fetches per host; repeated samples catch rotated CA chains",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    output = Path(args.output)
    hosts = args.host if args.host is not None else list(DEFAULT_HOSTS)
    if args.samples < 1:
        raise SystemExit("--samples must be at least 1")

    leaves: list[tuple[str, int, bytes, list[bytes]]] = []
    ca_by_fingerprint: dict[str, bytes] = {}

    for host in hosts:
        for sample in range(1, args.samples + 1):
            certs = fetch_server_chain(host, args.port, args.timeout)
            leaf = certs[0]
            ca_certs = [cert for cert in certs[1:] if is_ca_certificate(cert)]
            if not ca_certs:
                raise SystemExit(
                    f"{host}:{args.port} did not include any CA certificates"
                )
            leaves.append((host, sample, leaf, ca_certs))
            for cert in ca_certs:
                ca_by_fingerprint.setdefault(cert_fingerprint(cert), cert)

    bundle_certs = list(ca_by_fingerprint.values())
    bundle = b"".join(bundle_certs)
    with tempfile.TemporaryDirectory(prefix="telegram-ca-bundle-") as tmp:
        candidate = Path(tmp) / "telegram-ca.pem"
        candidate.write_bytes(bundle)
        for host, sample, leaf, ca_certs in leaves:
            try:
                verify_bundle(leaf, ca_certs, candidate)
            except SystemExit as exc:
                raise SystemExit(
                    f"{host}:{args.port} sample {sample} verification failed: {exc}"
                ) from None

    write_atomic(output, bundle)
    print(f"wrote {output} with {len(bundle_certs)} unique CA certificate(s)")
    for host, sample, _, ca_certs in leaves:
        print(
            f"verified {host}:{args.port} sample {sample} "
            f"with {len(ca_certs)} CA certificate(s) from server chain"
        )
    for index, cert in enumerate(bundle_certs, start=1):
        print(f"{index}: {describe_cert(cert)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
