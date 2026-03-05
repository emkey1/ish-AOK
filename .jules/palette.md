## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2026-03-05 - UITableViewCell Checkmark Accessibility
**Learning:** Setting `cell.accessoryType = UITableViewCellAccessoryCheckmark` in iOS `UITableView` implementations does not automatically confer the `UIAccessibilityTraitSelected` trait for VoiceOver users.
**Action:** Manually toggle `UIAccessibilityTraitSelected` on cells when applying or removing `UITableViewCellAccessoryCheckmark` to ensure VoiceOver users are aware of the selection state.
