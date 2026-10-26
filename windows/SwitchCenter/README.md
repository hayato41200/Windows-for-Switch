# Switch control center (SwitchCenter)

Notification-area app for the Switch drivers. Stage 1: CPU profile and screen
brightness from the tray menu (replaces `Switch-ClockTray/SwitchClock.exe`).
Design (Japanese): `../Design-ControlCenter-ja.md`.

Build (needs the .NET SDK, any version that can target .NET 8):

    dotnet publish src/SwitchCenter.csproj -c Release -o out

The output is a self-contained win-arm64 folder (about 160 MB, no .NET install
needed on the Switch). Copy it to the Switch and run `SwitchCenter.exe`.
The binaries are not stored in this repository.
