# pystro stub for `marshal` (CPython binary serialiser used for .pyc).
# Implementations approximate using pickle.

import pickle as _pickle


version = 4


def dumps(obj, version=4):
    return _pickle.dumps(obj)


def loads(data):
    return _pickle.loads(data)


def dump(obj, file, version=4):
    file.write(dumps(obj, version))


def load(file):
    data = file.read()
    return loads(data)


__all__ = ["dumps", "loads", "dump", "load", "version"]
