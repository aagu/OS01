"""Shared post-marker panic observation window helpers."""

from __future__ import annotations

_PANIC_TOKENS = (
    "kernel panic",
    "panic:",
    "fatal exception",
)


def panic_in(text: str) -> bool:
    """Return True if ``text`` contains a kernel-panic marker."""
    low = text.lower()
    return any(tok in low for tok in _PANIC_TOKENS)


def panic_window(session, seconds: float = 1.0) -> bool:
    """Observe ``session`` for ``seconds`` and return True if a panic is seen."""
    tail = session.observe(seconds)
    return panic_in(tail)
