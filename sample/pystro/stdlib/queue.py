# pystro stdlib `queue` (single-threaded simplified).

class Empty(Exception):
    pass

class Full(Exception):
    pass


class Queue:
    def __init__(self, maxsize=0):
        self.maxsize = maxsize
        self._items = []
    def put(self, item, block=True, timeout=None):
        if self.maxsize > 0 and len(self._items) >= self.maxsize:
            if not block:
                raise Full()
        self._items.append(item)
    def put_nowait(self, item):
        self.put(item, block=False)
    def get(self, block=True, timeout=None):
        if not self._items:
            if not block:
                raise Empty()
        return self._items.pop(0)
    def get_nowait(self):
        return self.get(block=False)
    def empty(self):
        return not self._items
    def full(self):
        return self.maxsize > 0 and len(self._items) >= self.maxsize
    def qsize(self):
        return len(self._items)
    def task_done(self):
        pass
    def join(self):
        pass


class LifoQueue(Queue):
    def get(self, block=True, timeout=None):
        if not self._items:
            if not block: raise Empty()
        return self._items.pop()


class PriorityQueue(Queue):
    def put(self, item, block=True, timeout=None):
        Queue.put(self, item, block, timeout)
        self._items.sort()


class SimpleQueue(Queue):
    pass
