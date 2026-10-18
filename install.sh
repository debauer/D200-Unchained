#!/bin/sh
# Build deckd for the D200, install the `deck` command for the current user, generate
# shell completion and the systemd user unit for `deck serve` (not enabled automatically).
# Needs: cargo (rustup), uv. Afterwards just run `deck serve`.
set -e
ROOT=$(cd "$(dirname "$0")" && pwd)
TARGET=armv7-unknown-linux-musleabihf
DATA_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/deck

for tool in cargo uv; do
    command -v "$tool" >/dev/null || { echo "missing $tool: install it first" >&2; exit 1; }
done
if command -v rustup >/dev/null; then rustup target add "$TARGET"; fi
(cd "$ROOT/device" && cargo build --release --bin deckd)
mkdir -p "$DATA_DIR"
install -m 755 "$ROOT/device/target/$TARGET/release/deckd" "$DATA_DIR/deckd"
echo "deckd installed: $DATA_DIR/deckd"

cd "$ROOT/host"
uv tool install --force .

BASH_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/bash-completion/completions
ZSH_DIR=${XDG_DATA_HOME:-$HOME/.local/share}/zsh/site-functions
FISH_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/fish/completions
mkdir -p "$BASH_DIR" "$ZSH_DIR" "$FISH_DIR"

# `--show-completion` only detects the current shell, so request each script explicitly.
# Leading blank lines are stripped: zsh needs `#compdef` on the first line.
completion() { _DECK_COMPLETE="source_$1" deck | sed '/./,$!d'; }
completion bash > "$BASH_DIR/deck"
completion zsh > "$ZSH_DIR/_deck"
completion fish > "$FISH_DIR/deck.fish"

DECK=$(command -v deck)
UNIT_DIR=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user
mkdir -p "$UNIT_DIR"
sed -e "s|@DECK@|$DECK|" -e "s|@DOCS@|$(pwd)/README.md|" systemd/deck.service.in > "$UNIT_DIR/deck.service"

echo "installed: $DECK"
echo "completion written:"
echo "  bash: $BASH_DIR/deck (loaded automatically by bash-completion)"
echo "  zsh:  $ZSH_DIR/_deck (add 'fpath+=($ZSH_DIR)' before compinit in ~/.zshrc)"
echo "  fish: $FISH_DIR/deck.fish (loaded automatically)"
echo "systemd user unit: $UNIT_DIR/deck.service"
echo "  enable with: systemctl --user daemon-reload && systemctl --user enable --now deck.service"
