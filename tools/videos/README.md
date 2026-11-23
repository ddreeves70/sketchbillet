# Tutorial videos

Videos of the tutorials, done entirely in the desktop app, for YouTube. Each
video is a **script** of clicks, typing and captions in `scripts/`. The app
plays it on its real buttons and boxes, with the mouse pointer gliding to each
thing it clicks, and `make_videos.py` records that and puts the captions on it.
When the app changes, make the videos again with one command.

## Make the videos

**On Linux**, with Xvfb, ffmpeg (with libx264 and libass) and Pillow. The videos
play on a virtual screen, so you can keep working while they're made:

```bash
sudo apt install xvfb ffmpeg python3-pil
python3 tools/videos/make_videos.py --sketchbillet-app build/dev/apps/sketchbillet/sketchbillet
python3 tools/videos/make_videos.py --sketchbillet-app build/dev/apps/sketchbillet/sketchbillet tutorial-01
```

The second line makes only the videos whose script name contains `tutorial-01`.

**On Windows**, in a Command Prompt. Install ffmpeg and Pillow once, then open a
new Command Prompt so it finds ffmpeg:

```bat
winget install Gyan.FFmpeg
py -m pip install pillow
```

Then, in the SketchBillet folder, after building with the `win` preset:

```bat
py tools\videos\make_videos.py --sketchbillet-app build\win\apps\sketchbillet\sketchbillet.exe tutorial-01
```

On Windows the video plays on your **main screen**: SketchBillet fills it, and
the mouse pointer moves by itself. Leave the mouse and keyboard alone until it
says `wrote ...`, or the video shows what you did. The video is made 1920 × 1080
whatever your screen's size.

- **Words too small or too big?** The app is drawn at your display's scaling
  (Settings > Display > Scale). At 100 %, add `--scale 1.5` so the words read
  well on a phone; at 150 % leave it out.
- **The app doesn't start** ("Qt6Core.dll was not found")? Put your Qt's `bin`
  folder on the path first, like `set PATH=C:\Qt\6.8.3\msvc2022_64\bin;%PATH%`.

Each video takes about as long as it lasts. In `build/videos/<name>/` you get:

| File | What it is |
| --- | --- |
| `<name>.mp4` | The video: 1920 × 1080, 30 frames a second, with a title card, the captions burnt in, and an end card |
| `<name>.srt` | The captions, to upload to YouTube as subtitles |
| `youtube.txt` | A title, and a description with chapters (YouTube makes them into chapter marks) |
| `narration.txt` | The captions with their times, to record a voice-over from |

The videos aren't kept in the repository: they're big, and they're made again
whenever the app changes. Upload them to YouTube, or attach them to a release.

## Write a script

A script is a JSON file in `scripts/`, named after the guide page it follows
(`tutorial-02-desk-organizer.json`):

```json
{
  "title": "Tutorial 1: Keychain tag",
  "subtitle": "Steps, boxes, cylinders, cutting a hole, and exporting for 3D printing",
  "page": "docs/tutorials/01-keychain-tag.md",
  "next": "Next: Tutorial 2, the desk organizer.",
  "steps": [
    {"chapter": "Step 1: Make the plate"},
    {"caption": "Click Box on the Model tab. It adds a 20 mm box."},
    {"click": "action:sb.modeling.box"},
    {"type": "Plate", "into": "#name"},
    {"type": "60", "into": "#size.x"}
  ],
  "expect": {"file": "tag.sb", "lines": ["Tag ok volume 7121.46 mm^3 mass 9.04 g (PETG)"]}
}
```

- **Steps** are what a student does: `click`, `type`, `key`, `drag`, `scroll`,
  `hover` and `wait`, and `caption` and `chapter` for the words on the screen.
  [`apps/sketchbillet/Demo.h`](../../apps/sketchbillet/Demo.h) lists them all,
  and the **targets** they take: a toolbar button (`action:sb.modeling.box`), a
  box in Properties by its object name (`#size.x`), a row of the feature tree
  (`tree:Plate`), an item of the open menu (`menu:Export...`) or dropdown
  (`list:PETG`), a dialog's button (`button:OK`), or a point in the view
  (`view:0.4,0.5`).
- **Captions** say what the student does next and why, in plain words for a
  14-year-old, with units, like the guides. Each one stays up until the next, and
  the video rests long enough to read it (about 3 words a second).
- **Chapters** split the video, and become YouTube's chapter marks. Use the guide
  page's headings.
- **`expect`** is what `sketchbillet_cli open` prints about the design the video
  ends with: the same numbers as the guide page, so the video makes what the page
  makes.
- **`setup`** (optional) is `sketchbillet_cli` commands run first, for a video
  that starts from a saved design, and **`open`** is the design to open.

Tools in a panel's dropdown have no button: click the dropdown, then the item
(`{"click": "#sb.ribbon.model.create"}`, then `{"click": "menu:Cylinder"}`). Turn
the view by dragging on an empty part of it, away from the part's move arrows.

Try a script fast while you write it, with no recording (on Windows it plays on
your screen for a few seconds):

```bash
python3 tools/videos/make_videos.py --check --sketchbillet-app build/dev/apps/sketchbillet/sketchbillet tutorial-01
```

The `videos.*` tests do the same for every script, so a change to the app that
breaks a video fails the build. Fix the script (or the app) in the same pull
request, as with the guides' examples.
