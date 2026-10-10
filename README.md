# Dynamic Clouds

Dynamic Clouds replaces the original cloud system in World of Warcraft Vanilla 1.12.1 with a new cloud system inspired by the style of *The Legend of Zelda: Breath of the Wild*.

## Features

- **Detailed clouds** with an enhanced visual appearance.
- **Dynamic cloud movement** for a more immersive sky.
- **Customizable cloud density, size, and opacity.**
- **Server weather-reactive clouds**, responding to clear weather, rain, snow, and dust storms.

## Showcase

### Dynamic Cloud Movement

![Image](https://github.com/Newmansk/WoW-Clouds/blob/main/media/clear%20clouds.gif)

### Weather-Reactive Cloud Density

![Image](https://github.com/Newmansk/WoW-Clouds/blob/main/media/rain%20clouds.gif)

## Compatibility (Tested)

| Component | Configuration |
|---|---|
| World of Warcraft | Vanilla 1.12.1 |
| Additional client support | 1.18.1+ with VanillaFixes and `wow_clouds.dll` listed in `dlls.txt` |
| DXVK | v2.7.1-1-gplasync |
| Operating System | Windows 10 Pro 64-bit (10.0, Build 19045) |
| GPU | AMD Radeon RX 5700 XT |
| Graphics Driver | Version 26.9.2 |

Compatibility may vary depending on the client version, hardware, drivers, and configuration.

## Dependencies

- **VanillaFixes**, including `VanillaFixes.exe` and its `dlls.txt` configuration file.
- `wow_clouds.dll`
- `wow_clouds.ini`

VanillaFixes is an external dependency and must be installed and configured separately.

## Installation

1. Download the latest release from the [Releases](../../releases) page.
2. Extract `wow_clouds.dll` and `wow_clouds.ini` into your World of Warcraft client root directory, alongside `wow.exe`.
3. Open the `dlls.txt` file used by your VanillaFixes installation.
4. Add `wow_clouds.dll` to the list, following the file's existing format.
5. Save the file and launch the game.
6. Verify that Dynamic Clouds is working correctly.

## Configuration

A dedicated in-game addon for real-time configuration is not available yet.

For now, you can customize the mod by editing `wow_clouds.ini` while the game is running. Modify the available settings to adjust the cloud appearance and behavior to your preferences.

The configuration file is automatically reloaded while the game is running, so restarting the client should not normally be necessary.

## Development

- **Language:** C++
- **IDE:** Microsoft Visual Studio 2022
- **AI-assisted development:** Claude Code (Sonnet 5, free version)

## Credits

- **Author:** Newmans
- **Development assistance:** Claude (Anthropic)

### Special Thanks

- **samwhosung** for documenting the reverse engineering of the World of Warcraft 1.12.1 client, especially the [lighting documentation](https://github.com/samwhosung/wow-1121-client-internals/blob/main/docs/lighting.md).

## License

This project is licensed under the [MIT License](LICENSE).

You are free to use, modify, fork, and redistribute this project's source code under the terms of the MIT License.

## Disclaimer

Dynamic Clouds is an unofficial community project and is not affiliated with, endorsed by, or sponsored by Blizzard Entertainment.

World of Warcraft is a trademark of Blizzard Entertainment. The Legend of Zelda: Breath of the Wild is a trademark of Nintendo.
