## 2024-05-23 - Toggle Button Accessibility
**Learning:** Returning "1" or "0" for `accessibilityValue` on toggle buttons is confusing for screen reader users. Standard practice is to use `UIAccessibilityTraitSelected`.
**Action:** Replace custom `accessibilityValue` implementations with `UIAccessibilityTraitSelected` for toggleable controls.

## 2024-05-24 - Accessibility Availability Checks
**Learning:** Accessibility properties like `accessibilityLabel` are often available in earlier iOS versions than visual features like SF Symbols. Wrapping them in `@available` checks for visual features unnecessarily restricts accessibility on older OS versions.
**Action:** Separate accessibility configuration from version-specific visual setup to ensure broader support.

## 2024-06-05 - Recycled Table View Cells and Selection State
**Learning:** When managing selection state for reused `UITableViewCell` instances (e.g., using `UITableViewCellAccessoryCheckmark`), relying only on `accessoryType` causes incorrect VoiceOver announcements on recycled cells, because accessibility traits persist.
**Action:** Always manually toggle `UIAccessibilityTraitSelected` using bitwise operators (`|=` to set, `&= ~` to clear) alongside `accessoryType` updates in `cellForRowAtIndexPath` or `willDisplayCell`.
