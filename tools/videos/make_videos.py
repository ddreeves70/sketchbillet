#!/usr/bin/env python3
"""Makes the tutorial videos: plays each script in tools/videos/scripts/ on the
real desktop app, with the mouse gliding to everything it clicks, records the
screen, and puts captions, a title card and an end card on it.

    python3 tools/videos/make_videos.py --sketchbillet-app build/dev/apps/sketchbillet/sketchbillet \\
        [--out build/videos] [names...]

Linux only (it records an Xvfb screen with ffmpeg). Needs Xvfb, ffmpeg (with
libx264 and libass) and Pillow. For each script it writes, in --out/<name>/:

    <name>.mp4      1920 x 1080, 30 frames a second, captions burnt in
    <name>.srt      the captions, to upload as subtitles instead (or as well)
    youtube.txt     a title and a description with chapters, ready to paste
    narration.txt   the captions as a script, to record a voice-over from

A script is a JSON object: "title", "subtitle", "page" (the guide page it
follows), "next" (what to watch next, for the end card), "setup"
(sketchbillet_cli commands run first, in the folder the app starts in, for
designs the video opens), "open" (a design to open at the start), "steps",
which the app plays (see apps/sketchbillet/Demo.h), and "expect": what
`sketchbillet_cli open <file>` prints about the design the video ends with,
{"file": "tag.sb", "lines": ["Tag ok volume 7121.46 mm^3 mass 9.04 g (PETG)"]},
so the video is known to make what its guide page makes.

With --check, it records nothing: it plays each script as fast as it can, on
a virtual screen (xvfb-run), and checks "expect". The videos.* tests do this, so a change to the
app that breaks a video's script fails the build.
"""

import argparse
import json
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SIZE = (1920, 1080)
FPS = 30
CARD_SECONDS = 4.0
# The same encoder settings for every piece, so they join without encoding again.
ENCODE = ["-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-pix_fmt", "yuv420p", "-r", str(FPS)]

# Colors of the cards, from the app's icons: the blue of a body, and a dark ink.
INK = (24, 32, 48)
BLUE = (120, 173, 233)
PAPER = (247, 249, 252)


def font(size, bold=False):
    from PIL import ImageFont
    for name in (["Inter-Bold.otf", "DejaVuSans-Bold.ttf"] if bold else ["Inter-Regular.otf", "DejaVuSans.ttf"]):
        for folder in ("/usr/share/fonts/opentype/inter", "/usr/share/fonts/truetype/dejavu"):
            path = Path(folder) / name
            if path.exists():
                return ImageFont.truetype(str(path), size)
    return ImageFont.load_default()


def wrap(draw, text, fnt, width):
    lines, line = [], ""
    for word in text.split():
        trial = (line + " " + word).strip()
        if draw.textlength(trial, font=fnt) > width and line:
            lines.append(line)
            line = word
        else:
            line = trial
    if line:
        lines.append(line)
    return lines


def card(path, heading, lines, footer):
    """A still: the heading big, then a few lines, on plain paper with a blue bar."""
    from PIL import Image, ImageDraw
    img = Image.new("RGB", SIZE, PAPER)
    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, 28, SIZE[1]], fill=BLUE)
    x, y = 160, 330
    big = font(96, bold=True)
    for line in wrap(d, heading, big, SIZE[0] - 2 * x):
        d.text((x, y), line, font=big, fill=INK)
        y += 120
    y += 30
    body = font(44)
    for text in lines:
        for line in wrap(d, text, body, SIZE[0] - 2 * x):
            d.text((x, y), line, font=body, fill=(70, 80, 96))
            y += 62
    d.text((x, SIZE[1] - 140), footer, font=font(36, bold=True), fill=(70, 80, 96))
    img.save(path)


