// {name} placeholders in key text, same rules as placeholders.py on the host.

export const BUILTINS = ["time", "date"];
export const NAME = /^[a-z][a-z0-9_]{0,31}$/;
export const PENDING = "...";
const TOKEN = /\{\{|\}\}|\{([a-z][a-z0-9_]{0,31})\}/g;
const WEEKDAYS = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]; // fixed English, like the host

export function names(text) {
  return new Set([...(text || "").matchAll(TOKEN)].filter((m) => m[1]).map((m) => m[1]));
}

export const isDynamic = (text) => names(text).size > 0;

// Known names without a value show PENDING; unknown names stay as written (typos visible).
export function fill(text, values, known) {
  return (text || "").replace(TOKEN, (token, name) => {
    if (!name) return token[0];
    if (name in values) return values[name];
    return known.has(name) ? PENDING : token;
  });
}

const two = (n) => String(n).padStart(2, "0");

export function builtinValues(now) {
  return {
    time: `${two(now.getHours())}:${two(now.getMinutes())}`,
    date: `${WEEKDAYS[now.getDay()]} ${two(now.getDate())}.${two(now.getMonth() + 1)}.`,
  };
}

// Why `name` cannot be used for a new or renamed variable ("" if it can).
export function nameProblem(name, taken) {
  if (!NAME.test(name)) return "use a-z, 0-9 and _, starting with a letter";
  if (BUILTINS.includes(name)) return `"${name}" is built in`;
  if (taken.includes(name)) return `"${name}" exists already`;
  return "";
}
