import glob
import math
import os
import random
import re
import sqlite3
from contextlib import closing

import numpy as np
import plotly.graph_objects as go
import streamlit as st
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter
from torchvision.io import decode_image

HR_SIZE = 256
DISPLAY_SIZE = 512
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.environ.get("SR_RATE_DB", os.path.join(SCRIPT_DIR, "ratings.db"))
DEFAULT_IMAGE_DIR = "/media/user/2TB/wt_screenshots"
MODEL_PATTERN = re.compile(r"sr_perc_(\d+)-to-(\d+)-Blocks-(\d+)-CHANNELS-(\d+)\.pt$")
PLACE_LABELS = {0: "-", 1: "1st", 2: "2nd", 3: "3rd"}
ELO_BASE = 1000
ELO_K = 32
ELO_SCALE = 400
BAR_COLORS = ["#4c78a8", "#f58518", "#e45756", "#72b7b2", "#54a24b", "#eeca3b", "#b279a2", "#ff9da6"]
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")


class ResBlock(nn.Module):
    def __init__(self, channels):
        super().__init__()
        self.conv1 = nn.Conv2d(channels, channels, 3, padding=1)
        self.conv2 = nn.Conv2d(channels, channels, 3, padding=1)
        self.relu = nn.ReLU()

    def forward(self, x):
        return x + self.conv2(self.relu(self.conv1(x)))


class SRNet(nn.Module):
    def __init__(self, channels, blocks, scale):
        super().__init__()
        self.head = nn.Conv2d(3, channels, 3, padding=1)
        self.relu = nn.ReLU()
        self.blocks = nn.ModuleList([ResBlock(channels) for _ in range(blocks)])
        self.tail = nn.Conv2d(channels, 3 * scale**2, 3, padding=1)
        self.shuffle = nn.PixelShuffle(scale)

    def forward(self, x):
        x = self.relu(self.head(x))
        for block in self.blocks:
            x = block(x)
        return self.shuffle(self.tail(x))


def discoverModels():
    grouped = {}
    skipped = []
    for path in sorted(glob.glob(os.path.join(SCRIPT_DIR, "*.pt"))):
        name = os.path.basename(path)
        match = MODEL_PATTERN.match(name)
        if not match or int(match.group(2)) != HR_SIZE:
            skipped.append(name)
            continue
        entry = {
            "path": path,
            "stem": name[:-3],
            "label": f"B{match.group(3)} C{match.group(4)} {match.group(1)}px",
            "category": f"{match.group(1)}-to-{match.group(2)}",
        }
        grouped.setdefault(entry["category"], []).append(entry)
    order = sorted(grouped, key=lambda category: int(category.split("-to-")[0]))
    return {category: sorted(grouped[category], key=lambda model: model["stem"]) for category in order}, skipped


