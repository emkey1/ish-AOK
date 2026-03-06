## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-05-25 - UITableViewCell State Reuse and Accessibility
**Learning:** When reusing `UITableViewCell`s with checkmarks (`UITableViewCellAccessoryCheckmark`), failing to explicitly clear the accessory type for unselected cells causes visual bugs when scrolling. Furthermore, relying only on the visual checkmark leaves VoiceOver users unaware of the selection state.
**Action:** Always explicitly set `accessoryType` to `UITableViewCellAccessoryNone` for unselected recycled cells. Simultaneously, toggle `UIAccessibilityTraitSelected` (using `|=` to set and `&= ~` to clear) alongside the checkmark to ensure screen reader users receive accurate selection feedback.
