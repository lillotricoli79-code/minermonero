export async function request<T>(
  path: string,
  init?: RequestInit,
): Promise<T> {
  const response = await fetch(`/api${path}`, {
    ...init,
    headers: {
      ...(init?.body ? { "Content-Type": "application/json" } : {}),
      ...init?.headers,
    },
  });

  const contentType = response.headers.get("content-type") ?? "";
  let payload: Record<string, unknown> = {};
  if (contentType.toLowerCase().includes("application/json")) {
    const parsed: unknown = await response.json().catch(() => ({}));
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
      payload = parsed as Record<string, unknown>;
    }
  } else {
    const body = await response.text().catch(() => "");
    if (body) payload.detail = body.slice(0, 240);
  }

  if (!response.ok) {
    const detail = typeof payload.detail === "string"
      ? payload.detail
      : `Richiesta fallita (${response.status}) su /api${path}`;
    throw new Error(detail);
  }
  if (!contentType.toLowerCase().includes("application/json")) {
    throw new Error(`Risposta non JSON dal bridge su /api${path} (HTTP ${response.status}). Verifica backend e percorso API.`);
  }
  return payload as T;
}
