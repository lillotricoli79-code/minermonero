export function formatHashrate(value: number | null | undefined): string {
  if (value == null || !Number.isFinite(value)) return "—";
  const units = ["H/s", "KH/s", "MH/s", "GH/s", "TH/s"];
  let amount = value;
  let unit = 0;
  while (amount >= 1000 && unit < units.length - 1) {
    amount /= 1000;
    unit += 1;
  }
  return `${amount.toLocaleString("it-IT", { maximumFractionDigits: 2 })} ${units[unit]}`;
}

export function formatUptime(value: number | null): string {
  if (value == null) return "—";
  const days = Math.floor(value / 86400);
  const hours = Math.floor((value % 86400) / 3600);
  const minutes = Math.floor((value % 3600) / 60);
  return days > 0 ? `${days}g ${hours}h` : `${hours}h ${minutes}m`;
}

export function formatCount(value: number | null): string {
  return value == null ? "—" : value.toLocaleString("it-IT");
}
