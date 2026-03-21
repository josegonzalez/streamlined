# Streamlined Menu User Guide

The Streamlined menu is a custom, MinUI-inspired menu designed for simplicity and speed. It replaces RetroArch's default interface with a clean, focused experience — dark backgrounds, rounded pill-shaped highlights, and teal accents give it a modern, minimal feel. Everything is built around getting you into your games quickly.

---

## Controls at a Glance

| Button             | Action                                                                |
| ------------------ | --------------------------------------------------------------------- |
| D-Pad Up / Down    | Navigate lists                                                        |
| D-Pad Left / Right | Cycle between Game Switcher, save slots, and color picker values      |
| A                  | Select / Confirm / Play                                               |
| B                  | Back — resumes your game when pressed from the Quick Menu main screen |
| Y                  | Open Game List Options for the highlighted game                       |
| X                  | Resume from autosave (when available)                                 |

The footer bar at the bottom of the screen always shows which buttons are available for the current context.

---

## Browsing and Launching Games

### Platform Folders

The main menu displays your game folders, each optionally showing a thumbnail image. Folders are sorted using a prefix pattern — for example, naming a folder `1) Game Boy` places it first in the list. The sort prefix is stripped from the display, so you'll just see "Game Boy."

### ROM Thumbnails

When you highlight a game, its thumbnail appears on the right side of the screen. Thumbnails are loaded from a `.media` folder inside the game's directory:

```text
{game_folder}/.media/{Type}/{romname}.png
```

Where `{Type}` is one of `Screenshot`, `Title`, or `Boxart`, depending on your **Primary Thumbnail** setting.

### Launching a Game

Press **A** to launch the highlighted game.

- If the folder already has a core assigned, the game launches immediately.
- If no core is set, you'll be prompted to choose one. Your choice is saved for the folder so you won't be asked again.

### Resuming a Game

If an autosave exists for the highlighted game, an **X** button prompt appears in the footer. Press **X** to resume exactly where you left off, or **A** to start fresh.

### Multi-Disc Games

Folders containing M3U playlists (used for multi-disc games) display as a single entry using the folder name. Disc management is handled automatically — you don't need to worry about selecting individual disc files.

### Clean ROM Names

Enabled by default, this feature tidies up game names by removing tags like `(USA)`, `[!]`, and similar markers. It also fixes article placement — for example, "Legend of Zelda, The" becomes "The Legend of Zelda."

---

## Favorites

Add any game to your Favorites by pressing **Y** to open Game List Options, then selecting **Add to Favorites**. Favorite games appear at the top of the main menu for quick access, regardless of which folder they came from. To remove a game from Favorites, open Game List Options on the favorited entry and select **Remove from Favorites**.

Favorites persist across sessions and are saved automatically.

---

## Game Switcher

The Game Switcher shows your recently played games, letting you jump back into any of them quickly. It offers two viewing modes:

- **Image-centric** (default): Shows a large thumbnail of the selected game. Use **Left** and **Right** to browse through your recent games, with the game name displayed in the footer.
- **Text list**: A traditional list view. Use **Up** and **Down** to browse.

Press **A** to play the selected game, or **Y** to open Game List Options.

You can switch between view modes in **Settings > User Interface > Appearance > Game Switcher View**.

---

## Game List Options

Press **Y** on any game to open the Game List Options menu. The available actions depend on context:

| Option                        | Description                                                                 |
| ----------------------------- | --------------------------------------------------------------------------- |
| **Reset Game**                | Deletes the autosave and restarts the game from the beginning               |
| **Search**                    | Opens a fuzzy search keyboard to find games by name                         |
| **Random Game**               | Picks a random game and shows a preview — press A to play or X to resume    |
| **Delete Autosave**           | Removes the autosave file for the selected game                             |
| **Set Folder Core**           | Assigns a default core for all games in the current folder                  |
| **Set Game Core**             | Assigns a specific core for just this game, overriding the folder core      |
| **Clear Game Core**           | Removes the per-game core override, falling back to the folder core         |
| **Add to Favorites**          | Adds the game to your Favorites list                                        |
| **Remove from Favorites**     | Removes the game from Favorites (shown only for favorited games)            |
| **Remove from Game Switcher** | Removes the entry from recent games (only in Game Switcher)                 |
| **Delete Game**               | Permanently deletes the game file, preserving saves, states, and thumbnails |

Not all options appear in every context. For example, Reset Game and Delete Autosave only appear when an autosave exists, and Delete Game is hidden if the game is currently running.

---

## Search

Selecting **Search** from Game List Options opens an on-screen QWERTY keyboard. Type part of a game name and results update as you go. The search uses fuzzy matching — it ignores case, special characters, and spaces, so you don't need to type an exact name.

Navigate between the keyboard and the results list to find your game, then select a result to launch it.

---

## In-Game Quick Menu

Press the **menu button** during gameplay to open the Quick Menu. The main screen shows these options:

- **Resume** — Return to your game
- **Save** — Save your current progress to a slot
- **Load** — Load a previously saved state
- **Advanced** — Opens the full settings submenu
- **Reset** — Restart the current game
- **Exit / Quit** — Leave the game (shows "Save and Quit" when autosave is enabled)

