# Resource Saver / 精打细算 v1.2.0

Author: **liuxin**  
Package: `celso.resourcesaver`  
Target: Terraria Android 1.4.5.6.4 + TEFKernel / KernelLoader

## v1.2.0: Custom switches

This version adds a persistent settings system and an in-game settings overlay.
Open Terraria's in-game **Settings / Options** screen. A `Resource Saver [ OPEN ]`
entry is drawn on the left side. Tap it to open the Resource Saver panel.

Available switches (all ON by default):

1. Master switch
2. Regular ammo +20%
3. Special ammo +10%
4. Magic mana -15%
5. Summon/Sentry mana -25%
6. Mana regen +20%
7. Melee potion recovery +10%
8. Combat buff duration +20%
9. Bait saver +20%

The panel also includes **Restore defaults**.

## Persistent config

Settings are written immediately to the mod private directory as:

`config.ini`

Example:

```ini
master=1
regular_ammo=1
special_ammo=1
magic_mana=1
summon_mana=1
mana_regen=1
melee_potion=1
combat_buff=1
bait=1
```

`1` means enabled and `0` means disabled. This file is also a fallback way to
change settings if the UI hook is unavailable on a future Terraria build.

## Build

The repository keeps the same KernelLoader/CMake structure as v1.1.0. Push the
source to GitHub and run the included Android workflow. The output library is:

`libResourceSaver.android.arm64.so`

## Runtime diagnostics

Look for these messages in TEFKernel logs:

- `Config loaded: ...`
- `Combat v1.2.0 hooks: ...`
- `Combat buff v1.2.0 hook: ready`
- `Bait saver v1.2.0 ... hook: ready`
- `Settings UI v1.2.0: ready`

If `Settings UI v1.2.0` reports `failed`, the gameplay features still load and
`config.ini` remains usable.
