import type { ReactNode } from "react";

export default function DataRow({
  icon,
  label,
  value,
  positive = false,
}: {
  icon: ReactNode;
  label: string;
  value: string;
  positive?: boolean;
}) {
  return (
    <div className="data-row">
      <span className="data-row-label">
        <i>{icon}</i>
        {label}
      </span>
      <span className={positive ? "row-value positive" : "row-value"}>
        {value}
      </span>
    </div>
  );
}
