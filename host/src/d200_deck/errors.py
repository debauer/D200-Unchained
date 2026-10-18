"""Exception types of the deckd client."""

from __future__ import annotations


class DeckError(Exception):
    """Base class for all client errors."""


class ConnectionClosedError(DeckError):
    """The device closed the connection."""


class ProtocolError(DeckError):
    """The device sent something that violates the protocol."""


class DeviceError(DeckError):
    """The device rejected a command (ERROR reply)."""


class InvalidColorError(DeckError, ValueError):
    """A colour string could not be parsed."""


class ImageLoadError(DeckError):
    """An image file could not be read or decoded."""


class ConfigError(DeckError):
    """The host configuration file is missing values or malformed."""


class UnknownKeyError(DeckError):
    """A key index that the device layout does not have."""


class UnknownPageError(DeckError):
    """A page id the configuration does not have."""


class ImageSizeError(DeckError):
    """Raw key image data does not match the key window size."""


class AdbError(DeckError):
    """An adb command failed on the host side (adb missing, server error, ...)."""


class DeviceNotFoundError(DeckError):
    """No D200 is connected via adb."""


class BootstrapError(DeckError):
    """deckd could not be installed or started on the device."""
