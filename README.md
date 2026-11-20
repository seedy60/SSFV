# SSFV, Seedy's Street Fighter V accessibility mod

This mod was inspired by, and modelled on, Skluttrell's [Killer Instinct access mod](https://github.com/skluttrell/killer-instinct-accessibility-mod). While there is [an existing accessibility mod for Street Fighter V](https://github.com/Kongster90/Street-Fighter-V-Access), it requires you to install Python and run an external script. This mod, however, is written in C++ and built native to the game, meaning all you have to do is copy a few files into the game folder and launch the game.

## What it does

Street Fighter V draws its menus with Scaleform GFx, Adobe's Flash middleware.
Windows accessibility sees one blank rectangle, and NVDA has nothing to read.
This mod sweeps the game's own heap for Scaleform text fields, reads each
field's paragraphs, its position on the 1920x1080 stage and the colour it is
tinted, and reports the field the game has tinted gold. That gold is the
cursor.

Speech goes through Prism, which chooses the best available screen reader
(NVDA, JAWS or another supported reader) or voice. The installer copies
`prism.dll` beside the mod. `speech_backend=auto` is the default; an explicit
Prism backend name, or `sapi` for direct Windows SAPI, can be set in
`ssfv.ini`. SAPI is the fallback when Prism is unavailable, and the log names
the backend it chose.

The mod never writes to game memory. Only the optional macros send input, and
only when you press their keys.

## Setup

### Requirements:

