## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-05-25 - UITableViewCellAccessoryCheckmark Accessibility
**Learning:** In iOS `UITableView` implementations, modifying `UITableViewCellAccessoryCheckmark` does not automatically toggle the `UIAccessibilityTraitSelected` trait for VoiceOver users. On reused cells, failing to explicitly reset this trait can lead to incorrect VoiceOver announcements.
**Action:** Always use bitwise operators (`|=` to set, `&= ~` to clear) to manually toggle `UIAccessibilityTraitSelected` alongside changes to `UITableViewCellAccessoryCheckmark` for cells.
