# The Dogear style

Qt Quick Controls style for Dogear, so every stock control (ComboBox, Switch,
Slider, Menu, ToolTip...) wears the design without per-file styling. Anything
not listed in `qmldir` falls back to Basic.

Colours come from the control's `palette`, which Qt passes down from the
window (set from the theme tokens in Main.qml) or from a reader panel (set
from the reading theme). The roles mean:

| Role | Token |
|---|---|
| window / windowText | ground / ink1 |
| base / alternateBase | field fill / field fill on hover |
| text / placeholderText | ink1 / ink4 |
| button / buttonText | fill / ink1 |
| mid | ink3, secondary text |
| midlight / dark | hairline / separator |
| light | overlay, the background of menus, popups and tooltips |
| shadow | shadow |
| highlight / highlightedText | accent / text on the accent |
| link / linkVisited | accent as ink / accent soft (selected rows) |

Corners follow Hyprland via the `theme.radius` context property.
