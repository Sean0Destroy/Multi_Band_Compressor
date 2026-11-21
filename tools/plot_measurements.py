#!/usr/bin/env python3
"""Plots the measurement CSVs written by the C++ test suite (TL_MEASURE_DIR).

Every curve is a MEASURED response of the production float32 engine (impulse responses /
gain traces captured by tests/*.cpp); the only model curves drawn are labelled as such
(float64 model, analog prototype, expected static curve).

    TL_MEASURE_DIR=/some/dir build/tests/tl_tests "[dsp]"
    python3 tools/plot_measurements.py /some/dir [out_dir]

Requires numpy and matplotlib. Writes PNG files next to the CSVs (or into out_dir).
"""
import csv
import os
import sys

import numpy as np
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import matplotlib.ticker  # noqa: E402

# Reference palette (light mode): ordinal blue ramp for magnitudes, categorical slots in order.
ORDINAL = ["#86b6ef", "#5598e7", "#2a78d6", "#1c5cab", "#104281"]
CATEGORICAL = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948"]
INK, INK2, MUTED, GRID, AXIS, SURFACE = "#0b0b0b", "#52514e", "#898781", "#e1e0d9", "#c3c2b7", "#fcfcfb"

plt.rcParams.update({
    "figure.facecolor": SURFACE, "axes.facecolor": SURFACE, "savefig.facecolor": SURFACE,
    "axes.edgecolor": AXIS, "axes.labelcolor": INK2, "text.color": INK, "xtick.color": MUTED, "ytick.color": MUTED,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False, "axes.spines.right": False,
    "lines.linewidth": 1.6, "font.size": 9, "legend.frameon": False, "axes.titlesize": 10, "axes.titleweight": "bold",
})


def load(path):
    with open(path) as f:
        rows = list(csv.reader(f))
    head, data = rows[0], np.array([[float(v) for v in r] for r in rows[1:]])
    return head, data


def save(fig, out, name):
    p = os.path.join(out, name)
    fig.tight_layout()
    fig.savefig(p, dpi=130)
    plt.close(fig)
    print("wrote", p)


def freq_axis(ax, lo=20, hi=20000):
    ax.set_xscale("log")
    ax.set_xlim(lo, hi)
    ax.set_xlabel("Frequency (Hz)")


def plot_a3_responses(src, out):
    for width, label in (("narrow", "1/3 octave"), ("medium", "1 octave"), ("broad", "3 octaves")):
        p = os.path.join(src, f"a3_response_{width}.csv")
        if not os.path.exists(p):
            continue
        head, d = load(p)
        fig, (am, ap) = plt.subplots(1, 2, figsize=(11, 4))
        for i, gr in enumerate((3, 6, 12, 24, 48)):
            am.plot(d[:, 0], d[:, 1 + 2 * i], color=ORDINAL[i], label=f"-{gr} dB GR")
            ap.plot(d[:, 0], d[:, 2 + 2 * i], color=ORDINAL[i], label=f"-{gr} dB GR")
        for a in (am, ap):
            freq_axis(a)
            for e in (1000 * 2 ** (-{"narrow": 1 / 3, "medium": 1, "broad": 3}[width] / 2),
                      1000 * 2 ** ({"narrow": 1 / 3, "medium": 1, "broad": 3}[width] / 2)):
                a.axvline(e, color=MUTED, lw=0.8, ls=":")
        am.set_ylabel("Magnitude (dB)")
        ap.set_ylabel("Phase (degrees)")
        am.set_title(f"A3 band at 1 kHz, {label}: measured magnitude (48 kHz)")
        ap.set_title("Measured phase")
        am.legend(loc="lower left")
        save(fig, out, f"a3_response_{width}.png")


