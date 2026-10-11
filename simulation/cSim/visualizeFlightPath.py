import csv
import sys
from pathlib import Path

import plotly.graph_objects as go
import streamlit as st
from plotly.subplots import make_subplots

SCRIPT_DIR = Path(__file__).resolve().parent
LOG_FILE = SCRIPT_DIR / "flightControlLogs.csv"

if not st.runtime.exists():
    sys.exit("Run with: streamlit run simulation/cSim/visualizeFlightPath.py")


@st.cache_data(show_spinner=False)
def loadLogs(logPath, mtime):
    """Parse the combined CSV into {model: {column: [values]}}."""
    runs = {}
    with open(logPath, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            run = runs.get(row["Model"])
            if run is None:
                run = runs[row["Model"]] = {col: [] for col in row if col != "Model"}
            for col, value in row.items():
                if col == "Model":
                    continue
                try:
                    run[col].append(float(value))
                except ValueError:
                    run[col].append(value)
    return runs


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

    # Orientation frames sampled along the path — forward (blue), up (green), right (red)
    n = len(data["PlanePositionX"])
    step = max(1, n // 40)
    idx = list(range(0, n, step))
    px = [data["PlanePositionX"][i] for i in idx]
    py = [data["PlanePositionY"][i] for i in idx]
    pz = [data["PlanePositionZ"][i] for i in idx]
    # Scale factor: ~2% of the path bounding box diagonal so cones are scene-relative
    xs, ys, zs = data["PlanePositionX"], data["PlanePositionY"], data["PlanePositionZ"]
    diagLen = ((max(xs)-min(xs))**2 + (max(ys)-min(ys))**2 + (max(zs)-min(zs))**2) ** 0.5
    arrowSize = max(diagLen * 0.0006, 0.1)

    orientationAxes = [
        ("Forward (nose)",  "PlaneForwardX", "PlaneForwardY", "PlaneForwardZ", "dodgerblue",  arrowSize),
        ("Up (top)",        "PlaneUpX",      "PlaneUpY",      "PlaneUpZ",      "limegreen",   arrowSize * 0.7),
        ("Right (wing)",    "PlaneRightX",   "PlaneRightY",   "PlaneRightZ",   "tomato",      arrowSize * 0.7),
    ]
    for axisName, ux, uy, uz, color, sz in orientationAxes:
        fig.add_trace(go.Cone(
            x=px, y=py, z=pz,
            u=[data[ux][i] for i in idx],
            v=[data[uy][i] for i in idx],
            w=[data[uz][i] for i in idx],
            sizemode="absolute", sizeref=sz, anchor="tail",
            colorscale=[[0, color], [1, color]],
            showscale=False, showlegend=True, name=axisName,
            legendgroup="orientation",
            hovertemplate=f"%{{customdata}} — {axisName}<extra></extra>",
            customdata=[int(data["Iteration"][i]) for i in idx],
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
        rows=3, cols=2,
        subplot_titles=(
            "Control Surface Values",
            "Alignment Loss (radians)",
            "Distance to Target",
            "Aggregate Metrics",
            "Velocity Components",
            "Loss Angle Change (rad)",
        ),
        vertical_spacing=0.08,
        horizontal_spacing=0.10,
    )

    iterations = data["Iteration"]
    distances = data["DistanceToTarget"]

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
        line=dict(color="coral", dash="dot"), legendgroup="losses",
    ), row=1, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["ElevatorLoss"], name="Pitch Loss",
        line=dict(color="mediumseagreen", dash="dot"), legendgroup="losses",
    ), row=1, col=2)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["RudderLoss"], name="Yaw Loss",
        line=dict(color="steelblue", dash="dot"), legendgroup="losses",
    ), row=1, col=2)

    # Aggregate loss (sum of all three axis losses)
    combinedLoss = [data["AileronLoss"][i] + data["ElevatorLoss"][i] + data["RudderLoss"][i]
                    for i in range(len(iterations))]
    fig.add_trace(go.Scatter(
        x=iterations, y=combinedLoss, name="Combined Loss",
        line=dict(color="black", width=3), legendgroup="losses",
    ), row=1, col=2)

    # --- Distance ---
    fig.add_trace(go.Scatter(
        x=iterations, y=distances, name="Distance",
        line=dict(color="firebrick"), fill="tozeroy",
        fillcolor="rgba(178,34,34,0.1)",
    ), row=2, col=1)

    # --- Aggregate Metrics ---
    # Distance delta per step (positive = getting closer to target)
    distDelta = [0.0] + [distances[i - 1] - distances[i] for i in range(1, len(distances))]
    fig.add_trace(go.Scatter(
        x=iterations, y=distDelta, name="Dist Delta (closer +)",
        line=dict(color="limegreen", width=2),
        fill="tozeroy", fillcolor="rgba(50,205,50,0.08)",
    ), row=2, col=2)

    # --- Velocity ---
    vx = data["PlaneVelocityX"]
    vy = data["PlaneVelocityY"]
    vz = data["PlaneVelocityZ"]
    totalV = [(vx[i]**2 + vy[i]**2 + vz[i]**2)**0.5 for i in range(len(vx))]

    fig.add_trace(go.Scatter(
        x=iterations, y=data["PlaneVelocityX"], name="Vx",
        line=dict(color="mediumpurple"), legendgroup="vel",
    ), row=3, col=1)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["PlaneVelocityY"], name="Vy",
        line=dict(color="goldenrod"), legendgroup="vel",
    ), row=3, col=1)
    fig.add_trace(go.Scatter(
        x=iterations, y=data["PlaneVelocityZ"], name="Vz",
        line=dict(color="darkcyan"), legendgroup="vel",
    ), row=3, col=1)
    fig.add_trace(go.Scatter(
        x=iterations, y=totalV, name="|V|",
        line=dict(color="orangered", width=3, dash="dot"), legendgroup="vel",
    ), row=3, col=1)

    # --- Loss Angle Change ---
    fig.add_trace(go.Scatter(
        x=iterations, y=data["LossAngleChange"], name="Loss Angle Change",
        line=dict(color="darkorange", width=2),
        fill="tozeroy", fillcolor="rgba(255,140,0,0.08)",
    ), row=3, col=2)

    fig.update_xaxes(title_text="Iteration", row=2, col=1)
    fig.update_xaxes(title_text="Iteration", row=2, col=2)
    fig.update_xaxes(title_text="Iteration", row=3, col=1)
    fig.update_xaxes(title_text="Iteration", row=3, col=2)
    fig.update_yaxes(title_text="Value [0,1]", row=1, col=1)
    fig.update_yaxes(title_text="Loss (rad)", row=1, col=2)
    fig.update_yaxes(title_text="Distance (units)", row=2, col=1)
    fig.update_yaxes(title_text="Delta (units/step)", row=2, col=2)
    fig.update_yaxes(title_text="Velocity (units/s)", row=3, col=1)
    fig.update_yaxes(title_text="Angle (rad)", row=3, col=2)

    fig.update_layout(
        title="Flight Controller Diagnostics",
        height=1100,
        hovermode="x unified",
    )
    return fig