- Windows 10 or 11, 64-bit
- Street Fighter V from Steam
- Visual Studio 2022 with the C++ workload, only to build
- [Git for Windows](https://gitforwindows.org/) to clone the repo
- `prism.dll` from [Prism](https://github.com/ethindp/prism) for
  screen-reader output. Prism reaches a running screen reader or uses another
  available voice; SAPI is the built-in fallback.

Prism is vendored in `vendor\`; its DLL is copied into the game folder with
the mod. No separate NVDA controller client is loaded or installed.

### Installing

1. [Download the latest release](https://github.com/seedy60/SSFV/releases/latest/download/SSFV.zip)
2. Extract the zip file to a location of your choosing.
3. Run install.exe.

The installer copies `xinput1_3.dll`, `prism.dll`, `ssfv.ini` and a small marker file into `StreetFighterV\Binaries\Win64`. Pass `-GamePath` if the game is somewhere else. Start the game normally through Steam.

### Compiling

1. Press Windows + R, type powershell and hit enter.
2. Run these commands to compile the mod files:
    ```powershell
    git clone https://github.com/seedy60/SSFV
    cd SSFV
    .\build.ps1
```

If `xinput1_3.dll` already exists, for example from SF5DInput, the
installer keeps it as `xinput1_3.dll.before-ssfv`. Two proxies cannot
share the name, so remove SF5DInput before using this mod.

## Keys

The keys work while the game has focus. They are released when you switch to
another program, so Ctrl+Shift+A and friends stay available everywhere else.
The three fight readout keys are configurable; the table shows their defaults.

| Key | Action |
| --- | --- |
| Ctrl+Shift+R | Repeat the last announcement |
| Ctrl+Shift+Down | Next line of the last announcement |
| Ctrl+Shift+Up | Previous line |
| Ctrl+Shift+A | Read everything on screen, in reading order |
| Ctrl+Shift+M | Turn menu narration on or off |
| Ctrl+Shift+I | Status: fields read, sweep time, speech backend (for testing) |
| Ctrl+Shift+S | Save a snapshot of every field to a file |
| Ctrl+Shift+X | Stop speaking |
| Ctrl+Shift+H | Read your health, V-Gauge and Critical level |
| Ctrl+Shift+O | Read the opponent's health, V-Gauge and Critical level |
| Ctrl+Shift+C | Read the fight clock |
| Ctrl+Shift+P | Turn the character radar on or off |
| Ctrl+Shift+D | Read the opponent's direction and distance |
| Ctrl+Shift+F1 | Play the current character's Critical Art macro |
| Ctrl+Shift+F2 | Play the current character's V-Skill macro |
| Ctrl+Shift+F3 | Play the current character's V-Trigger macro |

The last three hotkeys are macros designed to make it easier for people who might find it difficult to perform more complex inputs, like the critical art, due to cognitive processing issues, fine motor problems etc. This mod is built with a simple philosophy: if you can't put aside your toxicity for five minutes, you are the one who should be locked out of the fighting game community, not the ones you quite proudly call a skill issue and a certain slur beginning with R. I've seen that toxicity for myself as a disabled gamer and it's why I've point blank refused to play multiplayer fighting games since 2016.

## Character radar

SSFV features a character radar that allows you to easily gage where your opponent is relative to your position on the field through constant beeps that change in pitch and speed. The faster these beeps, the closer you are to your opponent. The higher the pitch, the higher your opponent is above you, E.G. if you're crouching or the opponent is in the air. Control + Shift + D gives you a spoken description of your opponent's relative position, E.G. to the left and below you.

## Settings

`ssfv.ini` sits beside the DLL.

| Setting | Default | Meaning |
| --- | --- | --- |
| `narration` | 1 | Menu narration at startup |
| `poll_ms` | 80 | How often the screen is read |
| `scan_ms` | 400 | How often the heap is swept for new text fields |
| `announce_delay_ms` | 80 | Settle time before a selection is spoken |
| `speech_backend` | `auto` | `auto` lets Prism choose; a Prism backend name selects it; `sapi` uses Windows SAPI |
| `speech_rate` | 0 | SAPI rate, -10 to 10 |
| `narrate_in_background` | 0 | Keep reading when the game is not in focus |
| `log_fields` | 0 | Write every field of every announcement to the log |
| `read_blurbs` | 0 | Read long descriptive blurbs, such as Arcade path stories |

Set the speech backend under `[general]`, for example:

```ini
[general]
speech_backend=auto
```

Use `auto` to let Prism pick, a Prism backend name such as `nvda` or `jaws`
to force that reader, or `sapi` for Windows SAPI directly.

Radar settings live under `[radar]`: `enabled`, `volume`, `range`, `height`,
`near_ms`, `far_ms` and `base_hz`. The defaults are 35, 1800, 220, 120, 650
and 520. `range` and `height` are world units; `near_ms` and `far_ms` are the
pulse interval at point-blank and full range.

Fight readout keys live under `[hotkeys]`: `your_stats`, `opponent_stats` and
`fight_clock`. They accept a letter, a digit, `F1` through `F24`, or a named
key such as `UP`, `DOWN`, `SPACE`, `TAB`, `ENTER`, `INSERT`, `DELETE`, `HOME`,
`END`, `PAGEUP` or `PAGEDOWN`. The defaults are `H`, `O` and `C`. A key must
not duplicate another Ctrl+Shift binding.

```ini
[hotkeys]
your_stats=H
opponent_stats=O
fight_clock=C
```

Macro settings live under `[macros]`: `enabled`, `critical_key`, `vskill_key`,
`vtrigger_key`, `step_ms`, `hold_ms`, `critical`, `vskill` and `vtrigger`.
The first three keys are registered with Ctrl+Shift. The last three are the
fallback sequences for characters without their own section.

```ini
[macros.RYU]
critical=236236P

[macros.GUILE]
critical=[4]646P
```

A section is `[macros.<name>]`, using the name on the health bar. Directions
are numpad directions relative to facing right; the macro reverses them
whenever the player is facing left. `236` is quarter-circle forward, `214`
quarter-circle back, `[4]6` charge back then forward, and `720` for two full
rotations. `P` means heavy punch, `K` heavy kick; `LP`, `MP`,
`HP`, `LK`, `MK` and `HK` are exact.

The INI ships a Critical Art preset for every character with a fixed input.
V-Skill and V-Trigger still come from the defaults for everyone. Eleven copies
its opponent, so it has no Critical Art section; Guile's preset charges
back once for his `4646P` motion, and Zangief's `720P` expands to two full
rotations.

Log: `ssfv-native.log`, beside the DLL. It records the speech backend,
each sweep, the build check and every announcement.