def plot_bandwidth(src, out):
    p = os.path.join(src, "a3_metrics.csv")
    if not os.path.exists(p):
        return
    head, d = load(p)
    fig, axs = plt.subplots(1, 3, figsize=(12, 3.8))
    for i, (w, label) in enumerate(((1 / 3, "1/3 oct"), (1.0, "1 oct"), (3.0, "3 oct"))):
        sel = np.isclose(d[:, 0], w)
        gr = -d[sel, 1]
        axs[0].plot(gr, d[sel, 3], "o-", color=CATEGORICAL[i], ms=5, label=label)
        axs[1].plot(gr, d[sel, 4], "o-", color=CATEGORICAL[i], ms=5, label=label)
        axs[2].plot(gr, d[sel, 6], "o-", color=CATEGORICAL[i], ms=5, label=label)
    axs[0].axhline(np.log2(2 ** (1 / 3)), color=CATEGORICAL[0], lw=0.8, ls=":")
    axs[0].axhline(1.0, color=CATEGORICAL[1], lw=0.8, ls=":")
    axs[0].axhline(3.0, color=CATEGORICAL[2], lw=0.8, ls=":")
    axs[0].set_title("Half-depth bandwidth (dotted: nominal)")
    axs[0].set_ylabel("Octaves")
    axs[1].set_title("At the nominal lower edge (same for all widths)")
    axs[1].set_ylabel("dB")
    axs[2].set_title("Attenuation 1 octave below the band")
    axs[2].set_ylabel("dB")
    for a in axs:
        a.set_xlabel("Gain reduction at centre (dB)")
        a.set_xscale("log")
        a.set_xticks([3, 6, 12, 24, 48])
        a.set_xticklabels(["3", "6", "12", "24", "48"])
        a.xaxis.set_minor_formatter(matplotlib.ticker.NullFormatter())
    axs[0].legend()
    save(fig, out, "a3_bandwidth_vs_gr.png")


def plot_adjacent(src, out):
    p = os.path.join(src, "adjacent_ripple.csv")
    if not os.path.exists(p):
        return
    head, d = load(p)
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.plot(d[:, 0], d[:, 3], color=CATEGORICAL[1], lw=1.2, ls="--", label="250-1000 Hz alone")
    ax.plot(d[:, 0], d[:, 4], color=CATEGORICAL[2], lw=1.2, ls="--", label="1000-4000 Hz alone")
    ax.plot(d[:, 0], d[:, 1], color=CATEGORICAL[0], lw=2.0, label="both (cascade)")
    j = np.interp(np.log(1000.0), np.log(d[:, 0]), d[:, 1])
    k = np.argmin(d[:, 1])
    ax.annotate(f"junction {j:.2f} dB", (1000, j), xytext=(1400, j + 1.0), color=INK2, arrowprops=dict(arrowstyle="-", color=MUTED))
    ax.annotate(f"dip {d[k, 1]:.2f} dB", (d[k, 0], d[k, 1]), xytext=(d[k, 0] * 0.25, d[k, 1] + 0.3), color=INK2, arrowprops=dict(arrowstyle="-", color=MUTED))
    freq_axis(ax)
    ax.set_ylabel("Magnitude (dB)")
    ax.set_title("Adjacent 2-octave bands at -6/-6 dB: measured junction ripple (48 kHz)")
    ax.legend(loc="lower left")
    save(fig, out, "adjacent_ripple.png")


def plot_samplerate(src, out):
    p = os.path.join(src, "samplerate_8k_16k_response.csv")
    if not os.path.exists(p):
        return
    head, d = load(p)
    fig, ax = plt.subplots(figsize=(8, 4))
    ax.plot(d[:, 0], d[:, 1], color=INK, lw=1.2, ls="--", label="analog prototype (model)")
    for i, name in enumerate(head[2:]):
        ax.plot(d[:, 0], d[:, 2 + i], color=ORDINAL[i], label=name.replace("_db", "").replace("fs", "") + " Hz (measured)")
    freq_axis(ax, 1000, 20000)
    ax.set_ylabel("Magnitude (dB)")
    ax.set_title("8-16 kHz band at -12 dB: bilinear warping vs sample rate")
    ax.legend(loc="lower left")
    save(fig, out, "samplerate_comparison.png")


