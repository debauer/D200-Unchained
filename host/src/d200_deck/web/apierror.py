"""Error carrying an HTTP status, raised by request handlers and turned into a JSON error."""

from __future__ import annotations

from http import HTTPStatus


class ApiError(Exception):
    def __init__(self, status: HTTPStatus, message: str, headers: dict[str, str] | None = None) -> None:
        super().__init__(message)
        self.status = status
        self.headers = headers or {}
