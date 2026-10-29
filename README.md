# AKI Launcher

A native Windows hub for the AKI-engine N64 wrestling game recompilations.
Browse the library as a box-art carousel, download each game's PC port
directly from its GitHub releases, and launch into any of them seamlessly —
with a Return to Launcher shortcut to hop back out of a game.

**This repository and its releases do not contain game assets or ROMs. Each
game requires the user's own legally dumped ROM.**

![Browsing the library and launching straight into a game](docs/demo.gif)

*Full demo — including the in-game Return to Launcher and a second launch:
[docs/demo.mp4](docs/demo.mp4)*

## The games it launches

| Game | Port | Required ROM (unmodified big-endian .z64) — SHA1 |
|---|---|---|
| WCW vs. nWo World Tour | [WCWvsNWOWorldTourRecomp](https://github.com/jessetbh/WCWvsNWOWorldTourRecomp) | North American (NTSC-U) — `5AD2D8359058C8BB71F08E3D3433B7A50D3BB645` |
| Virtual Pro Wrestling 64 | [VPW64Recomp](https://github.com/jessetbh/VPW64Recomp) | Japanese (Japan-only) — `F9E9FA2ED819C3A39DB5CB6AFECA186F021DB5ED` |
| WCW/nWo Revenge | [WCWnWoRevengeRecomp](https://github.com/jessetbh/WCWnWoRevengeRecomp) | North American (NTSC-U) — `E1711A2511394B9357B5F1AC8CA5CC17BD674836` |
| WWF WrestleMania 2000 | [WWFWrestleMania2000Recomp](https://github.com/jessetbh/WWFWrestleMania2000Recomp) | North American (NTSC-U) — `D7D1FAD473FEF9B61FE5F8273C975EE7C603A51B` |
| Virtual Pro Wrestling 2 | [VPW2Recomp](https://github.com/jessetbh/VPW2Recomp) | Japanese (Japan-only; fan-translation patches won't match) — `82DD25A044689EAB57AB362FE10C0DA6388C217A` |
| WWF No Mercy | [WWFNoMercyRecomp](https://github.com/jessetbh/WWFNoMercyRecomp) | North American (NTSC-U) **Rev 1** (Rev 0 won't match) — `91CEE3D096F4A76644D8B35B9AEAD6448909ABD1` |

Each port is a static recompilation — the original N64 game running as a
native PC app (no emulator), with RT64 rendering, high frame-rate-friendly
timing, and file-backed saves.

## Features

- **Plug and play**: pick a game, press Download — the launcher fetches the
  port from its GitHub release (SHA256-verified) into `games\` next to the
  exe, then asks you to select your own ROM dump (SHA1-validated, copied
  alongside the game).
- **Seamless launching**: games open borderless over the launcher window with
  a cartridge-insert animation on the way in; a black transition masks the
  port's boot-time window churn. `Shift+F12` or holding the `View` button
  (about a second) in game brings up the port's quit prompt and returns you to the
  launcher; saves complete before exit.
- **Update flow**: when a newer port release is published, an update line
  appears — `U` / `RB` installs it in place (ROM and saves untouched).
- **Box-art carousel** with front/back flips, per-game blurred backdrops,
  play-time stats, and controller-aware button hints.
- **Windowed or fullscreen** (`F11` / `Alt+Enter`); running games follow the
  launcher window live.
- **Quick match** (`Q` / `Back`): the game starts straight into a CPU-vs-CPU
  match in the venue you pick, through the Quick Match mod the games bundle.

## Controls

| Input | Action |
|---|---|
| Left / Right, D-Pad | browse games |
| Enter, A | play / download / select ROM |
| F, X | flip the box art |
| U, RB | install an available update |
| M, Start | the game's Mods page (install / remove mods and texture packs) |
| Q, Back | Quick match: straight into a CPU-vs-CPU match, venue and match picked (or Random) |
| S, Y | settings (Return to Launcher binding, per-game paths) |
| F11, Alt+Enter | windowed / fullscreen |
| Esc | quit |
| Shift+F12, hold View (in game) | quit prompt → back to launcher |

## FAQ

**How does Quick match work, and does it change my mod settings?** `Q` (`Back` on a controller) opens the selected
game's Quick match page. It needs the game's bundled Quick Match mod (`assets\mods\aki_quickmatch.nrm` next to the game,
or `aki_quickmatch.nrm` in its mods folder), and offers that mod's own options: **Venue** and **Match** (each with
**Random**, a venue or match of the game's own list, never the game's pick), and on some games **Demo length** and
**Entrances**. What a match is in each game (the attract demo in No Mercy and VPW2, a Watch exhibition in the others) is
in the AkiMods `quickmatch` README. **Start match** turns the mod on for that run only:
- It adds `aki_quickmatch` to the game's `mods.json` and writes the picks to `mod_config\aki_quickmatch.json`.
- First, it notes what it changed in `aki-launcher-quickmatch.json` in the game's data folder.
- When the game closes, the mod leaves `mods.json` again, unless it was already on. Other mods turned on or off in the
  game meanwhile stay that way. The mod's config file goes back to exactly what it was, or away if there was none.
- If the launcher closes first, the next launcher start does the undo, or the game's next launch does it if the game is
  still running then. An undo that fails (a file locked by another program) is tried again before the game's next
  launch, which waits for it.

**Where do downloaded games go?** `games\` next to `AkiLauncher.exe` (or
`%AKI_GAMES_ROOT%` if set) — the launcher is portable by default. Settings
and play stats live in `%LOCALAPPDATA%\AkiLauncher\settings.ini`; create an
empty `portable.txt` next to `AkiLauncher.exe` (like the game ports) to keep
`settings.ini` in the launcher folder instead.

**Does portable mode cover the games too?** Yes, for the games the launcher
installed (in its `games\` folder). When the launcher runs portable, it puts a
`portable.txt` in such a game's folder before the game first starts (or its
Mods page opens). The game then keeps its config, saves, mods and log there
instead of in `%LOCALAPPDATA%\<Game>Recompiled\`. That first time, the launcher
copies what the game had in AppData (not logs or texture dumps), never over a
file already in the game folder; the AppData copy stays as it was. Games built
in a depot checkout are left as they are. `portable_games=0` in `settings.ini`
turns this off. (With `AKI_GAMES_ROOT` set, installed games live there, and so
does their portable data.) If a copy fails, the game keeps using AppData and
the launcher tries again on its next start.

**What about ROMs?** You provide your own dumps (`.z64`, big-endian). The
launcher hash-checks your pick against the known-good dump for each game and
keeps a copy with the installed port. Nothing is downloaded from anywhere
except the ports' own GitHub releases, which contain no game data.

**How do I add mods and texture packs?** Select the game and press `M`
(`Start` on a controller) for its Mods page. It lists what is in the game's
mods folder: name, version, authors, type (code mod, texture pack or both)
and thumbnail. *Install files...* takes `.nrm` mods, `.rtz` texture packs and
`.zip` files (a texture pack zipped as is becomes an `.rtz`; a mod package
installs the `.nrm`/`.rtz` files inside it). *Install a pack folder...* copies
a texture-pack folder. You can also drop any of these on the launcher window.
- A mod whose `mod.json` names another game is refused.
- Replacing a file moves the old copy to the Recycle Bin, and *Remove* does
  the same, so both can be undone.
- Mods that come with the game can't be removed (the game copies them back on
  its next start): turn them off in the game's own Mods menu, which is also
  where you turn mods on and off and set their order.
- The mods folder is `%LOCALAPPDATA%\<Game>Recompiled\mods\`, or `mods\` in
  the game's folder when it runs portable. *Open mods folder* opens it.

**And model packs (imported 3D models)?** For No Mercy and VPW2, the Mods page has a **Model packs** section. It lists
the packs in the game's `mods\models\` with their preview, name, author and license, plus a warning when a pack has no
license. Each pack has two controls, saved to `models\models.txt` straight away:
- **On:** unticked writes the `off <pack>` line.
- **Replaces:** nobody, a slot 1-4 of the match, or a wrestler by name (with a costume when the roster names them).
  The pack's line goes first among the rules, ahead of other packs' lines for the same slot or wrestler. The mod still
  lets a `slot` line beat a `wrestler` line when both match.

Each change is made to `models.txt` as it is on disk at that moment, so edits made in Notepad meanwhile are kept. A pack
is read-only on the page, with the reason shown, in these cases:
- its folder isn't a valid pack id (`a-z 0-9 _ -`, at most 31);
- it has several lines in `models.txt`;
- `models.txt` couldn't be read.

Everything else in `models.txt` (comments, other packs' lines, lines the launcher doesn't know) stays as it was, and the
file is replaced in one step, so the game never reads half of it. Wrestler names come from the game's roster:
`assets\rosters\<game>-roster.json` next to the launcher, or AkiTools' `model-templates\profiles\` in a depot. Without
one, a wrestler line is shown but can only be changed in `models.txt` (**Open models.txt**).

Installing packs: the Mods page's **Install files...**, **Install a pack folder...** or a drop on the window takes a
model pack too. It accepts an `<id>.akimodel` folder, a folder of them, or a zip of either.
- The pack goes into `mods\models\`, and its `<id>-hd.rtz` (inside the pack, or next to it) into `mods\`.
- A pack built for another game is refused.
- A pack of the same id is replaced, and the old copy goes to the Recycle Bin.
- Nothing is assigned: pick who it replaces under Model packs.
- **Remove** on a pack moves it and its HD textures to the Recycle Bin. Its `models.txt` lines stay, listed as
  naming a pack that isn't installed.

Packs are made with AkiTools (`aki-model.py quick`), and the aki_modelimport mod (on in the Mods menu) draws them.

**I have a folder full of ROMs. Can it find them?** Yes: Settings → ROM
Folder → *Scan a folder for ROMs...* reads every `.z64`/`.v64`/`.n64` (also
`.rom`/`.bin`, three folders deep) and sorts them under their games:
- the accepted dump of an installed game gets a *Use this ROM* button (a
  byte-swapped dump is saved as `.z64`);
- another build of a supported game (a hack, translation or other revision)
  is named as such, with how it plays: as a patch mod made with
  `tools\romhack\make_patch_mod.py` from the game's repository;
- everything else is listed as not supported.

Only files whose header names one of the games are read in full and
hashed. The ROMs stay where they are; nothing is uploaded.

**Where does the list of games come from?** From `assets\games.json` (the
same file is built into `AkiLauncher.exe`, so a missing or broken copy changes
nothing). Edit it to change or add games: each entry has an `id` (also the box
art name, `assets\boxart\<id>_front.png`), a `title` and an `exe` (relative to
the game's depot folder `repo_dir`; installs use its file name), plus optional
`release_date`, `region`, `rom_file`, `rom_sha1`, `rom_note`, `rom_code`,
`rom_version`, `mod_game_id`, `github_repo`, `force_borderless` and
`coming_soon`. The launcher logs why it ignores a file that doesn't parse.

**Will it find a new recomp by itself?** Yes, if the recomp has an
`aki-game.json` (one entry with the same keys) in its folder. The launcher looks
one level inside the depot (any folder name: clones and worktrees work) and in
its `games\` folder (there the folder must be named after the `id`). Found games
are only ever added: an `id` the list already has is skipped. A manifest with
`"base": "<id>"` is a variant of that game (a mod build, a ROM-hack edition),
and its tile sits right after the base game's.

**Does it update itself?** Yes. At start and every few hours it asks GitHub
whether there is a newer AKI Launcher (cheaply: the answer is cached and only
re-sent when it changed). If there is, a notice appears at the top right;
`L` (or a click) shows the release notes with *Update now*, *Later* and *Skip
this version*. *Update now* downloads the release, checks its size and its
SHA256SUMS (an update without them is refused), swaps the files and restarts.
- Never while a game, a build or an install runs.
- Only the files in the release are replaced: `settings.ini`, `portable.txt`,
  `games\`, mods and saves are never touched.
- The replaced files are kept in `.update\previous\`. If the new version
  doesn't start within 30 seconds, the old one puts everything back and keeps
  running.
- Settings: *Check for launcher updates* (off: `update_checks=0`) and
  *Include pre-releases*. A launcher run from a build folder says "dev build,
  updates off" and never checks.
- `AKI_UPDATE_FEED=<file>` reads the release list from a local JSON file
  instead of GitHub (for trying the notice).

**Can I change the box art?** Yes — swap the images in `assets\boxart\`
(see the README there for file naming).

**Can I point it at my own builds?** Yes — per-game EXE/ROM path overrides
are in Settings.

## Building

Not required to play — grab a release instead. To build: Visual Studio 2022
Build Tools (or full VS) with the C++ workload, CMake ≥ 3.24, Ninja.

```
cmake -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-msvc
```

Dependencies (Dear ImGui, miniz) are fetched automatically by CMake.
`tools\package.ps1` reproduces the release zip.

### Dev mode

For developers working in the depot (the folder with the game checkouts and
`AkiTools`); players never see it. Turn it on with `--dev`, `AKI_DEV=1`, or
`dev_mode=1` in `settings.ini`. Then the selected game shows a line:
`R` / `LB` rebuilds its depot checkout. The launcher runs
`AkiTools\aki-build.ps1 -Worktree <checkout> -Target <exe> -Owner Launcher`.
That build waits for the shared build slot like any agent's build and shows
its output and Ninja progress in a panel. `Backspace` / `B` cancels it (the
whole process tree), or dismisses the result. A finished build re-scans the
games, so a first build becomes playable. Closing the launcher cancels a
running build.

`cmake -B build-msvc -DAKI_LAUNCHER_TESTS=ON` also builds
`AkiLauncherTests.exe`, console checks for the rebuild action (output
parsing, exit codes, cancel) against a fake `aki-build.ps1`. Its exit code is
the number of failures. `--test-rebuild <game index>` rebuilds one game
through the UI loop and exits with 0 (built) or 4.

## Libraries used

- [Dear ImGui](https://github.com/ocornut/imgui) (MIT) — UI
- [miniz](https://github.com/richgel999/miniz) (MIT) — zip extraction
- Windows: Direct3D 11, WIC, WinHTTP, XInput, winmm

## Legal

Licensed under the GPLv3 — see [COPYING](COPYING).

This is an unofficial fan preservation project. It is not affiliated with,
endorsed by, or sponsored by AKI Corporation / syn Sophia, Hyperfocus Games,
WWE, THQ, Nintendo, or any rights holder of the games it launches. All
trademarks are the property of their respective owners. No game code, game
assets, or ROMs are included in this repository or its releases.
