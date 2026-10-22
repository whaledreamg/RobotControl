"""全局异步事件总线 —— 替代 Qt 的 Signal/Slot。

用法：
    from app.events.bus import event_bus
    await event_bus.publish("agv.state", {"speed": 0.5, ...})

    @event_bus.subscribe("agv.state")
    async def push_to_ws(payload): ...
"""
from __future__ import annotations

import asyncio
from collections import defaultdict
from typing import Any, Awaitable, Callable

Subscriber = Callable[[str, Any], Awaitable[None]]


class EventBus:
    def __init__(self) -> None:
        self._subs: dict[str, list[Subscriber]] = defaultdict(list)
        self._lock = asyncio.Lock()

    async def publish(self, topic: str, payload: Any) -> None:
        if not self._subs.get(topic):
            return
        # 复制一份再迭代，避免回调里 unsubscribe 时崩坏
        await asyncio.gather(
            *(cb(topic, payload) for cb in list(self._subs[topic])),
            return_exceptions=True,
        )

    def subscribe(self, topic: str) -> Callable[[Subscriber], Subscriber]:
        def deco(cb: Subscriber) -> Subscriber:
            self._subs[topic].append(cb)
            return cb
        return deco

    def unsubscribe(self, topic: str, cb: Subscriber) -> None:
        if cb in self._subs.get(topic, []):
            self._subs[topic].remove(cb)


event_bus = EventBus()