def archFromState(state):
    channels = state["head.weight"].shape[0]
    blocks = 0
    while f"blocks.{blocks}.conv1.weight" in state:
        blocks += 1
    scale = math.isqrt(state["tail.weight"].shape[0] // 3)
    return channels, blocks, scale


@st.cache_resource
def loadModel(path):
    state = torch.load(path, map_location="cpu")
    if isinstance(state, dict) and "state_dict" in state:
        state = state["state_dict"]
    if any(key.startswith("module.") for key in state):
        state = {key.removeprefix("module."): value for key, value in state.items()}
    channels, blocks, scale = archFromState(state)
    model = SRNet(channels, blocks, scale)
    model.load_state_dict(state)
    return model.eval().to(DEVICE), scale


def sr(model, lr, scale):
    return model(lr) + F.interpolate(lr, scale_factor=scale, mode="bilinear")


@st.cache_data
def listImages(folder):
    files = []
    for pattern in ("*.png", "*.jpg", "*.jpeg", "*.PNG", "*.JPG", "*.JPEG"):
        files.extend(glob.glob(os.path.join(folder, pattern)))
    valid = []
    for path in sorted(set(files)):
        try:
            with Image.open(path) as image:
                if min(image.size) >= HR_SIZE:
                    valid.append(path)
        except OSError:
            continue
    return valid


@st.cache_data(max_entries=8)
def loadImage(path):
    try:
        image = decode_image(path, mode="RGB").permute(1, 2, 0).numpy()
        return np.ascontiguousarray(image)
    except (RuntimeError, OSError):
        with Image.open(path) as image:
            return np.asarray(image.convert("RGB"), dtype=np.uint8)


@st.cache_data(max_entries=128)
def cropHr(path, x, y):
    image = loadImage(path)
    return image[y:y + HR_SIZE, x:x + HR_SIZE].copy()


@st.cache_data(max_entries=128)
def makeInput(path, x, y, lrSize):
    hr = cropHr(path, x, y)
    hrTensor = torch.from_numpy(np.ascontiguousarray(hr.transpose(2, 0, 1))).float()[None] / 255
    lr = F.interpolate(hrTensor, size=(lrSize, lrSize), mode="bicubic", antialias=True).clamp(0, 1)
    panel = F.interpolate(lr, size=(HR_SIZE, HR_SIZE), mode="nearest")[0]
    panelU8 = np.rint(np.clip(panel.permute(1, 2, 0).numpy(), 0, 1) * 255).astype(np.uint8)
    return lr[0].numpy(), panelU8


def psnr(a, b):
    mse = float(np.mean((a - b) ** 2))
    return 10 * math.log10(1.0 / max(mse, 1e-12))


def ssim(a, b):
    c1, c2 = 0.01**2, 0.03**2
    values = []
    for channel in range(a.shape[2]):
        x = a[:, :, channel].astype(np.float64)
        y = b[:, :, channel].astype(np.float64)
        muX = gaussian_filter(x, 1.5)
        muY = gaussian_filter(y, 1.5)
        sigmaX = gaussian_filter(x * x, 1.5) - muX * muX
        sigmaY = gaussian_filter(y * y, 1.5) - muY * muY
        sigmaXY = gaussian_filter(x * y, 1.5) - muX * muY
        numerator = (2 * muX * muY + c1) * (2 * sigmaXY + c2)
        denominator = (muX**2 + muY**2 + c1) * (sigmaX + sigmaY + c2)
        values.append(float(np.mean(numerator / denominator)))
    return float(np.mean(values))


@st.cache_data(max_entries=256)
def evalModel(modelPath, path, x, y):
    model, scale = loadModel(modelPath)
    lrNp, _ = makeInput(path, x, y, HR_SIZE // scale)
    lr = torch.from_numpy(lrNp)[None].to(DEVICE)
    with torch.inference_mode():
        out = sr(model, lr, scale).clamp(0, 1)
    outF = out[0].permute(1, 2, 0).cpu().numpy()
    gtF = cropHr(path, x, y).astype(np.float32) / 255
    outU8 = np.rint(outF * 255).astype(np.uint8)
    return outU8, psnr(outF, gtF), ssim(outF, gtF)


def dbConnect():
    return sqlite3.connect(DB_PATH)


def dbInit():
    with closing(dbConnect()) as con:
        con.execute(
            "CREATE TABLE IF NOT EXISTS rankings ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ts TEXT NOT NULL DEFAULT (datetime('now')), "
            "category TEXT NOT NULL, "
            "model TEXT NOT NULL, "
            "image TEXT NOT NULL, "
            "crop_x INTEGER NOT NULL, "
            "crop_y INTEGER NOT NULL, "
            "rank INTEGER NOT NULL, "
            "UNIQUE(category, image, crop_x, crop_y, model))"
        )
        con.execute(
            "CREATE TABLE IF NOT EXISTS model_metrics ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "ts TEXT NOT NULL DEFAULT (datetime('now')), "
            "category TEXT NOT NULL, "
            "model TEXT NOT NULL, "
            "image TEXT NOT NULL, "
            "crop_x INTEGER NOT NULL, "
            "crop_y INTEGER NOT NULL, "
            "psnr REAL NOT NULL, "
            "ssim REAL NOT NULL, "
            "UNIQUE(category, image, crop_x, crop_y, model))"
        )
        con.commit()


def dbRanks(category, path, x, y):
    with closing(dbConnect()) as con:
        rows = con.execute(
            "SELECT model, rank FROM rankings WHERE category=? AND image=? AND crop_x=? AND crop_y=?",
            (category, path, x, y),
        ).fetchall()
    return {model: rank for model, rank in rows}


def dbSave(category, path, x, y, ranks, metrics):
    with closing(dbConnect()) as con:
        con.executemany(
            "INSERT INTO rankings (category, model, image, crop_x, crop_y, rank) VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(category, image, crop_x, crop_y, model) DO UPDATE SET rank=excluded.rank, ts=datetime('now')",
            [(category, model, path, x, y, rank) for model, rank in ranks.items()],
        )
        dbWriteMetrics(con, category, path, x, y, metrics)
        con.commit()


def dbWriteMetrics(con, category, path, x, y, metrics, ts=None):
    if ts is None:
        con.executemany(
            "INSERT INTO model_metrics (category, model, image, crop_x, crop_y, psnr, ssim) VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(category, image, crop_x, crop_y, model) DO UPDATE SET psnr=excluded.psnr, ssim=excluded.ssim, ts=datetime('now')",
            [(category, model, path, x, y, psnrValue, ssimValue) for model, (psnrValue, ssimValue) in metrics.items()],
        )
        return
    con.executemany(
        "INSERT INTO model_metrics (ts, category, model, image, crop_x, crop_y, psnr, ssim) VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(category, image, crop_x, crop_y, model) DO UPDATE SET psnr=excluded.psnr, ssim=excluded.ssim, ts=excluded.ts",
        [(ts, category, model, path, x, y, psnrValue, ssimValue) for model, (psnrValue, ssimValue) in metrics.items()],
    )


def dbMetricStats(category):
    with closing(dbConnect()) as con:
        return con.execute(
            "SELECT model, AVG(psnr), AVG(ssim) FROM model_metrics WHERE category=? GROUP BY model",
            (category,),
        ).fetchall()


def dbMetricPending():
    with closing(dbConnect()) as con:
        return con.execute(
            "SELECT COUNT(*) FROM rankings r LEFT JOIN model_metrics m "
            "ON m.category=r.category AND m.model=r.model AND m.image=r.image "
            "AND m.crop_x=r.crop_x AND m.crop_y=r.crop_y WHERE m.id IS NULL"
        ).fetchone()[0]


def dbBackfillMetrics(categories):
    grouped = {}
    with closing(dbConnect()) as con:
        pending = con.execute(
            "SELECT r.category, r.ts, r.image, r.crop_x, r.crop_y, r.model "
            "FROM rankings r LEFT JOIN model_metrics m "
            "ON m.category=r.category AND m.model=r.model AND m.image=r.image "
            "AND m.crop_x=r.crop_x AND m.crop_y=r.crop_y WHERE m.id IS NULL "
            "ORDER BY r.ts, r.id"
        ).fetchall()
    for category, ts, path, x, y, modelName in pending:
        grouped.setdefault((category, ts, path, x, y), []).append(modelName)

    modelMap = {
        (category, model["stem"]): model
        for category, models in categories.items()
        for model in models
    }
    inserted = 0
    for (category, ts, path, x, y), modelNames in grouped.items():
        if not os.path.isfile(path):
            continue
        metrics = {}
        for modelName in modelNames:
            model = modelMap.get((category, modelName))
            if model is None:
                continue
            try:
                _, psnrValue, ssimValue = evalModel(model["path"], path, x, y)
            except (OSError, RuntimeError, ValueError):
                continue
            metrics[modelName] = (psnrValue, ssimValue)
        if not metrics:
            continue
        with closing(dbConnect()) as con:
            dbWriteMetrics(con, category, path, x, y, metrics, ts)
            con.commit()
        inserted += len(metrics)
    return inserted


def dbStats(category):
    with closing(dbConnect()) as con:
        aggregated = con.execute(
            "SELECT model, COUNT(*), AVG(rank) FROM rankings WHERE category=? GROUP BY model ORDER BY AVG(rank)",
            (category,),
        ).fetchall()
        distribution = con.execute(
            "SELECT model, rank, COUNT(*) FROM rankings WHERE category=? GROUP BY model, rank",
            (category,),
        ).fetchall()
        crops = con.execute(
            "SELECT COUNT(*) FROM (SELECT DISTINCT image, crop_x, crop_y FROM rankings WHERE category=?)",
            (category,),
        ).fetchone()[0]
    return aggregated, distribution, crops


def dbMatches(category):
    with closing(dbConnect()) as con:
        return con.execute(
            "SELECT ts, image, crop_x, crop_y, model, rank FROM rankings WHERE category=? ORDER BY ts, id",
            (category,),
        ).fetchall()


def ratingHistory(rows):
    groups = []
    seen = {}
    for _, image, x, y, model, rank in rows:
        key = (image, x, y)
        if key not in seen:
            seen[key] = len(groups)
            groups.append([])
        groups[seen[key]].append((model, rank))
    ratings = {}
    totals = {}
    counts = {}
    eloEvents = []
    avgEvents = []
    for index, pairs in enumerate(groups, start=1):
        for i in range(len(pairs)):
            for j in range(i + 1, len(pairs)):
                modelA, rankA = pairs[i]
                modelB, rankB = pairs[j]
                ratingA = ratings.get(modelA, ELO_BASE)
                ratingB = ratings.get(modelB, ELO_BASE)
                expected = 1 / (1 + 10 ** ((ratingB - ratingA) / ELO_SCALE))
                score = 1.0 if rankA < rankB else 0.0
                ratings[modelA] = ratingA + ELO_K * (score - expected)
                ratings[modelB] = ratingB + ELO_K * ((1 - score) - (1 - expected))
        for model, rank in pairs:
            totals[model] = totals.get(model, 0) + rank
            counts[model] = counts.get(model, 0) + 1
        eloEvents.append((index, {model: round(value, 1) for model, value in ratings.items()}))
        avgEvents.append((index, {model: round(totals[model] / counts[model], 3) for model in totals}))
    return ratings, eloEvents, avgEvents


def rankingMetrics(results):
    return {
        model["stem"]: (psnrValue, ssimValue)
        for model, _, _, psnrValue, ssimValue in results
    }


def viewport(array, cx, cy, zoom):
    side = HR_SIZE // zoom
    half = side // 2
    x0 = int(np.clip(cx - half, 0, HR_SIZE - side))
    y0 = int(np.clip(cy - half, 0, HR_SIZE - side))
    box = array[y0:y0 + side, x0:x0 + side]
    return np.asarray(Image.fromarray(box).resize((DISPLAY_SIZE, DISPLAY_SIZE), Image.NEAREST))


def overviewImage(gt, cx, cy, zoom):
    image = Image.fromarray(gt).copy()
    side = HR_SIZE // zoom
    half = side // 2
    x0 = int(np.clip(cx - half, 0, HR_SIZE - side))
    y0 = int(np.clip(cy - half, 0, HR_SIZE - side))
    ImageDraw.Draw(image).rectangle([x0, y0, x0 + side - 1, y0 + side - 1], outline=(255, 70, 70), width=2)
    return np.asarray(image)


def placeLabel(value):
    return PLACE_LABELS.get(value, f"{value}th")


def metricBars(rows, key, fmt):
    values = [row[key] for row in rows]
    low = min(values) if values else 0
    high = max(values) if values else 1
    pad = (high - low) * 0.6 if high > low else max(abs(high) * 0.02, 0.01)
    figure = go.Figure(
        go.Bar(
            x=[row["model"] for row in rows],
            y=values,
            marker_color=[BAR_COLORS[index % len(BAR_COLORS)] for index in range(len(rows))],
            text=[format(value, fmt) for value in values],
            textposition="outside",
        )
    )
    figure.update_layout(
        height=260,
        margin=dict(l=40, r=20, t=20, b=70),
        showlegend=False,
        yaxis_range=[low - pad, high + pad],
    )
    figure.update_xaxes(tickangle=-20)
    return figure


def resetView():
    st.session_state.zoom = 1
    st.session_state.viewX = HR_SIZE // 2
    st.session_state.viewY = HR_SIZE // 2


def main():
    st.set_page_config(page_title="upscale rating", layout="wide")
    dbInit()

    categories, skipped = discoverModels()
    if not categories:
        st.error("no models found in " + SCRIPT_DIR)
        return
    if len(categories) > 1:
        categories["all"] = [model for group in categories.values() for model in group]
    if not st.session_state.get("backfillDone", False):
        st.session_state.backfillDone = True
        if dbMetricPending():
            with st.spinner("backfilling metrics"):
                dbBackfillMetrics(categories)

    with st.sidebar:
        category = st.selectbox("category", list(categories))
        models = categories[category]
        st.caption(f"{len(models)} models" + (f" | skipped: {', '.join(skipped)}" if skipped else ""))

        folder = st.text_input("image folder", DEFAULT_IMAGE_DIR, key="folderPath")
        if st.button("rescan folder"):
            st.cache_data.clear()
        if st.session_state.get("lastFolder") != folder:
            st.session_state.lastFolder = folder
            st.session_state.imagePick = 0
        images = listImages(folder)
        if not images:
            st.error("no usable images in " + folder)
            return

        if st.session_state.get("imagePick", 0) >= len(images):
            st.session_state.imagePick = 0
        index = st.session_state.get("imagePick", 0)
        navPrev, navNext, navRandom = st.columns(3)
        if navPrev.button("prev", width="stretch"):
            st.session_state.imagePick = (index - 1) % len(images)
        if navNext.button("next", width="stretch"):
            st.session_state.imagePick = (index + 1) % len(images)
        if navRandom.button("random", width="stretch"):
            st.session_state.imagePick = random.randrange(len(images))
            st.session_state.randomCropOnNav = True
        index = st.selectbox(
            "image",
            range(len(images)),
            index=None if "imagePick" in st.session_state else index,
            format_func=lambda i: os.path.basename(images[i]),
            key="imagePick",
            label_visibility="collapsed",
        )
        path = images[index]
        image = loadImage(path)
        maxX = image.shape[1] - HR_SIZE
        maxY = image.shape[0] - HR_SIZE
        st.caption(f"{index + 1}/{len(images)} {os.path.basename(path)} {image.shape[1]}x{image.shape[0]}")

        keyX, keyY = f"cropX_{path}", f"cropY_{path}"
        if st.session_state.pop("randomCropOnNav", False):
            st.session_state[keyX] = random.randrange(maxX + 1)
            st.session_state[keyY] = random.randrange(maxY + 1)
        if st.button("random crop"):
            st.session_state[keyX] = random.randrange(maxX + 1)
            st.session_state[keyY] = random.randrange(maxY + 1)
        x = st.slider("crop x", 0, maxX, None if keyX in st.session_state else maxX // 2, key=keyX) if maxX > 0 else 0
        y = st.slider("crop y", 0, maxY, None if keyY in st.session_state else maxY // 2, key=keyY) if maxY > 0 else 0

        zoom = st.select_slider("zoom", options=[1, 2, 4, 8], key="zoom")
        side = HR_SIZE // zoom
        half = side // 2
        if zoom == 1:
            cx = cy = HR_SIZE // 2
        else:
            for key in ("viewX", "viewY"):
                if key in st.session_state:
                    st.session_state[key] = int(np.clip(st.session_state[key], half, HR_SIZE - 1 - half))
            cx = st.slider("view x", half, HR_SIZE - 1 - half, None if "viewX" in st.session_state else HR_SIZE // 2, key="viewX")
            cy = st.slider("view y", half, HR_SIZE - 1 - half, None if "viewY" in st.session_state else HR_SIZE // 2, key="viewY")
        st.button("reset view", on_click=resetView)
        panelsPerRow = st.select_slider("panels per row", options=[2, 3, 4, 5, 6], value=5, key="panelsPerRow")

        gt = cropHr(path, x, y)
        st.image(overviewImage(gt, cx, cy, zoom), caption="zoom position", width="stretch")

    results = []
    for model in models:
        _, modelScale = loadModel(model["path"])
        results.append((model, modelScale, *evalModel(model["path"], path, x, y)))
    lrSizes = sorted({HR_SIZE // scale for _, scale, _, _, _ in results})
    inputPanels = [(lrSize, makeInput(path, x, y, lrSize)[1]) for lrSize in lrSizes]

    saved = dbRanks(category, path, x, y)
    ranking = {}
    panels = [("gt", "ground truth", gt)]
    for model, _, output, _, _ in results:
        panels.append(("model", model["stem"], output))
    for lrSize, inputPanel in inputPanels:
        panels.append(("input", f"input {lrSize}px", inputPanel))
    perRow = min(panelsPerRow, len(panels))
    for start in range(0, len(panels), perRow):
        columns = st.columns(perRow)
        for column, (kind, label, array) in zip(columns, panels[start:start + perRow]):
            with column:
                st.image(viewport(array, cx, cy, zoom), caption=label, width="stretch")
                if kind == "model" and len(models) > 1:
                    ranking[label] = st.selectbox(
                        "place",
                        range(len(models) + 1),
                        index=min(saved.get(label, 0), len(models)),
                        format_func=placeLabel,
                        key=f"place_{category}_{path}_{x}_{y}_{label}",
                        label_visibility="collapsed",
                    )

    if len(models) > 1:
        chosen = {stem: value for stem, value in ranking.items() if value > 0}
        complete = len(chosen) == len(models) and len(set(chosen.values())) == len(models)
        if complete and any(chosen.get(stem) != saved.get(stem) for stem in chosen):
            dbSave(category, path, x, y, chosen, rankingMetrics(results))
            st.toast("ranking saved")
        elif not complete and chosen:
            st.warning("assign a distinct place to every model")

    with st.expander("stats"):
        metricsColumn, statsColumn = st.columns(2)
        with metricsColumn:
            st.caption("current crop")
            st.dataframe(
                {
                    "model": [model["stem"] for model, _, _, _, _ in results],
                    "psnr": [round(psnrValue, 2) for _, _, _, psnrValue, _ in results],
                    "ssim": [round(ssimValue, 4) for _, _, _, _, ssimValue in results],
                },
                hide_index=True,
                width="stretch",
            )
        aggregated, distribution, crops = dbStats(category)
        distributionMap = {}
        for modelName, rank, count in distribution:
            distributionMap.setdefault(modelName, {})[rank] = count
        ratings, eloEvents, avgEvents = ratingHistory(dbMatches(category))
        with statsColumn:
            if aggregated:
                st.caption(f"rankings ({crops} crops)")
                statsTable = {
                    "model": [row[0] for row in aggregated],
                    "elo": [round(ratings.get(row[0], ELO_BASE)) for row in aggregated],
                    "n": [row[1] for row in aggregated],
                    "avg": [round(row[2], 2) for row in aggregated],
                }
                for place in range(1, len(models) + 1):
                    statsTable[placeLabel(place)] = [distributionMap.get(row[0], {}).get(place, 0) for row in aggregated]
                st.dataframe(statsTable, hide_index=True, width="stretch")
            else:
                st.caption("no rankings yet")

        if aggregated:
            labelOf = {model["stem"]: model["label"] for model in models}
            labels = [labelOf.get(row[0], row[0]) for row in aggregated]
            events = [index for index, _ in eloEvents]
            eloTable = {"rating": events}
            avgTable = {"rating": events}
            for row in aggregated:
                eloValues = []
                avgValues = []
                eloLast = None
                avgLast = None
                for _, eloSnapshot in eloEvents:
                    value = eloSnapshot.get(row[0])
                    if value is not None:
                        eloLast = value
                    eloValues.append(eloLast)
                for _, avgSnapshot in avgEvents:
                    value = avgSnapshot.get(row[0])
                    if value is not None:
                        avgLast = value
                    avgValues.append(avgLast)
                eloTable[labelOf.get(row[0], row[0])] = eloValues
                avgTable[labelOf.get(row[0], row[0])] = avgValues
            metricStats = {
                modelName: (psnrMean, ssimMean)
                for modelName, psnrMean, ssimMean in dbMetricStats(category)
            }
            psnrRows = [
                {"model": labelOf.get(row[0], row[0]), "psnr": round(metricStats[row[0]][0], 2)}
                for row in aggregated
                if row[0] in metricStats
            ]
            ssimRows = [
                {"model": labelOf.get(row[0], row[0]), "ssim": round(metricStats[row[0]][1], 4)}
                for row in aggregated
                if row[0] in metricStats
            ]
            distributionRows = [
                {"model": labelOf.get(row[0], row[0]), "place": placeLabel(place), "count": distributionMap.get(row[0], {}).get(place, 0)}
                for place in range(1, len(models) + 1)
                for row in aggregated
            ]
            chartLeft, chartMid, chartRight = st.columns(3)
            with chartLeft:
                st.caption("elo over time (per rating, higher is better)")
                st.line_chart(eloTable, x="rating", y=labels, height=260)
            with chartMid:
                st.caption("avg place over time (per rating, lower is better)")
                st.line_chart(avgTable, x="rating", y=labels, height=260)
            with chartRight:
                st.caption("place counts")
                st.bar_chart(distributionRows, x="model", y="count", color="place", sort=False, stack=True, height=260)
            if psnrRows:
                psnrColumn, ssimColumn = st.columns(2)
                with psnrColumn:
                    st.caption("mean psnr (all rated crops, higher is better)")
                    st.plotly_chart(metricBars(psnrRows, "psnr", ".2f"), width="stretch")
                with ssimColumn:
                    st.caption("mean ssim (all rated crops, higher is better)")
                    st.plotly_chart(metricBars(ssimRows, "ssim", ".4f"), width="stretch")


if __name__ == "__main__":
    main()
