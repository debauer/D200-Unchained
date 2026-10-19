# MQTT

Keys can publish MQTT messages, and incoming messages can show values and change key colours.
This works with any broker (Mosquitto, the Home Assistant broker add-on, zigbee2mqtt, ...).

Incoming messages are only ever **shown**, as text on keys or as a colour. Nothing received is
executed, and a message cannot press keys, change pages or run commands.

## Broker

Enter the broker in the web UI (left sidebar, `// MQTT`): host, port, user, password, TLS.
An empty host turns MQTT off. The status shows `connected`, `connecting...` or the error.

The settings are stored in `mqtt.json` next to `config.json` (`$XDG_CONFIG_HOME/deck/`) with mode
`0600`. They are **not** part of `config.json`: the password is never sent back by the web API and
never ends up in the JSON export, so you can share a configuration safely. Leave the password field
empty to keep the stored one. Without TLS, user and password travel in plain text.

## Publish a message when a key is pressed

Choose the action **MQTT publish** on a key and set the topic, the payload, `retain` and the QoS.
The message is sent when the key is pressed on the device. In the web UI's TEST tab it is only
shown in the log, never sent.

```json
{"index": 0, "label": "Light", "action": {
  "type": "mqtt", "topic": "home/light/set", "payload": "toggle", "retain": false, "qos": 0}}
```

The topic has no wildcards (`+`, `#`). Messages are sent as UTF-8 text.

## Show incoming values

Add a **variable** of type `mqtt` with the topic to read. Use it in a key text like every other
variable, for example `Light: {light}`. The key shows `...` until the first message arrives;
messages sent with the retain flag arrive right after connecting.

For JSON messages enter a **JSON path** to pick one value, for example `state.power` for
`{"state": {"power": "ON"}}`, or `items.0.on` for a list. Booleans become `true` or `false`.
If the path does not exist, the key shows `!`. Values are cut at 64 characters.

```json
{"name": "light", "type": "mqtt", "topic": "home/light/state", "json_path": "state"}
```

All `mqtt` variables are subscribed at once, whichever page is shown, so a page shows current
values as soon as you open it.

## Colour rules

A key can change its colours depending on a variable. In the key editor, under **Colour rules**,
add a rule: a variable, the exact value (compared after trimming spaces) and the key and text
colour to show while it matches. The first matching rule wins; without a match the key looks as
usual. Rules work with any variable, not only MQTT ones, and they also work for keys that only
have a plain colour.

```json
{"index": 0, "label": "Light", "rules": [
  {"variable": "light", "value": "ON", "color": "ffcc00", "label_color": "000000"},
  {"variable": "light", "value": "OFF", "color": "202040"}]}
```

A key with a picture needs a stored icon to be recoloured. Pictures added in the web UI have one.

## Example: toggle a light and show its state

1. Broker: your host, port 1883.
2. Variable `light`, type `mqtt`, topic `home/light/state`.
3. Key text `Light {light}`, action **MQTT publish** to `home/light/set` with payload `toggle`.
4. Colour rule: `light` = `ON` with a yellow key colour.

Test from a shell:

```sh
mosquitto_pub -t home/light/state -m ON    # the key turns yellow
mosquitto_sub -t 'home/light/#'            # shows "toggle" when you press the key
```

## Security

- Use a broker that needs a login, and TLS if the broker is not on your own machine.
- Anyone who can publish to a topic you subscribe to can change what the key shows. Do not use
  such values for anything that matters, and subscribe only to topics you trust.
- `mqtt.json` holds the password in plain text; it is readable only by your user.
