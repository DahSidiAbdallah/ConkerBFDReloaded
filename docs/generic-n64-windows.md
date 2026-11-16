# Optional generic N64 USB controller profile (Windows)

This profile describes one transparent-red, three-prong Chinese N64 USB pad.
It is opt-in: other controllers share USB VID/PID `0079:0006` with incompatible
layouts. Neither the case color nor a matching GUID/CRC establishes the layout.
Compare the physical inputs before using it.

| Physical control | Raw SDL input, zero-based |
| --- | --- |
| A / B / Start | buttons 5 / 4 / 9 |
| Z / L / R | buttons 8 / 6 / 7 |
| C up / right / down / left | buttons 0 / 1 / 2 / 3 |
| Stick X / Y | axes 0 / 1 |
| D-pad | hat 0 |

Native SDL reports 4 axes, 12 buttons and 1 hat. The same physical unit was
observed as `USB Joystick` with SDL2 2.32.10 (CRC `8630`) and SDL2 2.26.3
(`8e4f`), and as `Microntek USB Joystick` with SDL3 3.4.18 (`79be`). The
[profile](controller-profiles/generic-n64-windows-n64recomp.txt) contains those
three Windows variants. CRCs are not serial numbers; older SDL can fall back
across CRCs. No Linux, macOS or Android identity is inferred.

## Load for one session

Download the profile, put it beside `ConkerBFDReloaded.exe`, and open PowerShell
in that folder. Close any running copy of the game, then run:

```powershell
$savedMapping = $env:SDL_GAMECONTROLLERCONFIG
try {
    $env:SDL_GAMECONTROLLERCONFIG = (Get-Content .\generic-n64-windows-n64recomp.txt |
        Where-Object { $_ -and -not $_.StartsWith('#') }) -join "`n"
    & .\ConkerBFDReloaded.exe
} finally {
    $env:SDL_GAMECONTROLLERCONFIG = $savedMapping
}
```

SDL reads the override at startup. This changes the environment only for the
session and launched process. It does not insert this shared identifier into a
shipped database or change system environment variables. Launch normally to
stop using the override.

## Check the bindings

This is a profile for this port's default bindings: physical B becomes SDL X
(West), R becomes the right trigger, L the left shoulder, Z the left trigger,
and the four C buttons become signed right-stick directions. Mapping B to SDL B
or R to the right shoulder would activate C-right or C-down under those defaults.
A, Start, the left stick and D-pad retain their usual roles.

Open Settings with **Escape** and check the assigned player's controller
profile in Controls, especially if you changed its bindings before. Saved
bindings are not reset by importing SDL mappings. No physical Select button was
identified; Back is deliberately unassigned. Do not sacrifice Z for a menu button.

The free camera and Swap Sticks features can change how the right stick is used.
Use Classic mode to check the original N64 control scheme. The SDL file does
not apply deadzone calibration; the frontend uses an axis deadzone, which should
not be treated as the radial calibration from the capture.

## Evidence and limits

MatyMeatBoy supplied the physical measurements: 18 assignments and 206 frames
over 10 seconds in the browser capture. Native SDL confirmed device identity,
counts and recognition after import. Virtual SDL2 2.32.10 checks validate the
mapping translation, including all C directions, D-pad diagonals, intermediate
stick values and release. They do not
replace pressing the physical controls in this game. No physical gameplay test
or full game build was performed for this contribution; the game build requires
the owner's ROM. Test idle drift, slow movement and every button before relying
on this layout.
