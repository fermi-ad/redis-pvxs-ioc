#!/usr/bin/env python3
"""Run a test against a private loopback Redis; never use an existing server."""
import argparse
import os
import pathlib
import socket
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('--redis-server', required=True)
parser.add_argument('--timeout', type=float, default=60)
parser.add_argument('command', nargs=argparse.REMAINDER)
args = parser.parse_args()
command = args.command[1:] if args.command[:1] == ['--'] else args.command
if not command:
    parser.error('test command is required')
with tempfile.TemporaryDirectory(prefix='redis-pvxs-test-') as directory:
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    logfile = pathlib.Path(directory) / 'redis.log'
    with logfile.open('w') as log:
        redis = subprocess.Popen([args.redis_server, '--bind', '127.0.0.1', '--port', str(port),
                                  '--save', '', '--appendonly', 'no', '--dir', directory],
                                 stdout=log, stderr=subprocess.STDOUT)
        try:
            for attempt in range(100):
                if redis.poll() is not None:
                    raise RuntimeError('isolated Redis exited during startup')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.1) as client:
                        client.sendall(b'*1\r\n$4\r\nPING\r\n')
                        if client.recv(64).startswith(b'+PONG'):
                            break
                except OSError:
                    pass
                time.sleep(.05)
            else:
                raise RuntimeError('isolated Redis startup timed out')
            result = subprocess.run(command, env=dict(os.environ, REDIS_PVXS_TEST_REDIS_PORT=str(port)),
                                    timeout=args.timeout)
            if result.returncode:
                print(logfile.read_text(), flush=True)
                raise SystemExit(result.returncode)
        finally:
            redis.terminate()
            try:
                redis.wait(timeout=5)
            except subprocess.TimeoutExpired:
                redis.kill()
                redis.wait()
