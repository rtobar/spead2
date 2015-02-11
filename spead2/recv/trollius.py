"""
Integration between spead2.recv and trollius
"""
from __future__ import absolute_import
import trollius
from trollius import From, Return
import collections
import spead2.recv

class Stream(spead2.recv.Stream):
    """Stream where `get` is a coroutine that yields the next heap.

    Internally, it maintains a queue of waiters, each represented by a future.
    When a heap becomes available, it is passed to the first waiter. We use
    a callback on a file description being readable, which happens when there
    might be data available. The callback is enabled when we have at least one
    waiter, otherwise disabled.

    Parameters
    ----------
    loop : event loop, optional
        Default event loop
    """

    def __init__(self, *args, **kwargs):
        if 'loop' in kwargs:
            self._loop = kwargs.pop('loop')
        else:
            self._loop = trollius.get_event_loop()
        super(Stream, self).__init__(*args, **kwargs)
        self._waiters = collections.deque()
        self._listening = False

    def _start_listening(self):
        if not self._listening:
            self._loop.add_reader(self.fd, self._ready_callback)
            self._listening = True

    def _stop_listening(self):
        if self._listening:
            self._loop.remove_reader(self.fd)
            self._listening = False

    def _clear_done_waiters(self):
        """Remove waiters that are done (should only happen if they are cancelled)"""
        while self._waiters and self._waiters[0].done():
            self._waiters.popleft()
        if not self._waiters:
            self._stop_listening()

    def _ready_callback(self):
        self._clear_done_waiters()
        if self._waiters:
            try:
                heap = self.get_nowait()
            except spead2.Empty:
                # This can happen if we were woken up for an incomplete heap.
                pass
            except spead2.Stopped as e:
                for waiter in self._waiters:
                    waiter.set_exception(e)
                self._waiters = []
                self._stop_listening()
            else:
                waiter = self._waiters.popleft()
                waiter.set_result(heap)
                if not self._waiters:
                    self._stop_listening()

    def get(self, loop=None):
        """Coroutine that waits for a heap to become available and returns it."""
        self._clear_done_waiters()
        if not self._waiters:
            # If something is available directly, we can avoid going back to
            # the scheduler
            try:
                heap = self.get_nowait()
            except spead2.Empty:
                pass
            else:
                raise Return(heap)

        if loop is None:
            loop = self._loop
        waiter = trollius.Future(loop=loop)
        self._waiters.append(waiter)
        self._start_listening()
        heap = yield From(waiter)
        raise Return(heap)