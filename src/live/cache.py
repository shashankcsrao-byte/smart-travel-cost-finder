"""
A small time-limited LRU cache, safe to share between server threads.

Why: every Ola Maps call uses quota and adds latency. Repeating the same search
within a short time can reuse the previous answer.

Data structure: an OrderedDict used as a hash map + doubly linked list.
  * get / set are O(1)
  * the least recently used entry is evicted when the cache is full
  * entries older than `ttl_seconds` are treated as missing (data stays "live")
  * a lock makes each get/set atomic: FastAPI runs requests on several threads
"""

import threading
import time
from collections import OrderedDict


class TTLCache:
    def __init__(self, max_items=256, ttl_seconds=60, clock=time.monotonic):
        self.max_items = max_items
        self.ttl_seconds = ttl_seconds
        self._clock = clock
        self._data = OrderedDict()   # key -> (expires_at, value)
        self._lock = threading.Lock()

    def get(self, key):
        with self._lock:
            item = self._data.get(key)
            if item is None:
                return None
            expires_at, value = item
            if self._clock() >= expires_at:
                self._data.pop(key, None)
                return None
            self._data.move_to_end(key)          # mark as recently used
            return value

    def set(self, key, value):
        with self._lock:
            self._data[key] = (self._clock() + self.ttl_seconds, value)
            self._data.move_to_end(key)
            while len(self._data) > self.max_items:
                self._data.popitem(last=False)   # evict least recently used

    def __len__(self):
        with self._lock:
            return len(self._data)
