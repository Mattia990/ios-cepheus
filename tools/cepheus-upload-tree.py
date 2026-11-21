#!/usr/bin/env python3
"""Upload a directory tree to the running phone over the file server.

  tools/cepheus-upload-tree.py LOCAL_DIR REMOTE_DIR

Creates directories, uploads regular files with their modes, and recreates
symlinks. Set CEPH_FILES_HOST=192.168.1.100 to talk to the phone directly
(default 127.0.0.1:2223 via FlameBridge).

The file server handles one command per connection (bsd/dev/sweet_files.c),
so this opens a connection per file; a bundle is a few hundred round trips.
"""
import os, socket, stat, sys

HOST, PORT = os.environ.get('CEPH_FILES_HOST', '127.0.0.1'), 2223


def cmd(line, payload=b''):
    s = socket.create_connection((HOST, PORT), timeout=30)
    s.sendall(line.encode() + b'\n' + payload)
    buf = b''
    while not buf.endswith(b'\n'):
        c = s.recv(1)
        if not c:
            raise SystemExit('connection closed without a reply')
        buf += c
    s.close()
    text = buf.decode().strip()
    if not text.startswith('OK'):
        raise SystemExit(f'phone: {line.split()[0]} {text}')
    return text


def main(argv):
    if len(argv) != 2:
        raise SystemExit(__doc__)
    src, dst = argv
    src = src.rstrip('/')
    dst = dst.rstrip('/')

    def walk(local, remote):
        st = os.lstat(local)
        if stat.S_ISLNK(st.st_mode):
            cmd(f'LINK {os.readlink(local)} {remote}')
            return
        if stat.S_ISDIR(st.st_mode):
            cmd(f'MKDIR {remote} {stat.S_IMODE(st.st_mode):o}')
            for name in sorted(os.listdir(local)):
                walk(os.path.join(local, name), remote + '/' + name)
            return
        mode = stat.S_IMODE(st.st_mode)
        data = open(local, 'rb').read()
        cmd(f'PUT {remote} {mode:o} {len(data)}', data)

    walk(src, dst)
    print(f'uploaded {src} -> {dst}')


if __name__ == '__main__':
    main(sys.argv[1:])
