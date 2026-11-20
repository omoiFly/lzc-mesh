#!/usr/bin/env python3
"""Exercise the clipboard transport with signed fixture devices and a synthetic 8K PNG."""

import argparse
from contextlib import ExitStack
from http.client import HTTPConnection
import os
from pathlib import Path
import random
import socket
import struct
import subprocess
import tempfile
import time
import zlib

from device_identity import Identity
from hub_ws import HubSocket

IMAGE = '/api/clipboard/image'
LIMIT = 32 * 1024 * 1024


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(data, zlib.crc32(kind)))


def sample_8k():
    width, height = 7680, 4320
    noise = random.Random(8)
    compressor = zlib.compressobj(level=1)
    bands = [compressor.compress(b'\0' + noise.randbytes(width * 3)) for _ in range(1200)]
    solid_row = bytes(1 + width * 3)
    bands.extend(compressor.compress(solid_row) for _ in range(height - 1200))
    bands.append(compressor.flush())
    image = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    image += chunk(b'IDAT', b''.join(bands))
    padding = LIMIT - len(image) - 24
    assert padding >= 0
    return image + chunk(b'npAd', bytes(padding)) + chunk(b'IEND', b'')


def read_until(peer, kind, predicate=lambda _: True):
    while True:
        frame = peer.read(5)
        if frame['t'] == kind and predicate(frame):
            return frame


def connect(stack, port, account):
    return stack.enter_context(HubSocket('127.0.0.1', port=port, tls=False, headers={'X-HC-User-ID': account}))


def approve(stack, port, manager, account, device, capability):
    peer = connect(stack, port, account)
    assert read_until(peer, 'welcome')['clipboard_image']
    identity = Identity()
    hello = dict(t='device_hello', id=device, name=device, platform='linux', w=1920, h=1080, public_key=identity.public)
    if capability:
        hello['clipboard_image'] = True
    peer.send(hello)
    nonce = read_until(peer, 'device_challenge')['nonce']
    peer.send(dict(t='device_proof', signature=identity.proof(account, device, nonce)))
    assert read_until(peer, 'device_auth')['status'] == 'pending'
    manager.send(dict(t='device_trust', id=device, fingerprint=identity.fingerprint, approved=True))
    token = read_until(peer, 'device_auth', lambda frame: frame['status'] == 'approved')['token']
    return peer, {'X-HC-User-ID': account, 'X-Lzc-Mesh-Device-Id': device, 'X-Lzc-Mesh-Device-Token': token}


def http(port, method, path, headers=None, body=None, chunked=False):
    connection = HTTPConnection('127.0.0.1', port, timeout=15)
    try:
        connection.request(method, path, body, headers or {}, encode_chunked=chunked)
        response = connection.getresponse()
        return response.status, response.getheader('Cache-Control'), response.read()
    finally:
        connection.close()


def no_image(peer):
    peer.send({'t': 'ping'})
    while True:
        frame = peer.read(5)
        assert frame['t'] != 'clipboard_image'
        if frame['t'] == 'pong':
            return


