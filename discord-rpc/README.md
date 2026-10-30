# Discord RPC

Shows "Playing <the game>" with the play time on your Discord profile, for every game Azimuth runs:
Forza Horizon 2, Fast & Furious, Forza Horizon 3, Forza Motorsport 5 and Forza Motorsport 6.

Install: put `FH2_DiscordRPC.dll` in the `mods` folder next to `azimuth.exe`. No settings. It only talks to the Discord app
on your PC.

## Linux (Proton)

It works when Discord or Vesktop is already running before you start the game: Steam's container
only picks up Discord's socket at game start. Vesktop shows the status after 30-50 seconds.

Flatpak Discord and Vesktop keep that socket where the game can't see it. Link it once; the link is
made again at every login:

```sh
mkdir -p ~/.config/user-tmpfiles.d
cat > ~/.config/user-tmpfiles.d/discord-rpc.conf <<'EOF'
L+ %t/discord-ipc-0 - - - - app/com.discordapp.Discord/discord-ipc-0
L+ %t/discord-ipc-1 - - - - .flatpak/dev.vencord.Vesktop/xdg-run/discord-ipc-0
EOF
systemctl --user enable systemd-tmpfiles-setup.service
systemd-tmpfiles --user --create
```
