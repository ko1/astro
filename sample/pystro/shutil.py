# pystro stdlib `shutil` (subset).
import os


def copy(src, dst):
    return copyfile(src, dst)


def copyfile(src, dst):
    with open(src, "rb") as f:
        data = f.read()
    if os.path.isdir(dst):
        dst = os.path.join(dst, os.path.basename(src))
    with open(dst, "wb") as f:
        f.write(data)
    return dst


def copy2(src, dst):
    return copyfile(src, dst)


def copytree(src, dst):
    os.makedirs(dst, exist_ok=True) if hasattr(os, "makedirs") else os.mkdir(dst)
    for entry in os.listdir(src):
        s = os.path.join(src, entry)
        d = os.path.join(dst, entry)
        if os.path.isdir(s):
            copytree(s, d)
        else:
            copyfile(s, d)
    return dst


def move(src, dst):
    copyfile(src, dst)
    os.remove(src)
    return dst


def rmtree(path):
    for entry in os.listdir(path):
        p = os.path.join(path, entry)
        if os.path.isdir(p):
            rmtree(p)
        else:
            os.remove(p)
    os.rmdir(path)


def which(cmd):
    p = os.environ.get("PATH", "")
    for d in p.split(":"):
        full = os.path.join(d, cmd)
        if os.path.exists(full):
            return full
    return None


def disk_usage(path):
    # Stub.
    class Usage:
        total = 0
        used = 0
        free = 0
    return Usage()