def exercise(port, png):
    with ExitStack() as stack:
        account = 'clipboard-test'
        manager = connect(stack, port, account)
        manager.send({'t': 'hello', 'platform': 'web'})
        read_until(manager, 'welcome', lambda frame: bool(frame['id']))
        sender, source = approve(stack, port, manager, account, 'sender', True)
        receiver, target = approve(stack, port, manager, account, 'receiver', True)
        second, second_target = approve(stack, port, manager, account, 'second', True)
        legacy, old_target = approve(stack, port, manager, account, 'legacy', False)
        foreign_manager = connect(stack, port, 'other-account')
        foreign_manager.send({'t': 'hello', 'platform': 'web'})
        read_until(foreign_manager, 'welcome', lambda frame: bool(frame['id']))
        foreign, foreign_target = approve(stack, port, foreign_manager, 'other-account', 'receiver', True)

        def upload(body, expected, headers=source, **options):
            status, cache, image = http(port, 'PUT', IMAGE, headers | {'Content-Type': 'image/png'}, body, **options)
            assert status == expected, f'Image upload returned {status}, expected {expected}'
            if status == 201:
                assert cache == 'no-store' and len(image) == 43
            return IMAGE + '/' + image.decode()

        assert len(png) == LIMIT
        path = upload(png, 201)
        assert http(port, 'GET', path, target)[0] == 404
        sender.send({'t': 'clipboard_image', 'id': path.rsplit('/', 1)[1]})
        for peer in (receiver, second):
            offer = read_until(peer, 'clipboard_image')
            assert offer['size'] == len(png) and offer['from'] == 'sender'
        for peer in (manager, sender, legacy, foreign):
            no_image(peer)
        assert http(port, 'GET', path, {'X-HC-User-ID': account})[0] == 403
        assert http(port, 'GET', path, source)[0] == 404
        assert http(port, 'GET', path, old_target)[0] == 403
        assert http(port, 'GET', path, foreign_target)[0] == 404
        assert http(port, 'GET', path, target) == (200, 'no-store', png)
        assert http(port, 'GET', path, target)[0] == 404
        assert http(port, 'GET', path, second_target) == (200, 'no-store', png)
        assert b'"files":[]' in http(port, 'GET', '/api/files', {'X-HC-User-ID': account})[2]

        upload(None, 413, headers=source | {'Content-Length': str(LIMIT + 1)})
        upload(iter((png, b'\0')), 413, chunked=True)
        upload(png[:16] + struct.pack('>I', 7681) + png[20:100], 422)
        assert b'"files":[]' in http(port, 'GET', '/api/files', {'X-HC-User-ID': account})[2]
        oversized = png[:-12] + chunk(b'npAd', b'\0') + png[-12:]
        fallback = '/api/files/clipboard-fixture.png'
        assert http(port, 'PUT', fallback, source, oversized)[0] == 200
        assert http(port, 'GET', fallback, target)[2] == oversized

        sender.send({'t': 'clipboard', 'text': 'legacy text'})
        assert read_until(legacy, 'clipboard')['text'] == 'legacy text'

        dormant = upload(png, 201)
        sender.send({'t': 'clipboard_image', 'id': dormant.rsplit('/', 1)[1]})
        read_until(receiver, 'clipboard_image')
        read_until(second, 'clipboard_image')
        for enabled in (False, True):
            manager.send({'t': 'device_enabled', 'id': 'receiver', 'enabled': enabled})
            read_until(manager, 'state', lambda frame: any(
                device['id'] == 'receiver' and device['enabled'] == enabled
                for device in frame['devices']
            ))
        assert http(port, 'GET', dormant, target)[0] == 404
        assert http(port, 'GET', dormant, second_target)[2] == png

        pending = upload(png, 201)
        manager.send({'t': 'settings', 'clipboard': False})
        read_until(manager, 'state', lambda frame: not frame['settings']['clipboard'])
        assert http(port, 'GET', pending, target)[0] == 403
        manager.send({'t': 'settings', 'clipboard': True})
        read_until(manager, 'state', lambda frame: frame['settings']['clipboard'])
        sender.send({'t': 'clipboard_image', 'id': pending.rsplit('/', 1)[1]})
        assert http(port, 'GET', pending, target)[0] == 404
        no_image(receiver)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--hub-bin', type=Path, required=True)
    parser.add_argument('--emulator', type=Path)
    args = parser.parse_args()
    png = sample_8k()
    with tempfile.TemporaryDirectory(prefix='lzc-mesh-image-test-') as directory:
        with socket.socket() as port_picker:
            port_picker.bind(('127.0.0.1', 0))
            port = port_picker.getsockname()[1]
        environment = os.environ | {
            'LZC_MESH_LISTEN': f'127.0.0.1:{port}',
            'LZC_MESH_DATA': str(Path(directory) / 'layouts'),
            'LZC_MESH_FILES': str(Path(directory) / 'files'),
        }
        command = [str(args.hub_bin.resolve())]
        if args.emulator:
            command.insert(0, str(args.emulator.resolve()))
        with subprocess.Popen(command, env=environment, stdout=subprocess.DEVNULL) as server:
            try:
                for attempt in range(100):
                    try:
                        assert http(port, 'GET', '/api/health')[0] == 200
                        break
                    except OSError:
                        if attempt == 99:
                            raise
                        time.sleep(.05)
                exercise(port, png)
            finally:
                server.terminate()
                server.wait(timeout=5)
    print('PASS: 8K PNG at 32 MiB, upload limits, shared-file fallback, account isolation, dormancy, cleanup, and legacy text')


if __name__ == '__main__':
    main()
