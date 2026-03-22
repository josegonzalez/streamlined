# Using the Streamlined Menu

The Streamlined Menu is a simple, clean way to browse your game collection and play games in RetroArch. Instead of showing you lots of menus and options, it focuses on what matters most: finding a game and playing it.

## The Main Screen

When you open RetroArch with the Streamlined Menu, you see a list of your game folders. Each folder usually holds games for one system (like "Game Boy" or "Super Nintendo"). At the bottom of the list, you will also see:

- **Settings** - Opens system settings
- **Quit** - Closes RetroArch (not shown on Apple TV)

Use **Up** and **Down** to move through the list. The item you have selected is highlighted with a white pill shape.

## Opening a Folder

Press **A** (or **OK**) on a folder to open it. Inside, you will see your games listed by name (without the file extension, so "Super Mario World.sfc" just shows as "Super Mario World").

If the folder has subfolders inside it, those show up with a `/` in front of the name. You can open them the same way.

To go back to the previous screen, press **B** (or **Back**).

## Playing a Game

Highlight a game and press **A** to start it.

- If the folder already knows which core (emulator) to use, the game starts right away.
- If the folder does not have a core set up yet, you will see a list of all the cores you have installed. Pick one, and the game will start. The menu remembers your choice, so next time it will use the same core automatically.

If a game has saved progress from a previous session (an auto save), you will see **X Resume** at the bottom of the screen. Pressing **X** will start the game and pick up right where you left off. If you have auto-load savestates turned on in RetroArch's settings, pressing **A** will also resume automatically. If auto-load is turned off, you will see both **X Resume** and **A OK** — press **X** to resume or **A** to start fresh.

## The Quick Menu (Pause Menu)

While playing a game, open the menu to see the Quick Menu. The title at the top shows the name of the game you are playing. You will see these options:

- **Resume** - Goes back to your game
- **Save** - Saves your progress
- **Load** - Loads a previous save
- **Advanced** - Opens more detailed settings for the game
- **Reset** - Restarts the game from the beginning
- **Quit** - Stops the game and goes back to the main screen

Pressing **B** (Back) also resumes your game, just like selecting Resume.

## Saving and Loading

When you highlight **Save** or **Load** in the Quick Menu, a save slot picker appears on the right side of the screen. It shows:

- A preview image (screenshot) of what is saved in that slot
- A row of dots at the bottom representing each slot

There are 9 slots total: **Auto** (marked with an "A") and slots **1 through 8** (shown as dots). Use **Left** and **Right** to switch between slots. The selected slot is highlighted in teal.

- If a slot has a save, you will see a screenshot of it.
- If a slot is empty, it will say "Empty."
- If a slot has a save but no screenshot, it will say "No Screenshot."

Press **A** to save to (or load from) the selected slot.

## Advanced Settings (In-Game)

Selecting **Advanced** from the Quick Menu opens a list of detailed settings you can change while your game is running. These include things like:

- Audio and Video settings
- Controls and Input
- Cheats
- Shaders (visual filters)
- Core Options (settings specific to the emulator)
- And more

Press **B** to go back to the Quick Menu.

## Settings (Main Menu)

Selecting **Settings** from the main screen opens a list of system-wide settings. These let you change things like video output, audio, input, network, and other RetroArch options.

Press **B** to go back to the main screen.

## Navigation Basics

Here is a quick summary of the controls:

| Button | What it does |
|---|---|
| **Up / Down** | Move through the list |
| **A / OK** | Select the highlighted item |
| **X / Resume** | Resume a game from where you left off (when available) |
| **B / Back** | Go back to the previous screen |
| **Left / Right** | Switch save slots (when on Save or Load) |

At the bottom of every screen, you will see a reminder showing **B** for Back on the left and **A** for OK on the right.

## Returning to Your Game

When you quit a game and go back to the main screen, the menu remembers where you were. It takes you right back to the same folder and highlights the same game you were playing. That way you can easily pick up where you left off or choose a different game from the same folder.

## Folder Setup Tips

You can control the order your folders appear by adding a number prefix like `1) `, `2) `, etc. to your folder names. The menu strips these prefixes away, so a folder named `1) Game Boy` will just show as "Game Boy" in the menu, but it will be sorted first.

Each folder can have a hidden file called `.core.txt` that tells the menu which core to use for games in that folder. If this file exists, games in the folder launch immediately without asking you to pick a core. The menu creates this file automatically the first time you pick a core for a folder.