st.set_page_config(page_title="Flight log viewer", layout="wide")
st.title("Flight controller logs")

if not LOG_FILE.exists():
    st.warning(f"No {LOG_FILE.name} in {SCRIPT_DIR}. Run `make flightController` first.")
    st.stop()

runs = loadLogs(str(LOG_FILE), LOG_FILE.stat().st_mtime)
if not runs:
    st.error(f"{LOG_FILE.name} has no runs.")
    st.stop()

models = sorted(runs)
with st.sidebar:
    st.header("Logs")
    selected = st.selectbox("Model / loss variant", models)
    st.caption(f"{len(models)} runs in {LOG_FILE.name}")

data = runs[selected]

distances = data["DistanceToTarget"]
minDist = min(distances)
minStep = distances.index(minDist)
finalDist = distances[-1]
overshoot = max(distances[minStep:]) - minDist
vx, vy, vz = data["PlaneVelocityX"][minStep], data["PlaneVelocityY"][minStep], data["PlaneVelocityZ"][minStep]
speedAtMin = (vx * vx + vy * vy + vz * vz) ** 0.5

with st.container(horizontal=True):
    st.metric("Closest approach", f"{minDist:.1f} m", border=True)
    st.metric("Final distance", f"{finalDist:.1f} m", border=True)
    st.metric("Closest at", f"step {minStep}", border=True)
    st.metric("Speed at closest", f"{speedAtMin:.0f} m/s", border=True)
    st.metric("Overshoot", f"{overshoot:.1f} m", border=True)

st.plotly_chart(plot3dPath(data))
st.plotly_chart(plotDashboard(data))