def encode_still(png, seconds, out):
    tmp = out.with_name(out.stem + ".part.mp4")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(FPS), "-i", str(png), "-vf",
                    f"format=yuv420p,loop=-1:1,trim=duration={seconds},fade=in:st=0:d=0.4,"
                    f"fade=out:st={seconds - 0.4}:d=0.4"] + ENCODE + [str(tmp)], check=True)
    tmp.rename(out)


def clock(seconds):
    seconds = max(0.0, seconds)
    h, rest = divmod(seconds, 3600)
    m, s = divmod(rest, 60)
    return int(h), int(m), s


def srt_time(seconds):
    h, m, s = clock(seconds)
    return f"{h:02d}:{m:02d}:{int(s):02d},{int(round((s - int(s)) * 1000)) % 1000:03d}"


def ass_time(seconds):
    h, m, s = clock(seconds)
    return f"{h:d}:{m:02d}:{s:05.2f}"


def chapter_time(seconds):
    h, m, s = clock(seconds)
    return f"{h:d}:{m:02d}:{int(s):02d}" if h else f"{m:d}:{int(s):02d}"


def ass_escape(text):
    return text.replace("\\", "\\\\").replace("{", "(").replace("}", ")")


def captions(events, offset, end):
    """The captions and chapters, each with its start and end in the video."""
    caps, chapters = [], []
    for e in events:
        t = e["t"] / 1000.0 + offset
        if e["event"] == "caption":
            caps.append([t, None, e["text"]])
        elif e["event"] == "chapter":
            chapters.append([t, None, e["text"]])
    for items in (caps, chapters):
        for i, item in enumerate(items):
            item[1] = items[i + 1][0] if i + 1 < len(items) else end
    return caps, chapters


def write_ass(path, caps, chapters):
    head = f"""[Script Info]
ScriptType: v4.00+
PlayResX: {SIZE[0]}
PlayResY: {SIZE[1]}
WrapStyle: 0

[V4+ Styles]
Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding
Style: Caption,Inter,46,&H00FFFFFF,&H00FFFFFF,&H30201810,&H30201810,0,0,0,0,100,100,0,0,3,14,0,2,220,220,60,1
Style: Chapter,Inter,36,&H00302018,&H00302018,&H00E9AD78,&H00E9AD78,1,0,0,0,100,100,0,0,3,12,0,8,40,40,232,1

[Events]
Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text
"""
    lines = [head]
    for start, end, text in caps:
        lines.append(f"Dialogue: 0,{ass_time(start)},{ass_time(end)},Caption,,0,0,0,,{ass_escape(text)}\n")
    for start, end, text in chapters:
        lines.append(f"Dialogue: 1,{ass_time(start)},{ass_time(end)},Chapter,,0,0,0,,{ass_escape(text)}\n")
    path.write_text("".join(lines), encoding="utf-8")


def write_srt(path, caps):
    out = []
    for i, (start, end, text) in enumerate(caps, 1):
        out.append(f"{i}\n{srt_time(start)} --> {srt_time(end)}\n{text}\n")
    path.write_text("\n".join(out), encoding="utf-8")


