"""
A small time-limited LRU cache.

Why: every Ola Maps call uses quota and adds latency. Repeating the same search
within a short time can reuse the previous answer.

Data structure: an OrderedDict used as a hash map + doubly linked list.
  * get / set are O(1)
  * the least recently used entry is evicted when the cache is full
  * entries older than `ttl_seconds` are treated as missing (data stays "live")
"""

import time
from collections import OrderedDict


class TTLCache:
    def __init__(self, max_items=256, ttl_seconds=60, clock=time.monotonic):
        self.max_items = max_items
        self.ttl_seconds = ttl_seconds
        self._clock = clock
        self._data = OrderedDict()   # key -> (expires_at, value)

    def get(self, key):
        item = self._data.get(key)
        if item is None:
            return None
        expires_at, value = item
        if self._clock() >= expires_at:
            del self._data[key]
            return None
        self._data.move_to_end(key)          # mark as recently used
        return value

    def set(self, key, value):
        self._data[key] = (self._clock() + self.ttl_seconds, value)
        self._data.move_to_end(key)
        while len(self._data) > self.max_items:
            self._data.popitem(last=False)   # evict least recently used

    def __len__(self):
        return len(self._data)