### Save Slot Selector

When **Save** or **Load** is highlighted, a slot selector appears on the right side of the screen. It shows a thumbnail preview of the selected slot with dot indicators below representing each slot.

- **9 slots** are available: Auto, plus slots 0 through 7
- Use **Left** and **Right** to cycle through slots
- The thumbnail preview updates as you browse
- Press **A** to save or load using the selected slot

### Advanced Submenu

Selecting **Advanced** opens access to the full range of RetroArch settings categories:

Achievements, Audio, Cheats, Controls, Core Options, Disc Control, Information, Input, Latency, Onscreen Overlay, Overrides, Recording, Rewind, Saving, Screenshot, Shaders, and Video.

Some entries (like Cheats, Disc Control, and Rewind) only appear when their corresponding features are available for the current core.

Press **B** to return to the main Quick Menu.

---

## Managing Cores

### Folder Cores

Assign a default core for an entire folder by selecting **Set Folder Core** in Game List Options. The core assignment is stored in a file called `.core.txt` inside the game folder. You can list multiple core names (one per line) in order of preference — the first installed core on your device will be used.

### Per-Game Core Overrides

If a specific game needs a different core than the rest of its folder, use **Set Game Core** from Game List Options. This creates a per-game override file (`.core.{gamename}.txt`) in the same folder. To remove the override and fall back to the folder core, use **Clear Game Core**.

### How Cores Are Resolved

When you launch a game, the menu checks for a core in this order:

1. Per-game core override (`.core.{gamename}.txt`)
2. Folder core (`.core.txt`)
3. Playlist entry fallback

---

## Setting Up Thumbnails

### Thumbnails Locations

Place thumbnail images for individual games inside a `.media` folder within the game's directory:

```text
{game_folder}/.media/Screenshot/{romname}.png
{game_folder}/.media/Title/{romname}.png
{game_folder}/.media/Boxart/{romname}.png
```

Use the ROM's filename without its extension as the image name. The type subfolder used depends on your **Primary Thumbnail** setting (Screenshot, Title Screen, or Boxart).

### Folder Thumbnails

To show a thumbnail for a folder on the main menu, place an image in the parent directory's `.media` folder:

```text
{parent_folder}/.media/{foldername}.png
```

Folder thumbnails do not use a type subfolder.

### Primary Thumbnail Setting

Choose which type of thumbnail to display in **Settings > User Interface > Appearance**. Options are Screenshot, Title Screen, or Boxart.

---

## Customizing the Menu

All Streamlined-specific settings are found in **Settings > User Interface > Appearance**.

| Setting                    | Default       | Description                                                                     |
| -------------------------- | ------------- | ------------------------------------------------------------------------------- |
| **Show Folder Thumbnails** | On            | Display thumbnail images for folders on the main menu                           |
| **Clean ROM Names**        | On            | Remove tags and fix article placement in game names                             |
| **Game Switcher View**     | Image-centric | Choose between a large thumbnail view or a text list for recent games           |
| **Thumbnail Height**       | 0.45          | Maximum thumbnail height as a percentage of screen height (range: 0.20 to 0.80) |
| **Thumbnail Width**        | 0.50          | Maximum thumbnail width as a percentage of screen width (range: 0.20 to 0.80)   |
| **Notification Duration**  | 3 seconds     | How long notification screens are displayed (range: 1 to 10 seconds)            |
| **Background Opacity**     | 0.85          | Opacity of the menu background overlay (range: 0.0 to 1.0)                      |
| **Selection Color**        | White         | Customize the highlight pill color using an RGB color picker                    |

### Selection Color Picker

The color picker lets you personalize the menu's highlight color. Each color channel (Red, Green, Blue) is adjusted individually:

- **Up / Down** selects the channel
- **Left / Right** adjusts the value (hold the button for faster changes)
- A hex color code is displayed as you make changes
- The default is white (R: 255, G: 255, B: 255)

---

## Tips and Recommendations

- **Organize with sort prefixes** — Name your folders with prefixes like `1) NES`, `2) SNES`, `3) Game Boy` to control their display order. The prefixes are hidden in the menu.

- **Set folder cores once** — Assigning a core to each folder means you'll never be prompted to choose one when launching a game. Use per-game overrides only when needed.

- **Keep Clean ROM Names on** — Unless you specifically need to see region tags and dump info, this setting makes your game lists much easier to read.

- **Adjust thumbnail size** — If artwork looks too small or too large on your screen, tweak the Thumbnail Height and Thumbnail Width settings to find the right balance.

- **Lower Background Opacity for in-game menus** — Reducing the opacity below 0.85 lets you see your game behind the Quick Menu, which can be helpful for context.

- **Personalize with Selection Color** — Use the RGB color picker to change the highlight pill from the default white to any color you like.

- **Choose your Game Switcher style** — Image-centric mode is great for visual browsing when you have good thumbnails. Switch to Text List mode for quick scanning when you know what you're looking for.
