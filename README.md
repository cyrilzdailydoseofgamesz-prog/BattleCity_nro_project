# Battle City - standalone Switch homebrew

Single NRO with the game embedded. No RetroArch or external emulator needed.
No sound (APU not implemented). Supports only this kind of ROM (NROM, mapper 0).

## Build without a PC (phone only)
1. Create a free GitHub account, make a new repo, upload ALL files of this folder
   (keep the .github/workflows/build.yml path).
2. Open the repo > Actions > "Build NRO" > Run workflow.
3. When it finishes, download the artifact "BattleCity-nro" (contains BattleCity.nro).

## Build on a PC
Install devkitPro + switch-dev, then run `make`.

## Install
Copy BattleCity.nro to SD card: /switch/BattleCity/BattleCity.nro
Launch from the Homebrew Menu (hold R on a game, open Album/hbmenu).

## Controls
D-pad/left stick move, A/B (also X/Y) fire, + Start, - Select, L+R quit.

## NSP (optional, home-menu icon)
Build a forwarder with NTON / TooTallNate's switch-nsp-forwarder pointing to
/switch/BattleCity/BattleCity.nro. Forwarders can risk bans online; stay offline.
