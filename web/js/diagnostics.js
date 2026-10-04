const MAX_MESSAGE_LENGTH = 3500;

export function describeError(error) {
  if (!error) return "unknown";
  if (typeof error === "string") return error;
  return [error.name, error.message].filter(Boolean).join(": ") || String(error);
}

export function clientLog(event, details = {}) {
  const message = JSON.stringify({
    event,
    elapsedMs: Math.round(performance.now()),
    ...details,
  }).slice(0, MAX_MESSAGE_LENGTH);
  const url = new URL("/client-log", location.href);
  url.searchParams.set("message", message);
  void fetch(url, {
    method: "GET",
    cache: "no-store",
    credentials: "same-origin",
    keepalive: true,
  }).catch(() => {});
}
