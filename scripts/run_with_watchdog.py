#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import shlex
import signal
import subprocess
import sys
import time
from collections.abc import Sequence

TIMEOUT_EXIT_CODE = 124
COMMAND_NOT_FOUND_EXIT_CODE = 127
INTERRUPTED_EXIT_CODE = 130
DEFAULT_GRACE_SECONDS = 5.0


def _positive_float(value: str) -> float:
    try:
        parsed = float(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid float value: {value!r}") from exc

    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("value must be greater than zero")

    return parsed


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Run a command with a wall-clock watchdog.",
    )
    parser.add_argument(
        "--timeout-seconds",
        required=True,
        type=_positive_float,
        help="Wall-clock deadline for the entire command.",
    )
    parser.add_argument(
        "--grace-seconds",
        default=DEFAULT_GRACE_SECONDS,
        type=_positive_float,
        help="Grace period after termination before forcing a kill.",
    )
    parser.add_argument(
        "command",
        nargs=argparse.REMAINDER,
        help="Command to execute. Prefix with -- to stop option parsing.",
    )
    return parser


def _format_command(command: Sequence[str]) -> str:
    return shlex.join(command)


def _normalize_exit_code(return_code: int) -> int:
    if return_code < 0:
        return 128 + abs(return_code)
    return return_code


def _spawn_command(command: Sequence[str]) -> subprocess.Popen[bytes]:
    kwargs: dict[str, object] = {}
    if os.name == "nt":
        kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True

    return subprocess.Popen(command, **kwargs)


def _signal_process_tree(process: subprocess.Popen[bytes], sig: signal.Signals) -> None:
    if process.poll() is not None:
        return

    if os.name == "nt":
        if sig == signal.SIGKILL:
            process.kill()
        else:
            process.terminate()
        return

    os.killpg(process.pid, sig)


def _terminate_process_tree(
    process: subprocess.Popen[bytes],
    initial_signal: signal.Signals,
    grace_seconds: float,
) -> None:
    try:
        _signal_process_tree(process, initial_signal)
        process.wait(timeout=grace_seconds)
        return
    except ProcessLookupError:
        return
    except subprocess.TimeoutExpired:
        pass

    try:
        _signal_process_tree(process, signal.SIGKILL)
        process.wait()
    except ProcessLookupError:
        return


def run_with_watchdog(
    command: Sequence[str],
    timeout_seconds: float,
    grace_seconds: float = DEFAULT_GRACE_SECONDS,
) -> int:
    started_at = time.monotonic()

    try:
        process = _spawn_command(command)
    except FileNotFoundError as exc:
        print(
            f"run_with_watchdog.py: command not found: {exc.filename}",
            file=sys.stderr,
        )
        return COMMAND_NOT_FOUND_EXIT_CODE

    try:
        return _normalize_exit_code(process.wait(timeout=timeout_seconds))
    except subprocess.TimeoutExpired:
        elapsed = time.monotonic() - started_at
        print(
            "run_with_watchdog.py: command timed out after "
            f"{elapsed:.1f}s (limit {timeout_seconds:.1f}s): {_format_command(command)}",
            file=sys.stderr,
        )
        _terminate_process_tree(process, signal.SIGTERM, grace_seconds)
        return TIMEOUT_EXIT_CODE
    except KeyboardInterrupt:
        print(
            f"run_with_watchdog.py: interrupted: {_format_command(command)}",
            file=sys.stderr,
        )
        _terminate_process_tree(process, signal.SIGINT, grace_seconds)
        return INTERRUPTED_EXIT_CODE


def main(argv: Sequence[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("missing command")

    return run_with_watchdog(
        command,
        timeout_seconds=args.timeout_seconds,
        grace_seconds=args.grace_seconds,
    )


if __name__ == "__main__":
    sys.exit(main())