def plot_traces(src, out):
    p = os.path.join(src, "attack_release_trace_peak.csv")
    if os.path.exists(p):
        head, d = load(p)
        fig, ax = plt.subplots(figsize=(9, 4))
        for i, name in enumerate(head[1:]):
            a, r = name.replace("gain_db_a", "").split("_r")
            ax.plot(d[:, 0] * 1e3, d[:, 1 + i], color=CATEGORICAL[i], label=f"attack {a} ms / release {r} ms")
        ax.axvspan(100, 500, color=GRID, alpha=0.5, lw=0)
        ax.set_xlabel("Time (ms)  (shaded: 1 kHz tone at -6 dBFS, otherwise -46 dBFS)")
        ax.set_ylabel("Applied band gain (dB)")
        ax.set_title("Measured GR traces, peak detector (thr -30 dB, 4:1, knee 6 dB)")
        ax.legend(loc="lower right")
        save(fig, out, "attack_release_peak.png")
    p = os.path.join(src, "attack_release_trace_rms.csv")
    if os.path.exists(p):
        head, d = load(p)
        fig, ax = plt.subplots(figsize=(9, 4))
        ax.plot(d[:, 0] * 1e3, d[:, 2], color=CATEGORICAL[0], label="applied gain (measured)")
        g = d[:, 2]
        fin = g[np.argmin(np.abs(d[:, 0] - 0.649))]
        ax.axhline(fin * (1 - np.exp(-1)), color=MUTED, lw=0.8, ls=":")
        ax.axhline(fin * np.exp(-1), color=MUTED, lw=0.8, ls=":")
        ax.set_xlabel("Time (ms)  (tone 50..650 ms)")
        ax.set_ylabel("Applied band gain (dB)")
        ax.set_title("Measured GR trace, RMS 1 ms, attack 30 ms / release 300 ms (dotted: 63 % / 37 %)")
        save(fig, out, "attack_release_rms.png")


def plot_static(src, out):
    p = os.path.join(src, "static_curves.csv")
    if not os.path.exists(p):
        return
    head, d = load(p)
    fig, (a1, a2) = plt.subplots(1, 2, figsize=(11, 4))
    i = 0
    for mode, mname in ((0, "peak"), (1, "RMS")):
        for knee in (0.0, 12.0):
            sel = (d[:, 0] == mode) & (d[:, 1] == knee)
            c = CATEGORICAL[i]
            a1.plot(d[sel, 2], d[sel, 4], color=c, lw=1.2, label=f"{mname}, knee {knee:g} dB: expected")
            a1.plot(d[sel, 2], d[sel, 5], "o", color=c, ms=4, label=f"{mname}, knee {knee:g} dB: measured")
            a2.plot(d[sel, 2], d[sel, 5] - d[sel, 4], "o-", color=c, ms=4, label=f"{mname}, knee {knee:g} dB")
            i += 1
    a1.set_title("Static curves at the band centre (thr -30 dB, 4:1)")
    a1.set_xlabel("Input sine peak (dBFS)")
    a1.set_ylabel("Gain reduction (dB)")
    a1.legend(fontsize=7)
    a2.axhspan(-0.25, 0.25, color=GRID, alpha=0.6, lw=0)
    a2.set_title("Measured - expected (shaded: +/-0.25 dB requirement)")
    a2.set_xlabel("Input sine peak (dBFS)")
    a2.set_ylabel("Error (dB)")
    a2.legend(fontsize=7)
    save(fig, out, "static_curves.png")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src
    os.makedirs(out, exist_ok=True)
    plot_a3_responses(src, out)
    plot_bandwidth(src, out)
    plot_adjacent(src, out)
    plot_samplerate(src, out)
    plot_traces(src, out)
    plot_static(src, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
