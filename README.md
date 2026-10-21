# top-processes-on-taskbar
Show the top two CPU-consuming processes on the Windows 11 taskbar. Windhawk mod.

Created with DeepSeek Agent, using [this mod](https://github.com/starychenko/windhawk-taskbar-system-info) as reference. Many thanks to the author for providing the foundation for other taskbar widget mods. GPL v3 license is inherited from the reference mod. I originally created this mod just for myself, but then I decided to share it, so I set up a repository. Don't expect any updates, there might be some, or there might not be - but the mod works fine for now on Windows 11 25H2.

<img width="1233" height="254" alt="image" src="https://github.com/user-attachments/assets/9e4df0d9-c299-47f9-bb0e-92fd70b4d91a" />


## Installation
1. Download mod file top-processes-on-taskbar.vN.wh.cpp or open it in raw format
2. Open Windhawk, click "Create a New Mod" in bottom-right corner
3. Copy the code from this mod and replace the example code in the editor
4. Click "Compile Mod"
5. Once compilation is complete, click Exit Editing Mode


## How it works

- CPU usage is read from the Windows performance counter
  `\Process V2(*)\% Processor Time` (with a fallback to `\Process`).
- Values are normalized to the whole system, like the Task Manager CPU column:
  one fully loaded core on an 8-core CPU shows about 12.5%.
- Processes with the same executable name are grouped (for example, all `firefox.exe`
  instances appear as one line).
- The widget is inserted into the taskbar's native XAML tree, so it moves and
  hides together with the taskbar, follows the taskbar theme and scales with
  display DPI.

## Settings

- **Left offset** - distance from the left edge of the taskbar, in XAML
  device-independent pixels. Increase it if the taskbar is left-aligned and
  the text overlaps the Start button.
- **Top offset** - fine vertical adjustment.
- **Font size**, **Font family**, **Max name width**, **Text opacity**.
- **Text color** - `#RRGGBB` or `#AARRGGBB`; empty means automatic light/dark
  color.
- **Update interval** - how often values are refreshed.
- **Excluded processes** - comma-separated executable names, for example
  `System, explorer.exe`.

## Limitations

- Taskbar XAML hooks depend on Windows symbols. A future Windows update may
  break them; the mod logs an error and does not crash Explorer.
- The widget is placed over the taskbar rather than reserving layout space.
  With a left-aligned taskbar it can overlap the Start/search area, so use a
  larger **Left offset** or a centered taskbar.
- Protected processes are read through performance counters, but processes
  that are invisible to the counter are not shown.
- On multi-monitor systems every taskbar that is discovered gets a widget.
