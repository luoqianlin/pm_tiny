#!/usr/bin/env python3

import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def run_case(pm, command, read_request, large_environment=False):
    with tempfile.TemporaryDirectory(prefix="pm_tiny_cli_signal.") as tmp:
        socket_path = os.path.join(tmp, "pm.sock")
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(socket_path)
        listener.listen(1)
        accepted = threading.Event()
        stop = threading.Event()

        def serve():
            connection, _ = listener.accept()
            accepted.set()
            if read_request:
                connection.recv(4096)
            stop.wait(5)
            connection.close()

        thread = threading.Thread(target=serve)
        thread.start()
        environment = os.environ.copy()
        environment.update({
            "PM_TINY_HOME": tmp,
            "PM_TINY_SOCK_FILE": socket_path,
            "PM_TINY_UDS_ABSTRACT_NAMESPACE": "0",
        })
        if large_environment:
            payload = "x" * 65536
            for index in range(16):
                environment[f"PM_TINY_SIGNAL_TEST_{index}"] = payload

        client = subprocess.Popen([pm] + command, env=environment,
                                  stdout=subprocess.DEVNULL,
                                  stderr=subprocess.PIPE,
                                  text=True)
        try:
            if not accepted.wait(2):
                raise RuntimeError("client did not connect to the test server")
            time.sleep(0.1)
            client.send_signal(signal.SIGINT)
            try:
                return_code = client.wait(timeout=2)
            except subprocess.TimeoutExpired as error:
                client.terminate()
                client.wait(timeout=2)
                raise RuntimeError("client did not exit after SIGINT") from error
            if return_code != 130:
                stderr = client.stderr.read() if client.stderr else ""
                raise RuntimeError(f"client exited with {return_code}: {stderr}")
        finally:
            stop.set()
            listener.close()
            thread.join(timeout=2)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli_signal_integration.py /path/to/pm")
    pm = os.path.abspath(sys.argv[1])
    run_case(pm, ["ls"], read_request=True)
    run_case(pm, ["start", "blocked_write", "--", "/usr/bin/sleep", "1"],
             read_request=False, large_environment=True)
    print("cli signal integration: PASS")


if __name__ == "__main__":
    main()
