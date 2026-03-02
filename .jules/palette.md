## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2025-03-02 - Table Cell Checkmark Accessibility
**Learning:** Adding `UITableViewCellAccessoryCheckmark` visually indicates selection but does not automatically notify VoiceOver users of the selected state.
**Action:** Always manually toggle `UIAccessibilityTraitSelected` on `cell.accessibilityTraits` whenever `UITableViewCellAccessoryCheckmark` is toggled.
