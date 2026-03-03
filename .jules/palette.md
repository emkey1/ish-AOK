## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-05-25 - Table View Cell Checkmarks
**Learning:** `UITableViewCellAccessoryCheckmark` does not automatically update accessibility traits to inform VoiceOver users that an item is selected. When using the checkmark accessory type to represent a selected state in a list, `UIAccessibilityTraitSelected` must be explicitly added to the cell's `accessibilityTraits`.
**Action:** When setting `cell.accessoryType = UITableViewCellAccessoryCheckmark`, also set `cell.accessibilityTraits |= UIAccessibilityTraitSelected` (and clear it when not selected).
