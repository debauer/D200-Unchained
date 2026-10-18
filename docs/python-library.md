# Writing your own Python programs

The `d200_deck` package is a normal Python library. Use it to talk to the D200 from your own
scripts: draw on keys, react to key presses, show your own data. The package is installed
by `./install.sh` into its own environment, so for your scripts either use the `deck` tool's
environment, or install it into yours: `pip install /path/to/repository/host`
(or `uv add --editable /path/to/repository/host` in a uv project).

The library is young; names and signatures may still change between versions.

## Before you start

Your script talks to `deckd` on the device through TCP port 7200 on your PC. Two ways to make
that port available:

- run `deck serve` (it starts `deckd` and forwards the port), or
- start `deckd` yourself and run `adb forward tcp:7200 tcp:7200`.

`deckd` accepts several clients at once. Key events go to all of them, but the screen is shared
and the **last command wins**: while `deck serve` runs, it redraws its own keys when its
configuration changes, which can overwrite what your script drew. For scripts that own the whole
panel, stop `deck serve` and use the manual way above.

## Connect and draw

```python
from d200_deck.device.client import DeckClient
from d200_deck.device.protocol import Color

with DeckClient.connect() as deck:        # 127.0.0.1:7200
    info = deck.hello()                   # must be the first call
    print(info.width, info.height, len(info.keys))
    deck.set_brightness(200)              # 0-255
    deck.clear(Color(0, 0, 0))            # whole screen
    deck.fill_key(0, Color.from_hex("ff8800"))   # key 0 orange
```

Keys are numbered 0-13, row by row from the top left. `info.keys` lists each key with its
window size (`width`, `height`); the wide key 13 is bigger than the others.

## Show an image or text

Key images are raw pixels in exactly the size of the key window. Draw with Pillow and convert:

```python
from PIL import Image, ImageDraw

from d200_deck.device.client import DeckClient
from d200_deck.render.images import to_rgb565

with DeckClient.connect() as deck:
    info = deck.hello()
    key = info.key(2)
    img = Image.new("RGB", (key.width, key.height), (20, 20, 60))
    ImageDraw.Draw(img).text((20, 80), "Hello", fill=(0, 240, 255))
    deck.set_key_image(key.index, key.width, key.height, to_rgb565(img))
```

To load a picture file and fit it into a key, use `load_key_image`:

```python
from pathlib import Path

from d200_deck.render.images import Fit, load_key_image

pixels = load_key_image(Path("icon.png"), key, Fit.COVER, label="Home")
deck.set_key_image(key.index, key.width, key.height, pixels)
```

## React to key presses

```python
from d200_deck.device.client import DeckClient
from d200_deck.device.events import KeyDispatcher
from d200_deck.device.protocol import Color

with DeckClient.connect() as deck:
    deck.hello()
    count = 0

    def pressed() -> None:
        global count
        count += 1
        deck.fill_key(0, Color(0, 255 if count % 2 else 0, 0))

    keys = KeyDispatcher(deck)
    keys.on_press(0, pressed)
    keys.on_release(0, lambda: print("released"))
    keys.on_event(lambda e: print("key", e.index, "pressed" if e.pressed else "released"))
    keys.run()                            # blocks until Ctrl-C
```

Callbacks run in the thread that calls `run()`, one after the other, so keep them short and
start a thread for slow work. An exception in a callback is logged and does not stop the loop.
To stop `run()` from another thread, pass a `threading.Event` and set it.

## Without callbacks

`deck.next_event(timeout=1.0)` returns the next `KeyEvent(index, pressed)` or raises
`TimeoutError`, which is handy in a loop that also does other things:

```python
while True:
    try:
        event = deck.next_event(timeout=1.0)
    except TimeoutError:
        continue                          # nothing pressed, do periodic work here
    if event.pressed:
        print("pressed", event.index)
```

## More

- `deck.sysinfo()` returns a dict with CPU, memory, storage and temperature of the device.
- Errors derive from `d200_deck.errors`: `DeviceError` (deckd refused a command),
  `ConnectionClosedError` (connection lost), `ProtocolError`.
- The commands behind these calls are described in [protocol.md](protocol.md). Another
  language can talk to `deckd` directly.
