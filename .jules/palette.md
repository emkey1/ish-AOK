## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-05-25 - UITableViewCell Checkmark Accessibility
**Learning:** Using `UITableViewCellAccessoryCheckmark` visually indicates a selected state, but VoiceOver does not automatically announce this to users.
**Action:** When using `UITableViewCellAccessoryCheckmark` in a `UITableView`, manually toggle `UIAccessibilityTraitSelected` on the cell to ensure the selection state is communicated to screen reader users.
