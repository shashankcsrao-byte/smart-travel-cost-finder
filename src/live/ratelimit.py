"""
Sliding-window rate limiter.

Each client key (here: the client's IP address) keeps a deque of the times of its
recent requests. On every request, times older than the window are dropped from
the left (O(1) each), and the request is allowed only if fewer than `limit`
remain. This protects the paid Ola Maps quota from runaway clients.
"""

import threading
import time
from collections import defaultdict, deque


class SlidingWindowLimiter:
    def __init__(self, limit, window_seconds, clock=time.monotonic):
        self.limit = limit
        self.window = window_seconds
        self._clock = clock
        self._hits = defaultdict(deque)
        self._lock = threading.Lock()

    def allow(self, key):
        """True if this request may go ahead (and records it)."""
        now = self._clock()
        with self._lock:
            hits = self._hits[key]
            while hits and now - hits[0] >= self.window:
                hits.popleft()
            if len(hits) >= self.limit:
                return False
            hits.append(now)
            if len(self._hits) > 10_000:          # forget idle clients
                for k in [k for k, v in self._hits.items() if not v]:
                    del self._hits[k]
            return True

    def retry_after(self, key):
        """Seconds until the oldest request in the window expires."""
        with self._lock:
            hits = self._hits.get(key)
            if not hits:
                return 0
            return max(0, int(self.window - (self._clock() - hits[0])) + 1)
