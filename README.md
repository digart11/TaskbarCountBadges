# Taskbar Count Badges

See how many windows are open for each app directly on the Windows 11 taskbar.

Taskbar Count Badges adds a small customizable indicator to taskbar app buttons
when an app has multiple open windows. The indicator can be shown as either a
**number badge** or a compact stack of **vertical dots**.

## Display styles

### Number badge

Shows the exact number of open windows for an app.

The badge can be customized with:

- Circle, rounded-square, or square shape
- Badge size and position
- Horizontal and vertical offsets
- Background, text, and border colors
- Border thickness
- Font family, size, and weight
- Configurable maximum number, with larger values shown using `+`

### Vertical dots

Shows a minimal vertical stack of dots beside the app icon.

- Position the dots on the left or right
- Change dot size and color
- Up to five dots are shown; five dots means **five or more windows**

## Behavior

By default, no indicator is shown for a single open window. The indicator appears
when an app reaches two open windows.

The minimum window count can be changed in the settings.

Window counts update automatically as windows are opened and closed, and
settings are applied live.

## Compatibility

- Windows 11 only
- Supports x64 and ARM64 Windows
- Works with multiple monitors and secondary Windows taskbars

The mod is designed primarily for grouped/combined taskbar app buttons. When
taskbar buttons are configured to stay uncombined, Windows can expose multiple
buttons for the same app group, so the same app count may appear on more than
one button.

Third-party taskbar replacements or tools that replace the native Windows 11
taskbar may not be compatible.

## Notes

The mod counts real top-level application windows and maps them to their Windows
application IDs. Windows shell infrastructure windows are excluded from the
count.

The badge is visual only and doesn't change taskbar grouping, combining, window
ordering, or application behavior.