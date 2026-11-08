import type { ReactNode } from "react";

export default function MetricCard({
  label,
  value,
  icon,
  tone,
  detail,
}: {
  label: string;
  value: string;
  icon: ReactNode;
  tone: string;
  detail: string;
}) {
  return (
    <article className="metric-card">
      <div className="metric-top">
        <span>{label}</span>
        <span className={`metric-icon ${tone}`}>{icon}</span>
      </div>
      <div className="metric-value">{value}</div>
      <div className="metric-detail">{detail}</div>
    </article>
  );
}
