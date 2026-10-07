#!/usr/bin/env python3
"""Launch real mock supplier processes and exercise C++ clients over loopback."""
import argparse
import json
from pathlib import Path
import selectors
import subprocess
import sys


def run(args, fault="none", service="llm", mode="success", demo=False):
    server_path = Path(__file__).resolve().parents[1] / "apps" / "mock_suppliers.py"
    server = subprocess.Popen([sys.executable, str(server_path), "--port", "0",
                               "--fault", fault, "--fault-service", service],
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(server.stdout, selectors.EVENT_READ)
            if not selector.select(timeout=10):
                raise RuntimeError("mock server readiness timeout")
        line = server.stdout.readline()
        if not line:
            raise RuntimeError("mock server exited: " + server.stderr.read())
        ready = json.loads(line)
        assert ready["ready"]
        port = str(ready["port"])
        subprocess.run([args.client, port, mode], check=True, timeout=15)
        if demo:
            subprocess.run([args.demo, port], check=True, timeout=15)
    finally:
        server.terminate()
        try:
            _, errors = server.communicate(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.communicate()
            raise RuntimeError("mock server failed cooperative shutdown")
        if server.returncode not in (0, -15):
            raise RuntimeError("mock server failed: " + errors)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", required=True)
    parser.add_argument("--demo", required=True)
    options = parser.parse_args()
    run(options, demo=True)
    for fault in ("fragment", "ping"):
        run(options, fault=fault)
    for fault in ("disconnect", "malformed", "oversize", "wrong_id", "error", "stall"):
        run(options, fault=fault, mode=fault)
    for service in ("asr", "llm", "tts"):
        run(options, fault="stall_after_first", service=service, mode="cancel_" + service)
    run(options, fault="bad_pcm", service="tts", mode="bad_pcm")
    run(options, fault="disconnect", service="asr", mode="asr_failure")
    print("14 WebSocket process scenarios and pipeline demo passed")
