import csv
import sys
from pathlib import Path

import plotly.graph_objects as go
from plotly.subplots import make_subplots


def loadLogs(csvPath):
    """Parse the CSV and return lists keyed by column name."""
    rows = []
    with open(csvPath, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            rows.append(row)

    # Convert all values to float where possible
    data = {}
    if not rows:
        return data
    for col in rows[0].keys():
        try:
            data[col] = [float(r[col]) for r in rows]
        except ValueError:
            data[col] = [r[col] for r in rows]
    return data


def plot3dPath(data):
    """Interactive 3D plot of the plane trajectory and target."""
    fig = go.Figure()

    # Plane trajectory
    fig.add_trace(go.Scatter3d(
        x=data["PlanePositionX"],
        y=data["PlanePositionY"],
        z=data["PlanePositionZ"],
        mode="lines+markers",
        name="Plane Path",
        line=dict(color="dodgerblue", width=3),
        marker=dict(size=3, color=data["Iteration"], colorscale="Viridis",
                    colorbar=dict(title="Iteration", x=0.82)),
    ))

    # Start and end markers
    fig.add_trace(go.Scatter3d(
        x=[data["PlanePositionX"][0]],
        y=[data["PlanePositionY"][0]],
        z=[data["PlanePositionZ"][0]],
        mode="markers",
        name="Start",
        marker=dict(color="limegreen", size=8, symbol="circle"),
    ))
    fig.add_trace(go.Scatter3d(
        x=[data["PlanePositionX"][-1]],
        y=[data["PlanePositionY"][-1]],
        z=[data["PlanePositionZ"][-1]],
        mode="markers",
        name="End",
        marker=dict(color="crimson", size=8, symbol="circle"),
    ))

    # Target
    fig.add_trace(go.Scatter3d(
        x=[data["TargetPositionX"][0]],
        y=[data["TargetPositionY"][0]],
        z=[data["TargetPositionZ"][0]],
        mode="markers",
        name="Target",
        marker=dict(color="gold", size=10, symbol="diamond"),
    ))

    # Line from last plane position to target
    fig.add_trace(go.Scatter3d(
        x=[data["PlanePositionX"][-1], data["TargetPositionX"][0]],
        y=[data["PlanePositionY"][-1], data["TargetPositionY"][0]],
        z=[data["PlanePositionZ"][-1], data["TargetPositionZ"][0]],
        mode="lines",
        name="Final Offset",
        line=dict(color="red", dash="dash", width=2),
    ))

    fig.update_layout(
        title="Flight Path to Target (3D)",
        scene=dict(
            xaxis_title="X",
            yaxis_title="Y",
            zaxis_title="Z",
            aspectmode="data",
        ),
        height=700,
        margin=dict(l=0, r=0, t=40, b=0),
    )
    return fig


def plotDashboard(data):
    """2x2 dashboard: controls, losses, distance, velocity."""
    fig = make_subplots(
        rows=2, cols=2,
        subplot_titles=(
            "Control Surface Values",
            "Alignment Loss (radians)",
            "Distance to Target",
            "Velocity Components",
        ),
        vertical_spacing=0.12,
        horizontal_spacing=0.10,
    )

    iterations = data["Iteration"]

    # --- Controls ---
    fig.add_trace(go.Scatter(
        x=iterations, y=data["Aileron"], name="Aileron",
        line=dict(color="coral"), legendgroup="controls",
    ), row=1, col=1)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["Elevator"], name="Elevator",
        line=dict(color="mediumseagreen"), legendgroup="controls",
    ), row=1, col=1)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["Rudder"], name="Rudder",
        line=dict(color="steelblue"), legendgroup="controls",
    ), row=1, col=1)

    # --- Losses ---
    fig.add_trace(go.Scatter(
        x=iterations, y=data["AileronLoss"], name="Roll Loss",
        line=dict(color="coral"), legendgroup="losses",
    ), row=1, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["ElevatorLoss"], name="Pitch Loss",
        line=dict(color="mediumseagreen"), legendgroup="losses",
    ), row=1, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["RudderLoss"], name="Yaw Loss",
        line=dict(color="steelblue"), legendgroup="losses",
    ), row=1, col=2)

    # --- Distance ---
    fig.add_trace(go.Scatter(
        x=iterations, y=data["DistanceToTarget"], name="Distance",
        line=dict(color="firebrick"), fill="tozeroy",
        fillcolor="rgba(178,34,34,0.1)",
    ), row=2, col=1)

    # --- Velocity ---
    vx = data["PlaneVelocityX"]
    vy = data["PlaneVelocityY"]
    vz = data["PlaneVelocityZ"]
    totalV = [(vx[i]**2 + vy[i]**2 + vz[i]**2)**0.5 for i in range(len(vx))]

    fig.add_trace(go.Scatter(
        x=iterations, y=data["PlaneVelocityX"], name="Vx",
        line=dict(color="mediumpurple"), legendgroup="vel",
    ), row=2, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["PlaneVelocityY"], name="Vy",
        line=dict(color="goldenrod"), legendgroup="vel",
    ), row=2, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["PlaneVelocityZ"], name="Vz",
        line=dict(color="darkcyan"), legendgroup="vel",
    ), row=2, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=totalV, name="|V|",
        line=dict(color="orangered", width=3, dash="dot"), legendgroup="vel",
    ), row=2, col=2)

    fig.update_xaxes(title_text="Iteration", row=2, col=1)
    fig.update_xaxes(title_text="Iteration", row=2, col=2)
    fig.update_yaxes(title_text="Value [0,1]", row=1, col=1)
    fig.update_yaxes(title_text="Loss (rad)", row=1, col=2)
    fig.update_yaxes(title_text="Distance (units)", row=2, col=1)
    fig.update_yaxes(title_text="Velocity (units/s)", row=2, col=2)

    fig.update_layout(
        title="Flight Controller Diagnostics",
        height=800,
        hovermode="x unified",
    )
    return fig


def main():
    scriptDir = Path(__file__).resolve().parent
    csvPath = scriptDir / "flightControlLogs.csv"

    if not csvPath.exists():
        print(f"CSV not found: {csvPath}", file=sys.stderr)
        sys.exit(1)

    data = loadLogs(str(csvPath))
    if not data:
        print("CSV is empty or could not be parsed.", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(data['Iteration'])} log entries from {csvPath}")

    fig3d = plot3dPath(data)
    figDashboard = plotDashboard(data)

    # Show both figures — they open in separate browser tabs
    fig3d.show()
    figDashboard.show()


if __name__ == "__main__":
    main()