def record(app, script_path, script, workdir, raw, log, display):
    """Plays the script on the app on an Xvfb screen, recording it. Returns the
    recording's start time (seconds since 1970)."""
    xvfb = subprocess.Popen(["Xvfb", display, "-screen", "0", f"{SIZE[0]}x{SIZE[1]}x24", "-nolisten", "tcp"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        env = dict(os.environ, DISPLAY=display, QT_QPA_PLATFORM="xcb", LIBGL_ALWAYS_SOFTWARE="1",
                   QT_SCALE_FACTOR="1.5", QT_QPA_PLATFORMTHEME="")
        grab = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "info", "-f", "x11grab", "-draw_mouse", "1",
                                 "-framerate", str(FPS), "-video_size", f"{SIZE[0]}x{SIZE[1]}", "-i", display]
                                + ENCODE + [str(raw)],
                                stdin=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        argv = [str(app), "--settings", str(workdir / "settings.ini"), "--demo", str(script_path),
                "--demo-log", str(log)]
        if script.get("open"):
            argv.append(script["open"])
        played = subprocess.run(argv, cwd=workdir, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        time.sleep(0.5)
        _, err = grab.communicate(b"q", timeout=120)
        if played.returncode != 0:
            sys.exit(f"{script_path.name}: the demo stopped:\n{played.stdout.decode(errors='replace')}")
        # x11grab stamps frames with the clock: "start: 1795459813.331" is the first frame.
        found = re.search(r"start: (\d+\.\d+)", err.decode(errors="replace"))
        if not found:
            sys.exit(f"{script_path.name}: couldn't tell when the recording started:\n{err.decode()[-2000:]}")
        return float(found.group(1))
    finally:
        xvfb.send_signal(signal.SIGTERM)
        xvfb.wait()


def run_setup(cli, script, work, name):
    for command in script.get("setup", []):
        argv = [str(cli)] + [str(REPO / a) if a.startswith(("tests/", "docs/")) else a
                             for a in shlex.split(command)]
        proc = subprocess.run(argv, cwd=work, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if proc.returncode != 0:
            sys.exit(f"{name}: '{command}' failed:\n{proc.stdout.decode()}")


def check_expect(cli, script, work, name):
    """Checks the design the script made is what its guide page makes."""
    expect = script.get("expect")
    if not expect:
        return
    proc = subprocess.run([str(cli), "open", expect["file"]], cwd=work, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)
    printed = proc.stdout.decode(errors="replace")
    squeeze = lambda text: " ".join(text.split())
    lines = [squeeze(line) for line in printed.splitlines()]
    for want in expect["lines"]:
        if squeeze(want) not in lines:
            sys.exit(f"{name}: expected '{want}' from sketchbillet_cli open {expect['file']}, but it printed:\n"
                     f"{printed}")


def check(app, cli, script_path):
    """Plays a script fast, on a virtual screen, and checks what it made."""
    script = json.loads(script_path.read_text(encoding="utf-8"))
    name = script_path.stem
    with tempfile.TemporaryDirectory(prefix="sb_video_check_") as tmp:
        work = Path(tmp)
        run_setup(cli, script, work, name)
        # On a virtual screen, with the real 3D view the video shows.
        env = dict(os.environ, QT_QPA_PLATFORM="xcb", LIBGL_ALWAYS_SOFTWARE="1", QT_SCALE_FACTOR="1.5")
        argv = [shutil.which("xvfb-run"), "-a", "-s", f"-screen 0 {SIZE[0]}x{SIZE[1]}x24", str(app),
                "--settings", str(work / "settings.ini"), "--demo", str(script_path), "--demo-speed", "0"]
        if script.get("open"):
            argv.append(script["open"])
        played = subprocess.run(argv, cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=600)
        if played.returncode != 0:
            sys.exit(f"{name}: the demo stopped:\n{played.stdout.decode(errors='replace')}")
        check_expect(cli, script, work, name)
    print(f"{name}: played to the end")


def make(app, cli, script_path, out_root, display):
    script = json.loads(script_path.read_text(encoding="utf-8"))
    name = script_path.stem
    out = out_root / name
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="sb_video_") as tmp:
        tmp = Path(tmp)
        work = tmp / "work"
        work.mkdir()
        run_setup(cli, script, work, name)
        raw, log = tmp / "raw.mp4", tmp / "log.jsonl"
        started = record(app, script_path, script, work, raw, log, display)
        check_expect(cli, script, work, name)
        events = [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines() if line.strip()]
        begin = next(e["t"] for e in events if e["event"] == "start") / 1000.0 - started - 0.6
        finish = next(e["t"] for e in events if e["event"] == "end") / 1000.0 - started + 0.4
        length = finish - begin

        # The recording, cut to the demo, with its captions.
        caps, chapters = captions(events, -started - begin, length)
        ass = tmp / "captions.ass"
        write_ass(ass, caps, chapters)
        body = tmp / "2-body.mp4"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-ss", f"{begin:.3f}", "-i", str(raw), "-t",
                        f"{length:.3f}", "-vf", f"ass={ass}:fontsdir=/usr/share/fonts/opentype/inter"]
                       + ENCODE + [str(body)], check=True)

        # Title and end cards.
        title, end = tmp / "title.png", tmp / "end.png"
        card(title, script["title"], [script.get("subtitle", "")], "SketchBillet: free 3D design for students")
        card(end, "Your turn!", [f"Follow along in the guide: {script.get('page', 'docs/tutorials')}",
                                 script.get("next", "")], "SketchBillet: free 3D design for students")
        encode_still(title, CARD_SECONDS, tmp / "1-title.mp4")
        encode_still(end, CARD_SECONDS, tmp / "3-end.mp4")
        listing = tmp / "parts.txt"
        listing.write_text("".join(f"file '{tmp / p}'\n" for p in ("1-title.mp4", "2-body.mp4", "3-end.mp4")))
        video = out / f"{name}.mp4"
        part = out / f"{name}.part.mp4"
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", str(listing),
                        "-c", "copy", "-movflags", "+faststart", str(part)], check=True)
        part.rename(video)

        # Captions as subtitles, chapters for YouTube, and a narration script, all
        # timed after the title card.
        shifted = [[s + CARD_SECONDS, e + CARD_SECONDS, t] for s, e, t in caps]
        write_srt(out / f"{name}.srt", shifted)
        marks = [("0:00", "Intro")] + [(chapter_time(s + CARD_SECONDS), t) for s, _, t in chapters]
        description = [script.get("subtitle", ""), "",
                       f"Follow along in the guide: {script.get('page', '')}", "", "Chapters:"]
        description += [f"{at} {text}" for at, text in marks]
        description += ["", "SketchBillet is a free, open-source 3D design program for students."]
        (out / "youtube.txt").write_text(f"Title: SketchBillet {script['title']}\n\n" + "\n".join(description) + "\n",
                                         encoding="utf-8")
        (out / "narration.txt").write_text(
            "".join(f"[{chapter_time(s)}] {t}\n" for s, _, t in shifted), encoding="utf-8")
        total = length + 2 * CARD_SECONDS
        print(f"wrote {video} ({int(total // 60)} min {int(total % 60)} s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sketchbillet-app", required=True)
    ap.add_argument("--sketchbillet-cli", help="for scripts with \"setup\" (default: next to the app)")
    ap.add_argument("--out", default=str(REPO / "build/videos"))
    ap.add_argument("--display", default=":97", help="the Xvfb screen to record on")
    ap.add_argument("--check", action="store_true",
                    help="play each script fast on a virtual screen, and check what it made; record nothing")
    ap.add_argument("names", nargs="*", help="only scripts whose name contains one of these")
    args = ap.parse_args()
    app = Path(args.sketchbillet_app).resolve()
    cli = Path(args.sketchbillet_cli).resolve() if args.sketchbillet_cli else \
        app.parent.parent / "sketchbillet_cli" / "sketchbillet_cli"
    scripts = [s for s in sorted((HERE / "scripts").glob("*.json"))
               if not args.names or any(n in s.stem for n in args.names)]
    if not scripts:
        sys.exit("no video scripts match " + " ".join(args.names))
    if args.check:
        if not shutil.which("xvfb-run"):
            sys.exit("make_videos.py --check needs xvfb-run (apt install xvfb)")
        for script in scripts:
            check(app, cli, script)
        return
    for tool in ("Xvfb", "ffmpeg"):
        if not shutil.which(tool):
            sys.exit(f"make_videos.py needs {tool} (apt install xvfb ffmpeg)")
    for script in scripts:
        make(app, cli, script, Path(args.out).resolve(), args.display)


if __name__ == "__main__":
    main()
