## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2025-01-20 - Cell Reuse and Accessibility State
**Learning:** Reusing `UITableViewCell` instances without explicitly resetting `accessoryType` to `None` and clearing `UIAccessibilityTraitSelected` can cause incorrect UI states and misleading VoiceOver announcements on recycled cells.
**Action:** Always ensure that both visual state (e.g. checkmarks) and accessibility traits are completely reset for unselected states in `cellForRowAtIndexPath`.
